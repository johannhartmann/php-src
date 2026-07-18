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

static bool zend_mir_view_is_available(const zend_mir_module *module)
{
	return module != NULL && module->state != ZEND_MIR_MODULE_FAILED;
}

static zend_mir_module_id zend_mir_view_module_id(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->id : ZEND_MIR_ID_INVALID;
}

static uint32_t zend_mir_view_function_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->functions.count : 0;
}

static bool zend_mir_view_function_at(const void *context, uint32_t index,
		zend_mir_function_record *out)
{
	const zend_mir_module *module = context;
	zend_mir_core_function *functions;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->functions.count) {
		return false;
	}
	functions = ZEND_MIR_CORE_ITEMS(module, functions, zend_mir_core_function);
	*out = functions[index].record;
	return true;
}

static uint32_t zend_mir_view_block_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->blocks.count : 0;
}

static bool zend_mir_view_block_at(const void *context, uint32_t index,
		zend_mir_block_record *out)
{
	const zend_mir_module *module = context;
	zend_mir_block_record *blocks;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->blocks.count) {
		return false;
	}
	blocks = ZEND_MIR_CORE_ITEMS(module, blocks, zend_mir_block_record);
	*out = blocks[index];
	return true;
}

static uint32_t zend_mir_view_instruction_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->instructions.count : 0;
}

static bool zend_mir_view_instruction_at(const void *context, uint32_t index,
		zend_mir_instruction_record *out)
{
	const zend_mir_module *module = context;
	zend_mir_core_instruction *instructions;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->instructions.count) {
		return false;
	}
	instructions = ZEND_MIR_CORE_ITEMS(module, instructions, zend_mir_core_instruction);
	*out = instructions[index].record;
	return true;
}

static uint32_t zend_mir_view_value_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->values.count : 0;
}

static bool zend_mir_view_value_at(const void *context, uint32_t index,
		zend_mir_value_record *out)
{
	const zend_mir_module *module = context;
	zend_mir_core_value *values;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->values.count) {
		return false;
	}
	values = ZEND_MIR_CORE_ITEMS(module, values, zend_mir_core_value);
	*out = values[index].record;
	return true;
}

static uint32_t zend_mir_view_constant_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->constants.count : 0;
}

static bool zend_mir_view_constant_at(const void *context, uint32_t index,
		zend_mir_constant_record *out)
{
	const zend_mir_module *module = context;
	zend_mir_constant_record *constants;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->constants.count) {
		return false;
	}
	constants = ZEND_MIR_CORE_ITEMS(module, constants, zend_mir_constant_record);
	*out = constants[index];
	return true;
}

static uint32_t zend_mir_view_empty_count(const void *context)
{
	(void) context;
	return 0;
}

static bool zend_mir_view_frame_state_at(const void *context, uint32_t index,
		zend_mir_frame_state_ref *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

static bool zend_mir_view_source_position_at(const void *context, uint32_t index,
		zend_mir_source_position_ref *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

static bool zend_mir_view_frame_slot_at(const void *context, uint32_t index,
		zend_mir_frame_slot_ref *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

static bool zend_mir_view_root_at(const void *context, uint32_t index,
		uint32_t *slot_id_out)
{
	(void) context;
	(void) index;
	(void) slot_id_out;
	return false;
}

static bool zend_mir_view_cleanup_at(const void *context, uint32_t index,
		zend_mir_cleanup_ref *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

static uint32_t zend_mir_view_instruction_operand_count(const void *context,
		zend_mir_instruction_id instruction_id)
{
	const zend_mir_module *module = context;
	zend_mir_core_instruction *instructions;

	if (!zend_mir_view_is_available(module)
			|| instruction_id >= module->instructions.count) {
		return 0;
	}
	instructions = ZEND_MIR_CORE_ITEMS(module, instructions, zend_mir_core_instruction);
	return instructions[instruction_id].record.id == instruction_id
		? instructions[instruction_id].operand_count : 0;
}

static bool zend_mir_view_instruction_operand_at(const void *context,
		zend_mir_instruction_id instruction_id, uint32_t index,
		zend_mir_value_id *out)
{
	const zend_mir_module *module = context;
	zend_mir_core_instruction *instructions;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| instruction_id >= module->instructions.count) {
		return false;
	}
	instructions = ZEND_MIR_CORE_ITEMS(module, instructions, zend_mir_core_instruction);
	if (instructions[instruction_id].record.id != instruction_id
			|| index >= instructions[instruction_id].operand_count) {
		return false;
	}
	*out = instructions[instruction_id].operands[index];
	return true;
}

static uint32_t zend_mir_view_successor_count(const void *context,
		zend_mir_block_id block_id)
{
	(void) context;
	(void) block_id;
	return 0;
}

static bool zend_mir_view_successor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	(void) context;
	(void) block_id;
	(void) index;
	(void) out;
	return false;
}

static uint32_t zend_mir_view_predecessor_count(const void *context,
		zend_mir_block_id block_id)
{
	(void) context;
	(void) block_id;
	return 0;
}

static bool zend_mir_view_predecessor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	(void) context;
	(void) block_id;
	(void) index;
	(void) out;
	return false;
}

static bool zend_mir_view_source_map_at(const void *context, uint32_t index,
		zend_mir_source_map_ref *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

void zend_mir_module_init_view(zend_mir_module *module)
{
	module->view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	module->view.context = module;
	module->view.module_id = zend_mir_view_module_id;
	module->view.function_count = zend_mir_view_function_count;
	module->view.function_at = zend_mir_view_function_at;
	module->view.block_count = zend_mir_view_block_count;
	module->view.block_at = zend_mir_view_block_at;
	module->view.instruction_count = zend_mir_view_instruction_count;
	module->view.instruction_at = zend_mir_view_instruction_at;
	module->view.value_count = zend_mir_view_value_count;
	module->view.value_at = zend_mir_view_value_at;
	module->view.constant_count = zend_mir_view_constant_count;
	module->view.constant_at = zend_mir_view_constant_at;
	module->view.frame_state_count = zend_mir_view_empty_count;
	module->view.frame_state_at = zend_mir_view_frame_state_at;
	module->view.source_position_count = zend_mir_view_empty_count;
	module->view.source_position_at = zend_mir_view_source_position_at;
	module->view.frame_slot_count = zend_mir_view_empty_count;
	module->view.frame_slot_at = zend_mir_view_frame_slot_at;
	module->view.root_count = zend_mir_view_empty_count;
	module->view.root_at = zend_mir_view_root_at;
	module->view.cleanup_count = zend_mir_view_empty_count;
	module->view.cleanup_at = zend_mir_view_cleanup_at;
	module->view.instruction_operand_count = zend_mir_view_instruction_operand_count;
	module->view.instruction_operand_at = zend_mir_view_instruction_operand_at;
	module->view.successor_count = zend_mir_view_successor_count;
	module->view.successor_at = zend_mir_view_successor_at;
	module->view.predecessor_count = zend_mir_view_predecessor_count;
	module->view.predecessor_at = zend_mir_view_predecessor_at;
	module->view.source_map_count = zend_mir_view_empty_count;
	module->view.source_map_at = zend_mir_view_source_map_at;
}
