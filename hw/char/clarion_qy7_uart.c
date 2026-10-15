/*
 * Clarion QY7221NL sub-CPU link UART.
 *
 * CAN.dll of the QY7221NL image drives a 16850-class UART at physical
 * 0x19B00000 (virtual 0xB00000 in the driver process).  The registers sit on
 * a 2-byte stride and are accessed as halfwords; only the low byte is
 * significant.  Register use is taken from CAN.dll (init 0x3DCCF70..0x3DCD088,
 * receive handler 0x3DCC834, transmit kick 0x3DCC128):
 *
 *   +0x00 RBR/THR (DLL when LCR.DLAB)   +0x08 MCR
 *   +0x02 IER     (DLM when LCR.DLAB)   +0x0A LSR
 *   +0x04 IIR/FCR                       +0x0C MSR
 *   +0x06 LCR                           +0x0E SCR
 *   +0x1C transmit FIFO level (byte access)
 *
 * Writes the driver makes to LSR (+0x0A, 0x40) and MSR (+0x0C, 0x10) during
 * init are stored and ignored.  The wire is the sub-CPU model's business:
 * transmitted bytes go to the TX callback, received bytes arrive through
 * clarion_qy7_uart_rx().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/clarion_qy7_uart.h"
#include "migration/vmstate.h"

#define UART_SIZE   0x20
#define FIFO_SIZE   128

#define REG_DATA    0
#define REG_IER     1
#define REG_IIR     2   /* FCR on write */
#define REG_LCR     3
#define REG_MCR     4
#define REG_LSR     5
#define REG_MSR     6
#define REG_SCR     7
#define REG_TXLVL   14  /* offset 0x1C */

#define IER_RX      0x01
#define IER_THRE    0x02
#define IER_RLS     0x04
#define LCR_DLAB    0x80
#define LSR_DR      0x01
#define LSR_OE      0x02
#define LSR_THRE    0x20
#define LSR_TEMT    0x40
#define IIR_NONE    0x01
#define IIR_THRE    0x02
#define IIR_RX      0x04
#define IIR_RLS     0x06

struct ClarionQy7UartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t fcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    uint8_t lsr_err;
    bool thre_pending;

    uint8_t rx[FIFO_SIZE];
    unsigned rx_head;
    unsigned rx_count;

    ClarionQy7UartTxFn tx_fn;
    void *tx_opaque;
};

static bool uart_irq_level(ClarionQy7UartState *s)
{
    return ((s->ier & IER_RX) && s->rx_count) ||
           ((s->ier & IER_RLS) && s->lsr_err) ||
           ((s->ier & IER_THRE) && s->thre_pending);
}

static void uart_update(ClarionQy7UartState *s)
{
    qemu_set_irq(s->irq, uart_irq_level(s));
}

static uint8_t uart_iir(ClarionQy7UartState *s)
{
    uint8_t iir = (s->fcr & 1) ? 0xc0 : 0;

    if ((s->ier & IER_RLS) && s->lsr_err) {
        return iir | IIR_RLS;
    }
    if ((s->ier & IER_RX) && s->rx_count) {
        return iir | IIR_RX;
    }
    if ((s->ier & IER_THRE) && s->thre_pending) {
        return iir | IIR_THRE;
    }
    return iir | IIR_NONE;
}

static uint8_t uart_lsr(ClarionQy7UartState *s)
{
    return (s->rx_count ? LSR_DR : 0) | s->lsr_err | LSR_THRE | LSR_TEMT;
}

static uint64_t uart_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionQy7UartState *s = opaque;
    unsigned reg = addr >> 1;
    uint8_t val = 0;

    if (addr & 1) {
        return 0;
    }
    switch (reg) {
    case REG_DATA:
        if (s->lcr & LCR_DLAB) {
            val = s->dll;
        } else if (s->rx_count) {
            val = s->rx[s->rx_head];
            s->rx_head = (s->rx_head + 1) % FIFO_SIZE;
            s->rx_count--;
        }
        break;
    case REG_IER:
        val = (s->lcr & LCR_DLAB) ? s->dlm : s->ier;
        break;
    case REG_IIR:
        val = uart_iir(s);
        if ((val & 0x0f) == IIR_THRE) {
            s->thre_pending = false;
        }
        break;
    case REG_LCR:
        val = s->lcr;
        break;
    case REG_MCR:
        val = s->mcr;
        break;
    case REG_LSR:
        val = uart_lsr(s);
        s->lsr_err = 0;
        break;
    case REG_MSR:
        val = 0;
        break;
    case REG_SCR:
        val = s->scr;
        break;
    case REG_TXLVL:
        val = 0;    /* bytes are consumed at once, the FIFO is always empty */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "clarion-qy7-uart: unimp read +0x%02"
                      HWADDR_PRIx "\n", addr);
    }
    uart_update(s);
    return val;
}

static void uart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ClarionQy7UartState *s = opaque;
    unsigned reg = addr >> 1;
    uint8_t v = val;

    if (addr & 1) {
        return;
    }
    switch (reg) {
    case REG_DATA:
        if (s->lcr & LCR_DLAB) {
            s->dll = v;
            break;
        }
        s->thre_pending = false;
        if (s->tx_fn) {
            s->tx_fn(s->tx_opaque, v);
        }
        s->thre_pending = true;
        break;
    case REG_IER:
        if (s->lcr & LCR_DLAB) {
            s->dlm = v;
        } else {
            /* Enabling the THRE interrupt with an empty THR raises it. */
            if ((v & IER_THRE) && !(s->ier & IER_THRE)) {
                s->thre_pending = true;
            }
            s->ier = v & 0x0f;
        }
        break;
    case REG_IIR:
        s->fcr = v;
        if (v & 0x02) {
            s->rx_count = 0;
            s->rx_head = 0;
        }
        break;
    case REG_LCR:
        s->lcr = v;
        break;
    case REG_MCR:
        s->mcr = v;
        break;
    case REG_LSR:
    case REG_MSR:
        break;
    case REG_SCR:
        s->scr = v;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "clarion-qy7-uart: unimp write +0x%02"
                      HWADDR_PRIx " = 0x%02x\n", addr, v);
    }
    uart_update(s);
}

static const MemoryRegionOps uart_ops = {
    .read = uart_read,
    .write = uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

void clarion_qy7_uart_set_tx(DeviceState *dev, ClarionQy7UartTxFn fn,
                             void *opaque)
{
    ClarionQy7UartState *s = CLARION_QY7_UART(dev);

    s->tx_fn = fn;
    s->tx_opaque = opaque;
}

int clarion_qy7_uart_rx_room(DeviceState *dev)
{
    ClarionQy7UartState *s = CLARION_QY7_UART(dev);

    return FIFO_SIZE - s->rx_count;
}

int clarion_qy7_uart_rx(DeviceState *dev, const uint8_t *buf, int len)
{
    ClarionQy7UartState *s = CLARION_QY7_UART(dev);
    int n = MIN(len, FIFO_SIZE - (int)s->rx_count);

    for (int i = 0; i < n; i++) {
        s->rx[(s->rx_head + s->rx_count) % FIFO_SIZE] = buf[i];
        s->rx_count++;
    }
    uart_update(s);
    return n;
}

static void uart_reset_hold(Object *obj, ResetType type)
{
    ClarionQy7UartState *s = CLARION_QY7_UART(obj);

    s->ier = s->lcr = s->mcr = s->fcr = s->scr = 0;
    s->dll = s->dlm = 0;
    s->lsr_err = 0;
    s->thre_pending = false;
    s->rx_head = s->rx_count = 0;
    qemu_set_irq(s->irq, 0);
}

static void uart_realize(DeviceState *dev, Error **errp)
{
    ClarionQy7UartState *s = CLARION_QY7_UART(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &uart_ops, s,
                          "clarion-qy7-uart", UART_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = uart_realize;
    dc->desc = "Clarion QY7221NL sub-CPU link UART";
    rc->phases.hold = uart_reset_hold;
}

static const TypeInfo uart_types[] = {
    {
        .name = TYPE_CLARION_QY7_UART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(ClarionQy7UartState),
        .class_init = uart_class_init,
    },
};

DEFINE_TYPES(uart_types)
