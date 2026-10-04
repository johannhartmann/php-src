#ifndef ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H
#define ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H

#include "../Core/zend_mir_lowering_internal.h"
#include "../zend_mir_control_flow.h"
#include "../../MIR/ControlFlow/zend_mir_control_flow_internal.h"

/* Values are the live zend_vm_opcodes.h opcode numbers. */
enum {
	ZEND_MIR_OPCODE_NOP = 0,
	ZEND_MIR_OPCODE_JMP = 42,
	ZEND_MIR_OPCODE_JMPZ = 43,
	ZEND_MIR_OPCODE_JMPNZ = 44,
	ZEND_MIR_OPCODE_JMPZ_EX = 46,
	ZEND_MIR_OPCODE_JMPNZ_EX = 47,
	ZEND_MIR_OPCODE_CATCH = 107,
	ZEND_MIR_OPCODE_FAST_CALL = 162,
	ZEND_MIR_OPCODE_FAST_RET = 163,
	ZEND_MIR_OPCODE_JMP_SET = 152,
	ZEND_MIR_OPCODE_COALESCE = 169,
	ZEND_MIR_OPCODE_FE_RESET_R = 77,
	ZEND_MIR_OPCODE_FE_FETCH_R = 78,
	ZEND_MIR_OPCODE_FE_RESET_RW = 125,
	ZEND_MIR_OPCODE_FE_FETCH_RW = 126,
	ZEND_MIR_OPCODE_JMP_NULL = 198,
	ZEND_MIR_SOURCE_OPCODE_THROW = 108,
	ZEND_MIR_OPCODE_ASSERT_CHECK = 151,
	ZEND_MIR_OPCODE_SWITCH_LONG = 187,
	ZEND_MIR_OPCODE_SWITCH_STRING = 188,
	ZEND_MIR_OPCODE_MATCH = 195,
	ZEND_MIR_OPCODE_BIND_INIT_STATIC_OR_JMP = 203,
	ZEND_MIR_OPCODE_JMP_FRAMELESS = 208
};

typedef struct _zend_mir_validation {
	uint32_t proofs;
	zend_mir_source_block_id entry_block_id;
	zend_mir_lowering_diagnostic_code diagnostic;
} zend_mir_validation;

bool zend_mir_validate_source(
	const zend_mir_lowering_source_view *source,
	zend_mir_validation *validation);
bool zend_mir_validate_source_for_protected_control_flow(
	const zend_mir_lowering_source_view *source,
	zend_mir_validation *validation);
zend_mir_branch_kind zend_mir_branch_kind_for_opcode(uint32_t opcode);
bool zend_mir_branch_edge_count_is_valid(
	zend_mir_branch_kind kind, uint32_t opcode, uint32_t edge_count);
bool zend_mir_emit_terminator(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	const zend_mir_source_edge_ref *edges,
	uint32_t edge_count,
	bool machine_condition,
	zend_mir_control_flow_map_storage *map);

#endif /* ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H */
