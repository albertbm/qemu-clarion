/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CLARION_QY8_RENDER_TEXTURE_H
#define CLARION_QY8_RENDER_TEXTURE_H

#include <stdint.h>

static inline int qy8_render_texture_pixel_rgba(uint32_t format,
                                              const uint8_t *source,
                                              uint8_t rgba[4])
{
    uint16_t value;

    if (!source || !rgba) {
        return 0;
    }
    if (format == 0x12) {
        value = (uint16_t)(source[0] | ((uint16_t)source[1] << 8));
        rgba[0] = (uint8_t)(((value >> 10) & 31) * 255 / 31);
        rgba[1] = (uint8_t)(((value >> 5) & 31) * 255 / 31);
        rgba[2] = (uint8_t)((value & 31) * 255 / 31);
        rgba[3] = (value & 0x8000) ? 255 : 0;
        return 1;
    }
    if (format == 0x13) {
        value = (uint16_t)(source[0] | ((uint16_t)source[1] << 8));
        rgba[0] = (uint8_t)(((value >> 8) & 15) * 17);
        rgba[1] = (uint8_t)(((value >> 4) & 15) * 17);
        rgba[2] = (uint8_t)((value & 15) * 17);
        rgba[3] = (uint8_t)(((value >> 12) & 15) * 17);
        return 1;
    }
    if (format == 0x14) {
        rgba[0] = source[2];
        rgba[1] = source[1];
        rgba[2] = source[0];
        rgba[3] = source[3];
        return 1;
    }
    return 0;
}

#endif
