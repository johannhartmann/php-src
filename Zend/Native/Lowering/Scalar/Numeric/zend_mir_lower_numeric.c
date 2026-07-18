/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <string.h>

#include "zend_mir_lower_numeric.h"

typedef struct _zend_mir_numeric_plan {
	zend_mir_opcode opcode;
	zend_mir_representation representation;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_value_fact_flags fact_flags;
	zend_mir_numeric_range result_range;
	zend_mir_value_id operands[2];
	uint32_t operand_count;
	zend_mir_value_id result_id;
	zend_mir_source_position_ref source_position;
} zend_mir_numeric_plan;

static void zend_mir_numeric_set_diagnostic(
	zend_mir_lowering_diagnostic_code *diagnostic_out,
	zend_mir_lowering_diagnostic_code diagnostic)
{
	if (diagnostic_out != NULL) {
		*diagnostic_out = diagnostic;
	}
}

static bool zend_mir_numeric_fact_contract_is_valid(
	const zend_mir_value_fact_ref *fact)
{
	const zend_mir_value_fact_flags known_flags =
		ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NONZERO
		| ZEND_MIR_VALUE_FACT_FINITE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;

	if (fact == NULL || !zend_mir_id_is_valid(fact->value_id)
			|| !zend_mir_scalar_type_is_exact(fact->exact_type)
			|| (fact->flags & ~known_flags) != 0
			|| fact->provenance < ZEND_MIR_FACT_PROVENANCE_SSA
			|| fact->provenance >= ZEND_MIR_FACT_PROVENANCE_COUNT) {
		return false;
	}
	if ((fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
			&& (fact->exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| fact->integer_min > fact->integer_max)) {
		return false;
	}
	if ((fact->flags & ZEND_MIR_VALUE_FACT_NONZERO) != 0
			&& (fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
			&& fact->integer_min <= 0 && fact->integer_max >= 0) {
		return false;
	}
	return true;
}

static bool zend_mir_numeric_resolve_fact(
	const zend_mir_numeric_provider_context *provider_context,
	const zend_mir_source_operand_ref *operand, zend_mir_value_id *value_id_out,
	zend_mir_value_fact_ref *fact_out)
{
	if (provider_context == NULL || operand == NULL || value_id_out == NULL
			|| fact_out == NULL || provider_context->resolve_operand == NULL
			|| provider_context->value_fact == NULL
			|| !provider_context->resolve_operand(
				provider_context->source_context, operand, value_id_out)
			|| !zend_mir_id_is_valid(*value_id_out)
			|| !provider_context->value_fact(
				provider_context->source_context, *value_id_out, fact_out)
			|| fact_out->value_id != *value_id_out) {
		return false;
	}
	return true;
}

static bool zend_mir_numeric_validate_use(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_opcode_ref *source_opcode,
	const zend_mir_source_operand_ref *operand, uint32_t operand_index)
{
	uint32_t matching_uses = 0;
	uint32_t index;

	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return true;
	}
	if (source == NULL || source->ssa_use_count == NULL
			|| source->ssa_use_at == NULL) {
		return false;
	}
	for (index = 0; index < source->ssa_use_count(source->context); index++) {
		zend_mir_source_ssa_use_ref use;

		if (!source->ssa_use_at(source->context, index, &use)) {
			return false;
		}
		if (use.ssa_variable_id == operand->ssa_variable_id
				&& use.opline_index == source_opcode->opline_index) {
			if (use.operand_index != operand_index || ++matching_uses != 1) {
				return false;
			}
		}
	}
	return matching_uses == 1;
}

static bool zend_mir_numeric_validate_definition(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_opcode_ref *source_opcode)
{
	uint32_t matching_definitions = 0;
	uint32_t index;

	if (source == NULL || source->ssa_def_count == NULL
			|| source->ssa_def_at == NULL) {
		return false;
	}
	for (index = 0; index < source->ssa_def_count(source->context); index++) {
		zend_mir_source_ssa_def_ref definition;

		if (!source->ssa_def_at(source->context, index, &definition)) {
			return false;
		}
		if (definition.ssa_variable_id
				== source_opcode->result.ssa_variable_id
				&& definition.opline_index == source_opcode->opline_index) {
			if (++matching_definitions != 1) {
				return false;
			}
		}
	}
	return matching_definitions == 1;
}

static bool zend_mir_numeric_fact_range(
	const zend_mir_value_fact_ref *fact, zend_mir_numeric_range *range_out)
{
	if (fact == NULL || range_out == NULL
			|| fact->exact_type != ZEND_MIR_SCALAR_TYPE_I64
			|| (fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) == 0
			|| fact->integer_min > fact->integer_max) {
		return false;
	}
	range_out->minimum = fact->integer_min;
	range_out->maximum = fact->integer_max;
	return true;
}

static zend_mir_numeric_proof_mask zend_mir_numeric_required_proofs(
	uint32_t zend_opcode_number)
{
	zend_mir_numeric_proof_mask required =
		ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK
		| ZEND_MIR_NUMERIC_PROOF_NO_CALLS
		| ZEND_MIR_NUMERIC_PROOF_NO_REENTRY;

	switch (zend_opcode_number) {
		case ZEND_MIR_NUMERIC_OPCODE_ADD:
		case ZEND_MIR_NUMERIC_OPCODE_SUB:
		case ZEND_MIR_NUMERIC_OPCODE_MUL:
		case ZEND_MIR_NUMERIC_OPCODE_MOD:
			required |= ZEND_MIR_NUMERIC_PROOF_NO_DESTRUCTOR
				| ZEND_MIR_NUMERIC_PROOF_NO_EXCEPTION;
			break;
		default:
			break;
	}
	return required;
}

static zend_mir_lowering_status zend_mir_numeric_check_environment(
	const zend_mir_numeric_provider_context *provider_context,
	uint32_t zend_opcode_number,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_numeric_hazard_mask reference_hazards =
		ZEND_MIR_NUMERIC_HAZARD_REFERENCE | ZEND_MIR_NUMERIC_HAZARD_COW;
	const zend_mir_numeric_hazard_mask runtime_hazards =
		ZEND_MIR_NUMERIC_HAZARD_STRING | ZEND_MIR_NUMERIC_HAZARD_OBJECT
		| ZEND_MIR_NUMERIC_HAZARD_ARRAY | ZEND_MIR_NUMERIC_HAZARD_HELPER
		| ZEND_MIR_NUMERIC_HAZARD_CALL | ZEND_MIR_NUMERIC_HAZARD_REENTRY
		| ZEND_MIR_NUMERIC_HAZARD_DESTRUCTOR
		| ZEND_MIR_NUMERIC_HAZARD_EXCEPTION;
	zend_mir_numeric_proof_mask required =
		zend_mir_numeric_required_proofs(zend_opcode_number);

	if ((provider_context->proofs & required) != required) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MISSING_PROOF);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if ((provider_context->hazards & reference_hazards) != 0) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if ((provider_context->hazards & runtime_hazards) != 0) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_numeric_prepare_operands(
	const zend_mir_source_opcode_ref *source_opcode,
	const zend_mir_numeric_provider_context *provider_context,
	zend_mir_numeric_plan *plan, zend_mir_value_fact_ref facts[2],
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	bool unary = source_opcode->zend_opcode_number
		== ZEND_MIR_NUMERIC_OPCODE_BW_NOT;
	uint32_t index;

	if (source_opcode->op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| source_opcode->result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| source_opcode->result.ssa_variable_id
				> ZEND_MIR_VALUE_ORIGINAL_MAX
			|| (unary
				? source_opcode->op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
				: source_opcode->op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_INVALID_SOURCE);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	plan->operand_count = unary ? 1 : 2;
	for (index = 0; index < plan->operand_count; index++) {
		const zend_mir_source_operand_ref *operand =
			index == 0 ? &source_opcode->op1 : &source_opcode->op2;

		if (!zend_mir_numeric_validate_use(
				provider_context->source, source_opcode, operand, index)) {
			zend_mir_numeric_set_diagnostic(
				diagnostic_out, ZEND_MIRL_INVALID_SOURCE);
			return ZEND_MIR_LOWERING_REJECTED;
		}
		if (!zend_mir_numeric_resolve_fact(
				provider_context, operand, &plan->operands[index],
				&facts[index])) {
			zend_mir_numeric_set_diagnostic(
				diagnostic_out, ZEND_MIRL_MISSING_PROOF);
			return ZEND_MIR_LOWERING_REJECTED;
		}
		if (!zend_mir_numeric_fact_contract_is_valid(&facts[index])) {
			zend_mir_numeric_set_diagnostic(
				diagnostic_out, ZEND_MIRL_CONTRADICTORY_FACT);
			return ZEND_MIR_LOWERING_REJECTED;
		}
		if ((facts[index].flags
				& ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
			zend_mir_numeric_set_diagnostic(
				diagnostic_out, ZEND_MIRL_MISSING_PROOF);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	if (!zend_mir_numeric_validate_definition(
			provider_context->source, source_opcode)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_INVALID_SOURCE);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	plan->result_id = zend_mir_value_from_original_ssa(
		source_opcode->result.ssa_variable_id);
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_numeric_prepare_arithmetic(
	uint32_t zend_opcode_number, const zend_mir_value_fact_ref facts[2],
	zend_mir_numeric_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_numeric_range left;
	zend_mir_numeric_range right;
	bool proven;

	if (facts[0].exact_type != facts[1].exact_type
			|| (facts[0].exact_type != ZEND_MIR_SCALAR_TYPE_I64
				&& facts[0].exact_type != ZEND_MIR_SCALAR_TYPE_F64)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	plan->exact_type = facts[0].exact_type;
	plan->fact_flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	if (facts[0].exact_type == ZEND_MIR_SCALAR_TYPE_F64) {
		plan->representation = ZEND_MIR_REPRESENTATION_DOUBLE;
		switch (zend_opcode_number) {
			case ZEND_MIR_NUMERIC_OPCODE_ADD:
				plan->opcode = ZEND_MIR_OPCODE_F64_ADD;
				break;
			case ZEND_MIR_NUMERIC_OPCODE_SUB:
				plan->opcode = ZEND_MIR_OPCODE_F64_SUB;
				break;
			default:
				plan->opcode = ZEND_MIR_OPCODE_F64_MUL;
				break;
		}
		return ZEND_MIR_LOWERING_SUCCESS;
	}
	if (!zend_mir_numeric_fact_range(&facts[0], &left)
			|| !zend_mir_numeric_fact_range(&facts[1], &right)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MISSING_PROOF);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	switch (zend_opcode_number) {
		case ZEND_MIR_NUMERIC_OPCODE_ADD:
			plan->opcode = ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW;
			proven = zend_mir_numeric_range_add(
				left, right, &plan->result_range);
			break;
		case ZEND_MIR_NUMERIC_OPCODE_SUB:
			plan->opcode = ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW;
			proven = zend_mir_numeric_range_subtract(
				left, right, &plan->result_range);
			break;
		default:
			plan->opcode = ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW;
			proven = zend_mir_numeric_range_multiply(
				left, right, &plan->result_range);
			break;
	}
	if (!proven) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MISSING_PROOF);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	plan->representation = ZEND_MIR_REPRESENTATION_I64;
	plan->fact_flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_numeric_prepare_integer(
	uint32_t zend_opcode_number, const zend_mir_value_fact_ref facts[2],
	zend_mir_numeric_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_numeric_range left;
	zend_mir_numeric_range right;
	bool proven;

	if (facts[0].exact_type != ZEND_MIR_SCALAR_TYPE_I64
			|| facts[1].exact_type != ZEND_MIR_SCALAR_TYPE_I64) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (!zend_mir_numeric_fact_range(&facts[0], &left)
			|| !zend_mir_numeric_fact_range(&facts[1], &right)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MISSING_PROOF);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	plan->representation = ZEND_MIR_REPRESENTATION_I64;
	plan->exact_type = ZEND_MIR_SCALAR_TYPE_I64;
	plan->fact_flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	switch (zend_opcode_number) {
		case ZEND_MIR_NUMERIC_OPCODE_MOD:
			plan->opcode = ZEND_MIR_OPCODE_I64_MOD_NONZERO;
			proven = zend_mir_numeric_modulo_is_safe(left, right);
			break;
		case ZEND_MIR_NUMERIC_OPCODE_SL:
			plan->opcode = ZEND_MIR_OPCODE_I64_SHL_CHECKED;
			proven = zend_mir_numeric_shift_left(
				left, right, &plan->result_range);
			if (proven) {
				plan->fact_flags |=
					ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
			}
			break;
		default:
			plan->opcode = ZEND_MIR_OPCODE_I64_SHR_CHECKED;
			proven = zend_mir_numeric_shift_right(
				left, right, &plan->result_range);
			if (proven) {
				plan->fact_flags |=
					ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
			}
			break;
	}
	if (!proven) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MISSING_PROOF);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_numeric_prepare_bitwise(
	uint32_t zend_opcode_number, const zend_mir_value_fact_ref facts[2],
	zend_mir_numeric_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	if (facts[0].exact_type != ZEND_MIR_SCALAR_TYPE_I64
			|| (plan->operand_count == 2
				&& facts[1].exact_type != ZEND_MIR_SCALAR_TYPE_I64)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	plan->representation = ZEND_MIR_REPRESENTATION_I64;
	plan->exact_type = ZEND_MIR_SCALAR_TYPE_I64;
	plan->fact_flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	switch (zend_opcode_number) {
		case ZEND_MIR_NUMERIC_OPCODE_BW_OR:
			plan->opcode = ZEND_MIR_OPCODE_I64_BIT_OR;
			break;
		case ZEND_MIR_NUMERIC_OPCODE_BW_AND:
			plan->opcode = ZEND_MIR_OPCODE_I64_BIT_AND;
			break;
		case ZEND_MIR_NUMERIC_OPCODE_BW_XOR:
			plan->opcode = ZEND_MIR_OPCODE_I64_BIT_XOR;
			break;
		default:
			plan->opcode = ZEND_MIR_OPCODE_I64_BIT_NOT;
			break;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static bool zend_mir_numeric_is_supported(uint32_t zend_opcode_number)
{
	switch (zend_opcode_number) {
		case ZEND_MIR_NUMERIC_OPCODE_ADD:
		case ZEND_MIR_NUMERIC_OPCODE_SUB:
		case ZEND_MIR_NUMERIC_OPCODE_MUL:
		case ZEND_MIR_NUMERIC_OPCODE_MOD:
		case ZEND_MIR_NUMERIC_OPCODE_SL:
		case ZEND_MIR_NUMERIC_OPCODE_SR:
		case ZEND_MIR_NUMERIC_OPCODE_BW_OR:
		case ZEND_MIR_NUMERIC_OPCODE_BW_AND:
		case ZEND_MIR_NUMERIC_OPCODE_BW_XOR:
		case ZEND_MIR_NUMERIC_OPCODE_BW_NOT:
			return true;
		default:
			return false;
	}
}

static zend_mir_lowering_status zend_mir_numeric_build_plan(
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_numeric_provider_context *provider_context,
	zend_mir_numeric_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_value_fact_ref facts[2];
	zend_mir_lowering_status status;

	memset(plan, 0, sizeof(*plan));
	memset(facts, 0, sizeof(facts));
	status = zend_mir_numeric_check_environment(
		provider_context, source_opcode->zend_opcode_number, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	status = zend_mir_numeric_prepare_operands(
		source_opcode, provider_context, plan, facts, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	switch (source_opcode->zend_opcode_number) {
		case ZEND_MIR_NUMERIC_OPCODE_ADD:
		case ZEND_MIR_NUMERIC_OPCODE_SUB:
		case ZEND_MIR_NUMERIC_OPCODE_MUL:
			status = zend_mir_numeric_prepare_arithmetic(
				source_opcode->zend_opcode_number, facts, plan,
				diagnostic_out);
			break;
		case ZEND_MIR_NUMERIC_OPCODE_MOD:
		case ZEND_MIR_NUMERIC_OPCODE_SL:
		case ZEND_MIR_NUMERIC_OPCODE_SR:
			status = zend_mir_numeric_prepare_integer(
				source_opcode->zend_opcode_number, facts, plan,
				diagnostic_out);
			break;
		default:
			status = zend_mir_numeric_prepare_bitwise(
				source_opcode->zend_opcode_number, facts, plan,
				diagnostic_out);
			break;
	}
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (provider_context->source_position == NULL
			|| !zend_mir_id_is_valid(source_opcode->source_position_id)
			|| !provider_context->source_position(
				provider_context->source_context,
				source_opcode->source_position_id, &plan->source_position)
			|| plan->source_position.id
				!= source_opcode->source_position_id) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_INVALID_SOURCE);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_numeric_emit(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator, const zend_mir_numeric_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_instruction_record instruction = {
		.id = ZEND_MIR_ID_INVALID,
		.block_id = ZEND_MIR_ID_INVALID,
		.opcode = ZEND_MIR_OPCODE_INVALID,
		.representation = ZEND_MIR_REPRESENTATION_INVALID,
		.result_id = ZEND_MIR_ID_INVALID,
		.frame_state_id = ZEND_MIR_ID_INVALID,
		.source_position_id = ZEND_MIR_ID_INVALID,
		.effects = 0,
		.reads = 0,
		.writes = 0,
		.barriers = 0,
		.ownership_actions = 0
	};
	zend_mir_value_fact_ref fact = {
		.id = ZEND_MIR_ID_INVALID,
		.value_id = ZEND_MIR_ID_INVALID,
		.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
		.flags = 0,
		.integer_min = 0,
		.integer_max = 0,
		.provenance = ZEND_MIR_FACT_PROVENANCE_INVALID,
		.provenance_source_position_id = ZEND_MIR_ID_INVALID
	};
	zend_mir_source_position_id source_position_id;
	zend_mir_instruction_id instruction_id;
	zend_mir_value_fact_id fact_id;
	uint32_t index;

	if (!mutator->add_source_position(
			mutator->context, &plan->source_position, &source_position_id)
			|| source_position_id != source_opcode->source_position_id
			|| !mutator->add_value(
				mutator->context, plan->result_id, plan->representation,
				ZEND_MIR_OWNERSHIP_STATE_OWNED)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MUTATION_FAILED);
		return ZEND_MIR_LOWERING_FAILED;
	}
	instruction.block_id = zend_mir_lowering_context_block_id(context);
	instruction.opcode = plan->opcode;
	instruction.representation = plan->representation;
	instruction.result_id = plan->result_id;
	instruction.source_position_id = source_position_id;
	if (!zend_mir_id_is_valid(instruction.block_id)
			|| !mutator->add_instruction(
				mutator->context, &instruction, &instruction_id)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MUTATION_FAILED);
		return ZEND_MIR_LOWERING_FAILED;
	}
	for (index = 0; index < plan->operand_count; index++) {
		if (!mutator->add_operand(
				mutator->context, instruction_id, plan->operands[index])) {
			zend_mir_numeric_set_diagnostic(
				diagnostic_out, ZEND_MIRL_MUTATION_FAILED);
			return ZEND_MIR_LOWERING_FAILED;
		}
	}
	fact.value_id = plan->result_id;
	fact.exact_type = plan->exact_type;
	fact.flags = plan->fact_flags;
	fact.integer_min = plan->result_range.minimum;
	fact.integer_max = plan->result_range.maximum;
	fact.provenance =
		(plan->fact_flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
		? ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS
		: ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS;
	fact.provenance_source_position_id = source_position_id;
	if (!mutator->add_value_fact(mutator->context, &fact, &fact_id)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_MUTATION_FAILED);
		return ZEND_MIR_LOWERING_FAILED;
	}
	zend_mir_numeric_set_diagnostic(diagnostic_out, ZEND_MIRL_OK);
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_lower_numeric(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_numeric_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_numeric_plan plan;
	zend_mir_lowering_status status;

	zend_mir_numeric_set_diagnostic(
		diagnostic_out, ZEND_MIRL_INVALID_SOURCE);
	if (context == NULL || source_opcode == NULL || mutator == NULL
			|| provider_context == NULL
			|| provider_context->source == NULL
			|| provider_context->source->contract_version
				!= ZEND_MIR_CONTRACT_VERSION
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (!zend_mir_numeric_is_supported(
			source_opcode->zend_opcode_number)) {
		zend_mir_numeric_set_diagnostic(
			diagnostic_out, ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (mutator->add_value == NULL || mutator->add_instruction == NULL
			|| mutator->add_operand == NULL
			|| mutator->add_source_position == NULL
			|| mutator->add_value_fact == NULL) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	status = zend_mir_numeric_build_plan(
		source_opcode, provider_context, &plan, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	return zend_mir_numeric_emit(
		context, source_opcode, mutator, &plan, diagnostic_out);
}
