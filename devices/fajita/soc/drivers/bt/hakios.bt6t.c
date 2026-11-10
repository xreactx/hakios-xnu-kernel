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
 *   bt6t=4        and its firmware (fw/crbtfw21.tlv, fw/crnv21.bin of the small image), HCI_Reset, its address, an inquiry
 *   bt6t=5        level 4 without the inquiry and the LE scan, then the controller handed to Darwin: /dev/uart.bluetooth and
 *                 /dev/cu.bluetooth, a tty of H4 bytes in both directions, and the IOService "bluetooth" (transport-encoding
 *                 1 = H4, transport-speed, local-mac-address) by which bluetoothd finds it (hakios.bt6t_ioreg.cpp)
 *   bt6t_bdaddr=  the phone's own address (xl adds it from fw-bt/bdaddr)
 *   bt6t_rfr=0    RFR held ready instead of by the hardware
 */
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/random.h>
#include <sys/tty.h>
#include <sys/proc.h>
#include <miscfs/devfs/devfs.h>
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
static uint32_t uart_clock_hz = 7372800;        /* (the engine's clock: 7.3728 MHz, then 96 or 102.4 MHz for 3 Mbit/s) */

/* The engine's RCG (0x17754) from GPLL0 even (300 MHz) through M/N: the rate 300 MHz * m / n. */
static void
uart_rcg(uint32_t m, uint32_t n)
{
	const uint32_t rcg = 0x17754, mask = 0xffff;
	uint32_t cfg, tries;

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
	uart_clock_hz = (uint32_t)(300000000ull * m / n);
	say("bt6t: RCG 0x%x: CFG 0x%08x, M 0x%04x, N 0x%04x, D 0x%04x; CMD 0x%08x (update %s): %u Hz\n", rcg,
	    rd(gcc + rcg + 0x4), rd(gcc + rcg + 0x8) & mask, rd(gcc + rcg + 0xc) & mask, rd(gcc + rcg + 0x10) & mask,
	    rd(gcc + rcg), (rd(gcc + rcg) & 1u) ? "NOT TAKEN" : "taken", uart_clock_hz);
}

static bool
uart_clock_and_pins(void)
{
	static const uint32_t pull[] = { 0, 1, 3, 0 };          /* 45 none, 46 down, 47 up, 48 none */
	uint32_t tries;
	bool on = true;

	say("bt6t: level 2: the UART's clock (7.3728 MHz) and its pins; the chip is not spoken to\n");
	uart_rcg(384, 15625);
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
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_UART_MANUAL_RFR      0x2ac
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
#define SE_GENI_IOS             0x908
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
	uint32_t divider = uart_clock_hz / (baud * uart_sampling), cfg = 1u | (divider << 4);

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

/*
 * What has come in, up to `size` bytes, within `us`; the count. The RX FIFO gives words of four bytes: what a read does
 * not take of a word is kept for the next (a read of one byte used to drop the other three -- every event read a byte at
 * a time came out as a byte of every fourth, and looked like no answer at all: runs 154612 to 160629).
 */
static uint8_t rx_kept[4 * 64];
static uint32_t rx_kept_at, rx_kept_count;
static uint32_t debug_packets, other_packets;  /* (ACL on the firmware's debug handle 0x2edc; other ACL/SCO) */
static uint32_t rx_overflows;                  /* (the RX FIFO full: bytes lost, events out of step after) */

static uint32_t
uart_read(uint8_t *bytes, uint32_t size, uint32_t us)
{
	uint32_t got = 0;

	for (uint32_t t = 0; t <= us && got < size; t += 100) {
		uint32_t status, words;

		while (got < size && rx_kept_count != 0) {
			bytes[got++] = rx_kept[rx_kept_at++];
			rx_kept_count--;
		}
		if (got == size) {
			break;
		}
		rx_kept_at = 0;
		if (rd(se6 + SE_GENI_S_IRQ_STATUS) & (1u << 25)) {    /* S_RX_FIFO_WR_ERR: bytes came to a full FIFO, and were lost */
			rx_overflows++;
			wr(se6 + SE_GENI_S_IRQ_CLEAR, 1u << 25);
		}
		status = rd(se6 + SE_GENI_RX_FIFO_STATUS);
		words = status & 0x01ffffffu;
		if (words > sizeof(rx_kept) / 4) {
			words = sizeof(rx_kept) / 4;
		}
		for (uint32_t w = 0; w < words; w++) {
			uint32_t word = rd(se6 + SE_GENI_RX_FIFOn), n = 4;

			if (w == words - 1 && (status & (1u << 31)) && ((status >> 28) & 7) != 0) {
				n = (status >> 28) & 7;                     /* RX_LAST: a last word only partly filled */
			}
			for (uint32_t b = 0; b < n; b++) {
				rx_kept[rx_kept_count++] = (uint8_t)(word >> (8 * b));
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

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern void *rootvnode;

static uint32_t bt_level;
static thread_call_t later;
static uint32_t bt_baud = 115200;               /* (the UART's rate now; bt6t_baud= is the one asked for, 3000000) */
static uint8_t bt_baud_code;                    /* (its QCA_BAUDRATE_ code: 0 115200, 0x0e 3000000, 0x11 3200000) */
static bool bt_ready, bt_have_bdaddr;           /* (level 5: the controller up, its firmware in, its address set) */
static uint8_t bt_bdaddr[6];                    /* (HCI order: least significant octet first) */

/* Bytes as hex into a line, at most `max` of them. */
static void
hex_line(char *line, size_t size, const uint8_t *bytes, uint32_t count, uint32_t max)
{
	line[0] = 0;
	for (uint32_t i = 0; i < count && i < max && 3 * i + 4 < size; i++) {
		snprintf(line + 3 * i, size - 3 * i, "%02x ", bytes[i]);
	}
}

/*
 * Qualcomm's in-band sleep (hci_qca.c, HCI_IBS_*): single bytes between the H4 packets. The controller says 0xfe when
 * it goes to sleep, and takes no command then until it is sent 0xfd and has answered 0xfc; when it has something to
 * send it says 0xfd first and waits for 0xfc. It runs on the ROM already: the controller answered the version request
 * and then neither HCI_Reset nor the rampatch's last segment (runs 155555, 155906) -- asleep, and waiting to be
 * answered.
 */
#define IBS_SLEEP_IND           0xfe
#define IBS_WAKE_IND            0xfd
#define IBS_WAKE_ACK            0xfc

static bool ibs_asleep, ibs_acked;
static uint32_t ibs_sleeps, ibs_wakes_ours, ibs_wakes_theirs, ibs_acks;

/* A byte outside a packet: in-band sleep's, or not (returns false: junk). */
static bool
ibs_byte(uint8_t byte)
{
	static const uint8_t ack = IBS_WAKE_ACK;

	switch (byte) {
	case IBS_SLEEP_IND:
		ibs_asleep = true;
		ibs_sleeps++;
		return true;
	case IBS_WAKE_IND:
		ibs_wakes_theirs++;
		uart_write(&ack, 1);
		ibs_asleep = false;
		return true;
	case IBS_WAKE_ACK:
		ibs_acked = true;
		ibs_acks++;
		ibs_asleep = false;
		return true;
	}
	return false;
}

/* The controller woken if it said it sleeps: 0xfd until it answers 0xfc (100 ms, three times). */
static bool
ibs_wake(void)
{
	static const uint8_t wake = IBS_WAKE_IND;

	if (!ibs_asleep) {
		return true;
	}
	for (int tries = 0; tries < 3; tries++) {
		ibs_acked = false;
		ibs_wakes_ours++;
		uart_write(&wake, 1);
		for (uint32_t t = 0; t < 100000 && !ibs_acked; t += 100) {
			uint8_t byte;

			if (uart_read(&byte, 1, 0) == 1) {
				ibs_byte(byte);
			} else {
				delay(100);
			}
		}
		if (ibs_acked) {
			return true;
		}
	}
	say("bt6t: the controller does not wake (0xfd sent three times, no 0xfc)\n");
	return false;
}

/* Sends an H4 command (01, opcode, length, parameters), waking the controller first if it sleeps. */
static bool
hci_command(uint16_t opcode, const uint8_t *parameters, uint8_t length)
{
	static uint8_t packet[4 + 255];

	/*
	 * (nothing is drained first: the controller sends events of its own -- vendor 04 ff ... -- and a drain took half of
	 * one, which left the next read out of step (runs 162304 to 163957); hci_event() reads them whole and the callers
	 * pass over what is not theirs; in-band sleep is off in the NVM, and a stray 0xfe/0xfd/0xfc is taken by hci_event())
	 */
	if (!ibs_wake()) {
		return false;
	}

	packet[0] = 0x01;
	packet[1] = (uint8_t)opcode;
	packet[2] = (uint8_t)(opcode >> 8);
	packet[3] = length;
	memcpy(packet + 4, parameters, length);
	return uart_write(packet, 4u + length);
}

/*
 * One H4 event, read whole (04, code, length, parameters) within `us`: the bytes into `event`, the count returned (0:
 * nothing, or not an event). Bytes before an 04 are dropped and counted in *junk.
 */
static uint32_t
hci_event(uint8_t *event, uint32_t size, uint32_t us, uint32_t *junk)
{
	uint32_t got = 0, need = 3;
	uint64_t spent = 0;

	while (got < need && spent <= us) {
		uint32_t n = uart_read(event + got, 1, 0);

		if (n == 0) {
			delay(50);
			spent += 50;
			continue;
		}
		if (got == 0 && (event[0] == 0x02 || event[0] == 0x03)) {
			/*
			 * An ACL (02: handle 2, length 2) or SCO (03: handle 2, length 1) packet, read whole and passed over: the
			 * QCA firmware sends its debug log as ACL on handle 0x2edc (hci_qca.c QCA_DEBUG_HANDLE). Searching for the
			 * next 04 used to land inside one and read its bytes as events 0x89, 0xfa..0xfd (runs 163957 to 173252).
			 */
			uint8_t header[4], skip[64];
			uint32_t header_length = event[0] == 0x02 ? 4 : 3, length, have = 0;
			uint64_t waited = 0;

			while (have < header_length && waited <= us) {
				uint32_t n = uart_read(header + have, header_length - have, 0);

				have += n;
				if (n == 0) {
					delay(50);
					waited += 50;
				}
			}
			if (have < header_length) {
				return 0;
			}
			length = event[0] == 0x02 ? (uint32_t)(header[2] | header[3] << 8) : header[2];
			if ((header[0] | (header[1] & 0x0f) << 8) == 0x2edc) {
				debug_packets++;
			} else {
				other_packets++;
			}
			for (have = 0; have < length && waited <= us;) {
				uint32_t want = length - have < sizeof(skip) ? length - have : sizeof(skip), n = uart_read(skip, want, 0);

				have += n;
				if (n == 0) {
					delay(50);
					waited += 50;
				}
			}
			spent += waited;
			continue;
		}
		if (got == 0 && event[0] != 0x04) {
			if (!ibs_byte(event[0]) && junk != NULL) {
				(*junk)++;
			}
			continue;
		}
		got++;
		if (got == 3) {
			need = 3u + event[2];
			if (need > size) {
				return 0;
			}
		}
	}
	return got == need ? got : 0;
}

/* A command and its Command Complete (04 0e len ncmd opcode-lo opcode-hi status ...); the status, or -1. */
static int
hci_command_complete(uint16_t opcode, const uint8_t *parameters, uint8_t length, uint8_t *event, uint32_t size, uint32_t us)
{
	if (!hci_command(opcode, parameters, length)) {
		return -1;
	}
	for (int tries = 0; tries < 32; tries++) {
		uint32_t got = hci_event(event, size, us, NULL);

		if (got == 0) {
			return -1;
		}
		if (got >= 7 && event[1] == 0x0e && (event[4] | event[5] << 8) == opcode) {
			return event[6];
		}
		if (got >= 7 && event[1] == 0x0f && (event[5] | event[6] << 8) == opcode && event[3] != 0) {
			return event[3];                                /* (Command Status with an error: no Complete follows) */
		}
		{
			static uint32_t told;
			char line[3 * 16 + 1];

			if (told++ < 12) {
				hex_line(line, sizeof(line), event, got, 16);
				say("bt6t: waiting for 0x%04x's Command Complete, another event: %u bytes: %s\n", opcode, got, line);
			}
		}
	}
	return -1;
}

/*
 * A firmware file into the controller as btqca.c qca_download_firmware() sends it: 243-byte segments, each the vendor
 * command 0xfc00 with EDL_PATCH_TLV_REQ_CMD 0x1e and the segment's length; in download mode 0 every segment is answered
 * with a vendor event 04 ff 03 00 04 <result> (EDL_TVL_DNLD_RES_EVT), in modes 1 and 3 (skip the VSE / the VSE and the
 * Command Complete) only the last segment is.
 */
static bool
tlv_download(const char *what, const uint8_t *data, uint32_t size, uint8_t mode)
{
	static uint8_t segment[2 + 243];
	uint8_t event[64];
	uint32_t sent = 0, segments = 0, acked = 0, junk = 0, ccs = 0;
	uint64_t began = mach_absolute_time(), ns;

	while (sent < size) {
		uint32_t n = size - sent < 243 ? size - sent : 243;
		bool last = sent + n == size || n < 243;

		segment[0] = 0x1e;
		segment[1] = (uint8_t)n;
		memcpy(segment + 2, data + sent, n);
		if (!hci_command(0xfc00, segment, (uint8_t)(2 + n))) {
			say("bt6t: %s: segment %u could not be sent\n", what, segments);
			return false;
		}
		segments++;
		sent += n;
		if (mode == 0 || last) {
			uint32_t got = hci_event(event, sizeof(event), 10000000, &junk);    /* (HCI_INIT_TIMEOUT: 10 s) */

			/* (mode 0: the vendor event and the Command Complete of 0xfc00, 04 0e 04 01 00 fc 00, in either order) */
			for (int more = 0; more < 4 && got >= 6 && event[1] == 0x0e && event[4] == 0x00 && event[5] == 0xfc; more++) {
				ccs++;
				got = hci_event(event, sizeof(event), 10000000, &junk);
			}
			if (got < 6 || event[1] != 0xff || event[3] != 0x00 || event[4] != 0x04) {
				char line[3 * 16 + 1];

				hex_line(line, sizeof(line), event, got, 16);
				absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
				say("bt6t: %s: segment %u (%u of %u bytes) NOT ANSWERED as expected: %u bytes: %s (%u stray bytes before; "
				    "%llu ms since the first segment, ~%llu ms the bytes need at 115200)\n", what, segments, sent, size, got,
				    got ? line : "(nothing in 10 s)", junk, ns / 1000000, (uint64_t)sent * 10 * 1000 / 115200);
				/* (and what follows, raw: lost bytes or a different event?) */
				got = uart_read(event, 32, 200000);
				hex_line(line, sizeof(line), event, got, 16);
				say("bt6t: %s: then, raw in 200 ms: %u bytes: %s (%u Command Completes skipped before)\n", what, got,
				    got ? line : "(nothing)", ccs);
				return false;
			}
			if (event[5] != 0) {
				say("bt6t: %s: segment %u: THE CONTROLLER REFUSES IT (result 0x%02x)\n", what, segments, event[5]);
				return false;
			}
			acked++;
			/* (its Command Complete, when it comes after: read now, not drained half-way by the next command) */
			if (mode == 0 && !last) {
				uint8_t cc[16];
				uint32_t n = hci_event(cc, sizeof(cc), 100000, &junk);

				if (n >= 6 && cc[1] == 0x0e) {   /* (04 0e 04 01 00 00 00: its opcode comes back as 0x0000) */
					ccs++;
				} else if (n != 0) {
					static bool told;
					char line[3 * 16 + 1];

					if (!told) {
						told = true;
						hex_line(line, sizeof(line), cc, n, 16);
						say("bt6t: %s: after an answer, another event: %s\n", what, line);
					}
					junk += n;
				}
			}
		} else {
			/* (nothing is to come but in-band sleep's bytes: read off, answered, the rest counted) */
			uint8_t drop[16];
			uint32_t n = uart_read(drop, sizeof(drop), 0);

			for (uint32_t b = 0; b < n; b++) {
				if (!ibs_byte(drop[b])) {
					junk++;
				}
			}
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	say("bt6t: %s: %u bytes in %u segments (download mode %u), %u answered (%u Command Completes too), all well; %u stray "
	    "bytes; %llu ms; in-band sleep so far: %u sleeps, %u wakes by us, %u by it, %u acks\n", what, size, segments, mode,
	    acked, ccs, junk, ns / 1000000,
	    ibs_sleeps, ibs_wakes_ours, ibs_wakes_theirs, ibs_acks);
	return true;
}

/*
 * bt6t_nvm=TAG.OFFSET=VALUE[,TAG.OFFSET=VALUE...] (decimal tag and offset into the tag's data, VALUE in hex): one
 * byte of the NVM set for a run, to find what a tag does without a build. First use: tag 36 byte 12, which is 02 in the
 * OnePlus 6T's NVM, 00 in Linux's crnv21.bin and 01 in a Chromebook's crnv32.bin -- perhaps where a call's voice goes
 * (on the 6T it comes back over no HCI SCO at all, run 190111).
 */
static void
nvm_override(uint16_t id, uint8_t *data, uint16_t tag_length)
{
	char text[128];
	const char *p = text;

	if (!PE_parse_boot_argn("bt6t_nvm", text, sizeof(text))) {
		return;
	}
	while (*p != '\0') {
		uint32_t tag = 0, offset = 0, value = 0;

		while (*p >= '0' && *p <= '9') {
			tag = tag * 10 + (uint32_t)(*p++ - '0');
		}
		if (*p++ != '.') {
			break;
		}
		while (*p >= '0' && *p <= '9') {
			offset = offset * 10 + (uint32_t)(*p++ - '0');
		}
		if (*p++ != '=') {
			break;
		}
		while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
			value = value * 16 + (uint32_t)(*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
			p++;
		}
		if (tag == id && offset < tag_length) {
			say("bt6t: NVM tag %u byte %u: %02x -> %02x (bt6t_nvm)\n", tag, offset, data[offset], value & 0xff);
			data[offset] = (uint8_t)value;
		}
		if (*p == ',') {
			p++;
		}
	}
}

/*
 * The NVM's tags edited as this side can stand them (btqca.c qca_tlv_check_data() edits them for Linux, which runs
 * the in-band sleep protocol): tag 17 (EDL_TAG_ID_HCI) byte 0 bit 7 off -- no in-band sleep, which this driver does
 * not speak -- and byte 2, the UART rate the controller goes to, 0 (QCA_BAUDRATE_115200; the file's is 0x11,
 * 3.2 Mbit/s); tag 27 (EDL_TAG_ID_DEEP_SLEEP) 0. Type 4 wraps a type 2 set of tags (12-byte tag headers).
 */
static void
nvm_edit(uint8_t *nvm, uint32_t size)
{
	uint32_t at = 0, type_len, length;

	if (size < 8) {
		return;
	}
	type_len = nvm[0] | nvm[1] << 8 | nvm[2] << 16 | (uint32_t)nvm[3] << 24;
	if ((type_len & 0xff) == 4) {
		at = 4;
		type_len = nvm[4] | nvm[5] << 8 | nvm[6] << 16 | (uint32_t)nvm[7] << 24;
	}
	if ((type_len & 0xff) != 2) {
		say("bt6t: the NVM's tags are not of type 2 (0x%x): left as they are\n", type_len & 0xff);
		return;
	}
	length = type_len >> 8;
	at += 4;
	for (uint32_t i = 0; i + 12 <= length && at + i + 12 <= size;) {
		uint8_t *tag = nvm + at + i;
		uint16_t id = tag[0] | tag[1] << 8, tag_length = tag[2] | tag[3] << 8;

		if (at + i + 12 + tag_length > size) {
			break;
		}
		if (id == 17 && tag_length >= 3) {
			say("bt6t: NVM tag 17 (HCI): %02x %02x %02x -> %02x %02x %02x (no in-band sleep, the UART's rate kept: %u)\n",
			    tag[12], tag[13], tag[14], tag[12] & 0x7f, tag[13], bt_baud_code, bt_baud);
			tag[12] &= 0x7f;
			tag[14] = bt_baud_code;
		} else if (id == 27 && tag_length >= 1) {
			say("bt6t: NVM tag 27 (deep sleep): %02x -> 00\n", tag[12]);
			tag[12] = 0;
		} else if (id == 2 && tag_length == 6) {
			say("bt6t: NVM tag 2 (the address it gives itself): %02x:%02x:%02x:%02x:%02x:%02x\n", tag[17], tag[16], tag[15],
			    tag[14], tag[13], tag[12]);
		}
		if (id != 2) {
			nvm_override(id, tag + 12, tag_length);
		}
		i += 12u + tag_length;
	}
}

/* (the file stays hakios.image.c's: it keeps one copy per file for every caller, so it is never given back here) */
static const uint8_t *
wait_file(const char *name, uint64_t *size)
{
	for (int i = 0; i < 120; i++) {
		const uint8_t *data = hakios_image_file("fw", name, size, NULL);

		if (data != NULL) {
			return data;
		}
		delay(500000);
	}
	return NULL;
}

/*
 * Level 4: btqca.c qca_uart_setup() for the WCN3990 at ROM 2.1, at 115200: the rampatch crbtfw21.tlv (its download
 * mode is in its header), 10 ms, the NVM crnv21.bin (edited, every segment answered), HCI_Reset; then
 * HCI_Read_Local_Version_Information and HCI_Read_BD_ADDR, and a classic inquiry of 10 s (GIAC 9e8b33, 8 x 1.28 s):
 * how many devices answer, and how strongly. Addresses of other people's devices are not written down whole.
 */
/*
 * The UART from 115200 to the operating rate, before the firmware, as Linux's hci_qca.c qca_set_speed(QCA_OPER_SPEED)
 * does for the WCN3990 (115200 carries some 90 kbit/s, A2DP wants 200-350; the rampatch takes 20 s at 115200): RFR held
 * not ready so the controller sends nothing meanwhile, its vendor command 0xfc48 with the rate's code, 10 ms, the
 * engine's clock and divider moved (96 MHz for 3 Mbit/s: GPLL0 even * 8/25; 102.4 MHz for 3.2: * 128/375; oversampling
 * 32, divider 1), RFR back, and the vendor event it sends for the change read off. Then CTS heeded on our side (the
 * controller's RTS), unless bt6t_cts=0. bt6t_baud=115200 leaves it all as it was.
 */
static uint32_t
uart_fast(void)
{
	uint32_t want = 3000000, cts = 1, rfr = 1, got, junk = 0;
	uint8_t cmd[] = { 0x01, 0x48, 0xfc, 0x01, 0x00 }, event[64];
	char line[3 * 16 + 1];

	(void)PE_parse_boot_argn("bt6t_baud", &want, sizeof(want));
	(void)PE_parse_boot_argn("bt6t_cts", &cts, sizeof(cts));
	(void)PE_parse_boot_argn("bt6t_rfr", &rfr, sizeof(rfr));
	if (want != 3000000 && want != 3200000) {
		say("bt6t: the UART stays at 115200 (bt6t_baud=%u)\n", want);
		return 115200;
	}
	cmd[4] = want == 3000000 ? 0x0e : 0x11;
	wr(se6 + SE_UART_MANUAL_RFR, (1u << 31) | (1u << 1));   /* (UART_MANUAL_RFR_EN, UART_RFR_NOT_READY) */
	if (!uart_write(cmd, sizeof(cmd))) {
		wr(se6 + SE_UART_MANUAL_RFR, rfr ? 0 : (1u << 31));
		return 115200;
	}
	delay(10000);
	if (want == 3000000) {
		uart_rcg(8, 25);
	} else {
		uart_rcg(128, 375);
	}
	uart_baud(want);
	wr(se6 + SE_UART_MANUAL_RFR, rfr ? 0 : (1u << 31));
	bt_baud = want;
	bt_baud_code = cmd[4];
	got = hci_event(event, sizeof(event), 200000, &junk);
	hex_line(line, sizeof(line), event, got, 16);
	say("bt6t: the controller at %u baud (code 0x%02x): its answer %s (%u stray bytes)\n", want, cmd[4],
	    got ? line : "(none in 200 ms)", junk);
	if (cts) {
		wr(se6 + SE_UART_TX_TRANS_CFG, rd(se6 + SE_UART_TX_TRANS_CFG) & ~2u);   /* (UART_CTS_MASK off: CTS heeded) */
	}
	/* (still in step? the version request, which the ROM answers) */
	{
		static const uint8_t version[] = { 0x19 };

		hci_command(0xfc00, version, sizeof(version));
		got = hci_event(event, sizeof(event), 1000000, &junk);
		hex_line(line, sizeof(line), event, got, 16);
		say("bt6t: at %u baud, CTS %s: the version asked again: %s\n", want, cts ? "heeded" : "not heeded",
		    got ? line : "NO ANSWER (bt6t_baud=115200 for the old way)");
	}
	return want;
}

/* bt6t_bdaddr=aa:bb:cc:dd:ee:ff (xl adds it from fw-bt/bdaddr) into HCI order: least significant octet first */
static bool
parse_bdaddr(uint8_t out[6])
{
	char text[32] = { 0 };
	int octet = 5;
	uint32_t value = 0, digits = 0;

	if (!PE_parse_boot_argn("bt6t_bdaddr", text, sizeof(text))) {
		return false;
	}
	for (const char *c = text;; c++) {
		int v = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 :
		    *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;

		if (v >= 0 && digits < 2) {
			value = value << 4 | (uint32_t)v;
			digits++;
			continue;
		}
		if (digits != 2 || octet < 0 || (*c != ':' && *c != 0)) {
			return false;
		}
		out[octet--] = (uint8_t)value;
		value = digits = 0;
		if (*c == 0) {
			return octet == -1;
		}
	}
}

static void
hci_firmware_and_scan(void)
{
	const uint8_t *file;
	uint8_t *copy, event[260];
	uint64_t size = 0;
	int status;
	uint8_t *bdaddr = bt_bdaddr;
	bool have_bdaddr = bt_have_bdaddr = parse_bdaddr(bdaddr);

	(void)uart_fast();
	/* (first: does a plain command get through (flow control off)? HCI_Reset on the ROM, answered by Command Complete) */
	status = hci_command_complete(0x0c03, NULL, 0, event, sizeof(event), 3000000);
	say("bt6t: HCI_Reset before the rampatch (ROM firmware): %s (status %d); in-band sleep so far: %u sleeps, %u wakes by us, "
	    "%u by it, %u acks\n", status == 0 ? "DONE" : "NOT ANSWERED", status, ibs_sleeps, ibs_wakes_ours, ibs_wakes_theirs,
	    ibs_acks);
	file = wait_file("crbtfw21.tlv", &size);
	if (file == NULL || size < 24) {
		say("bt6t: no fw/crbtfw21.tlv in the small image (../fw-bt, userland/build.sh)\n");
		return;
	}
	/* (struct tlv_type_hdr, then tlv_type_patch: total_size 4, data_length 4, format_version, signature, download_mode) */
	if (!tlv_download("the rampatch crbtfw21.tlv", file, (uint32_t)size, file[14])) {
		/* (alive and in step? the version request again, which the ROM answered before the rampatch) */
		static const uint8_t version[] = { 0x19 };
		uint32_t got, junk = 0;
		char line[3 * 24 + 1];

		hci_command(0xfc00, version, sizeof(version));
		got = hci_event(event, sizeof(event), 2000000, &junk);
		hex_line(line, sizeof(line), event, got, 24);
		say("bt6t: after the rampatch, the version request again: %u bytes: %s (%u stray)\n", got,
		    got ? line : "(nothing in 2 s: the controller is not listening, or not in step)", junk);
		return;
	}
	delay(10000);
	file = wait_file("crnv21.bin", &size);
	if (file == NULL || size < 8) {
		say("bt6t: no fw/crnv21.bin in the small image\n");
		return;
	}
	copy = kalloc_data(size, Z_WAITOK);
	if (copy == NULL) {
		return;
	}
	memcpy(copy, file, size);
	nvm_edit(copy, (uint32_t)size);
	if (!tlv_download("the NVM crnv21.bin", copy, (uint32_t)size, 0)) {
		kfree_data(copy, size);
		return;
	}
	kfree_data(copy, size);
	status = hci_command_complete(0x0c03, NULL, 0, event, sizeof(event), 3000000);
	say("bt6t: HCI_Reset: %s (status %d)\n", status == 0 ? "DONE" : "FAILED", status);
	if (status != 0) {
		return;
	}
	if (have_bdaddr) {
		/* btqca.c qca_set_bdaddr(): EDL_WRITE_BD_ADDR 0xfc14, the address most significant octet first (swapped), answered by a
		 * vendor event (and a Command Complete); the controller keeps it until it is reset or powered off */
		uint32_t got, junk = 0, codes = 0;

		uint8_t swapped[6];

		for (int i = 0; i < 6; i++) {
			swapped[i] = bdaddr[5 - i];             /* (qca_set_bdaddr() baswap()s: this command wants it the other way) */
		}
		if (hci_command(0xfc14, swapped, sizeof(swapped))) {
			for (int i = 0; i < 4 && (got = hci_event(event, sizeof(event), 1000000, &junk)) != 0; i++) {
				codes = codes << 8 | event[1];
				if (event[1] == 0x0e && (event[4] | event[5] << 8) == 0xfc14) {
					break;
				}
			}
			say("bt6t: EDL_WRITE_BD_ADDR (the phone's own, ending :%02x): answered by event(s) 0x%x\n", bdaddr[0], codes);
		}
	}
	status = hci_command_complete(0x1001, NULL, 0, event, sizeof(event), 1000000);
	if (status == 0 && event[2] >= 9) {
		say("bt6t: HCI_Read_Local_Version: HCI %u rev 0x%04x, LMP %u subversion 0x%04x, manufacturer %u (29: Qualcomm)\n",
		    event[7], event[8] | event[9] << 8, event[10], event[13] | event[14] << 8, event[11] | event[12] << 8);
	}
	status = hci_command_complete(0x1009, NULL, 0, event, sizeof(event), 1000000);
	if (status == 0 && event[2] >= 10) {
		if (!have_bdaddr) {
			/* (no bt6t_bdaddr: the controller's own is what Darwin is given -- bluetoothd will not start without one) */
			memcpy(bt_bdaddr, event + 7, 6);
			bt_have_bdaddr = true;
		}
		/* (the phone's own address is not written down whole: only whether it took, and its last two octets) */
		say("bt6t: HCI_Read_BD_ADDR: THE CONTROLLER IS xx:xx:xx:xx:%02x:%02x%s\n", event[8], event[7],
		    !have_bdaddr ? " (no bt6t_bdaddr: the NVM's own)" : memcmp(event + 7, bdaddr, 6) == 0 ?
		    " -- THE PHONE'S OWN ADDRESS (bt6t_bdaddr)" : " -- NOT bt6t_bdaddr: the write did not take");
	}
	if (bt_level >= 5) {
		bt_ready = true;                                /* (the scans are level 4's; at 5 the controller is Darwin's now) */
		return;
	}
	{
		static const uint8_t inquiry[] = { 0x33, 0x8b, 0x9e, 0x08, 0x00 };    /* GIAC, 8 x 1.28 s, no limit */
		uint32_t results = 0, got, junk = 0;
		int strongest = -128;
		uint64_t start = mach_absolute_time(), ns = 0;

		if (!hci_command(0x0401, inquiry, sizeof(inquiry))) {
			return;
		}
		while (ns < 15000000000ull) {
			got = hci_event(event, sizeof(event), 1000000, &junk);
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
			if (got == 0) {
				continue;
			}
			if (event[1] == 0x0f) {
				say("bt6t: HCI_Inquiry: command status 0x%02x\n", event[3]);
			} else if (event[1] == 0x02 || event[1] == 0x22 || event[1] == 0x2f) {
				/* Inquiry Result / with RSSI / Extended: address at +4 (the first response), RSSI per kind */
				int rssi = event[1] == 0x02 ? 0 : (int8_t)event[1 + 2 + 1 + 6 + 1 + 1 + 3 + 2 + 1];

				results++;
				if (rssi > strongest && event[1] != 0x02) {
					strongest = rssi;
				}
				say("bt6t: a device answers (event 0x%02x): xx:xx:xx:xx:%02x:%02x%s%d dBm\n", event[1], event[5], event[4],
				    event[1] == 0x02 ? ", no RSSI in this kind" : ", ", event[1] == 0x02 ? 0 : rssi);
			} else if (event[1] == 0x01) {
				say("bt6t: HCI_Inquiry complete (status 0x%02x): %u answers%s\n", event[3], results,
				    results ? "" : " -- nothing in Bluetooth range is discoverable, or the radio does not hear");
				goto le;
			}
		}
		say("bt6t: HCI_Inquiry: no Inquiry Complete within 15 s (%u answers)\n", results);
	}
le:
	/*
	 * A passive LE scan of 10 s: LE gadgets advertise all the time, so this says whether the radio hears at all. The
	 * LE Meta event (bit 61) into the event mask first; then Set Scan Parameters (passive, 60 ms / 30 ms) and Scan
	 * Enable (no duplicate filter). Advertisers are counted by address; addresses are not written down whole.
	 */
	{
		static const uint8_t mask[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
		static const uint8_t parameters[] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
		static const uint8_t enable[] = { 0x01, 0x00 }, disable[] = { 0x00, 0x00 };
		static uint8_t seen[64][6];
		uint32_t heard = 0, reports = 0, junk = 0, got;
		int strongest = -128, weakest = 127;
		uint64_t start, ns = 0;

		const char *step = "Set_Event_Mask";

		event[0] = event[1] = 0;
		status = hci_command_complete(0x0c01, mask, sizeof(mask), event, sizeof(event), 1000000);
		if (status == 0) {
			step = "LE_Set_Scan_Parameters";
			status = hci_command_complete(0x200b, parameters, sizeof(parameters), event, sizeof(event), 1000000);
		}
		if (status == 0) {
			step = "LE_Set_Scan_Enable";
			status = hci_command_complete(0x200c, enable, sizeof(enable), event, sizeof(event), 1000000);
		}
		if (status != 0) {
			char line[3 * 12 + 1];

			hex_line(line, sizeof(line), event, 12, 12);
			say("bt6t: LE scan: not started: %s gave %d (the last event read: %s); RX FIFO overflows so far: %u\n", step,
			    status, line, rx_overflows);
			return;
		}
		start = mach_absolute_time();
		while (ns < 10000000000ull) {
			got = hci_event(event, sizeof(event), 500000, &junk);
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
			if (got < 15 || event[1] != 0x3e || event[3] != 0x02) {
				continue;
			}
			/* (LE Advertising Report, the first of its reports: type, address type, address at +7, data length, data, RSSI) */
			uint32_t data_length = event[13], k;
			int rssi = 14u + data_length < got ? (int8_t)event[14 + data_length] : 0;

			reports++;
			if (rssi > strongest) {
				strongest = rssi;
			}
			if (rssi < weakest) {
				weakest = rssi;
			}
			for (k = 0; k < heard && memcmp(seen[k], event + 7, 6) != 0; k++) {
			}
			if (k == heard && heard < 64) {
				memcpy(seen[heard++], event + 7, 6);
				if (heard <= 6) {
					say("bt6t: LE advertiser %u: xx:xx:xx:xx:%02x:%02x (address type %u), %d dBm\n", heard, event[8], event[7],
					    event[6], rssi);
				}
			}
		}
		(void)hci_command_complete(0x200c, disable, sizeof(disable), event, sizeof(event), 1000000);
		say("bt6t: RX FIFO overflows so far: %u; ACL packets passed over: %u of the firmware's debug log (0x2edc), %u "
		    "other\n", rx_overflows, debug_packets, other_packets);
		if (reports != 0) {
			say("bt6t: LE scan of 10 s: THE RADIO HEARS: %u advertisers, %u reports, %d to %d dBm\n", heard, reports, weakest,
			    strongest);
		} else {
			say("bt6t: LE scan of 10 s: nothing heard (%u stray bytes) -- the radio, or its antenna path, is not working\n",
			    junk);
		}
	}
}


/*
 * Level 5: the controller as a tty, the way iOS's bluetoothd takes a UART controller (its transport-encoding 1, "H4":
 * /dev/cu.bluetooth, else /dev/uart.bluetooth; H4 packets both ways, the transport set with termios). This is XNU's
 * own tty layer, as the console's (bsd/dev/arm/km.c): what is written goes out through bt_tty_start() to the UART; a
 * thread of ours polls the RX FIFO (RFR holds the controller off while it is full) and hands each byte to the line
 * discipline. The line starts raw. The UART stays at 115200 whatever termios is set to (the controller's NVM says
 * 115200; a rate asked for is written down, not taken). Both names are one tty.
 */
extern void IOSleep(unsigned int milliseconds);
extern int HakiosBt6tPublish(const uint8_t *address_msb_first, uint32_t speed);     /* hakios.bt6t_ioreg.cpp */

static struct tty *bt_tty;
static uint32_t bt_tx_bytes, bt_rx_bytes, bt_rx_dropped, bt_opens;

static void
bt_tty_start(struct tty *tp)                    /* t_oproc; tty locked */
{
	uint8_t buffer[256];

	if (tp->t_state & (TS_TIMEOUT | TS_BUSY | TS_TTSTOP)) {
		goto out;
	}
	tp->t_state |= TS_BUSY;
	while (tp->t_outq.c_cc > 0) {
		int count = ndqb(&tp->t_outq, 0);

		if (count <= 0) {
			break;
		}
		if (count > (int)sizeof(buffer)) {
			count = (int)sizeof(buffer);
		}
		count = q_to_b(&tp->t_outq, buffer, count);
		if (bt_tx_bytes < 64) {
			char line[3 * 16 + 1];

			hex_line(line, sizeof(line), buffer, (uint32_t)count, 16);
			say("bt6t: tty out %d bytes: %s\n", count, line);
		}
		bt_tx_bytes += (uint32_t)count;
		uart_write(buffer, (uint32_t)count);
	}
	tp->t_state &= ~TS_BUSY;
out:
	(*linesw[tp->t_line].l_start)(tp);      /* (ttwwakeup: writers waiting for room) */
}

static int
bt_tty_param(__unused struct tty *tp, struct termios *t)
{
	static speed_t told;

	if (t->c_ospeed != told) {
		told = t->c_ospeed;
		say("bt6t: tty: %lu baud asked for (the UART stays at 115200), cflag 0x%lx%s\n", (unsigned long)t->c_ospeed,
		    (unsigned long)t->c_cflag, (t->c_cflag & CRTSCTS) ? " (RTS/CTS)" : "");
	}
	return 0;
}

static int
bt_tty_open(__unused dev_t dev, __unused int flag, __unused int devtype, __unused proc_t p)
{
	struct tty *tp = bt_tty;
	int error;

	if (tp == NULL || !bt_ready) {
		return ENXIO;
	}
	tty_lock(tp);
	tp->t_oproc = bt_tty_start;
	tp->t_param = bt_tty_param;
	tp->t_dev = dev;
	if (!(tp->t_state & TS_ISOPEN)) {
		/* raw from the start: an echo of what came in would go back to the controller as a command */
		tp->t_iflag = 0;
		tp->t_oflag = 0;
		tp->t_lflag = 0;
		tp->t_cflag = CREAD | CS8 | CLOCAL | CRTSCTS;
		tp->t_ispeed = tp->t_ospeed = bt_baud;
		termioschars(&tp->t_termios);
		ttsetwater(tp);
	}
	tp->t_state |= TS_CARR_ON;
	error = (*linesw[tp->t_line].l_open)(dev, tp);
	tty_unlock(tp);
	bt_opens++;
	say("bt6t: tty opened (%s, by pid %d, open %u): %d\n", minor(dev) ? "cu.bluetooth" : "uart.bluetooth", proc_selfpid(),
	    bt_opens, error);
	return error;
}

static int
bt_tty_close(__unused dev_t dev, int flag, __unused int mode, __unused proc_t p)
{
	struct tty *tp = bt_tty;
	int error;

	tty_lock(tp);
	error = (*linesw[tp->t_line].l_close)(tp, flag);
	ttyclose(tp);
	tty_unlock(tp);
	say("bt6t: tty closed: %u bytes out, %u in, %u dropped while it was closed\n", bt_tx_bytes, bt_rx_bytes, bt_rx_dropped);
	return error;
}

static int
bt_tty_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	struct tty *tp = bt_tty;
	int error;

	tty_lock(tp);
	error = (*linesw[tp->t_line].l_read)(tp, uio, ioflag);
	tty_unlock(tp);
	return error;
}

static int
bt_tty_write(__unused dev_t dev, struct uio *uio, int ioflag)
{
	struct tty *tp = bt_tty;
	int error;

	tty_lock(tp);
	error = (*linesw[tp->t_line].l_write)(tp, uio, ioflag);
	tty_unlock(tp);
	return error;
}

static int
bt_tty_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, int flag, proc_t p)
{
	struct tty *tp = bt_tty;
	int error = 0;

	tty_lock(tp);
	switch (cmd) {
	case TIOCMGET:                  /* (the lines as a UART's: all up; RTS/CTS are the hardware's) */
		*(int *)data = TIOCM_LE | TIOCM_DTR | TIOCM_RTS | TIOCM_CTS | TIOCM_CAR | TIOCM_DSR;
		break;
	case TIOCMSET:
	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCSDTR:
	case TIOCCDTR:
	case TIOCSBRK:
	case TIOCCBRK:
		break;
	case TIOCSCTTY:
		error = ENOTTY;
		break;
	default:
		error = (*linesw[tp->t_line].l_ioctl)(tp, cmd, data, flag, p);
		if (error == ENOTTY) {
			error = ttioctl_locked(tp, cmd, data, flag, p);
		}
		if (error == ENOTTY) {
			static uint32_t told;

			if (told++ < 8) {
				say("bt6t: tty: ioctl 0x%lx not known (ENOTTY)\n", cmd);
			}
		}
		break;
	}
	tty_unlock(tp);
	return error;
}

static int
bt_tty_select(__unused dev_t dev, int which, void *wql, proc_t p)
{
	return ttyselect(bt_tty, which, wql, p);
}

static const struct cdevsw bt_tty_cdevsw = {
	.d_open       = bt_tty_open,
	.d_close      = bt_tty_close,
	.d_read       = bt_tty_read,
	.d_write      = bt_tty_write,
	.d_ioctl      = bt_tty_ioctl,
	.d_stop       = (stop_fcn_t *)nulldev,
	.d_reset      = (reset_fcn_t *)nulldev,
	.d_ttys       = NULL,
	.d_select     = bt_tty_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_TTY
};


/*
 * Level 5's transport for bluetoothd: what iOS 16's H4 driver ("POSIX", bluetoothd 20F66 0x1000666xx) asks for is not a
 * tty but a kernel control, com.apple.uart.sk.bluetooth (PF_SYSTEM, SOCK_STREAM), with the UART's termios by socket
 * option (getsockopt TIOCGETA 0x40487413, setsockopt TIOCSETA 0x80487414), a 12-byte setting (setsockopt 0x800c5414),
 * and by getsockopt 0x40105412 the UUID of a Skywalk nexus whose channel (os_channel_create(uuid, 0)) carries the H4
 * bytes: what it writes into the TX ring's slots goes to the UART here (bt_sk_sync_tx), what the UART gives is put into
 * the RX ring's (bt_sk_sync_rx, after the RX thread's kern_channel_notify). The nexus is a kernel pipe, as utun's
 * (bsd/net/if_utun.c utun_register_kernel_pipe_nexus, utun_kpipe_sync_tx/rx), set up at the first ask for its UUID.
 */
#if SKYWALK && CONFIG_NEXUS_KERNEL_PIPE
#include <sys/kern_control.h>
#include <skywalk/os_skywalk_private.h>

#define BT_SK_CONTROL           "com.apple.uart.sk.bluetooth"
#define BT_SK_GET_TERMIOS       0x40487413u     /* TIOCGETA */
#define BT_SK_SET_TERMIOS       0x80487414u     /* TIOCSETA */
#define BT_SK_SET_12            0x800c5414u     /* _IOW('T', 20, 12 bytes): written down, not understood yet */
#define BT_SK_GET_UUID          0x40105412u     /* _IOR('T', 18, uuid_t) */
#define BT_SK_BUF_SIZE          2048

LCK_GRP_DECLARE(bt_sk_grp, "bt6t_sk");
LCK_MTX_DECLARE(bt_sk_lock, &bt_sk_grp);

static kern_ctl_ref bt_sk_ctl;
static nexus_controller_t bt_sk_ncd;
static uuid_t bt_sk_prov_uuid, bt_sk_nx_uuid;
static kern_channel_ring_t bt_sk_txring, bt_sk_rxring;
static volatile bool bt_sk_open;               /* (a channel is connected: the RX thread gives the UART's bytes to it) */
static uint8_t bt_sk_termios[72];
static uint32_t bt_sk_tx_bytes, bt_sk_rx_bytes, bt_sk_rx_lost, bt_sk_renotify, bt_sk_rx_short, bt_sk_tx_failed;
static uint32_t bt_sk_tx_logged;                   /* (the first 300 commands are written down by opcode) */
static struct {
	uint8_t type, header[4];
	uint32_t have, need, body, at;          /* (bytes of the header had; header size; body length; body bytes had) */
	bool patch;                             /* (this is the Command Complete of 0x1002) */
	uint8_t first[17];                      /* (an event's first parameter bytes: ncmd, opcode, status; a
	                                           Synchronous Connection Complete's up to its air mode) */
} bt_sk_parse;
static uint32_t bt_sk_events_logged, bt_sk_acl_in, bt_sk_bad_bytes;
/* (SCO, the voice of a call: packets each way, their sizes the first few times -- never their contents) */
static uint32_t bt_sk_sco_in, bt_sk_sco_out;
/* (a packet is handed on whole once it is all there -- or not at all: the Command Complete of a command of ours) */
static uint8_t bt_sk_pkt[4 + 1100];
static uint32_t bt_sk_pkt_len, bt_sk_swallow_2005, bt_sk_vendor_dropped, bt_sk_debug_dropped;
static bool bt_sk_random_set;                   /* (LE Set Random Address done since the last HCI_Reset) */
/*
 * The last 64 packets each way, for the end of a channel: which way, H4 type, length, an event's code or a command's
 * opcode or an ACL packet's handle and flags, and when (ms since the channel connected) -- never their contents.
 */
static uint32_t bt_sk_rx_count;                 /* (declared again below, with the RX buffer: a tentative definition) */
static struct bt_sk_mark {
	uint32_t ms;
	uint16_t length, what;
	uint8_t way, type, rx_ring_slots;
} bt_sk_marks[128];
static uint32_t bt_sk_marks_n;
static uint64_t bt_sk_since;

static void
bt_sk_mark(uint8_t way, uint8_t type, uint16_t length, uint16_t what)
{
	uint64_t ns;
	struct bt_sk_mark *m = &bt_sk_marks[bt_sk_marks_n++ % (sizeof(bt_sk_marks) / sizeof(bt_sk_marks[0]))];

	absolutetime_to_nanoseconds(mach_absolute_time() - bt_sk_since, &ns);
	m->ms = (uint32_t)(ns / 1000000);
	m->way = way;
	m->type = type;
	m->length = length;
	m->what = what;
	m->rx_ring_slots = (uint8_t)MIN(bt_sk_rx_count / 64, 255);
}

static uint8_t bt_sk_rx_buf[16384];
static uint32_t bt_sk_rx_head, bt_sk_rx_count;  /* (head: the first byte not yet given; count: bytes waiting) */

static errno_t
bt_sk_pre_connect(__unused kern_nexus_provider_t nxprov, __unused proc_t p, __unused kern_nexus_t nexus,
    __unused nexus_port_t port, __unused kern_channel_t channel, __unused void **ch_ctx)
{
	return 0;
}

static errno_t
bt_sk_connected(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	lck_mtx_lock(&bt_sk_lock);
	bt_sk_rx_head = bt_sk_rx_count = 0;
	bt_sk_parse.type = 0;
	bt_sk_pkt_len = 0;
	bt_sk_swallow_2005 = 0;
	bt_sk_random_set = false;
	bt_sk_marks_n = 0;
	bt_sk_since = mach_absolute_time();
	lck_mtx_unlock(&bt_sk_lock);
	bt_sk_open = true;
	say("bt6t: sk: THE CHANNEL IS CONNECTED (pid %d)\n", proc_selfpid());
	return 0;
}

static void
bt_sk_pre_disconnect(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	bt_sk_open = false;
}

static void
bt_sk_disconnected(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel)
{
	say("bt6t: sk: channel disconnected: %u bytes to the controller, %u from it, %u lost, %u ACL in, %u bytes out of step; "
	    "%u syncs left bytes behind, %u notifies again\n", bt_sk_tx_bytes, bt_sk_rx_bytes, bt_sk_rx_lost, bt_sk_acl_in,
	    bt_sk_bad_bytes, bt_sk_rx_short, bt_sk_renotify);
	say("bt6t: sk: SCO (a call's voice): %u packets from the controller, %u to it\n", bt_sk_sco_in, bt_sk_sco_out);
	say("bt6t: sk: %u QCA vendor events and %u debug-log ACL packets kept back; %u UART writes failed; the last packets (way, type, length, opcode/event/handle, ms, bytes waiting/64):\n",
	    bt_sk_vendor_dropped, bt_sk_debug_dropped, bt_sk_tx_failed);
	{
		uint32_t total = sizeof(bt_sk_marks) / sizeof(bt_sk_marks[0]), first = bt_sk_marks_n > total ? bt_sk_marks_n - total : 0;

		for (uint32_t i = first; i < bt_sk_marks_n; i++) {
			struct bt_sk_mark *m = &bt_sk_marks[i % total];

			say("bt6t: sk: | %c %02x %4u 0x%04x %7u ms %u\n", m->way, m->type, m->length, m->what, m->ms, m->rx_ring_slots);
		}
	}
}

static errno_t
bt_sk_ring_init(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, __unused kern_channel_t channel,
    kern_channel_ring_t ring, boolean_t is_tx_ring, __unused void **ring_ctx)
{
	if (is_tx_ring) {
		bt_sk_txring = ring;
	} else {
		bt_sk_rxring = ring;
	}
	return 0;
}

static void
bt_sk_ring_fini(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring)
{
	if (ring == bt_sk_txring) {
		bt_sk_txring = NULL;
	} else if (ring == bt_sk_rxring) {
		bt_sk_rxring = NULL;
	}
}

/*
 * Commands bluetoothd will not go on without but the WCN3990 has not got: answered here with a Command Complete of
 * status 0, never sent, and marked as supported in the controller's answer to Read Local Supported Commands (0x1002;
 * the bit in its 64-byte mask, Core 5.0 Vol 2 Part E 6.27). Run 185844: bluetoothd gives up ("HCI initialization
 * failed", status 628) on Write Default Erroneous Data Reporting, octet 18 bit 3, clear on this chip (0x83).
 */
static const struct {
	uint16_t opcode;
	int16_t sub;                            /* (the first parameter byte to match, a vendor command's sub-command; -1 any) */
	uint8_t octet, bit;                     /* (octet 0xff: not in the mask -- a vendor command) */
	uint8_t reply;                          /* (bytes of return parameters after the status: the sub-command, then zeros) */
	const char *what;
} bt_sk_emulated[] = {
	{ 0x0c5b, -1, 18, 3, 0, "Write Default Erroneous Data Reporting" },
	/*
	 * LE Read Periodic Advertiser List Size: one of the commands that puts a 5.0 controller in its "extended" LE mode
	 * (Core 5.0 Vol 4 Part E 3.1.1), after which it refuses the legacy scan and advertising commands bluetoothd goes on
	 * to use (LE Set Scan Parameters 0x0c, Command Disallowed, and a restart, reason 712: run 193008). Answered here --
	 * a list of 0 -- so the WCN3990 stays in legacy mode.
	 */
	{ 0x204a, -1, 0xff, 0, 1, "LE Read Periodic Advertiser List Size (kept legacy)" },
	/*
	 * Apple's own (Broadcom) vendor commands: the QCA firmware answers "unknown command" (0x01), which bluetoothd takes
	 * for most of them (its "status 701": BD_VSC_LE_META_ENA_FEATURES 0xfce9, OLYMPIC_CHANGE_LE_SETTINGS 0xfce2/0x0b,
	 * OLYMPIC_SET_MIN_AFH_LE_CHANNELS 0xfce2/0x1c, ENABLE_EXTENDED_DUPLICATE_FILTER 0xfd47), but not for this one:
	 * BD_VSC_OLYMPIC_ANTENNA_PREF_B, 0xfce2 sub-command 0x15, whose answer must hold at least 2 bytes past the status
	 * (bluetoothd 0x100125fa8), or it restarts (reason 3602, runs 190934, 191425). Answered with no preference.
	 */
	/*
	 * 0xfce2 carries all of Apple's OLYMPIC settings (the first parameter byte is which: 0x0b CHANGE_LE_SETTINGS, 0x15
	 * ANTENNA_PREF_B, 0x1c SET_MIN_AFH_LE_CHANNELS, 0x36 SET_RETAIN_LE_SCAN_DUPLICATES_ON_START, ...): knobs of Apple's
	 * own chips, which this one has not got. Some of them bluetoothd lets fail, others not (0x15 run 191425, 0x36 run
	 * 192506): every one is taken here, as set, with its sub-command echoed and zeros.
	 */
	{ 0xfce2, -1, 0xff, 0, 8, "Apple vendor OLYMPIC setting" },
	/* BD_VSC_FACTORY_CAL_READ_TABLE, 0xfd1b <table>: no data is fatal too (run 191957); an empty table of zeros */
	{ 0xfd1b, -1, 0xff, 0, 16, "Apple vendor FACTORY_CAL_READ_TABLE" },
};

static void bt_sk_put(const uint8_t *bytes, uint32_t n);

/*
 * The controller's H4 stream, packet by packet (bytes come in pieces of any size): events written down by code and,
 * for a Command Complete or Status, opcode and status -- never their parameters, which hold addresses -- and the
 * supported-commands mask patched as above. ACL and SCO packets are counted.
 */

static void bt_sk_parse_one(uint8_t *byte);

static void
bt_sk_parse_byte(uint8_t *byte)
{
	bool starting = bt_sk_parse.type == 0;

	bt_sk_parse_one(byte);
	if (starting && bt_sk_parse.type == 0) {
		bt_sk_put(byte, 1);                     /* (a byte out of step: passed on as it is) */
		return;
	}
	if (bt_sk_pkt_len < sizeof(bt_sk_pkt)) {
		bt_sk_pkt[bt_sk_pkt_len++] = *byte;
	}
	if (bt_sk_parse.type == 0) {                    /* (the packet is whole) */
		bt_sk_mark('<', bt_sk_pkt[0], (uint16_t)bt_sk_pkt_len, bt_sk_pkt[0] == 0x04 ? (bt_sk_pkt[1] == 0x0e && bt_sk_pkt_len >= 6 ?
		    (uint16_t)(bt_sk_pkt[4] | bt_sk_pkt[5] << 8) : bt_sk_pkt[1]) : (uint16_t)(bt_sk_pkt[1] | bt_sk_pkt[2] << 8));
		bool drop = bt_sk_pkt_len >= 6 && bt_sk_pkt[0] == 0x04 && bt_sk_pkt[1] == 0x0e &&
		    (bt_sk_pkt[4] | bt_sk_pkt[5] << 8) == 0x2005 && bt_sk_swallow_2005 > 0;
		/*
		 * The QCA firmware's own vendor events (04 ff ...: it sends one before its "unknown command" for each of
		 * Apple's vendor commands) are not given to bluetoothd: it reads an 0xff event as a Broadcom one of its own,
		 * and one of these, so read, had it wait for bytes that never came -- "Timed out while receiving a packet",
		 * the link given up (reasons 129, 3602; runs 203400, 204010, 204722).
		 */
		bool vendor = bt_sk_pkt_len >= 2 && bt_sk_pkt[0] == 0x04 && bt_sk_pkt[1] == 0xff;
		/*
		 * Nor its debug log: ACL packets on handle 0xedc (0x2edc with the flags; hci_qca.c QCA_DEBUG_HANDLE, which
		 * Linux takes out before the stack), some 500 bytes each, many at 3 Mbit/s with a link up. bluetoothd took
		 * them for data of a connection it never made, waited for the rest, and gave the transport up ("Timed out
		 * while receiving a packet", every HCI command after it status 129, A2DP's start with them: run 215408).
		 */
		bool debug = bt_sk_pkt_len >= 3 && bt_sk_pkt[0] == 0x02 && (bt_sk_pkt[1] | (bt_sk_pkt[2] & 0x0f) << 8) == 0x0edc;

		if (drop) {
			bt_sk_swallow_2005--;
		} else if (vendor) {
			bt_sk_vendor_dropped++;
		} else if (debug) {
			bt_sk_debug_dropped++;
		} else {
			bt_sk_put(bt_sk_pkt, bt_sk_pkt_len);
		}
		bt_sk_pkt_len = 0;
	}
}

static void
bt_sk_parse_one(uint8_t *byte)
{
	if (bt_sk_parse.type == 0) {
		if (*byte != 0x02 && *byte != 0x03 && *byte != 0x04) {
			bt_sk_bad_bytes++;
			return;
		}
		bt_sk_parse.type = *byte;
		bt_sk_parse.have = bt_sk_parse.at = 0;
		bt_sk_parse.need = *byte == 0x02 ? 4 : *byte == 0x03 ? 3 : 2;
		bt_sk_parse.patch = false;
		return;
	}
	if (bt_sk_parse.have < bt_sk_parse.need) {
		bt_sk_parse.header[bt_sk_parse.have++] = *byte;
		if (bt_sk_parse.have < bt_sk_parse.need) {
			return;
		}
		bt_sk_parse.body = bt_sk_parse.type == 0x02 ? (uint32_t)(bt_sk_parse.header[2] | bt_sk_parse.header[3] << 8) :
		    bt_sk_parse.type == 0x03 ? bt_sk_parse.header[2] : bt_sk_parse.header[1];
		if (bt_sk_parse.body != 0) {
			return;
		}
	} else {
		uint32_t at = bt_sk_parse.at++;

		if (at < sizeof(bt_sk_parse.first)) {
			bt_sk_parse.first[at] = *byte;
			if (at == 2 && bt_sk_parse.type == 0x04 && bt_sk_parse.header[0] == 0x0e &&
			    (bt_sk_parse.first[1] | bt_sk_parse.first[2] << 8) == 0x1002) {
				bt_sk_parse.patch = true;
			}
		}
		if (bt_sk_parse.patch && at >= 4) {
			for (unsigned i = 0; i < sizeof(bt_sk_emulated) / sizeof(bt_sk_emulated[0]); i++) {
				if (bt_sk_emulated[i].octet != 0xff && at == 4u + bt_sk_emulated[i].octet) {
					*byte |= (uint8_t)(1u << bt_sk_emulated[i].bit);
				}
			}
		}
		if (bt_sk_parse.at < bt_sk_parse.body) {
			return;
		}
	}
	/* a whole packet */
	if (bt_sk_parse.type == 0x04) {
		uint8_t sync_code = bt_sk_parse.header[0];
		uint16_t status_opcode = bt_sk_parse.first[2] | bt_sk_parse.first[3] << 8;

		/*
		 * Whatever the count, the setting up of a call's voice link: Synchronous Connection Complete (0x2c: status,
		 * handle, -address-, link type 0 SCO 2 eSCO, intervals, packet lengths, air mode 2 CVSD 3 transparent) and
		 * Changed (0x2d), and the Command Status of the commands that ask for one.
		 */
		if (sync_code == 0x2c && bt_sk_parse.body >= 17) {
			const uint8_t *e = bt_sk_parse.first;

			say("bt6t: sk: < Synchronous Connection Complete: status 0x%02x handle 0x%03x link type %u interval %u "
			    "window %u rx %u tx %u bytes, air mode %u\n", e[0], (e[1] | e[2] << 8) & 0xfff, e[9], e[10], e[11],
			    e[12] | e[13] << 8, e[14] | e[15] << 8, e[16]);
		} else if (sync_code == 0x2d && bt_sk_parse.body >= 4) {
			say("bt6t: sk: < Synchronous Connection Changed: status 0x%02x handle 0x%03x\n", bt_sk_parse.first[0],
			    (bt_sk_parse.first[1] | bt_sk_parse.first[2] << 8) & 0xfff);
		} else if (sync_code == 0x0f && bt_sk_parse.body >= 4 && (status_opcode == 0x0428 || status_opcode == 0x0429 ||
		    status_opcode == 0x043d || status_opcode == 0x043e)) {
			say("bt6t: sk: < Command Status 0x%04x status 0x%02x (a voice link)\n", status_opcode, bt_sk_parse.first[0]);
		}
		if (bt_sk_events_logged++ < 300) {
			uint8_t code = bt_sk_parse.header[0];

			if (code == 0x0e && bt_sk_parse.body >= 4) {
				say("bt6t: sk: < Command Complete 0x%04x status 0x%02x%s\n", bt_sk_parse.first[1] | bt_sk_parse.first[2] << 8,
				    bt_sk_parse.first[3], bt_sk_parse.patch ? " (the mask patched for the emulated commands)" : "");
			} else if (code == 0x0f && bt_sk_parse.body >= 4) {
				say("bt6t: sk: < Command Status 0x%04x status 0x%02x\n", bt_sk_parse.first[2] | bt_sk_parse.first[3] << 8,
				    bt_sk_parse.first[0]);
			} else {
				say("bt6t: sk: < event 0x%02x (%u bytes)%s\n", code, bt_sk_parse.body,
				    code == 0x3e && bt_sk_parse.body >= 1 ? " LE meta" : "");
			}
		}
	} else if (bt_sk_parse.type == 0x03) {
		if (bt_sk_sco_in++ < 3) {
			say("bt6t: sk: < SCO packet, handle 0x%03x, %u bytes of voice\n",
			    (bt_sk_parse.header[0] | bt_sk_parse.header[1] << 8) & 0xfff, bt_sk_parse.body);
		}
	} else {
		bt_sk_acl_in++;
	}
	bt_sk_parse.type = 0;
}

/* bluetoothd's H4 bytes, slot by slot, to the controller */
static errno_t
bt_sk_sync_tx(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring,
    __unused uint32_t flags)
{
	kern_channel_slot_t slot = kern_channel_get_next_slot(ring, NULL, NULL), last = NULL;

	while (slot != NULL) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, slot);
		kern_buflet_t buf;

		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
		if (ph == 0 || (buf = kern_packet_get_next_buflet(ph, NULL)) == NULL) {
			continue;
		}
		uint8_t *data = (uint8_t *)kern_buflet_get_data_address(buf) + kern_buflet_get_data_offset(buf);
		uint32_t length = kern_packet_get_data_length(ph);

		if (length > BT_SK_BUF_SIZE) {
			length = BT_SK_BUF_SIZE;
		}
		if (length >= 4 && data[0] == 0x01) {
			uint16_t opcode = (uint16_t)(data[1] | data[2] << 8);
			bool emulated = false;

			for (unsigned i = 0; i < sizeof(bt_sk_emulated) / sizeof(bt_sk_emulated[0]); i++) {
				if (bt_sk_emulated[i].opcode == opcode &&
				    (bt_sk_emulated[i].sub < 0 || (length > 4 && data[4] == bt_sk_emulated[i].sub))) {
					uint8_t complete[7 + 32] = { 0x04, 0x0e, (uint8_t)(4 + bt_sk_emulated[i].reply), 0x01, data[1], data[2],
						                     0x00, bt_sk_emulated[i].reply && (opcode >> 10) == 0x3f && length > 4 ?
						                     data[4] : 0 };

					say("bt6t: sk: > command 0x%04x/%02x (%s): answered here, not sent\n", opcode, length > 4 ? data[4] : 0,
					    bt_sk_emulated[i].what);
					bt_sk_put(complete, 7u + bt_sk_emulated[i].reply);
					emulated = true;
				}
			}
			if (emulated) {
				continue;
			}
			/*
			 * A random own address with none set: Apple's controllers make their private addresses themselves (the
			 * LE_META IRK vendor commands, which this one refuses), so bluetoothd never sends LE Set Random Address,
			 * and LE Set Scan Enable is refused, Invalid HCI Command Parameters (0x12, Core 5.0 Vol 2 Part E 7.8.11;
			 * reason 718, run 194208). Given here before the command that needs it: a non-resolvable private address,
			 * new after every HCI_Reset (which clears it), its answer kept from bluetoothd.
			 */
			if (opcode == 0x0c03) {
				bt_sk_random_set = false;
			} else if (opcode == 0x2005) {
				bt_sk_random_set = true;
			} else if (!bt_sk_random_set && (((opcode == 0x200b || opcode == 0x2006) && length > 9 &&
			    (data[9] == 0x01 || data[9] == 0x03)) || (opcode == 0x200d && length > 16 &&
			    (data[16] == 0x01 || data[16] == 0x03)))) {
				uint8_t set[4 + 6] = { 0x01, 0x05, 0x20, 0x06 };

				read_random(set + 4, 6);
				set[9] &= 0x3f;                 /* (the top two bits 00: non-resolvable private) */
				set[4] |= 0x01;                 /* (not all zeros) */
				lck_mtx_lock(&bt_sk_lock);
				bt_sk_swallow_2005++;
				lck_mtx_unlock(&bt_sk_lock);
				uart_write(set, sizeof(set));
				bt_sk_random_set = true;
				say("bt6t: sk: > LE Set Random Address (a non-resolvable private one, ours) before command 0x%04x\n",
				    opcode);
			}
			/*
			 * LE Set Scan Enable's Filter_Duplicates is 0 or 1 for a legacy scan; Apple's stack asks for more (2, "reset
			 * each period", is an extended scan's), which the WCN3990 refuses as invalid parameters (0x12) -- and
			 * bluetoothd restarts, reason 718 (run 193613). Made 1: duplicates filtered.
			 */
			if (opcode == 0x200c && length == 6 && data[5] > 1) {
				static uint32_t told;

				if (told++ < 4) {
					say("bt6t: sk: > LE Set Scan Enable %u with Filter_Duplicates %u: sent as 1\n", data[4], data[5]);
				}
				data[5] = 1;
			}
			if ((opcode == 0x0428 || opcode == 0x0429) && length >= 20) {
				/* (Setup/Accept: bandwidths, latency, voice setting, retransmission effort, packet types) */
				const uint8_t *c = data + 4 + (opcode == 0x0428 ? 2 : 6);

				say("bt6t: sk: > %s Synchronous Connection: tx %u rx %u B/s, latency %u ms, voice setting 0x%03x, "
				    "retransmission %u, packet types 0x%04x\n", opcode == 0x0428 ? "Setup" : "Accept",
				    c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24, c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24,
				    c[8] | c[9] << 8, c[10] | c[11] << 8, c[12], c[13] | c[14] << 8);
			} else if (opcode == 0x043d || opcode == 0x043e) {
				say("bt6t: sk: > Enhanced %s Synchronous Connection (%u bytes)\n", opcode == 0x043d ? "Setup" : "Accept",
				    length);
			}
			if (bt_sk_tx_logged++ < 300) {
				if ((opcode >> 10) == 0x3f) {
					/* (a vendor command's parameters in full: they are settings, not addresses) */
					char line[3 * 24 + 1];

					hex_line(line, sizeof(line), data + 4, length - 4, 24);
					say("bt6t: sk: > vendor command 0x%04x: %s\n", opcode, line);
				} else {
					say("bt6t: sk: > command 0x%04x (%u bytes)\n", opcode, length);
				}
			}
		} else if (length && data[0] == 0x03) {
			if (bt_sk_sco_out++ < 3) {
				say("bt6t: sk: > SCO packet, handle 0x%03x, %u bytes of voice\n",
				    length >= 3 ? (data[1] | data[2] << 8) & 0xfff : 0, length >= 4 ? data[3] : 0);
			}
		} else if (bt_sk_tx_logged++ < 300) {
			say("bt6t: sk: > %s packet, %u bytes\n", length && data[0] == 0x02 ? "ACL" : "other", length);
		}
		bt_sk_mark('>', length ? data[0] : 0, (uint16_t)length, length >= 3 ? (uint16_t)(data[1] | data[2] << 8) : 0);
		bt_sk_tx_bytes += length;
		if (!uart_write(data, length)) {
			bt_sk_tx_failed++;
		}
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
		(void)kern_channel_reclaim(ring);
	}
	return 0;
}

/* what the controller said, into the RX ring's slots */
static errno_t
bt_sk_sync_rx(__unused kern_nexus_provider_t nxprov, __unused kern_nexus_t nexus, kern_channel_ring_t ring,
    __unused uint32_t flags)
{
	struct kern_pbufpool *pp = ring->ckr_pp;
	kern_channel_slot_t slot, last = NULL;

	(void)kern_channel_reclaim(ring);
	slot = kern_channel_get_next_slot(ring, NULL, NULL);
	while (slot != NULL) {
		kern_packet_t ph = 0;
		kern_buflet_t buf;
		uint8_t *data;
		uint32_t n;

		lck_mtx_lock(&bt_sk_lock);
		n = bt_sk_rx_count < BT_SK_BUF_SIZE ? bt_sk_rx_count : BT_SK_BUF_SIZE;
		lck_mtx_unlock(&bt_sk_lock);
		if (n == 0 || kern_pbufpool_alloc_nosleep(pp, 1, &ph) != 0) {
			break;
		}
		buf = kern_packet_get_next_buflet(ph, NULL);
		data = buf != NULL ? kern_buflet_get_data_address(buf) : NULL;
		if (data == NULL) {
			kern_pbufpool_free(pp, ph);
			break;
		}
		lck_mtx_lock(&bt_sk_lock);
		for (uint32_t i = 0; i < n; i++) {
			data[i] = bt_sk_rx_buf[(bt_sk_rx_head + i) % sizeof(bt_sk_rx_buf)];
		}
		bt_sk_rx_head = (bt_sk_rx_head + n) % sizeof(bt_sk_rx_buf);
		bt_sk_rx_count -= n;
		lck_mtx_unlock(&bt_sk_lock);
		bt_sk_rx_bytes += n;
		kern_packet_clear_flow_uuid(ph);
		if (kern_buflet_set_data_offset(buf, 0) != 0 || kern_buflet_set_data_length(buf, (uint16_t)n) != 0 ||
		    kern_packet_finalize(ph) != 0 || kern_channel_slot_attach_packet(ring, slot, ph) != 0) {
			say("bt6t: sk: an RX packet could not be put in its slot\n");
			kern_pbufpool_free(pp, ph);
			break;
		}
		last = slot;
		slot = kern_channel_get_next_slot(ring, slot, NULL);
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	if (bt_sk_rx_count != 0) {
		bt_sk_rx_short++;                       /* (bytes left for a later sync: no slot or no packet for them now) */
	}
	return 0;
}

/* The kernel pipe and its one instance, at the first ask for the UUID (a getsockopt: a thread that may block). */
static errno_t
bt_sk_nexus_up(void)
{
	nexus_attr_t nxa = NULL;
	uuid_t domain;
	errno_t error;
	struct kern_nexus_provider_init init = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = bt_sk_pre_connect,
		.nxpi_connected = bt_sk_connected,
		.nxpi_pre_disconnect = bt_sk_pre_disconnect,
		.nxpi_disconnected = bt_sk_disconnected,
		.nxpi_ring_init = bt_sk_ring_init,
		.nxpi_ring_fini = bt_sk_ring_fini,
		.nxpi_sync_tx = bt_sk_sync_tx,
		.nxpi_sync_rx = bt_sk_sync_rx,
	};
	struct kern_nexus_init nx_init = { .nxi_version = KERN_NEXUS_CURRENT_VERSION };

	if (!uuid_is_null(bt_sk_prov_uuid)) {
		goto instance;
	}
	if ((error = kern_nexus_controller_create(&bt_sk_ncd)) != 0 ||
	    (error = kern_nexus_get_default_domain_provider(NEXUS_TYPE_KERNEL_PIPE, &domain)) != 0 ||
	    (error = kern_nexus_attr_create(&nxa)) != 0) {
		goto out;
	}
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_SLOT_BUF_SIZE, BT_SK_BUF_SIZE);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_TX_SLOTS, 64);
	(void)kern_nexus_attr_set(nxa, NEXUS_ATTR_RX_SLOTS, 64);
	if ((error = kern_nexus_controller_register_provider(bt_sk_ncd, domain, (const uint8_t *)"com.hakios.nexus.bt6t",
	    &init, sizeof(init), nxa, &bt_sk_prov_uuid)) != 0) {
		goto out;
	}
instance:
	/*
	 * An instance for the process that asks, bound to it on the kernel pipe's client port (as utun_enable_channel()
	 * does): an unbound one is refused to os_channel_create() with EPERM (run 184818). bluetoothd starts again with a
	 * new pid after a crash, so each ask has an instance of its own and the last one is let go.
	 */
	if (!uuid_is_null(bt_sk_nx_uuid)) {
		if (bt_sk_open) {
			say("bt6t: sk: a channel is still open on the last instance: it is kept\n");
			return 0;
		}
		(void)kern_nexus_controller_free_provider_instance(bt_sk_ncd, bt_sk_nx_uuid);
		uuid_clear(bt_sk_nx_uuid);
	}
	error = kern_nexus_controller_alloc_provider_instance(bt_sk_ncd, bt_sk_prov_uuid, NULL, NULL, &bt_sk_nx_uuid,
	    &nx_init);
	if (error == 0) {
		nexus_port_t port = NEXUS_PORT_KERNEL_PIPE_CLIENT;

		error = kern_nexus_controller_bind_provider_instance(bt_sk_ncd, bt_sk_nx_uuid, &port, proc_selfpid(), NULL,
		    NULL, 0, NEXUS_BIND_PID);
		if (error != 0) {
			(void)kern_nexus_controller_free_provider_instance(bt_sk_ncd, bt_sk_nx_uuid);
			uuid_clear(bt_sk_nx_uuid);
		}
	}
out:
	if (nxa != NULL) {
		kern_nexus_attr_destroy(nxa);
	}
	{
		uuid_string_t text;

		uuid_unparse_upper(bt_sk_nx_uuid, text);
		say("bt6t: sk: the kernel pipe: %s for pid %d (error %d)\n", error == 0 ? text : "NOT MADE", proc_selfpid(), error);
	}
	return error;
}

static errno_t
bt_sk_ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *sac, __unused void **unitinfo)
{
	say("bt6t: sk: %s connected (unit %u, pid %d)\n", BT_SK_CONTROL, sac->sc_unit, proc_selfpid());
	return 0;
}

static errno_t
bt_sk_ctl_disconnect(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo)
{
	say("bt6t: sk: %s disconnected\n", BT_SK_CONTROL);
	return 0;
}

static errno_t
bt_sk_ctl_send(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, mbuf_t m, __unused int flags)
{
	say("bt6t: sk: %lu bytes written to the control socket itself (not passed on)\n", (unsigned long)mbuf_pkthdr_len(m));
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_sk_ctl_getopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data,
    size_t *len)
{
	errno_t error = 0;

	switch ((uint32_t)opt) {
	case BT_SK_GET_TERMIOS:
		if (data != NULL) {
			memcpy(data, bt_sk_termios, MIN(*len, sizeof(bt_sk_termios)));
		}
		*len = MIN(*len, sizeof(bt_sk_termios));
		break;
	case BT_SK_GET_UUID:
		if ((error = bt_sk_nexus_up()) == 0) {
			if (data != NULL) {
				memcpy(data, bt_sk_nx_uuid, MIN(*len, sizeof(uuid_t)));
			}
			*len = MIN(*len, sizeof(uuid_t));
		}
		break;
	default:
		error = ENOPROTOOPT;
		break;
	}
	say("bt6t: sk: getsockopt 0x%08x (%lu bytes): %d\n", (uint32_t)opt, (unsigned long)*len, error);
	return error;
}

static errno_t
bt_sk_ctl_setopt(__unused kern_ctl_ref ref, __unused u_int32_t unit, __unused void *unitinfo, int opt, void *data,
    size_t len)
{
	char line[3 * 16 + 1];

	hex_line(line, sizeof(line), data, (uint32_t)len, 16);
	say("bt6t: sk: setsockopt 0x%08x (%lu bytes): %s\n", (uint32_t)opt, (unsigned long)len, line);
	if ((uint32_t)opt == BT_SK_SET_TERMIOS && len == sizeof(bt_sk_termios)) {
		memcpy(bt_sk_termios, data, len);
		say("bt6t: sk: termios set: %llu baud (the UART stays where the kernel put it), cflag 0x%llx\n",
		    *(uint64_t *)(bt_sk_termios + 64), *(uint64_t *)(bt_sk_termios + 16));
	}
	return 0;
}

/* The kernel control, from the RX thread before its loop. */
static void
bt_sk_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	/* the line as bluetoothd will find it: 8N1, local, 115200 (struct termios of a 64-bit process: 72 bytes) */
	bzero(bt_sk_termios, sizeof(bt_sk_termios));
	*(uint64_t *)(bt_sk_termios + 16) = CS8 | CREAD | CLOCAL;
	*(uint64_t *)(bt_sk_termios + 56) = bt_baud;
	*(uint64_t *)(bt_sk_termios + 64) = bt_baud;
	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, BT_SK_CONTROL, sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = reg.ctl_recvsize = 16384;
	reg.ctl_connect = bt_sk_ctl_connect;
	reg.ctl_disconnect = bt_sk_ctl_disconnect;
	reg.ctl_send = bt_sk_ctl_send;
	reg.ctl_getopt = bt_sk_ctl_getopt;
	reg.ctl_setopt = bt_sk_ctl_setopt;
	error = ctl_register(&reg, &bt_sk_ctl);
	say("bt6t: sk: the kernel control %s: %s (%d)\n", BT_SK_CONTROL, error == 0 ? "registered" : "NOT REGISTERED", error);
}

/* From the RX thread: the UART's bytes for the channel; false when no channel is there to take them. */
static bool
bt_sk_give(const uint8_t *bytes, uint32_t n)
{
	kern_channel_ring_t ring = bt_sk_rxring;

	uint8_t copy[64];

	if (!bt_sk_open || ring == NULL) {
		return false;
	}
	while (n != 0) {
		uint32_t k = n < sizeof(copy) ? n : (uint32_t)sizeof(copy);

		memcpy(copy, bytes, k);
		for (uint32_t i = 0; i < k; i++) {
			bt_sk_parse_byte(&copy[i]);     /* (which hands each packet on when it is whole) */
		}
		bytes += k;
		n -= k;
	}
	return true;
}

/*
 * From the RX thread when the UART is quiet: bytes the RX ring had no room for at its last sync (its slots, or the
 * pool's packets, all out in a burst) wait for a notify, and the next would only come with the controller's next
 * bytes -- which, when it waits for the host, never come: bluetoothd sat on half an ACL packet, "Timed out while
 * receiving a packet", and restarted (reason 129, run 203400). So the ring is told again while any wait.
 */
static void
bt_sk_idle(void)
{
	kern_channel_ring_t ring = bt_sk_rxring;

	if (bt_sk_open && ring != NULL && bt_sk_rx_count != 0) {
		bt_sk_renotify++;
		kern_channel_notify(ring, 0);
	}
}

/* Bytes for bluetoothd's RX ring (the controller's, or a Command Complete of ours), and the ring told. */
static void
bt_sk_put(const uint8_t *bytes, uint32_t n)
{
	kern_channel_ring_t ring = bt_sk_rxring;

	lck_mtx_lock(&bt_sk_lock);
	for (uint32_t i = 0; i < n; i++) {
		if (bt_sk_rx_count == sizeof(bt_sk_rx_buf)) {
			bt_sk_rx_lost += n - i;
			break;
		}
		bt_sk_rx_buf[(bt_sk_rx_head + bt_sk_rx_count) % sizeof(bt_sk_rx_buf)] = bytes[i];
		bt_sk_rx_count++;
	}
	lck_mtx_unlock(&bt_sk_lock);
	if (ring != NULL) {
		kern_channel_notify(ring, 0);
	}
}
#else
static void
bt_sk_register(void)
{
	say("bt6t: sk: this kernel has no Skywalk kernel pipes: no transport for bluetoothd\n");
}

static bool
bt_sk_give(__unused const uint8_t *bytes, __unused uint32_t n)
{
	return false;
}

static void
bt_sk_idle(void)
{
}
#endif

/* The tty and its two names, the IOService; then the RX FIFO into the tty for good (this thread does not return). */
static void
bt_tty_serve(void)
{
	int major;
	uint8_t msb_first[6];

	bt_tty = ttymalloc();
	major = cdevsw_add(-1, &bt_tty_cdevsw);
	if (bt_tty == NULL || major < 0 || cdevsw_setkqueueok(major, &bt_tty_cdevsw, 0) == -1) {
		say("bt6t: level 5: no tty (major %d)\n", major);
		return;
	}
	bt_tty->t_dev = makedev(major, 0);
	(void)devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth");
	(void)devfs_make_node(makedev(major, 1), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth");
	for (int i = 0; i < 6; i++) {
		msb_first[i] = bt_bdaddr[5 - i];
	}
	say("bt6t: level 5: /dev/uart.bluetooth and /dev/cu.bluetooth (major %d); the IOService \"bluetooth\": %d\n", major,
	    HakiosBt6tPublish(bt_have_bdaddr ? msb_first : NULL, bt_baud));
	bt_sk_register();
	for (;;) {
		uint8_t bytes[64];
		uint32_t n = uart_read(bytes, sizeof(bytes), 0);

		if (n == 0) {
			bt_sk_idle();
			IOSleep(1);
			continue;
		}
		if (bt_sk_give(bytes, n)) {
			continue;                       /* (bluetoothd's channel has them) */
		}
		if (!(bt_tty->t_state & TS_ISOPEN)) {
			bt_rx_dropped += n;
			continue;
		}
		tty_lock(bt_tty);
		for (uint32_t i = 0; i < n; i++) {
			(*linesw[bt_tty->t_line].l_rint)(bytes[i], bt_tty);
		}
		tty_unlock(bt_tty);
		bt_rx_bytes += n;
	}
}

static bool hci_hello(void);

/* The controller's bring-up on a thread of its own: it waits for the small image and takes some 30 s. */
static void
bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (hci_hello() && bt_level >= 4) {
		hci_firmware_and_scan();
		if (bt_level >= 5 && bt_ready) {
			bt_tty_serve();
		}
	}
	thread_terminate(current_thread());
}

/* (from a thread call, so that the thread is made once the scheduler runs: not at EARLY_BOOT) */
static void
bt_later(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("bt6t: no thread for the controller's bring-up\n");
	}
}

static bool
hci_hello(void)
{
	static const uint8_t off = 0xc0, on = 0xfc, version[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	uint8_t answer[64];
	uint32_t got;
	char hex[3 * sizeof(answer) + 1];

	if (!uart_setup()) {
		return false;
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
	/*
	 * No hardware flow control (hci_qca.c turns it on here): with TX waiting for CTS the controller answered nothing more
	 * -- not even HCI_Reset -- and the rampatch's 217 KB took 57 s (run 155555); at 115200 it keeps up without. The
	 * lines' levels, for when it is to be turned on (SE_GENI_IOS: bit 0 RX, bit 1 CTS).
	 */
	/*
	 * (that run was before uart_read() kept a word's other bytes, so it proved little.) What is on now is the RX half
	 * only: RFR left to the hardware (SE_UART_MANUAL_RFR 0, deasserted at the RFR watermark, depth - 2 words), so the
	 * controller holds off while this side is busy (logging) instead of the FIFO overflowing -- run 165530 lost bytes
	 * and read events out of step after the inquiry. Our TX still ignores CTS. bt6t_rfr=0: RFR held ready, as before.
	 */
	{
		uint32_t rfr = 1;

		(void)PE_parse_boot_argn("bt6t_rfr", &rfr, sizeof(rfr));
		wr(se6 + SE_UART_MANUAL_RFR, rfr ? 0 : (1u << 31));     /* (UART_MANUAL_RFR_EN, UART_RFR_READY 0) */
		say("bt6t: SE_GENI_IOS after the pulses: 0x%08x (RX %u, CTS %u); RFR %s, our TX ignores CTS\n",
		    rd(se6 + SE_GENI_IOS), rd(se6 + SE_GENI_IOS) & 1, (rd(se6 + SE_GENI_IOS) >> 1) & 1,
		    rfr ? "BY THE HARDWARE (the controller held off when the FIFO fills)" : "held ready (bt6t_rfr=0)");
	}
	say("bt6t: HCI 01 00 fc 01 19 (QCA: read the version): %s\n", uart_write(version, sizeof(version)) ? "sent" : "NOT SENT");
	got = uart_read(answer, sizeof(answer), 1000000);
	hex[0] = 0;
	for (uint32_t i = 0; i < got; i++) {
		snprintf(hex + 3 * i, sizeof(hex) - 3 * i, "%02x ", answer[i]);
	}
	say("bt6t: THE CONTROLLER ANSWERS %u bytes: %s\n", got, got ? hex : "(nothing in 1 s)");
	/* (the vendor event wherever it begins: a byte left from the pulses may come first) */
	for (uint32_t at = 0; at + 17 <= got; at++) {
		const uint8_t *v = answer + at + 5;

		if (!(answer[at] == 0x04 && answer[at + 1] == 0xff && answer[at + 3] == 0x00 && answer[at + 4] == 0x02)) {
			continue;
		}

		say("bt6t: THE WCN3990 IS UP: product id 0x%08x, patch version 0x%04x, ROM version 0x%04x, SoC id 0x%08x\n",
		    v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24, v[4] | v[5] << 8, v[6] | v[7] << 8,
		    v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24);
		/* (the Command Complete that follows the vendor event, if it is in what came: read off already) */
		return true;
	}
	return false;
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
	if (level > 5) {
		say("bt6t: bt6t=%u: only levels 1 to 5 are written so far: level 5\n", level);
		level = 5;
	}
	bt_level = level;
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
		/* (on a thread: the firmware is a file of the small image, which is there only later, and its 217 KB take ~20 s) */
		later = thread_call_allocate(bt_later, NULL);
		thread_call_enter(later);
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, bt6t_init);
