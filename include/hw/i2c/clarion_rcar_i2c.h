#ifndef HW_I2C_CLARION_RCAR_I2C_H
#define HW_I2C_CLARION_RCAR_I2C_H

#include "hw/core/qdev.h"
#include "hw/i2c/i2c.h"

#define TYPE_CLARION_RCAR_I2C4 "clarion-rcar-i2c4"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionRcarI2C4State, CLARION_RCAR_I2C4)

#define TYPE_CLARION_I2C4_RECORDER "clarion-i2c4-recorder"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionI2C4Recorder, CLARION_I2C4_RECORDER)

#endif
