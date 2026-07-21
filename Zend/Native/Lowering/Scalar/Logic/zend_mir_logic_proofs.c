/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   +----------------------------------------------------------------------+
*/

#include <string.h>

#include "zend_mir_logic_internal.h"

static bool zend_mir_logic_operand_equal(
	const zend_mir_source_operand_ref *left,
	const zend_mir_source_operand_ref *right)
{
	return left->kind == right->kind
		&& left->slot_kind == right->slot_kind
		&& left->index == right->index
		&& left->ssa_variable_id == right->ssa_variable_id;
}

static bool zend_mir_logic_fact_valid(
	const zend_mir_logic_value_binding *binding)
{
	const zend_mir_value_fact_ref *fact = &binding->fact;

	if (!binding->has_fact
			|| !zend_mir_id_is_valid(binding->value_id)
			|| fact->value_id != binding->value_id
			|| !zend_mir_scalar_type_is_exact(fact->exact_type)
			|| (fact->flags & ~(
				ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
				| ZEND_MIR_VALUE_FACT_NONZERO
				| ZEND_MIR_VALUE_FACT_FINITE
				| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED)) != 0
			|| fact->provenance < ZEND_MIR_FACT_PROVENANCE_SSA
			|| fact->provenance >= ZEND_MIR_FACT_PROVENANCE_COUNT) {
		return false;
	}
	if ((fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
			&& (fact->exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| fact->integer_min > fact->integer_max)) {
		return false;
	}
	return true;
}

zend_mir_lowering_status zend_mir_logic_fail(
	zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic)
{
	if (context != NULL) {
		(void) zend_mir_lowering_context_set_provider_failure(
			context, status, diagnostic);
	}
	return status;
}

bool zend_mir_logic_input_at(
	const zend_mir_logic_context *context,
	const zend_mir_source_operand_ref *source,
	zend_mir_logic_input *out,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	uint32_t index;
	const zend_mir_logic_value_binding *match = NULL;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
	}
	if (context == NULL || source == NULL || out == NULL
			|| diagnostic_out == NULL
			|| source->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| source->kind < ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| source->kind > ZEND_MIR_SOURCE_OPERAND_SSA) {
		return false;
	}
	for (index = 0; index < context->binding_count; index++) {
		const zend_mir_logic_value_binding *candidate = &context->bindings[index];

		if (!zend_mir_logic_operand_equal(&candidate->source, source)) {
			continue;
		}
		if (match != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
			return false;
		}
		match = candidate;
	}
	if (match == NULL) {
		*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		return false;
	}
	if (!zend_mir_logic_fact_valid(match)) {
		*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		return false;
	}
	out->value_id = match->value_id;
	out->fact = match->fact;
	*diagnostic_out = ZEND_MIRL_OK;
	return true;
}

static bool zend_mir_logic_result_id_at(
	const zend_mir_logic_context *context,
	const zend_mir_source_operand_ref *source,
	zend_mir_value_id *out,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	uint32_t index;
	const zend_mir_logic_value_binding *match = NULL;

	for (index = 0; index < context->binding_count; index++) {
		const zend_mir_logic_value_binding *candidate = &context->bindings[index];

		if (!zend_mir_logic_operand_equal(&candidate->source, source)) {
			continue;
		}
		if (match != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
			return false;
		}
		match = candidate;
	}
	if (match == NULL) {
		*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		return false;
	}
	if (!zend_mir_id_is_valid(match->value_id)) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
		return false;
	}
	*out = match->value_id;
	return true;
}

static bool zend_mir_logic_opcode_proof_at(
	const zend_mir_logic_context *context,
	uint32_t opline_index,
	const zend_mir_logic_opcode_proof **out,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	uint32_t index;
	const zend_mir_logic_opcode_proof *match = NULL;

	for (index = 0; index < context->opcode_proof_count; index++) {
		const zend_mir_logic_opcode_proof *candidate =
			&context->opcode_proofs[index];

		if (candidate->opline_index != opline_index) {
			continue;
		}
		if (match != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
			return false;
		}
		match = candidate;
	}
	if (match == NULL) {
		*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		return false;
	}
	if ((match->proofs
			& ~(ZEND_MIR_LOGIC_PROOF_ALL
				| ZEND_MIR_LOGIC_PROOF_SOURCE_CFG)) != 0) {
		*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		return false;
	}
	*out = match;
	return true;
}

zend_mir_lowering_status zend_mir_logic_prepare(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	const zend_mir_logic_context **logic_context_out,
	const zend_mir_logic_opcode_proof **proof_out,
	zend_mir_value_id *result_id_out,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_logic_context *logic_context;
	zend_mir_block_id block_id;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
	}
	if (context == NULL || source_opcode == NULL || plan == NULL
			|| logic_context_out == NULL || proof_out == NULL
			|| result_id_out == NULL || diagnostic_out == NULL
			|| source_opcode->result.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| !zend_mir_id_is_valid(source_opcode->source_position_id)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	logic_context = (const zend_mir_logic_context *)
		zend_mir_lowering_context_provider_context(context);
	block_id = zend_mir_lowering_context_block_id(context);
	if (logic_context == NULL || !zend_mir_id_is_valid(block_id)
			|| (logic_context->binding_count != 0
				&& logic_context->bindings == NULL)
			|| (logic_context->opcode_proof_count != 0
				&& logic_context->opcode_proofs == NULL)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (!zend_mir_logic_opcode_proof_at(
			logic_context, source_opcode->opline_index, proof_out,
			diagnostic_out)
			|| !zend_mir_logic_result_id_at(
				logic_context, &source_opcode->result, result_id_out,
				diagnostic_out)) {
		return *diagnostic_out == ZEND_MIRL_MISSING_PROOF
			? ZEND_MIR_LOWERING_DEFERRED : ZEND_MIR_LOWERING_REJECTED;
	}
	memset(plan, 0, sizeof(*plan));
	plan->block_id = block_id;
	plan->source_position_id = source_opcode->source_position_id;
	*logic_context_out = logic_context;
	*diagnostic_out = ZEND_MIRL_OK;
	return ZEND_MIR_LOWERING_SUCCESS;
}

bool zend_mir_logic_require_proofs(
	const zend_mir_logic_opcode_proof *proof,
	zend_mir_logic_proof_mask required,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_logic_proof_mask exact;
	bool cfg_required;
	if (proof == NULL || diagnostic_out == NULL) {
		return false;
	}
	exact = required & ~ZEND_MIR_LOGIC_PROOF_SINGLE_REACHABLE_BLOCK;
	cfg_required =
		(required & ZEND_MIR_LOGIC_PROOF_SINGLE_REACHABLE_BLOCK) != 0;
	if ((proof->proofs & exact) != exact
			|| (cfg_required
				&& (proof->proofs
					& (ZEND_MIR_LOGIC_PROOF_SINGLE_REACHABLE_BLOCK
						| ZEND_MIR_LOGIC_PROOF_SOURCE_CFG)) == 0)) {
		*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		return false;
	}
	return true;
}

bool zend_mir_logic_require_finite(
	const zend_mir_logic_opcode_proof *proof,
	const zend_mir_logic_input *input,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	if (input->fact.exact_type != ZEND_MIR_SCALAR_TYPE_F64) {
		return true;
	}
	if ((proof->proofs & ZEND_MIR_LOGIC_PROOF_FINITE_F64) == 0
			|| (input->fact.flags & ZEND_MIR_VALUE_FACT_FINITE) == 0) {
		*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		return false;
	}
	return true;
}

bool zend_mir_logic_add_step(
	zend_mir_logic_plan *plan,
	zend_mir_opcode opcode,
	zend_mir_representation representation,
	zend_mir_value_id result_id,
	zend_mir_scalar_type_mask exact_type,
	zend_mir_value_fact_flags fact_flags,
	int64_t integer_min,
	int64_t integer_max,
	const zend_mir_value_id *operands,
	uint32_t operand_count)
{
	zend_mir_logic_plan_step *step;
	uint32_t index;

	if (plan == NULL || plan->step_count >= ZEND_MIR_LOGIC_MAX_PLAN_STEPS
			|| opcode < ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
			|| opcode > ZEND_MIR_OPCODE_SCALAR_DROP
			|| representation <= ZEND_MIR_REPRESENTATION_CONTROL
			|| representation >= ZEND_MIR_REPRESENTATION_COUNT
			|| !zend_mir_id_is_valid(result_id)
			|| !zend_mir_scalar_type_is_exact(exact_type)
			|| operand_count == 0
			|| operand_count > ZEND_MIR_LOGIC_MAX_OPERANDS
			|| operands == NULL) {
		return false;
	}
	for (index = 0; index < operand_count; index++) {
		if (!zend_mir_id_is_valid(operands[index])) {
			return false;
		}
	}
	step = &plan->steps[plan->step_count++];
	memset(step, 0, sizeof(*step));
	step->opcode = opcode;
	step->representation = representation;
	step->result_id = result_id;
	step->result_fact.id = ZEND_MIR_ID_INVALID;
	step->result_fact.value_id = result_id;
	step->result_fact.exact_type = exact_type;
	step->result_fact.flags = fact_flags | ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	step->result_fact.integer_min = integer_min;
	step->result_fact.integer_max = integer_max;
	step->result_fact.provenance = ZEND_MIR_FACT_PROVENANCE_CONTRACT;
	step->result_fact.provenance_source_position_id =
		plan->source_position_id;
	memcpy(step->operands, operands, operand_count * sizeof(*operands));
	step->operand_count = operand_count;
	return true;
}
