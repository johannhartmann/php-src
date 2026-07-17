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

static bool zend_mir_checked_multiply_size(size_t left, size_t right, size_t *out)
{
	if (out == NULL || (right != 0 && left > SIZE_MAX / right)) {
		return false;
	}
	*out = left * right;
	return true;
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
	return module;
}

bool zend_mir_module_grow_table(zend_mir_module *module, zend_mir_core_table *table,
		size_t item_size, size_t item_alignment, uint32_t limit)
{
	uint32_t required;
	uint32_t capacity;
	size_t bytes;
	void *items;

	if (!zend_mir_module_require_building(module) || table == NULL
			|| item_size == 0 || table->count >= limit
			|| !zend_mir_core_next_id(table->count, &required)) {
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
		if (capacity > UINT32_MAX / 2) {
			capacity = limit;
		} else {
			capacity *= 2;
			if (capacity > limit) {
				capacity = limit;
			}
		}
		if (capacity < required) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
				"core table capacity overflow");
		}
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
			|| representation >= ZEND_MIR_REPRESENTATION_COUNT
			|| ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT) {
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
			|| constant->representation >= ZEND_MIR_REPRESENTATION_COUNT
			|| constant->kind >= ZEND_MIR_CONSTANT_KIND_COUNT
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
	block = zend_mir_find_block(module, record->block_id);
	if (block == NULL || !zend_mir_function_is_open(module, block->function_id)
			|| record->opcode >= ZEND_MIR_OPCODE_COUNT
			|| record->representation >= ZEND_MIR_REPRESENTATION_COUNT
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

static bool zend_mir_unmodeled_mutation(void *context)
{
	return zend_mir_module_fail(context, ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS,
		"mutation belongs to another W02 ownership track");
}

static bool zend_mir_add_edge(void *context, zend_mir_block_id from,
		zend_mir_block_id to)
{
	(void) from;
	(void) to;
	return zend_mir_unmodeled_mutation(context);
}

static bool zend_mir_add_source_position(void *context,
		const zend_mir_source_position_ref *source_position,
		zend_mir_source_position_id *out)
{
	(void) source_position;
	(void) out;
	return zend_mir_unmodeled_mutation(context);
}

static bool zend_mir_add_frame_slot(void *context,
		const zend_mir_frame_slot_ref *slot, uint32_t *index_out)
{
	(void) slot;
	(void) index_out;
	return zend_mir_unmodeled_mutation(context);
}

static bool zend_mir_add_root(void *context, uint32_t slot_id, uint32_t *index_out)
{
	(void) slot_id;
	(void) index_out;
	return zend_mir_unmodeled_mutation(context);
}

static bool zend_mir_add_cleanup(void *context,
		const zend_mir_cleanup_ref *cleanup, uint32_t *index_out)
{
	(void) cleanup;
	(void) index_out;
	return zend_mir_unmodeled_mutation(context);
}

static bool zend_mir_add_frame_state(void *context,
		const zend_mir_frame_state_ref *frame_state, zend_mir_frame_state_id *out)
{
	(void) frame_state;
	(void) out;
	return zend_mir_unmodeled_mutation(context);
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
	module->state = ZEND_MIR_MODULE_FINALIZED;
	return true;
}

void zend_mir_module_destroy(zend_mir_module *module)
{
	if (module != NULL) {
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
