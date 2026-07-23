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

static bool zend_mir_verify_frame_has_slot(
		const zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame, uint32_t slot_id)
{
	uint32_t index;
	for (index = 0; index < frame->slots.count; index++) {
		if (context->slots[frame->slots.offset + index].slot_id == slot_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_verify_frame_has_value(
		const zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame, zend_mir_value_id value_id)
{
	uint32_t index;
	if (!zend_mir_verify_span_is_valid(frame->slots, context->slot_count)) {
		return false;
	}
	for (index = 0; index < frame->slots.count; index++) {
		if (context->slots[frame->slots.offset + index].value_id == value_id) {
			return true;
		}
	}
	return false;
}

static uint32_t zend_mir_verify_frame_root_occurrences(
		const zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame, uint32_t slot_id)
{
	uint32_t count = 0;
	uint32_t index;
	for (index = 0; index < frame->roots.count; index++) {
		if (context->roots[frame->roots.offset + index] == slot_id) {
			count++;
		}
	}
	return count;
}

static uint32_t zend_mir_verify_frame_cleanup_occurrences(
		const zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame, uint32_t slot_id)
{
	uint32_t count = 0;
	uint32_t index;
	for (index = 0; index < frame->cleanup_obligations.count; index++) {
		if (context->cleanups[frame->cleanup_obligations.offset + index].slot_id
				== slot_id) {
			count++;
		}
	}
	return count;
}

static void zend_mir_verify_frame_layout(
		zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame)
{
	zend_mir_diagnostic_location location =
		zend_mir_verify_frame_location(context, frame);
	uint32_t index;

	if (!zend_mir_verify_span_is_valid(frame->slots, context->slot_count)
			|| !zend_mir_verify_span_is_valid(frame->roots, context->root_count)
			|| !zend_mir_verify_span_is_valid(
				frame->cleanup_obligations, context->cleanup_count)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_FRAME,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
			ZEND_MIR_ID_INVALID, "frame contains an out-of-bounds layout span");
		return;
	}
	for (index = 0; index < frame->slots.count; index++) {
		const zend_mir_frame_slot_ref *slot =
			&context->slots[frame->slots.offset + index];
		uint32_t earlier;

		if (!zend_mir_id_is_valid(slot->slot_id)
				|| (uint32_t) slot->kind >= ZEND_MIR_FRAME_SLOT_KIND_COUNT
				|| (uint32_t) slot->representation
					>= ZEND_MIR_FRAME_SLOT_REPRESENTATION_COUNT
				|| (uint32_t) slot->materialization >= ZEND_MIR_MATERIALIZATION_COUNT
				|| (uint32_t) slot->ownership >= ZEND_MIR_FRAME_SLOT_OWNERSHIP_COUNT
				|| ((slot->materialization == ZEND_MIR_MATERIALIZATION_UNDEF
						|| slot->materialization
							== ZEND_MIR_MATERIALIZATION_SOURCE_ZVAL)
					? zend_mir_id_is_valid(slot->value_id)
					: zend_mir_verify_find_value(context, slot->value_id) == NULL)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SLOT,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				slot->value_id, "frame slot is invalid or references an unknown value");
		}
		for (earlier = 0; earlier < index; earlier++) {
			if (context->slots[frame->slots.offset + earlier].slot_id == slot->slot_id) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SLOT,
					ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
					slot->value_id, "frame contains a duplicate slot ID");
				break;
			}
		}
		if (zend_mir_verify_frame_root_occurrences(context, frame, slot->slot_id)
				!= (slot->rooted ? 1U : 0U)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_ROOT,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				slot->value_id, "root list does not exactly match rooted frame slots");
		}
		if (zend_mir_verify_frame_cleanup_occurrences(context, frame, slot->slot_id)
				!= (slot->cleanup_required ? 1U : 0U)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_CLEANUP,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				slot->value_id,
				"cleanup ledger does not exactly match cleanup-required slots");
		}
	}
	for (index = 0; index < frame->roots.count; index++) {
		uint32_t slot_id = context->roots[frame->roots.offset + index];
		if (!zend_mir_verify_frame_has_slot(context, frame, slot_id)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_ROOT,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				ZEND_MIR_ID_INVALID, "root references a slot outside its frame");
		}
	}
	for (index = 0; index < frame->cleanup_obligations.count; index++) {
		const zend_mir_cleanup_ref *cleanup =
			&context->cleanups[frame->cleanup_obligations.offset + index];
		if (!zend_mir_verify_frame_has_slot(context, frame, cleanup->slot_id)
				|| (uint32_t) cleanup->action >= ZEND_MIR_CLEANUP_ACTION_COUNT
				|| (uint32_t) cleanup->state >= ZEND_MIR_CLEANUP_STATE_COUNT
				|| cleanup->action == ZEND_MIR_CLEANUP_ACTION_NONE) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_CLEANUP,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
				ZEND_MIR_ID_INVALID, "cleanup obligation is invalid");
		}
	}
}

static bool zend_mir_verify_continuation_valid(
		const zend_mir_verify_context *context,
		const zend_mir_continuation_ref *continuation)
{
	if ((uint32_t) continuation->kind >= ZEND_MIR_CONTINUATION_KIND_COUNT) {
		return false;
	}
	if (continuation->kind == ZEND_MIR_CONTINUATION_KIND_TERMINAL
			|| continuation->kind == ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT) {
		return continuation->frame_state_id == ZEND_MIR_ID_INVALID
			&& continuation->opline_index == ZEND_MIR_ID_INVALID;
	}
	return zend_mir_id_is_valid(continuation->opline_index)
		&& (!zend_mir_id_is_valid(continuation->frame_state_id)
			|| zend_mir_verify_find_frame(
				context, continuation->frame_state_id) != NULL);
}

static void zend_mir_verify_frame_resume(
		zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame)
{
	bool valid = true;

	if ((uint32_t) frame->suspend_kind >= ZEND_MIR_SUSPEND_KIND_COUNT
			|| (frame->suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE)
				!= (frame->suspend_state_id == ZEND_MIR_ID_INVALID)) {
		valid = false;
	}
	if (!frame->resume.allowed) {
		valid = valid
			&& frame->resume.entry_kind == ZEND_MIR_RESUME_ENTRY_KIND_NONE
			&& frame->resume.resume_id == ZEND_MIR_ID_INVALID
			&& frame->resume.code_version_id == ZEND_MIR_ID_INVALID
			&& frame->resume.target_opline_index == ZEND_MIR_ID_INVALID
			&& frame->suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
			&& frame->opline_phase != ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	} else {
		valid = valid
			&& frame->resume.entry_kind
				== ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER
			&& zend_mir_id_is_valid(frame->resume.resume_id)
			&& zend_mir_id_is_valid(frame->resume.target_opline_index)
			&& frame->resume.code_version_id == frame->code_version_id
			&& frame->suspend_kind != ZEND_MIR_SUSPEND_KIND_NONE
			&& frame->opline_phase == ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	}
	if (frame->safepoint_class == ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND
			&& frame->suspend_kind != ZEND_MIR_SUSPEND_KIND_GENERATOR) {
		valid = false;
	}
	if (frame->safepoint_class == ZEND_MIR_SAFEPOINT_CLASS_FIBER_SWITCH
			&& frame->suspend_kind != ZEND_MIR_SUSPEND_KIND_FIBER) {
		valid = false;
	}
	if (!valid) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_RESUME,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
			zend_mir_verify_frame_location(context, frame),
			ZEND_MIR_ID_INVALID,
			"suspend and single-entry resume metadata are inconsistent");
	}
}

static void zend_mir_verify_frame_record(
		zend_mir_verify_context *context,
		const zend_mir_frame_state_ref *frame)
{
	zend_mir_diagnostic_location location =
		zend_mir_verify_frame_location(context, frame);

	if (zend_mir_verify_find_function(context, frame->function_id) == NULL
			|| (uint32_t) frame->function_kind >= ZEND_MIR_FUNCTION_KIND_COUNT
			|| (uint32_t) frame->opline_phase >= ZEND_MIR_OPLINE_PHASE_COUNT
			|| (uint32_t) frame->safepoint_class >= ZEND_MIR_SAFEPOINT_CLASS_COUNT
			|| !zend_mir_id_is_valid(frame->code_version_id)
			|| !frame->canonical
			|| (frame->function_kind == ZEND_MIR_FUNCTION_KIND_INTERNAL
				? frame->opline_index != ZEND_MIR_ID_INVALID
				: !zend_mir_id_is_valid(frame->opline_index))
			|| (zend_mir_id_is_valid(frame->parent_id)
				&& zend_mir_verify_find_frame(context, frame->parent_id) == NULL)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_FRAME,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
			ZEND_MIR_ID_INVALID, "frame identity, enum, parent, or canonical state is invalid");
	}
	zend_mir_verify_frame_layout(context, frame);
	if (!zend_mir_verify_continuation_valid(context, &frame->return_continuation)
			|| !zend_mir_verify_continuation_valid(
				context, &frame->exception_continuation)
			|| !zend_mir_verify_continuation_valid(
				context, &frame->bailout_continuation)
			|| frame->bailout_continuation.kind
				!= ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_CONTINUATION,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE, location,
			ZEND_MIR_ID_INVALID,
			"return, exception, or bailout continuation is invalid");
	}
	zend_mir_verify_frame_resume(context, frame);
}

static void zend_mir_verify_parent_cycles(zend_mir_verify_context *context)
{
	uint32_t index;
	for (index = 0; index < context->frame_count; index++) {
		const zend_mir_frame_state_ref *origin = &context->frames[index].record;
		const zend_mir_frame_state_ref *current = origin;
		uint32_t depth;

		for (depth = 0; depth <= context->frame_count; depth++) {
			const zend_mir_verify_frame *parent;
			if (!zend_mir_id_is_valid(current->parent_id)) {
				break;
			}
			parent = zend_mir_verify_find_frame(context, current->parent_id);
			if (parent == NULL) {
				break;
			}
			current = &parent->record;
			if (current->id == origin->id || depth == context->frame_count) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_PARENT_CYCLE,
					ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
					zend_mir_verify_frame_location(context, origin),
					ZEND_MIR_ID_INVALID, "frame parent chain contains a cycle");
				break;
			}
		}
	}
}

static void zend_mir_verify_sources(zend_mir_verify_context *context)
{
	uint32_t index;
	for (index = 0; index < context->source_count; index++) {
		const zend_mir_source_position_ref *source =
			&context->sources[index].record;
		zend_mir_diagnostic_location location = zend_mir_verify_location();

		location.module_id = context->module_id;
		location.source_position_id = source->id;
		if (!zend_mir_id_is_valid(source->file_symbol_id) || source->line == 0
				|| source->column_start == 0
				|| source->column_end < source->column_start) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SOURCE,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
				location, ZEND_MIR_ID_INVALID,
				"source position has invalid file, line, or column metadata");
		}
	}
	for (index = 0; index < context->source_map_count; index++) {
		const zend_mir_source_map_ref *map = &context->source_maps[index].record;
		const zend_mir_verify_frame *frame =
			zend_mir_verify_find_frame(context, map->owner_frame_id);
		zend_mir_diagnostic_location location =
			zend_mir_verify_frame_location(
				context, frame != NULL ? &frame->record : NULL);
		uint32_t earlier;

		location.source_position_id = map->source_position_id;
		if (zend_mir_verify_find_source(context, map->source_position_id) == NULL
				|| frame == NULL || !zend_mir_id_is_valid(map->op_array_id)
				|| (uint32_t) map->opline_phase >= ZEND_MIR_OPLINE_PHASE_COUNT
				|| (frame != NULL
					&& (frame->record.opline_index != map->opline_index
						|| frame->record.opline_phase != map->opline_phase))) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SOURCE_MAP,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
				location,
				ZEND_MIR_ID_INVALID,
				"source map does not match its source and owning frame");
		}
		for (earlier = 0; earlier < index; earlier++) {
			const zend_mir_source_map_ref *other =
				&context->source_maps[earlier].record;
			if (map->source_position_id == other->source_position_id
					&& map->op_array_id == other->op_array_id
					&& map->opline_index == other->opline_index
					&& map->opline_phase == other->opline_phase
					&& map->owner_frame_id == other->owner_frame_id) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SOURCE_MAP,
					ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID,
					location,
					ZEND_MIR_ID_INVALID,
					"source map association is duplicated");
				break;
			}
		}
	}
}

static bool zend_mir_verify_has_source_map(
		const zend_mir_verify_context *context,
		zend_mir_frame_state_id frame_id, zend_mir_source_position_id source_id)
{
	uint32_t index;
	for (index = 0; index < context->source_map_count; index++) {
		const zend_mir_source_map_ref *map = &context->source_maps[index].record;
		if (map->owner_frame_id == frame_id
				&& map->source_position_id == source_id) {
			return true;
		}
	}
	return false;
}

static zend_mir_safepoint_class zend_mir_verify_expected_frame_class(
		const zend_mir_instruction_record *instruction,
		const zend_mir_frame_state_ref *frame, bool entry)
{
	if (entry) {
		return ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY;
	}
	if ((instruction->barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_DESTRUCTOR)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR;
	}
	if ((instruction->barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_BAILOUT)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_BAILOUT_HELPER;
	}
	if ((instruction->barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_OBSERVER;
	}
	if ((instruction->barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_INTERRUPT)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_INTERRUPT;
	}
	if ((instruction->effects & ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_RESUME)) != 0) {
		return frame->suspend_kind == ZEND_MIR_SUSPEND_KIND_FIBER
			? ZEND_MIR_SAFEPOINT_CLASS_FIBER_SWITCH
			: frame->suspend_kind == ZEND_MIR_SUSPEND_KIND_DEOPT
				? ZEND_MIR_SAFEPOINT_CLASS_DEOPT_RESUME
				: ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_RESUME;
	}
	if ((instruction->barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_SUSPEND)) != 0) {
		return frame->suspend_kind == ZEND_MIR_SUSPEND_KIND_FIBER
			? ZEND_MIR_SAFEPOINT_CLASS_FIBER_SWITCH
			: ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	}
	if ((instruction->barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_EXCEPTION)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_EXCEPTION_EDGE;
	}
	if ((instruction->effects
			& (ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_CALL_PHP)
				| ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_REENTER_PHP))) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_USER_CALL;
	}
	if ((instruction->effects
			& ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_CALL_INTERNAL)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_INTERNAL_CALL;
	}
	if ((instruction->effects
			& ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_ALLOCATE)) != 0) {
		return ZEND_MIR_SAFEPOINT_CLASS_ALLOCATION;
	}
	return ZEND_MIR_SAFEPOINT_CLASS_INVALID;
}

static void zend_mir_verify_instruction_frame(
		zend_mir_verify_context *context,
		const zend_mir_verify_instruction *instruction, bool entry)
{
	bool observable = entry || instruction->record.barriers != 0
		|| instruction->record.opcode == ZEND_MIR_OPCODE_STATEPOINT
		|| instruction->record.opcode == ZEND_MIR_OPCODE_RETURN
		|| instruction->record.opcode == ZEND_MIR_OPCODE_THROW
		|| instruction->record.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
		|| instruction->record.opcode == ZEND_MIR_OPCODE_UNREACHABLE;
	const zend_mir_verify_frame *frame;
	const zend_mir_verify_block *block;
	zend_mir_safepoint_class expected;

	if (!observable) {
		return;
	}
	if (!zend_mir_id_is_valid(instruction->record.frame_state_id)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_MISSING_FRAME,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"observable instruction has no frame-state snapshot");
		return;
	}
	frame = zend_mir_verify_find_frame(context, instruction->record.frame_state_id);
	block = zend_mir_verify_find_block(context, instruction->record.block_id);
	if (frame == NULL || block == NULL
			|| frame->record.function_id != block->record.function_id) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_FRAME,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"instruction frame is unknown or belongs to another function");
		return;
	}
	if (!zend_mir_id_is_valid(instruction->record.source_position_id)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_MISSING_SOURCE,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"observable instruction has no source position");
	} else if (!zend_mir_verify_has_source_map(context,
			frame->record.id, instruction->record.source_position_id)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_SOURCE_MAP,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"observable instruction has no matching frame/source map");
	}
	expected = zend_mir_verify_expected_frame_class(
		&instruction->record, &frame->record, entry);
	if (expected != ZEND_MIR_SAFEPOINT_CLASS_INVALID
			&& frame->record.safepoint_class != expected) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_FRAME_CLASS_MISMATCH,
			ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
			zend_mir_verify_instruction_location(context, &instruction->record),
			ZEND_MIR_ID_INVALID,
			"frame safepoint class does not match observable semantics");
	}
	if (instruction->record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			|| instruction->record.barriers != 0) {
		uint32_t operand_index;
		for (operand_index = 0; operand_index < instruction->operands_count;
				operand_index++) {
			zend_mir_value_id value_id =
				context->operands[instruction->operands_offset + operand_index];
			if (!zend_mir_verify_frame_has_value(context, &frame->record, value_id)) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_MISSING_FRAME,
					ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
					zend_mir_verify_instruction_location(
						context, &instruction->record),
					value_id,
					"statepoint live operand is absent from the frame snapshot");
			}
		}
	}
}

static void zend_mir_verify_entry_statepoints(zend_mir_verify_context *context)
{
	uint32_t function_index;
	for (function_index = 0; function_index < context->function_count;
			function_index++) {
		const zend_mir_function_record *function =
			&context->functions[function_index].record;
		const zend_mir_verify_instruction *first = NULL;
		uint32_t index;

		for (index = 0; index < context->instruction_count; index++) {
			if (context->instructions[index].record.block_id
					== function->entry_block_id) {
				first = &context->instructions[index];
				break;
			}
		}
		if (first == NULL || first->record.opcode != ZEND_MIR_OPCODE_STATEPOINT) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_MISSING_FRAME,
				ZEND_MIR_DIAGNOSTIC_INVALID_FRAME_STATE,
				zend_mir_verify_function_location(context, function->id),
				ZEND_MIR_ID_INVALID,
				"function entry must begin with a statepoint snapshot");
		} else {
			zend_mir_verify_instruction_frame(context, first, true);
		}
	}
}

void zend_mir_verify_frames(zend_mir_verify_context *context)
{
	uint32_t index;

	for (index = 0; index < context->frame_count; index++) {
		zend_mir_verify_frame_record(context, &context->frames[index].record);
	}
	zend_mir_verify_parent_cycles(context);
	zend_mir_verify_sources(context);
	zend_mir_verify_entry_statepoints(context);
	for (index = 0; index < context->instruction_count; index++) {
		zend_mir_verify_instruction_frame(
			context, &context->instructions[index], false);
		if (context->halted) {
			return;
		}
	}
}
