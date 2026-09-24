/*
 * Renesas CAN controller (R-Car Gen1 / R8A7778) — модель для плати Clarion QY8.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_NET_RENESAS_CAN_H
#define HW_NET_RENESAS_CAN_H

#define TYPE_RENESAS_CAN "renesas-can"

/*
 * Вікно блока. Найбільша адреса, яку чіпає `CAN.dll`, — +0x858; у мапі
 * Linux (`struct rcar_can_regs`) останній регістр — MBSMR за +0x863.
 * Беремо круглу сторінку.
 */
#define RENESAS_CAN_SIZE 0x1000

#endif
