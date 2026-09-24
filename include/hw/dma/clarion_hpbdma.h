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

/*
 * --- Модуль, який сам тримає дані (SDHI) -------------------------------
 *
 * SCIF байт за байтом ВІДДАЄ прийняте через clarion_hpbdma_feed(). SDHI так
 * не може: блок він отримує цілком, і на живій платі порядок такий (траса
 * docs/qy8-sdhi-dma-blocker-20260924.log у репозиторії nissan-can-explore):
 *
 *     DCR/DSAR/DDAR/DTCR каналу 21  ->  CMD17  ->  DCMDR.DMEN
 *
 * тобто дані в контролері з'являються РАНІШЕ, ніж канал озброєно. Отже
 * напрямок тут зворотний: DMAC САМ ЧИТАЄ регістр модуля, поки той каже, що
 * дані ще є. Саме так поводиться залізо — DMAC обслуговує піднятий запит,
 * коли його ввімкнули.
 *
 * `ready` — це і є запит DMA від модуля: «в буфері просто зараз є що
 * віддати». DMAC питає його перед КОЖНОЮ одиницею передачі, тож модель не
 * має жодного лічильника, який треба тримати в синхроні, і не може
 * вигадати даних, яких у модуля немає.
 *
 * clarion_hpbdma_module_attach() модуль кличе один раз (при realize),
 * clarion_hpbdma_module_poke() — коли дані з'явилися вже після DMEN.
 */
typedef bool (*ClarionHpbModuleReady)(void *opaque);

void clarion_hpbdma_module_attach(DeviceState *dev, hwaddr periph_addr,
                                  ClarionHpbModuleReady ready, void *opaque);
void clarion_hpbdma_module_poke(DeviceState *dev, hwaddr periph_addr);

#endif
