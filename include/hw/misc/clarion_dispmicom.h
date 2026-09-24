/*
 * Display Micom — мікроконтролер панелі Clarion QY8XXX, лінк по SCIF1.
 *
 * Окремий вузол від супутнього МК плати (clarion_micom, SCIF4): інший дріт,
 * інший протокол, інший драйвер у гості (lcddrv.dll проти EdaDrv.dll).
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
 * Куди панель віддає байти. Повертає, скільки ПРИЙНЯТО: FIFO приймача SCIF
 * має 16 байтів, а кадр 0x24 разом з обгорткою — 20, тож решту модель
 * досилає, коли драйвер вичитає попереднє.
 */
typedef int (*ClarionDispMicomSink)(void *opaque, const uint8_t *buf, int len);

void clarion_dispmicom_set_sink(DeviceState *dev, ClarionDispMicomSink fn,
                                void *opaque);

/* Байт від гостя (запис у SCFTDR того SCIF, до якого під'єднано панель). */
void clarion_dispmicom_rx_byte(DeviceState *dev, uint8_t b);

#endif
