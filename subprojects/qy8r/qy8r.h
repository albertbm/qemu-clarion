/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QY8R_H
#define QY8R_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *qy8r_open(char *error, size_t error_size);
void qy8r_close(void *ctx);
int qy8r_get_info(void *ctx, char *buffer, size_t buffer_size);
int qy8r_get_caps(void *ctx, int *color_buffer_float, int *tf_essl100);
void *qy8r_program_create(void *ctx, const uint8_t *vs, size_t vs_size,
                          const uint8_t *fs, size_t fs_size, char *log,
                          size_t log_size);
void *qy8r_program_create_tf(void *ctx, const char *vs, const char *fs,
                             const char *const *varyings, int varying_count,
                             char *log, size_t log_size);
void qy8r_program_destroy(void *ctx, void *program);
void *qy8r_target_create(void *ctx, int width, int height, int rgba32f);
void qy8r_target_destroy(void *ctx, void *target);
int qy8r_target_bind(void *ctx, void *target);
int qy8r_viewport(void *ctx, int x, int y, int width, int height);
int qy8r_clear(void *ctx, float r, float g, float b, float a);
int qy8r_use_program(void *ctx, void *program);
int qy8r_uniform_f32(void *ctx, void *program, const char *name,
                     const float *values, int count);
int qy8r_uniform_i32(void *ctx, void *program, const char *name,
                     const int *values, int count);
int qy8r_texture_rgba32f(void *ctx, void *program, const char *sampler,
                         const float *rgba, int unit);
int qy8r_begin_draw(void *ctx, void *program);
int qy8r_attribute_f32(void *ctx, void *program, const char *name,
                       const float *values, int components, int count);
int qy8r_draw_arrays(void *ctx, int mode, int first, int count);
int qy8r_blend_state(void *ctx, int enable, int src_rgb, int dst_rgb,
                     int src_alpha, int dst_alpha, const float color[4]);
int qy8r_draw_points(void *ctx, int count);
int qy8r_draw_transform_feedback(void *ctx, int count, float *out,
                                 size_t float_count);
int qy8r_read_rgba8(void *ctx, void *target, unsigned char rgba[4]);
int qy8r_read_rgba8_rect(void *ctx, void *target, unsigned char *rgba,
                         size_t byte_count);
int qy8r_read_rgba32f(void *ctx, void *target, float rgba[4]);
int qy8r_texture_upload_rgba8(void *ctx, unsigned key, int width, int height,
                              const unsigned char *rgba, size_t byte_count);
int qy8r_texture_parameter(void *ctx, unsigned key, int pname, int value);
int qy8r_texture_bind(void *ctx, unsigned key, int unit);
int qy8r_target_copy_texture(void *ctx, void *target, unsigned key);
int qy8r_bind_target_texture(void *ctx, void *target, int unit);
int qy8r_texture_set_sampler(void *ctx, void *program, const char *sampler,
                             unsigned key, int unit);
const char *qy8r_last_error(void *ctx);

#ifdef __cplusplus
}
#endif

#endif
