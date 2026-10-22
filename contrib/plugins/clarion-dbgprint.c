/*
 * Print Windows CE debug strings emitted by Clarion guests.
 *
 * Load with -plugin libclarion-dbgprint.dylib,syms=FILE[,out=FILE]. The
 * symbol file contains one "address name" pair per line.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

typedef struct {
    uint32_t addr;
    char *name;
} DbgSymbol;

static GHashTable *symbols;
static FILE *output;
static bool arm_target;
static GMutex output_lock;
static struct qemu_plugin_register *reg_arg[4];
static struct qemu_plugin_register *reg_sp;
static struct qemu_plugin_register *reg_lr;

static void free_symbol(gpointer value)
{
    DbgSymbol *sym = value;

    g_free(sym->name);
    g_free(sym);
}

static struct qemu_plugin_register *find_register(const char *name)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    for (guint i = 0; i < regs->len; i++) {
        qemu_plugin_reg_descriptor *reg =
            &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!strcmp(reg->name, name)) {
            return reg->handle;
        }
    }
    return NULL;
}

static uint32_t read_reg(struct qemu_plugin_register *reg)
{
    g_autoptr(GByteArray) value = g_byte_array_new();
    uint32_t result = 0;
    if (qemu_plugin_read_register(reg, value) && value->len >= 4) {
        memcpy(&result, value->data, sizeof(result));
    }
    return result;
}

static bool read_guest(uint32_t addr, void *dst, size_t len)
{
    g_autoptr(GByteArray) value = g_byte_array_new();
    if (!addr || !len || !qemu_plugin_read_memory_vaddr(addr, value, len) ||
        value->len < len) {
        return false;
    }
    memcpy(dst, value->data, len);
    return true;
}

static uint32_t read_u32(uint32_t addr)
{
    uint32_t value = 0;
    read_guest(addr, &value, sizeof(value));
    return value;
}

static void read_wide(uint32_t addr, char *dst, size_t size)
{
    size_t i;
    for (i = 0; i + 1 < size; i++) {
        uint16_t ch = 0;
        if (!read_guest(addr + i * 2, &ch, sizeof(ch)) || !ch) {
            break;
        }
        dst[i] = ch < 0x80 ? (char)ch : '?';
    }
    dst[i] = '\0';
}

static void read_ascii(uint32_t addr, char *dst, size_t size)
{
    size_t i;
    for (i = 0; i + 1 < size; i++) {
        if (!read_guest(addr + i, &dst[i], 1) || !dst[i]) {
            break;
        }
    }
    dst[i] = '\0';
}

typedef struct {
    const uint32_t *regs;
    unsigned int count;
    unsigned int index;
    uint32_t stack;
} ArgIter;

static uint32_t next_arg(ArgIter *it)
{
    if (it->index < it->count) {
        return it->regs[it->index++];
    }
    uint32_t value = read_u32(it->stack);
    it->stack += sizeof(value);
    return value;
}

static void format_message(uint32_t fmt_addr, ArgIter *args, GString *message)
{
    char fmt[512];
    read_wide(fmt_addr, fmt, sizeof(fmt));
    for (const char *p = fmt; *p; p++) {
        char buf[256], spec[16];
        size_t n = 0;
        if (*p != '%') {
            g_string_append_c(message, *p);
            continue;
        }
        spec[n++] = *p++;
        while (*p && strchr("-+ #0123456789.lhwI", *p) && n < 12) {
            if (*p != 'l' && *p != 'h' && *p != 'w' && *p != 'I') {
                spec[n++] = *p;
            }
            p++;
        }
        if (!*p) {
            break;
        }
        switch (*p) {
        case '%':
            g_string_append_c(message, '%');
            break;
        case 's':
            read_wide(next_arg(args), buf, sizeof(buf));
            g_string_append(message, buf);
            break;
        case 'S':
            read_ascii(next_arg(args), buf, sizeof(buf));
            g_string_append(message, buf);
            break;
        case 'c':
        case 'C':
            g_string_append_c(message, (char)next_arg(args));
            break;
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        case 'p':
            spec[n++] = *p == 'p' ? 'x' : *p;
            spec[n] = '\0';
            snprintf(buf, sizeof(buf), spec, next_arg(args));
            g_string_append(message, buf);
            break;
        default:
            g_string_append_c(message, *p);
            break;
        }
    }
}

static void print_debug(unsigned int vcpu_index, void *userdata)
{
    DbgSymbol *sym = userdata;
    uint32_t args[4] = { 0 }, sp, lr, fmt_addr;
    g_autoptr(GString) message = g_string_new(NULL);
    (void)vcpu_index;
    for (unsigned int i = 0; i < 4; i++) {
        args[i] = read_reg(reg_arg[i]);
    }
    sp = read_reg(reg_sp);
    lr = read_reg(reg_lr);
    if (!strcmp(sym->name, "OutputDebugStringW")) {
        char text[512];
        read_wide(args[0], text, sizeof(text));
        g_string_assign(message, text);
    } else {
        ArgIter iter;
        fmt_addr = args[0];
        if (!strcmp(sym->name, "NKvDbgPrintfW")) {
            iter = (ArgIter){ NULL, 0, 0, args[1] };
        } else if (arm_target) {
            iter = (ArgIter){ args + 1, 3, 0, sp };
        } else {
            iter = (ArgIter){ args + 1, 3, 0, sp + 16 };
        }
        format_message(fmt_addr, &iter, message);
    }
    while (message->len && (message->str[message->len - 1] == '\n' ||
                            message->str[message->len - 1] == '\r')) {
        g_string_truncate(message, message->len - 1);
    }
    if (message->len) {
        g_mutex_lock(&output_lock);
        fprintf(output, "DBG %08" PRIx32 " %s\n", lr, message->str);
        fflush(output);
        g_mutex_unlock(&output_lock);
    }
}

static void vcpu_init(unsigned int vcpu_index, void *data)
{
    (void)vcpu_index;
    (void)data;
    for (unsigned int i = 0; i < 4; i++) {
        char name[16];
        if (arm_target) {
            snprintf(name, sizeof(name), "r%u", i);
        } else {
            snprintf(name, sizeof(name), "r%u", i + 4);
        }
        reg_arg[i] = find_register(name);
    }
    reg_sp = find_register(arm_target ? "sp" : "r15");
    reg_lr = find_register(arm_target ? "lr" : "pr");
}

static void tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    (void)userdata;
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        DbgSymbol *sym = g_hash_table_lookup(
            symbols, GUINT_TO_POINTER((uint32_t)qemu_plugin_insn_vaddr(insn)));
        if (sym) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, print_debug,
                                                   QEMU_PLUGIN_CB_R_REGS, sym);
        }
    }
}

static bool load_symbols(const char *path)
{
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        return false;
    }
    lines = g_strsplit(contents, "\n", -1);
    for (size_t i = 0; lines[i]; i++) {
        uint32_t addr;
        char name[128];
        if (sscanf(lines[i], "%" SCNx32 " %127s", &addr, name) != 2) {
            continue;
        }
        DbgSymbol *sym = g_new0(DbgSymbol, 1);
        sym->addr = addr;
        sym->name = g_strdup(name);
        g_hash_table_insert(symbols, GUINT_TO_POINTER(addr), sym);
    }
    return g_hash_table_size(symbols) > 0;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info, int argc,
                                           char **argv)
{
    g_autofree char *sym_path = NULL;
    g_autofree char *out_path = NULL;
    if (!strcmp(info->target_name, "arm")) {
        arm_target = true;
    } else if (!strcmp(info->target_name, "sh4")) {
        arm_target = false;
    } else {
        fprintf(stderr, "clarion-dbgprint: unsupported target %s\n",
                info->target_name);
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) option = g_strsplit(argv[i], "=", 2);
        if (!option[1]) {
            return -1;
        }
        if (!strcmp(option[0], "syms")) {
            g_free(sym_path);
            sym_path = g_strdup(option[1]);
        } else if (!strcmp(option[0], "out")) {
            g_free(out_path);
            out_path = g_strdup(option[1]);
        } else {
            fprintf(stderr, "clarion-dbgprint: unknown option %s\n", option[0]);
            return -1;
        }
    }
    if (!sym_path) {
        fprintf(stderr, "clarion-dbgprint: requires syms=FILE\n");
        return -1;
    }
    symbols =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free_symbol);
    if (!load_symbols(sym_path)) {
        fprintf(stderr, "clarion-dbgprint: cannot load symbols from %s\n",
                sym_path);
        return -1;
    }
    output = out_path ? fopen(out_path, "w") : stderr;
    if (!output) {
        fprintf(stderr, "clarion-dbgprint: cannot open output file\n");
        return -1;
    }
    setvbuf(output, NULL, _IOLBF, 0);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    return 0;
}
