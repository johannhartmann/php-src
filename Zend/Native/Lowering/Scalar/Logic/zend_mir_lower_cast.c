/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license.      |
   +----------------------------------------------------------------------+
*/

#include <stdint.h>

#include "zend_mir_logic_internal.h"

zend_mir_lowering_status zend_mir_logic_build_cast_plan(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_logic_context *logic_context;
	const zend_mir_logic_opcode_proof *proof;
	zend_mir_logic_input input;
	zend_mir_value_id result_id;
	zend_mir_opcode opcode;
	zend_mir_representation representation;
	zend_mir_scalar_type_mask result_type;
	zend_mir_value_fact_flags result_flags = 0;
	int64_t result_min = 0;
	int64_t result_max = 0;
	zend_mir_lowering_status status;

	status = zend_mir_logic_prepare(
		context, source_opcode, plan, &logic_context, &proof, &result_id,
		diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (source_opcode->zend_opcode_number != ZEND_MIR_LOGIC_ZEND_CAST) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (!zend_mir_logic_require_proofs(
			proof, ZEND_MIR_LOGIC_COMMON_PROOFS
				| ZEND_MIR_LOGIC_PROOF_SAFE_SCALAR_CAST
				| ZEND_MIR_LOGIC_PROOF_NO_DESTRUCTOR
				| ZEND_MIR_LOGIC_PROOF_NO_EXCEPTION,
			diagnostic_out)
			|| !zend_mir_logic_input_at(
				logic_context, &source_opcode->op1, &input,
				diagnostic_out)) {
		return *diagnostic_out == ZEND_MIRL_CONTRADICTORY_FACT
			|| *diagnostic_out == ZEND_MIRL_INVALID_SOURCE
			? ZEND_MIR_LOWERING_REJECTED : ZEND_MIR_LOWERING_DEFERRED;
	}
	if (!zend_mir_logic_require_finite(proof, &input, diagnostic_out)) {
		return ZEND_MIR_LOWERING_DEFERRED;
	}

	if (source_opcode->extended_value == ZEND_MIR_LOGIC_CAST_LONG) {
		result_type = ZEND_MIR_SCALAR_TYPE_I64;
		representation = ZEND_MIR_REPRESENTATION_I64;
		result_flags = ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
		if (input.fact.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			opcode = ZEND_MIR_OPCODE_I1_TO_I64;
			result_min = 0;
			result_max = 1;
		} else if (input.fact.exact_type == ZEND_MIR_SCALAR_TYPE_F64) {
			opcode = ZEND_MIR_OPCODE_F64_TO_I64_CHECKED;
			result_min = INT64_MIN;
			result_max = INT64_MAX;
		} else {
			*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
			return ZEND_MIR_LOWERING_DEFERRED;
		}
	} else if (source_opcode->extended_value
			== ZEND_MIR_LOGIC_CAST_DOUBLE) {
		result_type = ZEND_MIR_SCALAR_TYPE_F64;
		representation = ZEND_MIR_REPRESENTATION_DOUBLE;
		result_flags = ZEND_MIR_VALUE_FACT_FINITE;
		if (input.fact.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			opcode = ZEND_MIR_OPCODE_I1_TO_F64;
		} else if (input.fact.exact_type == ZEND_MIR_SCALAR_TYPE_I64) {
			opcode = ZEND_MIR_OPCODE_I64_TO_F64;
		} else {
			*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
			return ZEND_MIR_LOWERING_DEFERRED;
		}
	} else {
		*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (!zend_mir_logic_add_step(
			plan, opcode, representation, result_id, result_type,
			result_flags, result_min, result_max, &input.value_id, 1)) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return ZEND_MIR_LOWERING_REJECTED;
	}
	*diagnostic_out = ZEND_MIRL_OK;
	return ZEND_MIR_LOWERING_SUCCESS;
}
