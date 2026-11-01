/*
 * ath10k's qmi.c writes the Wi-Fi firmware's build into the shared memory's table of image versions
 * (ath10k_qmi_add_wlan_ver_smem()), for whoever reads that on Android. Nothing here does: the item is
 * "not found", which qmi.c says and goes on from.
 */
#ifndef _LX_LINUX_SOC_QCOM_SMEM_H_
#define _LX_LINUX_SOC_QCOM_SMEM_H_

#include <linux/err.h>
#include <linux/errno.h>

#define QCOM_SMEM_HOST_ANY      -1

static inline void *
qcom_smem_get(unsigned host __unused, unsigned item __unused, size_t *size)
{
	if (size != NULL) {
		*size = 0;
	}
	return ERR_PTR(-ENOENT);
}

#endif
