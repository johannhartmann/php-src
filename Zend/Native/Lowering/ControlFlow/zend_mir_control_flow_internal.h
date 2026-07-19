#ifndef ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H
#define ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H

#include "../Core/zend_mir_lowering_internal.h"
#include "../zend_mir_control_flow.h"
#include "../../MIR/ControlFlow/zend_mir_control_flow_internal.h"

/* Values are the live zend_vm_opcodes.h identities frozen by the W04 profile. */
enum {
	ZEND_MIR_W04_OPCODE_JMP = 42,
	ZEND_MIR_W04_OPCODE_JMPZ = 43,
	ZEND_MIR_W04_OPCODE_JMPNZ = 44,
	ZEND_MIR_W04_OPCODE_JMPZ_EX = 46,
	ZEND_MIR_W04_OPCODE_JMPNZ_EX = 47
};

typedef struct _zend_mir_w04_validation {
	uint32_t proofs;
	zend_mir_source_block_id entry_block_id;
	zend_mir_lowering_diagnostic_code diagnostic;
} zend_mir_w04_validation;

bool zend_mir_w04_validate_source(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation);
zend_mir_w04_branch_kind zend_mir_w04_branch_kind_for_opcode(uint32_t opcode);
bool zend_mir_w04_validate_branch_proofs(
	const zend_mir_lowering_context *context);
bool zend_mir_w04_emit_terminator(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	zend_mir_control_flow_map_storage *map);

#endif /* ZEND_MIR_LOWERING_CONTROL_FLOW_INTERNAL_H */
