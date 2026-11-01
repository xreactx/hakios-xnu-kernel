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
 * Boot-arg slim=1; without it nothing here runs.
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
	printf("slim: THE AUDIO SLIMBUS IS OURS AND POWERED (the DSP its master); next: the NGD and the codec\n");
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
