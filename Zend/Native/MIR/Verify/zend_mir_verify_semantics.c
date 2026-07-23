/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include "zend_mir_verify_internal.h"

#include "Zend/Native/MIR/Semantics/zend_mir_effect_summary.h"
#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"

static uint32_t zend_mir_verify_semantics_value_index(
		const zend_mir_verify_context *context, zend_mir_value_id id)
{
	uint32_t left = 0;
	uint32_t right = context->value_count;

	while (left < right) {
		uint32_t middle = left + (right - left) / 2;
		zend_mir_value_id current = context->values[middle].record.id;
		if (current < id) {
			left = middle + 1;
		} else if (current > id) {
			right = middle;
		} else {
			return middle;
		}
	}
	return UINT32_MAX;
}

static uint32_t zend_mir_verify_action_count(zend_mir_ownership_action_mask actions)
{
	uint32_t count = 0;
	while (actions != 0) {
		count += actions & 1U;
		actions = (zend_mir_ownership_action_mask) (actions >> 1);
	}
	return count;
}

static zend_mir_ownership_action zend_mir_verify_single_action(
		zend_mir_ownership_action_mask actions)
{
	uint32_t index;
	for (index = 0; index < ZEND_MIR_OWNERSHIP_ACTION_COUNT; index++) {
		if ((actions & ZEND_MIR_OWNERSHIP_ACTION_MASK(index)) != 0) {
			return (zend_mir_ownership_action) index;
		}
	}
	return ZEND_MIR_OWNERSHIP_ACTION_INVALID;
}

static bool zend_mir_verify_effect_descriptors(
		zend_mir_verify_context *context,
		const zend_mir_instruction_record *instruction)
{
	uint32_t effect;
	bool valid = true;

	for (effect = 0; effect < ZEND_MIR_EFFECT_COUNT; effect++) {
		const zend_mir_atomic_effect_descriptor *descriptor;
		if ((instruction->effects & ZEND_MIR_EFFECT_MASK(effect)) == 0) {
			continue;
		}
		descriptor = zend_mir_atomic_effect_descriptor_at((zend_mir_effect) effect);
		if (descriptor == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_SEMANTICS,
				ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS,
				zend_mir_verify_instruction_location(context, instruction),
				ZEND_MIR_ID_INVALID, "effect has no registered atomic descriptor");
			valid = false;
			continue;
		}
		if ((instruction->reads & descriptor->reads) != descriptor->reads
				|| (instruction->writes & descriptor->writes) != descriptor->writes
				|| (instruction->barriers & descriptor->barriers)
					!= descriptor->barriers) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INCOMPLETE_SEMANTICS,
				ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS,
				zend_mir_verify_instruction_location(context, instruction),
				ZEND_MIR_ID_INVALID,
				"effect summary omits registered domains or barriers");
			valid = false;
		}
	}
	return valid;
}

static bool zend_mir_verify_action_descriptor(
		zend_mir_verify_context *context,
		const zend_mir_instruction_record *instruction,
		zend_mir_ownership_action action)
{
	const zend_mir_ownership_action_descriptor *descriptor =
		zend_mir_ownership_action_descriptor_at(action);
	if (descriptor == NULL) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID, "ownership action has no registered descriptor");
		return false;
	}
	if ((instruction->effects & descriptor->effects) != descriptor->effects
			|| (instruction->reads & descriptor->reads) != descriptor->reads
			|| (instruction->writes & descriptor->writes) != descriptor->writes
			|| (instruction->barriers & descriptor->barriers)
				!= descriptor->barriers) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INCOMPLETE_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID,
			"ownership action summary omits registered effects, domains, or barriers");
		return false;
	}
	return true;
}

static void zend_mir_verify_summary(
		zend_mir_verify_context *context,
		const zend_mir_instruction_record *instruction)
{
	zend_mir_effect_summary summary;

	if (zend_mir_verify_mask_has_unknown(
			instruction->effects, ZEND_MIR_EFFECT_COUNT)
			|| zend_mir_verify_mask_has_unknown(
				instruction->reads, ZEND_MIR_MEMORY_DOMAIN_COUNT)
			|| zend_mir_verify_mask_has_unknown(
				instruction->writes, ZEND_MIR_MEMORY_DOMAIN_COUNT)
			|| zend_mir_verify_mask_has_unknown(
				instruction->ownership_actions, ZEND_MIR_OWNERSHIP_ACTION_COUNT)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID,
			"instruction contains an unregistered effect, domain, or ownership action");
		return;
	}
	(void) zend_mir_verify_effect_descriptors(context, instruction);
	if (!zend_mir_effect_summary_init(&summary, instruction->effects,
			instruction->reads, instruction->writes, instruction->barriers,
			0, instruction->ownership_actions)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID,
			"semantic closure is unmodeled and is rejected fail-closed");
		return;
	}
	if (summary.effects != instruction->effects
			|| summary.reads != instruction->reads
			|| summary.writes != instruction->writes
			|| summary.barriers != instruction->barriers) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INCOMPLETE_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID,
			"instruction summary is not closed under W01 composition rules");
	}
	if ((instruction->opcode == ZEND_MIR_OPCODE_THROW
			|| instruction->opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL)
			&& (instruction->effects
				& ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_THROW)) == 0) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INCOMPLETE_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID, "THROW must carry throw semantics");
	}
	if (instruction->opcode == ZEND_MIR_OPCODE_UNREACHABLE
			&& (instruction->effects
				& ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_TERMINATE)) == 0) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INCOMPLETE_SEMANTICS,
			ZEND_MIR_DIAGNOSTIC_INVALID_EFFECTS,
			zend_mir_verify_instruction_location(context, instruction),
			ZEND_MIR_ID_INVALID, "UNREACHABLE must carry terminate semantics");
	}
}

static void zend_mir_verify_ownership_action(
		zend_mir_verify_context *context,
		const zend_mir_verify_instruction *instruction,
		zend_mir_ownership_state *states)
{
	zend_mir_ownership_action action;
	zend_mir_ownership_transition transition;
	zend_mir_ownership_state before;
	zend_mir_value_id source_id = ZEND_MIR_ID_INVALID;
	uint32_t source_index = UINT32_MAX;
	uint32_t result_index = UINT32_MAX;
	bool produces_directly;
	zend_mir_ownership_transition_status status;

	if (instruction->record.ownership_actions == 0) {
		return;
	}
	if (states == NULL) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
			ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"ownership action has no registered value state");
		return;
	}
	if (zend_mir_verify_action_count(instruction->record.ownership_actions) != 1) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
			ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"stage-1 requires one unambiguous ownership action per instruction");
		return;
	}
	action = zend_mir_verify_single_action(instruction->record.ownership_actions);
	if (!zend_mir_verify_action_descriptor(context, &instruction->record, action)) {
		return;
	}
	produces_directly = action == ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED
		|| action == ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_BORROWED;
	if (produces_directly) {
		before = ZEND_MIR_OWNERSHIP_STATE_UNINITIALIZED;
	} else {
		if (instruction->operands_count == 0) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
				ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
				zend_mir_verify_instruction_location(context, &instruction->record),
				ZEND_MIR_ID_INVALID,
				"ownership action requires operand zero as its source");
			return;
		}
		source_id = context->operands[instruction->operands_offset];
		source_index = zend_mir_verify_semantics_value_index(context, source_id);
		if (source_index == UINT32_MAX) {
			return;
		}
		before = states[source_index];
		if (!zend_mir_ownership_state_is_usable(before)) {
			const zend_mir_ownership_state_descriptor *descriptor =
				zend_mir_ownership_state_descriptor_at(before);
			zend_mir_verify_emit(context,
				descriptor != NULL && descriptor->terminal
					? ZEND_MIR_VERIFY_DOUBLE_CONSUME
					: ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
				ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
				zend_mir_verify_instruction_location(context, &instruction->record),
				source_id,
				descriptor != NULL && descriptor->terminal
					? "ownership action consumes an already terminal value"
					: "ownership action source is not usable");
			return;
		}
	}
	status = zend_mir_ownership_apply(before, action, &transition);
	if (status != ZEND_MIR_OWNERSHIP_TRANSITION_OK) {
		zend_mir_verify_emit(context,
			status == ZEND_MIR_OWNERSHIP_TRANSITION_TERMINAL_STATE
				? ZEND_MIR_VERIFY_DOUBLE_CONSUME
				: ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
			ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
			zend_mir_verify_instruction_location(context, &instruction->record),
			source_id, "ownership transition is rejected by the W01 lattice");
		return;
	}
	if (source_index != UINT32_MAX) {
		states[source_index] = transition.source_after;
	}
	if (zend_mir_id_is_valid(instruction->record.result_id)) {
		result_index = zend_mir_verify_semantics_value_index(
			context, instruction->record.result_id);
	}
	if (transition.has_result) {
		if (result_index == UINT32_MAX
				|| states[result_index] != transition.result_state) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
				ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
				zend_mir_verify_instruction_location(context, &instruction->record),
				instruction->record.result_id,
				"declared result ownership differs from action transition");
		}
	} else if (produces_directly) {
		if (result_index == UINT32_MAX
				|| states[result_index] != transition.source_after) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_OWNERSHIP,
				ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
				zend_mir_verify_instruction_location(context, &instruction->record),
				instruction->record.result_id,
				"produced result ownership differs from action transition");
		}
	}
}

void zend_mir_verify_semantics(zend_mir_verify_context *context)
{
	zend_mir_ownership_state *states = NULL;
	uint32_t index;

	if (context->value_count != 0) {
		states = zend_mir_verify_allocate(
			context, context->value_count, sizeof(*states));
		if (states == NULL) {
			return;
		}
		for (index = 0; index < context->value_count; index++) {
			states[index] = context->values[index].record.ownership;
		}
	}
	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_verify_instruction *instruction = &context->instructions[index];
		uint32_t operand_index;

		zend_mir_verify_summary(context, &instruction->record);
		for (operand_index = 0; operand_index < instruction->operands_count;
				operand_index++) {
			zend_mir_value_id operand =
				context->operands[instruction->operands_offset + operand_index];
			uint32_t value_index =
				zend_mir_verify_semantics_value_index(context, operand);
			const zend_mir_ownership_state_descriptor *descriptor;

			if (value_index == UINT32_MAX || states == NULL) {
				continue;
			}
			descriptor = zend_mir_ownership_state_descriptor_at(states[value_index]);
			if (descriptor != NULL && descriptor->terminal) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_TERMINAL_VALUE_USE,
					ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
					zend_mir_verify_instruction_location(context, &instruction->record),
					operand, "instruction uses a moved, released, or destroyed value");
			}
		}
		zend_mir_verify_ownership_action(context, instruction, states);
		if (context->halted) {
			return;
		}
	}
}
