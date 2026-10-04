/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Software rasterizer interfaces derived from qy8gl, PR #5. */
#ifndef QY8R_SW_RASTER_H
#define QY8R_SW_RASTER_H

#include <stddef.h>
#include <stdint.h>

#define QY8R_SW_MAX_VARYINGS 16

typedef struct qy8r_sw_surface {
    int width;
    int height;
    uint8_t *rgba;
} qy8r_sw_surface;

typedef struct qy8r_sw_vertex {
    float x;
    float y;
    float varying[QY8R_SW_MAX_VARYINGS];
} qy8r_sw_vertex;

typedef struct qy8r_sw_texture {
    int width;
    int height;
    const uint8_t *rgba;
    int min_filter;
    int mag_filter;
    int wrap_s;
    int wrap_t;
} qy8r_sw_texture;

typedef struct qy8r_sw_blend {
    int enabled;
    int src_rgb;
    int dst_rgb;
    int src_alpha;
    int dst_alpha;
    float color[4];
} qy8r_sw_blend;

typedef int (*qy8r_sw_fragment_fn)(void *opaque, const float *varyings,
                                   size_t varying_count, float rgba[4]);

int qy8r_sw_raster_triangle(qy8r_sw_surface *surface,
                            const qy8r_sw_vertex vertices[3],
                            size_t varying_count,
                            qy8r_sw_fragment_fn fragment, void *opaque,
                            const qy8r_sw_blend *blend);
int qy8r_sw_sample_texture(const qy8r_sw_texture *texture, float u,
                           float v, float rgba[4]);

#endif
