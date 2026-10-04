/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GLSL ES 1.00 lowering of the qy8r_usp IR.
 *
 * The generated shader keeps the register model of the original program:
 * each USE register is a vec4 whose .x and .z members stand for the full
 * 32-bit value / low 16-bit lane and the high 16-bit lane respectively.
 * Only the "main" variant is lowered. Constructs outside the reviewed
 * instruction set make the lowering fail without producing output.
 */
#include "qy8r_usp_glsl.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define EXPR_CAP 256

typedef struct outbuf {
    char *p;
    size_t cap;
    size_t len;
    int bad;
} outbuf;

static void put(outbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (b->bad || b->len >= b->cap) {
        b->bad = 1;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->bad = 1;
        return;
    }
    b->len += (size_t)n;
}

static int fail(qy8r_usp_error *e, const char *message)
{
    if (e) {
        e->code = QY8R_USP_ERR_INSTRUCTION;
        e->offset = 0;
        snprintf(e->message, sizeof(e->message), "%s", message);
    }
    return 0;
}

static const char *glsl_type(unsigned type)
{
    switch (type) {
    case 2:
        return "float";
    case 3:
        return "vec2";
    case 4:
        return "vec3";
    case 5:
        return "vec4";
    case 22:
        return "mat4";
    case 24:
        return "sampler2D";
    default:
        return NULL;
    }
}

static void float_text(char *out, size_t cap, double x)
{
    size_t n;

    snprintf(out, cap, "%.9g", x);
    n = strlen(out);
    if (n + 2 < cap && !strchr(out, '.') && !strchr(out, 'e') &&
        !strchr(out, 'E')) {
        out[n++] = '.';
        out[n++] = '0';
        out[n] = 0;
    }
}

static const qy8r_usp_symbol *symbol(const qy8r_usp_program *p, unsigned i)
{
    return i < p->symbol_count ? &p->symbols[i] : NULL;
}

static unsigned popcount16(uint16_t v)
{
    unsigned n = 0;

    while (v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

/*
 * Name of the GLSL array that backs a register bank. SA_UPDATE code runs
 * with the secondary attributes addressed through the PA bank.
 */
static const char *glsl_bank(const qy8r_usp_operand *operand, int secondary)
{
    if (operand->kind != QY8R_USP_OPERAND_REGISTER) {
        return NULL;
    }
    switch (operand->bank) {
    case QY8R_USP_BANK_R:
        return "r";
    case QY8R_USP_BANK_O:
        return "o";
    case QY8R_USP_BANK_PA:
        return secondary ? "sa" : "pa";
    case QY8R_USP_BANK_SA:
        return "sa";
    default:
        return NULL;
    }
}

/* Lane member for a register operand, or NULL for an unreviewed one. */
static const char *glsl_lane(const qy8r_usp_operand *operand)
{
    if (!(operand->flags & QY8R_USP_OPERAND_HAS_COMPONENT) ||
        operand->component == 0) {
        return "x";
    }
    return operand->component == 2 ? "z" : NULL;
}

/* Value of a hardware constant register; only three are reviewed. */
static int constant_value(unsigned number, double *value)
{
    switch (number) {
    case 45:
    case 52:
        *value = 1.0;
        return 1;
    case 48:
        *value = 0.0;
        return 1;
    default:
        return 0;
    }
}

/* Renders a source operand as a GLSL expression. */
static int source_expr(char *out, size_t cap, const qy8r_usp_operand *operand,
                       int secondary, int f16)
{
    char tmp[EXPR_CAP];
    const char *bank, *lane;
    double value;

    if (operand->kind == QY8R_USP_OPERAND_IMMEDIATE) {
        if (operand->flags &
            (QY8R_USP_OPERAND_NEGATE | QY8R_USP_OPERAND_ABSOLUTE)) {
            return 0;
        }
        snprintf(out, cap, "%.1f", (double)operand->immediate);
        return 1;
    }
    if (operand->kind != QY8R_USP_OPERAND_REGISTER) {
        return 0;
    }
    if (operand->bank == QY8R_USP_BANK_I) {
        if (operand->number > 1 || operand->flags) {
            return 0;
        }
        snprintf(out, cap, "internal%u", operand->number);
        return 1;
    }
    if (operand->bank == QY8R_USP_BANK_C) {
        if (!constant_value(operand->number, &value)) {
            return 0;
        }
        /*
         * GLSL does not preserve the sign of zero, so a negated zero is
         * emitted as plain 0.0.
         */
        if ((operand->flags & QY8R_USP_OPERAND_NEGATE) && value != 0.0) {
            value = -value;
        }
        snprintf(out, cap, "%.1f", value);
        return 1;
    }
    bank = glsl_bank(operand, secondary);
    lane = glsl_lane(operand);
    if (!bank || !lane) {
        return 0;
    }
    snprintf(out, cap, "%s[%u].%s", bank, operand->number, lane);
    if (operand->flags & QY8R_USP_OPERAND_ABSOLUTE) {
        snprintf(tmp, sizeof(tmp), "abs(%s)", out);
        snprintf(out, cap, "%s", tmp);
    }
    if (operand->flags & QY8R_USP_OPERAND_NEGATE) {
        snprintf(tmp, sizeof(tmp), "(-(%s))", out);
        snprintf(out, cap, "%s", tmp);
    }
    if (f16 || (operand->flags & QY8R_USP_OPERAND_FLT16)) {
        snprintf(tmp, sizeof(tmp), "hwF16(%s)", out);
        snprintf(out, cap, "%s", tmp);
    }
    return 1;
}

/* Source of one 16-bit half of an FMAD16: lane 0 or 1 of the operand. */
static int source_half_expr(char *out, size_t cap,
                            const qy8r_usp_operand *operand, int secondary,
                            unsigned half)
{
    qy8r_usp_operand lane = *operand;

    if (operand->kind == QY8R_USP_OPERAND_REGISTER) {
        if (operand->bank == QY8R_USP_BANK_I) {
            return 0;
        }
        lane.flags &= (uint8_t)~QY8R_USP_OPERAND_FLT16;
        lane.flags |= QY8R_USP_OPERAND_HAS_COMPONENT;
        lane.component = half ? 2 : 0;
    }
    return source_expr(out, cap, &lane, secondary, 1);
}

/*
 * Renders a destination. @lane overrides the lane member; with a byte mask
 * the whole vec4 is named and the caller selects members.
 */
static int dest_expr(char *out, size_t cap, const qy8r_usp_operand *operand,
                     int secondary, const char *lane)
{
    const char *bank = glsl_bank(operand, secondary);

    if (!bank) {
        return 0;
    }
    if (operand->flags & QY8R_USP_OPERAND_HAS_BYTEMASK) {
        snprintf(out, cap, "%s[%u]", bank, operand->number);
        return 1;
    }
    if (!lane) {
        lane = glsl_lane(operand);
        if (!lane) {
            return 0;
        }
    }
    snprintf(out, cap, "%s[%u].%s", bank, operand->number, lane);
    return 1;
}

static const char *predicate_expr(unsigned predicate)
{
    switch (predicate) {
    case QY8R_USP_PRED_ALWAYS:
        return "true";
    case QY8R_USP_PRED_P0:
        return "p0";
    case QY8R_USP_PRED_P1:
        return "p1";
    case QY8R_USP_PRED_NOT_P0:
        return "!p0";
    case QY8R_USP_PRED_NOT_P1:
        return "!p1";
    default:
        return NULL;
    }
}

/*
 * Two EFO configurations are reviewed: the first step of a dot product
 * (dest = src0 + src1, i0 = src0 * src1, i1 = src0 * src2) and the
 * accumulating step (dest = i0, i0 += src0 * src1, i1 += src0 * src2).
 */
static int lower_efo(outbuf *b, const qy8r_usp_instruction *ins,
                     const qy8r_usp_operand *args, unsigned iter, int sec)
{
    const qy8r_usp_efo *efo = &ins->efo;
    char d[EXPR_CAP], s[3][EXPR_CAP], name[48];
    int first = efo->adder_source == 3;
    unsigned k;

    if (ins->operand_count != 4 || efo->multiplier_source != 0 ||
        !efo->write_i0 || !efo->write_i1 || efo->negate_a1) {
        return 0;
    }
    if (first ? (efo->dest_source != 2 || efo->internal_source != 2) :
                (efo->adder_source != 2 || efo->dest_source != 0 ||
                 efo->internal_source != 0)) {
        return 0;
    }
    for (k = 0; k < 3; k++) {
        if (efo->format_dependent[k] ||
            !source_expr(s[k], sizeof(s[k]), &args[1 + k], sec, 0)) {
            return 0;
        }
    }
    if (!dest_expr(d, sizeof(d), &args[0], sec, NULL)) {
        return 0;
    }
    snprintf(name, sizeof(name), "efo_%u_%u", ins->pc, iter);
    put(b, "highp float %s_s0=%s; highp float %s_s1=%s; "
        "highp float %s_s2=%s;\n", name, s[0], name, s[1], name, s[2]);
    if (first) {
        put(b, "%s=(%s_s0+%s_s1); internal0=(%s_s0*%s_s1); "
            "internal1=(%s_s0*%s_s2);\n", d, name, name, name, name, name,
            name);
    } else {
        put(b, "%s=internal0; internal0=(internal0+%s_s0*%s_s1); "
            "internal1=(internal1+%s_s0*%s_s2);\n", d, name, name, name,
            name);
    }
    return 1;
}

static int lower_mov(outbuf *b, const qy8r_usp_operand *args, int sec)
{
    const char *dbank = glsl_bank(&args[0], sec);
    const char *sbank = glsl_bank(&args[1], sec);
    char d[EXPR_CAP], a[EXPR_CAP];

    /* A plain register-to-register move copies all lanes. */
    if (dbank && sbank && !args[0].flags && !args[1].flags) {
        put(b, "%s[%u]=%s[%u];\n", dbank, args[0].number, sbank,
            args[1].number);
        return 1;
    }
    if (!dest_expr(d, sizeof(d), &args[0], sec, NULL) ||
        !source_expr(a, sizeof(a), &args[1], sec, 0)) {
        return 0;
    }
    put(b, "%s=%s;\n", d, a);
    return 1;
}

static int lower_fmad16(outbuf *b, const qy8r_usp_operand *args, int sec)
{
    static const char *const lanes[] = { "x", "z" };
    char d[EXPR_CAP], a[EXPR_CAP], c[EXPR_CAP], x[EXPR_CAP];
    unsigned half;

    for (half = 0; half < 2; half++) {
        if (!dest_expr(d, sizeof(d), &args[0], sec, lanes[half]) ||
            !source_half_expr(a, sizeof(a), &args[1], sec, half) ||
            !source_half_expr(c, sizeof(c), &args[2], sec, half) ||
            !source_half_expr(x, sizeof(x), &args[3], sec, half)) {
            return 0;
        }
        put(b, "%s=hwF16(hwF16((%s)*(%s))+(%s));\n", d, a, c, x);
    }
    return 1;
}

/* FADD, FSUB, FMIN, FMAX, FMAD and FMSA, with or without a TEST. */
static int lower_arithmetic(outbuf *b, const qy8r_usp_instruction *ins,
                            const qy8r_usp_operand *args, int sec)
{
    char d[EXPR_CAP], a[EXPR_CAP], c[EXPR_CAP], z[EXPR_CAP];
    char x[4 * EXPR_CAP + 32];
    unsigned count = ins->operand_count;
    unsigned off = 0;
    int to_predicate = args[0].kind == QY8R_USP_OPERAND_NO_RESULT;

    if (to_predicate) {
        if (count < 4 || args[1].kind != QY8R_USP_OPERAND_PREDICATE) {
            return 0;
        }
        off = 1;
        snprintf(d, sizeof(d), "p%u", args[1].number);
    } else if (!dest_expr(d, sizeof(d), &args[0], sec, NULL)) {
        return 0;
    }
    if (!source_expr(a, sizeof(a), &args[1 + off], sec, 0) ||
        !source_expr(c, sizeof(c), &args[2 + off], sec, 0)) {
        return 0;
    }
    switch (ins->opcode) {
    case QY8R_USP_OP_FADD:
        snprintf(x, sizeof(x), "(%s+%s)", a, c);
        break;
    case QY8R_USP_OP_FSUB:
        snprintf(x, sizeof(x), "(%s-%s)", a, c);
        break;
    case QY8R_USP_OP_FMIN:
        snprintf(x, sizeof(x), "min(%s,%s)", a, c);
        break;
    case QY8R_USP_OP_FMAX:
        snprintf(x, sizeof(x), "max(%s,%s)", a, c);
        break;
    default:
        if (count <= 3 + off) {
            snprintf(z, sizeof(z), "0.0");
        } else if (!source_expr(z, sizeof(z), &args[3 + off], sec, 0)) {
            return 0;
        }
        if (ins->opcode == QY8R_USP_OP_FMSA) {
            snprintf(x, sizeof(x), "((%s)*(%s)+(%s)*(%s))", a, a, c, z);
        } else {
            snprintf(x, sizeof(x), "((%s)*(%s)+(%s))", a, c, z);
        }
        break;
    }
    if (ins->test != QY8R_USP_TEST_NONE) {
        const char *cmp = ins->test == QY8R_USP_TEST_TANZ ? "!=" :
                          ins->test == QY8R_USP_TEST_TAZ ? "==" :
                          ins->test == QY8R_USP_TEST_NAT ? "<" : ">";

        if (to_predicate) {
            put(b, "%s=((%s)%s0.0);\n", d, x, cmp);
        } else {
            put(b, "%s=((%s)%s0.0)?-1.0:0.0;\n", d, x, cmp);
        }
    } else {
        put(b, "%s=%s;\n", d, x);
    }
    return 1;
}

static int lower_movc(outbuf *b, const qy8r_usp_operand *args, int sec)
{
    char d[EXPR_CAP], a[EXPR_CAP], c[EXPR_CAP], x[EXPR_CAP];

    if (!dest_expr(d, sizeof(d), &args[0], sec, NULL) ||
        !source_expr(a, sizeof(a), &args[1], sec, 0) ||
        !source_expr(c, sizeof(c), &args[2], sec, 0) ||
        !source_expr(x, sizeof(x), &args[3], sec, 0)) {
        return 0;
    }
    put(b, "%s=(%s!=0.0?%s:%s);\n", d, a, c, x);
    return 1;
}

/* Only the reviewed sign tests of a shift by 31 are lowered. */
static int lower_shl(outbuf *b, const qy8r_usp_instruction *ins,
                     const qy8r_usp_operand *args, int sec)
{
    const qy8r_usp_operand *target =
        args[0].kind == QY8R_USP_OPERAND_NO_RESULT ? &args[1] : &args[0];
    char d[EXPR_CAP], a[EXPR_CAP];

    if (!source_expr(a, sizeof(a), &args[2], sec, 0)) {
        return 0;
    }
    if (target->kind == QY8R_USP_OPERAND_PREDICATE) {
        put(b, "p%u=(%s%s0.0);\n", target->number, a,
            ins->test == QY8R_USP_TEST_NAT ? "!=" : "==");
        return 1;
    }
    if (!dest_expr(d, sizeof(d), target, sec, NULL)) {
        return 0;
    }
    put(b, "%s=(%s!=0.0?-0.0:0.0);\n", d, a);
    return 1;
}

static int lower_pack(outbuf *b, const qy8r_usp_instruction *ins,
                      const qy8r_usp_operand *args, int sec)
{
    const char *bank = glsl_bank(&args[0], sec);
    unsigned mask = (args[0].flags & QY8R_USP_OPERAND_HAS_BYTEMASK) ?
                    args[0].bytemask : 15;
    unsigned k, used = 0;
    char a[EXPR_CAP];

    if (!bank) {
        return 0;
    }
    /* Enabled bytes take the two sources alternately. */
    for (k = 0; k < 4; k++) {
        if (!(mask & (1u << k))) {
            continue;
        }
        if (1 + used % 2 >= ins->operand_count ||
            !source_expr(a, sizeof(a), &args[1 + used % 2], sec, 0)) {
            return 0;
        }
        put(b, "%s[%u].%c=hwPackU8(%s);\n", bank, args[0].number, "xyzw"[k],
            a);
        used++;
    }
    return 1;
}

static int lower_unpack_f16(outbuf *b, const qy8r_usp_operand *args, int sec)
{
    char d[EXPR_CAP], a[EXPR_CAP];

    if (!dest_expr(d, sizeof(d), &args[0], sec, NULL) ||
        !source_expr(a, sizeof(a), &args[1], sec, 0)) {
        return 0;
    }
    put(b, "%s=hwF16(%s);\n", d, a);
    return 1;
}

static int lower_unpack_u8(outbuf *b, const qy8r_usp_operand *args, int sec)
{
    const char *dbank = glsl_bank(&args[0], sec);
    const char *sbank = glsl_bank(&args[1], sec);
    unsigned channel = 0, k;

    if (!dbank || !sbank ||
        !(args[0].flags & QY8R_USP_OPERAND_HAS_BYTEMASK)) {
        return 0;
    }
    if (args[1].flags & QY8R_USP_OPERAND_HAS_COMPONENT) {
        channel = args[1].component & 3;
    }
    for (k = 0; k < 4; k++) {
        if (args[0].bytemask & (1u << k)) {
            put(b, "%s[%u].%c=%s[%u].%c;\n", dbank, args[0].number,
                "xyzw"[k], sbank, args[1].number, "xyzw"[channel]);
        }
    }
    return 1;
}

static int lower_instruction(outbuf *b, const qy8r_usp_instruction *ins,
                             const qy8r_usp_operand *args, unsigned iter)
{
    int sec = ins->execution_domain == 1;
    unsigned count = ins->operand_count;

    switch (ins->opcode) {
    case QY8R_USP_OP_NOP:
        put(b, "/* nop */\n");
        return 1;
    case QY8R_USP_OP_SMLSI:
        put(b, "// SMLSI MOE state %d,%d,%d,%d\n", ins->moe_after[0],
            ins->moe_after[1], ins->moe_after[2], ins->moe_after[3]);
        return 1;
    case QY8R_USP_OP_BR:
        return 1; /* handled through the node labels */
    case QY8R_USP_OP_EFO:
        return lower_efo(b, ins, args, iter, sec);
    case QY8R_USP_OP_MOV:
        return count >= 2 && lower_mov(b, args, sec);
    case QY8R_USP_OP_FMAD16:
        return count >= 4 && lower_fmad16(b, args, sec);
    case QY8R_USP_OP_FADD:
    case QY8R_USP_OP_FSUB:
    case QY8R_USP_OP_FMIN:
    case QY8R_USP_OP_FMAX:
    case QY8R_USP_OP_FMAD:
    case QY8R_USP_OP_FMSA:
        return count >= 3 && lower_arithmetic(b, ins, args, sec);
    case QY8R_USP_OP_MOVC:
        return count >= 4 && lower_movc(b, args, sec);
    case QY8R_USP_OP_SHL:
        return count >= 3 && lower_shl(b, ins, args, sec);
    case QY8R_USP_OP_PCKU8F16:
    case QY8R_USP_OP_PCKU8F32:
        return count >= 2 && lower_pack(b, ins, args, sec);
    case QY8R_USP_OP_UNPCKF32F16:
        return count >= 2 && lower_unpack_f16(b, args, sec);
    case QY8R_USP_OP_UNPCKU8U8:
        return count >= 2 && lower_unpack_u8(b, args, sec);
    default:
        return 0;
    }
}

/* The texture coordinate of every sample is the first varying. */
static int lower_sample(outbuf *b, const qy8r_usp_program *p,
                        const qy8r_usp_sample *s, int *sid, int *last)
{
    unsigned i;

    if (s->kind == 5) {
        const qy8r_usp_symbol *sampler = NULL, *coord = NULL;
        char raw[32], sample[32];

        for (i = 0; i < p->bindings.sampler_count; i++) {
            const qy8r_usp_symbol *x = symbol(p, p->bindings.samplers[i]);

            if (x && x->base_comp_or_texture_unit == s->texture_index) {
                sampler = x;
                break;
            }
        }
        if (p->bindings.varying_count) {
            coord = symbol(p, p->bindings.varyings[0]);
        }
        if (!sampler || !coord) {
            return 0;
        }
        snprintf(raw, sizeof(raw), "sampleRaw%d", ++*sid);
        snprintf(sample, sizeof(sample), "sample%d", *sid);
        put(b, "vec4 %s=texture2D(%s,%s);\nvec4 %s=vec4(", raw,
            sampler->name, coord->name, sample);
        for (i = 0; i < 4; i++) {
            unsigned sel = (s->swizzle >> (3 * i)) & 7;

            if (i) {
                put(b, ", ");
            }
            if (sel < 4) {
                put(b, "%s[%u]", raw, sel);
            } else if (sel == 4) {
                put(b, "1.0");
            } else if (sel == 5) {
                put(b, "0.0");
            } else {
                return 0;
            }
        }
        put(b, ");\n");
        for (i = 0; i < 4; i++) {
            put(b, "%s[%u].x=%s[%u];\n", s->base_dest_type == 2 ? "pa" : "r",
                s->base_dest_num + i, sample, i);
        }
        *last = *sid;
        return 1;
    }
    if (s->kind == 6) {
        if (*last < 0) {
            return 0;
        }
        for (i = 0; i < 4; i++) {
            const char *bank;

            if (!s->dest_type[i]) {
                continue;
            }
            bank = s->dest_type[i] == 1 ? "r" :
                   s->dest_type[i] == 2 ? "o" :
                   s->dest_type[i] == 3 ? "pa" : NULL;
            if (!bank) {
                return 0;
            }
            if (s->dest_format[i] == 2) {
                put(b, "%s[%u].%c=hwF16(sample%d[%u]);\n", bank,
                    s->dest_num[i], s->dest_component[i] == 2 ? 'z' : 'x',
                    *last, i);
            } else {
                put(b, "%s[%u].x=sample%d[%u];\n", bank, s->dest_num[i],
                    *last, i);
            }
        }
        return 1;
    }
    return 0;
}

static void declare(outbuf *b, const qy8r_usp_program *p, const uint16_t *ids,
                    unsigned n, const char *qualifier, int highp)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        const qy8r_usp_symbol *s = symbol(p, ids[i]);
        const char *type = s ? glsl_type(s->type) : NULL;

        if (!type) {
            b->bad = 1;
            return;
        }
        put(b, "%s%s%s %s;\n", qualifier, highp ? "highp " : "", type,
            s->name);
    }
}

static void lower_preamble(outbuf *b, const qy8r_usp_program *p, int vertex)
{
    unsigned i;

    put(b, "#version 100\nprecision highp float;\n");
    declare(b, p, p->bindings.attributes, p->bindings.attribute_count,
            "attribute ", 0);
    if (!vertex) {
        declare(b, p, p->bindings.varyings, p->bindings.varying_count,
                "varying ", 0);
    }
    for (i = 0; i < p->symbol_count; i++) {
        if (p->symbols[i].qualifier == 5 &&
            strncmp(p->symbols[i].name, "gl_", 3)) {
            put(b, "varying %s %s;\n", glsl_type(p->symbols[i].type),
                p->symbols[i].name);
        }
    }
    declare(b, p, p->bindings.uniforms, p->bindings.uniform_count,
            "uniform ", 0);
    declare(b, p, p->bindings.samplers, p->bindings.sampler_count,
            "uniform ", 1);
    put(b, "highp float hwRoundEven(highp float x){highp float f=floor(x);"
        "highp float d=x-f;if(d<0.5)return f;if(d>0.5)return f+1.0;"
        "return mod(f,2.0)==0.0?f:f+1.0;}\n");
    put(b, "highp float hwF16(highp float x){if(x==0.0||x!=x)return x;"
        "highp float a=abs(x);highp float s=(a<0.00006103515625)?"
        "0.000000059604644775390625:exp2(floor(log2(a))-10.0);"
        "highp float y=hwRoundEven(a/s)*s;return x<0.0?-y:y;}\n");
    put(b, "float hwPackU8(float x){return floor(clamp(x,0.0,1.0)*255.0+0.5)"
        "/255.0;}\n");
    put(b, "void main(){\n"
        " vec4 r[128];vec4 pa[64];vec4 sa[64];vec4 o[64];"
        "float internal0=0.0,internal1=0.0;bool p0=false,p1=false;"
        "for(int i=0;i<128;i++)r[i]=vec4(0.0);"
        "for(int i=0;i<64;i++){pa[i]=vec4(0.0);sa[i]=vec4(0.0);"
        "o[i]=vec4(0.0);}\n");
}

/* Loads attributes (vertex) or iterated varyings (fragment) into PA. */
static int lower_inputs(outbuf *b, const qy8r_usp_program *p, int vertex,
                        qy8r_usp_error *e)
{
    unsigned i, j;

    if (vertex) {
        for (i = 0; i < p->bindings.attribute_count; i++) {
            const qy8r_usp_symbol *s = symbol(p, p->bindings.attributes[i]);
            unsigned n;

            if (!s) {
                return fail(e, "invalid attribute symbol index");
            }
            n = s->component_use_mask ? popcount16(s->component_use_mask) :
                                        s->component_count;
            for (j = 0; j < n && j < 4; j++) {
                if (s->type == 2) {
                    put(b, "pa[%u].x=%s;\n",
                        s->base_comp_or_texture_unit + j, s->name);
                } else {
                    put(b, "pa[%u].x=%s[%u];\n",
                        s->base_comp_or_texture_unit + j, s->name, j);
                }
            }
        }
        return 1;
    }
    for (i = 0; i < p->ps_input_count && i < p->bindings.varying_count;
         i++) {
        const qy8r_usp_symbol *s = symbol(p, p->bindings.varyings[i]);
        unsigned n;

        if (!s) {
            return fail(e, "invalid varying symbol index");
        }
        n = p->ps_inputs[i].coord_dim + 1;
        if (n > s->component_count) {
            n = s->component_count;
        }
        for (j = 0; j < n && j < 4; j++) {
            put(b, "pa[%u].x=%s[%u];\n", p->ps_inputs[i].coord + j, s->name,
                j);
        }
    }
    return 1;
}

/* Loads uniforms and literals into the secondary attribute registers. */
static int lower_regconsts(outbuf *b, const qy8r_usp_program *p,
                           qy8r_usp_error *e)
{
    unsigned i;

    for (i = 0; i < p->regconst_count; i++) {
        const qy8r_usp_regconst *r = &p->regconsts[i];
        char value[160];

        if (r->uniform_symbol >= 0) {
            const qy8r_usp_symbol *s = symbol(p, (unsigned)r->uniform_symbol);

            if (!s) {
                return fail(e, "invalid RegConst uniform symbol");
            }
            if (s->type == 22) {
                snprintf(value, sizeof(value), "%s[%u][%u]", s->name,
                         r->uniform_component / 4, r->uniform_component % 4);
            } else if (s->type == 3 || s->type == 4 || s->type == 5) {
                snprintf(value, sizeof(value), "%s[%u]", s->name,
                         r->uniform_component);
            } else if (s->type == 2) {
                snprintf(value, sizeof(value), "%s", s->name);
            } else {
                return fail(e, "unsupported RegConst uniform type");
            }
        } else if (r->has_literal) {
            float_text(value, sizeof(value), r->literal);
        } else {
            return fail(e, "RegConst has no source");
        }
        if (r->format != 1 && r->format != 2) {
            return fail(e, "unsupported RegConst format");
        }
        put(b, r->format == 2 ? "sa[%u].%c=hwF16(%s);\n" : "sa[%u].%c=%s;\n",
            r->destination_sa_register,
            r->destination_halfword_shift == 16 ? 'z' : 'x', value);
    }
    return 1;
}

/*
 * Walks the scheduled nodes. A forward branch opens an "if" block that the
 * target label closes; anything else is rejected.
 */
static int lower_nodes(outbuf *b, const qy8r_usp_program *p,
                       const qy8r_usp_variant *v, qy8r_usp_error *e)
{
    uint16_t open[QY8R_USP_MAX_NODES];
    unsigned depth = 0, i, j;
    int sid = -1, last = -1;

    put(b, "// Scheduled shader instructions follow\n");
    for (i = 0; i < v->node_count; i++) {
        const qy8r_usp_node *n = &v->nodes[i];
        const qy8r_usp_instruction *ins;
        const char *condition;
        unsigned count;

        if (n->kind == QY8R_USP_NODE_PHASE_DISCARD) {
            continue;
        }
        if (n->kind == QY8R_USP_NODE_LABEL) {
            while (depth && open[depth - 1] == n->label) {
                put(b, "}\n");
                depth--;
            }
            for (j = 0; j < v->node_count; j++) {
                const qy8r_usp_node *phase = &v->nodes[j];

                if (phase->kind != QY8R_USP_NODE_PHASE_DISCARD ||
                    phase->phase1_start_label != n->label) {
                    continue;
                }
                if (phase->phase_predicate != QY8R_USP_PRED_NOT_P1) {
                    return fail(e, "unsupported phase discard predicate");
                }
                put(b, "if (!p1) discard;\n");
            }
            put(b, "// IR label %u\n", n->label);
            continue;
        }
        if (n->kind == QY8R_USP_NODE_END) {
            while (depth) {
                put(b, "}\n");
                depth--;
            }
            continue;
        }
        if (n->kind == QY8R_USP_NODE_SAMPLE) {
            if (n->index >= v->sample_count ||
                !lower_sample(b, p, &v->samples[n->index], &sid, &last)) {
                return fail(e, "unsupported sample IR construct");
            }
            continue;
        }
        if (n->kind != QY8R_USP_NODE_INSTRUCTION ||
            n->index >= v->instruction_count) {
            return fail(e, "invalid IR node reference");
        }
        ins = &v->instructions[n->index];
        condition = predicate_expr(ins->predicate);
        if (ins->opcode == QY8R_USP_OP_BR) {
            int forward = 0;

            for (j = i + 1; j < v->node_count; j++) {
                if (v->nodes[j].kind == QY8R_USP_NODE_LABEL &&
                    v->nodes[j].label == ins->branch_target_label) {
                    forward = 1;
                }
            }
            if (!condition || !forward || depth >= QY8R_USP_MAX_NODES) {
                return fail(e, "unsupported branch target");
            }
            put(b, "if (!(%s)) {\n", condition);
            open[depth++] = ins->branch_target_label;
            continue;
        }
        if (!condition) {
            return fail(e, "unsupported predicate");
        }
        count = ins->expanded_count ? ins->expanded_count : 1;
        for (j = 0; j < count; j++) {
            const qy8r_usp_operand *args =
                ins->expanded_count ? ins->expanded[j] : ins->operands;

            put(b, "if (%s) {\n", condition);
            if (!lower_instruction(b, ins, args, j)) {
                char message[256], operand[64] = "";

                if (ins->operand_count) {
                    qy8r_usp_operand_dump(ins, &ins->operands[0], operand,
                                          sizeof(operand));
                }
                snprintf(message, sizeof(message),
                         "unsupported opcode %s operand %s",
                         qy8r_usp_opcode_name(ins->opcode), operand);
                return fail(e, message);
            }
            put(b, "}\n");
        }
    }
    if (depth) {
        return fail(e, "unclosed branch target guard");
    }
    return 1;
}

static int lower_outputs(outbuf *b, const qy8r_usp_program *p, int vertex,
                         qy8r_usp_error *e)
{
    unsigned base = p->bindings.position_output_base;
    unsigned i, j;

    if (!vertex) {
        put(b, "gl_FragColor=pa[%u].zyxw;\n", p->bindings.fragment_result_pa);
        return 1;
    }
    if (p->bindings.position_output_components != 4) {
        return fail(e, "position binding is incomplete");
    }
    put(b, "gl_Position=vec4(o[%u].x,o[%u].x,o[%u].x,o[%u].x);\n", base,
        base + 1, base + 2, base + 3);
    for (i = 0; i < p->symbol_count; i++) {
        const qy8r_usp_symbol *s = &p->symbols[i];
        const char *type;
        unsigned components = 0;

        if (s->qualifier != 5 || !strncmp(s->name, "gl_", 3)) {
            continue;
        }
        type = glsl_type(s->type);
        if (!type) {
            return fail(e, "unsupported output varying type");
        }
        put(b, "%s= %s(", s->name, type);
        for (j = 0; j < p->bindings.output_component_count; j++) {
            const qy8r_usp_output_component *m =
                &p->bindings.output_components[j];

            if (m->symbol_index != i) {
                continue;
            }
            if (components++) {
                put(b, ", ");
            }
            put(b, "o[%u].x", m->output_register);
        }
        put(b, ");\n");
    }
    return 1;
}

int qy8r_usp_glsl_lower(const qy8r_usp_program *p, char *out, size_t cap,
                        qy8r_usp_error *e)
{
    outbuf b = { out, cap, 0, 0 };
    const qy8r_usp_variant *v;
    int vertex;

    if (!p || !out || cap < 2 || !p->variant_count ||
        !p->variants[0].valid) {
        return fail(e, "invalid program or no main IR");
    }
    out[0] = 0;
    v = &p->variants[0];
    vertex = p->bindings.attribute_count != 0;
    lower_preamble(&b, p, vertex);
    if (!lower_inputs(&b, p, vertex, e) || !lower_regconsts(&b, p, e) ||
        !lower_nodes(&b, p, v, e) || !lower_outputs(&b, p, vertex, e)) {
        out[0] = 0;
        return 0;
    }
    put(&b, "}\n");
    if (b.bad) {
        out[0] = 0;
        return fail(e, "GLSL output buffer too small");
    }
    if (e) {
        e->code = QY8R_USP_OK;
        e->offset = 0;
        e->message[0] = 0;
    }
    return 1;
}
