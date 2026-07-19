/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license.      |
   +----------------------------------------------------------------------+
*/

#include <string.h>

#include "zend_mir_logic_internal.h"

static const uint32_t zend_mir_logic_claimed_opcodes[] = {
	ZEND_MIR_LOGIC_ZEND_BOOL_NOT,
	ZEND_MIR_LOGIC_ZEND_BOOL_XOR,
	ZEND_MIR_LOGIC_ZEND_IS_IDENTICAL,
	ZEND_MIR_LOGIC_ZEND_IS_NOT_IDENTICAL,
	ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
	ZEND_MIR_LOGIC_ZEND_IS_NOT_EQUAL,
	ZEND_MIR_LOGIC_ZEND_IS_SMALLER,
	ZEND_MIR_LOGIC_ZEND_IS_SMALLER_OR_EQUAL,
	ZEND_MIR_LOGIC_ZEND_CAST,
	ZEND_MIR_LOGIC_ZEND_BOOL,
	ZEND_MIR_LOGIC_ZEND_SPACESHIP
};

static uint32_t zend_mir_logic_claim_count(const void *context)
{
	return context != NULL
		? (uint32_t) (
			sizeof(zend_mir_logic_claimed_opcodes)
				/ sizeof(zend_mir_logic_claimed_opcodes[0]))
		: 0;
}

static bool zend_mir_logic_claim_at(
	const void *context,
	uint32_t index,
	zend_mir_lowering_claim *out)
{
	if (context == NULL || out == NULL
			|| index >= zend_mir_logic_claim_count(context)) {
		return false;
	}
	out->zend_opcode_number = zend_mir_logic_claimed_opcodes[index];
	out->semantic_family_id = ZEND_MIR_LOGIC_SEMANTIC_FAMILY_ID;
	return true;
}

static bool zend_mir_logic_mutator_valid(const zend_mir_mutator *mutator)
{
	return mutator != NULL
		&& zend_mir_contract_is_compatible(mutator->contract_version)
		&& mutator->add_value != NULL
		&& mutator->add_instruction != NULL
		&& mutator->add_operand != NULL
		&& mutator->add_value_fact != NULL;
}

static zend_mir_lowering_status zend_mir_logic_commit_plan(
	zend_mir_lowering_context *context,
	const zend_mir_logic_plan *plan,
	zend_mir_mutator *mutator)
{
	const zend_mir_logic_context *logic_context =
		zend_mir_lowering_context_provider_context(context);
	uint32_t step_index;

	if (!zend_mir_logic_mutator_valid(mutator) || logic_context == NULL
			|| plan == NULL
			|| plan->step_count == 0
			|| plan->step_count > ZEND_MIR_LOGIC_MAX_PLAN_STEPS) {
		return zend_mir_logic_fail(
			context, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE);
	}
	for (step_index = 0; step_index < plan->step_count; step_index++) {
		const zend_mir_logic_plan_step *step = &plan->steps[step_index];
		zend_mir_instruction_record instruction;
		zend_mir_instruction_id instruction_id;
		zend_mir_value_fact_id fact_id;
		uint32_t operand_index;

		if ((!logic_context->values_predeclared
				&& !mutator->add_value(
				mutator->context, step->result_id, step->representation,
				ZEND_MIR_OWNERSHIP_STATE_OWNED))) {
			return zend_mir_logic_fail(
				context, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_MUTATION_FAILED);
		}
		memset(&instruction, 0, sizeof(instruction));
		instruction.id = ZEND_MIR_ID_INVALID;
		instruction.block_id = plan->block_id;
		instruction.opcode = step->opcode;
		instruction.representation = step->representation;
		instruction.result_id = step->result_id;
		instruction.frame_state_id = ZEND_MIR_ID_INVALID;
		instruction.source_position_id = plan->source_position_id;
		if (!mutator->add_instruction(
				mutator->context, &instruction, &instruction_id)) {
			return zend_mir_logic_fail(
				context, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_MUTATION_FAILED);
		}
		for (operand_index = 0; operand_index < step->operand_count;
				operand_index++) {
			if (!mutator->add_operand(
					mutator->context, instruction_id,
					step->operands[operand_index])) {
				return zend_mir_logic_fail(
					context, ZEND_MIR_LOWERING_FAILED,
					ZEND_MIRL_MUTATION_FAILED);
			}
		}
		if (!logic_context->values_predeclared
				&& !mutator->add_value_fact(
				mutator->context, &step->result_fact, &fact_id)) {
			return zend_mir_logic_fail(
				context, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_MUTATION_FAILED);
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_logic_lower(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator)
{
	zend_mir_logic_plan plan;
	zend_mir_lowering_diagnostic_code diagnostic = ZEND_MIRL_OK;
	zend_mir_lowering_status status;

	if (source_opcode == NULL) {
		return zend_mir_logic_fail(
			context, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE);
	}
	if (source_opcode->zend_opcode_number >= ZEND_MIR_LOGIC_ZEND_IS_IDENTICAL
			&& source_opcode->zend_opcode_number
				<= ZEND_MIR_LOGIC_ZEND_IS_SMALLER_OR_EQUAL) {
		status = zend_mir_logic_build_compare_plan(
			context, source_opcode, &plan, &diagnostic);
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_LOGIC_ZEND_SPACESHIP) {
		status = zend_mir_logic_build_compare_plan(
			context, source_opcode, &plan, &diagnostic);
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_LOGIC_ZEND_CAST) {
		status = zend_mir_logic_build_cast_plan(
			context, source_opcode, &plan, &diagnostic);
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_LOGIC_ZEND_BOOL
			|| source_opcode->zend_opcode_number
				== ZEND_MIR_LOGIC_ZEND_BOOL_NOT
			|| source_opcode->zend_opcode_number
				== ZEND_MIR_LOGIC_ZEND_BOOL_XOR) {
		status = zend_mir_logic_build_boolean_plan(
			context, source_opcode, &plan, &diagnostic);
	} else {
		status = ZEND_MIR_LOWERING_REJECTED;
		diagnostic = ZEND_MIRL_INVALID_SOURCE;
	}
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return zend_mir_logic_fail(context, status, diagnostic);
	}
	return zend_mir_logic_commit_plan(context, &plan, mutator);
}

void zend_mir_logic_provider_init(
	zend_mir_lowering_provider *provider,
	const zend_mir_logic_context *logic_context)
{
	if (provider == NULL) {
		return;
	}
	memset(provider, 0, sizeof(*provider));
	provider->provider_id = ZEND_MIR_LOGIC_PROVIDER_ID;
	provider->semantic_family_id = ZEND_MIR_LOGIC_SEMANTIC_FAMILY_ID;
	provider->context = logic_context;
	provider->claim_count = zend_mir_logic_claim_count;
	provider->claim_at = zend_mir_logic_claim_at;
	provider->lower = zend_mir_logic_lower;
}
