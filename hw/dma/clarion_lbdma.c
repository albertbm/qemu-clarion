/*
 * LBSC DMAC, the parallel NOR read DMA engine for the Clarion QY8XXX board
 * (WinCE 7.0).
 *
 * Why this model is needed: `Flash.dll` does not read NOR with CFI commands.
 * IOCTL `0x01112020` (`CFlashMain::FlashReadToPhysMem`) computes a physical
 * chip offset and delegates the transfer to a DMA engine, which reads the NOR
 * array and writes to RAM at a physical address. Without this model, the
 * board's broad interceptor swallowed requests and left the destination zero.
 * `CFlashDmacCtl::Transfer` still returned TRUE because it discards the wait
 * result. The guest then saw an empty NK2 version, reported `Version Error`,
 * and after a 5-second interrupt timeout hit a race that crashed AppLaunch
 * in `memcpy` with a null source.
 *
 * Register map sources: the driver's own code (`Flash.dll`, vbase
 * 0xEFA00000) and a live run. The driver keeps an array of pointers to
 * registers; this array was captured from the guest (structure @0xC483D310):
 *
 *   +0x00 = 0            channel number     -> bit 1<<0 in 0x410/0x414/0x418
 *   +0x04 = 2            mode ("not 1" branch in dmac_IsFinished)
 *   +0x10..+0x18 -> 0x00/0x04/0x08   set 0 {SAR, DAR, TCR}
 *   +0x1c..+0x24 -> 0x0c/0x10/0x14   set 1
 *   +0x28..+0x30 -> 0x18/0x1c/0x20   set 2
 *   +0x38 -> 0x28        CTRL   (`CFlashDmacCtl::Transfer` writes 0x00203515)
 *   +0x3c -> 0x2c        START  (write 1)
 *   +0x40 -> 0x30        STOP   (write 1)
 *   +0x44 -> 0x34        STATUS (bit 0 = busy)
 *   +0x5c -> 0x410       INTSTAT (read, one bit per channel)
 *   +0x68 -> 0x414       INTCLR  (write 1 to channel bit)
 *   +0x74 -> 0x418       INTMASK (interrupt enable, one bit per channel)
 *
 * Firmware sequence (verified with `-d unimp` on a clean boot):
 *
 *   SAR <- 0x00AC0000 ; DAR <- 0x70196000 ; TCR <- 0x20
 *   CTRL <- 0x00203515
 *   ResetEvent(m_hFlashInterEvent)
 *   INTMASK |= 1                      ; @0xEFA0F590
 *   START <- 1                        ; @0xEFA0F514, transfer starts here
 *   WaitForSingleObject(event, 5000)   ; @0xEFA0E9F0 FlashDmacIsFinish
 *   ... interrupt -> OAL demux clears INTMASK bit 0, signals SYSINTR 0x13 ...
 *   InterruptDone
 *   STOP <- 1 ; wait for STATUS.bit0 == 0 (up to 100 ms) ; @0xEFA0E9AC
 *   check INTSTAT.bit0, write 1 to INTCLR                 ; @0xEFA0F8D8
 *
 * Therefore the model raises the interrupt ON WRITE TO START, not earlier:
 * the guest enables interrupts between CTRL and START, and level logic
 * `INTSTAT & INTMASK` reproduces this without assumptions about ordering.
 *
 * On write 1 to START, transfer `TCR * 16` bytes from `SAR` to `DAR` through
 * physical memory (the actual NOR bytes provided by the flash model), advance
 * SAR/DAR, clear TCR and busy, and set the channel bit in INTSTAT. The model
 * has no knowledge of block 0x56, address 0xAC0000, NK2 headers or AppLaunch;
 * it uses only values written by the guest.
 *
 * Deliberate omissions (sources are silent, so behavior is not invented):
 *   CTRL (0x28): store 0x00203515 but do not interpret it; the driver writes
 *       a constant, so its fields are unknown;
 *   0x400/0x404/0x40c: the driver keeps pointers to these but never reads them
 *       in mode 2 (used here);
 *   sets 1 and 2: firmware uses only set 0 for reads. The model handles any
 *       set with nonzero TCR as controller behavior, not an assumption about
 *       set purpose;
 *   channels other than 0: the guest does not access their pages;
 *   transfer duration: the model transfers everything in one tick.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/dma/clarion_lbdma.h"
#include "system/address-spaces.h"
#include "qom/object.h"
#include "trace.h"

/* Channel page offsets. */
#define LBDMA_SAR(i)        (0x00 + (i) * 0x0C)
#define LBDMA_DAR(i)        (0x04 + (i) * 0x0C)
#define LBDMA_TCR(i)        (0x08 + (i) * 0x0C)
#define LBDMA_CTRL          0x28
#define LBDMA_START         0x2C
#define LBDMA_STOP          0x30
#define LBDMA_STATUS        0x34
#define LBDMA_INTSTAT       0x410
#define LBDMA_INTCLR        0x414
#define LBDMA_INTMASK       0x418

#define LBDMA_STATUS_BUSY   (1u << 0)

/*
 * Page 0xFF801000 is channel 0: the driver reads the channel number from its
 * own structure (field +0x00 = 0) and uses it to index bits in 0x410/0x414/0x418.
 */
#define LBDMA_CH            0
#define LBDMA_CH_BIT        (1u << LBDMA_CH)

struct ClarionLbDmaState {
    SysBusDevice parent_obj;

    MemoryRegion mr;
    qemu_irq irq;

    uint32_t sar[CLARION_LBDMA_NDESC];
    uint32_t dar[CLARION_LBDMA_NDESC];
    uint32_t tcr[CLARION_LBDMA_NDESC];
    uint32_t ctrl;
    uint32_t status;
    uint32_t intstat;
    uint32_t intmask;
};
typedef struct ClarionLbDmaState ClarionLbDmaState;

OBJECT_DECLARE_SIMPLE_TYPE(ClarionLbDmaState, CLARION_LBDMA)

static void lbdma_update_irq(ClarionLbDmaState *s)
{
    qemu_set_irq(s->irq, (s->intstat & s->intmask) != 0);
}

/* Transfer one {SAR, DAR, TCR} set. Return the number of bytes transferred. */
static uint32_t lbdma_run_desc(ClarionLbDmaState *s, int i)
{
    uint8_t buf[256];
    uint32_t left = s->tcr[i] * CLARION_LBDMA_UNIT;
    uint32_t done = 0;

    if (!left) {
        return 0;
    }
    trace_clarion_lbdma_run(i, s->sar[i], s->dar[i], s->tcr[i], left);

    while (left) {
        uint32_t n = MIN(left, sizeof(buf));

        address_space_read(&address_space_memory, s->sar[i] + done,
                           MEMTXATTRS_UNSPECIFIED, buf, n);
        address_space_write(&address_space_memory, s->dar[i] + done,
                            MEMTXATTRS_UNSPECIFIED, buf, n);
        done += n;
        left -= n;
    }

    /*
     * Count is exhausted and addresses advanced, matching controller
     * behavior and preventing a second transfer if START is written again
     * without reinitialization.
     */
    s->sar[i] += done;
    s->dar[i] += done;
    s->tcr[i] = 0;
    return done;
}

static void lbdma_start(ClarionLbDmaState *s)
{
    uint32_t total = 0;
    int i;

    for (i = 0; i < CLARION_LBDMA_NDESC; i++) {
        total += lbdma_run_desc(s, i);
    }
    trace_clarion_lbdma_start(s->ctrl, total);

    /*
     * The model transfers instantly, so busy never remains set: after STOP,
     * the driver polls STATUS.bit0 and expects zero.
     */
    s->status &= ~LBDMA_STATUS_BUSY;
    s->intstat |= LBDMA_CH_BIT;
    lbdma_update_irq(s);
}

static uint64_t lbdma_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionLbDmaState *s = opaque;
    int i;

    for (i = 0; i < CLARION_LBDMA_NDESC; i++) {
        if (addr == LBDMA_SAR(i)) {
            return s->sar[i];
        }
        if (addr == LBDMA_DAR(i)) {
            return s->dar[i];
        }
        if (addr == LBDMA_TCR(i)) {
            return s->tcr[i];
        }
    }

    switch (addr) {
    case LBDMA_CTRL:
        return s->ctrl;
    case LBDMA_STATUS:
        return s->status;
    case LBDMA_INTSTAT:
        return s->intstat;
    case LBDMA_INTMASK:
        return s->intmask;
    case LBDMA_START:
    case LBDMA_STOP:
    case LBDMA_INTCLR:
        return 0;               /* write-only */
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-lbdma: read from unknown register +0x%03"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void lbdma_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ClarionLbDmaState *s = opaque;
    int i;

    trace_clarion_lbdma_write((uint32_t)addr, val);

    for (i = 0; i < CLARION_LBDMA_NDESC; i++) {
        if (addr == LBDMA_SAR(i)) {
            s->sar[i] = val;
            return;
        }
        if (addr == LBDMA_DAR(i)) {
            s->dar[i] = val;
            return;
        }
        if (addr == LBDMA_TCR(i)) {
            s->tcr[i] = val;
            return;
        }
    }

    switch (addr) {
    case LBDMA_CTRL:
        s->ctrl = val;
        break;
    case LBDMA_START:
        if (val & 1) {
            lbdma_start(s);
        }
        break;
    case LBDMA_STOP:
        if (val & 1) {
            s->status &= ~LBDMA_STATUS_BUSY;
        }
        break;
    case LBDMA_INTCLR:
        s->intstat &= ~(uint32_t)val;
        lbdma_update_irq(s);
        break;
    case LBDMA_INTMASK:
        s->intmask = val;
        lbdma_update_irq(s);
        break;
    case LBDMA_STATUS:
    case LBDMA_INTSTAT:
        break;                  /* read-only */
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-lbdma: write to unknown register +0x%03"
                      HWADDR_PRIx " <- 0x%08" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps lbdma_ops = {
    .read = lbdma_read,
    .write = lbdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void clarion_lbdma_reset_hold(Object *obj, ResetType type)
{
    ClarionLbDmaState *s = CLARION_LBDMA(obj);

    memset(s->sar, 0, sizeof(s->sar));
    memset(s->dar, 0, sizeof(s->dar));
    memset(s->tcr, 0, sizeof(s->tcr));
    s->ctrl = 0;
    s->status = 0;
    s->intstat = 0;
    s->intmask = 0;
    lbdma_update_irq(s);
}

static void clarion_lbdma_realize(DeviceState *dev, Error **errp)
{
    ClarionLbDmaState *s = CLARION_LBDMA(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mr, OBJECT(dev), &lbdma_ops, s,
                          "clarion-lbdma", CLARION_LBDMA_SIZE);
    sysbus_init_mmio(sbd, &s->mr);
    sysbus_init_irq(sbd, &s->irq);
}

static void clarion_lbdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = clarion_lbdma_realize;
    dc->desc = "Clarion QY8XXX LBSC NOR DMA";
    rc->phases.hold = clarion_lbdma_reset_hold;
}

static const TypeInfo clarion_lbdma_types[] = {
    {
        .name          = TYPE_CLARION_LBDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ClarionLbDmaState),
        .class_init    = clarion_lbdma_class_init,
    },
};

DEFINE_TYPES(clarion_lbdma_types)
