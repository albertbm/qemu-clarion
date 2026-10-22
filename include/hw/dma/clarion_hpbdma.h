/*
 * Renesas HPB-DMAC (R8A7778) model for the Clarion QY8XXX board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_CLARION_HPBDMA_H
#define HW_DMA_CLARION_HPBDMA_H

#define TYPE_CLARION_HPBDMA "clarion-hpbdma"

/*
 * Sizes and base addresses are from arch/arm/mach-shmobile/setup-r8a7778.c,
 * hpb_dmae_resources[] (Linux v4.0 kernel):
 *
 *   channel registers   0xffc08000, 0x1000
 *   shared registers    0xffc09000, 0x170
 *   async reset         0xffc00300, 4
 *   async mode          0xffc00400, 4
 *   interrupts          gic_iid(0x7b), 5 lines  (IRQ 123..127)
 */
#define CLARION_HPBDMA_CHAN_BASE    0xFFC08000
#define CLARION_HPBDMA_CHAN_SIZE    0x1000
#define CLARION_HPBDMA_COMM_BASE    0xFFC09000
#define CLARION_HPBDMA_COMM_SIZE    0x170

/* num_hw_channels = 39 (hpb_dmae_pdata in the same file) */
#define CLARION_HPBDMA_NUM_CHAN     39

/* QY8 exposes five legacy IRQs; QY7 routes one output per channel. */
#define CLARION_HPBDMA_QY7_NUM_IRQ  32
#define CLARION_HPBDMA_NUM_IRQ      CLARION_HPBDMA_QY7_NUM_IRQ
#define CLARION_HPBDMA_QY8_NUM_IRQ  5
#define CLARION_HPBDMA_IRQ_BASE_SPI 91      /* IRQ 123 - 32 */

/*
 * Deliver a byte received on peripheral register periph_addr to an armed
 * module-to-memory channel. Returns true if DMA consumed the byte.
 * Called by SCIF: without this, receive transfers would invent data.
 */
bool clarion_hpbdma_feed(DeviceState *dev, hwaddr periph_addr, uint8_t val);

/*
 * Signal end of a module-side burst: the receiver is idle, and the bytes
 * already transferred in this set are all that will arrive. The channel ends
 * the set early, leaves the untransferred remainder in DTCSR, and raises its
 * interrupt. Without this, a short frame would sit silently in the guest
 * buffer until the full DTCR count arrived, which does not happen on a live link.
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
