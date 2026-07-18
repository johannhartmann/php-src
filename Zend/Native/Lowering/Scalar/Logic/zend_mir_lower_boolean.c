/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license.      |
   +----------------------------------------------------------------------+
*/

#include "zend_mir_logic_internal.h"

zend_mir_lowering_status zend_mir_logic_build_boolean_plan(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_logic_context *logic_context;
	const zend_mir_logic_opcode_proof *proof;
	zend_mir_logic_input left;
	zend_mir_logic_input right;
	zend_mir_value_id result_id;
	zend_mir_value_id operands[2];
	zend_mir_opcode opcode;
	zend_mir_lowering_status status;
	uint32_t operand_count = 1;
	zend_mir_logic_proof_mask required = ZEND_MIR_LOGIC_COMMON_PROOFS;

	status = zend_mir_logic_prepare(
		context, source_opcode, plan, &logic_context, &proof, &result_id,
		diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (source_opcode->zend_opcode_number == ZEND_MIR_LOGIC_ZEND_BOOL) {
		required |= ZEND_MIR_LOGIC_PROOF_SAFE_SCALAR_CAST;
	} else if (source_opcode->zend_opcode_number
			!= ZEND_MIR_LOGIC_ZEND_BOOL_NOT
			&& source_opcode->zend_opcode_number
				!= ZEND_MIR_LOGIC_ZEND_BOOL_XOR) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (!zend_mir_logic_require_proofs(proof, required, diagnostic_out)
			|| !zend_mir_logic_input_at(
				logic_context, &source_opcode->op1, &left,
				diagnostic_out)) {
		return *diagnostic_out == ZEND_MIRL_CONTRADICTORY_FACT
			|| *diagnostic_out == ZEND_MIRL_INVALID_SOURCE
			? ZEND_MIR_LOWERING_REJECTED : ZEND_MIR_LOWERING_DEFERRED;
	}
	operands[0] = left.value_id;
	if (source_opcode->zend_opcode_number == ZEND_MIR_LOGIC_ZEND_BOOL_XOR) {
		if (!zend_mir_logic_input_at(
				logic_context, &source_opcode->op2, &right,
				diagnostic_out)) {
			return *diagnostic_out == ZEND_MIRL_CONTRADICTORY_FACT
				|| *diagnostic_out == ZEND_MIRL_INVALID_SOURCE
				? ZEND_MIR_LOWERING_REJECTED : ZEND_MIR_LOWERING_DEFERRED;
		}
		if (left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1
				|| right.fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1) {
			*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		operands[1] = right.value_id;
		operand_count = 2;
		opcode = ZEND_MIR_OPCODE_I1_XOR;
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_LOGIC_ZEND_BOOL_NOT) {
		if (left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1) {
			*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		opcode = ZEND_MIR_OPCODE_I1_NOT;
	} else if (left.fact.exact_type == ZEND_MIR_SCALAR_TYPE_I64) {
		opcode = ZEND_MIR_OPCODE_I64_TO_I1;
	} else if (left.fact.exact_type == ZEND_MIR_SCALAR_TYPE_F64) {
		opcode = ZEND_MIR_OPCODE_F64_TO_I1;
	} else {
		*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (!zend_mir_logic_add_step(
			plan, opcode, ZEND_MIR_REPRESENTATION_I1, result_id,
			ZEND_MIR_SCALAR_TYPE_I1, 0, 0, 0, operands, operand_count)) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return ZEND_MIR_LOWERING_REJECTED;
	}
	*diagnostic_out = ZEND_MIRL_OK;
	return ZEND_MIR_LOWERING_SUCCESS;
}
