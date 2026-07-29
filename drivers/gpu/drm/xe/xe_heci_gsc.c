// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2026, Intel Corporation. All rights reserved.
 */

#include <linux/irq.h>
#include <linux/mei_aux.h>
#include <linux/pci.h>
#include <linux/sizes.h>

#include <drm/drm_print.h>

#include "xe_device_types.h"
#include "xe_heci_gsc.h"
#include "regs/xe_gsc_regs.h"
#include "xe_platform_types.h"
#include "xe_survivability_mode.h"
#include "xe_gt.h"
#include "xe_mmio.h"

#define GSC_BAR_LENGTH  0x00000FFC

static void heci_gsc_irq_mask(struct irq_data *d) {}
static void heci_gsc_irq_unmask(struct irq_data *d) {}

static const struct irq_chip heci_gsc_irq_chip = {
	.name = "gsc_irq_chip",
	.irq_mask = heci_gsc_irq_mask,
	.irq_unmask = heci_gsc_irq_unmask,
};

static int heci_gsc_irq_init(int irq)
{
	irq_set_chip_and_handler_name(irq, &heci_gsc_irq_chip,
				      handle_simple_irq, "heci_gsc_irq_handler");
	return irq_set_chip_data(irq, NULL);
}

struct heci_gsc_def {
	const char *name;
	unsigned long bar;
	size_t bar_size;
	bool use_polling;
	bool slow_firmware;
};

static const struct heci_gsc_def heci_gsc_def_dg1 = {
	.name = "mei-gscfi",
	.bar = DG1_GSC_HECI2_BASE,
	.bar_size = GSC_BAR_LENGTH,
};

/* Регистрируем оба HECI устройства БЕЗ дублирующего выделения памяти */
static const struct heci_gsc_def heci_gsc_def_dg2[] = {
	{
		.name = "mei-gsc",
		.bar = DG2_GSC_HECI1_BASE,
		.bar_size = GSC_BAR_LENGTH,
	},
	{
		.name = "mei-gscfi",
		.bar = DG2_GSC_HECI2_BASE,
		.bar_size = GSC_BAR_LENGTH,
	}
};

static const struct heci_gsc_def heci_gsc_def_pvc = {
	.name = "mei-gscfi",
	.bar = PVC_GSC_HECI2_BASE,
	.bar_size = GSC_BAR_LENGTH,
	.slow_firmware = true,
};

static void heci_gsc_release_dev(struct device *dev)
{
	struct auxiliary_device *aux_dev = to_auxiliary_dev(dev);
	struct mei_aux_device *adev = auxiliary_dev_to_mei_aux_dev(aux_dev);
	kfree(adev);
}

static void xe_heci_gsc_fini(void *arg)
{
	struct xe_heci_gsc *heci_gsc = arg;
	int i;

	for (i = 0; i < 2; i++) {
		if (heci_gsc->adev[i]) {
			struct auxiliary_device *aux_dev = &heci_gsc->adev[i]->aux_dev;
			auxiliary_device_delete(aux_dev);
			auxiliary_device_uninit(aux_dev);
			heci_gsc->adev[i] = NULL;
		}
		if (heci_gsc->irq[i] >= 0)
			irq_free_desc(heci_gsc->irq[i]);
		heci_gsc->irq[i] = -1;
	}
}

static int heci_gsc_irq_setup(struct xe_device *xe)
{
	struct xe_heci_gsc *heci_gsc = &xe->heci_gsc;
	int i, ret;

	for (i = 0; i < 2; i++) {
		heci_gsc->irq[i] = irq_alloc_desc(0);
		if (heci_gsc->irq[i] < 0) {
			drm_err(&xe->drm, "gsc irq error %d\n", heci_gsc->irq[i]);
			return heci_gsc->irq[i];
		}
		ret = heci_gsc_irq_init(heci_gsc->irq[i]);
		if (ret < 0)
			drm_err(&xe->drm, "gsc irq init failed %d\n", ret);
	}

	return 0;
}

static int heci_gsc_add_device(struct xe_device *xe, const struct heci_gsc_def *def, int intf_id)
{
	struct xe_heci_gsc *heci_gsc = &xe->heci_gsc;
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct auxiliary_device *aux_dev;
	struct mei_aux_device *adev;
	int ret;

	adev = kzalloc_obj(*adev);
	if (!adev)
		return -ENOMEM;
	adev->irq = heci_gsc->irq[intf_id];
	adev->bar.parent = &pdev->resource[0];
	adev->bar.start = def->bar + pdev->resource[0].start;
	adev->bar.end = adev->bar.start + def->bar_size - 1;
	adev->bar.flags = IORESOURCE_MEM;
	adev->bar.desc = IORES_DESC_NONE;
	adev->slow_firmware = def->slow_firmware;

	aux_dev = &adev->aux_dev;
	aux_dev->name = def->name;
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) | PCI_DEVID(pdev->bus->number, pdev->devfn) | (intf_id << 8);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = heci_gsc_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret < 0) {
		drm_err(&xe->drm, "gsc aux init failed %d\n", ret);
		kfree(adev);
		return ret;
	}

	heci_gsc->adev[intf_id] = adev;
	ret = auxiliary_device_add(aux_dev);
	if (ret < 0) {
		drm_err(&xe->drm, "gsc aux add failed %d\n", ret);
		heci_gsc->adev[intf_id] = NULL;
		auxiliary_device_uninit(aux_dev);
	}
	return ret;
}

int xe_heci_gsc_init(struct xe_device *xe)
{
	struct xe_heci_gsc *heci_gsc = &xe->heci_gsc;
	int ret;

	if (!xe->info.has_heci_gscfi && !xe->info.has_heci_cscfi)
		return 0;

	heci_gsc->irq[0] = -1;
	heci_gsc->irq[1] = -1;

	ret = devm_add_action_or_reset(xe->drm.dev, xe_heci_gsc_fini, heci_gsc);
	if (ret)
		return ret;

	if (!xe_survivability_mode_is_boot_enabled(xe)) {
		ret = heci_gsc_irq_setup(xe);
		if (ret)
			return ret;
	}

	if (xe->info.platform == XE_DG2) {
		struct xe_gt *gt = xe->tiles[0].primary_gt;
		u32 mask, en;

		mask = xe_mmio_read32(&gt->mmio, XE_REG(0x1900f4));
		mask &= ~BIT(15);
		mask &= ~BIT(14);
		xe_mmio_write32(&gt->mmio, XE_REG(0x1900f4), mask);

		en = xe_mmio_read32(&gt->mmio, XE_REG(0x190044));
		en |= BIT(15);
		en |= BIT(14);
		xe_mmio_write32(&gt->mmio, XE_REG(0x190044), en);

		heci_gsc_add_device(xe, &heci_gsc_def_dg2[0], 0);
		heci_gsc_add_device(xe, &heci_gsc_def_dg2[1], 1);
		return 0;
	} else if (xe->info.platform == XE_BATTLEMAGE) {
		return heci_gsc_add_device(xe, &heci_gsc_def_dg2[1], 1);
	} else if (xe->info.platform == XE_PVC) {
		return heci_gsc_add_device(xe, &heci_gsc_def_pvc, 1);
	} else if (xe->info.platform == XE_DG1) {
		return heci_gsc_add_device(xe, &heci_gsc_def_dg1, 1);
	}

	drm_warn(&xe->drm, "HECI is not implemented!\n");
	return 0;
}

void xe_heci_gsc_irq_handler(struct xe_device *xe, u32 iir)
{
	if (!xe->info.has_heci_gscfi) {
		drm_warn_once(&xe->drm, "GSC irq: not supported");
		return;
	}

	if (iir & GSC_IRQ_INTF(0) && xe->heci_gsc.irq[0] >= 0) {
		if (generic_handle_irq_safe(xe->heci_gsc.irq[0]))
			drm_err_ratelimited(&xe->drm, "error handling GSC irq 0\n");
	}

	if (iir & GSC_IRQ_INTF(1) && xe->heci_gsc.irq[1] >= 0) {
		if (generic_handle_irq_safe(xe->heci_gsc.irq[1]))
			drm_err_ratelimited(&xe->drm, "error handling GSC irq 1\n");
	}
}

void xe_heci_csc_irq_handler(struct xe_device *xe, u32 iir)
{
	if ((iir & CSC_IRQ_INTF(1)) == 0)
		return;

	if (!xe->info.has_heci_cscfi) {
		drm_warn_once(&xe->drm, "CSC irq: not supported");
		return;
	}

	if (xe->heci_gsc.irq[1] < 0)
		return;

	if (generic_handle_irq_safe(xe->heci_gsc.irq[1]))
		drm_err_ratelimited(&xe->drm, "error handling CSC irq\n");
}
