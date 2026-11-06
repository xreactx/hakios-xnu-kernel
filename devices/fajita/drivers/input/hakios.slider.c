/*
 * The OnePlus 6T's alert slider: a three-position switch on the phone's side, read from three pins of the
 * SoC's TLMM -- the phone's device tree's tri_state_key ("oneplus,tri-state-key"): tristate,gpio_key1 is
 * GPIO 24, key2 GPIO 52, key3 GPIO 126, all "bias-disable" (the board pulls them). Their tiles, from Linux
 * drivers/pinctrl/qcom/pinctrl-sdm845.c: 24 the south one, 52 the north, 126 the east; each pin's
 * registers are its own 4 KB page there (as hakios.synaptics.c maps its pins).
 *
 *   slider=1   a look, reading only: the three pins' setup and levels at the start, then every 50 ms,
 *              and each change said with its time. Nothing is written to the pins. (On the 6T UEFI leaves
 *              all three inputs with the SoC's pull-down on, and they read 0 whatever the slider does.)
 *   slider=2   ... with the pull switched off first, as Android has them (bias-disable): only the two pull
 *              bits of each pin's configuration change, and they are put back at the run's end. The
 *              selected position pulls its pin low: key1 in the lower one (ring), key2 in the other; key3
 *              (the third position) is not reached on this phone. Between two, all three read 1.
 *   slider=3   ... and Darwin told, as its ring/silent switch (HAKIOS_BUTTON_RINGER to /dev/hakios-touch,
 *              which backboardd's libhakios.dylib makes a HID event of): ring while key1 is low, silent
 *              in any other position, once at the start and at every change; between positions nothing.
 *              (Darwin's switch is the telephony page's usage 0x2e, "down" = ring; backboardd keeps the
 *              last position and SpringBoard asks it for that when it starts.)
 *
 * No PMIC and no shared bus is involved: unlike the buttons there is nothing to wait out at the start.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);

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
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

#define TLMM_PHYS               0x03400000UL
#define TILE_NORTH              0x500000UL
#define TILE_SOUTH              0x900000UL
#define TILE_EAST               0x100000UL
#define CFG_OUTPUT              (1u << 9)       /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define POLL_MS                 50

static const struct {
	unsigned pin;
	vm_offset_t tile;
	const char *name;
} pins[] = {
	{ 24, TILE_SOUTH, "key1" },
	{ 52, TILE_NORTH, "key2" },
	{ 126, TILE_EAST, "key3" },
};
#define PINS                    (sizeof(pins) / sizeof(pins[0]))

static vm_offset_t page[PINS];
static uint32_t level, found_cfg[PINS];
static bool pulls_changed;

static unsigned
levels(void)
{
	unsigned bits = 0;

	for (unsigned i = 0; i < PINS; i++) {
		bits |= (*(volatile uint32_t *)(page[i] + 4) & 1) << i;
	}
	return bits;
}

/* level 3: the position to Darwin -- ring while key1 (the lower position) is low, silent in any other */
static int reported = -1;       /* 0 ring, 1 silent */

static void
report(unsigned bits)
{
	int silent;

	if (bits == 7) {
		return;                 /* between two positions */
	}
	silent = (bits & 1) != 0;
	if (silent == reported) {
		return;
	}
	reported = silent;
	hakios_touch_post(silent ? HAKIOS_BUTTON_UP : HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_RINGER, 0, 0);
	say("slider: Darwin told: %s\n", silent ? "SILENT" : "RING");
}

static void
slider_thread(__unused void *arg, __unused wait_result_t wr)
{
	unsigned last, now, changes = 0;
	uint64_t ns;

	for (unsigned i = 0; i < PINS; i++) {
		page[i] = ml_io_map(TLMM_PHYS + pins[i].tile + 0x1000UL * pins[i].pin, 0x1000);
	}
	say("slider: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the pulls off, then a look" :
	    "the pulls off, and Darwin told");
	for (unsigned i = 0; i < PINS; i++) {
		uint32_t cfg = *(volatile uint32_t *)page[i], io = *(volatile uint32_t *)(page[i] + 4);

		say("slider:   gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].name,
		    (cfg >> 2) & 15, cfg & 3, (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
	if (level >= 2) {
		for (unsigned i = 0; i < PINS; i++) {
			found_cfg[i] = *(volatile uint32_t *)page[i];
		}
		pulls_changed = true;
		for (unsigned i = 0; i < PINS; i++) {
			*(volatile uint32_t *)page[i] = found_cfg[i] & ~3u;
		}
		delay_for_interval(10, NSEC_PER_MSEC);
		say("slider: the pulls switched off (as Android has them); configurations now %#x %#x %#x\n",
		    *(volatile uint32_t *)page[0], *(volatile uint32_t *)page[1], *(volatile uint32_t *)page[2]);
	}
	last = levels();
	say("slider: key1 key2 key3 read %u %u %u -- move the slider\n", last & 1, (last >> 1) & 1, (last >> 2) & 1);
	if (level >= 3) {
		report(last);
	}
	for (;;) {
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
		now = levels();
		if (now == last) {
			continue;
		}
		absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
		changes++;
		say("slider: %llu.%03llu s: key1 key2 key3 read %u %u %u (change %u)\n", ns / NSEC_PER_SEC, ns / NSEC_PER_MSEC % 1000,
		    now & 1, (now >> 1) & 1, (now >> 2) & 1, changes);
		last = now;
		if (level >= 3) {
			report(now);
		}
	}
}

void
hakios_slider_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("slider", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 3) {
		level = 3;
	}
	if (kernel_thread_start(slider_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end (a panic's too): the pins' configurations as they were found. */
void
hakios_slider_quiesce(void)
{
	if (!pulls_changed) {
		return;
	}
	pulls_changed = false;
	for (unsigned i = 0; i < PINS; i++) {
		*(volatile uint32_t *)page[i] = found_cfg[i];
	}
	say("slider: at the run's end: the pins' configurations as they were found\n");
}
