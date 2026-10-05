/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CLARION_QY8_RENDER_CATALOG_H
#define CLARION_QY8_RENDER_CATALOG_H

struct Qy8RenderDispatch;

typedef struct Qy8RenderExport {
    const char *name;
    const struct Qy8RenderDispatch *dispatch;
    uint32_t rva;
    uint32_t word;
    uint32_t base;
    size_t index;
} Qy8RenderExport;

typedef struct Qy8RenderModule {
    char *name;
    uint32_t e32;
    uint32_t o32;
    uint32_t vbase;
    uint32_t vsize;
    uint32_t export_rva;
    uint32_t export_size;
    uint32_t sections;
} Qy8RenderModule;

typedef struct Qy8RenderReturnSite {
    uint32_t site;
    uint32_t word;
    uint8_t count;
    struct {
        uint32_t prev;
        uint8_t size;
        uint8_t bytes[4];
    } c[4];
} Qy8RenderReturnSite;

gboolean qy8_render_catalog_ensure(const char *const *names, size_t count,
                                   GHashTable *exports, GPtrArray *modules);
Qy8RenderExport *qy8_render_export_by_name(GHashTable *exports,
                                           const char *name);
Qy8RenderModule *qy8_render_module_for_pc(GPtrArray *modules, uint32_t pc);
Qy8RenderModule *qy8_render_module_by_name(GPtrArray *modules,
                                           const char *name);

#endif
