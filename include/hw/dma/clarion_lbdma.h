/*
 * LBSC DMAC (DMA-двигун читання паралельної NOR) плати Clarion QY8XXX.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_CLARION_LBDMA_H
#define HW_DMA_CLARION_LBDMA_H

#define TYPE_CLARION_LBDMA "clarion-lbdma"

/*
 * База й розмір — із самої прошивки, а не з аналогії:
 *
 *   Flash.dll CFlashDmacCtl::Init @0xEFA0E6C4 кладе в
 *   DEVICE_LOCATION.LogicalLoc значення 0x1000 - 0x800000 = 0xFF801000 і
 *   питає OAL IOCTL_HAL_REQUEST_IRQ саме для цієї адреси. Далі той самий
 *   блок мапиться MmMapIoSpace (єдиний виклик у Flash.dll) — гість бачить
 *   його за VA 0xD5BD0000.
 *
 * Найбільший зсув, який чіпає драйвер, — 0x418, тож вікна 0x800 досить.
 */
#define CLARION_LBDMA_BASE      0xFF801000
#define CLARION_LBDMA_SIZE      0x800

/*
 * Лінія переривання. Не вгадана: OAL прошивки сам віддає її драйверу.
 *
 *   KernelIoControl(IOCTL_HAL_REQUEST_IRQ, {LogicalLoc=0xFF801000}) -> 0x6F
 *   KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, 0x6F)                -> 0x13
 *
 * (заміряно probe-ом tools/qy8_flash_irq_probe.py репозиторію
 * nissan-can-explore). «Логічний IRQ» OAL — це GIC INTID: таблиця переходів
 * OEMInterruptHandler @0x8800B5A0 індексується `IAR & 0x3FF` мінус 0x3B, і
 * для INTID 111 (= 0x6F) дає case @0x8800BC14, який маскує рівно цю лінію в
 * GICD_ICENABLER3 і кличе демукс @0x8800D904 -> OALIntrTranslateIrq(0x6F).
 * Отже SPI = 111 - 32 = 79.
 *
 * ⚠ Демукс цієї лінії, на відміну від DU й SDHI, НЕ читає жодного статусного
 * слова INTC2: він безумовно маскує джерело в 0xFE782040, знімає біт 0 у
 * 0xFF801418, розмасковує в 0xFE782044 і повертає SYSINTR. Тому окремого
 * вікна INTC2 для цього блока не потрібно.
 */
#define CLARION_LBDMA_SPI       79

/* Три набори {SAR, DAR, TCR} у канальній сторінці (0x00..0x20). */
#define CLARION_LBDMA_NDESC     3

/* TCR рахує одиниці по 16 байтів: 0x200 Б -> 0x20. */
#define CLARION_LBDMA_UNIT      16

#endif /* HW_DMA_CLARION_LBDMA_H */
