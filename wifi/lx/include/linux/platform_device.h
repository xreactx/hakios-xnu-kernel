/*
 * Platform devices as ath10k's snoc.c uses them, in place of LinuxKPI's (which has no probe, no resources):
 * the radio's node, its register window and its interrupts, and the driver's probe -- what the glue
 * (wifi/lx/ath10k/lx_snoc.c) makes of the 6T's device tree and calls. The functions are the glue's.
 */
#ifndef _LX_LINUX_PLATFORM_DEVICE_H_
#define _LX_LINUX_PLATFORM_DEVICE_H_

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>        /* (after FreeBSD's systm.h, whose msleep() is another: LinuxKPI's is put back) */

struct platform_device {
	const char *name;
	int id;
	bool id_auto;
	struct device dev;
	u32 num_resources;
	struct resource *resource;
	const int *irqs;                /* the glue's: the interrupts, as platform_get_irq() hands them out */
	unsigned num_irqs;
};

struct platform_driver {
	int (*probe)(struct platform_device *);
	void (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
};

struct platform_device_info {
	struct device *parent;
	struct fwnode_handle *fwnode;
	bool of_node_reused;
	const char *name;
	int id;
	const struct resource *res;
	unsigned num_res;
	const void *data;
	size_t size_data;
	u64 dma_mask;
};

#define to_platform_device(d)   container_of((d), struct platform_device, dev)
#define dev_is_platform(d)      (true)

static inline void *
platform_get_drvdata(const struct platform_device *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}

static inline void
platform_set_drvdata(struct platform_device *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}

struct resource *platform_get_resource_byname(struct platform_device *pdev, unsigned type, const char *name);
int platform_get_irq(struct platform_device *pdev, unsigned index);
struct platform_device *platform_device_register_full(const struct platform_device_info *info);
void platform_device_unregister(struct platform_device *pdev);
int of_dma_configure(struct device *dev, struct device_node *np, bool force_dma);
const void *of_device_get_match_data(const struct device *dev);
const void *device_get_match_data(const struct device *dev);

/* the driver the object holds: the glue probes it with the 6T's radio (one driver, one device) */
#define module_platform_driver(d)       struct platform_driver *const lx_platform_driver = &(d)

#endif
