/*
 * The vibration motor of the OnePlus 6T (fajita): a linear resonant actuator driven by the haptics block
 * of the PMI8998 (slave 3, peripheral 0xc0: registers 0xc000), reached through the PMIC arbiter as the
 * buttons are (hakios.buttons.c's hakios_spmi_read()). How Android drives it, from the phone's device
 * tree (qcom,haptics@c000, "qcom,qpnp-haptics"):
 *
 *   actuator   LRA (qcom,actuator-type 0), 2088 mV at most (vmax-mv), 800 mA (ilim-ma)
 *   wave       sine, buffer play mode, a period of 4255 us (wave-play-rate-us: about 235 Hz), resonance
 *              followed automatically (lra-auto-mode, auto-res-mode zxd-eop), braking (brake-pattern 3 3 3 3)
 *
 * Register layout: Linux's downstream drivers/soc/qcom/qpnp-haptic.c (msm-4.9, the phone's own kernel).
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   pmi_haptics=1   a look, reading only: which channel of the arbiter is the block's and which system
 *                   may write through it, what the block says it is, and its registers as UEFI left them.
 *                   (On the 6T: channel 250, ours to write through; type 0x15, subtype 0x9; the maximum
 *                   voltage and the play rate already Android's, 2088 mV and 4255 us.)
 *   pmi_haptics=2   ... and configured for a test, each write read back: 800 mA (ILIM), Android's brake
 *                   pattern, and "direct" play (the motor driven at the maximum voltage while PLAY is set;
 *                   Android plays from a buffer of wave samples instead). Enable and play stay 0: nothing
 *                   moves. At the run's end the three registers are put back as they were found.
 *
 * Registers (offsets in the block): 0x46 enable ([7]), 0x4e play mode ([5:4]: 0 buffer, 1 direct), 0x51 the
 * maximum voltage ([6:1] x 116 mV), 0x52 current limit ([0]: 400 or 800 mA), 0x54/0x55 the play rate (x 5 us),
 * 0x5c brake pattern (four 2-bit steps), 0x60-0x67 wave samples, 0x70 play ([7]).
 *
 * Like every user of the PMIC bus here it waits until the run's first 24 s are over (the loader's core
 * then runs UEFI's PMIC drivers; see hakios.buttons.c's buttons_after): pmi_haptics_after=S, 25.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);
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
	printf("%s", line);
	return length;
}

#define HAPTICS_ID              0x3c0           /* slave 3, peripheral 0xc0 */
#define REG_TYPE                0x04            /* and 0x05, the subtype */

static uint32_t level, after_s = 25;

#define REG_ENABLE              0x46
#define REG_PLAY_MODE           0x4e
#define REG_ILIM                0x52
#define REG_BRAKE               0x5c
#define REG_PLAY                0x70

/* what level 2 writes, and what it found there (put back at the run's end) */
static struct {
	uint8_t reg, value, found;
	const char *what;
} config[] = {
	{ REG_ILIM, 0x01, 0, "current limit 800 mA" },
	{ REG_BRAKE, 0xff, 0, "brake pattern 3 3 3 3" },
	{ REG_PLAY_MODE, 0x10, 0, "direct play" },
};
static unsigned configured;     /* how many of config[] were written */

static bool
write_checked(uint8_t reg, uint8_t value, const char *what)
{
	uint8_t back = 0;
	const char *problem = hakios_spmi_write(HAPTICS_ID, reg, &value, 1);

	if (problem == NULL) {
		problem = hakios_spmi_read(HAPTICS_ID, reg, &back, 1);
	}
	if (problem != NULL || back != value) {
		say("pmi_haptics: 0x%02x <- 0x%02x (%s): %s (reads 0x%02x)\n", reg, value, what,
		    problem != NULL ? problem : "NOT what was written", back);
		return false;
	}
	say("pmi_haptics: 0x%02x <- 0x%02x (%s), read back\n", reg, value, what);
	return true;
}

/* level 2: the test's configuration, after the originals are kept */
static bool
configure(void)
{
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (hakios_spmi_read(HAPTICS_ID, config[i].reg, &config[i].found, 1) != NULL) {
			say("pmi_haptics: 0x%02x does not read: not configured\n", config[i].reg);
			return false;
		}
	}
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		configured = i + 1;     /* (put back even if this one did not take) */
		if (!write_checked(config[i].reg, config[i].value, config[i].what)) {
			return false;
		}
	}
	return true;
}

/* the block's registers, eight at a time, from `from' to `to' (inclusive, multiples of 8) */
static void
dump(uint8_t from, uint8_t to)
{
	for (unsigned reg = from; reg <= to; reg += 8) {
		uint8_t r[8];
		const char *problem = hakios_spmi_read(HAPTICS_ID, (uint8_t)reg, r, sizeof(r));

		if (problem != NULL) {
			say("pmi_haptics: 0x%02x..: %s\n", reg, problem);
			continue;
		}
		say("pmi_haptics: 0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x\n", reg, r[0], r[1], r[2], r[3], r[4],
		    r[5], r[6], r[7]);
	}
}

static void
pmi_haptics_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t owner = 0;
	uint8_t is[2];
	const char *problem;
	int channel;

	say("pmi_haptics: level %u: %s; the bus is left to the loader's core for %u s\n", level,
	    level == 1 ? "a look, reading only" : "a look, then configured (nothing moves)", after_s);
	delay_for_interval(after_s * 1000, NSEC_PER_MSEC);
	channel = hakios_spmi_channel(HAPTICS_ID, &owner);
	if (channel < 0) {
		say("pmi_haptics: no channel of the arbiter is the haptics block (slave 3, peripheral 0xc0)\n");
		goto out;
	}
	say("pmi_haptics: channel %d is the haptics block; written by system %u%s\n", channel, owner,
	    owner == 0 ? " (ours)" : " (NOT ours: this system cannot write to it)");
	problem = hakios_spmi_read(HAPTICS_ID, REG_TYPE, is, 2);
	if (problem != NULL) {
		say("pmi_haptics: reading its type: %s\n", problem);
		goto out;
	}
	say("pmi_haptics: type %#x, subtype %#x\n", is[0], is[1]);
	say("pmi_haptics: its registers as UEFI left them:\n");
	dump(0x00, 0x7f);
	dump(0xd0, 0xef);
	if (level >= 2) {
		if (owner != 0) {
			say("pmi_haptics: its channel is not ours to write through: not configured\n");
			goto out;
		}
		if (configure()) {
			say("pmi_haptics: configured; enable and play left at 0 (nothing moves)\n");
			dump(0x40, 0x5f);
			dump(0x70, 0x77);
		}
	}
out:
	say("pmi_haptics: done\n");
	thread_terminate(current_thread());
}

void
hakios_pmi_haptics_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("pmi_haptics", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 2) {
		level = 2;
	}
	PE_parse_boot_argn("pmi_haptics_after", &after_s, sizeof(after_s));
	if (kernel_thread_start(pmi_haptics_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end (a panic's too), with interrupts off: play and enable off, the registers as they were found. */
void
hakios_pmi_haptics_quiesce(void)
{
	uint8_t off = 0;
	const char *problem;

	if (configured == 0) {
		return;
	}
	problem = hakios_spmi_write(HAPTICS_ID, REG_PLAY, &off, 1);
	if (problem == NULL) {
		problem = hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &off, 1);
	}
	for (unsigned i = configured; problem == NULL && i-- > 0;) {
		problem = hakios_spmi_write(HAPTICS_ID, config[i].reg, &config[i].found, 1);
	}
	say("pmi_haptics: at the run's end: play and enable off, %u registers as they were found%s%s\n", configured,
	    problem != NULL ? ": " : "", problem != NULL ? problem : "");
	configured = 0;
}
