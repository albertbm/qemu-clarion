/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Software triangle rasterization adapted from albertbm's qy8gl plugin,
 * QEMU PR #5, commit 669b76a47114dc653556b5910b4a62c9c66ed886.
 * The shader-specific fragment-color branches and DU frame overlay are not
 * included; fragment colors are supplied by the qy8r program interpreter.
 */
#include "qy8r_sw_raster.h"

#include <math.h>
#include <string.h>

static int wrap_coord(int coord, int size, int mode)
{
    if (mode == 0x2901) {
        coord %= size;
        return coord < 0 ? coord + size : coord;
    }
    if (mode == 0x8370) {
        int period = size * 2;

        coord %= period;
        if (coord < 0) {
            coord += period;
        }
        return coord < size ? coord : period - coord - 1;
    }
    if (coord < 0) {
        return 0;
    }
    return coord >= size ? size - 1 : coord;
}

static float texel(const qy8r_sw_texture *texture, int x, int y, int c)
{
    size_t offset;

    x = wrap_coord(x, texture->width, texture->wrap_s);
    y = wrap_coord(y, texture->height, texture->wrap_t);
    offset = ((size_t)y * texture->width + x) * 4 + c;
    return texture->rgba[offset] / 255.0f;
}

int qy8r_sw_sample_texture(const qy8r_sw_texture *texture, float u, float v,
                           float rgba[4])
{
    int filter;

    if (!texture || !texture->rgba || texture->width < 1 ||
        texture->height < 1 || !rgba) {
        return 0;
    }
    filter = texture->mag_filter;
    if (filter == 0x2600 || filter == 0x2700 || filter == 0x2702) {
        int x = (int)floorf(u * texture->width);
        int y = (int)floorf(v * texture->height);

        for (int c = 0; c < 4; c++) {
            rgba[c] = texel(texture, x, y, c);
        }
        return 1;
    }

    u = u * texture->width - 0.5f;
    v = v * texture->height - 0.5f;
    int x0 = (int)floorf(u);
    int y0 = (int)floorf(v);
    float fx = u - x0;
    float fy = v - y0;
    for (int c = 0; c < 4; c++) {
        float a = texel(texture, x0, y0, c);
        float b = texel(texture, x0 + 1, y0, c);
        float d = texel(texture, x0, y0 + 1, c);
        float e = texel(texture, x0 + 1, y0 + 1, c);

        rgba[c] = (a + (b - a) * fx) * (1.0f - fy) + (d + (e - d) * fx) * fy;
    }
    return 1;
}

static float blend_factor(int factor, const float *src, const float *dst,
                          const float *constant, int channel)
{
    switch (factor) {
    case 0x0000:
        return 0.0f;
    case 0x0001:
        return 1.0f;
    case 0x0300:
        return src[channel];
    case 0x0301:
        return 1.0f - src[channel];
    case 0x0302:
        return src[3];
    case 0x0303:
        return 1.0f - src[3];
    case 0x0304:
        return dst[3];
    case 0x0305:
        return 1.0f - dst[3];
    case 0x0306:
        return dst[channel];
    case 0x0307:
        return 1.0f - dst[channel];
    case 0x8001:
        return constant[channel];
    case 0x8002:
        return 1.0f - constant[channel];
    case 0x8003:
        return constant[3];
    case 0x8004:
        return 1.0f - constant[3];
    default:
        return 1.0f;
    }
}

static uint8_t quantize(float value)
{
    if (!(value > 0.0f)) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return (uint8_t)(value * 255.0f + 0.5f);
}

int qy8r_sw_raster_triangle(qy8r_sw_surface *surface,
                            const qy8r_sw_vertex vertices[3],
                            size_t varying_count, qy8r_sw_fragment_fn fragment,
                            void *opaque, const qy8r_sw_blend *blend)
{
    float area;
    int x0, x1, y0, y1;

    if (!surface || !surface->rgba || surface->width < 1 ||
        surface->height < 1 || !vertices || !fragment || !blend ||
        varying_count > QY8R_SW_MAX_VARYINGS) {
        return 0;
    }
    area = (vertices[1].x - vertices[0].x) * (vertices[2].y - vertices[0].y) -
           (vertices[2].x - vertices[0].x) * (vertices[1].y - vertices[0].y);
    if (fabsf(area) < 0.000001f) {
        return 1;
    }
    x0 = (int)floorf(fminf(vertices[0].x, fminf(vertices[1].x, vertices[2].x)));
    x1 = (int)ceilf(fmaxf(vertices[0].x, fmaxf(vertices[1].x, vertices[2].x)));
    y0 = (int)floorf(fminf(vertices[0].y, fminf(vertices[1].y, vertices[2].y)));
    y1 = (int)ceilf(fmaxf(vertices[0].y, fmaxf(vertices[1].y, vertices[2].y)));
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= surface->width) {
        x1 = surface->width - 1;
    }
    if (y1 >= surface->height) {
        y1 = surface->height - 1;
    }

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f;
            float py = y + 0.5f;
            float w0 = ((vertices[1].x - px) * (vertices[2].y - py) -
                        (vertices[2].x - px) * (vertices[1].y - py)) /
                       area;
            float w1 = ((vertices[2].x - px) * (vertices[0].y - py) -
                        (vertices[0].x - px) * (vertices[2].y - py)) /
                       area;
            float w2 = 1.0f - w0 - w1;
            float varying[QY8R_SW_MAX_VARYINGS];
            float src[4];
            size_t offset;
            uint8_t *dst8;
            float dst[4];

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }
            for (size_t i = 0; i < varying_count; i++) {
                varying[i] = w0 * vertices[0].varying[i] +
                             w1 * vertices[1].varying[i] +
                             w2 * vertices[2].varying[i];
            }
            int drawn = fragment(opaque, varying, varying_count, src);

            if (drawn < 0) {
                return 0;
            }
            if (!drawn) {
                continue;
            }
            offset = ((size_t)y * surface->width + x) * 4;
            dst8 = surface->rgba + offset;
            for (int c = 0; c < 4; c++) {
                dst[c] = dst8[c] / 255.0f;
            }
            for (int c = 0; c < 4; c++) {
                float value = src[c];

                if (blend->enabled) {
                    int sf = c < 3 ? blend->src_rgb : blend->src_alpha;
                    int df = c < 3 ? blend->dst_rgb : blend->dst_alpha;

                    value =
                        src[c] * blend_factor(sf, src, dst, blend->color, c) +
                        dst[c] * blend_factor(df, src, dst, blend->color, c);
                }
                dst8[c] = quantize(value);
            }
        }
    }
    return 1;
}
