/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <qemu-plugin.h>
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "qemu/error-report.h"
#include "qy8r.h"
#include "hw/display/clarion_qy8_render.h"
#include "clarion_qy8_render_texture.h"
#include "clarion_qy8_render_names.h"
#include "clarion_qy8_render_catalog.h"

typedef struct {
    struct qemu_plugin_register *h[16], *ttbr;
    gboolean have[16], have_ttbr, ready;
} RegSet;
typedef struct {
    uint32_t index, size, type, normalized, stride;
    uint64_t ptr;
    gboolean enabled, valid;
} VertexAttr;
typedef struct {
    uint64_t id, lr, sp;
    uint32_t return_pc, ttbr;
    const Qy8RenderExport *fn;
    uint64_t args[4], stack[8];
} Pending;
typedef struct {
    uint32_t type;
    char *hash;
    uint8_t *blob;
    size_t blob_size;
} ShaderState;
typedef struct {
    uint32_t shaders[8];
    guint n;
    gboolean linked;
} ProgramState;
typedef struct {
    void *handle;
    const char *vs_hash;
    const char *fs_hash;
    guint index;
} HostProgram;
typedef struct {
    char *name;
    int kind, count;
    float f[16];
    int i[4];
} UniformState;
typedef struct {
    uint32_t width, height, format, stride;
    uint64_t linear;
    uint32_t handle;
    char *sha256;
    guint unread_pages;
} ImageState;
typedef struct {
    uint32_t id, image;
    int min_filter, mag_filter, wrap_s, wrap_t;
    char *sha256;
} TextureState;
typedef struct {
    uint32_t handle;
    int width, height;
    void *target;
} SurfaceState;
typedef struct {
    uint32_t c_program, c_bound_textures[16], c_active_unit;
    VertexAttr c_attrs[16];
    int c_viewport[4], c_blend_enabled, c_blend_func[4];
    float c_blend_color[4], c_clear_rgba[4];
    gboolean c_have_viewport;
} ContextState;

typedef struct {
    uint32_t ttbr;
    VertexAttr p_attrs[16];
    GHashTable *p_shader_state, *p_program_state, *p_uniform_locations;
    GHashTable *p_attrib_locations, *p_uniform_state, *p_mirror_programs;
    GHashTable *p_images, *p_textures, *p_surfaces, *p_context_states;
    void *p_mirror_lib, *p_mirror_ctx, *p_mirror_program, *p_mirror_target;
    guint p_mirror_program_index;
    uint32_t p_current_program, p_active_context, p_active_surface;
    uint32_t p_bound_textures[16], p_active_texture_unit;
    int p_viewport[4], p_blend_enabled, p_blend_func[4];
    float p_blend_color[4], p_clear_rgba[4];
    gboolean p_have_viewport, p_have_makecurrent, p_mirror_stopped;
    uint64_t p_frame_no, p_completed_swaps;
    uint64_t p_service_draw_calls, p_service_clear_calls, p_service_other_calls;
} ProcessState;

static GHashTable *processes;
static GHashTable *dynamic_exports;
static GMutex process_lock;
static _Thread_local ProcessState *current_proc;
#define attrs (current_proc->p_attrs)
#define shader_state (current_proc->p_shader_state)
#define program_state (current_proc->p_program_state)
#define uniform_locations (current_proc->p_uniform_locations)
#define attrib_locations (current_proc->p_attrib_locations)
#define uniform_state (current_proc->p_uniform_state)
#define mirror_programs (current_proc->p_mirror_programs)
#define images (current_proc->p_images)
#define textures (current_proc->p_textures)
#define surfaces (current_proc->p_surfaces)
#define context_states (current_proc->p_context_states)
#define mirror_lib (current_proc->p_mirror_lib)
#define mirror_ctx (current_proc->p_mirror_ctx)
#define mirror_program (current_proc->p_mirror_program)
#define mirror_target (current_proc->p_mirror_target)
#define mirror_program_index (current_proc->p_mirror_program_index)
#define current_program (current_proc->p_current_program)
#define active_context (current_proc->p_active_context)
#define active_surface (current_proc->p_active_surface)
#define bound_textures (current_proc->p_bound_textures)
#define active_texture_unit (current_proc->p_active_texture_unit)
#define viewport (current_proc->p_viewport)
#define blend_enabled (current_proc->p_blend_enabled)
#define blend_func (current_proc->p_blend_func)
#define blend_color (current_proc->p_blend_color)
#define clear_rgba (current_proc->p_clear_rgba)
#define have_viewport (current_proc->p_have_viewport)
#define have_makecurrent (current_proc->p_have_makecurrent)
#define mirror_stopped (current_proc->p_mirror_stopped)
#define frame_no (current_proc->p_frame_no)
#define completed_swaps (current_proc->p_completed_swaps)
#define service_draw_calls (current_proc->p_service_draw_calls)
#define service_clear_calls (current_proc->p_service_clear_calls)
#define service_other_calls (current_proc->p_service_other_calls)

static ProcessState *proc_get(uint32_t ttbr)
{
    ProcessState *state;

    g_mutex_lock(&process_lock);
    state = g_hash_table_lookup(processes, GUINT_TO_POINTER(ttbr));
    if (!state) {
        state = g_new0(ProcessState, 1);
        state->ttbr = ttbr;
        state->p_shader_state =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        state->p_program_state =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        state->p_uniform_locations =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        state->p_attrib_locations =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        state->p_uniform_state =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        state->p_mirror_programs =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        state->p_images =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        state->p_textures =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        state->p_surfaces =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        state->p_context_states =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
        state->p_blend_func[0] = 1;
        state->p_blend_func[2] = 1;
        g_hash_table_insert(processes, GUINT_TO_POINTER(ttbr), state);
    }
    g_mutex_unlock(&process_lock);
    return state;
}

static FILE *out;
static uint64_t next_seq = 1, next_call = 1, next_dynamic_call = 1;
static uint64_t fn_counts[G_N_ELEMENTS(required_names)];
static GHashTable *module_counts;
static GPtrArray *pending;
static RegSet regs[64];
static GMutex lock;
static char *out_path;
static char *dump_directory;
static char *qy8r_path;
static gboolean angle_backend;
static gboolean dump_frames;
static uint32_t wsegl_linear[3] = { 0x710000, 0x7d0000, 0x890000 };
static gboolean
    present_enabled; /* Present qy8r output in the guest WSEGL buffer. */
static gboolean service_draws;

typedef void *(*QOpen)(char *, size_t);
typedef void (*QClose)(void *);
typedef void *(*QProgramCreate)(void *, const uint8_t *, size_t,
                                const uint8_t *, size_t, char *, size_t);
typedef void *(*QTargetCreate)(void *, int, int, int);
typedef int (*QTargetBind)(void *, void *);
typedef int (*QViewport)(void *, int, int, int, int);
typedef int (*QClear)(void *, float, float, float, float);
typedef int (*QBeginDraw)(void *, void *);
typedef int (*QUniformF)(void *, void *, const char *, const float *, int);
typedef int (*QUniformI)(void *, void *, const char *, const int *, int);
typedef int (*QAttrib)(void *, void *, const char *, const float *, int, int);
typedef int (*QDraw)(void *, int, int, int);
typedef int (*QBlend)(void *, int, int, int, int, int, const float *);
typedef int (*QReadRect)(void *, void *, unsigned char *, size_t);
typedef int (*QTexUpload)(void *, unsigned int, int, int, const unsigned char *,
                          size_t);
typedef int (*QTexParam)(void *, unsigned int, int, int);
typedef int (*QTexBind)(void *, unsigned int, int);
typedef int (*QTargetCopy)(void *, void *, unsigned int);
typedef int (*QSetSampler)(void *, void *, const char *, unsigned int, int);
typedef const char *(*QLastError)(void *);
static QOpen q_open;
static QClose q_close;
static QProgramCreate q_program_create;
static QTargetCreate q_target_create;
static QTargetBind q_target_bind;
static QViewport q_viewport;
static QClear q_clear;
static QBeginDraw q_begin_draw;
static QUniformF q_uniform_f;
static QUniformI q_uniform_i;
static QAttrib q_attrib;
static QDraw q_draw;
static QBlend q_blend;
static QReadRect q_read_rect;
static QLastError q_last_error;
static QTexUpload q_tex_upload;
static QTexParam q_tex_param;
static QTexBind q_tex_bind;
static QTargetCopy q_target_copy;
static QSetSampler q_set_sampler;
static void json_quote(GString *s, const char *v);
static void emit(GString *s);
static gboolean read_mem(uint64_t addr, size_t len, GByteArray *b);
static gboolean read_u32(uint64_t addr, uint32_t *v);
static char *sha256_mem(uint64_t addr, size_t len);
static gboolean capture_draw(uint64_t call_id, uint32_t mode, uint32_t first,
                             uint32_t count);
static void dump_frame(uint32_t surface_handle);
static void present_frame(uint32_t surface_handle);

static gpointer idkey(uint32_t id)
{
    return GUINT_TO_POINTER(id);
}
static char *loc_key(uint32_t program, int loc)
{
    return g_strdup_printf("%08x:%d", program, loc);
}
static gboolean mirror_fail(const char *what)
{
    mirror_stopped = TRUE;
    const char *detail =
        mirror_ctx && q_last_error ? q_last_error(mirror_ctx) : NULL;
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s, "{\"seq\":\"%" PRIu64 "\",\"kind\":\"mirror_error\",\"what\":",
        next_seq++);
    json_quote(s, what);
    g_string_append(s, ",\"detail\":");
    if (detail) {
        json_quote(s, detail);
    } else {
        g_string_append(s, "null");
    }
    g_string_append_c(s, '}');
    emit(s);
    g_string_free(s, TRUE);
    return FALSE;
}
static gboolean load_qy8r(void)
{
    if (mirror_ctx) {
        return TRUE;
    }
    if (!angle_backend) {
        q_open = qy8r_open;
        q_close = qy8r_close;
        q_program_create = qy8r_program_create;
        q_target_create = qy8r_target_create;
        q_target_bind = qy8r_target_bind;
        q_viewport = qy8r_viewport;
        q_clear = qy8r_clear;
        q_begin_draw = qy8r_begin_draw;
        q_uniform_f = qy8r_uniform_f32;
        q_uniform_i = qy8r_uniform_i32;
        q_attrib = qy8r_attribute_f32;
        q_draw = qy8r_draw_arrays;
        q_blend = qy8r_blend_state;
        q_read_rect = qy8r_read_rgba8_rect;
        q_last_error = qy8r_last_error;
        q_tex_upload = qy8r_texture_upload_rgba8;
        q_tex_param = qy8r_texture_parameter;
        q_tex_bind = qy8r_texture_bind;
        q_target_copy = qy8r_target_copy_texture;
        q_set_sampler = qy8r_texture_set_sampler;
    } else {
        const char *library = qy8r_path;
        const char *default_library;

        if (!library) {
#ifdef G_OS_DARWIN
            default_library = "libqy8r.dylib";
#else
            default_library = "libqy8r.so";
#endif
            library = default_library;
        }
        mirror_lib = dlopen(library, RTLD_NOW | RTLD_LOCAL);
        if (!mirror_lib) {
            char *message = g_strdup_printf("cannot load render-lib '%s': %s",
                                            library, dlerror());
            gboolean result = mirror_fail(message);

            g_free(message);
            return result;
        }
#define SYM(v, n)                                                              \
    do {                                                                       \
        *(void **)(&v) = dlsym(mirror_lib, n);                                 \
        if (!v)                                                                \
            return mirror_fail("missing qy8r symbol " n);                      \
    } while (0)
        SYM(q_open, "qy8r_open");
        SYM(q_close, "qy8r_close");
        SYM(q_program_create, "qy8r_program_create");
        SYM(q_target_create, "qy8r_target_create");
        SYM(q_target_bind, "qy8r_target_bind");
        SYM(q_viewport, "qy8r_viewport");
        SYM(q_clear, "qy8r_clear");
        SYM(q_begin_draw, "qy8r_begin_draw");
        SYM(q_uniform_f, "qy8r_uniform_f32");
        SYM(q_uniform_i, "qy8r_uniform_i32");
        SYM(q_attrib, "qy8r_attribute_f32");
        SYM(q_draw, "qy8r_draw_arrays");
        SYM(q_blend, "qy8r_blend_state");
        SYM(q_read_rect, "qy8r_read_rgba8_rect");
        SYM(q_last_error, "qy8r_last_error");
        SYM(q_tex_upload, "qy8r_texture_upload_rgba8");
        SYM(q_tex_param, "qy8r_texture_parameter");
        SYM(q_tex_bind, "qy8r_texture_bind");
        SYM(q_target_copy, "qy8r_target_copy_texture");
        SYM(q_set_sampler, "qy8r_texture_set_sampler");
#undef SYM
    }
    char error[512] = { 0 };
    mirror_ctx = q_open(error, sizeof error);
    if (!mirror_ctx) {
        return mirror_fail(error);
    }
    return TRUE;
}

int clarion_qy8_render_configure(const char *render, const char *render_lib,
                                 const char *render_log,
                                 const char *render_dump_dir)
{
    angle_backend = !strcmp(render, "angle");
    g_free(qy8r_path);
    qy8r_path = render_lib && *render_lib ? g_strdup(render_lib) : NULL;
    service_draws = TRUE;
    present_enabled = TRUE;
    g_free(out_path);
    out_path = render_log && *render_log ? g_strdup(render_log) : NULL;
    g_free(dump_directory);
    dump_directory =
        render_dump_dir && *render_dump_dir ? g_strdup(render_dump_dir) : NULL;
    dump_frames = dump_directory != NULL;
    return 0;
}

static char *sha256_data(const unsigned char *data, size_t n)
{
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, n);
}

static gboolean
get_program_shader_hashes(uint32_t guest_program, const char **vs_hash,
                          const char **fs_hash, const uint8_t **vs_blob,
                          size_t *vs_size, const uint8_t **fs_blob,
                          size_t *fs_size)
{
    ProgramState *ps = g_hash_table_lookup(program_state, idkey(guest_program));
    if (!ps || !ps->linked || ps->n != 2) {
        return mirror_fail("live program missing linked stock shader pair");
    }

    *vs_hash = NULL;
    *fs_hash = NULL;
    for (guint i = 0; i < ps->n; i++) {
        ShaderState *sh =
            g_hash_table_lookup(shader_state, idkey(ps->shaders[i]));
        if (!sh || !sh->hash || !sh->blob || !sh->blob_size) {
            return mirror_fail("draw shader lacks accepted blob hash");
        }
        if (sh->type == 0x8b31) {
            *vs_hash = sh->hash;
            *vs_blob = sh->blob;
            *vs_size = sh->blob_size;
        } else if (sh->type == 0x8b30) {
            *fs_hash = sh->hash;
            *fs_blob = sh->blob;
            *fs_size = sh->blob_size;
        }
    }
    if (!*vs_hash || !*fs_hash) {
        return mirror_fail("unrecognized stock shader stages");
    }
    return TRUE;
}

static gboolean select_host_program(uint32_t guest_program,
                                    const char **vs_hash, const char **fs_hash)
{
    char *key;
    HostProgram *host;
    const uint8_t *vs_blob = NULL, *fs_blob = NULL;
    size_t vs_size = 0, fs_size = 0;

    if (!get_program_shader_hashes(guest_program, vs_hash, fs_hash, &vs_blob,
                                   &vs_size, &fs_blob, &fs_size)) {
        return FALSE;
    }
    key = g_strdup_printf("%s|%s", *vs_hash, *fs_hash);
    host = g_hash_table_lookup(mirror_programs, key);
    if (!host) {
        char log[8192] = { 0 };

        if (!load_qy8r()) {
            g_free(key);
            return FALSE;
        }
        host = g_new0(HostProgram, 1);
        host->handle = q_program_create(mirror_ctx, vs_blob, vs_size, fs_blob,
                                        fs_size, log, sizeof(log));
        if (!host->handle) {
            g_free(host);
            g_free(key);
            return mirror_fail(log[0] ? log : "qy8r compile/link failed");
        }
        host->vs_hash = *vs_hash;
        host->fs_hash = *fs_hash;
        host->index = g_hash_table_size(mirror_programs) + 1;
        g_hash_table_insert(mirror_programs, key, host);

        GString *s = g_string_new(NULL);
        g_string_append_printf(
            s,
            "{\"kind\":\"host_program_create\",\"guest_program\":\"0x%08x\","
            "\"host_program_index\":%u,\"cache_size\":%u,\"vs_blob_sha256\":",
            guest_program, host->index, g_hash_table_size(mirror_programs));
        json_quote(s, *vs_hash);
        g_string_append(s, ",\"fs_blob_sha256\":");
        json_quote(s, *fs_hash);
        g_string_append_c(s, '}');
        emit(s);
        g_string_free(s, TRUE);
    } else {
        g_free(key);
    }

    mirror_program = host->handle;
    mirror_program_index = host->index;
    return TRUE;
}

static gboolean set_uniforms(void)
{
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, uniform_state);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        UniformState *u = v;
        uint32_t owner = (uint32_t)g_ascii_strtoull(
            (char *)k, NULL, 16); /* keys begin with program id */
        if (owner != current_program) {
            continue;
        }
        int status = u->kind == 2 ? q_uniform_i(mirror_ctx, mirror_program,
                                                u->name, u->i, u->count)
                                  : q_uniform_f(mirror_ctx, mirror_program,
                                                u->name, u->f, u->count);
        if (status == 2) {
            GString *s = g_string_new(NULL);
            g_string_append_printf(s,
                                   "{\"kind\":\"uniform_inactive\",\"program\":"
                                   "\"0x%08x\",\"name\":",
                                   owner);
            json_quote(s, u->name);
            g_string_append_c(s, '}');
            emit(s);
            g_string_free(s, TRUE);
            continue;
        }
        if (!status) {
            return mirror_fail("apply captured uniform");
        }
    }
    return TRUE;
}

static SurfaceState *surface_get(uint32_t handle)
{
    return g_hash_table_lookup(surfaces, idkey(handle));
}
static void surface_add(uint32_t handle, int width, int height)
{
    if (!handle) {
        return;
    }
    SurfaceState *s = g_new0(SurfaceState, 1);
    s->handle = handle;
    s->width = width;
    s->height = height;
    g_hash_table_replace(surfaces, idkey(handle), s);
}
static void surface_add_from_create(const Qy8RenderExport *fn, Pending *p,
                                    uint32_t handle)
{
    int width = 0, height = 0;
    uint64_t attributes_ptr =
        !strcmp(fn->name, "eglCreatePbufferSurface") ? p->args[2] : p->args[3];
    for (int i = 0; i < 64; i++) {
        uint32_t k = 0, v = 0;
        if (!read_u32(attributes_ptr + i * 8, &k) || k == 0x3038) {
            break;
        }
        if (!read_u32(attributes_ptr + i * 8 + 4, &v)) {
            break;
        }
        if (k == 0x3057) {
            width = (int)v;
        } else if (k == 0x3056) {
            height = (int)v;
        }
    }
    if (!strcmp(fn->name, "eglCreateWindowSurface")) {
        width = 800;
        height = 480;
    }
    if (width < 1 || height < 1) {
        mirror_fail(
            "surface dimensions unavailable from captured create attributes");
        return;
    }
    surface_add(handle, width, height);
}
static void save_context_state(uint32_t id)
{
    if (!id) {
        return;
    }
    ContextState *s = g_new0(ContextState, 1);
    s->c_program = current_program;
    memcpy(s->c_bound_textures, bound_textures, sizeof(bound_textures));
    s->c_active_unit = active_texture_unit;
    memcpy(s->c_attrs, attrs, sizeof(attrs));
    memcpy(s->c_viewport, viewport, sizeof(viewport));
    s->c_have_viewport = have_viewport;
    s->c_blend_enabled = blend_enabled;
    memcpy(s->c_blend_func, blend_func, sizeof(blend_func));
    memcpy(s->c_blend_color, blend_color, sizeof(blend_color));
    memcpy(s->c_clear_rgba, clear_rgba, sizeof(clear_rgba));
    g_hash_table_replace(context_states, idkey(id), s);
}
static void load_context_state(uint32_t id)
{
    ContextState *s = g_hash_table_lookup(context_states, idkey(id));
    if (s) {
        current_program = s->c_program;
        memcpy(bound_textures, s->c_bound_textures, sizeof(bound_textures));
        active_texture_unit = s->c_active_unit;
        memcpy(attrs, s->c_attrs, sizeof(attrs));
        memcpy(viewport, s->c_viewport, sizeof(viewport));
        have_viewport = s->c_have_viewport;
        blend_enabled = s->c_blend_enabled;
        memcpy(blend_func, s->c_blend_func, sizeof(blend_func));
        memcpy(blend_color, s->c_blend_color, sizeof(blend_color));
        memcpy(clear_rgba, s->c_clear_rgba, sizeof(clear_rgba));
        return;
    }
    memset(attrs, 0, sizeof(attrs));
    memset(bound_textures, 0, sizeof(bound_textures));
    memset(viewport, 0, sizeof(viewport));
    memset(blend_color, 0, sizeof(blend_color));
    memset(clear_rgba, 0, sizeof(clear_rgba));
    current_program = 0;
    active_texture_unit = 0;
    have_viewport = FALSE;
    blend_enabled = FALSE;
    blend_func[0] = 1;
    blend_func[1] = 0;
    blend_func[2] = 1;
    blend_func[3] = 0;
}
static SurfaceState *surface_ensure_target(uint32_t handle)
{
    SurfaceState *s = surface_get(handle);
    if (!s) {
        return NULL;
    }
    if (!s->target) {
        s->target = q_target_create(mirror_ctx, s->width, s->height, 0);
        if (!s->target) {
            return NULL;
        }
        if (!q_target_bind(mirror_ctx, s->target) ||
            !q_clear(mirror_ctx, 0, 0, 0, 0)) {
            return NULL;
        }
    }
    return s;
}
static gboolean guest_read_pagewise(uint64_t address, size_t length,
                                    GByteArray *buffer, guint *unread)
{
    if (!address || !length || length > 64u * 1024u * 1024u) {
        return FALSE;
    }
    guint missing = 0;
    size_t done = 0;
    while (done < length) {
        uint64_t va = address + done;
        size_t chunk = 4096 - (size_t)(va & 4095);
        if (chunk > length - done) {
            chunk = length - done;
        }
        GByteArray *page = g_byte_array_new();
        if (qemu_plugin_read_memory_vaddr(va, page, chunk) &&
            page->len == chunk) {
            g_byte_array_append(buffer, page->data, page->len);
        } else {
            guint8 *zero = g_malloc0(chunk);
            g_byte_array_append(buffer, zero, chunk);
            g_free(zero);
            missing++;
        }
        g_byte_array_unref(page);
        done += chunk;
    }
    if (unread) {
        *unread = missing;
    }
    return TRUE;
}
static gboolean image_rgba(ImageState *im, GByteArray *rgba)
{
    if (!im || !im->width || !im->height || im->width > 8192 ||
        im->height > 8192 ||
        im->stride < (im->format == 0x14 ? im->width * 4 : im->width * 2)) {
        return FALSE;
    }
    size_t source_len = (size_t)im->stride * im->height;
    if (source_len > 64u * 1024u * 1024u) {
        return FALSE;
    }
    GByteArray *src = g_byte_array_new();
    if (!guest_read_pagewise(im->linear, source_len, src, &im->unread_pages)) {
        g_byte_array_unref(src);
        return FALSE;
    }
    g_byte_array_set_size(rgba, (guint)((size_t)im->width * im->height * 4));
    for (uint32_t y = 0; y < im->height; y++) {
        for (uint32_t x = 0; x < im->width; x++) {
            guint8 *r = &rgba->data[((size_t)y * im->width + x) * 4];
            const guint8 *pixel = src->data + (size_t)y * im->stride +
                                  x * (im->format == 0x14 ? 4 : 2);
            if (!qy8_render_texture_pixel_rgba(im->format, pixel, r)) {
                g_byte_array_unref(src);
                return FALSE;
            }
        }
    }
    char *hash = sha256_data(rgba->data, rgba->len);
    g_free(im->sha256);
    im->sha256 = hash;
    g_byte_array_unref(src);
    return TRUE;
}
static gboolean sync_guest_texture(TextureState *t, uint32_t *render_key)
{
    if (!t || !t->id) {
        return FALSE;
    }
    uint32_t key = t->id;
    *render_key = key;
    if (t->image) {
        ImageState *im = g_hash_table_lookup(images, idkey(t->image));
        if (!im) {
            return mirror_fail(
                "draw samples EGLImage with no captured create-image state");
        }
        GByteArray *rgba = g_byte_array_new();
        if (!image_rgba(im, rgba)) {
            g_byte_array_unref(rgba);
            return mirror_fail("unsupported EGLImage dimensions/stride/format");
        }
        if (g_strcmp0(im->sha256, t->sha256)) {
            if (!q_tex_upload(mirror_ctx, key, (int)im->width, (int)im->height,
                              rgba->data, rgba->len)) {
                g_byte_array_unref(rgba);
                return mirror_fail("upload converted EGLImage pixels");
            }
            g_free(t->sha256);
            t->sha256 = g_strdup(im->sha256);
            q_tex_param(mirror_ctx, key, 0x2801, t->min_filter);
            q_tex_param(mirror_ctx, key, 0x2800, t->mag_filter);
            q_tex_param(mirror_ctx, key, 0x2802, t->wrap_s);
            q_tex_param(mirror_ctx, key, 0x2803, t->wrap_t);
            GString *s = g_string_new(NULL);
            g_string_append_printf(
                s,
                "{\"kind\":\"texture_upload\",\"texture\":\"0x%08x\","
                "\"format\":\"0x%08x\",\"width\":%u,\"height\":%u,\"sha256\":",
                key, im->format, im->width, im->height);
            json_quote(s, im->sha256);
            g_string_append_printf(s, ",\"unread_pages\":%u}",
                                   im->unread_pages);
            emit(s);
            g_string_free(s, TRUE);
        }
        g_byte_array_unref(rgba);
    }
    return TRUE;
}
static gboolean capture_draw(uint64_t call_id, uint32_t mode, uint32_t first,
                             uint32_t count)
{
    gint64 draw_started_us = g_get_monotonic_time();
    if (mirror_stopped) {
        return TRUE;
    }
    if (!have_makecurrent || !active_surface || !have_viewport) {
        return mirror_fail("draw without live EGL/viewport state");
    }
    const char *vh = NULL, *fh = NULL;
    if (!select_host_program(current_program, &vh, &fh)) {
        return FALSE;
    }
    SurfaceState *surface = surface_ensure_target(active_surface);
    if (!surface) {
        return mirror_fail("current EGL surface has no host render target");
    }
    mirror_target = surface->target;
    if (!q_target_bind(mirror_ctx, mirror_target)) {
        return mirror_fail("bind current surface target");
    }
    size_t bytes = (size_t)surface->width * surface->height * 4;
    unsigned char *before = g_malloc0(bytes), *after = g_malloc0(bytes);
    if (!q_read_rect(mirror_ctx, mirror_target, before, bytes)) {
        return mirror_fail("read target before draw");
    }
    if (!q_begin_draw(mirror_ctx, mirror_program) ||
        !q_viewport(mirror_ctx, viewport[0], viewport[1], viewport[2],
                    viewport[3]) ||
        !set_uniforms()) {
        return mirror_fail("apply guest draw state");
    }
    GHashTableIter ai;
    gpointer ak, av;
    g_hash_table_iter_init(&ai, attrib_locations);
    while (g_hash_table_iter_next(&ai, &ak, &av)) {
        uint32_t owner = (uint32_t)g_ascii_strtoull((char *)ak, NULL, 16);
        if (owner != current_program) {
            continue;
        }
        int loc = atoi(strchr((char *)ak, ':') + 1);
        VertexAttr *a =
            (loc >= 0 && loc < (int)G_N_ELEMENTS(attrs)) ? &attrs[loc] : NULL;
        if (!a || !a->enabled || a->type != 0x1406 || !a->size || a->size > 4 ||
            count > 4) {
            return mirror_fail("unsupported active vertex attribute");
        }
        int stride = a->stride ? a->stride : a->size * 4;
        float values[16] = { 0 };
        for (uint32_t j = 0; j < count; j++) {
            for (uint32_t c = 0; c < a->size; c++) {
                uint32_t raw = 0;
                if (!read_u32(a->ptr + (uint64_t)(first + j) * stride + c * 4,
                              &raw)) {
                    return mirror_fail("guest vertex memory unreadable");
                }
                memcpy(&values[j * a->size + c], &raw, 4);
            }
        }
        if (!q_attrib(mirror_ctx, mirror_program, av, values, (int)a->size,
                      (int)count)) {
            return mirror_fail("apply guest vertex attribute");
        }
    }
    char *sampler_key = g_strdup_printf("%08x:u_Sampler", current_program);
    UniformState *sampler = g_hash_table_lookup(uniform_state, sampler_key);
    g_free(sampler_key);
    int unit = sampler && sampler->kind == 2 ? sampler->i[0] : 0;
    if (unit < 0 || unit >= 16) {
        return mirror_fail("sampler unit outside supported range");
    }
    TextureState *t =
        g_hash_table_lookup(textures, idkey(bound_textures[unit]));
    uint32_t key = 0;
    if (t) {
        if (!sync_guest_texture(t, &key)) {
            return FALSE;
        }
    } else {
        key = 0xffffffffu;
        static const unsigned char black[4] = { 0, 0, 0, 255 };
        if (!q_tex_upload(mirror_ctx, key, 1, 1, black, 4)) {
            return mirror_fail("create default black texture");
        }
    }
    if (!q_tex_bind(mirror_ctx, key, unit) ||
        !q_set_sampler(mirror_ctx, mirror_program, "u_Sampler", key, unit)) {
        return mirror_fail("bind captured texture/sampler");
    }
    if (!q_blend(mirror_ctx, blend_enabled, blend_func[0], blend_func[1],
                 blend_func[2], blend_func[3], blend_color) ||
        !q_draw(mirror_ctx, (int)mode, (int)first, (int)count) ||
        !q_read_rect(mirror_ctx, mirror_target, after, bytes)) {
        return mirror_fail("draw/read target");
    }
    size_t n_pixels = (size_t)surface->width * surface->height,
           mask_size = (n_pixels + 7) / 8;
    guint8 *mask = g_malloc0(mask_size);
    uint64_t changed = 0;
    int minx = surface->width, miny = surface->height, maxx = -1, maxy = -1;
    for (int y = 0; y < surface->height; y++) {
        for (int x = 0; x < surface->width; x++) {
            size_t pixel = (size_t)y * surface->width + x, off = pixel * 4;
            if (memcmp(before + off, after + off, 4)) {
                mask[pixel >> 3] |= (guint8)(1u << (pixel & 7));
                changed++;
                if (x < minx) {
                    minx = x;
                }
                if (x > maxx) {
                    maxx = x;
                }
                if (y < miny) {
                    miny = y;
                }
                if (y > maxy) {
                    maxy = y;
                }
            }
        }
    }
    char *mask_path = NULL;
    if (dump_directory) {
        mask_path = g_strdup_printf("%s/draw-%" PRIu64 ".changed.mask",
                                    dump_directory, call_id);
        GError *write_error = NULL;

        if (!g_file_set_contents(mask_path, (const char *)mask,
                                 (gssize)mask_size, &write_error)) {
            g_free(mask);
            g_free(mask_path);
            return mirror_fail(write_error ? write_error->message
                                           : "write changed-pixel mask");
        }
    }
    g_free(mask);
    ImageState *im =
        t && t->image ? g_hash_table_lookup(images, idkey(t->image)) : NULL;
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s,
        "{\"kind\":\"mirror_draw\",\"call_id\":\"%" PRIu64
        "\",\"context\":\"0x%08x\",\"surface\":\"0x%08x\",\"program\":\"0x%"
        "08x\",\"host_program_index\":%u,\"vs_blob_sha256\":",
        call_id, active_context, active_surface, current_program,
        mirror_program_index);
    json_quote(s, vh);
    g_string_append(s, ",\"fs_blob_sha256\":");
    json_quote(s, fh);
    g_string_append_printf(
        s,
        ",\"texture\":\"0x%08x\",\"texture_unread_pages\":%u,\"viewport\":[%d,%"
        "d,%d,%d],\"target_size\":[%d,%d],\"mode\":%u,\"first\":%u,\"count\":%"
        "u,\"blend\":%s,\"changed_pixels\":%" PRIu64 ",\"mirror_us\":%" PRIi64
        ",\"changed_mask_path\":",
        key, im ? im->unread_pages : 0, viewport[0], viewport[1], viewport[2],
        viewport[3], surface->width, surface->height, mode, first, count,
        blend_enabled ? "true" : "false", changed,
        (gint64)g_get_monotonic_time() - draw_started_us);
    if (mask_path) {
        json_quote(s, mask_path);
    } else {
        g_string_append(s, "null");
    }
    g_free(mask_path);
    g_string_append(s, ",\"bbox\":");
    if (maxx < 0) {
        g_string_append(s, "null");
    } else {
        g_string_append_printf(s, "[%d,%d,%d,%d]", minx, miny, maxx, maxy);
    }
    g_string_append_c(s, '}');
    emit(s);
    g_string_free(s, TRUE);
    g_free(before);
    g_free(after);
    return TRUE;
}
static void dump_frame(uint32_t handle)
{
    if (!dump_frames || mirror_stopped || !dump_directory) {
        return;
    }
    if (!mirror_ctx && !load_qy8r()) {
        return;
    }
    SurfaceState *surface = surface_get(handle);
    if (!surface || !surface->target) {
        mirror_fail("swap target is not a captured rendered surface");
        return;
    }
    size_t bytes = (size_t)surface->width * surface->height * 4;
    unsigned char *rgba = g_malloc(bytes);
    if (!q_target_bind(mirror_ctx, surface->target) ||
        !q_read_rect(mirror_ctx, surface->target, rgba, bytes)) {
        g_free(rgba);
        mirror_fail("swap target readback failed");
        return;
    }
    char *filename = g_strdup_printf("frame-%" PRIu64 ".rgba", frame_no);
    char *path = g_build_filename(dump_directory, filename, NULL);
    g_free(filename);
    GError *error = NULL;
    if (!g_file_set_contents(path, (const char *)rgba, (gssize)bytes, &error)) {
        mirror_fail(error ? error->message : "write frame RGBA failed");
        g_clear_error(&error);
        g_free(path);
        g_free(rgba);
        return;
    }
    char *hash = sha256_data(rgba, bytes);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
                           "{\"kind\":\"frame_dump\",\"swap\":%" PRIu64
                           ",\"width\":%d,\"height\":%d,\"sha256\":",
                           frame_no, surface->width, surface->height);
    json_quote(s, hash);
    g_string_append(s, ",\"path\":");
    json_quote(s, path);
    g_string_append_c(s, '}');
    emit(s);
    g_string_free(s, TRUE);
    g_free(hash);
    g_free(path);
    g_free(rgba);
    GString *b = g_string_new(NULL);
    g_string_append_printf(
        b, "{\"kind\":\"wsegl_buffers_pa\",\"swap\":%" PRIu64 ",\"buffers\":[",
        frame_no);
    for (int i = 0; i < 3; i++) {
        uint64_t pa = 0;
        if (i) {
            g_string_append_c(b, ',');
        }
        g_string_append_printf(
            b,
            "{\"linear\":\"0x%08x\",\"translated\":%s,\"pa\":", wsegl_linear[i],
            qemu_plugin_translate_vaddr(wsegl_linear[i], &pa) ? "true"
                                                              : "false");
        if (pa) {
            g_string_append_printf(b, "\"0x%08" PRIx64 "\"}", pa);
        } else {
            g_string_append(b, "null}");
        }
    }
    g_string_append(b, "]}");
    emit(b);
    g_string_free(b, TRUE);
}

/*
 * Present the qy8r frame in the guest WSEGL buffer on eglSwapBuffers.
 * WSEGL provides the drawable and buffer addresses:
 * drawable = [surface+0x1C], index = [drawable+0x10],
 * buffer = [drawable+0x14+4*index], linear address = [buffer+0].
 *
 * The plugin does not read DU registers. A mismatch skips presentation and
 * is recorded in the log.
 */
static void present_skip(const char *reason, uint32_t a, uint32_t b)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s, "{\"kind\":\"present_skip\",\"swap\":%" PRIu64 ",\"reason\":",
        frame_no);
    json_quote(s, reason);
    g_string_append_printf(s, ",\"a\":\"0x%08x\",\"b\":\"0x%08x\"}", a, b);
    emit(s);
    g_string_free(s, TRUE);
}
static void present_frame(uint32_t handle)
{
    uint32_t drawable = 0, tag = 0, idx = 0, w = 0, h = 0, stride = 0, buf = 0,
             linear = 0, hw = 0;
    if (mirror_stopped) {
        present_skip("mirror_stopped", handle, 0);
        return;
    }
    if (!read_u32((uint64_t)handle + 0x1c, &drawable) || !drawable ||
        !read_u32(drawable, &tag) ||
        !read_u32((uint64_t)drawable + 0x10, &idx) ||
        !read_u32((uint64_t)drawable + 0x7c, &w) ||
        !read_u32((uint64_t)drawable + 0x80, &h) ||
        !read_u32((uint64_t)drawable + 8, &stride)) {
        present_skip("drawable_unreadable", handle, drawable);
        return;
    }
    if (tag != 1) {
        present_skip("drawable_tag_not_window", drawable, tag);
        return;
    }
    if (idx >= 8 || !read_u32((uint64_t)drawable + 0x14 + 4 * idx, &buf) ||
        !buf || !read_u32(buf, &linear) || !read_u32((uint64_t)buf + 8, &hw)) {
        present_skip("buffer_unreadable", drawable, idx);
        return;
    }
    gboolean known = FALSE;
    for (int i = 0; i < 3; i++) {
        if (wsegl_linear[i] == linear) {
            known = TRUE;
        }
    }
    if (!known) {
        present_skip("linear_not_in_ring", linear, idx);
        return;
    }
    SurfaceState *surface = surface_get(handle);
    if (!surface || !surface->target) {
        present_skip("no_host_target", handle, linear);
        return;
    }
    if ((int)w != surface->width || (int)h != surface->height || stride != w) {
        present_skip("geometry_mismatch", w << 16 | h, stride);
        return;
    }
    size_t bytes = (size_t)w * h * 4, out_bytes = (size_t)w * h * 2;
    unsigned char *rgba = g_malloc(bytes);
    if (!q_target_bind(mirror_ctx, surface->target) ||
        !q_read_rect(mirror_ctx, surface->target, rgba, bytes)) {
        g_free(rgba);
        present_skip("readback_failed", handle, linear);
        return;
    }
    /* Read back bottom-up and reverse the rows. ARGB1555 bit 15 is alpha. */
    unsigned char *px = g_malloc(out_bytes);
    size_t d = 0;
    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *row = rgba + (size_t)(h - 1 - y) * w * 4;
        for (uint32_t x = 0; x < w; x++) {
            const unsigned char *p = row + x * 4;
            uint16_t word =
                (uint16_t)((p[3] >= 128 ? 0x8000 : 0) | ((p[0] >> 3) << 10) |
                           ((p[1] >> 3) << 5) | (p[2] >> 3));
            px[d++] = (unsigned char)(word & 0xff);
            px[d++] = (unsigned char)(word >> 8);
        }
    }
    uint64_t pa = 0;
    gboolean translated = qemu_plugin_translate_vaddr(linear, &pa);
    unsigned failed = 0, pages = 0;
    for (size_t off = 0; off < out_bytes; off += 4096) {
        size_t n = MIN((size_t)4096, out_bytes - off);
        GByteArray *page = g_byte_array_sized_new(n);
        g_byte_array_append(page, px + off, n);
        if (!qemu_plugin_write_memory_vaddr((uint64_t)linear + off, page)) {
            failed++;
        }
        pages++;
        g_byte_array_unref(page);
    }
    char *rh = sha256_data(rgba, bytes), *ph = sha256_data(px, out_bytes);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s,
                           "{\"kind\":\"present_write\",\"swap\":%" PRIu64
                           ",\"surface\":\"0x%08x\",\"drawable\":\"0x%08x\","
                           "\"ring_index\":%u,\"buffer\":\"0x%08x\",\"linear\":"
                           "\"0x%08x\",\"translated\":%s,\"pa\":\"0x%08" PRIx64
                           "\",\"buffer_hw_word\":\"0x%08x\",\"width\":%u,"
                           "\"height\":%u,\"stride\":%u,\"bytes\":%zu,"
                           "\"pages\":%u,\"failed_pages\":%u,\"rgba_sha256\":",
                           frame_no, handle, drawable, idx, buf, linear,
                           translated ? "true" : "false", pa, hw, w, h, stride,
                           out_bytes, pages, failed);
    json_quote(s, rh);
    g_string_append(s, ",\"argb1555_sha256\":");
    json_quote(s, ph);
    g_string_append_c(s, '}');
    emit(s);
    g_string_free(s, TRUE);
    g_free(rh);
    g_free(ph);
    g_free(px);
    g_free(rgba);
}

static UniformState *uniform_get_or_add(uint32_t program, const char *name)
{
    char *key = g_strdup_printf("%08x:%s", program, name);
    UniformState *u = g_hash_table_lookup(uniform_state, key);
    if (!u) {
        u = g_new0(UniformState, 1);
        u->name = g_strdup(name);
        g_hash_table_insert(uniform_state, key, u);
    } else {
        g_free(key);
    }
    return u;
}

static void update_live_state(const Qy8RenderExport *fn, Pending *p)
{
    uint64_t *a = p->args, *st = p->stack;
    if (!strcmp(fn->name, "glShaderBinary")) {
        uint32_t n = MIN((uint32_t)a[0], 8u), len = (uint32_t)st[0];
        GByteArray *ids = g_byte_array_new(), *blob = g_byte_array_new();
        if (n && read_mem(a[1], n * 4, ids) && read_mem(a[3], len, blob)) {
            char *hash = sha256_mem(a[3], len);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t id = 0;
                memcpy(&id, ids->data + i * 4, 4);
                ShaderState *sh = g_hash_table_lookup(shader_state, idkey(id));
                if (sh) {
                    g_free(sh->hash);
                    g_free(sh->blob);
                    sh->hash = g_strdup(hash);
                    sh->blob = g_memdup2(blob->data, blob->len);
                    sh->blob_size = blob->len;
                }
            }
            g_free(hash);
        }
        g_byte_array_unref(blob);
        g_byte_array_unref(ids);
    } else if (!strcmp(fn->name, "glAttachShader")) {
        ProgramState *pr =
            g_hash_table_lookup(program_state, idkey((uint32_t)a[0]));
        if (pr && pr->n < G_N_ELEMENTS(pr->shaders)) {
            pr->shaders[pr->n++] = (uint32_t)a[1];
        }
    } else if (!strcmp(fn->name, "glLinkProgram")) {
        ProgramState *pr =
            g_hash_table_lookup(program_state, idkey((uint32_t)a[0]));
        if (pr) {
            pr->linked = FALSE;
        }
    } else if (!strcmp(fn->name, "glUseProgram")) {
        current_program = (uint32_t)a[0];
    } else if (!strcmp(fn->name, "glGetUniformLocation") ||
               !strcmp(fn->name, "glGetAttribLocation")) {
        /* Return values are associated with the pending call in return_cb. */
    } else if (!strcmp(fn->name, "glUniformMatrix4fv")) {
        char *k = loc_key(current_program, (int32_t)a[0]);
        const char *name = g_hash_table_lookup(uniform_locations, k);
        if (name) {
            UniformState *u = uniform_get_or_add(current_program, name);
            u->kind = 1;
            u->count = 16;
            for (int i = 0; i < 16; i++) {
                uint32_t raw = 0;
                if (read_u32(a[3] + i * 4, &raw)) {
                    memcpy(&u->f[i], &raw, 4);
                }
            }
        }
        g_free(k);
    } else if (!strcmp(fn->name, "glUniform1f") ||
               !strcmp(fn->name, "glUniform1i") ||
               !strcmp(fn->name, "glUniform4f")) {
        char *k = loc_key(current_program, (int32_t)a[0]);
        const char *name = g_hash_table_lookup(uniform_locations, k);
        if (name) {
            UniformState *u = uniform_get_or_add(current_program, name);
            if (!strcmp(fn->name, "glUniform1i")) {
                u->kind = 2;
                u->count = 1;
                u->i[0] = (int32_t)a[1];
            } else {
                u->kind = 1;
                u->count = !strcmp(fn->name, "glUniform4f") ? 4 : 1;
                for (int i = 0; i < u->count; i++) {
                    uint32_t raw = i < 3 ? (uint32_t)a[i + 1] : (uint32_t)st[0];
                    memcpy(&u->f[i], &raw, 4);
                }
            }
        }
        g_free(k);
    } else if (!strcmp(fn->name, "glEnableVertexAttribArray") ||
               !strcmp(fn->name, "glDisableVertexAttribArray")) {
        uint32_t ix = (uint32_t)a[0];
        if (ix < G_N_ELEMENTS(attrs)) {
            attrs[ix].enabled = !strcmp(fn->name, "glEnableVertexAttribArray");
        }
    } else if (!strcmp(fn->name, "glViewport")) {
        for (int i = 0; i < 4; i++) {
            viewport[i] = (int32_t)a[i];
        }
        have_viewport = TRUE;
    } else if (!strcmp(fn->name, "glEnable")) {
        if (a[0] == 0x0be2) {
            blend_enabled = 1;
        } else {
            mirror_fail("unsupported enabled GL capability");
        }
    } else if (!strcmp(fn->name, "glDisable")) {
        if (a[0] != 0x0be2 && a[0] != 0x0bd0 && a[0] != 0x0b71 &&
            a[0] != 0x0b90) {
            mirror_fail("unsupported disabled GL capability");
        }
    } else if (!strcmp(fn->name, "glBlendFuncSeparate")) {
        for (int i = 0; i < 4; i++) {
            blend_func[i] = (int32_t)a[i];
        }
    } else if (!strcmp(fn->name, "glBlendColor")) {
        for (int i = 0; i < 4; i++) {
            uint32_t raw = (uint32_t)a[i];
            memcpy(&blend_color[i], &raw, 4);
        }
    } else if (!strcmp(fn->name, "glClearColor")) {
        for (int i = 0; i < 4; i++) {
            uint32_t raw = (uint32_t)a[i];
            memcpy(&clear_rgba[i], &raw, 4);
        }
    } else if (!strcmp(fn->name, "glClear")) {
        uint32_t mask = (uint32_t)a[0];
        if (mask & ~0x4100u) {
            mirror_fail("unsupported glClear bit");
            return;
        }
        if (!mirror_ctx && !load_qy8r()) {
            return;
        }
        SurfaceState *surface = surface_ensure_target(active_surface);
        if (!surface || !q_target_bind(mirror_ctx, surface->target) ||
            !q_clear(mirror_ctx, clear_rgba[0], clear_rgba[1], clear_rgba[2],
                     clear_rgba[3])) {
            mirror_fail("apply guest clear");
        }
    } else if (!strcmp(fn->name, "eglBindTexImage")) {
        if (!mirror_ctx && !load_qy8r()) {
            return;
        }
        SurfaceState *surface = surface_get((uint32_t)a[1]);
        uint32_t unit = active_texture_unit < G_N_ELEMENTS(bound_textures)
                            ? active_texture_unit
                            : 0,
                 id = bound_textures[unit];
        if (!surface || !surface->target || !id) {
            mirror_fail("eglBindTexImage lacks known pbuffer target/texture");
            return;
        }
        if (!q_target_copy(mirror_ctx, surface->target, id)) {
            mirror_fail("copy pbuffer target into guest texture");
        } else {
            GString *s = g_string_new(NULL);
            g_string_append_printf(
                s,
                "{\"kind\":\"pbuffer_texture_copy\",\"call_id\":\"%" PRIu64
                "\",\"surface\":\"0x%08x\",\"texture\":\"0x%08x\",\"unit\":%u,"
                "\"size\":[%d,%d],\"status\":\"copied\"}",
                p->id, (uint32_t)a[1], id, unit, surface->width,
                surface->height);
            emit(s);
            g_string_free(s, TRUE);
        }
    } else if (!strcmp(fn->name, "glActiveTexture")) {
        uint32_t unit = (uint32_t)a[0];
        if (unit < 0x84c0 || unit >= 0x84d0) {
            mirror_fail("unsupported active texture unit");
            return;
        }
        active_texture_unit = unit - 0x84c0;
    } else if (!strcmp(fn->name, "glBindTexture")) {
        if ((uint32_t)a[0] != 0x0de1 ||
            active_texture_unit >= G_N_ELEMENTS(bound_textures)) {
            mirror_fail("unsupported texture binding target/unit");
            return;
        }
        bound_textures[active_texture_unit] = (uint32_t)a[1];
    } else if (!strcmp(fn->name, "glTexParameteri")) {
        uint32_t target = (uint32_t)a[0], pname = (uint32_t)a[1],
                 value = (uint32_t)a[2],
                 id = active_texture_unit < G_N_ELEMENTS(bound_textures)
                          ? bound_textures[active_texture_unit]
                          : 0;
        if (target != 0x0de1 || !id) {
            mirror_fail(
                "texture parameter has unsupported target or no bound texture");
            return;
        }
        if ((pname == 0x2800 || pname == 0x2801)
                ? (value != 0x2600 && value != 0x2601)
            : (pname == 0x2802 || pname == 0x2803)
                ? (value != 0x2901 && value != 0x812f && value != 0x8370)
                : TRUE) {
            mirror_fail("unsupported texture parameter pname/value");
            return;
        }
        TextureState *t = g_hash_table_lookup(textures, idkey(id));
        if (!t) {
            t = g_new0(TextureState, 1);
            t->id = id;
            t->min_filter = t->mag_filter = 0x2601;
            t->wrap_s = t->wrap_t = 0x2901;
            g_hash_table_insert(textures, idkey(id), t);
        }
        if (pname == 0x2801) {
            t->min_filter = value;
        } else if (pname == 0x2800) {
            t->mag_filter = value;
        } else if (pname == 0x2802) {
            t->wrap_s = value;
        } else {
            t->wrap_t = value;
        }
        if (!mirror_ctx || !q_tex_param) {
            return;
        }
        if (!q_tex_param(mirror_ctx, id, (int)pname, (int)value)) {
            mirror_fail("apply texture parameter");
        }
    } else if (!strcmp(fn->name, "glDrawArrays")) {
        capture_draw(p->id, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]);
    } else if (!strcmp(fn->name, "eglSwapBuffers")) {
        if (present_enabled) {
            present_frame((uint32_t)a[1]);
        }
        dump_frame((uint32_t)a[1]);
    }
}

static GHashTable *export_map, *return_map;
static GPtrArray *export_modules;
static gboolean export_words_checked;
static guint64 catalog_tb_count;
static gboolean catalog_logged;

static void json_quote(GString *s, const char *v)
{
    g_string_append_c(s, '"');
    for (const unsigned char *p = (const unsigned char *)v; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            g_string_append_c(s, '\\');
            g_string_append_c(s, *p);
        } else if (*p == '\n') {
            g_string_append(s, "\\n");
        } else if (*p == '\r') {
            g_string_append(s, "\\r");
        } else if (*p == '\t') {
            g_string_append(s, "\\t");
        } else if (*p < 0x20) {
            g_string_append_printf(s, "\\u%04x", *p);
        } else {
            g_string_append_c(s, *p);
        }
    }
    g_string_append_c(s, '"');
}

static void emit(GString *s)
{
    if (!out) {
        return;
    }
    g_mutex_lock(&lock);
    fprintf(out, "%s\n", s->str);
    fflush(out);
    g_mutex_unlock(&lock);
}

static void emit_invalid(const char *what, uint32_t pc, uint32_t expected,
                         uint32_t actual)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s,
        "{\"seq\":\"%" PRIu64 "\",\"kind\":\"invalid\",\"what\":", next_seq++);
    json_quote(s, what);
    g_string_append_printf(
        s, ",\"pc\":\"0x%08x\",\"expected\":\"0x%08x\",\"actual\":\"0x%08x\"}",
        pc, expected, actual);
    emit(s);
    g_string_free(s, TRUE);
}

static void emit_export_catalog(void)
{
    GString *s = g_string_new(NULL);

    g_string_append_printf(
        s,
        "{\"seq\":\"%" PRIu64
        "\",\"kind\":\"export_catalog\",\"count\":%zu,\"exports\":[",
        next_seq++, G_N_ELEMENTS(required_names));
    for (size_t i = 0; i < G_N_ELEMENTS(required_names); i++) {
        Qy8RenderExport *entry =
            qy8_render_export_by_name(export_map, required_names[i]);

        if (i) {
            g_string_append_c(s, ',');
        }
        g_string_append(s, "{\"name\":");
        json_quote(s, entry->name);
        g_string_append_printf(
            s, ",\"va\":\"0x%08x\",\"rva\":\"0x%08x\",\"word\":\"0x%08x\"}",
            entry->base + entry->rva, entry->rva, entry->word);
    }
    g_string_append(s, "]}");
    emit(s);
    g_string_free(s, TRUE);
}

static const char *module_for_lr(uint64_t raw, char *unknown, size_t n)
{
    Qy8RenderModule *module =
        qy8_render_module_for_pc(export_modules, (uint32_t)raw);

    if (module) {
        return module->name;
    }
    uint32_t a = (uint32_t)raw & ~1u;
    snprintf(unknown, n, "unknown_image@0x%08x", a & 0xffff0000u);
    return unknown;
}

static const Qy8RenderExport *find_export(uint32_t pc)
{
    const Qy8RenderExport *x =
        g_hash_table_lookup(export_map, GUINT_TO_POINTER(pc));
    if (!x) {
        x = g_hash_table_lookup(dynamic_exports, GUINT_TO_POINTER(pc));
    }
    return x;
}

/* CE KData current-thread slot -> THREAD+8 owner PROCESS; PROCESS+0x20 name. */
static gboolean current_process_is_aui(uint32_t *process_out)
{
    static const char expected[] = "AuiApp.exe";
    uint32_t thread = 0, process = 0, name_ptr = 0;
    uint8_t name[sizeof(expected) * 2] = { 0 };
    GByteArray *bytes = g_byte_array_new();
    gboolean ok = read_u32(0xffffc824u, &thread) && thread >= 0x1000 &&
                  read_u32((uint64_t)thread + 8, &process) &&
                  process >= 0x1000 &&
                  read_u32((uint64_t)process + 0x20, &name_ptr) &&
                  name_ptr >= 0x1000 && read_mem(name_ptr, sizeof(name), bytes);
    if (ok) {
        memcpy(name, bytes->data, sizeof(name));
    }
    g_byte_array_unref(bytes);
    if (process_out) {
        *process_out = process;
    }
    if (!ok) {
        return FALSE;
    }
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (name[i * 2] != (uint8_t)expected[i] || name[i * 2 + 1] != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

static void emit_service_foreign(const char *name, const char *module,
                                 uint32_t lr, uint32_t process,
                                 gboolean process_read)
{
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s,
        "{\"seq\":\"%" PRIu64
        "\",\"kind\":\"service_foreign_caller\",\"function\":",
        next_seq++);
    json_quote(s, name);
    g_string_append(s, ",\"lr_module\":");
    json_quote(s, module);
    g_string_append_printf(s, ",\"lr\":\"0x%08x\",\"process\":", lr);
    if (process_read) {
        g_string_append_printf(s, "\"0x%08x\"", process);
    } else {
        g_string_append(s, "null");
    }
    g_string_append_c(s, '}');
    emit(s);
    g_string_free(s, TRUE);
}
static const Qy8RenderReturnSite *find_return(uint32_t pc)
{
    return g_hash_table_lookup(return_map, GUINT_TO_POINTER(pc));
}

static gboolean is_client_pc(uint32_t pc)
{
    Qy8RenderModule *module = qy8_render_module_for_pc(export_modules, pc);

    return module && !g_ascii_strcasecmp(module->name, "auirtdll.dll");
}

static gboolean previous_instruction_is_call(uint32_t pc, uint8_t bytes[4],
                                             uint8_t *size)
{
    GByteArray *code = g_byte_array_new();
    uint32_t arm;
    uint16_t first, second, half;
    gboolean call = FALSE;

    if (pc >= 4 && read_mem(pc - 4, 4, code)) {
        memcpy(bytes, code->data, 4);
        memcpy(&arm, code->data, 4);
        memcpy(&first, code->data, 2);
        memcpy(&second, code->data + 2, 2);
        if ((arm & 0x0f000000u) == 0x0b000000u ||
            (arm & 0xfe000000u) == 0xfa000000u ||
            (arm & 0x0ffffff0u) == 0x012fff30u ||
            ((first & 0xf800u) == 0xf000u && (second & 0xc000u) == 0xc000u)) {
            *size = 4;
            call = TRUE;
        }
    }
    g_byte_array_set_size(code, 0);
    if (!call && pc >= 2 && read_mem(pc - 2, 2, code)) {
        memcpy(bytes, code->data, 2);
        memcpy(&half, code->data, 2);
        if ((half & 0xff87u) == 0x4780u) {
            *size = 2;
            call = TRUE;
        }
    }
    g_byte_array_unref(code);
    return call;
}

static Qy8RenderReturnSite *new_return_site(uint32_t pc,
                                            struct qemu_plugin_insn *insn)
{
    uint8_t call_bytes[4] = { 0 }, call_size = 0;
    uint8_t data[4] = { 0 };
    size_t data_size;
    Qy8RenderReturnSite *site;

    if (!is_client_pc(pc) ||
        !previous_instruction_is_call(pc, call_bytes, &call_size)) {
        return NULL;
    }
    data_size = qemu_plugin_insn_data(insn, data, sizeof(data));
    if (data_size != 2 && data_size != 4) {
        return NULL;
    }
    site = g_new0(Qy8RenderReturnSite, 1);
    site->site = pc;
    site->count = 1;
    site->c[0].prev = pc - call_size;
    site->c[0].size = call_size;
    memcpy(site->c[0].bytes, call_bytes, call_size);
    memcpy(&site->word, data, MIN(data_size, sizeof(site->word)));
    return site;
}

static RegSet *regset(unsigned int cpu)
{
    if (cpu >= G_N_ELEMENTS(regs)) {
        return NULL;
    }
    RegSet *r = &regs[cpu];
    if (r->ready) {
        return r;
    }
    GArray *list = qemu_plugin_get_registers();
    if (!list) {
        return NULL;
    }
    for (guint i = 0; i < list->len; ++i) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(list, qemu_plugin_reg_descriptor, i);
        char name[24];
        g_strlcpy(name, d->name, sizeof(name));
        for (char *p = name; *p; ++p) {
            *p = g_ascii_tolower(*p);
        }
        int ix = -1;
        if (name[0] == 'r' && g_ascii_isdigit(name[1])) {
            ix = atoi(name + 1);
        } else if (!strcmp(name, "sp")) {
            ix = 13;
        } else if (!strcmp(name, "lr")) {
            ix = 14;
        } else if (!strcmp(name, "pc")) {
            ix = 15;
        } else if (!strcmp(name, "ttbr0") || !strcmp(name, "ttbr0_el1")) {
            r->ttbr = d->handle;
            r->have_ttbr = TRUE;
        }
        if (ix >= 0 && ix < 16) {
            r->h[ix] = d->handle;
            r->have[ix] = TRUE;
        }
    }
    g_array_free(list, TRUE);
    r->ready = TRUE;
    return r;
}

static gboolean read_ttbr(unsigned int cpu, uint32_t *value)
{
    RegSet *r = regset(cpu);
    GByteArray *b;
    gboolean ok;

    if (!r || !r->have_ttbr) {
        return FALSE;
    }
    b = g_byte_array_new();
    ok = qemu_plugin_read_register(r->ttbr, b) && b->len >= sizeof(*value);
    if (ok) {
        memcpy(value, b->data, sizeof(*value));
    }
    g_byte_array_unref(b);
    return ok;
}

static gboolean read_reg(unsigned int cpu, int ix, uint32_t *value)
{
    RegSet *r = regset(cpu);
    if (!r || ix < 0 || ix >= 16 || !r->have[ix]) {
        return FALSE;
    }
    GByteArray *b = g_byte_array_new();
    gboolean ok = qemu_plugin_read_register(r->h[ix], b) && b->len >= 4;
    if (ok) {
        memcpy(value, b->data, 4);
    }
    g_byte_array_unref(b);
    return ok;
}

static gboolean read_mem(uint64_t addr, size_t len, GByteArray *b)
{
    if (!addr || !len || len > 64u * 1024u * 1024u) {
        return FALSE;
    }
    return qemu_plugin_read_memory_vaddr(addr, b, len) && b->len == len;
}
static gboolean read_u32(uint64_t addr, uint32_t *v)
{
    GByteArray *b = g_byte_array_new();
    gboolean ok = read_mem(addr, 4, b);
    if (ok) {
        memcpy(v, b->data, 4);
    }
    g_byte_array_unref(b);
    return ok;
}
static gboolean read_stack(uint32_t sp, uint64_t values[8])
{
    gboolean all = TRUE;
    for (int i = 0; i < 8; ++i) {
        uint32_t v = 0;
        gboolean ok = read_u32((uint64_t)sp + i * 4, &v);
        values[i] = v;
        all &= ok;
    }
    return all;
}
static char *read_cstr(uint64_t addr)
{
    if (!addr) {
        return NULL;
    }
    char *s = g_malloc0(513);
    for (size_t i = 0; i < 512; ++i) {
        GByteArray *b = g_byte_array_new();
        gboolean ok = read_mem(addr + i, 1, b);
        if (!ok) {
            g_byte_array_unref(b);
            g_free(s);
            return NULL;
        }
        s[i] = b->data[0];
        g_byte_array_unref(b);
        if (!s[i]) {
            return s;
        }
    }
    g_free(s);
    return NULL;
}
static char *base64_mem(uint64_t addr, size_t len)
{
    GByteArray *b = g_byte_array_new();
    if (!read_mem(addr, len, b)) {
        g_byte_array_unref(b);
        return NULL;
    }
    char *s = g_base64_encode(b->data, b->len);
    g_byte_array_unref(b);
    return s;
}
static char *sha256_mem(uint64_t addr, size_t len)
{
    GByteArray *b = g_byte_array_new();
    if (!read_mem(addr, len, b)) {
        g_byte_array_unref(b);
        return NULL;
    }
    char *s = sha256_data(b->data, b->len);
    g_byte_array_unref(b);
    return s;
}
static void append_word_array(GString *s, uint64_t ptr, size_t count)
{
    g_string_append_c(s, '[');
    for (size_t i = 0; i < count; ++i) {
        uint32_t v = 0;
        gboolean ok = read_u32(ptr + i * 4, &v);
        if (i) {
            g_string_append_c(s, ',');
        }
        if (ok) {
            g_string_append_printf(s, "\"0x%08x\"", v);
        } else {
            g_string_append(s, "null");
        }
    }
    g_string_append_c(s, ']');
}
static void append_attr_list(GString *s, uint64_t ptr)
{
    g_string_append_c(s, '[');
    if (!ptr) {
        g_string_append_c(s, ']');
        return;
    }
    for (int i = 0; i < 64; ++i) {
        uint32_t v = 0;
        if (!read_u32(ptr + i * 4, &v)) {
            break;
        }
        if (i) {
            g_string_append_c(s, ',');
        }
        g_string_append_printf(s, "\"0x%08x\"", v);
        if (v == 0x3038) {
            break;
        }
    }
    g_string_append_c(s, ']');
}
static size_t gl_type_width(uint32_t type)
{
    switch (type) {
    case 0x1400:
    case 0x1401:
        return 1;
    case 0x1402:
    case 0x1403:
        return 2;
    case 0x1404:
    case 0x1405:
    case 0x1406:
    case 0x140c:
        return 4;
    default:
        return 0;
    }
}

static void append_capture(GString *s, const Qy8RenderExport *fn, Pending *p)
{
    uint64_t *a = p->args, *st = p->stack;
    gboolean comma = FALSE;
#define SEP()                                                                  \
    do {                                                                       \
        if (comma)                                                             \
            g_string_append_c(s, ',');                                         \
        comma = TRUE;                                                          \
    } while (0)
#define KEY(k)                                                                 \
    do {                                                                       \
        SEP();                                                                 \
        json_quote(s, k);                                                      \
        g_string_append_c(s, ':');                                             \
    } while (0)
    g_string_append_c(s, '{');
    if (!strcmp(fn->name, "eglGetProcAddress")) {
        KEY("name");
        char *v = read_cstr(a[0]);
        if (v) {
            json_quote(s, v);
            g_free(v);
        } else {
            g_string_append(s, "null");
        }
    } else if (!strcmp(fn->name, "glGetUniformLocation") ||
               !strcmp(fn->name, "glGetAttribLocation")) {
        KEY("name");
        char *v = read_cstr(a[1]);
        if (v) {
            json_quote(s, v);
            g_free(v);
        } else {
            g_string_append(s, "null");
        }
    } else if (!strcmp(fn->name, "eglChooseConfig") ||
               !strcmp(fn->name, "eglCreateWindowSurface") ||
               !strcmp(fn->name, "eglCreatePbufferSurface") ||
               !strcmp(fn->name, "eglCreateContext")) {
        uint64_t ptr =
            !strcmp(fn->name, "eglChooseConfig")
                ? a[1]
                : (!strcmp(fn->name, "eglCreatePbufferSurface") ? a[2] : a[3]);
        KEY("attributes");
        append_attr_list(s, ptr);
    } else if (!strcmp(fn->name, "glShaderBinary")) {
        uint32_t n = (uint32_t)a[0], len = (uint32_t)st[0];
        KEY("shader_ids");
        append_word_array(s, a[1], MIN(n, 64));
        KEY("length");
        g_string_append_printf(s, "%u", len);
        KEY("blob_sha256");
        char *hash = sha256_mem(a[3], len);
        if (hash) {
            json_quote(s, hash);
            g_free(hash);
        } else {
            g_string_append(s, "null");
        }
        KEY("blob_base64");
        char *blob = len <= 4u * 1024u * 1024u ? base64_mem(a[3], len) : NULL;
        if (blob) {
            json_quote(s, blob);
            g_free(blob);
        } else {
            g_string_append(s, "null");
        }
    } else if (!strcmp(fn->name, "glCompressedTexImage2D") ||
               !strcmp(fn->name, "glCompressedTexSubImage2D")) {
        uint32_t len =
            (uint32_t)(!strcmp(fn->name, "glCompressedTexImage2D") ? st[2]
                                                                   : st[3]);
        uint64_t ptr =
            !strcmp(fn->name, "glCompressedTexImage2D") ? st[3] : st[4];
        KEY("image_size");
        g_string_append_printf(s, "%u", len);
        KEY("data_sha256");
        char *hash = sha256_mem(ptr, len);
        if (hash) {
            json_quote(s, hash);
            g_free(hash);
        } else {
            g_string_append(s, "null");
        }
    } else if (!strcmp(fn->name, "glUniformMatrix4fv")) {
        size_t len = (size_t)MIN((uint64_t)4096, a[1]) * 64;
        KEY("values_base64");
        char *v = base64_mem(a[3], len);
        if (v) {
            json_quote(s, v);
            g_free(v);
        } else {
            g_string_append(s, "null");
        }
    } else if (!strcmp(fn->name, "glUniform1f") ||
               !strcmp(fn->name, "glUniform1i")) {
        KEY("location");
        g_string_append_printf(s, "%u", (uint32_t)a[0]);
        KEY("value_bits");
        g_string_append_printf(s, "\"0x%08x\"", (uint32_t)a[1]);
    } else if (!strcmp(fn->name, "glUniform4f")) {
        KEY("location");
        g_string_append_printf(s, "%u", (uint32_t)a[0]);
        KEY("value_bits");
        g_string_append_c(s, '[');
        for (int i = 1; i < 4; i++) {
            if (i > 1) {
                g_string_append_c(s, ',');
            }
            g_string_append_printf(s, "\"0x%08x\"", (uint32_t)a[i]);
        }
        g_string_append_printf(s, ",\"0x%08x\"]", (uint32_t)st[0]);
    } else if (!strcmp(fn->name, "glUniform3fv")) {
        size_t len = (size_t)MIN((uint64_t)4096, a[0]) * 12;
        KEY("values_base64");
        char *v = base64_mem(a[1], len);
        if (v) {
            json_quote(s, v);
            g_free(v);
        } else {
            g_string_append(s, "null");
        }
    } else if (!strcmp(fn->name, "glVertexAttribPointer")) {
        uint32_t ix = (uint32_t)a[0];
        if (ix < G_N_ELEMENTS(attrs)) {
            attrs[ix] = (VertexAttr){ .index = ix,
                                      .size = (uint32_t)a[1],
                                      .type = (uint32_t)a[2],
                                      .normalized = (uint32_t)a[3],
                                      .stride = (uint32_t)st[0],
                                      .ptr = st[1],
                                      .valid = TRUE,
                                      .enabled = attrs[ix].enabled };
        }
        KEY("stride");
        g_string_append_printf(s, "%" PRIu64, st[0]);
        KEY("pointer");
        g_string_append_printf(s, "\"0x%08" PRIx64 "\"", st[1]);
    } else if (!strcmp(fn->name, "glEnableVertexAttribArray") ||
               !strcmp(fn->name, "glDisableVertexAttribArray")) {
        uint32_t ix = (uint32_t)a[0];
        if (ix < G_N_ELEMENTS(attrs)) {
            attrs[ix].enabled = !strcmp(fn->name, "glEnableVertexAttribArray");
        }
    } else if (!strcmp(fn->name, "glDrawArrays")) {
        uint32_t first = (uint32_t)a[1], count = MIN((uint32_t)a[2], 4096u);
        KEY("attributes");
        g_string_append_c(s, '[');
        gboolean first_attr = TRUE;
        for (size_t i = 0; i < G_N_ELEMENTS(attrs); ++i) {
            if (attrs[i].valid && attrs[i].enabled) {
                VertexAttr *v = &attrs[i];
                size_t width = gl_type_width(v->type), elem = width * v->size,
                       stride = v->stride ? v->stride : elem;
                size_t len = count ? ((size_t)(count - 1) * stride + elem) : 0;
                uint64_t address = v->ptr + (uint64_t)first * stride;
                char *b64 = len && len <= 4u * 1024u * 1024u
                                ? base64_mem(address, len)
                                : g_strdup("");
                if (!first_attr) {
                    g_string_append_c(s, ',');
                }
                first_attr = FALSE;
                g_string_append_printf(
                    s,
                    "{\"index\":%zu,\"size\":%u,\"type\":\"0x%x\","
                    "\"normalized\":%s,\"stride\":%zu,\"pointer\":\"0x%"
                    "08" PRIx64
                    "\",\"byte_offset\":%zu,\"count\":%u,\"data_base64\":",
                    i, v->size, v->type, v->normalized ? "true" : "false",
                    stride, v->ptr, (size_t)first * stride, count);
                if (b64) {
                    json_quote(s, b64);
                    g_free(b64);
                } else {
                    g_string_append(s, "null");
                }
                g_string_append_c(s, '}');
            }
        }
        g_string_append_c(s, ']');
    } else if (!strcmp(fn->name, "eglSwapBuffers")) {
        KEY("frame");
        g_string_append_printf(s, "%" PRIu64, ++frame_no);
    }
    g_string_append_c(s, '}');
#undef KEY
#undef SEP
}

static void check_all_export_words(void)
{
    if (export_words_checked) {
        return;
    }
    export_words_checked = TRUE;
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s,
        "{\"seq\":\"%" PRIu64
        "\",\"kind\":\"export_words\",\"expected_count\":%zu,\"matches\":",
        next_seq++, G_N_ELEMENTS(required_names));
    size_t matches = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(required_names); ++i) {
        Qy8RenderExport *entry =
            qy8_render_export_by_name(export_map, required_names[i]);
        uint32_t actual = 0;
        if (entry && read_u32((uint64_t)entry->base + entry->rva, &actual) &&
            actual == entry->word) {
            matches++;
        }
    }
    g_string_append_printf(s, "%zu,\"mismatches\":[", matches);
    gboolean comma = FALSE;
    for (size_t i = 0; i < G_N_ELEMENTS(required_names); ++i) {
        Qy8RenderExport *entry =
            qy8_render_export_by_name(export_map, required_names[i]);
        uint32_t actual = 0;
        gboolean readable =
            entry && read_u32((uint64_t)entry->base + entry->rva, &actual);
        if (readable && actual == entry->word) {
            continue;
        }
        if (comma) {
            g_string_append_c(s, ',');
        }
        comma = TRUE;
        g_string_append(s, "{\"name\":");
        json_quote(s, required_names[i]);
        g_string_append_printf(
            s, ",\"va\":\"0x%08x\",\"expected\":\"0x%08x\",\"actual\":",
            entry ? entry->base + entry->rva : 0, entry ? entry->word : 0);
        if (readable) {
            g_string_append_printf(s, "\"0x%08x\"}", actual);
        } else {
            g_string_append(s, "null}");
        }
    }
    g_string_append(s, "]}");
    emit(s);
    g_string_free(s, TRUE);
}

static void append_u32_outputs(GString *s, const char *name, uint64_t ptr,
                               size_t count)
{
    if (!ptr || !count) {
        return;
    }
    g_string_append(s, ",\"out_");
    g_string_append(s, name);
    g_string_append(s, "\":");
    append_word_array(s, ptr, MIN(count, 64));
}

static void append_return_string(GString *s, const char *key, uint64_t ptr)
{
    if (!ptr) {
        return;
    }
    char *v = read_cstr(ptr);
    if (!v) {
        return;
    }
    g_string_append(s, ",\"");
    g_string_append(s, key);
    g_string_append(s, "\":");
    json_quote(s, v);
    g_free(v);
}

static void entry_cb(unsigned int cpu, void *userdata)
{
    check_all_export_words();
    const Qy8RenderExport *fn = userdata;
    uint32_t r[4] = { 0 }, sp = 0, lr = 0, pcword = 0, ttbr = 0;
    gboolean ok = read_reg(cpu, 0, &r[0]) && read_reg(cpu, 1, &r[1]) &&
                  read_reg(cpu, 2, &r[2]) && read_reg(cpu, 3, &r[3]) &&
                  read_reg(cpu, 13, &sp) && read_reg(cpu, 14, &lr);
    if (!ok) {
        emit_invalid("register_read", fn->base + fn->rva, fn->word, 0);
        return;
    }
    if (!read_ttbr(cpu, &ttbr)) {
        emit_invalid("ttbr0_read", fn->base + fn->rva, fn->word, 0);
        return;
    }
    current_proc = proc_get(ttbr);
    uint32_t pc = fn->base + fn->rva;
    if (!read_u32(pc, &pcword) || pcword != fn->word) {
        emit_invalid("export_word", pc, fn->word, pcword);
        return;
    }
    uint64_t stack[8] = { 0 };
    read_stack(sp, stack);
    char unknown[48];
    const char *module = module_for_lr(lr, unknown, sizeof(unknown));
    g_mutex_lock(&lock);
    if (fn->index < G_N_ELEMENTS(required_names)) {
        fn_counts[fn->index]++;
    }
    uint64_t *mc = g_hash_table_lookup(module_counts, module);
    if (!mc) {
        mc = g_new0(uint64_t, 1);
        g_hash_table_insert(module_counts, g_strdup(module), mc);
    }
    (*mc)++;
    g_mutex_unlock(&lock);
    gboolean service_target =
        service_draws &&
        (!strcmp(fn->name, "glDrawArrays") ||
         !strcmp(fn->name, "glDrawElements") || !strcmp(fn->name, "glClear") ||
         !strcmp(fn->name, "glFlush") || !strcmp(fn->name, "glFinish"));
    uint32_t current_process = 0;
    gboolean process_is_aui =
        service_target && current_process_is_aui(&current_process);
    gboolean caller_is_aui = !strcmp(module, "auirtdll.dll");
    if (service_target && (!caller_is_aui || !process_is_aui)) {
        emit_service_foreign(fn->name, module, lr, current_process,
                             current_process != 0);
    }
    if (strcmp(module, "auirtdll.dll")) {
        GString *s = g_string_new(NULL);
        g_string_append_printf(
            s, "{\"seq\":\"%" PRIu64 "\",\"kind\":\"count\",\"name\":",
            next_seq++);
        json_quote(s, fn->name);
        g_string_append(s, ",\"lr_module\":");
        json_quote(s, module);
        g_string_append_printf(s, ",\"lr\":\"0x%08x\"}", lr);
        emit(s);
        g_string_free(s, TRUE);
        return;
    }
    gboolean is_dynamic =
        g_hash_table_lookup(dynamic_exports, GUINT_TO_POINTER(fn->base)) == fn;
    Pending *p = g_new0(Pending, 1);
    p->id = is_dynamic ? next_dynamic_call++ : next_call++;
    p->lr = lr;
    p->sp = sp;
    p->return_pc = lr & ~1u;
    p->fn = fn;
    p->ttbr = ttbr;
    for (int i = 0; i < 4; i++) {
        p->args[i] = r[i];
    }
    memcpy(p->stack, stack, sizeof(stack));
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s,
        "{\"seq\":\"%" PRIu64 "\",\"kind\":\"%s\",\"call_id\":\"%" PRIu64
        "\",\"name\":",
        next_seq++, is_dynamic ? "dynamic_entry" : "entry", p->id);
    json_quote(s, fn->name);
    g_string_append_printf(
        s,
        ",\"lr\":\"0x%08x\",\"lr_module\":\"auirtdll.dll\",\"sp\":\"0x%08x\","
        "\"word\":\"0x%08x\",\"args\":[\"0x%08x\",\"0x%08x\",\"0x%08x\",\"0x%"
        "08x\"],\"stack\":[",
        lr, sp, pcword, r[0], r[1], r[2], r[3]);
    for (int i = 0; i < 8; i++) {
        if (i) {
            g_string_append_c(s, ',');
        }
        g_string_append_printf(s, "\"0x%08" PRIx64 "\"", stack[i]);
    }
    g_string_append(s, "],\"capture\":");
    append_capture(s, fn, p);
    g_string_append_c(s, '}');
    emit(s);
    g_string_free(s, TRUE);
    update_live_state(fn, p);
    if (service_target && caller_is_aui && process_is_aui) {
        uint64_t n =
            (!strcmp(fn->name, "glDrawArrays") ||
             !strcmp(fn->name, "glDrawElements"))
                ? ++service_draw_calls
                : (!strcmp(fn->name, "glClear") ? ++service_clear_calls
                                                : ++service_other_calls);
        GString *skip = g_string_new(NULL);
        g_string_append_printf(skip,
                               "{\"seq\":\"%" PRIu64
                               "\",\"kind\":\"service_skip\",\"function\":",
                               next_seq++);
        json_quote(skip, fn->name);
        g_string_append_printf(
            skip, ",\"call\":\"%" PRIu64 "\",\"call_id\":\"%" PRIu64 "\"}", n,
            p->id);
        emit(skip);
        g_string_free(skip, TRUE);
        g_free(p);
        qemu_plugin_set_pc(lr);
        return;
    }
    g_ptr_array_add(pending, p);
}

static gboolean callsite_valid(const Qy8RenderReturnSite *site)
{
    for (int i = 0; i < site->count; i++) {
        const typeof(site->c[0]) * c = &site->c[i];
        GByteArray *b = g_byte_array_new();
        if (read_mem(c->prev, c->size, b) &&
            !memcmp(b->data, c->bytes, c->size)) {
            g_byte_array_unref(b);
            return TRUE;
        }
        g_byte_array_unref(b);
    }
    return FALSE;
}

static void return_cb(unsigned int cpu, void *userdata)
{
    const Qy8RenderReturnSite *site = userdata;
    if (!pending->len) {
        return;
    }
    uint32_t sp = 0, r0 = 0, ttbr = 0;
    if (!read_reg(cpu, 13, &sp) || !read_ttbr(cpu, &ttbr)) {
        return;
    }
    for (gint i = (gint)pending->len - 1; i >= 0; --i) {
        Pending *p = g_ptr_array_index(pending, i);
        if (p->return_pc != site->site || p->sp != sp || p->ttbr != ttbr) {
            continue;
        }
        current_proc = proc_get(ttbr);
        if (!callsite_valid(site)) {
            emit_invalid("return_callsite_word", site->site, site->word, 0);
            return;
        }
        if (!read_reg(cpu, 0, &r0)) {
            emit_invalid("return_r0", site->site, site->word, 0);
            return;
        }
        gboolean is_dynamic =
            g_hash_table_lookup(dynamic_exports,
                                GUINT_TO_POINTER(p->fn->base)) == p->fn;
        GString *s = g_string_new(NULL);
        g_string_append_printf(
            s,
            "{\"seq\":\"%" PRIu64 "\",\"kind\":\"%s\",\"call_id\":\"%" PRIu64
            "\",\"name\":",
            next_seq++, is_dynamic ? "dynamic_return" : "return", p->id);
        json_quote(s, p->fn->name);
        g_string_append_printf(
            s,
            ",\"lr\":\"0x%08" PRIx64 "\",\"sp\":\"0x%08" PRIx64
            "\",\"r0\":\"0x%08x\",\"return_site\":\"0x%08x\"",
            p->lr, p->sp, r0, site->site);
        if (!strcmp(p->fn->name, "eglGetProcAddress")) {
            char *name = read_cstr(p->args[0]);
            if (name &&
                (!strcmp(name, "eglCreateImageKHR") ||
                 !strcmp(name, "glEGLImageTargetTexture2DOES") ||
                 !strcmp(name, "glFlush") || !strcmp(name, "glFinish")) &&
                r0) {
                uint32_t va = r0 & ~1u, word = 0;
                if (!read_u32(va, &word)) {
                    mirror_fail(
                        "cannot read dynamically resolved EGL/GLES entry word");
                } else {
                    Qy8RenderExport *x = g_new0(Qy8RenderExport, 1);
                    x->name = g_strdup(name);
                    x->base = va;
                    x->rva = 0;
                    x->word = word;
                    x->index = G_MAXSIZE;
                    g_hash_table_replace(dynamic_exports, GUINT_TO_POINTER(va),
                                         x);
                }
            }
            g_free(name);
        } else if (!strcmp(p->fn->name, "eglCreateImageKHR") && r0) {
            uint32_t words[12] = { 0 };
            for (int j = 0; j < 12; j++) {
                read_u32((uint64_t)r0 + j * 4, &words[j]);
            }
            ImageState *im = g_new0(ImageState, 1);
            im->handle = r0;
            im->width = words[1];
            im->height = words[2];
            im->format = words[3];
            im->stride = words[5];
            im->linear = ((uint64_t)words[6]);
            uint32_t bm = 0;
            read_u32((uint64_t)r0 + 0x2c, &bm);
            im->handle = bm;
            g_hash_table_replace(images, idkey(r0), im);
            GString *extra = g_string_new(NULL);
            g_string_append_printf(
                extra,
                "{\"kind\":\"image_state\",\"image\":\"0x%08x\",\"handle\":"
                "\"0x%08x\",\"width\":%u,\"height\":%u,\"format\":\"0x%08x\","
                "\"stride\":%u,\"pvLinAddr\":\"0x%08" PRIx64 "\"}",
                r0, bm, im->width, im->height, im->format, im->stride,
                im->linear);
            emit(extra);
            g_string_free(extra, TRUE);
        } else if (!strcmp(p->fn->name, "eglCreateWindowSurface") ||
                   !strcmp(p->fn->name, "eglCreatePbufferSurface")) {
            if (r0) {
                surface_add_from_create(p->fn, p, r0);
            }
        } else if (!strcmp(p->fn->name, "glEGLImageTargetTexture2DOES")) {
            uint32_t texture =
                active_texture_unit < G_N_ELEMENTS(bound_textures)
                    ? bound_textures[active_texture_unit]
                    : 0;
            TextureState *t = g_hash_table_lookup(textures, idkey(texture));
            if (texture && !t) {
                t = g_new0(TextureState, 1);
                t->id = texture;
                t->min_filter = t->mag_filter = 0x2601;
                t->wrap_s = t->wrap_t = 0x2901;
                g_hash_table_insert(textures, idkey(texture), t);
            }
            if (t) {
                t->image = (uint32_t)p->args[1];
            } else {
                mirror_fail("EGLImageTarget without guest texture");
            }
        } else if (!strcmp(p->fn->name, "glGetProgramiv")) {
            append_u32_outputs(s, "words", p->args[2], 1);
            uint32_t value = 0;
            if (p->args[1] == 0x8b82 && read_u32(p->args[2], &value)) {
                ProgramState *pr = g_hash_table_lookup(
                    program_state, idkey((uint32_t)p->args[0]));
                if (pr) {
                    pr->linked = value != 0;
                }
            }
        } else if (!strcmp(p->fn->name, "glCreateShader")) {
            ShaderState *sh = g_new0(ShaderState, 1);
            sh->type = (uint32_t)p->args[0];
            g_hash_table_replace(shader_state, idkey(r0), sh);
        } else if (!strcmp(p->fn->name, "glCreateProgram")) {
            ProgramState *pr = g_new0(ProgramState, 1);
            g_hash_table_replace(program_state, idkey(r0), pr);
        } else if (!strcmp(p->fn->name, "glGetUniformLocation") ||
                   !strcmp(p->fn->name, "glGetAttribLocation")) {
            char *name = read_cstr(p->args[1]);
            if (name && ((int32_t)r0) >= 0) {
                char *k = loc_key((uint32_t)p->args[0], (int32_t)r0);
                g_hash_table_replace(
                    !strcmp(p->fn->name, "glGetUniformLocation")
                        ? uniform_locations
                        : attrib_locations,
                    k, name);
            } else {
                g_free(name);
            }
        } else if (!strcmp(p->fn->name, "eglMakeCurrent") && r0 == 1) {
            if (p->args[1] != p->args[2]) {
                emit_invalid("makecurrent_draw_read_surface",
                             (uint32_t)p->args[1], (uint32_t)p->args[1],
                             (uint32_t)p->args[2]);
            }
            uint32_t next = (uint32_t)p->args[3];
            if (active_context != next) {
                save_context_state(active_context);
                load_context_state(next);
            }
            active_context = next;
            active_surface = (uint32_t)p->args[1];
            have_makecurrent = TRUE;
        } else if (!strcmp(p->fn->name, "glGenTextures")) {
            append_u32_outputs(s, "words", p->args[1], MIN(p->args[0], 64));
        } else if (!strcmp(p->fn->name, "eglInitialize")) {
            append_u32_outputs(s, "major", p->args[1], 1);
            append_u32_outputs(s, "minor", p->args[2], 1);
        } else if (!strcmp(p->fn->name, "eglChooseConfig")) {
            append_u32_outputs(s, "num_configs", p->stack[0], 1);
            append_u32_outputs(s, "configs", p->args[2], MIN(p->args[3], 64));
        } else if (!strcmp(p->fn->name, "eglGetConfigs")) {
            append_u32_outputs(s, "num_configs", p->args[3], 1);
            append_u32_outputs(s, "configs", p->args[1], MIN(p->args[2], 64));
        } else if (!strcmp(p->fn->name, "eglGetConfigAttrib")) {
            append_u32_outputs(s, "value", p->args[3], 1);
        } else if (!strcmp(p->fn->name, "glGetIntegerv")) {
            append_u32_outputs(s, "words", p->args[1], 4);
        } else if (!strcmp(p->fn->name, "glGetString")) {
            append_return_string(s, "string", r0);
        }
        g_string_append_c(s, '}');
        emit(s);
        g_string_free(s, TRUE);
        if (!strcmp(p->fn->name, "eglSwapBuffers")) {
            completed_swaps++;
        }
        g_ptr_array_remove_index(pending, i);
        g_free(p);
        return;
    }
}

static void tb_trans_cb(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;
    if (!export_modules->len && catalog_tb_count++ % 256 == 0 &&
        qy8_render_catalog_ensure(required_names, G_N_ELEMENTS(required_names),
                                  export_map, export_modules)) {
        for (size_t i = 0; i < G_N_ELEMENTS(required_names); i++) {
            Qy8RenderExport *entry =
                qy8_render_export_by_name(export_map, required_names[i]);

            entry->index = i;
        }
        if (!catalog_logged) {
            catalog_logged = TRUE;
            emit_export_catalog();
        }
    }
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint32_t pc = (uint32_t)qemu_plugin_insn_vaddr(insn);
        const Qy8RenderExport *fn = find_export(pc);
        if (fn) {
            uint8_t data[4] = { 0 };
            size_t z = qemu_plugin_insn_data(insn, data, sizeof(data));
            uint32_t word = 0;
            if (z >= 4) {
                memcpy(&word, data, 4);
            }
            if (z < 4 || word != fn->word) {
                emit_invalid("translation_export_word", pc, fn->word, word);
            } else {
                enum qemu_plugin_cb_flags flags =
                    (!strcmp(fn->name, "glDrawArrays") ||
                     !strcmp(fn->name, "glDrawElements") ||
                     !strcmp(fn->name, "glClear") ||
                     !strcmp(fn->name, "glFlush") ||
                     !strcmp(fn->name, "glFinish"))
                        ? QEMU_PLUGIN_CB_RW_REGS_PC
                        : QEMU_PLUGIN_CB_R_REGS;
                qemu_plugin_register_vcpu_insn_exec_cb(insn, entry_cb, flags,
                                                       (void *)fn);
            }
        }
        Qy8RenderReturnSite *site = (Qy8RenderReturnSite *)find_return(pc);
        if (!site) {
            site = new_return_site(pc, insn);
            if (site) {
                g_hash_table_insert(return_map, GUINT_TO_POINTER(pc), site);
            }
        }
        if (site) {
            uint8_t data[4] = { 0 };
            size_t z = qemu_plugin_insn_data(insn, data, sizeof(data));
            uint32_t word = 0;
            if (z >= 4) {
                memcpy(&word, data, 4);
            }
            uint32_t mask = z >= 4 ? 0xffffffffu : (z == 2 ? 0xffffu : 0u);
            if (!z || (word & mask) != (site->word & mask)) {
                emit_invalid("translation_return_word", pc, site->word, word);
            } else {
                qemu_plugin_register_vcpu_insn_exec_cb(
                    insn, return_cb, QEMU_PLUGIN_CB_R_REGS, (void *)site);
            }
        }
    }
}

static void plugin_exit(void *userdata)
{
    (void)userdata;
    GString *s = g_string_new(NULL);
    g_string_append_printf(
        s, "{\"seq\":\"%" PRIu64 "\",\"kind\":\"histogram\",\"functions\":{",
        next_seq++);
    for (size_t i = 0; i < G_N_ELEMENTS(required_names); i++) {
        if (i) {
            g_string_append_c(s, ',');
        }
        json_quote(s, required_names[i]);
        g_string_append_printf(s, ":%" PRIu64, fn_counts[i]);
    }
    g_string_append(s, "},\"lr_modules\":{");
    GHashTableIter it;
    gpointer key, val;
    gboolean first = TRUE;
    g_hash_table_iter_init(&it, module_counts);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (!first) {
            g_string_append_c(s, ',');
        }
        first = FALSE;
        json_quote(s, key);
        g_string_append_printf(s, ":%" PRIu64, *(uint64_t *)val);
    }
    g_string_append(s, "},\"pending\":[");
    for (guint i = 0; i < pending->len; i++) {
        Pending *p = g_ptr_array_index(pending, i);
        if (i) {
            g_string_append_c(s, ',');
        }
        g_string_append(s, "{\"call_id\":");
        g_string_append_printf(s, "\"%" PRIu64 "\",\"name\":", p->id);
        json_quote(s, p->fn->name);
        g_string_append_printf(
            s, ",\"lr\":\"0x%08" PRIx64 "\",\"sp\":\"0x%08" PRIx64 "\"}", p->lr,
            p->sp);
    }
    g_string_append(s, "]}");
    emit(s);
    g_string_free(s, TRUE);
    GHashTableIter process_iter;
    gpointer process_value;
    g_hash_table_iter_init(&process_iter, processes);
    while (g_hash_table_iter_next(&process_iter, NULL, &process_value)) {
        ProcessState *state = process_value;
        if (state->p_mirror_ctx && q_close) {
            q_close(state->p_mirror_ctx);
        }
        if (state->p_mirror_lib) {
            dlclose(state->p_mirror_lib);
        }
    }
    if (out) {
        fclose(out);
        out = NULL;
    }
}

int clarion_qy8_render_install(qemu_plugin_id_t id, const qemu_info_t *info,
                               int argc, char **argv)
{
    (void)info;
    (void)argc;
    (void)argv;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            return -1;
        }
    }
    if (dump_directory && g_mkdir_with_parents(dump_directory, 0755) < 0) {
        error_report("clarion-qy8-render: cannot create dump directory %s",
                     dump_directory);
        return -1;
    }
    export_map =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    return_map =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    export_modules = g_ptr_array_new();
    module_counts =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    pending = g_ptr_array_new();
    processes = g_hash_table_new(g_direct_hash, g_direct_equal);
    dynamic_exports =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans_cb, NULL);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
