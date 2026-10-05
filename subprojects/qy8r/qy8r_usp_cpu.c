/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Interpreter for reviewed target-1.7 USP IR. */
#include "qy8r_usp_cpu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float bits_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float half_float(uint16_t half)
{
    uint32_t sign = (uint32_t)(half & 0x8000) << 16;
    uint32_t exponent = (half >> 10) & 31;
    uint32_t fraction = half & 1023;
    uint32_t bits;

    if (!exponent) {
        if (!fraction) {
            bits = sign;
        } else {
            int shift = 0;

            while (!(fraction & 0x400)) {
                fraction <<= 1;
                shift++;
            }
            fraction &= 0x3ff;
            bits = sign | (uint32_t)(113 - shift) << 23 | fraction << 13;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000 | fraction << 13;
    } else {
        bits = sign | (exponent + 112) << 23 | fraction << 13;
    }
    return bits_float(bits);
}

static uint16_t float_half(float value)
{
    uint32_t bits = float_bits(value);
    uint32_t sign = (bits >> 16) & 0x8000;
    int exponent = (int)((bits >> 23) & 255) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;

    if (exponent <= 0) {
        uint32_t significand;
        unsigned shift;

        if (exponent < -10) {
            return (uint16_t)sign;
        }
        significand = mantissa | 0x800000;
        shift = (unsigned)(14 - exponent);
        significand = (significand + ((1u << (shift - 1)) - 1) +
                       ((significand >> shift) & 1)) >>
                      shift;
        return (uint16_t)(sign | significand);
    }
    if (exponent >= 31) {
        return (uint16_t)(sign | 0x7c00 | (mantissa ? 0x0200 : 0));
    }
    mantissa += 0xfff + ((mantissa >> 13) & 1);
    if (mantissa & 0x800000) {
        mantissa = 0;
        exponent++;
        if (exponent >= 31) {
            return (uint16_t)(sign | 0x7c00);
        }
    }
    return (uint16_t)(sign | (uint32_t)exponent << 10 | mantissa >> 13);
}

static uint32_t *register_bank(qy8r_usp_cpu_state *state, unsigned bank,
                               unsigned number)
{
    switch (bank) {
    case QY8R_USP_BANK_R:
        return number < 128 ? &state->r[number] : NULL;
    case QY8R_USP_BANK_O:
        return number < 64 ? &state->o[number] : NULL;
    case QY8R_USP_BANK_PA:
        if (state->secondary_update) {
            return number < 512 ? &state->sa[number] : NULL;
        }
        return number < 64 ? &state->pa[number] : NULL;
    case QY8R_USP_BANK_SA:
        return number < 512 ? &state->sa[number] : NULL;
    case QY8R_USP_BANK_I:
        return number < 2 ? &state->i[number] : NULL;
    default:
        return NULL;
    }
}

static int operand_raw(qy8r_usp_cpu_state *state,
                       const qy8r_usp_operand *operand, uint32_t *raw)
{
    uint32_t *reg;

    if (operand->kind == QY8R_USP_OPERAND_IMMEDIATE ||
        operand->kind == QY8R_USP_OPERAND_BRANCH_OFFSET) {
        *raw = operand->immediate;
        return 1;
    }
    if (operand->bank == QY8R_USP_BANK_C) {
        switch (operand->number) {
        case 45:
        case 52:
            *raw = (operand->flags & QY8R_USP_OPERAND_FLT16) ? 0x3c003c00
                                                             : 0x3f800000;
            return 1;
        case 48:
            *raw = 0;
            return 1;
        default:
            return 0;
        }
    }
    reg = register_bank(state, operand->bank, operand->number);
    if (!reg) {
        return 0;
    }
    *raw = *reg;
    return 1;
}

static int operand_float(qy8r_usp_cpu_state *state,
                         const qy8r_usp_operand *operand, float *value)
{
    uint32_t raw;

    if (!operand_raw(state, operand, &raw)) {
        return 0;
    }
    *value =
        (operand->flags & QY8R_USP_OPERAND_FLT16)
            ? half_float((uint16_t)(raw >> (operand->component == 2 ? 16 : 0)))
            : bits_float(raw);
    if (operand->flags & QY8R_USP_OPERAND_NEGATE) {
        *value = -*value;
    }
    if (operand->flags & QY8R_USP_OPERAND_ABSOLUTE) {
        *value = fabsf(*value);
    }
    return 1;
}

static int write_raw(qy8r_usp_cpu_state *state,
                     const qy8r_usp_operand *destination, uint32_t value)
{
    uint32_t *reg =
        register_bank(state, destination->bank, destination->number);
    uint32_t mask = 0;
    unsigned byte;

    if (!reg) {
        return 0;
    }
    if (destination->flags & QY8R_USP_OPERAND_HAS_BYTEMASK) {
        for (byte = 0; byte < 4; byte++) {
            if (destination->bytemask & (1u << byte)) {
                mask |= 0xffu << (8 * byte);
            }
        }
        *reg = (*reg & ~mask) | (value & mask);
    } else if (destination->flags & QY8R_USP_OPERAND_HAS_COMPONENT) {
        unsigned shift = destination->component == 2 ? 16 : 0;

        *reg = (*reg & ~(0xffffu << shift)) | ((value & 0xffffu) << shift);
    } else {
        *reg = value;
    }
    return 1;
}

static int write_float(qy8r_usp_cpu_state *state,
                       const qy8r_usp_operand *destination, float value)
{
    return write_raw(state, destination, float_bits(value));
}

static int predicate_true(const qy8r_usp_cpu_state *state, unsigned pred)
{
    switch (pred) {
    case QY8R_USP_PRED_ALWAYS:
        return 1;
    case QY8R_USP_PRED_P0:
        return state->p[0] != 0;
    case QY8R_USP_PRED_P1:
        return state->p[1] != 0;
    case QY8R_USP_PRED_P2:
        return state->p[2] != 0;
    case QY8R_USP_PRED_P3:
        return state->p[3] != 0;
    case QY8R_USP_PRED_NOT_P0:
        return state->p[0] == 0;
    case QY8R_USP_PRED_NOT_P1:
        return state->p[1] == 0;
    default:
        return 0;
    }
}

static int set_test(const qy8r_usp_instruction *ins, qy8r_usp_cpu_state *state,
                    float value)
{
    int result;
    unsigned i;

    switch (ins->test) {
    case QY8R_USP_TEST_TANZ:
        result = value != 0.0f;
        break;
    case QY8R_USP_TEST_TAZ:
        result = value == 0.0f;
        break;
    case QY8R_USP_TEST_NAT:
        result = signbit(value) != 0;
        break;
    case QY8R_USP_TEST_PAT:
        result = !signbit(value);
        break;
    case QY8R_USP_TEST_PANZ:
        result = value > 0.0f;
        break;
    default:
        return 1;
    }
    for (i = 0; i < ins->operand_count; i++) {
        const qy8r_usp_operand *operand = &ins->operands[i];

        if (operand->kind == QY8R_USP_OPERAND_PREDICATE &&
            operand->number < 4) {
            state->p[operand->number] = (uint8_t)result;
        }
    }
    return result;
}

static int execute_instruction(const qy8r_usp_instruction *ins,
                               qy8r_usp_cpu_state *state)
{
    unsigned rep, count = ins->expanded_count ? ins->expanded_count : 1;

    if (!predicate_true(state, ins->predicate)) {
        return 1;
    }
    if (ins->opcode == QY8R_USP_OP_SMLSI || ins->opcode == QY8R_USP_OP_NOP ||
        ins->opcode == QY8R_USP_OP_BR) {
        return 1;
    }
    if (ins->opcode == QY8R_USP_OP_EFO) {
        float source[3], m0, m1, a0, a1, selected;
        float old_i0 = bits_float(state->i[0]);
        float old_i1 = bits_float(state->i[1]);
        uint32_t *destination;
        unsigned i;

        for (i = 0; i < 3; i++) {
            const qy8r_usp_operand *operand = ins->efo.format_dependent[i]
                                                  ? &ins->efo.source_f16[i]
                                                  : &ins->operands[i + 1];
            if (!operand_float(state, operand, &source[i])) {
                return 0;
            }
        }
        m0 = source[0] * source[1];
        m1 = source[0] * source[2];
        if (ins->efo.adder_source == 3) {
            a0 = source[0] + source[1];
            a1 = source[2] + source[0];
        } else {
            a0 = old_i0 + m0;
            a1 = old_i1 + m1;
        }
        if (ins->efo.internal_source == 2) {
            if (ins->efo.write_i0) {
                state->i[0] = float_bits(m0);
            }
            if (ins->efo.write_i1) {
                state->i[1] = float_bits(m1);
            }
        } else {
            if (ins->efo.write_i0) {
                state->i[0] = float_bits(a0);
            }
            if (ins->efo.write_i1) {
                state->i[1] = float_bits(a1);
            }
        }
        switch (ins->efo.dest_source) {
        case 0:
            selected = old_i0;
            break;
        case 1:
            selected = old_i1;
            break;
        case 2:
            selected = a0;
            break;
        case 3:
            selected = a1;
            break;
        default:
            return 0;
        }
        destination = register_bank(state, ins->operands[0].bank,
                                    ins->operands[0].number);
        return destination && write_float(state, &ins->operands[0], selected);
    }
    for (rep = 0; rep < count; rep++) {
        const qy8r_usp_operand *a = ins->expanded[rep];
        float x, y, z;
        uint32_t raw;

        switch (ins->opcode) {
        case QY8R_USP_OP_MOV:
            if (!operand_raw(state, &a[1], &raw) ||
                !write_raw(state, &a[0], raw)) {
                return 0;
            }
            break;
        case QY8R_USP_OP_MOVC:
            if (!operand_raw(state, &a[1], &raw)) {
                return 0;
            }
            if (ins->data_type == QY8R_USP_DATA_I32) {
                x = (float)(int32_t)raw;
            } else if (!operand_float(state, &a[1], &x)) {
                return 0;
            }
            if ((ins->data_type == QY8R_USP_DATA_I32 && raw != 0) ||
                (ins->data_type != QY8R_USP_DATA_I32 && x != 0.0f)) {
                if (!operand_raw(state, &a[2], &raw)) {
                    return 0;
                }
            } else if (!operand_raw(state, &a[3], &raw)) {
                return 0;
            }
            if (!write_raw(state, &a[0], raw)) {
                return 0;
            }
            break;
        case QY8R_USP_OP_FADD:
        case QY8R_USP_OP_FSUB:
        case QY8R_USP_OP_FMIN:
        case QY8R_USP_OP_FMAX:
        case QY8R_USP_OP_FMAD:
        case QY8R_USP_OP_FMAD16:
        case QY8R_USP_OP_FMSA: {
            unsigned source = a[0].kind == QY8R_USP_OPERAND_NO_RESULT ? 2 : 1;

            if (ins->opcode == QY8R_USP_OP_FMAD16) {
                uint32_t rx, ry, rz;
                uint16_t lo, hi;
                float halves[3][2];
                const qy8r_usp_operand *operands[3] = { &a[source],
                                                        &a[source + 1],
                                                        &a[source + 2] };
                unsigned lane, operand;

                if (!operand_raw(state, operands[0], &rx) ||
                    !operand_raw(state, operands[1], &ry) ||
                    !operand_raw(state, operands[2], &rz)) {
                    return 0;
                }
                uint32_t packed[3] = { rx, ry, rz };

                /*
                 * The reference constant map defines c45 and c52 as 1.0.
                 * Their typed IR operands do not carry FLT16, so encode
                 * both half lanes for this instruction.
                 */
                for (operand = 0; operand < 3; operand++) {
                    if (operands[operand]->bank == QY8R_USP_BANK_C &&
                        (operands[operand]->number == 45 ||
                         operands[operand]->number == 52)) {
                        packed[operand] = 0x3c003c00;
                    }
                }
                for (operand = 0; operand < 3; operand++) {
                    for (lane = 0; lane < 2; lane++) {
                        float value = half_float(
                            (uint16_t)(packed[operand] >> (lane * 16)));
                        if (operands[operand]->flags &
                            QY8R_USP_OPERAND_NEGATE) {
                            value = -value;
                        }
                        if (operands[operand]->flags &
                            QY8R_USP_OPERAND_ABSOLUTE) {
                            value = fabsf(value);
                        }
                        halves[operand][lane] = value;
                    }
                }
                lo = float_half(halves[0][0] * halves[1][0] + halves[2][0]);
                hi = float_half(halves[0][1] * halves[1][1] + halves[2][1]);
                if (!write_raw(state, &a[0], lo | (uint32_t)hi << 16)) {
                    return 0;
                }
                break;
            }
            if (!operand_float(state, &a[source], &x) ||
                !operand_float(state, &a[source + 1], &y)) {
                return 0;
            }
            z = 0.0f;
            if (ins->operand_count > source + 2 &&
                !operand_float(state, &a[source + 2], &z)) {
                return 0;
            }
            switch (ins->opcode) {
            case QY8R_USP_OP_FADD:
                z = x + y;
                break;
            case QY8R_USP_OP_FSUB:
                z = x - y;
                break;
            case QY8R_USP_OP_FMIN:
                z = fminf(x, y);
                break;
            case QY8R_USP_OP_FMAX:
                z = fmaxf(x, y);
                break;
            case QY8R_USP_OP_FMAD:
                z = fmaf(x, y, z);
                break;
            case QY8R_USP_OP_FMSA:
                z = x * x + y * z;
                break;
            default:
                break;
            }
            if (a[0].kind == QY8R_USP_OPERAND_NO_RESULT) {
                set_test(ins, state, z);
            } else if (ins->test_mask == QY8R_USP_TEST_MASK_DMSK) {
                raw = set_test(ins, state, z) ? UINT32_MAX : 0;
                if (!write_raw(state, &a[0], raw)) {
                    return 0;
                }
            } else if (!write_float(state, &a[0], z)) {
                return 0;
            } else {
                set_test(ins, state, z);
            }
            break;
        }
        case QY8R_USP_OP_SHL:
            if (!operand_raw(state, &a[2], &raw)) {
                return 0;
            }
            raw <<= a[3].immediate;
            if (a[0].kind == QY8R_USP_OPERAND_NO_RESULT) {
                set_test(ins, state, bits_float(raw));
            } else if (!write_raw(state, &a[0], raw)) {
                return 0;
            }
            break;
        case QY8R_USP_OP_UNPCKF32F16:
            if (!operand_raw(state, &a[1], &raw) ||
                !write_float(
                    state, &a[0],
                    half_float(
                        (uint16_t)(raw >> (a[1].component == 2 ? 16 : 0))))) {
                return 0;
            }
            break;
        case QY8R_USP_OP_UNPCKU8U8:
            if (!operand_raw(state, &a[1], &raw) ||
                !write_raw(state, &a[0],
                           ((raw >> (8 * a[1].component)) & 255) *
                               0x01010101u)) {
                return 0;
            }
            break;
        case QY8R_USP_OP_PCKU8F16:
        case QY8R_USP_OP_PCKU8F32: {
            uint32_t old;
            unsigned lane, next = 0;
            unsigned byte_mask = (a[0].flags & QY8R_USP_OPERAND_HAS_BYTEMASK)
                                     ? a[0].bytemask
                                     : 15;
            const qy8r_usp_operand *sources[2] = { &a[1], &a[2] };

            if (!operand_raw(state, &a[0], &old)) {
                return 0;
            }
            raw = old;
            for (lane = 0; lane < 4; lane++) {
                float value;
                uint32_t src;
                unsigned byte;

                if (!(byte_mask & (1u << lane))) {
                    continue;
                }
                if (!operand_raw(state, sources[next++ & 1], &src)) {
                    return 0;
                }
                if (ins->opcode == QY8R_USP_OP_PCKU8F16) {
                    value = half_float(
                        (uint16_t)(src >>
                                   (sources[(next - 1) & 1]->component == 2
                                        ? 16
                                        : 0)));
                } else {
                    value = bits_float(src);
                }
                value = fminf(1.0f, fmaxf(0.0f, value));
                byte = (unsigned)floorf(value * 255.0f + 0.5f);
                raw = (raw & ~(255u << (lane * 8))) | byte << (lane * 8);
            }
            if (!write_raw(state, &a[0], raw)) {
                return 0;
            }
            break;
        }
        default:
            return 0;
        }
    }
    return 1;
}

static int sample_node(const qy8r_usp_program *program,
                       const qy8r_usp_sample *sample, qy8r_usp_cpu_io *io)
{
    qy8r_usp_cpu_state *state = &io->state;
    uint32_t *coord =
        register_bank(state, sample->source_type, sample->source_num);
    float u, v;
    unsigned i;

    if (sample->kind == 5) {
        for (i = 0; i < program->ps_input_count; i++) {
            if (program->ps_inputs[i].texture == sample->texture_index) {
                coord = register_bank(state, QY8R_USP_BANK_PA,
                                      program->ps_inputs[i].coord);
                break;
            }
        }
        if (!coord || !io->sample) {
            return 0;
        }
        u = bits_float(coord[0]);
        v = bits_float(coord[1]);
        if (!io->sample(io->sample_opaque, sample->texture_index, u, v,
                        state->sample)) {
            return 0;
        }
        for (i = 0; i < 4; i++) {
            uint32_t *dest = register_bank(state, sample->base_dest_type,
                                           sample->base_dest_num + i);
            if (dest) {
                *dest = state->sample[i];
            }
        }
        return 1;
    }
    if (sample->kind != 6) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        uint32_t *dest;
        unsigned shift;
        uint32_t value = state->sample[i];

        if (!sample->dest_type[i]) {
            continue;
        }
        dest = register_bank(state, sample->dest_type[i], sample->dest_num[i]);
        if (!dest) {
            return 0;
        }
        if (sample->dest_format[i] == 2) {
            value = float_half(bits_float(value));
            shift = sample->dest_component[i] == 2 ? 16 : 0;
            *dest =
                (*dest & ~(0xffffu << shift)) | ((value & 0xffffu) << shift);
        } else {
            *dest = value;
        }
    }
    return 1;
}

static int load_inputs(const qy8r_usp_program *program, qy8r_usp_cpu_io *io,
                       char *error, size_t error_size)
{
    const qy8r_usp_cpu_inputs *inputs = io->inputs;
    unsigned i, j;

    if (!inputs) {
        return 1;
    }
    io->state.p[0] = 1;
    io->state.p[1] = 1;
    for (i = 0; i < 4; i++) {
        if (inputs->predicate_mask & (1u << i)) {
            io->state.p[i] = inputs->predicate_values[i] != 0;
        }
    }
    for (i = 0; i < program->bindings.attribute_count; i++) {
        unsigned symbol_index = program->bindings.attributes[i];
        const qy8r_usp_symbol *symbol;
        const float *values;
        unsigned count;

        if (symbol_index >= program->symbol_count) {
            if (error && error_size) {
                snprintf(error, error_size,
                         "attribute symbol index is invalid");
            }
            return 0;
        }
        symbol = &program->symbols[symbol_index];
        values = inputs->symbols[symbol_index];
        count = inputs->symbol_counts[symbol_index];
        if (!values) {
            continue;
        }
        if (count > symbol->component_count) {
            if (error && error_size) {
                snprintf(error, error_size,
                         "attribute component count is invalid");
            }
            return 0;
        }
        for (j = 0; j < count; j++) {
            unsigned reg = symbol->base_comp_or_texture_unit + j;
            if (reg >= 64) {
                if (error && error_size) {
                    snprintf(error, error_size,
                             "attribute register is invalid");
                }
                return 0;
            }
            io->state.pa[reg] = float_bits(values[j]);
        }
    }
    for (i = 0; i < program->regconst_count; i++) {
        const qy8r_usp_regconst *rc = &program->regconsts[i];
        float value;
        uint32_t raw;
        unsigned shift = rc->destination_halfword_shift;

        if (rc->uniform_symbol >= 0) {
            unsigned symbol = (unsigned)rc->uniform_symbol;
            const float *values = inputs->symbols[symbol];
            unsigned count = inputs->symbol_counts[symbol];

            if (values && rc->uniform_component < count) {
                value = values[rc->uniform_component];
            } else if (rc->source_constant_index < program->constant_count) {
                value = program->constants[rc->source_constant_index];
            } else {
                if (error && error_size) {
                    snprintf(error, error_size,
                             "uniform %u component %u has no supplied or "
                             "default value",
                             symbol, rc->uniform_component);
                }
                return 0;
            }
        } else if (rc->has_literal) {
            value = rc->literal;
        } else {
            if (error && error_size) {
                snprintf(error, error_size, "constant RegConst has no value");
            }
            return 0;
        }
        raw = rc->format == 2 ? float_half(value) : float_bits(value);
        if (rc->destination_sa_register >= 512 || shift > 16) {
            if (error && error_size) {
                snprintf(error, error_size, "RegConst destination is invalid");
            }
            return 0;
        }
        if (shift) {
            io->state.sa[rc->destination_sa_register] =
                (io->state.sa[rc->destination_sa_register] &
                 ~(0xffffu << shift)) |
                ((raw & 0xffffu) << shift);
        } else if (rc->format == 2) {
            io->state.sa[rc->destination_sa_register] =
                (io->state.sa[rc->destination_sa_register] & 0xffff0000u) |
                (raw & 0xffffu);
        } else {
            io->state.sa[rc->destination_sa_register] = raw;
        }
    }
    for (i = 0; i < program->ps_input_count; i++) {
        unsigned coord = program->ps_inputs[i].coord;
        unsigned texture = program->ps_inputs[i].texture;
        unsigned dimension =
            texture < 10 ? program->texcoord_dimensions[texture] : 0;
        unsigned count;

        if (!dimension) {
            dimension = program->ps_inputs[i].coord_dim + 1;
        }
        if (coord >= 64) {
            if (error && error_size) {
                snprintf(error, error_size,
                         "pixel input coordinate is invalid");
            }
            return 0;
        }
        if (dimension > 4) {
            dimension = 4;
        }
        count = inputs->varying_counts[coord];
        if (count > dimension) {
            count = dimension;
        }
        if (count > 64 - coord) {
            if (error && error_size) {
                snprintf(error, error_size,
                         "pixel input register range is invalid");
            }
            return 0;
        }
        for (j = 0; j < count; j++) {
            io->state.pa[coord + j] = float_bits(inputs->varyings[coord][j]);
        }
    }
    return 1;
}

int qy8r_usp_cpu_execute(const qy8r_usp_program *program,
                         unsigned variant_index, qy8r_usp_cpu_io *io,
                         char *error, size_t error_size)
{
    const qy8r_usp_variant *variant;
    uint16_t label_node[QY8R_USP_MAX_NODES];
    uint16_t label_count = 0;
    unsigned i, steps = 0, limit;

    if (!program || !io || variant_index >= program->variant_count ||
        !program->variants[variant_index].valid) {
        if (error && error_size) {
            snprintf(error, error_size, "invalid program or variant");
        }
        return 0;
    }
    variant = &program->variants[variant_index];
    if (!load_inputs(program, io, error, error_size)) {
        if (error && error_size) {
            if (!error[0]) {
                snprintf(error, error_size,
                         "input binding exceeds IR metadata");
            }
        }
        return 0;
    }
    for (i = 0; i < variant->node_count; i++) {
        if (variant->nodes[i].kind == QY8R_USP_NODE_LABEL) {
            label_node[label_count++] = (uint16_t)i;
        }
    }
    limit = io->max_steps ? io->max_steps : 4096;
    for (i = 0; i < variant->node_count && steps < limit;) {
        const qy8r_usp_node *node = &variant->nodes[i];

        steps++;
        switch (node->kind) {
        case QY8R_USP_NODE_INSTRUCTION: {
            const qy8r_usp_instruction *ins;

            if (node->index >= variant->instruction_count) {
                goto malformed;
            }
            ins = &variant->instructions[node->index];
            if (ins->opcode == QY8R_USP_OP_BR &&
                predicate_true(&io->state, ins->predicate)) {
                unsigned j;
                for (j = 0; j < label_count; j++) {
                    const qy8r_usp_node *label = &variant->nodes[label_node[j]];
                    if (label->label == ins->branch_target_label) {
                        i = label_node[j];
                        break;
                    }
                }
                if (j == label_count) {
                    goto malformed;
                }
                continue;
            }
            io->state.secondary_update = ins->execution_domain != 0;
            if (!execute_instruction(ins, &io->state)) {
                io->state.secondary_update = 0;
                goto unsupported;
            }
            io->state.secondary_update = 0;
            i++;
            break;
        }
        case QY8R_USP_NODE_SAMPLE:
            if (node->index >= variant->sample_count ||
                !sample_node(program, &variant->samples[node->index], io)) {
                goto unsupported;
            }
            i++;
            break;
        case QY8R_USP_NODE_PHASE_DISCARD:
            i++;
            break;
        case QY8R_USP_NODE_LABEL:
            if (node->label == program->phase1_start_label &&
                predicate_true(&io->state, QY8R_USP_PRED_NOT_P1)) {
                io->state.discard = 1;
                while (i < variant->node_count &&
                       variant->nodes[i].kind != QY8R_USP_NODE_END) {
                    i++;
                }
                break;
            }
            i++;
            break;
        case QY8R_USP_NODE_END:
            io->stage = program->stage;
            if (program->stage == 1 && program->result_pa_register < 64) {
                io->result_raw = io->state.pa[program->result_pa_register];
            }
            return 1;
        default:
            goto malformed;
        }
    }
    if (error && error_size) {
        snprintf(error, error_size, "instruction step limit exceeded");
    }
    return 0;

unsupported:
    if (error && error_size) {
        snprintf(error, error_size, "unsupported IR operation at node %u", i);
    }
    return 0;
malformed:
    if (error && error_size) {
        snprintf(error, error_size, "malformed IR at node %u", i);
    }
    return 0;
}

int qy8r_usp_cpu_run_blob(const uint8_t *bytes, size_t length,
                          unsigned variant_index,
                          const qy8r_usp_cpu_inputs *inputs,
                          qy8r_usp_cpu_io *io, char *error, size_t error_size)
{
    qy8r_usp_program *program;
    qy8r_usp_error parse_error;
    int result;

    if (!bytes || !io) {
        if (error && error_size) {
            snprintf(error, error_size, "invalid blob or execution state");
        }
        return 0;
    }
    program = malloc(sizeof(*program));
    if (!program) {
        if (error && error_size) {
            snprintf(error, error_size, "out of memory parsing shader blob");
        }
        return 0;
    }
    if (!qy8r_usp_parse(bytes, length, 0, program, &parse_error)) {
        if (error && error_size) {
            snprintf(error, error_size, "%s at 0x%zx: %s",
                     qy8r_usp_error_name(parse_error.code), parse_error.offset,
                     parse_error.message);
        }
        free(program);
        return 0;
    }
    io->inputs = inputs;
    result =
        qy8r_usp_cpu_execute(program, variant_index, io, error, error_size);
    free(program);
    return result;
}
