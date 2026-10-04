/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Standalone SGXBS/USP parser and reviewed USE IR.
 *
 * This interface deliberately has no EGL, GLES, QEMU, or GLib dependency.
 */
#ifndef QY8R_USP_H
#define QY8R_USP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QY8R_USP_MAX_VARIANTS 2
#define QY8R_USP_MAX_NODES 512
#define QY8R_USP_MAX_INSTRUCTIONS 512
#define QY8R_USP_MAX_SYMBOLS 128
#define QY8R_USP_MAX_MEMBERS 16
#define QY8R_USP_MAX_REGCONST 256
#define QY8R_USP_MAX_CONSTANTS 512
#define QY8R_USP_MAX_OPERANDS 12
#define QY8R_USP_MAX_REPEAT 4
#define QY8R_USP_NAME_CAP 128

typedef enum qy8r_usp_error_code {
    QY8R_USP_OK = 0,
    QY8R_USP_ERR_ARGUMENT,
    QY8R_USP_ERR_TRUNCATED,
    QY8R_USP_ERR_CONTAINER,
    QY8R_USP_ERR_REVISION,
    QY8R_USP_ERR_PROFILE,
    QY8R_USP_ERR_BLOCK,
    QY8R_USP_ERR_INSTRUCTION,
    QY8R_USP_ERR_LIMIT,
    QY8R_USP_ERR_BINDING
} qy8r_usp_error_code;

typedef struct qy8r_usp_error {
    qy8r_usp_error_code code;
    size_t offset;
    char message[256];
} qy8r_usp_error;

typedef enum qy8r_usp_node_kind {
    QY8R_USP_NODE_INSTRUCTION = 1,
    QY8R_USP_NODE_SAMPLE,
    QY8R_USP_NODE_LABEL,
    QY8R_USP_NODE_END,
    QY8R_USP_NODE_PHASE_DISCARD
} qy8r_usp_node_kind;

typedef enum qy8r_usp_opcode {
    QY8R_USP_OP_INVALID,
    QY8R_USP_OP_BR,
    QY8R_USP_OP_NOP,
    QY8R_USP_OP_SMLSI,
    QY8R_USP_OP_EFO,
    QY8R_USP_OP_MOV,
    QY8R_USP_OP_MOVC,
    QY8R_USP_OP_FMAD,
    QY8R_USP_OP_FMAD16,
    QY8R_USP_OP_FMSA,
    QY8R_USP_OP_FADD,
    QY8R_USP_OP_FSUB,
    QY8R_USP_OP_FMIN,
    QY8R_USP_OP_FMAX,
    QY8R_USP_OP_SHL,
    QY8R_USP_OP_UNPCKU8U8,
    QY8R_USP_OP_UNPCKF32F16,
    QY8R_USP_OP_PCKU8F16,
    QY8R_USP_OP_PCKU8F32
} qy8r_usp_opcode;

typedef enum qy8r_usp_bank {
    QY8R_USP_BANK_NONE,
    QY8R_USP_BANK_R,   /* temporary */
    QY8R_USP_BANK_O,   /* output */
    QY8R_USP_BANK_PA,  /* primary attribute */
    QY8R_USP_BANK_SA,  /* secondary attribute */
    QY8R_USP_BANK_C,   /* hardware constant */
    QY8R_USP_BANK_G,   /* global */
    QY8R_USP_BANK_I    /* internal i0/i1 */
} qy8r_usp_bank;

typedef enum qy8r_usp_operand_kind {
    QY8R_USP_OPERAND_NONE,
    QY8R_USP_OPERAND_REGISTER,
    QY8R_USP_OPERAND_IMMEDIATE,
    QY8R_USP_OPERAND_BRANCH_OFFSET,
    QY8R_USP_OPERAND_NO_RESULT,       /* TEST that writes no register */
    QY8R_USP_OPERAND_PREDICATE,       /* predicate written by a TEST */
    QY8R_USP_OPERAND_INCREMENT_MODE,  /* SMLSI per-operand mode */
    QY8R_USP_OPERAND_ROUND_NEAREST    /* pack/unpack rounding mode */
} qy8r_usp_operand_kind;

typedef enum qy8r_usp_predicate {
    QY8R_USP_PRED_ALWAYS,
    QY8R_USP_PRED_P0,
    QY8R_USP_PRED_P1,
    QY8R_USP_PRED_P2,
    QY8R_USP_PRED_P3,
    QY8R_USP_PRED_NOT_P0,
    QY8R_USP_PRED_NOT_P1
} qy8r_usp_predicate;

typedef enum qy8r_usp_test {
    QY8R_USP_TEST_NONE,
    QY8R_USP_TEST_TANZ,
    QY8R_USP_TEST_TAZ,
    QY8R_USP_TEST_NAT,
    QY8R_USP_TEST_PAT,
    QY8R_USP_TEST_PANZ
} qy8r_usp_test;

typedef enum qy8r_usp_test_mask {
    QY8R_USP_TEST_MASK_NONE,
    QY8R_USP_TEST_MASK_CHANNEL,  /* result taken from one channel */
    QY8R_USP_TEST_MASK_DMSK      /* per-channel mask written to the dest */
} qy8r_usp_test_mask;

typedef enum qy8r_usp_data_type {
    QY8R_USP_DATA_NONE,
    QY8R_USP_DATA_FLT,
    QY8R_USP_DATA_I32
} qy8r_usp_data_type;

enum qy8r_usp_instruction_flags {
    QY8R_USP_INSN_SKIPINV = 1u << 0,
    QY8R_USP_INSN_SCALE = 1u << 1
};

enum qy8r_usp_operand_flags {
    QY8R_USP_OPERAND_NEGATE = 1u << 0,
    QY8R_USP_OPERAND_ABSOLUTE = 1u << 1,
    QY8R_USP_OPERAND_HAS_COMPONENT = 1u << 2,
    QY8R_USP_OPERAND_HAS_BYTEMASK = 1u << 3,
    QY8R_USP_OPERAND_FLT16 = 1u << 4
};

/*
 * One decoded operand. For registers, component selects the 16-bit lane
 * (0 or 2) of an F16 operand or the source channel of a pack/unpack;
 * bytemask is the destination byte-enable mask (bit n enables byte n).
 */
typedef struct qy8r_usp_operand {
    uint32_t immediate;
    uint16_t number;
    uint8_t kind;       /* qy8r_usp_operand_kind */
    uint8_t bank;       /* qy8r_usp_bank */
    uint8_t component;
    uint8_t bytemask;
    uint8_t flags;      /* qy8r_usp_operand_flags */
} qy8r_usp_operand;

/*
 * EFO micro-operation selectors, as encoded. operands[0] of the
 * instruction is the architectural destination and operands[1..3] are
 * src0..src2 decoded with F16 format control off; source_f16[] holds the
 * same slots decoded with it on, which differs only when format_dependent
 * is set for that slot.
 */
typedef struct qy8r_usp_efo {
    uint8_t dest_source;        /* 0=i0 1=i1 2=a0 3=a1 */
    uint8_t internal_source;
    uint8_t adder_source;
    uint8_t multiplier_source;
    uint8_t write_i0;
    uint8_t write_i1;
    uint8_t negate_a1;
    uint8_t format_dependent[3];
    qy8r_usp_operand source_f16[3];
} qy8r_usp_efo;

typedef struct qy8r_usp_instruction {
    uint32_t pc;
    uint32_t words[2];
    uint8_t signature;      /* reviewed signature number, 1..37 */
    uint8_t opcode;         /* qy8r_usp_opcode */
    uint8_t predicate;      /* qy8r_usp_predicate */
    uint8_t flags;          /* qy8r_usp_instruction_flags */
    uint8_t data_type;      /* qy8r_usp_data_type, MOVC only */
    uint8_t test;           /* qy8r_usp_test */
    uint8_t test_mask;      /* qy8r_usp_test_mask */
    uint8_t test_channel;
    uint8_t repeat_count;
    uint8_t operand_count;
    uint8_t expanded_count;
    qy8r_usp_operand operands[QY8R_USP_MAX_OPERANDS];
    /* operands of each repeat iteration after applying the MOE state */
    qy8r_usp_operand expanded[QY8R_USP_MAX_REPEAT][QY8R_USP_MAX_OPERANDS];
    qy8r_usp_efo efo;
    int8_t moe_before[4];
    int8_t moe_after[4];
    uint8_t execution_domain; /* 0=main, 1=PROGDESC SA_UPDATE */
    uint16_t source_block_offset;
    uint16_t branch_target_label;
    uint8_t has_branch_target;
} qy8r_usp_instruction;

typedef struct qy8r_usp_sample {
    uint8_t kind; /* 5=SAMPLE, 6=SAMPLEUNPACK */
    uint16_t flags;
    uint16_t texture_index;
    uint16_t swizzle;
    uint16_t base_dest_type;
    uint16_t base_dest_num;
    uint16_t direct_dest_type;
    uint16_t direct_dest_num;
    uint16_t sample_temp_type;
    uint16_t sample_temp_num;
    uint16_t u_mask;
    uint16_t u_live;
    uint16_t i_regs_live;
    uint16_t c10_i_regs_live;
    uint16_t texture_precision;
    uint16_t dest_type[4];
    uint16_t dest_num[4];
    uint16_t dest_format[4];
    uint16_t dest_component[4];
    uint16_t source_type;
    uint16_t source_num;
    uint32_t words[2];
    uint8_t has_words;
} qy8r_usp_sample;

typedef struct qy8r_usp_node {
    qy8r_usp_node_kind kind;
    uint16_t index;
    uint16_t label;
    uint16_t phase0_end_label;
    uint16_t phase1_start_label;
    uint8_t phase_predicate; /* qy8r_usp_predicate */
} qy8r_usp_node;

typedef struct qy8r_usp_symbol {
    char name[QY8R_USP_NAME_CAP];
    uint16_t builtin_id;
    uint8_t type;
    uint8_t qualifier;
    uint8_t precision;
    uint8_t varying_flags;
    uint16_t active_array_size;
    uint16_t declared_array_size;
    uint8_t register_type;
    uint16_t base_comp_or_texture_unit;
    uint8_t component_count;
    uint16_t component_use_mask;
    uint8_t member_count;
} qy8r_usp_symbol;

typedef struct qy8r_usp_regconst {
    uint16_t source_constant_index;
    uint16_t source_halfword_shift;
    uint16_t destination_sa_register;
    uint16_t destination_halfword_shift;
    uint16_t format;
    int16_t uniform_symbol; /* -1 when sourced from constant_data */
    uint16_t uniform_component;
    float literal;
    uint8_t has_literal;
} qy8r_usp_regconst;

typedef struct qy8r_usp_output_component {
    uint16_t symbol_index;
    uint16_t component;
    uint16_t output_register;
} qy8r_usp_output_component;

typedef struct qy8r_usp_bindings {
    uint16_t attribute_count;
    uint16_t attributes[QY8R_USP_MAX_SYMBOLS];
    uint16_t uniform_count;
    uint16_t uniforms[QY8R_USP_MAX_SYMBOLS];
    uint16_t sampler_count;
    uint16_t samplers[QY8R_USP_MAX_SYMBOLS];
    uint16_t varying_count;
    uint16_t varyings[QY8R_USP_MAX_SYMBOLS];
    uint16_t output_component_count;
    qy8r_usp_output_component output_components[QY8R_USP_MAX_SYMBOLS * 4];
    uint16_t position_output_base;
    uint16_t position_output_components;
    uint16_t fragment_result_pa;
} qy8r_usp_bindings;

typedef struct qy8r_usp_variant {
    uint8_t valid;
    uint8_t is_translucent_variant;
    uint8_t translated;
    uint16_t usp_version;
    uint16_t instruction_count;
    uint16_t sample_count;
    uint16_t node_count;
    qy8r_usp_instruction instructions[QY8R_USP_MAX_INSTRUCTIONS];
    qy8r_usp_sample samples[QY8R_USP_MAX_NODES];
    qy8r_usp_node nodes[QY8R_USP_MAX_NODES];
} qy8r_usp_variant;

typedef struct qy8r_usp_program {
    uint32_t container_hash;
    uint32_t computed_container_hash;
    uint16_t software_version;
    uint16_t core_id;
    uint16_t core_revision;
    uint32_t project_hash;
    uint32_t glsl_interface;
    uint32_t usp_pc_shader_version;
    uint8_t stage; /* 0=vertex, 1=fragment */
    uint32_t program_flags;
    uint32_t active_varying_mask;
    uint8_t texcoord_dimensions[10];
    uint8_t texcoord_precisions[10];
    uint16_t result_pa_register;
    uint16_t result_register_type;
    uint16_t result_register_format;
    uint16_t result_register_count;
    uint16_t result_temp_register;
    uint16_t result_output_register;
    uint16_t phase0_end_label;
    uint16_t phase1_start_label;
    uint16_t regconst_base_register;
    uint16_t regconst_max_count;
    uint16_t ps_input_count;
    struct {
        uint16_t flags, texture, coord, coord_dim, format, data_size;
    } ps_inputs[32];
    uint16_t constant_count;
    float constants[QY8R_USP_MAX_CONSTANTS];
    uint16_t symbol_count;
    qy8r_usp_symbol symbols[QY8R_USP_MAX_SYMBOLS];
    uint16_t regconst_count;
    qy8r_usp_regconst regconsts[QY8R_USP_MAX_REGCONST];
    qy8r_usp_bindings bindings;
    uint8_t variant_count;
    uint8_t trans_variant_present;
    qy8r_usp_variant variants[QY8R_USP_MAX_VARIANTS];
} qy8r_usp_program;

/*
 * Parses the QY8 SGXBS container and builds main IR. A present MSAA-trans
 * USP is structurally validated but not translated. The caller must pass
 * request_msaa_trans explicitly; such requests fail closed.
 */
int qy8r_usp_parse(const uint8_t *bytes, size_t length,
                   int request_msaa_trans,
                   qy8r_usp_program *out, qy8r_usp_error *error);

/*
 * Text renderers for diagnostics and listing comparison. The text is derived
 * from the typed IR; nothing parses it back.
 */
const char *qy8r_usp_opcode_name(unsigned opcode);
const char *qy8r_usp_predicate_name(unsigned predicate);

/* Renders one operand, e.g. "sa9.neg.flt16.0". Returns 0 on overflow. */
int qy8r_usp_operand_dump(const qy8r_usp_instruction *instruction,
                          const qy8r_usp_operand *operand,
                          char *output, size_t output_size);

/*
 * Renders "IR<TAB>pc<TAB>word0<TAB>word1<TAB>signature<TAB>opcode<TAB>
 * predicate<TAB>modifiers<TAB>operands". Returns 0 on overflow.
 */
int qy8r_usp_instruction_dump(const qy8r_usp_instruction *instruction,
                              char *output, size_t output_size);

const char *qy8r_usp_error_name(qy8r_usp_error_code code);

#ifdef __cplusplus
}
#endif
#endif
