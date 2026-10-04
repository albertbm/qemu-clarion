/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone SGXBS and target-1.7 USP reader. */
#include "qy8r_usp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cursor {
    const uint8_t *data;
    size_t size;
    size_t pos;
    size_t base;
} cursor;

static int fail(qy8r_usp_error *e, qy8r_usp_error_code code, size_t off,
                const char *fmt, ...)
{
    va_list ap;
    if (e) {
        e->code = code;
        e->offset = off;
        va_start(ap, fmt);
        vsnprintf(e->message, sizeof(e->message), fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int take(cursor *c, size_t n, const uint8_t **p, qy8r_usp_error *e,
                const char *field)
{
    if (n > c->size - c->pos) {
        return fail(e, QY8R_USP_ERR_TRUNCATED, c->base + c->pos,
                    "%s: need %zu bytes, have %zu", field, n, c->size - c->pos);
    }
    *p = c->data + c->pos;
    c->pos += n;
    return 1;
}

static int u8(cursor *c, uint8_t *v, qy8r_usp_error *e, const char *f)
{
    const uint8_t *p;
    if (!take(c, 1, &p, e, f)) {
        return 0;
    }
    *v = p[0];
    return 1;
}

static int u16le(cursor *c, uint16_t *v, qy8r_usp_error *e, const char *f)
{
    const uint8_t *p;
    if (!take(c, 2, &p, e, f)) {
        return 0;
    }
    *v = (uint16_t)p[0] | (uint16_t)p[1] << 8;
    return 1;
}

static int u32le(cursor *c, uint32_t *v, qy8r_usp_error *e, const char *f)
{
    const uint8_t *p;
    if (!take(c, 4, &p, e, f)) {
        return 0;
    }
    *v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
    return 1;
}

static int u16be(cursor *c, uint16_t *v, qy8r_usp_error *e, const char *f)
{
    const uint8_t *p;
    if (!take(c, 2, &p, e, f)) {
        return 0;
    }
    *v = (uint16_t)p[0] << 8 | p[1];
    return 1;
}

static int u32be(cursor *c, uint32_t *v, qy8r_usp_error *e, const char *f)
{
    const uint8_t *p;
    if (!take(c, 4, &p, e, f)) {
        return 0;
    }
    *v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
    return 1;
}

static int f32be(cursor *c, float *v, qy8r_usp_error *e, const char *f)
{
    uint32_t bits;
    if (!u32be(c, &bits, e, f)) {
        return 0;
    }
    memcpy(v, &bits, sizeof(bits));
    return 1;
}

const char *qy8r_usp_error_name(qy8r_usp_error_code code)
{
    static const char *const names[] = {
        "ok",      "argument", "truncated",   "container", "revision",
        "profile", "block",    "instruction", "limit",     "binding"
    };
    return (unsigned)code < sizeof(names) / sizeof(names[0]) ? names[code]
                                                             : "unknown";
}

/* The SGXBS checksum is the legacy lookup3-style hash used by this wrapper. */
static uint32_t sgxbs_hash(const uint8_t *p, size_t n)
{
    uint32_t h = (uint32_t)n;
    size_t i = 0, words = n >> 2;
    while (words--) {
        uint32_t a = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
        uint32_t b = ((uint32_t)p[i + 3] << 19) | ((uint32_t)p[i + 2] << 11);
        h += a;
        b ^= h;
        h = (h << 16) ^ b;
        h += h >> 11;
        i += 4;
    }
    switch (n & 3) {
    case 3:
        h += (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
        h ^= h << 16;
        h ^= (uint32_t)p[i + 2] << 18;
        h += h >> 11;
        break;
    case 2:
        h += (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
        h ^= h << 11;
        h += h >> 17;
        break;
    case 1:
        h += p[i];
        h ^= h << 10;
        h += h >> 1;
        break;
    default:
        break;
    }
    h ^= h << 3;
    h += h >> 5;
    h ^= h << 4;
    h += h >> 17;
    h ^= h << 25;
    h += h >> 6;
    return h + 0x8001;
}

/*
 * USE instruction decoding.
 *
 * Only the instruction shapes reviewed against the frozen listings are
 * accepted; every other encoding fails closed. The decoder fills the typed
 * IR straight from the two instruction words.
 */

#define USE_W1_SKIPINV (1u << 23)
#define USE_W1_FMTCTL (1u << 22) /* F16 format select on sources */
#define USE_W1_REPEAT_ENABLE (1u << 21)
#define USE_W1_DEST_EXT (1u << 19)
#define USE_W1_SRC0_EXT (1u << 18)
#define USE_W1_SRC1_EXT (1u << 17)
#define USE_W1_SRC2_EXT (1u << 16)
#define USE_F16_SELECT 64u        /* in a 7-bit source number */
#define USE_PACK_SCALE (1u << 18) /* in word 0 */

static void set_register(qy8r_usp_operand *operand, unsigned bank,
                         unsigned number)
{
    memset(operand, 0, sizeof(*operand));
    operand->kind = QY8R_USP_OPERAND_REGISTER;
    operand->bank = (uint8_t)bank;
    operand->number = (uint16_t)number;
}

static void set_immediate(qy8r_usp_operand *operand, uint32_t value)
{
    memset(operand, 0, sizeof(*operand));
    operand->kind = QY8R_USP_OPERAND_IMMEDIATE;
    operand->immediate = value;
}

static void set_kind(qy8r_usp_operand *operand, unsigned kind)
{
    memset(operand, 0, sizeof(*operand));
    operand->kind = (uint8_t)kind;
}

static void set_component(qy8r_usp_operand *operand, unsigned component)
{
    operand->component = (uint8_t)component;
    operand->flags |= QY8R_USP_OPERAND_HAS_COMPONENT;
}

static int decode_dest(uint32_t w0, uint32_t w1, qy8r_usp_operand *operand,
                       qy8r_usp_error *e, size_t offset)
{
    static const uint8_t banks[] = { QY8R_USP_BANK_R, QY8R_USP_BANK_O,
                                     QY8R_USP_BANK_PA };
    unsigned number = (w0 >> 21) & 63;
    unsigned bank = w1 & 3;

    if (w1 & USE_W1_DEST_EXT) {
        if (bank != 0) {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "extended destination bank %u is not in reviewed "
                        "profile",
                        bank);
        }
        set_register(operand, QY8R_USP_BANK_SA, number);
        return 1;
    }
    if (bank == 3) {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "indexed destination is not in reviewed use profile");
    }
    set_register(operand, banks[bank], number);
    return 1;
}

/*
 * Source slot 0 has a one-bit bank; slots 1 and 2 have two-bit banks. With
 * F16 format control active, a register number with USE_F16_SELECT set
 * packs the format flag, the register number and the 16-bit lane.
 */
static int decode_source(uint32_t w0, uint32_t w1, unsigned slot, int f16,
                         unsigned modifier, qy8r_usp_operand *operand,
                         qy8r_usp_error *e, size_t offset)
{
    unsigned number, bank;

    if (slot == 0) {
        number = (w0 >> 14) & 127;
        if (w1 & USE_W1_SRC0_EXT) {
            bank = (w1 & 4) ? QY8R_USP_BANK_SA : QY8R_USP_BANK_O;
        } else {
            bank = (w1 & 4) ? QY8R_USP_BANK_PA : QY8R_USP_BANK_R;
        }
    } else if (slot == 1 || slot == 2) {
        static const uint8_t banks[] = { QY8R_USP_BANK_R, QY8R_USP_BANK_O,
                                         QY8R_USP_BANK_PA, QY8R_USP_BANK_SA };
        unsigned raw = slot == 1 ? (w0 >> 30) & 3 : (w0 >> 28) & 3;
        int extended =
            (w1 & (slot == 1 ? USE_W1_SRC1_EXT : USE_W1_SRC2_EXT)) != 0;

        number = slot == 1 ? (w0 >> 7) & 127 : w0 & 127;
        if (!extended) {
            bank = banks[raw];
        } else if (raw == 1) {
            if (number & 64) {
                number &= 63;
                bank = QY8R_USP_BANK_G;
            } else {
                bank = QY8R_USP_BANK_C;
            }
        } else if (raw == 2) {
            set_immediate(operand, number);
            operand->flags |= (uint8_t)(modifier & 3);
            return 1;
        } else if (raw == 3) {
            if (number > 1) {
                return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                            "internal source register %u is unsupported",
                            number);
            }
            bank = QY8R_USP_BANK_I;
        } else {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "extended source bank %u is not in reviewed profile",
                        raw);
        }
    } else {
        return fail(e, QY8R_USP_ERR_ARGUMENT, offset, "invalid source slot %u",
                    slot);
    }
    if (bank == QY8R_USP_BANK_R && number >= 124) {
        bank = QY8R_USP_BANK_I;
        number -= 124;
    }
    if (f16 && (number & USE_F16_SELECT) &&
        (bank == QY8R_USP_BANK_R || bank == QY8R_USP_BANK_O ||
         bank == QY8R_USP_BANK_PA || bank == QY8R_USP_BANK_SA)) {
        set_register(operand, bank, (number & 63) >> 1);
        set_component(operand, (number & 1) ? 2 : 0);
        operand->flags |= QY8R_USP_OPERAND_FLT16;
    } else {
        set_register(operand, bank, number);
    }
    /* NEGATE and ABSOLUTE share the encoding of the modifier field. */
    operand->flags |= (uint8_t)(modifier & 3);
    return 1;
}

/* Destination plus the first @sources source slots, with modifiers. */
static int decode_alu(qy8r_usp_instruction *ins, unsigned sources, int f16,
                      qy8r_usp_error *e, size_t offset)
{
    static const unsigned shifts[] = { 7, 5, 3 };
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    unsigned i;

    if (!decode_dest(w0, w1, &ins->operands[0], e, offset)) {
        return 0;
    }
    for (i = 0; i < sources; i++) {
        if (!decode_source(w0, w1, i, f16, (w1 >> shifts[i]) & 3,
                           &ins->operands[1 + i], e, offset)) {
            return 0;
        }
    }
    ins->operand_count = (uint8_t)(1 + sources);
    return 1;
}

static int decode_efo(qy8r_usp_instruction *ins, unsigned pred,
                      qy8r_usp_error *e, size_t offset)
{
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    uint32_t src_w1 =
        w1 & ~(USE_W1_SRC0_EXT | USE_W1_SRC1_EXT | USE_W1_SRC2_EXT);
    unsigned s;

    if (pred != 0 || ((w1 >> 12) & 3) != 0) {
        return fail(
            e, QY8R_USP_ERR_INSTRUCTION, offset,
            "EFO predicate or repeat is outside the reviewed signatures");
    }
    ins->opcode = QY8R_USP_OP_EFO;
    ins->signature = 2;
    ins->repeat_count = 1;
    ins->efo.dest_source = (w1 >> 20) & 3;
    ins->efo.internal_source = (w1 >> 18) & 3;
    ins->efo.adder_source = (w1 >> 16) & 3;
    ins->efo.multiplier_source = (w1 >> 14) & 3;
    ins->efo.write_i0 = (w1 & (1u << 10)) != 0;
    ins->efo.write_i1 = (w1 & (1u << 9)) != 0;
    ins->efo.negate_a1 = (w1 & (1u << 22)) != 0;
    /* In an EFO the extension bits are selector fields, not bank bits. */
    if (!decode_dest(w0, w1 & ~USE_W1_DEST_EXT, &ins->operands[0], e, offset)) {
        return 0;
    }
    for (s = 0; s < 3; s++) {
        unsigned modifier = (w1 >> (7 - 2 * s)) & 3;

        if (!decode_source(w0, src_w1, s, 0, modifier, &ins->operands[1 + s], e,
                           offset) ||
            !decode_source(w0, src_w1, s, 1, modifier, &ins->efo.source_f16[s],
                           e, offset)) {
            return 0;
        }
        ins->efo.format_dependent[s] =
            memcmp(&ins->operands[1 + s], &ins->efo.source_f16[s],
                   sizeof(qy8r_usp_operand)) != 0;
    }
    ins->operand_count = 4;
    return 1;
}

static int decode_mov(qy8r_usp_instruction *ins, unsigned pred, unsigned repeat,
                      qy8r_usp_error *e, size_t offset)
{
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    unsigned data_type = (w1 >> 8) & 7;
    unsigned i;

    if (data_type == 0) {
        if (repeat == 1 && pred == 0) {
            ins->signature = 18;
        } else if (repeat == 1 && pred == 5) {
            ins->signature = 19;
        } else if (repeat == 1 && pred == 1) {
            ins->signature = 20;
        } else if (repeat == 2 && pred == 0) {
            ins->signature = 21;
        } else if (repeat == 3 && pred == 1) {
            ins->signature = 22;
        } else {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "mov predicate/repeat combination is outside "
                        "the reviewed signatures");
        }
        ins->opcode = QY8R_USP_OP_MOV;
        if (!decode_dest(w0, w1, &ins->operands[0], e, offset) ||
            !decode_source(w0, w1, 1, 0, 0, &ins->operands[1], e, offset)) {
            return 0;
        }
        ins->operand_count = 2;
        return 1;
    }
    if (data_type == 4 && pred == 1 && repeat == 1) {
        ins->signature = 24;
        ins->data_type = QY8R_USP_DATA_FLT;
    } else if (data_type == 4 && pred == 1 && repeat == 3) {
        ins->signature = 23;
        ins->data_type = QY8R_USP_DATA_FLT;
    } else if (data_type == 3 && pred == 0 && repeat == 3) {
        ins->signature = 25;
        ins->data_type = QY8R_USP_DATA_I32;
    } else if (data_type == 3 && pred == 1 && repeat == 3) {
        ins->signature = 26;
        ins->data_type = QY8R_USP_DATA_I32;
    } else {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "movc data type/predicate/repeat is outside the reviewed "
                    "signatures");
    }
    ins->opcode = QY8R_USP_OP_MOVC;
    if (!decode_dest(w0, w1, &ins->operands[0], e, offset)) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (!decode_source(w0, w1, i, 0, 0, &ins->operands[1 + i], e, offset)) {
            return 0;
        }
    }
    ins->operand_count = 4;
    return 1;
}

static int decode_float(qy8r_usp_instruction *ins, unsigned op, unsigned op2,
                        unsigned pred, unsigned repeat, qy8r_usp_error *e,
                        size_t offset)
{
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    int f16 = (w1 & USE_W1_FMTCTL) != 0;

    if (op == 0 && op2 == 0) {
        if (repeat == 1 && pred == 5) {
            ins->signature = 5;
        } else if (repeat == 1 && pred == 1) {
            ins->signature = 6;
        } else if (repeat == 2 && pred == 0) {
            ins->signature = 7;
        } else if (repeat == 3 && pred == 1) {
            ins->signature = 8;
        } else if (repeat == 1 && pred == 0) {
            ins->signature = 4;
        } else {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "fmad predicate/repeat combination is outside "
                        "the reviewed signatures");
        }
        ins->opcode = QY8R_USP_OP_FMAD;
        return decode_alu(ins, 3, f16, e, offset);
    }
    if (op == 0 && op2 == 2) {
        if (pred != 1 || repeat != 1) {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "fmsa predicate/repeat combination is outside "
                        "the reviewed signatures");
        }
        ins->opcode = QY8R_USP_OP_FMSA;
        ins->signature = 14;
        return decode_alu(ins, 3, f16, e, offset);
    }
    if (op == 6 && op2 == 0) {
        if (pred != 0 || repeat != 2) {
            return fail(
                e, QY8R_USP_ERR_INSTRUCTION, offset,
                "fmad16 predicate/repeat is outside the reviewed signatures");
        }
        ins->opcode = QY8R_USP_OP_FMAD16;
        ins->signature = 9;
        return decode_alu(ins, 3, 1, e, offset);
    }
    /* op == 3: FMIN (op2 0) or FMAX (op2 1) */
    if (op2 == 0) {
        if (pred != 1) {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "fmin predicate is outside the reviewed signatures");
        }
        ins->opcode = QY8R_USP_OP_FMIN;
        ins->signature = 12;
    } else {
        if (pred != 0 && pred != 1) {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "fmax predicate is outside the reviewed signatures");
        }
        ins->opcode = QY8R_USP_OP_FMAX;
        ins->signature = pred == 1 ? 11 : 10;
    }
    if (repeat != 1) {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "min/max repeat is outside the reviewed signatures");
    }
    if (!decode_dest(w0, w1, &ins->operands[0], e, offset) ||
        !decode_source(w0, w1, 1, f16, 0, &ins->operands[1], e, offset) ||
        !decode_source(w0, w1, 2, f16, 0, &ins->operands[2], e, offset)) {
        return 0;
    }
    ins->operand_count = 3;
    return 1;
}

static int decode_test(qy8r_usp_instruction *ins, unsigned pred,
                       unsigned repeat, qy8r_usp_error *e, size_t offset)
{
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    unsigned alu_sel = (w0 >> 18) & 3;
    unsigned alu_op = (w0 >> 14) & 15;
    unsigned sign_test = (w1 >> 10) & 3;
    unsigned zero_test = (w1 >> 8) & 3;
    unsigned channel = (w1 >> 4) & 7;
    unsigned pdest = (w1 >> 2) & 3;
    int source1_f16 = 0, source2_f16 = 0;
    int writes_register;

    if (repeat != 1) {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "TEST repeat is outside the reviewed signatures");
    }
    if (alu_sel == 3 && alu_op == 3 && zero_test == 0 && channel == 0 &&
        ((sign_test == 1 && pdest <= 1) || (sign_test == 2 && pdest == 0))) {
        ins->opcode = QY8R_USP_OP_SHL;
        ins->signature = sign_test == 1 ? 33 : 34;
        ins->test = sign_test == 1 ? QY8R_USP_TEST_NAT : QY8R_USP_TEST_PAT;
        ins->test_mask = QY8R_USP_TEST_MASK_CHANNEL;
        writes_register = 0;
    } else if (alu_sel == 0 && alu_op == 9 && pred == 0 && sign_test == 0 &&
               zero_test == 2) {
        ins->opcode = QY8R_USP_OP_FMIN;
        ins->signature = 13;
        ins->test = QY8R_USP_TEST_TANZ;
        ins->test_mask = QY8R_USP_TEST_MASK_DMSK;
        writes_register = 1;
    } else if (alu_sel == 0 && alu_op == 0 && pred == 0 && sign_test == 0 &&
               zero_test == 2 && channel == 0) {
        ins->opcode = QY8R_USP_OP_FADD;
        ins->signature = 3;
        ins->test = QY8R_USP_TEST_TANZ;
        ins->test_mask = QY8R_USP_TEST_MASK_CHANNEL;
        source1_f16 = 1;
        writes_register = 0;
        pdest = 0;
    } else if (alu_sel == 0 && alu_op == 14 && pred == 0 && sign_test == 0 &&
               zero_test == 1 && channel == 0) {
        ins->opcode = QY8R_USP_OP_FSUB;
        ins->signature = 17;
        ins->test = QY8R_USP_TEST_TAZ;
        ins->test_mask = QY8R_USP_TEST_MASK_CHANNEL;
        source1_f16 = 1;
        writes_register = 0;
        pdest = 0;
    } else if (alu_sel == 0 && alu_op == 14 && sign_test == 2 &&
               zero_test == 2 && ((w1 >> 4) & 3) == 2 &&
               (pred == 0 || pred == 1)) {
        ins->opcode = QY8R_USP_OP_FSUB;
        ins->signature = pred == 0 ? 15 : 16;
        ins->test = QY8R_USP_TEST_PANZ;
        ins->test_mask = QY8R_USP_TEST_MASK_DMSK;
        source1_f16 = pred == 0;
        source2_f16 = 1;
        writes_register = 1;
    } else {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "TEST signature outside the reviewed set "
                    "(alu=%u/%u pred=%u test=%u/%u)",
                    alu_sel, alu_op, pred, sign_test, zero_test);
    }
    if (writes_register) {
        if (!decode_dest(w0, w1, &ins->operands[0], e, offset) ||
            !decode_source(w0, w1, 1, source1_f16, 0, &ins->operands[1], e,
                           offset) ||
            !decode_source(w0, w1, 2, source2_f16, 0, &ins->operands[2], e,
                           offset)) {
            return 0;
        }
        ins->operand_count = 3;
        return 1;
    }
    set_kind(&ins->operands[0], QY8R_USP_OPERAND_NO_RESULT);
    set_kind(&ins->operands[1], QY8R_USP_OPERAND_PREDICATE);
    ins->operands[1].number = (uint16_t)pdest;
    if (!decode_source(w0, w1, 1, source1_f16, 0, &ins->operands[2], e,
                       offset) ||
        !decode_source(w0, w1, 2, source2_f16, 0, &ins->operands[3], e,
                       offset)) {
        return 0;
    }
    ins->operand_count = 4;
    return 1;
}

static int decode_pack(qy8r_usp_instruction *ins, unsigned pred,
                       unsigned repeat, qy8r_usp_error *e, size_t offset)
{
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    unsigned srcf = (w1 >> 9) & 7;
    unsigned dstf = (w1 >> 6) & 7;
    unsigned mask = (w1 >> 2) & 15;
    unsigned c;

    if (srcf == 0 && dstf == 0 && pred == 5 && repeat == 1) {
        if (mask != 8) {
            return fail(
                e, QY8R_USP_ERR_INSTRUCTION, offset,
                "UNPCKU8U8 byte mask is outside the reviewed signature");
        }
        ins->opcode = QY8R_USP_OP_UNPCKU8U8;
        ins->signature = 37;
        if (!decode_dest(w0, w1, &ins->operands[0], e, offset) ||
            !decode_source(w0, w1, 1, 0, 0, &ins->operands[1], e, offset)) {
            return 0;
        }
        ins->operands[0].bytemask = (uint8_t)mask;
        ins->operands[0].flags |= QY8R_USP_OPERAND_HAS_BYTEMASK;
        set_component(&ins->operands[1], 3);
    } else if (srcf == 5 && dstf == 6 && pred == 0 && repeat == 1) {
        ins->opcode = QY8R_USP_OP_UNPCKF32F16;
        ins->signature = 36;
        if (!decode_dest(w0, w1, &ins->operands[0], e, offset) ||
            !decode_source(w0, w1, 1, 0, 0, &ins->operands[1], e, offset)) {
            return 0;
        }
        set_component(&ins->operands[1], (w0 >> 16) & 3);
    } else {
        if (dstf != 0 || (srcf != 5 && srcf != 6) || repeat != 1 ||
            !(w0 & USE_PACK_SCALE)) {
            if (srcf == 7 && dstf == 7) {
                return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                            "PCK C10->C10 (srcf=7 dstf=7) is outside "
                            "the reviewed signatures");
            }
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "PCK op=8 srcf=%u dstf=%u scale=%u repeat=%u is "
                        "outside the reviewed signatures",
                        srcf, dstf, (w0 >> 18) & 1, repeat);
        }
        if (srcf == 5 && pred == 0) {
            ins->signature = 28;
        } else if (srcf == 5 && pred == 1) {
            ins->signature = 29;
        } else if (srcf == 6 && pred == 0) {
            ins->signature = 30;
        } else if (srcf == 6 && pred == 5) {
            ins->signature = 31;
        } else if (srcf == 6 && pred == 1) {
            ins->signature = 32;
        } else {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "pack predicate is outside the reviewed signatures");
        }
        ins->opcode = srcf == 5 ? QY8R_USP_OP_PCKU8F16 : QY8R_USP_OP_PCKU8F32;
        ins->flags |= QY8R_USP_INSN_SCALE;
        if (!decode_dest(w0, w1, &ins->operands[0], e, offset)) {
            return 0;
        }
        if (mask != 15) {
            ins->operands[0].bytemask = (uint8_t)mask;
            ins->operands[0].flags |= QY8R_USP_OPERAND_HAS_BYTEMASK;
        }
        for (c = 0; c < 2; c++) {
            if (!decode_source(w0, w1, c + 1, 0, 0, &ins->operands[1 + c], e,
                               offset)) {
                return 0;
            }
            set_component(&ins->operands[1 + c],
                          c ? (w0 >> 14) & 3 : (w0 >> 16) & 3);
        }
        set_kind(&ins->operands[3], QY8R_USP_OPERAND_ROUND_NEAREST);
        ins->operand_count = 4;
        return 1;
    }
    /* Unpacks have one data source; the second slot is a zero constant. */
    set_immediate(&ins->operands[2], 0);
    set_component(&ins->operands[2], 0);
    set_kind(&ins->operands[3], QY8R_USP_OPERAND_ROUND_NEAREST);
    ins->operand_count = 4;
    return 1;
}

static int decode_instruction(qy8r_usp_instruction *ins, qy8r_usp_error *e,
                              size_t offset)
{
    static const uint8_t predicates[] = {
        QY8R_USP_PRED_ALWAYS, QY8R_USP_PRED_P0, QY8R_USP_PRED_P1,
        QY8R_USP_PRED_P2,     QY8R_USP_PRED_P3, QY8R_USP_PRED_NOT_P0,
        QY8R_USP_PRED_NOT_P1
    };
    uint32_t w0 = ins->words[0], w1 = ins->words[1];
    unsigned op = w1 >> 27, op2 = (w1 >> 9) & 3;
    unsigned pred = (w1 >> 24) & 7;
    unsigned repeat = (w1 & USE_W1_REPEAT_ENABLE) ? ((w1 >> 12) & 15) + 1 : 1;
    unsigned i;

    ins->predicate = QY8R_USP_PRED_ALWAYS;
    ins->repeat_count = 1;
    if (ins->has_branch_target) {
        if (op != 31 || pred != 5) {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "branch block does not contain the reviewed !p0 "
                        "branch encoding");
        }
        ins->opcode = QY8R_USP_OP_BR;
        ins->signature = 1;
        ins->predicate = QY8R_USP_PRED_NOT_P0;
        set_kind(&ins->operands[0], QY8R_USP_OPERAND_BRANCH_OFFSET);
        ins->operand_count = 1;
        return 1;
    }
    if (op == 31 && w0 == 0 && w1 == 0xf8000140u) {
        ins->opcode = QY8R_USP_OP_NOP;
        ins->signature = 27;
        return 1;
    }
    if (op == 31 && w1 == 0xfa100000u) {
        if (w0 != 0x01000101u && w0 != 0x00010100u) {
            return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                        "SMLSI increments are outside the reviewed signature");
        }
        ins->opcode = QY8R_USP_OP_SMLSI;
        ins->signature = 35;
        for (i = 0; i < 4; i++) {
            set_immediate(&ins->operands[i], (w0 >> (24 - i * 8)) & 0xff);
            set_kind(&ins->operands[4 + i], QY8R_USP_OPERAND_INCREMENT_MODE);
        }
        for (i = 8; i < 11; i++) {
            set_immediate(&ins->operands[i], 0);
        }
        ins->operand_count = 11;
        return 1;
    }
    if ((op != 0 && op != 3 && op != 5 && op != 6 && op != 7 && op != 8 &&
         op != 9 && op != 15) ||
        pred == 7) {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "unsupported instruction encoding op=%u op2=%u "
                    "word1=%08x",
                    op, op2, w1);
    }
    ins->predicate = predicates[pred];
    if (w1 & USE_W1_SKIPINV) {
        ins->flags |= QY8R_USP_INSN_SKIPINV;
    }
    ins->repeat_count = (uint8_t)repeat;
    if (op == 7) {
        return decode_efo(ins, pred, e, offset);
    }
    if (op == 5) {
        return decode_mov(ins, pred, repeat, e, offset);
    }
    if ((op == 0 && (op2 == 0 || op2 == 2)) || (op == 6 && op2 == 0) ||
        (op == 3 && (op2 == 0 || op2 == 1))) {
        return decode_float(ins, op, op2, pred, repeat, e, offset);
    }
    if (op == 9 || op == 15) {
        return decode_test(ins, pred, repeat, e, offset);
    }
    if (op == 8) {
        return decode_pack(ins, pred, repeat, e, offset);
    }
    return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                "encoding op=%u op2=%u has no reviewed signature", op, op2);
}

/*
 * Expands a repeated instruction into its iterations. The MOE state gives a
 * per-operand register increment; SMLSI replaces that state.
 */
static int expand_repeat(qy8r_usp_instruction *ins, int8_t moe[4],
                         qy8r_usp_error *e, size_t offset)
{
    unsigned rep, arg;

    if (ins->repeat_count < 1 || ins->repeat_count > QY8R_USP_MAX_REPEAT) {
        return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                    "repeat count %u exceeds IR capacity", ins->repeat_count);
    }
    memcpy(ins->moe_before, moe, sizeof(ins->moe_before));
    if (ins->opcode == QY8R_USP_OP_SMLSI) {
        for (arg = 0; arg < 4; arg++) {
            if (ins->operands[arg].immediate > INT8_MAX) {
                return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                            "SMLSI increment %u is outside signed 8-bit IR",
                            arg);
            }
            moe[arg] = (int8_t)ins->operands[arg].immediate;
        }
        memcpy(ins->moe_after, moe, sizeof(ins->moe_after));
        return 1;
    }
    memcpy(ins->moe_after, moe, sizeof(ins->moe_after));
    ins->expanded_count = ins->repeat_count;
    for (rep = 0; rep < ins->repeat_count; rep++) {
        for (arg = 0; arg < ins->operand_count; arg++) {
            qy8r_usp_operand *operand = &ins->expanded[rep][arg];
            int number;

            *operand = ins->operands[arg];
            if (!rep || arg >= 4 ||
                operand->kind != QY8R_USP_OPERAND_REGISTER ||
                operand->bank == QY8R_USP_BANK_C ||
                operand->bank == QY8R_USP_BANK_G) {
                continue;
            }
            number = (int)operand->number + moe[arg] * (int)rep;
            if (operand->bank == QY8R_USP_BANK_I) {
                number = (number % 2 + 2) % 2;
            }
            if (number < 0 || number > UINT16_MAX) {
                return fail(e, QY8R_USP_ERR_INSTRUCTION, offset,
                            "repeat MOE produces invalid register %d", number);
            }
            operand->number = (uint16_t)number;
        }
    }
    return 1;
}

static int read_string(cursor *c, char *dst, size_t cap, qy8r_usp_error *e,
                       const char *field)
{
    size_t start = c->pos, n;
    while (c->pos < c->size && c->data[c->pos]) {
        c->pos++;
    }
    if (c->pos == c->size) {
        return fail(e, QY8R_USP_ERR_TRUNCATED, c->base + start,
                    "%s: missing NUL terminator", field);
    }
    n = c->pos - start;
    c->pos++;
    if (n >= cap) {
        return fail(e, QY8R_USP_ERR_LIMIT, c->base + start,
                    "%s: string length %zu exceeds %zu", field, n, cap - 1);
    }
    memcpy(dst, c->data + start, n);
    dst[n] = 0;
    return 1;
}

static int parse_symbols(cursor *c, qy8r_usp_program *out, unsigned depth,
                         qy8r_usp_error *e)
{
    uint16_t count, i;
    if (depth > 16) {
        return fail(e, QY8R_USP_ERR_LIMIT, c->base + c->pos,
                    "symbol member nesting exceeds 16");
    }
    if (!u16be(c, &count, e, "symbol_count")) {
        return 0;
    }
    if (depth == 0 && count > QY8R_USP_MAX_SYMBOLS) {
        return fail(e, QY8R_USP_ERR_LIMIT, c->base + c->pos - 2,
                    "symbol count %u exceeds limit %u", count,
                    QY8R_USP_MAX_SYMBOLS);
    }
    for (i = 0; i < count; i++) {
        qy8r_usp_symbol tmp;
        qy8r_usp_symbol *s = NULL;
        uint8_t u8v;
        uint16_t u16v;
        memset(&tmp, 0, sizeof(tmp));
        if (depth == 0) {
            s = &out->symbols[out->symbol_count++];
        }
        if (!read_string(c, s ? s->name : tmp.name,
                         s ? sizeof(s->name) : sizeof(tmp.name), e,
                         "symbol.name") ||
            !u16be(c, &u16v, e, "symbol.builtin_id")) {
            return 0;
        }
        if (s) {
            s->builtin_id = u16v;
        }
        if (!u8(c, &u8v, e, "symbol.type")) {
            return 0;
        }
        if (s) {
            s->type = u8v;
        }
        if (!u8(c, &u8v, e, "symbol.qualifier")) {
            return 0;
        }
        if (s) {
            s->qualifier = u8v;
        }
        if (!u8(c, &u8v, e, "symbol.precision")) {
            return 0;
        }
        if (s) {
            s->precision = u8v;
        }
        if (!u8(c, &u8v, e, "symbol.varying_flags")) {
            return 0;
        }
        if (s) {
            s->varying_flags = u8v;
        }
#define READ_SYM16(field)                                                      \
    do {                                                                       \
        if (!u16be(c, &u16v, e, "symbol." #field))                             \
            return 0;                                                          \
        if (s)                                                                 \
            s->field = u16v;                                                   \
    } while (0)
        READ_SYM16(active_array_size);
        READ_SYM16(declared_array_size);
#undef READ_SYM16
        if (!u8(c, &u8v, e, "symbol.register_type")) {
            return 0;
        }
        if (s) {
            s->register_type = u8v;
        }
        if (!u16be(c, &u16v, e, "symbol.base_comp_or_texture_unit")) {
            return 0;
        }
        if (s) {
            s->base_comp_or_texture_unit = u16v;
        }
        if (!u8(c, &u8v, e, "symbol.component_count")) {
            return 0;
        }
        if (s) {
            s->component_count = u8v;
        }
        if (!u16be(c, &u16v, e, "symbol.component_use_mask")) {
            return 0;
        }
        if (s) {
            s->component_use_mask = u16v;
        }
        if (!u16be(c, &u16v, e, "symbol.member_count")) {
            return 0;
        }
        if (u16v > QY8R_USP_MAX_MEMBERS) {
            return fail(e, QY8R_USP_ERR_LIMIT, c->base + c->pos - 2,
                        "symbol has %u members; limit is %u", u16v,
                        QY8R_USP_MAX_MEMBERS);
        }
        if (s) {
            s->member_count = (uint8_t)u16v;
        }
        /*
         * Recursively consume nested metadata. The current public manifest
         * keeps only top-level symbols; consumers can still fail closed on
         * aggregates until an aggregate binding is explicitly required.
         */
        if (u16v) {
            uint16_t member_count = u16v;
            size_t count_pos = c->pos;
            if (!parse_symbols(c, out, depth + 1, e)) {
                return 0;
            }
            if (((uint16_t)c->data[count_pos] << 8 | c->data[count_pos + 1]) !=
                member_count) {
                return fail(e, QY8R_USP_ERR_PROFILE, c->base + count_pos,
                            "member count changed while parsing");
            }
        }
    }
    return 1;
}

static int validate_usp_header(const uint8_t *data, size_t size,
                               qy8r_usp_program *out, qy8r_usp_error *e,
                               size_t absolute)
{
    cursor c = { data, size, 0, absolute };
    uint32_t magic, version, declared;
    if (!u32le(&c, &magic, e, "usp.magic") ||
        !u32le(&c, &version, e, "usp.version") ||
        !u32le(&c, &declared, e, "usp.size")) {
        return 0;
    }
    if (magic != 0x55535020u) {
        return fail(e, QY8R_USP_ERR_PROFILE, absolute, "USP magic is 0x%08x",
                    magic);
    }
    if (declared != size - 12) {
        return fail(e, QY8R_USP_ERR_PROFILE, absolute + 8,
                    "USP declares %u payload bytes, received %zu", declared,
                    size - 12);
    }
    if (version != 35u) {
        return fail(e, QY8R_USP_ERR_PROFILE, absolute + 4,
                    "unsupported USP version %u", version);
    }
    out->variants[0].usp_version = (uint16_t)version;
    return 1;
}

static int skip_bytes(cursor *c, size_t n, qy8r_usp_error *e, const char *field)
{
    const uint8_t *p;
    return take(c, n, &p, e, field);
}

static int skip_moe(cursor *c, qy8r_usp_error *e)
{
    return skip_bytes(c, 18, e, "USP MOE state");
}

static int append_raw_instruction(cursor *c, qy8r_usp_variant *v,
                                  uint8_t domain, uint16_t block_offset,
                                  uint32_t *pc, uint16_t branch_label,
                                  int has_branch, int decode_now, int8_t moe[4],
                                  qy8r_usp_error *e)
{
    qy8r_usp_instruction *ins;
    qy8r_usp_node *node;
    if (v->instruction_count >= QY8R_USP_MAX_INSTRUCTIONS ||
        v->node_count >= QY8R_USP_MAX_NODES) {
        return fail(e, QY8R_USP_ERR_LIMIT, c->base + c->pos,
                    "instruction or IR node capacity exceeded");
    }
    ins = &v->instructions[v->instruction_count];
    memset(ins, 0, sizeof(*ins));
    ins->pc = *pc;
    ins->execution_domain = domain;
    ins->source_block_offset = block_offset;
    ins->branch_target_label = branch_label;
    ins->has_branch_target = (uint8_t)has_branch;
    if (!u32le(c, &ins->words[0], e, "instruction.word0") ||
        !u32le(c, &ins->words[1], e, "instruction.word1")) {
        return 0;
    }
    if (decode_now && (!decode_instruction(ins, e, c->base + c->pos - 8) ||
                       !expand_repeat(ins, moe, e, c->base + c->pos - 8))) {
        return 0;
    }
    node = &v->nodes[v->node_count++];
    node->kind = QY8R_USP_NODE_INSTRUCTION;
    node->index = v->instruction_count++;
    *pc += 8;
    return 1;
}

static qy8r_usp_node *append_node(qy8r_usp_variant *v, qy8r_usp_node_kind kind,
                                  qy8r_usp_error *e, size_t offset)
{
    qy8r_usp_node *node;
    if (v->node_count >= QY8R_USP_MAX_NODES) {
        fail(e, QY8R_USP_ERR_LIMIT, offset, "IR node capacity exceeded");
        return NULL;
    }
    node = &v->nodes[v->node_count++];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    return node;
}

static int validate_usp_blocks(const uint8_t *data, size_t size,
                               size_t absolute, qy8r_usp_program *out,
                               qy8r_usp_variant *variant,
                               int translate_instructions, qy8r_usp_error *e)
{
    cursor c = { data, size, 12, absolute };
    uint32_t pc = 0;
    int8_t moe[4] = { 1, 1, 1, 1 };
    int ended = 0;
    while (c.pos < c.size) {
        size_t block_at = c.pos;
        uint32_t type;
        if (!u32le(&c, &type, e, "block.type")) {
            return 0;
        }
        if (type == 7) {
            if (c.pos != c.size) {
                return fail(
                    e, QY8R_USP_ERR_BLOCK, absolute + block_at,
                    "target-1.7 END block is ambiguous before exact EOF");
            }
            if (!append_node(variant, QY8R_USP_NODE_END, e,
                             absolute + block_at)) {
                return 0;
            }
            ended = 1;
            break;
        }
        switch (type) {
        case 1: { /* PROGDESC */
            static const uint8_t fixed_widths[] = { 4, 4, 4, 4, 4, 4, 4, 4, 2,
                                                    2, 2, 2, 4, 2, 2, 2, 2, 2,
                                                    2, 2, 4, 2, 2, 4, 2, 4, 4,
                                                    4, 2, 2, 2, 2, 2, 2, 2, 2,
                                                    2, 2, 2, 2, 2, 2, 2, 2 };
            uint32_t fields[64] = { 0 };
            size_t i;
            for (i = 0; i < sizeof(fixed_widths); i++) {
                uint32_t v = 0;
                if (fixed_widths[i] == 4) {
                    if (!u32le(&c, &v, e, "PROGDESC field")) {
                        return 0;
                    }
                } else {
                    uint16_t v16;
                    if (!u16le(&c, &v16, e, "PROGDESC field")) {
                        return 0;
                    }
                    v = v16;
                }
                fields[i] = v;
            }
            /* indices follow the target-1.7 USP_PC_PROGDESC layout */
            {
                uint32_t brns = fields[6], psinputs = fields[21];
                uint32_t memconst = fields[23], regconst = fields[22];
                uint32_t texstate = fields[24], saupdate = fields[40];
                uint32_t vsusage = fields[42], outputs = fields[43];
                uint32_t shader_type = fields[0];
                if (brns > 256 || psinputs > 32 || memconst > 4096 ||
                    regconst > QY8R_USP_MAX_REGCONST || texstate > 4096 ||
                    saupdate > QY8R_USP_MAX_INSTRUCTIONS || vsusage > 256 ||
                    outputs > 256) {
                    return fail(e, QY8R_USP_ERR_LIMIT, absolute + block_at,
                                "PROGDESC count exceeds parser limits");
                }
                if (!skip_bytes(&c, (size_t)brns * 4, e, "PROGDESC BRN list")) {
                    return 0;
                }
                if (psinputs >
                    sizeof(((qy8r_usp_program *)0)->ps_inputs) /
                        sizeof(((qy8r_usp_program *)0)->ps_inputs[0])) {
                    return fail(e, QY8R_USP_ERR_LIMIT, absolute + block_at,
                                "PS input count exceeds public IR capacity");
                }
                out->ps_input_count = (uint16_t)psinputs;
                for (i = 0; i < psinputs; i++) {
                    if (!u16le(&c, &out->ps_inputs[i].flags, e,
                               "PROGDESC PS input flags") ||
                        !u16le(&c, &out->ps_inputs[i].texture, e,
                               "PROGDESC PS input texture") ||
                        !u16le(&c, &out->ps_inputs[i].coord, e,
                               "PROGDESC PS input coord") ||
                        !u16le(&c, &out->ps_inputs[i].coord_dim, e,
                               "PROGDESC PS input coord_dim") ||
                        !u16le(&c, &out->ps_inputs[i].format, e,
                               "PROGDESC PS input format") ||
                        !u16le(&c, &out->ps_inputs[i].data_size, e,
                               "PROGDESC PS input data_size")) {
                        return 0;
                    }
                }
                out->regconst_base_register = (uint16_t)fields[13];
                out->regconst_max_count = (uint16_t)fields[14];
                out->result_register_type = (uint16_t)fields[29];
                out->result_register_count = (uint16_t)fields[32];
                out->result_temp_register = (uint16_t)fields[33];
                out->result_pa_register = (uint16_t)fields[34];
                out->result_output_register = (uint16_t)fields[35];
                out->result_register_format = (uint16_t)fields[31];
                out->phase0_end_label = (uint16_t)fields[37];
                out->phase1_start_label = (uint16_t)fields[38];
                if ((fields[37] == UINT16_MAX) != (fields[38] == UINT16_MAX)) {
                    return fail(e, QY8R_USP_ERR_PROFILE, absolute + block_at,
                                "incomplete PROGDESC phase boundary");
                }
                if (fields[37] != UINT16_MAX) {
                    qy8r_usp_node *phase =
                        append_node(variant, QY8R_USP_NODE_PHASE_DISCARD, e,
                                    absolute + block_at);
                    if (!phase) {
                        return 0;
                    }
                    phase->phase0_end_label = (uint16_t)fields[37];
                    phase->phase1_start_label = (uint16_t)fields[38];
                    phase->phase_predicate = QY8R_USP_PRED_NOT_P1;
                }
                for (i = 0; i < memconst; i++) {
                    uint16_t fmt;
                    if (!u16le(&c, &fmt, e, "constant load format")) {
                        return 0;
                    }
                    if (fmt == 5) {
                        if (!skip_bytes(&c, 4, e, "constant static value")) {
                            return 0;
                        }
                    } else {
                        if (!skip_bytes(&c, 2, e, "constant source index")) {
                            return 0;
                        }
                        if (fmt == 3 &&
                            !skip_bytes(&c, 2, e, "constant source shift")) {
                            return 0;
                        }
                    }
                    if (!skip_bytes(&c, 4, e, "constant destination")) {
                        return 0;
                    }
                }
                out->regconst_count = (uint16_t)regconst;
                for (i = 0; i < regconst; i++) {
                    qy8r_usp_regconst *rc = &out->regconsts[i];
                    uint16_t fmt, source = 0, source_shift = 0, dst, dst_shift;
                    uint32_t literal_bits;
                    rc->uniform_symbol = -1;
                    if (!u16le(&c, &fmt, e, "regconst.format")) {
                        return 0;
                    }
                    rc->format = fmt;
                    if (fmt == 5) {
                        if (!u32le(&c, &literal_bits, e,
                                   "regconst.static_value")) {
                            return 0;
                        }
                        memcpy(&rc->literal, &literal_bits,
                               sizeof(literal_bits));
                        rc->has_literal = 1;
                    } else {
                        if (!u16le(&c, &source, e, "regconst.source_index")) {
                            return 0;
                        }
                        if (fmt == 3 && !u16le(&c, &source_shift, e,
                                               "regconst.source_shift")) {
                            return 0;
                        }
                        rc->source_constant_index = source;
                        rc->source_halfword_shift = source_shift;
                    }
                    if (!u16le(&c, &dst, e, "regconst.destination_index") ||
                        !u16le(&c, &dst_shift, e,
                               "regconst.destination_shift")) {
                        return 0;
                    }
                    if ((uint32_t)fields[13] + dst > UINT16_MAX) {
                        return fail(e, QY8R_USP_ERR_LIMIT, absolute + c.pos,
                                    "regconst SA register overflows 16 bits");
                    }
                    rc->destination_sa_register = (uint16_t)(fields[13] + dst);
                    rc->destination_halfword_shift = dst_shift;
                }
                if (!skip_bytes(&c, (size_t)texstate * 6, e,
                                "PROGDESC texture state")) {
                    return 0;
                }
                for (i = 0; i < saupdate; i++) {
                    if (!append_raw_instruction(
                            &c, variant, 1, (uint16_t)block_at, &pc, 0, 0,
                            translate_instructions, moe, e)) {
                        return 0;
                    }
                }
                if (shader_type == 1 &&
                    !skip_bytes(&c, ((size_t)vsusage + 31) / 32 * 4, e,
                                "PROGDESC VS input usage")) {
                    return 0;
                }
                if (!skip_bytes(&c, ((size_t)outputs + 31) / 32 * 4, e,
                                "PROGDESC output mask")) {
                    return 0;
                }
            }
            break;
        }
        case 2: { /* HWCODE */
            uint16_t count;
            if (!u16le(&c, &count, e, "HWCODE instruction count") ||
                !skip_moe(&c, e)) {
                return 0;
            }
            if (count > QY8R_USP_MAX_INSTRUCTIONS) {
                return fail(e, QY8R_USP_ERR_LIMIT, absolute + block_at,
                            "HWCODE instruction count %u exceeds limit", count);
            }
            {
                uint16_t i;
                if (!skip_bytes(&c, (size_t)count * 2, e,
                                "HWCODE descriptors")) {
                    return 0;
                }
                for (i = 0; i < count; i++) {
                    if (!append_raw_instruction(
                            &c, variant, 0, (uint16_t)block_at, &pc, 0, 0,
                            translate_instructions, moe, e)) {
                        return 0;
                    }
                }
            }
            break;
        }
        case 3: { /* BRANCH */
            uint16_t target;
            if (!append_raw_instruction(&c, variant, 0, (uint16_t)block_at, &pc,
                                        0, 1, 0, moe, e) ||
                !u16le(&c, &target, e, "BRANCH destination label")) {
                return 0;
            }
            variant->instructions[variant->instruction_count - 1]
                .branch_target_label = target;
            variant->instructions[variant->instruction_count - 1]
                .has_branch_target = 1;
            if (translate_instructions &&
                (!decode_instruction(
                     &variant->instructions[variant->instruction_count - 1], e,
                     c.base + c.pos - 10) ||
                 !expand_repeat(
                     &variant->instructions[variant->instruction_count - 1],
                     moe, e, c.base + c.pos - 10))) {
                return 0;
            }
            break;
        }
        case 4: { /* LABEL */
            uint16_t label;
            qy8r_usp_node *node;
            if (!u16le(&c, &label, e, "LABEL id")) {
                return 0;
            }
            node = append_node(variant, QY8R_USP_NODE_LABEL, e,
                               absolute + block_at);
            if (!node) {
                return 0;
            }
            node->label = label;
            break;
        }
        case 5: { /* SAMPLE */
            uint16_t flags;
            uint32_t sample_id;
            uint16_t desc, values[11];
            qy8r_usp_sample *sample;
            qy8r_usp_node *node;
            unsigned i;
            if (variant->sample_count >= QY8R_USP_MAX_NODES) {
                return fail(e, QY8R_USP_ERR_LIMIT, absolute + block_at,
                            "sample descriptor capacity exceeded");
            }
            if (!u16le(&c, &flags, e, "SAMPLE flags") || !skip_moe(&c, e) ||
                !u32le(&c, &sample_id, e, "SAMPLE id") ||
                !u16le(&c, &desc, e, "SAMPLE instruction descriptor")) {
                return 0;
            }
            for (i = 0; i < 11; i++) {
                if (!u16le(&c, &values[i], e, "SAMPLE field")) {
                    return 0;
                }
            }
            sample = &variant->samples[variant->sample_count];
            memset(sample, 0, sizeof(*sample));
            sample->kind = 5;
            sample->flags = flags;
            sample->texture_index = values[0];
            sample->swizzle = values[1];
            sample->base_dest_type = values[2];
            sample->base_dest_num = values[3];
            sample->direct_dest_type = values[4];
            sample->direct_dest_num = values[5];
            sample->sample_temp_type = values[6];
            sample->sample_temp_num = values[7];
            sample->i_regs_live = values[8];
            sample->c10_i_regs_live = values[9];
            sample->texture_precision = values[10];
            if (flags & 1) {
                uint16_t coord, tex_dim;
                if (!u16le(&c, &coord, e, "SAMPLE coordinate") ||
                    !u16le(&c, &tex_dim, e, "SAMPLE texture dimension") ||
                    !u16le(&c, &sample->source_type, e,
                           "SAMPLE direct source type") ||
                    !u16le(&c, &sample->source_num, e,
                           "SAMPLE direct source number")) {
                    return 0;
                }
                (void)coord;
                (void)tex_dim;
            } else {
                sample->has_words = 1;
                if (!u32le(&c, &sample->words[0], e, "SAMPLE word0") ||
                    !u32le(&c, &sample->words[1], e, "SAMPLE word1")) {
                    return 0;
                }
            }
            node = append_node(variant, QY8R_USP_NODE_SAMPLE, e,
                               absolute + block_at);
            if (!node) {
                return 0;
            }
            node->index = variant->sample_count++;
            (void)sample_id;
            (void)desc;
            break;
        }
        case 6: { /* SAMPLEUNPACK */
            uint32_t sample_id;
            uint16_t desc, val;
            qy8r_usp_sample *sample;
            qy8r_usp_node *node;
            unsigned i;
            if (variant->sample_count >= QY8R_USP_MAX_NODES) {
                return fail(e, QY8R_USP_ERR_LIMIT, absolute + block_at,
                            "sample descriptor capacity exceeded");
            }
            if (!skip_moe(&c, e) ||
                !u32le(&c, &sample_id, e, "SAMPLEUNPACK id") ||
                !u16le(&c, &desc, e, "SAMPLEUNPACK instruction descriptor") ||
                !u16le(&c, &val, e, "SAMPLEUNPACK mask")) {
                return 0;
            }
            sample = &variant->samples[variant->sample_count];
            memset(sample, 0, sizeof(*sample));
            sample->kind = 6;
            sample->u_mask = val;
            if (!u16le(&c, &sample->u_live, e, "SAMPLEUNPACK live")) {
                return 0;
            }
            for (i = 0; i < 4; i++) {
                if (!u16le(&c, &sample->dest_type[i], e,
                           "SAMPLEUNPACK dest type")) {
                    return 0;
                }
            }
            for (i = 0; i < 4; i++) {
                if (!u16le(&c, &sample->dest_num[i], e,
                           "SAMPLEUNPACK dest num")) {
                    return 0;
                }
            }
            for (i = 0; i < 4; i++) {
                if (!u16le(&c, &sample->dest_format[i], e,
                           "SAMPLEUNPACK dest format")) {
                    return 0;
                }
            }
            for (i = 0; i < 4; i++) {
                if (!u16le(&c, &sample->dest_component[i], e,
                           "SAMPLEUNPACK dest component")) {
                    return 0;
                }
            }
            if (!u16le(&c, &sample->source_type, e,
                       "SAMPLEUNPACK source type") ||
                !u16le(&c, &sample->source_num, e, "SAMPLEUNPACK source num") ||
                !u16le(&c, &sample->base_dest_type, e,
                       "SAMPLEUNPACK base source type") ||
                !u16le(&c, &sample->base_dest_num, e,
                       "SAMPLEUNPACK base source num") ||
                !u16le(&c, &sample->sample_temp_type, e,
                       "SAMPLEUNPACK temp type") ||
                !u16le(&c, &sample->sample_temp_num, e,
                       "SAMPLEUNPACK temp num") ||
                !u16le(&c, &sample->i_regs_live, e,
                       "SAMPLEUNPACK i-reg live")) {
                return 0;
            }
            node = append_node(variant, QY8R_USP_NODE_SAMPLE, e,
                               absolute + block_at);
            if (!node) {
                return 0;
            }
            node->index = variant->sample_count++;
            (void)sample_id;
            (void)desc;
            break;
        }
        case 7: /* handled as target-1.7 END above */
            break;
        case 8:
            return fail(e, QY8R_USP_ERR_BLOCK, absolute + block_at,
                        "block type 8 is outside target-1.7 profile");
        default:
            return fail(e, QY8R_USP_ERR_BLOCK, absolute + block_at,
                        "unknown target-1.7 USP block type %u", type);
        }
    }
    if (!ended) {
        return fail(e, QY8R_USP_ERR_BLOCK, absolute + c.pos,
                    "target-1.7 USP missing END block at exact EOF");
    }
    return 1;
}

static unsigned popcount16(uint16_t value)
{
    unsigned count = 0;
    while (value) {
        value &= (uint16_t)(value - 1);
        count++;
    }
    return count;
}

static int ends_with(const char *value, const char *suffix)
{
    size_t value_len = strlen(value), suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

static int build_bindings(qy8r_usp_program *program, qy8r_usp_error *e)
{
    qy8r_usp_bindings *b = &program->bindings;
    uint16_t i;
    for (i = 0; i < program->symbol_count; i++) {
        const qy8r_usp_symbol *s = &program->symbols[i];
        if (s->qualifier == 4) {
            b->attributes[b->attribute_count++] = i;
        }
        if (s->qualifier == 3 && s->type == 24) {
            b->samplers[b->sampler_count++] = i;
        } else if (s->qualifier == 3) {
            b->uniforms[b->uniform_count++] = i;
        }
        if ((s->qualifier == 5 || s->qualifier == 6) &&
            strncmp(s->name, "gl_", 3) != 0) {
            b->varyings[b->varying_count++] = i;
        }
    }
    for (i = 0; i < program->regconst_count; i++) {
        qy8r_usp_regconst *rc = &program->regconsts[i];
        uint16_t j, match = UINT16_MAX;
        if (rc->has_literal) {
            continue;
        }
        for (j = 0; j < b->uniform_count; j++) {
            uint16_t symbol_index = b->uniforms[j];
            const qy8r_usp_symbol *s = &program->symbols[symbol_index];
            uint32_t first = s->base_comp_or_texture_unit;
            uint32_t end = first + s->component_count;
            if (rc->source_constant_index >= first &&
                rc->source_constant_index < end) {
                if (match != UINT16_MAX) {
                    return fail(e, QY8R_USP_ERR_BINDING, i,
                                "RegConst source %u overlaps multiple uniforms",
                                rc->source_constant_index);
                }
                match = symbol_index;
            }
        }
        if (match != UINT16_MAX) {
            const qy8r_usp_symbol *s = &program->symbols[match];
            rc->uniform_symbol = (int16_t)match;
            rc->uniform_component = (uint16_t)(rc->source_constant_index -
                                               s->base_comp_or_texture_unit);
        } else if (rc->source_constant_index < program->constant_count) {
            rc->literal = program->constants[rc->source_constant_index];
            rc->has_literal = 1;
        } else {
            return fail(
                e, QY8R_USP_ERR_BINDING, i,
                "RegConst source %u has no uniform or constant-data value",
                rc->source_constant_index);
        }
    }
    if (b->attribute_count) {
        uint16_t v;
        b->position_output_base = 0;
        b->position_output_components = 4;
        for (v = 0; v < b->varying_count; v++) {
            uint16_t varying_index = b->varyings[v];
            const qy8r_usp_symbol *varying = &program->symbols[varying_index];
            const char *suffix = strncmp(varying->name, "v_", 2) == 0
                                     ? varying->name + 2
                                     : varying->name;
            uint16_t attr_index = UINT16_MAX;
            unsigned a, used;
            for (a = 0; a < b->attribute_count; a++) {
                uint16_t candidate = b->attributes[a];
                if (ends_with(program->symbols[candidate].name, suffix)) {
                    attr_index = candidate;
                    break;
                }
            }
            if (attr_index == UINT16_MAX) {
                return fail(e, QY8R_USP_ERR_BINDING, v,
                            "varying %s has no matching attribute",
                            varying->name);
            }
            {
                const qy8r_usp_symbol *attr = &program->symbols[attr_index];
                uint16_t source_base = attr->base_comp_or_texture_unit;
                used = popcount16(attr->component_use_mask);
                if (!used) {
                    used = attr->component_count;
                }
                for (i = 0; i < program->variants[0].instruction_count; i++) {
                    const qy8r_usp_instruction *ins =
                        &program->variants[0].instructions[i];
                    unsigned repetition;
                    if (ins->opcode != QY8R_USP_OP_MOV) {
                        continue;
                    }
                    for (repetition = 0; repetition < ins->expanded_count;
                         repetition++) {
                        uint16_t output, source;
                        const qy8r_usp_operand *dst =
                            &ins->expanded[repetition][0];
                        const qy8r_usp_operand *src =
                            &ins->expanded[repetition][1];
                        output = dst->number;
                        source = src->number;
                        if (dst->kind == QY8R_USP_OPERAND_REGISTER &&
                            dst->bank == QY8R_USP_BANK_O &&
                            src->kind == QY8R_USP_OPERAND_REGISTER &&
                            src->bank == QY8R_USP_BANK_PA &&
                            source >= source_base &&
                            source < source_base + used) {
                            qy8r_usp_output_component *m;
                            if (b->output_component_count >=
                                QY8R_USP_MAX_SYMBOLS * 4) {
                                return fail(
                                    e, QY8R_USP_ERR_LIMIT, i,
                                    "vertex output mapping capacity exceeded");
                            }
                            m = &b->output_components
                                     [b->output_component_count++];
                            m->symbol_index = varying_index;
                            m->component = (uint16_t)(source - source_base);
                            m->output_register = output;
                        }
                    }
                }
                {
                    unsigned mapped = 0;
                    uint16_t j;
                    for (j = 0; j < b->output_component_count; j++) {
                        if (b->output_components[j].symbol_index ==
                            varying_index) {
                            mapped++;
                        }
                    }
                    if (mapped != used) {
                        return fail(
                            e, QY8R_USP_ERR_BINDING, v,
                            "varying %s maps %u of %u attribute components",
                            varying->name, mapped, used);
                    }
                }
            }
        }
    } else {
        int found = 0;
        for (i = 0; i < program->symbol_count; i++) {
            if (strcmp(program->symbols[i].name, "gl_FragColor") == 0) {
                found = 1;
            }
        }
        if (!found) {
            return fail(e, QY8R_USP_ERR_BINDING, 0,
                        "fragment result symbol gl_FragColor is missing");
        }
        b->fragment_result_pa = program->result_pa_register;
    }
    return 1;
}

/*
 * Remaining block parsing and instruction decoding are kept in this unit so
 * the public IR can be built without QEMU, EGL, GLES, or GLib.
 */
static int qy8r_usp_parse_impl(const uint8_t *bytes, size_t length,
                               int request_msaa_trans, qy8r_usp_program *out,
                               qy8r_usp_error *error)
{
    cursor c;
    uint32_t hash;
    size_t revision_start = 8, selected_body = 0, selected_size = 0;
    size_t selected_usp = 0, selected_usp_size = 0;
    size_t selected_trans = 0, selected_trans_size = 0;
    unsigned revisions = 0;
    int selected = 0;
    if (error) {
        memset(error, 0, sizeof(*error));
    }
    if (!bytes || !out) {
        return fail(error, QY8R_USP_ERR_ARGUMENT, 0,
                    "input and output are required");
    }
    if (request_msaa_trans != 0 && request_msaa_trans != 1) {
        return fail(error, QY8R_USP_ERR_ARGUMENT, 0,
                    "request_msaa_trans must be 0 or 1");
    }
    if (length < 8) {
        return fail(error, QY8R_USP_ERR_TRUNCATED, length,
                    "SGXBS header needs 8 bytes, received %zu", length);
    }
    memset(out, 0, sizeof(*out));
    if (memcmp(bytes, "\x38\xb4\xfa\x10", 4) != 0) {
        return fail(error, QY8R_USP_ERR_CONTAINER, 0, "invalid SGXBS magic");
    }
    c = (cursor){ bytes, length, 4, 0 };
    if (!u32be(&c, &hash, error, "sgxbs.hash")) {
        return 0;
    }
    out->container_hash = hash;
    out->computed_container_hash = sgxbs_hash(bytes + 8, length - 8);
    if (out->container_hash != out->computed_container_hash) {
        return fail(error, QY8R_USP_ERR_CONTAINER, 4,
                    "SGXBS hash mismatch: header %08x computed %08x",
                    out->container_hash, out->computed_container_hash);
    }
    c.pos = revision_start;
    while (c.pos < c.size) {
        size_t body_start, body_end, usp_pos, usp_len;
        size_t trans_pos = 0;
        uint16_t sw, core, rev, reserved;
        uint32_t project, glsl, uspver, body_size, ptype;
        cursor r;
        if (c.size - c.pos < 24) {
            return fail(error, QY8R_USP_ERR_TRUNCATED, c.pos,
                        "revision header needs 24 bytes, have %zu",
                        c.size - c.pos);
        }
        if (!u16be(&c, &sw, error, "revision.software") ||
            !u16be(&c, &core, error, "revision.core") ||
            !u16be(&c, &rev, error, "revision.core_revision") ||
            !u16be(&c, &reserved, error, "revision.reserved") ||
            !u32be(&c, &project, error, "revision.project_hash") ||
            !u32be(&c, &glsl, error, "revision.glsl_interface") ||
            !u32be(&c, &uspver, error, "revision.usp_version") ||
            !u32be(&c, &body_size, error, "revision.body_size")) {
            return 0;
        }
        if (reserved != 0) {
            return fail(error, QY8R_USP_ERR_REVISION, c.pos - 20,
                        "revision reserved field is nonzero");
        }
        if (body_size > c.size - c.pos) {
            return fail(error, QY8R_USP_ERR_TRUNCATED, c.pos,
                        "revision body declares %u bytes, have %zu", body_size,
                        c.size - c.pos);
        }
        body_start = c.pos;
        body_end = body_start + body_size;
        r = (cursor){ bytes + body_start, body_size, 0, body_start };
        {
            uint32_t flags, reserved_body, varying;
            uint8_t dims[10], prec[10];
            uint32_t main_size, trans_size = 0;
            const uint8_t *payload;
            unsigned i;
            if (!u32be(&r, &ptype, error, "program.type") ||
                !u32be(&r, &flags, error, "program.flags") ||
                !u32be(&r, &reserved_body, error, "program.reserved") ||
                !u32be(&r, &varying, error, "program.active_varying_mask")) {
                return 0;
            }
            if (reserved_body != 0) {
                return fail(error, QY8R_USP_ERR_REVISION, body_start + 8,
                            "program reserved field is nonzero");
            }
            for (i = 0; i < 10; i++) {
                if (!u8(&r, &dims[i], error, "program.texcoord_dimension")) {
                    return 0;
                }
            }
            for (i = 0; i < 10; i++) {
                if (!u8(&r, &prec[i], error, "program.texcoord_precision")) {
                    return 0;
                }
            }
            if (!u32be(&r, &main_size, error, "program.main_usp_size")) {
                return 0;
            }
            usp_pos = body_start + r.pos;
            usp_len = main_size;
            if (!take(&r, main_size, &payload, error, "program.main_usp")) {
                return 0;
            }
            if (ptype == 1) {
                if (!u32be(&r, &trans_size, error, "program.trans_usp_size")) {
                    return 0;
                }
                trans_pos = body_start + r.pos;
                if (!take(&r, trans_size, &payload, error,
                          "program.trans_usp")) {
                    return 0;
                }
            } else if (ptype != 0) {
                return fail(error, QY8R_USP_ERR_REVISION, body_start,
                            "unsupported SGXBS program type %u", ptype);
            }
            if (core == 0x0540 && rev == 0x0120) {
                if (selected) {
                    return fail(error, QY8R_USP_ERR_REVISION, body_start,
                                "duplicate SGX540 r1.2 revisions");
                }
                selected = 1;
                selected_body = body_start;
                selected_size = body_size;
                selected_usp = usp_pos;
                selected_usp_size = usp_len;
                selected_trans = trans_pos;
                selected_trans_size = trans_size;
                out->software_version = sw;
                out->core_id = core;
                out->core_revision = rev;
                out->project_hash = project;
                out->glsl_interface = glsl;
                out->usp_pc_shader_version = uspver;
                out->stage = ptype == 0 ? 0 : 1;
                out->program_flags = flags;
                out->active_varying_mask = varying;
                memcpy(out->texcoord_dimensions, dims, 10);
                memcpy(out->texcoord_precisions, prec, 10);
                {
                    uint16_t constants;
                    if (!u16be(&r, &constants, error,
                               "program.constant_count")) {
                        return 0;
                    }
                    if (constants > QY8R_USP_MAX_CONSTANTS) {
                        return fail(error, QY8R_USP_ERR_LIMIT,
                                    body_start + r.pos - 2,
                                    "constant count %u exceeds limit %u",
                                    constants, QY8R_USP_MAX_CONSTANTS);
                    }
                    out->constant_count = constants;
                    for (i = 0; i < constants; i++) {
                        if (!f32be(&r, &out->constants[i], error,
                                   "program.constant")) {
                            return 0;
                        }
                    }
                    if (!parse_symbols(&r, out, 0, error)) {
                        return 0;
                    }
                    if (r.pos != r.size) {
                        return fail(
                            error, QY8R_USP_ERR_REVISION, body_start + r.pos,
                            "revision has %zu unexplained trailing bytes",
                            r.size - r.pos);
                    }
                }
            }
            /*
             * Remaining body fields are parsed in the next stage; reject
             * unexplained trailing bytes instead of accepting a prefix.
             */
        }
        c.pos = body_end;
        revisions++;
    }
    if (!revisions || !selected) {
        return fail(error, QY8R_USP_ERR_REVISION, 8,
                    "SGX540 r1.2 revision not present");
    }
    (void)selected_body;
    (void)selected_size;
    if (!validate_usp_header(bytes + selected_usp, selected_usp_size, out,
                             error, selected_usp)) {
        return 0;
    }
    out->variant_count = selected_trans_size ? 2 : 1;
    out->variants[0].valid = 1;
    out->variants[0].is_translucent_variant = 0;
    out->variants[0].translated = 1;
    if (!validate_usp_blocks(bytes + selected_usp, selected_usp_size,
                             selected_usp, out, &out->variants[0], 1, error)) {
        return 0;
    }
    if (!build_bindings(out, error)) {
        return 0;
    }
    if (selected_trans_size) {
        qy8r_usp_program *trans_scratch = calloc(1, sizeof(*trans_scratch));
        int structurally_valid;
        if (!trans_scratch) {
            return fail(
                error, QY8R_USP_ERR_LIMIT, selected_trans,
                "unable to allocate MSAA-trans structural validation state");
        }
        if (!validate_usp_header(bytes + selected_trans, selected_trans_size,
                                 trans_scratch, error, selected_trans)) {
            free(trans_scratch);
            return 0;
        }
        structurally_valid = validate_usp_blocks(
            bytes + selected_trans, selected_trans_size, selected_trans,
            trans_scratch, &trans_scratch->variants[0], 0, error);
        free(trans_scratch);
        if (!structurally_valid) {
            return 0;
        }
        out->trans_variant_present = 1;
        out->variants[1].valid = 1;
        out->variants[1].is_translucent_variant = 1;
        out->variants[1].translated = 0;
    }
    if (request_msaa_trans) {
        return fail(error, QY8R_USP_ERR_INSTRUCTION, selected_trans,
                    "MSAA-trans variant not supported");
    }
    return 1;
}

/*
 * Text rendering of the typed IR. This is the only place where instruction
 * text exists; it reproduces the listing syntax used by the review tooling.
 */

typedef struct dump_buffer {
    char *data;
    size_t size;
    size_t length;
    int failed;
} dump_buffer;

static void dump_append(dump_buffer *b, const char *format, ...)
{
    va_list ap;
    int n;

    if (b->failed || b->length >= b->size) {
        b->failed = 1;
        return;
    }
    va_start(ap, format);
    n = vsnprintf(b->data + b->length, b->size - b->length, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->size - b->length) {
        b->failed = 1;
        return;
    }
    b->length += (size_t)n;
}

const char *qy8r_usp_opcode_name(unsigned opcode)
{
    static const char *const names[] = {
        "invalid",   "br",          "nop",      "smlsi",   "efo",
        "mov",       "movc",        "fmad",     "fmad16",  "fmsa",
        "fadd",      "fsub",        "fmin",     "fmax",    "shl",
        "unpcku8u8", "unpckf32f16", "pcku8f16", "pcku8f32"
    };

    return opcode < sizeof(names) / sizeof(names[0]) ? names[opcode]
                                                     : "invalid";
}

const char *qy8r_usp_predicate_name(unsigned predicate)
{
    static const char *const names[] = { "always", "p0",  "p1", "p2",
                                         "p3",     "!p0", "!p1" };

    return predicate < sizeof(names) / sizeof(names[0]) ? names[predicate]
                                                        : "invalid";
}

static const char *bank_name(unsigned bank)
{
    static const char *const names[] = { "?",  "r", "o", "pa",
                                         "sa", "c", "g", "i" };

    return bank < sizeof(names) / sizeof(names[0]) ? names[bank] : "?";
}

static void dump_source_modifier(dump_buffer *b, unsigned flags)
{
    if (flags & QY8R_USP_OPERAND_ABSOLUTE) {
        dump_append(b, ".abs");
    }
    if (flags & QY8R_USP_OPERAND_NEGATE) {
        dump_append(b, ".neg");
    }
}

static void dump_operand(dump_buffer *b, const qy8r_usp_instruction *ins,
                         const qy8r_usp_operand *operand)
{
    switch (operand->kind) {
    case QY8R_USP_OPERAND_REGISTER:
        dump_append(b, "%s%u", bank_name(operand->bank), operand->number);
        if (operand->flags & QY8R_USP_OPERAND_FLT16) {
            dump_source_modifier(b, operand->flags);
            dump_append(b, ".flt16");
            /* TEST listings leave the low lane implicit. */
            if (ins->test == QY8R_USP_TEST_NONE || operand->component) {
                dump_append(b, ".%u", operand->component);
            }
            break;
        }
        if (operand->flags & QY8R_USP_OPERAND_HAS_BYTEMASK) {
            dump_append(b, ".bytemask%u%u%u%u", (operand->bytemask >> 3) & 1,
                        (operand->bytemask >> 2) & 1,
                        (operand->bytemask >> 1) & 1, operand->bytemask & 1);
        }
        if (operand->flags & QY8R_USP_OPERAND_HAS_COMPONENT) {
            dump_append(b, ".%u", operand->component);
        }
        dump_source_modifier(b, operand->flags);
        break;
    case QY8R_USP_OPERAND_IMMEDIATE:
        dump_append(b, "#%u", operand->immediate);
        if (operand->flags & QY8R_USP_OPERAND_HAS_COMPONENT) {
            dump_append(b, ".%u", operand->component);
        }
        dump_source_modifier(b, operand->flags);
        break;
    case QY8R_USP_OPERAND_BRANCH_OFFSET:
        dump_append(b, "#0x%08x", operand->immediate);
        break;
    case QY8R_USP_OPERAND_NO_RESULT:
        dump_append(b, "!c0");
        break;
    case QY8R_USP_OPERAND_PREDICATE:
        dump_append(b, "p%u", operand->number);
        break;
    case QY8R_USP_OPERAND_INCREMENT_MODE:
        dump_append(b, "incrementmode");
        break;
    case QY8R_USP_OPERAND_ROUND_NEAREST:
        dump_append(b, "nearest");
        break;
    default:
        b->failed = 1;
        break;
    }
}

static void dump_efo(dump_buffer *b, const qy8r_usp_instruction *ins)
{
    static const char *const dest[] = { "i0", "i1", "a0", "a1" };
    static const char *const i0[] = { "a0", "a1", "m0", "a0" };
    static const char *const i1[] = { "a1", "a0", "m1", "m1" };
    static const char *const m0[] = { "m0=src0*src1", "m0=src0*src1",
                                      "m0=src1*src2", "m0=src1*i0" };
    static const char *const m1[] = { "m1=src0*src2", "m1=src0*src0",
                                      "m1=src0*src0", "m1=src0*i1" };
    const qy8r_usp_efo *efo = &ins->efo;
    const char *negate = efo->negate_a1 ? "-" : "";
    unsigned s;

    dump_operand(b, ins, &ins->operands[0]);
    dump_append(b, "= %s, %si0 = %s, %si1 = %s, ", dest[efo->dest_source & 3],
                efo->write_i0 ? "" : "!", i0[efo->internal_source & 3],
                efo->write_i1 ? "" : "!", i1[efo->internal_source & 3]);
    switch (efo->adder_source & 3) {
    case 0:
        dump_append(b, "a0=m0+m1, a1=%si1+i0", negate);
        break;
    case 1:
        dump_append(b, "a0=m0+src2, a1=%si1+i0", negate);
        break;
    case 2:
        dump_append(b, "a0=i0+m0, a1=%si1+m1", negate);
        break;
    default:
        dump_append(b, "a0=src0+src1 a1=%ssrc2+src0", negate);
        break;
    }
    dump_append(b, ", %s, %s", m0[efo->multiplier_source & 3],
                m1[efo->multiplier_source & 3]);
    for (s = 0; s < 3; s++) {
        dump_append(b, ", ");
        dump_operand(b, ins, &ins->operands[1 + s]);
        if (efo->format_dependent[s]) {
            dump_append(b, "/");
            dump_operand(b, ins, &efo->source_f16[s]);
        }
    }
}

static void dump_modifiers(dump_buffer *b, const qy8r_usp_instruction *ins)
{
    static const char *const tests[] = { NULL,      "testtanz", "testtaz",
                                         "testnat", "testpat",  "testpanz" };
    const char *separator = "";

    if (ins->flags & QY8R_USP_INSN_SKIPINV) {
        dump_append(b, "skipinv");
        separator = ".";
    }
    if (ins->data_type != QY8R_USP_DATA_NONE) {
        dump_append(b, "%s%s", separator,
                    ins->data_type == QY8R_USP_DATA_FLT ? "flt" : "i32");
        separator = ".";
    }
    if (ins->repeat_count > 1) {
        dump_append(b, "%srepeat%u", separator, ins->repeat_count);
        separator = ".";
    }
    if (ins->opcode == QY8R_USP_OP_MOVC) {
        dump_append(b, "%stnz", separator);
        separator = ".";
    }
    if (ins->test != QY8R_USP_TEST_NONE &&
        ins->test < sizeof(tests) / sizeof(tests[0])) {
        dump_append(b, "%s%s", separator, tests[ins->test]);
        separator = ".";
        if (ins->test_mask == QY8R_USP_TEST_MASK_DMSK) {
            dump_append(b, ".dmsk");
        } else if (ins->test_mask == QY8R_USP_TEST_MASK_CHANNEL) {
            dump_append(b, ".chan%u", ins->test_channel);
        }
    }
    if (ins->flags & QY8R_USP_INSN_SCALE) {
        dump_append(b, "%sscale", separator);
    }
}

int qy8r_usp_operand_dump(const qy8r_usp_instruction *ins,
                          const qy8r_usp_operand *operand, char *output,
                          size_t output_size)
{
    dump_buffer b = { output, output_size, 0, 0 };

    if (!ins || !operand || !output || !output_size) {
        return 0;
    }
    output[0] = 0;
    dump_operand(&b, ins, operand);
    return !b.failed;
}

int qy8r_usp_instruction_dump(const qy8r_usp_instruction *ins, char *output,
                              size_t output_size)
{
    dump_buffer b = { output, output_size, 0, 0 };
    unsigned i;

    if (!ins || !output || !output_size) {
        return 0;
    }
    output[0] = 0;
    dump_append(&b, "IR\t%04u\t%08x\t%08x\tP%02u\t%s\t%s\t", ins->pc,
                ins->words[0], ins->words[1], ins->signature,
                qy8r_usp_opcode_name(ins->opcode),
                qy8r_usp_predicate_name(ins->predicate));
    dump_modifiers(&b, ins);
    dump_append(&b, "\t");
    if (ins->opcode == QY8R_USP_OP_EFO) {
        dump_efo(&b, ins);
    } else {
        for (i = 0; i < ins->operand_count; i++) {
            if (i) {
                dump_append(&b, ", ");
            }
            dump_operand(&b, ins, &ins->operands[i]);
        }
    }
    return !b.failed;
}

int qy8r_usp_parse(const uint8_t *bytes, size_t length, int request_msaa_trans,
                   qy8r_usp_program *out, qy8r_usp_error *error)
{
    qy8r_usp_program *staged;
    int ok;
    if (error) {
        memset(error, 0, sizeof(*error));
    }
    if (!out) {
        return fail(error, QY8R_USP_ERR_ARGUMENT, 0,
                    "output program is required");
    }
    memset(out, 0, sizeof(*out));
    staged = calloc(1, sizeof(*staged));
    if (!staged) {
        return fail(error, QY8R_USP_ERR_LIMIT, 0,
                    "unable to allocate staged program");
    }
    ok = qy8r_usp_parse_impl(bytes, length, request_msaa_trans, staged, error);
    if (ok) {
        memcpy(out, staged, sizeof(*out));
    }
    free(staged);
    return ok;
}
