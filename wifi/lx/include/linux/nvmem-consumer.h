/*
 * No NVMEM here: ath10k's core.c asks it for the radio's calibration data (ath10k_download_cal_nvmem()), which
 * it then takes from elsewhere. The answer is "no such cell", as from a device tree without one.
 */
#ifndef _LX_LINUX_NVMEM_CONSUMER_H_
#define _LX_LINUX_NVMEM_CONSUMER_H_

#include <linux/err.h>
#include <linux/device.h>

struct nvmem_cell;

static inline struct nvmem_cell *
devm_nvmem_cell_get(struct device *dev __unused, const char *name __unused)
{
	return ERR_PTR(-ENOENT);
}

static inline void *
nvmem_cell_read(struct nvmem_cell *cell __unused, size_t *len __unused)
{
	return ERR_PTR(-ENOENT);
}

static inline void
nvmem_cell_put(struct nvmem_cell *cell __unused)
{
}

#endif
