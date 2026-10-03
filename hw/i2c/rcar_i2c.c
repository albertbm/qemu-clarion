/*
 * Renesas R-Car I2C master (i2c-rcar), as on R8A7778 (R-Car M1A)
 *
 * Register and status semantics follow Linux drivers/i2c/busses/i2c-rcar.c
 * and the Clarion QY8 WinCE I2C.dll. Only master mode is modelled; the
 * slave registers are plain storage.
 *
 * Bytes move when the guest acknowledges a status bit: clearing MAT/MDE
 * shifts out a pending ICRXTX byte, clearing MAT/MDR clocks in the next
 * one, FSB stops the bus once the data path is idle. Each of these lands
 * one byte time later, never inside the register write: the WinCE driver
 * re-reads ICMSR right after handling MAT and would take an early MST as
 * the end of a one-byte read it has not fetched yet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/rcar_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"

#define ICSCR   0x00
#define ICMCR   0x04
#define ICSSR   0x08
#define ICMSR   0x0c
#define ICSIER  0x10
#define ICMIER  0x14
#define ICCCR   0x18
#define ICSAR   0x1c
#define ICMAR   0x20
#define ICRXTX  0x24

/* ICMCR */
#define MDBS    0x80
#define FSCL    0x40
#define FSDA    0x20
#define OBPC    0x10
#define MIE     0x08
#define TSBE    0x04
#define FSB     0x02
#define ESG     0x01

/* ICMSR */
#define MNR     0x40
#define MAL     0x20
#define MST     0x10
#define MDE     0x08
#define MDT     0x04
#define MDR     0x02
#define MAT     0x01

#define REG(s, off) ((s)->regs[(off) >> 2])

/* 9 bits at 400 kHz */
#define RCAR_I2C_BYTE_NS    (23 * SCALE_US)

static void rcar_i2c_update_irq(RCarI2CState *s)
{
    bool level = (REG(s, ICMCR) & MIE) &&
                 (REG(s, ICMSR) & REG(s, ICMIER) & 0x7f);

    qemu_set_irq(s->irq, level);
}

static void rcar_i2c_stop(RCarI2CState *s)
{
    trace_rcar_i2c_stop();
    if (s->phase != RCAR_I2C_IDLE) {
        i2c_end_transfer(s->bus);
        s->phase = RCAR_I2C_IDLE;
    }
    REG(s, ICMSR) |= MST;
}

/* Address phase, for both the first START and a repeated one. */
static void rcar_i2c_start(RCarI2CState *s)
{
    uint8_t mar = REG(s, ICMAR);
    bool recv = mar & 1;
    int nack = i2c_start_transfer(s->bus, mar >> 1, recv);

    trace_rcar_i2c_start(mar >> 1, recv, nack);
    if (nack) {
        /* the controller sends STOP by itself after a NACK */
        if (s->phase != RCAR_I2C_IDLE) {
            i2c_end_transfer(s->bus);
            s->phase = RCAR_I2C_IDLE;
        }
        REG(s, ICMSR) |= MNR | MST;
        return;
    }
    s->phase = recv ? RCAR_I2C_RX : RCAR_I2C_TX;
    REG(s, ICMSR) |= MAT | (recv ? MDR : MDE);
}

static void rcar_i2c_step(RCarI2CState *s)
{
    uint32_t mcr = REG(s, ICMCR);
    uint32_t msr = REG(s, ICMSR);

    switch (s->phase) {
    case RCAR_I2C_IDLE:
        if (s->start_req) {
            s->start_req = false;
            if (mcr & ESG) {
                rcar_i2c_start(s);
            }
        }
        break;
    case RCAR_I2C_TX:
        if (msr & (MAT | MDE)) {
            break;
        }
        if (s->tx_full) {
            s->tx_full = false;
            if (i2c_send(s->bus, REG(s, ICRXTX))) {
                REG(s, ICMSR) |= MNR;
                rcar_i2c_stop(s);
                break;
            }
            REG(s, ICMSR) |= MDE | MDT;
            if (mcr & FSB) {
                rcar_i2c_stop(s);
            }
        } else if (mcr & FSB) {
            rcar_i2c_stop(s);
        } else if (mcr & ESG) {
            rcar_i2c_start(s);
        }
        break;
    case RCAR_I2C_RX:
        if (msr & (MAT | MDR)) {
            break;
        }
        if (mcr & ESG) {
            rcar_i2c_start(s);
            break;
        }
        s->rx = i2c_recv(s->bus);
        REG(s, ICMSR) |= MDR;
        if (mcr & FSB) {
            i2c_nack(s->bus);
            rcar_i2c_stop(s);
        }
        break;
    }
}

static void rcar_i2c_tick(void *opaque)
{
    RCarI2CState *s = opaque;

    rcar_i2c_step(s);
    rcar_i2c_update_irq(s);
}

static uint64_t rcar_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    RCarI2CState *s = opaque;

    switch (addr) {
    case ICRXTX:
        return s->rx;
    case ICSCR ... ICMAR:
    case ICRXTX + 4 ... (RCAR_I2C_NREGS - 1) * 4:
        return s->regs[addr >> 2];
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void rcar_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    RCarI2CState *s = opaque;

    switch (addr) {
    case ICMCR:
        REG(s, ICMCR) = val & 0xff;
        if (!(val & MDBS) && s->phase != RCAR_I2C_IDLE) {
            /* master interface switched off mid-transfer */
            i2c_end_transfer(s->bus);
            s->phase = RCAR_I2C_IDLE;
        } else if ((val & ESG) && s->phase == RCAR_I2C_IDLE) {
            s->start_req = true;
        }
        break;
    case ICMSR:
        /* write 0 to clear */
        REG(s, ICMSR) &= val;
        break;
    case ICRXTX:
        REG(s, ICRXTX) = val & 0xff;
        s->tx_full = true;
        break;
    case ICMAR:
    case ICSAR:
        REG(s, addr) = val & 0xff;
        break;
    case ICSCR:
    case ICSSR:
    case ICSIER:
    case ICMIER:
    case ICCCR:
    case ICRXTX + 4 ... (RCAR_I2C_NREGS - 1) * 4:
        s->regs[addr >> 2] = val;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return;
    }
    if (s->start_req || s->phase != RCAR_I2C_IDLE) {
        timer_mod(s->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + RCAR_I2C_BYTE_NS);
    }
    rcar_i2c_update_irq(s);
}

static const MemoryRegionOps rcar_i2c_ops = {
    .read = rcar_i2c_read,
    .write = rcar_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void rcar_i2c_reset(DeviceState *dev)
{
    RCarI2CState *s = RCAR_I2C(dev);

    if (s->phase != RCAR_I2C_IDLE) {
        i2c_end_transfer(s->bus);
    }
    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    s->phase = RCAR_I2C_IDLE;
    s->start_req = false;
    s->tx_full = false;
    s->rx = 0;
    rcar_i2c_update_irq(s);
}

static void rcar_i2c_init(Object *obj)
{
    RCarI2CState *s = RCAR_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rcar_i2c_ops, s, TYPE_RCAR_I2C,
                          RCAR_I2C_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rcar_i2c_tick, s);
}

static const VMStateDescription vmstate_rcar_i2c = {
    .name = TYPE_RCAR_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RCarI2CState, RCAR_I2C_NREGS),
        VMSTATE_UINT32(phase, RCarI2CState),
        VMSTATE_BOOL(start_req, RCarI2CState),
        VMSTATE_BOOL(tx_full, RCarI2CState),
        VMSTATE_UINT8(rx, RCarI2CState),
        VMSTATE_TIMER_PTR(timer, RCarI2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void rcar_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, rcar_i2c_reset);
    dc->vmsd = &vmstate_rcar_i2c;
    dc->desc = "Renesas R-Car I2C master";
}

static const TypeInfo rcar_i2c_types[] = {
    {
        .name          = TYPE_RCAR_I2C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RCarI2CState),
        .instance_init = rcar_i2c_init,
        .class_init    = rcar_i2c_class_init,
    },
};

DEFINE_TYPES(rcar_i2c_types)
