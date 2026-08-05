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
	int32_t index = zend_mir_id_index_find(
		context->value_index, context->value_index_capacity, id);
	return index < 0 ? UINT32_MAX : (uint32_t) index;
}

static uint32_t zend_mir_verify_local_block_index(
		const zend_mir_id_index_entry *index, uint32_t capacity,
		zend_mir_block_id id)
{
	int32_t found = zend_mir_id_index_find(index, capacity, id);
	return found < 0 ? UINT32_MAX : (uint32_t) found;
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
		const zend_mir_id_index_entry *block_index, uint32_t block_index_capacity,
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
					block_index, block_index_capacity, predecessor_id);
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
		const zend_mir_id_index_entry *block_index, uint32_t block_index_capacity,
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
				uint32_t local = zend_mir_verify_local_block_index(
					block_index, block_index_capacity,
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

static void zend_mir_verify_use(zend_mir_verify_context *context,
		const zend_mir_verify_instruction *instruction, zend_mir_value_id operand,
		const uint32_t *definitions,
		const zend_mir_id_index_entry *block_index, uint32_t block_index_capacity,
		const zend_mir_id_index_entry *entry_parameters,
		uint32_t entry_parameter_capacity,
		const zend_mir_instruction_id *last_instructions,
		const uint64_t *dominators, uint32_t words,
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
		if (use_block == NULL || zend_mir_id_index_find(entry_parameters,
				entry_parameter_capacity, operand) < 0) {
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
		block_index, block_index_capacity, definition_block->record.id);
	use_local = zend_mir_verify_local_block_index(block_index, block_index_capacity,
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
			&& definition->record.id >= last_instructions[use_local]) {
		zend_mir_verify_emit(context, ZEND_MIR_VERIFY_PHI_EDGE_NOT_DOMINATING,
			ZEND_MIR_DIAGNOSTIC_INVALID_PHI,
			zend_mir_verify_instruction_location(context, &instruction->record),
			operand, "PHI edge input is not defined before the predecessor terminator");
	}
}

static void zend_mir_verify_function_dominance(zend_mir_verify_context *context,
		const zend_mir_function_record *function, const uint32_t *definitions,
		const uint32_t *first_instructions)
{
	const zend_mir_verify_block **blocks;
	zend_mir_id_index_entry *block_index;
	zend_mir_id_index_entry *entry_parameters = NULL;
	zend_mir_instruction_id *last_instructions;
	bool *reachable;
	uint64_t *dominators;
	uint32_t block_count = 0;
	uint32_t entry;
	uint32_t block_index_capacity;
	uint32_t entry_parameter_capacity = 0;
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
	if (!zend_mir_id_index_capacity(block_count, &block_index_capacity)) {
		return;
	}
	block_index = zend_mir_verify_allocate(
		context, block_index_capacity, sizeof(*block_index));
	last_instructions = zend_mir_verify_allocate(
		context, block_count, sizeof(*last_instructions));
	reachable = zend_mir_verify_allocate(context, block_count, sizeof(*reachable));
	words = (block_count + 63) / 64;
	dominators = zend_mir_verify_allocate(
		context, block_count * words, sizeof(*dominators));
	if (blocks == NULL || block_index == NULL || last_instructions == NULL
			|| reachable == NULL || dominators == NULL) {
		return;
	}
	block_count = 0;
	for (index = 0; index < context->block_count; index++) {
		if (context->blocks[index].record.function_id == function->id) {
			blocks[block_count] = &context->blocks[index];
			last_instructions[block_count] = ZEND_MIR_ID_INVALID;
			(void) zend_mir_id_index_insert(block_index, block_index_capacity,
				context->blocks[index].record.id, block_count, NULL);
			block_count++;
		}
	}
	entry = zend_mir_verify_local_block_index(
		block_index, block_index_capacity, function->entry_block_id);
	if (entry == UINT32_MAX) {
		return;
	}
	{
		uint32_t first_index = first_instructions[
			(uint32_t) (blocks[entry] - context->blocks)];
		if (first_index != UINT32_MAX
				&& context->instructions[first_index].record.opcode
					== ZEND_MIR_OPCODE_STATEPOINT) {
			const zend_mir_verify_instruction *first =
				&context->instructions[first_index];
			if (!zend_mir_id_index_capacity(
					first->operands_count, &entry_parameter_capacity)) {
				return;
			}
			entry_parameters = zend_mir_verify_allocate(context,
				entry_parameter_capacity, sizeof(*entry_parameters));
			if (entry_parameters == NULL) {
				return;
			}
			for (index = 0; index < first->operands_count; index++) {
				(void) zend_mir_id_index_insert(entry_parameters,
					entry_parameter_capacity,
					context->operands[first->operands_offset + index], index, NULL);
			}
		}
	}
	for (index = 0; index < context->instruction_count; index++) {
		uint32_t local = zend_mir_verify_local_block_index(block_index,
			block_index_capacity, context->instructions[index].record.block_id);
		if (local != UINT32_MAX) {
			last_instructions[local] = context->instructions[index].record.id;
		}
	}
	zend_mir_verify_compute_reachability(
		context, blocks, block_count, block_index, block_index_capacity,
		entry, reachable);
	zend_mir_verify_compute_dominators(
		context, blocks, block_count, block_index, block_index_capacity,
		entry, reachable, dominators, words);

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
				definitions, block_index, block_index_capacity,
				entry_parameters, entry_parameter_capacity, last_instructions,
				dominators, words, predecessor);
		}
	}
}

void zend_mir_verify_dominance(zend_mir_verify_context *context)
{
	uint32_t *definitions;
	uint32_t *first_instructions;
	uint32_t *publications;
	uint32_t *last_publication_function;
	uint32_t index;

	if (context->value_count == 0) {
		return;
	}
	definitions = zend_mir_verify_allocate(
		context, context->value_count, sizeof(*definitions));
	first_instructions = zend_mir_verify_allocate(
		context, context->block_count, sizeof(*first_instructions));
	publications = zend_mir_verify_allocate(
		context, context->value_count, sizeof(*publications));
	last_publication_function = zend_mir_verify_allocate(
		context, context->value_count, sizeof(*last_publication_function));
	if (definitions == NULL || (context->block_count != 0 && first_instructions == NULL)
			|| publications == NULL || last_publication_function == NULL) {
		return;
	}
	for (index = 0; index < context->value_count; index++) {
		definitions[index] = UINT32_MAX;
		publications[index] = 0;
		last_publication_function[index] = UINT32_MAX;
	}
	for (index = 0; index < context->block_count; index++) {
		first_instructions[index] = UINT32_MAX;
	}
	for (index = 0; index < context->instruction_count; index++) {
		zend_mir_value_id result = context->instructions[index].record.result_id;
		const zend_mir_verify_block *block = zend_mir_verify_find_block(
			context, context->instructions[index].record.block_id);
		uint32_t value_index;
		if (block != NULL) {
			uint32_t block_index = (uint32_t) (block - context->blocks);
			if (first_instructions[block_index] == UINT32_MAX) {
				first_instructions[block_index] = index;
			}
		}
		if (!zend_mir_id_is_valid(result)) {
			continue;
		}
		value_index = zend_mir_verify_dominance_value_index(context, result);
		if (value_index != UINT32_MAX && definitions[value_index] == UINT32_MAX) {
			definitions[value_index] = index;
		}
	}
	for (index = 0; index < context->function_count; index++) {
		const zend_mir_verify_block *entry = zend_mir_verify_find_block(
			context, context->functions[index].record.entry_block_id);
		uint32_t first_index;
		uint32_t operand_index;
		if (entry == NULL) {
			continue;
		}
		first_index = first_instructions[(uint32_t) (entry - context->blocks)];
		if (first_index == UINT32_MAX
				|| context->instructions[first_index].record.opcode
					!= ZEND_MIR_OPCODE_STATEPOINT) {
			continue;
		}
		for (operand_index = 0;
				operand_index < context->instructions[first_index].operands_count;
				operand_index++) {
			uint32_t value_index = zend_mir_verify_dominance_value_index(context,
				context->operands[context->instructions[first_index].operands_offset
					+ operand_index]);
			if (value_index != UINT32_MAX
					&& last_publication_function[value_index] != index) {
				publications[value_index]++;
				last_publication_function[value_index] = index;
			}
		}
	}
	for (index = 0; index < context->value_count; index++) {
		if (definitions[index] != UINT32_MAX) {
			continue;
		}
		if (publications[index] != 1) {
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
			context, &context->functions[index].record, definitions,
			first_instructions);
		if (context->halted) {
			return;
		}
	}
}
