// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Intel Corporation. All rights reserved.
 */

#include <linux/device.h>
#include <linux/irq.h>
#include <linux/mei_aux.h>
#include <drm/drm_managed.h>
#include <uapi/drm/xe_drm.h>
#include <linux/mei_cl_bus.h>
#include <linux/string.h>
#include <linux/pci.h>

#include "xe_mei_dg2.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_mmio.h"
#include "xe_printk.h"
#include "xe_pxp.h"
#include "xe_pxp_submit.h"
#include "xe_pxp_types.h"
#include "regs/xe_regs.h"

/* PXP Interface Definitions */
#define PXP_APIVER(x, y) (((x) & 0xFFFF) << 16 | ((y) & 0xFFFF))

struct pxp_cmd_header {
	u32 api_version;
	u32 command_id;
	u32 status_stream_id;
	u32 buffer_len;
} __packed;

#define PXP43_CMDID_START_HUC_AUTH 0x0000003A

struct pxp43_start_huc_auth_in {
	struct pxp_cmd_header header;
	__le64 huc_base_address;
} __packed;

#define DG2_GSC_HECI1_BASE      0x00373000
#define GSC_BAR_LENGTH          0x00000FFC

static void xe_mei_dg2_release_dev(struct device *dev)
{
	struct auxiliary_device *aux_dev = to_auxiliary_dev(dev);
	struct mei_aux_device *adev = auxiliary_dev_to_mei_aux_dev(aux_dev);
	kfree(adev);
}

/* Фиктивный обработчик прерываний для обмана ядра */
static void gsc_irq_mask(struct irq_data *d) {}
static void gsc_irq_unmask(struct irq_data *d) {}

static struct irq_chip gsc_irq_chip = {
	.name = "gsc_irq_chip",
	.irq_mask = gsc_irq_mask,
	.irq_unmask = gsc_irq_unmask,
};

static int xe_mei_dg2_init_one(struct xe_mei_dg2 *mei, unsigned int intf_id)
{
	struct pci_dev *pdev = to_pci_dev(mei->xe->drm.dev);
	struct mei_aux_device *adev;
	struct auxiliary_device *aux_dev;
	struct xe_tile *tile = xe_device_get_root_tile(mei->xe);
	int ret;

	adev = kzalloc(sizeof(*adev), GFP_KERNEL);
	if (!adev) return -ENOMEM;

	mei->lmem_bo = xe_bo_create_pin_map_novm(mei->xe, tile, SZ_4M,
						 ttm_bo_type_kernel,
					  XE_BO_FLAG_STOLEN | XE_BO_FLAG_GGTT, false);
	if (IS_ERR(mei->lmem_bo)) {
		ret = PTR_ERR(mei->lmem_bo);
		goto fail;
	}

	adev->ext_op_mem.start = xe_bo_ggtt_addr(mei->lmem_bo);
	adev->ext_op_mem.end = adev->ext_op_mem.start + SZ_4M;

	/* ВЫДЕЛЯЕМ ВИРТУАЛЬНОЕ ПРЕРЫВАНИЕ КАК В i915 */
	mei->irq = irq_alloc_desc(0);
	if (mei->irq < 0) {
		ret = mei->irq;
		goto fail;
	}
	irq_set_chip_and_handler_name(mei->irq, &gsc_irq_chip,
				      handle_simple_irq, "gsc_irq_handler");
	irq_set_chip_data(mei->irq, NULL);
	adev->irq = mei->irq;

	adev->bar.parent = &pdev->resource[0];
	adev->bar.start = DG2_GSC_HECI1_BASE + pdev->resource[0].start;
	adev->bar.end = adev->bar.start + GSC_BAR_LENGTH - 1;
	adev->bar.flags = IORESOURCE_MEM;
	adev->bar.desc = IORES_DESC_NONE;

	aux_dev = &adev->aux_dev;
	aux_dev->name = "mei-gsc";
	aux_dev->id = (pci_domain_nr(pdev->bus) << 16) | PCI_DEVID(pdev->bus->number, pdev->devfn);
	aux_dev->dev.parent = &pdev->dev;
	aux_dev->dev.release = xe_mei_dg2_release_dev;

	ret = auxiliary_device_init(aux_dev);
	if (ret < 0) goto fail;

	ret = auxiliary_device_add(aux_dev);
	if (ret < 0) {
		auxiliary_device_uninit(aux_dev);
		goto fail;
	}

	mei->adev[intf_id] = adev;
	return 0;

	fail:
	kfree(adev);
	return ret;
}

int xe_mei_dg2_init(struct xe_device *xe)
{
	struct xe_mei_dg2 *mei;
	int ret;

	if (xe->info.platform != XE_DG2) return 0;

	mei = devm_kzalloc(xe->drm.dev, sizeof(*mei), GFP_KERNEL);
	if (!mei) return -ENOMEM;

	mei->xe = xe;
	mei->irq = -1;
	xe->mei_dg2 = mei;

	ret = xe_mei_dg2_init_one(mei, 0);
	if (ret) return ret;

	return 0;
}

static int match_pxp_client(struct device *dev, void *data)
{
	struct mei_cl_device *cldev;
	if (!dev->bus || !dev->bus->name || strcmp(dev->bus->name, "mei") != 0)
		return 0;

	cldev = to_mei_cl_device(dev);
	if (strstr(cldev->name, "fbf6fcf1")) {
		struct device **result = data;
		*result = dev;
		return 1;
	}
	return 0;
}

static int find_pxp_client_deep(struct device *dev, void *data)
{
	if (match_pxp_client(dev, data)) return 1;
	return device_for_each_child(dev, data, find_pxp_client_deep);
}

static int xe_mei_dg2_pxp_send_recv(struct xe_mei_dg2 *mei, u32 cmd_id,
				    void *in, size_t in_len, void *out, size_t out_len)
{
	struct mei_aux_device *adev;
	struct device *dev, *cl_dev = NULL;
	struct mei_cl_device *cldev;
	ssize_t byte;
	int ret;

	if (!mei || !mei->adev[0]) return -EAGAIN;

	adev = mei->adev[0];
	dev = &adev->aux_dev.dev;

	device_for_each_child(dev, &cl_dev, find_pxp_client_deep);
	if (!cl_dev) return -EAGAIN;

	get_device(cl_dev);
	cldev = to_mei_cl_device(cl_dev);

	if (!mei_cldev_enabled(cldev)) {
		ret = mei_cldev_enable(cldev);
		if (ret) { put_device(cl_dev); return -EAGAIN; }
	}

	byte = mei_cldev_send(cldev, (u8 *)in, in_len);
	if (byte < 0) { put_device(cl_dev); return byte; }

	if (out && out_len > 0) {
		byte = mei_cldev_recv(cldev, (u8 *)out, out_len);
		if (byte < 0) { put_device(cl_dev); return byte; }
	}

	put_device(cl_dev);
	return 0;
}

int xe_mei_dg2_auth_huc(struct xe_device *xe, struct xe_huc *huc)
{
	struct xe_mei_dg2 *mei = xe->mei_dg2;
	struct pxp43_start_huc_auth_in in = {0};
	int ret;

	if (!mei || !mei->adev[0]) return -ENODEV;

	in.header.api_version = PXP_APIVER(4, 3);
	in.header.command_id = PXP43_CMDID_START_HUC_AUTH;
	in.header.buffer_len = sizeof(in) - sizeof(struct pxp_cmd_header);
	in.huc_base_address = cpu_to_le64(xe_bo_ggtt_addr(huc->fw.bo));

	ret = xe_mei_dg2_pxp_send_recv(mei, PXP43_CMDID_START_HUC_AUTH,
				       &in, sizeof(in), NULL, 0);
	if (ret) return ret;

	return 0;
}

void xe_mei_dg2_fini(struct xe_device *xe)
{
	struct xe_mei_dg2 *mei = xe->mei_dg2;

	if (!mei) return;

	if (mei->lmem_bo)
		xe_bo_unpin_map_no_vm(mei->lmem_bo);

	if (mei->adev[0]) {
		auxiliary_device_delete(&mei->adev[0]->aux_dev);
		auxiliary_device_uninit(&mei->adev[0]->aux_dev);
	}

	if (mei->irq >= 0)
		irq_free_desc(mei->irq);
}
