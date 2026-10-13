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

#include "zend_mir_module_internal.h"

#include <stdint.h>
#include <stdlib.h>

static bool zend_mir_checked_multiply_size(size_t left, size_t right, size_t *out)
{
	if (out == NULL || (right != 0 && left > SIZE_MAX / right)) {
		return false;
	}
	*out = left * right;
	return true;
}

typedef struct _zend_mir_core_edge_pair {
	zend_mir_block_id from;
	zend_mir_block_id to;
	bool occupied;
} zend_mir_core_edge_pair;

static size_t zend_mir_core_edge_pair_hash(zend_mir_block_id from,
		zend_mir_block_id to)
{
	uint64_t value = ((uint64_t) from << 32) | to;

	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return (size_t) value;
}

static bool zend_mir_core_edge_pair_insert(zend_mir_core_edge_pair *pairs,
		size_t capacity, zend_mir_block_id from, zend_mir_block_id to)
{
	size_t slot = zend_mir_core_edge_pair_hash(from, to) & (capacity - 1);

	while (pairs[slot].occupied) {
		if (pairs[slot].from == from && pairs[slot].to == to) {
			return false;
		}
		slot = (slot + 1) & (capacity - 1);
	}
	pairs[slot].from = from;
	pairs[slot].to = to;
	pairs[slot].occupied = true;
	return true;
}

bool zend_mir_module_prepare_edge_index(zend_mir_module *module)
{
	zend_mir_core_edge *edges;
	zend_mir_core_edge_pair *pairs = NULL;
	uint32_t *successor_cursors = NULL;
	uint32_t *predecessor_cursors = NULL;
	size_t pair_capacity = 8;
	size_t offsets_bytes;
	size_t edge_bytes;
	uint32_t predecessor_count = 0;
	uint32_t index;

	if (module->successor_offsets != NULL
			&& module->predecessor_offsets != NULL) {
		return true;
	}
	if (!zend_mir_checked_multiply_size(
			(size_t) module->blocks.count + 1, sizeof(uint32_t),
			&offsets_bytes)
			|| !zend_mir_checked_multiply_size(module->edges.count,
				sizeof(zend_mir_block_id), &edge_bytes)) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"MIR edge index capacity overflow");
	}
	while (pair_capacity < (size_t) module->edges.count * 2) {
		if (pair_capacity > SIZE_MAX / 2) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				"MIR edge-pair index capacity overflow");
		}
		pair_capacity *= 2;
	}
	module->successor_offsets = zend_mir_arena_allocate(&module->arena,
		offsets_bytes, alignof(uint32_t));
	module->predecessor_offsets = zend_mir_arena_allocate(&module->arena,
		offsets_bytes, alignof(uint32_t));
	pairs = calloc(pair_capacity, sizeof(*pairs));
	if (module->successor_offsets == NULL
			|| module->predecessor_offsets == NULL || pairs == NULL) {
		free(pairs);
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"MIR edge index allocation failed");
	}
	memset(module->successor_offsets, 0, offsets_bytes);
	memset(module->predecessor_offsets, 0, offsets_bytes);
	edges = ZEND_MIR_CORE_ITEMS(module, edges, zend_mir_core_edge);
	for (index = 0; index < module->edges.count; index++) {
		module->successor_offsets[edges[index].from + 1]++;
		if (zend_mir_core_edge_pair_insert(pairs, pair_capacity,
				edges[index].from, edges[index].to)) {
			module->predecessor_offsets[edges[index].to + 1]++;
			predecessor_count++;
		}
	}
	for (index = 0; index < module->blocks.count; index++) {
		module->successor_offsets[index + 1] +=
			module->successor_offsets[index];
		module->predecessor_offsets[index + 1] +=
			module->predecessor_offsets[index];
	}
	if (module->edges.count != 0) {
		module->successors = zend_mir_arena_allocate(&module->arena,
			edge_bytes, alignof(zend_mir_block_id));
	}
	if (predecessor_count != 0) {
		module->predecessors = zend_mir_arena_allocate(&module->arena,
			(size_t) predecessor_count * sizeof(zend_mir_block_id),
			alignof(zend_mir_block_id));
	}
	successor_cursors = malloc(
		(size_t) module->blocks.count * sizeof(*successor_cursors));
	predecessor_cursors = malloc(
		(size_t) module->blocks.count * sizeof(*predecessor_cursors));
	if ((module->edges.count != 0 && module->successors == NULL)
			|| (predecessor_count != 0 && module->predecessors == NULL)
			|| (module->blocks.count != 0
				&& (successor_cursors == NULL
					|| predecessor_cursors == NULL))) {
		free(pairs);
		free(successor_cursors);
		free(predecessor_cursors);
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"MIR edge index allocation failed");
	}
	if (module->blocks.count != 0) {
		memcpy(successor_cursors, module->successor_offsets,
			(size_t) module->blocks.count * sizeof(*successor_cursors));
		memcpy(predecessor_cursors, module->predecessor_offsets,
			(size_t) module->blocks.count * sizeof(*predecessor_cursors));
	}
	memset(pairs, 0, pair_capacity * sizeof(*pairs));
	for (index = 0; index < module->edges.count; index++) {
		module->successors[successor_cursors[edges[index].from]++] =
			edges[index].to;
		if (zend_mir_core_edge_pair_insert(pairs, pair_capacity,
				edges[index].from, edges[index].to)) {
			module->predecessors[predecessor_cursors[edges[index].to]++] =
				edges[index].from;
		}
	}
	free(pairs);
	free(successor_cursors);
	free(predecessor_cursors);
	return true;
}

static bool zend_mir_representation_is_valid(
		zend_mir_representation representation)
{
	return representation >= 0 && representation < ZEND_MIR_REPRESENTATION_COUNT;
}

static bool zend_mir_ownership_state_is_valid(
		zend_mir_ownership_state ownership)
{
	return ownership >= 0 && ownership < ZEND_MIR_OWNERSHIP_STATE_COUNT;
}

static bool zend_mir_constant_kind_is_valid(zend_mir_constant_kind kind)
{
	return kind >= 0 && kind < ZEND_MIR_CONSTANT_KIND_COUNT;
}

static bool zend_mir_opcode_is_valid(zend_mir_opcode opcode)
{
	return opcode >= 0 && opcode < ZEND_MIR_OPCODE_COUNT;
}

static void zend_mir_emit_diagnostic(zend_mir_diagnostic_sink *sink,
		zend_mir_module_id module_id, zend_mir_diagnostic_code code,
		const char *message)
{
	zend_mir_diagnostic diagnostic;
	size_t index = 0;

	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = code;
	diagnostic.severity = ZEND_MIR_DIAGNOSTIC_FATAL;
	diagnostic.location.module_id = module_id;
	diagnostic.location.function_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.block_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	if (message != NULL) {
		while (message[index] != '\0'
				&& index + 1 < ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY) {
			diagnostic.message[index] = message[index];
			index++;
		}
	}
	diagnostic.message[index] = '\0';
	(void) zend_mir_diagnostic_sink_emit(sink, &diagnostic);
}

zend_mir_core_limits zend_mir_core_default_limits(void)
{
	zend_mir_core_limits limits;

	limits.functions = UINT32_MAX;
	limits.blocks = UINT32_MAX;
	limits.instructions = UINT32_MAX;
	limits.values = UINT32_MAX;
	limits.constants = UINT32_MAX;
	limits.operands = UINT32_MAX;
	return limits;
}

bool zend_mir_module_fail(zend_mir_module *module, zend_mir_diagnostic_code code,
		const char *message)
{
	if (module == NULL) {
		return false;
	}
	if (module->state == ZEND_MIR_MODULE_BUILDING) {
		module->state = ZEND_MIR_MODULE_FAILED;
		zend_mir_emit_diagnostic(module->diagnostics, module->id, code, message);
	}
	return false;
}

bool zend_mir_module_require_building(zend_mir_module *module)
{
	return module != NULL && module->state == ZEND_MIR_MODULE_BUILDING;
}

zend_mir_module *zend_mir_module_create(zend_mir_module_id module_id,
		const zend_mir_allocator *allocator, size_t chunk_size,
		const zend_mir_core_limits *limits, zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_module *module;
	zend_mir_core_limits effective_limits;
	void *allocation;

	if (!zend_mir_core_id_validate(module_id) || allocator == NULL
			|| allocator->allocate == NULL || allocator->reset == NULL) {
		zend_mir_emit_diagnostic(diagnostics, module_id,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID, "invalid module ID or allocator");
		return NULL;
	}
	allocation = allocator->allocate(
		allocator->context, sizeof(zend_mir_module), alignof(zend_mir_module));
	if (allocation == NULL
			|| ((uintptr_t) allocation & (alignof(zend_mir_module) - 1)) != 0) {
		zend_mir_emit_diagnostic(diagnostics, module_id,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED, "module allocation failed");
		allocator->reset(allocator->context);
		return NULL;
	}
	module = (zend_mir_module *) allocation;
	memset(module, 0, sizeof(*module));
	if (!zend_mir_arena_init(&module->arena, allocator, chunk_size)) {
		allocator->reset(allocator->context);
		return NULL;
	}
	effective_limits = limits != NULL ? *limits : zend_mir_core_default_limits();
	module->id = module_id;
	module->state = ZEND_MIR_MODULE_BUILDING;
	module->limits = effective_limits;
	module->diagnostics = diagnostics;
	zend_mir_module_init_view(module);
	zend_mir_module_init_mutator(module);
	zend_mir_module_init_call_view(module);
	zend_mir_module_init_call_mutator(module);
	return module;
}

bool zend_mir_module_reserve_table(zend_mir_module *module,
		zend_mir_core_table *table, size_t item_size, size_t item_alignment,
		uint32_t limit, uint32_t count)
{
	uint32_t required;
	uint32_t capacity;
	size_t bytes;
	void *items;

	if (!zend_mir_module_require_building(module) || table == NULL
			|| item_size == 0 || count == 0 || count - 1 >= limit
			|| !zend_mir_core_next_id(count - 1, &required)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"core table count limit exceeded");
	}
	required++;
	if (required <= table->capacity) {
		return true;
	}
	capacity = table->capacity == 0 ? UINT32_C(4) : table->capacity;
	if (capacity > limit) {
		capacity = limit;
	}
	while (capacity < required) {
		uint32_t next = capacity > UINT32_MAX / 2 ? limit : capacity * 2;

		if (next > limit) {
			next = limit;
		}
		if (next <= capacity) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				"core table capacity overflow");
		}
		capacity = next;
	}
	if ((size_t) capacity > SIZE_MAX / item_size) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"core table byte size overflow");
	}
	bytes = (size_t) capacity * item_size;
	items = zend_mir_arena_allocate(&module->arena, bytes, item_alignment);
	if (items == NULL) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"core table allocation failed");
	}
	if (table->count != 0) {
		memcpy(items, table->items, (size_t) table->count * item_size);
	}
	table->items = items;
	table->capacity = capacity;
	return true;
}

bool zend_mir_module_grow_table(zend_mir_module *module, zend_mir_core_table *table,
		size_t item_size, size_t item_alignment, uint32_t limit)
{
	return zend_mir_module_reserve_table(module, table, item_size,
		item_alignment, limit,
		table != NULL && table->count != UINT32_MAX ? table->count + 1 : 0);
}

bool zend_mir_module_find_value(const zend_mir_module *module, zend_mir_value_id id,
		uint32_t *index_out)
{
	uint32_t slot;
	uint32_t probe;

	if (module == NULL || module->value_index_capacity == 0) {
		return false;
	}
	slot = zend_mir_core_hash_id(id) & (module->value_index_capacity - 1);
	for (probe = 0; probe < module->value_index_capacity; probe++) {
		uint32_t stored = module->value_index[slot];
		zend_mir_core_value *values;

		if (stored == 0) {
			return false;
		}
		values = ZEND_MIR_CORE_ITEMS(module, values, zend_mir_core_value);
		if (values[stored - 1].record.id == id) {
			if (index_out != NULL) {
				*index_out = stored - 1;
			}
			return true;
		}
		slot = (slot + 1) & (module->value_index_capacity - 1);
	}
	return false;
}

void zend_mir_module_insert_value_index(zend_mir_module *module,
		zend_mir_value_id id, uint32_t index)
{
	uint32_t slot = zend_mir_core_hash_id(id) & (module->value_index_capacity - 1);

	while (module->value_index[slot] != 0) {
		slot = (slot + 1) & (module->value_index_capacity - 1);
	}
	module->value_index[slot] = index + 1;
}

bool zend_mir_module_prepare_value_index(zend_mir_module *module)
{
	uint32_t required;
	uint32_t capacity;
	uint32_t *index;
	uint32_t item;
	size_t bytes;

	if (module->values.count == UINT32_MAX) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"value index count overflow");
	}
	required = module->values.count + 1;
	if (module->value_index_capacity != 0
			&& required <= module->value_index_capacity / 2) {
		return true;
	}
	capacity = module->value_index_capacity == 0
		? UINT32_C(8) : module->value_index_capacity;
	while (required > capacity / 2) {
		if (capacity > UINT32_MAX / 2) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				"value index capacity overflow");
		}
		capacity *= 2;
	}
	if (!zend_mir_checked_multiply_size(
			(size_t) capacity, sizeof(uint32_t), &bytes)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"value index byte size overflow");
	}
	index = zend_mir_arena_allocate(&module->arena, bytes, alignof(uint32_t));
	if (index == NULL) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"value index allocation failed");
	}
	memset(index, 0, bytes);
	module->value_index = index;
	module->value_index_capacity = capacity;
	for (item = 0; item < module->values.count; item++) {
		zend_mir_core_value *values =
			ZEND_MIR_CORE_ITEMS(module, values, zend_mir_core_value);

		zend_mir_module_insert_value_index(module, values[item].record.id, item);
	}
	return true;
}

static zend_mir_core_function *zend_mir_find_function(zend_mir_module *module,
		zend_mir_function_id id)
{
	zend_mir_core_function *functions;

	if (module == NULL || id >= module->functions.count) {
		return NULL;
	}
	functions = ZEND_MIR_CORE_ITEMS(module, functions, zend_mir_core_function);
	return functions[id].record.id == id ? &functions[id] : NULL;
}

static zend_mir_block_record *zend_mir_find_block(zend_mir_module *module,
		zend_mir_block_id id)
{
	zend_mir_block_record *blocks;

	if (module == NULL || id >= module->blocks.count) {
		return NULL;
	}
	blocks = ZEND_MIR_CORE_ITEMS(module, blocks, zend_mir_block_record);
	return blocks[id].id == id ? &blocks[id] : NULL;
}

static zend_mir_core_instruction *zend_mir_find_instruction(zend_mir_module *module,
		zend_mir_instruction_id id)
{
	zend_mir_core_instruction *instructions;

	if (module == NULL || id >= module->instructions.count) {
		return NULL;
	}
	instructions = ZEND_MIR_CORE_ITEMS(module, instructions, zend_mir_core_instruction);
	return instructions[id].record.id == id ? &instructions[id] : NULL;
}

static bool zend_mir_function_is_open(zend_mir_module *module,
		zend_mir_function_id id)
{
	zend_mir_core_function *function = zend_mir_find_function(module, id);

	return function != NULL && !function->sealed;
}

static bool zend_mir_add_function(void *context, zend_mir_symbol_id symbol_id,
		zend_mir_function_id *out)
{
	zend_mir_module *module = context;
	zend_mir_core_function *functions;
	zend_mir_function_id id;

	if (!zend_mir_module_require_building(module) || out == NULL
			|| !zend_mir_core_id_validate(symbol_id)
			|| !zend_mir_module_grow_table(module, &module->functions,
				sizeof(zend_mir_core_function), alignof(zend_mir_core_function),
				module->limits.functions)
			|| !zend_mir_core_next_id(module->functions.count, &id)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"cannot add function");
	}
	functions = ZEND_MIR_CORE_ITEMS(module, functions, zend_mir_core_function);
	functions[module->functions.count].record.id = id;
	functions[module->functions.count].record.symbol_id = symbol_id;
	functions[module->functions.count].record.entry_block_id = ZEND_MIR_ID_INVALID;
	functions[module->functions.count].record.flags = 0;
	functions[module->functions.count].sealed = false;
	module->functions.count++;
	*out = id;
	return true;
}

static bool zend_mir_add_block(void *context, zend_mir_function_id function_id,
		zend_mir_block_id *out)
{
	zend_mir_module *module = context;
	zend_mir_block_record *blocks;
	zend_mir_block_id id;

	if (!zend_mir_module_require_building(module) || out == NULL
			|| !zend_mir_function_is_open(module, function_id)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"cannot add block to function");
	}
	if (!zend_mir_module_grow_table(module, &module->blocks,
			sizeof(zend_mir_block_record), alignof(zend_mir_block_record),
			module->limits.blocks)
			|| !zend_mir_core_next_id(module->blocks.count, &id)) {
		return false;
	}
	blocks = ZEND_MIR_CORE_ITEMS(module, blocks, zend_mir_block_record);
	blocks[module->blocks.count].id = id;
	blocks[module->blocks.count].function_id = function_id;
	module->blocks.count++;
	*out = id;
	return true;
}

static bool zend_mir_set_entry_block(void *context, zend_mir_function_id function_id,
		zend_mir_block_id block_id)
{
	zend_mir_module *module = context;
	zend_mir_core_function *function;
	zend_mir_block_record *block;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	function = zend_mir_find_function(module, function_id);
	block = zend_mir_find_block(module, block_id);
	if (function == NULL || function->sealed || block == NULL
			|| block->function_id != function_id) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"entry block does not belong to open function");
	}
	function->record.entry_block_id = block_id;
	return true;
}

static bool zend_mir_add_value(void *context, zend_mir_value_id requested_id,
		zend_mir_representation representation, zend_mir_ownership_state ownership)
{
	zend_mir_module *module = context;
	zend_mir_core_value *values;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	if (!zend_mir_core_id_validate(requested_id)
			|| !zend_mir_representation_is_valid(representation)
			|| !zend_mir_ownership_state_is_valid(ownership)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid value record");
	}
	if (zend_mir_module_find_value(module, requested_id, NULL)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID,
			"duplicate value ID");
	}
	if (module->values.count >= module->limits.values
			|| !zend_mir_module_prepare_value_index(module)
			|| !zend_mir_module_grow_table(module, &module->values,
				sizeof(zend_mir_core_value), alignof(zend_mir_core_value),
				module->limits.values)) {
		return false;
	}
	values = ZEND_MIR_CORE_ITEMS(module, values, zend_mir_core_value);
	values[module->values.count].record.id = requested_id;
	values[module->values.count].record.representation = representation;
	values[module->values.count].record.ownership = ownership;
	values[module->values.count].constant_index = ZEND_MIR_ID_INVALID;
	zend_mir_module_insert_value_index(module, requested_id, module->values.count);
	module->values.count++;
	return true;
}

static bool zend_mir_add_constant(void *context,
		const zend_mir_constant_record *constant)
{
	zend_mir_module *module = context;
	zend_mir_core_value *values;
	zend_mir_constant_record *constants;
	uint32_t value_index;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	if (constant == NULL
			|| !zend_mir_representation_is_valid(constant->representation)
			|| !zend_mir_constant_kind_is_valid(constant->kind)
			|| !zend_mir_module_find_value(module, constant->value_id, &value_index)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid constant record");
	}
	values = ZEND_MIR_CORE_ITEMS(module, values, zend_mir_core_value);
	if (values[value_index].constant_index != ZEND_MIR_ID_INVALID) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID,
			"value already has a constant");
	}
	if (!zend_mir_module_grow_table(module, &module->constants,
			sizeof(zend_mir_constant_record), alignof(zend_mir_constant_record),
			module->limits.constants)) {
		return false;
	}
	constants = ZEND_MIR_CORE_ITEMS(module, constants, zend_mir_constant_record);
	memset(&constants[module->constants.count], 0,
		sizeof(constants[module->constants.count]));
	constants[module->constants.count].value_id = constant->value_id;
	constants[module->constants.count].representation = constant->representation;
	constants[module->constants.count].kind = constant->kind;
	constants[module->constants.count].payload_bits = constant->payload_bits;
	constants[module->constants.count].symbol_id = constant->symbol_id;
	values[value_index].constant_index = module->constants.count;
	module->constants.count++;
	return true;
}

static bool zend_mir_add_instruction(void *context,
		const zend_mir_instruction_record *record, zend_mir_instruction_id *out)
{
	zend_mir_module *module = context;
	zend_mir_block_record *block;
	zend_mir_core_instruction *instructions;
	zend_mir_instruction_id id;

	if (!zend_mir_module_require_building(module) || record == NULL || out == NULL) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid instruction arguments");
	}
	/* An instruction added after the call model follows its calls. */
	if (module->pending_calls.pending
			&& !zend_mir_module_compose_instructions(module, NULL, 0, NULL)) {
		return false;
	}
	block = zend_mir_find_block(module, record->block_id);
	if (block == NULL || !zend_mir_function_is_open(module, block->function_id)
			|| !zend_mir_opcode_is_valid(record->opcode)
			|| !zend_mir_representation_is_valid(record->representation)
			|| (record->result_id != ZEND_MIR_ID_INVALID
				&& !zend_mir_module_find_value(module, record->result_id, NULL))) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_OPCODE,
			"invalid instruction record");
	}
	if (!zend_mir_module_grow_table(module, &module->instructions,
			sizeof(zend_mir_core_instruction), alignof(zend_mir_core_instruction),
			module->limits.instructions)
			|| !zend_mir_core_next_id(module->instructions.count, &id)) {
		return false;
	}
	instructions = ZEND_MIR_CORE_ITEMS(module, instructions, zend_mir_core_instruction);
	memset(&instructions[module->instructions.count], 0,
		sizeof(instructions[module->instructions.count]));
	instructions[module->instructions.count].record.id = id;
	instructions[module->instructions.count].record.block_id = record->block_id;
	instructions[module->instructions.count].record.opcode = record->opcode;
	instructions[module->instructions.count].record.representation =
		record->representation;
	instructions[module->instructions.count].record.result_id = record->result_id;
	instructions[module->instructions.count].record.frame_state_id =
		record->frame_state_id;
	instructions[module->instructions.count].record.source_position_id =
		record->source_position_id;
	instructions[module->instructions.count].record.effects = record->effects;
	instructions[module->instructions.count].record.reads = record->reads;
	instructions[module->instructions.count].record.writes = record->writes;
	instructions[module->instructions.count].record.barriers = record->barriers;
	instructions[module->instructions.count].record.ownership_actions =
		record->ownership_actions;
	module->instructions.count++;
	*out = id;
	return true;
}

static bool zend_mir_grow_operands(zend_mir_module *module,
		zend_mir_core_instruction *instruction)
{
	uint32_t required;
	uint32_t capacity;
	zend_mir_value_id *operands;
	size_t bytes;

	if (module->operand_count >= module->limits.operands
			|| instruction->operand_count == UINT32_MAX) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"operand count limit exceeded");
	}
	required = instruction->operand_count + 1;
	if (required <= instruction->operand_capacity) {
		return true;
	}
	capacity = instruction->operand_capacity == 0
		? UINT32_C(4) : instruction->operand_capacity;
	while (capacity < required) {
		if (capacity > UINT32_MAX / 2) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				"operand capacity overflow");
		}
		capacity *= 2;
	}
	if (!zend_mir_checked_multiply_size(
			(size_t) capacity, sizeof(zend_mir_value_id), &bytes)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"operand byte size overflow");
	}
	operands = zend_mir_arena_allocate(
		&module->arena, bytes, alignof(zend_mir_value_id));
	if (operands == NULL) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"operand allocation failed");
	}
	if (instruction->operand_count != 0) {
		memcpy(operands, instruction->operands,
			(size_t) instruction->operand_count * sizeof(zend_mir_value_id));
	}
	instruction->operands = operands;
	instruction->operand_capacity = capacity;
	return true;
}

static bool zend_mir_add_operand(void *context,
		zend_mir_instruction_id instruction_id, zend_mir_value_id value_id)
{
	zend_mir_module *module = context;
	zend_mir_core_instruction *instruction;
	zend_mir_block_record *block;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	instruction = zend_mir_find_instruction(module, instruction_id);
	block = instruction != NULL ? zend_mir_find_block(module,
		instruction->record.block_id) : NULL;
	if (instruction == NULL || block == NULL
			|| !zend_mir_function_is_open(module, block->function_id)
			|| !zend_mir_module_find_value(module, value_id, NULL)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid instruction operand");
	}
	if (!zend_mir_grow_operands(module, instruction)) {
		return false;
	}
	instruction->operands[instruction->operand_count] = value_id;
	instruction->operand_count++;
	module->operand_count++;
	return true;
}

static bool zend_mir_add_edge(void *context, zend_mir_block_id from,
		zend_mir_block_id to)
{
	zend_mir_module *module = context;
	zend_mir_block_record *from_block;
	zend_mir_block_record *to_block;
	zend_mir_core_edge *edges;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	from_block = zend_mir_find_block(module, from);
	to_block = zend_mir_find_block(module, to);
	if (from_block == NULL || to_block == NULL
			|| from_block->function_id != to_block->function_id
			|| !zend_mir_function_is_open(module, from_block->function_id)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"CFG edge blocks must belong to the same open function");
	}
	if (!zend_mir_module_grow_table(module, &module->edges,
			sizeof(zend_mir_core_edge), alignof(zend_mir_core_edge),
			UINT32_MAX)) {
		return false;
	}
	edges = ZEND_MIR_CORE_ITEMS(module, edges, zend_mir_core_edge);
	edges[module->edges.count].from = from;
	edges[module->edges.count].to = to;
	module->successor_offsets = NULL;
	module->successors = NULL;
	module->predecessor_offsets = NULL;
	module->predecessors = NULL;
	module->edges.count++;
	return true;
}

static bool zend_mir_add_source_position(void *context,
		const zend_mir_source_position_ref *source_position,
		zend_mir_source_position_id *out)
{
	zend_mir_module *module = context;
	zend_mir_source_position_ref *records;
	zend_mir_source_position_ref record;
	uint32_t index;

	if (!zend_mir_module_require_building(module)
			|| source_position == NULL || out == NULL
			|| !zend_mir_id_is_valid(source_position->file_symbol_id)
			|| source_position->line == 0
			|| source_position->column_start == 0
			|| source_position->column_start > source_position->column_end) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid source position");
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, source_positions, zend_mir_source_position_ref);
	if (source_position->id != ZEND_MIR_ID_INVALID
			&& source_position->id < module->source_positions.count) {
		const zend_mir_source_position_ref *existing =
			&records[source_position->id];

		if (existing->file_symbol_id != source_position->file_symbol_id
				|| existing->line != source_position->line
				|| existing->column_start != source_position->column_start
				|| existing->column_end != source_position->column_end) {
			return zend_mir_module_fail(
				module, ZEND_MIR_DIAGNOSTIC_DUPLICATE_ID,
				"source position ID conflicts with existing record");
		}
		*out = existing->id;
		return true;
	}
	if (source_position->id == ZEND_MIR_ID_INVALID) {
		for (index = 0; index < module->source_positions.count; index++) {
			if (records[index].file_symbol_id
						== source_position->file_symbol_id
					&& records[index].line == source_position->line
					&& records[index].column_start
						== source_position->column_start
					&& records[index].column_end
						== source_position->column_end) {
				*out = records[index].id;
				return true;
			}
		}
	}
	if (source_position->id != ZEND_MIR_ID_INVALID
			&& source_position->id != module->source_positions.count) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"source position IDs must be canonical");
	}
	if (!zend_mir_module_grow_table(
			module, &module->source_positions,
			sizeof(zend_mir_source_position_ref),
			alignof(zend_mir_source_position_ref), UINT32_MAX)) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, source_positions, zend_mir_source_position_ref);
	record = *source_position;
	record.id = module->source_positions.count;
	records[module->source_positions.count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_add_frame_slot(void *context,
		const zend_mir_frame_slot_ref *slot, uint32_t *index_out)
{
	zend_mir_module *module = context;
	zend_mir_frame_slot_ref *records;

	if (!zend_mir_module_require_building(module) || slot == NULL
			|| index_out == NULL || !zend_mir_id_is_valid(slot->slot_id)
			|| slot->kind < 0 || slot->kind >= ZEND_MIR_FRAME_SLOT_KIND_COUNT
			|| slot->representation < 0
			|| slot->representation
				>= ZEND_MIR_FRAME_SLOT_REPRESENTATION_COUNT
			|| slot->materialization < 0
			|| slot->materialization >= ZEND_MIR_MATERIALIZATION_COUNT
			|| slot->ownership < 0
			|| slot->ownership >= ZEND_MIR_FRAME_SLOT_OWNERSHIP_COUNT
			|| ((slot->materialization == ZEND_MIR_MATERIALIZATION_UNDEF
					|| slot->materialization
						== ZEND_MIR_MATERIALIZATION_SOURCE_ZVAL)
				? zend_mir_id_is_valid(slot->value_id)
				: !zend_mir_module_find_value(
					module, slot->value_id, NULL))) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid frame slot");
	}
	if (!zend_mir_module_grow_table(
			module, &module->frame_slots, sizeof(zend_mir_frame_slot_ref),
			alignof(zend_mir_frame_slot_ref), UINT32_MAX)) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, frame_slots, zend_mir_frame_slot_ref);
	*index_out = module->frame_slots.count;
	records[module->frame_slots.count++] = *slot;
	return true;
}

static bool zend_mir_has_frame_slot_id(
	const zend_mir_module *module, uint32_t slot_id)
{
	const zend_mir_frame_slot_ref *records = ZEND_MIR_CORE_ITEMS(
		module, frame_slots, zend_mir_frame_slot_ref);
	uint32_t index;

	for (index = 0; index < module->frame_slots.count; index++) {
		if (records[index].slot_id == slot_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_add_root(void *context, uint32_t slot_id, uint32_t *index_out)
{
	zend_mir_module *module = context;
	uint32_t *records;

	if (!zend_mir_module_require_building(module) || index_out == NULL
			|| !zend_mir_id_is_valid(slot_id)
			|| !zend_mir_has_frame_slot_id(module, slot_id)) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid frame root");
	}
	if (!zend_mir_module_grow_table(
			module, &module->roots, sizeof(uint32_t), alignof(uint32_t),
			UINT32_MAX)) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, roots, uint32_t);
	*index_out = module->roots.count;
	records[module->roots.count++] = slot_id;
	return true;
}

static bool zend_mir_add_cleanup(void *context,
		const zend_mir_cleanup_ref *cleanup, uint32_t *index_out)
{
	zend_mir_module *module = context;
	zend_mir_cleanup_ref *records;

	if (!zend_mir_module_require_building(module) || cleanup == NULL
			|| index_out == NULL || !zend_mir_id_is_valid(cleanup->slot_id)
			|| !zend_mir_has_frame_slot_id(module, cleanup->slot_id)
			|| cleanup->action < 0
			|| cleanup->action >= ZEND_MIR_CLEANUP_ACTION_COUNT
			|| cleanup->state < 0
			|| cleanup->state >= ZEND_MIR_CLEANUP_STATE_COUNT) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid cleanup record");
	}
	if (!zend_mir_module_grow_table(
			module, &module->cleanups, sizeof(zend_mir_cleanup_ref),
			alignof(zend_mir_cleanup_ref), UINT32_MAX)) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, cleanups, zend_mir_cleanup_ref);
	*index_out = module->cleanups.count;
	records[module->cleanups.count++] = *cleanup;
	return true;
}

static bool zend_mir_add_frame_state(void *context,
		const zend_mir_frame_state_ref *frame_state, zend_mir_frame_state_id *out)
{
	zend_mir_module *module = context;
	zend_mir_frame_state_ref *records;
	zend_mir_frame_state_ref record;

	if (!zend_mir_module_require_building(module) || frame_state == NULL
			|| out == NULL
			|| (frame_state->id != ZEND_MIR_ID_INVALID
				&& frame_state->id != module->frame_states.count)
			|| zend_mir_find_function(module, frame_state->function_id) == NULL
			|| (frame_state->parent_id != ZEND_MIR_ID_INVALID
				&& frame_state->parent_id >= module->frame_states.count)
			|| frame_state->function_kind < 0
			|| frame_state->function_kind >= ZEND_MIR_FUNCTION_KIND_COUNT
			|| frame_state->opline_phase < 0
			|| frame_state->opline_phase >= ZEND_MIR_OPLINE_PHASE_COUNT
			|| frame_state->slots.offset > module->frame_slots.count
			|| frame_state->slots.count
				> module->frame_slots.count - frame_state->slots.offset
			|| frame_state->roots.offset > module->roots.count
			|| frame_state->roots.count
				> module->roots.count - frame_state->roots.offset
			|| frame_state->cleanup_obligations.offset
				> module->cleanups.count
			|| frame_state->cleanup_obligations.count
				> module->cleanups.count
					- frame_state->cleanup_obligations.offset
			|| frame_state->return_continuation.kind < 0
			|| frame_state->return_continuation.kind
				>= ZEND_MIR_CONTINUATION_KIND_COUNT
			|| frame_state->exception_continuation.kind < 0
			|| frame_state->exception_continuation.kind
				>= ZEND_MIR_CONTINUATION_KIND_COUNT
			|| frame_state->bailout_continuation.kind < 0
			|| frame_state->bailout_continuation.kind
				>= ZEND_MIR_CONTINUATION_KIND_COUNT
			|| frame_state->suspend_kind < 0
			|| frame_state->suspend_kind >= ZEND_MIR_SUSPEND_KIND_COUNT
			|| frame_state->resume.entry_kind < 0
			|| frame_state->resume.entry_kind
				>= ZEND_MIR_RESUME_ENTRY_KIND_COUNT
			|| frame_state->safepoint_class < 0
			|| frame_state->safepoint_class >= ZEND_MIR_SAFEPOINT_CLASS_COUNT) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid frame state");
	}
	if (!zend_mir_module_grow_table(
			module, &module->frame_states, sizeof(zend_mir_frame_state_ref),
			alignof(zend_mir_frame_state_ref), UINT32_MAX)) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, frame_states, zend_mir_frame_state_ref);
	record = *frame_state;
	record.id = module->frame_states.count;
	records[module->frame_states.count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_add_source_map(void *context,
		const zend_mir_source_map_ref *source_map, zend_mir_source_map_id *out)
{
	zend_mir_module *module = context;
	zend_mir_source_map_ref *records;
	zend_mir_source_map_ref record;

	if (!zend_mir_module_require_building(module) || source_map == NULL
			|| out == NULL
			|| (source_map->id != ZEND_MIR_ID_INVALID
				&& source_map->id != module->source_maps.count)
			|| source_map->source_position_id
				>= module->source_positions.count
			|| !zend_mir_id_is_valid(source_map->op_array_id)
			|| source_map->opline_phase < 0
			|| source_map->opline_phase >= ZEND_MIR_OPLINE_PHASE_COUNT
			|| source_map->owner_frame_id >= module->frame_states.count) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid source map");
	}
	if (!zend_mir_module_grow_table(
			module, &module->source_maps, sizeof(zend_mir_source_map_ref),
			alignof(zend_mir_source_map_ref), UINT32_MAX)) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, source_maps, zend_mir_source_map_ref);
	record = *source_map;
	record.id = module->source_maps.count;
	records[module->source_maps.count++] = record;
	*out = record.id;
	return true;
}

static bool zend_mir_seal_function(void *context,
		zend_mir_function_id function_id)
{
	zend_mir_module *module = context;
	zend_mir_core_function *function;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	function = zend_mir_find_function(module, function_id);
	if (function == NULL || function->sealed
			|| function->record.entry_block_id == ZEND_MIR_ID_INVALID) {
		return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"cannot seal function without a valid entry block");
	}
	function->sealed = true;
	return true;
}

static bool zend_mir_core_reserve_table(zend_mir_module *module,
	zend_mir_core_table *table, uint32_t required, size_t item_size,
	size_t alignment, uint32_t limit)
{
	uint32_t capacity;
	void *items;

	if (!zend_mir_module_require_building(module) || table == NULL
			|| required > limit || required < table->count) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"call-model table capacity overflow");
	}
	if (required <= table->capacity) {
		return true;
	}
	capacity = table->capacity == 0 ? UINT32_C(4) : table->capacity;
	while (capacity < required) {
		if (capacity > limit / 2) {
			capacity = limit;
		} else {
			capacity *= 2;
		}
		if (capacity == 0) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				"call-model table capacity overflow");
		}
	}
	if ((size_t) capacity > SIZE_MAX / item_size) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"call-model table byte size overflow");
	}
	items = zend_mir_arena_allocate(
		&module->arena, (size_t) capacity * item_size, alignment);
	if (items == NULL) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"call-model table allocation failed");
	}
	if (table->count != 0) {
		memcpy(items, table->items, (size_t) table->count * item_size);
	}
	table->items = items;
	table->capacity = capacity;
	return true;
}

static bool zend_mir_core_grow_staging(void **items, uint32_t count,
	uint32_t *capacity, size_t item_size)
{
	uint32_t new_capacity;
	void *new_items;

	if (items == NULL || capacity == NULL || count == UINT32_MAX) {
		return false;
	}
	if (count < *capacity) {
		return true;
	}
	new_capacity = *capacity == 0 ? UINT32_C(4) : *capacity;
	while (new_capacity <= count) {
		if (new_capacity > UINT32_MAX / 2) {
			return false;
		}
		new_capacity *= 2;
	}
	if ((size_t) new_capacity > SIZE_MAX / item_size) {
		return false;
	}
	new_items = realloc(*items, (size_t) new_capacity * item_size);
	if (new_items == NULL) {
		return false;
	}
	*items = new_items;
	*capacity = new_capacity;
	return true;
}

static bool zend_mir_core_stage_call_target(
	void *context, const zend_mir_call_target_ref *target)
{
	zend_mir_module *module = context;
	zend_mir_core_call_staging *staging;

	if (!zend_mir_module_require_building(module) || target == NULL) {
		return false;
	}
	staging = &module->call_staging;
	if (staging->committed || target->id != staging->target_count
			|| (target->kind != ZEND_MIR_CALL_TARGET_DIRECT_USER
				&& target->kind != ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
				&& target->kind != ZEND_MIR_CALL_TARGET_METHOD_USER
				&& target->kind != ZEND_MIR_CALL_TARGET_DYNAMIC)
			|| !zend_mir_id_is_valid(target->function_symbol_id)
			|| ((target->kind == ZEND_MIR_CALL_TARGET_DIRECT_USER
					|| target->kind == ZEND_MIR_CALL_TARGET_METHOD_USER)
				&& !zend_mir_id_is_valid(target->op_array_id))
			|| !zend_mir_core_grow_staging(
				(void **) &staging->targets, staging->target_count,
				&staging->target_capacity, sizeof(*staging->targets))) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"invalid or unallocatable call target");
	}
	staging->targets[staging->target_count++] = *target;
	return true;
}

static bool zend_mir_core_stage_call_argument(
	void *context, const zend_mir_call_argument_ref *argument)
{
	zend_mir_module *module = context;
	zend_mir_core_call_staging *staging;
	bool machine_value;

	if (!zend_mir_module_require_building(module) || argument == NULL) {
		return false;
	}
	staging = &module->call_staging;
	machine_value =
		argument->ownership == ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
		|| (argument->ownership
				== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE
			&& zend_mir_id_is_valid(argument->value_id));
	if (staging->committed || argument->id != staging->argument_count
			|| argument->ownership < ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
			|| argument->ownership
				> ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
			|| (machine_value
				? !zend_mir_module_find_value(
					module, argument->value_id, NULL)
				: (zend_mir_id_is_valid(argument->value_id)
					|| argument->source_mode < ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					|| argument->source_mode
						> ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
					|| (argument->source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
						? argument->source_operand.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						: (argument->source_operand.kind
								< ZEND_MIR_SOURCE_OPERAND_LITERAL
							|| argument->source_operand.kind
								> ZEND_MIR_SOURCE_OPERAND_SSA))))
			|| !zend_mir_core_grow_staging(
				(void **) &staging->arguments, staging->argument_count,
				&staging->argument_capacity, sizeof(*staging->arguments))) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"invalid or unallocatable call argument");
	}
	staging->arguments[staging->argument_count++] = *argument;
	return true;
}

static bool zend_mir_core_stage_call_continuation(
	void *context, const zend_mir_call_continuation_ref *continuation)
{
	zend_mir_module *module = context;
	zend_mir_core_call_staging *staging;

	if (!zend_mir_module_require_building(module) || continuation == NULL) {
		return false;
	}
	staging = &module->call_staging;
	if (staging->committed
			|| continuation->id != staging->continuation_count
			|| continuation->kind < 0
			|| continuation->kind
				> ZEND_MIR_CALL_CONTINUATION_OBSERVER
			|| !zend_mir_core_grow_staging(
				(void **) &staging->continuations,
				staging->continuation_count,
				&staging->continuation_capacity,
				sizeof(*staging->continuations))) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"invalid or unallocatable call continuation");
	}
	staging->continuations[staging->continuation_count++] = *continuation;
	return true;
}

static bool zend_mir_core_stage_call_site(
	void *context, const zend_mir_call_site_ref *site)
{
	zend_mir_module *module = context;
	zend_mir_core_call_staging *staging;

	if (!zend_mir_module_require_building(module) || site == NULL) {
		return false;
	}
	staging = &module->call_staging;
	if (staging->committed || site->id != staging->site_count
			|| site->target_id >= staging->target_count
			|| (zend_mir_id_is_valid(site->result_id)
				&& !zend_mir_module_find_value(
					module, site->result_id, NULL))
			|| site->arguments.offset > staging->argument_count
			|| site->arguments.count
				> staging->argument_count - site->arguments.offset
			|| site->continuations.offset > staging->continuation_count
			|| site->continuations.count
				> staging->continuation_count - site->continuations.offset
			|| !zend_mir_core_grow_staging(
				(void **) &staging->sites, staging->site_count,
				&staging->site_capacity, sizeof(*staging->sites))) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"invalid or unallocatable call site");
	}
	staging->sites[staging->site_count++] = *site;
	return true;
}

static bool zend_mir_core_call_site_block(
	const zend_mir_core_call_staging *staging, uint32_t site_index,
	zend_mir_block_id *block_out)
{
	const zend_mir_call_site_ref *site = &staging->sites[site_index];
	uint32_t index;

	for (index = 0; index < site->continuations.count; index++) {
		const zend_mir_call_continuation_ref *continuation =
			&staging->continuations[site->continuations.offset + index];
		if (continuation->kind == ZEND_MIR_CALL_CONTINUATION_NORMAL
				&& zend_mir_id_is_valid(continuation->block_id)) {
			*block_out = continuation->block_id;
			return true;
		}
	}
	return false;
}

static void zend_mir_core_init_frame_state(
	zend_mir_frame_state_ref *frame, zend_mir_frame_state_id id,
	zend_mir_function_id function_id,
	zend_mir_frame_state_id parent_id, uint32_t opline,
	zend_mir_span slots, zend_mir_safepoint_class safepoint)
{
	memset(frame, 0, sizeof(*frame));
	frame->id = id;
	frame->function_id = function_id;
	frame->parent_id = parent_id;
	frame->function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame->opline_index = opline;
	frame->opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame->slots = slots;
	frame->roots.offset = 0;
	frame->roots.count = 0;
	frame->cleanup_obligations.offset = 0;
	frame->cleanup_obligations.count = 0;
	frame->return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_NATIVE;
	frame->return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame->return_continuation.opline_index = opline + 1;
	frame->exception_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_ZEND_EXCEPTION;
	frame->exception_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame->exception_continuation.opline_index = opline;
	frame->bailout_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame->bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame->bailout_continuation.opline_index = opline;
	frame->suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame->suspend_state_id = ZEND_MIR_ID_INVALID;
	frame->code_version_id = 1;
	frame->resume.allowed = false;
	frame->resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame->resume.resume_id = ZEND_MIR_ID_INVALID;
	frame->resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame->resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame->safepoint_class = safepoint;
	frame->canonical = true;
}

/* Fills the call instruction of a site; its id is assigned on composition. */
static bool zend_mir_core_build_call_instruction(
	zend_mir_module *module, zend_mir_core_instruction *instruction,
	uint32_t site_index, zend_mir_block_id block_id)
{
	zend_mir_core_call_staging *staging = &module->call_staging;
	zend_mir_call_site_ref *site = &staging->sites[site_index];
	zend_mir_core_value *values;
	zend_mir_value_id *operands = NULL;
	uint32_t result_index;
	uint32_t index;
	bool source_arguments =
		staging->targets[site->target_id].kind
			== ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL;

	for (index = 0;
			!source_arguments && index < site->arguments.count; index++) {
		source_arguments = staging->arguments[
			site->arguments.offset + index].ownership
				!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR;
	}
	if (!source_arguments
			&& site->arguments.count != 0) {
		operands = zend_mir_arena_allocate(
			&module->arena,
			(size_t) site->arguments.count * sizeof(*operands),
			alignof(zend_mir_value_id));
		if (operands == NULL) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
				"call operand allocation failed");
		}
		for (index = 0; index < site->arguments.count; index++) {
			operands[index] = staging->arguments[
				site->arguments.offset + index].value_id;
		}
	}
	memset(instruction, 0, sizeof(*instruction));
	instruction->record.id = ZEND_MIR_ID_INVALID;
	instruction->record.block_id = block_id;
	instruction->record.opcode = staging->targets[site->target_id].kind
		== ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
		? ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
		: ZEND_MIR_OPCODE_CALL_DIRECT_USER;
	if (zend_mir_id_is_valid(site->result_id)) {
		if (!zend_mir_module_find_value(
				module, site->result_id, &result_index)) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID,
				"call result value is absent");
		}
		values = ZEND_MIR_CORE_ITEMS(
			module, values, zend_mir_core_value);
		instruction->record.representation =
			values[result_index].record.representation;
		instruction->record.result_id = site->result_id;
	} else {
		instruction->record.representation = ZEND_MIR_REPRESENTATION_VOID;
		instruction->record.result_id = ZEND_MIR_ID_INVALID;
	}
	instruction->record.frame_state_id =
		site->caller_frame.frame_state_id;
	/* During staging, instruction_id carries the source DO opline. */
	instruction->record.source_position_id = site->instruction_id;
	instruction->record.effects = site->effects;
	instruction->record.reads = site->reads;
	instruction->record.writes = site->writes;
	instruction->record.barriers = site->barriers;
	instruction->record.ownership_actions = source_arguments
		? ZEND_MIR_OWNERSHIP_ACTION_MASK(
			ZEND_MIR_OWNERSHIP_ACTION_COPY_ADDREF)
		: ZEND_MIR_OWNERSHIP_ACTION_MASK(
			ZEND_MIR_OWNERSHIP_ACTION_BORROW);
	instruction->operands = operands;
	instruction->operand_count = operands != NULL ? site->arguments.count : 0;
	instruction->operand_capacity = instruction->operand_count;
	return true;
}

typedef struct _zend_mir_core_ordered_call {
	zend_mir_block_id block_id;
	zend_mir_source_position_id source_position;
	uint32_t site_index;
} zend_mir_core_ordered_call;

static int zend_mir_core_compare_ordered_calls(const void *left_ptr,
	const void *right_ptr)
{
	const zend_mir_core_ordered_call *left = left_ptr;
	const zend_mir_core_ordered_call *right = right_ptr;

	if (left->block_id != right->block_id) {
		return left->block_id < right->block_id ? -1 : 1;
	}
	if (left->source_position != right->source_position) {
		return left->source_position < right->source_position ? -1 : 1;
	}
	if (left->site_index != right->site_index) {
		return left->site_index < right->site_index ? -1 : 1;
	}
	return 0;
}

/*
 * Places a call instruction before the first instruction of its block whose
 * source position follows the call's DO opline. Calls of one block stay in
 * (source position, site) order; zend_mir_module_compose_instructions places
 * them, together with the executable value operations, in one pass.
 */
static bool zend_mir_core_stage_call_instructions(zend_mir_module *module)
{
	zend_mir_core_call_staging *staging = &module->call_staging;
	zend_mir_core_pending_calls *pending = &module->pending_calls;
	zend_mir_core_ordered_call *ordered;
	uint32_t index;

	ordered = malloc((size_t) staging->site_count * sizeof(*ordered));
	pending->instructions = zend_mir_arena_allocate(&module->arena,
		(size_t) staging->site_count * sizeof(*pending->instructions),
		alignof(zend_mir_core_instruction));
	pending->sites = zend_mir_arena_allocate(&module->arena,
		(size_t) staging->site_count * sizeof(*pending->sites),
		alignof(uint32_t));
	if (ordered == NULL || pending->instructions == NULL
			|| pending->sites == NULL) {
		free(ordered);
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"call instruction publication failed");
	}
	for (index = 0; index < staging->site_count; index++) {
		zend_mir_block_id block_id;
		/* zend_mir_core_commit_call_model() validated every site's block. */
		(void) zend_mir_core_call_site_block(staging, index, &block_id);
		ordered[index].block_id = block_id;
		ordered[index].source_position = staging->sites[index].instruction_id;
		ordered[index].site_index = index;
	}
	qsort(ordered, staging->site_count, sizeof(*ordered),
		zend_mir_core_compare_ordered_calls);
	for (index = 0; index < staging->site_count; index++) {
		if (!zend_mir_core_build_call_instruction(module,
				&pending->instructions[index], ordered[index].site_index,
				ordered[index].block_id)) {
			free(ordered);
			return false;
		}
		pending->sites[index] = ordered[index].site_index;
	}
	free(ordered);
	pending->count = staging->site_count;
	pending->pending = true;
	return true;
}

/* Emits the value operations of a block up to a source position. */
static bool zend_mir_core_flush_value_operations(
	zend_mir_module *module, zend_mir_core_instruction *instructions,
	uint32_t *next_index, zend_mir_executable_value_ref *operations,
	uint32_t *block_operation_head, const uint32_t *next_block_operation,
	zend_mir_block_id block_id, zend_mir_source_position_id source_position,
	uint32_t *emitted_count, zend_mir_core_value_emitter emit)
{
	uint32_t operation_index = block_operation_head[block_id];

	while (operation_index != ZEND_MIR_ID_INVALID) {
		zend_mir_executable_value_ref *operation =
			&operations[operation_index];

		if (operation->source_position_id > source_position) {
			break;
		}
		if (!emit(module, &instructions[*next_index], operation,
				*next_index)) {
			return false;
		}
		operation->id = *next_index;
		(*emitted_count)++;
		(*next_index)++;
		operation_index = next_block_operation[operation_index];
	}
	block_operation_head[block_id] = operation_index;
	return true;
}

bool zend_mir_module_compose_instructions(zend_mir_module *module,
	zend_mir_executable_value_ref *operations, uint32_t operation_count,
	zend_mir_core_value_emitter emit)
{
	zend_mir_core_pending_calls *pending = &module->pending_calls;
	const uint32_t call_count = pending->pending ? pending->count : 0;
	const uint32_t old_count = module->instructions.count;
	const uint32_t block_count = module->blocks.count;
	zend_mir_core_instruction *old_instructions;
	zend_mir_core_instruction *new_instructions;
	zend_mir_call_site_ref *call_sites;
	uint32_t *attached_operation_by_instruction = NULL;
	uint32_t *block_operation_head = NULL;
	uint32_t *block_operation_tail = NULL;
	uint32_t *next_block_operation = NULL;
	uint8_t *block_has_terminator = NULL;
	uint32_t *call_begin = NULL;
	uint32_t *call_end = NULL;
	bool *inserted = NULL;
	size_t instruction_bytes;
	uint32_t materialized_count = 0;
	uint32_t emitted_count = 0;
	uint32_t new_index = 0;
	uint32_t total_count;
	uint32_t index;
	bool success = false;

	if (call_count == 0 && operation_count == 0) {
		pending->pending = false;
		return true;
	}
	for (index = 0; index < operation_count; index++) {
		if (!zend_mir_id_is_valid(operations[index].id)) {
			materialized_count++;
		}
	}
	if (old_count > UINT32_MAX - call_count
			|| (operation_count != 0
				&& (old_count + call_count > module->limits.instructions
					|| materialized_count > module->limits.instructions
						- old_count - call_count))) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"executable value instruction count overflow");
	}
	total_count = old_count + call_count + materialized_count;
	if (!zend_mir_checked_multiply_size(
			total_count, sizeof(*new_instructions), &instruction_bytes)) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"executable value composition size overflow");
	}
	new_instructions = zend_mir_arena_allocate(
		&module->arena, instruction_bytes, alignof(zend_mir_core_instruction));
	if (operation_count != 0) {
		attached_operation_by_instruction = malloc(
			(old_count == 0 ? 1 : (size_t) old_count)
				* sizeof(*attached_operation_by_instruction));
		block_operation_head = malloc(
			(block_count == 0 ? 1 : (size_t) block_count)
				* sizeof(*block_operation_head));
		block_operation_tail = malloc(
			(block_count == 0 ? 1 : (size_t) block_count)
				* sizeof(*block_operation_tail));
		next_block_operation = malloc(
			(size_t) operation_count * sizeof(*next_block_operation));
	}
	block_has_terminator = calloc(block_count == 0 ? 1 : block_count,
		sizeof(*block_has_terminator));
	if (call_count != 0) {
		call_begin = malloc(
			(block_count == 0 ? 1 : (size_t) block_count) * sizeof(*call_begin));
		call_end = malloc(
			(block_count == 0 ? 1 : (size_t) block_count) * sizeof(*call_end));
		/* Indexed by call site. */
		inserted = calloc(call_count, sizeof(*inserted));
	}
	if ((total_count != 0 && new_instructions == NULL)
			|| (operation_count != 0
				&& (attached_operation_by_instruction == NULL
					|| block_operation_head == NULL
					|| block_operation_tail == NULL
					|| next_block_operation == NULL))
			|| block_has_terminator == NULL
			|| (call_count != 0
				&& (call_begin == NULL || call_end == NULL
					|| inserted == NULL))) {
		zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"instruction composition allocation failed");
		goto done;
	}
	if (operation_count != 0) {
		memset(attached_operation_by_instruction, 0xff,
			(size_t) old_count * sizeof(*attached_operation_by_instruction));
		memset(block_operation_head, 0xff,
			(size_t) block_count * sizeof(*block_operation_head));
		memset(block_operation_tail, 0xff,
			(size_t) block_count * sizeof(*block_operation_tail));
		memset(next_block_operation, 0xff,
			(size_t) operation_count * sizeof(*next_block_operation));
	}
	for (index = 0; index < operation_count; index++) {
		const zend_mir_executable_value_ref *operation = &operations[index];

		if (operation->block_id >= block_count) {
			zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				"executable value operation references an unknown block");
			goto done;
		}
		if (zend_mir_id_is_valid(operation->id)) {
			if (operation->id >= old_count
					|| attached_operation_by_instruction[operation->id]
						!= ZEND_MIR_ID_INVALID) {
				zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
					"executable value operation references an invalid instruction");
				goto done;
			}
			attached_operation_by_instruction[operation->id] = index;
			continue;
		}
		if (block_operation_tail[operation->block_id] == ZEND_MIR_ID_INVALID) {
			block_operation_head[operation->block_id] = index;
		} else {
			next_block_operation[block_operation_tail[operation->block_id]] =
				index;
		}
		block_operation_tail[operation->block_id] = index;
	}
	if (call_count != 0) {
		/* The pending calls are grouped by block in block order. */
		memset(call_begin, 0xff, (size_t) block_count * sizeof(*call_begin));
		for (index = 0; index < call_count; index++) {
			const zend_mir_block_id block_id =
				pending->instructions[index].record.block_id;

			if (call_begin[block_id] == ZEND_MIR_ID_INVALID) {
				call_begin[block_id] = index;
			}
			call_end[block_id] = index + 1;
		}
	}
	call_sites = ZEND_MIR_CORE_ITEMS(module, call_sites, zend_mir_call_site_ref);
	old_instructions = ZEND_MIR_CORE_ITEMS(
		module, instructions, zend_mir_core_instruction);

	/*
	 * One pass places both streams where splicing them one after the other
	 * would. A call goes before the first instruction of its block that is a
	 * terminator or has a later source position; calls a block never reaches
	 * that way follow the table in the order of their first site. A value
	 * operation goes before the first instruction of its block other than a
	 * PHI or FINALLY_ENTER, calls included, that is a terminator or has a
	 * later source position; the rest follow every instruction. CFG lowering
	 * emits every block's PHIs before the block bodies, so the table is not
	 * block-sorted and each block keeps its own cursors.
	 */
#define ZEND_MIR_PLACE_CALL(position) \
	do { \
		zend_mir_core_instruction *call = &pending->instructions[position]; \
		const uint32_t site = pending->sites[position]; \
		if (operation_count != 0 \
				&& !zend_mir_core_flush_value_operations(module, \
					new_instructions, &new_index, operations, \
					block_operation_head, next_block_operation, \
					call->record.block_id, call->record.source_position_id, \
					&emitted_count, emit)) { \
			goto done; \
		} \
		new_instructions[new_index] = *call; \
		new_instructions[new_index].record.id = new_index; \
		module->call_staging.sites[site].instruction_id = new_index; \
		call_sites[site].instruction_id = new_index; \
		inserted[site] = true; \
		new_index++; \
	} while (0)

	for (index = 0; index < old_count; index++) {
		const zend_mir_instruction_record *old = &old_instructions[index].record;
		const bool terminator = zend_mir_opcode_is_terminator(old->opcode);
		uint32_t attached_operation;

		if (call_count != 0
				&& (terminator || zend_mir_id_is_valid(old->source_position_id))
				&& call_begin[old->block_id] != ZEND_MIR_ID_INVALID) {
			while (call_begin[old->block_id] < call_end[old->block_id]) {
				const uint32_t position = call_begin[old->block_id];

				if (!terminator
						&& pending->instructions[position]
							.record.source_position_id
							> old->source_position_id) {
					break;
				}
				ZEND_MIR_PLACE_CALL(position);
				call_begin[old->block_id]++;
			}
		}
		if (operation_count != 0
				&& old->opcode != ZEND_MIR_OPCODE_PHI
				&& old->opcode != ZEND_MIR_OPCODE_FINALLY_ENTER
				&& (zend_mir_id_is_valid(old->source_position_id)
					|| terminator)
				&& !zend_mir_core_flush_value_operations(module,
					new_instructions, &new_index, operations,
					block_operation_head, next_block_operation,
					old->block_id, old->source_position_id,
					&emitted_count, emit)) {
			goto done;
		}
		if (terminator) {
			block_has_terminator[old->block_id] = 1;
		}
		new_instructions[new_index] = old_instructions[index];
		new_instructions[new_index].record.id = new_index;
		if (operation_count != 0) {
			attached_operation = attached_operation_by_instruction[index];
			if (attached_operation != ZEND_MIR_ID_INVALID) {
				operations[attached_operation].id = new_index;
				emitted_count++;
			}
		}
		new_index++;
	}
	for (index = 0; index < call_count; index++) {
		zend_mir_block_id block_id;

		if (inserted[index]) {
			continue;
		}
		/* Validated when the call model was committed. */
		(void) zend_mir_core_call_site_block(
			&module->call_staging, index, &block_id);
		while (call_begin[block_id] < call_end[block_id]) {
			ZEND_MIR_PLACE_CALL(call_begin[block_id]);
			call_begin[block_id]++;
		}
	}
#undef ZEND_MIR_PLACE_CALL
	/*
	 * A block of a suspendable function may end at a source-level suspension
	 * without a MIR terminator; its remaining operations (GENERATOR_CREATE,
	 * YIELD, RETURN) go after its instructions. After a terminator they would
	 * be malformed.
	 */
	for (index = 0; index < operation_count; index++) {
		zend_mir_executable_value_ref *operation = &operations[index];

		if (zend_mir_id_is_valid(operation->id)) {
			continue;
		}
		if (block_has_terminator[operation->block_id]
				|| !emit(module, &new_instructions[new_index],
					operation, new_index)) {
			zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
				"executable value operation follows an owning block terminator");
			goto done;
		}
		operation->id = new_index;
		emitted_count++;
		new_index++;
	}
	if (emitted_count != operation_count || new_index != total_count) {
		zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
			"executable value operation has no ordered owning block instruction");
		goto done;
	}
	module->instructions.items = new_instructions;
	module->instructions.count = total_count;
	module->instructions.capacity = total_count;
	pending->pending = false;
	success = true;

done:
	free(inserted);
	free(call_end);
	free(call_begin);
	free(block_has_terminator);
	free(next_block_operation);
	free(block_operation_tail);
	free(block_operation_head);
	free(attached_operation_by_instruction);
	return success;
}

static bool zend_mir_core_commit_call_model(zend_mir_module *module)
{
	zend_mir_core_call_staging *staging = &module->call_staging;
	zend_mir_frame_slot_ref *slots;
	zend_mir_frame_state_ref *frames;
	uint32_t total_slots = module->frame_slots.count;
	uint32_t total_call_operands = 0;
	uint32_t total_frames;
	uint32_t next_slot_id = 0;
	uint32_t site_index;
	uint32_t index;

	if (!zend_mir_module_require_building(module)
			|| staging->committed || staging->site_count == 0
			|| staging->target_count == 0
			|| module->frame_states.count
				> UINT32_MAX - staging->site_count
			|| module->instructions.count
				> UINT32_MAX - staging->site_count) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid call model");
	}
	for (site_index = 0; site_index < staging->site_count; site_index++) {
		const zend_mir_call_site_ref *site = &staging->sites[site_index];
		const zend_mir_call_continuation_ref *exception_continuation;
		zend_mir_block_id block_id;
		if (total_slots == UINT32_MAX
				|| site->arguments.count
					> (UINT32_MAX - total_slots - 1) / 2
				|| !zend_mir_core_call_site_block(
					staging, site_index, &block_id)
				|| zend_mir_find_block(module, block_id) == NULL) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID,
				"invalid call-site shape");
		}
		total_slots += site->arguments.count * 2 + 1;
		if (site->continuations.count != 4
				|| site->continuations.offset > staging->continuation_count
				|| site->continuations.count
					> staging->continuation_count - site->continuations.offset) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID,
				"invalid call continuation span");
		}
		exception_continuation =
			&staging->continuations[site->continuations.offset + 1];
		if (zend_mir_id_is_valid(exception_continuation->block_id)) {
			if (exception_continuation->kind
					!= ZEND_MIR_CALL_CONTINUATION_EXCEPTION
					|| !zend_mir_id_is_valid(
						exception_continuation->source_opline_index)
					|| zend_mir_find_block(
						module, exception_continuation->block_id) == NULL) {
				return zend_mir_module_fail(module,
					ZEND_MIR_DIAGNOSTIC_INVALID_ID,
					"invalid catch continuation");
			}
		}
		if (staging->targets[site->target_id].kind
				!= ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			bool scalar_arguments = true;
			for (index = 0; index < site->arguments.count; index++) {
				if (staging->arguments[
						site->arguments.offset + index].ownership
						!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR) {
					scalar_arguments = false;
					break;
				}
			}
			if (!scalar_arguments) {
				continue;
			}
			if (site->arguments.count
					> UINT32_MAX - total_call_operands) {
				return zend_mir_module_fail(module,
					ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
					"call operand count overflow");
			}
			total_call_operands += site->arguments.count;
		}
	}
	total_frames = module->frame_states.count + staging->site_count;
	if (module->operand_count > module->limits.operands
			|| total_call_operands
				> module->limits.operands - module->operand_count
			|| !zend_mir_core_reserve_table(
				module, &module->frame_slots, total_slots,
				sizeof(zend_mir_frame_slot_ref),
				alignof(zend_mir_frame_slot_ref), UINT32_MAX)
			|| !zend_mir_core_reserve_table(
				module, &module->frame_states, total_frames,
				sizeof(zend_mir_frame_state_ref),
				alignof(zend_mir_frame_state_ref), UINT32_MAX)
			|| !zend_mir_core_reserve_table(
				module, &module->call_targets, staging->target_count,
				sizeof(zend_mir_call_target_ref),
				alignof(zend_mir_call_target_ref), UINT32_MAX)
			|| !zend_mir_core_reserve_table(
				module, &module->call_arguments, staging->argument_count,
				sizeof(zend_mir_call_argument_ref),
				alignof(zend_mir_call_argument_ref), UINT32_MAX)
			|| !zend_mir_core_reserve_table(
				module, &module->call_continuations,
				staging->continuation_count,
				sizeof(zend_mir_call_continuation_ref),
				alignof(zend_mir_call_continuation_ref), UINT32_MAX)
			|| !zend_mir_core_reserve_table(
				module, &module->call_sites, staging->site_count,
				sizeof(zend_mir_call_site_ref),
				alignof(zend_mir_call_site_ref), UINT32_MAX)) {
		return false;
	}
	slots = ZEND_MIR_CORE_ITEMS(
		module, frame_slots, zend_mir_frame_slot_ref);
	frames = ZEND_MIR_CORE_ITEMS(
		module, frame_states, zend_mir_frame_state_ref);
	for (index = 0; index < module->frame_slots.count; index++) {
		if (slots[index].slot_id >= next_slot_id) {
			if (slots[index].slot_id == ZEND_MIR_ID_MAX) {
				return zend_mir_module_fail(module,
					ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
					"call frame slot ID overflow");
			}
			next_slot_id = slots[index].slot_id + 1;
		}
	}
	if (total_slots - module->frame_slots.count
			> (ZEND_MIR_ID_MAX - next_slot_id) + 1) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"call frame slot ID overflow");
	}
	for (site_index = 0; site_index < staging->site_count; site_index++) {
		zend_mir_call_site_ref *site = &staging->sites[site_index];
		const bool internal_call = staging->targets[site->target_id].kind
			== ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL;
		uint32_t caller_offset = module->frame_slots.count;
		uint32_t argument_index;
		uint32_t pending_slot_id;
		zend_mir_span caller_span;
		zend_mir_span callee_span;
		zend_mir_frame_state_id caller_frame_id =
			module->frame_states.count;
		uint32_t opline = site->instruction_id;

		for (argument_index = 0;
			argument_index < site->arguments.count; argument_index++) {
			const zend_mir_call_argument_ref *argument =
				&staging->arguments[
					site->arguments.offset + argument_index];
			const bool source_argument = internal_call
				|| argument->ownership
					!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR;
			zend_mir_frame_slot_ref *slot =
				&slots[module->frame_slots.count++];
			memset(slot, 0, sizeof(*slot));
			slot->slot_id = next_slot_id++;
			slot->value_id = argument->value_id;
			slot->index = argument_index;
			slot->kind = ZEND_MIR_FRAME_SLOT_KIND_ARGUMENT;
			slot->representation =
				ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
			slot->materialization = source_argument
				? ZEND_MIR_MATERIALIZATION_SOURCE_ZVAL
				: ZEND_MIR_MATERIALIZATION_MATERIALIZED;
			slot->ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_CALLER_OWNED;
		}
		pending_slot_id = next_slot_id++;
		memset(&slots[module->frame_slots.count], 0,
			sizeof(slots[module->frame_slots.count]));
		slots[module->frame_slots.count].slot_id = pending_slot_id;
		slots[module->frame_slots.count].value_id = ZEND_MIR_ID_INVALID;
		slots[module->frame_slots.count].index = 0;
		slots[module->frame_slots.count].kind =
			ZEND_MIR_FRAME_SLOT_KIND_PENDING_CALL;
		slots[module->frame_slots.count].representation =
			ZEND_MIR_FRAME_SLOT_REPRESENTATION_EXECUTE_DATA_FIELD;
		slots[module->frame_slots.count].materialization =
			ZEND_MIR_MATERIALIZATION_UNDEF;
		slots[module->frame_slots.count].ownership =
			ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		module->frame_slots.count++;
		caller_span.offset = caller_offset;
		caller_span.count = site->arguments.count + 1;
		callee_span.offset = module->frame_slots.count;
		callee_span.count = site->arguments.count;
		for (argument_index = 0;
			argument_index < site->arguments.count; argument_index++) {
			const zend_mir_call_argument_ref *argument =
				&staging->arguments[
					site->arguments.offset + argument_index];
			const bool source_argument = internal_call
				|| argument->ownership
					!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR;
			zend_mir_frame_slot_ref *slot =
				&slots[module->frame_slots.count++];
			memset(slot, 0, sizeof(*slot));
			slot->slot_id = next_slot_id++;
			slot->value_id = argument->value_id;
			slot->index = argument_index;
			slot->kind = ZEND_MIR_FRAME_SLOT_KIND_ARGUMENT;
			slot->representation =
				ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
			slot->materialization = source_argument
				? ZEND_MIR_MATERIALIZATION_SOURCE_ZVAL
				: ZEND_MIR_MATERIALIZATION_MATERIALIZED;
			slot->ownership = source_argument
				? ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED
				: ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED;
		}
		zend_mir_core_init_frame_state(
			&frames[module->frame_states.count++], caller_frame_id,
			site->caller_frame.function_id,
			ZEND_MIR_ID_INVALID, opline, caller_span,
			internal_call ? ZEND_MIR_SAFEPOINT_CLASS_INTERNAL_CALL
				: ZEND_MIR_SAFEPOINT_CLASS_USER_CALL);
		site->caller_frame.frame_state_id = caller_frame_id;
		site->caller_frame.slots = caller_span;
		site->caller_frame.pending_call_slot_id = pending_slot_id;
		site->callee_entry_frame.frame_state_id = ZEND_MIR_ID_INVALID;
		site->callee_entry_frame.function_id = ZEND_MIR_ID_INVALID;
		site->callee_entry_frame.slots = callee_span;
		site->callee_entry_frame.pending_call_slot_id =
			ZEND_MIR_ID_INVALID;
	}
	/* The call instructions are placed when the instruction table is
	 * composed: with the value operations, or at finalization. */
	if (!zend_mir_core_stage_call_instructions(module)) {
		return false;
	}
	module->operand_count += total_call_operands;
	if (staging->target_count != 0) {
		memcpy(module->call_targets.items, staging->targets,
			(size_t) staging->target_count * sizeof(*staging->targets));
	}
	if (staging->argument_count != 0) {
		memcpy(module->call_arguments.items, staging->arguments,
			(size_t) staging->argument_count * sizeof(*staging->arguments));
	}
	if (staging->continuation_count != 0) {
		memcpy(module->call_continuations.items, staging->continuations,
			(size_t) staging->continuation_count
				* sizeof(*staging->continuations));
	}
	if (staging->site_count != 0) {
		memcpy(module->call_sites.items, staging->sites,
			(size_t) staging->site_count * sizeof(*staging->sites));
	}
	module->call_targets.count = staging->target_count;
	module->call_arguments.count = staging->argument_count;
	module->call_continuations.count = staging->continuation_count;
	module->call_sites.count = staging->site_count;
	staging->committed = true;
	return true;
}

static bool zend_mir_core_commit_staged_calls(void *context)
{
	return zend_mir_core_commit_call_model(context);
}

bool zend_mir_module_commit_empty_call_model(zend_mir_module *module)
{
	zend_mir_core_call_staging *staging;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	staging = &module->call_staging;
	if (staging->committed || staging->target_count != 0
			|| staging->argument_count != 0 || staging->continuation_count != 0
			|| staging->site_count != 0 || module->call_targets.count != 0
			|| module->call_arguments.count != 0
			|| module->call_continuations.count != 0
			|| module->call_sites.count != 0) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_INVALID_ID,
			"invalid empty call model");
	}
	staging->committed = true;
	return true;
}

void zend_mir_module_init_call_mutator(zend_mir_module *module)
{
	memset(&module->call_mutator, 0, sizeof(module->call_mutator));
	module->call_mutator.contract_version = ZEND_MIR_CONTRACT_VERSION;
	module->call_mutator.context = module;
	module->call_mutator.add_call_target =
		zend_mir_core_stage_call_target;
	module->call_mutator.add_call_argument =
		zend_mir_core_stage_call_argument;
	module->call_mutator.add_call_continuation =
		zend_mir_core_stage_call_continuation;
	module->call_mutator.add_call_site = zend_mir_core_stage_call_site;
	module->call_mutator.commit_call_model =
		zend_mir_core_commit_staged_calls;
}

void zend_mir_module_init_mutator(zend_mir_module *module)
{
	module->mutator.contract_version = ZEND_MIR_CONTRACT_VERSION;
	module->mutator.context = module;
	module->mutator.diagnostics = module->diagnostics;
	module->mutator.add_function = zend_mir_add_function;
	module->mutator.add_block = zend_mir_add_block;
	module->mutator.set_entry_block = zend_mir_set_entry_block;
	module->mutator.add_value = zend_mir_add_value;
	module->mutator.add_constant = zend_mir_add_constant;
	module->mutator.add_instruction = zend_mir_add_instruction;
	module->mutator.add_operand = zend_mir_add_operand;
	module->mutator.add_edge = zend_mir_add_edge;
	module->mutator.add_source_position = zend_mir_add_source_position;
	module->mutator.add_frame_slot = zend_mir_add_frame_slot;
	module->mutator.add_root = zend_mir_add_root;
	module->mutator.add_cleanup = zend_mir_add_cleanup;
	module->mutator.add_frame_state = zend_mir_add_frame_state;
	module->mutator.seal_function = zend_mir_seal_function;
	module->mutator.add_source_map = zend_mir_add_source_map;
	module->mutator.add_value_fact = zend_mir_core_add_value_fact;
}

bool zend_mir_module_finalize(zend_mir_module *module)
{
	zend_mir_core_function *functions;
	uint32_t index;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	functions = ZEND_MIR_CORE_ITEMS(module, functions, zend_mir_core_function);
	for (index = 0; index < module->functions.count; index++) {
		if (!functions[index].sealed) {
			return zend_mir_module_fail(module, ZEND_MIR_DIAGNOSTIC_INVALID_ID,
				"all functions must be sealed before finalization");
		}
	}
	if ((module->pending_calls.pending
				&& !zend_mir_module_compose_instructions(
					module, NULL, 0, NULL))
			|| !zend_mir_module_prepare_edge_index(module)) {
		return false;
	}
	module->state = ZEND_MIR_MODULE_FINALIZED;
	return true;
}

void zend_mir_module_destroy(zend_mir_module *module)
{
	if (module != NULL) {
		free(module->call_staging.targets);
		free(module->call_staging.arguments);
		free(module->call_staging.continuations);
		free(module->call_staging.sites);
		zend_mir_arena_release(&module->arena);
	}
}

zend_mir_module_state zend_mir_module_get_state(const zend_mir_module *module)
{
	return module != NULL ? module->state : ZEND_MIR_MODULE_FAILED;
}

const zend_mir_view *zend_mir_module_get_view(const zend_mir_module *module)
{
	return module != NULL && module->state != ZEND_MIR_MODULE_FAILED
		? &module->view : NULL;
}

zend_mir_mutator *zend_mir_module_get_mutator(zend_mir_module *module)
{
	return zend_mir_module_require_building(module) ? &module->mutator : NULL;
}

zend_mir_call_mutator *zend_mir_module_get_call_mutator(
	zend_mir_module *module)
{
	return zend_mir_module_require_building(module)
		&& !module->call_staging.committed
		? &module->call_mutator : NULL;
}

const zend_mir_call_view *zend_mir_module_get_call_view(
	const zend_mir_module *module)
{
	return module != NULL && module->state != ZEND_MIR_MODULE_FAILED
		&& module->call_staging.committed
		? &module->call_view : NULL;
}
