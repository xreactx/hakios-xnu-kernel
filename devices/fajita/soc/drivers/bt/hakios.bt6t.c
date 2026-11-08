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
 *   bt6t=3        and the engine as a UART, the WCN3990's power pulses, and its version asked for (HCI)
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

/*
 * ---- Level 3: the engine as a UART, the WCN3990's power pulses, and its version ----------------------------
 *
 * The engine set up as Linux's qcom_geni_serial.c does (port_setup, set_termios), polled, in FIFO mode, without
 * interrupts: GENI (geni-se.h, qcom-geni-se.c v6.12) clock gating and I/O defaults, interrupts cleared, FIFO mode,
 * 8-bit words packed four to a FIFO word least significant first (geni_se_config_packing(8, 4, false):
 * 0x4380e / 0xc3e0e, byte granularity 0), the RX watermarks (2; RFR at depth - 2), RX stale 10 * 16 bits; then 8N1, no
 * flow control (UART_CTS_MASK), the divider from the engine's 7.3728 MHz: oversampling 32, halved from QUP 2.5
 * (0x20050000; QUP_HW_VER at the wrapper, qcom,qupv3_0_geni_se@8c0000 + 4). A transfer: TX_TRANS_LEN, then M_CMD0 =
 * UART_START_TX << 27, the bytes into TX_FIFOn, M_IRQ_STATUS bit 0 (command done) awaited; reading: S_CMD0 =
 * UART_START_READ << 27, words out of RX_FIFOn as RX_FIFO_STATUS counts them.
 *
 * Then the WCN3990 (hci_qca.c qca_regulator_init(), qca_send_power_pulse(); btqca.h): its supplies on do not start
 * Bluetooth; a circuit on TX decodes single bytes sent at given rates: 0xC0 at 2400 (off, into its boot mode), then
 * 0xFC at 115200 (on), and 100 ms for it to boot, flow control off for both. Then btqca.c qca_read_soc_version():
 * the vendor command EDL_PATCH_CMD_OPCODE 0xFC00 with EDL_PATCH_VER_REQ_CMD 0x19, as H4 01 00 fc 01 19; the WCN3990
 * (below the WCN3991) answers with a vendor event: 04 ff 0e, cresp 0 (EDL_CMD_REQ_RES_EVT), rtype 2
 * (EDL_APP_VER_RES_EVT), then product id (4), patch version (2), ROM version (2), SoC id (4), little endian.
 */
#define QUP0_PHYS               0x008c0000UL
#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define GENI_CGC_CTRL           0x28
#define SE_GENI_STATUS          0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_SER_S_CLK_CFG      0x4c
#define SE_UART_LOOPBACK_CFG    0x22c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_UART_TX_TRANS_CFG    0x25c
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL_REG  0x634
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_RX_WATERMARK_REG 0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL_CFG      0xe30

static uint32_t uart_tx_depth, uart_rx_depth, uart_sampling;

static bool
wait_bits(vm_offset_t address, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t t = 0; t < us; t += 10) {
		if ((rd(address) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (rd(address) & mask) == want;
}

static void
uart_stop_rx(void)
{
	if (rd(se6 + SE_GENI_STATUS) & (1u << 12)) {           /* S_GENI_CMD_ACTIVE */
		wr(se6 + SE_GENI_S_CMD_CTRL_REG, 1u << 2);       /* S_GENI_CMD_CANCEL */
		wait_bits(se6 + SE_GENI_STATUS, 1u << 12, 0, 20000);
	}
	wr(se6 + SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
}

static void
uart_start_rx(void)
{
	wr(se6 + SE_GENI_S_CMD0, 1u << 27);                     /* UART_START_READ */
}

static void
uart_baud(uint32_t baud)
{
	uint32_t divider = 7372800u / (baud * uart_sampling), cfg = 1u | (divider << 4);

	uart_stop_rx();
	wr(se6 + GENI_SER_M_CLK_CFG, cfg);
	wr(se6 + GENI_SER_S_CLK_CFG, cfg);
	uart_start_rx();
	say("bt6t: the UART at %u baud (divider %u, oversampling %u)\n", baud, divider, uart_sampling);
}

static bool
uart_write(const uint8_t *bytes, uint32_t length)
{
	wr(se6 + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(se6 + SE_UART_TX_TRANS_LEN, length);
	wr(se6 + SE_GENI_M_CMD0, 1u << 27);                     /* UART_START_TX */
	for (uint32_t at = 0; at < length; at += 4) {
		uint32_t word = 0;

		for (uint32_t b = 0; b < 4 && at + b < length; b++) {
			word |= (uint32_t)bytes[at + b] << (8 * b);
		}
		if (!wait_bits(se6 + SE_GENI_TX_FIFO_STATUS, 0x0fffffffu, 0, 0) &&
		    (rd(se6 + SE_GENI_TX_FIFO_STATUS) & 0x0fffffffu) >= uart_tx_depth) {
			for (uint32_t t = 0; t < 100000 && (rd(se6 + SE_GENI_TX_FIFO_STATUS) & 0x0fffffffu) >= uart_tx_depth; t += 10) {
				delay(10);
			}
		}
		wr(se6 + SE_GENI_TX_FIFOn, word);
	}
	if (!wait_bits(se6 + SE_GENI_M_IRQ_STATUS, 1u, 1u, 200000)) {   /* M_CMD_DONE */
		say("bt6t: the UART did not finish sending %u bytes (M_IRQ_STATUS 0x%08x, TX FIFO 0x%08x)\n", length,
		    rd(se6 + SE_GENI_M_IRQ_STATUS), rd(se6 + SE_GENI_TX_FIFO_STATUS));
		return false;
	}
	wr(se6 + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	return true;
}

/* What has come in, up to `size` bytes, within `us`; the count. */
static uint32_t
uart_read(uint8_t *bytes, uint32_t size, uint32_t us)
{
	uint32_t got = 0;

	for (uint32_t t = 0; t <= us && got < size; t += 100) {
		uint32_t status = rd(se6 + SE_GENI_RX_FIFO_STATUS), words = status & 0x01ffffffu;

		for (uint32_t w = 0; w < words && got < size; w++) {
			uint32_t word = rd(se6 + SE_GENI_RX_FIFOn), n = 4;

			if (w == words - 1 && (status & (1u << 31)) && ((status >> 28) & 7) != 0) {
				n = (status >> 28) & 7;                     /* RX_LAST: a last word only partly filled */
			}
			for (uint32_t b = 0; b < n && got < size; b++) {
				bytes[got++] = (uint8_t)(word >> (8 * b));
			}
		}
		if (words == 0) {
			delay(100);
		}
	}
	return got;
}

static bool
uart_setup(void)
{
	vm_offset_t qup = ml_io_map(QUP0_PHYS, 0x4000);
	uint32_t version = qup != 0 ? rd(qup + 4) : 0, proto = (rd(se6 + 0x68) >> 8) & 0xff;

	if (proto != 2) {
		say("bt6t: the engine's firmware is protocol %u, not the UART (2)\n", proto);
		return false;
	}
	uart_tx_depth = (rd(se6 + SE_HW_PARAM_0) >> 16) & 0x3f;
	uart_rx_depth = (rd(se6 + SE_HW_PARAM_1) >> 16) & 0x3f;
	uart_sampling = version >= 0x20050000u ? 16 : 32;
	say("bt6t: level 3: QUP version 0x%08x, FIFOs %u/%u words: the engine set up as a UART (polled FIFO mode, 8N1, no flow "
	    "control)\n", version, uart_tx_depth, uart_rx_depth);
	uart_stop_rx();
	/* geni_se_init(): interrupts cleared, I/O defaults, mode, watermarks */
	wr(se6 + SE_GSI_EVENT_EN, 0);
	wr(se6 + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(se6 + SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	wr(se6 + SE_DMA_TX_IRQ_CLR, 0xffffffffu);
	wr(se6 + SE_DMA_RX_IRQ_CLR, 0xffffffffu);
	wr(se6 + GENI_CGC_CTRL, rd(se6 + GENI_CGC_CTRL) | 0x7fu);
	wr(se6 + SE_DMA_GENERAL_CFG, rd(se6 + SE_DMA_GENERAL_CFG) | 0xfu);
	wr(se6 + GENI_OUTPUT_CTRL, 0x7fu);
	wr(se6 + GENI_FORCE_DEFAULT_REG, 1u);
	wr(se6 + SE_GENI_DMA_MODE_EN, rd(se6 + SE_GENI_DMA_MODE_EN) & ~1u);
	wr(se6 + SE_GENI_RX_WATERMARK_REG, 2);
	wr(se6 + SE_GENI_RX_RFR_WATERMARK_REG, uart_rx_depth - 2);
	/* geni_se_config_packing(8, 4, false, true, true) */
	wr(se6 + SE_GENI_TX_PACKING_CFG0, 0x4380eu);
	wr(se6 + SE_GENI_TX_PACKING_CFG1, 0xc3e0eu);
	wr(se6 + SE_GENI_RX_PACKING_CFG0, 0x4380eu);
	wr(se6 + SE_GENI_RX_PACKING_CFG1, 0xc3e0eu);
	wr(se6 + SE_GENI_BYTE_GRAN, 0);
	/* qcom_geni_serial port_setup and set_termios: stale count, 8N1, no flow control, no loopback */
	wr(se6 + SE_UART_RX_STALE_CNT, 10 * 16);
	wr(se6 + SE_UART_LOOPBACK_CFG, 0);
	wr(se6 + SE_UART_TX_TRANS_CFG, (rd(se6 + SE_UART_TX_TRANS_CFG) & ~1u) | 2u);   /* no parity, UART_CTS_MASK */
	wr(se6 + SE_UART_TX_PARITY_CFG, rd(se6 + SE_UART_TX_PARITY_CFG) & ~1u);
	wr(se6 + SE_UART_RX_TRANS_CFG, rd(se6 + SE_UART_RX_TRANS_CFG) & ~(1u << 3));
	wr(se6 + SE_UART_RX_PARITY_CFG, rd(se6 + SE_UART_RX_PARITY_CFG) & ~1u);
	wr(se6 + SE_UART_TX_WORD_LEN, 8);
	wr(se6 + SE_UART_RX_WORD_LEN, 8);
	wr(se6 + SE_UART_TX_STOP_BIT_LEN, 0);
	return true;
}

static void
hci_hello(void)
{
	static const uint8_t off = 0xc0, on = 0xfc, version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	uint8_t answer[64];
	uint32_t got;
	char hex[3 * sizeof(answer) + 1];

	if (!uart_setup()) {
		return;
	}
	uart_baud(2400);
	say("bt6t: the power-off pulse (0xc0 at 2400 baud): %s\n", uart_write(&off, 1) ? "sent" : "NOT SENT");
	delay(10000);
	uart_baud(115200);
	say("bt6t: the power-on pulse (0xfc at 115200 baud): %s\n", uart_write(&on, 1) ? "sent" : "NOT SENT");
	delay(100000);
	got = uart_read(answer, sizeof(answer), 0);
	if (got != 0) {
		say("bt6t: %u bytes came in after the pulses (dropped)\n", got);
	}
	say("bt6t: HCI 01 00 fc 01 19 (QCA: read the version): %s\n", uart_write(version, sizeof(version)) ? "sent" : "NOT SENT");
	got = uart_read(answer, sizeof(answer), 1000000);
	hex[0] = 0;
	for (uint32_t i = 0; i < got; i++) {
		snprintf(hex + 3 * i, sizeof(hex) - 3 * i, "%02x ", answer[i]);
	}
	say("bt6t: THE CONTROLLER ANSWERS %u bytes: %s\n", got, got ? hex : "(nothing in 1 s)");
	if (got >= 17 && answer[0] == 0x04 && answer[1] == 0xff && answer[3] == 0x00 && answer[4] == 0x02) {
		const uint8_t *v = answer + 5;

		say("bt6t: THE WCN3990 IS UP: product id 0x%08x, patch version 0x%04x, ROM version 0x%04x, SoC id 0x%08x\n",
		    v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24, v[4] | v[5] << 8, v[6] | v[7] << 8,
		    v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24);
	}
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
	if (level > 3) {
		say("bt6t: bt6t=%u: only levels 1 to 3 are written so far: level 3\n", level);
		level = 3;
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
	if (level >= 3) {
		hci_hello();
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt6t_init);
