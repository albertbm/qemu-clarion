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

/* QY8 exposes five legacy IRQs; QY7 routes one output per channel. */
#define CLARION_HPBDMA_QY7_NUM_IRQ  32
#define CLARION_HPBDMA_NUM_IRQ      CLARION_HPBDMA_QY7_NUM_IRQ
#define CLARION_HPBDMA_QY8_NUM_IRQ  5
#define CLARION_HPBDMA_IRQ_BASE_SPI 91      /* IRQ 123 - 32 */

/*
 * Подати байт, що прийшов на периферійний регістр periph_addr, озброєному
 * каналу «модуль -> пам'ять». Повертає true, якщо байт забрав DMA.
 * Кличе SCIF: без цього напрямок прийому переносив би вигадані дані.
 */
bool clarion_hpbdma_feed(DeviceState *dev, hwaddr periph_addr, uint8_t val);

/*
 * «Посилка скінчилася» на модульному боці: приймач замовк, і те, що вже
 * перенесено в цьому set, — усе, що буде. Канал завершує set достроково,
 * лишаючи в DTCSR недобраний залишок, і піднімає своє переривання. Без
 * цього короткий кадр лежав би в буфері гостя мовчки, поки не набереться
 * повний DTCR — чого на живому лінку не буває.
 */
void clarion_hpbdma_eod(DeviceState *dev, hwaddr periph_addr);

/*
 * A module with its own data buffer (such as SDHI) can service HPB-DMAC
 * in either direction. The ready callback is queried before every unit:
 * true means the module can provide data (module-to-memory) or accept data
 * (memory-to-module). The module calls module_poke() when that state changes.
 */
typedef bool (*ClarionHpbModuleReady)(void *opaque);

void clarion_hpbdma_module_attach(DeviceState *dev, hwaddr periph_addr,
                                  ClarionHpbModuleReady ready, void *opaque);
void clarion_hpbdma_module_poke(DeviceState *dev, hwaddr periph_addr);

#endif
