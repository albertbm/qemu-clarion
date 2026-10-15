/*
 * Clarion QY7221NL sub-CPU link UART: a TL16C850-style UART on the external
 * bus (physical 0x19B00000, registers on a 2-byte stride).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_CLARION_QY7_UART_H
#define HW_CHAR_CLARION_QY7_UART_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_CLARION_QY7_UART "clarion-qy7-uart"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionQy7UartState, CLARION_QY7_UART)

#define CLARION_QY7_UART_BASE 0x19b00000

/* Called for every byte the guest writes to the transmit holding register. */
typedef void (*ClarionQy7UartTxFn)(void *opaque, uint8_t byte);

void clarion_qy7_uart_set_tx(DeviceState *dev, ClarionQy7UartTxFn fn,
                             void *opaque);

/* Number of bytes the receive FIFO can still take. */
int clarion_qy7_uart_rx_room(DeviceState *dev);

/* Push received bytes; returns how many were accepted. */
int clarion_qy7_uart_rx(DeviceState *dev, const uint8_t *buf, int len);

#endif
