/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <string.h>

#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"

#include "zend_mir_straight_line_internal.h"

static zend_mir_straight_line_value *zend_mir_straight_line_mutable_value(
	zend_mir_straight_line_lifetime *lifetime, zend_mir_value_id value_id)
{
	uint32_t index;

	for (index = 0; index < lifetime->count; index++) {
		if (lifetime->values[index].value_id == value_id) {
			return &lifetime->values[index];
		}
	}
	return NULL;
}

static bool zend_mir_straight_line_resolve_operand(
	const zend_mir_straight_line_entry *entry,
	const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out)
{
	if (entry->resolve_operand != NULL
			&& entry->resolve_operand(
				entry->source_context, operand, value_id_out)) {
		return zend_mir_id_is_valid(*value_id_out);
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_SSA
			&& operand->ssa_variable_id <= ZEND_MIR_VALUE_ORIGINAL_MAX) {
		*value_id_out =
			zend_mir_value_from_original_ssa(operand->ssa_variable_id);
		return true;
	}
	return false;
}

static bool zend_mir_straight_line_analyze_uses(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_opcode_ref *source_opcode,
	uint32_t ssa_variable_id, bool *later_out)
{
	uint32_t count;
	uint32_t current_uses = 0;
	uint32_t index;

	if (source == NULL || source->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| source->ssa_use_count == NULL
			|| source->ssa_use_at == NULL || later_out == NULL) {
		return false;
	}
	*later_out = false;
	count = source->ssa_use_count(source->context);
	for (index = 0; index < count; index++) {
		zend_mir_source_ssa_use_ref use;

		if (!source->ssa_use_at(source->context, index, &use)) {
			return false;
		}
		if (use.ssa_variable_id != ssa_variable_id) {
			continue;
		}
		if (use.opline_index == source_opcode->opline_index) {
			if (use.operand_index != 0 || ++current_uses != 1) {
				return false;
			}
		} else if (use.opline_index > source_opcode->opline_index) {
			*later_out = true;
		}
	}
	return current_uses == 1;
}

static zend_mir_straight_line_slot *zend_mir_straight_line_destination_slot(
	zend_mir_straight_line_entry *entry,
	const zend_mir_source_operand_ref *destination)
{
	zend_mir_frame_slot_kind kind;
	uint32_t index;

	if (destination->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& destination->kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return NULL;
	}
	switch (destination->slot_kind) {
		case ZEND_MIR_SOURCE_SLOT_CV:
			kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
			break;
		case ZEND_MIR_SOURCE_SLOT_TMP:
			kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
			break;
		case ZEND_MIR_SOURCE_SLOT_VAR:
			kind = ZEND_MIR_FRAME_SLOT_KIND_VAR;
			break;
		default:
			return NULL;
	}
	for (index = 0; index < entry->slot_count; index++) {
		if (entry->slots[index].kind == kind
				&& entry->slots[index].index == destination->index) {
			return &entry->slots[index];
		}
	}
	return NULL;
}

static void zend_mir_straight_line_move_slots(
	zend_mir_straight_line_entry *entry,
	zend_mir_value_id source_id,
	const zend_mir_source_operand_ref *destination,
	const zend_mir_straight_line_value *result, bool moved)
{
	zend_mir_straight_line_slot *destination_slot =
		zend_mir_straight_line_destination_slot(entry, destination);
	uint32_t index;

	if (moved) {
		for (index = 0; index < entry->slot_count; index++) {
			if (&entry->slots[index] != destination_slot
					&& entry->slots[index].value_id == source_id) {
				entry->slots[index].value_id = ZEND_MIR_ID_INVALID;
				entry->slots[index].value_representation =
					ZEND_MIR_REPRESENTATION_INVALID;
				entry->slots[index].materialization =
					ZEND_MIR_MATERIALIZATION_UNDEF;
			}
		}
	}
	if (destination_slot != NULL) {
		destination_slot->value_id = result->value_id;
		destination_slot->value_representation = result->representation;
		destination_slot->materialization =
			ZEND_MIR_MATERIALIZATION_MATERIALIZED;
		destination_slot->ownership =
			result->ownership == ZEND_MIR_OWNERSHIP_STATE_BORROWED
				? ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED
				: ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	}
}

zend_mir_lowering_status zend_mir_lower_copy_move(
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
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED;
	zend_mir_straight_line_value *source;
	zend_mir_straight_line_value result;
	zend_mir_ownership_transition transition;
	zend_mir_ownership_action action;
	zend_mir_instruction_record instruction;
	zend_mir_instruction_id instruction_id;
	zend_mir_value_fact_ref fact;
	zend_mir_value_fact_id fact_id;
	zend_mir_source_position_id source_position_id;
	zend_mir_value_id source_id;
	zend_mir_value_id result_id;
	zend_mir_frame_state_id prior_entry_frame_id;
	bool prior_entry_emitted;
	bool later_use;
	zend_mir_lowering_status status;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
	}
	if (context == NULL || source_opcode == NULL || mutator == NULL
			|| provider_context == NULL || provider_context->source == NULL
			|| provider_context->entry == NULL
			|| provider_context->lifetime == NULL
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| source_opcode->zend_opcode_number
				!= ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN
			|| source_opcode->op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| source_opcode->result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| source_opcode->result.ssa_variable_id
				> ZEND_MIR_VALUE_ORIGINAL_MAX
			|| mutator->add_value == NULL
			|| mutator->add_instruction == NULL
			|| mutator->add_operand == NULL
			|| mutator->add_value_fact == NULL) {
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
	if ((provider_context->hazards
			& (ZEND_MIR_STRAIGHT_LINE_HAZARD_REFERENCE
				| ZEND_MIR_STRAIGHT_LINE_HAZARD_OLD_VALUE)) != 0) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
		}
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if ((provider_context->hazards
			& (ZEND_MIR_STRAIGHT_LINE_HAZARD_PENDING_CALL
				| ZEND_MIR_STRAIGHT_LINE_HAZARD_CLEANUP
				| ZEND_MIR_STRAIGHT_LINE_HAZARD_DESTRUCTOR
				| ZEND_MIR_STRAIGHT_LINE_HAZARD_OBSERVER
				| ZEND_MIR_STRAIGHT_LINE_HAZARD_INTERRUPT
				| ZEND_MIR_STRAIGHT_LINE_HAZARD_EXCEPTION)) != 0) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
		}
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (!zend_mir_straight_line_resolve_operand(
			provider_context->entry, &source_opcode->op1, &source_id)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (!zend_mir_straight_line_analyze_uses(
			provider_context->source, source_opcode,
			source_opcode->op1.ssa_variable_id, &later_use)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	source = zend_mir_straight_line_mutable_value(
		provider_context->lifetime, source_id);
	if (!zend_mir_straight_line_value_contract_is_valid(source)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	action = !later_use && source->ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED
		? ZEND_MIR_OWNERSHIP_ACTION_MOVE
		: ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED;
	if (zend_mir_ownership_apply(
			action == ZEND_MIR_OWNERSHIP_ACTION_MOVE
				? source->ownership
				: ZEND_MIR_OWNERSHIP_STATE_UNINITIALIZED,
			action, &transition) != ZEND_MIR_OWNERSHIP_TRANSITION_OK
			|| (action == ZEND_MIR_OWNERSHIP_ACTION_MOVE
				? !transition.has_result
				: transition.has_result)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	result_id = zend_mir_value_from_original_ssa(
		source_opcode->result.ssa_variable_id);
	result = *source;
	result.value_id = result_id;
	result.ownership = transition.has_result
		? transition.result_state : transition.source_after;
	if (zend_mir_straight_line_mutable_value(
			provider_context->lifetime, result_id) != NULL
			|| provider_context->lifetime->count
				>= provider_context->lifetime->capacity
			|| zend_mir_straight_line_destination_slot(
				provider_context->entry, &source_opcode->result) == NULL) {
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
	if (!provider_context->values_predeclared
			&& !mutator->add_value(mutator->context, result_id,
				result.representation, result.ownership)) {
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
	instruction.opcode = ZEND_MIR_OPCODE_COPY;
	instruction.representation = result.representation;
	instruction.result_id = result_id;
	instruction.frame_state_id = ZEND_MIR_ID_INVALID;
	instruction.source_position_id = source_position_id;
	instruction.effects = transition.summary.effects;
	instruction.reads = transition.summary.reads;
	instruction.writes = transition.summary.writes;
	instruction.barriers = transition.summary.barriers;
	instruction.ownership_actions = transition.summary.ownership_actions;
	if (!mutator->add_instruction(
			mutator->context, &instruction, &instruction_id)
			|| !mutator->add_operand(
				mutator->context, instruction_id, source_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	memset(&fact, 0, sizeof(fact));
	fact.id = ZEND_MIR_ID_INVALID;
	fact.value_id = result_id;
	fact.exact_type = result.exact_type;
	fact.flags = result.fact_flags;
	fact.integer_min = result.integer_min;
	fact.integer_max = result.integer_max;
	fact.provenance = ZEND_MIR_FACT_PROVENANCE_SSA;
	fact.provenance_source_position_id = source_position_id;
	if (!provider_context->values_predeclared
			&& !mutator->add_value_fact(mutator->context, &fact, &fact_id)) {
		zend_mir_straight_line_restore_entry_state(
			provider_context->lifetime, prior_entry_emitted,
			prior_entry_frame_id);
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
		}
		return ZEND_MIR_LOWERING_FAILED;
	}
	if (action == ZEND_MIR_OWNERSHIP_ACTION_MOVE) {
		source->ownership = transition.source_after;
	}
	provider_context->lifetime->values[
		provider_context->lifetime->count++] = result;
	zend_mir_straight_line_move_slots(
		provider_context->entry, source_id, &source_opcode->result,
		&result, action == ZEND_MIR_OWNERSHIP_ACTION_MOVE);
	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_OK;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}
