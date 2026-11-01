/*
 * ath10k's snoc.c asks to hear when the modem -- whose domain its firmware is -- restarts (subsystem restart,
 * "SSR"), to take the driver down and up again. Our modem is started and stopped by the kernel
 * (bsd/dev/hakios.mpss.c) and never restarts on its own: the registration succeeds and never calls back.
 */
#ifndef _LX_LINUX_REMOTEPROC_QCOM_RPROC_H_
#define _LX_LINUX_REMOTEPROC_QCOM_RPROC_H_

#include <linux/notifier.h>

enum qcom_ssr_notify_type {
	QCOM_SSR_BEFORE_POWERUP,
	QCOM_SSR_AFTER_POWERUP,
	QCOM_SSR_BEFORE_SHUTDOWN,
	QCOM_SSR_AFTER_SHUTDOWN,
};

struct qcom_ssr_notify_data {
	const char *name;
	bool crashed;
};

static inline void *
qcom_register_ssr_notifier(const char *name __unused, struct notifier_block *nb __unused)
{
	static int registered;

	return &registered;
}

static inline int
qcom_unregister_ssr_notifier(void *notify __unused, struct notifier_block *nb __unused)
{
	return 0;
}

#endif
