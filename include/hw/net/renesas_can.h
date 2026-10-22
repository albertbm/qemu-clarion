/*
 * Renesas CAN controller (R-Car Gen1 / R8A7778) model for the Clarion QY8 board.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_RENESAS_CAN_H
#define HW_NET_RENESAS_CAN_H

#define TYPE_RENESAS_CAN "renesas-can"

/*
 * Block window. The highest address accessed by `CAN.dll` is +0x858; in the
 * Linux map (`struct rcar_can_regs`), the last register is MBSMR at +0x863.
 * Use one aligned page.
 */
#define RENESAS_CAN_SIZE 0x1000

#endif
