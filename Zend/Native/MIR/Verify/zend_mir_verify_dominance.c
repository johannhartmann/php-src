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

#include <string.h>

static uint32_t zend_mir_verify_dominance_value_index(
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

static uint32_t zend_mir_verify_local_block_index(
		const zend_mir_verify_block *const *blocks, uint32_t count,
		zend_mir_block_id id)
{
	uint32_t index;

	for (index = 0; index < count; index++) {
		if (blocks[index]->record.id == id) {
			return index;
		}
	}
	return UINT32_MAX;
}

static bool zend_mir_verify_bit_get(
		const uint64_t *sets, uint32_t words, uint32_t row, uint32_t bit)
{
	return (sets[(size_t) row * words + bit / 64] & (UINT64_C(1) << (bit % 64))) != 0;
}

static void zend_mir_verify_bit_set(
		uint64_t *sets, uint32_t words, uint32_t row, uint32_t bit)
{
	sets[(size_t) row * words + bit / 64] |= UINT64_C(1) << (bit % 64);
}

static void zend_mir_verify_compute_reachability(
		const zend_mir_verify_context *context,
		const zend_mir_verify_block *const *blocks, uint32_t block_count,
		uint32_t entry, bool *reachable)
{
	bool changed = true;

	reachable[entry] = true;
	while (changed) {
		uint32_t index;
		changed = false;
		for (index = 0; index < block_count; index++) {
			const zend_mir_verify_block *block = blocks[index];
			uint32_t predecessor;
			if (reachable[index]) {
				continue;
			}
			for (predecessor = 0; predecessor < block->predecessors_count; predecessor++) {
				zend_mir_block_id predecessor_id =
					context->predecessors[block->predecessors_offset + predecessor];
				uint32_t local = zend_mir_verify_local_block_index(
					blocks, block_count, predecessor_id);
				if (local != UINT32_MAX && reachable[local]) {
					reachable[index] = true;
					changed = true;
					break;
				}
			}
		}
	}
}

static void zend_mir_verify_compute_dominators(
		const zend_mir_verify_context *context,
		const zend_mir_verify_block *const *blocks, uint32_t block_count,
		uint32_t entry, const bool *reachable, uint64_t *dominators, uint32_t words)
{
	uint32_t row;
	bool changed = true;

	for (row = 0; row < block_count; row++) {
		uint32_t bit;
		if (!reachable[row] || row == entry) {
			zend_mir_verify_bit_set(dominators, words, row, row);
			continue;
		}
		for (bit = 0; bit < block_count; bit++) {
			if (reachable[bit]) {
				zend_mir_verify_bit_set(dominators, words, row, bit);
			}
		}
	}
	while (changed) {
		changed = false;
		for (row = 0; row < block_count; row++) {
			const zend_mir_verify_block *block = blocks[row];
			uint64_t *current = &dominators[(size_t) row * words];
			uint32_t predecessor;
			bool have_predecessor = false;
			uint32_t word;
			uint64_t replacement[ZEND_MIR_VERIFY_DOMINANCE_BLOCK_HARD_LIMIT / 64];

			if (!reachable[row] || row == entry) {
				continue;
			}
			for (word = 0; word < words; word++) {
				replacement[word] = UINT64_MAX;
			}
			for (predecessor = 0; predecessor < block->predecessors_count; predecessor++) {
				uint32_t local = zend_mir_verify_local_block_index(blocks, block_count,
					context->predecessors[block->predecessors_offset + predecessor]);
				if (local == UINT32_MAX || !reachable[local]) {
					continue;
				}
				have_predecessor = true;
				for (word = 0; word < words; word++) {
					replacement[word] &=
						dominators[(size_t) local * words + word];
				}
			}
			if (!have_predecessor) {
				memset(replacement, 0, sizeof(uint64_t) * words);
			}
			replacement[row / 64] |= UINT64_C(1) << (row % 64);
			if (memcmp(current, replacement, sizeof(uint64_t) * words) != 0) {
				memcpy(current, replacement, sizeof(uint64_t) * words);
				changed = true;
			}
		}
	}
}

static zend_mir_instruction_id zend_mir_verify_last_instruction_id(
		const zend_mir_verify_context *context, zend_mir_block_id block_id)
{
	zend_mir_instruction_id result = ZEND_MIR_ID_INVALID;
	uint32_t index;

	for (index = 0; index < context->instruction_count; index++) {
		if (context->instructions[index].record.block_id == block_id) {
			result = context->instructions[index].record.id;
		}
	}
	return result;
}

static bool zend_mir_verify_is_entry_parameter(
		const zend_mir_verify_context *context,
		zend_mir_function_id function_id, zend_mir_value_id value_id)
{
	const zend_mir_verify_function *function =
		zend_mir_verify_find_function(context, function_id);
	const zend_mir_verify_instruction *first = NULL;
	uint32_t index;

	if (function == NULL) {
		return false;
	}
	for (index = 0; index < context->instruction_count; index++) {
		if (context->instructions[index].record.block_id
				== function->record.entry_block_id) {
			first = &context->instructions[index];
			break;
		}
	}
	if (first == NULL || first->record.opcode != ZEND_MIR_OPCODE_STATEPOINT) {
		return false;
	}
	for (index = 0; index < first->operands_count; index++) {
		if (context->operands[first->operands_offset + index] == value_id) {
			return true;
		}
	}
	return false;
}

static void zend_mir_verify_use(zend_mir_verify_context *context,
		const zend_mir_verify_instruction *instruction, zend_mir_value_id operand,
		const uint32_t *definitions, const zend_mir_verify_block *const *blocks,
		uint32_t block_count, const uint64_t *dominators, uint32_t words,
		zend_mir_block_id edge_predecessor)
{
	uint32_t value_index = zend_mir_verify_dominance_value_index(context, operand);
	uint32_t definition_index;
	const zend_mir_verify_instruction *definition;
	const zend_mir_verify_block *definition_block;
	const zend_mir_verify_block *use_block;
	uint32_t definition_local;
	uint32_t use_local;
	bool phi_edge = zend_mir_id_is_valid(edge_predecessor);

	if (value_index == UINT32_MAX) {
		return;
	}
	definition_index = definitions[value_index];
	if (definition_index == UINT32_MAX) {
		/*
		 * The frozen opcode set has no ARG opcode. An undefined value is only
		 * well-formed when the function-entry STATEPOINT publishes it.
		 */
		use_block = zend_mir_verify_find_block(context, instruction->record.block_id);
		if (use_block == NULL || !zend_mir_verify_is_entry_parameter(
				context, use_block->record.function_id, operand)) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_USE_BEFORE_DEFINITION,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_instruction_location(context, &instruction->record),
				operand,
				"value has no instruction definition or function-entry publication");
		}
		return;
	}
	definition = &context->instructions[definition_index];
	definition_block = zend_mir_verify_find_block(context, definition->record.block_id);
	use_block = zend_mir_verify_find_block(context, instruction->record.block_id);
	if (definition_block == NULL || use_block == NULL
			|| definition_block->record.function_id != use_block->record.function_id) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_DEFINITION_NOT_DOMINATING,
			ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
			zend_mir_verify_instruction_location(context, &instruction->record),
			operand, "value definition belongs to another function");
		return;
	}
	definition_local = zend_mir_verify_local_block_index(
		blocks, block_count, definition_block->record.id);
	use_local = zend_mir_verify_local_block_index(blocks, block_count,
		phi_edge ? edge_predecessor : use_block->record.id);
	if (definition_local == UINT32_MAX || use_local == UINT32_MAX) {
		return;
	}
	if (!phi_edge && definition_block->record.id == use_block->record.id) {
		if (definition->record.id >= instruction->record.id) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_USE_BEFORE_DEFINITION,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_instruction_location(context, &instruction->record),
				operand, "value is used before its definition in the block");
		}
		return;
	}
	if (!zend_mir_verify_bit_get(
			dominators, words, use_local, definition_local)) {
		zend_mir_verify_emit(context,
			phi_edge ? ZEND_MIR_VERIFY_PHI_EDGE_NOT_DOMINATING
				: ZEND_MIR_VERIFY_DEFINITION_NOT_DOMINATING,
			phi_edge ? ZEND_MIR_DIAGNOSTIC_INVALID_PHI : ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
			zend_mir_verify_instruction_location(context, &instruction->record),
			operand,
			phi_edge ? "PHI input definition does not dominate its predecessor edge"
				: "value definition does not dominate its use");
	} else if (phi_edge && definition_block->record.id == edge_predecessor
			&& definition->record.id >=
				zend_mir_verify_last_instruction_id(context, edge_predecessor)) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_PHI_EDGE_NOT_DOMINATING,
			ZEND_MIR_DIAGNOSTIC_INVALID_PHI,
			zend_mir_verify_instruction_location(context, &instruction->record),
			operand, "PHI edge input is not defined before the predecessor terminator");
	}
}

static void zend_mir_verify_function_dominance(zend_mir_verify_context *context,
		const zend_mir_function_record *function, const uint32_t *definitions)
{
	const zend_mir_verify_block **blocks;
	bool *reachable;
	uint64_t *dominators;
	uint32_t block_count = 0;
	uint32_t entry;
	uint32_t words;
	uint32_t index;

	for (index = 0; index < context->block_count; index++) {
		if (context->blocks[index].record.function_id == function->id) {
			block_count++;
		}
	}
	if (block_count == 0 || block_count > ZEND_MIR_VERIFY_DOMINANCE_BLOCK_HARD_LIMIT) {
		if (block_count > ZEND_MIR_VERIFY_DOMINANCE_BLOCK_HARD_LIMIT) {
			zend_mir_verify_emit(context, ZEND_MIR_VERIFY_CAPACITY_EXCEEDED,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				zend_mir_verify_function_location(context, function->id),
				ZEND_MIR_ID_INVALID,
				"function block count exceeds deterministic dominance limit");
		}
		return;
	}
	blocks = zend_mir_verify_allocate(context, block_count, sizeof(*blocks));
	reachable = zend_mir_verify_allocate(context, block_count, sizeof(*reachable));
	words = (block_count + 63) / 64;
	dominators = zend_mir_verify_allocate(
		context, block_count * words, sizeof(*dominators));
	if (blocks == NULL || reachable == NULL || dominators == NULL) {
		return;
	}
	block_count = 0;
	for (index = 0; index < context->block_count; index++) {
		if (context->blocks[index].record.function_id == function->id) {
			blocks[block_count++] = &context->blocks[index];
		}
	}
	entry = zend_mir_verify_local_block_index(
		blocks, block_count, function->entry_block_id);
	if (entry == UINT32_MAX) {
		return;
	}
	zend_mir_verify_compute_reachability(
		context, blocks, block_count, entry, reachable);
	zend_mir_verify_compute_dominators(
		context, blocks, block_count, entry, reachable, dominators, words);

	for (index = 0; index < context->instruction_count; index++) {
		const zend_mir_verify_instruction *instruction = &context->instructions[index];
		const zend_mir_verify_block *block =
			zend_mir_verify_find_block(context, instruction->record.block_id);
		uint32_t operand_index;

		if (block == NULL || block->record.function_id != function->id) {
			continue;
		}
		for (operand_index = 0; operand_index < instruction->operands_count;
				operand_index++) {
			zend_mir_block_id predecessor = ZEND_MIR_ID_INVALID;
			if (instruction->record.opcode == ZEND_MIR_OPCODE_PHI
					&& operand_index < block->predecessors_count) {
				predecessor =
					context->predecessors[block->predecessors_offset + operand_index];
			}
			zend_mir_verify_use(context, instruction,
				context->operands[instruction->operands_offset + operand_index],
				definitions, blocks, block_count, dominators, words, predecessor);
		}
	}
}

void zend_mir_verify_dominance(zend_mir_verify_context *context)
{
	uint32_t *definitions;
	uint32_t index;

	if (context->value_count == 0) {
		return;
	}
	definitions = zend_mir_verify_allocate(
		context, context->value_count, sizeof(*definitions));
	if (definitions == NULL) {
		return;
	}
	for (index = 0; index < context->value_count; index++) {
		definitions[index] = UINT32_MAX;
	}
	for (index = 0; index < context->instruction_count; index++) {
		zend_mir_value_id result = context->instructions[index].record.result_id;
		uint32_t value_index;
		if (!zend_mir_id_is_valid(result)) {
			continue;
		}
		value_index = zend_mir_verify_dominance_value_index(context, result);
		if (value_index != UINT32_MAX && definitions[value_index] == UINT32_MAX) {
			definitions[value_index] = index;
		}
	}
	for (index = 0; index < context->value_count; index++) {
		uint32_t function_index;
		uint32_t publications = 0;

		if (definitions[index] != UINT32_MAX) {
			continue;
		}
		for (function_index = 0; function_index < context->function_count;
				function_index++) {
			if (zend_mir_verify_is_entry_parameter(context,
					context->functions[function_index].record.id,
					context->values[index].record.id)) {
				publications++;
			}
		}
		if (publications != 1) {
			zend_mir_verify_emit(context,
				ZEND_MIR_VERIFY_USE_BEFORE_DEFINITION,
				ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				zend_mir_verify_location(),
				context->values[index].record.id,
				"value requires exactly one instruction definition or entry publication");
		}
	}
	for (index = 0; index < context->function_count; index++) {
		zend_mir_verify_function_dominance(
			context, &context->functions[index].record, definitions);
		if (context->halted) {
			return;
		}
	}
}
