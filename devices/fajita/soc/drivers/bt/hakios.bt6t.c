/*
 * Bluetooth of the OnePlus 6T (SDM845): STEP ONE, a look. Only with boot-arg bt6t=1, and it only READS.
 *
 * The radio is the WCN3990, whose Wi-Fi half is ath10k (hakios.wifi6t.c). Its Bluetooth half is a plain HCI
 * controller behind a UART, as Linux runs it (hci_qca.c, btqca.c: "qcom,wcn3990-bt"). What the phone's own device
 * tree says (devices/fajita/device-tree.dtb; numbers read out by script):
 *
 *   the UART     qcom,qup_uart@0x898000 (qcom,msm-geni-serial-hs, "ok"): QUP wrapper 0's serial engine 6
 *   its pins     GPIO 45 CTS and 48 RX (no pull), 46 RTS (pull down), 47 TX (pull up), all function qup6, 2 mA;
 *                the TLMM's EAST tile (Linux pinctrl-sdm845.c: PINGROUP(45..48, EAST, qup6, ...): qup6 = function 1)
 *   its clocks   GCC (0x100000), Linux gcc-sdm845.c (v6.12):
 *                  gcc_qupv3_wrap0_s6_clk      CBCR 0x17750, vote 0x5200c bit 16 (its RCG at 0x17754)
 *                  gcc_qupv3_wrap_0_m_ahb_clk  CBCR 0x17004, vote 0x5200c bit 6
 *                  gcc_qupv3_wrap_0_s_ahb_clk  CBCR 0x17008, vote 0x5200c bit 7
 *   the supplies (vendor/bt_wcn3990, all RPMh): core pm8998_l7 1.8 V, pa l17 1.304 V, ldo l25 3.312 V,
 *                io s3 1.352 V, xtal s5 2.04 V
 *   firmware     the phone's bluetooth_b partition (FAT): image/crbtfw21.tlv (rampatch) and crnv21.bin (NVM) for
 *                the WCN3990 2.1, put into ../fw-bt (not in the repository)
 *
 * What this does, at bt6t=1: reads the four pins' configuration and level, the three clocks' CBCRs (bit 31: off)
 * and the GCC's vote register, and -- only if all three clocks run, since a read of an engine whose clock is off
 * hangs the bus -- a few of the serial engine's registers (GENI, Linux geni-se.h): which protocol its firmware is
 * (GENI_FW_REVISION_RO [15:8]: 2 is the UART), its status, clock configuration and FIFOs. It writes NOTHING.
 *
 * bt6t=2: the clocks voted on (the engine's at 7.3728 MHz) and the pins set, then the same look at the engine. Next
 * (bt6t=3, not here): the engine set up as a UART at 115200 (Linux qcom_geni_serial.c), then HCI_Reset and Read
 * Local Version (btqca.c: the controller's version says which rampatch).
 *
 * Boot-args:
 *   bt6t=1        the look
 *   bt6t=2        the UART's clock and pins set, then the look at the engine
 */
#include <kern/startup.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <stdarg.h>

extern void loader_log_putc(char c);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static int
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}

#define TLMM_EAST_PHYS          0x03500000UL                    /* 0x03400000 + EAST (0x100000) */
#define TLMM_EAST_SIZE          0x00100000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_OUTPUT              (1u << 9)

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x00060000UL                    /* up to and past the vote register at 0x5200c */
#define GCC_VOTE                0x5200c
#define CBCR_OFF                (1u << 31)

#define SE6_PHYS                0x00898000UL
#define SE6_SIZE                0x4000UL

static vm_offset_t tlmm, gcc, se6;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static const struct {
	unsigned pin;
	const char *what;
} pins[] = {
	{ 45, "CTS" }, { 46, "RTS" }, { 47, "TX" }, { 48, "RX" },
};

static const struct {
	uint32_t cbcr, vote_bit;
	const char *what;
} clocks[] = {
	{ 0x17750, 16, "gcc_qupv3_wrap0_s6_clk (the engine's)" },
	{ 0x17004, 6, "gcc_qupv3_wrap_0_m_ahb_clk" },
	{ 0x17008, 7, "gcc_qupv3_wrap_0_s_ahb_clk" },
};

static const struct {
	uint32_t offset;
	const char *what;
} engine[] = {
	{ 0x68, "GENI_FW_REVISION_RO (protocol in [15:8]: 2 = UART)" },
	{ 0x6c, "GENI_FW_S_REVISION_RO" },
	{ 0x40, "SE_GENI_STATUS" },
	{ 0x48, "GENI_SER_M_CLK_CFG" },
	{ 0x64, "GENI_IF_DISABLE_RO" },
	{ 0x7c, "SE_GENI_CLK_SEL" },
	{ 0x258, "SE_GENI_DMA_MODE_EN" },
	{ 0x610, "SE_GENI_M_IRQ_STATUS" },
	{ 0x800, "SE_GENI_TX_FIFO_STATUS" },
	{ 0x804, "SE_GENI_RX_FIFO_STATUS" },
	{ 0x908, "SE_GENI_IOS (bit 0 RX, bit 1 CTS)" },
	{ 0xe24, "SE_HW_PARAM_0" },
	{ 0xe28, "SE_HW_PARAM_1" },
};

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * Level 2: the UART's clock at 7.3728 MHz and its pins, as Linux sets them (the first writes). UEFI leaves all of it
 * off (run 151629: pins function 0, the engine's clock off and not voted; its DFS table empty and DFS off, run 152047),
 * so the RCG is programmed in its plain mode with Linux's entry for 7372800 Hz (gcc-sdm845.c
 * ftbl_gcc_qupv3_wrap0_s0_clk_src: GPLL0 even, divider 1, M/N 384/15625), as clk-rcg2.c __clk_rcg2_configure() writes
 * it (mnd_width 16, hid_width 5): M = m, N = ~(n - m), D = ~n (n within [m, 2(n - m)]), CFG = (2*1-1) | source 6 << 8 |
 * dual-edge 2 << 12; then CMD_UPDATE (bit 0), which the RCG clears when it has taken them. Then the three clocks voted
 * on at 0x5200c (bits 16, 6, 7; the others left as they are), and the four pins to function 1 (qup6) with the device
 * tree's pulls, 2 mA. The chip is not spoken to. Returns whether all three clocks run.
 */
static bool
uart_clock_and_pins(void)
{
	const uint32_t rcg = 0x17754, m = 384, n = 15625, mask = 0xffff;
	static const uint32_t pull[] = { 0, 1, 3, 0 };          /* 45 none, 46 down, 47 up, 48 none */
	uint32_t cfg, tries;
	bool on = true;

	say("bt6t: level 2: the UART's clock (7.3728 MHz) and its pins; the chip is not spoken to\n");
	wr(gcc + rcg + 0x8, (rd(gcc + rcg + 0x8) & ~mask) | m);
	wr(gcc + rcg + 0xc, (rd(gcc + rcg + 0xc) & ~mask) | (~(n - m) & mask));
	wr(gcc + rcg + 0x10, (rd(gcc + rcg + 0x10) & ~mask) | (~n & mask));
	cfg = rd(gcc + rcg + 0x4);
	cfg &= ~(0x1fu | (7u << 8) | (3u << 12) | (1u << 20));
	cfg |= 1u | (6u << 8) | (2u << 12);
	wr(gcc + rcg + 0x4, cfg);
	wr(gcc + rcg, rd(gcc + rcg) | 1u);
	for (tries = 0; tries < 1000 && (rd(gcc + rcg) & 1u); tries++) {
		delay(1);
	}
	say("bt6t: RCG 0x%x: CFG 0x%08x, M 0x%04x, N 0x%04x, D 0x%04x; CMD 0x%08x (update %s)\n", rcg, rd(gcc + rcg + 0x4),
	    rd(gcc + rcg + 0x8) & mask, rd(gcc + rcg + 0xc) & mask, rd(gcc + rcg + 0x10) & mask, rd(gcc + rcg),
	    (rd(gcc + rcg) & 1u) ? "NOT TAKEN" : "taken");
	wr(gcc + GCC_VOTE, rd(gcc + GCC_VOTE) | (1u << 16) | (1u << 6) | (1u << 7));
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		for (tries = 0; tries < 1000 && (rd(gcc + clocks[i].cbcr) & CBCR_OFF); tries++) {
			delay(1);
		}
		say("bt6t: %s: CBCR 0x%08x (%s)\n", clocks[i].what, rd(gcc + clocks[i].cbcr),
		    (rd(gcc + clocks[i].cbcr) & CBCR_OFF) ? "STILL OFF" : "running");
		on = on && !(rd(gcc + clocks[i].cbcr) & CBCR_OFF);
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		wr(GPIO_CFG(pins[i].pin), pull[i] | (1u << 2));
		say("bt6t: gpio%u (%s) now: cfg 0x%03x (function %u, pull %u), reads %u\n", pins[i].pin, pins[i].what,
		    rd(GPIO_CFG(pins[i].pin)), (rd(GPIO_CFG(pins[i].pin)) >> 2) & 15, rd(GPIO_CFG(pins[i].pin)) & 3,
		    rd(GPIO_IO(pins[i].pin)) & 1);
	}
	return on;
}

__startup_func
static void
bt6t_init(void)
{
	uint32_t level = 0, vote;
	bool all_on = true;

	if (!PE_parse_boot_argn("bt6t", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 2) {
		say("bt6t: bt6t=%u: only levels 1 (a look) and 2 (the UART's clock and pins) are written so far: level 2\n", level);
		level = 2;
	}
	tlmm = ml_io_map(TLMM_EAST_PHYS, TLMM_EAST_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	if (tlmm == 0 || gcc == 0) {
		say("bt6t: could not map the TLMM's east tile or the GCC; off\n");
		return;
	}
	say("bt6t: level 1: a look at the WCN3990's Bluetooth UART (QUP0 SE6), reading only; nothing is written\n");
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("bt6t: gpio%u (%s): function %u%s, pull %u (0 none, 1 down, 3 up), %u mA, %s, reads %u\n", pins[i].pin,
		    pins[i].what, (cfg >> 2) & 15, ((cfg >> 2) & 15) == 1 ? " (qup6)" : "", cfg & 3, (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
	vote = rd(gcc + GCC_VOTE);
	say("bt6t: the GCC's clock votes (0x%x) = 0x%08x\n", GCC_VOTE, vote);
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		uint32_t cbcr = rd(gcc + clocks[i].cbcr);

		say("bt6t: %s: CBCR 0x%08x (%s), voted %s\n", clocks[i].what, cbcr, (cbcr & CBCR_OFF) ? "OFF" : "running",
		    (vote >> clocks[i].vote_bit) & 1 ? "on" : "off");
		all_on = all_on && !(cbcr & CBCR_OFF);
	}
	/*
	 * The engine's clock source is a DFS one (Linux gcc-sdm845.c gcc_dfs_clocks, clk-rcg2.c): the boot firmware fills a table
	 * of up to 8 rates and the engine picks one by index (SE_GENI_CLK_SEL); its RCG at 0x17754: +0x14 DFS enable (bit 0),
	 * +0x1c+4*l a level's source [10:8] and half-divider [4:0], mode [13:12]; +0x5c+4*l its M; +0x9c+4*l ~(N - M). Rates
	 * from the sources of gcc_parent_map_0: 0 TCXO 19.2 MHz, 1 GPLL0 600 MHz, 6 GPLL0 even 300 MHz.
	 */
	{
		uint32_t rcg = 0x17754, dfs = rd(gcc + rcg + 0x14);

		say("bt6t: the engine's clock source (RCG 0x%x): DFS %s (0x%08x)\n", rcg, (dfs & 1) ? "on" : "OFF", dfs);
		for (unsigned l = 0; l < 8; l++) {
			uint32_t cfg = rd(gcc + rcg + 0x1c + 4 * l), m = rd(gcc + rcg + 0x5c + 4 * l) & 0xffff;
			uint32_t notnm = rd(gcc + rcg + 0x9c + 4 * l), src = (cfg >> 8) & 7, hid = cfg & 31, mode = (cfg >> 12) & 3;
			uint64_t parent = src == 0 ? 19200000ull : src == 1 ? 600000000ull : src == 6 ? 300000000ull : 0, rate = parent;
			uint32_t n = ((~notnm) & 0xffff) + m;

			if (hid != 0) {
				rate = rate * 2 / (hid + 1);
			}
			if (mode != 0 && n != 0) {
				rate = rate * m / n;
			}
			say("bt6t:   level %u: cfg 0x%08x (source %u, half-divider %u, mode %u), M %u, N %u: %llu Hz\n", l, cfg, src, hid,
			    mode, m, mode ? n : 0, (unsigned long long)rate);
		}
	}
	if (level >= 2 && !all_on) {
		all_on = uart_clock_and_pins();
	}
	if (!all_on) {
		say("bt6t: a clock of the serial engine is off: its registers are not read (a read would hang the bus)\n");
		return;
	}
	se6 = ml_io_map(SE6_PHYS, SE6_SIZE);
	if (se6 == 0) {
		say("bt6t: could not map the serial engine; off\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(engine) / sizeof(engine[0]); i++) {
		say("bt6t: SE6 +0x%03x %s = 0x%08x\n", engine[i].offset, engine[i].what, rd(se6 + engine[i].offset));
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt6t_init);
