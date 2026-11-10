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
#include <sys/hakios.audio.h>
#include <sys/hakios.qrtr.h>
#define HZ                      1000
#include <sys/hakios.qmi.h>

extern const struct hakios_qrtr_ops *hakios_adsp_qrtr(void);           /* bsd/dev/hakios.adsp.c */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool hakios_adsp_rpmh_set(const char *name, uint32_t offset, uint32_t value);     /* bsd/dev/hakios.adsp.c */

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
slim_qmi_request_on(struct qmi_handle *handle, const char *what, int msg_id, size_t largest,
    const struct qmi_elem_info *req_ei, const void *req, const struct qmi_elem_info *resp_ei,
    struct qmi_response_type_v01 *resp)
{
	struct qmi_txn txn;
	int rc;

	rc = qmi_txn_init(handle, &txn, resp_ei, resp);
	if (rc < 0) {
		printf("slim: qmi: %s: no transaction (%d)\n", what, rc);
		return rc;
	}
	rc = qmi_send_request(handle, NULL, &txn, msg_id, largest, req_ei, req);
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
#define slim_qmi_request(...) slim_qmi_request_on(&slim_qmi, __VA_ARGS__)

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
#define SLIM_MSG_MC_REQUEST_VALUE       0x60
#define SLIM_MSG_MC_REPLY_VALUE         0x64
#define SLIM_MSG_MC_CHANGE_VALUE        0x68
#define SLIM_USR_MC_ADDR_QUERY          0xd
#define SLIM_USR_MC_ADDR_REPLY          0xe
#define SLIM_USR_MC_MASTER_CAPABILITY   0x0
#define SLIM_USR_MC_REPORT_SATELLITE    0x1
#define SLIM_USR_MC_DEF_ACT_CHAN        0x21
#define SLIM_USR_MC_RECONFIG_NOW        0x24
#define SLIM_USR_MC_GENERIC_ACK         0x25
#define SLIM_USR_MC_CONNECT_SRC         0x2c
#define SLIM_USR_MC_CONNECT_SINK        0x2d
#define SLIM_USR_MC_DISCONNECT_PORT     0x2e
#define SLIM_LA_MGR                     0xff
#define SLIM_MSG_ASM_FIRST_WORD(l, mt, mc, dt, ad) ((uint32_t)(l) | (uint32_t)(mt) << 5 | (uint32_t)(mc) << 8 | \
	                                            (uint32_t)(dt) << 15 | (uint32_t)(ad) << 16)
#define SAT_MAGIC_LSB                   0xd9
#define SAT_MAGIC_MSB                   0xc5
#define SAT_MSG_VER                     0x1
#define SAT_MSG_PROT                    0x1

static volatile uint32_t *slim_ctrl, *slim_ngd;
static bool ngd_quiet;          /* the codec's register traffic: only what goes wrong logged */

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
	if (ngd_quiet) {
		return length;
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
	if (ngd_quiet && (stat & NGD_INT_TX_MSG_SENT)) {
		return true;
	}
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

static uint8_t slim_tid = 1;

/* A message sent, then the reply of that MT and MC with that TID (at byte 3) waited for (1 s); its length, or 0. */
static unsigned
ngd_transact(const uint8_t *message, unsigned length, unsigned mt, unsigned mc, uint8_t tid, uint8_t *reply)
{
	if (!ngd_send(message, length)) {
		return 0;
	}
	for (int ms = 0; ms < 1000; ms++) {
		unsigned got = ngd_receive(reply);

		if (got >= 4 && (reply[0] >> 5) == mt && (reply[1] & 0x7f) == mc && reply[3] == tid) {
			return got;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	printf("slim: ngd: no reply (MT %u MC %#x TID %u) in 1 s\n", mt, mc, tid);
	return 0;
}

/* Linux's qcom_slim_ngd_get_laddr(): the manager asked which logical address the device of that EA has; 0 if none. */
static uint8_t
codec_laddr(const uint8_t ea[6], const char *what)
{
	uint8_t tid = slim_tid++, message[10], reply[40];
	uint32_t first = SLIM_MSG_ASM_FIRST_WORD(10, SLIM_MSG_MT_DEST_REFERRED_USER, SLIM_USR_MC_ADDR_QUERY, 0, SLIM_LA_MGR);
	unsigned got;

	message[0] = (uint8_t)first;
	message[1] = (uint8_t)(first >> 8);
	message[2] = (uint8_t)(first >> 16);
	message[3] = tid;
	memcpy(message + 4, ea, 6);
	got = ngd_transact(message, 10, SLIM_MSG_MT_SRC_REFERRED_USER, SLIM_USR_MC_ADDR_REPLY, tid, reply);
	if (got < 11) {
		printf("slim: codec: the manager does not say where the %s is\n", what);
		return 0;
	}
	if (memcmp(reply + 4, "\0\0\0\0\0\0", 6) == 0) {
		printf("slim: codec: the manager knows no %s (EA %02x %02x %02x %02x %02x %02x)\n", what, ea[0], ea[1], ea[2], ea[3], ea[4],
		    ea[5]);
		return 0;
	}
	printf("slim: codec: THE %s IS AT LOGICAL ADDRESS %#x\n", what, reply[10]);
	return reply[10];
}

/* One value element of a device written (CHANGE_VALUE: no reply); true when the NGD has sent it. */
static bool
codec_write(uint8_t la, uint32_t element, uint8_t value)
{
	uint16_t ec = (uint16_t)(0x8 | (element & 0xfff) << 4);        /* one byte (slice code 0), byte-addressed */
	uint32_t first = SLIM_MSG_ASM_FIRST_WORD(6, SLIM_MSG_MT_CORE, SLIM_MSG_MC_CHANGE_VALUE, 0, la);
	uint8_t message[6] = { (uint8_t)first, (uint8_t)(first >> 8), (uint8_t)(first >> 16), (uint8_t)ec, (uint8_t)(ec >> 8), value };

	return ngd_send(message, 6);
}

/* One value element read (REQUEST_VALUE, REPLY_VALUE); -1 if no answer. */
static int
codec_read(uint8_t la, uint32_t element)
{
	uint8_t tid = slim_tid++, reply[40];
	uint16_t ec = (uint16_t)(0x8 | (element & 0xfff) << 4);
	uint32_t first = SLIM_MSG_ASM_FIRST_WORD(6, SLIM_MSG_MT_CORE, SLIM_MSG_MC_REQUEST_VALUE, 0, la);
	uint8_t message[6] = { (uint8_t)first, (uint8_t)(first >> 8), (uint8_t)(first >> 16), tid, (uint8_t)ec, (uint8_t)(ec >> 8) };

	if (ngd_transact(message, 6, SLIM_MSG_MT_CORE, SLIM_MSG_MC_REPLY_VALUE, tid, reply) < 5) {
		return -1;
	}
	return reply[4];
}

/* The codec's registers by their 16-bit addresses: the page selected when it changes, then the window. */
static uint8_t codec_pgd, codec_ifd;
static int codec_page = -1;
static volatile bool codec_ready;       /* brought up: the earpiece can be asked for (hakios.sound.c) */

static bool
codec_page_select(unsigned reg)
{
	if (codec_page == (int)(reg >> 8)) {
		return true;
	}
	if (!codec_write(codec_pgd, 0x800, (uint8_t)(reg >> 8))) {
		codec_page = -1;
		return false;
	}
	codec_page = (int)(reg >> 8);
	return true;
}

static bool
codec_reg_write(unsigned reg, uint8_t value)
{
	return codec_page_select(reg) && codec_write(codec_pgd, 0x800 + (reg & 0xff), value);
}

static int
codec_reg_read(unsigned reg)
{
	return codec_page_select(reg) ? codec_read(codec_pgd, 0x800 + (reg & 0xff)) : -1;
}

/* Linux's wcd934x_bring_up(): its digital core reset, its internal supplies (SIDO) started, the core powered. */
static bool
codec_bring_up(void)
{
	static const struct {
		uint16_t reg;
		uint8_t value;
		uint8_t settle_ms;
	} steps[] = {
		{ 0x0009, 0x01, 0 },    /* CODEC_RPM_RST_CTL */
		{ 0x071b, 0x19, 0 },    /* SIDO_NEW_VOUT_A_STARTUP */
		{ 0x071c, 0x15, 1 },    /* SIDO_NEW_VOUT_D_STARTUP; 1 ms for VOUT to settle */
		{ 0x0011, 0x05, 0 },    /* CODEC_RPM_PWR_CDC_DIG_HM_CTL */
		{ 0x0011, 0x07, 0 },
		{ 0x0009, 0x03, 0 },
		{ 0x0009, 0x07, 0 },
		{ 0x0011, 0x03, 0 },
	};
	static const uint16_t shown[] = { 0x0009, 0x0011, 0x071b, 0x071c, 0x0003 };

	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		if (!codec_reg_write(steps[i].reg, steps[i].value)) {
			printf("slim: codec: bring-up write %u (%#06x = %#x) not sent\n", i, steps[i].reg, steps[i].value);
			return false;
		}
		if (steps[i].settle_ms) {
			delay_for_interval(steps[i].settle_ms, NSEC_PER_MSEC);
		}
	}
	for (unsigned i = 0; i < sizeof(shown) / sizeof(shown[0]); i++) {
		printf("slim: codec: register %#06x reads %#x\n", shown[i], codec_reg_read(shown[i]));
	}
	printf("slim: codec: BROUGHT UP (reset 0x7 and power 0x3 expected above)\n");
	return true;
}

/* Bits of a register changed, the rest kept. */
static bool
codec_update(unsigned reg, uint8_t mask, uint8_t value)
{
	int old = codec_reg_read(reg);

	if (old < 0 || !codec_reg_write(reg, (uint8_t)(((unsigned)old & ~(unsigned)mask) | (value & mask)))) {
		printf("slim: codec: register %#06x (mask %#x to %#x): NOT CHANGED\n", reg, mask, value);
		return false;
	}
	return true;
}

/* A register of the interface device (Linux's if_regmap: windows of 0x400 at 0x800, the selector at 0x800). */
static bool
codec_ifd_write(unsigned reg, uint8_t value)
{
	return codec_write(codec_ifd, 0x800, (uint8_t)(reg / 0x400)) && codec_write(codec_ifd, 0x800 + reg % 0x400, value);
}

struct codec_step {
	uint16_t reg;
	uint8_t mask, value;
	uint16_t settle_us;
};

static bool
codec_steps(const char *what, const struct codec_step *steps, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		if (!codec_update(steps[i].reg, steps[i].mask, steps[i].value)) {
			printf("slim: codec: %s: STOPPED at step %u\n", what, i);
			return false;
		}
		if (steps[i].settle_us) {
			delay_for_interval(steps[i].settle_us, NSEC_PER_USEC);
		}
	}
	return true;
}

/*
 * The earpiece's path in the codec, as Linux's DAPM powers it for SLIM RX0 -> RX INT0 -> EAR (sound/soc/codecs/
 * wcd934x.c, wcd-clsh-v2.c), in that order: the master clock and analog bias (wcd934x_enable_ana_bias_and_sysclk(),
 * the 19.2 MHz MCLK halved to the 9.6 MHz the machine driver asks for), the efuse sensed (wcd934x_comp_init()), the
 * supplies (RX_BIAS, INT0_CLK, DSMDEM0_CLK), the muxes (INT0_1 MIX1 INP0 = RX0, DEM = CLSH_DSM_OUT with its look-ahead
 * delay), the EAR DAC's class-H (wcd_clsh_state_ear(), CLS_H_NORMAL), and the EAR PA.
 */
static const struct codec_step ear_clock[] = {
	{ 0x0003, 0x03, 0x01, 0 },      /* CODEC_RPM_CLK_MCLK_CFG: 9.6 MHz */
	{ 0x0601, 0x80, 0x80, 0 },      /* ANA_BIAS: on */
	{ 0x0601, 0x40, 0x40, 1000 },   /* pre-charge, 1 ms */
	{ 0x0601, 0x40, 0x00, 0 },
	{ 0x0601, 0x20, 0x00, 0 },
	{ 0x0711, 0x80, 0x80, 0 },      /* CLK_SYS_MCLK_PRG: the external clock's buffer */
	{ 0x0711, 0x30, 0x10, 0 },      /* divided by 2 */
	{ 0x0711, 0x02, 0x00, 0 },      /* the source: the external clock */
	{ 0x0711, 0x01, 0x01, 0 },      /* on */
	{ 0x0d42, 0x01, 0x01, 0 },      /* CDC_CLK_RST_CTRL_FS_CNT_CONTROL */
	{ 0x0d41, 0x01, 0x01, 0 },      /* CDC_CLK_RST_CTRL_MCLK_CONTROL */
	{ 0x0002, 0x03, 0x00, 15 },     /* CODEC_RPM_CLK_GATE: ungated */
	{ 0x0603, 0x80, 0x80, 110 },    /* ANA_RCO: the bandgap for SIDO */
	{ 0x0025, 0x1e, 0x10, 0 },      /* CHIP_TIER_CTRL_EFUSE_CTL: sense state */
	{ 0x0025, 0x01, 0x01, 5000 },   /* sensing, 5 ms */
}, ear_path[] = {
	{ 0x0608, 0x01, 0x01, 0 },      /* ANA_RX_SUPPLIES: RX_BIAS */
	{ 0x0b41, 0x20, 0x20, 0 },      /* CDC_RX0_RX_PATH_CTL: INT0's clock */
	{ 0x0b53, 0x01, 0x01, 0 },      /* CDC_RX0_RX_PATH_DSMDEM_CTL: DSMDEM0's clock */
	{ 0x0d01, 0x0f, 0x05, 0 },      /* CDC_RX_INP_MUX_RX_INT0_CFG0: INP0 = RX0 */
	{ 0x0b42, 0x08, 0x08, 0 },      /* CDC_RX0_RX_PATH_CFG0: the look-ahead delay (CLSH_DSM_OUT) */
	{ 0x0b49, 0x01, 0x01, 0 },      /* CDC_RX0_RX_PATH_SEC0: DEM = CLSH_DSM_OUT */
	{ 0x073a, 0x02, 0x00, 0 },      /* HPH_NEW_INT_HPH_TIMER1: no auto-chop while powering up */
	{ 0x0c01, 0x01, 0x01, 0 },      /* CDC_CLSH_CRC: class-H's clock */
	{ 0x0b42, 0x40, 0x40, 0 },      /* CDC_RX0_RX_PATH_CFG0: class-H on */
	{ 0x0608, 0x08, 0x00, 0 },      /* ANA_RX_SUPPLIES: buck, normal power */
	{ 0x0608, 0x04, 0x00, 0 },      /* flyback, normal power */
	{ 0x0608, 0x40, 0x40, 600 },    /* flyback (VNEG) on */
	{ 0x06c7, 0x0f, 0x0a, 0 },      /* RX_BIAS_FLYB_BUFF: VPOS 5 uA x 10 */
	{ 0x06c7, 0xf0, 0x00, 100 },    /* VNEG: Linux's 0x0a under the mask 0xf0 */
	{ 0x0608, 0x80, 0x80, 500 },    /* buck (VPOS) on */
	{ 0x060a, 0x80, 0x80, 0 },      /* ANA_EAR: the EAR PA on */
}, clock_off[] = {
	{ 0x0711, 0x81, 0x00, 0 },      /* the master clock off */
	{ 0x0601, 0xc0, 0x00, 0 },      /* the bias off */
}, ear_off[] = {
	{ 0x060a, 0x80, 0x00, 0 },      /* the EAR PA off */
	{ 0x0b42, 0x40, 0x00, 0 },      /* class-H off */
	{ 0x0c01, 0x01, 0x00, 0 },
	{ 0x0608, 0x80, 0x00, 500 },    /* buck off */
	{ 0x0608, 0x40, 0x00, 600 },    /* flyback off */
	{ 0x0b53, 0x01, 0x00, 0 },
	{ 0x0b41, 0x20, 0x00, 0 },
	{ 0x0608, 0x01, 0x00, 0 },
};

/*
 * The handset microphone, as Android's "handset-mic" path has it (mixer_paths_tavil.xml: amic4) and Linux's DAPM
 * powers it: MIC BIAS1 (the tree's routing: AMIC4 from MIC BIAS1; qcom,cdc-micbias1-mv 2700: (2700 - 1000) / 50),
 * ADC4 with its TX hold (wcd934x_codec_enable_adc()), AMIC4_5 SEL = AMIC4, AMIC MUX0 = ADC4, ADC MUX0 = AMIC (and
 * the decimator's input an ADC, wcd934x_dec_enum_put()), CDC_IF TX0 MUX = DEC0, decimator 0 at 48 kHz
 * (wcd934x_set_decimator_rate()) enabled, its high-pass at 150 Hz and its gain written again (wcd934x_codec_enable_dec()).
 */
static const struct codec_step mic_path[] = {
	{ 0x0622, 0x3f, 0x22, 0 },      /* ANA_MICB1: 2.7 V */
	{ 0x0622, 0xc0, 0x40, 1000 },   /* MIC BIAS1 on */
	{ 0x0a31, 0x0f, 0x04, 0 },      /* CDC_TX0_TX_PATH_CTL: 48 kHz */
	{ 0x0d3a, 0x03, 0x02, 0 },      /* CDC_IF_ROUTER_TX_MUX_CFG0: TX0 = DEC0 */
	{ 0x0727, 0x80, 0x00, 0 },      /* TX_NEW_AMIC_4_5_SEL: AMIC4 */
	{ 0x0d1d, 0x07, 0x04, 0 },      /* CDC_TX_INP_MUX_ADC_MUX0_CFG0: AMIC MUX0 = ADC4 */
	{ 0x0a32, 0x80, 0x00, 0 },      /* CDC_TX0_TX_PATH_CFG0: the decimator's input an ADC */
	{ 0x0d1e, 0x03, 0x01, 0 },      /* CDC_TX_INP_MUX_ADC_MUX0_CFG1: ADC MUX0 = AMIC */
	{ 0x0611, 0x20, 0x20, 0 },      /* ANA_AMIC4: the TX hold */
	{ 0x0611, 0x80, 0x80, 0 },      /* ADC4 on */
	{ 0x0a32, 0x06, 0x00, 0 },      /* the decimator's power level: the default */
	{ 0x0a31, 0x20, 0x20, 0 },      /* decimator 0 on */
	{ 0x0a32, 0x60, 0x40, 0 },      /* its high-pass: 150 Hz */
	{ 0x0a39, 0x02, 0x02, 1000 },   /* CDC_TX0_TX_PATH_SEC2: the cut-off's change asked for */
	{ 0x0a39, 0x02, 0x00, 0 },
	{ 0x0a34, 0x00, 0x00, 0 },      /* CDC_TX0_TX_VOL_CTL: written again */
}, mic_off[] = {
	{ 0x0a31, 0x20, 0x00, 0 },      /* decimator 0 off */
	{ 0x0a31, 0x10, 0x00, 0 },
	{ 0x0611, 0x80, 0x00, 0 },      /* ADC4 off */
	{ 0x0622, 0xc0, 0x00, 0 },      /* MIC BIAS1 off */
};

static LCK_GRP_DECLARE(codec_lock_group, "hakios-slim");
static LCK_MTX_DECLARE(codec_lock, &codec_lock_group);  /* the bus and the codec: the earpiece's thread, the microphone's */
static unsigned clock_users;

static bool
codec_clock(bool on)
{
	bool ok = true;

	if (on && clock_users++ == 0) {
		ok = codec_steps("the master clock", ear_clock, sizeof(ear_clock) / sizeof(ear_clock[0]));
		printf("slim: codec: the master clock %s; efuse %#x\n", ok ? "ON" : "NOT ON", codec_reg_read(0x0039));
	} else if (!on && clock_users > 0 && --clock_users == 0) {
		ok = codec_steps("the master clock off", clock_off, sizeof(clock_off) / sizeof(clock_off[0]));
	}
	return ok;
}

/* A user message to the manager (Linux's NGD rewrites the core ones so) and its GENERIC_ACK; payload ends where told. */
static bool
ngd_ask_manager(const char *what, uint8_t mc, uint8_t *payload, unsigned length, uint8_t tid)
{
	uint8_t message[40], reply[40];
	uint32_t first = SLIM_MSG_ASM_FIRST_WORD(3 + length, SLIM_MSG_MT_DEST_REFERRED_USER, mc, 0, SLIM_LA_MGR);
	unsigned got;

	message[0] = (uint8_t)first;
	message[1] = (uint8_t)(first >> 8);
	message[2] = (uint8_t)(first >> 16);
	memcpy(message + 3, payload, length);
	got = ngd_transact(message, 3 + length, SLIM_MSG_MT_SRC_REFERRED_USER, SLIM_USR_MC_GENERIC_ACK, tid, reply);
	printf("slim: stream: %s: %s", what, got ? "ACKNOWLEDGED (" : "NO ACKNOWLEDGEMENT\n");
	for (unsigned i = 4; got && i < got; i++) {
		printf("%s%02x", i == 4 ? "" : " ", reply[i]);
	}
	printf(got ? ")\n" : "");
	return got != 0;
}

#define EAR_PORT        16              /* the codec's RX0: its port 16 (WCD934X_RX_START + 0) */
#define EAR_CHANNEL     144             /* the machine driver's first RX channel (sdm845.c) */

/*
 * The channel from the DSP's SLIMBUS_0_RX to the codec's port 16 (Linux: the codec's hw_params and trigger,
 * slim_stream_prepare() and qcom_slim_ngd_enable_stream()): the port's slot in the interface device, then the
 * manager asked to connect it as a sink of channel 144 and to define and activate that channel -- 16 bits (4
 * slots), 48 kHz as 3 x 2^2 of the 4 kHz superframe rate (coefficient 3, exponent 2), isochronous, fixed --
 * and to reconfigure now.
 */
static bool
ear_channel(bool on)
{
	uint8_t tid, payload[8];

	if (!on) {
		tid = slim_tid++;
		payload[0] = codec_pgd;
		payload[1] = EAR_PORT;
		payload[2] = tid;
		return ngd_ask_manager("port 16 disconnected", SLIM_USR_MC_DISCONNECT_PORT, payload, 3, tid);
	}
	if (!codec_ifd_write(0x140 + 4 * EAR_PORT, 0x01) ||   /* SLIM_PGD_RX_PORT_MULTI_CHNL_0(16): RX0 */
	    !codec_ifd_write(0x30 + EAR_PORT, 0x05)) {         /* SLIM_PGD_RX_PORT_CFG(16): watermark 12 bytes, enabled */
		printf("slim: stream: the interface device's port 16: NOT SET\n");
		return false;
	}
	tid = slim_tid++;
	payload[0] = codec_pgd;
	payload[1] = EAR_PORT;
	payload[2] = EAR_CHANNEL;
	payload[3] = tid;
	if (!ngd_ask_manager("port 16 a sink of channel 144", SLIM_USR_MC_CONNECT_SINK, payload, 4, tid)) {
		return false;
	}
	tid = slim_tid++;
	payload[0] = codec_pgd;
	payload[1] = 16 >> 2 | 1 << 5;          /* 16 bits; coefficient 3 */
	payload[2] = 2 << 4 | 0;                /* exponent 2; isochronous */
	payload[3] = 3 | 0x80;                  /* 48 kHz (presence rate 3); fixed content */
	payload[4] = tid;
	payload[5] = EAR_CHANNEL;
	if (!ngd_ask_manager("channel 144 defined and activated", SLIM_USR_MC_DEF_ACT_CHAN, payload, 6, tid)) {
		return false;
	}
	tid = slim_tid++;
	payload[0] = tid;
	payload[1] = codec_pgd;
	return ngd_ask_manager("reconfigure now", SLIM_USR_MC_RECONFIG_NOW, payload, 2, tid);
}

#define MIC_PORT_TX     0               /* the codec's TX0: its port 0 */
#define MIC_CHANNEL     128             /* the machine driver's first TX channel (sdm845.c) */

/* The channel from the codec's port 0 to the DSP's SLIMBUS_0_TX: as ear_channel(), the port a source. */
static bool
mic_channel(bool on)
{
	uint8_t tid, payload[8];

	if (!on) {
		tid = slim_tid++;
		payload[0] = codec_pgd;
		payload[1] = MIC_PORT_TX;
		payload[2] = tid;
		return ngd_ask_manager("port 0 disconnected", SLIM_USR_MC_DISCONNECT_PORT, payload, 3, tid);
	}
	if (!codec_ifd_write(0x100 + 4 * MIC_PORT_TX, 0x01) ||  /* SLIM_PGD_TX_PORT_MULTI_CHNL_0(0): TX0 */
	    !codec_ifd_write(0x101 + 4 * MIC_PORT_TX, 0x00) ||  /* SLIM_PGD_TX_PORT_MULTI_CHNL_1(0) */
	    !codec_ifd_write(0x50 + MIC_PORT_TX, 0x05)) {       /* SLIM_PGD_TX_PORT_CFG(0): watermark 12 bytes, enabled */
		printf("slim: stream: the interface device's port 0: NOT SET\n");
		return false;
	}
	tid = slim_tid++;
	payload[0] = codec_pgd;
	payload[1] = MIC_PORT_TX;
	payload[2] = MIC_CHANNEL;
	payload[3] = tid;
	if (!ngd_ask_manager("port 0 a source of channel 128", SLIM_USR_MC_CONNECT_SRC, payload, 4, tid)) {
		return false;
	}
	tid = slim_tid++;
	payload[0] = codec_pgd;
	payload[1] = 16 >> 2 | 1 << 5;
	payload[2] = 2 << 4 | 0;
	payload[3] = 3 | 0x80;
	payload[4] = tid;
	payload[5] = MIC_CHANNEL;
	if (!ngd_ask_manager("channel 128 defined and activated", SLIM_USR_MC_DEF_ACT_CHAN, payload, 6, tid)) {
		return false;
	}
	tid = slim_tid++;
	payload[0] = tid;
	payload[1] = codec_pgd;
	return ngd_ask_manager("reconfigure now", SLIM_USR_MC_RECONFIG_NOW, payload, 2, tid);
}

/* hakios.mic.c, the AFE's SLIMBUS_0_TX started (on) or about to stop (off): the codec's microphone path and the channel. */
bool
hakios_slim_mic(bool on)
{
	bool ok;

	if (!codec_ready) {
		return false;
	}
	lck_mtx_lock(&codec_lock);
	ngd_quiet = true;
	if (on) {
		ok = codec_clock(true) && codec_steps("the microphone's path", mic_path, sizeof(mic_path) / sizeof(mic_path[0]));
		printf("slim: codec: the microphone's path %s: micb1 %#x amic4 %#x tx0 ctl %#x cfg0 %#x vol %#x, adc mux %#x %#x, if tx %#x\n",
		    ok ? "UP" : "NOT UP", codec_reg_read(0x0622), codec_reg_read(0x0611), codec_reg_read(0x0a31), codec_reg_read(0x0a32),
		    codec_reg_read(0x0a34), codec_reg_read(0x0d1d), codec_reg_read(0x0d1e), codec_reg_read(0x0d3a));
		ngd_quiet = false;
		ok = ok && mic_channel(true);
		printf("slim: THE MICROPHONE %s\n", ok ? "FEEDS THE DSP: AMIC4 -> ADC4 -> DEC0 -> TX0 -> channel 128 -> SLIMBUS_0_TX" :
		    "DOES NOT FEED THE DSP");
		lck_mtx_unlock(&codec_lock);
		return ok;
	}
	ngd_quiet = false;
	ok = mic_channel(false);
	ngd_quiet = true;
	ok = codec_steps("the microphone off", mic_off, sizeof(mic_off) / sizeof(mic_off[0])) && ok;
	ok = codec_clock(false) && ok;
	ngd_quiet = false;
	printf("slim: the microphone %s\n", ok ? "OFF" : "NOT ALL OFF");
	lck_mtx_unlock(&codec_lock);
	return ok;
}

/* hakios.sound.c: the codec brought up, waited for. */
bool
hakios_slim_codec_ready(uint32_t ms)
{
	for (uint32_t i = 0; i < ms / 10 && !codec_ready; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	return codec_ready;
}

/* hakios.sound.c, the AFE's SLIMBUS_0_RX started (on) or about to stop (off): the codec's path and the channel. */
bool
hakios_slim_earpiece(bool on)
{
	bool ok;

	if (!codec_ready) {
		return false;
	}
	lck_mtx_lock(&codec_lock);
	ngd_quiet = true;
	if (on) {
		ok = codec_clock(true);
		ok = ok && codec_steps("the earpiece's path", ear_path, sizeof(ear_path) / sizeof(ear_path[0]));
		/* (Linux's main path: the digital volume written again once the interpolator runs) */
		ok = ok && codec_update(0x0b45, 0, 0);
		printf("slim: codec: the earpiece's path %s: rx0 ctl %#x cfg0 %#x vol %#x sec0 %#x, supplies %#x, ear %#x, int0 mux %#x\n",
		    ok ? "UP" : "NOT UP", codec_reg_read(0x0b41), codec_reg_read(0x0b42), codec_reg_read(0x0b45), codec_reg_read(0x0b49),
		    codec_reg_read(0x0608), codec_reg_read(0x060a), codec_reg_read(0x0d01));
		ngd_quiet = false;
		ok = ok && ear_channel(true);
		printf("slim: THE EARPIECE %s\n", ok ? "IS FED: SLIMBUS_0_RX -> channel 144 -> RX0 -> INT0 -> EAR" : "IS NOT FED");
		lck_mtx_unlock(&codec_lock);
		return ok;
	}
	ngd_quiet = false;
	ok = ear_channel(false);
	ngd_quiet = true;
	ok = codec_steps("the earpiece off", ear_off, sizeof(ear_off) / sizeof(ear_off[0])) && ok;
	ok = codec_clock(false) && ok;
	ngd_quiet = false;
	printf("slim: the earpiece %s\n", ok ? "OFF" : "NOT ALL OFF");
	lck_mtx_unlock(&codec_lock);
	return ok;
}

/*
 * The codec's registers (Linux: drivers/mfd/wcd934x.c, its regmap over SLIMbus): 16-bit addresses through a window
 * -- the page (address >> 8) into the selector at value element 0x800, the register at 0x800 + its low byte. Its
 * identity first, as wcd934x_bring_up() reads it: CHIP_ID bytes 0..3 (0x21..0x24).
 */
static void
codec_talk(void)
{
	static const uint8_t pgd_ea[6] = { 0x00, 0x01, 0x50, 0x02, 0x17, 0x02 }, ifd_ea[6] = { 0x00, 0x00, 0x50, 0x02, 0x17, 0x02 };
	uint8_t pgd = codec_laddr(pgd_ea, "codec's generic device"), id[4];
	int value;

	codec_ifd = codec_laddr(ifd_ea, "codec's interface");
	if (pgd == 0) {
		return;
	}
	if (!codec_write(pgd, 0x800, 0)) {
		printf("slim: codec: page 0 not selected\n");
		return;
	}
	for (unsigned i = 0; i < 4; i++) {
		if ((value = codec_read(pgd, 0x800 + 0x21 + i)) < 0) {
			printf("slim: codec: CHIP_ID byte %u: no answer\n", i);
			return;
		}
		id[i] = (uint8_t)value;
	}
	printf("slim: codec: THE WCD934x ANSWERS: chip id major %#x, minor %#x (bytes %02x %02x %02x %02x)\n", id[2] | id[3] << 8,
	    id[0] | id[1] << 8, id[0], id[1], id[2], id[3]);
	codec_pgd = pgd;
	codec_page = 0;
	codec_ready = codec_bring_up() && codec_ifd != 0;
}

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
	if (!hakios_adsp_rpmh_set("lnbclka2", 4, 1)) {         /* (its enable: Linux's CLK_RPMH_VRM_EN_OFFSET) */
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
	printf("slim: codec: %u reports of presence in 2 s%s\n", present, present ? "" : " (the master keeps them: asked below)");
	codec_talk();
}

/* ---- the Bluetooth SLIMbus: the WCN3990's voice (btslim=N) ---------------------------------------------------- */

/*
 * The second SLIMbus controller (dt: slim@17240000, cell-index 3, its BAM 0x17204000), on which the WCN3990's
 * Bluetooth half is a device (qcom,btfmslim_slave, EA 00 01 20 02 17 02): Android's btfm_slim driver sends a call's
 * voice over it to and from the audio DSP (AFE SLIMBUS_7_RX/TX, channels 157 and 159), and the chip gives none of it
 * back over HCI (bsd/dev/hakios.bt6t.c, runs 190111..195751). Downstream's slim-msm-ngd.c for it: QMI select instance
 * ctrl.nr >> 1 = 1 on the same DSP service, NGD_BASE_V2(3) = 0x1000. References: ~/oneplus6t-ref/bt-slimbus.
 *
 * btslim=1: the QMI part (instance 1, powered); btslim=2: its NGD too, a logical address, where the chip is and its
 * hardware revision (CHRK_SB_SLAVE_HW_REV_MSB/LSB). Run after the codec's bus is up; the NGD helpers above are pointed
 * at this controller under codec_lock and back after.
 */
#define BTSLIM_CTRL_PHYS        0x17240000ull
#define BTSLIM_QMI_INSTANCE     1
#define CHRK_SB_SLAVE_HW_REV_MSB 0x1
#define CHRK_SB_SLAVE_HW_REV_LSB 0x2
/*
 * The chip's registers (btfm_slim_wcn3990.h), value elements 0x800 on of its INTERFACE device (EA 00 00 20 02 17 02,
 * dt qcom,btfm-slim-ifd-elemental-addr), unpaged -- unlike the codec's IFD, whose element 0x800 is a page register.
 */
#define CHRK_ELEMENT(reg)               (0x800u + (reg))
#define CHRK_SB_PGD_PORT_RX_CFGN(n)     (0x40 + (n))
#define CHRK_SB_PGD_PORT_TX_CFGN(n)     (0x50 + (n))
#define CHRK_SB_PGD_TX_PORTn_MULTI_CHNL_0(n) (0x100 + 4 * (n))
#define CHRK_SB_PGD_RX_PORTn_MULTI_CHNL_0(n) (0x180 + 4 * (n))
#define CHRK_SB_PGD_PORT_TX_OR_UR_CFGN(n) (0x1f0 + (n))
#define CHRK_PORT_ENABLE                0x1
#define CHRK_PORT_WM_L1                 (0x1 << 1)
#define CHRK_PORT_WM_LB                 (0xb << 1)
#define CHRK_PORT_TX_SCO                0       /* the headset's microphone, to the DSP: channel 159 */
#define CHRK_PORT_RX_SCO                16      /* to the headset, from the DSP: channel 157 */
#define BTSLIM_CH_RX                    157     /* (sdm845.c msm_wcn_init: rx {157, 158}, tx {159, 160, 161}) */
#define BTSLIM_CH_TX                    159

static struct qmi_handle btslim_qmi;
static volatile bool btslim_server_there;
static volatile uint32_t btslim_server_node, btslim_server_port;
static volatile uint32_t *btslim_ctrl;

static int
btslim_new_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	btslim_server_node = service->node;
	btslim_server_port = service->port;
	btslim_server_there = true;
	return 0;
}

static void
btslim_del_server(__unused struct qmi_handle *qmi, __unused struct qmi_service *service)
{
	btslim_server_there = false;
}

static const struct qmi_ops btslim_qmi_ops = { .new_server = btslim_new_server, .del_server = btslim_del_server };

/* the NGD helpers on the Bluetooth controller for a while (codec_lock held by the caller) */
static volatile uint32_t *btslim_saved_ctrl, *btslim_saved_ngd;

static void
btslim_enter(void)
{
	btslim_saved_ctrl = slim_ctrl;
	btslim_saved_ngd = slim_ngd;
	slim_ctrl = btslim_ctrl;
	slim_ngd = btslim_ctrl + SLIM_NGD / 4;
}

static void
btslim_leave(void)
{
	slim_ctrl = btslim_saved_ctrl;
	slim_ngd = btslim_saved_ngd;
}

static uint32_t btslim_level;

/* the manager's GENERIC_ACK for one request (ngd_ask_manager), its name prefixed for this bus */
static bool
btslim_sco_ports(uint8_t ifd, bool on)
{
	bool ok = true;

	if (on) {
		ok = codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_TX_PORTn_MULTI_CHNL_0(CHRK_PORT_TX_SCO)), 1u << CHRK_PORT_TX_SCO) &&
		    codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_TX_OR_UR_CFGN(CHRK_PORT_TX_SCO)), 0x3) &&
		    codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_TX_CFGN(CHRK_PORT_TX_SCO)), CHRK_PORT_ENABLE | CHRK_PORT_WM_L1) &&
		    codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_RX_PORTn_MULTI_CHNL_0(CHRK_PORT_RX_SCO - 16)), 1u << 0) &&
		    codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_RX_CFGN(CHRK_PORT_RX_SCO - 16)), CHRK_PORT_ENABLE | CHRK_PORT_WM_LB);
	} else {
		ok = codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_TX_CFGN(CHRK_PORT_TX_SCO)), 0) &&
		    codec_write(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_RX_CFGN(CHRK_PORT_RX_SCO - 16)), 0);
	}
	printf("btslim: the chip's SCO ports (TX 0, RX 16) %s: %s\n", on ? "set up and enabled" : "disabled", ok ? "sent" : "NOT SENT");
	if (on) {
		int tx = codec_read(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_TX_CFGN(CHRK_PORT_TX_SCO)));
		int rx = codec_read(ifd, CHRK_ELEMENT(CHRK_SB_PGD_PORT_RX_CFGN(CHRK_PORT_RX_SCO - 16)));

		printf("btslim: read back: TX 0 cfg %#x (want %#x), RX 16 cfg %#x (want %#x)\n", tx, CHRK_PORT_ENABLE | CHRK_PORT_WM_L1,
		    rx, CHRK_PORT_ENABLE | CHRK_PORT_WM_LB);
	}
	return ok;
}

/*
 * btslim=3: the SCO paths as Android's btfm_slim_enable_ch() sets them for a call at 8 kHz -- the chip's ports, TX 0 the
 * source of channel 159 and RX 16 the sink of channel 157, both channels defined (16-bit LPCM, isochronous, rate code
 * 0x11 = 8000, Linux's slim_get_prate_code) and activated -- held 3 s and taken down again. The DSP's side is not there
 * yet (step 3): what this shows is whether the chip and the bus's manager take it.
 */
static void
btslim_sco_test(uint8_t pgd)
{
	static const uint8_t chip_ifd_ea[6] = { 0x00, 0x00, 0x20, 0x02, 0x17, 0x02 };
	uint8_t ifd = codec_laddr(chip_ifd_ea, "WCN3990's Bluetooth interface"), tid, payload[8];
	int msb, lsb;

	if (ifd == 0) {
		return;
	}
	msb = codec_read(ifd, CHRK_ELEMENT(CHRK_SB_SLAVE_HW_REV_MSB));
	lsb = codec_read(ifd, CHRK_ELEMENT(CHRK_SB_SLAVE_HW_REV_LSB));
	printf("btslim: the chip's interface at %#x: hardware revision major %d minor %d step %d\n", ifd,
	    msb >= 0 ? (msb & 0xf0) >> 4 : -1, msb >= 0 ? msb & 0xf : -1, lsb);
	if (!btslim_sco_ports(ifd, true)) {
		return;
	}
	tid = slim_tid++;
	payload[0] = pgd; payload[1] = CHRK_PORT_TX_SCO; payload[2] = BTSLIM_CH_TX; payload[3] = tid;
	(void)ngd_ask_manager("btslim: port TX 0 the source of channel 159", SLIM_USR_MC_CONNECT_SRC, payload, 4, tid);
	tid = slim_tid++;
	payload[0] = pgd; payload[1] = CHRK_PORT_RX_SCO; payload[2] = BTSLIM_CH_RX; payload[3] = tid;
	(void)ngd_ask_manager("btslim: port RX 16 the sink of channel 157", SLIM_USR_MC_CONNECT_SINK, payload, 4, tid);
	/* DEF_ACT_CHAN (Linux qcom_slim_ngd_enable_stream): 8 kHz -> ratem 2, segment interval 1536/2 = 768, a multiple of 3 */
	tid = slim_tid++;
	payload[0] = pgd;
	payload[1] = 16 >> 2;                   /* 16 bits a sample; segment interval a multiple of 3: bit 5 clear */
	payload[2] = 0 << 4 | 0;                /* exponent 0, isochronous */
	payload[3] = 0x11 | 0x80;               /* 8000 Hz, channel content fixed */
	payload[4] = tid;
	payload[5] = BTSLIM_CH_TX;
	payload[6] = BTSLIM_CH_RX;
	(void)ngd_ask_manager("btslim: channels 159 and 157 defined and activated", SLIM_USR_MC_DEF_ACT_CHAN, payload, 7, tid);
	tid = slim_tid++;
	payload[0] = tid;
	payload[1] = pgd;
	(void)ngd_ask_manager("btslim: reconfigure now", SLIM_USR_MC_RECONFIG_NOW, payload, 2, tid);
	delay_for_interval(3000, NSEC_PER_MSEC);
	for (int port = 0; port < 2; port++) {
		tid = slim_tid++;
		payload[0] = pgd; payload[1] = port == 0 ? CHRK_PORT_TX_SCO : CHRK_PORT_RX_SCO; payload[2] = tid;
		(void)ngd_ask_manager(port == 0 ? "btslim: port TX 0 disconnected" : "btslim: port RX 16 disconnected",
		    SLIM_USR_MC_DISCONNECT_PORT, payload, 3, tid);
	}
	(void)btslim_sco_ports(ifd, false);
}

static void
btslim_ngd(void)
{
	static const uint8_t chip_pgd[6] = { 0x00, 0x01, 0x20, 0x02, 0x17, 0x02 };
	uint8_t message[40];
	bool answered = false, addressed = false;
	uint8_t la;

	btslim_ctrl = (volatile uint32_t *)ml_io_map(BTSLIM_CTRL_PHYS, SLIM_CTRL_SIZE);
	if (btslim_ctrl == NULL) {
		printf("btslim: the controller cannot be mapped\n");
		return;
	}
	lck_mtx_lock(&codec_lock);
	btslim_enter();
	printf("btslim: the controller (%#llx) is version %#x; NGD: cfg %#x status %#x int en %#x stat %#x\n", BTSLIM_CTRL_PHYS,
	    slim_ctrl[0] >> 16, ngd_rd(NGD_CFG), ngd_rd(NGD_STATUS), ngd_rd(NGD_INT_EN), ngd_rd(NGD_INT_STAT));
	ngd_wr(NGD_INT_EN, DEF_NGD_INT_MASK);
	ngd_wr(NGD_RX_MSGQ_CFG, ngd_rd(NGD_RX_MSGQ_CFG) | SLIM_RX_MSGQ_TIMEOUT_VAL);
	ngd_wr(NGD_CFG, (ngd_rd(NGD_CFG) & ~(NGD_CFG_RX_MSGQ_EN | NGD_CFG_TX_MSGQ_EN)) | NGD_CFG_ENABLE);
	printf("btslim: NGD enabled in register mode (cfg %#x); waiting for the master's capability\n", ngd_rd(NGD_CFG));
	for (int ms = 0; ms < 3000; ms++) {
		unsigned length = ngd_receive(message);

		if (length >= 2 && (message[0] >> 5) == SLIM_MSG_MT_SRC_REFERRED_USER &&
		    (message[1] & 0x7f) == SLIM_USR_MC_MASTER_CAPABILITY && !answered) {
			uint32_t first = SLIM_MSG_ASM_FIRST_WORD(7, SLIM_MSG_MT_SRC_REFERRED_USER, SLIM_USR_MC_REPORT_SATELLITE, 0,
			    SLIM_LA_MGR);
			uint8_t reply[8] = { (uint8_t)first, (uint8_t)(first >> 8), (uint8_t)(first >> 16), SAT_MAGIC_LSB, SAT_MAGIC_MSB,
				             SAT_MSG_VER, SAT_MSG_PROT, 0 };

			printf("btslim: the master's capability: answered with REPORT_SATELLITE\n");
			answered = ngd_send(reply, 7);
		} else if (length >= 2) {
			printf("btslim: a message: MT %u MC %#x, %u bytes\n", message[0] >> 5, message[1] & 0x7f, length);
		}
		if (answered && (ngd_rd(NGD_STATUS) & NGD_LADDR)) {
			printf("btslim: THE NGD HAS ITS LOGICAL ADDRESS after %d ms\n", ms);
			addressed = true;
			break;
		}
		{
			uint32_t stat = ngd_rd(NGD_INT_STAT) & ~(NGD_INT_RX_MSG_RCVD | NGD_INT_TX_MSG_SENT);

			if (stat != 0) {
				printf("btslim: int stat %#x\n", stat);
				ngd_wr(NGD_INT_CLR, stat);
			}
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (!addressed) {
		printf("btslim: no logical address: cfg %#x status %#x int stat %#x%s\n", ngd_rd(NGD_CFG), ngd_rd(NGD_STATUS),
		    ngd_rd(NGD_INT_STAT), answered ? "" : "; NO CAPABILITY CAME");
		goto done;
	}
	la = codec_laddr(chip_pgd, "WCN3990's Bluetooth device");
	if (la != 0) {
		int msb = codec_read(la, CHRK_SB_SLAVE_HW_REV_MSB), lsb = codec_read(la, CHRK_SB_SLAVE_HW_REV_LSB);

		printf("btslim: THE WCN3990 ON THE BUS: logical address %#x (its generic device: %#x.%#x)\n", la, msb, lsb);
	}
	if (la != 0 && btslim_level >= 3) {
		btslim_sco_test(la);
	}
done:
	btslim_leave();
	lck_mtx_unlock(&codec_lock);
}

static void
btslim_up(uint32_t level)
{
	const struct hakios_qrtr_ops *transport = hakios_adsp_qrtr();
	struct slimbus_select_inst_req_msg_v01 select = { .instance = BTSLIM_QMI_INSTANCE, .mode_valid = 1,
		                                          .mode = SLIMBUS_MODE_MASTER_V01 };
	struct slimbus_select_inst_resp_msg_v01 selected = { { 0, 0 } };
	struct slimbus_power_req_msg_v01 power = { .pm_req = SLIMBUS_PM_ACTIVE_V01, .resp_type_valid = 1,
		                                   .resp_type = SLIMBUS_RESP_SYNCHRONOUS_V01 };
	struct slimbus_power_resp_msg_v01 powered = { { 0, 0 } };
	int rc;

	printf("btslim: level %u: the Bluetooth chip's SLIMbus (instance %u)\n", level, BTSLIM_QMI_INSTANCE);
	btslim_level = level;
	rc = hakios_qmi_handle_init_transport(&btslim_qmi, SLIMBUS_QMI_POWER_REQ_MAX_MSG_LEN + 64, &btslim_qmi_ops,
	    slim_qmi_handlers, transport);
	if (rc < 0) {
		printf("btslim: qmi: no handle (%d)\n", rc);
		return;
	}
	rc = qmi_add_lookup(&btslim_qmi, SLIMBUS_QMI_SVC_ID, SLIMBUS_QMI_SVC_V1, SLIMBUS_QMI_INS_ID);
	for (int i = 0; i < 500 && !btslim_server_there && rc == 0; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	if (!btslim_server_there || hakios_qmi_connect(btslim_qmi.sock, btslim_server_node, btslim_server_port) != 0) {
		printf("btslim: the DSP's SLIMbus service: not reached\n");
		qmi_handle_release(&btslim_qmi);
		return;
	}
	if (slim_qmi_request_on(&btslim_qmi, "btslim: select instance 1, the DSP the master",
	    SLIMBUS_QMI_SELECT_INSTANCE_REQ_V01, SLIMBUS_QMI_SELECT_INSTANCE_REQ_MAX_MSG_LEN, slimbus_select_inst_req_msg_v01_ei,
	    &select, slimbus_select_inst_resp_msg_v01_ei, &selected.resp) != 0 ||
	    slim_qmi_request_on(&btslim_qmi, "btslim: power the bus up", SLIMBUS_QMI_POWER_REQ_V01,
	    SLIMBUS_QMI_POWER_REQ_MAX_MSG_LEN, slimbus_power_req_msg_v01_ei, &power, slimbus_power_resp_msg_v01_ei,
	    &powered.resp) != 0) {
		qmi_handle_release(&btslim_qmi);
		return;
	}
	printf("btslim: THE BLUETOOTH SLIMBUS IS OURS AND POWERED\n");
	if (level >= 2) {
		btslim_ngd();
	}
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
		level = 0;
		if (PE_parse_boot_argn("btslim", &level, sizeof(level)) && level != 0) {
			btslim_up(level);
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
