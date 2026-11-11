/*
 * The OnePlus 6T's fuel gauge: a TI bq27541 (or its bq27411 variant) at I2C address 0x55 on QUP SE10 (GPIO 55 data,
 * 56 clock, the north tile), where Android reads the battery's charge (fajita.dtsi: bq27541-battery@55; the PMI8998's
 * own gauge is marked "oem,use_external_fg" and goes unused, uncalibrated -- it said 23 % while Android said 10 %,
 * 10 November 2026). Register maps and the way the two are told apart: OnePlus's bq27541_fuelgauger.c (LineageOS
 * android_kernel_oneplus_sdm845).
 *
 *   bq27541=1   (the default on the 6T) the gauge's kind once (the CONTROL register's DEVICE_TYPE request, as
 *               OnePlus's driver asks it: 0x0421 is a bq27411), then every bq27541_every=S seconds (10) its charge,
 *               voltage, average current, temperature and capacities -- handed to Darwin's battery
 *               (hakios_battery_report(): drivers/power/hakios.battery.c and .cpp, the power source iOS shows),
 *               with the charger's input as hakios.pmi_charger.c last found it. A line in the log once a minute.
 *               The only thing written to the gauge is that request; nothing is configured.
 *   bq27541=0   leaves it alone.
 *
 * The I2C by hand, as hakios.tfa9894.c's (100 kHz at most): the two pins are taken for each transfer and given back
 * to what they were found to be after it, and once more at the run's end.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);
extern bool hakios_pmi_charger_input(int32_t *external, int32_t *usb_type);     /* hakios.pmi_charger.c */

static uint32_t level = 1, every_s = 10;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return length;
}

static uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* ---- the pins -------------------------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x03400000UL
#define TILE_NORTH              0x500000UL      /* (Linux's pinctrl-sdm845.c: 55 and 56 are NORTH) */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

enum { SDA, SCL, PINS };
static const unsigned pin_number[PINS] = { 55, 56 };    /* the serial engine's first pin is the data line */
static vm_offset_t page[PINS];
static uint32_t found_cfg[PINS], found_io[PINS];
static volatile bool mapped, holding;

#define CFG(i)                  (page[i])               /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define IO(i)                   (page[i] + 4)           /* bit 0 in, bit 1 out */

static void
map_pins(void)
{
	for (unsigned i = 0; i < PINS; i++) {
		page[i] = ml_io_map(TLMM_PHYS + TILE_NORTH + 0x1000UL * pin_number[i], 0x1000);
		found_cfg[i] = rd(CFG(i));
		found_io[i] = rd(IO(i));
	}
	mapped = true;
}

static void
half_bit(void)
{
	spin_us(5);
}

static void
line(unsigned i, bool high)
{
	wr(IO(i), 0);
	wr(CFG(i), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned i)
{
	return rd(IO(i)) & 1;
}

static void
i2c_start(void)
{
	line(SDA, true);
	line(SCL, true);
	half_bit();
	line(SDA, false);
	half_bit();
	line(SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(SDA, false);
	half_bit();
	line(SCL, true);
	half_bit();
	line(SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(SDA, out);
	half_bit();
	line(SCL, true);
	for (unsigned stretched = 0; !level_of(SCL) && stretched < 200; stretched++) {
		half_bit();             /* (the gauge stretches the clock) */
	}
	half_bit();
	in = level_of(SDA);
	line(SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static bool
bus_take(void)
{
	holding = true;
	line(SDA, true);
	line(SCL, true);
	half_bit();
	if (!level_of(SDA) || !level_of(SCL)) {
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	for (unsigned i = 0; i < PINS; i++) {
		wr(IO(i), found_io[i] & 2);
		wr(CFG(i), found_cfg[i]);
	}
	holding = false;
}

/* ---- the gauge ------------------------------------------------------------------------------------------------- */

#define GAUGE_ADDRESS           0x55

/* a 16-bit command register (little-endian), as the gauge's standard commands are read */
static bool
gauge_read(uint8_t reg, uint16_t *value)
{
	uint8_t lo = 0, hi = 0;
	bool answered;

	if (!bus_take()) {
		bus_give_back();
		return false;
	}
	i2c_start();
	answered = i2c_write(GAUGE_ADDRESS << 1) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(GAUGE_ADDRESS << 1 | 1);
	}
	if (answered) {
		lo = i2c_read(true);
		hi = i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	spin_us(100);                   /* (the gauge wants 66 us between transfers) */
	*value = (uint16_t)(lo | hi << 8);
	return answered;
}

/* a CONTROL request: the sub-command written to register 0, its answer read from it (bq27541_cntl_cmd) */
static bool
gauge_control(uint16_t subcommand, uint16_t *answer)
{
	bool answered;

	if (!bus_take()) {
		bus_give_back();
		return false;
	}
	i2c_start();
	answered = i2c_write(GAUGE_ADDRESS << 1) && i2c_write(0x00) && i2c_write((uint8_t)subcommand) &&
	    i2c_write((uint8_t)(subcommand >> 8));
	i2c_stop();
	bus_give_back();
	spin_us(100);
	return answered && gauge_read(0x00, answer);
}

/* the two kinds' registers (bq27541_fuelgauger.c: BQ27541_REG_* and BQ27411_REG_*) */
static struct {
	const char *name;
	uint8_t temp, volt, flags, rm, fcc, ai, soc;
} map;
static const typeof(map) map_541 = { "bq27541", 0x06, 0x08, 0x0a, 0x10, 0x12, 0x14, 0x2c };
static const typeof(map) map_411 = { "bq27411", 0x02, 0x04, 0x06, 0x0c, 0x0e, 0x10, 0x1c };

static bool
identify(void)
{
	uint16_t type = 0, volt = 0;

	if (gauge_control(0x0001, &type)) {
		map = type == 0x0421 ? map_411 : map_541;
		say("bq27541: THE FUEL GAUGE ANSWERS at %#x on gpio%u/%u: device type 0x%04x, a %s\n", GAUGE_ADDRESS, pin_number[SDA],
		    pin_number[SCL], type, map.name);
		return true;
	}
	/* (no answer to the request: told apart by which register holds a battery's voltage) */
	if (gauge_read(map_541.volt, &volt) && volt >= 2500 && volt <= 4600) {
		map = map_541;
	} else if (gauge_read(map_411.volt, &volt) && volt >= 2500 && volt <= 4600) {
		map = map_411;
	} else {
		say("bq27541: NOBODY ANSWERS at %#x on gpio%u/%u (sda reads %u, scl %u)\n", GAUGE_ADDRESS, pin_number[SDA],
		    pin_number[SCL], found_io[SDA] & 1, found_io[SCL] & 1);
		return false;
	}
	say("bq27541: the fuel gauge answers, not to the device-type request: a %s by its voltage register (%u mV)\n", map.name, volt);
	return true;
}

static void
bq27541_thread(__unused void *arg, __unused wait_result_t wr_)
{
	unsigned tick = 0, failures = 0;

	map_pins();
	if (!identify()) {
		say("bq27541: done\n");
		thread_terminate(current_thread());
		return;
	}
	for (;; tick++) {
		uint16_t soc = 0, volt = 0, temp = 0, flags = 0, rm = 0, fcc = 0, ai = 0;
		struct hakios_battery_state b;
		int32_t external = 0, usb_type = 0;

		if (!gauge_read(map.soc, &soc) || !gauge_read(map.volt, &volt) || !gauge_read(map.ai, &ai) ||
		    !gauge_read(map.temp, &temp) || !gauge_read(map.flags, &flags) || !gauge_read(map.rm, &rm) ||
		    !gauge_read(map.fcc, &fcc) || soc > 100 || volt < 2500 || volt > 4600) {
			if (failures++ < 5) {
				say("bq27541: a reading failed or made no sense (charge %u %%, %u mV): not reported\n", soc, volt);
			}
		} else {
			bool known = hakios_pmi_charger_input(&external, &usb_type);
			int32_t current_ma = (int16_t)ai;       /* (signed: more than 0 while the battery takes charge) */

			memset(&b, 0, sizeof(b));
			b.size = sizeof(b);
			b.percent_hundredths = soc * 100;
			b.voltage_uv = volt * 1000;
			b.current_ua = current_ma * 1000;
			b.temperature_hundredths = (int32_t)temp * 10 - 27315;  /* (0.1 K) */
			b.present = 1;
			b.external = known ? external : 0;
			b.status = !b.external ? 2 : soc >= 100 ? 4 : current_ma > 0 ? 1 : 3;
			b.charge_uah = rm * 1000;
			b.full_uah = fcc * 1000;
			b.usb_type = known ? usb_type : 0;
			hakios_battery_report(&b);
			if (tick % (60 / every_s ? 60 / every_s : 1) == 0) {
				int32_t tenths = b.temperature_hundredths / 10;

				say("bq27541: %u %%, %u mV, %d mA, %d.%d C, %u of %u mAh, flags 0x%04x; %s\n", soc, volt, current_ma,
				    tenths / 10, tenths < 0 ? -(tenths % 10) : tenths % 10, rm, fcc, flags,
				    !known ? "the charger not yet looked at" : b.status == 1 ? "charging" : b.status == 4 ? "full" :
				    b.status == 3 ? "on a charger, not charging" : "not on a charger");
			}
		}
		delay_for_interval(every_s * 1000, NSEC_PER_MSEC);
	}
}

void
hakios_bq27541_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("bq27541", &level, sizeof(level));
	if (level == 0) {
		return;
	}
	(void)PE_parse_boot_argn("bq27541_every", &every_s, sizeof(every_s));
	if (every_s < 2) {
		every_s = 2;
	}
	if (kernel_thread_start(bq27541_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end, with interrupts off: the two pins as they were found, if a transfer was cut short. */
void
hakios_bq27541_quiesce(void)
{
	if (mapped && holding) {
		bus_give_back();
	}
}
