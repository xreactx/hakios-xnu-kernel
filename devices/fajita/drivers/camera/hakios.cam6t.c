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
 *   cam6t=5   ... and instead, a frame of a sensor's own (cam6t_sensor=2 the front IMX371 by default, 1 the IMX376K,
 *             0 the IMX519 over its three C-PHY trios; cam6t_settle=N the PHY's settle count instead of the computed;
 *             cam6t_mode=N its mode), through its CSIPHY, with its tables from the small image's fw/camera-*.tab.
 *   cam6t=6   ... and instead, /dev/hakios-camera for Darwin's camera stack: redwood's cam=5 (sys/hakios.camera.h), the
 *             frames streaming while it is read; camera 1 the IMX519, 2 the IMX371, 3 the IMX376K (see below).
 *   cam6t_after=S   when (seconds after the driver starts; 10): the steps that are not Darwin's (1 to 5) were run at 50, away
 *             from the modem's and the audio DSP's starts; Darwin's sessions start only when the device is read anyway.
 *
 * cam6t=6 is the default (the loader puts the iPhone's camera nodes into the device tree for it: devices/fajita;
 * cam6t=0 leaves the cameras alone, and then the loader does not either).
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
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/hakios.camera.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);
extern lck_mtx_t hakios_rsc_lock;               /* hakios.wifi.c: one RPMh request at a time */

static uint32_t level = 6, which, after_s = 10, use_xo;    /* (cam6t=6, the cameras for Darwin, by default: run 190508) */
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
	{ 31, TILE_NORTH, "IMX519 lens motor 2.8V" }, { 77, TILE_EAST, "IMX376K lens motor 2.8V" },
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

static uint32_t cap_width = TPG_WIDTH, cap_height = TPG_HEIGHT;
/* the write master puts each line on a 16-byte boundary: 2328 pixels of RAW10 (2910 bytes) take 2912 in memory */
#define TPG_STRIDE              (((cap_width * 10 / 8) + 15) & ~15u)
#define TPG_HEIGHT_NOW          cap_height

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
static uint64_t frames_bytes, frames_kept, page_tables_bytes;         /* frames_kept: what cam6t=5 took at boot, while the memory is in one piece */
#define FRAMES_KEPT_BYTES       (16ull << 20)
static uint32_t smmu_ctx, smmu_match[IFE_STREAMS], smmu_old_smr[IFE_STREAMS], smmu_old_s2cr[IFE_STREAMS];
static unsigned smmu_matches;
static bool smmu_on;

static bool
frames_alloc(uint32_t source, const char *name, uint32_t bayer)
{
	struct cam_header *h;
	uint32_t frame_bytes = TPG_STRIDE * TPG_HEIGHT_NOW;

	frames_bytes = round_page(CAM_HEADER_BYTES + (uint64_t)CAM_FRAMES * frame_bytes);
	if (frames != 0 && frames_bytes > (frames_kept ? frames_kept : frames_bytes)) {
		say("cam6t: %u frames want %llu KB, more than the %llu KB taken at boot\n", CAM_FRAMES, frames_bytes >> 10, frames_kept >> 10);
		return false;
	}
	if (frames != 0) {
		bzero((void *)frames, CAM_HEADER_BYTES);
	}
	if (frames == 0 && kmem_alloc_contig(kernel_map, &frames, frames_bytes, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		frames = 0;
		say("cam6t: no memory for %u frames (%llu KB)\n", CAM_FRAMES, frames_bytes >> 10);
		return false;
	}
	h = (struct cam_header *)frames;
	memcpy(h->magic, "RWCAM001", 8);
	h->width = cap_width;
	h->height = cap_height;
	h->stride = TPG_STRIDE;
	h->format = 1;
	h->bayer = bayer;
	h->frame_bytes = frame_bytes;
	h->source = source;
	strlcpy(h->sensor, name, sizeof(h->sensor));
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
	if (page_tables != 0 && page_tables_bytes != (2 + thirds) * 4096ull) {
		kmem_free(kernel_map, page_tables, page_tables_bytes);   /* (another size of frames: tables of their own) */
		page_tables = 0;
	}
	if (page_tables != 0) {
		bzero((void *)page_tables, page_tables_bytes);
	}
	page_tables_bytes = (2 + thirds) * 4096ull;
	if (page_tables == 0 && kmem_alloc(kernel_map, &page_tables, page_tables_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
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

/* the write master to address (as the IFE sees it), each next frame frame_inc on (0: every frame there, until the next update) */
static bool
ife_config_at(uint64_t address, uint32_t frame_inc)
{

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
	wr(ife_base + WM0_IMAGE_ADDR, (uint32_t)address);
	wr(ife_base + WM0_FRAME_INC, frame_inc);
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
ife_config(void)
{
	return ife_config_at(FRAME_ADDRESSES + CAM_HEADER_BYTES, TPG_STRIDE * TPG_HEIGHT_NOW);
}

/*
 * phy < 0: the CSID's own generator; else that CSIPHY's lanes (four D-PHY lanes, or with cphy three C-PHY trios:
 * cam_ife_csid_core.c's rx cfg0, the lane type in bit 24), in lane_assign's order
 */
static bool
csid_config(int phy, uint32_t lane_assign, bool cphy)
{
	uint32_t vc = 0, cfg0 = vc << 22 | RAW10_DT << 16 | 0xfu << 12 | 1u << 2 | 3u;

	wr(csid_base + CSID_CSI2_RX_CFG0, (cphy ? 2u : 3u) | (phy < 0 ? 0x3210u : lane_assign) << 4 |
	    (uint32_t)(phy < 0 ? 0 : phy & 3) << 20 | (uint32_t)cphy << 24);
	wr(csid_base + CSID_CSI2_RX_CFG1, 1u << 6 | 1u);
	if (phy >= 0) {
		goto rdi;
	}
	/* camss-csid-gen2.c's __csid_configure_testgen(): one data type, frames without end, colour bars */
	wr(csid_base + CSID_TPG_VC_CFG0, vc | 1u << 10);
	wr(csid_base + CSID_TPG_VC_CFG1, 0x740u | 0x3ffu << 12);
	wr(csid_base + CSID_TPG_LFSR_SEED, 0x12345678);
	wr(csid_base + CSID_TPG_DT0_CFG0, cap_height | cap_width << 16);
	wr(csid_base + CSID_TPG_DT0_CFG1, RAW10_DT);
	wr(csid_base + CSID_TPG_DT0_CFG2, 8u | 0xbeu << 4 | 2u << 16);   /* colour bars, RAW10 */
	wr(csid_base + CSID_TPG_COLOR_BARS_CFG, 0);
	wr(csid_base + CSID_TPG_COLOR_BOX_CFG, 0);
rdi:
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
	cap_width = TPG_WIDTH;
	cap_height = TPG_HEIGHT;
	if (!frames_alloc(0, "CSID TPG", 0) || !blocks_on(true)) {
		return;
	}
	if (!smmu_up() || !csid_reset() || !ife_config() || !csid_config(-1, 0, false)) {
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

	for (uint32_t i = 0; i < TPG_STRIDE * TPG_HEIGHT_NOW; i++) {
		nonzero += p[i] != 0;
	}
	say("cam6t: the frame's first bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; %u of %u bytes not 0 (xl camera fetches it)\n",
	    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], nonzero, (unsigned)(TPG_STRIDE * TPG_HEIGHT_NOW));
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

/* ---- cam6t=5: a sensor's own frames (the front IMX371 by default) ---------------------------------------------- */

/*
 * The sensor powered as for its chip ID and left up; its tables (fw/camera-<sensor>.tab in the small image, which
 * tools/camsensor.py makes of the module file: userland/build.sh) written over the CCI pins: the initialisation,
 * then a mode's (cam6t_mode=N; 1 for the IMX371, 2304x1728). Its CSIPHY's four D-PHY lanes (camss-csiphy-3ph-1-0.c's
 * SDM845 table, the settle count from the lane rate the mode gives) into CSID-lite, RDI 0, IFE-lite, memory, as the
 * test pattern went; then "stream on". The clocks at Android's rates for this path: cphy_rx and the CSID 384 MHz
 * (PLL3), IFE-lite 320 MHz (PLL2's even output / 1.5), the PHY's timer 240 MHz (PLL2 / 2); the PHY's two supplies
 * (vdda-phy: the PM8998's LDO1 at 0.88 V, vdda-pll: LDO26 at 1.2 V), as Android had them with the front camera on.
 * The IMX519's three C-PHY trios: the downstream driver's tables (phy_config_cphy()).
 */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static uint32_t cam_mode = ~0u, cam_exposure = ~0u, cam_gain = ~0u, cam_settle = ~0u, cam_focus = ~0u;

/*
 * The rear cameras' lens motors (the module files' actuator data; fajita.dtsi's actuator_rear_0/1 on the sensors'
 * CCI masters, their 2.8 V fixed regulators fed by BOB and switched by GPIO 31 and 77). Two makers' chips are fitted
 * to 6Ts, the files say: an LC898217XC (IMX519 at 0xe4, IMX376K at 0xe8: ready when 0xb3 reads 0, the position a
 * 16-bit word at 0x84) or an SA3103 (0x18 and 0x1a: 0x81 <- 1 to start, the position's high byte at 0x82, the low
 * at 0x83). cam6t_focus=N puts the lens at N before the frame (the module files' range: 0 to 4095); 500 by default,
 * where the 6T's IMX519 (an LC898217XC) sees a room sharp (run 184639; 200 and 800 less so), and 0 leaves the motor off.
 */
static const struct {
	uint8_t lc, sa;
	unsigned pin;
} motors[2] = { { 0xe4, 0x18, 31 }, { 0xe8, 0x1a, 77 } };
static bool motor_on;
static unsigned motor_pin;


struct ctab_head {
	char            magic[8];       /* "RWCTAB02" */
	uint32_t        slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes;
};
struct ctab_mode {
	uint32_t        width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};
struct ctab_entry {
	uint16_t        address, data;
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay;
};

static const struct ctab_head *ctab;
static uint64_t ctab_size;

static const char *const tab_names[SENSORS] = { "camera-imx519.tab", "camera-imx376k.tab", "camera-imx371.tab" };
static const uint32_t sensor_csiphy[SENSORS] = { 0, 1, 2 };
static const uint32_t sensor_bayer[SENSORS] = { 0, 0, 0 };     /* RGGB: the module files' colour filter order */

/* a table by its name: its entries and how many; NULL when there is none */
static const struct ctab_entry *
ctab_find(const char *name, uint32_t *count)
{
	const uint8_t *end = (const uint8_t *)ctab + ctab_size;
	const uint8_t *at = (const uint8_t *)ctab + sizeof(*ctab) + ctab->modes * sizeof(struct ctab_mode);
	uint32_t tables;

	for (int p = 0; p < 2; p++) {           /* (the power sequences, which the driver does by hand) */
		if (at + 12 > end) {
			return NULL;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct ctab_entry);
	}
	if (at + 4 > end) {
		return NULL;
	}
	tables = *(const uint32_t *)at;
	at += 4;
	for (uint32_t t = 0; t < tables && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (at + 12 + n * sizeof(struct ctab_entry) > end) {
			return NULL;
		}
		if (strncmp((const char *)at, name, 8) == 0) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	return NULL;
}

static bool
ctab_load(unsigned i)
{
	ctab = (const struct ctab_head *)hakios_image_file("fw", tab_names[i], &ctab_size, NULL);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB02", 8) != 0 || ctab->modes == 0 ||
	    ctab->modes > 16 || sizeof(*ctab) + ctab->modes * sizeof(struct ctab_mode) > ctab_size) {
		say("cam6t: no %s (RWCTAB02) in fw/ of the small image (userland/build.sh makes it from fw-camera/)\n", tab_names[i]);
		ctab = NULL;
		return false;
	}
	say("cam6t: %s: slave %#x, chip ID %#x, lanes %#x, %u modes\n", tab_names[i], ctab->slave, ctab->id, ctab->lane_assign,
	    ctab->modes);
	return true;
}

/* one register write: a 16-bit address, 8 or 16 bits of data */
static bool
write_reg(const struct sensor *s, const struct ctab_entry *e)
{
	bool answered;

	bus_sda = s->sda;
	bus_scl = s->scl;
	i2c_start();
	answered = i2c_write(s->address) && i2c_write((uint8_t)(e->address >> 8)) && i2c_write((uint8_t)e->address);
	if (answered && e->data_bytes == 2) {
		answered = i2c_write((uint8_t)(e->data >> 8));
	}
	answered = answered && i2c_write((uint8_t)e->data);
	i2c_stop();
	bus_give_back();
	if (e->delay != 0) {
		spin_us(e->delay);
	}
	return answered;
}

static bool
write_table(const struct sensor *s, const char *name)
{
	uint32_t count = 0, failed = 0;
	const struct ctab_entry *e = ctab_find(name, &count);

	if (e == NULL) {
		say("cam6t: the %s has no table \"%s\"\n", s->name, name);
		return false;
	}
	for (uint32_t k = 0; k < count; k++) {
		if (e[k].operation != 0) {
			continue;               /* (only writes are in these tables) */
		}
		if (!write_reg(s, &e[k]) && failed++ < 3) {
			say("cam6t: the %s did not take %#06x <- %#x\n", s->name, e[k].address, e[k].data);
		}
	}
	say("cam6t: the %s's \"%s\": %u writes%s\n", s->name, name, count, failed ? ", SOME NOT TAKEN" : "");
	return failed == 0;
}

/*
 * Exposure and gain: the tables leave the sensor's power-on ones (Android's auto exposure sets them later), far too
 * little light. Sony's registers, as the module file names them: the coarse integration time in lines at 0x0202 (16
 * bits), the analog gain code at 0x0204: gain = 1024 / (1024 - code) on the IMX519 and the IMX371, 512 / (512 - code)
 * on the IMX376K (whose codes stop at 480: 768 left it black, run 182907). cam6t_exposure=N lines (frame length less
 * 20 by default), cam6t_gain=code (4x by default: 768, or 384 on the IMX376K).
 */
static bool
write16(const struct sensor *s, uint16_t reg, uint16_t value)
{
	struct ctab_entry e = { reg, value, 2, 2, 0, 0, 0 };

	return write_reg(s, &e);
}

/* a register of 8-bit address on another chip of the sensor's bus: count bytes read or written (most first) */
static bool
bus_xfer(const struct sensor *s, uint8_t address, uint8_t reg, uint8_t *data, unsigned count, bool reading)
{
	bool answered;

	bus_sda = s->sda;
	bus_scl = s->scl;
	i2c_start();
	answered = i2c_write(address) && i2c_write(reg);
	if (answered && reading) {
		i2c_start();
		answered = i2c_write(address | 1);
		for (unsigned k = 0; answered && k < count; k++) {
			data[k] = i2c_read(k + 1 < count);
		}
	} else {
		for (unsigned k = 0; answered && k < count; k++) {
			answered = i2c_write(data[k]);
		}
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static void
focus(unsigned i, const struct sensor *s)
{
	uint8_t b[2] = { 0, 0 };
	unsigned go;

	if (cam_focus == ~0u) {
		cam_focus = 500;
	}
	if (i > 1 || cam_focus == 0) {
		return;
	}
	motor_pin = motors[i].pin;
	drive(motor_pin, true);
	motor_on = true;
	wait_ms(10);
	if (bus_xfer(s, motors[i].lc, 0xb3, b, 1, true)) {
		for (go = 0; go < 50 && b[0] != 0; go++) {
			wait_ms(1);
			(void)bus_xfer(s, motors[i].lc, 0xb3, b, 1, true);
		}
		b[0] = (uint8_t)(cam_focus >> 8);
		b[1] = (uint8_t)cam_focus;
		say("cam6t: the lens motor is an LC898217XC (%#x; ready %s): position %u %s\n", motors[i].lc, go < 50 ? "at once" : "NOT",
		    cam_focus, bus_xfer(s, motors[i].lc, 0x84, b, 2, false) ? "set" : "NOT TAKEN");
	} else if (b[0] = 1, bus_xfer(s, motors[i].sa, 0x81, b, 1, false)) {
		wait_ms(1);
		b[0] = (uint8_t)(cam_focus >> 8);
		b[1] = (uint8_t)cam_focus;
		say("cam6t: the lens motor is an SA3103 (%#x): position %u %s\n", motors[i].sa, cam_focus,
		    bus_xfer(s, motors[i].sa, 0x82, b, 2, false) ? "set" : "NOT TAKEN");
	} else {
		say("cam6t: no lens motor answers (%#x, %#x) with gpio%u on\n", motors[i].lc, motors[i].sa, motor_pin);
		return;
	}
	wait_ms(30);                    /* (the lens settles) */
}

static void
focus_off(void)
{
	if (motor_on) {
		drive(motor_pin, false);
		motor_on = false;
	}
}

/* camcc's PLL3 (Fabia, 384 MHz: L 0x14, as camcc-sdm845.c configures it), its even output /1 */
#define PLL3                    0x3000
static bool pll3_ours;

static bool
pll3_up(void)
{
	vm_offset_t p = camcc + PLL3;

	if ((rd(p + PLL_OPMODE) & 1) && (rd(p + PLL_MODE) & PLL_OUTCTRL)) {
		return (rd(p + PLL_L) & 0xffff) == 0x14;
	}
	wr(p + PLL_L, 0x14);
	wr(p + PLL_FRAC, 0);
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) & ~PLL_POST_DIV_EVEN);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_UPDATE_BYPASS);
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
	wr(p + PLL_OPMODE, 0);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_RESET_N);
	wr(p + PLL_OPMODE, 1);
	for (int i = 0; !(rd(p + PLL_MODE) & PLL_LOCK_DET); i++) {
		if (i > 1000) {
			say("cam6t: PLL3 DOES NOT LOCK (mode %#010x)\n", rd(p + PLL_MODE));
			wr(p + PLL_OPMODE, 0);
			return false;
		}
		spin_us(10);
	}
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) | PLL_OUT_MASK);
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_OUTCTRL);
	pll3_ours = true;
	say("cam6t: PLL3 locked at 384 MHz\n");
	return true;
}

static void
pll3_down(void)
{
	vm_offset_t p = camcc + PLL3;

	if (pll3_ours) {
		/* (what PLL3 feeds back on the crystal first, as clk_rcg2_shared_ops park them) */
		(void)rcg_xo(CAMCC_CPHY_RX_RCG, "cphy_rx_src");
		(void)rcg_xo(CAMCC_IFE_LITE_CSID_RCG, "ife_lite_csid_src");
		wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
		wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) & ~PLL_OUT_MASK);
		wr(p + PLL_OPMODE, 0);
		pll3_ours = false;
	}
}

/* an RCG with no M/N to a source and a divider (cfg as clk-rcg2.c writes it: [4:0] 2d-1, [10:8] source) */
static bool
rcg_set(uint32_t rcg, uint32_t cfg, const char *name)
{
	wr(camcc + rcg + RCG_CFG, cfg);
	wr(camcc + rcg + RCG_CMD, rd(camcc + rcg + RCG_CMD) | 1);
	for (int i = 0; rd(camcc + rcg + RCG_CMD) & 1; i++) {
		if (i > 1000) {
			say("cam6t: %s: the RCG does not take %#x (cmd %#010x)\n", name, cfg, rd(camcc + rcg + RCG_CMD));
			return false;
		}
		spin_us(1);
	}
	return true;
}

#define CAMCC_CSI_PHYTIMER_RCG(n)  (0x5004 + 0x24 * (n))       /* csi0phytimer 0x5004, 1 0x5028, 2 0x504c */
#define CAMCC_CSI_PHYTIMER_CBCR(n) (0x501c + 0x24 * (n))       /* 0x501c, 0x5040, 0x5064 */
#define CAMCC_CSIPHY_CBCR(n)       (0x5020 + 0x24 * (n))       /* csiphy0 0x5020, 1 0x5044, 2 0x5068 */
#define CSIPHY_PHYS(n)             (0x0ac65000UL + 0x1000UL * (n))
#define CSIPHY_TIMER_HZ            240000000u
#define CSIPHY_COMMON(n)           (0x800 + 4 * (n))
#define CSIPHY_STATUS(n)           (0x8b0 + 4 * (n))

static vm_offset_t csiphy_base;
static uint32_t phy_n = ~0u;
static bool phy_clocks_ours, phy_on;

static bool
phy_clocks_on(uint32_t n)
{
	vm_offset_t t = camcc + CAMCC_CSI_PHYTIMER_CBCR(n), c = camcc + CAMCC_CSIPHY_CBCR(n);

	if (!rcg_set(CAMCC_CSI_PHYTIMER_RCG(n), 3u | 1u << 8, "csiphytimer")) {   /* PLL2 even / 2: 240 MHz */
		return false;
	}
	phy_clocks_ours = true;
	phy_n = n;
	wr(t, rd(t) | CBCR_ENABLE);
	wr(c, rd(c) | CBCR_ENABLE);
	for (int i = 0; (rd(t) & CBCR_OFF) || (rd(c) & CBCR_OFF); i++) {
		if (i > 1000) {
			say("cam6t: csiphy%u's clocks stay off (timer %#010x, phy %#010x)\n", n, rd(t), rd(c));
			return false;
		}
		spin_us(1);
	}
	return true;
}

static void
phy_clocks_off(void)
{
	if (phy_clocks_ours) {
		wr(camcc + CAMCC_CSIPHY_CBCR(phy_n), rd(camcc + CAMCC_CSIPHY_CBCR(phy_n)) & ~CBCR_ENABLE);
		wr(camcc + CAMCC_CSI_PHYTIMER_CBCR(phy_n), rd(camcc + CAMCC_CSI_PHYTIMER_CBCR(phy_n)) & ~CBCR_ENABLE);
		phy_clocks_ours = false;
	}
}

/* the PHY's two LDOs through RPMh (VRM: voltage, enable, mode 7 = high power) */
static bool
phy_rails_on(void)
{
	static const struct {
		const char *name;
		uint32_t mv;
	} ldo[2] = { { "ldoa1", 880 }, { "ldoa26", 1200 } };

	for (unsigned k = 0; k < 2; k++) {
		uint32_t base = rpmh_address(ldo[k].name), address[3], data[3] = { ldo[k].mv, 1, 7 };

		if (base == 0 || (base >> 16 & 0xf) != 4) {
			say("cam6t: RPMh: no regulator \"%s\"\n", ldo[k].name);
			return false;
		}
		address[0] = base + VRM_VOLTAGE;
		address[1] = base + VRM_ENABLE;
		address[2] = base + VRM_MODE;
		if (!rpmh_write(address, data, 3)) {
			return false;
		}
	}
	say("cam6t: the CSIPHYs' supplies voted on (LDO1 0.88 V, LDO26 1.2 V)\n");
	return true;
}

/* mainline's csiphy_settle_cnt_calc(): the lane's rate in bits a second */
static uint32_t
settle_count(uint64_t lane_bps)
{
	uint64_t ui_ps = 1000000000000ull / lane_bps;           /* one bit */
	uint64_t settle_ps = 85000 + 6 * ui_ps, timer_ps = 1000000000000ull / CSIPHY_TIMER_HZ;

	return (uint32_t)(settle_ps / timer_ps - 6);
}

static const uint16_t phy_regs[5][14][2] = {          /* camss-csiphy-3ph-1-0.c: lane_regs_sdm845; 0xffff the settle count */
	{ { 0x0004, 0x0c }, { 0x002c, 0x01 }, { 0x0034, 0x0f }, { 0x001c, 0x0a }, { 0x0014, 0x60 }, { 0, 0 }, { 0x003c, 0xb8 },
	  { 0x0000, 0x91 }, { 0x0008, 0xffff }, { 0, 0 }, { 0x0010, 0x52 }, { 0x0038, 0xfe }, { 0x0060, 0x00 }, { 0x0064, 0x7f } },
	{ { 0x0704, 0x0c }, { 0x072c, 0x01 }, { 0x0734, 0x0f }, { 0x071c, 0x0a }, { 0x0714, 0x60 }, { 0x0728, 0x04 },
	  { 0x073c, 0xb8 }, { 0x0700, 0x80 }, { 0x0708, 0xffff }, { 0x070c, 0xa5 }, { 0x0710, 0x52 }, { 0x0738, 0x1f },
	  { 0x0760, 0x00 }, { 0x0764, 0x7f } },
	{ { 0x0204, 0x0c }, { 0x022c, 0x01 }, { 0x0234, 0x0f }, { 0x021c, 0x0a }, { 0x0214, 0x60 }, { 0, 0 }, { 0x023c, 0xb8 },
	  { 0x0200, 0x91 }, { 0x0208, 0xffff }, { 0, 0 }, { 0x0210, 0x52 }, { 0x0238, 0xfe }, { 0x0260, 0x00 }, { 0x0264, 0x7f } },
	{ { 0x0404, 0x0c }, { 0x042c, 0x01 }, { 0x0434, 0x0f }, { 0x041c, 0x0a }, { 0x0414, 0x60 }, { 0, 0 }, { 0x043c, 0xb8 },
	  { 0x0400, 0x91 }, { 0x0408, 0xffff }, { 0, 0 }, { 0x0410, 0x52 }, { 0x0438, 0xfe }, { 0x0460, 0x00 }, { 0x0464, 0x7f } },
	{ { 0x0604, 0x0c }, { 0x062c, 0x01 }, { 0x0634, 0x0f }, { 0x061c, 0x0a }, { 0x0614, 0x60 }, { 0, 0 }, { 0x063c, 0xb8 },
	  { 0x0600, 0x91 }, { 0x0608, 0xffff }, { 0, 0 }, { 0x0610, 0x52 }, { 0x0638, 0xfe }, { 0x0660, 0x00 }, { 0x0664, 0x7f } },
};

/* csiphy_reset() and csiphy_lanes_enable() for four data lanes and the clock lane */
static void
phy_config(uint32_t settle)
{
	uint32_t version;

	wr(csiphy_base + CSIPHY_COMMON(6), 2);                  /* show the revision */
	version = rd(csiphy_base + CSIPHY_STATUS(12)) | rd(csiphy_base + CSIPHY_STATUS(13)) << 8 |
	    rd(csiphy_base + CSIPHY_STATUS(14)) << 16 | rd(csiphy_base + CSIPHY_STATUS(15)) << 24;
	wr(csiphy_base + CSIPHY_COMMON(0), 1);
	spin_us(5000);
	wr(csiphy_base + CSIPHY_COMMON(0), 0);
	wr(csiphy_base + CSIPHY_COMMON(5), 0x80u | 1u | 1u << 2 | 1u << 4 | 1u << 6);   /* the clock and lanes 0-3 */
	wr(csiphy_base + CSIPHY_COMMON(6), 1);                  /* common power-down off */
	wr(csiphy_base + CSIPHY_COMMON(7), 2);
	wr(csiphy_base + CSIPHY_COMMON(0), 0);
	for (unsigned l = 0; l < 5; l++) {
		for (unsigned i = 0; i < 14; i++) {
			if (phy_regs[l][i][0] == 0 && phy_regs[l][i][1] == 0 && !(l == 0 && i == 7)) {
				continue;
			}
			wr(csiphy_base + phy_regs[l][i][0], phy_regs[l][i][1] == 0xffff ? (settle & 0xff) : phy_regs[l][i][1]);
		}
	}
	for (unsigned i = 11; i < 22; i++) {
		wr(csiphy_base + CSIPHY_COMMON(i), 0);
	}
	phy_on = true;
	say("cam6t: csiphy%u (version %#010x) set for four D-PHY lanes, settle count %u\n", phy_n, version, settle);
}

/*
 * Three C-PHY trios (the IMX519's): the downstream driver's way (cam_csiphy_core.c's cam_csiphy_config_dev() with
 * cam_csiphy_1_0_hwreg.h, OnePlus's kernel): the common registers with the trios' enable mask (bits 1, 3, 5), a
 * reset, then each trio's 19 registers, the settle count where the table has it; the interrupts masked as
 * cam_csiphy_cphy_irq_config() leaves them.
 */
static const uint16_t cphy_trio[19][2] = {
	{ 0x15c, 0x63 }, { 0x168, 0xac }, { 0x16c, 0xa5 }, { 0x104, 0x06 }, { 0x10c, 0xffff }, { 0x108, 0xfffe }, { 0x114, 0x20 },
	{ 0x150, 0x00 }, { 0x118, 0x3e }, { 0x11c, 0x41 }, { 0x120, 0x41 }, { 0x124, 0x7f }, { 0x128, 0x00 }, { 0x12c, 0x00 },
	{ 0x144, 0x12 }, { 0x160, 0x02 }, { 0x1cc, 0x41 }, { 0x164, 0x00 }, { 0x1dc, 0x51 },
};
static const uint8_t cphy_irq[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };     /* 0x82c on */

static void
phy_config_cphy(uint32_t settle)
{
	uint32_t version;

	wr(csiphy_base + CSIPHY_COMMON(6), 2);
	version = rd(csiphy_base + CSIPHY_STATUS(12)) | rd(csiphy_base + CSIPHY_STATUS(13)) << 8 |
	    rd(csiphy_base + CSIPHY_STATUS(14)) << 16 | rd(csiphy_base + CSIPHY_STATUS(15)) << 24;
	wr(csiphy_base + 0x814, 0x2a);                          /* trios 0-2 */
	wr(csiphy_base + 0x818, 0x01);
	wr(csiphy_base + 0x81c, 0x02);
	wr(csiphy_base + 0x800, 0x01);
	spin_us(1000);
	wr(csiphy_base + 0x800, 0x00);
	for (unsigned t = 0; t < 3; t++) {
		for (unsigned i = 0; i < 19; i++) {
			uint32_t v = cphy_trio[i][1] == 0xffff ? (settle & 0xff) : cphy_trio[i][1] == 0xfffe ? (settle >> 8 & 0xff) :
			    cphy_trio[i][1];

			wr(csiphy_base + cphy_trio[i][0] + 0x200 * t, v);
		}
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy_base + 0x82c + 4 * i, cphy_irq[i]);
	}
	phy_on = true;
	say("cam6t: csiphy%u (version %#010x) set for three C-PHY trios, settle count %u\n", phy_n, version, settle);
}

static void
phy_off(void)
{
	if (phy_on) {
		wr(csiphy_base + CSIPHY_COMMON(5), 0);
		wr(csiphy_base + CSIPHY_COMMON(6), 0);
		phy_on = false;
	}
}

static void
show_phy(void)
{
	say("cam6t: csiphy%u status: %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x\n", phy_n, rd(csiphy_base + CSIPHY_STATUS(0)),
	    rd(csiphy_base + CSIPHY_STATUS(1)), rd(csiphy_base + CSIPHY_STATUS(2)), rd(csiphy_base + CSIPHY_STATUS(3)),
	    rd(csiphy_base + CSIPHY_STATUS(4)), rd(csiphy_base + CSIPHY_STATUS(5)), rd(csiphy_base + CSIPHY_STATUS(6)),
	    rd(csiphy_base + CSIPHY_STATUS(7)), rd(csiphy_base + CSIPHY_STATUS(8)), rd(csiphy_base + CSIPHY_STATUS(9)),
	    rd(csiphy_base + CSIPHY_STATUS(10)));
}

/* the receive path at its rates: cphy_rx and the CSID 384 MHz (PLL3, source 5), IFE-lite 320 MHz (PLL2 even / 1.5) */
static bool
rates_up(void)
{
	return pll3_up() && rcg_set(CAMCC_CPHY_RX_RCG, 1u | 5u << 8, "cphy_rx_src") &&
	    rcg_set(CAMCC_IFE_LITE_CSID_RCG, 1u | 5u << 8, "ife_lite_csid_src") && rcg_set(CAMCC_IFE_LITE_RCG, 2u | 1u << 8, "ife_lite_src") &&
	    rcg_set(CAMCC_SLOW_AHB_RCG, 11u | 1u << 8, "slow_ahb_src");         /* 80 MHz: PLL2 even / 6 */
}

static void
capture_sensor(unsigned i)
{
	const struct sensor *s = &sensors[i];
	const struct ctab_mode *m;
	struct cam_header *h;
	char res[8];
	unsigned ms;
	uint64_t lane_bps;
	uint16_t id = 0;

	if (!ctab_load(i)) {
		return;
	}
	if (cam_mode == ~0u) {
		cam_mode = i == 2 ? 1 : i == 0 ? 10 : 0;        /* the IMX519's 10: 2328x1746, 60 frames/s */
	}
	if (cam_mode >= ctab->modes) {
		say("cam6t: the %s has no mode %u\n", s->name, cam_mode);
		return;
	}
	m = &((const struct ctab_mode *)(ctab + 1))[cam_mode];
	if (m->data_type != RAW10_DT || (m->cphy ? m->lanes != 3 : m->lanes != 4)) {
		say("cam6t: mode %u is not four D-PHY lanes or three C-PHY trios of RAW10\n", cam_mode);
		return;
	}
	cap_width = m->width;
	cap_height = m->height;
	lane_bps = (uint64_t)m->pixel_clock * 10 / m->lanes;
	say("cam6t: the %s, mode %u: %ux%u, %u.%03u frames/s, %s, %llu Mbit/s of pixels a %s\n", s->name, cam_mode, m->width,
	    m->height, m->fps_milli / 1000, m->fps_milli % 1000, m->cphy ? "C-PHY" : "D-PHY", lane_bps / 1000000,
	    m->cphy ? "trio" : "lane");
	if (csid_base == 0) {
		csid_base = ml_io_map(CSID_LITE_PHYS, 0x1000);
		ife_base = ml_io_map(VFE_LITE_PHYS, 0x4000);
	}
	csiphy_base = ml_io_map(CSIPHY_PHYS(sensor_csiphy[i]), 0x1000);
	if (!frames_alloc(1, s->name, sensor_bayer[i]) || !power_up(s)) {
		power_down();
		return;
	}
	for (unsigned go = 0; go < 20 && !read16(s, ID_REGISTER, &id); go++) {
		wait_ms(2);
	}
	say("cam6t: the %s's chip ID %#06x\n", s->name, id);
	snprintf(res, sizeof(res), "res%u", cam_mode);
	if (id != s->id || !write_table(s, "init") || !write_table(s, res)) {
		power_down();
		return;
	}
	{
		uint16_t exposure = 0, gain = 0;
		uint32_t want = cam_exposure != ~0u ? cam_exposure : m->frame_length > 20 ? m->frame_length - 20 : m->frame_length;
		uint32_t gain_code = cam_gain != ~0u ? cam_gain : i == 1 ? 384 : 768;

		(void)read16(s, 0x0202, &exposure);
		(void)read16(s, 0x0204, &gain);
		if (!write16(s, 0x0202, (uint16_t)want) || !write16(s, 0x0204, (uint16_t)gain_code)) {
			say("cam6t: the %s did not take its exposure or gain\n", s->name);
		}
		say("cam6t: exposure %u lines, gain code %u (were %u and %u; frame %u lines)\n", want, gain_code, exposure, gain,
		    m->frame_length);
	}
	focus(i, s);
	if (!blocks_on(true)) {
		focus_off();
		power_down();
		return;
	}
	if (!rates_up() || !phy_rails_on() || !phy_clocks_on(sensor_csiphy[i]) || !smmu_up() || !csid_reset() || !ife_config() ||
	    !csid_config((int)sensor_csiphy[i], ctab->lane_assign, m->cphy != 0)) {
		goto out;
	}
	if (m->cphy) {
		/* the module file's settle count is for Android's 269.33 MHz timer: scaled to this one's 240 */
		phy_config_cphy(cam_settle != ~0u ? cam_settle : m->settle * 240 / 269);
	} else {
		phy_config(cam_settle != ~0u ? cam_settle : settle_count(lane_bps));
	}
	streaming = true;
	show_paths("configured");
	if (!write_table(s, "on")) {
		goto out;
	}
	h = (struct cam_header *)frames;
	for (ms = 0; ms < 3000; ms += 50) {
		wait_ms(50);
		if (rd(ife_base + BUS_IRQ_STATUS1) & 1) {
			h->frames = 1;
			break;
		}
	}
	say("cam6t: %s\n", h->frames ? "WRITE MASTER 0 SAYS A FRAME OF THE SENSOR IS IN MEMORY" : "no frame done in 3 s");
	show_phy();
	show_paths("after");
	(void)write_table(s, "off");
out:
	stream_stop();
	phy_off();
	smmu_faults();
	smmu_down();
	phy_clocks_off();
	blocks_off();
	pll3_down();
	focus_off();
	power_down();
	for (vm_offset_t line = frames; line < frames + frames_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	const uint8_t *p = (const uint8_t *)(frames + CAM_HEADER_BYTES);
	uint32_t nonzero = 0, bytes = TPG_STRIDE * TPG_HEIGHT_NOW;

	for (uint32_t k = 0; k < bytes; k++) {
		nonzero += p[k] != 0;
	}
	say("cam6t: the frame's first bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; %u of %u bytes not 0 (xl camera fetches it)\n",
	    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], nonzero, bytes);
	for (vm_offset_t line = frames; line < frames + CAM_HEADER_BYTES; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
}

/* ---- cam6t=6: the cameras for Darwin (/dev/hakios-camera) -------------------------------------------------------- */

/*
 * redwood's cam=5 for the 6T (drivers/camera/hakios.camera.c, sys/hakios.camera.h): the device node Darwin's camera
 * stack reads (libhakios.media.dylib in mediaserverd: src/camera_device.c, src/camera_isp.c), so that the same user
 * space works here. Its sensors are numbered as redwood's: 1 the back (wide) camera, the IMX519; 2 the front one, the
 * IMX371; 3 the IMX376K (redwood's ultra-wide's place); 0 (the macro) there is none of. A session, from the first
 * read after an open to the last close, a second without a read, or a fault: the sensor powered and streaming as
 * cam6t=5 sets it up, its frames going into three places in turn (one the IFE writes, one a read may be copying, one
 * ready), the write master given the next place's address at each start of frame (RDI 0's SOF, bit 27 of IFE-lite's
 * IRQ status 0, as on redwood's; polled every millisecond) and taking it at the next.
 *
 * The gain is told to user space as a linear number (GAIN_UNITY = 1x, up to 16x: camera_isp.c reads gain / gain_unity
 * as the analog gain) and written as the Sony code it means: 1024 - 1024 / gain (512 - 512 / gain on the IMX376K).
 * The exposure, gain and frame length go in the sensor's group hold (its "holdon" and "holdoff" tables), between two
 * frames. The lens (the rear ones): position 0 to 1023 of the frames' header straight to the motor.
 */
#define RING                    3
#define PLACE_SPARE             (256u << 10)
#define IDLE_MS                 1000
#define FRAME_TIMEOUT_MS        1500
#define EXPOSURE_MARGIN         20
#define GAIN_UNITY              256u
#define GAIN_MAX                (16 * GAIN_UNITY)
#define FRAME_LINES_MAX         0xfff0
#define RDI0_SOF                (1u << 27)      /* IFE_IRQ_STATUS0 */
#define WM0_DONE                1u              /* BUS_IRQ_STATUS1 */
#define BUS_IRQ_CLEAR1          0x2054
#define BUS_IRQ_GLOBAL_CLEAR    0x2068
#define CSID_RDI0_BYTES_PONG    0x2e4

static const int darwin_to_sensor[4] = { -1, 0, 2, 1 };       /* redwood's numbers -> sensors[] */
static const int sensor_to_darwin[SENSORS] = { 1, 3, 2 };

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

static LCK_GRP_DECLARE(darwin_lck_grp, "hakios-camera");
static LCK_MTX_DECLARE(darwin_lock, &darwin_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
} place[RING];
static vm_offset_t ring;
static uint64_t ring_bytes, ring_place_bytes;
static uint32_t ring_frame_bytes, darwin_odd_frames, darwin_line_ns, darwin_mode_n;
static unsigned darwin_index;                   /* sensors[] */
static const struct ctab_mode *darwin_mode;
static volatile bool darwin_open, darwin_streaming, darwin_wanted, broken;
static uint64_t darwin_last_read, darwin_given, darwin_frames_done, darwin_writes_late;
static unsigned darwin_readers;
static struct { uint32_t lines, gain, frame_lines; bool pending; uint32_t focus; bool focus_pending; } darwin_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } darwin_now, darwin_next;
static volatile unsigned darwin_switch_to;      /* 1 + redwood's number; 0: none asked for */
static uint32_t darwin_switch_mode;
static int motor_kind;                          /* 0 none, 1 LC898217XC, 2 SA3103 (of the sensor streaming) */
static uint8_t motor_address;
static uint32_t motor_position = HAKIOS_CAMERA_NO_FOCUS;

static uint64_t
darwin_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static void
give_up(const char *why)
{
	if (!broken) {
		say("cam6t: THE CAMERA GIVES UP: %s (off for the rest of the run)\n", why);
	}
	broken = true;
}

/* the linear gain (GAIN_UNITY = 1x) as the sensor's code */
static uint16_t
gain_code(uint32_t gain)
{
	uint32_t scale = darwin_index == 1 ? 512 : 1024, most = darwin_index == 1 ? 480 : 960;
	uint32_t code = scale - (uint32_t)((uint64_t)scale * GAIN_UNITY / gain);

	return (uint16_t)(code > most ? most : code);
}

static void
exposure_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = darwin_mode->frame_length;

	*gain = *gain < GAIN_UNITY ? GAIN_UNITY : *gain > GAIN_MAX ? GAIN_MAX : *gain;
	*lines = *lines < 4 ? 4 : *lines > FRAME_LINES_MAX - EXPOSURE_MARGIN ? FRAME_LINES_MAX - EXPOSURE_MARGIN : *lines;
	if (*frame_lines == 0) {
		*frame_lines = *lines + EXPOSURE_MARGIN > shortest ? *lines + EXPOSURE_MARGIN : shortest;
	}
	*frame_lines = *frame_lines < shortest ? shortest : *frame_lines > FRAME_LINES_MAX ? FRAME_LINES_MAX : *frame_lines;
	if (*lines > *frame_lines - EXPOSURE_MARGIN) {
		*lines = *frame_lines - EXPOSURE_MARGIN;
	}
}

static bool
exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	const struct sensor *s = &sensors[darwin_index];
	bool ok = write_table(s, "holdon") && write16(s, 0x0340, (uint16_t)frame_lines) && write16(s, 0x0202, (uint16_t)lines) &&
	    write16(s, 0x0204, gain_code(gain)) && write_table(s, "holdoff");

	if (!ok) {
		give_up("a sensor write nobody acknowledged");
	}
	return ok;
}

/* the lens motor of the sensor streaming: which one, then positions 0-1023 */
static void
motor_up(void)
{
	const struct sensor *s = &sensors[darwin_index];
	uint8_t b[2] = { 0, 0 };

	motor_kind = 0;
	motor_position = HAKIOS_CAMERA_NO_FOCUS;
	if (darwin_index > 1) {
		return;
	}
	motor_pin = motors[darwin_index].pin;
	drive(motor_pin, true);
	motor_on = true;
	wait_ms(10);
	if (bus_xfer(s, motors[darwin_index].lc, 0xb3, b, 1, true)) {
		for (unsigned go = 0; go < 50 && b[0] != 0; go++) {
			wait_ms(1);
			(void)bus_xfer(s, motors[darwin_index].lc, 0xb3, b, 1, true);
		}
		motor_kind = 1;
		motor_address = motors[darwin_index].lc;
	} else if (b[0] = 1, bus_xfer(s, motors[darwin_index].sa, 0x81, b, 1, false)) {
		motor_kind = 2;
		motor_address = motors[darwin_index].sa;
	} else {
		say("cam6t: the %s's lens motor does not answer: no focus\n", s->name);
		focus_off();
	}
}

static void
motor_move(uint32_t position)
{
	uint8_t b[2];

	if (motor_kind == 0) {
		return;
	}
	position = position > 1023 ? 1023 : position;
	b[0] = (uint8_t)(position >> 8);
	b[1] = (uint8_t)position;
	if (bus_xfer(&sensors[darwin_index], motor_address, motor_kind == 1 ? 0x84 : 0x82, b, 2, false)) {
		motor_position = position;
	}
}

static void
ring_free(void)
{
	if (ring != 0) {
		kmem_free(kernel_map, ring, ring_bytes);
		ring = 0;
	}
	frames = 0;
	frames_bytes = 0;
}

static bool
ring_alloc(uint32_t frame_bytes)
{
	ring_frame_bytes = frame_bytes;
	ring_place_bytes = round_page(frame_bytes) + PLACE_SPARE;
	ring_bytes = RING * ring_place_bytes;
	if (kmem_alloc(kernel_map, &ring, ring_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("cam6t: no memory for %u frames (%llu KB)\n", RING, ring_bytes >> 10);
		ring = 0;
		return false;
	}
	for (vm_offset_t line = ring; line < ring + ring_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	frames = ring;                  /* (what smmu_up() maps) */
	frames_bytes = ring_bytes;
	return true;
}

static void
darwin_drain_readers(void)
{
	lck_mtx_lock(&darwin_lock);
	while (darwin_readers != 0) {
		(void)msleep(&darwin_readers, &darwin_lock, PZERO, "hakios-camera-drain", NULL);
	}
	lck_mtx_unlock(&darwin_lock);
}

static void
darwin_stop(const char *why)
{
	const struct sensor *s = &sensors[darwin_index];

	lck_mtx_lock(&darwin_lock);
	darwin_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_frames_done);
	if (up != NULL) {
		(void)write_table(s, "off");
	}
	if (csid_base != 0) {
		show_paths("at the end");
		stream_stop();
	}
	phy_off();
	smmu_faults();
	smmu_down();
	phy_clocks_off();
	blocks_off();
	pll3_down();
	focus_off();
	power_down();
	darwin_drain_readers();
	ring_free();
	say("cam6t: the frames for Darwin stopped (%s): %llu done, %llu given, %llu written over\n", why, darwin_frames_done,
	    darwin_given, darwin_writes_late);
}

static bool
darwin_start(void)
{
	const struct sensor *s = &sensors[darwin_index];
	const struct ctab_mode *m = darwin_mode;
	uint32_t lines, gain, frame_lines;
	uint16_t id = 0;
	char res[8];

	darwin_frames_done = darwin_given = darwin_writes_late = 0;
	darwin_odd_frames = 0;
	cap_width = m->width;
	cap_height = m->height;
	if (csid_base == 0) {
		csid_base = ml_io_map(CSID_LITE_PHYS, 0x1000);
		ife_base = ml_io_map(VFE_LITE_PHYS, 0x4000);
	}
	csiphy_base = ml_io_map(CSIPHY_PHYS(sensor_csiphy[darwin_index]), 0x1000);
	if (!ring_alloc(TPG_STRIDE * m->height) || !power_up(s)) {
		return false;
	}
	for (unsigned go = 0; go < 20 && !read16(s, ID_REGISTER, &id); go++) {
		wait_ms(2);
	}
	snprintf(res, sizeof(res), "res%u", darwin_mode_n);
	if (id != s->id || !write_table(s, "init") || !write_table(s, res) || !blocks_on(true) || !rates_up() || !phy_rails_on() ||
	    !phy_clocks_on(sensor_csiphy[darwin_index]) || !smmu_up() || !csid_reset() || !ife_config_at(FRAME_ADDRESSES, 0) ||
	    !csid_config((int)sensor_csiphy[darwin_index], ctab->lane_assign, m->cphy != 0)) {
		return false;
	}
	if (m->cphy) {
		phy_config_cphy(cam_settle != ~0u ? cam_settle : m->settle * 240 / 269);
	} else {
		phy_config(cam_settle != ~0u ? cam_settle : settle_count((uint64_t)m->pixel_clock * 10 / m->lanes));
	}
	streaming = true;
	motor_up();
	lck_mtx_lock(&darwin_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	place[0].state = PLACE_WRITING;
	lines = darwin_ask.lines;
	gain = darwin_ask.gain;
	frame_lines = darwin_ask.frame_lines;
	darwin_ask.pending = false;
	lck_mtx_unlock(&darwin_lock);
	exposure_limits(&lines, &gain, &frame_lines);
	if (!write_table(s, "on") || !exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	if (motor_kind != 0) {
		motor_move(cam_focus != ~0u && cam_focus != 0 ? cam_focus : 500);
	}
	darwin_now.lines = darwin_next.lines = lines;
	darwin_now.gain = darwin_next.gain = gain;
	darwin_now.frame_lines = darwin_next.frame_lines = frame_lines;
	darwin_next.from = 0;
	say("cam6t: the frames for Darwin: the %s, mode %u, %ux%u, %u lines of %u ns a frame at the shortest; exposure %u lines, "
	    "gain %u/%u, frame %u lines; lens %s\n", s->name, darwin_mode_n, m->width, m->height, m->frame_length, darwin_line_ns, lines,
	    gain, GAIN_UNITY, frame_lines, motor_kind == 1 ? "LC898217XC" : motor_kind == 2 ? "SA3103" : "fixed");
	return true;
}

static void
darwin_session(void)
{
	int writing = 0, next = 0;
	uint64_t last_frame;

	if (!darwin_start()) {
		give_up("the camera did not start");
		darwin_stop("it did not start");
		return;
	}
	last_frame = darwin_ms();
	wr(ife_base + IFE_IRQ_CLEAR0, ~0u);
	wr(ife_base + IFE_IRQ_GLOBAL_CLEAR, 1);
	wr(ife_base + BUS_IRQ_CLEAR1, ~0u);
	wr(ife_base + BUS_IRQ_GLOBAL_CLEAR, 1);
	lck_mtx_lock(&darwin_lock);
	darwin_streaming = true;
	darwin_last_read = darwin_ms();
	lck_mtx_unlock(&darwin_lock);
	while (!broken && darwin_open && darwin_switch_to == 0) {
		uint32_t top, bus;
		uint64_t now;

		wait_ms(1);
		now = darwin_ms();
		top = rd(ife_base + IFE_IRQ_STATUS0);
		bus = rd(ife_base + BUS_IRQ_STATUS1);
		if (!(top & RDI0_SOF) && !(bus & WM0_DONE)) {
			if (now - last_frame > FRAME_TIMEOUT_MS) {
				show_paths("no frame");
				give_up("no frame for 1.5 s");
				break;
			}
			if (now - darwin_last_read > IDLE_MS) {
				break;
			}
			continue;
		}
		wr(ife_base + IFE_IRQ_CLEAR0, top & RDI0_SOF);
		wr(ife_base + IFE_IRQ_GLOBAL_CLEAR, 1);
		wr(ife_base + BUS_IRQ_CLEAR1, bus & WM0_DONE);
		wr(ife_base + BUS_IRQ_GLOBAL_CLEAR, 1);
		lck_mtx_lock(&darwin_lock);
		if (bus & WM0_DONE) {
			uint32_t ping = rd(csid_base + CSID_RDI0_BYTES_PING), data = darwin_mode->width * 10 / 8 * darwin_mode->height;

			last_frame = now;
			darwin_frames_done++;
			if (ping != data && darwin_odd_frames++ < 8) {
				say("cam6t: frame %llu: the CSID counted %u bytes (the mode's frame is %u)\n", darwin_frames_done, ping, data);
			}
			if (darwin_frames_done >= darwin_next.from && darwin_next.from != 0) {
				darwin_now = darwin_next;
				darwin_next.from = 0;
			}
			place[writing].state = PLACE_READY;
			place[writing].sequence = darwin_frames_done;
			place[writing].time = mach_absolute_time();
			place[writing].exposure_lines = darwin_now.lines;
			place[writing].gain = darwin_now.gain;
			place[writing].frame_lines = darwin_now.frame_lines;
		}
		if (top & RDI0_SOF) {
			if (next >= 0) {
				writing = next;
				next = -1;
			} else {
				darwin_writes_late++;
			}
			place[writing].state = PLACE_WRITING;
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;

				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				wr(ife_base + WM0_IMAGE_ADDR, (uint32_t)(FRAME_ADDRESSES + (uint64_t)next * ring_place_bytes));
				wr(ife_base + IFE_REG_UPDATE, 0x2);
			}
		}
		bool ask = (bus & WM0_DONE) && darwin_ask.pending, move = (bus & WM0_DONE) && darwin_ask.focus_pending;
		uint32_t lines = darwin_ask.lines, gain = darwin_ask.gain, frame_lines = darwin_ask.frame_lines, position = darwin_ask.focus;

		darwin_ask.pending = darwin_ask.pending && !ask;
		darwin_ask.focus_pending = darwin_ask.focus_pending && !move;
		lck_mtx_unlock(&darwin_lock);
		if (move) {
			motor_move(position);
		}
		if (bus & WM0_DONE) {
			wakeup(&darwin_frames_done);
		}
		if (ask) {
			exposure_limits(&lines, &gain, &frame_lines);
			if (lines != darwin_now.lines || gain != darwin_now.gain || frame_lines != darwin_now.frame_lines) {
				if (!exposure_write(lines, gain, frame_lines)) {
					break;
				}
				darwin_next.lines = lines;
				darwin_next.gain = gain;
				darwin_next.frame_lines = frame_lines;
				darwin_next.from = darwin_frames_done + 2;
			}
		}
		if ((bus & WM0_DONE) && darwin_frames_done % 30 == 0) {
			if (rd(smmu_context + SMMU_CB_FSR) & SMMU_FSR_ALL) {
				smmu_faults();
				give_up("the SMMU refused the IFE");
				break;
			}
			if (darwin_frames_done == 30 || darwin_frames_done % 900 == 0) {
				show_paths("streaming");
			}
		}
	}
	darwin_stop(broken ? "a fault" : !darwin_open ? "closed" : darwin_switch_to != 0 ? "another sensor asked for" : "nobody reads");
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	darwin_open = true;
	return 0;
}

static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	darwin_open = false;
	darwin_wanted = false;
	wakeup(&darwin_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_frame f;
	uint64_t since = darwin_ms();
	int b = -1, error = 0;
	vm_offset_t at = 0;
	uint32_t frame_bytes = 0;

	lck_mtx_lock(&darwin_lock);
	darwin_last_read = darwin_ms();
	if (!darwin_streaming && !darwin_wanted) {
		darwin_wanted = true;
		wakeup(&darwin_open);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&darwin_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > darwin_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (darwin_ms() - since > (darwin_streaming ? 3000u : 15000u)) {
			lck_mtx_unlock(&darwin_lock);
			return EAGAIN;
		}
		if (!darwin_streaming && !darwin_wanted) {
			darwin_wanted = true;
			wakeup(&darwin_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&darwin_frames_done, &darwin_lock, PZERO | PCATCH, "hakios-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&darwin_lock);
			return error;
		}
		darwin_last_read = darwin_ms();
	}
	place[b].state = PLACE_READING;
	darwin_readers++;
	at = ring + (uint64_t)b * ring_place_bytes;
	frame_bytes = ring_frame_bytes;
	darwin_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HAKIOS_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = darwin_mode->width;
	f.height = darwin_mode->height;
	f.stride = ((darwin_mode->width * 10 / 8) + 15) & ~15u;
	f.format = 1;
	f.bayer = sensor_bayer[darwin_index];
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = darwin_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = GAIN_UNITY;
	f.gain_max = GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = darwin_mode->frame_length;
	f.frame_lines_max = FRAME_LINES_MAX;
	f.margin = EXPOSURE_MARGIN;
	f.sensor = (uint32_t)sensor_to_darwin[darwin_index];
	f.focus = motor_position;
	lck_mtx_unlock(&darwin_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		uint32_t n = (uint32_t)MIN((uint64_t)frame_bytes, (uint64_t)uio_resid(uio));

		uint64_t t0 = mach_absolute_time(), t1, t2, a, c;

		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy");
		t1 = mach_absolute_time();
		error = uiomove((caddr_t)at, (int)n, uio);
		t2 = mach_absolute_time();
		if (f.sequence <= 3 || f.sequence % 300 == 0) {
			absolutetime_to_nanoseconds(t1 - t0, &a);
			absolutetime_to_nanoseconds(t2 - t1, &c);
			say("cam6t: frame %llu read: %u bytes; the caches' lines forgotten in %llu us, copied in %llu us (cpu %d)\n",
			    f.sequence, n, a / 1000, c / 1000, cpu_number());
		}
	}
	lck_mtx_lock(&darwin_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	darwin_last_read = darwin_ms();
	darwin_readers--;
	lck_mtx_unlock(&darwin_lock);
	wakeup(&darwin_readers);
	return error;
}

/* HAKIOS_CAMERA_KEEP: a picture for the host (what the program made of a frame), in the piece taken at boot */
static vm_offset_t keep;
static uint64_t keep_bytes;
static LCK_MTX_DECLARE(keep_lock, &darwin_lck_grp);

static int
keep_picture(const struct hakios_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(CAM_HEADER_BYTES + (uint64_t)c->bytes);
	int error;

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (keep == 0 || keep_bytes < want) {
		return ENOSPC;
	}
	lck_mtx_lock(&keep_lock);
	h = (struct cam_header *)keep;
	bzero(h, CAM_HEADER_BYTES);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = c->d == 1 ? sensor_bayer[darwin_index] : 0;
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "Darwin", sizeof(h->sensor));
	error = uiomove((caddr_t)(keep + CAM_HEADER_BYTES), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	for (vm_offset_t line = keep; line < keep + want; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	loader_recording[0] = kvtophys(keep);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)keep;
	lck_mtx_unlock(&keep_lock);
	say("cam6t: a picture from Darwin for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hakios_camera_command c;
	int error;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || (error = uiomove((caddr_t)&c, sizeof(c), uio)) != 0) {
		return EINVAL;
	}
	if (c.command == HAKIOS_CAMERA_EXPOSURE) {
		lck_mtx_lock(&darwin_lock);
		darwin_ask.lines = c.a;
		darwin_ask.gain = c.b;
		darwin_ask.frame_lines = c.c;
		darwin_ask.pending = true;
		lck_mtx_unlock(&darwin_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_FOCUS) {
		lck_mtx_lock(&darwin_lock);
		darwin_ask.focus = c.a;
		darwin_ask.focus_pending = true;
		lck_mtx_unlock(&darwin_lock);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_SENSOR) {
		if (c.a >= 4 || darwin_to_sensor[c.a] < 0) {
			return EINVAL;
		}
		lck_mtx_lock(&darwin_lock);
		darwin_switch_mode = c.b;
		darwin_switch_to = c.a + 1;
		lck_mtx_unlock(&darwin_lock);
		wakeup(&darwin_open);
		return 0;
	}
	if (c.command == HAKIOS_CAMERA_KEEP) {
		return keep_picture(&c, uio);
	}
	return EINVAL;
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool camdev_made;

static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	camdev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-camera") != NULL;
	say("cam6t: /dev/hakios-camera %s\n", camdev_made ? "made" : "CANNOT BE MADE");
}

/* the sensor (sensors[] index) and its mode for the sessions: its tables; the mode asked for, else the 6T's usual */
static bool
darwin_select(unsigned index, uint32_t mode_n)
{
	if (!ctab_load(index)) {
		return false;
	}
	if (mode_n >= ctab->modes) {
		/*
		 * the sensors' biggest 4:3 at 30 frames a second, as redwood's: the ISP's preview (1504x1128) and the
		 * photograph take a cell (2x2 pixels) a pixel from them; from the half-size modes (2328x1746, 2304x1728) it had
		 * to blend four cells for each pixel, 120 ms a frame, and the preview kept up with one frame in four
		 */
		mode_n = index == 0 ? 9 : 0;    /* IMX519 4352x3264, IMX371 4608x3456, IMX376K 2592x1940 */
	}
	darwin_index = index;
	darwin_mode_n = mode_n;
	darwin_mode = &((const struct ctab_mode *)(ctab + 1))[mode_n];
	if (darwin_mode->data_type != RAW10_DT || (darwin_mode->cphy ? darwin_mode->lanes != 3 : darwin_mode->lanes != 4) ||
	    darwin_mode->width % 4 != 0 || darwin_mode->fps_milli == 0 || darwin_mode->frame_length <= EXPOSURE_MARGIN ||
	    darwin_mode->frame_length > FRAME_LINES_MAX || darwin_mode->width > 8192 || darwin_mode->height > 8192) {
		say("cam6t: the %s's mode %u is not what this is written for\n", sensors[index].name, mode_n);
		return false;
	}
	darwin_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)darwin_mode->fps_milli * darwin_mode->frame_length));
	darwin_ask.lines = darwin_mode->frame_length - EXPOSURE_MARGIN;
	darwin_ask.gain = 4 * GAIN_UNITY;
	darwin_ask.frame_lines = 0;
	say("cam6t: /dev/hakios-camera: the %s (camera %d), mode %u: %ux%u, %u.%03u frames/s at the most, whenever it is read\n",
	    sensors[index].name, sensor_to_darwin[index], mode_n, darwin_mode->width, darwin_mode->height, darwin_mode->fps_milli / 1000,
	    darwin_mode->fps_milli % 1000);
	return true;
}

static void
serve_darwin(void)
{
	uint32_t mode_n = cam_mode;

	if (!camdev_made) {
		return;
	}
	if (!darwin_select(which_given && which < SENSORS ? which : 0, mode_n)) {
		return;
	}
	while (!broken) {
		lck_mtx_lock(&darwin_lock);
		while (!broken && !(darwin_open && darwin_wanted) && darwin_switch_to == 0) {
			(void)msleep(&darwin_open, &darwin_lock, PZERO, "hakios-camera-idle", NULL);
		}
		unsigned to = darwin_switch_to;

		darwin_switch_to = 0;
		lck_mtx_unlock(&darwin_lock);
		if (broken) {
			break;
		}
		if (to != 0) {
			if (!darwin_select((unsigned)darwin_to_sensor[to - 1], darwin_switch_mode)) {
				give_up("a sensor that cannot be used");
				break;
			}
			continue;
		}
		lck_mtx_lock(&darwin_lock);
		darwin_wanted = false;
		lck_mtx_unlock(&darwin_lock);
		darwin_session();
	}
	say("cam6t: /dev/hakios-camera: no more frames in this run\n");
}

static void
cam6t_thread(__unused void *arg, __unused wait_result_t wr_)
{
	bool clocks;

	if (level >= 6) {
		camdev_make();          /* (before mediaserverd starts: its capture stack looks for cameras once) */
	}
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
		for (unsigned i = 0; i < SENSORS && (level == 2 || (which_given && level < 5)); i++) {
			if (which >= SENSORS || which == i) {
				ask(&sensors[i]);
			}
		}
		if (level == 3) {
			blocks_look();
		} else if (level == 4) {
			capture_tpg();
		} else if (level == 5) {
			capture_sensor(which_given && which < SENSORS ? which : 2);
		} else if (level >= 6) {
			serve_darwin();
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

	(void)PE_parse_boot_argn("cam6t", &level, sizeof(level));
	if (level == 0) {
		return;
	}
	which_given = PE_parse_boot_argn("cam6t_sensor", &which, sizeof(which));
	(void)PE_parse_boot_argn("cam6t_after", &after_s, sizeof(after_s));
	(void)PE_parse_boot_argn("cam6t_xo", &use_xo, sizeof(use_xo));
	(void)PE_parse_boot_argn("cam6t_mode", &cam_mode, sizeof(cam_mode));
	(void)PE_parse_boot_argn("cam6t_exposure", &cam_exposure, sizeof(cam_exposure));
	(void)PE_parse_boot_argn("cam6t_gain", &cam_gain, sizeof(cam_gain));
	(void)PE_parse_boot_argn("cam6t_settle", &cam_settle, sizeof(cam_settle));
	(void)PE_parse_boot_argn("cam6t_focus", &cam_focus, sizeof(cam_focus));
	if (level >= 6) {
		/* (a picture for the host, cam6t=6: in one piece, taken while there is memory in one piece) */
		if (kmem_alloc_contig(kernel_map, &keep, FRAMES_KEPT_BYTES, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
			keep_bytes = FRAMES_KEPT_BYTES;
		} else {
			keep = 0;
		}
	}
	if (level == 5) {
		/* a sensor's frames are megabytes: taken now, in one piece, as redwood's driver takes its own at boot */
		if (kmem_alloc_contig(kernel_map, &frames, FRAMES_KEPT_BYTES, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS) {
			frames_kept = FRAMES_KEPT_BYTES;
		} else {
			frames = 0;
		}
	}
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
	focus_off();
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
		phy_off();
		smmu_down();
		phy_clocks_off();
		blocks_off();
		pll3_down();
		pll2_down();
		gdsc_off();
	}
	pins_give_back();
}
