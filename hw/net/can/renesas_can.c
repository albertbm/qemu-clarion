/*
 * Renesas CAN controller (R-Car Gen1, R8A7778) — QY8XXX @ 0xFFFD1000.
 *
 * This is a CONTROLLER, not a vehicle. The model implements its modes
 * (reset / halt / operation / sleep) and mailbox memory only. There is no CAN
 * bus here: transmission and reception are not implemented, and the model
 * does not invent substitutes.
 *
 * --- Register map sources ------------------------------------------------
 *
 * 1. Live QEMU trace (2026-09-24): 64 groups of 16 bytes at +0x000, 64
 *    single-byte registers at +0x800, followed by separate registers.
 *    +0x840, +0x842, +0x844/846, +0x84C, +0x856, +0x858.
 * 2. Driver `CAN.dll` (section 0, VA 0xEF621000) provides the semantics;
 *    no public datasheet for this block is available.
 * 3. Register names come from Linux driver's `struct rcar_can_regs` in
 *    `drivers/net/can/rcar/rcar_can.c`; its map matches (1) byte for byte:
 *    byte for byte: mb[64]×16, mkr/fidcr/mier at +0x400, mctl[64] at +0x800,
 *    CTLR +0x840, STR +0x842, BCR +0x844..846.
 *
 * --- Established driver behavior -----------------------------------------
 *
 *   0xEF628E80  wake from sleep: if CTLR & 0x400, clear the bit and wait for
 *               STR & 0x400 to clear.
 *   0xEF629308  initialization:
 *                 CTLR = (CTLR & ~0x300) | 0x100      - request reset mode
 *                 poll STR & 0x100 up to 100 times   - else error 0x21
 *                 clear 64 mailboxes, MCTL[i] = 0x40
 *                 BCR: 0xF004 -> +0x844, 0x12 -> +0x846 (bit timing)
 *                 CTLR = (CTLR & ~0x300) | 0x200      - request halt mode
 *                 poll STR & 0x200 up to 100 times   - else error 0x21
 *                 +0x858 = (+0x858 & 0x20) | 0x07, +0x84C = 0x08
 *                 CTLR &= ~0x300                      - request operation mode
 *                 wait up to 100 times for both STR 0x100 and 0x200 to clear
 *   0xEF6294BC  mailbox memory test: write 0x5555 and 0xAAAA to all 64
 *               mailboxes at offsets 0x0, 0x2, 0x6, 0x8, 0xA, 0xC, then
 *               verify masked reads (table at 0xEF6216AC). First halfword
 *               mask is 0xDFFF; the rest use 0xFFFF.
 *
 * --- Model scope ---------------------------------------------------------
 *
 * * The CTLR reset value is not confirmed by a datasheet. Use 0x0100, reset
 *   mode after power-on as expected for a CAN controller. The driver forces
 *   CANM itself before waiting for confirmation.
 * * Real hardware enters operation mode after 11 recessive bus bits. No
 *   receiver is connected, so the bus is idle and those bits elapse
 *   immediately. This models an idle bus rather than stubbing the transition.
 * * Error counters (RECR/TECR/ECSR) read zero because there is no bus and no
 *   errors. MSSR returns SEST=1 (no mailbox found) because there is nothing to
 *   receive.
 * * Bit 13 of the first mailbox halfword reads zero; it is the only bit the
 *   driver excludes from its memory test (mask 0xDFFF). In the Linux map this
 *   corresponds to reserved bit 29 of the 32-bit ID field.
 * * No interrupt is generated: no model event raises one, and the CAN GIC
 *   line number is unproven.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "accel/tcg/cpu-loop.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/net/renesas_can.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(RenesasCANState, RENESAS_CAN)

/* --- block map ---------------------------------------------------------- */

#define CAN_MB_BASE     0x000   /* 64 mailboxes, 16 bytes each            */
#define CAN_MB_SIZE     0x400
#define CAN_MKR_BASE    0x400   /* MKR2..9, FIDCR0/1, MKIVLR*, MIER*      */
#define CAN_MKR_SIZE    0x040
#define CAN_CTL_BASE    0x800   /* MCTL[64] and control registers         */
#define CAN_CTL_SIZE    0x100

#define CAN_CTLR        0x840   /* Control Register, 16 bits              */
#define CAN_STR         0x842   /* Status Register, 16 bits, read-only    */

/* CTLR */
#define CTLR_CANM       0x0300  /* controller mode                        */
#define CTLR_CANM_OPER  0x0000
#define CTLR_CANM_RESET 0x0100
#define CTLR_CANM_HALT  0x0200
#define CTLR_CANM_FRST  0x0300  /* force reset                           */
#define CTLR_SLPM       0x0400  /* sleep request                           */

/* STR */
#define STR_RSTST       0x0100
#define STR_HLTST       0x0200
#define STR_SLPST       0x0400

/* Registers with behavior beyond storage (offsets from CAN_CTL_BASE). */
#define CTL_OFF(x)      ((x) - CAN_CTL_BASE)
#define CTL_MSSR        CTL_OFF(0x852)
#define MSSR_SEST       0x80    /* 1 = no search result                   */

enum {
    CAN_MODE_RESET,
    CAN_MODE_HALT,
    CAN_MODE_OPERATION,
};

struct RenesasCANState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint8_t mb[CAN_MB_SIZE];    /* mailbox memory                         */
    uint8_t mkr[CAN_MKR_SIZE];  /* masks/filters/interrupt enables        */
    uint8_t ctl[CAN_CTL_SIZE];  /* MCTL[64] + control registers            */

    uint8_t mode;               /* CAN_MODE_*                            */
    bool sleep;

    /* QY8_CAN_PC=1 adds caller pc/lr to each trace line. */
    bool trace_pc;
};

static const char *can_mode_name(uint8_t mode)
{
    switch (mode) {
    case CAN_MODE_RESET:     return "reset";
    case CAN_MODE_HALT:      return "halt";
    case CAN_MODE_OPERATION: return "operation";
    default:                 return "?";
    }
}

/*
 * Derive STR from controller state instead of storing it, avoiding a fake
 * "read back the expected value" implementation.
 */
static uint16_t can_str(RenesasCANState *s)
{
    uint16_t str = 0;

    switch (s->mode) {
    case CAN_MODE_RESET:
        str |= STR_RSTST;
        break;
    case CAN_MODE_HALT:
        str |= STR_HLTST;
        break;
    default:
        break;
    }
    if (s->sleep) {
        str |= STR_SLPST;
    }
    return str;
}

static void can_ctlr_write(RenesasCANState *s, uint16_t ctlr)
{
    uint8_t old = s->mode;
    bool oldsleep = s->sleep;

    switch (ctlr & CTLR_CANM) {
    case CTLR_CANM_RESET:
    case CTLR_CANM_FRST:
        s->mode = CAN_MODE_RESET;
        break;
    case CTLR_CANM_HALT:
        s->mode = CAN_MODE_HALT;
        break;
    default:
        /*
         * Request operation mode. Hardware joins the bus after observing 11
         * recessive bits; the bus is idle here, so this happens immediately.
         */
        s->mode = CAN_MODE_OPERATION;
        break;
    }
    s->sleep = (ctlr & CTLR_SLPM) != 0;

    if (s->mode != old || s->sleep != oldsleep) {
        trace_renesas_can_mode(can_mode_name(old), can_mode_name(s->mode),
                               s->sleep, can_str(s));
    }
}

/* --- access byte arrays at any width ----------------------------------- */

static uint64_t can_ld(const uint8_t *p, unsigned size)
{
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static void can_st(uint8_t *p, unsigned size, uint64_t v)
{
    unsigned i;

    for (i = 0; i < size; i++) {
        p[i] = v >> (8 * i);
    }
}

/*
 * Identify the caller. As in qy8.periph, cpu_unwind_state_data provides the
 * exact PC (not cpu_restore_state, which writes unwound state back to env and
 * changes the guest's execution path). Only used with QY8_CAN_PC=1 because
 * unwinding state is expensive.
 */
static void can_ctx(RenesasCANState *s, char *buf, size_t len)
{
    CPUState *cs = current_cpu;
    uint64_t data[4];
    uint64_t pc;

    buf[0] = '\0';
    if (!s->trace_pc || !cs) {
        return;
    }
    if (cpu_unwind_state_data(cs, cs->mem_io_pc, data)) {
        /*
         * With CF_PCREL, data[0] is an offset within the page; use high bits
         * from the current PC because a translation block does not cross a
         * page boundary.
         */
        pc = data[0];
        if (pc < qemu_target_page_size()) {
            pc |= CPU_GET_CLASS(cs)->get_pc(cs) & qemu_target_page_mask();
        }
    } else {
        pc = CPU_GET_CLASS(cs)->get_pc(cs);   /* fallback: start of block */
    }
    snprintf(buf, len, " pc=0x%08" PRIx64 " t=%" PRId64, pc,
             qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

static uint64_t can_read(void *opaque, hwaddr addr, unsigned size)
{
    RenesasCANState *s = opaque;
    char ctx[80];
    uint64_t val;

    if (addr < CAN_MB_BASE + CAN_MB_SIZE) {
        val = can_ld(&s->mb[addr - CAN_MB_BASE], size);
    } else if (addr >= CAN_MKR_BASE && addr < CAN_MKR_BASE + CAN_MKR_SIZE) {
        val = can_ld(&s->mkr[addr - CAN_MKR_BASE], size);
    } else if (addr == CAN_STR && size == 2) {
        val = can_str(s);
    } else if (addr >= CAN_CTL_BASE && addr < CAN_CTL_BASE + CAN_CTL_SIZE) {
        val = can_ld(&s->ctl[addr - CAN_CTL_BASE], size);
    } else {
        qemu_log_mask(LOG_UNIMP, "renesas-can: read outside map +%#"
                      HWADDR_PRIx " (%u)\n", addr, size);
        val = 0;
    }

    can_ctx(s, ctx, sizeof(ctx));
    trace_renesas_can_read(addr, size, val, ctx);
    return val;
}

static void can_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RenesasCANState *s = opaque;
    char ctx[80];

    can_ctx(s, ctx, sizeof(ctx));
    trace_renesas_can_write(addr, size, val, ctx);

    if (addr < CAN_MB_BASE + CAN_MB_SIZE) {
        unsigned i;

        for (i = 0; i < size; i++) {
            hwaddr a = addr - CAN_MB_BASE + i;
            uint8_t b = val >> (8 * i);

            /*
             * The second mailbox byte is the high half of the 16-bit word at
             * +0x00; bit 5 (word bit 13) is not written because the driver
             * excludes it from its memory test.
             */
            if ((a & 0xf) == 1) {
                b &= 0xdf;
            }
            s->mb[a] = b;
        }
        return;
    }
    if (addr >= CAN_MKR_BASE && addr < CAN_MKR_BASE + CAN_MKR_SIZE) {
        can_st(&s->mkr[addr - CAN_MKR_BASE], size, val);
        return;
    }
    if (addr == CAN_STR && size == 2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "renesas-can: write to read-only STR %#" PRIx64 "\n",
                      val);
        return;
    }
    if (addr >= CAN_CTL_BASE && addr < CAN_CTL_BASE + CAN_CTL_SIZE) {
        can_st(&s->ctl[addr - CAN_CTL_BASE], size, val);
        if (addr <= CAN_CTLR && addr + size > CAN_CTLR) {
            can_ctlr_write(s, can_ld(&s->ctl[CTL_OFF(CAN_CTLR)], 2));
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP, "renesas-can: write outside map +%#" HWADDR_PRIx
                  " (%u) = %#" PRIx64 "\n", addr, size, val);
}

static const MemoryRegionOps can_ops = {
    .read = can_read,
    .write = can_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void can_reset_hold(Object *obj, ResetType type)
{
    RenesasCANState *s = RENESAS_CAN(obj);

    memset(s->mb, 0, sizeof(s->mb));
    memset(s->mkr, 0, sizeof(s->mkr));
    memset(s->ctl, 0, sizeof(s->ctl));

    /* Controller starts in reset mode after power-on. */
    s->mode = CAN_MODE_RESET;
    s->sleep = false;
    can_st(&s->ctl[CTL_OFF(CAN_CTLR)], 2, CTLR_CANM_RESET);
    /* No mailbox search has run, so there is no result. */
    s->ctl[CTL_MSSR] = MSSR_SEST;
}

static void can_init(Object *obj)
{
    RenesasCANState *s = RENESAS_CAN(obj);

    s->trace_pc = getenv("QY8_CAN_PC") != NULL;
    memory_region_init_io(&s->iomem, obj, &can_ops, s, "renesas-can",
                          RENESAS_CAN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_renesas_can = {
    .name = "renesas-can",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mb, RenesasCANState, CAN_MB_SIZE),
        VMSTATE_UINT8_ARRAY(mkr, RenesasCANState, CAN_MKR_SIZE),
        VMSTATE_UINT8_ARRAY(ctl, RenesasCANState, CAN_CTL_SIZE),
        VMSTATE_UINT8(mode, RenesasCANState),
        VMSTATE_BOOL(sleep, RenesasCANState),
        VMSTATE_END_OF_LIST()
    },
};

static void can_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Renesas CAN controller (R-Car Gen1)";
    dc->vmsd = &vmstate_renesas_can;
    rc->phases.hold = can_reset_hold;
}

static const TypeInfo renesas_can_types[] = {
    {
        .name          = TYPE_RENESAS_CAN,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RenesasCANState),
        .instance_init = can_init,
        .class_init    = can_class_init,
    },
};

DEFINE_TYPES(renesas_can_types)
