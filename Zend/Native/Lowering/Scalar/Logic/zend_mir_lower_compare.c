/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license.      |
   +----------------------------------------------------------------------+
*/

#include "zend_mir_logic_internal.h"

static bool zend_mir_logic_is_equality(uint32_t opcode)
{
	return opcode == ZEND_MIR_LOGIC_ZEND_IS_IDENTICAL
		|| opcode == ZEND_MIR_LOGIC_ZEND_IS_NOT_IDENTICAL
		|| opcode == ZEND_MIR_LOGIC_ZEND_IS_EQUAL
		|| opcode == ZEND_MIR_LOGIC_ZEND_IS_NOT_EQUAL;
}

static bool zend_mir_logic_is_negated_equality(uint32_t opcode)
{
	return opcode == ZEND_MIR_LOGIC_ZEND_IS_NOT_IDENTICAL
		|| opcode == ZEND_MIR_LOGIC_ZEND_IS_NOT_EQUAL;
}

static zend_mir_opcode zend_mir_logic_compare_opcode(
	uint32_t source_opcode,
	zend_mir_scalar_type_mask type)
{
	if (zend_mir_logic_is_equality(source_opcode)) {
		if (type == ZEND_MIR_SCALAR_TYPE_I1) {
			return ZEND_MIR_OPCODE_I1_EQ;
		}
		return type == ZEND_MIR_SCALAR_TYPE_I64
			? ZEND_MIR_OPCODE_I64_EQ : ZEND_MIR_OPCODE_F64_EQ;
	}
	if (source_opcode == ZEND_MIR_LOGIC_ZEND_IS_SMALLER) {
		return type == ZEND_MIR_SCALAR_TYPE_I64
			? ZEND_MIR_OPCODE_I64_LT : ZEND_MIR_OPCODE_F64_LT;
	}
	if (source_opcode == ZEND_MIR_LOGIC_ZEND_IS_SMALLER_OR_EQUAL) {
		return type == ZEND_MIR_SCALAR_TYPE_I64
			? ZEND_MIR_OPCODE_I64_LE : ZEND_MIR_OPCODE_F64_LE;
	}
	return type == ZEND_MIR_SCALAR_TYPE_I64
		? ZEND_MIR_OPCODE_I64_CMP : ZEND_MIR_OPCODE_F64_CMP;
}

zend_mir_lowering_status zend_mir_logic_build_compare_plan(
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
	zend_mir_value_id first_result;
	zend_mir_opcode opcode;
	zend_mir_representation representation;
	zend_mir_scalar_type_mask result_type;
	zend_mir_value_fact_flags result_flags = 0;
	int64_t result_min = 0;
	int64_t result_max = 0;
	zend_mir_lowering_status status;
	uint32_t zend_opcode;

	status = zend_mir_logic_prepare(
		context, source_opcode, plan, &logic_context, &proof, &result_id,
		diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	zend_opcode = source_opcode->zend_opcode_number;
	if (!zend_mir_logic_is_equality(zend_opcode)
			&& zend_opcode != ZEND_MIR_LOGIC_ZEND_IS_SMALLER
			&& zend_opcode != ZEND_MIR_LOGIC_ZEND_IS_SMALLER_OR_EQUAL
			&& zend_opcode != ZEND_MIR_LOGIC_ZEND_SPACESHIP) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (!zend_mir_logic_require_proofs(
			proof, ZEND_MIR_LOGIC_COMMON_PROOFS
				| ZEND_MIR_LOGIC_PROOF_SAME_EXACT_TYPE,
			diagnostic_out)
			|| !zend_mir_logic_input_at(
				logic_context, &source_opcode->op1, &left, diagnostic_out)
			|| !zend_mir_logic_input_at(
				logic_context, &source_opcode->op2, &right, diagnostic_out)) {
		return *diagnostic_out == ZEND_MIRL_CONTRADICTORY_FACT
			|| *diagnostic_out == ZEND_MIRL_INVALID_SOURCE
			? ZEND_MIR_LOWERING_REJECTED : ZEND_MIR_LOWERING_DEFERRED;
	}
	if (left.fact.exact_type != right.fact.exact_type) {
		*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (!zend_mir_logic_require_finite(proof, &left, diagnostic_out)
			|| !zend_mir_logic_require_finite(proof, &right, diagnostic_out)) {
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (zend_mir_logic_is_equality(zend_opcode)) {
		if (left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1
				&& left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_I64
				&& left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_F64) {
			*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
			return ZEND_MIR_LOWERING_DEFERRED;
		}
	} else if (left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_I64
			&& left.fact.exact_type != ZEND_MIR_SCALAR_TYPE_F64) {
		*diagnostic_out = ZEND_MIRL_DEFERRED_OPCODE;
		return ZEND_MIR_LOWERING_DEFERRED;
	}

	operands[0] = left.value_id;
	operands[1] = right.value_id;
	opcode = zend_mir_logic_compare_opcode(zend_opcode, left.fact.exact_type);
	result_type = zend_opcode == ZEND_MIR_LOGIC_ZEND_SPACESHIP
		? ZEND_MIR_SCALAR_TYPE_I64 : ZEND_MIR_SCALAR_TYPE_I1;
	representation = result_type == ZEND_MIR_SCALAR_TYPE_I64
		? ZEND_MIR_REPRESENTATION_I64 : ZEND_MIR_REPRESENTATION_I1;
	if (result_type == ZEND_MIR_SCALAR_TYPE_I64) {
		result_flags = ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
		result_min = -1;
		result_max = 1;
	}
	first_result = result_id;
	if (zend_mir_logic_is_negated_equality(zend_opcode)) {
		first_result = proof->temporary_value_id;
		if (!zend_mir_value_is_synthetic(first_result)
				|| first_result == result_id
				|| first_result == left.value_id
				|| first_result == right.value_id) {
			*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
			return ZEND_MIR_LOWERING_DEFERRED;
		}
	}
	if (!zend_mir_logic_add_step(
			plan, opcode, representation, first_result, result_type,
			result_flags, result_min, result_max, operands, 2)) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (zend_mir_logic_is_negated_equality(zend_opcode)) {
		zend_mir_value_id not_operand = first_result;

		if (!zend_mir_logic_add_step(
				plan, ZEND_MIR_OPCODE_I1_NOT, ZEND_MIR_REPRESENTATION_I1,
				result_id, ZEND_MIR_SCALAR_TYPE_I1, 0, 0, 0,
				&not_operand, 1)) {
			*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	*diagnostic_out = ZEND_MIRL_OK;
	return ZEND_MIR_LOWERING_SUCCESS;
}
