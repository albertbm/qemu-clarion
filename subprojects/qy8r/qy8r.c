/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Public API and runtime selection between qy8r rendering backends. */
#include "qy8r.h"

#include "qy8r_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct qy8r_context {
    const qy8r_backend_ops *backend;
    void *state;
    char error[512];
} qy8r_context;

static void copy_error(char *destination, size_t size, const char *source)
{
    if (destination && size) {
        snprintf(destination, size, "%s", source ? source : "unknown error");
    }
}

static void set_error(qy8r_context *context, const char *message)
{
    if (context) {
        snprintf(context->error, sizeof(context->error), "%s", message);
    }
}

static qy8r_context *open_backend(const qy8r_backend_ops *backend, char *error,
                                  size_t error_size)
{
    qy8r_context *context = calloc(1, sizeof(*context));
    char backend_error[512] = { 0 };

    if (!context) {
        copy_error(error, error_size, "out of memory creating qy8r context");
        return NULL;
    }
    context->state = backend->open(backend_error, sizeof(backend_error));
    if (!context->state) {
        copy_error(error, error_size, backend_error);
        free(context);
        return NULL;
    }
    context->backend = backend;
    return context;
}

void *qy8r_open(char *error, size_t error_size)
{
    const char *selection = getenv("QY8R_BACKEND");
#ifdef QY8R_HAVE_GPU
    char gpu_error[512] = { 0 };
    qy8r_context *context;
#endif

    if (error && error_size) {
        error[0] = 0;
    }

    if (!selection || !selection[0] || !strcmp(selection, "auto")) {
#ifdef QY8R_HAVE_GPU
        context = open_backend(&qy8r_gl_backend, gpu_error, sizeof(gpu_error));
        if (context) {
            return context;
        }
#endif
        return open_backend(&qy8r_sw_backend, error, error_size);
    }
    if (!strcmp(selection, "cpu")) {
        return open_backend(&qy8r_sw_backend, error, error_size);
    }
    if (!strcmp(selection, "gpu")) {
#ifdef QY8R_HAVE_GPU
        return open_backend(&qy8r_gl_backend, error, error_size);
#else
        copy_error(error, error_size, "GPU backend was not built");
        return NULL;
#endif
    }
    copy_error(error, error_size, "QY8R_BACKEND must be auto, cpu, or gpu");
    return NULL;
}

void qy8r_close(void *opaque)
{
    qy8r_context *context = opaque;

    if (context) {
        context->backend->close(context->state);
        free(context);
    }
}

int qy8r_get_info(void *opaque, char *buffer, size_t size)
{
    qy8r_context *context = opaque;

    return context && context->backend->get_info(context->state, buffer, size);
}

int qy8r_get_caps(void *opaque, int *color_buffer_float, int *tf_essl100)
{
    qy8r_context *context = opaque;

    return context && context->backend->get_caps(
                          context->state, color_buffer_float, tf_essl100);
}

void *qy8r_program_create(void *opaque, const uint8_t *vs, size_t vs_size,
                          const uint8_t *fs, size_t fs_size, char *log,
                          size_t log_size)
{
    qy8r_context *context = opaque;

    return context
               ? context->backend->program_create(context->state, vs, vs_size,
                                                  fs, fs_size, log, log_size)
               : NULL;
}

void *qy8r_program_create_tf(void *opaque, const char *vs, const char *fs,
                             const char *const *varyings, int count, char *log,
                             size_t log_size)
{
    qy8r_context *context = opaque;

    if (!context || !context->backend->program_create_tf) {
        set_error(context, "transform feedback is unavailable in this backend");
        return NULL;
    }
    return context->backend->program_create_tf(context->state, vs, fs, varyings,
                                               count, log, log_size);
}

void qy8r_program_destroy(void *opaque, void *program)
{
    qy8r_context *context = opaque;

    if (context) {
        context->backend->program_destroy(context->state, program);
    }
}

void *qy8r_target_create(void *opaque, int width, int height, int rgba32f)
{
    qy8r_context *context = opaque;

    return context ? context->backend->target_create(context->state, width,
                                                     height, rgba32f)
                   : NULL;
}

void qy8r_target_destroy(void *opaque, void *target)
{
    qy8r_context *context = opaque;

    if (context) {
        context->backend->target_destroy(context->state, target);
    }
}

int qy8r_target_bind(void *opaque, void *target)
{
    qy8r_context *context = opaque;

    return context && context->backend->target_bind(context->state, target);
}

int qy8r_viewport(void *opaque, int x, int y, int width, int height)
{
    qy8r_context *context = opaque;

    return context &&
           context->backend->viewport(context->state, x, y, width, height);
}

int qy8r_clear(void *opaque, float r, float g, float b, float a)
{
    qy8r_context *context = opaque;

    return context && context->backend->clear(context->state, r, g, b, a);
}

int qy8r_use_program(void *opaque, void *program)
{
    qy8r_context *context = opaque;

    return context && context->backend->use_program(context->state, program);
}

int qy8r_uniform_f32(void *opaque, void *program, const char *name,
                     const float *values, int count)
{
    qy8r_context *context = opaque;

    return context && context->backend->uniform_f32(context->state, program,
                                                    name, values, count);
}

int qy8r_uniform_i32(void *opaque, void *program, const char *name,
                     const int *values, int count)
{
    qy8r_context *context = opaque;

    return context && context->backend->uniform_i32(context->state, program,
                                                    name, values, count);
}

int qy8r_texture_rgba32f(void *opaque, void *program, const char *sampler,
                         const float *rgba, int unit)
{
    qy8r_context *context = opaque;

    return context && context->backend->texture_rgba32f(context->state, program,
                                                        sampler, rgba, unit);
}

int qy8r_begin_draw(void *opaque, void *program)
{
    qy8r_context *context = opaque;

    return context && context->backend->begin_draw(context->state, program);
}

int qy8r_attribute_f32(void *opaque, void *program, const char *name,
                       const float *values, int components, int count)
{
    qy8r_context *context = opaque;

    return context &&
           context->backend->attribute_f32(context->state, program, name,
                                           values, components, count);
}

int qy8r_draw_arrays(void *opaque, int mode, int first, int count)
{
    qy8r_context *context = opaque;

    return context &&
           context->backend->draw_arrays(context->state, mode, first, count);
}

int qy8r_blend_state(void *opaque, int enable, int src_rgb, int dst_rgb,
                     int src_alpha, int dst_alpha, const float color[4])
{
    qy8r_context *context = opaque;

    return context &&
           context->backend->blend_state(context->state, enable, src_rgb,
                                         dst_rgb, src_alpha, dst_alpha, color);
}

int qy8r_draw_points(void *opaque, int count)
{
    qy8r_context *context = opaque;

    return context && context->backend->draw_points(context->state, count);
}

int qy8r_draw_transform_feedback(void *opaque, int count, float *out,
                                 size_t float_count)
{
    qy8r_context *context = opaque;

    return context && context->backend->draw_transform_feedback(
                          context->state, count, out, float_count);
}

int qy8r_read_rgba8(void *opaque, void *target, unsigned char rgba[4])
{
    qy8r_context *context = opaque;

    return context &&
           context->backend->read_rgba8(context->state, target, rgba);
}

int qy8r_read_rgba8_rect(void *opaque, void *target, unsigned char *rgba,
                         size_t byte_count)
{
    qy8r_context *context = opaque;

    return context && context->backend->read_rgba8_rect(context->state, target,
                                                        rgba, byte_count);
}

int qy8r_read_rgba32f(void *opaque, void *target, float rgba[4])
{
    qy8r_context *context = opaque;

    return context &&
           context->backend->read_rgba32f(context->state, target, rgba);
}

static int unsupported(qy8r_context *context)
{
    set_error(context, "operation is unsupported by the selected backend");
    return 0;
}

int qy8r_texture_upload_rgba8(void *opaque, unsigned key, int width, int height,
                              const unsigned char *rgba, size_t byte_count)
{
    qy8r_context *context = opaque;

    if (!context) {
        return 0;
    }
    if (!context->backend->texture_upload_rgba8) {
        return unsupported(context);
    }
    return context->backend->texture_upload_rgba8(context->state, key, width,
                                                  height, rgba, byte_count);
}

int qy8r_texture_parameter(void *opaque, unsigned key, int pname, int value)
{
    qy8r_context *context = opaque;

    if (!context) {
        return 0;
    }
    if (!context->backend->texture_parameter) {
        return unsupported(context);
    }
    return context->backend->texture_parameter(context->state, key, pname,
                                               value);
}

int qy8r_texture_bind(void *opaque, unsigned key, int unit)
{
    qy8r_context *context = opaque;

    if (!context) {
        return 0;
    }
    if (!context->backend->texture_bind) {
        return unsupported(context);
    }
    return context->backend->texture_bind(context->state, key, unit);
}

int qy8r_target_copy_texture(void *opaque, void *target, unsigned key)
{
    qy8r_context *context = opaque;

    if (!context) {
        return 0;
    }
    if (!context->backend->target_copy_texture) {
        return unsupported(context);
    }
    return context->backend->target_copy_texture(context->state, target, key);
}

int qy8r_bind_target_texture(void *opaque, void *target, int unit)
{
    qy8r_context *context = opaque;

    if (!context) {
        return 0;
    }
    if (!context->backend->bind_target_texture) {
        return unsupported(context);
    }
    return context->backend->bind_target_texture(context->state, target, unit);
}

int qy8r_texture_set_sampler(void *opaque, void *program, const char *sampler,
                             unsigned key, int unit)
{
    qy8r_context *context = opaque;

    if (!context) {
        return 0;
    }
    if (!context->backend->texture_set_sampler) {
        return unsupported(context);
    }
    return context->backend->texture_set_sampler(context->state, program,
                                                 sampler, key, unit);
}

const char *qy8r_last_error(void *opaque)
{
    qy8r_context *context = opaque;

    if (!context) {
        return "invalid qy8r context";
    }
    if (context->error[0]) {
        return context->error;
    }
    return context->backend->last_error(context->state);
}
