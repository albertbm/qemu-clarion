/*
 * Renesas HPB-DMAC (R8A7778) — модель для плати Clarion QY8XXX.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_CLARION_HPBDMA_H
#define HW_DMA_CLARION_HPBDMA_H

#define TYPE_CLARION_HPBDMA "clarion-hpbdma"

/*
 * Розміри й бази — з arch/arm/mach-shmobile/setup-r8a7778.c,
 * hpb_dmae_resources[] (ядро Linux v4.0):
 *
 *   канальні регістри   0xffc08000, 0x1000
 *   спільні регістри    0xffc09000, 0x170
 *   async reset         0xffc00300, 4
 *   async mode          0xffc00400, 4
 *   переривання         gic_iid(0x7b), 5 ліній  (IRQ 123..127)
 */
#define CLARION_HPBDMA_CHAN_BASE    0xFFC08000
#define CLARION_HPBDMA_CHAN_SIZE    0x1000
#define CLARION_HPBDMA_COMM_BASE    0xFFC09000
#define CLARION_HPBDMA_COMM_SIZE    0x170

/* num_hw_channels = 39 (hpb_dmae_pdata у тому ж файлі) */
#define CLARION_HPBDMA_NUM_CHAN     39

/* IRQ-ліній рівно 5: DEFINE_RES_NAMED(gic_iid(0x7b), 5, ...) */
#define CLARION_HPBDMA_NUM_IRQ      5
#define CLARION_HPBDMA_IRQ_BASE_SPI 91      /* IRQ 123 - 32 */

/*
 * Подати байт, що прийшов на периферійний регістр periph_addr, озброєному
 * каналу «модуль -> пам'ять». Повертає true, якщо байт забрав DMA.
 * Кличе SCIF: без цього напрямок прийому переносив би вигадані дані.
 */
bool clarion_hpbdma_feed(DeviceState *dev, hwaddr periph_addr, uint8_t val);

#endif
