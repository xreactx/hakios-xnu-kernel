/*
 * The one call of Qualcomm's secure-monitor interface ath10k makes (qmi.c): handing the Wi-Fi firmware's
 * memory to the modem's MSA, the Wi-Fi and its copy engines through the hypervisor, and back. The kernel
 * has the call (bsd/dev/hakios.mpss.c: hakios_mpss_assign_wlan()); the glue passes it on (wifi/lx/ath10k).
 */
#ifndef _LX_LINUX_QCOM_SCM_H_
#define _LX_LINUX_QCOM_SCM_H_

#include <linux/types.h>

#define QCOM_SCM_VMID_HLOS      0x3
#define QCOM_SCM_VMID_MSS_MSA   0xf
#define QCOM_SCM_VMID_WLAN      0x18
#define QCOM_SCM_VMID_WLAN_CE   0x19
#define QCOM_SCM_PERM_READ      0x4
#define QCOM_SCM_PERM_WRITE     0x2
#define QCOM_SCM_PERM_EXEC      0x1
#define QCOM_SCM_PERM_RW        (QCOM_SCM_PERM_READ | QCOM_SCM_PERM_WRITE)
#define QCOM_SCM_PERM_RWX       (QCOM_SCM_PERM_RW | QCOM_SCM_PERM_EXEC)

struct qcom_scm_vmperm {
	int vmid;
	int perm;
};

int qcom_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *src, const struct qcom_scm_vmperm *newvm,
    unsigned int dest_cnt);

#endif
