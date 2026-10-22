/*
 * LBSC DMAC (parallel NOR read DMA engine) for the Clarion QY8XXX board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_CLARION_LBDMA_H
#define HW_DMA_CLARION_LBDMA_H

#define TYPE_CLARION_LBDMA "clarion-lbdma"

/*
 * Base and size come from the firmware, not from analogy:
 *
 *   Flash.dll CFlashDmacCtl::Init @0xEFA0E6C4 sets
 *   DEVICE_LOCATION.LogicalLoc to 0x1000 - 0x800000 = 0xFF801000 and
 *   requests an OAL IOCTL_HAL_REQUEST_IRQ for that address. The same block
 *   is then mapped with MmMapIoSpace (the only call in Flash.dll); the guest
 *   sees it at VA 0xD5BD0000.
 *
 * The largest offset accessed by the driver is 0x418, so a 0x800 window suffices.
 */
#define CLARION_LBDMA_BASE      0xFF801000
#define CLARION_LBDMA_SIZE      0x800

/*
 * Interrupt line. It is not inferred: the firmware OAL provides it to the driver.
 *
 *   KernelIoControl(IOCTL_HAL_REQUEST_IRQ, {LogicalLoc=0xFF801000}) -> 0x6F
 *   KernelIoControl(IOCTL_HAL_REQUEST_SYSINTR, 0x6F)                -> 0x13
 *
 * (measured with a local diagnostic probe).
 * The OAL's "logical IRQ" is the GIC INTID: the OEMInterruptHandler
 * @0x8800B5A0 dispatch table is indexed by `IAR & 0x3FF` minus 0x3B. For
 * INTID 111 (= 0x6F), it selects case @0x8800BC14, which masks this line in
 * GICD_ICENABLER3 and calls demux @0x8800D904 -> OALIntrTranslateIrq(0x6F).
 * Therefore SPI = 111 - 32 = 79.
 *
 * ⚠ Unlike DU and SDHI, this line's demux does NOT read an INTC2 status
 * word: it unconditionally masks the source at 0xFE782040, clears bit 0 at
 * 0xFF801418, unmasks at 0xFE782044, and returns SYSINTR. Therefore this
 * block does not need a separate INTC2 window.
 */
#define CLARION_LBDMA_SPI       79

/* Three {SAR, DAR, TCR} sets in the channel page (0x00..0x20). */
#define CLARION_LBDMA_NDESC     3

/* TCR counts units of 16 bytes: 0x200 bytes -> 0x20. */
#define CLARION_LBDMA_UNIT      16

#endif /* HW_DMA_CLARION_LBDMA_H */
