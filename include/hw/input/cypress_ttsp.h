/*
 * Cypress TrueTouch (TTSP) touch screen controllers on I2C
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_INPUT_CYPRESS_TTSP_H
#define HW_INPUT_CYPRESS_TTSP_H

#define TYPE_CYPRESS_TTSP "cypress-ttsp"
#define TYPE_CY_TMA460    "cy-tma460"
#define TYPE_CY_TMA616    "cy-tma616"

/* named GPIO lines: "reset" in (active low), "int" out (active low) */
#define CYPRESS_TTSP_RESET "reset"
#define CYPRESS_TTSP_INT   "int"

#endif
