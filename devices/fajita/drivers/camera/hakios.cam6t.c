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
 *   cam6t=3   ... and the camera subsystem's own blocks reached: the bus clocks and the lite path's (IFE-lite,
 *             CSID-lite, cphy_rx) on, from the crystal, and the blocks' hardware versions read (titan's CPAS, the
 *             camera NoC, IFE-lite, CSID-lite); then off. No sensor is asked unless cam6t_sensor= says one.
 *   cam6t=4   ... and instead of the look, a frame of the CSID's test pattern (colour bars, RAW10, 640x480) through
 *             IFE-lite into memory, through an SMMU context of ours; the loader hands it to the host (xl camera).
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
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);
extern lck_mtx_t hakios_rsc_lock;               /* hakios.wifi.c: one RPMh request at a time */

static uint32_t level, which, after_s = 50, use_xo;
static bool which_given;

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

/* ---- cam6t=3: the camera subsystem's own blocks reached (camss.c's csid_res_845 / vfe_res_845, the lite path) ------- */

/*
 * Before anything streams, the blocks are only read: their hardware version, with their clocks on, all of them
 * from the crystal (19.2 MHz: enough to reach the registers; Android's rates come with streaming). The bus clocks
 * first (soc_ahb; slow_ahb_src for cpas_ahb; camnoc_axi riding GCC's camera_axi), then the lite path: ife_lite,
 * ife_lite_csid, ife_lite_cphy_rx (and its source cphy_rx_src). A block is read only when every branch it needs
 * says it runs: an access to a block without its clocks hangs the bus.
 */
#define CAMCC_SLOW_AHB_RCG      0x6054
#define CAMCC_CPHY_RX_RCG       0x9060
#define CAMCC_IFE_LITE_RCG      0xb004
#define CAMCC_IFE_LITE_CSID_RCG 0xb024
#define CAMCC_SOC_AHB_CBCR      0xb13c
#define CAMCC_CPAS_AHB_CBCR     0xb11c
#define CAMCC_CAMNOC_AXI_CBCR   0xb124
#define CAMCC_IFE_LITE_CBCR     0xb01c
#define CAMCC_IFE_LITE_CSID_CBCR 0xb03c
#define CAMCC_IFE_LITE_CPHY_CBCR 0xb044
#define CPAS_PHYS               0x0ac40000UL    /* cam_cpas_top: titan's version at 0 */
#define CAMNOC_PHYS             0x0ac42000UL
#define VFE_LITE_PHYS           0x0acc4000UL
#define CSID_LITE_PHYS          0x0acc8000UL

static const struct {
	uint32_t cbcr;
	const char *name;
} blocks_branches[] = {
	{ CAMCC_SOC_AHB_CBCR, "soc_ahb" }, { CAMCC_CPAS_AHB_CBCR, "cpas_ahb" }, { CAMCC_CAMNOC_AXI_CBCR, "camnoc_axi" },
	{ CAMCC_IFE_LITE_CBCR, "ife_lite" }, { CAMCC_IFE_LITE_CSID_CBCR, "ife_lite_csid" },
	{ CAMCC_IFE_LITE_CPHY_CBCR, "ife_lite_cphy_rx" },
};
#define BLOCKS_BRANCHES         (sizeof(blocks_branches) / sizeof(blocks_branches[0]))
static bool branch_ours[BLOCKS_BRANCHES], camera_axi_ours;

/* an RCG with no M/N to the crystal, /1 */
static bool
rcg_xo(uint32_t rcg, const char *name)
{
	wr(camcc + rcg + RCG_CFG, 0);
	wr(camcc + rcg + RCG_CMD, rd(camcc + rcg + RCG_CMD) | 1);
	for (int i = 0; rd(camcc + rcg + RCG_CMD) & 1; i++) {
		if (i > 1000) {
			say("cam6t: %s: the RCG does not take the crystal (cmd %#010x)\n", name, rd(camcc + rcg + RCG_CMD));
			return false;
		}
		spin_us(1);
	}
	return true;
}

static bool
branch_on(unsigned k)
{
	vm_offset_t b = camcc + blocks_branches[k].cbcr;

	if (!(rd(b) & CBCR_ENABLE)) {
		branch_ours[k] = true;
		wr(b, rd(b) | CBCR_ENABLE);
	}
	for (int i = 0; rd(b) & CBCR_OFF; i++) {
		if (i > 1000) {
			say("cam6t: %s stays off (%#010x)\n", blocks_branches[k].name, rd(b));
			return false;
		}
		spin_us(1);
	}
	return true;
}

static void
blocks_off(void)
{
	for (unsigned k = BLOCKS_BRANCHES; k-- > 0;) {
		if (branch_ours[k]) {
			wr(camcc + blocks_branches[k].cbcr, rd(camcc + blocks_branches[k].cbcr) & ~CBCR_ENABLE);
			branch_ours[k] = false;
		}
	}
	if (camera_axi_ours) {
		wr(gcc + (GCC_CAMERA_AXI_CBCR & 0xfff), rd(gcc + (GCC_CAMERA_AXI_CBCR & 0xfff)) & ~CBCR_ENABLE);
		camera_axi_ours = false;
	}
}

/* the bus clocks and the lite path's on (see above); false, with them off again, when one does not run */
static bool
blocks_on(bool quiet)
{
	vm_offset_t axi = gcc + (GCC_CAMERA_AXI_CBCR & 0xfff);
	bool all = true;

	if (!(rd(axi) & CBCR_ENABLE)) {
		camera_axi_ours = true;
		wr(axi, rd(axi) | CBCR_ENABLE);
		spin_us(10);
	}
	say("cam6t: GCC camera_axi %#010x\n", rd(axi));
	if (!rcg_xo(CAMCC_SLOW_AHB_RCG, "slow_ahb_src") || !rcg_xo(CAMCC_CPHY_RX_RCG, "cphy_rx_src") ||
	    !rcg_xo(CAMCC_IFE_LITE_RCG, "ife_lite_src") || !rcg_xo(CAMCC_IFE_LITE_CSID_RCG, "ife_lite_csid_src")) {
		blocks_off();
		return false;
	}
	for (unsigned k = 0; k < BLOCKS_BRANCHES; k++) {
		bool on = branch_on(k);

		if (!quiet || !on) {
			say("cam6t: %s %s (%#010x)\n", blocks_branches[k].name, on ? "runs" : "DOES NOT RUN",
			    rd(camcc + blocks_branches[k].cbcr));
		}
		all = all && on;
	}
	if (!all) {
		say("cam6t: not every clock runs: the blocks are not touched\n");
		blocks_off();
	}
	return all;
}

static void
blocks_look(void)
{
	if (blocks_on(false)) {
		vm_offset_t cpas = ml_io_map(CPAS_PHYS, 0x1000), camnoc = ml_io_map(CAMNOC_PHYS, 0x1000);
		vm_offset_t vfe = ml_io_map(VFE_LITE_PHYS, 0x1000), csid = ml_io_map(CSID_LITE_PHYS, 0x1000);

		say("cam6t: THE CAMERA'S BLOCKS: titan (cpas top) version %#010x, camnoc %#010x, IFE-lite %#010x, CSID-lite %#010x\n",
		    rd(cpas), rd(camnoc), rd(vfe), rd(csid));
	}
	blocks_off();
}

/* ---- cam6t=4: the CSID's test pattern into memory ------------------------------------------------------------- */

/*
 * The CSID-lite's own pattern generator (colour bars, RAW10, 640x480, frames without end) into its RDI 0, which
 * feeds IFE-lite's write master 0 (camss-csid-gen2.c, camss-vfe-17x.c; redwood's hakios.camera.c does the same on
 * Titan 570, whose registers here are the same). The write master's addresses are 32 bits and go through the apps
 * SMMU (the hypervisor grants no bypass: hakios.wifi6t.c): a free context of ours with tables that put the frames'
 * pages at FRAME_ADDRESSES, and free match registers sending the IFE's four streams (the tree's msm_cam_smmu_ife:
 * 0x808, 0x810/8, 0xc08, 0xc10/8) through it. The frames' memory, with redwood's header, is handed to the loader
 * for the host (xl camera; tools/camraw.py makes pictures of it).
 */
#define APPS_SMMU_PHYS          0x15000000UL
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define CAM_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define FRAME_ADDRESSES         0x40000000ull
#define IFE_STREAMS             4

static const struct {
	uint16_t stream, mask;
} ife_streams[IFE_STREAMS] = { { 0x808, 0 }, { 0x810, 8 }, { 0xc08, 0 }, { 0xc10, 8 } };

#define CAM_HEADER_BYTES        4096
#define CAM_FRAMES              2
#define TPG_WIDTH               640
#define TPG_HEIGHT              480
#define TPG_STRIDE              (TPG_WIDTH * 10 / 8)

struct cam_header {                     /* redwood's (tools/camraw.py) */
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height, stride;
	uint32_t        format;         /* 1: MIPI RAW10, packed */
	uint32_t        bayer;          /* 0 RGGB */
	uint32_t        frames;
	uint32_t        frame_bytes;
	uint32_t        source;         /* 0: the CSID's test pattern */
	char            sensor[16];
	uint32_t        last;
};

extern uint64_t loader_recording[3];
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;

static vm_offset_t smmu, smmu_second, smmu_context, frames, page_tables;
static uint64_t frames_bytes;
static uint32_t smmu_ctx, smmu_match[IFE_STREAMS], smmu_old_smr[IFE_STREAMS], smmu_old_s2cr[IFE_STREAMS];
static unsigned smmu_matches;
static bool smmu_on;

static bool
frames_alloc(void)
{
	struct cam_header *h;
	uint32_t frame_bytes = TPG_STRIDE * TPG_HEIGHT;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (frames == 0 && kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		frames = 0;
		say("cam6t: no memory for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = TPG_WIDTH;
	h->height = TPG_HEIGHT;
	h->stride = TPG_STRIDE;
	h->format = 1;
	h->bayer = 0;
	h->frame_bytes = frame_bytes;
	h->source = 0;
	strlcpy(h->sensor, "CSID TPG", sizeof(h->sensor));
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(frames);
	loader_recording[1] = frames_bytes;
	loader_recording[2] = (uint64_t)frames;
	say("cam6t: %llu KB for %u frames at %#llx, handed to the loader for the host\n", frames_bytes >> 10, CAM_FRAMES,
	    loader_recording[0]);
	return true;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("cam6t: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		return false;
	}
	return true;
}

static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t idr0, idr1, page, pages;

	if (smmu == 0) {
		smmu = ml_io_map(APPS_SMMU_PHYS, 0x1000);
	}
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	if (2 * pages * page != 0x80000) {
		say("cam6t: the apps SMMU's layout is not the SDM845's (idr1 %#x)\n", idr1);
		return false;
	}
	smmu_matches = 0;
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (smr >> 31) {
			for (unsigned k = 0; k < IFE_STREAMS; k++) {
				if ((((smr & 0x7fff) ^ ife_streams[k].stream) & ~((smr >> 16 & 0x7fff) | ife_streams[k].mask)) == 0) {
					say("cam6t: SMMU match register %u (%#x, s2cr %#x) has the IFE's stream %#x already: left alone\n", n,
					    smr, s2cr, ife_streams[k].stream);
					return false;
				}
			}
			if ((s2cr >> 16 & 3) == 0) {
				used[s2cr & 0xff] = true;
			}
		} else if (smmu_matches < IFE_STREAMS) {
			smmu_match[smmu_matches] = n;
			smmu_old_smr[smmu_matches] = smr;
			smmu_old_s2cr[smmu_matches] = s2cr;
			smmu_matches++;
		}
	}
	for (smmu_ctx = 0; smmu_ctx < (idr1 & 0xff) && used[smmu_ctx]; smmu_ctx++) {
	}
	if (smmu_matches < IFE_STREAMS || smmu_ctx >= (idr1 & 0xff)) {
		say("cam6t: the apps SMMU has not %u match registers and a context free (%u, context %u)\n", IFE_STREAMS,
		    smmu_matches, smmu_ctx);
		smmu_matches = 0;
		return false;
	}
	smmu_second = ml_io_map(APPS_SMMU_PHYS + page, 0x1000);
	smmu_context = ml_io_map(APPS_SMMU_PHYS + (uint64_t)(pages + smmu_ctx) * page, 0x1000);
	if (rd(smmu_context + SMMU_CB_SCTLR) & 1) {
		say("cam6t: SMMU context %u is on although no stream uses it: somebody's\n", smmu_ctx);
		smmu_matches = 0;
		return false;
	}
	return true;
}

static bool
smmu_up(void)
{
	uint64_t *root, *second, table_root;
	uint32_t thirds = (uint32_t)((frames_bytes + 0x1fffff) >> 21), npages = (uint32_t)(frames_bytes >> 12);
	bool ok;

	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (page_tables == 0 && kmem_alloc(kernel_map, &page_tables, (2 + thirds) * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		page_tables = 0;
		smmu_matches = 0;
		lck_mtx_unlock(&hakios_smmu_lock);
		say("cam6t: no memory for the SMMU's tables\n");
		return false;
	}
	root = (uint64_t *)page_tables;
	second = (uint64_t *)(page_tables + 4096);
	root[FRAME_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(page_tables + (2 + t) * 4096);

		second[(FRAME_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < npages; p++) {
			third[p] = kvtophys(frames + 4096ull * (t * 512 + p)) | CAM_PTE;
		}
	}
	for (vm_offset_t line = page_tables; line < page_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(page_tables) | (uint64_t)smmu_ctx << 48;
	ok = smmu_put(smmu_context, SMMU_CB_SCTLR, 0, ~0u) && smmu_put(smmu_second, SMMU_CBA2R(smmu_ctx), 1, 1) &&
	    smmu_put(smmu_second, SMMU_CBAR(smmu_ctx), SMMU_CBAR_STAGE_1, 0x30000) &&
	    smmu_put(smmu_context, SMMU_CB_TCR2, SMMU_TCR2, 0) && smmu_put(smmu_context, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_TTBR1, 0, ~0u) && smmu_put(smmu_context, SMMU_CB_TTBR1 + 4, 0, ~0u) &&
	    smmu_put(smmu_context, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) && smmu_put(smmu_context, SMMU_CB_MAIR1, 0, ~0u);
	if (ok) {
		wr(smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_ctx);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
		for (unsigned waited = 0; ok && (rd(smmu_context + SMMU_CB_TLBSTATUS) & 1); waited++) {
			ok = waited < 1000;
			spin_us(10);
		}
		ok = ok && smmu_put(smmu_context, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1);
	}
	smmu_on = ok;
	for (unsigned k = 0; ok && k < IFE_STREAMS; k++) {
		uint32_t smr = 1u << 31 | (uint32_t)ife_streams[k].mask << 16 | ife_streams[k].stream;

		ok = smmu_put(smmu, SMMU_S2CR(smmu_match[k]), smmu_ctx, 0x300ff) && smmu_put(smmu, SMMU_SMR(smmu_match[k]), smr, ~0u);
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	if (ok) {
		say("cam6t: SMMU: context %u maps %llu KB at %#llx; match registers %u %u %u %u send the IFE's streams through it\n",
		    smmu_ctx, frames_bytes >> 10, FRAME_ADDRESSES, smmu_match[0], smmu_match[1], smmu_match[2], smmu_match[3]);
	} else {
		say("cam6t: THE SMMU DID NOT TAKE ITS SETTING\n");
	}
	return ok;
}

static void
smmu_faults(void)
{
	uint32_t status, global;

	if (!smmu_on) {
		return;
	}
	status = rd(smmu_context + SMMU_CB_FSR);
	global = rd(smmu + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("cam6t: the SMMU refused the IFE nothing (context %u)\n", smmu_ctx);
		return;
	}
	say("cam6t: THE SMMU REFUSED A REQUEST: context %u status %#x, address %#llx, syndrome %#x; global %#x, stream %#x\n", smmu_ctx,
	    status, (uint64_t)rd(smmu_context + SMMU_CB_FAR + 4) << 32 | rd(smmu_context + SMMU_CB_FAR),
	    rd(smmu_context + SMMU_CB_FSYNR0), global, rd(smmu + SMMU_SGFSYNR1));
}

/* back as found; with interrupts off (the run's end) the lock is not taken */
static void
smmu_down(void)
{
	bool lock = ml_get_interrupts_enabled();

	if (!smmu_on && smmu_matches == 0) {
		return;
	}
	if (lock) {
		lck_mtx_lock(&hakios_smmu_lock);
	}
	for (unsigned k = 0; k < smmu_matches; k++) {
		wr(smmu + SMMU_SMR(smmu_match[k]), smmu_old_smr[k]);
		wr(smmu + SMMU_S2CR(smmu_match[k]), smmu_old_s2cr[k]);
	}
	if (smmu_on) {
		wr(smmu_context + SMMU_CB_SCTLR, 0);
		wr(smmu_context + SMMU_CB_TLBIASID, smmu_ctx);
		wr(smmu_context + SMMU_CB_TLBSYNC, 1);
	}
	if (lock) {
		lck_mtx_unlock(&hakios_smmu_lock);
	}
	smmu_on = false;
	smmu_matches = 0;
}

/* CSID-lite and IFE-lite (redwood's offsets, which are Titan 170's: camss-csid-gen2.c, camss-vfe-17x.c) */
#define CSID_HW_VERSION         0x000
#define CSID_RST_STROBES        0x010
#define CSID_CSI2_RX_IRQ_STATUS 0x020
#define CSID_CSI2_RX_IRQ_MASK   0x024
#define CSID_CSI2_RX_IRQ_CLEAR  0x028
#define CSID_RDI0_IRQ_STATUS    0x030
#define CSID_RDI0_IRQ_MASK      0x034
#define CSID_RDI0_IRQ_CLEAR     0x038
#define CSID_TOP_IRQ_STATUS     0x070
#define CSID_TOP_IRQ_MASK       0x074
#define CSID_TOP_IRQ_CLEAR      0x078
#define CSID_IRQ_CMD            0x080
#define CSID_CSI2_RX_CFG0       0x100
#define CSID_CSI2_RX_CFG1       0x104
#define CSID_CSI2_RX_PKTS       0x160
#define CSID_RDI0_CFG0          0x200
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208
#define CSID_RDI0_FRM_DROP_PATTERN 0x20c
#define CSID_RDI0_FRM_DROP_PERIOD  0x210
#define CSID_RDI0_IRQ_SUB_PATTERN  0x214
#define CSID_RDI0_IRQ_SUB_PERIOD   0x218
#define CSID_RDI0_PIX_DROP_PATTERN 0x224
#define CSID_RDI0_PIX_DROP_PERIOD  0x228
#define CSID_RDI0_LINE_DROP_PATTERN 0x22c
#define CSID_RDI0_LINE_DROP_PERIOD  0x230
#define CSID_RDI0_STATUS        0x250
#define CSID_RDI0_BYTES_PING    0x2e0
#define CSID_TPG_CTRL           0x600
#define CSID_TPG_VC_CFG0        0x604
#define CSID_TPG_VC_CFG1        0x608
#define CSID_TPG_LFSR_SEED      0x60c
#define CSID_TPG_DT0_CFG0       0x610
#define CSID_TPG_DT0_CFG1       0x614
#define CSID_TPG_DT0_CFG2       0x618
#define CSID_TPG_COLOR_BARS_CFG 0x640
#define CSID_TPG_COLOR_BOX_CFG  0x644
#define RAW10_DT                0x2b

#define IFE_HW_VERSION          0x0000
#define IFE_GLOBAL_RESET        0x0018
#define IFE_IRQ_GLOBAL_CLEAR    0x0058
#define IFE_IRQ_MASK0           0x005c
#define IFE_IRQ_MASK1           0x0060
#define IFE_IRQ_CLEAR0          0x0064
#define IFE_IRQ_CLEAR1          0x0068
#define IFE_IRQ_STATUS0         0x006c
#define IFE_IRQ_STATUS1         0x0070
#define IFE_VIOLATION_STATUS    0x007c
#define IFE_REG_UPDATE          0x04ac
#define BUS_HW_VERSION          0x2000
#define BUS_CGC_OVD             0x200c
#define BUS_IRQ_MASK1           0x2048
#define BUS_IRQ_STATUS0         0x205c
#define BUS_IRQ_STATUS1         0x2060
#define BUS_IRQ_STATUS2         0x2064
#define BUS_ADDR_SYNC_FRAME_HDR 0x2080
#define BUS_ADDR_SYNC_NO_SYNC   0x2084
#define BUS_TEST_CTRL           0x211c
#define WM0_STATUS0             0x2200
#define WM0_STATUS1             0x2204
#define WM0_CFG                 0x2208
#define WM0_IMAGE_ADDR          0x2214
#define WM0_WIDTH               0x221c
#define WM0_HEIGHT              0x2220
#define WM0_PACKER              0x2224
#define WM0_STRIDE              0x2228
#define WM0_IRQ_SUB_PERIOD      0x2248
#define WM0_IRQ_SUB_PATTERN     0x224c
#define WM0_FRAMEDROP_PERIOD    0x2250
#define WM0_FRAMEDROP_PATTERN   0x2254
#define WM0_FRAME_INC           0x2258
#define WM0_BURST_LIMIT         0x225c

static vm_offset_t csid_base, ife_base;
static bool streaming;

static bool
wait_bits_at(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i < us; i += 10) {
		if ((rd(base + offset) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

static bool
csid_reset(void)
{
	wr(csid_base + CSID_CSI2_RX_IRQ_MASK, 0);
	wr(csid_base + CSID_RDI0_IRQ_MASK, 0);
	wr(csid_base + CSID_TOP_IRQ_CLEAR, 1);
	wr(csid_base + CSID_CSI2_RX_IRQ_CLEAR, 0xfffffff);
	wr(csid_base + CSID_RDI0_IRQ_CLEAR, 0x7fff);
	wr(csid_base + CSID_IRQ_CMD, 1);
	wr(csid_base + CSID_TOP_IRQ_MASK, 1);
	wr(csid_base + CSID_IRQ_CMD, 1);
	wr(csid_base + CSID_RST_STROBES, 0x1e);         /* (camss-csid-gen2.c: the registers kept) */
	if (!wait_bits_at(csid_base, CSID_TOP_IRQ_STATUS, 1, 1, 100000)) {
		say("cam6t: the CSID does not reset (top irq %#x, version %#x)\n", rd(csid_base + CSID_TOP_IRQ_STATUS),
		    rd(csid_base + CSID_HW_VERSION));
		return false;
	}
	wr(csid_base + CSID_TOP_IRQ_CLEAR, 1);
	wr(csid_base + CSID_IRQ_CMD, 1);
	say("cam6t: CSID-lite reset (version %#x)\n", rd(csid_base + CSID_HW_VERSION));
	return true;
}

static bool
ife_config(void)
{
	uint32_t frame_bytes = TPG_STRIDE * TPG_HEIGHT;

	wr(ife_base + IFE_IRQ_MASK0, 1u << 31);
	wr(ife_base + IFE_GLOBAL_RESET, 0x3f9f);
	if (!wait_bits_at(ife_base, IFE_IRQ_STATUS0, 1u << 31, 1u << 31, 100000)) {
		say("cam6t: IFE-lite does not reset (irq %#x)\n", rd(ife_base + IFE_IRQ_STATUS0));
		return false;
	}
	wr(ife_base + IFE_IRQ_CLEAR0, ~0u);
	wr(ife_base + IFE_IRQ_CLEAR1, ~0u);
	wr(ife_base + IFE_IRQ_GLOBAL_CLEAR, 1);
	say("cam6t: IFE-lite reset (version %#x, bus %#x)\n", rd(ife_base + IFE_HW_VERSION), rd(ife_base + BUS_HW_VERSION));
	wr(ife_base + BUS_ADDR_SYNC_FRAME_HDR, 0);
	wr(ife_base + BUS_CGC_OVD, 0xfffff);
	wr(ife_base + BUS_TEST_CTRL, 0);
	wr(ife_base + BUS_ADDR_SYNC_NO_SYNC, 0xffffff);
	wr(ife_base + WM0_BURST_LIMIT, 0xf);
	wr(ife_base + WM0_WIDTH, 0xff01);
	wr(ife_base + WM0_HEIGHT, 0);
	wr(ife_base + WM0_PACKER, 0);
	wr(ife_base + WM0_STRIDE, 0xff01);
	wr(ife_base + WM0_FRAMEDROP_PERIOD, 0);
	wr(ife_base + WM0_FRAMEDROP_PATTERN, 1);
	wr(ife_base + WM0_IRQ_SUB_PERIOD, 0);
	wr(ife_base + WM0_IRQ_SUB_PATTERN, 1);
	wr(ife_base + BUS_IRQ_MASK1, 1);
	wr(ife_base + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + CAM_HEADER_BYTES));
	wr(ife_base + WM0_FRAME_INC, frame_bytes);
	wr(ife_base + WM0_CFG, 3);                      /* on, MIPI RAW: the frames as they come, packed */
	/* (the image address reads 0 until a register update takes it: only the configuration is read back) */
	if (rd(ife_base + WM0_CFG) != 3) {
		say("cam6t: IFE-lite's write master 0 did not take its setting (cfg %#x, address %#x)\n", rd(ife_base + WM0_CFG),
		    rd(ife_base + WM0_IMAGE_ADDR));
		return false;
	}
	wr(ife_base + IFE_REG_UPDATE, 0x2);             /* RDI 0's register update */
	return true;
}

static bool
csid_config(void)
{
	uint32_t vc = 0, cfg0 = vc << 22 | RAW10_DT << 16 | 0xfu << 12 | 1u << 2 | 3u;

	wr(csid_base + CSID_CSI2_RX_CFG0, 3u | 0x3210u << 4);          /* four lanes, phy 0: the generator's */
	wr(csid_base + CSID_CSI2_RX_CFG1, 1u << 6 | 1u);
	/* camss-csid-gen2.c's __csid_configure_testgen(): one data type, frames without end, colour bars */
	wr(csid_base + CSID_TPG_VC_CFG0, vc | 1u << 10);
	wr(csid_base + CSID_TPG_VC_CFG1, 0x740u | 0x3ffu << 12);
	wr(csid_base + CSID_TPG_LFSR_SEED, 0x12345678);
	wr(csid_base + CSID_TPG_DT0_CFG0, TPG_HEIGHT | (uint32_t)TPG_WIDTH << 16);
	wr(csid_base + CSID_TPG_DT0_CFG1, RAW10_DT);
	wr(csid_base + CSID_TPG_DT0_CFG2, 8u | 0xbeu << 4 | 2u << 16);   /* colour bars, RAW10 */
	wr(csid_base + CSID_TPG_COLOR_BARS_CFG, 0);
	wr(csid_base + CSID_TPG_COLOR_BOX_CFG, 0);
	/* __csid_configure_rdi_stream() */
	wr(csid_base + CSID_RDI0_CFG0, cfg0);
	wr(csid_base + CSID_RDI0_CFG1, 2);
	wr(csid_base + CSID_RDI0_FRM_DROP_PERIOD, 1);
	wr(csid_base + CSID_RDI0_FRM_DROP_PATTERN, 0);
	wr(csid_base + CSID_RDI0_IRQ_SUB_PERIOD, 1);
	wr(csid_base + CSID_RDI0_IRQ_SUB_PATTERN, 0);
	wr(csid_base + CSID_RDI0_PIX_DROP_PERIOD, 1);
	wr(csid_base + CSID_RDI0_PIX_DROP_PATTERN, 0);
	wr(csid_base + CSID_RDI0_LINE_DROP_PERIOD, 1);
	wr(csid_base + CSID_RDI0_LINE_DROP_PATTERN, 0);
	wr(csid_base + CSID_RDI0_CTRL, 0);
	wr(csid_base + CSID_RDI0_CFG0, cfg0 | 1u << 31);
	if (rd(csid_base + CSID_RDI0_CFG0) != (cfg0 | 1u << 31)) {
		say("cam6t: the CSID's RDI 0 did not take its setting (%#x)\n", rd(csid_base + CSID_RDI0_CFG0));
		return false;
	}
	wr(csid_base + CSID_RDI0_CTRL, 1);              /* resume at the frame's boundary */
	return true;
}

static void
show_paths(const char *when)
{
	say("cam6t: %s: CSID rx irq %#x, packets %u, RDI0 irq %#x status %#x bytes %u; IFE irq %#x %#x violation %#x, bus %#x %#x %#x, "
	    "wm0 status %#x %#x\n", when, rd(csid_base + CSID_CSI2_RX_IRQ_STATUS), rd(csid_base + CSID_CSI2_RX_PKTS),
	    rd(csid_base + CSID_RDI0_IRQ_STATUS), rd(csid_base + CSID_RDI0_STATUS), rd(csid_base + CSID_RDI0_BYTES_PING),
	    rd(ife_base + IFE_IRQ_STATUS0), rd(ife_base + IFE_IRQ_STATUS1), rd(ife_base + IFE_VIOLATION_STATUS),
	    rd(ife_base + BUS_IRQ_STATUS0), rd(ife_base + BUS_IRQ_STATUS1), rd(ife_base + BUS_IRQ_STATUS2), rd(ife_base + WM0_STATUS0),
	    rd(ife_base + WM0_STATUS1));
}

static void
stream_stop(void)
{
	if (!streaming) {
		return;
	}
	wr(csid_base + CSID_TPG_CTRL, 0);
	wr(csid_base + CSID_RDI0_CTRL, 0);              /* halt at the frame's boundary */
	wr(csid_base + CSID_RDI0_CFG0, rd(csid_base + CSID_RDI0_CFG0) & ~(1u << 31));
	wr(ife_base + WM0_CFG, 0);
	wr(ife_base + IFE_REG_UPDATE, 0x2);
	spin_us(2000);
	streaming = false;
}

static void
capture_tpg(void)
{
	struct cam_header *h;
	unsigned ms;

	if (csid_base == 0) {
		csid_base = ml_io_map(CSID_LITE_PHYS, 0x1000);
		ife_base = ml_io_map(VFE_LITE_PHYS, 0x4000);
	}
	if (!frames_alloc() || !blocks_on(true)) {
		return;
	}
	if (!smmu_up() || !csid_reset() || !ife_config() || !csid_config()) {
		smmu_down();
		blocks_off();
		return;
	}
	streaming = true;
	show_paths("configured");
	wr(csid_base + CSID_TPG_CTRL, 7u | 3u << 4 | 0x64u << 8 | 0xau << 20);  /* on, frame start and end packets, 4 lanes */
	h = (struct cam_header *)frames;
	for (ms = 0; ms < 3000; ms += 50) {
		wait_ms(50);
		if (rd(ife_base + BUS_IRQ_STATUS1) & 1) {
			h->frames = 1;
			break;
		}
	}
	say("cam6t: %s\n", h->frames ? "WRITE MASTER 0 SAYS A FRAME OF THE TEST PATTERN IS IN MEMORY" :
	    "no frame done in 3 s");
	if (h->frames) {
		say("cam6t: (after %u ms)\n", ms + 50);
	}
	show_paths("after");
	stream_stop();
	smmu_faults();
	smmu_down();
	blocks_off();
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	uint32_t nonzero = 0;

	for (uint32_t i = 0; i < TPG_STRIDE * TPG_HEIGHT; i++) {
		nonzero += p[i] != 0;
	}
	say("cam6t: the frame's first bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; %u of %u bytes not 0 (xl camera fetches it)\n",
	    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], nonzero, (unsigned)(TPG_STRIDE * TPG_HEIGHT));
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
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
		for (unsigned i = 0; i < SENSORS && (level == 2 || which_given); i++) {
			if (which >= SENSORS || which == i) {
				ask(&sensors[i]);
			}
		}
		if (level == 3) {
			blocks_look();
		} else if (level >= 4) {
			capture_tpg();
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
	which_given = PE_parse_boot_argn("cam6t_sensor", &which, sizeof(which));
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
		if (csid_base != 0) {
			stream_stop();
		}
		smmu_down();
		blocks_off();
		pll2_down();
		gdsc_off();
	}
	pins_give_back();
}
