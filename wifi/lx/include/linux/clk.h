/*
 * No clock framework here: the clocks ath10k's snoc.c would switch on (the radio's reference, "cxo_ref_clk_pin")
 * are on as the boot chain leaves them. Every call succeeds and does nothing.
 */
#ifndef _LX_LINUX_CLK_H_
#define _LX_LINUX_CLK_H_

#include <linux/device.h>

struct clk;
struct clk_bulk_data {
	const char *id;
	struct clk *clk;
};

static inline int
devm_clk_bulk_get_optional(struct device *dev __unused, int num __unused, struct clk_bulk_data *clks __unused)
{
	return 0;
}

static inline int
clk_bulk_prepare_enable(int num __unused, const struct clk_bulk_data *clks __unused)
{
	return 0;
}

static inline void
clk_bulk_disable_unprepare(int num __unused, const struct clk_bulk_data *clks __unused)
{
}

#endif
