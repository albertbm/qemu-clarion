/*
 * Clarion QY7221NL: synthetic body control module (BCM) answering the
 * head unit's antitheft exchange on CAN (0x71E request / 0x72E answer).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_CLARION_BCM_H
#define HW_MISC_CLARION_BCM_H

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_CLARION_BCM "clarion-bcm"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionBcmState, CLARION_BCM)

/*
 * Attach the bus: @scpu is the sub-CPU transport frames travel through
 * (clarion-scpu).  The BCM registers itself as the scpu's CAN sink.
 */
void clarion_bcm_attach(DeviceState *bcm, DeviceState *scpu);

#endif
