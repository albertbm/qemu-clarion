#define EGL_EGLEXT_PROTOTYPES 1
#define qy8r_open qy8r_gl_open
#define qy8r_close qy8r_gl_close
#define qy8r_get_info qy8r_gl_get_info
#define qy8r_get_caps qy8r_gl_get_caps
#define qy8r_program_create qy8r_gl_program_create
#define qy8r_program_create_glsl qy8r_gl_program_create_glsl
#define qy8r_program_create_tf qy8r_gl_program_create_tf
#define qy8r_program_destroy qy8r_gl_program_destroy
#define qy8r_target_create qy8r_gl_target_create
#define qy8r_target_destroy qy8r_gl_target_destroy
#define qy8r_target_bind qy8r_gl_target_bind
#define qy8r_viewport qy8r_gl_viewport
#define qy8r_clear qy8r_gl_clear
#define qy8r_use_program qy8r_gl_use_program
#define qy8r_uniform_f32 qy8r_gl_uniform_f32
#define qy8r_uniform_i32 qy8r_gl_uniform_i32
#define qy8r_texture_rgba32f qy8r_gl_texture_rgba32f
#define qy8r_texture_upload_rgba8 qy8r_gl_texture_upload_rgba8
#define qy8r_texture_parameter qy8r_gl_texture_parameter
#define qy8r_texture_bind qy8r_gl_texture_bind
#define qy8r_target_copy_texture qy8r_gl_target_copy_texture
#define qy8r_bind_target_texture qy8r_gl_bind_target_texture
#define qy8r_texture_set_sampler qy8r_gl_texture_set_sampler
#define qy8r_begin_draw qy8r_gl_begin_draw
#define qy8r_attribute_f32 qy8r_gl_attribute_f32
#define qy8r_draw_arrays qy8r_gl_draw_arrays
#define qy8r_blend_state qy8r_gl_blend_state
#define qy8r_draw_points qy8r_gl_draw_points
#define qy8r_draw_transform_feedback qy8r_gl_draw_transform_feedback
#define qy8r_read_rgba8 qy8r_gl_read_rgba8
#define qy8r_read_rgba8_rect qy8r_gl_read_rgba8_rect
#define qy8r_read_rgba32f qy8r_gl_read_rgba32f
#define qy8r_last_error qy8r_gl_last_error

#include "qy8r.h"
#include "qy8r_backend.h"
#include "qy8r_usp.h"
#include "qy8r_usp_glsl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef QY8R_HAVE_ANGLE_EGL
#include <EGL/eglext_angle.h>
#endif
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GLuint id;
} qy8r_program;
typedef struct {
    GLuint fbo, tex;
    int w, h, is_float;
} qy8r_target;
#define QY8R_GL_MAX_GUEST_TEXTURES 256
typedef struct {
    unsigned key;
    GLuint tex;
    int w, h, live;
} qy8r_guest_texture;
typedef struct {
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surface;
    GLuint vao;
    GLuint *textures;
    GLint max_texture_units;
    qy8r_guest_texture guest_textures[QY8R_GL_MAX_GUEST_TEXTURES];
    GLuint buffers[16];
    int n_buffers;
    char error[512];
} qy8r_context;
static void seterr(qy8r_context *c, const char *s)
{
    snprintf(c->error, sizeof c->error, "%s (egl=0x%x gl=0x%x)", s,
             eglGetError(), glGetError());
}
static int glok(qy8r_context *c, const char *where)
{
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) {
        return 1;
    }
    snprintf(c->error, sizeof c->error, "%s: GL error 0x%x", where, e);
    return 0;
}
static void copyerr(char *dst, size_t n, const char *s)
{
    if (dst && n) {
        snprintf(dst, n, "%s", s ? s : "unknown error");
    }
}
void *qy8r_open(char *error, size_t error_size)
{
    qy8r_context *c = calloc(1, sizeof *c);
    if (!c) {
        copyerr(error, error_size, "out of memory");
        return NULL;
    }
    PFNEGLGETPLATFORMDISPLAYEXTPROC getp =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
#ifdef QY8R_HAVE_ANGLE_EGL
    if (getp) {
        const EGLint angle_attrs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE,
                                       EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
                                       EGL_NONE };
        c->dpy = getp(EGL_PLATFORM_ANGLE_ANGLE, NULL, angle_attrs);
    }
#endif
    if (c->dpy == EGL_NO_DISPLAY && getp) {
        c->dpy = getp(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    }
    if (c->dpy == EGL_NO_DISPLAY) {
        c->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    EGLint major = 0, minor = 0;
    if (c->dpy == EGL_NO_DISPLAY || !eglInitialize(c->dpy, &major, &minor) ||
        major < 1 || (major == 1 && minor < 4)) {
        seterr(c, "EGL 1.4+ initialization failed");
        goto bad;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        seterr(c, "eglBindAPI GLES failed");
        goto bad;
    }
    const EGLint ca[] = { EGL_RENDERABLE_TYPE,
                          EGL_OPENGL_ES3_BIT,
                          EGL_SURFACE_TYPE,
                          EGL_PBUFFER_BIT,
                          EGL_RED_SIZE,
                          8,
                          EGL_GREEN_SIZE,
                          8,
                          EGL_BLUE_SIZE,
                          8,
                          EGL_ALPHA_SIZE,
                          8,
                          EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(c->dpy, ca, &cfg, 1, &n) || n != 1) {
        seterr(c, "no ES3 RGBA8 pbuffer config");
        goto bad;
    }
    const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    c->surface = eglCreatePbufferSurface(c->dpy, cfg, pa);
    const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    c->ctx = eglCreateContext(c->dpy, cfg, EGL_NO_CONTEXT, xa);
    if (c->surface == EGL_NO_SURFACE || c->ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(c->dpy, c->surface, c->surface, c->ctx)) {
        seterr(c, "ES3 context creation/current failed");
        goto bad;
    }
    GLint maj = 0, min = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &maj);
    glGetIntegerv(GL_MINOR_VERSION, &min);
    if (maj < 3) {
        seterr(c, "backend did not provide GLES 3.0+");
        goto bad;
    }
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &c->max_texture_units);
    if (c->max_texture_units < 1) {
        seterr(c, "backend has no texture units");
        goto bad;
    }
    c->textures = calloc((size_t)c->max_texture_units, sizeof *c->textures);
    if (!c->textures) {
        seterr(c, "out of memory allocating texture-unit table");
        goto bad;
    }
    glGenVertexArrays(1, &c->vao);
    glBindVertexArray(c->vao);
    if (!glok(c, "initialize VAO")) {
        goto bad;
    }
    return c;
bad:
    copyerr(error, error_size, c->error);
    if (c->ctx != EGL_NO_CONTEXT) {
        eglDestroyContext(c->dpy, c->ctx);
    }
    if (c->surface != EGL_NO_SURFACE) {
        eglDestroySurface(c->dpy, c->surface);
    }
    if (c->dpy != EGL_NO_DISPLAY) {
        eglTerminate(c->dpy);
    }
    free(c->textures);
    free(c);
    return NULL;
}
void qy8r_close(void *p)
{
    qy8r_context *c = p;
    if (!c) {
        return;
    }
    EGLBoolean current = eglMakeCurrent(c->dpy, c->surface, c->surface, c->ctx);
    if (current) {
        if (c->textures) {
            for (GLint i = 0; i < c->max_texture_units; i++) {
                if (c->textures[i]) {
                    glDeleteTextures(1, &c->textures[i]);
                }
            }
        }
        for (size_t i = 0; i < QY8R_GL_MAX_GUEST_TEXTURES; i++) {
            if (c->guest_textures[i].live && c->guest_textures[i].tex) {
                glDeleteTextures(1, &c->guest_textures[i].tex);
            }
        }
        for (int i = 0; i < c->n_buffers; i++) {
            glDeleteBuffers(1, &c->buffers[i]);
        }
        if (c->vao) {
            glDeleteVertexArrays(1, &c->vao);
        }
        eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    eglDestroyContext(c->dpy, c->ctx);
    eglDestroySurface(c->dpy, c->surface);
    eglTerminate(c->dpy);
    free(c->textures);
    free(c);
}
int qy8r_get_info(void *p, char *b, size_t n)
{
    qy8r_context *c = p;
    if (!c || !b || !n) {
        return 0;
    }
    int used =
        snprintf(b, n,
                 "EGL_VERSION=%s\nGL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%"
                 "s\nGLSL=%s\nGL_EXTENSIONS=",
                 eglQueryString(c->dpy, EGL_VERSION), glGetString(GL_VENDOR),
                 glGetString(GL_RENDERER), glGetString(GL_VERSION),
                 glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint ext = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &ext);
    for (GLint i = 0; i < ext && used > 0 && (size_t)used < n; i++) {
        const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        used += snprintf(b + used, n - (size_t)used, "%s%s", i ? " " : "",
                         x ? x : "");
    }
    return glok(c, "query info");
}
static int has_ext(const char *name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *x = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (x && !strcmp(x, name)) {
            return 1;
        }
    }
    return 0;
}
static int tfprobe(void)
{
    const char *vs =
        "#version 100\nprecision highp float; attribute highp float a; varying "
        "highp float v; void main(){v=a;gl_Position=vec4(0.0,0.0,0.0,1.0);}";
    const char *fs = "#version 100\nprecision highp float; varying highp float "
                     "v; void main(){gl_FragColor=vec4(v);}";
    GLuint v = glCreateShader(GL_VERTEX_SHADER),
           f = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
    glShaderSource(v, 1, &vs, NULL);
    glCompileShader(v);
    glShaderSource(f, 1, &fs, NULL);
    glCompileShader(f);
    GLint ok = 0;
    glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        goto no;
    }
    glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        goto no;
    }
    glAttachShader(p, v);
    glAttachShader(p, f);
    const GLchar *varyings[] = { "gl_Position", "v" };
    glTransformFeedbackVaryings(p, 2, varyings, GL_INTERLEAVED_ATTRIBS);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    return ok ? 1 : 0;
no:
    glDeleteProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    return 0;
}
int qy8r_get_caps(void *p, int *cbf, int *tf)
{
    qy8r_context *c = p;
    if (!c) {
        return 0;
    }
    if (cbf) {
        *cbf = has_ext("GL_EXT_color_buffer_float");
    }
    if (tf) {
        *tf = tfprobe();
    }
    return 1;
}
static GLuint compile(GLenum type, const char *src, char *log, size_t n)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok && log && n) {
        glGetShaderInfoLog(s, (GLsizei)n, NULL, log);
    }
    if (!ok) {
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static void *program_create(void *p, const char *vs, const char *fs,
                            const char *const *tfv, int tfn, char *log,
                            size_t n)
{
    qy8r_context *c = p;
    if (!c || !vs || !fs) {
        return NULL;
    }
    if (log && n) {
        log[0] = 0;
    }
    GLuint v = compile(GL_VERTEX_SHADER, vs, log, n);
    if (!v) {
        return NULL;
    }
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, log, n);
    if (!f) {
        glDeleteShader(v);
        return NULL;
    }
    qy8r_program *q = calloc(1, sizeof *q);
    if (!q) {
        glDeleteShader(v);
        glDeleteShader(f);
        return NULL;
    }
    q->id = glCreateProgram();
    glAttachShader(q->id, v);
    glAttachShader(q->id, f);
    if (tfn > 0) {
        glTransformFeedbackVaryings(q->id, tfn, (const GLchar *const *)tfv,
                                    GL_INTERLEAVED_ATTRIBS);
    }
    glLinkProgram(q->id);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(q->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        if (log && n) {
            glGetProgramInfoLog(q->id, (GLsizei)n, NULL, log);
        }
        glDeleteProgram(q->id);
        free(q);
        return NULL;
    }
    if (!glok(c, "link program")) {
        glDeleteProgram(q->id);
        free(q);
        return NULL;
    }
    return q;
}
static char *lower_blob(const uint8_t *bytes, size_t size, unsigned stage,
                        char *log, size_t log_size)
{
    qy8r_usp_program *ir;
    qy8r_usp_error error;
    char *source;
    const size_t source_size = 1024 * 1024;

    ir = calloc(1, sizeof(*ir));
    if (!ir) {
        if (log && log_size) {
            snprintf(log, log_size, "out of memory parsing shader blob");
        }
        return NULL;
    }
    if (!qy8r_usp_parse(bytes, size, 0, ir, &error)) {
        if (log && log_size) {
            snprintf(log, log_size, "%s at 0x%zx: %s",
                     qy8r_usp_error_name(error.code), error.offset,
                     error.message);
        }
        free(ir);
        return NULL;
    }
    if (ir->stage != stage) {
        if (log && log_size) {
            snprintf(log, log_size, "shader blob has the wrong stage");
        }
        free(ir);
        return NULL;
    }
    source = malloc(source_size);
    if (!source) {
        if (log && log_size) {
            snprintf(log, log_size, "out of memory lowering shader blob");
        }
        free(ir);
        return NULL;
    }
    if (!qy8r_usp_glsl_lower(ir, source, source_size, &error)) {
        if (log && log_size) {
            snprintf(log, log_size, "%s", error.message);
        }
        free(source);
        source = NULL;
    }
    free(ir);
    return source;
}

void *qy8r_program_create(void *p, const uint8_t *vs, size_t vs_size,
                          const uint8_t *fs, size_t fs_size, char *log,
                          size_t log_size)
{
    char *vertex_source;
    char *fragment_source;
    void *program;

    if (!p || !vs || !fs || !vs_size || !fs_size) {
        if (log && log_size) {
            snprintf(log, log_size,
                     "program creation needs vertex and fragment blobs");
        }
        return NULL;
    }
    if (log && log_size) {
        log[0] = 0;
    }
    vertex_source = lower_blob(vs, vs_size, 0, log, log_size);
    if (!vertex_source) {
        return NULL;
    }
    fragment_source = lower_blob(fs, fs_size, 1, log, log_size);
    if (!fragment_source) {
        free(vertex_source);
        return NULL;
    }
    program = program_create(p, vertex_source, fragment_source, NULL, 0, log,
                             log_size);
    free(vertex_source);
    free(fragment_source);
    return program;
}
void *qy8r_program_create_tf(void *p, const char *vs, const char *fs,
                             const char *const *varyings, int count, char *log,
                             size_t n)
{
    if (!varyings || count < 1) {
        return NULL;
    }
    return program_create(p, vs, fs, varyings, count, log, n);
}
void *qy8r_program_create_glsl(void *p, const char *vs, const char *fs,
                               char *log, size_t n)
{
    return program_create(p, vs, fs, NULL, 0, log, n);
}
void qy8r_program_destroy(void *p, void *q)
{
    (void)p;
    qy8r_program *x = q;
    if (x) {
        glDeleteProgram(x->id);
        free(x);
    }
}
void *qy8r_target_create(void *p, int w, int h, int fl)
{
    qy8r_context *c = p;
    if (!c || w < 1 || h < 1) {
        return NULL;
    }
    qy8r_target *t = calloc(1, sizeof *t);
    if (!t) {
        return NULL;
    }
    t->w = w;
    t->h = h;
    t->is_float = !!fl;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLint internal = fl ? GL_RGBA32F : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA,
                 fl ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           t->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
        !glok(c, "create target")) {
        qy8r_target_destroy(c, t);
        return NULL;
    }
    return t;
}
void qy8r_target_destroy(void *p, void *q)
{
    (void)p;
    qy8r_target *t = q;
    if (!t) {
        return;
    }
    if (t->fbo) {
        glDeleteFramebuffers(1, &t->fbo);
    }
    if (t->tex) {
        glDeleteTextures(1, &t->tex);
    }
    free(t);
}
int qy8r_target_bind(void *p, void *q)
{
    qy8r_context *c = p;
    qy8r_target *t = q;
    if (!c || !t) {
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, t->w, t->h);
    return glok(c, "bind target");
}
int qy8r_viewport(void *p, int x, int y, int w, int h)
{
    qy8r_context *c = p;
    if (!c || w < 1 || h < 1) {
        return 0;
    }
    glViewport(x, y, w, h);
    return glok(c, "set viewport");
}
int qy8r_clear(void *p, float r, float g, float b, float a)
{
    qy8r_context *c = p;
    if (!c) {
        return 0;
    }
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    return glok(c, "clear");
}
int qy8r_use_program(void *p, void *q)
{
    qy8r_context *c = p;
    qy8r_program *x = q;
    if (!c || !x) {
        return 0;
    }
    glUseProgram(x->id);
    return glok(c, "use program");
}
int qy8r_uniform_f32(void *p, void *q, const char *name, const float *v,
                     int count)
{
    qy8r_context *c = p;
    qy8r_program *x = q;
    if (!c || !x || !name || !v) {
        return 0;
    }
    GLint l = glGetUniformLocation(x->id, name);
    if (l < 0) {
        snprintf(c->error, sizeof c->error, "required uniform not active: %s",
                 name);
        return 0;
    }
    if (count == 1) {
        glUniform1fv(l, 1, v);
    } else if (count == 2) {
        glUniform2fv(l, 1, v);
    } else if (count == 3) {
        glUniform3fv(l, 1, v);
    } else if (count == 4) {
        glUniform4fv(l, 1, v);
    } else if (count == 16) {
        glUniformMatrix4fv(l, 1, GL_FALSE, v);
    } else {
        return 0;
    }
    return glok(c, "set float uniform");
}
int qy8r_uniform_i32(void *p, void *q, const char *name, const int *v,
                     int count)
{
    qy8r_context *c = p;
    qy8r_program *x = q;
    if (!c || !x || !name || !v) {
        return 0;
    }
    GLint l = glGetUniformLocation(x->id, name);
    if (l < 0) {
        snprintf(c->error, sizeof c->error, "required uniform not active: %s",
                 name);
        return 0;
    }
    if (count == 1) {
        glUniform1iv(l, 1, v);
    } else if (count == 2) {
        glUniform2iv(l, 1, v);
    } else if (count == 3) {
        glUniform3iv(l, 1, v);
    } else if (count == 4) {
        glUniform4iv(l, 1, v);
    } else {
        return 0;
    }
    return glok(c, "set integer uniform");
}
int qy8r_texture_rgba32f(void *p, void *q, const char *sampler,
                         const float *rgba, int unit)
{
    qy8r_context *c = p;
    qy8r_program *x = q;
    if (!c || !x || !sampler || !rgba || unit < 0) {
        return 0;
    }
    if (unit >= c->max_texture_units) {
        snprintf(c->error, sizeof c->error,
                 "texture unit %d exceeds backend limit %d", unit,
                 c->max_texture_units);
        return 0;
    }
    GLint l = glGetUniformLocation(x->id, sampler);
    if (l < 0) {
        snprintf(c->error, sizeof c->error, "required sampler not active: %s",
                 sampler);
        return 0;
    }
    if (!c->textures[unit]) {
        glGenTextures(1, &c->textures[unit]);
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, c->textures[unit]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT,
                 rgba);
    glUniform1i(l, unit);
    return glok(c, "upload float texture");
}
static qy8r_guest_texture *guest_texture(qy8r_context *c, unsigned key,
                                         int create)
{
    qy8r_guest_texture *free_slot = NULL;
    if (!c || !key) {
        return NULL;
    }
    for (size_t i = 0; i < QY8R_GL_MAX_GUEST_TEXTURES; i++) {
        qy8r_guest_texture *t = &c->guest_textures[i];
        if (t->live && t->key == key) {
            return t;
        }
        if (!t->live && !free_slot) {
            free_slot = t;
        }
    }
    if (!create || !free_slot) {
        return NULL;
    }
    free_slot->key = key;
    free_slot->live = 1;
    glGenTextures(1, &free_slot->tex);
    if (!glok(c, "create guest texture")) {
        memset(free_slot, 0, sizeof(*free_slot));
        return NULL;
    }
    return free_slot;
}
int qy8r_texture_upload_rgba8(void *p, unsigned key, int width, int height,
                              const unsigned char *rgba, size_t byte_count)
{
    qy8r_context *c = p;
    if (!c || !rgba || width < 1 || height < 1 ||
        (size_t)width > SIZE_MAX / (size_t)height / 4 ||
        byte_count < (size_t)width * (size_t)height * 4) {
        return 0;
    }
    qy8r_guest_texture *t = guest_texture(c, key, 1);
    if (!t) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    t->w = width;
    t->h = height;
    return glok(c, "upload guest RGBA8 texture");
}
int qy8r_texture_parameter(void *p, unsigned key, int pname, int value)
{
    qy8r_context *c = p;
    qy8r_guest_texture *t = guest_texture(c, key, 1);
    if (!c || !t) {
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexParameteri(GL_TEXTURE_2D, (GLenum)pname, value);
    return glok(c, "set guest texture parameter");
}
int qy8r_texture_bind(void *p, unsigned key, int unit)
{
    qy8r_context *c = p;
    qy8r_guest_texture *t = guest_texture(c, key, 1);
    if (!c || !t || unit < 0 || unit >= c->max_texture_units) {
        return 0;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    return glok(c, "bind guest texture");
}
int qy8r_target_copy_texture(void *p, void *q, unsigned key)
{
    qy8r_context *c = p;
    qy8r_target *target = q;
    qy8r_guest_texture *t = guest_texture(c, key, 1);
    if (!c || !target || !t) {
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, target->w, target->h, 0);
    t->w = target->w;
    t->h = target->h;
    return glok(c, "copy target to guest texture");
}
int qy8r_bind_target_texture(void *p, void *q, int unit)
{
    qy8r_context *c = p;
    qy8r_target *target = q;
    if (!c || !target || unit < 0 || unit >= c->max_texture_units) {
        return 0;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, target->tex);
    return glok(c, "bind render target texture");
}
int qy8r_texture_set_sampler(void *p, void *q, const char *sampler,
                             unsigned key, int unit)
{
    qy8r_context *c = p;
    qy8r_program *program = q;
    qy8r_guest_texture *t = guest_texture(c, key, 1);
    if (!c || !program || !sampler || !t || unit < 0 ||
        unit >= c->max_texture_units) {
        return 0;
    }
    GLint location = glGetUniformLocation(program->id, sampler);
    if (location < 0) {
        snprintf(c->error, sizeof c->error, "required sampler not active: %s",
                 sampler);
        return 0;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glUniform1i(location, unit);
    return glok(c, "set sampler texture");
}
int qy8r_begin_draw(void *p, void *q)
{
    qy8r_context *c = p;
    qy8r_program *x = q;
    if (!c || !x) {
        return 0;
    }
    glBindVertexArray(c->vao);
    GLint n = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &n);
    for (GLint i = 0; i < n; i++) {
        glDisableVertexAttribArray((GLuint)i);
    }
    for (int i = 0; i < c->n_buffers; i++) {
        glDeleteBuffers(1, &c->buffers[i]);
    }
    c->n_buffers = 0;
    glUseProgram(x->id);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DITHER);
    return glok(c, "begin draw");
}
int qy8r_attribute_f32(void *p, void *q, const char *name, const float *v,
                       int comps, int count)
{
    qy8r_context *c = p;
    qy8r_program *x = q;
    if (!c || !x || !name || !v || comps < 1 || comps > 4 || count < 1 ||
        c->n_buffers >= 16) {
        return 0;
    }
    GLint loc = glGetAttribLocation(x->id, name);
    if (loc < 0) {
        snprintf(c->error, sizeof c->error, "required attribute not active: %s",
                 name);
        return 0;
    }
    GLuint b;
    glGenBuffers(1, &b);
    c->buffers[c->n_buffers++] = b;
    glBindBuffer(GL_ARRAY_BUFFER, b);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * comps * count),
                 v, GL_STREAM_DRAW);
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, comps, GL_FLOAT, GL_FALSE, 0, 0);
    return glok(c, "set attribute");
}
int qy8r_draw_arrays(void *p, int mode, int first, int count)
{
    qy8r_context *c = p;
    if (!c || first < 0 || count < 1 ||
        (mode != GL_TRIANGLES && mode != GL_TRIANGLE_STRIP &&
         mode != GL_TRIANGLE_FAN)) {
        if (c) {
            snprintf(c->error, sizeof c->error,
                     "unsupported draw mode/arguments");
        }
        return 0;
    }
    glDrawArrays((GLenum)mode, first, count);
    glFinish();
    return glok(c, "draw arrays");
}
int qy8r_blend_state(void *p, int enable, int sr, int dr, int sa, int da,
                     const float color[4])
{
    qy8r_context *c = p;
    if (!c || !color) {
        return 0;
    }
    if (enable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate((GLenum)sr, (GLenum)dr, (GLenum)sa, (GLenum)da);
        glBlendColor(color[0], color[1], color[2], color[3]);
    } else {
        glDisable(GL_BLEND);
    }
    return glok(c, "set blend state");
}
int qy8r_draw_points(void *p, int count)
{
    qy8r_context *c = p;
    if (!c || count < 1) {
        return 0;
    }
    glDrawArrays(GL_POINTS, 0, count);
    glFinish();
    return glok(c, "draw points");
}
int qy8r_draw_transform_feedback(void *p, int count, float *out, size_t nfloat)
{
    qy8r_context *c = p;
    if (!c || count < 1 || !out) {
        return 0;
    }
    size_t bytes = nfloat * sizeof(float);
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER, b);
    glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER, (GLsizeiptr)bytes, NULL,
                 GL_STREAM_READ);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, b);
    glEnable(GL_RASTERIZER_DISCARD);
    glBeginTransformFeedback(GL_POINTS);
    glDrawArrays(GL_POINTS, 0, count);
    glEndTransformFeedback();
    glDisable(GL_RASTERIZER_DISCARD);
    glFinish();
    void *m = glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER, 0,
                               (GLsizeiptr)bytes, GL_MAP_READ_BIT);
    if (!m) {
        glDeleteBuffers(1, &b);
        return glok(c, "map transform feedback");
    }
    memcpy(out, m, bytes);
    GLboolean ok = glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);
    glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER, 0, 0);
    glDeleteBuffers(1, &b);
    return ok && glok(c, "transform feedback readback");
}
int qy8r_read_rgba8(void *p, void *q, unsigned char out[4])
{
    qy8r_context *c = p;
    qy8r_target *t = q;
    if (!c || !t || !out || t->is_float) {
        return 0;
    }
    qy8r_target_bind(c, t);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return glok(c, "read RGBA8");
}
int qy8r_read_rgba8_rect(void *p, void *q, unsigned char *out, size_t n)
{
    qy8r_context *c = p;
    qy8r_target *t = q;
    if (!c || !t || !out || t->is_float ||
        n < (size_t)t->w * (size_t)t->h * 4) {
        return 0;
    }
    qy8r_target_bind(c, t);
    glReadPixels(0, 0, t->w, t->h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return glok(c, "read RGBA8 rect");
}
int qy8r_read_rgba32f(void *p, void *q, float out[4])
{
    qy8r_context *c = p;
    qy8r_target *t = q;
    if (!c || !t || !out || !t->is_float) {
        return 0;
    }
    qy8r_target_bind(c, t);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, out);
    return glok(c, "read RGBA32F");
}
const char *qy8r_last_error(void *p)
{
    qy8r_context *c = p;
    return c ? c->error : "invalid context";
}

const qy8r_backend_ops qy8r_gl_backend = {
    .open = qy8r_gl_open,
    .close = qy8r_gl_close,
    .get_info = qy8r_gl_get_info,
    .get_caps = qy8r_gl_get_caps,
    .program_create = qy8r_gl_program_create,
    .program_create_glsl = qy8r_gl_program_create_glsl,
    .program_create_tf = qy8r_gl_program_create_tf,
    .program_destroy = qy8r_gl_program_destroy,
    .target_create = qy8r_gl_target_create,
    .target_destroy = qy8r_gl_target_destroy,
    .target_bind = qy8r_gl_target_bind,
    .viewport = qy8r_gl_viewport,
    .clear = qy8r_gl_clear,
    .use_program = qy8r_gl_use_program,
    .uniform_f32 = qy8r_gl_uniform_f32,
    .uniform_i32 = qy8r_gl_uniform_i32,
    .texture_rgba32f = qy8r_gl_texture_rgba32f,
    .begin_draw = qy8r_gl_begin_draw,
    .attribute_f32 = qy8r_gl_attribute_f32,
    .draw_arrays = qy8r_gl_draw_arrays,
    .blend_state = qy8r_gl_blend_state,
    .draw_points = qy8r_gl_draw_points,
    .draw_transform_feedback = qy8r_gl_draw_transform_feedback,
    .read_rgba8 = qy8r_gl_read_rgba8,
    .read_rgba8_rect = qy8r_gl_read_rgba8_rect,
    .read_rgba32f = qy8r_gl_read_rgba32f,
    .texture_upload_rgba8 = qy8r_gl_texture_upload_rgba8,
    .texture_parameter = qy8r_gl_texture_parameter,
    .texture_bind = qy8r_gl_texture_bind,
    .target_copy_texture = qy8r_gl_target_copy_texture,
    .bind_target_texture = qy8r_gl_bind_target_texture,
    .texture_set_sampler = qy8r_gl_texture_set_sampler,
    .last_error = qy8r_gl_last_error,
};
