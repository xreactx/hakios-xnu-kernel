/*
 * The buttons of the POCO X5 Pro (redwood) -- power, volume up, volume down -- as a source of
 * button records for /dev/hakios-touch (sys/hakios.input.h). Only with boot-arg buttons=1:
 * every address below belongs to this phone. buttons=2 is the OnePlus 6T's (fajita) instead; the
 * differences are said at "the phones" below.
 *
 * None of the three is a pin of the SoC. They are inputs of two of the power management chips:
 *   power        KPDPWR_N of the power-on block ("PON") of the PMK8350
 *   volume down  RESIN_N of the same block
 *   volume up    GPIO6 of the PM7325, pulled up; the key pulls it low
 * Both chips sit on the SPMI bus (slaves 0 and 1), and the way to that bus is the SoC's "PMIC
 * arbiter": it owns the bus and gives each of the systems on the chip (this one, the modem, the
 * always-on processor, TrustZone...) a set of registers per PMIC peripheral -- a "channel" -- to
 * ask for a transfer with. Reads go through the "observer" channels, which every system has for
 * every peripheral, whoever owns it.
 *
 * THIS DRIVER ONLY READS. Nothing is written to either chip, neither a register of a PMIC nor
 * the configuration of the pin: the PMICs make the phone's supplies, and a wrong write there
 * can switch one off. The firmware reads the same keys itself (volume down and volume up choose
 * between fastboot and recovery) and has left both inputs as they are needed; the driver checks
 * that before it believes them, and where it finds something else it says so and leaves that
 * key alone. The only thing written is the command word that asks the arbiter for a read.
 *
 * What a press is: the PON's "real time status" has a bit per key, set while the key is held
 * (after the block's own debouncing); the GPIO's has the level of the pin. A thread reads the
 * two registers 60 times a second, and a key has changed when two looks in a row say so. A key
 * that reads as held before it has ever been seen released is not believed (a pin that is not
 * what it was taken for must not become a volume key held down for the whole session).
 * No interrupt is used; what one would take is said at the end of this file.
 *
 * The mapping: the phone has what the iPhone X has, whose user space runs here -- a side button
 * and two volume keys, no home button. Power is the side button (HAKIOS_BUTTON_LOCK).
 *
 * The bring-up is made in steps, each said in the log before it is made. Boot-arg
 * buttons_steps=N stops after the first N:
 *   1  map the arbiter and read its version             (the first access to that block)
 *   2  find the channels of the two peripherals         (reads of the arbiter's tables)
 *   3  ask each peripheral what it is                   (the first transfers on the bus)
 *   4  read how the keys' inputs are set up, and what they say now
 *   5  watch them                                       (the default)
 *
 * The OnePlus 6T (buttons=2): the SoC (SDM845) has the same arbiter at the same addresses, and all
 * three keys are on one PMIC, the PM8998 (slave 0): power KPDPWR_N and volume down RESIN_N of its
 * power-on block (an older kind, 0x800, the keys in bits 0 and 1), volume up its GPIO6 (0xc500),
 * pulled up. Volume down is the PM8998's GPIO5 (0xc400), pulled up as well: the phone's device tree
 * names both it and RESIN_N, and a hand pressing the key moved GPIO5 alone. RESIN_N is still read
 * (with power, in the same register) and logged, not reported.
 *
 * Register layouts: Linux drivers/spmi/spmi-pmic-arb.c (arbiter version 5),
 * drivers/input/misc/pm8941-pwrkey.c (the "pmk8350" kind), drivers/pinctrl/qcom/pinctrl-spmi-gpio.c,
 * and the phone's own device tree (qcom,spmi@c440000, pon_hlos@1300, gpio_keys).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/*
 * Every line goes to the loader's log directly as well: without -v the kernel's own messages
 * reach that log only at the end of a run (with the kernel's message buffer), and "xl log"
 * during a run is to show a press when it happens.
 */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter ---------------------------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL    /* "core": the version, and which peripheral each channel is */
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL    /* "cnfg": which system owns each channel */
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL    /* "obsrvr": 0x10000 per system, 0x80 per channel in it */
#define ARB_OBSERVER_SIZE       0x10000UL       /* ours is the first */
#define ARB_SYSTEM              0               /* who we are to the arbiter: the device tree's "qcom,ee" */

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u     /* what follows is version 5's; 7 moved things again */
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004           /* [10:0] the number of channels */
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))       /* core: [19:8] slave and peripheral, [24] interrupts go to the owner */
#define ARB_CHANNEL_INTERRUPTS  (1u << 24)
#define ARB_OWNER(n)            (0x700 + 4 * (n))       /* cnfg: [2:0] the system that may write through channel n */
#define ARB_MAX_CHANNELS        512

#define OBSERVER_COMMAND        0x00
#define OBSERVER_STATUS         0x08
#define OBSERVER_DATA0          0x18            /* the bytes read, the first in the lowest byte */
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)      /* "extended register read long"; [11:4] register, [2:0] bytes - 1 */
#define READ_TIMEOUT_US         2000            /* a read takes some tens of microseconds */

static vm_offset_t arb_core, arb_cnfg, arb_observer;

/* one transfer at a time through the arbiter: hakios_spmi_lock() (drivers/core/hakios.platform.c) */

struct peripheral {
	const char *    name;
	uint16_t        id;             /* slave << 8 | peripheral: the high byte of its register addresses */
	uint16_t        channel;
	bool            found;
};
static struct peripheral pon = { .name = "the PMK8350's power-on block", .id = 0x013 };          /* registers 0x1300 of slave 0 */
static struct peripheral pin = { .name = "GPIO6 of the PM7325", .id = 0x18d };                   /* registers 0x8d00 of slave 1 */
static struct peripheral pin2 = { .name = "GPIO5 of the PM8998" };                               /* the 6T's other volume down */

/* How the reads have gone: said once a minute. */
static struct {
	uint64_t reads, failed, total_ns, longest_ns, last_ns;
	uint64_t dropped_early, dropped_late;   /* "dropped": in the first DROP_EARLY_S of watching, and after */
	uint64_t retried;                       /* reads asked again after a drop */
	uint64_t yielded;                       /* reads that waited for the loader's core to leave the bus */
} bus;
#define DROP_EARLY_S            20
static uint64_t watch_started;

/*
 * "Dropped": the arbiter had another transfer at that moment. On the OnePlus 6T about one read in 20,000
 * is, all run long, with the loader's core quiet (no battery=1, its interrupts off) -- something else on
 * the bus, probably another of the SoC's processors. Asked again at once, the read goes through. Counted
 * in the first DROP_EARLY_S of watching and after, for the minute's line.
 */
/* a millisecond's wait: asleep if the caller may sleep, spinning if it came with interrupts off (a run's end) */
static void
spmi_pause_1ms(boolean_t interrupts)
{
	uint64_t until;

	if (interrupts) {
		delay_for_interval(1, NSEC_PER_MSEC);
		return;
	}
	nanoseconds_to_absolutetime(NSEC_PER_MSEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
spmi_count_drop(uint64_t now)
{
	uint64_t since;

	if (watch_started == 0) {
		return;
	}
	absolutetime_to_nanoseconds(now - watch_started, &since);
	if (since < DROP_EARLY_S * NSEC_PER_SEC) {
		bus.dropped_early++;
	} else {
		bus.dropped_late++;
	}
}

/*
 * The arbiter knows the loader's core as the same system as ours (0), and two transfers of one system at
 * once make it drop one. For the first 24 s of a run (xnuloader's NOTE_SECONDS) that core still takes
 * interrupts, and UEFI's PMIC drivers do their own transfers from timer events. One of ours dropped is
 * one look missed (and asked again); one of theirs dropped may not be survivable: on the OnePlus 6T, a
 * test reading every 1 ms from the start left the loader's core answering nothing (the kernel ran on, its
 * run's end never answered), and the same test waiting 25 s first ran out its 120 s and returned. So
 * the bus is not touched until boot-arg buttons_after=S seconds after this driver starts: 25 by default
 * with the 6T's layout (past the 24 s, however late the kernel got here), 0 with redwood's as before.
 */
static uint32_t buttons_after;

/*
 * Read `count` (1 to 8) registers of a peripheral, from register `reg` on. Returns NULL, or what
 * went wrong.
 */
static const char *
spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	uint64_t start = mach_absolute_time(), now, interval, taken;
	uint32_t status, data[2];
	const char *problem = NULL;
	boolean_t interrupts;

	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	/*
	 * Not while the loader's core reads the fuel gauge (battery=1, every 5 s): it says so 5 ms ahead,
	 * and from this look to the transfer's end (2 ms at most) nothing here is interrupted.
	 */
	for (unsigned waited = 0;; waited++) {
		interrupts = ml_set_interrupts_enabled(FALSE);
		hakios_spmi_lock();
		if (!hakios_battery_loader_bus_busy()) {
			break;
		}
		hakios_spmi_unlock();
		ml_set_interrupts_enabled(interrupts);
		if (waited == 0) {
			bus.yielded++;
		}
		if (waited >= 200) {
			return "the loader's core keeps the bus";
		}
		spmi_pause_1ms(interrupts);
	}
	start = mach_absolute_time();
	/* a dropped read is asked once more at once (see "dropped" below) */
	for (int attempt = 0; attempt < 2; attempt++) {
		if (attempt > 0) {
			bus.retried++;
			start = mach_absolute_time();
		}
		wr(channel + OBSERVER_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
		do {
			status = rd(channel + OBSERVER_STATUS);
			now = mach_absolute_time();
		} while (!(status & STATUS_DONE) && now - start < interval);
		if (!(status & STATUS_DONE) || !(status & STATUS_DROPPED) || (status & (STATUS_DENIED | STATUS_FAILURE))) {
			break;
		}
		spmi_count_drop(now);
	}
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	hakios_spmi_unlock();
	ml_set_interrupts_enabled(interrupts);

	if (!(status & STATUS_DONE)) {
		problem = "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		problem = "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		problem = "the transfer failed (nobody at that address?)";
	} else if (status & STATUS_DROPPED) {
		problem = "the transfer was dropped, twice";
	} else {
		for (uint32_t i = 0; i < count; i++) {
			bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
		}
	}
	absolutetime_to_nanoseconds(now - start, &taken);
	bus.reads++;
	bus.total_ns += taken;
	bus.last_ns = taken;
	if (taken > bus.longest_ns) {
		bus.longest_ns = taken;
	}
	if (problem != NULL) {
		bus.failed++;
	}
	return problem;
}

/*
 * Which channel is that peripheral's. Several can be: one per system that may write to it. As
 * Linux chooses: the first, unless a later one is ours to write through.
 */
static void
find_channel(struct peripheral *p, uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != p->id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		printf("buttons: channel %u is %s (%#x); written by system %u%s\n", n, p->name, map, owner,
		    (map & ARB_CHANNEL_INTERRUPTS) ? ", which also gets its interrupts" : "");
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
		}
	}
	if (!p->found) {
		printf("buttons: no channel of the arbiter is %s (slave %u, peripheral %#x)\n", p->name, p->id >> 8, p->id & 0xff);
	}
}

/*
 * For other drivers (hakios.pmi_haptics.c): reads by a peripheral's id (slave << 8 | peripheral), through
 * this system's observer channels, with everything spmi_read() does -- the lock, the loader's bus_busy,
 * one more try after a drop. The arbiter is mapped and its channels counted on first use. Whoever calls
 * this keeps to the rule spmi_read() keeps: not in the run's first 24 s (see buttons_after).
 */
static uint32_t arb_channels;

static bool
spmi_map(void)
{
	uint32_t version;

	if (arb_core == 0) {
		arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
		arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
		arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	}
	version = rd(arb_core + ARB_VERSION);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		return false;
	}
	arb_channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (arb_channels == 0 || arb_channels > ARB_MAX_CHANNELS) {
		arb_channels = ARB_MAX_CHANNELS;
	}
	return true;
}

/* the channel find_channel() would choose, quietly; -1 if none. *owner: the system that may write through it */
int
hakios_spmi_channel(uint16_t id, uint32_t *owner)
{
	int found = -1;

	if (!spmi_map()) {
		return -1;
	}
	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), who;

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		who = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		if (found < 0 || who == ARB_SYSTEM) {
			found = (int)n;
			*owner = who;
		}
	}
	return found;
}

/* `count' (1 to 8) registers from `reg' on; NULL, or what went wrong */
const char *
hakios_spmi_read(uint16_t id, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	struct peripheral p = { .name = "", .id = id };
	uint32_t owner;
	int channel = hakios_spmi_channel(id, &owner);

	if (channel < 0) {
		return "no channel of the arbiter is that peripheral";
	}
	p.channel = (uint16_t)channel;
	p.found = true;
	return spmi_read(&p, reg, bytes, count);
}

/*
 * WRITES. The only ones to a PMIC in this kernel, and only to the peripherals listed here: the PMICs make
 * the phone's supplies, and a write to the wrong register can switch one off. A write goes through the
 * peripheral's own channel in the "chnls" region (0x10000 a channel), which only its owner may use; the
 * data first, then the command (Linux spmi-pmic-arb.c, version 5, pmic_arb_write_cmd()).
 */
#define ARB_CHNLS_PHYS          0x0c600000UL    /* the phone's device tree: qcom,spmi@c440000's second "reg" */
#define CHNL_WDATA0             0x10
#define CHNL_WDATA1             0x14
#define COMMAND_WRITE           (0u << 27)      /* "extended register write long"; [11:4] register, [2:0] bytes - 1 */

static const uint16_t spmi_writable[] = {
	0x3c0,          /* the PMI8998's haptics block (hakios.pmi_haptics.c) */
};

const char *
hakios_spmi_write(uint16_t id, uint8_t reg, const uint8_t *bytes, uint32_t count)
{
	static vm_offset_t chnl_page[sizeof(spmi_writable) / sizeof(spmi_writable[0])];
	uint64_t start, now, interval;
	uint32_t owner = 0, status = 0, data[2] = { 0, 0 };
	boolean_t interrupts;
	vm_offset_t chnl = 0;
	unsigned which;
	int channel;

	for (which = 0; which < sizeof(spmi_writable) / sizeof(spmi_writable[0]); which++) {
		if (spmi_writable[which] == id) {
			break;
		}
	}
	if (which == sizeof(spmi_writable) / sizeof(spmi_writable[0])) {
		return "not a peripheral this kernel writes to";
	}
	if (count == 0 || count > 8) {
		return "1 to 8 bytes";
	}
	channel = hakios_spmi_channel(id, &owner);
	if (channel < 0) {
		return "no channel of the arbiter is that peripheral";
	}
	if (owner != ARB_SYSTEM) {
		return "its channel is another system's to write through";
	}
	if (chnl_page[which] == 0) {
		chnl_page[which] = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * (uint32_t)channel, 0x1000);
	}
	chnl = chnl_page[which];
	for (uint32_t i = 0; i < count; i++) {
		data[i / 4] |= (uint32_t)bytes[i] << (8 * (i % 4));
	}
	nanoseconds_to_absolutetime(READ_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	for (unsigned waited = 0;; waited++) {
		interrupts = ml_set_interrupts_enabled(FALSE);
		hakios_spmi_lock();
		if (!hakios_battery_loader_bus_busy()) {
			break;
		}
		hakios_spmi_unlock();
		ml_set_interrupts_enabled(interrupts);
		if (waited >= 200) {
			return "the loader's core keeps the bus";
		}
		spmi_pause_1ms(interrupts);
	}
	for (int attempt = 0; attempt < 2; attempt++) {
		wr(chnl + CHNL_WDATA0, data[0]);
		if (count > 4) {
			wr(chnl + CHNL_WDATA1, data[1]);
		}
		wr(chnl + OBSERVER_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4 | (count - 1));
		start = mach_absolute_time();
		do {
			status = rd(chnl + OBSERVER_STATUS);
			now = mach_absolute_time();
		} while (!(status & STATUS_DONE) && now - start < interval);
		if (!(status & STATUS_DONE) || !(status & STATUS_DROPPED) || (status & (STATUS_DENIED | STATUS_FAILURE))) {
			break;
		}
		bus.retried++;
	}
	hakios_spmi_unlock();
	ml_set_interrupts_enabled(interrupts);
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	} else if (status & STATUS_DENIED) {
		return "the arbiter refuses";
	} else if (status & STATUS_FAILURE) {
		return "the transfer failed";
	} else if (status & STATUS_DROPPED) {
		return "the transfer was dropped, twice";
	}
	return NULL;
}

/* ---- the two peripherals ------------------------------------------------------------------------- */

/* every peripheral of these PMICs */
#define REG_TYPE                0x04
#define REG_SUBTYPE             0x05
#define REG_RT_STATUS           0x10            /* the inputs as they are now */
#define REG_INT_SET_TYPE        0x11            /* interrupts: a bit per input in each of these */
#define REG_INT_POLARITY_HIGH   0x12
#define REG_INT_POLARITY_LOW    0x13
#define REG_INT_ENABLED         0x15
#define REG_INT_LATCHED         0x18
#define REG_INT_PENDING         0x19

/* the power-on block, of the kind that is split in two ("generation 3"): this is its half for the operating system */
#define PON_TYPE                0x01
#define PON_SUBTYPE_HLOS        0x09
/* ... and the older kinds (1 to 5: primary, secondary, one register, generation 2), the PM8998's */
#define PON_SUBTYPE_OLD_FROM    0x01
#define PON_SUBTYPE_OLD_TO      0x05
/* the keys in the status and in the interrupt registers (set from the phone's layout) */
static uint8_t pon_kpdpwr = 0x80, pon_resin = 0x40, pon_subtype_from = PON_SUBTYPE_HLOS, pon_subtype_to = PON_SUBTYPE_HLOS;

/* a GPIO of the low or medium voltage kind */
#define GPIO_TYPE               0x10
#define GPIO_SUBTYPE_LV         0x10
#define GPIO_SUBTYPE_MV_VIN3    0x13            /* 0x10 to 0x13: one layout */
/* ... or of the older kinds (4 or 8 channels, 0x01 to 0x0d: the PM8998's), where the mode is [6:4] */
#define GPIO_SUBTYPE_OLD_FROM   0x01
#define GPIO_SUBTYPE_OLD_TO     0x0d
#define GPIO_LEVEL              0x01            /* in the status */
#define GPIO_REG_MODE           0x40            /* [1:0]: 0 input, 1 output, 2 both, 3 analog */
#define GPIO_REG_SUPPLY         0x41
#define GPIO_REG_PULL           0x42            /* [2:0]: 0 to 3 up (30, 1.5, 31.5 uA, 1.5 with a boost), 4 down, 5 none */
#define GPIO_REG_ENABLE         0x46            /* [7] */

#define POLL_MS                 16              /* boot-arg buttons_poll_ms=N: another period (1 to 1000) */
static uint32_t poll_ms = POLL_MS;

static struct button {
	const char *    name;
	uint32_t        record;         /* HAKIOS_BUTTON_* */
	bool            usable;         /* its input is what it was taken for */
	bool            live;           /* it has been seen released */
	bool            last;           /* the look before this one */
	bool            down;           /* as reported */
	uint64_t        since;          /* when it went down */
	uint32_t        presses;
} power = { .name = "power", .record = HAKIOS_BUTTON_LOCK },
    volume_up = { .name = "volume up", .record = HAKIOS_BUTTON_VOLUME_UP },
    volume_down = { .name = "volume down", .record = HAKIOS_BUTTON_VOLUME_DOWN },
    volume_down2 = { .name = "volume down (GPIO5)", .record = 0 };      /* the 6T's (record 0: logged, not reported) */

/* "12.345 s (08:13:05.918 UTC)": since boot as in the log's own stamps, and as the daemons' lines have it */
static void
now_text(char *text, size_t size, uint64_t now)
{
	clock_sec_t seconds;
	clock_usec_t micro;
	uint64_t up;

	absolutetime_to_nanoseconds(now, &up);
	clock_get_calendar_microtime(&seconds, &micro);
	snprintf(text, size, "%llu.%03llu s (%02u:%02u:%02u.%03u UTC)", up / NSEC_PER_SEC, up / NSEC_PER_MSEC % 1000,
	    (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60), (unsigned)(micro / 1000));
}

static void
button_look(struct button *b, bool pressed)
{
	uint64_t now = mach_absolute_time(), held;
	bool steady = pressed == b->last;
	char when[64];

	b->last = pressed;
	if (!b->usable || !steady) {
		return;
	}
	if (!b->live) {
		if (!pressed) {
			b->live = true;
			printf("buttons: %s is released now: believed from here on\n", b->name);
		}
		return;
	}
	if (pressed == b->down) {
		return;
	}
	b->down = pressed;
	now_text(when, sizeof(when), now);
	if (pressed) {
		b->since = now;
		b->presses++;
		printf("buttons: %s PRESSED at %s\n", b->name, when);
	} else {
		absolutetime_to_nanoseconds(now - b->since, &held);
		printf("buttons: %s released at %s, after %llu ms\n", b->name, when, held / NSEC_PER_MSEC);
	}
	if (b->record != 0) {
		hakios_touch_post(pressed ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, b->record, 0, 0);
	}
}

/* One look at both peripherals. */
static void
buttons_poll(void)
{
	static unsigned said;
	const char *problem;
	uint8_t status;

	if (power.usable || volume_down.usable) {
		problem = spmi_read(&pon, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&power, status & pon_kpdpwr);
			button_look(&volume_down, status & pon_resin);
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pon.name, problem);
		}
	}
	if (volume_up.usable) {
		problem = spmi_read(&pin, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&volume_up, !(status & GPIO_LEVEL));
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pin.name, problem);
		}
	}
	if (volume_down2.usable) {
		problem = spmi_read(&pin2, REG_RT_STATUS, &status, 1);
		if (problem == NULL) {
			button_look(&volume_down2, !(status & GPIO_LEVEL));
		} else if (said < 8) {
			said++;
			printf("buttons: reading %s: %s\n", pin2.name, problem);
		}
	}
}

/* Step 3: is each peripheral what the registers above are those of? */
static uint8_t identified_subtype;

static bool
identify(const struct peripheral *p, uint8_t type, uint8_t subtype_from, uint8_t subtype_to)
{
	const char *problem;
	uint8_t is[2];

	if (!p->found) {
		return false;
	}
	printf("buttons: next: read the type of %s through channel %u (2 registers, 0x%02x04)\n", p->name, p->channel, p->id & 0xff);
	problem = spmi_read(p, REG_TYPE, is, 2);
	if (problem != NULL) {
		printf("buttons: %s: %s\n", p->name, problem);
		return false;
	}
	printf("buttons: %s says type %#x, subtype %#x (the read took %llu us)\n", p->name, is[0], is[1], bus.last_ns / 1000);
	identified_subtype = is[1];
	if (is[0] != type || is[1] < subtype_from || is[1] > subtype_to) {
		printf("buttons: ... which is not what this driver knows (type %#x, subtype %#x to %#x): left alone\n", type,
		    subtype_from, subtype_to);
		return false;
	}
	return true;
}

/* Step 4 for the power-on block: nothing to check but what it says now; its interrupt setup goes to the log. */
static void
pon_inputs(void)
{
	const char *problem;
	uint8_t r[8], more[2];

	printf("buttons: next: read the status and the interrupt setup of %s (registers 0x%02x10 to 0x%02x19)\n", pon.name,
	    pon.id & 0xff, pon.id & 0xff);
	problem = spmi_read(&pon, REG_RT_STATUS, r, 8);
	if (problem == NULL) {
		problem = spmi_read(&pon, REG_INT_LATCHED, more, 2);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", pon.name, problem);
		return;
	}
	printf("buttons: power-on block: status %#x (power %s, volume down %s); interrupts: edge %#x, high %#x, low %#x, "
	    "enabled %#x, latched %#x, pending %#x\n", r[0], (r[0] & pon_kpdpwr) ? "HELD" : "released",
	    (r[0] & pon_resin) ? "HELD" : "released", r[REG_INT_SET_TYPE - REG_RT_STATUS], r[REG_INT_POLARITY_HIGH - REG_RT_STATUS],
	    r[REG_INT_POLARITY_LOW - REG_RT_STATUS], r[REG_INT_ENABLED - REG_RT_STATUS], more[0], more[1]);
	power.usable = volume_down.usable = true;
	power.last = r[0] & pon_kpdpwr;
	volume_down.last = r[0] & pon_resin;
}

/* Step 4 for a pin: it is a key only if it is an input with a pull-up, and switched on. */
static void
pin_input(const struct peripheral *p, uint8_t subtype, struct button *b)
{
	const char *problem;
	uint8_t c[8], status = 0;
	unsigned mode, pull;

	printf("buttons: next: read the setup and the level of %s (registers 0x%02x40 to 0x%02x47, 0x%02x10)\n", p->name,
	    p->id & 0xff, p->id & 0xff, p->id & 0xff);
	problem = spmi_read(p, GPIO_REG_MODE, c, 8);
	if (problem == NULL) {
		problem = spmi_read(p, REG_RT_STATUS, &status, 1);
	}
	if (problem != NULL) {
		printf("buttons: %s: %s\n", p->name, problem);
		return;
	}
	mode = subtype < GPIO_SUBTYPE_LV ? (c[0] >> 4) & 7 : c[0] & 3;
	pull = c[GPIO_REG_PULL - GPIO_REG_MODE] & 7;
	printf("buttons: %s's pin: mode %u (%s), supply %u, pull %u (%s), %s; registers 0x40.. = %02x %02x %02x %02x %02x %02x "
	    "%02x %02x; level %s\n", b->name, mode, mode == 0 ? "input" : mode == 1 ? "output" : mode == 2 ? "input and output" : "analog",
	    c[GPIO_REG_SUPPLY - GPIO_REG_MODE] & 7, pull, pull < 4 ? "up" : pull == 4 ? "down" : "none",
	    (c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80) ? "enabled" : "DISABLED", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7],
	    (status & GPIO_LEVEL) ? "high (released)" : "LOW (held)");
	if (mode != 0 || pull >= 4 || !(c[GPIO_REG_ENABLE - GPIO_REG_MODE] & 0x80)) {
		printf("buttons: %s's pin is not an enabled input with a pull-up. Making it one is a write to the PMIC, "
		    "which this driver does not do: %s is left alone\n", b->name, b->name);
		return;
	}
	b->usable = true;
	b->last = !(status & GPIO_LEVEL);
}

static void
say_summary(void)
{
	printf("buttons: so far %u presses of power, %u of volume up, %u of volume down; now%s%s%s%s; %llu reads of the bus, "
	    "%llu us each (the longest %llu), %llu failed (dropped: %llu in the first %u s of watching, %llu after; %llu asked "
	    "again; %llu waited for the loader's core)\n",
	    power.presses, volume_up.presses,
	    volume_down.presses + volume_down2.presses, power.down ? " power" : "", volume_up.down ? " volume-up" : "",
	    (volume_down.down || volume_down2.down) ? " volume-down" : "",
	    (power.down || volume_up.down || volume_down.down || volume_down2.down) ? " held" : " none held", bus.reads,
	    bus.reads ? bus.total_ns / bus.reads / 1000 : 0, bus.longest_ns / 1000, bus.failed, bus.dropped_early, DROP_EARLY_S,
	    bus.dropped_late, bus.retried, bus.yielded);
}

static void
buttons_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter, version, channels;
	bool pon_known, pin_known, pin2_known = false;
	uint8_t pin_subtype, pin2_subtype = 0;

	if (buttons_after != 0) {
		printf("buttons: the bus is left to the loader's core for %u s (buttons_after)\n", buttons_after);
		delay_for_interval(buttons_after * 1000, NSEC_PER_MSEC);
	}
	printf("buttons: step 1: map the PMIC arbiter (%#lx, %#lx, %#lx) and read its version\n", ARB_CORE_PHYS, ARB_CNFG_PHYS,
	    ARB_OBSERVER_PHYS);
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	printf("buttons: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		printf("buttons: this driver knows version 5 of the arbiter only: no buttons\n");
		goto out;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	if (steps < 2) {
		goto out;
	}

	printf("buttons: step 2: look through the arbiter's table for the channels of the two peripherals\n");
	find_channel(&pon, channels);
	find_channel(&pin, channels);
	if (pin2.id != 0) {
		find_channel(&pin2, channels);
	}
	if (steps < 3) {
		goto out;
	}

	printf("buttons: step 3: the first transfers on the SPMI bus\n");
	pon_known = identify(&pon, PON_TYPE, pon_subtype_from, pon_subtype_to);
	pin_known = identify(&pin, GPIO_TYPE, pin2.id != 0 ? GPIO_SUBTYPE_OLD_FROM : GPIO_SUBTYPE_LV, GPIO_SUBTYPE_MV_VIN3);
	pin_subtype = identified_subtype;
	if (pin2.id != 0) {
		pin2_known = identify(&pin2, GPIO_TYPE, GPIO_SUBTYPE_OLD_FROM, GPIO_SUBTYPE_MV_VIN3);
		pin2_subtype = identified_subtype;
	}
	if (steps < 4) {
		goto out;
	}

	printf("buttons: step 4: how the keys' inputs are set up\n");
	if (pon_known) {
		pon_inputs();
	}
	if (pin_known) {
		pin_input(&pin, pin_subtype, &volume_up);
	}
	if (pin2_known) {
		pin_input(&pin2, pin2_subtype, &volume_down2);
	}
	if (!power.usable && !volume_up.usable) {
		printf("buttons: nothing that can be read: no buttons\n");
		goto out;
	}
	if (power.last || volume_down.last || volume_up.last) {
		printf("buttons: a key reads as held before anything was watched:%s%s%s. It is not reported until it has been "
		    "seen released\n", power.last ? " power" : "", volume_up.last ? " volume-up" : "",
		    volume_down.last ? " volume-down" : "");
	}
	/* (a key that is released now is believed at once) */
	power.live = power.usable && !power.last;
	volume_down.live = volume_down.usable && !volume_down.last;
	volume_up.live = volume_up.usable && !volume_up.last;
	volume_down2.live = volume_down2.usable && !volume_down2.last;
	if (steps < 5) {
		goto out;
	}

	printf("buttons: step 5: watching%s%s%s, every %u ms: power is the side button\n", power.usable ? " power" : "",
	    volume_up.usable ? " volume-up" : "", volume_down.usable ? " volume-down" : "", poll_ms);
	watch_started = mach_absolute_time();
	for (unsigned tick = 1;; tick++) {
		buttons_poll();
		/* once early on as well: a short run is to say what the reads cost too */
		if (tick % (60 * 1000 / poll_ms) == 0 || tick == 10 * 1000 / poll_ms) {
			say_summary();
		}
		if (bus.failed >= 1000 && bus.failed * 2 > bus.reads) {
			printf("buttons: more than half of %llu reads have failed: giving up\n", bus.reads);
			break;
		}
		delay_for_interval(poll_ms, NSEC_PER_MSEC);
	}
out:
	if (steps < 5) {
		printf("buttons: stopping here (buttons_steps=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

/*
 * boot-arg press_power=<seconds>: a short press of the power key that many seconds after boot, and another ten
 * seconds later, given to Darwin as the key's own are (hakios_touch_post): the lock, and the wake, tried with nobody
 * at the phone. press_power_ms=<ms> is how long each is held (default 150: a tap; 3000 and up is a long press).
 */
static uint32_t press_ms = 150, press_left;
static thread_call_t press_call;

static void
press_step(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static bool down;
	uint64_t deadline;
	char when[64];

	down = !down;
	now_text(when, sizeof(when), mach_absolute_time());
	printf("buttons: press_power: power %s at %s\n", down ? "PRESSED (by the test)" : "released (by the test)", when);
	hakios_touch_post(down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, HAKIOS_BUTTON_LOCK, 0, 0);
	if (down) {
		clock_interval_to_deadline(press_ms, NSEC_PER_MSEC, &deadline);
	} else if (--press_left > 0) {
		clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
	} else {
		return;
	}
	thread_call_enter_delayed(press_call, deadline);
}

static void
press_power_init(void)
{
	uint32_t seconds = 0;
	uint64_t deadline;

	if (!PE_parse_boot_argn("press_power", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	PE_parse_boot_argn("press_power_ms", &press_ms, sizeof(press_ms));
	press_left = 2;
	press_call = thread_call_allocate(press_step, NULL);
	clock_interval_to_deadline(seconds, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(press_call, deadline);
	printf("buttons: press_power: the power key will be pressed for %u ms at %u s and at %u s\n", press_ms, seconds,
	    seconds + 10);
}

void
hakios_buttons_init(void)
{
	uint32_t enabled = 0, steps = 5;
	thread_t thread;

	press_power_init();

	/*
	 * On with the touch screen (the phone, somebody at it) unless buttons=0 says otherwise: a hand
	 * has pressed all three keys and each press and release was read as it was made.
	 */
	if (!PE_parse_boot_argn("buttons", &enabled, sizeof(enabled))) {
		uint32_t touch_screen = 0;

		enabled = PE_parse_boot_argn("touchscreen", &touch_screen, sizeof(touch_screen)) && touch_screen != 0;
	}
	if (!enabled) {
		return;
	}
	/* the phones: 1 redwood (as above), 2 the OnePlus 6T */
	if (enabled == 2) {
		pon = (struct peripheral){ .name = "the PM8998's power-on block", .id = 0x008 };     /* registers 0x800 of slave 0 */
		pin = (struct peripheral){ .name = "GPIO6 of the PM8998", .id = 0x0c5 };             /* registers 0xc500 */
		pin2 = (struct peripheral){ .name = "GPIO5 of the PM8998", .id = 0x0c4 };            /* registers 0xc400 */
		pon_kpdpwr = 0x01;
		pon_resin = 0x02;
		pon_subtype_from = PON_SUBTYPE_OLD_FROM;
		pon_subtype_to = PON_SUBTYPE_OLD_TO;
		buttons_after = 25;
		/* volume down is GPIO5 here; RESIN_N, which nothing was seen to press, only logged */
		volume_down.name = "RESIN_N";
		volume_down.record = 0;
		volume_down2.name = "volume down";
		volume_down2.record = HAKIOS_BUTTON_VOLUME_DOWN;
	}
	PE_parse_boot_argn("buttons_steps", &steps, sizeof(steps));
	PE_parse_boot_argn("buttons_poll_ms", &poll_ms, sizeof(poll_ms));
	PE_parse_boot_argn("buttons_after", &buttons_after, sizeof(buttons_after));
	if (poll_ms == 0 || poll_ms > 1000) {
		poll_ms = POLL_MS;
	}
	if (steps == 0) {
		return;
	}
	if (kernel_thread_start(buttons_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * What interrupts would take, instead of looking 60 times a second:
 *
 *   - The arbiter has one interrupt line for all the PMICs' peripherals ("periph_irq": through the
 *     power domain controller's pin 1 to the interrupt controller's SPI 481). This kernel has no
 *     driver that takes a hardware interrupt other than the timer's so far; that is the first
 *     thing to add (a handler registered for one more interrupt of the GICv3 the kernel already
 *     drives, and that line unmasked and set to level, high).
 *   - Per channel, the arbiter has an enable bit ("ACC_ENABLE"), a status and a clear register
 *     (in the "chnls" region: 0x100, 0x104, 0x108 of 0x10000 per channel), and per system a
 *     summary of which channels have something. Those are writes to the arbiter, not to a PMIC.
 *   - But the peripheral must be told to raise them as well: for each key a bit in "interrupt
 *     type" (edge), "polarity high" and "polarity low" (both edges), and "enable set". Those are
 *     WRITES TO THE PMIC -- to the power-on block for power and volume down, to the GPIO for
 *     volume up -- unless the firmware has left them set; step 4 prints what it finds there.
 *     After an interrupt the peripheral's latched status is cleared by another write to it.
 *   - Only the system that owns a peripheral's channel gets its interrupts (step 2 prints the
 *     owners).
 * Looking costs two reads of some tens of microseconds each, 60 times a second (the minute's
 * summary line says what they really take), so the reason to want interrupts would not be
 * processor time; it would be a system that sleeps and is to be woken by a key, and this one
 * does not sleep.
 */
