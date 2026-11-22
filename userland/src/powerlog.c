/*
 * powerlog: where the battery goes -- a line a minute (and one at each screen change) into
 * /private/var/mobile/Library/HakiOS/power.log, which var_store keeps: the battery as the 6T's fuel gauge says it
 * (charge, voltage, current, temperature, charging), whether the screen is on (SpringBoard's own
 * com.apple.springboard.hasBlankedScreen), how busy the cores were since the line before (the kernel's ticks per
 * processor), and the hottest of the SoC's sensors.
 *
 * For measuring the drain of a phone that runs with no host (xl standalone): a run over USB charges it. Read it back
 * from Android: tools/var-store-read.py --fajita extract mobile/Library/HakiOS DEST.
 *
 * The file is cut back to its last half when it passes 1 MB (about two weeks of lines).
 */
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <notify.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define LOG     "/private/var/mobile/Library/HakiOS/power.log"
#define MAX     (1u << 20)

static int
num(const char *name)
{
	int v = -1;
	size_t size = sizeof(v);

	return sysctlbyname(name, &v, &size, NULL, 0) == 0 ? v : -1;
}

/* the cores' busy share since the call before, in percent of all cores; -1 the first time */
static int
busy(void)
{
	static uint64_t last_busy, last_all;
	processor_info_array_t info;
	mach_msg_type_number_t count;
	natural_t n;
	uint64_t b = 0, a = 0;
	int pct = -1;

	if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &n, &info, &count) != KERN_SUCCESS) {
		return -1;
	}
	for (natural_t i = 0; i < n; i++) {
		unsigned *t = (unsigned *)info + CPU_STATE_MAX * i;

		b += t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE];
		a += t[CPU_STATE_USER] + t[CPU_STATE_SYSTEM] + t[CPU_STATE_NICE] + t[CPU_STATE_IDLE];
	}
	vm_deallocate(mach_task_self(), (vm_address_t)info, count * sizeof(int));
	if (last_all != 0 && a > last_all) {
		pct = (int)((b - last_busy) * 100 / (a - last_all));
	}
	last_busy = b;
	last_all = a;
	return pct;
}

static void
trim(void)
{
	struct stat st;
	FILE *f;
	static char buf[MAX];
	size_t n;

	if (stat(LOG, &st) != 0 || st.st_size < MAX || (f = fopen(LOG, "r")) == NULL) {
		return;
	}
	fseek(f, -(long)(MAX / 2), SEEK_END);
	n = fread(buf, 1, MAX / 2, f);
	fclose(f);
	if ((f = fopen(LOG, "w")) != NULL) {
		char *nl = memchr(buf, '\n', n);
		size_t from = nl != NULL ? (size_t)(nl + 1 - buf) : 0;

		fwrite(buf + from, 1, n - from, f);
		fclose(f);
	}
}

static void
line(const char *why, int blanked)
{
	FILE *f = fopen(LOG, "a");
	char when[32];
	time_t now = time(NULL);
	struct tm tm;

	if (f == NULL) {
		return;
	}
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm));
	/* time, why, screen, battery %, mV, mA (negative: out of the battery), 0.1 C, charging, charger, busy %, hottest m°C */
	fprintf(f, "%s %ld %s screen=%s soc=%d mv=%d ma=%d temp=%d charging=%d charger=%d busy=%d hot=%d\n", when, (long)now, why,
	    blanked < 0 ? "?" : blanked ? "off" : "on", num("kern.hakios.battery_soc"), num("kern.hakios.battery_voltage_mv"),
	    num("kern.hakios.battery_current_ma"), num("kern.hakios.battery_temperature_c"), num("kern.hakios.battery_charging"),
	    num("kern.hakios.battery_charger_present"), busy(), num("kern.hakios.thermal_hottest"));
	fclose(f);
	trim();
}

int
main(void)
{
	static int token = -1;
	dispatch_queue_t q = dispatch_get_main_queue();
	dispatch_source_t minute = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);

	/* (woken only by the screen's changes and once a minute: a poll would itself keep the cores from idling) */
	notify_register_dispatch("com.apple.springboard.hasBlankedScreen", &token, q, ^(int t) {
		uint64_t state = 0;

		notify_get_state(t, &state);
		line(state ? "screen-off" : "screen-on", state != 0);
	});
	line("start", -1);
	dispatch_source_set_timer(minute, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), 60 * NSEC_PER_SEC, 5 * NSEC_PER_SEC);
	dispatch_source_set_event_handler(minute, ^{
		uint64_t state = 0;
		int blanked = -1;

		if (token >= 0 && notify_get_state(token, &state) == NOTIFY_STATUS_OK) {
			blanked = state != 0;
		}
		line("minute", blanked);
	});
	dispatch_resume(minute);
	dispatch_main();
}
