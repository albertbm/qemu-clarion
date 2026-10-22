/*
 * Display Micom, the Clarion QY8XXX panel microcontroller, linked over SCIF1.
 *
 * Separate from the board's companion MCU (clarion_micom, SCIF4): different
 * link, protocol, and guest driver (lcddrv.dll vs. EdaDrv.dll).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_CLARION_DISPMICOM_H
#define HW_MISC_CLARION_DISPMICOM_H

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_CLARION_DISPMICOM "clarion-dispmicom"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionDispMicomState, CLARION_DISPMICOM)

/*
 * Deliver bytes from the panel. Returns the number ACCEPTED: the SCIF RX FIFO
 * holds 16 bytes, while a 0x24-byte frame plus its wrapper is 20, so the model
 * sends the remainder after the driver drains the FIFO.
 */
typedef int (*ClarionDispMicomSink)(void *opaque, const uint8_t *buf, int len);
typedef void (*ClarionDispMicomCanKey)(void *opaque, uint8_t code, bool down);

void clarion_dispmicom_set_sink(DeviceState *dev, ClarionDispMicomSink fn,
                                void *opaque);
void clarion_dispmicom_set_can_key_sink(DeviceState *dev,
                                        ClarionDispMicomCanKey fn,
                                        void *opaque);

/* Byte from the guest (write to SCFTDR of the SCIF connected to the panel). */
void clarion_dispmicom_rx_byte(DeviceState *dev, uint8_t b);

/* Host wheel as TUNE detents; positive is clockwise. No-op without a panel. */
void clarion_dispmicom_wheel(int detents);

#endif
