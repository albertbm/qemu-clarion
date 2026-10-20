/*
 * Clarion QY7 RCAN-TL1 compatible register window.
 *
 * The register layout and mailbox count follow the QY7 CAN.dll accesses.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/misc/clarion_rcan.h"
#include "system/reset.h"
#include "trace.h"

#define RCAN_SIZE 0x1000
#define RCAN_MCR 0x000
#define RCAN_GSR 0x002
#define RCAN_BCR1 0x004
#define RCAN_BCR0 0x006
#define RCAN_IRR 0x008
#define RCAN_IMR 0x00a
#define RCAN_TXACK1 0x030
#define RCAN_TXACK0 0x032
#define RCAN_TXPR1 0x020
#define RCAN_TXPR0 0x022
#define RCAN_RXPR1 0x040
#define RCAN_RXPR0 0x042
#define RCAN_MBIMR1 0x050
#define RCAN_MBIMR0 0x052
#define RCAN_MAILBOX_BASE 0x100
#define RCAN_MAILBOX_STRIDE 0x20
#define RCAN_MAILBOX_COUNT 32

#define RCAN_MCR_RESET BIT(0)
#define RCAN_MCR_HALT BIT(1)
#define RCAN_GSR_RESET BIT(3)
#define RCAN_GSR_HALT BIT(4)
#define RCAN_GSR_NOT_BUS_OFF BIT(2)
#define RCAN_IRR_RESET_HALT BIT(0)
#define RCAN_IRR_RX BIT(1)
#define RCAN_IRR_TX BIT(8)

struct ClarionRcan {
    MemoryRegion iomem;
    qemu_irq irq;
    uint16_t regs[RCAN_SIZE / sizeof(uint16_t)];
    bool reset_requested;
    bool halt_requested;
};

static uint16_t *rcan_reg(ClarionRcan *s, hwaddr offset)
{
    return &s->regs[offset / sizeof(uint16_t)];
}

static uint32_t rcan_rxpr(ClarionRcan *s)
{
    return *rcan_reg(s, RCAN_RXPR0) |
           ((uint32_t)*rcan_reg(s, RCAN_RXPR1) << 16);
}

static uint32_t rcan_mbimr(ClarionRcan *s)
{
    return *rcan_reg(s, RCAN_MBIMR0) |
           ((uint32_t)*rcan_reg(s, RCAN_MBIMR1) << 16);
}

static uint32_t rcan_txack(ClarionRcan *s)
{
    return *rcan_reg(s, RCAN_TXACK0) |
           ((uint32_t)*rcan_reg(s, RCAN_TXACK1) << 16);
}

static uint16_t rcan_mailbox_id(ClarionRcan *s, hwaddr base)
{
    uint16_t control0 = *rcan_reg(s, base);
    uint16_t control1 = *rcan_reg(s, base + 2);

    return ((control0 & 0x1f) << 6) | ((control1 >> 10) & 0x3f);
}

static uint16_t rcan_mailbox_id_mask(ClarionRcan *s, hwaddr base)
{
    uint16_t mask0 = *rcan_reg(s, base + 4);
    uint16_t mask1 = *rcan_reg(s, base + 6);

    return ((mask0 & 0x1f) << 6) | ((mask1 >> 10) & 0x3f);
}

static unsigned rcan_mailbox_config(ClarionRcan *s, hwaddr base)
{
    return (*rcan_reg(s, base + 0x10) >> 6) & 7;
}

static void rcan_update_rx_flag(ClarionRcan *s)
{
    uint16_t *irr = rcan_reg(s, RCAN_IRR);

    if (rcan_rxpr(s) & ~rcan_mbimr(s)) {
        *irr |= RCAN_IRR_RX;
    } else {
        *irr &= ~RCAN_IRR_RX;
    }
}

static void rcan_update_irq(ClarionRcan *s)
{
    uint32_t unmasked_txack = rcan_txack(s) & ~rcan_mbimr(s);
    uint16_t *irr = rcan_reg(s, RCAN_IRR);
    uint16_t imr = *rcan_reg(s, RCAN_IMR);
    bool pending;

    rcan_update_rx_flag(s);
    if (unmasked_txack) {
        *irr |= RCAN_IRR_TX;
    } else {
        *irr &= ~RCAN_IRR_TX;
    }
    pending = (*irr & ~imr) != 0;
    qemu_set_irq(s->irq, pending);
}

static void rcan_reset_registers(ClarionRcan *s, uint16_t mcr)
{
    memset(s->regs, 0, sizeof(s->regs));
    *rcan_reg(s, RCAN_MCR) = mcr;
    *rcan_reg(s, RCAN_GSR) = RCAN_GSR_RESET | RCAN_GSR_NOT_BUS_OFF;
    *rcan_reg(s, RCAN_IRR) = RCAN_IRR_RESET_HALT;
    *rcan_reg(s, RCAN_IMR) = 0xffff;
    *rcan_reg(s, RCAN_MBIMR0) = 0xffff;
    *rcan_reg(s, RCAN_MBIMR1) = 0xffff;
    s->reset_requested = true;
    s->halt_requested = false;
    qemu_set_irq(s->irq, 0);
}

static void rcan_start_transmit(ClarionRcan *s, uint32_t requested)
{
    uint32_t txack = rcan_txack(s);

    for (int i = 0; i < RCAN_MAILBOX_COUNT; i++) {
        hwaddr base = RCAN_MAILBOX_BASE + i * RCAN_MAILBOX_STRIDE;
        uint32_t bit = BIT(i);
        unsigned config;

        if (!(requested & bit)) {
            continue;
        }
        config = rcan_mailbox_config(s, base);
        if (s->reset_requested || s->halt_requested || config > 1) {
            continue;
        }
        txack |= bit;
    }
    *rcan_reg(s, RCAN_TXACK0) = txack;
    *rcan_reg(s, RCAN_TXACK1) = txack >> 16;
    *rcan_reg(s, RCAN_TXPR0) &= ~requested;
    *rcan_reg(s, RCAN_TXPR1) &= ~(requested >> 16);
    rcan_update_irq(s);
}

static void rcan_write_mcr(ClarionRcan *s, uint16_t value)
{
    uint16_t *gsr = rcan_reg(s, RCAN_GSR);

    bool old_reset = s->reset_requested;
    bool old_halt = s->halt_requested;

    if ((value & RCAN_MCR_RESET) && !old_reset) {
        rcan_reset_registers(s, value);
    }
    *rcan_reg(s, RCAN_MCR) = value;
    s->reset_requested = value & RCAN_MCR_RESET;
    s->halt_requested = value & RCAN_MCR_HALT;
    if (s->reset_requested) {
        *gsr = RCAN_GSR_RESET | RCAN_GSR_NOT_BUS_OFF;
        *gsr &= ~RCAN_GSR_HALT;
    } else {
        *gsr = RCAN_GSR_NOT_BUS_OFF;
        if (s->halt_requested) {
            *gsr |= RCAN_GSR_HALT;
        } else {
            *gsr &= ~RCAN_GSR_HALT;
        }
    }
    if (old_reset != s->reset_requested || old_halt != s->halt_requested) {
        *rcan_reg(s, RCAN_IRR) |= RCAN_IRR_RESET_HALT;
    }
    rcan_update_irq(s);
}

static uint64_t rcan_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionRcan *s = opaque;
    uint64_t value = 0;

    if (addr + size > RCAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "clarion-rcan: out of range read +0x%03" HWADDR_PRIx
                      " size %u\n",
                      addr, size);
        return 0;
    }
    if (size == 2 && !(addr & 1)) {
        value = *rcan_reg(s, addr);
    } else if (size == 1) {
        uint16_t word = *rcan_reg(s, addr & ~1);

        value = (addr & 1) ? word >> 8 : word & 0xff;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "clarion-rcan: unsupported read +0x%03" HWADDR_PRIx
                      " size %u\n",
                      addr, size);
    }
    trace_clarion_rcan_read(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), addr,
                            value);
    return value;
}

static void rcan_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ClarionRcan *s = opaque;
    uint16_t old_value;

    if (addr + size > RCAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "clarion-rcan: out of range write +0x%03" HWADDR_PRIx
                      " size %u\n",
                      addr, size);
        return;
    }
    if (size == 1) {
        uint16_t *reg = rcan_reg(s, addr & ~1);
        uint16_t byte_mask = (addr & 1) ? 0xff00 : 0x00ff;
        uint16_t byte_value = (addr & 1) ? (value & 0xff) << 8 : value & 0xff;

        *reg = (*reg & ~byte_mask) | byte_value;
    } else if (size == 2 && !(addr & 1)) {
        if (addr == RCAN_MCR) {
            rcan_write_mcr(s, value);
        } else if (addr == RCAN_RXPR0 || addr == RCAN_RXPR1) {
            old_value = *rcan_reg(s, addr);
            *rcan_reg(s, addr) = old_value & ~value;
        } else if (addr == RCAN_IRR) {
            old_value = *rcan_reg(s, addr);
            old_value &= ~value;
            *rcan_reg(s, addr) = old_value;
        } else if (addr == RCAN_TXPR0 || addr == RCAN_TXPR1) {
            *rcan_reg(s, addr) |= value;
            rcan_start_transmit(s, addr == RCAN_TXPR0 ? value : value << 16);
        } else if (addr == RCAN_TXACK0 || addr == RCAN_TXACK1) {
            old_value = *rcan_reg(s, addr);
            *rcan_reg(s, addr) = old_value & ~value;
        } else {
            *rcan_reg(s, addr) = value;
        }
        if (addr == RCAN_RXPR0 || addr == RCAN_RXPR1 || addr == RCAN_MBIMR0 ||
            addr == RCAN_MBIMR1 || addr == RCAN_IMR || addr == RCAN_IRR ||
            addr == RCAN_TXACK0 || addr == RCAN_TXACK1) {
            rcan_update_irq(s);
        }
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "clarion-rcan: unsupported write +0x%03" HWADDR_PRIx
                      " size %u\n",
                      addr, size);
        return;
    }
    trace_clarion_rcan_write(qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), addr,
                             value);
}

static const MemoryRegionOps rcan_ops = {
    .read = rcan_read,
    .write = rcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
};

static void rcan_reset(void *opaque)
{
    ClarionRcan *s = opaque;

    rcan_reset_registers(s, 0x8001);
}

ClarionRcan *clarion_rcan_init(MemoryRegion *sysmem, hwaddr base, qemu_irq irq)
{
    ClarionRcan *s = g_new0(ClarionRcan, 1);

    s->irq = irq;
    memory_region_init_io(&s->iomem, NULL, &rcan_ops, s, "clarion.rcan",
                          RCAN_SIZE);
    memory_region_add_subregion(sysmem, base, &s->iomem);
    qemu_register_reset(rcan_reset, s);
    return s;
}

bool clarion_rcan_inject(ClarionRcan *s, uint16_t id, const uint8_t *data,
                         unsigned len)
{
    g_assert(len <= 8);
    for (int i = RCAN_MAILBOX_COUNT - 1; i >= 0; i--) {
        hwaddr base = RCAN_MAILBOX_BASE + i * RCAN_MAILBOX_STRIDE;
        uint32_t bit = BIT(i);
        unsigned config = rcan_mailbox_config(s, base);

        if ((config != 2 && config != 3) ||
            ((rcan_mailbox_id(s, base) ^ id) & ~rcan_mailbox_id_mask(s, base) &
             0x7ff)) {
            continue;
        }
        for (unsigned j = 0; j < len; j += 2) {
            uint16_t word = data[j];

            if (j + 1 < len) {
                word |= (uint16_t)data[j + 1] << 8;
            }
            *rcan_reg(s, base + 0x08 + j) = word;
        }
        for (unsigned j = len; j < 8; j++) {
            uint16_t *word = rcan_reg(s, base + 0x08 + (j & ~1u));

            if (j & 1) {
                *word &= 0x00ff;
            } else {
                *word &= 0xff00;
            }
        }
        *rcan_reg(s, base) =
            (*rcan_reg(s, base) & ~0x001f) | ((id >> 6) & 0x001f);
        *rcan_reg(s, base + 2) =
            (*rcan_reg(s, base + 2) & ~0xfc00) | ((id & 0x003f) << 10);
        *rcan_reg(s, base + 0x10) =
            (*rcan_reg(s, base + 0x10) & ~0x000f) | (len & 0x000f);
        if (i < 16) {
            *rcan_reg(s, RCAN_RXPR0) |= bit;
        } else {
            *rcan_reg(s, RCAN_RXPR1) |= bit >> 16;
        }
        rcan_update_irq(s);
        qemu_log("clarion-rcan: rx id=%03x mailbox=%u len=%u\n", id, i, len);
        return true;
    }
    return false;
}
