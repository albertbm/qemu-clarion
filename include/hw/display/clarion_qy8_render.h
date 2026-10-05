/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_DISPLAY_CLARION_QY8_RENDER_H
#define HW_DISPLAY_CLARION_QY8_RENDER_H

#include "plugins/qemu-plugin.h"

int clarion_qy8_render_configure(const char *render, const char *render_lib,
                                 const char *render_log,
                                 const char *render_dump_dir);
int clarion_qy8_render_install(qemu_plugin_id_t id, const qemu_info_t *info,
                               int argc, char **argv);

#endif
