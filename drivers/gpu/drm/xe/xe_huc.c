// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_huc.h"

#include <linux/delay.h>

#include <drm/drm_managed.h>

#include "abi/gsc_pxp_commands_abi.h"
#include "regs/xe_gsc_regs.h"
#include "regs/xe_guc_regs.h"
#include "xe_bo.h"
#include "xe_device.h"
#include "xe_force_wake.h"
#include "xe_gsc_submit.h"
#include "xe_gt.h"
#include "xe_gt_printk.h"
#include "xe_guc.h"
#include "xe_map.h"
#include "xe_mmio.h"
#include "xe_sriov.h"
#include "xe_uc_fw.h"
#include "xe_pm.h"
#include "xe_mei_dg2.h"

static struct xe_gt *
huc_to_gt(struct xe_huc *huc)
{
	return container_of(huc, struct xe_gt, uc.huc);
}

static struct xe_device *
huc_to_xe(struct xe_huc *huc)
{
	return gt_to_xe(huc_to_gt(huc));
}

static struct xe_guc *
huc_to_guc(struct xe_huc *huc)
{
	return &container_of(huc, struct xe_uc, huc)->guc;
}

static int huc_auth_via_gsccs(struct xe_huc *huc);

static const struct {
	const char *name;
	struct xe_reg reg;
	u32 val;
} huc_auth_modes[XE_HUC_AUTH_TYPES_COUNT] = {
	[XE_HUC_AUTH_VIA_GUC] = { "GuC",
				  HUC_KERNEL_LOAD_INFO,
				  HUC_LOAD_SUCCESSFUL },
	[XE_HUC_AUTH_VIA_GSC] = { "GSC",
				  HECI_FWSTS5(MTL_GSC_HECI1_BASE),
				  HECI1_FWSTS5_HUC_AUTH_DONE },
};

static void xe_huc_auth_work(struct work_struct *work)
{
	struct xe_huc *huc = container_of(work, struct xe_huc, auth_work.work);
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	int ret;

	if (xe->info.platform == XE_DG2)
		ret = xe_mei_dg2_auth_huc(xe, huc);
	else
		ret = huc_auth_via_gsccs(huc);

	if (ret == 0) {
		if (xe->info.platform == XE_DG2) {
			ret = xe_mmio_wait32(&gt->mmio, huc_auth_modes[XE_HUC_AUTH_VIA_GSC].reg,
					     huc_auth_modes[XE_HUC_AUTH_VIA_GSC].val,
					     huc_auth_modes[XE_HUC_AUTH_VIA_GSC].val,
					     100000, NULL, false);
			if (ret) {
				xe_gt_err(gt, "HuC: delayed MEI verification failed: %pe\n", ERR_PTR(ret));
				goto fail;
			}
		}

		xe_pm_runtime_get(xe);
		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_RUNNING);
		xe_pm_runtime_put(xe);
		xe_info(xe, "HuC: delayed authentication successful!\n");
		return;
	}

	if (ret == -ENODEV || ret == -EAGAIN) {
		schedule_delayed_work(&huc->auth_work, msecs_to_jiffies(5000));
		return;
	}

fail:
	xe_gt_err(gt, "HuC: Deferred authentication failed with error: %d\n", ret);
	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_LOAD_FAIL);
}

#define PXP43_HUC_AUTH_INOUT_SIZE SZ_4K
static int huc_alloc_gsc_pkt(struct xe_huc *huc)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_bo *bo;

	bo = xe_managed_bo_create_pin_map(xe, gt_to_tile(gt),
					  PXP43_HUC_AUTH_INOUT_SIZE * 2,
					  XE_BO_FLAG_SYSTEM |
					  XE_BO_FLAG_GGTT);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	huc->gsc_pkt = bo;

	return 0;
}

int xe_huc_init(struct xe_huc *huc)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	int ret;

	INIT_DELAYED_WORK(&huc->auth_work, xe_huc_auth_work);

	huc->fw.type = XE_UC_FW_TYPE_HUC;

	if (!xe_gt_is_media_type(gt) && GRAPHICS_VERx100(xe) > 1255) {
		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_NOT_SUPPORTED);
		return 0;
	}

	ret = xe_uc_fw_init(&huc->fw);
	if (ret)
		goto out;

	if (!xe_uc_fw_is_enabled(&huc->fw))
		return 0;

	if (IS_SRIOV_VF(xe))
		return 0;

	if (huc->fw.has_gsc_headers) {
		ret = huc_alloc_gsc_pkt(huc);
		if (ret)
			goto out;
	}

	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_LOADABLE);

	return 0;

out:
	xe_gt_err(gt, "HuC: initialization failed: %pe\n", ERR_PTR(ret));
	return ret;
}

int xe_huc_init_post_hwconfig(struct xe_huc *huc)
{
	struct xe_tile *tile = gt_to_tile(huc_to_gt(huc));
	struct xe_device *xe = huc_to_xe(huc);
	int ret;

	if (!IS_DGFX(huc_to_xe(huc)))
		return 0;

	if (!xe_uc_fw_is_loadable(&huc->fw))
		return 0;

	ret = xe_managed_bo_reinit_in_vram(xe, tile, &huc->fw.bo);
	if (ret)
		return ret;

	return 0;
}

int xe_huc_upload(struct xe_huc *huc)
{
	if (!xe_uc_fw_is_loadable(&huc->fw))
		return 0;
	return xe_uc_fw_upload(&huc->fw, 0, HUC_UKERNEL);
}

#define huc_auth_msg_wr(xe_, map_, offset_, field_, val_) \
	xe_map_wr_field(xe_, map_, offset_, struct pxp43_new_huc_auth_in, field_, val_)
#define huc_auth_msg_rd(xe_, map_, offset_, field_) \
	xe_map_rd_field(xe_, map_, offset_, struct pxp43_huc_auth_out, field_)

static u32 huc_emit_pxp_auth_msg(struct xe_device *xe, struct iosys_map *map,
				 u32 wr_offset, u32 huc_offset, u32 huc_size)
{
	xe_map_memset(xe, map, wr_offset, 0, sizeof(struct pxp43_new_huc_auth_in));

	huc_auth_msg_wr(xe, map, wr_offset, header.api_version, PXP_APIVER(4, 3));
	huc_auth_msg_wr(xe, map, wr_offset, header.command_id, PXP43_CMDID_NEW_HUC_AUTH);
	huc_auth_msg_wr(xe, map, wr_offset, header.status, 0);
	huc_auth_msg_wr(xe, map, wr_offset, header.buffer_len,
			sizeof(struct pxp43_new_huc_auth_in) - sizeof(struct pxp_cmd_header));
	huc_auth_msg_wr(xe, map, wr_offset, huc_base_address, huc_offset);
	huc_auth_msg_wr(xe, map, wr_offset, huc_size, huc_size);

	return wr_offset + sizeof(struct pxp43_new_huc_auth_in);
}

static int huc_auth_via_gsccs(struct xe_huc *huc)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_bo *pkt = huc->gsc_pkt;
	u32 wr_offset;
	u32 rd_offset;
	u64 ggtt_offset;
	u32 out_status;
	int retry = 5;
	int err = 0;

	if (!pkt)
		return -ENODEV;

	ggtt_offset = xe_bo_ggtt_addr(pkt);

	wr_offset = xe_gsc_emit_header(xe, &pkt->vmap, 0, HECI_MEADDRESS_PXP, 0,
				       sizeof(struct pxp43_new_huc_auth_in));
	wr_offset = huc_emit_pxp_auth_msg(xe, &pkt->vmap, wr_offset,
					  xe_bo_ggtt_addr(huc->fw.bo),
					  xe_bo_size(huc->fw.bo));
	do {
		err = xe_gsc_pkt_submit_kernel(&gt->uc.gsc, ggtt_offset, wr_offset,
					       ggtt_offset + PXP43_HUC_AUTH_INOUT_SIZE,
					       PXP43_HUC_AUTH_INOUT_SIZE);
		if (err)
			break;

		if (xe_gsc_check_and_update_pending(xe, &pkt->vmap, 0, &pkt->vmap,
						    PXP43_HUC_AUTH_INOUT_SIZE)) {
			err = -EBUSY;
			msleep(50);
		}
	} while (--retry && err == -EBUSY);

	if (err) {
		xe_gt_err(gt, "HuC: failed to submit GSC request to auth: %pe\n", ERR_PTR(err));
		return err;
	}

	err = xe_gsc_read_out_header(xe, &pkt->vmap, PXP43_HUC_AUTH_INOUT_SIZE,
				     sizeof(struct pxp43_huc_auth_out), &rd_offset);
	if (err) {
		xe_gt_err(gt, "HuC: invalid GSC reply for auth: %pe\n", ERR_PTR(err));
		return err;
	}

	out_status = huc_auth_msg_rd(xe, &pkt->vmap, rd_offset, header.status);
	if (out_status != PXP_STATUS_SUCCESS && out_status != PXP_STATUS_OP_NOT_PERMITTED) {
		xe_gt_err(gt, "HuC: authentication failed with GSC error = %#x\n", out_status);
		return -EIO;
	}

	return 0;
}

bool xe_huc_is_authenticated(struct xe_huc *huc, enum xe_huc_auth_types type)
{
	struct xe_gt *gt = huc_to_gt(huc);

	return xe_mmio_read32(&gt->mmio, huc_auth_modes[type].reg) & huc_auth_modes[type].val;
}

int xe_huc_auth(struct xe_huc *huc, enum xe_huc_auth_types type)
{
	struct xe_gt *gt = huc_to_gt(huc);
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_guc *guc = huc_to_guc(huc);
	int ret;

	if (!xe_uc_fw_is_loadable(&huc->fw))
		return 0;

	if (xe_huc_is_authenticated(huc, type)) {
		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_RUNNING);
		return 0;
	}

	if (!xe_uc_fw_is_loaded(&huc->fw))
		return -ENOEXEC;

	/* ЖЕСТКИЙ ПЕРЕХВАТ ДЛЯ DG2: Игнорируем запрашиваемый тип, идем через MEI */
	if (xe->info.platform == XE_DG2) {
		ret = xe_mei_dg2_auth_huc(xe, huc);
		if (ret) {
			if (ret == -ENODEV || ret == -EAGAIN) {
				xe_info(xe, "HuC: MEI bridge not ready, deferring authentication\n");
				schedule_delayed_work(&huc->auth_work, msecs_to_jiffies(1000));
				return 0;
			}
			xe_gt_err(gt, "HuC: failed to trigger auth via MEI: %pe\n", ERR_PTR(ret));
			goto fail;
		}
		
		/* Ждем подтверждения аппаратуры, что статус изменился на RUNNING */
		ret = xe_mmio_wait32(&gt->mmio, huc_auth_modes[XE_HUC_AUTH_VIA_GSC].reg,
				     huc_auth_modes[XE_HUC_AUTH_VIA_GSC].val,
				     huc_auth_modes[XE_HUC_AUTH_VIA_GSC].val,
				     100000, NULL, false);
		if (ret) {
			xe_gt_err(gt, "HuC: firmware not verified by MEI: %pe\n", ERR_PTR(ret));
			goto fail;
		}

		xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_RUNNING);
		xe_info(xe, "HuC: authenticated successfully via MEI GSC Bridge!\n");
		return 0;
	}

	switch (type) {
	case XE_HUC_AUTH_VIA_GUC:
		ret = xe_guc_auth_huc(guc, xe_bo_ggtt_addr(huc->fw.bo) +
				      xe_uc_fw_rsa_offset(&huc->fw));
		break;
	case XE_HUC_AUTH_VIA_GSC:
		ret = huc_auth_via_gsccs(huc);
		break;
	default:
		XE_WARN_ON(type);
		return -EINVAL;
	}

	if (ret) {
		xe_gt_err(gt, "HuC: failed to trigger auth via %s: %pe\n",
			  huc_auth_modes[type].name, ERR_PTR(ret));
		goto fail;
	}

	ret = xe_mmio_wait32(&gt->mmio, huc_auth_modes[type].reg, huc_auth_modes[type].val,
			     huc_auth_modes[type].val, 500000, NULL, false);
	if (ret) {
		xe_gt_err(gt, "HuC: firmware not verified: %pe\n", ERR_PTR(ret));
		goto fail;
	}

	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_RUNNING);
	xe_gt_dbg(gt, "HuC: authenticated via %s\n", huc_auth_modes[type].name);

	return 0;

fail:
	xe_gt_err(gt, "HuC: authentication failed: %pe\n", ERR_PTR(ret));
	xe_uc_fw_change_status(&huc->fw, XE_UC_FIRMWARE_LOAD_FAIL);

	return ret;
}

void xe_huc_sanitize(struct xe_huc *huc)
{
	xe_uc_fw_sanitize(&huc->fw);
}

void xe_huc_print_info(struct xe_huc *huc, struct drm_printer *p)
{
	struct xe_gt *gt = huc_to_gt(huc);

	xe_uc_fw_print(&huc->fw, p);

	if (!xe_uc_fw_is_enabled(&huc->fw))
		return;

	CLASS(xe_force_wake, fw_ref)(gt_to_fw(gt), XE_FW_GT);
	if (!fw_ref.domains)
		return;

	drm_printf(p, "\nHuC status: 0x%08x\n",
		   xe_mmio_read32(&gt->mmio, HUC_KERNEL_LOAD_INFO));
}