/*
 * The OnePlus 6T's radio as ath10k's snoc.c expects to find it on a Linux machine: a platform device made
 * from the phone's device tree (qcom,icnss@18800000 there; ath10k's binding, qcom,wcn3990-wifi, here), the
 * functions a platform device and its interrupts are used through, a NAPI of our own for the copy engines,
 * and the entries the kernel calls (lx_start() and the rest, sys/hakios.lx.h). What src/lx_bus.c is for
 * redwood's ath11k, without its stand-in for the driver's bus: snoc.c is built, and runs as it is.
 *
 * The radio, from the tree: its registers at 0x18800000 (8 MB, "membase"); twelve interrupts, one a copy
 * engine, SPI 414 to 425; the firmware's memory (MSA) 1 MB, the loader's wlan_fw carveout at 0x8df00000
 * (bsd/dev/hakios.wlfw10.c found that the firmware takes it); the one-byte form of the host's capability
 * (that firmware calls ath10k's four bytes malformed: "qcom,snoc-host-cap-8bit-quirk"). Its supplies and
 * clocks are on as the boot chain leaves them (wifi/lx/include/linux/regulator/consumer.h, clk.h); its
 * firmware is the modem's (bsd/dev/hakios.mpss.c starts the domain); its memory is handed to the firmware
 * by the hypervisor through the kernel (qcom_scm_assign_mem() below).
 */
#define LX_QMI_AS_IT_IS				/* (this file is where the driver's requests go past) */
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/netdevice.h>

#include "core.h"
#include "debug.h"
#include "snoc.h"
#include "qmi.h"

#include <lx_os.h>
#include "lx.h"

#define RADIO_REGISTERS		0x18800000ull
#define RADIO_REGISTERS_SIZE	0x800000ull
#define RADIO_IRQS		12
#define RADIO_FIRST_INTID	(414 + 32)		/* the GIC's number of SPI 414 */
#define RADIO_MSA		0x8df00000ull
#define RADIO_MSA_SIZE		0x100000ull

extern struct platform_driver *const lx_platform_driver;	/* snoc.c's (module_platform_driver(), ours) */

/* ---- the radio's node and device ------------------------------------------------------------------- */

static const char compatible[] = "qcom,wcn3990-wifi";
static const struct property properties[] = {
	{ "compatible", sizeof(compatible), compatible },
	{ "qcom,snoc-host-cap-8bit-quirk", 0, "" },
};
static struct device_node node = {
	.name = "wifi", .full_name = "/soc/wifi@18800000", .properties = properties,
	.nproperties = sizeof(properties) / sizeof(properties[0]),
};
static struct resource resources[] = {
	{ .start = RADIO_REGISTERS, .end = RADIO_REGISTERS + RADIO_REGISTERS_SIZE - 1, .flags = IORESOURCE_MEM, .name = "membase" },
};
static int irqs[RADIO_IRQS];
static struct platform_device radio = {
	.name = "wifi", .id = -1, .num_resources = 1, .resource = resources, .irqs = irqs, .num_irqs = RADIO_IRQS,
};

const struct property *
of_find_property(const struct device_node *np, const char *name, int *lenp)
{
	for (int i = 0; np != NULL && i < np->nproperties; i++) {
		if (strcmp(np->properties[i].name, name) == 0) {
			if (lenp != NULL) {
				*lenp = np->properties[i].length;
			}
			return (&np->properties[i]);
		}
	}
	return (NULL);
}

const void *
of_get_property(const struct device_node *np, const char *name, int *lenp)
{
	const struct property *p = of_find_property(np, name, lenp);

	return (p != NULL ? p->value : NULL);
}

int
of_property_read_string(const struct device_node *np, const char *name, const char **out)
{
	const struct property *p = of_find_property(np, name, NULL);

	if (p == NULL || p->length == 0) {
		return (-EINVAL);
	}
	*out = p->value;
	return (0);
}

int
of_property_read_u32_index(const struct device_node *np, const char *name, u32 index, u32 *out)
{
	const struct property *p = of_find_property(np, name, NULL);

	if (p == NULL || (index + 1) * sizeof(u32) > (size_t)p->length) {
		return (-EINVAL);
	}
	*out = ((const u32 *)p->value)[index];
	return (0);
}

bool
of_device_is_compatible(const struct device_node *np, const char *compat)
{
	const char *c = of_get_property(np, "compatible", NULL);

	return (c != NULL && strcmp(c, compat) == 0);
}

struct device_node *
of_get_child_by_name(const struct device_node *np, const char *name)
{
	return (NULL);				/* (no "wifi-firmware" child: TrustZone has the firmware's memory) */
}

bool
of_machine_is_compatible(const char *compat)
{
	return (false);
}

const struct of_device_id *
of_match_node(const struct of_device_id *matches, const struct device_node *np)
{
	for (; matches != NULL && matches->compatible[0] != 0; matches++) {
		if (of_device_is_compatible(np, matches->compatible)) {
			return (matches);
		}
	}
	return (NULL);
}

const void *
of_device_get_match_data(const struct device *dev)
{
	const struct of_device_id *match = of_match_node(lx_platform_driver->driver.of_match_table, dev->of_node);

	return (match != NULL ? match->data : NULL);
}

const void *
device_get_match_data(const struct device *dev)
{
	return (of_device_get_match_data(dev));
}

/* the firmware's memory: the carveout the loader keeps for it */
int
of_reserved_mem_region_to_resource(const struct device_node *np, unsigned int index, struct resource *res)
{
	if (np != &node || index != 0) {
		return (-ENODEV);
	}
	memset(res, 0, sizeof(*res));
	res->start = RADIO_MSA;
	res->end = RADIO_MSA + RADIO_MSA_SIZE - 1;
	res->flags = IORESOURCE_MEM;
	return (0);
}

struct resource *
platform_get_resource_byname(struct platform_device *pdev, unsigned type, const char *name)
{
	for (u32 i = 0; i < pdev->num_resources; i++) {
		if ((pdev->resource[i].flags & type) && pdev->resource[i].name != NULL && strcmp(pdev->resource[i].name, name) == 0) {
			return (&pdev->resource[i]);
		}
	}
	return (NULL);
}

int
platform_get_irq(struct platform_device *pdev, unsigned index)
{
	return (index < pdev->num_irqs ? pdev->irqs[index] : -ENXIO);
}

struct platform_device *
platform_device_register_full(const struct platform_device_info *info)
{
	return (ERR_PTR(-ENODEV));
}

void
platform_device_unregister(struct platform_device *pdev)
{
}

int
of_dma_configure(struct device *dev, struct device_node *np, bool force_dma)
{
	return (0);
}

/* The radio's own address, if the kernel was given one; else the driver takes the firmware's. */
int
linuxkpi_device_get_mac_address(struct device *dev, char *address)
{
	return (lx_os_radio_address((unsigned char *)address) ? 0 : -ENOENT);
}

/* ---- the firmware's memory, handed over by the hypervisor (qmi.c: ath10k_qmi_map_msa_permission()) ---- */

int
qcom_scm_assign_mem(phys_addr_t mem_addr, size_t mem_sz, u64 *src, const struct qcom_scm_vmperm *newvm,
    unsigned int dest_cnt)
{
	bool to_firmware = dest_cnt != 1 || newvm[0].vmid != QCOM_SCM_VMID_HLOS;
	bool secure = to_firmware ? dest_cnt == 2 : !(*src & BIT(QCOM_SCM_VMID_WLAN_CE));
	int ret = lx_os_wlan_memory(mem_addr, mem_sz, secure, to_firmware);

	if (ret == 0) {
		*src = to_firmware ? BIT(QCOM_SCM_VMID_MSS_MSA) | BIT(QCOM_SCM_VMID_WLAN) |
		    (secure ? 0 : BIT(QCOM_SCM_VMID_WLAN_CE)) : BIT(QCOM_SCM_VMID_HLOS);
	}
	return (ret == 0 ? 0 : -EIO);
}

/* The driver's requests to the firmware, on their way out: as they are. */
int
lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure)
{
	return (qmi_send_request(qmi, sq, txn, msg_id, largest, ei, structure));
}

/* ---- the radio's interrupts ------------------------------------------------------------------------- */

/*
 * Linux's way, on the kernel's: an interrupt stays enabled until the driver disables it (snoc.c does from
 * its handler, until its NAPI poll is done). The kernel masks one when it arrives and calls its function on
 * its own thread; so the function calls the handler and enables the line again unless the driver has
 * disabled it meanwhile, or did before.
 */
static struct {
	irq_handler_t handler;
	void *argument;
	int disabled;			/* disable_irq() depth */
	bool attached;
} line[RADIO_IRQS];

static void
irq_arrived(void *argument)
{
	unsigned v = (unsigned)(uintptr_t)argument;

	if (!line[v].attached) {
		return;
	}
	line[v].handler(RADIO_FIRST_INTID + v, line[v].argument);
	if (line[v].disabled == 0) {
		lx_os_irq_enable(v);
	}
}

int
lkpi_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler, irq_handler_t thread, unsigned long flags,
    const char *name, void *arg)
{
	unsigned v = irq - RADIO_FIRST_INTID;
	int ret;

	if (v >= RADIO_IRQS || handler == NULL) {
		return (-EINVAL);
	}
	line[v].handler = handler;
	line[v].argument = arg;
	line[v].disabled = (flags & IRQF_NO_AUTOEN) ? 1 : 0;
	line[v].attached = true;
	if ((ret = lx_os_irq_attach(v, irq_arrived, (void *)(uintptr_t)v)) != 0) {
		line[v].attached = false;
		return (-ret);
	}
	if (line[v].disabled == 0) {
		lx_os_irq_enable(v);
	}
	return (0);
}

int
lkpi_enable_irq(unsigned int irq)
{
	unsigned v = irq - RADIO_FIRST_INTID;

	if (v < RADIO_IRQS && line[v].attached && line[v].disabled > 0 && --line[v].disabled == 0) {
		lx_os_irq_enable(v);
	}
	return (0);
}

void
lkpi_disable_irq(unsigned int irq)
{
	unsigned v = irq - RADIO_FIRST_INTID;

	if (v < RADIO_IRQS && line[v].attached && line[v].disabled++ == 0) {
		lx_os_irq_disable(v);
	}
}

void
lkpi_free_irq(unsigned int irq, void *device)
{
	unsigned v = irq - RADIO_FIRST_INTID;

	if (v < RADIO_IRQS && line[v].attached) {
		lx_os_irq_disable(v);
		line[v].attached = false;
	}
}

/* ---- NAPI: a poll run on a thread of its own until it says it is done --------------------------------- */

static struct napi_struct *volatile napi_wanted[4];

static void
napi_runs(void *argument)
{
	struct napi_struct *napi = argument;

	for (;;) {
		lx_os_lock();
		while (!test_bit(LKPI_NAPI_FLAG_LOST_RACE_TRY_AGAIN, &napi->state) && !test_bit(LKPI_NAPI_FLAG_SHUTDOWN, &napi->state)) {
			lx_os_sleep(napi, 0);
		}
		clear_bit(LKPI_NAPI_FLAG_LOST_RACE_TRY_AGAIN, &napi->state);
		lx_os_unlock();
		if (test_bit(LKPI_NAPI_FLAG_SHUTDOWN, &napi->state)) {
			return;
		}
		/* (the poll asks to be run again by not completing: then it is) */
		while (test_bit(NAPI_STATE_SCHED, &napi->state) && !test_bit(LKPI_NAPI_FLAG_DISABLE_PENDING, &napi->state)) {
			napi->poll(napi, napi->budget);
		}
	}
}

void
linuxkpi_init_dummy_netdev(struct net_device *dev)
{
}

void
linuxkpi_netif_napi_add(struct net_device *dev, struct napi_struct *napi, int (*poll)(struct napi_struct *, int))
{
	napi->dev = dev;
	napi->poll = poll;
	napi->budget = 64;
	napi->state = 0;
	set_bit(LKPI_NAPI_FLAG_DISABLE_PENDING, &napi->state);	/* (disabled until napi_enable()) */
	lx_os_thread("napi", napi_runs, napi);
}

void
linuxkpi_netif_napi_del(struct napi_struct *napi)
{
	lx_os_lock();
	set_bit(LKPI_NAPI_FLAG_SHUTDOWN, &napi->state);
	lx_os_wakeup(napi);
	lx_os_unlock();
}

bool
linuxkpi_napi_schedule_prep(struct napi_struct *napi)
{
	return (!test_bit(LKPI_NAPI_FLAG_DISABLE_PENDING, &napi->state) && !test_and_set_bit(NAPI_STATE_SCHED, &napi->state));
}

void
linuxkpi___napi_schedule(struct napi_struct *napi)
{
	lx_os_lock();
	set_bit(LKPI_NAPI_FLAG_LOST_RACE_TRY_AGAIN, &napi->state);
	lx_os_wakeup(napi);
	lx_os_unlock();
}

bool
linuxkpi_napi_schedule(struct napi_struct *napi)
{
	if (!linuxkpi_napi_schedule_prep(napi)) {
		return (false);
	}
	linuxkpi___napi_schedule(napi);
	return (true);
}

void
linuxkpi_napi_reschedule(struct napi_struct *napi)
{
	linuxkpi_napi_schedule(napi);
}

bool
linuxkpi_napi_complete_done(struct napi_struct *napi, int done)
{
	clear_bit(NAPI_STATE_SCHED, &napi->state);
	return (true);
}

bool
linuxkpi_napi_complete(struct napi_struct *napi)
{
	return (linuxkpi_napi_complete_done(napi, 0));
}

void
linuxkpi_napi_enable(struct napi_struct *napi)
{
	clear_bit(LKPI_NAPI_FLAG_DISABLE_PENDING, &napi->state);
}

void
linuxkpi_napi_disable(struct napi_struct *napi)
{
	set_bit(LKPI_NAPI_FLAG_DISABLE_PENDING, &napi->state);
	while (test_bit(NAPI_STATE_SCHED, &napi->state)) {
		lx_pause_ms(1);
	}
}

void
linuxkpi_napi_synchronize(struct napi_struct *napi)
{
	while (test_bit(NAPI_STATE_SCHED, &napi->state)) {
		lx_pause_ms(1);
	}
}

/* ---- the entries ---------------------------------------------------------------------------------- */

static struct ath10k *volatile driver;	/* the driver, while it is up: between lx_start() and lx_stop() */

int
lx_prepare(void)
{
	return (lx_base_init() != 0 || lx_pool_init() != 0 ? ENOMEM : 0);
}

int
lx_start(void)
{
	struct ath10k_snoc *ar_snoc;
	struct ath10k *ar;
	unsigned long long began;
	bool up;
	int ret;

	if (lx_prepare() != 0) {
		lx_tell("lx: no thread, or no memory the radio can be given\n");
		return (ENOMEM);
	}
	if (!lx_os_option("wifi_quiet")) {
		ath10k_debug_mask = ATH10K_DBG_SNOC | ATH10K_DBG_QMI | ATH10K_DBG_BOOT;
	}
	for (unsigned i = 0; i < RADIO_IRQS; i++) {
		irqs[i] = RADIO_FIRST_INTID + i;
	}
	if (!lx_os_irq_lines(RADIO_FIRST_INTID)) {
		lx_tell("lx: the kernel takes no interrupts for the radio\n");
		return (ENXIO);
	}
	radio.dev.of_node = &node;
	radio.dev.kobj.name = "wifi";
	lx_tell("lx: ath10k's snoc driver probes the radio (%s, registers %#llx, interrupts SPI 414..425, MSA %#llx)\n", compatible,
	    RADIO_REGISTERS, RADIO_MSA);
	if ((ret = lx_platform_driver->probe(&radio)) != 0) {
		lx_tell("lx: the probe fails: %d\n", ret);
		return (-ret);
	}
	ar = platform_get_drvdata(&radio);
	ar_snoc = ath10k_snoc_priv(ar);

	/* the rest happens on the driver's threads: its exchange with the firmware, then its start (snoc.c, core.c) */
	began = lx_os_nanoseconds();
	while (!test_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags) && lx_os_nanoseconds() - began < 60000000000ull) {
		lx_pause_ms(10);
	}
	up = test_bit(ATH10K_SNOC_FLAG_REGISTERED, &ar_snoc->flags);
	lx_tell("lx: %llu ms after the probe the driver %s\n", (lx_os_nanoseconds() - began) / 1000000ull,
	    up ? "IS REGISTERED: the firmware is up and the radio is the 802.11 layer's" : "HAS NOT REGISTERED in 60 s");
	if (!up) {
		return (ETIMEDOUT);
	}
	driver = ar;
	if (lx_os_option("wifi_scan")) {
		lx_station_scan();
	}
	if (lx_os_option("wifi_join")) {
		if ((ret = lx_station_join()) != 0) {
			return (ret);
		}
	}
	lx_station_offer();
	return (0);
}

void
lx_stop(void)
{
	if (driver == NULL) {
		return;
	}
	driver = NULL;
	lx_station_withdraw();
	lx_station_leave();
	lx_platform_driver->remove(&radio);
	lx_tell("lx: the driver is down again\n");
}

int
lx_transmit(const unsigned char *frame, unsigned long length)
{
	return (driver != NULL ? lx_station_send(frame, length) : ENETDOWN);
}
