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
 *   pmi_haptics=3   ... and one pulse of pmi_haptics_ms=N milliseconds (100) 3 s after that: enable, play,
 *                   the status read while it plays, then play and enable off again.
 *   pmi_haptics=4   ... configured as for 2, and instead of the pulse the motor for Darwin until the run ends:
 *                   what Darwin plays on its "Actuator" (userland/src/audio_plugin.c) comes to
 *                   /dev/hakios-haptic as samples, and is told apart as redwood's driver tells it
 *                   (hakios.haptic.c, haptic=5): a waveform after a pause is a tap -- a pulse of
 *                   pmi_haptics_tap_ms=N (30) -- and one that goes on past 60 ms a vibration, the motor
 *                   held on while it lasts, HAPTIC_MAX_MS at most, then as long a rest. The strength is
 *                   the maximum voltage (0x51): Android's 2088 mV from a third of full scale up, down to
 *                   five eighths of it for the softest, never more. The device is there from the start
 *                   (the plug-in opens it once); what comes before the motor is ready is not played.
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
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
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
	/* (not with interrupts off, at a run's end: console output there is a panic, and the panic's way out ends here again) */
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

#define HAPTICS_ID              0x3c0           /* slave 3, peripheral 0xc0 */
#define REG_TYPE                0x04            /* and 0x05, the subtype */

static uint32_t level, after_s = 25, pulse_ms = 100;

#define REG_ENABLE              0x46
#define REG_PLAY_MODE           0x4e
#define REG_ILIM                0x52
#define REG_BRAKE               0x5c
#define REG_PLAY                0x70
#define REG_STATUS              0x0a            /* (read while it plays) */
#define REG_VMAX                0x51            /* [6:1]: the maximum voltage, in 116 mV */
#define VMAX_ANDROID            18              /* 2088 mV, the phone's device tree's vmax-mv */
#define ENABLE_ON               0x80
#define PLAY_ON                 0x80

/* what level 2 writes, and what it found there (put back at the run's end) */
static struct {
	uint8_t reg, value, found;
	const char *what;
} config[] = {
	{ REG_ILIM, 0x01, 0, "current limit 800 mA" },
	{ REG_BRAKE, 0xff, 0, "brake pattern 3 3 3 3" },
	{ REG_PLAY_MODE, 0x10, 0, "direct play" },
	{ REG_VMAX, 0x24, 0, "maximum voltage 2088 mV" },       /* (UEFI leaves it so; level 4 changes it) */
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

/* level 3: one pulse */
static void pulse(void);

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
pulse(void)
{
	uint8_t on_enable = ENABLE_ON, on_play = PLAY_ON, off = 0, status = 0xff;
	const char *problem, *stop_problem;

	say("pmi_haptics: a pulse of %u ms in 3 s\n", pulse_ms);
	delay_for_interval(3000, NSEC_PER_MSEC);
	problem = hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &on_enable, 1);
	if (problem == NULL) {
		problem = hakios_spmi_write(HAPTICS_ID, REG_PLAY, &on_play, 1);
	}
	if (problem == NULL) {
		delay_for_interval(pulse_ms / 2, NSEC_PER_MSEC);
		(void)hakios_spmi_read(HAPTICS_ID, REG_STATUS, &status, 1);
		delay_for_interval(pulse_ms - pulse_ms / 2, NSEC_PER_MSEC);
	}
	/* stopped whatever happened above */
	stop_problem = hakios_spmi_write(HAPTICS_ID, REG_PLAY, &off, 1);
	if (stop_problem == NULL) {
		stop_problem = hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &off, 1);
	}
	say("pmi_haptics: PULSE %s: status while playing 0x%02x; stopped%s%s\n", problem == NULL ? "played" : problem, status,
	    stop_problem != NULL ? " -- NOT: " : "", stop_problem != NULL ? stop_problem : "");
}


/* ---- pmi_haptics=4: the motor for Darwin (as hakios.haptic.c's haptic=5) ------------------------------- */

#define HAPTIC_MAX_MS           1000
#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long Darwin's waveform is listened to before a tap: its loudness */
#define FOLLOW_US               20000           /* how often a vibration's strength follows Darwin's */
#define TOLD_EVENTS             24              /* the first so many taps and vibrations are each said */

static volatile uint64_t heard_us, quiet_us, onset_us;
static volatile uint32_t heard_peak;
static volatile bool ready;                     /* configured: what comes from here on is played */
static uint32_t tap_ms = 30;
static unsigned taps, loops, capped;

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* From the audio server's I/O thread: never waits (redwood's haptic_write(), the same) */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		if (!ready) {
			continue;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
				heard_peak = 0;
			}
			heard_us = now;
			if (peak > heard_peak) {
				heard_peak = peak;
			}
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* the maximum voltage for a waveform of Darwin's of this loudness: 5/8 of Android's for the softest, Android's from a third up */
static uint8_t
vmax_for(uint32_t peak)
{
	uint32_t low = VMAX_ANDROID * 5 / 8, third = 32767 / 3;

	return (uint8_t)(peak >= third ? VMAX_ANDROID : low + (VMAX_ANDROID - low) * peak / third);
}

static bool
motor(bool on, uint8_t vmax)
{
	uint8_t v = (uint8_t)(vmax << 1), enable = on ? ENABLE_ON : 0, play = on ? PLAY_ON : 0;
	const char *problem = NULL;

	if (on) {
		problem = hakios_spmi_write(HAPTICS_ID, REG_VMAX, &v, 1);
		if (problem == NULL) {
			problem = hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &enable, 1);
		}
		if (problem == NULL) {
			problem = hakios_spmi_write(HAPTICS_ID, REG_PLAY, &play, 1);
		}
	} else {
		problem = hakios_spmi_write(HAPTICS_ID, REG_PLAY, &play, 1);
		if (problem == NULL) {
			problem = hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &enable, 1);
		}
	}
	if (problem != NULL) {
		say("pmi_haptics: switching the motor %s: %s -- given up\n", on ? "on" : "off", problem);
		if (on) {
			uint8_t off = 0;

			(void)hakios_spmi_write(HAPTICS_ID, REG_PLAY, &off, 1);
			(void)hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &off, 1);
		}
		return false;
	}
	return true;
}

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, tap_onset = 0, followed = 0;
	uint8_t playing = 0;

	ready = true;
	say("pmi_haptics: THE MOTOR PLAYS WHAT IOS PLAYS ON ITS ACTUATOR from here on (taps %u ms, vibrations %u ms at most)\n",
	    tap_ms, HAPTIC_MAX_MS);
	for (;;) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;
		uint8_t vmax = vmax_for(peak);

		switch (state) {
		case IDLE:
			/* a waveform that began after a pause, once its first milliseconds say how loud it is: a tap */
			if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
				tap_onset = onset_us;
				if (!motor(true, vmax)) {
					return;
				}
				state = TAP;
				since = now;
				playing = vmax;
				taps++;
				if (taps + loops <= TOLD_EVENTS) {
					say("pmi_haptics: tap %u: Darwin's waveform peaks at %u of 32768: %u ms at %u mV\n", taps, peak, tap_ms,
					    vmax * 116);
				}
			}
			break;
		case TAP:
			/* still going past TAP_US without a pause: a vibration, the motor left on */
			if (active && quiet_us < tap_onset && now - since > TAP_US) {
				state = LOOP;
				followed = now;
				loops++;
				heard_peak = 0;
				if (taps + loops <= TOLD_EVENTS) {
					say("pmi_haptics: vibration %u: Darwin goes on playing (peak %u): held on, %u ms at most\n", loops, peak,
					    HAPTIC_MAX_MS);
				}
				break;
			}
			if (now - since >= tap_ms * 1000ull && !(active && quiet_us < tap_onset)) {
				if (!motor(false, 0)) {
					return;
				}
				state = IDLE;
			}
			break;
		case LOOP:
			if (active && now - followed >= FOLLOW_US) {
				uint8_t v = (uint8_t)(vmax << 1);

				followed = now;
				heard_peak = 0;
				if (peak != 0 && vmax != playing) {
					if (hakios_spmi_write(HAPTICS_ID, REG_VMAX, &v, 1) != NULL) {
						(void)motor(false, 0);
						return;
					}
					playing = vmax;
				}
			}
			if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
				bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

				if (!motor(false, 0)) {
					return;
				}
				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				state = cut ? REST : IDLE;
				if (taps + loops <= TOLD_EVENTS + 1) {
					say("pmi_haptics: vibration %u: %s after %llu ms\n", loops, cut ? "CUT at the limit" :
					    "Darwin stopped, and so does the motor", (now - since) / 1000);
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		delay_for_interval(4, NSEC_PER_MSEC);
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
	    level == 1 ? "a look, reading only" : level == 2 ? "a look, then configured (nothing moves)" :
	    level == 3 ? "configured, then one pulse" : "configured, then the motor for Darwin",
	    after_s);
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
			if (level == 3) {
				pulse();
			} else if (level >= 4) {
				serve();
				say("pmi_haptics: the motor is given up on (%u taps and %u vibrations played, %u cut)\n", taps, loops,
				    capped);
			}
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
	if (level > 4) {
		level = 4;
	}
	PE_parse_boot_argn("pmi_haptics_tap_ms", &tap_ms, sizeof(tap_ms));
	if (tap_ms == 0 || tap_ms > 200) {
		tap_ms = 30;
	}
	if (level >= 4) {
		int major = cdevsw_add(-1, &haptic_cdevsw);

		if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
			say("pmi_haptics: /dev/hakios-haptic cannot be made\n");
		}
	}
	PE_parse_boot_argn("pmi_haptics_after", &after_s, sizeof(after_s));
	PE_parse_boot_argn("pmi_haptics_ms", &pulse_ms, sizeof(pulse_ms));
	if (pulse_ms == 0 || pulse_ms > 1000) {
		pulse_ms = 100;
	}
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
	unsigned count = configured;

	if (count == 0) {
		return;
	}
	configured = 0;         /* (once, whatever happens below) */
	problem = hakios_spmi_write(HAPTICS_ID, REG_PLAY, &off, 1);
	if (problem == NULL) {
		problem = hakios_spmi_write(HAPTICS_ID, REG_ENABLE, &off, 1);
	}
	for (unsigned i = count; problem == NULL && i-- > 0;) {
		problem = hakios_spmi_write(HAPTICS_ID, config[i].reg, &config[i].found, 1);
	}
	say("pmi_haptics: at the run's end: play and enable off, %u registers as they were found%s%s\n", count,
	    problem != NULL ? ": " : "", problem != NULL ? problem : "");
}
