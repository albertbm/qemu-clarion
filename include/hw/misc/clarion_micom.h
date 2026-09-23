/*
 * Micom — супутній мікроконтролер плати Clarion QY8XXX, лінк по SCIF4.
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
 * Куди micom віддає байти. Повертає, скільки з них ПРИЙНЯТО: приймач SCIF
 * має FIFO на 16 байтів, і довгий кадр (набір команд — 62 байти даних) не
 * влазить у нього цілком. Решту micom тримає в себе й досилає, коли DMA
 * звільнить місце — так само, як стримував би реальний лінк.
 */
typedef int (*ClarionMicomSink)(void *opaque, const uint8_t *buf, int len);

void clarion_micom_set_sink(DeviceState *dev, ClarionMicomSink fn,
                            void *opaque);

/* Байт від гостя (запис у SCFTDR того SCIF, до якого під'єднано micom). */
void clarion_micom_rx_byte(DeviceState *dev, uint8_t b);

#endif
