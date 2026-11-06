/*
 * How warm the OnePlus 6T's SoC (SDM845) is: its temperature sensors ("tsens", v2), read and logged.
 * Only with boot-arg thermal=1: the addresses and the sensors' names are this phone's, from its own
 * device tree (tsens@c222000 and tsens@c223000, their thermal zones). Nothing is written: the sensors
 * run as the boot loader set them up, and this only reads what they last measured.
 *
 *   thermal=1          all 21 sensors once, then every thermal_every=S seconds (default 10) the eight
 *                      cores and the hottest sensor; at the run's end the most each core reached
 *
 * Register layout: Linux drivers/thermal/qcom/tsens-v2.c. Each sensor has a status word at 0xa0 + 4n of
 * its block's "TM" registers: [11:0] the last temperature in tenths of a degree, signed, [21] valid.
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

/* the loader's log too; with interrupts off (at the run's end) only there */
static void
say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define TSENS0_TM_PHYS          0x0c263000UL
#define TSENS1_TM_PHYS          0x0c265000UL
#define TM_SENSOR_STATUS(n)     (0xa0 + 4 * (n))
#define STATUS_VALID            (1u << 21)
#define EVERY_S_DEFAULT         10

static const struct {
	uint8_t block, sensor;
	const char *name;
} sensors[] = {
	{ 0, 0, "aoss0" }, { 0, 1, "cpu0" }, { 0, 2, "cpu1" }, { 0, 3, "cpu2" }, { 0, 4, "cpu3" },
	{ 0, 5, "l3-0" }, { 0, 6, "l3-1" }, { 0, 7, "cpu4" }, { 0, 8, "cpu5" }, { 0, 9, "cpu6" },
	{ 0, 10, "cpu7" }, { 0, 11, "gpu0" }, { 0, 12, "gpu1" },
	{ 1, 0, "aoss1" }, { 1, 1, "mdm-dsp" }, { 1, 2, "ddr" }, { 1, 3, "wlan" }, { 1, 4, "hvx" },
	{ 1, 5, "camera" }, { 1, 6, "mmss" }, { 1, 7, "mdm-core" },
};
#define SENSORS                 (sizeof(sensors) / sizeof(sensors[0]))

static vm_offset_t tm[2];
static int16_t most[SENSORS];
static bool watching;
static volatile bool leaving;

/* tenths of a degree; false if the sensor has nothing valid (Linux tries three times as well) */
static bool
read_sensor(unsigned i, int *tenths)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		uint32_t status = *(volatile uint32_t *)(tm[sensors[i].block] + TM_SENSOR_STATUS(sensors[i].sensor));

		if (status & STATUS_VALID) {
			*tenths = (int)((int32_t)(status << 20) >> 20);
			return true;
		}
	}
	return false;
}

/* cpu0..cpu3 are entries 1-4, cpu4..cpu7 entries 7-10 */
static bool
is_core(unsigned i)
{
	return (i >= 1 && i <= 4) || (i >= 7 && i <= 10);
}

/* one reading of every sensor, the highest of each kept */
static void
read_all(int *tenths, bool *valid)
{
	for (unsigned i = 0; i < SENSORS; i++) {
		valid[i] = read_sensor(i, &tenths[i]);
		if (valid[i] && tenths[i] > most[i]) {
			most[i] = (int16_t)tenths[i];
		}
	}
}

#define DEG(t)                  (t) / 10, ((t) < 0 ? -(t) : (t)) % 10

static void
thermal_thread(__unused void *arg, __unused wait_result_t wr)
{
	int tenths[SENSORS];
	bool valid[SENSORS];
	uint32_t every = EVERY_S_DEFAULT;
	char line[300];
	size_t used = 0;

	PE_parse_boot_argn("thermal_every", &every, sizeof(every));
	if (every == 0) {
		every = EVERY_S_DEFAULT;
	}
	read_all(tenths, valid);
	line[0] = 0;
	for (unsigned i = 0; i < SENSORS; i++) {
		used += valid[i] ? snprintf(line + used, sizeof(line) - used, " %s %d.%d", sensors[i].name, DEG(tenths[i]))
		    : snprintf(line + used, sizeof(line) - used, " %s -", sensors[i].name);
		if (used > sizeof(line) - 30 || i == SENSORS - 1) {
			say("thermal: C:%s\n", line);
			used = 0;
			line[0] = 0;
		}
	}
	watching = true;
	while (!leaving) {
		unsigned hottest = 0;

		delay_for_interval(every * 1000, NSEC_PER_MSEC);
		if (leaving) {
			break;
		}
		read_all(tenths, valid);
		used = 0;
		line[0] = 0;
		for (unsigned i = 0; i < SENSORS; i++) {
			if (is_core(i)) {
				used += valid[i] ? snprintf(line + used, sizeof(line) - used, " %d", tenths[i] / 10)
				    : snprintf(line + used, sizeof(line) - used, " -");
			}
			if (valid[i] && (!valid[hottest] || tenths[i] > tenths[hottest])) {
				hottest = i;
			}
		}
		say("thermal: cores C (small 0-3, big 4-7):%s; hottest %s %d.%d\n", line, sensors[hottest].name,
		    DEG(tenths[hottest]));
	}
}

void
hakios_thermal_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("thermal", &on, sizeof(on)) || on == 0) {
		return;
	}
	tm[0] = ml_io_map(TSENS0_TM_PHYS, 0x1000);
	tm[1] = ml_io_map(TSENS1_TM_PHYS, 0x1000);
	for (unsigned i = 0; i < SENSORS; i++) {
		most[i] = INT16_MIN;
	}
	if (kernel_thread_start(thermal_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end (a panic's too), with interrupts off: the most each core and the hottest sensor reached. */
void
hakios_thermal_quiesce(void)
{
	char line[200];
	size_t used = 0;
	unsigned hottest = 0;

	if (!watching) {
		return;
	}
	leaving = true;
	line[0] = 0;
	for (unsigned i = 0; i < SENSORS; i++) {
		if (is_core(i)) {
			used += most[i] != INT16_MIN ? snprintf(line + used, sizeof(line) - used, " %d", most[i] / 10)
			    : snprintf(line + used, sizeof(line) - used, " -");
		}
		if (most[i] > most[hottest]) {
			hottest = i;
		}
	}
	say("thermal: the most this run reached, cores C (small 0-3, big 4-7):%s; of all, %s %d.%d\n", line,
	    sensors[hottest].name, DEG(most[hottest]));
}
