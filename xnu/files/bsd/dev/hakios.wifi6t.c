/*
 * The OnePlus 6T's Wi-Fi under ath10k (boot-arg wifi6t=1, a kernel linked with tools/mk-ath10k.sh's object:
 * ATH=ath10k ./xnu-build.sh). What redwood's bsd/dev/hakios.wifi.c is for its WCN6750 and ath11k, for the
 * 6T's WCN3990: the kernel's side of what the driver's object asks (sys/hakios.lx.h, through
 * bsd/dev/hakios.lx.c) where it differs from redwood's --
 *
 *   - the radio's DMA: its stream (0x40, the tree's iommus = <&apps_smmu 0x40 0x1>, with
 *     qcom,smmu-s1-bypass) let through the apps SMMU untranslated, so that the addresses the
 *     radio is given are the memory's own;
 *   - the files ath10k asks for by Linux's names: its table of the firmware's features (firmware-5.bin,
 *     from linux-firmware, put into fw-wifi/ and staged into the small image's /fw), and the board's
 *     data, which on the 6T is the modem's image's bdwlan.bin (/fw/image, the modem's firmware);
 *   - the start: when the modem announces the Wi-Fi firmware's service (bsd/dev/hakios.mpss.c, with
 *     modem_keep=1), the driver's QMI goes over the modem's QRTR and lx_start() runs on a thread of
 *     its own: ath10k's snoc probe, its exchange with the firmware, its start; with wifi_scan=1 a scan.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.qrtr.h>
#include <sys/hakios.qmi.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern const struct hakios_qrtr_ops *hakios_mpss_qrtr(void);           /* bsd/dev/hakios.mpss.c */
extern const uint8_t *hakios_mpss_fw_file(const char *name, uint64_t *size);
extern void hakios_qmi_set_transport(const struct hakios_qrtr_ops *ops); /* bsd/dev/hakios.qmi.c */
extern int hakios_lx_start(void);                                       /* bsd/dev/hakios.lx.c */
extern lck_mtx_t hakios_smmu_lock;                                      /* bsd/dev/hakios.sound.c */
extern void loader_log_putc(char c);
extern void thread_set_thread_name(thread_t thread, const char *name);

bool hakios_wifi6t(void);
bool hakios_wifi6t_radio_map(uint64_t physical, uint64_t size, unsigned long long *address);
const uint8_t *hakios_wifi6t_file(const char *name, uint64_t *size);
void hakios_wifi6t_start(void);

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

bool
hakios_wifi6t(void)
{
	static int wanted = -1;
	uint32_t value = 0;

	if (wanted < 0) {
		wanted = PE_parse_boot_argn("wifi6t", &value, sizeof(value)) && value != 0;
	}
	return wanted != 0;
}

/* ---- the radio's stream through the apps SMMU ------------------------------------------------------------ */

#define APPS_SMMU               0x15000000ull
#define SMMU_IDR0               0x20
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define RADIO_STREAM            0x40
#define RADIO_STREAM_MASK       0x1

static bool
stream_bypass(void)
{
	static int done;                        /* 1: let through; -1: cannot be */
	volatile uint32_t *smmu;
	uint32_t matches, free_match = ~0u;

	if (done != 0) {
		return done > 0;
	}
	done = -1;
	smmu = (volatile uint32_t *)ml_io_map(APPS_SMMU, 0x1000);
	if (smmu == NULL) {
		say("wifi6t: the apps SMMU cannot be mapped\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	matches = smmu[SMMU_IDR0 / 4] & 0xff;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = smmu[SMMU_SMR(n) / 4], s2cr = smmu[SMMU_S2CR(n) / 4];
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (smr >> 31) {
			if (((stream ^ RADIO_STREAM) & ~mask) == 0) {
				lck_mtx_unlock(&hakios_smmu_lock);
				if ((s2cr >> 16 & 3) == 1) {
					say("wifi6t: match register %u lets the radio's stream through already\n", n);
					done = 1;
					return true;
				}
				say("wifi6t: match register %u has the radio's stream (%#x) for a context of somebody's: left alone\n", n,
				    s2cr);
				return false;
			}
		} else if (free_match == ~0u) {
			free_match = n;
		}
	}
	if (free_match == ~0u) {
		lck_mtx_unlock(&hakios_smmu_lock);
		say("wifi6t: the apps SMMU has no match register left for the radio's stream\n");
		return false;
	}
	smmu[SMMU_S2CR(free_match) / 4] = 1u << 16;            /* bypass */
	__asm__ volatile ("dsb sy" ::: "memory");
	smmu[SMMU_SMR(free_match) / 4] = 1u << 31 | RADIO_STREAM_MASK << 16 | RADIO_STREAM;
	__asm__ volatile ("dsb sy" ::: "memory");
	lck_mtx_unlock(&hakios_smmu_lock);
	say("wifi6t: THE RADIO'S STREAM (%#x, mask %#x) GOES THROUGH THE APPS SMMU UNTRANSLATED: match register %u (smr %#x, s2cr %#x)\n",
	    RADIO_STREAM, RADIO_STREAM_MASK, free_match, smmu[SMMU_SMR(free_match) / 4], smmu[SMMU_S2CR(free_match) / 4]);
	done = 1;
	return true;
}

/* hakios.lx.c's lx_os_radio_memory(): memory the radio reads and writes, known to it by its own address */
bool
hakios_wifi6t_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	if (!stream_bypass()) {
		return false;
	}
	*address = physical;
	say("wifi6t: %llu MB for the radio at %#llx, its own address\n", size >> 20, physical);
	return true;
}

/* ---- files, by the names ath10k asks for them ----------------------------------------------------------- */

const uint8_t *
hakios_wifi6t_file(const char *name, uint64_t *size)
{
	const char *last = strrchr(name, '/');
	const uint8_t *data = NULL;

	last = last == NULL ? name : last + 1;
	if (strcmp(last, "board.bin") == 0) {
		data = hakios_mpss_fw_file("image/bdwlan.bin", size);
	} else if (strcmp(last, "firmware-5.bin") == 0) {
		data = hakios_mpss_fw_file("ath10k-wcn3990-firmware-5.bin", size);
	}
	say("wifi6t: the driver asks for %s: %s\n", name, data != NULL ? "given" : "not here");
	return data;
}

/* ---- the start ------------------------------------------------------------------------------------------- */

static void
wifi6t_thread(__unused void *parameter, __unused wait_result_t result)
{
	const struct hakios_qrtr_ops *transport = hakios_mpss_qrtr();
	int error;

	if (transport == NULL) {
		say("wifi6t: the modem's link is not up\n");
		return;
	}
	hakios_qmi_set_transport(transport);
	say("wifi6t: ath10k starts, its QMI over the modem's QRTR\n");
	error = hakios_lx_start();
	say("wifi6t: the driver's start returns %d\n", error);
}

/* hakios.mpss.c, when the modem announces WLFW: the driver started, once, with wifi6t=1. */
void
hakios_wifi6t_start(void)
{
	static bool started;
	thread_t thread;

	if (started || !hakios_wifi6t()) {
		return;
	}
	started = true;
	if (kernel_thread_start(wifi6t_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "wifi6t");
		thread_deallocate(thread);
	} else {
		say("wifi6t: no thread\n");
	}
}
