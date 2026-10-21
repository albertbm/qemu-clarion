#ifndef HW_MISC_CLARION_QY7_REGS_H
#define HW_MISC_CLARION_QY7_REGS_H

#include "system/memory.h"

void clarion_qy7_regs_init(MemoryRegion *sysmem, uint8_t dipsw,
                           uint16_t status0_inputs);

#endif
