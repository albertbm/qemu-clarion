/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CPU execution interface for the typed qy8r USP IR. */
#ifndef QY8R_USP_CPU_H
#define QY8R_USP_CPU_H

#include "qy8r_usp.h"

typedef struct qy8r_usp_cpu_state {
    uint32_t r[128];
    uint32_t o[64];
    uint32_t pa[64];
    uint32_t sa[512];
    uint32_t i[2];
    uint8_t p[4];
    uint32_t sample[4];
    uint8_t discard;
    uint8_t secondary_update;
} qy8r_usp_cpu_state;

typedef int (*qy8r_usp_cpu_sample_fn)(void *opaque, unsigned texture, float u,
                                      float v, uint32_t rgba[4]);

typedef struct qy8r_usp_cpu_inputs {
    const float *symbols[QY8R_USP_MAX_SYMBOLS];
    uint16_t symbol_counts[QY8R_USP_MAX_SYMBOLS];
    float varyings[64][4];
    uint8_t varying_counts[64];
    uint8_t predicate_values[4];
    uint8_t predicate_mask;
} qy8r_usp_cpu_inputs;

typedef struct qy8r_usp_cpu_io {
    qy8r_usp_cpu_state state;
    const qy8r_usp_cpu_inputs *inputs;
    qy8r_usp_cpu_sample_fn sample;
    void *sample_opaque;
    unsigned max_steps;
    uint8_t stage;
    uint32_t result_raw;
} qy8r_usp_cpu_io;

/* Execute one translated variant using only typed IR and caller state. */
int qy8r_usp_cpu_execute(const qy8r_usp_program *program,
                         unsigned variant_index, qy8r_usp_cpu_io *io,
                         char *error, size_t error_size);
int qy8r_usp_cpu_run_blob(const uint8_t *bytes, size_t length,
                          unsigned variant_index,
                          const qy8r_usp_cpu_inputs *inputs,
                          qy8r_usp_cpu_io *io, char *error, size_t error_size);

#endif
