/*
 * The OnePlus 6T's cameras, first steps: the three sensors powered as Android powers them and asked their chip ID.
 * Every address and number here is the 6T's own, from four places, read as data:
 *
 *   - the phone's device tree (fajita.dtsi: qcom,cam-sensor@0..2; ~/oneplus6t-ref/camera)
 *   - the sensors' module files of its camera stack (/vendor/lib/camera/com.qti.sensormodule.liteon_*.bin, decoded by
 *     tools/camsensor.py): the slave address, the chip ID and where it is, the power sequence
 *   - Linux's drivers for the SoC (camcc-sdm845.c, gcc-sdm845.c, clk-alpha-pll.c, clk-rcg2.c, gdsc.c,
 *     pinctrl-sdm845.c, qcom-rpmh-regulator.c)
 *   - what Android's kernel had switched on with the stock Camera app open (debugfs clk, gpio and regulator
 *     summaries, 11 November 2026): the same rails, pins and clocks as below, MCLK at 24 MHz from PLL2.
 *
 *   slot  sensor   where       I2C (CCI by hand)        MCLK            reset  VANA en  VDIG en  CSIPHY
 *   0     IMX519   rear main   master 0: GPIO 17/18, 0x34  mclk0, GPIO 13  28     27       8        0 (3 C-PHY trios)
 *   1     IMX376K  rear 2nd    master 1: GPIO 19/20, 0x20  mclk2, GPIO 15  23     78       79       1 (4 D-PHY lanes)
 *   2     IMX371   front       master 0: GPIO 17/18, 0x20  mclk1, GPIO 14  9      104      117      2 (4 D-PHY lanes)
 *
 * The supplies: VANA and VDIG are fixed regulators fed by the PMI8998's BOB (Android votes it 3.312 V), each switched
 * by its GPIO above; VIO is the PM8998's LVS1 (1.8 V); "cam_clk" is camcc's titan_top_gdsc. BOB and LVS1 are asked of
 * RPMh, as hakios.synaptics.c asks for its LDO28 (cmd-db "bobb1" and "vsa1"). The power sequence is the module
 * files' (the same for all three): VANA, VDIG, VIO, MCLK at 24 MHz, reset high, 1 ms after each; down in the reverse
 * order. The chip ID: a 16-bit register at 0x0016 (0x0519, 0x0376, 0x0371).
 *
 *   cam6t=1   a look, reading only: GCC's camera clocks, then (only when GCC's camera AHB clock runs, as nothing
 *             else may touch camcc) the power domain, PLL2 and the three MCLKs as they are; every pin above; where
 *             RPMh has BOB and LVS1.
 *   cam6t=2   ... and one sensor (cam6t_sensor=0, 1 or 2; 0 by default) powered up, its chip ID read, powered down
 *             again. MCLK is camcc's PLL2 at 960 MHz (L 0x32), its even output 480 MHz, the RCG's /10 and M/N 1/2:
 *             24 MHz, as Android has it (cam6t_xo=1 takes the crystal's 19.2 MHz instead, with no PLL). All
 *             three with cam6t_sensor=3, one after the other.
 *   cam6t_after=S   when (seconds after the driver starts; 50): away from the modem's and the audio DSP's starts.
 *
 * At the run's end (hakios_cam6t_quiesce) whatever is still up goes down: reset low, MCLK off, the enables low, the
 * pins as they were found. The two RPMh votes are left as they are (on): other drivers' parts may hang on BOB, and
 * the next boot starts over anyway.
 */
#include <kern/clock.h>
#include <kern/locks.h>
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
extern lck_mtx_t hakios_rsc_lock;               /* hakios.wifi.c: one RPMh request at a time */

static uint32_t level, which, after_s = 50, use_xo;

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

static void
wait_ms(uint32_t ms)
{
	if (ml_get_interrupts_enabled()) {
		delay_for_interval(ms, NSEC_PER_MSEC);
	} else {
		spin_us(ms * 1000);
	}
}

/* ---- the sensors ------------------------------------------------------------------------------------------------ */

struct sensor {
	const char *name, *where;
	uint8_t address;                /* 8-bit, as the module file and Android's log give it */
	uint16_t id;
	unsigned sda, scl, mclk_pin, reset, vana, vdig;
	unsigned mclk;                  /* camcc's MCLK n */
};

static const struct sensor sensors[] = {
	{ "IMX519", "rear main", 0x34, 0x0519, 17, 18, 13, 28, 27, 8, 0 },
	{ "IMX376K", "rear second", 0x20, 0x0376, 19, 20, 15, 23, 78, 79, 2 },
	{ "IMX371", "front", 0x20, 0x0371, 17, 18, 14, 9, 104, 117, 1 },
};
#define SENSORS                 (sizeof(sensors) / sizeof(sensors[0]))
#define ID_REGISTER             0x0016

/* ---- the pins (Linux's pinctrl-sdm845.c: each pin's tile; a pin's registers are its own 4 KB page there) ------- */

#define TLMM_PHYS               0x03400000UL
#define TILE_NORTH              0x500000UL
#define TILE_SOUTH              0x900000UL
#define TILE_EAST               0x100000UL
#define CFG_PULL_MASK           3u
#define CFG_PULL_DOWN           1u
#define CFG_PULL_UP             3u
#define CFG_FUNC_SHIFT          2
#define CFG_DRIVE_SHIFT         6
#define CFG_OUTPUT              (1u << 9)

static const struct {
	unsigned pin;
	vm_offset_t tile;
	const char *what;
} pin_list[] = {
	{ 8, TILE_EAST, "IMX519 VDIG enable" }, { 9, TILE_EAST, "IMX371 reset" }, { 13, TILE_SOUTH, "MCLK0" },
	{ 14, TILE_SOUTH, "MCLK1" }, { 15, TILE_SOUTH, "MCLK2" }, { 17, TILE_SOUTH, "CCI master 0 SDA" },
	{ 18, TILE_SOUTH, "CCI master 0 SCL" }, { 19, TILE_SOUTH, "CCI master 1 SDA" }, { 20, TILE_SOUTH, "CCI master 1 SCL" },
	{ 23, TILE_SOUTH, "IMX376K reset" }, { 27, TILE_EAST, "IMX519 VANA enable" }, { 28, TILE_EAST, "IMX519 reset" },
	{ 78, TILE_EAST, "IMX376K VANA enable" }, { 79, TILE_NORTH, "IMX376K VDIG enable" },
	{ 104, TILE_NORTH, "IMX371 VANA enable" }, { 117, TILE_NORTH, "IMX371 VDIG enable" },
};
#define PINS                    (sizeof(pin_list) / sizeof(pin_list[0]))

static vm_offset_t pin_page[PINS];
static uint32_t found_cfg[PINS], found_io[PINS];
static bool pins_mapped, pins_touched[PINS];

static int
pin_index(unsigned pin)
{
	for (unsigned i = 0; i < PINS; i++) {
		if (pin_list[i].pin == pin) {
			return (int)i;
		}
	}
	return -1;
}

static void
map_pins(void)
{
	if (pins_mapped) {
		return;
	}
	for (unsigned i = 0; i < PINS; i++) {
		pin_page[i] = ml_io_map(TLMM_PHYS + pin_list[i].tile + 0x1000UL * pin_list[i].pin, 0x1000);
		found_cfg[i] = rd(pin_page[i]);
		found_io[i] = rd(pin_page[i] + 4);
	}
	pins_mapped = true;
}

#define PIN_IO(pin)             (pin_page[pin_index(pin)] + 4)

static void
show_pins(void)
{
	for (unsigned i = 0; i < PINS; i++) {
		uint32_t cfg = found_cfg[i];

		say("cam6t: gpio%-3u %-20s func %u, %s, %u mA, pull %s, in %u out %u\n", pin_list[i].pin, pin_list[i].what,
		    cfg >> CFG_FUNC_SHIFT & 0xf, cfg & CFG_OUTPUT ? "output" : "input", ((cfg >> CFG_DRIVE_SHIFT & 7) + 1) * 2,
		    (const char *[]){ "none", "down", "keeper", "up" }[cfg & CFG_PULL_MASK], found_io[i] & 1, found_io[i] >> 1 & 1);
	}
}

/* a plain output pin, function 0, 2 mA, no pull (as Android has the enables and resets while the sensor runs) */
static void
drive(unsigned pin, bool high)
{
	int i = pin_index(pin);

	pins_touched[i] = true;
	wr(pin_page[i] + 4, high ? 2 : 0);
	wr(pin_page[i], CFG_OUTPUT);
}

/* the MCLK pin to its camera function (func 1, 2 mA, no pull), or back to a quiet input pulled down */
static void
mclk_pin(unsigned pin, bool on)
{
	int i = pin_index(pin);

	pins_touched[i] = true;
	wr(pin_page[i], on ? 1u << CFG_FUNC_SHIFT : CFG_PULL_DOWN);
}

static void
pins_give_back(void)
{
	for (unsigned i = 0; i < PINS; i++) {
		if (pins_touched[i]) {
			wr(pin_page[i] + 4, found_io[i] & 2);
			wr(pin_page[i], found_cfg[i]);
			pins_touched[i] = false;
		}
	}
}

/* ---- the CCI by hand (100 kHz at most; the pins taken for each transfer and given back as they were) -------- */

static unsigned bus_sda, bus_scl;

static void
half_bit(void)
{
	spin_us(5);
}

static void
line(unsigned pin, bool high)
{
	int i = pin_index(pin);

	pins_touched[i] = true;
	wr(pin_page[i] + 4, 0);
	wr(pin_page[i], CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(PIN_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(bus_sda, true);
	line(bus_scl, true);
	half_bit();
	line(bus_sda, false);
	half_bit();
	line(bus_scl, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(bus_sda, false);
	half_bit();
	line(bus_scl, true);
	half_bit();
	line(bus_sda, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(bus_sda, out);
	half_bit();
	line(bus_scl, true);
	for (unsigned stretched = 0; !level_of(bus_scl) && stretched < 200; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(bus_sda);
	line(bus_scl, false);
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

static void
bus_give_back(void)
{
	int a = pin_index(bus_sda), b = pin_index(bus_scl);

	wr(pin_page[a] + 4, found_io[a] & 2);
	wr(pin_page[a], found_cfg[a]);
	wr(pin_page[b] + 4, found_io[b] & 2);
	wr(pin_page[b], found_cfg[b]);
}

/* a 16-bit register at a 16-bit address; false when nobody answers or the bus is held low */
static bool
read16(const struct sensor *s, uint16_t reg, uint16_t *value)
{
	uint8_t hi = 0, lo = 0;
	bool answered;

	bus_sda = s->sda;
	bus_scl = s->scl;
	line(bus_sda, true);
	line(bus_scl, true);
	half_bit();
	if (!level_of(bus_sda) || !level_of(bus_scl)) {
		say("cam6t: the bus (gpio%u/%u) is held low: sda %u, scl %u\n", bus_sda, bus_scl, level_of(bus_sda), level_of(bus_scl));
		bus_give_back();
		return false;
	}
	i2c_start();
	answered = i2c_write(s->address) && i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(s->address | 1);
	}
	if (answered) {
		hi = i2c_read(true);
		lo = i2c_read(false);
	}
	i2c_stop();
	bus_give_back();
	*value = (uint16_t)(hi << 8 | lo);
	return answered;
}

/* ---- the clocks: GCC's camera branches, camcc's power domain, PLL2 and the MCLKs ------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_CAMERA_AHB_CBCR     0xb008          /* critical in Linux: what every access to camcc rides on */
#define GCC_CAMERA_AXI_CBCR     0xb020
#define GCC_CAMERA_XO_CBCR      0xb02c          /* critical in Linux */
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x10000UL
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)

#define PLL2                    0x2000          /* Fabia (clk-alpha-pll.c) */
#define PLL_MODE                0x00
#define PLL_L                   0x04
#define PLL_USER_CTL            0x0c
#define PLL_STATUS              0x24
#define PLL_OPMODE              0x2c
#define PLL_FRAC                0x38
#define PLL_OUTCTRL             (1u << 0)
#define PLL_RESET_N             (1u << 2)
#define PLL_VOTE_FSM_ENA        (1u << 20)
#define PLL_UPDATE_BYPASS       (1u << 23)
#define PLL_LOCK_DET            (1u << 31)
#define PLL_OUT_MASK            7u
#define PLL_POST_DIV_EVEN       (0xfu << 8)     /* USER_CTL: the even output's divider, 1 = /2 */
#define PLL2_L                  0x32            /* 50 x 19.2 MHz = 960 MHz (camcc-sdm845.c's probe) */

#define MCLK_RCG(n)             (0x4004 + 0x20 * (n))   /* mclk0 0x4004, 1 0x4024, 2 0x4044 */
#define MCLK_CBCR(n)            (0x401c + 0x20 * (n))
#define RCG_CMD                 0x00            /* bit 0 update, bit 31 root off */
#define RCG_CFG                 0x04            /* [4:0] divider 2d-1, [10:8] source, [13:12] mode (2 = M/N) */
#define RCG_M                   0x08
#define RCG_N                   0x0c
#define RCG_D                   0x10
#define TITAN_TOP_GDSC          0xb134          /* bit 0 collapse (1 = off), bit 31 on */
#define GDSC_CFG                0x04            /* bit 16 power-up complete (POLL_CFG_GDSCR) */

static vm_offset_t gcc, camcc;
static bool gdsc_ours, pll_ours, mclk_running[3];

static bool
gcc_camera_ok(void)
{
	uint32_t ahb, xo;

	if (gcc == 0) {
		gcc = ml_io_map(GCC_PHYS + 0xb000, 0x1000);
	}
	ahb = rd(gcc + (GCC_CAMERA_AHB_CBCR & 0xfff));
	xo = rd(gcc + (GCC_CAMERA_XO_CBCR & 0xfff));
	say("cam6t: GCC: camera_ahb %#010x (%s), camera_axi %#010x, camera_xo %#010x (%s)\n", ahb, ahb & CBCR_OFF ? "off" : "on",
	    rd(gcc + (GCC_CAMERA_AXI_CBCR & 0xfff)), xo, xo & CBCR_OFF ? "off" : "on");
	return !(ahb & CBCR_OFF) && !(xo & CBCR_OFF);
}

/* GCC's two critical camera clocks on, as Linux keeps them; true when both run */
static bool
gcc_camera_on(void)
{
	static const uint32_t cbcr[2] = { GCC_CAMERA_AHB_CBCR, GCC_CAMERA_XO_CBCR };

	if (gcc_camera_ok()) {
		return true;
	}
	for (unsigned k = 0; k < 2; k++) {
		vm_offset_t r = gcc + (cbcr[k] & 0xfff);

		wr(r, rd(r) | CBCR_ENABLE);
		for (int i = 0; i < 100 && (rd(r) & CBCR_OFF); i++) {
			spin_us(10);
		}
	}
	return gcc_camera_ok();
}

static void
show_camcc(void)
{
	say("cam6t: camcc: titan_top_gdsc %#010x (cfg %#010x); PLL2 mode %#010x L %#x frac %#x user %#010x opmode %#x status %#x\n",
	    rd(camcc + TITAN_TOP_GDSC), rd(camcc + TITAN_TOP_GDSC + GDSC_CFG), rd(camcc + PLL2 + PLL_MODE), rd(camcc + PLL2 + PLL_L),
	    rd(camcc + PLL2 + PLL_FRAC), rd(camcc + PLL2 + PLL_USER_CTL), rd(camcc + PLL2 + PLL_OPMODE), rd(camcc + PLL2 + PLL_STATUS));
	for (unsigned n = 0; n < 3; n++) {
		say("cam6t: camcc: mclk%u rcg cmd %#010x cfg %#010x m %#x n %#x d %#x, branch %#010x\n", n, rd(camcc + MCLK_RCG(n)),
		    rd(camcc + MCLK_RCG(n) + RCG_CFG), rd(camcc + MCLK_RCG(n) + RCG_M), rd(camcc + MCLK_RCG(n) + RCG_N),
		    rd(camcc + MCLK_RCG(n) + RCG_D), rd(camcc + MCLK_CBCR(n)));
	}
}

static bool
gdsc_on(void)
{
	uint32_t g = rd(camcc + TITAN_TOP_GDSC);

	if (!(g & 1)) {
		return true;                    /* (on already: left on afterwards) */
	}
	gdsc_ours = true;
	wr(camcc + TITAN_TOP_GDSC, g & ~1u);
	for (int i = 0; i < 1000; i++) {
		if (rd(camcc + TITAN_TOP_GDSC + GDSC_CFG) & (1u << 16) || rd(camcc + TITAN_TOP_GDSC) & (1u << 31)) {
			say("cam6t: titan_top_gdsc on (%#010x)\n", rd(camcc + TITAN_TOP_GDSC));
			return true;
		}
		spin_us(10);
	}
	say("cam6t: titan_top_gdsc DOES NOT COME ON (%#010x, cfg %#010x)\n", rd(camcc + TITAN_TOP_GDSC),
	    rd(camcc + TITAN_TOP_GDSC + GDSC_CFG));
	return false;
}

static void
gdsc_off(void)
{
	if (gdsc_ours) {
		wr(camcc + TITAN_TOP_GDSC, rd(camcc + TITAN_TOP_GDSC) | 1);
		gdsc_ours = false;
	}
}

/* camcc-sdm845.c's configuration of PLL2 (L only, alpha 0), then alpha_pll_fabia_enable(); the even output /2 */
static bool
pll2_up(void)
{
	vm_offset_t p = camcc + PLL2;
	uint32_t mode = rd(p + PLL_MODE);

	if ((rd(p + PLL_OPMODE) & 1) && (mode & PLL_OUTCTRL)) {
		say("cam6t: PLL2 already runs (L %#x, user %#010x)\n", rd(p + PLL_L), rd(p + PLL_USER_CTL));
		return (rd(p + PLL_L) & 0xffff) == PLL2_L;
	}
	if (mode & PLL_VOTE_FSM_ENA) {
		say("cam6t: PLL2 is in FSM (voted) mode (%#010x): not taken\n", mode);
		return false;
	}
	wr(p + PLL_L, PLL2_L);
	wr(p + PLL_FRAC, 0);
	wr(p + PLL_USER_CTL, (rd(p + PLL_USER_CTL) & ~PLL_POST_DIV_EVEN) | 1u << 8);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_UPDATE_BYPASS);
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
	wr(p + PLL_OPMODE, 0);                          /* standby */
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_RESET_N);
	wr(p + PLL_OPMODE, 1);                          /* run */
	for (int i = 0; !(rd(p + PLL_MODE) & PLL_LOCK_DET); i++) {
		if (i > 1000) {
			say("cam6t: PLL2 DOES NOT LOCK (mode %#010x, status %#x)\n", rd(p + PLL_MODE), rd(p + PLL_STATUS));
			wr(p + PLL_OPMODE, 0);
			return false;
		}
		spin_us(10);
	}
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) | PLL_OUT_MASK);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_OUTCTRL);
	pll_ours = true;
	say("cam6t: PLL2 locked at 960 MHz (mode %#010x, user %#010x): its even output 480 MHz\n", rd(p + PLL_MODE),
	    rd(p + PLL_USER_CTL));
	return true;
}

static void
pll2_down(void)
{
	vm_offset_t p = camcc + PLL2;

	if (!pll_ours) {
		return;
	}
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) & ~PLL_OUT_MASK);
	wr(p + PLL_OPMODE, 0);
	pll_ours = false;
}

/*
 * MCLK n at 24 MHz (ftbl_cam_cc_mclk0_clk_src: PLL2's even output, source 1, /10, M/N 1/2; clk-rcg2.c's way of
 * writing them: N as ~(n - m), D as ~(2d) with d = n), or the crystal's 19.2 MHz (source 0, /1, no M/N).
 */
static bool
mclk_on(unsigned n)
{
	vm_offset_t r = camcc + MCLK_RCG(n), b = camcc + MCLK_CBCR(n);
	uint32_t cfg;

	if (use_xo) {
		wr(r + RCG_M, 0);
		wr(r + RCG_N, 0);
		wr(r + RCG_D, 0);
		cfg = 0;                                /* /1 (field 0: bypass), source 0, no M/N */
	} else {
		wr(r + RCG_M, 1);
		wr(r + RCG_N, ~(2u - 1u) & 0xff);
		wr(r + RCG_D, ~2u & 0xff);
		cfg = (2 * 10 - 1) | 1u << 8 | 2u << 12;
	}
	wr(r + RCG_CFG, cfg);
	wr(r + RCG_CMD, rd(r + RCG_CMD) | 1);
	for (int i = 0; rd(r + RCG_CMD) & 1; i++) {
		if (i > 1000) {
			say("cam6t: mclk%u: the RCG does not take its new setting (cmd %#010x)\n", n, rd(r + RCG_CMD));
			return false;
		}
		spin_us(1);
	}
	wr(b, rd(b) | CBCR_ENABLE);
	for (int i = 0; rd(b) & CBCR_OFF; i++) {
		if (i > 1000) {
			say("cam6t: mclk%u: the branch stays off (%#010x; rcg cmd %#010x)\n", n, rd(b), rd(r + RCG_CMD));
			return false;
		}
		spin_us(1);
	}
	mclk_running[n] = true;
	say("cam6t: mclk%u on at %s (rcg cfg %#010x, branch %#010x)\n", n, use_xo ? "19.2 MHz" : "24 MHz", rd(r + RCG_CFG), rd(b));
	return true;
}

static void
mclk_off(unsigned n)
{
	if (mclk_running[n]) {
		wr(camcc + MCLK_CBCR(n), rd(camcc + MCLK_CBCR(n)) & ~CBCR_ENABLE);
		mclk_running[n] = false;
	}
}

/* ---- RPMh: BOB and LVS1 (the RSC 2.x slots as hakios.synaptics.c drives them) ---------------------------------- */

#define CMD_DB_POINTER_PHYS     0x0c3f0000UL
#define CMD_DB_POINTER_OFFSET   0xc
#define CMD_DB_MOST             0x40000u
#define RSC_PHYS                0x179e0000UL
#define RSC_SIZE                0x10000UL
#define TCS(reg)                (rsc + 0xd00 + (reg))
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define BOB_MODE_AUTO           2               /* PMIC4_BOB_MODE_AUTO (qcom-rpmh-regulator.c) */
#define BOB_MV                  3312
#define BOB_NAME                "bobb1"         /* the PMI8998's (PMIC "b") */
#define LVS1_NAME               "vsa1"          /* the PM8998's LVS1 */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static vm_offset_t rsc, cmd_db;
static uint32_t cmd_db_size, bob, lvs1;

static bool
find_cmd_db(void)
{
	vm_offset_t pointer;
	uint32_t at, size;
	const struct cmd_db_header *db;

	if (cmd_db != 0) {
		return true;
	}
	pointer = ml_io_map(CMD_DB_POINTER_PHYS, 0x1000);
	at = rd(pointer + CMD_DB_POINTER_OFFSET);
	size = rd(pointer + CMD_DB_POINTER_OFFSET + 4);
	if (at == 0 || (at & 0xfff) != 0 || size < sizeof(struct cmd_db_header) || size > CMD_DB_MOST) {
		say("cam6t: RPMh: no command database where AOP says (%#x, %u bytes)\n", at, size);
		return false;
	}
	cmd_db = ml_io_map(at, (size + 0xfff) & ~0xfffu);
	cmd_db_size = size;
	db = (const struct cmd_db_header *)cmd_db;
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		say("cam6t: RPMh: no command database at %#x\n", at);
		cmd_db = 0;
		return false;
	}
	return true;
}

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || sizeof(*db) + slave->header_offset + (uint32_t)slave->count * sizeof(*entry) > cmd_db_size) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		spin_us(100);
	}
	say("cam6t: RPMh: timeout (register %#x = %#x)\n", reg, rd(TCS(reg)));
	return false;
}

static bool
rpmh_write_held(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			say("cam6t: RPMh: the command slots stay busy\n");
			return false;
		}
		spin_us(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);
	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	bool done;

	lck_mtx_lock(&hakios_rsc_lock);
	done = rpmh_write_held(address, data, count);
	lck_mtx_unlock(&hakios_rsc_lock);
	return done;
}

static bool
find_rails(void)
{
	if (!find_cmd_db()) {
		return false;
	}
	bob = rpmh_address(BOB_NAME);
	lvs1 = rpmh_address(LVS1_NAME);
	say("cam6t: RPMh: %s at %#x, %s at %#x\n", BOB_NAME, bob, LVS1_NAME, lvs1);
	/* (regulators, "VRM", have 4 in bits 19:16) */
	if (bob == 0 || (bob >> 16 & 0xf) != 4 || lvs1 == 0 || (lvs1 >> 16 & 0xf) != 4) {
		say("cam6t: RPMh: not both regulators found: nothing is powered\n");
		return false;
	}
	if (rsc == 0) {
		rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	}
	return true;
}

static bool
bob_on(void)
{
	uint32_t address[3] = { bob + VRM_VOLTAGE, bob + VRM_MODE, bob + VRM_ENABLE }, data[3] = { BOB_MV, BOB_MODE_AUTO, 1 };

	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	say("cam6t: BOB voted on (%u mV, auto)\n", BOB_MV);
	return true;
}

static bool
lvs1_on(void)
{
	uint32_t address[1] = { lvs1 + VRM_ENABLE }, data[1] = { 1 };

	if (!rpmh_write(address, data, 1)) {
		return false;
	}
	say("cam6t: LVS1 (VIO, 1.8 V) voted on\n");
	return true;
}

/* ---- power up, ask, power down ---------------------------------------------------------------------------------- */

static const struct sensor *up;         /* the sensor powered up now, if one is */

static void
power_down(void)
{
	const struct sensor *s = up;

	if (s == NULL) {
		return;
	}
	drive(s->reset, false);
	spin_us(1000);
	mclk_off(s->mclk);
	mclk_pin(s->mclk_pin, false);
	spin_us(1000);
	/* (VIO, LVS1, stays voted: see the top) */
	drive(s->vdig, false);
	spin_us(1000);
	drive(s->vana, false);
	spin_us(1000);
	up = NULL;
}

static bool
power_up(const struct sensor *s)
{
	up = s;
	drive(s->reset, false);
	drive(s->vana, true);
	wait_ms(1);
	drive(s->vdig, true);
	wait_ms(1);
	if (!lvs1_on()) {
		return false;
	}
	wait_ms(1);
	mclk_pin(s->mclk_pin, true);
	if (!mclk_on(s->mclk)) {
		return false;
	}
	wait_ms(1);
	drive(s->reset, true);
	wait_ms(1);
	say("cam6t: the %s (%s) powered up: VANA gpio%u, VDIG gpio%u, VIO, mclk%u, reset gpio%u high\n", s->name, s->where, s->vana,
	    s->vdig, s->mclk, s->reset);
	return true;
}

static void
ask(const struct sensor *s)
{
	uint16_t id = 0;
	unsigned go;

	if (!power_up(s)) {
		say("cam6t: the %s NOT POWERED UP\n", s->name);
		power_down();
		return;
	}
	for (go = 0; go < 20 && !read16(s, ID_REGISTER, &id); go++) {
		wait_ms(2);
	}
	if (go == 20) {
		say("cam6t: the %s (%s) DOES NOT ANSWER at %#04x on gpio%u/%u\n", s->name, s->where, s->address, s->sda, s->scl);
	} else {
		say("cam6t: THE %s (%s) ANSWERS at %#04x on gpio%u/%u%s: chip ID %#06x at %#06x -- %s\n", s->name, s->where, s->address,
		    s->sda, s->scl, go ? " (after more goes)" : "", id, ID_REGISTER, id == s->id ? "as Android finds it" : "NOT what Android finds");
	}
	power_down();
	say("cam6t: the %s powered down\n", s->name);
}

static void
cam6t_thread(__unused void *arg, __unused wait_result_t wr_)
{
	bool clocks;

	delay_for_interval(after_s * 1000, NSEC_PER_MSEC);
	map_pins();
	say("cam6t: level %u%s\n", level, level >= 2 ? (which >= SENSORS ? ", all three sensors" : "") : ", reading only");
	show_pins();
	clocks = gcc_camera_ok();
	if (clocks) {
		camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
		show_camcc();
	} else {
		say("cam6t: GCC's camera AHB or XO clock is off: camcc is not read\n");
	}
	if (!find_rails() || level < 2) {
		say("cam6t: done\n");
		thread_terminate(current_thread());
		return;
	}
	if (!clocks && (!gcc_camera_on() || (camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE)) == 0)) {
		say("cam6t: GCC's camera clocks do not come on: nothing is powered\n");
		thread_terminate(current_thread());
		return;
	}
	if (!bob_on() || !gdsc_on() || (!use_xo && !pll2_up())) {
		say("cam6t: the camera's supplies or clocks did not come up: no sensor is powered\n");
	} else {
		for (unsigned i = 0; i < SENSORS; i++) {
			if (which >= SENSORS || which == i) {
				ask(&sensors[i]);
			}
		}
	}
	pll2_down();
	gdsc_off();
	pins_give_back();
	say("cam6t: done (the camera's clocks off again; BOB and LVS1 left voted)\n");
	thread_terminate(current_thread());
}

void
hakios_cam6t_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cam6t", &level, sizeof(level)) || level == 0) {
		return;
	}
	(void)PE_parse_boot_argn("cam6t_sensor", &which, sizeof(which));
	(void)PE_parse_boot_argn("cam6t_after", &after_s, sizeof(after_s));
	(void)PE_parse_boot_argn("cam6t_xo", &use_xo, sizeof(use_xo));
	if (kernel_thread_start(cam6t_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end, interrupts off: a sensor still up powered down, the clocks this driver started stopped, the pins back. */
void
hakios_cam6t_quiesce(void)
{
	if (!pins_mapped) {
		return;
	}
	if (up != NULL) {
		power_down();
	}
	if (camcc != 0) {
		for (unsigned n = 0; n < 3; n++) {
			mclk_off(n);
		}
		pll2_down();
		gdsc_off();
	}
	pins_give_back();
}
