// SPDX-License-Identifier: MIT
/*
 * Copyright (c) 2026 Intel Corporation. All rights reserved.
 */

#ifndef __XE_MEI_DG2_H__
#define __XE_MEI_DG2_H__

#include "xe_device.h"
#include "xe_gt.h"

struct xe_mei_dg2 {
	struct xe_device *xe;
	struct xe_gt *gt;
	struct mei_aux_device *adev[2];
	struct xe_bo *lmem_bo;
	int irq; /* Виртуальное прерывание для маршрутизации */
};

int xe_mei_dg2_init(struct xe_device *xe);
int xe_mei_dg2_auth_huc(struct xe_device *xe, struct xe_huc *huc);
void xe_mei_dg2_fini(struct xe_device *xe);

#endif /* ___XE_MEI_DG2_H__ */
