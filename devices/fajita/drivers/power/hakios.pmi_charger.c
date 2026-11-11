/*
 * The OnePlus 6T's battery charger: the SMB2 charger of the PMI8998 (Linux downstream: qcom,qpnp-smb2,
 * drivers/power/supply/qcom/smb-reg.h, smb-lib.c), on the PMI8998's first SPMI ID -- slave 2, its blocks
 * CHGR 0x1000, USBIN 0x1300 and MISC 0x1600 (the haptics are on its second, slave 3: hakios.pmi_haptics.c).
 *
 * Why: during a run the battery runs down although the phone is plugged in (runs 181806, 185309..190430:
 * "charger=1 charging=0", the fuel gauge's current +612 mA, out of the battery). Android charges from the
 * same port. Whether the charger is off, suspended, held at a small input limit or stopped by its own
 * watchdog (UEFI's QcomChargerDxe arms it; nobody pets it once the kernel runs) is what this looks at first.
 *
 *   pmi_charger=1   reading only: which blocks answer and what they say they are,
 *                   then every pmi_charger_every=S seconds (30) one line: the charger's state, its input
 *                   (the adapter detected, USB suspended or not, the input current limit asked and in effect,
 *                   AICL), and its watchdog. Nothing is written.
 *   pmi_charger=2   (the default on the 6T) ... and on a computer's port (the adapter SDP) the input current limit raised from USB 2.0's
 *                   500 mA to USB 3.0's 900 mA: USBIN_CURRENT_LIMIT_CFG (0x1370) at 900 mA, then USBIN_ICL_OPTIONS
 *                   (0x1366) [2:0] = high-current mode, the mode UEFI leaves it in (found 0x03, 0x1370 at 500 mA).
 *                   Linux's way for a computer's port, set_sdp_current() (USB3 | USB51, high-current mode off), did
 *                   NOT move the limit in effect here (runs 193804, 194313: still 500 mA 2 s after). 900 mA is the
 *                   most this driver asks; only those two registers are written, each read back; at the run's end
 *                   both go back to what they were found to be.
 *                   And the battery's fast-charge current (CHGR 0x1061), which UEFI leaves at 0 -- the input
 *                   had room at times in run 193804 and the battery took none of it (-9 mA) -- set to
 *                   pmi_charger_fcc_ma=N (500: OnePlus's own qcom,fcc-max-ua for the 6T, fajita.dtsi; its driver
 *                   allows up to 1950 mA at room temperature, less cool or warm; 500 is the most this driver asks),
 *                   only while the charger's own temperature flags (CHGR 0x1007 [3:0], JEITA) are all clear, and
 *                   put back as found the moment one is not, and at the run's end.
 *                   (Run 192230, at 500 mA: the charger "full-on" but input-limited, the battery giving 291 to
 *                   511 mA besides -- a run takes more than a computer's port gives.)
 *
 * Like every user of the PMIC bus here it waits until the run's first 25 s are over (the loader's core then
 * runs UEFI's PMIC drivers; hakios.buttons.c's buttons_after): pmi_charger_after=S, 25.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

#define CHGR_ID         0x210           /* slave 2, peripheral 0x10: the charger proper */
#define USBIN_ID        0x213           /* the USB input */
#define MISC_ID         0x216           /* input current limit in effect, AICL, power path, watchdog */

/* CHGR */
#define CHGR_STATUS_1   0x06            /* [2:0] the charge state */
#define CHGR_STATUS_2   0x07            /* [7] input current limited, [6] safety timer, [5] battery OV, [4] no termination, [3:0] temperature */
#define CHGR_EN_CMD     0x42            /* [0] charging enabled by command */
#define CHGR_CFG2       0x51            /* [7] enable source (1: the pin), [6] polarity, [0] charge inhibit */
#define CHGR_FCC        0x61            /* fast-charge current, 25 mA steps */
#define CHGR_FV         0x70            /* float voltage, 7.5 mV steps over 3487.5 mV */
/* USBIN */
#define USBIN_INPUT     0x06            /* [0] 5 V, [3] 9 V, [5] 12 V present */
#define USBIN_APSD      0x08            /* [6] QC3 [5] QC2 [4] float [3] DCP [2] CDP [1] OCP [0] SDP; [7] ICL override latched */
#define USBIN_CMD_IL    0x40            /* [0] USB input suspended by command */
#define USBIN_CMD_APSD  0x41            /* [1] ICL override */
#define USBIN_ICL_OPT   0x66            /* [0] USBIN_MODE_CHG (HC mode), [1] USB51 mode, [2] USB 3 */
#define USBIN_ICL_CFG   0x70            /* the input current limit asked, 25 mA steps */
/* MISC */
#define MISC_ICL_STS    0x07            /* the input current limit in effect, 25 mA steps */
#define MISC_AICL_STS   0x0a            /* [4] collapse, [2] at the minimum, [1] failed, [0] done */
#define MISC_PATH_STS   0x0b            /* [6] USB suspended, [4] using USB, [2:1] path, [0] a valid input */
#define MISC_WDOG_STS   0x0c            /* [0] bark/bite */
#define MISC_WD_CFG     0x51            /* [0] watchdog on, [1] on at plug-in */
#define MISC_WD_CFG2    0x53            /* [7] a bite stops charging; timeouts */

static uint32_t level = 2, after_s = 25, every_s = 30;
static volatile int32_t seen_input = -1, seen_usb_type;        /* from the latest look: a valid input, the adapter (-1: not yet) */
static uint8_t found_opt, found_cfg;            /* USBIN_ICL_OPTIONS and USBIN_ICL_CFG as found (pmi_charger=2) */
static volatile bool raised;                    /* written, to be put back at the run's end */
static uint8_t found_fcc;                       /* CHGR_FCC as found */
static volatile bool fcc_set;                   /* CHGR_FCC written, to be put back */
static uint32_t fcc_ma = 500;
#define FCC_MAX_MA      500                     /* OnePlus's qcom,fcc-max-ua for the 6T: the most this driver asks */

static void fcc_follow(uint8_t s2);


#define RAISED_MA       900                     /* USB 3.0's 900 mA: the most this driver asks of a computer's port */

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
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

static bool
rd(uint16_t id, uint8_t reg, uint8_t *value)
{
	const char *problem = hakios_spmi_read(id, reg, value, 1);

	if (problem != NULL) {
		say("pmi_charger: %#x+0x%02x: %s\n", id, reg, problem);
		return false;
	}
	return true;
}

static void
sleep_s(uint32_t seconds)
{
	delay_for_interval(seconds * 1000, NSEC_PER_MSEC);
}

static const char *
charge_state(uint8_t s1)
{
	static const char *const names[] = {
		"trickle", "pre-charge", "fast", "full-on", "taper", "terminated", "INHIBITED", "DISABLED",
	};

	return names[s1 & 7];
}

static const char *
adapter(uint8_t apsd)
{
	return (apsd & 0x40) ? "QC3" : (apsd & 0x20) ? "QC2" : (apsd & 0x10) ? "floating" : (apsd & 0x08) ? "DCP (wall)" :
	       (apsd & 0x04) ? "CDP" : (apsd & 0x02) ? "OCP" : (apsd & 0x01) ? "SDP (a computer's port)" : "none detected";
}

static void
look(void)
{
	uint8_t s1 = 0, s2 = 0, en = 0, cfg2 = 0, fcc = 0, fv = 0, in = 0, apsd = 0, il = 0, cmd_apsd = 0, opt = 0, icl = 0,
	    icl_sts = 0, aicl = 0, path = 0, wdog = 0, wd = 0, wd2 = 0;

	if (!rd(CHGR_ID, CHGR_STATUS_1, &s1) || !rd(CHGR_ID, CHGR_STATUS_2, &s2) || !rd(CHGR_ID, CHGR_EN_CMD, &en) ||
	    !rd(CHGR_ID, CHGR_CFG2, &cfg2) || !rd(CHGR_ID, CHGR_FCC, &fcc) || !rd(CHGR_ID, CHGR_FV, &fv) ||
	    !rd(USBIN_ID, USBIN_INPUT, &in) || !rd(USBIN_ID, USBIN_APSD, &apsd) || !rd(USBIN_ID, USBIN_CMD_IL, &il) ||
	    !rd(USBIN_ID, USBIN_CMD_APSD, &cmd_apsd) || !rd(USBIN_ID, USBIN_ICL_OPT, &opt) || !rd(USBIN_ID, USBIN_ICL_CFG, &icl) ||
	    !rd(MISC_ID, MISC_ICL_STS, &icl_sts) || !rd(MISC_ID, MISC_AICL_STS, &aicl) || !rd(MISC_ID, MISC_PATH_STS, &path) ||
	    !rd(MISC_ID, MISC_WDOG_STS, &wdog) || !rd(MISC_ID, MISC_WD_CFG, &wd) || !rd(MISC_ID, MISC_WD_CFG2, &wd2)) {
		return;
	}
	say("pmi_charger: charging %s (status 0x%02x 0x%02x%s); enable cmd %u, cfg2 0x%02x (source %s%s); fast-charge %u mA, "
	    "float %u.%u mV\n", charge_state(s1), s1, s2, (s2 & 0x80) ? ": INPUT CURRENT LIMITED" : "", en & 1, cfg2,
	    (cfg2 & 0x80) ? "the pin" : "the command", (cfg2 & 1) ? ", inhibit on" : "", fcc * 25u, (34875u + fv * 75u) / 10u,
	    (34875u + fv * 75u) % 10u);
	say("pmi_charger: USB input 0x%02x, adapter %s (0x%02x); suspended by command %u, ICL override %u, options 0x%02x; "
	    "input limit asked %u mA, IN EFFECT %u mA; AICL 0x%02x%s%s; path 0x%02x%s%s; watchdog cfg 0x%02x 0x%02x%s, status 0x%02x\n",
	    in, adapter(apsd), apsd, il & 1, (cmd_apsd >> 1) & 1, opt, icl * 25u, icl_sts * 25u, aicl, (aicl & 1) ? " done" : "",
	    (aicl & 2) ? " FAILED" : "", path, (path & 0x40) ? " USB SUSPENDED" : "", (path & 1) ? " valid input" : " NO VALID INPUT",
	    wd, wd2, (wd & 1) && (wd2 & 0x80) ? " (ON: a bite stops charging)" : (wd & 1) ? " (on)" : " (off)", wdog);
	if (level >= 2 && raised) {
		fcc_follow(s2);
	}
	/* (Linux's POWER_SUPPLY_USB_TYPE_*: 1 SDP, 2 DCP, 3 CDP; 0 none or another) */
	seen_usb_type = (apsd & 0x01) ? 1 : (apsd & 0x08) ? 2 : (apsd & 0x04) ? 3 : 0;
	seen_input = (path & 1) ? 1 : 0;
}

/*
 * For the fuel gauge's reports (hakios.bq27541.c): whether the charger has a valid input, and what kind, as the
 * latest look found them; false before the first look.
 */
bool
hakios_pmi_charger_input(int32_t *external, int32_t *usb_type)
{
	if (seen_input < 0) {
		return false;
	}
	*external = seen_input;
	*usb_type = seen_usb_type;
	return true;
}

static bool
wr_checked(uint8_t reg, uint8_t value, const char *what)
{
	uint8_t back = 0;
	const char *problem = hakios_spmi_write(USBIN_ID, reg, &value, 1);

	if (problem == NULL) {
		problem = hakios_spmi_read(USBIN_ID, reg, &back, 1);
	}
	if (problem != NULL || back != value) {
		say("pmi_charger: USBIN+0x%02x <- 0x%02x (%s): %s (reads 0x%02x)\n", reg, value, what,
		    problem != NULL ? problem : "NOT what was written", back);
		return false;
	}
	say("pmi_charger: USBIN+0x%02x <- 0x%02x (%s), read back\n", reg, value, what);
	return true;
}

/* the fast-charge current: set while the temperature flags are clear, put back the moment one is not */
static void
fcc_follow(uint8_t s2)
{
	uint8_t value, back = 0;
	const char *problem;
	bool clear = (s2 & 0x0f) == 0;

	if (clear == fcc_set) {
		return;
	}
	value = clear ? (uint8_t)(fcc_ma / 25) : found_fcc;
	problem = hakios_spmi_write(CHGR_ID, CHGR_FCC, &value, 1);
	if (problem == NULL) {
		problem = hakios_spmi_read(CHGR_ID, CHGR_FCC, &back, 1);
	}
	if (problem != NULL || back != value) {
		say("pmi_charger: CHGR+0x61 <- 0x%02x: %s (reads 0x%02x)\n", value, problem != NULL ? problem : "NOT what was written",
		    back);
		if (clear) {
			fcc_set = true;         /* (whatever is there now goes back at the end) */
		}
		return;
	}
	fcc_set = clear;
	if (clear) {
		say("pmi_charger: THE BATTERY'S FAST-CHARGE CURRENT IS %u mA (it was %u), the temperature flags clear\n", fcc_ma,
		    found_fcc * 25u);
	} else {
		say("pmi_charger: a temperature flag (status 2 0x%02x): the fast-charge current back to %u mA\n", s2, found_fcc * 25u);
	}
}

/* pmi_charger=2: a computer's port's 900 mA (see the top) */
static void
raise_limit(void)
{
	uint8_t apsd = 0, icl_sts = 0;

	if (!rd(USBIN_ID, USBIN_APSD, &apsd) || !rd(USBIN_ID, USBIN_ICL_OPT, &found_opt) || !rd(USBIN_ID, USBIN_ICL_CFG, &found_cfg) ||
	    !rd(CHGR_ID, CHGR_FCC, &found_fcc)) {
		return;
	}
	if ((apsd & 0x7f) != 0x01) {
		say("pmi_charger: the adapter is %s (0x%02x), not a computer's port: its limit left as it is\n", adapter(apsd), apsd);
		return;
	}
	raised = true;          /* (from the first write on: put back at the end whatever happens) */
	if (!wr_checked(USBIN_ICL_CFG, RAISED_MA / 25, "the input current limit, 900 mA") ||
	    !wr_checked(USBIN_ICL_OPT, (uint8_t)((found_opt & ~0x07) | 0x01), "high-current mode")) {
		return;
	}
	delay_for_interval(2000, NSEC_PER_MSEC);
	if (rd(MISC_ID, MISC_ICL_STS, &icl_sts)) {
		say("pmi_charger: the input limit in effect: %u mA%s\n", icl_sts * 25u, icl_sts * 25u >= RAISED_MA ? " (RAISED)" :
		    " (NOT RAISED)");
	}
}

static void
pmi_charger_thread(__unused void *arg, __unused wait_result_t wr)
{
	static const struct { uint16_t id; const char *name; } blocks[] = {
		{ CHGR_ID, "CHGR" }, { USBIN_ID, "USBIN" }, { MISC_ID, "MISC" },
	};

	sleep_s(after_s);
	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		uint32_t owner = 0;
		int channel = hakios_spmi_channel(blocks[i].id, &owner);
		uint8_t type[2] = { 0, 0 };
		const char *problem = channel < 0 ? "no channel" : hakios_spmi_read(blocks[i].id, 0x04, type, 2);

		say("pmi_charger: %s (%#x): channel %d, written by system %u; type 0x%02x subtype 0x%02x%s%s\n", blocks[i].name,
		    blocks[i].id, channel, owner, type[0], type[1], problem ? ": " : "", problem ? problem : "");
		if (problem != NULL) {
			say("pmi_charger: done\n");
			thread_terminate(current_thread());
			return;
		}
	}
	look();
	if (level >= 2) {
		raise_limit();
	}
	for (;;) {
		look();
		sleep_s(every_s);
	}
}

void
hakios_pmi_charger_init(void)
{
	thread_t thread;

	(void)PE_parse_boot_argn("pmi_charger", &level, sizeof(level));
	if (level == 0) {
		return;
	}
	(void)PE_parse_boot_argn("pmi_charger_after", &after_s, sizeof(after_s));
	(void)PE_parse_boot_argn("pmi_charger_every", &every_s, sizeof(every_s));
	(void)PE_parse_boot_argn("pmi_charger_fcc_ma", &fcc_ma, sizeof(fcc_ma));
	if (fcc_ma > FCC_MAX_MA) {
		fcc_ma = FCC_MAX_MA;
	}
	if (every_s < 5) {
		every_s = 5;
	}
	if (kernel_thread_start(pmi_charger_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end (a panic's too), with interrupts off: the input limit's two registers as they were found. */
void
hakios_pmi_charger_quiesce(void)
{
	const char *problem;

	if (!raised) {
		return;
	}
	raised = false;         /* (once, whatever happens below) */
	if (fcc_set) {
		fcc_set = false;
		problem = hakios_spmi_write(CHGR_ID, CHGR_FCC, &found_fcc, 1);
		say("pmi_charger: at the run's end: the fast-charge current back to %u mA%s%s\n", found_fcc * 25u,
		    problem != NULL ? ": " : "", problem != NULL ? problem : "");
	}
	problem = hakios_spmi_write(USBIN_ID, USBIN_ICL_CFG, &found_cfg, 1);
	if (problem == NULL) {
		problem = hakios_spmi_write(USBIN_ID, USBIN_ICL_OPT, &found_opt, 1);
	}
	say("pmi_charger: at the run's end: the input limit's registers as they were found (0x%02x, 0x%02x)%s%s\n", found_opt,
	    found_cfg, problem != NULL ? ": " : "", problem != NULL ? problem : "");
}
