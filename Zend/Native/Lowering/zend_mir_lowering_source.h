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
} zend_mir_lowering_source_view;

#endif /* ZEND_MIR_LOWERING_SOURCE_H */
