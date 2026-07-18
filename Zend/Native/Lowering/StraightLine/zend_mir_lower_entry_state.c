/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <string.h>

#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"

#include "zend_mir_straight_line.h"

static bool zend_mir_straight_line_mutator_has_frame_ops(
	const zend_mir_mutator *mutator)
{
	return mutator != NULL
		&& mutator->contract_version == ZEND_MIR_CONTRACT_VERSION
		&& mutator->add_value != NULL
		&& mutator->add_instruction != NULL
		&& mutator->add_operand != NULL
		&& mutator->add_source_position != NULL
		&& mutator->add_frame_slot != NULL
		&& mutator->add_frame_state != NULL
		&& mutator->add_source_map != NULL;
}

bool zend_mir_straight_line_emit_source_position(
	const zend_mir_straight_line_entry *entry,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_source_position_id *source_id_out)
{
	zend_mir_source_position_ref source;

	return entry != NULL && source_opcode != NULL && mutator != NULL
		&& source_id_out != NULL && entry->source_position_at != NULL
		&& mutator->add_source_position != NULL
		&& entry->source_position_at(
			entry->source_context, source_opcode->source_position_id, &source)
		&& mutator->add_source_position(
			mutator->context, &source, source_id_out);
}

static zend_mir_straight_line_value *zend_mir_straight_line_find_value(
	zend_mir_straight_line_lifetime *lifetime, zend_mir_value_id value_id)
{
	uint32_t index;

	if (lifetime == NULL) {
		return NULL;
	}
	for (index = 0; index < lifetime->count; index++) {
		if (lifetime->values[index].value_id == value_id) {
			return &lifetime->values[index];
		}
	}
	return NULL;
}

bool zend_mir_straight_line_value_contract_is_valid(
	const zend_mir_straight_line_value *value)
{
	const zend_mir_value_fact_flags known_flags =
		ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NONZERO
		| ZEND_MIR_VALUE_FACT_FINITE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;

	if (value == NULL || !zend_mir_id_is_valid(value->value_id)
			|| (value->ownership != ZEND_MIR_OWNERSHIP_STATE_BORROWED
				&& value->ownership != ZEND_MIR_OWNERSHIP_STATE_OWNED)
			|| !zend_mir_scalar_type_is_exact(value->exact_type)
			|| (value->fact_flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0
			|| (value->fact_flags & ~known_flags) != 0) {
		return false;
	}
	switch (value->exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			if (value->representation != ZEND_MIR_REPRESENTATION_ZVAL) {
				return false;
			}
			break;
		case ZEND_MIR_SCALAR_TYPE_I1:
			if (value->representation != ZEND_MIR_REPRESENTATION_I1) {
				return false;
			}
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			if (value->representation != ZEND_MIR_REPRESENTATION_I64) {
				return false;
			}
			break;
		case ZEND_MIR_SCALAR_TYPE_F64:
			if (value->representation != ZEND_MIR_REPRESENTATION_DOUBLE) {
				return false;
			}
			break;
		default:
			return false;
	}
	if ((value->fact_flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0) {
		if (value->exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| value->integer_min > value->integer_max) {
			return false;
		}
	} else if (value->integer_min != 0 || value->integer_max != 0) {
		return false;
	}
	if ((value->fact_flags & ZEND_MIR_VALUE_FACT_NONZERO) != 0
			&& (value->exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| ((value->fact_flags
						& ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
					&& value->integer_min <= 0
					&& value->integer_max >= 0))) {
		return false;
	}
	if ((value->fact_flags & ZEND_MIR_VALUE_FACT_FINITE) != 0
			&& value->exact_type != ZEND_MIR_SCALAR_TYPE_F64) {
		return false;
	}
	return true;
}

void zend_mir_straight_line_restore_entry_state(
	zend_mir_straight_line_lifetime *lifetime, bool entry_emitted,
	zend_mir_frame_state_id entry_frame_state_id)
{
	if (lifetime != NULL) {
		lifetime->entry_emitted = entry_emitted;
		lifetime->entry_frame_state_id = entry_frame_state_id;
	}
}

bool zend_mir_straight_line_lifetime_init(
	zend_mir_straight_line_lifetime *lifetime,
	zend_mir_straight_line_value *storage, uint32_t capacity)
{
	if (lifetime == NULL || storage == NULL || capacity == 0) {
		return false;
	}
	memset(lifetime, 0, sizeof(*lifetime));
	lifetime->values = storage;
	lifetime->capacity = capacity;
	lifetime->entry_frame_state_id = ZEND_MIR_ID_INVALID;
	return true;
}

bool zend_mir_straight_line_track_value(
	zend_mir_straight_line_lifetime *lifetime,
	const zend_mir_straight_line_value *value)
{
	zend_mir_straight_line_value *current;

	if (lifetime == NULL || value == NULL
			|| !zend_mir_straight_line_value_contract_is_valid(value)) {
		return false;
	}
	current = zend_mir_straight_line_find_value(lifetime, value->value_id);
	if (current != NULL) {
		return current->representation == value->representation
			&& current->ownership == value->ownership
			&& current->exact_type == value->exact_type
			&& current->fact_flags == value->fact_flags
			&& current->integer_min == value->integer_min
			&& current->integer_max == value->integer_max;
	}
	if (lifetime->count >= lifetime->capacity) {
		return false;
	}
	lifetime->values[lifetime->count++] = *value;
	return true;
}

bool zend_mir_straight_line_value_at(
	const zend_mir_straight_line_lifetime *lifetime,
	zend_mir_value_id value_id, zend_mir_straight_line_value *out)
{
	uint32_t index;

	if (lifetime == NULL || out == NULL) {
		return false;
	}
	for (index = 0; index < lifetime->count; index++) {
		if (lifetime->values[index].value_id == value_id) {
			*out = lifetime->values[index];
			return true;
		}
	}
	return false;
}

static bool zend_mir_straight_line_entry_contract_is_valid(
	const zend_mir_straight_line_provider_context *provider_context)
{
	const zend_mir_straight_line_entry *entry;
	uint32_t index;

	if (provider_context == NULL || provider_context->lifetime == NULL
			|| provider_context->entry == NULL) {
		return false;
	}
	entry = provider_context->entry;
	if ((uint32_t) entry->function_kind >= ZEND_MIR_FUNCTION_KIND_COUNT
			|| !zend_mir_id_is_valid(entry->op_array_id)
			|| !zend_mir_id_is_valid(entry->code_version_id)
			|| (entry->slot_count != 0 && entry->slots == NULL)) {
		return false;
	}
	for (index = 0; index < entry->slot_count; index++) {
		const zend_mir_straight_line_slot *slot = &entry->slots[index];
		zend_mir_straight_line_value value;
		uint32_t earlier;

		if (!zend_mir_id_is_valid(slot->slot_id)
				|| (uint32_t) slot->kind >= ZEND_MIR_FRAME_SLOT_KIND_COUNT
				|| (slot->kind != ZEND_MIR_FRAME_SLOT_KIND_ARGUMENT
					&& slot->kind != ZEND_MIR_FRAME_SLOT_KIND_CV
					&& slot->kind != ZEND_MIR_FRAME_SLOT_KIND_TMP
					&& slot->kind != ZEND_MIR_FRAME_SLOT_KIND_VAR)
				|| (uint32_t) slot->ownership
					>= ZEND_MIR_FRAME_SLOT_OWNERSHIP_COUNT
				|| (slot->materialization != ZEND_MIR_MATERIALIZATION_MATERIALIZED
					&& slot->materialization != ZEND_MIR_MATERIALIZATION_UNDEF)) {
			return false;
		}
		if (index != 0
				&& (entry->slots[index - 1].kind > slot->kind
					|| (entry->slots[index - 1].kind == slot->kind
						&& entry->slots[index - 1].index >= slot->index))) {
			return false;
		}
		for (earlier = 0; earlier < index; earlier++) {
			if (entry->slots[earlier].slot_id == slot->slot_id) {
				return false;
			}
		}
		if (slot->materialization == ZEND_MIR_MATERIALIZATION_UNDEF) {
			if (zend_mir_id_is_valid(slot->value_id)
					|| slot->value_representation
						!= ZEND_MIR_REPRESENTATION_INVALID) {
				return false;
			}
			continue;
		}
		if (!zend_mir_straight_line_value_at(
				provider_context->lifetime, slot->value_id, &value)
				|| !zend_mir_straight_line_value_contract_is_valid(&value)
				|| value.representation != slot->value_representation
				|| (value.ownership == ZEND_MIR_OWNERSHIP_STATE_BORROWED
					? slot->ownership
						!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED
					: slot->ownership
						!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_straight_line_slot_value(
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_straight_line_slot *slot,
	zend_mir_frame_slot_ref *frame_slot)
{
	zend_mir_straight_line_value value;

	memset(frame_slot, 0, sizeof(*frame_slot));
	frame_slot->slot_id = slot->slot_id;
	frame_slot->index = slot->index;
	frame_slot->kind = slot->kind;
	frame_slot->representation =
		ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	frame_slot->materialization = slot->materialization;
	frame_slot->ownership = slot->ownership;
	frame_slot->value_id = ZEND_MIR_ID_INVALID;
	if (slot->materialization == ZEND_MIR_MATERIALIZATION_UNDEF) {
		return true;
	}
	if (slot->materialization != ZEND_MIR_MATERIALIZATION_MATERIALIZED
			|| !zend_mir_straight_line_value_at(
				provider_context->lifetime, slot->value_id, &value)
			|| !zend_mir_ownership_state_is_usable(value.ownership)
			|| !zend_mir_scalar_type_is_exact(value.exact_type)
			|| (value.fact_flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0
			|| value.representation != slot->value_representation) {
		return false;
	}
	frame_slot->value_id = value.value_id;
	return true;
}

static bool zend_mir_straight_line_emit_frame(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_safepoint_class safepoint_class,
	zend_mir_frame_state_id *frame_id_out,
	zend_mir_source_position_id *source_id_out)
{
	zend_mir_straight_line_entry *entry = provider_context->entry;
	zend_mir_frame_state_ref frame;
	zend_mir_source_map_ref map;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_position_id source_id;
	zend_mir_source_map_id map_id;
	zend_mir_function_id function_id =
		zend_mir_lowering_context_function_id(context);
	uint32_t first_slot = 0;
	uint32_t index;

	if (!zend_mir_id_is_valid(function_id)
			|| !zend_mir_straight_line_entry_contract_is_valid(provider_context)
			|| !zend_mir_straight_line_emit_source_position(
				entry, source_opcode, mutator, &source_id)) {
		return false;
	}

	for (index = 0; index < entry->slot_count; index++) {
		zend_mir_frame_slot_ref slot;
		uint32_t slot_index;

		if (!zend_mir_straight_line_slot_value(
				provider_context, &entry->slots[index], &slot)
				|| !mutator->add_frame_slot(
					mutator->context, &slot, &slot_index)
				|| (index != 0 && slot_index != first_slot + index)) {
			return false;
		}
		if (index == 0) {
			first_slot = slot_index;
		}
	}

	memset(&frame, 0, sizeof(frame));
	frame.id = ZEND_MIR_ID_INVALID;
	frame.function_id = function_id;
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = entry->function_kind;
	frame.opline_index = source_opcode->opline_index;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame.slots.offset = entry->slot_count != 0 ? first_slot : 0;
	frame.slots.count = entry->slot_count;
	frame.return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame.return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.return_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.exception_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame.exception_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.exception_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame.bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame.suspend_state_id = ZEND_MIR_ID_INVALID;
	frame.code_version_id = entry->code_version_id;
	frame.resume.allowed = false;
	frame.resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame.resume.resume_id = ZEND_MIR_ID_INVALID;
	frame.resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame.resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame.safepoint_class = safepoint_class;
	frame.canonical = true;
	if (!mutator->add_frame_state(mutator->context, &frame, &frame_id)) {
		return false;
	}

	memset(&map, 0, sizeof(map));
	map.id = ZEND_MIR_ID_INVALID;
	map.source_position_id = source_id;
	map.op_array_id = entry->op_array_id;
	map.opline_index = frame.opline_index;
	map.opline_phase = frame.opline_phase;
	map.owner_frame_id = frame_id;
	if (!mutator->add_source_map(mutator->context, &map, &map_id)) {
		return false;
	}
	*frame_id_out = frame_id;
	*source_id_out = source_id;
	return true;
}

zend_mir_lowering_status zend_mir_lower_entry_state(
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
	uint32_t index;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_MUTATION_FAILED;
	}
	if (context == NULL || source_opcode == NULL || provider_context == NULL
			|| provider_context->lifetime == NULL
			|| provider_context->entry == NULL
			|| !zend_mir_straight_line_mutator_has_frame_ops(mutator)) {
		return ZEND_MIR_LOWERING_FAILED;
	}
	if (!zend_mir_straight_line_entry_contract_is_valid(provider_context)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_CONTRADICTORY_FACT;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if (provider_context->lifetime->entry_emitted) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_OK;
		}
		return ZEND_MIR_LOWERING_SUCCESS;
	}
	if (!zend_mir_straight_line_emit_frame(
			context, source_opcode, mutator, provider_context,
			ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY, &frame_id, &source_id)) {
		return ZEND_MIR_LOWERING_FAILED;
	}
	memset(&instruction, 0, sizeof(instruction));
	instruction.id = ZEND_MIR_ID_INVALID;
	instruction.block_id = zend_mir_lowering_context_block_id(context);
	instruction.opcode = ZEND_MIR_OPCODE_STATEPOINT;
	instruction.representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	instruction.frame_state_id = frame_id;
	instruction.source_position_id = source_id;
	if (!zend_mir_id_is_valid(instruction.block_id)
			|| !mutator->add_instruction(
				mutator->context, &instruction, &instruction_id)) {
		return ZEND_MIR_LOWERING_FAILED;
	}
	for (index = 0; index < provider_context->entry->slot_count; index++) {
		const zend_mir_straight_line_slot *slot =
			&provider_context->entry->slots[index];
		if (slot->materialization == ZEND_MIR_MATERIALIZATION_MATERIALIZED
				&& !mutator->add_operand(
					mutator->context, instruction_id, slot->value_id)) {
			return ZEND_MIR_LOWERING_FAILED;
		}
	}
	provider_context->lifetime->entry_emitted = true;
	provider_context->lifetime->entry_frame_state_id = frame_id;
	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_OK;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

bool zend_mir_straight_line_emit_observable_frame(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_frame_state_id *frame_id_out,
	zend_mir_source_position_id *source_id_out)
{
	return zend_mir_straight_line_emit_frame(
		context, source_opcode, mutator, provider_context,
		ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY,
		frame_id_out, source_id_out);
}
