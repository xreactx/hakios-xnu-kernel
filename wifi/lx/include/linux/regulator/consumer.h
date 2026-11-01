/*
 * The radio's supplies as ath10k's snoc.c switches them (vdd-0.8-cx-mx, -1.8-xo, -1.3-rfa, -3.3-ch0): on the
 * 6T they are on as the boot chain leaves them -- the Wi-Fi firmware answers and says "ready" without our
 * touching them (bsd/dev/hakios.wlfw10.c) -- so every call succeeds and nothing is switched.
 */
#ifndef _LX_LINUX_REGULATOR_CONSUMER_H_
#define _LX_LINUX_REGULATOR_CONSUMER_H_

#include <linux/device.h>

struct regulator;
struct regulator_bulk_data {
	const char *supply;
	struct regulator *consumer;
	int init_load_uA;
	int ret;
};

static inline int
regulator_bulk_get(struct device *dev __unused, int num __unused, struct regulator_bulk_data *data __unused)
{
	return 0;
}

static inline int
devm_regulator_bulk_get(struct device *dev __unused, int num __unused, struct regulator_bulk_data *data __unused)
{
	return 0;
}

static inline int
regulator_bulk_enable(int num __unused, struct regulator_bulk_data *data __unused)
{
	return 0;
}

static inline int
regulator_bulk_disable(int num __unused, struct regulator_bulk_data *data __unused)
{
	return 0;
}

static inline void
regulator_bulk_free(int num __unused, struct regulator_bulk_data *data __unused)
{
}

static inline int
regulator_set_load(struct regulator *r __unused, int load __unused)
{
	return 0;
}

#endif
