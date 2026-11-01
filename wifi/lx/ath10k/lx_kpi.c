/*
 * What ath10k asks of LinuxKPI beyond what ath11k did (wifi/lx/src has the rest): device-managed memory, the
 * dummy network device its NAPI hangs on, copies of buffers, mac80211's transmit queues (which our 802.11
 * layer, src/lx_80211.c, does not use: it hands frames to the driver's tx() itself), a regulatory domain to
 * fill in, and the device-tree readers snoc.c and core.c ask for things the 6T's node does not have.
 */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <net/mac80211.h>

#include "lx.h"

void *linux_dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag);

int
linux_dma_tag_init(struct device *dev, u64 mask)
{
	return (0);
}

int
linux_dma_tag_init_coherent(struct device *dev, u64 mask)
{
	return (0);
}

void *
linuxkpi_dmam_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t flag)
{
	return (linux_dma_alloc_coherent(dev, size, dma_handle, flag));
}

/* device-managed memory: the radio's device is never removed here, so what is managed is simply kept */
void *
lkpi_devres_alloc(void (*release)(struct device *, void *), size_t size, gfp_t gfp)
{
	return (kzalloc(size, gfp));
}

void
lkpi_devres_add(struct device *dev, void *p)
{
}

void
lkpi_devm_kmalloc_release(struct device *dev, void *p)
{
}

struct net_device *
linuxkpi_alloc_netdev(size_t length, const char *name, uint32_t flags, void (*setup)(struct net_device *))
{
	struct net_device *dev = kzalloc(sizeof(*dev) + length, GFP_KERNEL);

	if (dev != NULL && setup != NULL) {
		setup(dev);
	}
	return (dev);
}

void
linuxkpi_free_netdev(struct net_device *dev)
{
	kfree(dev);
}

int
netif_threaded_enable(struct net_device *dev)
{
	return (0);
}

struct sk_buff *
linuxkpi_skb_copy(const struct sk_buff *skb, gfp_t gfp)
{
	unsigned headroom = skb_headroom(skb);
	struct sk_buff *copy = linuxkpi_dev_alloc_skb(headroom + skb->len, gfp);

	if (copy == NULL) {
		return (NULL);
	}
	skb_reserve(copy, headroom);
	memcpy(skb_put(copy, skb->len), skb->data, skb->len);
	return (copy);
}

uint32_t
linuxkpi_cfg80211_calculate_bitrate(struct rate_info *rate)
{
	return (0);
}

struct linuxkpi_ieee80211_regdomain *
lkpi_get_linuxkpi_ieee80211_regdomain(size_t rules)
{
	return (kzalloc(sizeof(struct linuxkpi_ieee80211_regdomain) + rules * sizeof(struct ieee80211_reg_rule), GFP_KERNEL));
}

int
lkpi_hex_dump_to_buffer(const void *buf, size_t len, int rowsize, int groupsize, char *linebuf, size_t linebuflen, bool ascii)
{
	if (linebuflen != 0) {
		linebuf[0] = 0;
	}
	return (0);
}

/* ---- mac80211's transmit queues: not used by our 802.11 layer ---- */

uint8_t
linuxkpi_ieee80211_get_tid(struct ieee80211_hdr *hdr, bool nonqos_ok)
{
	if (ieee80211_is_data_qos(hdr->frame_control)) {
		return (*ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK);
	}
	return (0);
}

struct ieee80211_txq *
linuxkpi_ieee80211_next_txq(struct ieee80211_hw *hw, uint8_t ac)
{
	return (NULL);
}

void
linuxkpi_ieee80211_schedule_txq(struct ieee80211_hw *hw, struct ieee80211_txq *txq, bool withoutpkts)
{
}

struct sk_buff *
linuxkpi_ieee80211_tx_dequeue(struct ieee80211_hw *hw, struct ieee80211_txq *txq)
{
	return (NULL);
}

void
linuxkpi_ieee80211_txq_get_depth(struct ieee80211_txq *txq, unsigned long *frame_cnt, unsigned long *byte_cnt)
{
	if (frame_cnt != NULL) {
		*frame_cnt = 0;
	}
	if (byte_cnt != NULL) {
		*byte_cnt = 0;
	}
}

void
linuxkpi_ieee80211_txq_schedule_start(struct ieee80211_hw *hw, uint8_t ac)
{
}

void
linuxkpi_ieee80211_stop_queue(struct ieee80211_hw *hw, int q)
{
}

void
linuxkpi_ieee80211_wake_queue(struct ieee80211_hw *hw, int q)
{
}

/* ---- device-tree readers: the 6T's node has none of what these are asked for ---- */

int
device_property_read_u32(struct device *dev, const char *name, u32 *value)
{
	return (-EINVAL);
}

int
of_property_read_string_index(const struct device_node *np, const char *name, int index, const char **out)
{
	return (-EINVAL);
}

int
of_property_read_u8(const struct device_node *np, const char *name, u8 *out)
{
	return (-EINVAL);
}

int
of_property_read_u8_array(const struct device_node *np, const char *name, u8 *out, size_t size)
{
	return (-EINVAL);
}
