/*
 * Bounded R-Car I2C4 model for the Clarion QY8 board.
 * Implements only the target accesses documented by project T142.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/clarion_rcar_i2c.h"
#include "qemu/module.h"

#define RCAR_I2C4_MMIO_SIZE 0x1000

#define ICMCR  0x04
#define ICMSR  0x0c
#define ICMIER 0x14
#define ICMAR  0x20
#define ICRXTX 0x24

#define ICMCR_FSDA BIT(5)
#define ICMCR_ESG  BIT(0)
#define ICMCR_MDBS BIT(7)
#define ICMCR_MIE  BIT(3)

#define ICMSR_MASK 0x7f
#define ICMSR_MNR  BIT(6)
#define ICMSR_MDE  BIT(3)
#define ICMSR_MDT  BIT(2)
#define ICMSR_MAT  BIT(0)

typedef enum ClarionRcarI2C4Phase {
    RCAR_I2C4_IDLE,
    RCAR_I2C4_WRITE_ADDRESS,
    RCAR_I2C4_WRITE_DATA,
    RCAR_I2C4_READ_ADDRESS,
    RCAR_I2C4_READ_DATA_WAIT,
} ClarionRcarI2C4Phase;

struct ClarionRcarI2C4State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;
    uint32_t icmcr;
    uint32_t icmsr;
    uint32_t icmier;
    uint32_t icmar;
    uint32_t icrxtx;
    ClarionRcarI2C4Phase phase;
    bool bus_active;
};

struct ClarionI2C4Recorder {
    I2CSlave parent_obj;
};

static int clarion_i2c4_recorder_event(I2CSlave *slave, enum i2c_event event)
{
    const char *name;

    switch (event) {
    case I2C_START_SEND:
        name = "start(0x24, write)";
        break;
    case I2C_START_RECV:
        name = "start(0x24, read)";
        break;
    case I2C_FINISH:
        name = "finish";
        break;
    case I2C_NACK:
        name = "nack";
        break;
    default:
        name = "other-event";
        break;
    }

    qemu_log_mask(LOG_UNIMP, "clarion-i2c4-recorder: %s\n", name);
    return 0;
}

static int clarion_i2c4_recorder_send(I2CSlave *slave, uint8_t data)
{
    qemu_log_mask(LOG_UNIMP,
                  "clarion-i2c4-recorder: send(0x%02x) ACK\n", data);
    return 0;
}

static uint8_t clarion_i2c4_recorder_recv(I2CSlave *slave)
{
    qemu_log_mask(LOG_UNIMP,
                  "clarion-i2c4-recorder: ERROR unexpected recv; B must stop before data\n");
    return 0xff;
}

static void clarion_i2c4_recorder_class_init(ObjectClass *klass,
                                              const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->event = clarion_i2c4_recorder_event;
    sc->send = clarion_i2c4_recorder_send;
    sc->recv = clarion_i2c4_recorder_recv;
}

static const TypeInfo clarion_i2c4_recorder_type_info = {
    .name = TYPE_CLARION_I2C4_RECORDER,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ClarionI2C4Recorder),
    .class_init = clarion_i2c4_recorder_class_init,
};

static void clarion_rcar_i2c4_update_irq(ClarionRcarI2C4State *s)
{
    bool level = !!(s->icmsr & s->icmier & ICMSR_MASK);

    qemu_set_irq(s->irq, level);
}

static void clarion_rcar_i2c4_start(ClarionRcarI2C4State *s)
{
    uint8_t address = (s->icmar >> 1) & 0x7f;
    bool read = !!(s->icmar & 1);
    int ret;

    ret = i2c_start_transfer(s->bus, address, read);
    /* QEMU's i2c_start_transfer() returns 1 when no slave ACKs. */
    if (ret != 0) {
        s->icmsr |= ICMSR_MNR;
        s->phase = RCAR_I2C4_IDLE;
        s->bus_active = false;
        qemu_log_mask(LOG_UNIMP,
                      "clarion-i2c4: address phase NACK addr=0x%02x %s\n",
                      address, read ? "read" : "write");
    } else {
        s->icmsr |= ICMSR_MAT;
        s->bus_active = true;
        s->phase = read ? RCAR_I2C4_READ_ADDRESS :
                          RCAR_I2C4_WRITE_ADDRESS;
        qemu_log_mask(LOG_UNIMP,
                      "clarion-i2c4: address phase ACK addr=0x%02x %s\n",
                      address, read ? "read" : "write");
    }
    clarion_rcar_i2c4_update_irq(s);
}

static uint64_t clarion_rcar_i2c4_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    ClarionRcarI2C4State *s = opaque;
    uint32_t value;

    switch (offset) {
    case ICMCR:
        /* FSDA reads as low while the modeled bus is idle. */
        value = s->icmcr & ~ICMCR_FSDA;
        break;
    case ICMSR:
        value = s->icmsr;
        break;
    case ICMIER:
        value = s->icmier;
        break;
    case ICMAR:
        value = s->icmar;
        break;
    case ICRXTX:
        value = s->icrxtx;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-i2c4: unimplemented read offset=0x%03" HWADDR_PRIx " size=%u\n",
                      offset, size);
        return 0;
    }

    qemu_log_mask(LOG_UNIMP,
                  "clarion-i2c4: read +0x%02" HWADDR_PRIx " = 0x%08x\n",
                  offset, value);
    return value;
}

static void clarion_rcar_i2c4_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    ClarionRcarI2C4State *s = opaque;
    uint32_t v = value;

    qemu_log_mask(LOG_UNIMP,
                  "clarion-i2c4: write +0x%02" HWADDR_PRIx " = 0x%08x\n",
                  offset, v);
    switch (offset) {
    case ICMCR:
        s->icmcr = v;
        if ((v & (ICMCR_MDBS | ICMCR_MIE | ICMCR_ESG)) ==
            (ICMCR_MDBS | ICMCR_MIE | ICMCR_ESG)) {
            clarion_rcar_i2c4_start(s);
        } else if (s->phase == RCAR_I2C4_WRITE_ADDRESS &&
                   !(v & ICMCR_ESG) && (s->icmsr & ICMSR_MAT)) {
            /* Target row 0xefa828cc clears ESG on the first MAT. */
            s->phase = RCAR_I2C4_WRITE_DATA;
        }
        break;
    case ICMSR:
        /* R-Car master status is write-zero-to-clear. */
        {
            bool mat_cleared = (s->icmsr & ICMSR_MAT) &&
                               !(v & ICMSR_MAT);

            /* Apply the target's W0C before modeling the hardware transition. */
            s->icmsr &= v | ~ICMSR_MASK;

            if (s->phase == RCAR_I2C4_WRITE_DATA && mat_cleared) {
                int ret = i2c_send(s->bus, s->icrxtx);

                if (ret) {
                    s->icmsr |= ICMSR_MNR;
                    qemu_log_mask(LOG_UNIMP,
                                  "clarion-i2c4: ICRXTX send NACK byte=0x%02x\n",
                                  s->icrxtx);
                } else {
                    s->icmsr |= ICMSR_MDE | ICMSR_MDT;
                    qemu_log_mask(LOG_UNIMP,
                                  "clarion-i2c4: W0C MAT -> send ICRXTX=0x%02x; set MDE|MDT\n",
                                  s->icrxtx);
                }
            } else if (s->phase == RCAR_I2C4_READ_ADDRESS && mat_cleared) {
                s->phase = RCAR_I2C4_READ_DATA_WAIT;
                qemu_log_mask(LOG_UNIMP,
                              "clarion-i2c4: read byte requested; ICMCR=0x%08x; withholding MDR/data\n",
                              s->icmcr);
            }
        }
        clarion_rcar_i2c4_update_irq(s);
        break;
    case ICMIER:
        s->icmier = v & ICMSR_MASK;
        clarion_rcar_i2c4_update_irq(s);
        break;
    case ICMAR:
        s->icmar = v;
        break;
    case ICRXTX:
        s->icrxtx = v & 0xff;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-i2c4: unimplemented write offset=0x%03" HWADDR_PRIx " value=0x%08x\n",
                      offset, v);
        break;
    }
}

static const MemoryRegionOps clarion_rcar_i2c4_ops = {
    .read = clarion_rcar_i2c4_read,
    .write = clarion_rcar_i2c4_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void clarion_rcar_i2c4_realize(DeviceState *dev, Error **errp)
{
    ClarionRcarI2C4State *s = CLARION_RCAR_I2C4(dev);

    s->bus = i2c_init_bus(dev, "i2c");
    memory_region_init_io(&s->iomem, OBJECT(dev), &clarion_rcar_i2c4_ops, s,
                          TYPE_CLARION_RCAR_I2C4, RCAR_I2C4_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void clarion_rcar_i2c4_class_init(ObjectClass *klass,
                                         const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = clarion_rcar_i2c4_realize;
}

static const TypeInfo clarion_rcar_i2c4_type_info = {
    .name = TYPE_CLARION_RCAR_I2C4,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ClarionRcarI2C4State),
    .class_init = clarion_rcar_i2c4_class_init,
};

static void clarion_rcar_i2c4_register_types(void)
{
    type_register_static(&clarion_rcar_i2c4_type_info);
    type_register_static(&clarion_i2c4_recorder_type_info);
}

type_init(clarion_rcar_i2c4_register_types)
