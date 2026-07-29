/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2026, Intel Corporation. All rights reserved.
 */
#ifndef _XE_HECI_GSC_H_
#define _XE_HECI_GSC_H_

#include <linux/types.h>

struct xe_device;
struct mei_aux_device;
struct xe_bo;

#define GSC_IRQ_INTF(_x) BIT(15 - (_x))
#define CSC_IRQ_INTF(_x) BIT(9 + (_x))

struct xe_heci_gsc {
	struct mei_aux_device *adev[2];
	int irq[2];
	struct xe_bo *gem_obj[2];
};

int xe_heci_gsc_init(struct xe_device *xe);
void xe_heci_gsc_init_heci1(struct xe_device *xe); /* ДОБАВЛЕНО */
void xe_heci_gsc_irq_handler(struct xe_device *xe, u32 iir);
void xe_heci_csc_irq_handler(struct xe_device *xe, u32 iir);

#endif /* _XE_HECI_GSC_H_ */
