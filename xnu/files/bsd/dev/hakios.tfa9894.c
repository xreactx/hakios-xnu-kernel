/*
 * The OnePlus 6T's speaker amplifier: an NXP TFA9894 ("smart amplifier", with a DSP of its own that
 * protects the speaker) on I2C -- the serial engine at 0x890000 of the phone's tree, GPIO 89 data and
 * 90 clock (function qup4, here taken as GPIOs and done by hand, as hakios.synaptics.c does its bus),
 * address 0x34 -- with its reset on GPIO 69 (high: in reset), fed over the quaternary MI2S
 * (hakios.sound.c, soc_sound_port=0x1006).
 *
 * It is not set up from its configuration file the way Android's driver (NXP's tfa98xx v6.5.2) does
 * it -- that is a container of a patch for the chip's DSP and of its parameters, in NXP's own format --
 * but by doing again what that driver did: the I2C transfers of its set-up at a cold boot, recorded
 * under Android with the kernel's I2C trace and made into fw/tfa9894-coldboot.replay by
 * tools/tfa-trace2replay.py (devices/oneplus-6t/README.md, step 10). The file holds the writes, and the
 * reads with what the chip answered then; it is the vendor's (the patch is in it), in the small image
 * with ADSP=1 userland/build.sh, never in the repository.
 *
 *   tfa9894=1  a look: the pins, then the chip's revision (register 3, 0x1a94) over I2C; reads only
 *   tfa9894=2  ... the chip reset (GPIO 69 high for 1 ms, as the driver does at its probe), the set-up
 *              replayed, then powered down (Android's stop); its registers said before and after
 *
 * and for hakios.sound.c's sound=3 on this phone: hakios_tfa9894_prepare() (reset, set-up, powered
 * down), hakios_tfa9894_start() (only while the port's clocks run: Android's start, the chip's status
 * said), hakios_tfa9894_stop() (Android's stop), hakios_tfa9894_quiesce() (the same at the run's end,
 * and on a panic's way out: no sleeping, nothing printed with interrupts off).
 *
 * Android's start is 04=0000 00=a274 01=15d6 e0=0001 e1=0001, then 00=a27c once register 0x10 says the
 * clocks are there (0x785e playing, 0x401c without them); its stop 04=0040 00=a27d 00=a275.
 *
 * Pins: the phone's tree (qupv3_se4_i2c_active; the tfa98xx node's reset-gpio) and Linux's
 * pinctrl-sdm845.c (89 and 90 in the south tile, 69 in the east).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static uint32_t level;
static bool leaving;                    /* on a panic's way out: nothing sleeps, nothing is printed */

static LCK_GRP_DECLARE(tfa_lock_group, "hakios-tfa9894");
static LCK_MTX_DECLARE(tfa_lock, &tfa_lock_group);

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
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
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

static void
wait_ms(uint32_t ms)
{
	if (leaving || !ml_get_interrupts_enabled()) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- pins: each its own page of the pin controller ----------------------------------------------------- */

#define TLMM_PHYS               0x03400000UL
#define TILE_SOUTH              0x900000UL
#define TILE_EAST               0x100000UL
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 89      /* the serial engine's first pin is the data line, the second the clock */
#define PIN_SCL                 90
#define PIN_RESET               69

enum { SDA, SCL, RESET, PINS };
static const struct {
	unsigned pin;
	vm_offset_t tile;
	const char *what;
} pins[PINS] = { { PIN_SDA, TILE_SOUTH, "I2C data" }, { PIN_SCL, TILE_SOUTH, "I2C clock" }, { PIN_RESET, TILE_EAST, "reset" } };
static vm_offset_t page[PINS];
static uint32_t found_cfg[PINS], found_io[PINS];
static bool mapped, reset_taken;

#define CFG(i)                  (page[i])               /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define IO(i)                   (page[i] + 4)           /* bit 0 in, bit 1 out */

static void
map_pins(void)
{
	if (mapped) {
		return;
	}
	for (unsigned i = 0; i < PINS; i++) {
		page[i] = ml_io_map(TLMM_PHYS + pins[i].tile + 0x1000UL * pins[i].pin, 0x1000);
		found_cfg[i] = rd(CFG(i));
		found_io[i] = rd(IO(i));
	}
	mapped = true;
}

static void
show_pins(const char *when)
{
	say("tfa9894: the pins %s:\n", when);
	for (unsigned i = 0; i < PINS; i++) {
		uint32_t cfg = rd(CFG(i)), io = rd(IO(i));

		say("tfa9894:   gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what, (cfg >> 2) & 15,
		    cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

/* ---- I2C by hand (as hakios.synaptics.c's) ------------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
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
	for (unsigned stretched = 0; !level_of(SCL) && stretched < 100; stretched++) {
		half_bit();
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
	line(SDA, true);
	line(SCL, true);
	half_bit();
	if (!level_of(SDA) || !level_of(SCL)) {
		say("tfa9894: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA, level_of(SDA),
		    PIN_SCL, level_of(SCL));
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	for (unsigned i = SDA; i <= SCL; i++) {
		wr(IO(i), found_io[i] & 2);
		wr(CFG(i), found_cfg[i]);
	}
}

#define TFA_ADDRESS             0x34

/* `count' bytes from register `reg' on (16-bit registers, most significant byte first) */
static bool
tfa_read(uint8_t reg, uint8_t *bytes, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(TFA_ADDRESS << 1) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(TFA_ADDRESS << 1 | 1);
	}
	for (unsigned i = 0; answered && i < count; i++) {
		bytes[i] = i2c_read(i + 1 < count);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

/* one transfer: the register, then the bytes (a long one runs on through the registers after it) */
static bool
tfa_write(uint8_t reg, const uint8_t *bytes, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(TFA_ADDRESS << 1) && i2c_write(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_write(bytes[i]);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static bool
reg_get(uint8_t reg, uint16_t *value)
{
	uint8_t bytes[2];

	if (!tfa_read(reg, bytes, 2)) {
		return false;
	}
	*value = (uint16_t)(bytes[0] << 8 | bytes[1]);
	return true;
}

static bool
reg_set(uint8_t reg, uint16_t value)
{
	uint8_t bytes[2] = { (uint8_t)(value >> 8), (uint8_t)value };

	return tfa_write(reg, bytes, 2);
}

static void
show_regs(const char *when)
{
	static const uint8_t regs[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x10, 0x13, 0x14, 0x15, 0x16, 0x20, 0x24, 0x51, 0xe0, 0xe1 };
	char text[260];
	int at = 0;

	for (unsigned i = 0; i < sizeof(regs) && at < (int)sizeof(text) - 16; i++) {
		uint16_t value;

		if (reg_get(regs[i], &value)) {
			at += snprintf(text + at, sizeof(text) - (size_t)at, " %02x=%04x", regs[i], value);
		} else {
			at += snprintf(text + at, sizeof(text) - (size_t)at, " %02x=-", regs[i]);
		}
	}
	say("tfa9894: registers %s:%s\n", when, text);
}

/* ---- the chip -------------------------------------------------------------------------------------------- */

#define TFA9894_REVISION        0x1a94

static bool prepared, started;

static bool
chip_answers(void)
{
	uint16_t revision = 0;

	if (!reg_get(0x03, &revision)) {
		say("tfa9894: NOBODY ANSWERS at %#x on gpio%u/%u\n", TFA_ADDRESS, PIN_SDA, PIN_SCL);
		return false;
	}
	say("tfa9894: register 3 says %#06x: %s\n", revision, revision == TFA9894_REVISION ? "A TFA9894" : "NOT THE TFA9894 EXPECTED");
	return revision == TFA9894_REVISION;
}

/* the reset pin: high for 1 ms, then low and left driven low (tfa98xx_i2c_probe() does the same) */
static void
reset_pulse(void)
{
	say("tfa9894: about to pulse gpio%u (reset) high for 1 ms, then drive it low\n", PIN_RESET);
	wr(IO(RESET), 2);
	wr(CFG(RESET), CFG_OUTPUT);
	wait_ms(1);
	wr(IO(RESET), 0);
	wait_ms(1);
	reset_taken = true;
}

/*
 * The recorded set-up again: each write as recorded; each read until it gives what Android's driver
 * got (20 times, a millisecond apart -- the chip's DSP answering the driver's messages, its status
 * settling), and where it never does, said and gone on with (a temperature, a counter, are not the same
 * twice). A write the chip does not acknowledge ends it.
 */
static bool
replay(void)
{
	uint64_t size = 0;
	const uint8_t *file = hakios_image_file("fw", "tfa9894-coldboot.replay", &size, NULL);
	uint32_t records, at = 14, writes = 0, reads = 0, differed = 0, bytes = 0;
	uint64_t began;

	if (file == NULL) {
		say("tfa9894: no fw/tfa9894-coldboot.replay in the small image: make it with tools/tfa-trace2replay.py into fw-audio/ "
		    "and build with ADSP=1\n");
		return false;
	}
	if (size < 14 || memcmp(file, "TFAREPLAY2", 10) != 0) {
		say("tfa9894: fw/tfa9894-coldboot.replay is not a replay file this knows\n");
		return false;
	}
	records = file[10] | file[11] << 8 | file[12] << 16 | (uint32_t)file[13] << 24;
	say("tfa9894: replaying the set-up Android's driver did: %u records, %llu bytes\n", records, size);
	began = mach_absolute_time();
	for (uint32_t r = 0; r < records; r++) {
		uint8_t kind, got[8];
		uint16_t length;
		const uint8_t *body;

		if (at + 3 > size) {
			say("tfa9894: the file ends at record %u of %u\n", r, records);
			return false;
		}
		kind = file[at];
		length = (uint16_t)(file[at + 1] | file[at + 2] << 8);
		body = file + at + 3;
		if (at + 3 + length > size || length < 2) {
			say("tfa9894: record %u runs past the file's end\n", r);
			return false;
		}
		at += 3u + length;
		if (kind == 'W') {
			if (!tfa_write(body[0], body + 1, length - 1u)) {
				say("tfa9894: THE CHIP DOES NOT TAKE WRITE %u (register %#x, %u bytes): the set-up ends here\n", r, body[0],
				    length - 1u);
				return false;
			}
			writes++;
			bytes += length - 1u;
		} else if (kind == 'R') {
			unsigned count = length - 1u, tries;

			if (count > sizeof(got)) {
				count = sizeof(got);
			}
			for (tries = 0; tries < 20; tries++) {
				if (tfa_read(body[0], got, count) && memcmp(got, body + 1, count) == 0) {
					break;
				}
				wait_ms(1);
			}
			reads++;
			if (tries == 20) {
				if (differed++ < 12) {
					say("tfa9894:   read %u: register %#x gives %02x%02x, Android's driver had %02x%02x\n", r, body[0], got[0],
					    count > 1 ? got[1] : 0, body[1], count > 1 ? body[2] : 0);
				}
			}
		} else {
			say("tfa9894: record %u is of a kind this does not know (%#x)\n", r, kind);
			return false;
		}
	}
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("tfa9894: THE SET-UP IS DONE AGAIN: %u writes (%u bytes), %u reads, %u of them not as Android's, in %llu ms\n",
		    writes, bytes, reads, differed, ns / NSEC_PER_MSEC);
	}
	return true;
}

/* Android's stop: muted, then powered down */
static bool
power_down(void)
{
	return reg_set(0x04, 0x0040) && reg_set(0x00, 0xa27d) && reg_set(0x00, 0xa275);
}

bool
hakios_tfa9894_prepare(void)
{
	bool ok = false;

	lck_mtx_lock(&tfa_lock);
	map_pins();
	show_pins("as found");
	reset_pulse();
	if (!chip_answers()) {
		goto out;
	}
	show_regs("after the reset");
	if (!replay()) {
		goto out;
	}
	ok = power_down();
	show_regs(ok ? "set up and powered down" : "set up, NOT POWERED DOWN");
	prepared = ok;
out:
	lck_mtx_unlock(&tfa_lock);
	return ok;
}

/* Android's start; only while the port's clocks run, which register 0x10 is asked about first */
bool
hakios_tfa9894_start(void)
{
	uint16_t status = 0;
	bool ok;

	lck_mtx_lock(&tfa_lock);
	if (!prepared) {
		lck_mtx_unlock(&tfa_lock);
		return false;
	}
	ok = reg_set(0x04, 0x0000) && reg_set(0x00, 0xa274) && reg_set(0x01, 0x15d6) && reg_set(0xe0, 0x0001) &&
	    reg_set(0xe1, 0x0001);
	for (unsigned ms = 0; ok && ms < 50; ms++) {
		if (reg_get(0x10, &status) && status == 0x785e) {
			break;
		}
		wait_ms(1);
	}
	say("tfa9894: before switching on, register 0x10 says %#06x (%s)\n", status, status == 0x785e ?
	    "as under Android while it played" : "NOT AS UNDER ANDROID WHILE IT PLAYED: 0x785e");
	ok = ok && reg_set(0x00, 0xa27c);
	started = ok;
	show_regs(ok ? "SWITCHED ON" : "NOT SWITCHED ON");
	lck_mtx_unlock(&tfa_lock);
	return ok;
}

void
hakios_tfa9894_stop(void)
{
	lck_mtx_lock(&tfa_lock);
	if (started) {
		say("tfa9894: %s\n", power_down() ? "muted and powered down" : "NOT POWERED DOWN");
		started = false;
	}
	lck_mtx_unlock(&tfa_lock);
}

/* At the run's end, and on a panic's way out: powered down, the pins as they were found (no lock, no sleep). */
void
hakios_tfa9894_quiesce(void)
{
	if (!mapped) {
		return;
	}
	leaving = true;
	if (started || prepared) {
		power_down();
		started = prepared = false;
	}
	if (reset_taken) {
		wr(IO(RESET), found_io[RESET] & 2);
		wr(CFG(RESET), found_cfg[RESET]);
		reset_taken = false;
	}
}

static void
look_thread(__unused void *arg, __unused wait_result_t wr_)
{
	if (level == 1) {
		lck_mtx_lock(&tfa_lock);
		map_pins();
		show_pins("as found");
		if (chip_answers()) {
			show_regs("as found");
		}
		lck_mtx_unlock(&tfa_lock);
	} else {
		say("tfa9894: %s\n", hakios_tfa9894_prepare() ? "SET UP, POWERED DOWN" : "NOT SET UP");
	}
	thread_terminate(current_thread());
}

/* Called while the system starts (bsd/dev/hakios.input.c). */
void
hakios_tfa9894_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("tfa9894", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 2) {
		level = 2;
	}
	say("tfa9894: level %u: %s\n", level, level == 1 ? "a look, reads only" : "reset, the recorded set-up, powered down");
	if (kernel_thread_start(look_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
