/*
 * The OnePlus 6T's Wi-Fi under ath10k (boot-arg wifi6t=1, a kernel linked with tools/mk-ath10k.sh's object:
 * ATH=ath10k ./xnu-build.sh). What redwood's bsd/dev/hakios.wifi.c is for its WCN6750 and ath11k, for the
 * 6T's WCN3990: the kernel's side of what the driver's object asks (sys/hakios.lx.h, through
 * bsd/dev/hakios.lx.c) where it differs from redwood's --
 *
 *   - the radio's DMA: its stream (0x40, the tree's iommus = <&apps_smmu 0x40 0x1>) through a context of
 *     the apps SMMU whose tables map the DDR onto itself, so that the addresses the radio is given are
 *     the memory's own (the hypervisor grants no bypass);
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
#include <vm/vm_kern.h>

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

/*
 * The hypervisor grants no bypass (24 October for the IPA; 1 November for the radio: the S2CR written
 * "bypass" read back "fault"). So, as bsd/dev/hakios.mpss.c's ipa_space_on() does for the IPA: a context of
 * ours (the lowest free), with one table whose 1 GB blocks map the phone's DDR (8 GB from 0x80000000) onto
 * itself, uncached, and the first free match register -- or the one a run before left with the radio's stream
 * and "fault" -- sending the stream (the tree's 0x40, mask 1, which the hypervisor grants) through it. The
 * addresses the radio is given are then the memory's own.
 */
#define APPS_SMMU               0x15000000ull
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* bit 31 valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate 1 bypass 2 fault, [7:0] context */
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44      /* 0 uncached, 1 cached, 2 device */
#define SMMU_FSR_ALL            0xc00001fe
#define DDR_BLOCK               (1ull | 0ull << 2 | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)  /* uncached, RW, shared */
#define RADIO_STREAM            0x40
#define RADIO_STREAM_MASK       0x1

extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);

static bool
stream_context(void)
{
	static int done;                        /* 1: through our context; -1: cannot be */
	bool used[256] = { false };
	volatile uint32_t *smmu, *second, *context;
	uint32_t idr1, matches, contexts, page, pages, match = ~0u, ctx, sees = 1u << 31 | RADIO_STREAM_MASK << 16 | RADIO_STREAM;
	vm_offset_t table = 0;
	uint64_t *root, ttbr;

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
	idr1 = smmu[SMMU_IDR1 / 4];
	matches = smmu[SMMU_IDR0 / 4] & 0xff;
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = smmu[SMMU_SMR(n) / 4], s2cr = smmu[SMMU_S2CR(n) / 4];
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if ((smr >> 31) && ((stream ^ RADIO_STREAM) & ~mask) == 0 && (s2cr >> 16 & 3) == 2) {
			match = n;              /* (a run before ours left it so: taken again) */
		} else if (smr >> 31) {
			if (((stream ^ RADIO_STREAM) & ~mask) == 0) {
				lck_mtx_unlock(&hakios_smmu_lock);
				say("wifi6t: match register %u has the radio's stream already (s2cr %#x): left alone\n", n, s2cr);
				return false;
			}
			if ((s2cr >> 16 & 3) == 0) {
				used[s2cr & 0xff] = true;
			}
		} else if (match == ~0u) {
			match = n;
		}
	}
	for (ctx = 0; ctx < contexts && used[ctx]; ctx++) {
	}
	if (match == ~0u || ctx >= contexts || 2 * pages * page != 0x80000) {
		lck_mtx_unlock(&hakios_smmu_lock);
		say("wifi6t: the apps SMMU has no match register or context left, or is not the SDM845's (idr1 %#x)\n", idr1);
		return false;
	}
	second = (volatile uint32_t *)ml_io_map(APPS_SMMU + page, 0x1000);
	context = (volatile uint32_t *)ml_io_map(APPS_SMMU + (uint64_t)(pages + ctx) * page, 0x1000);
	if (second == NULL || context == NULL || (context[SMMU_CB_SCTLR / 4] & 1) ||
	    kmem_alloc(kernel_map, &table, 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		lck_mtx_unlock(&hakios_smmu_lock);
		say("wifi6t: context %u of the apps SMMU cannot be had\n", ctx);
		return false;
	}
	root = (uint64_t *)table;
	for (unsigned g = 2; g < 10; g++) {
		root[g] = ((uint64_t)g << 30) | DDR_BLOCK;
	}
	FlushPoC_DcacheRegion(table, 4096);
	__asm__ volatile ("dsb sy" ::: "memory");
	ttbr = kvtophys(table) | (uint64_t)ctx << 48;
	context[SMMU_CB_SCTLR / 4] = 0;
	second[SMMU_CBA2R(ctx) / 4] = SMMU_CBA2R_64_BIT;
	second[SMMU_CBAR(ctx) / 4] = SMMU_CBAR_STAGE_1;
	context[SMMU_CB_TCR2 / 4] = SMMU_TCR2;
	context[SMMU_CB_TCR / 4] = SMMU_TCR_39_BITS;
	context[SMMU_CB_TTBR0 / 4] = (uint32_t)ttbr;
	context[SMMU_CB_TTBR0 / 4 + 1] = (uint32_t)(ttbr >> 32);
	context[SMMU_CB_TTBR1 / 4] = 0;
	context[SMMU_CB_TTBR1 / 4 + 1] = 0;
	context[SMMU_CB_MAIR0 / 4] = SMMU_MAIR0;
	context[SMMU_CB_MAIR1 / 4] = 0;
	context[SMMU_CB_FSR / 4] = SMMU_FSR_ALL;
	context[SMMU_CB_TLBIASID / 4] = ctx;
	context[SMMU_CB_TLBSYNC / 4] = 1;
	for (unsigned i = 0; (context[SMMU_CB_TLBSTATUS / 4] & 1) && i < 100000; i++) {
	}
	context[SMMU_CB_SCTLR / 4] = SMMU_SCTLR_ON;
	__asm__ volatile ("dsb sy" ::: "memory");
	smmu[SMMU_S2CR(match) / 4] = ctx;
	smmu[SMMU_SMR(match) / 4] = sees;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (smmu[SMMU_SMR(match) / 4] != sees || (smmu[SMMU_S2CR(match) / 4] & 0x300ff) != ctx || !(context[SMMU_CB_SCTLR / 4] & 1)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		say("wifi6t: THE SMMU DID NOT TAKE WHAT WAS WRITTEN (smr %#x s2cr %#x sctlr %#x)\n", smmu[SMMU_SMR(match) / 4],
		    smmu[SMMU_S2CR(match) / 4], context[SMMU_CB_SCTLR / 4]);
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	say("wifi6t: THE RADIO'S STREAM (%#x, mask %#x) GOES THROUGH CONTEXT %u OF THE APPS SMMU (match register %u): the phone's DDR "
	    "as it is\n", RADIO_STREAM, RADIO_STREAM_MASK, ctx, match);
	done = 1;
	return true;
}

/* hakios.lx.c's lx_os_radio_memory(): memory the radio reads and writes, known to it by its own address */
bool
hakios_wifi6t_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
	if (!stream_context()) {
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
