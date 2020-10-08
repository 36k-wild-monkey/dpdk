/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020 Intel Corporation
 */

#include <rte_bus_pci.h>
#include <rte_memzone.h>

#include "ioat_private.h"
#include "ioat_spec.h"

#define IDXD_VENDOR_ID		0x8086
#define IDXD_DEVICE_ID_SPR	0x0B25

#define IDXD_PMD_RAWDEV_NAME_PCI rawdev_idxd_pci

const struct rte_pci_id pci_id_idxd_map[] = {
	{ RTE_PCI_DEVICE(IDXD_VENDOR_ID, IDXD_DEVICE_ID_SPR) },
	{ .vendor_id = 0, /* sentinel */ },
};

static inline int
idxd_pci_dev_command(struct idxd_rawdev *idxd, enum rte_idxd_cmds command)
{
	uint8_t err_code;
	uint16_t qid = idxd->qid;
	int i = 0;

	if (command >= idxd_disable_wq && command <= idxd_reset_wq)
		qid = (1 << qid);
	rte_spinlock_lock(&idxd->u.pci->lk);
	idxd->u.pci->regs->cmd = (command << IDXD_CMD_SHIFT) | qid;

	do {
		rte_pause();
		err_code = idxd->u.pci->regs->cmdstatus;
		if (++i >= 1000) {
			IOAT_PMD_ERR("Timeout waiting for command response from HW");
			rte_spinlock_unlock(&idxd->u.pci->lk);
			return err_code;
		}
	} while (idxd->u.pci->regs->cmdstatus & CMDSTATUS_ACTIVE_MASK);
	rte_spinlock_unlock(&idxd->u.pci->lk);

	return err_code & CMDSTATUS_ERR_MASK;
}

static int
idxd_is_wq_enabled(struct idxd_rawdev *idxd)
{
	uint32_t state = idxd->u.pci->wq_regs[idxd->qid].wqcfg[WQ_STATE_IDX];
	return ((state >> WQ_STATE_SHIFT) & WQ_STATE_MASK) == 0x1;
}

static const struct rte_rawdev_ops idxd_pci_ops = {
		.dev_close = idxd_rawdev_close,
		.dev_selftest = idxd_rawdev_test,
};

/* each portal uses 4 x 4k pages */
#define IDXD_PORTAL_SIZE (4096 * 4)

static int
init_pci_device(struct rte_pci_device *dev, struct idxd_rawdev *idxd)
{
	struct idxd_pci_common *pci;
	uint8_t nb_groups, nb_engines, nb_wqs;
	uint16_t grp_offset, wq_offset; /* how far into bar0 the regs are */
	uint16_t wq_size, total_wq_size;
	uint8_t lg2_max_batch, lg2_max_copy_size;
	unsigned int i, err_code;

	pci = malloc(sizeof(*pci));
	if (pci == NULL) {
		IOAT_PMD_ERR("%s: Can't allocate memory", __func__);
		goto err;
	}
	rte_spinlock_init(&pci->lk);

	/* assign the bar registers, and then configure device */
	pci->regs = dev->mem_resource[0].addr;
	grp_offset = (uint16_t)pci->regs->offsets[0];
	pci->grp_regs = RTE_PTR_ADD(pci->regs, grp_offset * 0x100);
	wq_offset = (uint16_t)(pci->regs->offsets[0] >> 16);
	pci->wq_regs = RTE_PTR_ADD(pci->regs, wq_offset * 0x100);
	pci->portals = dev->mem_resource[2].addr;

	/* sanity check device status */
	if (pci->regs->gensts & GENSTS_DEV_STATE_MASK) {
		/* need function-level-reset (FLR) or is enabled */
		IOAT_PMD_ERR("Device status is not disabled, cannot init");
		goto err;
	}
	if (pci->regs->cmdstatus & CMDSTATUS_ACTIVE_MASK) {
		/* command in progress */
		IOAT_PMD_ERR("Device has a command in progress, cannot init");
		goto err;
	}

	/* read basic info about the hardware for use when configuring */
	nb_groups = (uint8_t)pci->regs->grpcap;
	nb_engines = (uint8_t)pci->regs->engcap;
	nb_wqs = (uint8_t)(pci->regs->wqcap >> 16);
	total_wq_size = (uint16_t)pci->regs->wqcap;
	lg2_max_copy_size = (uint8_t)(pci->regs->gencap >> 16) & 0x1F;
	lg2_max_batch = (uint8_t)(pci->regs->gencap >> 21) & 0x0F;

	IOAT_PMD_DEBUG("nb_groups = %u, nb_engines = %u, nb_wqs = %u",
			nb_groups, nb_engines, nb_wqs);

	/* zero out any old config */
	for (i = 0; i < nb_groups; i++) {
		pci->grp_regs[i].grpengcfg = 0;
		pci->grp_regs[i].grpwqcfg[0] = 0;
	}
	for (i = 0; i < nb_wqs; i++)
		pci->wq_regs[i].wqcfg[0] = 0;

	/* put each engine into a separate group to avoid reordering */
	if (nb_groups > nb_engines)
		nb_groups = nb_engines;
	if (nb_groups < nb_engines)
		nb_engines = nb_groups;

	/* assign engines to groups, round-robin style */
	for (i = 0; i < nb_engines; i++) {
		IOAT_PMD_DEBUG("Assigning engine %u to group %u",
				i, i % nb_groups);
		pci->grp_regs[i % nb_groups].grpengcfg |= (1ULL << i);
	}

	/* now do the same for queues and give work slots to each queue */
	wq_size = total_wq_size / nb_wqs;
	IOAT_PMD_DEBUG("Work queue size = %u, max batch = 2^%u, max copy = 2^%u",
			wq_size, lg2_max_batch, lg2_max_copy_size);
	for (i = 0; i < nb_wqs; i++) {
		/* add engine "i" to a group */
		IOAT_PMD_DEBUG("Assigning work queue %u to group %u",
				i, i % nb_groups);
		pci->grp_regs[i % nb_groups].grpwqcfg[0] |= (1ULL << i);
		/* now configure it, in terms of size, max batch, mode */
		pci->wq_regs[i].wqcfg[WQ_SIZE_IDX] = wq_size;
		pci->wq_regs[i].wqcfg[WQ_MODE_IDX] = (1 << WQ_PRIORITY_SHIFT) |
				WQ_MODE_DEDICATED;
		pci->wq_regs[i].wqcfg[WQ_SIZES_IDX] = lg2_max_copy_size |
				(lg2_max_batch << WQ_BATCH_SZ_SHIFT);
	}

	/* dump the group configuration to output */
	for (i = 0; i < nb_groups; i++) {
		IOAT_PMD_DEBUG("## Group %d", i);
		IOAT_PMD_DEBUG("    GRPWQCFG: %"PRIx64, pci->grp_regs[i].grpwqcfg[0]);
		IOAT_PMD_DEBUG("    GRPENGCFG: %"PRIx64, pci->grp_regs[i].grpengcfg);
		IOAT_PMD_DEBUG("    GRPFLAGS: %"PRIx32, pci->grp_regs[i].grpflags);
	}

	idxd->u.pci = pci;
	idxd->max_batches = wq_size;

	/* enable the device itself */
	err_code = idxd_pci_dev_command(idxd, idxd_enable_dev);
	if (err_code) {
		IOAT_PMD_ERR("Error enabling device: code %#x", err_code);
		return err_code;
	}
	IOAT_PMD_DEBUG("IDXD Device enabled OK");

	return nb_wqs;

err:
	free(pci);
	return -1;
}

static int
idxd_rawdev_probe_pci(struct rte_pci_driver *drv, struct rte_pci_device *dev)
{
	struct idxd_rawdev idxd = {{0}}; /* Double {} to avoid error on BSD12 */
	uint8_t nb_wqs;
	int qid, ret = 0;
	char name[PCI_PRI_STR_SIZE];

	rte_pci_device_name(&dev->addr, name, sizeof(name));
	IOAT_PMD_INFO("Init %s on NUMA node %d", name, dev->device.numa_node);
	dev->device.driver = &drv->driver;

	ret = init_pci_device(dev, &idxd);
	if (ret < 0) {
		IOAT_PMD_ERR("Error initializing PCI hardware");
		return ret;
	}
	nb_wqs = (uint8_t)ret;

	/* set up one device for each queue */
	for (qid = 0; qid < nb_wqs; qid++) {
		char qname[32];

		/* add the queue number to each device name */
		snprintf(qname, sizeof(qname), "%s-q%d", name, qid);
		idxd.qid = qid;
		idxd.public.portal = RTE_PTR_ADD(idxd.u.pci->portals,
				qid * IDXD_PORTAL_SIZE);
		if (idxd_is_wq_enabled(&idxd))
			IOAT_PMD_ERR("Error, WQ %u seems enabled", qid);
		ret = idxd_rawdev_create(qname, &dev->device,
				&idxd, &idxd_pci_ops);
		if (ret != 0) {
			IOAT_PMD_ERR("Failed to create rawdev %s", name);
			if (qid == 0) /* if no devices using this, free pci */
				free(idxd.u.pci);
			return ret;
		}
	}

	return 0;
}

static int
idxd_rawdev_destroy(const char *name)
{
	int ret;
	uint8_t err_code;
	struct rte_rawdev *rdev;
	struct idxd_rawdev *idxd;

	if (!name) {
		IOAT_PMD_ERR("Invalid device name");
		return -EINVAL;
	}

	rdev = rte_rawdev_pmd_get_named_dev(name);
	if (!rdev) {
		IOAT_PMD_ERR("Invalid device name (%s)", name);
		return -EINVAL;
	}

	idxd = rdev->dev_private;

	/* disable the device */
	err_code = idxd_pci_dev_command(idxd, idxd_disable_dev);
	if (err_code) {
		IOAT_PMD_ERR("Error disabling device: code %#x", err_code);
		return err_code;
	}
	IOAT_PMD_DEBUG("IDXD Device disabled OK");

	/* free device memory */
	if (rdev->dev_private != NULL) {
		IOAT_PMD_DEBUG("Freeing device driver memory");
		rdev->dev_private = NULL;
		rte_free(idxd->public.batch_ring);
		rte_free(idxd->public.hdl_ring);
		rte_memzone_free(idxd->mz);
	}

	/* rte_rawdev_close is called by pmd_release */
	ret = rte_rawdev_pmd_release(rdev);
	if (ret)
		IOAT_PMD_DEBUG("Device cleanup failed");

	return 0;
}

static int
idxd_rawdev_remove_pci(struct rte_pci_device *dev)
{
	char name[PCI_PRI_STR_SIZE];
	int ret = 0;

	rte_pci_device_name(&dev->addr, name, sizeof(name));

	IOAT_PMD_INFO("Closing %s on NUMA node %d",
			name, dev->device.numa_node);

	ret = idxd_rawdev_destroy(name);

	return ret;
}

struct rte_pci_driver idxd_pmd_drv_pci = {
	.id_table = pci_id_idxd_map,
	.drv_flags = RTE_PCI_DRV_NEED_MAPPING,
	.probe = idxd_rawdev_probe_pci,
	.remove = idxd_rawdev_remove_pci,
};

RTE_PMD_REGISTER_PCI(IDXD_PMD_RAWDEV_NAME_PCI, idxd_pmd_drv_pci);
RTE_PMD_REGISTER_PCI_TABLE(IDXD_PMD_RAWDEV_NAME_PCI, pci_id_idxd_map);
RTE_PMD_REGISTER_KMOD_DEP(IDXD_PMD_RAWDEV_NAME_PCI,
			  "* igb_uio | uio_pci_generic | vfio-pci");