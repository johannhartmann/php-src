#ifndef ZEND_MIR_LOWERING_SOURCE_H
#define ZEND_MIR_LOWERING_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "../MIR/zend_mir_ids.h"

typedef enum _zend_mir_source_operand_kind {
	ZEND_MIR_SOURCE_OPERAND_UNUSED = 0,
	ZEND_MIR_SOURCE_OPERAND_LITERAL = 1,
	ZEND_MIR_SOURCE_OPERAND_SLOT = 2,
	ZEND_MIR_SOURCE_OPERAND_SSA = 3,
	ZEND_MIR_SOURCE_OPERAND_KIND_INVALID = -1
} zend_mir_source_operand_kind;

typedef enum _zend_mir_source_slot_kind {
	ZEND_MIR_SOURCE_SLOT_CV = 0,
	ZEND_MIR_SOURCE_SLOT_TMP = 1,
	ZEND_MIR_SOURCE_SLOT_VAR = 2,
	ZEND_MIR_SOURCE_SLOT_KIND_INVALID = -1
} zend_mir_source_slot_kind;

typedef enum _zend_mir_source_literal_kind {
	ZEND_MIR_SOURCE_LITERAL_NULL = 0,
	ZEND_MIR_SOURCE_LITERAL_FALSE = 1,
	ZEND_MIR_SOURCE_LITERAL_TRUE = 2,
	ZEND_MIR_SOURCE_LITERAL_LONG_BITS = 3,
	ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS = 4,
	ZEND_MIR_SOURCE_LITERAL_KIND_INVALID = -1
} zend_mir_source_literal_kind;

typedef enum _zend_mir_source_block_flag {
	ZEND_MIR_SOURCE_BLOCK_ENTRY = UINT32_C(1) << 0,
	ZEND_MIR_SOURCE_BLOCK_REACHABLE = UINT32_C(1) << 1,
	ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER = UINT32_C(1) << 2,
	ZEND_MIR_SOURCE_BLOCK_PROTECTED = UINT32_C(1) << 3,
	ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE = UINT32_C(1) << 4,
	ZEND_MIR_SOURCE_BLOCK_CATCH_ENTRY = UINT32_C(1) << 5,
	ZEND_MIR_SOURCE_BLOCK_FINALLY_ENTRY = UINT32_C(1) << 6
} zend_mir_source_block_flag;

typedef enum _zend_mir_source_edge_flag {
	ZEND_MIR_SOURCE_EDGE_FALLTHROUGH = UINT32_C(1) << 0,
	ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP = UINT32_C(1) << 1,
	ZEND_MIR_SOURCE_EDGE_BACKEDGE = UINT32_C(1) << 2,
	ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY = UINT32_C(1) << 3
} zend_mir_source_edge_flag;

typedef enum _zend_mir_source_phi_kind {
	ZEND_MIR_SOURCE_PHI_MERGE = 0,
	ZEND_MIR_SOURCE_PHI_PI_TYPE = 1,
	ZEND_MIR_SOURCE_PHI_PI_RANGE = 2,
	ZEND_MIR_SOURCE_PHI_KIND_INVALID = -1
} zend_mir_source_phi_kind;

typedef enum _zend_mir_source_phi_constraint_flag {
	ZEND_MIR_SOURCE_PHI_RANGE_MIN_UNBOUNDED = UINT32_C(1) << 0,
	ZEND_MIR_SOURCE_PHI_RANGE_MAX_UNBOUNDED = UINT32_C(1) << 1,
	ZEND_MIR_SOURCE_PHI_RANGE_NEGATED = UINT32_C(1) << 2
} zend_mir_source_phi_constraint_flag;

typedef struct _zend_mir_source_operand_ref {
	zend_mir_source_operand_kind kind;
	zend_mir_source_slot_kind slot_kind;
	uint32_t index;
	uint32_t ssa_variable_id;
} zend_mir_source_operand_ref;

typedef struct _zend_mir_source_opcode_ref {
	uint32_t opline_index;
	uint32_t zend_opcode_number;
	zend_mir_source_operand_ref op1;
	zend_mir_source_operand_ref op2;
	zend_mir_source_operand_ref result;
	uint32_t extended_value;
	zend_mir_source_position_id source_position_id;
	zend_mir_source_block_id block_id;
} zend_mir_source_opcode_ref;

typedef struct _zend_mir_source_ssa_ref {
	uint32_t ssa_variable_id;
	uint32_t definition_opline_index;
	uint32_t source_slot;
	zend_mir_source_slot_kind source_slot_kind;
} zend_mir_source_ssa_ref;

typedef struct _zend_mir_source_ssa_use_ref {
	uint32_t ssa_variable_id;
	uint32_t opline_index;
	uint32_t operand_index;
} zend_mir_source_ssa_use_ref;

typedef struct _zend_mir_source_ssa_def_ref {
	uint32_t ssa_variable_id;
	uint32_t opline_index;
	zend_mir_source_operand_ref destination;
} zend_mir_source_ssa_def_ref;

typedef struct _zend_mir_source_literal_ref {
	uint32_t literal_index;
	zend_mir_source_literal_kind kind;
	uint64_t payload_bits;
} zend_mir_source_literal_ref;

typedef struct _zend_mir_source_block_ref {
	zend_mir_source_block_id id;
	uint32_t first_opcode_ordinal;
	uint32_t opcode_count;
	uint32_t flags;
	zend_mir_source_block_id immediate_dominator;
	zend_mir_source_block_id loop_header;
} zend_mir_source_block_ref;

typedef struct _zend_mir_source_edge_ref {
	zend_mir_source_edge_id id;
	zend_mir_source_block_id from_block_id;
	zend_mir_source_block_id to_block_id;
	uint32_t successor_index;
	uint32_t predecessor_index;
	uint32_t flags;
} zend_mir_source_edge_ref;

/*
 * Constraint fields are pointer-free snapshots. Type Pi records use type_mask;
 * range Pi records use the range and optional symbolic SSA bounds. Unused IDs
 * are ZEND_MIR_ID_INVALID.
 */
typedef struct _zend_mir_source_phi_constraint {
	uint32_t type_mask;
	int64_t range_min;
	int64_t range_max;
	uint32_t min_ssa_variable_id;
	uint32_t max_ssa_variable_id;
	uint32_t flags;
} zend_mir_source_phi_constraint;

typedef struct _zend_mir_source_phi_ref {
	zend_mir_source_phi_id id;
	zend_mir_source_block_id block_id;
	uint32_t result_ssa_variable_id;
	zend_mir_source_slot_kind source_slot_kind;
	uint32_t source_slot_index;
	zend_mir_source_phi_kind kind;
	zend_mir_source_phi_constraint constraint;
} zend_mir_source_phi_ref;

typedef struct _zend_mir_source_phi_input_ref {
	zend_mir_source_phi_id phi_id;
	uint32_t input_index;
	zend_mir_source_block_id predecessor_block_id;
	uint32_t source_ssa_variable_id;
} zend_mir_source_phi_input_ref;

typedef struct _zend_mir_lowering_source_view {
	uint32_t contract_version;
	const void *context;
	uint32_t (*opcode_count)(const void *context);
	bool (*opcode_at)(const void *context, uint32_t index, zend_mir_source_opcode_ref *out);
	uint32_t (*ssa_count)(const void *context);
	bool (*ssa_at)(const void *context, uint32_t index, zend_mir_source_ssa_ref *out);
	uint32_t (*ssa_use_count)(const void *context);
	bool (*ssa_use_at)(const void *context, uint32_t index, zend_mir_source_ssa_use_ref *out);
	uint32_t (*ssa_def_count)(const void *context);
	bool (*ssa_def_at)(const void *context, uint32_t index, zend_mir_source_ssa_def_ref *out);
	uint32_t (*literal_count)(const void *context);
	bool (*literal_at)(const void *context, uint32_t index, zend_mir_source_literal_ref *out);
	uint32_t (*block_count)(const void *context);
	bool (*block_at)(const void *context, uint32_t index, zend_mir_source_block_ref *out);
	uint32_t (*edge_count)(const void *context);
	bool (*edge_at)(const void *context, uint32_t index, zend_mir_source_edge_ref *out);
	uint32_t (*phi_count)(const void *context);
	bool (*phi_at)(const void *context, uint32_t index, zend_mir_source_phi_ref *out);
	uint32_t (*phi_input_count)(const void *context);
	bool (*phi_input_at)(const void *context, uint32_t index,
		zend_mir_source_phi_input_ref *out);
} zend_mir_lowering_source_view;

#endif /* ZEND_MIR_LOWERING_SOURCE_H */
