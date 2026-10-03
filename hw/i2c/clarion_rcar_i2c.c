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
#define ICMSR_MDR  BIT(1)
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
    bool icrxtx_written;
    bool read_last_pending;
};

struct ClarionI2C4Recorder {
    I2CSlave parent_obj;
};

struct ClarionTma460 {
    I2CSlave parent_obj;
    qemu_irq irq;
    uint8_t selector;
    uint8_t command[9];
    uint8_t command_len;
    uint8_t response_index;
    bool selector_valid;
    bool command_valid;
    bool command_pending;
    bool response_active;
    bool reset_released;
    bool ready_pulsed;
};

static const uint8_t clarion_tma460_enter_active[9] = {
    0x00, 0xff, 0x01, 0x38, 0x00, 0x00, 0xa0, 0x09, 0x17,
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

static int clarion_tma460_event(I2CSlave *slave, enum i2c_event event)
{
    ClarionTma460 *s = CLARION_TMA460(slave);

    switch (event) {
    case I2C_START_SEND:
        s->selector_valid = false;
        s->command_valid = false;
        s->command_len = 0;
        s->response_index = 0;
        s->response_active = false;
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: start(0x24, write)\n");
        break;
    case I2C_START_RECV:
        s->response_index = 0;
        s->response_active = s->command_pending && s->selector_valid &&
                             s->selector == 0x00 && s->command_len == 1;
        if (!s->reset_released || !s->selector_valid || s->command_len != 1 ||
            (s->command_pending && !s->response_active)) {
            qemu_log_mask(LOG_UNIMP,
                          "clarion-tma460: NACK read start selector=0x%02x write_len=%u\n",
                          s->selector, s->command_len);
            return 1;
        }
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: start(0x24, read) selector=0x%02x write_len=%u command=%s\n",
                      s->selector, s->command_len,
                      s->response_active ? "EnterActiveState" : "selector-read");
        break;
    case I2C_FINISH:
        if (s->command_valid && s->command_len == sizeof(s->command)) {
            s->command_pending = true;
        }
        if (s->response_active) {
            s->command_pending = false;
            s->response_active = false;
        }
        qemu_log_mask(LOG_UNIMP, "clarion-tma460: finish\n");
        break;
    case I2C_NACK:
        qemu_log_mask(LOG_UNIMP, "clarion-tma460: nack\n");
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: unimplemented I2C event %d\n",
                      event);
        break;
    }
    return 0;
}

static int clarion_tma460_send(I2CSlave *slave, uint8_t data)
{
    ClarionTma460 *s = CLARION_TMA460(slave);

    if (!s->reset_released) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: NACK write byte=0x%02x reset_released=0\n",
                      data);
        return 1;
    }

    if (!s->command_len) {
        if (data != 0x00 && data != 0x01) {
            qemu_log_mask(LOG_UNIMP,
                          "clarion-tma460: NACK unsupported selector=0x%02x\n",
                          data);
            return 1;
        }
        s->selector = data;
        s->selector_valid = true;
        s->command[0] = data;
        s->command_len = 1;
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: selector 0x%02x ACK\n", data);
        return 0;
    }

    if (s->selector != 0x00 || s->command_len >= sizeof(s->command) ||
        data != clarion_tma460_enter_active[s->command_len]) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: NACK command byte[%u]=0x%02x\n",
                      s->command_len, data);
        s->command_valid = false;
        return 1;
    }

    s->command[s->command_len++] = data;
    s->command_valid = s->command_len == sizeof(s->command);
    qemu_log_mask(LOG_UNIMP,
                  "clarion-tma460: command byte[%u]=0x%02x ACK\n",
                  s->command_len - 1, data);
    if (s->command_valid) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: Enter Active State command accepted\n");
    }
    return 0;
}

static uint8_t clarion_tma460_recv(I2CSlave *slave)
{
    ClarionTma460 *s = CLARION_TMA460(slave);

    if (!s->reset_released || !s->selector_valid ||
        (s->command_len != 1 && !s->command_valid)) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: ERROR read without supported selector\n");
        return 0xff;
    }

    if (s->response_active && s->response_index >= 15) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: ERROR response overread index=%u\n",
                      s->response_index);
        return 0xff;
    }

    if (!s->response_active && s->response_index >= 1) {
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: ERROR selector response overread index=%u\n",
                      s->response_index);
        return 0xff;
    }

    /* T143 selector reads and the T144 checked fields are zero. */
    qemu_log_mask(LOG_UNIMP,
                  "clarion-tma460: read selector=0x%02x response[%u]=0x00\n",
                  s->selector, s->response_index);
    s->response_index++;
    return 0x00;
}

static void clarion_tma460_instance_init(Object *obj)
{
    ClarionTma460 *s = CLARION_TMA460(obj);

    s->reset_released = false;
    s->ready_pulsed = false;
    qdev_init_gpio_out(DEVICE(s), &s->irq, 1);
}

static void clarion_tma460_class_init(ObjectClass *klass, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->event = clarion_tma460_event;
    sc->send = clarion_tma460_send;
    sc->recv = clarion_tma460_recv;
}

static const TypeInfo clarion_tma460_type_info = {
    .name = TYPE_CLARION_TMA460,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(ClarionTma460),
    .instance_init = clarion_tma460_instance_init,
    .class_init = clarion_tma460_class_init,
};

void clarion_tma460_set_reset(DeviceState *dev, bool gpio_level)
{
    ClarionTma460 *s = CLARION_TMA460(dev);
    bool release = gpio_level;

    if (s->reset_released == release) {
        return;
    }
    s->reset_released = release;
    s->selector_valid = false;
    qemu_log_mask(LOG_UNIMP,
                  "clarion-tma460: reset %s (GPIO4.OUTDT bit10=%d)\n",
                  release ? "release" : "assert", gpio_level);

    if (!release) {
        s->ready_pulsed = false;
        return;
    }

    if (!s->ready_pulsed) {
        s->ready_pulsed = true;
        qemu_log_mask(LOG_UNIMP,
                      "clarion-tma460: ready IRQ pulse after reset release\n");
        qemu_irq_raise(s->irq);
        qemu_irq_lower(s->irq);
    }
}

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
            bool send_ready_cleared =
                (s->icmsr & (ICMSR_MDE | ICMSR_MDT)) ==
                    (ICMSR_MDE | ICMSR_MDT) &&
                !(v & ICMSR_MDE) && !(v & ICMSR_MDT);
            bool receive_ready_cleared = (s->icmsr & ICMSR_MDR) &&
                                         !(v & ICMSR_MDR);

            /* Apply the target's W0C before modeling the hardware transition. */
            s->icmsr &= v | ~ICMSR_MASK;

            if (s->phase == RCAR_I2C4_WRITE_DATA && mat_cleared) {
                int ret = i2c_send(s->bus, s->icrxtx);
                s->icrxtx_written = false;

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
            } else if (s->phase == RCAR_I2C4_WRITE_DATA &&
                       send_ready_cleared && s->icrxtx_written) {
                int ret = i2c_send(s->bus, s->icrxtx);
                s->icrxtx_written = false;

                if (ret) {
                    s->icmsr |= ICMSR_MNR;
                    qemu_log_mask(LOG_UNIMP,
                                  "clarion-i2c4: MDE|MDT send NACK byte=0x%02x\n",
                                  s->icrxtx);
                } else {
                    s->icmsr |= ICMSR_MDE | ICMSR_MDT;
                    qemu_log_mask(LOG_UNIMP,
                              "clarion-i2c4: W0C MDE|MDT -> send ICRXTX=0x%02x; set MDE|MDT\n",
                              s->icrxtx);
                }
            } else if (s->phase == RCAR_I2C4_WRITE_DATA &&
                       send_ready_cleared && !s->icrxtx_written &&
                       (s->icmcr & BIT(1))) {
                i2c_end_transfer(s->bus);
                s->bus_active = false;
                s->phase = RCAR_I2C4_IDLE;
                s->icmsr |= BIT(4); /* MST: write STOP completion. */
                qemu_log_mask(LOG_UNIMP,
                              "clarion-i2c4: write FSB completed; set MST\n");
            } else if (s->phase == RCAR_I2C4_READ_ADDRESS && mat_cleared) {
                s->phase = RCAR_I2C4_READ_DATA_WAIT;
                s->icrxtx = i2c_recv(s->bus);
                s->icmsr |= ICMSR_MDR;
                s->read_last_pending = !!(s->icmcr & BIT(1));
                qemu_log_mask(LOG_UNIMP,
                              "clarion-i2c4: read byte supplied=0x%02x; set MDR\n",
                              s->icrxtx);
            }

            if (s->phase == RCAR_I2C4_READ_DATA_WAIT &&
                receive_ready_cleared) {
                if (s->read_last_pending) {
                    i2c_end_transfer(s->bus);
                    s->bus_active = false;
                    s->phase = RCAR_I2C4_IDLE;
                    s->read_last_pending = false;
                    s->icmsr |= BIT(4); /* MST: FSB completion. */
                    qemu_log_mask(LOG_UNIMP,
                                  "clarion-i2c4: FSB completed; set MST\n");
                } else {
                    s->icrxtx = i2c_recv(s->bus);
                    s->icmsr |= ICMSR_MDR;
                    s->read_last_pending = !!(s->icmcr & BIT(1));
                    qemu_log_mask(LOG_UNIMP,
                                  "clarion-i2c4: MDR clear without FSB -> read byte 0x%02x; set MDR\n",
                                  s->icrxtx);
                }
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
        s->icrxtx_written = true;
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
    type_register_static(&clarion_tma460_type_info);
}

type_init(clarion_rcar_i2c4_register_types)
