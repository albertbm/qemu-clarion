/*
 * Clarion QY7221NL sub-CPU ("SCPU") transport on the CAN.dll UART link.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_CLARION_SCPU_H
#define HW_MISC_CLARION_SCPU_H

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_CLARION_SCPU "clarion-scpu"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionScpuState, CLARION_SCPU)

/* UART the sub-CPU talks through (clarion-qy7-uart); set before realize. */
void clarion_scpu_attach_uart(DeviceState *scpu, DeviceState *uart);

/*
 * Who answers a de-framed, checksum-verified body from the guest. Called
 * with the body bytes (command code first) and its length; may call
 * clarion_scpu_send() to queue a reply frame. NULL: unsupported bodies get
 * only the transport-level DLE ACK.
 */
typedef void (*ClarionScpuBodyFn)(void *opaque, const uint8_t *body,
                                  unsigned len);

void clarion_scpu_set_body_sink(DeviceState *scpu, ClarionScpuBodyFn fn,
                                void *opaque);

/* Queue a framed reply (DLE STX .. DLE ETX BCC) built from a raw body. */
void clarion_scpu_send(DeviceState *scpu, const uint8_t *data, unsigned len);

/* One byte the guest wrote to the UART transmit register. */
void clarion_scpu_rx_byte(DeviceState *scpu, uint8_t byte);

#endif
