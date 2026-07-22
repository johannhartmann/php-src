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

#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"

static uint32_t zend_mir_verify_edge_occurrences(
		const zend_mir_verify_context *context, const zend_mir_verify_block *block,
		zend_mir_block_id other, bool successor)
{
	const zend_mir_block_id *ids = successor ? context->successors : context->predecessors;
	uint32_t offset = successor ? block->successors_offset : block->predecessors_offset;
	uint32_t count = successor ? block->successors_count : block->predecessors_count;
	uint32_t occurrences = 0;
	uint32_t index;

	for (index = 0; index < count; index++) {
		if (ids[offset + index] == other) {
			occurrences++;
		}
	}
	return occurrences;
}

static void zend_mir_verify_edges(zend_mir_verify_context *context)
{
	uint32_t index;

	for (index = 0; index < context->block_count; index++) {
		const zend_mir_verify_block *block = &context->blocks[index];
		uint32_t edge_index;

		for (edge_index = 0; edge_index < block->successors_count; edge_index++) {
			zend_mir_block_id target_id =
				context->successors[block->successors_offset + edge_index];
			const zend_mir_verify_block *target =
				zend_mir_verify_find_block(context, target_id);
			uint32_t earlier;

			if (target == NULL || target->record.function_id != block->record.function_id) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_EDGE,
					ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
					zend_mir_verify_block_location(context, block->record.id),
					ZEND_MIR_ID_INVALID,
					"successor is unknown or belongs to another function");
				continue;
			}
			for (earlier = 0; earlier < edge_index; earlier++) {
				if (context->successors[block->successors_offset + earlier] == target_id) {
					zend_mir_verify_emit(context, ZEND_MIR_VERIFY_DUPLICATE_EDGE,
						ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
						zend_mir_verify_block_location(context, block->record.id),
						ZEND_MIR_ID_INVALID, "successor edge is duplicated");
					break;
				}
			}
			if (zend_mir_verify_edge_occurrences(
					context, target, block->record.id, false) != 1) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_EDGE_MISMATCH,
					ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
					zend_mir_verify_block_location(context, block->record.id),
					ZEND_MIR_ID_INVALID,
					"successor edge has no exact predecessor counterpart");
			}
		}
		for (edge_index = 0; edge_index < block->predecessors_count; edge_index++) {
			zend_mir_block_id source_id =
				context->predecessors[block->predecessors_offset + edge_index];
			const zend_mir_verify_block *source =
				zend_mir_verify_find_block(context, source_id);
			uint32_t earlier;

			if (source == NULL || source->record.function_id != block->record.function_id) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_EDGE,
					ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
					zend_mir_verify_block_location(context, block->record.id),
					ZEND_MIR_ID_INVALID,
					"predecessor is unknown or belongs to another function");
				continue;
			}
			for (earlier = 0; earlier < edge_index; earlier++) {
				if (context->predecessors[block->predecessors_offset + earlier] == source_id) {
					zend_mir_verify_emit(context, ZEND_MIR_VERIFY_DUPLICATE_EDGE,
						ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
						zend_mir_verify_block_location(context, block->record.id),
						ZEND_MIR_ID_INVALID, "predecessor edge is duplicated");
					break;
				}
			}
			if (zend_mir_verify_edge_occurrences(
					context, source, block->record.id, true) != 1) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_EDGE_MISMATCH,
					ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
					zend_mir_verify_block_location(context, block->record.id),
					ZEND_MIR_ID_INVALID,
					"predecessor edge has no exact successor counterpart");
			}
		}
	}
}

static void zend_mir_verify_reachability(zend_mir_verify_context *context)
{
	bool *reachable;
	uint32_t *worklist;
	uint32_t function_index;
	uint32_t block_index;

	if (context->block_count == 0) {
		return;
	}
	reachable = zend_mir_verify_allocate(
		context, context->block_count, sizeof(*reachable));
	worklist = zend_mir_verify_allocate(
		context, context->block_count, sizeof(*worklist));
	if (reachable == NULL || worklist == NULL) {
		return;
	}
	for (function_index = 0; function_index < context->function_count;
			function_index++) {
		const zend_mir_function_record *function =
			&context->functions[function_index].record;
		const zend_mir_verify_block *entry =
			zend_mir_verify_find_block(context, function->entry_block_id);
		uint32_t worklist_count = 0;

		if (entry == NULL || entry->record.function_id != function->id) {
			continue;
		}
		block_index = (uint32_t) (entry - context->blocks);
		if (!reachable[block_index]) {
			reachable[block_index] = true;
			worklist[worklist_count++] = block_index;
		}
		while (worklist_count != 0) {
			const zend_mir_verify_block *block =
				&context->blocks[worklist[--worklist_count]];
			uint32_t successor_index;

			for (successor_index = 0;
					successor_index < block->successors_count;
					successor_index++) {
				const zend_mir_verify_block *successor =
					zend_mir_verify_find_block(context,
						context->successors[
							block->successors_offset + successor_index]);
				uint32_t reachable_index;

				if (successor == NULL
						|| successor->record.function_id != function->id) {
					continue;
				}
				reachable_index = (uint32_t) (successor - context->blocks);
				if (!reachable[reachable_index]) {
					reachable[reachable_index] = true;
					worklist[worklist_count++] = reachable_index;
				}
			}
		}
	}
	for (block_index = 0; block_index < context->block_count; block_index++) {
		const zend_mir_verify_block *block = &context->blocks[block_index];
		const zend_mir_verify_function *function =
			zend_mir_verify_find_function(context, block->record.function_id);
		const zend_mir_verify_block *entry;

		if (function == NULL) {
			continue;
		}
		entry = zend_mir_verify_find_block(
			context, function->record.entry_block_id);
		if (entry != NULL && entry->record.function_id == function->record.id
				&& !reachable[block_index]) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_ENTRY,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_block_location(context, block->record.id),
				ZEND_MIR_ID_INVALID,
				"block is unreachable from its function entry");
		}
	}
}

static uint32_t zend_mir_verify_expected_operands(zend_mir_opcode opcode,
		uint32_t predecessor_count, bool *variable)
{
	*variable = false;
	switch (opcode) {
		case ZEND_MIR_OPCODE_CONSTANT:
		case ZEND_MIR_OPCODE_BRANCH:
		case ZEND_MIR_OPCODE_CATCH_ENTER:
		case ZEND_MIR_OPCODE_FINALLY_ENTER:
		case ZEND_MIR_OPCODE_FINALLY_CALL:
		case ZEND_MIR_OPCODE_FINALLY_RETURN:
		case ZEND_MIR_OPCODE_UNREACHABLE:
			return 0;
		case ZEND_MIR_OPCODE_COPY:
		case ZEND_MIR_OPCODE_CANONICALIZE:
		case ZEND_MIR_OPCODE_COND_BRANCH:
		case ZEND_MIR_OPCODE_THROW:
			return 1;
		case ZEND_MIR_OPCODE_PHI:
			return predecessor_count;
		case ZEND_MIR_OPCODE_RETURN:
			*variable = true;
			return 1;
		case ZEND_MIR_OPCODE_STATEPOINT:
			*variable = true;
			return UINT32_MAX;
		default: {
			const zend_mir_scalar_descriptor *descriptor =
				zend_mir_scalar_descriptor_at(opcode);

			return descriptor != NULL ? descriptor->operand_count : UINT32_MAX;
		}
	}
}

static bool zend_mir_verify_result_contract(
		const zend_mir_instruction_record *instruction)
{
	switch (instruction->opcode) {
		case ZEND_MIR_OPCODE_CONSTANT:
		case ZEND_MIR_OPCODE_PHI:
		case ZEND_MIR_OPCODE_COPY:
		case ZEND_MIR_OPCODE_CANONICALIZE:
			return zend_mir_id_is_valid(instruction->result_id)
				&& instruction->representation != ZEND_MIR_REPRESENTATION_VOID
				&& instruction->representation != ZEND_MIR_REPRESENTATION_CONTROL;
		case ZEND_MIR_OPCODE_BRANCH:
		case ZEND_MIR_OPCODE_COND_BRANCH:
		case ZEND_MIR_OPCODE_CATCH_ENTER:
		case ZEND_MIR_OPCODE_FINALLY_CALL:
		case ZEND_MIR_OPCODE_FINALLY_RETURN:
			return !zend_mir_id_is_valid(instruction->result_id)
				&& instruction->representation == ZEND_MIR_REPRESENTATION_CONTROL;
		case ZEND_MIR_OPCODE_STATEPOINT:
		case ZEND_MIR_OPCODE_FINALLY_ENTER:
		case ZEND_MIR_OPCODE_RETURN:
		case ZEND_MIR_OPCODE_THROW:
		case ZEND_MIR_OPCODE_UNREACHABLE:
			return !zend_mir_id_is_valid(instruction->result_id)
				&& instruction->representation == ZEND_MIR_REPRESENTATION_VOID;
		default: {
			const zend_mir_scalar_descriptor *descriptor =
				zend_mir_scalar_descriptor_at(instruction->opcode);

			if (descriptor == NULL) {
				return false;
			}
			return descriptor->has_result
				? zend_mir_id_is_valid(instruction->result_id)
					&& instruction->representation
						== descriptor->result.representation
				: !zend_mir_id_is_valid(instruction->result_id)
					&& instruction->representation
						== ZEND_MIR_REPRESENTATION_VOID;
		}
	}
}

static void zend_mir_verify_phi(zend_mir_verify_context *context,
		const zend_mir_verify_block *block,
		const zend_mir_verify_instruction *instruction)
{
	const zend_mir_verify_value *result =
		zend_mir_verify_find_value(context, instruction->record.result_id);
	zend_mir_ownership_state merged = ZEND_MIR_OWNERSHIP_STATE_INVALID;
	zend_mir_cleanup_obligation cleanup = ZEND_MIR_CLEANUP_INVALID;
	uint32_t index;

	if (instruction->operands_count != block->predecessors_count || result == NULL) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_PHI,
			ZEND_MIR_DIAGNOSTIC_INVALID_PHI,
			zend_mir_verify_instruction_location(context, &instruction->record),
			instruction->record.result_id,
			"PHI operand count must exactly match predecessor count");
		return;
	}
	for (index = 0; index < instruction->operands_count; index++) {
		zend_mir_value_id operand =
			context->operands[instruction->operands_offset + index];
		const zend_mir_verify_value *value = zend_mir_verify_find_value(context, operand);

		if (value == NULL) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_VALUE,
				ZEND_MIR_DIAGNOSTIC_INVALID_PHI,
				zend_mir_verify_instruction_location(context, &instruction->record),
				operand, "PHI input references an unknown value");
			continue;
		}
		if (value->record.representation != result->record.representation) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_PHI,
				ZEND_MIR_DIAGNOSTIC_INVALID_PHI,
				zend_mir_verify_instruction_location(context, &instruction->record),
				operand, "PHI input and result representations differ");
		}
		if (index == 0) {
			merged = value->record.ownership;
		} else if (zend_mir_ownership_phi_merge(
				merged, value->record.ownership, &merged, &cleanup)
				!= ZEND_MIR_PHI_MERGE_ALLOWED) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_PHI,
				ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
				zend_mir_verify_instruction_location(context, &instruction->record),
				operand,
				"PHI ownership merge is rejected or requires CANONICALIZE");
		}
	}
	if (instruction->operands_count != 0 && merged != result->record.ownership) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_PHI,
			ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
			zend_mir_verify_instruction_location(context, &instruction->record),
			result->record.id, "PHI result ownership does not match merged inputs");
	}
}

static void zend_mir_verify_block_instructions(
		zend_mir_verify_context *context, const zend_mir_verify_block *block)
{
	const zend_mir_verify_instruction *last = NULL;
	uint32_t instruction_in_block = 0;
	uint32_t terminators = 0;
	bool saw_non_phi = false;
	uint32_t index;

	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_verify_instruction *instruction = &context->instructions[index];
		bool variable;
		uint32_t expected;
		uint32_t operand_index;

		if (instruction->record.block_id != block->record.id) {
			continue;
		}
		instruction_in_block++;
		last = instruction;
		if (instruction->record.opcode == ZEND_MIR_OPCODE_PHI) {
			if (saw_non_phi) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_PHI,
					ZEND_MIR_DIAGNOSTIC_INVALID_PHI,
					zend_mir_verify_instruction_location(context, &instruction->record),
					ZEND_MIR_ID_INVALID, "PHI instructions must be first in their block");
			}
			zend_mir_verify_phi(context, block, instruction);
		} else {
			saw_non_phi = true;
		}
		if (zend_mir_opcode_is_terminator(instruction->record.opcode)) {
			terminators++;
		}
		expected = zend_mir_verify_expected_operands(
			instruction->record.opcode, block->predecessors_count, &variable);
		if (expected == UINT32_MAX
				? !variable
				: (variable ? instruction->operands_count > expected
					: instruction->operands_count != expected)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_OPERAND_COUNT,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE,
				zend_mir_verify_instruction_location(context, &instruction->record),
				ZEND_MIR_ID_INVALID, "opcode has an invalid operand count");
		}
		if (!zend_mir_verify_result_contract(&instruction->record)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_REPRESENTATION,
				ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE,
				zend_mir_verify_instruction_location(context, &instruction->record),
				instruction->record.result_id,
				"opcode result and representation contract is violated");
		}
		for (operand_index = 0; operand_index < instruction->operands_count; operand_index++) {
			zend_mir_value_id operand =
				context->operands[instruction->operands_offset + operand_index];
			if (zend_mir_verify_find_value(context, operand) == NULL) {
				zend_mir_verify_emit(context, ZEND_MIR_VERIFY_UNKNOWN_VALUE,
					ZEND_MIR_DIAGNOSTIC_INVALID_ID,
					zend_mir_verify_instruction_location(context, &instruction->record),
					operand, "instruction operand references an unknown value");
			}
		}
	}
	if (instruction_in_block == 0 || terminators == 0) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_MISSING_TERMINATOR,
			ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
			zend_mir_verify_block_location(context, block->record.id),
			ZEND_MIR_ID_INVALID, "block has no terminator");
	} else if (terminators != 1 || last == NULL
			|| !zend_mir_opcode_is_terminator(last->record.opcode)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_TERMINATOR,
			ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
			zend_mir_verify_block_location(context, block->record.id),
			ZEND_MIR_ID_INVALID,
			"block must end with exactly one terminator");
	} else {
		uint32_t expected_successors =
			last->record.opcode == ZEND_MIR_OPCODE_BRANCH ? 1
			: last->record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| last->record.opcode == ZEND_MIR_OPCODE_FINALLY_CALL ? 2 : 0;
		if ((last->record.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
				? block->successors_count != 1 && block->successors_count != 2
				: block->successors_count != expected_successors)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_TERMINATOR,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_instruction_location(context, &last->record),
				ZEND_MIR_ID_INVALID,
				"terminator successor count does not match opcode");
		}
	}
}

void zend_mir_verify_cfg(zend_mir_verify_context *context)
{
	uint32_t index;

	for (index = 0; index < context->function_count; index++) {
		const zend_mir_function_record *function = &context->functions[index].record;
		const zend_mir_verify_block *entry =
			zend_mir_verify_find_block(context, function->entry_block_id);

		if (entry == NULL || entry->record.function_id != function->id) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_ENTRY,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_function_location(context, function->id),
				ZEND_MIR_ID_INVALID,
				"function entry block is missing or belongs to another function");
		} else if (entry->predecessors_count != 0) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_ENTRY,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_block_location(context, entry->record.id),
				ZEND_MIR_ID_INVALID, "function entry block has predecessors");
		}
	}
	zend_mir_verify_edges(context);
	zend_mir_verify_reachability(context);
	if (context->halted) {
		return;
	}
	for (index = 0; index < context->block_count; index++) {
		const zend_mir_verify_block *block = &context->blocks[index];
		const zend_mir_verify_function *function =
			zend_mir_verify_find_function(context, block->record.function_id);
		if (function != NULL && block->record.id != function->record.entry_block_id
				&& block->predecessors_count == 0) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_INVALID_ENTRY,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_block_location(context, block->record.id),
				ZEND_MIR_ID_INVALID,
				"non-entry block has no predecessor in a single-entry function");
		}
		zend_mir_verify_block_instructions(context, &context->blocks[index]);
	}
}
