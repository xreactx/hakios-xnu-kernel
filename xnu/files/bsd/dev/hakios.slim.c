/*
 * The OnePlus 6T's audio SLIMbus: the bus between the SoC's audio block and the WCD934x ("Tavil") codec, which
 * has the earpiece's amplifier and the microphones' converters (the loudspeaker is a TFA9894 on QUAT MI2S,
 * bsd/dev/hakios.tfa9894.c, and needs none of this).
 *
 * On the SDM845 the bus belongs to the audio DSP; this processor's side of it is a "non-ported generic
 * device" (NGD) in the audio block (dt: slim@171c0000, qcom,slim-ngd), and before that side may speak on the
 * bus the DSP has to be asked, over QMI, to give it an instance of the bus and to power it -- Linux's
 * drivers/slimbus/qcom-ngd-ctrl.c, whose messages and order are followed here. The DSP offers that service
 * (769, SLIMbus) on its QRTR link once it is up (bsd/dev/hakios.adsp.c starts this when it is told of it).
 *
 * Done so far: the two QMI requests -- select instance 0 with the DSP the bus's master, and power it up -- and
 * what the DSP answers. Next: the NGD itself (its message queues, the codec's REPORT_PRESENT, a logical
 * address), then the codec's reset (TLMM GPIO 64), its master clock (RPMh LN_BB_CLK2) and its registers.
 *
 * Boot-arg slim=1: the QMI part alone; slim=2: the NGD as well; slim=3: the codec's clock and reset too.
 * Without it nothing here runs.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.qrtr.h>
#define HZ                      1000
#include <sys/hakios.qmi.h>

extern const struct hakios_qrtr_ops *hakios_adsp_qrtr(void);           /* bsd/dev/hakios.adsp.c */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool hakios_adsp_rpmh_set(const char *name, uint32_t value);   /* bsd/dev/hakios.adsp.c */

/* Every line goes to the loader's log directly as well (as hakios.buttons.c's): this runs early in a boot. */
extern void loader_log_putc(char c);

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
#define printf say

/* ---- the DSP's SLIMbus service (Linux: qcom-ngd-ctrl.c, its QMI part) ------------------------------------- */

#define SLIMBUS_QMI_SVC_ID                      0x0301
#define SLIMBUS_QMI_SVC_V1                      1
#define SLIMBUS_QMI_INS_ID                      0
#define SLIMBUS_QMI_SELECT_INSTANCE_REQ_V01     0x0020
#define SLIMBUS_QMI_POWER_REQ_V01               0x0021
#define SLIMBUS_QMI_CHECK_FRAMER_STATUS_REQ     0x0022
#define SLIMBUS_QMI_POWER_REQ_MAX_MSG_LEN       14
#define SLIMBUS_QMI_SELECT_INSTANCE_REQ_MAX_MSG_LEN 14
#define SLIMBUS_QMI_RESP_TOUT                   1000

#define SLIMBUS_MODE_SATELLITE_V01              1
#define SLIMBUS_MODE_MASTER_V01                 2
#define SLIMBUS_PM_INACTIVE_V01                 1
#define SLIMBUS_PM_ACTIVE_V01                   2
#define SLIMBUS_RESP_SYNCHRONOUS_V01            1

struct slimbus_select_inst_req_msg_v01 {
	uint32_t instance;
	uint8_t mode_valid;
	uint32_t mode;
};

struct slimbus_select_inst_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct slimbus_power_req_msg_v01 {
	uint32_t pm_req;
	uint8_t resp_type_valid;
	uint32_t resp_type;
};

struct slimbus_power_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

static const struct qmi_elem_info slimbus_select_inst_req_msg_v01_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct slimbus_select_inst_req_msg_v01, instance) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct slimbus_select_inst_req_msg_v01, mode_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct slimbus_select_inst_req_msg_v01, mode) },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY },
};

static const struct qmi_elem_info slimbus_select_inst_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct slimbus_select_inst_resp_msg_v01, resp), .ei_array = qmi_response_type_v01_ei },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY },
};

static const struct qmi_elem_info slimbus_power_req_msg_v01_ei[] = {
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x01, .offset = offsetof(struct slimbus_power_req_msg_v01, pm_req) },
	{ .data_type = QMI_OPT_FLAG, .elem_len = 1, .elem_size = sizeof(uint8_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct slimbus_power_req_msg_v01, resp_type_valid) },
	{ .data_type = QMI_UNSIGNED_4_BYTE, .elem_len = 1, .elem_size = sizeof(uint32_t), .array_type = NO_ARRAY,
	  .tlv_type = 0x10, .offset = offsetof(struct slimbus_power_req_msg_v01, resp_type) },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY },
};

static const struct qmi_elem_info slimbus_power_resp_msg_v01_ei[] = {
	{ .data_type = QMI_STRUCT, .elem_len = 1, .elem_size = sizeof(struct qmi_response_type_v01), .array_type = NO_ARRAY,
	  .tlv_type = 0x02, .offset = offsetof(struct slimbus_power_resp_msg_v01, resp), .ei_array = qmi_response_type_v01_ei },
	{ .data_type = QMI_EOTI, .array_type = NO_ARRAY },
};

static struct qmi_handle slim_qmi;
static volatile bool slim_server_there;
static volatile uint32_t slim_server_node, slim_server_port;

/* From the DSP's link thread: the service is there (once for the one there was when it was asked). */
static int
slim_new_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	slim_server_node = service->node;
	slim_server_port = service->port;
	slim_server_there = true;
	return 0;
}

static void
slim_del_server(__unused struct qmi_handle *qmi, __unused struct qmi_service *service)
{
	slim_server_there = false;
}

static const struct qmi_ops slim_qmi_ops = { .new_server = slim_new_server, .del_server = slim_del_server };
static const struct qmi_msg_handler slim_qmi_handlers[] = { { 0 } };

/* One request and its answer: what the DSP said (its result and error), or a negative errno of ours. */
static int
slim_qmi_request(const char *what, int msg_id, size_t largest, const struct qmi_elem_info *req_ei, const void *req,
    const struct qmi_elem_info *resp_ei, struct qmi_response_type_v01 *resp)
{
	struct qmi_txn txn;
	int rc;

	rc = qmi_txn_init(&slim_qmi, &txn, resp_ei, resp);
	if (rc < 0) {
		printf("slim: qmi: %s: no transaction (%d)\n", what, rc);
		return rc;
	}
	rc = qmi_send_request(&slim_qmi, NULL, &txn, msg_id, largest, req_ei, req);
	if (rc < 0) {
		printf("slim: qmi: %s: not sent (%d)\n", what, rc);
		qmi_txn_cancel(&txn);
		return rc;
	}
	rc = qmi_txn_wait(&txn, SLIMBUS_QMI_RESP_TOUT);
	if (rc < 0) {
		printf("slim: qmi: %s: NO ANSWER (%d)\n", what, rc);
		return rc;
	}
	printf("slim: qmi: %s: the DSP answers result %u error %u%s\n", what, resp->result, resp->error,
	    resp->result == QMI_RESULT_SUCCESS_V01 ? ": DONE" : ": REFUSED");
	return resp->result == QMI_RESULT_SUCCESS_V01 ? 0 : EIO;
}

/* ---- the NGD (Linux: qcom-ngd-ctrl.c; its registers' register mode: the downstream slim-msm-ngd.c) ------------ */

/*
 * The controller (dt: slim@171c0000, 0x2c000) and in it NGD 1, 0x1000 on. Linux moves the NGD's messages
 * through the BAM (its message queues, NGD_CFG bits 1 and 2); with those off the NGD takes one message to
 * send in NGD_TX_MSG and holds one received in NGD_RX_MSG, and says so in NGD_INT_STAT -- the register mode
 * of Qualcomm's own driver, which needs no DMA. Polled here: no interrupt is taken.
 */
#define SLIM_CTRL_PHYS          0x171c0000ull
#define SLIM_CTRL_SIZE          0x2c000
#define SLIM_NGD                0x1000          /* NGD 1 (v2 and later: 0x1000 for an odd one) */
#define NGD_CFG                 0x0
#define NGD_CFG_ENABLE          (1u << 0)
#define NGD_CFG_RX_MSGQ_EN      (1u << 1)
#define NGD_CFG_TX_MSGQ_EN      (1u << 2)
#define NGD_STATUS              0x4
#define NGD_LADDR               (1u << 1)
#define NGD_RX_MSGQ_CFG         0x8
#define NGD_INT_EN              0x10
#define NGD_INT_STAT            0x14
#define NGD_INT_CLR             0x18
#define NGD_TX_MSG              0x30
#define NGD_RX_MSG              0x70
#define NGD_INT_RECFG_DONE      (1u << 24)
#define NGD_INT_TX_NACKED_2     (1u << 25)
#define NGD_INT_MSG_BUF_CONTE   (1u << 26)
#define NGD_INT_MSG_TX_INVAL    (1u << 27)
#define NGD_INT_IE_VE_CHG       (1u << 28)
#define NGD_INT_DEV_ERR         (1u << 29)
#define NGD_INT_RX_MSG_RCVD     (1u << 30)
#define NGD_INT_TX_MSG_SENT     (1u << 31)
#define DEF_NGD_INT_MASK        (NGD_INT_TX_NACKED_2 | NGD_INT_MSG_BUF_CONTE | NGD_INT_MSG_TX_INVAL | NGD_INT_IE_VE_CHG | \
	                         NGD_INT_DEV_ERR | NGD_INT_TX_MSG_SENT | NGD_INT_RX_MSG_RCVD)
#define SLIM_RX_MSGQ_TIMEOUT_VAL 0x10000

/* the messages (Linux: drivers/slimbus/slimbus.h; byte 0 MT[7:5] RL[4:0], byte 1 MC[6:0]) */
#define SLIM_MSG_MT_CORE                0x0
#define SLIM_MSG_MT_DEST_REFERRED_USER  0x2
#define SLIM_MSG_MT_SRC_REFERRED_USER   0x6
#define SLIM_MSG_MC_REPORT_PRESENT      0x1
#define SLIM_USR_MC_MASTER_CAPABILITY   0x0
#define SLIM_USR_MC_REPORT_SATELLITE    0x1
#define SLIM_LA_MGR                     0xff
#define SLIM_MSG_ASM_FIRST_WORD(l, mt, mc, dt, ad) ((uint32_t)(l) | (uint32_t)(mt) << 5 | (uint32_t)(mc) << 8 | \
	                                            (uint32_t)(dt) << 15 | (uint32_t)(ad) << 16)
#define SAT_MAGIC_LSB                   0xd9
#define SAT_MAGIC_MSB                   0xc5
#define SAT_MSG_VER                     0x1
#define SAT_MSG_PROT                    0x1

static volatile uint32_t *slim_ctrl, *slim_ngd;

static inline uint32_t
ngd_rd(uint32_t offset)
{
	return slim_ngd[offset / 4];
}

static inline void
ngd_wr(uint32_t offset, uint32_t value)
{
	slim_ngd[offset / 4] = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* A message received, taken out of NGD_RX_MSG (up to 40 bytes) and logged; its length, or 0 when there was none. */
static unsigned
ngd_receive(uint8_t *message)
{
	uint32_t words[10];
	unsigned length;

	if (!(ngd_rd(NGD_INT_STAT) & NGD_INT_RX_MSG_RCVD)) {
		return 0;
	}
	for (unsigned i = 0; i < 10; i++) {
		words[i] = ngd_rd(NGD_RX_MSG + 4 * i);
	}
	ngd_wr(NGD_INT_CLR, NGD_INT_RX_MSG_RCVD);
	memcpy(message, words, sizeof(words));
	length = (message[0] & 0x1f) + 1;
	if (length > 40) {
		length = 40;
	}
	printf("slim: ngd: received %u bytes, MT %u MC %#x:", length, message[0] >> 5, message[1] & 0x7f);
	for (unsigned i = 0; i < length; i++) {
		printf(" %02x", message[i]);
	}
	printf("\n");
	return length;
}

/* One message out through NGD_TX_MSG (words, the length's byte first); true once the NGD says it is sent. */
static bool
ngd_send(const uint8_t *message, unsigned length)
{
	uint32_t words[10] = { 0 }, stat = 0;

	memcpy(words, message, length);
	ngd_wr(NGD_INT_CLR, NGD_INT_TX_MSG_SENT | NGD_INT_TX_NACKED_2 | NGD_INT_MSG_TX_INVAL);
	for (unsigned i = 0; i < (length + 3) / 4; i++) {
		ngd_wr(NGD_TX_MSG + 4 * i, words[i]);
	}
	for (int i = 0; i < 1000; i++) {
		stat = ngd_rd(NGD_INT_STAT);
		if (stat & (NGD_INT_TX_MSG_SENT | NGD_INT_TX_NACKED_2 | NGD_INT_MSG_TX_INVAL)) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	ngd_wr(NGD_INT_CLR, stat & (NGD_INT_TX_MSG_SENT | NGD_INT_TX_NACKED_2 | NGD_INT_MSG_TX_INVAL));
	printf("slim: ngd: %u bytes sent: %s (int stat %#x)\n", length, (stat & NGD_INT_TX_MSG_SENT) ? "SENT" :
	    (stat & NGD_INT_TX_NACKED_2) ? "NACKED" : (stat & NGD_INT_MSG_TX_INVAL) ? "INVALID" : "NO ANSWER", stat);
	return (stat & NGD_INT_TX_MSG_SENT) != 0;
}

/*
 * Linux's qcom_slim_ngd_power_up() and the master's capability exchange (qcom_slim_ngd_master_worker()):
 * the interrupts' sources on (polled here), the NGD on in register mode, and to the DSP's MASTER_CAPABILITY
 * our REPORT_SATELLITE; then the logical address the master gives the NGD. Every message is logged.
 */
static void
ngd_bringup(void)
{
	uint8_t message[40];
	bool answered = false;
	uint32_t status;

	slim_ctrl = (volatile uint32_t *)ml_io_map(SLIM_CTRL_PHYS, SLIM_CTRL_SIZE);
	if (slim_ctrl == NULL) {
		printf("slim: ngd: the controller cannot be mapped\n");
		return;
	}
	slim_ngd = slim_ctrl + SLIM_NGD / 4;
	printf("slim: ngd: about to read the controller's version (%#llx: the audio block, the DSP's)\n", SLIM_CTRL_PHYS);
	printf("slim: ngd: the controller is version %#x; NGD 1: cfg %#x status %#x rx msgq cfg %#x int en %#x stat %#x\n",
	    slim_ctrl[0] >> 16, ngd_rd(NGD_CFG), ngd_rd(NGD_STATUS), ngd_rd(NGD_RX_MSGQ_CFG), ngd_rd(NGD_INT_EN), ngd_rd(NGD_INT_STAT));
	status = ngd_rd(NGD_STATUS);
	if (status & NGD_LADDR) {
		printf("slim: ngd: it has a logical address already (status %#x)\n", status);
	}
	ngd_wr(NGD_INT_EN, DEF_NGD_INT_MASK);
	ngd_wr(NGD_RX_MSGQ_CFG, ngd_rd(NGD_RX_MSGQ_CFG) | SLIM_RX_MSGQ_TIMEOUT_VAL);
	ngd_wr(NGD_CFG, (ngd_rd(NGD_CFG) & ~(NGD_CFG_RX_MSGQ_EN | NGD_CFG_TX_MSGQ_EN)) | NGD_CFG_ENABLE);
	printf("slim: ngd: ENABLED in register mode (cfg %#x); waiting for the master's capability\n", ngd_rd(NGD_CFG));
	for (int ms = 0; ms < 3000; ms++) {
		unsigned length = ngd_receive(message);

		if (length >= 2 && (message[0] >> 5) == SLIM_MSG_MT_SRC_REFERRED_USER &&
		    (message[1] & 0x7f) == SLIM_USR_MC_MASTER_CAPABILITY && !answered) {
			/* REPORT_SATELLITE to the manager: 3 bytes of header (the logical address's form), 4 of payload */
			uint32_t first = SLIM_MSG_ASM_FIRST_WORD(7, SLIM_MSG_MT_SRC_REFERRED_USER, SLIM_USR_MC_REPORT_SATELLITE, 0,
			    SLIM_LA_MGR);
			uint8_t reply[8] = { (uint8_t)first, (uint8_t)(first >> 8), (uint8_t)(first >> 16), SAT_MAGIC_LSB, SAT_MAGIC_MSB,
				             SAT_MSG_VER, SAT_MSG_PROT, 0 };

			printf("slim: ngd: THE MASTER'S CAPABILITY: answered with REPORT_SATELLITE\n");
			answered = ngd_send(reply, 7);
		}
		if (answered && (ngd_rd(NGD_STATUS) & NGD_LADDR)) {
			printf("slim: ngd: THE NGD HAS ITS LOGICAL ADDRESS (status %#x) after %d ms: the bus is ours to speak on\n",
			    ngd_rd(NGD_STATUS), ms);
			break;
		}
		{
			uint32_t stat = ngd_rd(NGD_INT_STAT) & ~(NGD_INT_RX_MSG_RCVD | NGD_INT_TX_MSG_SENT);

			if (stat != 0) {
				printf("slim: ngd: int stat %#x\n", stat);
				ngd_wr(NGD_INT_CLR, stat);
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	printf("slim: ngd: after the wait: cfg %#x status %#x int stat %#x%s\n", ngd_rd(NGD_CFG), ngd_rd(NGD_STATUS),
	    ngd_rd(NGD_INT_STAT), answered ? "" : "; NO CAPABILITY CAME");
}

/* ---- the codec: its clock, its reset, and what it says on the bus ------------------------------------------- */

/*
 * The WCD934x (dt: tavil_codec, qcom,tavil-slim-pgd): its supplies are all pm8998_s4, on always; its master clock
 * audio_ext_clk_lnbb, the PMIC's LN_BB_CLK2 (RPMh "lnbclka2", 19.2 MHz, which the codec divides to 9.6); its reset
 * TLMM GPIO 64 (north tile), low to hold it -- Linux's wcd934x.c drives it low, waits 20 ms, high, 20 ms. Out of
 * reset and clocked it reports itself present on the bus, two devices: its interface (EA 00 00 50 02 17 02) and
 * its generic device (00 01 50 02 17 02), whose registers are the codec's.
 */
#define TLMM_PHYS               0x03400000UL
#define TILE_NORTH              0x500000UL
#define WCD_RESET_GPIO          64
#define CFG_OUTPUT              (1u << 9)

static void
codec_up(void)
{
	volatile uint32_t *pin = (volatile uint32_t *)ml_io_map(TLMM_PHYS + TILE_NORTH + 0x1000UL * WCD_RESET_GPIO, 0x1000);
	uint8_t message[40];
	unsigned present = 0;

	if (pin == NULL) {
		printf("slim: codec: its reset pin cannot be mapped\n");
		return;
	}
	printf("slim: codec: its reset, gpio%u: cfg %#x, io %#x (bit 1: driven high, bit 0: reads)\n", WCD_RESET_GPIO, pin[0], pin[1]);
	if (!hakios_adsp_rpmh_set("lnbclka2", 1)) {
		printf("slim: codec: its clock is not on: it is not let out of reset\n");
		return;
	}
	pin[1] = 0;                                     /* low: held in reset */
	pin[0] = CFG_OUTPUT;                            /* function 0 (a GPIO), no pull, 2 mA, an output */
	__asm__ volatile ("dsb sy" ::: "memory");
	delay_for_interval(20, NSEC_PER_MSEC);
	pin[1] = 2;                                     /* high: out of reset */
	__asm__ volatile ("dsb sy" ::: "memory");
	delay_for_interval(20, NSEC_PER_MSEC);
	printf("slim: codec: OUT OF RESET (gpio%u cfg %#x io %#x), clocked; listening on the bus\n", WCD_RESET_GPIO, pin[0], pin[1]);
	for (int ms = 0; ms < 2000; ms++) {
		unsigned length = ngd_receive(message);

		if (length >= 8 && (message[0] >> 5) == SLIM_MSG_MT_CORE && (message[1] & 0x7f) == SLIM_MSG_MC_REPORT_PRESENT) {
			present++;
			printf("slim: codec: A DEVICE REPORTS PRESENT: enumeration address %02x %02x %02x %02x %02x %02x\n",
			    message[length - 6], message[length - 5], message[length - 4], message[length - 3], message[length - 2],
			    message[length - 1]);
		}
		{
			uint32_t stat = ngd_rd(NGD_INT_STAT) & ~(NGD_INT_RX_MSG_RCVD | NGD_INT_TX_MSG_SENT);

			if (stat != 0) {
				printf("slim: ngd: int stat %#x\n", stat);
				ngd_wr(NGD_INT_CLR, stat);
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	printf("slim: codec: %u reports of presence in 2 s%s\n", present, present ? "" : " (the master may keep them to itself: next, ask it)");
}

/* ---- the thread ---------------------------------------------------------------------------------------------- */

static void
slim_thread(__unused void *parameter, __unused wait_result_t result)
{
	const struct hakios_qrtr_ops *transport = hakios_adsp_qrtr();
	struct slimbus_select_inst_req_msg_v01 select = { .instance = 0, .mode_valid = 1, .mode = SLIMBUS_MODE_MASTER_V01 };
	struct slimbus_select_inst_resp_msg_v01 selected = { { 0, 0 } };
	struct slimbus_power_req_msg_v01 power = { .pm_req = SLIMBUS_PM_ACTIVE_V01, .resp_type_valid = 1,
		                                   .resp_type = SLIMBUS_RESP_SYNCHRONOUS_V01 };
	struct slimbus_power_resp_msg_v01 powered = { { 0, 0 } };
	int rc;

	if (transport == NULL) {
		printf("slim: the audio DSP's QRTR link is not up: nothing done\n");
		goto out;
	}
	rc = hakios_qmi_handle_init_transport(&slim_qmi, SLIMBUS_QMI_POWER_REQ_MAX_MSG_LEN + 64, &slim_qmi_ops, slim_qmi_handlers,
	    transport);
	if (rc < 0) {
		printf("slim: qmi: no handle on the DSP's link (%d)\n", rc);
		goto out;
	}
	rc = qmi_add_lookup(&slim_qmi, SLIMBUS_QMI_SVC_ID, SLIMBUS_QMI_SVC_V1, SLIMBUS_QMI_INS_ID);
	for (int i = 0; i < 500 && !slim_server_there && rc == 0; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	if (!slim_server_there) {
		printf("slim: the DSP's SLIMbus service (%u) is not found (%d)\n", SLIMBUS_QMI_SVC_ID, rc);
		goto release;
	}
	rc = hakios_qmi_connect(slim_qmi.sock, slim_server_node, slim_server_port);
	printf("slim: the DSP's SLIMbus service at node %u port %u%s\n", slim_server_node, slim_server_port,
	    rc == 0 ? "" : ": NOT CONNECTED");
	if (rc != 0) {
		goto release;
	}
	/* Linux's qcom_slim_qmi_init(ctrl, false): instance ngd->id >> 1 (the NGD is 1), the DSP the bus's master */
	if (slim_qmi_request("select instance 0, the DSP the master", SLIMBUS_QMI_SELECT_INSTANCE_REQ_V01,
	    SLIMBUS_QMI_SELECT_INSTANCE_REQ_MAX_MSG_LEN, slimbus_select_inst_req_msg_v01_ei, &select,
	    slimbus_select_inst_resp_msg_v01_ei, &selected.resp) != 0) {
		goto release;
	}
	/* Linux's qcom_slim_qmi_power_request(ctrl, true) */
	if (slim_qmi_request("power the bus up", SLIMBUS_QMI_POWER_REQ_V01, SLIMBUS_QMI_POWER_REQ_MAX_MSG_LEN,
	    slimbus_power_req_msg_v01_ei, &power, slimbus_power_resp_msg_v01_ei, &powered.resp) != 0) {
		goto release;
	}
	printf("slim: THE AUDIO SLIMBUS IS OURS AND POWERED (the DSP its master)\n");
	{
		uint32_t level = 0;

		PE_parse_boot_argn("slim", &level, sizeof(level));
		if (level >= 2) {
			ngd_bringup();
		}
		if (level >= 3 && slim_ngd != NULL && (ngd_rd(NGD_STATUS) & NGD_LADDR)) {
			codec_up();
		}
	}
	return;                                 /* (the handle stays: the bus is to be powered down through it one day) */
release:
	qmi_handle_release(&slim_qmi);
out:
	return;
}

/* bsd/dev/hakios.adsp.c, when the DSP tells of its SLIMbus service: the thread above, once, with slim=1. */
void
hakios_slim_start(void)
{
	static bool started;
	uint32_t wanted = 0;
	thread_t thread;

	if (started || !PE_parse_boot_argn("slim", &wanted, sizeof(wanted)) || wanted == 0) {
		return;
	}
	started = true;
	if (kernel_thread_start(slim_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("slim: no thread\n");
	}
}
