/*
 * Renesas R-Car I2C master (i2c-rcar)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_I2C_RCAR_I2C_H
#define HW_I2C_RCAR_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RCAR_I2C "rcar-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(RCarI2CState, RCAR_I2C)

#define RCAR_I2C_MMIO_SIZE  0x1000
#define RCAR_I2C_NREGS      (0x40 / 4)

typedef enum RCarI2CPhase {
    RCAR_I2C_IDLE,
    RCAR_I2C_TX,
    RCAR_I2C_RX,
} RCarI2CPhase;

struct RCarI2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;
    QEMUTimer *timer;           /* one bus event per tick */

    uint32_t regs[RCAR_I2C_NREGS];
    uint32_t phase;             /* RCarI2CPhase */
    bool start_req;             /* ESG written while the bus was idle */
    bool tx_full;               /* ICRXTX holds a byte not yet shifted out */
    uint8_t rx;
};

#endif
