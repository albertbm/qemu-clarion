/*
 * Micom, the Clarion QY8XXX board's companion microcontroller, linked over SCIF4.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_CLARION_MICOM_H
#define HW_MISC_CLARION_MICOM_H

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_CLARION_MICOM "clarion-micom"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionMicomState, CLARION_MICOM)

/*
 * Deliver bytes from the micom. Returns the number ACCEPTED: the SCIF RX FIFO
 * holds 16 bytes, so a long frame (a command set has 62 data bytes) will not
 * fit at once. The micom retains and sends the remainder when DMA frees space,
 * as a real link would.
 */
typedef int (*ClarionMicomSink)(void *opaque, const uint8_t *buf, int len);

void clarion_micom_set_sink(DeviceState *dev, ClarionMicomSink fn,
                            void *opaque);

/* Byte from the guest (write to SCFTDR of the SCIF connected to the micom). */
void clarion_micom_rx_byte(DeviceState *dev, uint8_t b);

#endif
