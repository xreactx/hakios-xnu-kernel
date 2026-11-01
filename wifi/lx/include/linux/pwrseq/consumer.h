/*
 * No power sequencer here. ath10k's snoc.c then takes the radio's supplies as regulators of its own, as for
 * device trees that name them directly, which the 6T's does: devm_pwrseq_get() answers -EPROBE_DEFER, which
 * snoc.c takes so; and with no sequencer, power on and off succeed and do nothing (as Linux's do for NULL).
 */
#ifndef _LX_LINUX_PWRSEQ_CONSUMER_H_
#define _LX_LINUX_PWRSEQ_CONSUMER_H_

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/device.h>

struct pwrseq_desc;

static inline struct pwrseq_desc *
devm_pwrseq_get(struct device *dev __unused, const char *target __unused)
{
	return ERR_PTR(-EPROBE_DEFER);
}

static inline int
pwrseq_power_on(struct pwrseq_desc *desc __unused)
{
	return 0;
}

static inline int
pwrseq_power_off(struct pwrseq_desc *desc __unused)
{
	return 0;
}

#endif
