/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Headless qy8r frontend using typed USP execution and CPU rasterization. */
#include "qy8r_sw_api.h"

#include "qy8r_sw_raster.h"
#include "qy8r_usp.h"
#include "qy8r_usp_cpu.h"

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QY8R_SW_MAX_TARGETS 64
#define QY8R_SW_MAX_TEXTURES 256
#define QY8R_SW_MAX_ATTRIBUTES 128
#define QY8R_SW_MAX_SYMBOLS QY8R_USP_MAX_SYMBOLS
#define QY8R_SW_MAX_VERTICES 4096
#define QY8R_SW_MAX_VARYING_FLOATS 16

typedef struct qy8r_sw_target {
    int width;
    int height;
    int rgba32f;
    uint8_t *rgba;
} qy8r_sw_target;

typedef struct qy8r_sw_texture_entry {
    unsigned key;
    qy8r_sw_texture texture;
    uint8_t *owned_rgba;
    int live;
} qy8r_sw_texture_entry;

typedef struct qy8r_sw_attribute {
    float *values;
    int components;
    int count;
} qy8r_sw_attribute;

typedef struct qy8r_sw_target_slot {
    qy8r_sw_target *target;
} qy8r_sw_target_slot;

typedef struct qy8r_sw_program {
    qy8r_usp_program *vs;
    qy8r_usp_program *fs;
    float vs_uniform_values[QY8R_USP_MAX_SYMBOLS][64];
    uint16_t vs_uniform_counts[QY8R_USP_MAX_SYMBOLS];
    float fs_uniform_values[QY8R_USP_MAX_SYMBOLS][64];
    uint16_t fs_uniform_counts[QY8R_USP_MAX_SYMBOLS];
    int sampler_units[QY8R_USP_MAX_SYMBOLS];
    qy8r_sw_attribute attributes[QY8R_SW_MAX_ATTRIBUTES];
    float *point_outputs;
} qy8r_sw_program;

typedef struct qy8r_sw_context {
    qy8r_sw_target_slot targets[QY8R_SW_MAX_TARGETS];
    unsigned target_count;
    qy8r_sw_target *target;
    qy8r_sw_program *program;
    qy8r_sw_texture_entry textures[QY8R_SW_MAX_TEXTURES];
    unsigned bound_textures[16];
    int viewport[4];
    qy8r_sw_blend blend;
    char error[512];
} qy8r_sw_context;

typedef struct qy8r_sw_fragment_context {
    qy8r_sw_context *context;
    qy8r_sw_program *program;
    float varying[QY8R_SW_MAX_VARYING_FLOATS];
    size_t varying_count;
} qy8r_sw_fragment_context;

static void set_error(qy8r_sw_context *context, const char *message)
{
    if (context) {
        snprintf(context->error, sizeof(context->error), "%s", message);
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

static int symbol_index(const qy8r_usp_program *program, const char *name)
{
    unsigned i;

    if (!program || !name) {
        return -1;
    }
    for (i = 0; i < program->symbol_count; i++) {
        if (!strcmp(program->symbols[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static qy8r_sw_texture_entry *texture_get(qy8r_sw_context *context,
                                          unsigned key, int create)
{
    qy8r_sw_texture_entry *free_entry = NULL;
    unsigned i;

    if (!context || !key) {
        return NULL;
    }
    for (i = 0; i < QY8R_SW_MAX_TEXTURES; i++) {
        qy8r_sw_texture_entry *entry = &context->textures[i];

        if (entry->live && entry->key == key) {
            return entry;
        }
        if (!entry->live && !free_entry) {
            free_entry = entry;
        }
    }
    if (!create || !free_entry) {
        return NULL;
    }
    memset(free_entry, 0, sizeof(*free_entry));
    free_entry->live = 1;
    free_entry->key = key;
    free_entry->texture.min_filter = 0x2601;
    free_entry->texture.mag_filter = 0x2601;
    free_entry->texture.wrap_s = 0x2901;
    free_entry->texture.wrap_t = 0x2901;
    return free_entry;
}

static int read_file_blob(const uint8_t *bytes, size_t size,
                          qy8r_usp_program **program, char *log,
                          size_t log_size)
{
    qy8r_usp_error error;

    *program = calloc(1, sizeof(**program));
    if (!*program) {
        if (log && log_size) {
            snprintf(log, log_size, "out of memory allocating USP program");
        }
        return 0;
    }
    if (!qy8r_usp_parse(bytes, size, 0, *program, &error)) {
        if (log && log_size) {
            snprintf(log, log_size, "%s at 0x%zx: %s",
                     qy8r_usp_error_name(error.code), error.offset,
                     error.message);
        }
        free(*program);
        *program = NULL;
        return 0;
    }
    return 1;
}

static void destroy_program(qy8r_sw_program *program)
{
    unsigned i;

    if (!program) {
        return;
    }
    for (i = 0; i < QY8R_SW_MAX_ATTRIBUTES; i++) {
        free(program->attributes[i].values);
    }
    free(program->point_outputs);
    free(program->vs);
    free(program->fs);
    free(program);
}

void *qy8r_open(char *error, size_t error_size)
{
    qy8r_sw_context *context = calloc(1, sizeof(*context));

    if (!context) {
        if (error && error_size) {
            snprintf(error, error_size,
                     "out of memory creating software renderer");
        }
        return NULL;
    }
    context->blend.src_rgb = 1;
    context->blend.src_alpha = 1;
    if (error && error_size) {
        error[0] = 0;
    }
    return context;
}

void qy8r_close(void *opaque)
{
    qy8r_sw_context *context = opaque;
    unsigned i;

    if (!context) {
        return;
    }
    for (i = 0; i < context->target_count; i++) {
        if (context->targets[i].target) {
            free(context->targets[i].target->rgba);
            free(context->targets[i].target);
        }
    }
    for (i = 0; i < QY8R_SW_MAX_TEXTURES; i++) {
        free(context->textures[i].owned_rgba);
    }
    free(context);
}

int qy8r_get_info(void *opaque, char *buffer, size_t size)
{
    qy8r_sw_context *context = opaque;

    if (!context || !buffer || !size) {
        return 0;
    }
    snprintf(buffer, size, "BACKEND=CPU\nRENDERER=qy8r software USP\n");
    return 1;
}

int qy8r_get_caps(void *opaque, int *color_buffer_float, int *tf_essl100)
{
    if (!opaque) {
        return 0;
    }
    if (color_buffer_float) {
        *color_buffer_float = 0;
    }
    if (tf_essl100) {
        *tf_essl100 = 0;
    }
    return 1;
}

void *qy8r_program_create(void *opaque, const uint8_t *vs, size_t vs_size,
                          const uint8_t *fs, size_t fs_size, char *log,
                          size_t log_size)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_program *program;
    unsigned i;

    if (!context || !vs || !fs || !vs_size || !fs_size) {
        set_error(context, "program creation needs vertex and fragment blobs");
        return NULL;
    }
    if (log && log_size) {
        log[0] = 0;
    }
    program = calloc(1, sizeof(*program));
    if (!program) {
        set_error(context, "out of memory creating software program");
        return NULL;
    }
    if (!read_file_blob(vs, vs_size, &program->vs, log, log_size) ||
        !read_file_blob(fs, fs_size, &program->fs, log, log_size)) {
        destroy_program(program);
        return NULL;
    }
    if (program->vs->stage != 0 || program->fs->stage != 1) {
        if (log && log_size) {
            snprintf(log, log_size, "shader stages are not vertex/fragment");
        }
        destroy_program(program);
        return NULL;
    }
    for (i = 0; i < QY8R_USP_MAX_SYMBOLS; i++) {
        program->sampler_units[i] = -1;
    }
    for (i = 0; i < program->fs->bindings.sampler_count; i++) {
        unsigned symbol = program->fs->bindings.samplers[i];
        if (symbol < program->fs->symbol_count) {
            program->sampler_units[symbol] =
                program->fs->symbols[symbol].base_comp_or_texture_unit;
        }
    }
    return program;
}

void qy8r_program_destroy(void *opaque, void *program)
{
    (void)opaque;
    destroy_program(program);
}

void *qy8r_target_create(void *opaque, int width, int height, int rgba32f)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_target *target;
    size_t bytes;

    if (!context || width < 1 || height < 1 ||
        context->target_count >= QY8R_SW_MAX_TARGETS) {
        set_error(context, "invalid render target size or capacity");
        return NULL;
    }
    if (rgba32f) {
        set_error(context,
                  "RGBA32F targets are unsupported by byte-backed rasterizer");
        return NULL;
    }
    if ((size_t)width > SIZE_MAX / (size_t)height / 4) {
        set_error(context, "render target size overflows host memory");
        return NULL;
    }
    target = calloc(1, sizeof(*target));
    if (!target) {
        set_error(context, "out of memory creating render target");
        return NULL;
    }
    bytes = (size_t)width * (size_t)height * 4;
    target->rgba = calloc(bytes, 1);
    if (!target->rgba) {
        free(target);
        set_error(context, "out of memory allocating render target pixels");
        return NULL;
    }
    target->width = width;
    target->height = height;
    target->rgba32f = !!rgba32f;
    context->targets[context->target_count++].target = target;
    return target;
}

void qy8r_target_destroy(void *opaque, void *target_ptr)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_target *target = target_ptr;
    unsigned i;

    if (!target) {
        return;
    }
    if (context && context->target == target) {
        context->target = NULL;
    }
    if (context) {
        for (i = 0; i < context->target_count; i++) {
            if (context->targets[i].target == target) {
                context->targets[i] = context->targets[--context->target_count];
                break;
            }
        }
    }
    free(target->rgba);
    free(target);
}

int qy8r_target_bind(void *opaque, void *target_ptr)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_target *target = target_ptr;

    if (!context || !target) {
        set_error(context, "invalid render target bind");
        return 0;
    }
    context->target = target;
    context->viewport[0] = 0;
    context->viewport[1] = 0;
    context->viewport[2] = target->width;
    context->viewport[3] = target->height;
    return 1;
}

int qy8r_viewport(void *opaque, int x, int y, int width, int height)
{
    qy8r_sw_context *context = opaque;

    if (!context || !context->target || width < 1 || height < 1) {
        set_error(context, "invalid viewport");
        return 0;
    }
    context->viewport[0] = x;
    context->viewport[1] = y;
    context->viewport[2] = width;
    context->viewport[3] = height;
    return 1;
}

int qy8r_clear(void *opaque, float r, float g, float b, float a)
{
    qy8r_sw_context *context = opaque;
    size_t pixels, i;
    uint8_t color[4];

    if (!context || !context->target) {
        set_error(context, "clear without a bound target");
        return 0;
    }
    color[0] = quantize(r);
    color[1] = quantize(g);
    color[2] = quantize(b);
    color[3] = quantize(a);
    pixels = (size_t)context->target->width * context->target->height;
    for (i = 0; i < pixels; i++) {
        memcpy(context->target->rgba + i * 4, color, 4);
    }
    return 1;
}

int qy8r_use_program(void *opaque, void *program)
{
    qy8r_sw_context *context = opaque;

    if (!context || !program) {
        set_error(context, "invalid program bind");
        return 0;
    }
    context->program = program;
    return 1;
}

static int update_uniform(qy8r_sw_program *program, const char *name,
                          const float *values, const int *integer_values,
                          int count, int integer)
{
    int vs_index;
    int fs_index;

    if (!program || !name || count < 1 || (!values && !integer_values)) {
        return 0;
    }
    fs_index = symbol_index(program->fs, name);
    vs_index = symbol_index(program->vs, name);
    if (fs_index < 0 && vs_index < 0) {
        return 2;
    }
    if (count > 64 ||
        (vs_index >= 0 &&
         count > program->vs->symbols[vs_index].component_count) ||
        (fs_index >= 0 &&
         count > program->fs->symbols[fs_index].component_count)) {
        return 0;
    }
    if (integer) {
        if (fs_index >= 0 && program->fs->symbols[fs_index].type == 24) {
            program->sampler_units[fs_index] = integer_values[0];
        }
        for (int i = 0; i < count; i++) {
            if (vs_index >= 0) {
                program->vs_uniform_values[vs_index][i] =
                    (float)integer_values[i];
            }
            if (fs_index >= 0) {
                program->fs_uniform_values[fs_index][i] =
                    (float)integer_values[i];
            }
        }
    } else {
        if (vs_index >= 0) {
            memcpy(program->vs_uniform_values[vs_index], values,
                   (size_t)count * sizeof(*values));
        }
        if (fs_index >= 0) {
            memcpy(program->fs_uniform_values[fs_index], values,
                   (size_t)count * sizeof(*values));
        }
    }
    if (vs_index >= 0) {
        program->vs_uniform_counts[vs_index] = (uint16_t)count;
    }
    if (fs_index >= 0) {
        program->fs_uniform_counts[fs_index] = (uint16_t)count;
    }
    return 1;
}

int qy8r_uniform_f32(void *opaque, void *program, const char *name,
                     const float *values, int count)
{
    (void)opaque;
    return update_uniform(program, name, values, NULL, count, 0);
}

int qy8r_uniform_i32(void *opaque, void *program, const char *name,
                     const int *values, int count)
{
    (void)opaque;
    return update_uniform(program, name, NULL, values, count, 1);
}

int qy8r_texture_upload_rgba8(void *opaque, unsigned key, int width, int height,
                              const unsigned char *rgba, size_t byte_count)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_texture_entry *entry;
    size_t required;
    uint8_t *copy;

    if (!context || !rgba || width < 1 || height < 1 ||
        (size_t)width > SIZE_MAX / (size_t)height / 4) {
        set_error(context, "invalid RGBA8 texture upload");
        return 0;
    }
    required = (size_t)width * height * 4;
    if (byte_count < required) {
        set_error(context, "RGBA8 texture data is truncated");
        return 0;
    }
    copy = malloc(required);
    if (!copy) {
        set_error(context, "out of memory uploading RGBA8 texture");
        return 0;
    }
    memcpy(copy, rgba, required);
    entry = texture_get(context, key, 1);
    if (!entry) {
        free(copy);
        set_error(context, "texture table is full");
        return 0;
    }
    free(entry->owned_rgba);
    entry->owned_rgba = copy;
    entry->texture.width = width;
    entry->texture.height = height;
    entry->texture.rgba = copy;
    return 1;
}

int qy8r_texture_parameter(void *opaque, unsigned key, int pname, int value)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_texture_entry *entry = texture_get(context, key, 1);

    if (!entry) {
        set_error(context, "texture table is full");
        return 0;
    }
    if (pname == 0x2800) {
        entry->texture.mag_filter = value;
    } else if (pname == 0x2801) {
        entry->texture.min_filter = value;
    } else if (pname == 0x2802) {
        entry->texture.wrap_s = value;
    } else if (pname == 0x2803) {
        entry->texture.wrap_t = value;
    } else {
        set_error(context, "unsupported texture parameter");
        return 0;
    }
    return 1;
}

int qy8r_texture_bind(void *opaque, unsigned key, int unit)
{
    qy8r_sw_context *context = opaque;

    if (!context || unit < 0 || unit >= 16 ||
        (key && !texture_get(context, key, 1))) {
        set_error(context, "invalid texture unit or texture id");
        return 0;
    }
    context->bound_textures[unit] = key;
    return 1;
}

int qy8r_texture_rgba32f(void *opaque, void *program, const char *sampler,
                         const float *rgba, int unit)
{
    qy8r_sw_context *context = opaque;
    uint8_t pixel[4];
    int sampler_unit = unit;

    if (!context || !rgba || unit < 0 || unit >= 16) {
        set_error(context, "invalid float texture upload");
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        pixel[i] = quantize(rgba[i]);
    }
    if (!qy8r_texture_upload_rgba8(context, (unsigned)(unit + 1), 1, 1, pixel,
                                   sizeof(pixel))) {
        return 0;
    }
    if (sampler) {
        int status =
            qy8r_uniform_i32(context, program, sampler, &sampler_unit, 1);
        if (!status) {
            return 0;
        }
    }
    return qy8r_texture_bind(context, (unsigned)(unit + 1), unit);
}

int qy8r_texture_set_sampler(void *opaque, void *program, const char *sampler,
                             unsigned key, int unit)
{
    int status;

    if (unit < 0 || unit >= 16 || !qy8r_texture_bind(opaque, key, unit)) {
        return 0;
    }
    status = qy8r_uniform_i32(opaque, program, sampler, &unit, 1);
    return status == 2 ? 0 : status;
}

int qy8r_begin_draw(void *opaque, void *program)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_program *active = program;
    unsigned i;

    if (!context || !active) {
        set_error(context, "begin draw without a program");
        return 0;
    }
    context->program = active;
    for (i = 0; i < QY8R_SW_MAX_ATTRIBUTES; i++) {
        free(active->attributes[i].values);
        memset(&active->attributes[i], 0, sizeof(active->attributes[i]));
    }
    return 1;
}

int qy8r_attribute_f32(void *opaque, void *program, const char *name,
                       const float *values, int components, int count)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_program *active = program;
    int index;
    float *copy;

    if (!context || !active || !name || !values || components < 1 ||
        components > 4 || count < 1 || count > QY8R_SW_MAX_VERTICES) {
        set_error(context, "invalid vertex attribute");
        return 0;
    }
    index = symbol_index(active->vs, name);
    if (index < 0) {
        return 2;
    }
    if ((size_t)count > SIZE_MAX / (size_t)components / sizeof(float)) {
        set_error(context, "vertex attribute size overflows");
        return 0;
    }
    copy = malloc((size_t)count * components * sizeof(float));
    if (!copy) {
        set_error(context, "out of memory storing vertex attribute");
        return 0;
    }
    memcpy(copy, values, (size_t)count * components * sizeof(float));
    free(active->attributes[index].values);
    active->attributes[index].values = copy;
    active->attributes[index].components = components;
    active->attributes[index].count = count;
    return 1;
}

static int cpu_texture_sample(void *opaque, unsigned texture, float u, float v,
                              uint32_t rgba[4])
{
    qy8r_sw_fragment_context *fragment = opaque;
    qy8r_sw_context *context = fragment->context;
    qy8r_sw_program *program = fragment->program;
    unsigned unit = texture;
    unsigned i;
    qy8r_sw_texture_entry *entry;
    float sampled[4];

    if (unit >= 16 || !context->bound_textures[unit]) {
        for (i = 0; i < QY8R_SW_MAX_SYMBOLS; i++) {
            if (program->sampler_units[i] >= 0 &&
                program->sampler_units[i] < 16 &&
                context->bound_textures[program->sampler_units[i]]) {
                unit = (unsigned)program->sampler_units[i];
                break;
            }
        }
    }
    if (unit >= 16 || !context->bound_textures[unit]) {
        rgba[0] = 0;
        rgba[1] = 0;
        rgba[2] = 0;
        rgba[3] = 0x3f800000;
        return 1;
    }
    entry = texture_get(context, context->bound_textures[unit], 0);
    if (!entry || !qy8r_sw_sample_texture(&entry->texture, u, v, sampled)) {
        set_error(context, "sampling an unavailable texture");
        return 0;
    }
    for (i = 0; i < 4; i++) {
        rgba[i] = 0;
        memcpy(&rgba[i], &sampled[i], sizeof(sampled[i]));
    }
    return 1;
}

static int run_vertex(qy8r_sw_context *context, qy8r_sw_program *program,
                      unsigned vertex_index, qy8r_sw_vertex *vertex)
{
    qy8r_usp_cpu_inputs inputs;
    qy8r_usp_cpu_io io;
    char error[256] = { 0 };
    unsigned i, varying_float_count = 0;
    float clip[4];
    float w;

    memset(&inputs, 0, sizeof(inputs));
    memset(&io, 0, sizeof(io));
    for (i = 0; i < program->vs->bindings.uniform_count; i++) {
        unsigned index = program->vs->bindings.uniforms[i];
        inputs.symbols[index] = program->vs_uniform_values[index];
        inputs.symbol_counts[index] = program->vs_uniform_counts[index];
    }
    for (i = 0; i < program->vs->bindings.attribute_count; i++) {
        unsigned index = program->vs->bindings.attributes[i];
        qy8r_sw_attribute *attribute = &program->attributes[index];
        float values[4] = { 0, 0, 0, 1 };

        if (attribute->values && vertex_index < (unsigned)attribute->count) {
            for (unsigned c = 0; c < (unsigned)attribute->components; c++) {
                values[c] =
                    attribute->values[vertex_index *
                                          (unsigned)attribute->components +
                                      c];
            }
            inputs.symbols[index] =
                attribute->values +
                vertex_index * (unsigned)attribute->components;
            inputs.symbol_counts[index] = (uint16_t)attribute->components;
        } else {
            inputs.symbols[index] = values;
            inputs.symbol_counts[index] = 4;
        }
    }
    io.inputs = &inputs;
    if (!qy8r_usp_cpu_execute(program->vs, 0, &io, error, sizeof(error))) {
        set_error(context, error);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        memcpy(&clip[i],
               &io.state.o[program->vs->bindings.position_output_base + i],
               sizeof(float));
    }
    w = clip[3];
    if (w == 0.0f || !isfinite(w)) {
        set_error(context, "vertex shader produced invalid clip W");
        return 0;
    }
    vertex->x = context->viewport[0] +
                (clip[0] / w + 1.0f) * (float)context->viewport[2] * 0.5f;
    vertex->y = context->viewport[1] +
                (clip[1] / w + 1.0f) * (float)context->viewport[3] * 0.5f;
    for (i = 0; i < program->vs->bindings.varying_count; i++) {
        unsigned symbol = program->vs->bindings.varyings[i];
        unsigned components = program->vs->symbols[symbol].component_count;
        unsigned c;

        if (varying_float_count + components > QY8R_SW_MAX_VARYING_FLOATS) {
            set_error(context, "vertex varying count exceeds software limit");
            return 0;
        }
        for (c = 0; c < components; c++) {
            unsigned output = UINT_MAX;
            for (unsigned j = 0;
                 j < program->vs->bindings.output_component_count; j++) {
                const qy8r_usp_output_component *mapping =
                    &program->vs->bindings.output_components[j];
                if (mapping->symbol_index == symbol &&
                    mapping->component == c) {
                    output = mapping->output_register;
                    break;
                }
            }
            if (output == UINT_MAX || output >= 64) {
                set_error(context, "vertex varying output has no IR mapping");
                return 0;
            }
            memcpy(&vertex->varying[varying_float_count++], &io.state.o[output],
                   sizeof(float));
        }
    }
    if (!varying_float_count) {
        varying_float_count = 1;
        vertex->varying[0] = 0.0f;
    }
    return 1;
}

static int run_fragment(void *opaque, const float *varying_values,
                        size_t varying_count, float rgba[4])
{
    qy8r_sw_fragment_context *fragment = opaque;
    qy8r_sw_context *context = fragment->context;
    qy8r_sw_program *program = fragment->program;
    qy8r_usp_cpu_inputs inputs;
    qy8r_usp_cpu_io io;
    char error[256] = { 0 };

    memset(&inputs, 0, sizeof(inputs));
    memset(&io, 0, sizeof(io));
    for (unsigned i = 0; i < program->fs->bindings.uniform_count; i++) {
        unsigned index = program->fs->bindings.uniforms[i];
        inputs.symbols[index] = program->fs_uniform_values[index];
        inputs.symbol_counts[index] = program->fs_uniform_counts[index];
    }
    for (unsigned i = 0; i < program->fs->ps_input_count; i++) {
        unsigned coord = program->fs->ps_inputs[i].coord;
        unsigned dimension = program->fs->ps_inputs[i].coord_dim + 1;
        unsigned available = varying_count > 4 ? 4 : (unsigned)varying_count;

        if (dimension > 4) {
            dimension = 4;
        }
        if (available > dimension) {
            available = dimension;
        }
        if (coord >= 64 || available == 0) {
            set_error(context, "fragment varying mapping is invalid");
            return -1;
        }
        inputs.varying_counts[coord] = (uint8_t)available;
        memcpy(inputs.varyings[coord], varying_values,
               available * sizeof(float));
    }
    io.inputs = &inputs;
    io.sample = cpu_texture_sample;
    io.sample_opaque = fragment;
    if (!qy8r_usp_cpu_execute(program->fs, 0, &io, error, sizeof(error))) {
        set_error(context, error);
        return -1;
    }
    if (io.state.discard) {
        return 0;
    }
    for (unsigned i = 0; i < 4; i++) {
        rgba[i] = ((io.result_raw >> (8 * i)) & 0xff) / 255.0f;
    }
    return 1;
}

int qy8r_draw_arrays(void *opaque, int mode, int first, int count)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_program *program;
    qy8r_sw_vertex vertices[QY8R_SW_MAX_VERTICES];
    qy8r_sw_surface surface;
    qy8r_sw_fragment_context fragment;
    unsigned i, varying_count = 0;

    if (!context || !context->target || !context->program || first < 0 ||
        count < 1 || count > QY8R_SW_MAX_VERTICES ||
        (mode != 4 && mode != 5 && mode != 6)) {
        set_error(context, "unsupported draw mode or arguments");
        return 0;
    }
    program = context->program;
    surface.width = context->target->width;
    surface.height = context->target->height;
    surface.rgba = context->target->rgba;
    for (i = 0; i < program->vs->bindings.varying_count; i++) {
        unsigned symbol = program->vs->bindings.varyings[i];
        varying_count += program->vs->symbols[symbol].component_count;
    }
    if (varying_count > QY8R_SW_MAX_VARYING_FLOATS) {
        set_error(context, "draw varying count exceeds software limit");
        return 0;
    }
    for (i = 0; i < (unsigned)count; i++) {
        if (!run_vertex(context, program, (unsigned)(first + (int)i),
                        &vertices[i])) {
            return 0;
        }
    }
    fragment.context = context;
    fragment.program = program;
    fragment.varying_count = varying_count;
    if (mode == 4) {
        for (i = 0; i + 2 < (unsigned)count; i += 3) {
            qy8r_sw_vertex tri[3] = { vertices[i], vertices[i + 1],
                                      vertices[i + 2] };
            if (!qy8r_sw_raster_triangle(&surface, tri, varying_count,
                                         run_fragment, &fragment,
                                         &context->blend)) {
                return 0;
            }
        }
    } else if (mode == 5) {
        for (i = 0; i + 2 < (unsigned)count; i++) {
            qy8r_sw_vertex tri[3];
            if (i & 1) {
                tri[0] = vertices[i + 1];
                tri[1] = vertices[i];
            } else {
                tri[0] = vertices[i];
                tri[1] = vertices[i + 1];
            }
            tri[2] = vertices[i + 2];
            if (!qy8r_sw_raster_triangle(&surface, tri, varying_count,
                                         run_fragment, &fragment,
                                         &context->blend)) {
                return 0;
            }
        }
    } else {
        for (i = 1; i + 1 < (unsigned)count; i++) {
            qy8r_sw_vertex tri[3] = { vertices[0], vertices[i],
                                      vertices[i + 1] };
            if (!qy8r_sw_raster_triangle(&surface, tri, varying_count,
                                         run_fragment, &fragment,
                                         &context->blend)) {
                return 0;
            }
        }
    }
    return 1;
}

int qy8r_blend_state(void *opaque, int enable, int src_rgb, int dst_rgb,
                     int src_alpha, int dst_alpha, const float color[4])
{
    qy8r_sw_context *context = opaque;

    if (!context || !color) {
        return 0;
    }
    context->blend.enabled = !!enable;
    context->blend.src_rgb = src_rgb;
    context->blend.dst_rgb = dst_rgb;
    context->blend.src_alpha = src_alpha;
    context->blend.dst_alpha = dst_alpha;
    memcpy(context->blend.color, color, sizeof(context->blend.color));
    return 1;
}

int qy8r_draw_points(void *opaque, int count)
{
    qy8r_sw_context *context = opaque;
    (void)count;
    set_error(context, "point rasterization is not in the AUI backend profile");
    return 0;
}

int qy8r_draw_transform_feedback(void *opaque, int count, float *out,
                                 size_t float_count)
{
    qy8r_sw_context *context = opaque;
    (void)count;
    (void)out;
    (void)float_count;
    set_error(context, "transform feedback is unavailable in the CPU backend");
    return 0;
}

int qy8r_read_rgba8(void *opaque, void *target_ptr, unsigned char rgba[4])
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_target *target = target_ptr;

    if (!context || !target || !rgba || target->rgba32f) {
        set_error(context, "invalid RGBA8 readback");
        return 0;
    }
    memcpy(rgba, target->rgba, 4);
    return 1;
}

int qy8r_read_rgba8_rect(void *opaque, void *target_ptr, unsigned char *rgba,
                         size_t byte_count)
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_target *target = target_ptr;
    size_t required;

    if (!context || !target || !rgba || target->rgba32f) {
        set_error(context, "invalid RGBA8 rectangle readback");
        return 0;
    }
    required = (size_t)target->width * target->height * 4;
    if (byte_count < required) {
        set_error(context, "RGBA8 rectangle output is too small");
        return 0;
    }
    memcpy(rgba, target->rgba, required);
    return 1;
}

int qy8r_read_rgba32f(void *opaque, void *target_ptr, float rgba[4])
{
    qy8r_sw_context *context = opaque;
    qy8r_sw_target *target = target_ptr;

    if (!context || !target || !rgba) {
        set_error(context, "invalid RGBA32F readback");
        return 0;
    }
    set_error(context,
              "RGBA32F readback is unavailable for byte-backed targets");
    return 0;
}

int qy8r_target_copy_texture(void *opaque, void *target_ptr, unsigned key)
{
    qy8r_sw_target *target = target_ptr;
    size_t bytes;

    if (!target) {
        return 0;
    }
    bytes = (size_t)target->width * target->height * 4;
    return qy8r_texture_upload_rgba8(opaque, key, target->width, target->height,
                                     target->rgba, bytes);
}

int qy8r_bind_target_texture(void *opaque, void *target, int unit)
{
    qy8r_sw_context *context = opaque;
    unsigned key;

    if (!context || !target || unit < 0 || unit >= 16) {
        return 0;
    }
    key = 0xf0000000u | (unsigned)(uintptr_t)target;
    if (!texture_get(context, key, 1)) {
        return 0;
    }
    return qy8r_target_copy_texture(context, target, key) &&
           qy8r_texture_bind(context, key, unit);
}

const char *qy8r_last_error(void *opaque)
{
    qy8r_sw_context *context = opaque;
    return context ? context->error : "invalid software renderer context";
}

const qy8r_backend_ops qy8r_sw_backend = {
    .open = qy8r_sw_open,
    .close = qy8r_sw_close,
    .get_info = qy8r_sw_get_info,
    .get_caps = qy8r_sw_get_caps,
    .program_create = qy8r_sw_program_create,
    .program_create_glsl = NULL,
    .program_create_tf = NULL,
    .program_destroy = qy8r_sw_program_destroy,
    .target_create = qy8r_sw_target_create,
    .target_destroy = qy8r_sw_target_destroy,
    .target_bind = qy8r_sw_target_bind,
    .viewport = qy8r_sw_viewport,
    .clear = qy8r_sw_clear,
    .use_program = qy8r_sw_use_program,
    .uniform_f32 = qy8r_sw_uniform_f32,
    .uniform_i32 = qy8r_sw_uniform_i32,
    .texture_rgba32f = qy8r_sw_texture_rgba32f,
    .begin_draw = qy8r_sw_begin_draw,
    .attribute_f32 = qy8r_sw_attribute_f32,
    .draw_arrays = qy8r_sw_draw_arrays,
    .blend_state = qy8r_sw_blend_state,
    .draw_points = qy8r_sw_draw_points,
    .draw_transform_feedback = qy8r_sw_draw_transform_feedback,
    .read_rgba8 = qy8r_sw_read_rgba8,
    .read_rgba8_rect = qy8r_sw_read_rgba8_rect,
    .read_rgba32f = qy8r_sw_read_rgba32f,
    .texture_upload_rgba8 = qy8r_sw_texture_upload_rgba8,
    .texture_parameter = qy8r_sw_texture_parameter,
    .texture_bind = qy8r_sw_texture_bind,
    .target_copy_texture = qy8r_sw_target_copy_texture,
    .bind_target_texture = qy8r_sw_bind_target_texture,
    .texture_set_sampler = qy8r_sw_texture_set_sampler,
    .last_error = qy8r_sw_last_error,
};
