/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional standalone GLSL ES 1.00 lowering for qy8r_usp IR. */
#ifndef QY8R_USP_GLSL_H
#define QY8R_USP_GLSL_H
#include "qy8r_usp.h"
#include <stddef.h>
int qy8r_usp_glsl_lower(const qy8r_usp_program *program, char *output,
                        size_t output_size, qy8r_usp_error *error);
#endif
