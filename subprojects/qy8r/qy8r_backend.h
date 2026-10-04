/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Internal backend dispatch table for the qy8r public API. */
#ifndef QY8R_BACKEND_H
#define QY8R_BACKEND_H

#include "qy8r.h"

typedef struct qy8r_backend_ops {
    void *(*open)(char *error, size_t error_size);
    void (*close)(void *context);
    int (*get_info)(void *context, char *buffer, size_t size);
    int (*get_caps)(void *context, int *color_buffer_float, int *tf_essl100);
    void *(*program_create)(void *context, const uint8_t *vs, size_t vs_size,
                            const uint8_t *fs, size_t fs_size, char *log,
                            size_t log_size);
    void *(*program_create_tf)(void *context, const char *vs, const char *fs,
                               const char *const *varyings, int count,
                               char *log, size_t log_size);
    void (*program_destroy)(void *context, void *program);
    void *(*target_create)(void *context, int width, int height, int rgba32f);
    void (*target_destroy)(void *context, void *target);
    int (*target_bind)(void *context, void *target);
    int (*viewport)(void *context, int x, int y, int width, int height);
    int (*clear)(void *context, float r, float g, float b, float a);
    int (*use_program)(void *context, void *program);
    int (*uniform_f32)(void *context, void *program, const char *name,
                       const float *values, int count);
    int (*uniform_i32)(void *context, void *program, const char *name,
                       const int *values, int count);
    int (*texture_rgba32f)(void *context, void *program, const char *sampler,
                           const float *rgba, int unit);
    int (*begin_draw)(void *context, void *program);
    int (*attribute_f32)(void *context, void *program, const char *name,
                         const float *values, int components, int count);
    int (*draw_arrays)(void *context, int mode, int first, int count);
    int (*blend_state)(void *context, int enable, int src_rgb, int dst_rgb,
                       int src_alpha, int dst_alpha, const float color[4]);
    int (*draw_points)(void *context, int count);
    int (*draw_transform_feedback)(void *context, int count, float *out,
                                   size_t float_count);
    int (*read_rgba8)(void *context, void *target, unsigned char rgba[4]);
    int (*read_rgba8_rect)(void *context, void *target, unsigned char *rgba,
                           size_t byte_count);
    int (*read_rgba32f)(void *context, void *target, float rgba[4]);
    int (*texture_upload_rgba8)(void *context, unsigned key, int width,
                                int height, const unsigned char *rgba,
                                size_t byte_count);
    int (*texture_parameter)(void *context, unsigned key, int pname, int value);
    int (*texture_bind)(void *context, unsigned key, int unit);
    int (*target_copy_texture)(void *context, void *target, unsigned key);
    int (*bind_target_texture)(void *context, void *target, int unit);
    int (*texture_set_sampler)(void *context, void *program,
                               const char *sampler, unsigned key, int unit);
    const char *(*last_error)(void *context);
} qy8r_backend_ops;

extern const qy8r_backend_ops qy8r_sw_backend;
#ifdef QY8R_HAVE_GPU
extern const qy8r_backend_ops qy8r_gl_backend;
#endif

#endif
