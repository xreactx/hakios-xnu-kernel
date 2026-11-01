/*
 * ath10k's snoc.c maps the firmware's memory through an IOMMU of its own only when the tree gives the radio a
 * "wifi-firmware" child; the 6T's (and the glue's) does not, and TrustZone has the memory instead (use_tz).
 * What that path names is declared here and never called.
 */
#ifndef _LX_LINUX_IOMMU_H_
#define _LX_LINUX_IOMMU_H_

#include <linux/device.h>
#include <linux/err.h>

#define IOMMU_READ      (1 << 0)
#define IOMMU_WRITE     (1 << 1)

struct iommu_domain;

static inline struct iommu_domain *
iommu_paging_domain_alloc(struct device *dev __unused)
{
	return ERR_PTR(-ENODEV);
}

static inline void iommu_domain_free(struct iommu_domain *d __unused) { }
static inline int iommu_attach_device(struct iommu_domain *d __unused, struct device *dev __unused) { return -ENODEV; }
static inline void iommu_detach_device(struct iommu_domain *d __unused, struct device *dev __unused) { }

static inline int
iommu_map(struct iommu_domain *d __unused, unsigned long iova __unused, phys_addr_t paddr __unused, size_t size __unused,
    int prot __unused, gfp_t gfp __unused)
{
	return -ENODEV;
}

static inline size_t
iommu_unmap(struct iommu_domain *d __unused, unsigned long iova __unused, size_t size __unused)
{
	return 0;
}

#endif
