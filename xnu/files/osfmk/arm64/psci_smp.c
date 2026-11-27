/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
}

/*
 * A read-only look at what the firmware's PSCI implementation offers, for the CPU idle
 * power-collapse work (screen-off power). It changes nothing: every call here only asks --
 * the version, and PSCI_FEATURES for the calls that matter -- so it is safe to run as it is.
 * Gated behind cpu_psci_probe=1 and off by default; the answers go to the console (-> the
 * logdump partition), the only way off the phone. What we are after:
 *   - is CPU_SUSPEND implemented at all on this TrustZone, and
 *   - which power_state format it wants (original vs extended StateID), and
 *   - whether OS-initiated mode is on offer (PSCI_SET_SUSPEND_MODE present), which decides
 *     whether we could ever ask for a cluster state ourselves or must leave that to the firmware.
 * The SDM845's device tree exposes only power-DOWN idle states (0x40000003 core, 0x40000004
 * rail, 0x400000f4 cluster -- all StateType=powerdown), no retention, so a collapsing idle here
 * will have to warm-boot through the reset vector; this probe is the harmless first confirmation.
 */
#define PSCI_VERSION_FN         0x84000000u
#define PSCI_FEATURES_FN        0x8400000Au
#define PSCI_SET_SUSPEND_MODE   0x8400000Fu
#define PSCI_CPU_SUSPEND_64     0xC4000001u
#define PSCI_SYSTEM_SUSPEND_64  0xC400000Eu

void
psci_smp_probe(void)
{
	uint32_t enable = 0;
	int64_t version, f_suspend, f_sysp, f_on, f_off, f_setmode;

	if (!PE_parse_boot_argn("cpu_psci_probe", &enable, sizeof(enable)) || enable == 0) {
		return;
	}

	version = psci_call(PSCI_VERSION_FN, 0, 0, 0);
	printf("psci: probe: version %lld.%lld, called with %s\n",
	    (version >> 16) & 0xffff, version & 0xffff, psci_smp_hvc ? "hvc" : "smc");

	f_suspend = psci_call(PSCI_FEATURES_FN, PSCI_CPU_SUSPEND_64, 0, 0);
	f_sysp    = psci_call(PSCI_FEATURES_FN, PSCI_SYSTEM_SUSPEND_64, 0, 0);
	f_on      = psci_call(PSCI_FEATURES_FN, PSCI_CPU_ON, 0, 0);
	f_off     = psci_call(PSCI_FEATURES_FN, PSCI_CPU_OFF, 0, 0);
	f_setmode = psci_call(PSCI_FEATURES_FN, PSCI_SET_SUSPEND_MODE, 0, 0);

	if (f_suspend < 0) {
		printf("psci: probe: CPU_SUSPEND is NOT supported (%lld, %s) -- idle power-collapse is not on offer here\n",
		    f_suspend, psci_error(f_suspend));
	} else {
		/* PSCI_FEATURES flags for CPU_SUSPEND: bit1 = power_state format (0 original, 1 extended StateID). */
		printf("psci: probe: CPU_SUSPEND supported, flags 0x%llx -- power_state format: %s StateID\n",
		    f_suspend, (f_suspend & 2) ? "extended" : "original");
	}
	printf("psci: probe: SYSTEM_SUSPEND=%lld (%s), CPU_ON=%lld, CPU_OFF=%lld\n",
	    f_sysp, f_sysp < 0 ? psci_error(f_sysp) : "supported", f_on, f_off);
	printf("psci: probe: SET_SUSPEND_MODE=%lld -- OS-initiated mode %s\n",
	    f_setmode, f_setmode < 0 ? "not available (platform-coordinated only)" : "is available");
}

/*
 * Phase 0 self-test for CPU idle power-collapse. On one chosen core, a bounded number of times,
 * the idle path collapses the core with PSCI CPU_SUSPEND (power-down, 0x40000003 -- the core only,
 * L2 and the cluster stay up) in place of a bare WFI, and the core warm-boots back through the
 * reset vector: reset_vector -> resume_idle_cpu -> arm_init_idle_cpu -> cpu_idle_exit, with the
 * idle thread's registers reloaded from its kernel stack (Idle_load_context). It proves that
 * resume path end to end before the hot idle path is ever changed for real.
 *
 * Off by default, never the boot processor (losing that one would take the log with it), and it
 * stops after N. The core prints a line before each collapse, so a resume that never comes back
 * leaves the attempt that hung as the last line in the log; seeing collapse #k+1 after #k is
 * itself the proof that resume #k succeeded (the core got back to the idle loop to ask again).
 *   cpu_psci_idle_test=N    collapse N times on the chosen core, then stop (0: off)
 *   cpu_psci_idle_core=C    the core to test (default 1; refused if it is the boot processor)
 */
#define PSCI_CPU_SUSPEND        0xC4000001u
#define PSCI_IDLE_PD_STATE      0x40000003u     /* sdm845 device tree: per-core power-down */
#define PSCI_CLUSTER_PD_STATE   0x4100c244u     /* sdm845 mainline dtsi: cluster-sleep-0 domain state (last-man-down; apps_rsc is in cluster_pd, so the RSC applies its sleep votes) */
#define PSCI_PERCORE_RAIL_STATE 0x40000004u     /* sdm845 mainline dtsi: per-core rail power-collapse (deeper than 0x40000003; local-timer-stop) */
#define PSCI_SUSPEND_MODE_OSI   1               /* PSCI_SET_SUSPEND_MODE: OS-initiated (lets the OS request domain/cluster states) */

static uint32_t         psci_idle_test_total;   /* N from the boot-arg; 0 = the self-test is off */
static _Atomic int      psci_idle_test_core = -1;/* the core to test; -1 = the first non-boot core that deep-idles claims it */
static bool             psci_idle_test_reported;
static _Atomic uint32_t psci_idle_suspends;     /* collapses attempted */
static _Atomic uint32_t psci_idle_resumes;      /* warm-boots completed */
static _Atomic uint32_t psci_idle_fallbacks;    /* CPU_SUSPEND returned without collapsing */
static int64_t          psci_idle_last_error;
static uint8_t          psci_idle_collapsed[MAX_CPUS];  /* set across a collapse, for the resume path */

/*
 * Phase 1 production idle collapse (separate from the self-test above).
 *   cpu_psci_idle=1       turn it on (0: off, the default)
 *   cpu_psci_idle_core=C  the one core to collapse in the real idle path (default 1, a big core)
 */
#define PSCI_END_OF_ALL_TIME    0xFFFFFFFFFFFFFFFFULL  /* rtclock.h EndOfAllTime: no finite timer pending */
#define PSCI_PROD_DEFAULT_CORE  1                       /* logical cpu 1 = phys 0x600, a big core that stays online */
#define PSCI_PROD_LOG_EVERY     512u                    /* throttle the hot-path breadcrumb to the log */

static bool             psci_prod_enabled;              /* cpu_psci_idle=1 (one core), 2 (all non-boot), or 3 (all cores + cluster state) */
static bool             psci_prod_all;                  /* cpu_psci_idle>=2: every non-boot core, not just one */
static bool             psci_prod_cluster;              /* cpu_psci_idle=3 (Stage B): request the cluster/system state, not the per-core one */
static bool             psci_prod_incl_boot;            /* cpu_psci_idle=3 (Stage B): collapse the boot cpu too, so the whole APSS can go down */
static bool             psci_prod_force;                /* cpu_psci_idle=4 (Stage B experiment): collapse even with a finite timer pending, to test whether the firmware programs the wake */
static bool             psci_prod_nocluster;            /* cpu_psci_nocluster=1: all cores collapse to the per-core rail state, NEVER request the cluster state (safe per-core-only measurement) */
static int              psci_prod_core = PSCI_PROD_DEFAULT_CORE;
static _Atomic uint32_t psci_prod_suspends;             /* collapses attempted from the real idle path */
static _Atomic uint32_t psci_prod_fallbacks;            /* CPU_SUSPEND returned without collapsing */
static _Atomic uint32_t psci_prod_skipped;              /* gated out: a finite timer deadline was pending */
static _Atomic uint32_t psci_cpus_down;                 /* cluster mode: cores currently collapsed; the one that makes it == all cores asks for the cluster state */
static bool             psci_osi_enabled;               /* PSCI OS-initiated mode turned on (needed to request the cluster state) */

/*
 * The collapse itself, shared by the self-test and the production path. A powered-down core loses
 * its caches, so everything it will read on the way back must be in memory first; the per-core
 * state (0x40000003) keeps L2 up, so the point of unification is enough, but the cluster/system
 * state (0x400000F4) can take L2 and the cluster rail down too, so clean to the point of coherency
 * for that one. The reset vector reads cpu data and page tables with its cache off; those are kept
 * clean by the running cores already. On success control does NOT return here -- the core
 * warm-boots in arm_init_idle_cpu() -> cpu_idle_exit(TRUE). It returns only when the firmware did
 * not collapse the core, with the PSCI status (<= 0); the caller then idles the ordinary way.
 */
static int64_t
psci_cpu_powerdown(int cpu, uint32_t state)
{
	int64_t status;

	bool deep = (state != PSCI_IDLE_PD_STATE);      /* rail/cluster states fully power the core off */

	psci_idle_collapsed[cpu] = 1;
	if (deep) {
		CleanPoC_Dcache();              /* cluster/system state may drop L2: flush to coherency */
		/*
		 * Put the redistributor into ProcessorSleep so a pending interrupt raises a WakeRequest
		 * -- the only thing that re-powers the APSS once the whole cluster is down. If it cannot
		 * quiesce (an interrupt is pending), do not power down: back out to WFI.
		 */
		if (!gic_v3_irq_cpu_sleep()) {
			psci_idle_collapsed[cpu] = 0;
			psci_idle_last_error = -1;
			return -1;
		}
	} else {
		CleanPoU_Dcache();
	}
	__builtin_arm_dsb(DSB_SY);

	status = psci_call(PSCI_CPU_SUSPEND, state,
	    ml_static_vtop((vm_offset_t)&LowResetVectorBase), 0);

	/* Only here if the firmware did NOT power the core down: the call returns in place with an error. */
	if (deep) {
		gic_v3_irq_cpu_resume();        /* undo the ProcessorSleep quiesce so the WFI fallback can still take IRQs */
	}
	psci_idle_collapsed[cpu] = 0;
	psci_idle_last_error = status;
	return status;
}

/*
 * Broadcast / wake timer for the deep (local-timer-stop) states. The per-core architected timers
 * are lost when a core collapses to 0x40000004 or the cluster state, so the system needs an
 * always-on timer to wake it. SDM845's memory-mapped timer (timer@0x17c90000) frame 0 at
 * 0x17ca0000 is in an always-on domain and survives the collapse; its physical compare, armed
 * before the last core goes down, fires GIC SPI 7 (intid 39) to wake the system through the
 * (still-powered at cluster level) GIC. The frame shares the system counter with mach_absolute_time
 * at the same 19.2 MHz rate (possibly a different zero point), so a core's next deadline converts
 * into the frame's domain through the current interval; psci_bcast_cover() keeps the compare at the
 * earliest such deadline of every core going down, and psci_bcast_wake() wakes the whole cluster.
 */
#define BCAST_TIMER_PHYS        0x17ca0000u
#define BCAST_TIMER_SIZE        0x1000u
#define BCAST_TIMER_SPI         (32u + 7u)      /* sdm845 dtsi: frame@0x17ca0000 physical-timer interrupt = GIC_SPI 7 */
#define BCAST_CNTPCT_LO         0x00
#define BCAST_CNTPCT_HI         0x04
#define BCAST_CNTFRQ            0x10
#define BCAST_CNTP_CVAL_LO      0x20
#define BCAST_CNTP_CVAL_HI      0x24
#define BCAST_CNTP_CTL          0x2c            /* bit0 enable, bit1 imask */
#define PSCI_BCAST_SAFETY_SECS  3u              /* bring-up backstop wake when no core has a real deadline */

static vm_offset_t      psci_bcast_base;
static uint32_t         psci_bcast_freq;
static uint64_t         psci_bcast_deadline;    /* broadcast-domain CNTPCT the compare is armed at; 0 = disarmed */
static _Atomic uint32_t psci_bcast_spin;        /* guards the deadline bookkeeping (interrupts are already off here) */

/*
 * A tiny spinlock around the broadcast deadline. Several cores can race to arm the timer to their
 * own next deadline as they enter idle; the lock makes "program the compare to the EARLIEST of them"
 * atomic (otherwise a later writer could clobber an earlier deadline and we would wake too late).
 * Interrupts are already disabled on the idle path, so this can never deadlock against an IRQ.
 */
static inline void
psci_bcast_spin_lock(void)
{
	while (os_atomic_xchg(&psci_bcast_spin, 1, acquire)) {
		__builtin_arm_yield();
	}
}

static inline void
psci_bcast_spin_unlock(void)
{
	os_atomic_store(&psci_bcast_spin, 0, release);
}

/* Read the always-on frame's system counter (the broadcast domain). */
static inline uint64_t
psci_bcast_now(void)
{
	return ((uint64_t)*(volatile uint32_t *)(psci_bcast_base + BCAST_CNTPCT_HI) << 32) |
	       *(volatile uint32_t *)(psci_bcast_base + BCAST_CNTPCT_LO);
}

static void
psci_bcast_disarm(void)
{
	if (psci_bcast_base != 0) {
		*(volatile uint32_t *)(psci_bcast_base + BCAST_CNTP_CTL) = 0;
		__builtin_arm_dsb(DSB_SY);
	}
}

/* Program the compare to an absolute broadcast-domain value. Mask while writing the two halves so a
 * torn 64-bit compare can never momentarily read as due. Caller holds psci_bcast_spin. */
static void
psci_bcast_arm_at(uint64_t cval)
{
	if (psci_bcast_base == 0) {
		return;
	}
	*(volatile uint32_t *)(psci_bcast_base + BCAST_CNTP_CTL) = 0;    /* disable while retargeting */
	*(volatile uint32_t *)(psci_bcast_base + BCAST_CNTP_CVAL_HI) = (uint32_t)(cval >> 32);
	*(volatile uint32_t *)(psci_bcast_base + BCAST_CNTP_CVAL_LO) = (uint32_t)cval;
	*(volatile uint32_t *)(psci_bcast_base + BCAST_CNTP_CTL) = 1;    /* enable, unmasked */
	__builtin_arm_dsb(DSB_SY);
}

/* Relative-interval arm (bring-up safety net only). Caller holds psci_bcast_spin. */
static void
psci_bcast_arm(uint64_t ticks)
{
	if (psci_bcast_base == 0) {
		return;
	}
	psci_bcast_deadline = psci_bcast_now() + ticks;
	psci_bcast_arm_at(psci_bcast_deadline);
}

/*
 * Register an idle core's next timer deadline with the broadcast timer. rtcPop is a finite
 * mach_absolute_time() deadline; the frame runs the same 19.2 MHz counter at (possibly) a different
 * zero point, so convert through the current interval -- timebase-offset-agnostic -- into the
 * broadcast domain, then keep the compare at the EARLIEST deadline of every core now going down.
 * Returns true once the broadcast is guaranteed to fire no later than rtcPop (safe to collapse a
 * core whose per-core timer the power-down will stop); false if there is no always-on wake or the
 * deadline is already due, in which case the caller must not power down (it does a plain WFI).
 */
static bool
psci_bcast_cover(uint64_t rtcPop)
{
	uint64_t mach_now, bcast_dl;

	if (psci_bcast_base == 0) {
		return false;                   /* no always-on wake available: cannot collapse with a deadline */
	}
	mach_now = mach_absolute_time();
	if (rtcPop <= mach_now) {
		return false;                   /* already due: stay out of the power-down */
	}
	psci_bcast_spin_lock();
	bcast_dl = psci_bcast_now() + (rtcPop - mach_now);
	if (psci_bcast_deadline == 0 || bcast_dl < psci_bcast_deadline) {
		psci_bcast_deadline = bcast_dl;
		psci_bcast_arm_at(bcast_dl);
	}
	psci_bcast_spin_unlock();
	return true;
}

/*
 * The broadcast fired: the earliest registered deadline is due. Stop it re-firing, forget the
 * deadline, and wake the whole cluster -- every core still collapsed gets a cpu_signal SGI so it
 * leaves CPU_SUSPEND, services its own (now due or still-pending) timer, and re-registers on its
 * next idle. Waking all of them (rather than only the owner of the earliest deadline) keeps this
 * correct without per-core deadline tracking: no deadline can be missed because every core is back
 * up to evaluate its own, and the only cost is an occasional early wake that immediately re-idles.
 */
static void
psci_bcast_wake(void *context __unused, uint32_t intid __unused)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	psci_bcast_spin_lock();
	psci_bcast_deadline = 0;
	psci_bcast_disarm();            /* the wake has happened; stop it re-firing */
	psci_bcast_spin_unlock();

	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp;

		if (cpu == me || cpu >= MAX_CPUS || !psci_idle_collapsed[cpu]) {
			continue;
		}
		cdp = cpu_datap(cpu);
		if (cdp != NULL) {
			gic_v3_irq_signal(cdp->cpu_phys_id);
		}
	}
}

static void
psci_bcast_init(void)
{
	psci_bcast_base = ml_io_map(BCAST_TIMER_PHYS, BCAST_TIMER_SIZE);
	psci_bcast_disarm();
	psci_bcast_freq = *(volatile uint32_t *)(psci_bcast_base + BCAST_CNTFRQ);
	if (gic_v3_spi_attach(BCAST_TIMER_SPI, false, psci_bcast_wake, NULL) != KERN_SUCCESS) {
		printf("psci: broadcast timer: could not attach GIC SPI %u -- deep-idle wake unavailable\n", BCAST_TIMER_SPI);
		psci_bcast_base = 0;
		return;
	}
	printf("psci: broadcast timer at %#x, %u Hz, wake on GIC SPI %u\n", BCAST_TIMER_PHYS, psci_bcast_freq, BCAST_TIMER_SPI);
}

void
psci_smp_idle_selftest_init(void)
{
	uint32_t n = 0, c = 0, on = 0;

	/* Production path. cpu_psci_idle=1: one chosen core (Phase 1). cpu_psci_idle=2: all non-boot cores (Phase 2). */
	if (PE_parse_boot_argn("cpu_psci_idle", &on, sizeof(on)) && on != 0) {
		uint32_t pc = PSCI_PROD_DEFAULT_CORE;

		if (on >= 3) {
			int64_t m;

			/* OS-initiated mode: without it the firmware denies a domain/cluster state request. */
			m = psci_call(PSCI_SET_SUSPEND_MODE, PSCI_SUSPEND_MODE_OSI, 0, 0);
			psci_osi_enabled = (m == 0);
			printf("psci: SET_SUSPEND_MODE(OS-initiated) -> %lld (%s)\n",
			    m, psci_osi_enabled ? "OSI on" : "stayed platform-coordinated");
			psci_bcast_init();                      /* the always-on wake for the deep states */

			/*
			 * Turn collapse ON only now that OSI and the broadcast timer are ready: set the
			 * flags, publish them, and set the gate (psci_prod_enabled) LAST -- otherwise a core
			 * could enter the collapse during this setup, before a wake exists, and hang.
			 */
			psci_prod_all = true;
			psci_prod_cluster = true;
			psci_prod_incl_boot = true;
			if (on >= 4) {
				psci_prod_force = true;         /* ignore the timer gate: force the collapse even with a pending deadline */
			}
			{
				uint32_t nc = 0;
				if (PE_parse_boot_argn("cpu_psci_nocluster", &nc, sizeof(nc)) && nc != 0) {
					psci_prod_nocluster = true;     /* per-core-only: never request the cluster state */
				}
			}
			printf("psci: idle power-collapse ON for EVERY core incl boot (GICR_WAKER ProcessorSleep handshake); "
			    "last-man-down asks for %s, the rest for the per-core rail state (0x%x)%s\n",
			    psci_prod_nocluster ? "the per-core rail state too (NO cluster)" : "the cluster state (0x4100c244)",
			    PSCI_PERCORE_RAIL_STATE, psci_prod_force ? ", FORCED (ignores pending timers)" : "");
			__builtin_arm_dsb(DSB_SY);
			psci_prod_enabled = true;
		} else if (on >= 2) {
			psci_prod_all = true;
			psci_prod_enabled = true;
			printf("psci: idle power-collapse ON for every non-boot core (CPU_SUSPEND power_state 0x%x), "
			    "each only when it has no finite timer deadline\n", PSCI_IDLE_PD_STATE);
		} else if (PE_parse_boot_argn("cpu_psci_idle_core", &pc, sizeof(pc)) && (int)pc == master_cpu) {
			printf("psci: idle refused: core %u is the boot processor\n", pc);
		} else {
			psci_prod_core = (int)pc;
			psci_prod_enabled = true;
			printf("psci: idle power-collapse ON for core %d (CPU_SUSPEND power_state 0x%x), "
			    "only when it has no finite timer deadline\n", psci_prod_core, PSCI_IDLE_PD_STATE);
		}
	}

	/* Phase 0 self-test (independent; shares cpu_psci_idle_core only when the production path is off). */
	if (!PE_parse_boot_argn("cpu_psci_idle_test", &n, sizeof(n)) || n == 0) {
		return;
	}
	if (PE_parse_boot_argn("cpu_psci_idle_core", &c, sizeof(c))) {
		if ((int)c == master_cpu) {
			printf("psci: idle self-test refused: core %u is the boot processor\n", c);
			return;
		}
		os_atomic_store(&psci_idle_test_core, (int)c, relaxed);
	}
	psci_idle_test_total = n;
	if (os_atomic_load(&psci_idle_test_core, relaxed) < 0) {
		printf("psci: idle self-test armed: %u collapses on the first non-boot core to deep-idle "
		    "(CPU_SUSPEND power_state 0x%x)\n", psci_idle_test_total, PSCI_IDLE_PD_STATE);
	} else {
		printf("psci: idle self-test armed: %u collapses on core %d (CPU_SUSPEND power_state 0x%x)\n",
		    psci_idle_test_total, os_atomic_load(&psci_idle_test_core, relaxed), PSCI_IDLE_PD_STATE);
	}
}

/*
 * The real idle path collapses an idle core with CPU_SUSPEND instead of a bare WFI, to cut its
 * screen-off draw: one chosen core with cpu_psci_idle=1 (Phase 1), or every non-boot core with
 * cpu_psci_idle=2 (Phase 2). Returns true when it handled the idle (collapsed-and-resumed, or
 * fell back to WFI itself); false means the caller should do the ordinary WFI.
 *
 * A powered-down core loses its per-core architected timer, so it can be woken only by an interrupt
 * at its redistributor -- a device IRQ, or an SGI another core sends. In the shallow paths
 * (cpu_psci_idle=1/2, no broadcast) that means it collapses ONLY with no finite deadline pending
 * (rtcPop == EndOfAllTime), falling through to WFI otherwise. In the cluster path (cpu_psci_idle=3)
 * a finite deadline is fine: psci_bcast_cover() registers it with the always-on broadcast timer,
 * which is kept at the earliest deadline of every core going down and, when it fires, wakes the
 * whole cluster so each core services its own timer (tick broadcast). The no-deadline case is still
 * the residency gate too: expected residency is then effectively unbounded, well past min-res.
 */
bool
psci_smp_idle(int cpu, uint64_t rtcPop)
{
	int64_t status;
	uint32_t n;

	if (!psci_prod_enabled) {
		return false;                   /* off: ordinary WFI */
	}
	if (cpu == master_cpu && !psci_prod_incl_boot) {
		return false;                   /* the boot processor is collapsed only in Stage B (cpu_psci_idle=3) */
	}
	if (!psci_prod_all && cpu != psci_prod_core) {
		return false;                   /* single-core mode and not the chosen core: ordinary WFI */
	}
	if (rtcPop != PSCI_END_OF_ALL_TIME) {
		/*
		 * A finite deadline. The cluster path can keep it across the power-down by arming the
		 * always-on broadcast timer (tick broadcast); the shallow paths have no such wake, so they
		 * must fall back to WFI. Either way, if we cannot cover the deadline we do not power down.
		 */
		bool covered = psci_prod_cluster && psci_bcast_cover(rtcPop);

		if (!covered) {
			os_atomic_inc(&psci_prod_skipped, relaxed);
			if (!psci_prod_force) {
				return false;   /* deadline we would lose across the power-down: WFI (unless forced, to test the firmware wake) */
			}
		}
	}

	n = os_atomic_inc_orig(&psci_prod_suspends, relaxed) + 1;
	if ((n % PSCI_PROD_LOG_EVERY) == 0) {
		/* A throttled breadcrumb to the log: a climbing count with no faults is the proof it works. */
		kprintf("psci: idle core %d: suspends=%u resumes=%u fallbacks=%u skipped=%u\n",
		    cpu, n, os_atomic_load(&psci_idle_resumes, relaxed),
		    os_atomic_load(&psci_prod_fallbacks, relaxed),
		    os_atomic_load(&psci_prod_skipped, relaxed));
	}

	if (psci_prod_cluster) {
		/*
		 * Last-man-down: the core whose entry makes every core collapsed asks for the cluster
		 * state; the rest ask for the per-core rail state. The firmware accepts the cluster state
		 * only if the other cores really are down -- otherwise it denies (-3) and we fall through
		 * to WFI -- so a stale count is safe, never a hang.
		 */
		uint32_t num = (uint32_t)ml_get_max_cpu_number() + 1;
		uint32_t down = os_atomic_inc_orig(&psci_cpus_down, relaxed) + 1;
		bool all_down = down >= num;
		/* The last man asks for the cluster state -- unless cpu_psci_nocluster=1, which keeps
		 * every core on the per-core rail state (safe per-core-only measurement, no cluster). */
		bool want_cluster = all_down && !psci_prod_nocluster;

		if (all_down && psci_bcast_freq != 0) {
			/*
			 * The whole APSS is down. If some core already registered a deadline the broadcast is
			 * armed to it and covers the wake; if NONE did (every core idle with no finite timer)
			 * arm a far backstop wake as a bring-up safety net so an untested wake path can never
			 * hang the system silently. A real deadline, being sooner, always wins the min.
			 */
			psci_bcast_spin_lock();
			if (psci_bcast_deadline == 0) {
				psci_bcast_arm(psci_bcast_freq * PSCI_BCAST_SAFETY_SECS);
			}
			psci_bcast_spin_unlock();
		}
		status = psci_cpu_powerdown(cpu, want_cluster ? PSCI_CLUSTER_PD_STATE : PSCI_PERCORE_RAIL_STATE);
		/*
		 * Only here if it did NOT collapse (a successful collapse returns through the resume path,
		 * which does the decrement). Back this core out of the down-count; if it was the last one
		 * still notionally down, nothing is left to wake, so drop the broadcast too.
		 */
		if (os_atomic_dec(&psci_cpus_down, relaxed) == 0) {
			psci_bcast_spin_lock();
			psci_bcast_deadline = 0;
			psci_bcast_disarm();
			psci_bcast_spin_unlock();
		}
	} else {
		status = psci_cpu_powerdown(cpu, PSCI_IDLE_PD_STATE);
	}

	/* Only here if it did not collapse: count it and idle the ordinary way. */
	os_atomic_inc(&psci_prod_fallbacks, relaxed);
	kprintf("psci: idle core %d CPU_SUSPEND returned %lld (%s) -- did not collapse\n",
	    cpu, status, psci_error(status));
	__builtin_arm_wfi();
	return true;
}

bool
psci_smp_idle_collapse(int cpu)
{
	uint32_t done;
	int64_t status;
	int target;

	if (psci_idle_test_total == 0) {
		return false;                   /* the self-test is off */
	}
	target = os_atomic_load(&psci_idle_test_core, relaxed);
	if (target < 0) {
		/* Auto mode: the first non-boot core to reach a deep idle claims the whole test. */
		if (cpu == master_cpu) {
			return false;
		}
		if (os_atomic_cmpxchg(&psci_idle_test_core, -1, cpu, acq_rel)) {
			kprintf("psci: idle self-test: core %d claimed the test\n", cpu);
			target = cpu;
		} else {
			target = os_atomic_load(&psci_idle_test_core, relaxed);
		}
	}
	if (cpu != target) {
		return false;                   /* not the core under test: the caller does a normal WFI */
	}
	done = os_atomic_load(&psci_idle_suspends, relaxed);
	if (done >= psci_idle_test_total) {
		if (!psci_idle_test_reported) {
			psci_idle_test_reported = true;
			kprintf("psci: idle self-test DONE on core %d: suspends=%u resumes=%u fallbacks=%u last_error=%lld\n",
			    cpu, done, os_atomic_load(&psci_idle_resumes, relaxed),
			    os_atomic_load(&psci_idle_fallbacks, relaxed), psci_idle_last_error);
		}
		return false;                   /* finished: a normal WFI from now on */
	}

	kprintf("psci: idle self-test: core %d collapse #%u\n", cpu, done + 1);
	os_atomic_inc(&psci_idle_suspends, relaxed);

	status = psci_cpu_powerdown(cpu, PSCI_IDLE_PD_STATE);

	/* Only here if the firmware did NOT power the core down: the call returned in place with an error. */
	os_atomic_inc(&psci_idle_fallbacks, relaxed);
	kprintf("psci: idle self-test: core %d CPU_SUSPEND returned %lld (%s) -- did not collapse\n",
	    cpu, status, psci_error(status));
	__builtin_arm_wfi();                    /* idle the ordinary way instead */
	return true;
}

void
psci_smp_idle_resumed(int cpu)
{
	if (cpu < 0 || cpu >= MAX_CPUS || !psci_idle_collapsed[cpu]) {
		return;                         /* not a core coming back from a power-collapse */
	}
	psci_idle_collapsed[cpu] = 0;
	if (psci_prod_cluster) {
		/*
		 * This core is back up: balance the last-man-down count. Drop the broadcast only once the
		 * LAST core is up -- a lone device-IRQ wake must not strip the timer out from under the
		 * cores still collapsed (they are still relying on it for their deadlines).
		 */
		if (os_atomic_dec(&psci_cpus_down, relaxed) == 0) {
			psci_bcast_spin_lock();
			psci_bcast_deadline = 0;
			psci_bcast_disarm();
			psci_bcast_spin_unlock();
		}
	}
	gic_v3_irq_cpu_resume();                /* the GIC CPU interface is gone; bring it back before anything needs it */
	os_atomic_inc(&psci_idle_resumes, relaxed);
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	int64_t status;

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}

#endif /* defined(GENERIC_PSCI_SMP) */
