/*
 * The OnePlus 6T's Wi-Fi firmware, asked what ath10k asks it first. A STAND-IN, as hakios.wlfw.c is for
 * redwood's WCN6750, TO BE THROWN AWAY when ath10k's own qmi.c runs here.
 *
 * The 6T's radio is a WCN3990. Its firmware does not run on a processor of its own: it is a protection
 * domain of the modem's ("msm/modem/wlan_pd", the modem's image's modemuw.jsn), which the modem starts once
 * our service registry tells it where kernel/elf_loader is (hakios.mpss.c), and which then offers the
 * service WLFW (QMI service 69). Before ath10k's snoc driver touches the radio's registers it has an
 * exchange with that service -- ath10k_qmi_event_server_arrive() and what follows it in qmi.c:
 *
 *   wifi10=1  the indications ath10k wants (firmware ready, MSA ready); the firmware's status
 *   wifi10=2  ... the host's capability (no daemon)
 *   wifi10=3  ... the firmware's memory: MSA, 1 MB (the tree's qcom,wlan-msa-memory; the loader keeps the
 *             wlan_fw carveout at 0x8df00000 for it, wifi10_msa=ADDRESS for another), and the regions of it the
 *             firmware says it will use
 *   wifi10=4  ... those regions given to the firmware through the hypervisor (the modem's MSA, the Wi-Fi, and
 *             unless secure its copy engines: ath10k_qmi_map_msa_permission()), 20 ms after the answer as
 *             ath10k waits on the SDM845; then "MSA ready"
 *   wifi10=5  ... the firmware's capabilities: chip, board, SoC, firmware version
 *   wifi10=6  ... the board's data file, bdwlan.bXX by the board id (bdwlan.bin without one), from the modem's
 *             image in /fw/image, in pieces of 6 KB, and the calibration report (none); then "firmware ready"
 *             waited for (10 s)
 *
 * Started on its own thread when the modem announces the service (hakios.mpss.c), with modem_keep=1 so
 * that the modem stays up after its steps. The regions given to the firmware stay its: Linux gives them
 * back only when the driver goes.
 *
 * The messages' structures and tables are ath10k's own (qmi_wlfw_v01.h and .c, ISC), put into the build
 * directory by tools/mk-wlfw10.sh and not kept here; without them this compiles to a line that says so.
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
#define HAKIOS_QMI_NO_KERNEL_CONNECT
#include <sys/hakios.qmi.h>

extern const struct hakios_qrtr_ops *hakios_mpss_qrtr(void);           /* bsd/dev/hakios.mpss.c */
extern bool hakios_mpss_assign_wlan(uint64_t address, uint64_t size, bool secure, bool to_firmware);
extern const uint8_t *hakios_mpss_fw_file(const char *name, uint64_t *size);
extern void hakios_wlfw10_start(void);

/* Every line goes to the loader's log directly as well: the kernel's own buffer loses lines of a long boot. */
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

#if !__has_include("hakios.wlfw_ath10k.h")

void
hakios_wlfw10_start(void)
{
	uint32_t level = 0;

	if (PE_parse_boot_argn("wifi10", &level, sizeof(level)) && level != 0) {
		say("wlfw10: NOT BUILT IN: ath10k's message descriptions were not there when this kernel was built "
		    "(tools/mk-wlfw10.sh, then build again)\n");
	}
}

#else

#include "hakios.wlfw_ath10k.h"

#define WLFW_SERVICE            69
#define WLFW_VERSION            1
#define ATH10K_QMI_CLIENT_ID    0x4b4e454c      /* qmi.h */
#define WLFW_TIMEOUT_MS         5000            /* qmi.h: ATH10K_QMI_TIMEOUT, 30 s; ours shorter, for a test */
#define MSA_DEFAULT             0x8df00000ull   /* the loader's "wlan_fw" carveout */
#define MSA_SIZE                0x100000ull     /* the tree's qcom,wlan-msa-memory */

static struct qmi_handle handle;
static uint32_t level;
static struct {
	volatile bool server, gone, fw_ready, msa_ready;
	uint64_t began;
} wlfw;

static uint64_t
now_ms(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000000;
}

static int
wlfw_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error = hakios_qmi_connect(qmi->sock, service->node, service->port);

	say("wlfw10: the service is at node %u port %u (version %u, instance %u)%s\n", service->node, service->port,
	    service->version, service->instance, error ? ": CANNOT BE CONNECTED TO" : "");
	wlfw.server = error == 0;
	return error;
}

static void
wlfw_del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	say("wlfw10: THE SERVICE AT NODE %u PORT %u HAS GONE\n", service->node, service->port);
	wlfw.gone = true;
}

static void
wlfw_fw_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	say("wlfw10: +%llu ms: THE FIRMWARE INDICATES \"firmware ready\"\n", now_ms() - wlfw.began);
	wlfw.fw_ready = true;
}

static void
wlfw_msa_ready(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq, __unused struct qmi_txn *txn,
    __unused const void *decoded)
{
	say("wlfw10: +%llu ms: the firmware indicates \"MSA ready\"\n", now_ms() - wlfw.began);
	wlfw.msa_ready = true;
}

static const struct qmi_ops wlfw_ops = { .new_server = wlfw_new_server, .del_server = wlfw_del_server };
static const struct qmi_msg_handler wlfw_handlers[] = {
	{ QMI_INDICATION, QMI_WLFW_FW_READY_IND_V01, wlfw_fw_ready_ind_msg_v01_ei, sizeof(struct wlfw_fw_ready_ind_msg_v01),
	  wlfw_fw_ready },
	{ QMI_INDICATION, QMI_WLFW_MSA_READY_IND_V01, wlfw_msa_ready_ind_msg_v01_ei, sizeof(struct wlfw_msa_ready_ind_msg_v01),
	  wlfw_msa_ready },
	{ 0 },
};

/* One request and its reply; `reply` begins with the result. True: success (or what `also_ok` names as good enough). */
static bool
ask(const char *what, int message, size_t largest, const struct qmi_elem_info *request_ei, const void *request,
    const struct qmi_elem_info *reply_ei, void *reply, uint16_t also_ok)
{
	const struct qmi_response_type_v01 *result = reply;
	struct qmi_txn txn;
	uint64_t began = now_ms();
	int ret;

	ret = qmi_txn_init(&handle, &txn, reply_ei, reply);
	if (ret >= 0) {
		ret = qmi_send_request(&handle, NULL, &txn, message, largest, request_ei, request);
		if (ret < 0) {
			qmi_txn_cancel(&txn);
		} else {
			ret = hakios_qmi_txn_wait_ms(&txn, WLFW_TIMEOUT_MS);
		}
	}
	if (ret >= 0 && (result->result == QMI_RESULT_SUCCESS_V01 || (also_ok != 0 && result->error == also_ok))) {
		say("wlfw10: %s (message %#x): %s, after %llu ms\n", what, message, result->result == QMI_RESULT_SUCCESS_V01 ?
		    "success" : "refused, as ath10k accepts", now_ms() - began);
		return true;
	}
	if (ret < 0) {
		say("wlfw10: %s (message %#x): NO ANSWER THAT CAN BE READ: %d, after %llu ms\n", what, message, ret, now_ms() - began);
	} else {
		say("wlfw10: %s (message %#x): THE FIRMWARE REFUSES: result %u, error %u, after %llu ms\n", what, message, result->result,
		    result->error, now_ms() - began);
	}
	return false;
}

static struct wlfw_msa_info_resp_msg_v01 msa_info;
static struct wlfw_cap_resp_msg_v01 cap;
static struct wlfw_bdf_download_req_msg_v01 bdf;        /* (6 KB of data: not on the stack) */

/* The board's data file, in pieces, as ath10k_qmi_bdf_dnld_send_sync(). */
static bool
board_data(void)
{
	struct wlfw_bdf_download_resp_msg_v01 resp;
	uint32_t board = cap.board_info_valid ? cap.board_info.board_id : 0xff;
	char name[40];
	uint64_t size = 0, at = 0;
	const uint8_t *data;
	bool ok = true;

	if (board < 0xff) {
		snprintf(name, sizeof(name), "image/bdwlan.b%02x", board);
	} else {
		snprintf(name, sizeof(name), "image/bdwlan.bin");
	}
	data = hakios_mpss_fw_file(name, &size);
	if (data == NULL && board < 0xff) {
		say("wlfw10: no /fw/%s: the generic bdwlan.bin instead\n", name);
		snprintf(name, sizeof(name), "image/bdwlan.bin");
		data = hakios_mpss_fw_file(name, &size);
	}
	if (data == NULL) {
		say("wlfw10: no /fw/%s (the modem's image: MODEM=1 builds stage it)\n", name);
		return false;
	}
	say("wlfw10: the board's data: /fw/%s, %llu bytes, for board %#x\n", name, size, board);
	bzero(&bdf, sizeof(bdf));
	while (ok && at < size) {
		bdf.valid = 1;
		bdf.file_id_valid = 1;
		bdf.file_id = 0;
		bdf.total_size_valid = 1;
		bdf.total_size = (uint32_t)size;
		bdf.seg_id_valid = 1;
		bdf.data_valid = 1;
		bdf.end_valid = 1;
		bdf.data_len = size - at > QMI_WLFW_MAX_DATA_SIZE_V01 ? QMI_WLFW_MAX_DATA_SIZE_V01 : (uint32_t)(size - at);
		bdf.end = at + bdf.data_len == size;
		memcpy(bdf.data, data + at, bdf.data_len);
		bzero(&resp, sizeof(resp));
		/* (the last piece makes the firmware check the file's CRC: a "malformed" answer then is ath10k's to accept) */
		ok = ask("the board's data, a piece", QMI_WLFW_BDF_DOWNLOAD_REQ_V01, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_bdf_download_req_msg_v01_ei, &bdf, wlfw_bdf_download_resp_msg_v01_ei, &resp,
		    bdf.end ? QMI_ERR_MALFORMED_MSG_V01 : 0);
		at += bdf.data_len;
		bdf.seg_id++;
	}
	{
		void *file = (void *)(uintptr_t)data;

		kfree_data(file, size);
	}
	say("wlfw10: the board's data %s in %u pieces\n", ok ? "SENT" : "NOT ALL SENT", bdf.seg_id);
	return ok;
}

static void
exchange(void)
{
	struct wlfw_ind_register_req_msg_v01 ind = { 0 };
	struct wlfw_ind_register_resp_msg_v01 ind_resp = { 0 };
	struct wlfw_host_cap_req_msg_v01 host = { 0 };
	struct wlfw_host_cap_resp_msg_v01 host_resp = { 0 };
	struct wlfw_msa_info_req_msg_v01 msa = { 0 };
	struct wlfw_msa_ready_req_msg_v01 ready = { 0 };
	struct wlfw_msa_ready_resp_msg_v01 ready_resp = { 0 };
	struct wlfw_cap_req_msg_v01 cap_req = { 0 };
	uint64_t msa_address = MSA_DEFAULT;
	unsigned given = 0;

	ind.client_id_valid = 1;
	ind.client_id = ATH10K_QMI_CLIENT_ID;
	ind.fw_ready_enable_valid = 1;
	ind.fw_ready_enable = 1;
	ind.msa_ready_enable_valid = 1;
	ind.msa_ready_enable = 1;
	if (!ask("the indications wanted", QMI_WLFW_IND_REGISTER_REQ_V01, WLFW_IND_REGISTER_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_ind_register_req_msg_v01_ei, &ind, wlfw_ind_register_resp_msg_v01_ei, &ind_resp, 0)) {
		return;
	}
	say("wlfw10: the firmware's status: %s%#llx%s\n", ind_resp.fw_status_valid ? "" : "(not given) ", ind_resp.fw_status,
	    ind_resp.fw_status_valid && (ind_resp.fw_status & QMI_WLFW_FW_READY_V01) ? ": READY ALREADY (a run before this one)" : "");
	if (level < 2) {
		return;
	}
	host.daemon_support_valid = 1;
	host.daemon_support = 0;
	if (!ask("the host's capability", QMI_WLFW_HOST_CAP_REQ_V01, WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_host_cap_req_msg_v01_ei, &host, wlfw_host_cap_resp_msg_v01_ei, &host_resp, QMI_ERR_NOT_SUPPORTED_V01)) {
		/*
		 * Some of the SDM845's firmware builds take the daemon's flag as one byte, and call the four of
		 * ath10k's message malformed: ath10k sends it so with the tree's qcom,snoc-host-cap-8bit-quirk.
		 */
		if (host_resp.resp.error != QMI_ERR_MALFORMED_MSG_V01) {
			return;
		}
		bzero(&host_resp, sizeof(host_resp));
		if (!ask("the host's capability, its flag as one byte", QMI_WLFW_HOST_CAP_REQ_V01, WLFW_HOST_CAP_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_host_cap_8bit_req_msg_v01_ei, &host, wlfw_host_cap_resp_msg_v01_ei, &host_resp, QMI_ERR_NOT_SUPPORTED_V01)) {
			return;
		}
	}
	if (level < 3) {
		return;
	}
	PE_parse_boot_argn("wifi10_msa", &msa_address, sizeof(msa_address));
	msa.msa_addr = msa_address;
	msa.size = MSA_SIZE;
	if (!ask("the firmware's memory (MSA)", QMI_WLFW_MSA_INFO_REQ_V01, WLFW_MSA_INFO_REQ_MSG_V01_MAX_MSG_LEN,
	    wlfw_msa_info_req_msg_v01_ei, &msa, wlfw_msa_info_resp_msg_v01_ei, &msa_info, 0)) {
		return;
	}
	say("wlfw10: MSA at %#llx, %llu KB: the firmware will use %u region%s of it\n", msa_address, MSA_SIZE >> 10,
	    msa_info.mem_region_info_len, msa_info.mem_region_info_len == 1 ? "" : "s");
	for (unsigned i = 0; i < msa_info.mem_region_info_len && i < QMI_WLFW_MAX_MEM_REG_V01; i++) {
		const struct wlfw_memory_region_info_s_v01 *r = &msa_info.mem_region_info[i];
		bool inside = r->region_addr >= msa_address && r->region_addr + r->size <= msa_address + MSA_SIZE;

		say("wlfw10:   region %u: %#llx, %u bytes, flags %#x%s\n", i, r->region_addr, r->size, r->secure_flag,
		    inside ? "" : ": OUTSIDE THE MSA (ath10k stops here)");
		if (!inside) {
			return;
		}
	}
	if (level < 4) {
		return;
	}
	delay_for_interval(20, NSEC_PER_MSEC);          /* (ath10k's: the SDM845 otherwise faults on its XPU) */
	for (given = 0; given < msa_info.mem_region_info_len; given++) {
		const struct wlfw_memory_region_info_s_v01 *r = &msa_info.mem_region_info[given];

		if (!hakios_mpss_assign_wlan(r->region_addr, r->size, r->secure_flag != 0, true)) {
			say("wlfw10: region %u NOT GIVEN to the firmware\n", given);
			return;
		}
	}
	if (!ask("MSA ready", QMI_WLFW_MSA_READY_REQ_V01, WLFW_MSA_READY_REQ_MSG_V01_MAX_MSG_LEN, wlfw_msa_ready_req_msg_v01_ei,
	    &ready, wlfw_msa_ready_resp_msg_v01_ei, &ready_resp, 0) || level < 5) {
		return;
	}
	if (!ask("the firmware's capabilities", QMI_WLFW_CAP_REQ_V01, WLFW_CAP_REQ_MSG_V01_MAX_MSG_LEN, wlfw_cap_req_msg_v01_ei,
	    &cap_req, wlfw_cap_resp_msg_v01_ei, &cap, 0)) {
		return;
	}
	say("wlfw10: THE FIRMWARE: chip %#x family %#x%s, board %#x%s, SoC %#x%s, firmware %#x %s%s\n", cap.chip_info.chip_id,
	    cap.chip_info.chip_family, cap.chip_info_valid ? "" : " (not given)", cap.board_info.board_id,
	    cap.board_info_valid ? "" : " (not given)", cap.soc_info.soc_id, cap.soc_info_valid ? "" : " (not given)",
	    cap.fw_version_info.fw_version, cap.fw_version_info.fw_build_timestamp, cap.fw_build_id_valid ? "" : "");
	if (cap.fw_build_id_valid) {
		say("wlfw10: its build: %s\n", cap.fw_build_id);
	}
	if (level < 6 || !board_data()) {
		return;
	}
	{
		/* (ath10k_qmi_event_msa_ready(): the board's data, then the calibration report -- none here, no XO data in the tree) */
		static struct wlfw_cal_report_req_msg_v01 report;
		static struct wlfw_cal_report_resp_msg_v01 report_resp;

		if (!ask("the calibration report (none)", QMI_WLFW_CAL_REPORT_REQ_V01, WLFW_CAL_REPORT_REQ_MSG_V01_MAX_MSG_LEN,
		    wlfw_cal_report_req_msg_v01_ei, &report, wlfw_cal_report_resp_msg_v01_ei, &report_resp, 0)) {
			return;
		}
	}
	for (unsigned ms = 0; ms < 10000 && !wlfw.fw_ready && !wlfw.gone; ms += 10) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	say("wlfw10: %s\n", wlfw.fw_ready ? "THE FIRMWARE IS READY: next, the radio's copy engines (ath10k's snoc)" :
	    "no \"firmware ready\" in 10 s after the board's data");
}

static void
wlfw10_thread(__unused void *parameter, __unused wait_result_t result)
{
	const struct hakios_qrtr_ops *transport = hakios_mpss_qrtr();
	int rc;

	wlfw.began = now_ms();
	if (transport == NULL) {
		say("wlfw10: the modem's link is not up\n");
		return;
	}
	rc = hakios_qmi_handle_init_transport(&handle, WLFW_BDF_DOWNLOAD_REQ_MSG_V01_MAX_MSG_LEN + 64, &wlfw_ops, wlfw_handlers,
	    transport);
	if (rc < 0) {
		say("wlfw10: no QMI handle on the modem's link (%d)\n", rc);
		return;
	}
	rc = qmi_add_lookup(&handle, WLFW_SERVICE, WLFW_VERSION, 0);
	for (unsigned i = 0; i < 500 && !wlfw.server && rc == 0; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	if (!wlfw.server) {
		say("wlfw10: the Wi-Fi firmware's service is not found (%d)\n", rc);
		qmi_handle_release(&handle);
		return;
	}
	say("wlfw10: level %u: ath10k's first exchange with the Wi-Fi firmware\n", level);
	exchange();
	say("wlfw10: done (the handle stays open: the firmware's regions stay its)\n");
}

/* hakios.mpss.c, when the modem announces WLFW: the exchange, once, on a thread, with wifi10=N. */
void
hakios_wlfw10_start(void)
{
	static bool started;
	thread_t thread;

	if (started || !PE_parse_boot_argn("wifi10", &level, sizeof(level)) || level == 0) {
		return;
	}
	started = true;
	if (kernel_thread_start(wlfw10_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("wlfw10: no thread\n");
	}
}

#endif
