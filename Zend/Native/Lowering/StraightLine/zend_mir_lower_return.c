/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <string.h>

#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"
#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"

#include "zend_mir_straight_line_internal.h"

static zend_mir_lowering_status zend_mir_lower_source_zval_return(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_instruction_record instruction;
	zend_mir_instruction_id instruction_id;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_position_id source_id;
	zend_mir_frame_state_id prior_entry_frame_id;
	bool prior_entry_emitted;
	zend_mir_lowering_status status;

	if (source_opcode->op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& source_opcode->op1.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		return ZEND_MIR_LOWERING_STATUS_INVALID;
	}
	if (context->zend_source == NULL || !context->zend_source->w08
			|| !zend_mir_straight_line_has_cfg_proof(provider_context->proofs)
			|| !zend_mir_zend_source_w08_return_source_zval(
				context->zend_source, source_opcode->opline_index)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	prior_entry_emitted = provider_context->lifetime->entry_emitted;
	prior_entry_frame_id = provider_context->lifetime->entry_frame_state_id;
	status = zend_mir_lower_entry_state(
		context, source_opcode, mutator, provider_context, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (!zend_mir_straight_line_emit_observable_frame(
			context, source_opcode, mutator, provider_context,
			&frame_id, &source_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	memset(&instruction, 0, sizeof(instruction));
	instruction.id = ZEND_MIR_ID_INVALID;
	instruction.block_id = zend_mir_lowering_context_block_id(context);
	instruction.opcode = ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL;
	instruction.representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	instruction.frame_state_id = frame_id;
	instruction.source_position_id = source_id;
	if (!mutator->add_instruction(
			mutator->context, &instruction, &instruction_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_OK;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static bool zend_mir_straight_line_resolve_return_value(
	const zend_mir_straight_line_provider_context *provider_context,
	const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out,
	zend_mir_straight_line_value *value_out)
{
	if (provider_context->entry->resolve_operand != NULL
			&& provider_context->entry->resolve_operand(
				provider_context->entry->source_context,
				operand, value_id_out)
			&& zend_mir_id_is_valid(*value_id_out)) {
		return zend_mir_straight_line_value_at(
			provider_context->lifetime, *value_id_out, value_out);
	}
	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| operand->ssa_variable_id > ZEND_MIR_VALUE_ORIGINAL_MAX) {
		return false;
	}
	*value_id_out =
		zend_mir_value_from_original_ssa(operand->ssa_variable_id);
	return zend_mir_straight_line_value_at(
		provider_context->lifetime, *value_id_out, value_out);
}

static bool zend_mir_straight_line_scalar_is_valid(
	const zend_mir_straight_line_value *value)
{
	return zend_mir_straight_line_value_contract_is_valid(value);
}

static zend_mir_lowering_status zend_mir_straight_line_check_hazards(
	const zend_mir_straight_line_provider_context *provider_context,
	bool for_return, zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	zend_mir_straight_line_hazard_mask reference_hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_REFERENCE
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_RETURN_BY_REFERENCE
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_OLD_VALUE;
	zend_mir_straight_line_hazard_mask runtime_hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_PENDING_CALL
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_CLEANUP
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_DESTRUCTOR
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_OBSERVER
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_INTERRUPT
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_EXCEPTION;

	if ((provider_context->hazards
			& (for_return ? reference_hazards
				: (ZEND_MIR_STRAIGHT_LINE_HAZARD_REFERENCE
					| ZEND_MIR_STRAIGHT_LINE_HAZARD_OLD_VALUE))) != 0) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
		}
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if ((provider_context->hazards & runtime_hazards) != 0) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
		}
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_lower_return(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_straight_line_proof_mask required =
		ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY
		| ZEND_MIR_STRAIGHT_LINE_PROOF_EXACT_SCALAR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NOT_BY_REFERENCE
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_OBSERVER
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_EXCEPTION;
	zend_mir_straight_line_value value;
	zend_mir_instruction_record instruction;
	zend_mir_instruction_id instruction_id;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_position_id source_id;
	zend_mir_value_id value_id;
	zend_mir_frame_state_id prior_entry_frame_id;
	bool prior_entry_emitted;
	zend_mir_lowering_status status;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
	}
	if (context == NULL || source_opcode == NULL || mutator == NULL
			|| provider_context == NULL || provider_context->entry == NULL
			|| provider_context->lifetime == NULL
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| source_opcode->zend_opcode_number
				!= ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN
			|| mutator->add_instruction == NULL
			|| mutator->add_operand == NULL) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (source_opcode->op1.kind == ZEND_MIR_SOURCE_OPERAND_SLOT) {
		return zend_mir_lower_source_zval_return(
			context, source_opcode, mutator, provider_context, diagnostic_out);
	}
	if ((provider_context->proofs & required) != required
			|| !zend_mir_straight_line_has_cfg_proof(
				provider_context->proofs)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	status = zend_mir_straight_line_check_hazards(
		provider_context, true, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (!zend_mir_straight_line_resolve_return_value(
			provider_context, &source_opcode->op1, &value_id, &value)
			|| !zend_mir_straight_line_scalar_is_valid(&value)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	prior_entry_emitted = provider_context->lifetime->entry_emitted;
	prior_entry_frame_id =
		provider_context->lifetime->entry_frame_state_id;
	status = zend_mir_lower_entry_state(
		context, source_opcode, mutator, provider_context, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (!zend_mir_straight_line_emit_observable_frame(
			context, source_opcode, mutator, provider_context,
			&frame_id, &source_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	memset(&instruction, 0, sizeof(instruction));
	instruction.id = ZEND_MIR_ID_INVALID;
	instruction.block_id = zend_mir_lowering_context_block_id(context);
	instruction.opcode = ZEND_MIR_OPCODE_RETURN;
	instruction.representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	instruction.frame_state_id = frame_id;
	instruction.source_position_id = source_id;
	if (!mutator->add_instruction(
			mutator->context, &instruction, &instruction_id)
			|| !mutator->add_operand(
				mutator->context, instruction_id, value_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_OK;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_lower_free(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_straight_line_proof_mask required =
		ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY
		| ZEND_MIR_STRAIGHT_LINE_PROOF_EXACT_SCALAR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_DESTRUCTOR;
	zend_mir_straight_line_value *tracked = NULL;
	zend_mir_straight_line_value value;
	zend_mir_instruction_record instruction;
	zend_mir_instruction_id instruction_id;
	zend_mir_source_position_id source_position_id;
	zend_mir_value_id value_id;
	zend_mir_frame_state_id prior_entry_frame_id;
	bool prior_entry_emitted;
	zend_mir_lowering_status status;
	uint32_t index;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
	}
	if (context == NULL || source_opcode == NULL || mutator == NULL
			|| provider_context == NULL
			|| provider_context->entry == NULL
			|| provider_context->lifetime == NULL
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| source_opcode->zend_opcode_number
				!= ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE
			|| mutator->add_instruction == NULL
			|| mutator->add_operand == NULL
			|| mutator->add_source_position == NULL) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if ((provider_context->proofs & required) != required
			|| !zend_mir_straight_line_has_cfg_proof(
				provider_context->proofs)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	status = zend_mir_straight_line_check_hazards(
		provider_context, false, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (!zend_mir_straight_line_resolve_return_value(
			provider_context, &source_opcode->op1, &value_id, &value)
			|| !zend_mir_straight_line_scalar_is_valid(&value)
			|| value.ownership != ZEND_MIR_OWNERSHIP_STATE_OWNED) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	for (index = 0; index < provider_context->lifetime->count; index++) {
		if (provider_context->lifetime->values[index].value_id == value_id) {
			tracked = &provider_context->lifetime->values[index];
			break;
		}
	}
	if (tracked == NULL) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	prior_entry_emitted = provider_context->lifetime->entry_emitted;
	prior_entry_frame_id =
		provider_context->lifetime->entry_frame_state_id;
	status = zend_mir_lower_entry_state(
		context, source_opcode, mutator, provider_context, diagnostic_out);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		return status;
	}
	if (!zend_mir_straight_line_emit_source_position(
			provider_context->entry, source_opcode, mutator,
			&source_position_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	memset(&instruction, 0, sizeof(instruction));
	instruction.id = ZEND_MIR_ID_INVALID;
	instruction.block_id = zend_mir_lowering_context_block_id(context);
	instruction.opcode = ZEND_MIR_OPCODE_SCALAR_DROP;
	instruction.representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	instruction.frame_state_id = ZEND_MIR_ID_INVALID;
	instruction.source_position_id = source_position_id;
	if (!zend_mir_id_is_valid(instruction.block_id)
			|| !mutator->add_instruction(
				mutator->context, &instruction, &instruction_id)
			|| !mutator->add_operand(
				mutator->context, instruction_id, value_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	/*
	 * SCALAR_DROP is the profiled, effect-free marker for an owned
	 * non-refcounted scalar.  The provider-local terminal state prevents
	 * subsequent lowering from reusing or consuming the source.
	 */
	tracked->ownership = ZEND_MIR_OWNERSHIP_STATE_RELEASED;
	for (index = 0; index < provider_context->entry->slot_count; index++) {
		if (provider_context->entry->slots[index].value_id == value_id) {
			provider_context->entry->slots[index].value_id =
				ZEND_MIR_ID_INVALID;
			provider_context->entry->slots[index].value_representation =
				ZEND_MIR_REPRESENTATION_INVALID;
			provider_context->entry->slots[index].materialization =
				ZEND_MIR_MATERIALIZATION_UNDEF;
		}
	}
	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_OK;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}
