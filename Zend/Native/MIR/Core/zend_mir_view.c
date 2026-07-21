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

static uint32_t zend_mir_view_frame_state_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->frame_states.count : 0;
}

static bool zend_mir_view_frame_state_at(const void *context, uint32_t index,
		zend_mir_frame_state_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_frame_state_ref *records;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->frame_states.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, frame_states, zend_mir_frame_state_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_view_source_position_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module)
		? module->source_positions.count : 0;
}

static bool zend_mir_view_source_position_at(const void *context, uint32_t index,
		zend_mir_source_position_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_source_position_ref *records;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->source_positions.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, source_positions, zend_mir_source_position_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_view_frame_slot_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->frame_slots.count : 0;
}

static bool zend_mir_view_frame_slot_at(const void *context, uint32_t index,
		zend_mir_frame_slot_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_frame_slot_ref *records;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->frame_slots.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, frame_slots, zend_mir_frame_slot_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_view_root_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->roots.count : 0;
}

static bool zend_mir_view_root_at(const void *context, uint32_t index,
		uint32_t *slot_id_out)
{
	const zend_mir_module *module = context;
	uint32_t *records;

	if (!zend_mir_view_is_available(module) || slot_id_out == NULL
			|| index >= module->roots.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, roots, uint32_t);
	*slot_id_out = records[index];
	return true;
}

static uint32_t zend_mir_view_cleanup_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->cleanups.count : 0;
}

static bool zend_mir_view_cleanup_at(const void *context, uint32_t index,
		zend_mir_cleanup_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_cleanup_ref *records;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->cleanups.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, cleanups, zend_mir_cleanup_ref);
	*out = records[index];
	return true;
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
	const zend_mir_module *module = context;
	zend_mir_core_edge *edges;
	uint32_t count = 0;
	uint32_t index;

	if (!zend_mir_view_is_available(module) || block_id >= module->blocks.count) {
		return 0;
	}
	edges = ZEND_MIR_CORE_ITEMS(module, edges, zend_mir_core_edge);
	for (index = 0; index < module->edges.count; index++) {
		if (edges[index].from == block_id) {
			count++;
		}
	}
	return count;
}

static bool zend_mir_view_successor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	const zend_mir_module *module = context;
	zend_mir_core_edge *edges;
	uint32_t edge_index;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| block_id >= module->blocks.count) {
		return false;
	}
	edges = ZEND_MIR_CORE_ITEMS(module, edges, zend_mir_core_edge);
	for (edge_index = 0; edge_index < module->edges.count; edge_index++) {
		if (edges[edge_index].from == block_id) {
			if (index == 0) {
				*out = edges[edge_index].to;
				return true;
			}
			index--;
		}
	}
	return false;
}

static uint32_t zend_mir_view_predecessor_count(const void *context,
		zend_mir_block_id block_id)
{
	const zend_mir_module *module = context;
	zend_mir_core_edge *edges;
	uint32_t count = 0;
	uint32_t index;

	if (!zend_mir_view_is_available(module) || block_id >= module->blocks.count) {
		return 0;
	}
	edges = ZEND_MIR_CORE_ITEMS(module, edges, zend_mir_core_edge);
	for (index = 0; index < module->edges.count; index++) {
		if (edges[index].to == block_id) {
			count++;
		}
	}
	return count;
}

static bool zend_mir_view_predecessor_at(const void *context,
		zend_mir_block_id block_id, uint32_t index, zend_mir_block_id *out)
{
	const zend_mir_module *module = context;
	zend_mir_core_edge *edges;
	uint32_t edge_index;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| block_id >= module->blocks.count) {
		return false;
	}
	edges = ZEND_MIR_CORE_ITEMS(module, edges, zend_mir_core_edge);
	for (edge_index = 0; edge_index < module->edges.count; edge_index++) {
		if (edges[edge_index].to == block_id) {
			if (index == 0) {
				*out = edges[edge_index].from;
				return true;
			}
			index--;
		}
	}
	return false;
}

static bool zend_mir_view_source_map_at(const void *context, uint32_t index,
		zend_mir_source_map_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_source_map_ref *records;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->source_maps.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, source_maps, zend_mir_source_map_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_view_source_map_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->source_maps.count : 0;
}

static uint32_t zend_mir_view_value_fact_count(const void *context)
{
	const zend_mir_module *module = context;

	return zend_mir_view_is_available(module) ? module->value_facts.count : 0;
}

static bool zend_mir_view_value_fact_at(const void *context, uint32_t index,
		zend_mir_value_fact_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_value_fact_ref *facts;

	if (!zend_mir_view_is_available(module) || out == NULL
			|| index >= module->value_facts.count) {
		return false;
	}
	facts = ZEND_MIR_CORE_ITEMS(module, value_facts, zend_mir_value_fact_ref);
	*out = facts[index];
	return true;
}

static bool zend_mir_call_view_is_available(const zend_mir_module *module)
{
	return zend_mir_view_is_available(module)
		&& module->call_staging.committed;
}

static uint32_t zend_mir_call_view_site_count(const void *context)
{
	const zend_mir_module *module = context;
	return zend_mir_call_view_is_available(module)
		? module->call_sites.count : 0;
}

static bool zend_mir_call_view_site_at(
	const void *context, uint32_t index, zend_mir_call_site_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_call_site_ref *records;
	if (!zend_mir_call_view_is_available(module) || out == NULL
			|| index >= module->call_sites.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(module, call_sites, zend_mir_call_site_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_call_view_argument_count(const void *context)
{
	const zend_mir_module *module = context;
	return zend_mir_call_view_is_available(module)
		? module->call_arguments.count : 0;
}

static bool zend_mir_call_view_argument_at(
	const void *context, uint32_t index, zend_mir_call_argument_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_call_argument_ref *records;
	if (!zend_mir_call_view_is_available(module) || out == NULL
			|| index >= module->call_arguments.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, call_arguments, zend_mir_call_argument_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_call_view_target_count(const void *context)
{
	const zend_mir_module *module = context;
	return zend_mir_call_view_is_available(module)
		? module->call_targets.count : 0;
}

static bool zend_mir_call_view_target_at(
	const void *context, uint32_t index, zend_mir_call_target_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_call_target_ref *records;
	if (!zend_mir_call_view_is_available(module) || out == NULL
			|| index >= module->call_targets.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, call_targets, zend_mir_call_target_ref);
	*out = records[index];
	return true;
}

static uint32_t zend_mir_call_view_continuation_count(const void *context)
{
	const zend_mir_module *module = context;
	return zend_mir_call_view_is_available(module)
		? module->call_continuations.count : 0;
}

static bool zend_mir_call_view_continuation_at(
	const void *context, uint32_t index,
	zend_mir_call_continuation_ref *out)
{
	const zend_mir_module *module = context;
	zend_mir_call_continuation_ref *records;
	if (!zend_mir_call_view_is_available(module) || out == NULL
			|| index >= module->call_continuations.count) {
		return false;
	}
	records = ZEND_MIR_CORE_ITEMS(
		module, call_continuations, zend_mir_call_continuation_ref);
	*out = records[index];
	return true;
}

void zend_mir_module_init_call_view(zend_mir_module *module)
{
	memset(&module->call_view, 0, sizeof(module->call_view));
	module->call_view.contract_version = ZEND_MIR_W05_CONTRACT_VERSION;
	module->call_view.context = module;
	module->call_view.call_site_count = zend_mir_call_view_site_count;
	module->call_view.call_site_at = zend_mir_call_view_site_at;
	module->call_view.call_argument_count =
		zend_mir_call_view_argument_count;
	module->call_view.call_argument_at = zend_mir_call_view_argument_at;
	module->call_view.call_target_count = zend_mir_call_view_target_count;
	module->call_view.call_target_at = zend_mir_call_view_target_at;
	module->call_view.call_continuation_count =
		zend_mir_call_view_continuation_count;
	module->call_view.call_continuation_at =
		zend_mir_call_view_continuation_at;
}

static bool zend_mir_value_view_is_available(const zend_mir_module *module)
{
	return zend_mir_view_is_available(module)
		&& module->value_staging.committed;
}

#define ZEND_MIR_VALUE_VIEW_ACCESSORS(prefix, field, type) \
static uint32_t prefix##_count(const void *context) \
{ \
	const zend_mir_module *module = (const zend_mir_module *) context; \
	return zend_mir_value_view_is_available(module) \
		? module->field.count : 0; \
} \
static bool prefix##_at(const void *context, uint32_t index, type *out) \
{ \
	const zend_mir_module *module = (const zend_mir_module *) context; \
	type *records; \
	if (!zend_mir_value_view_is_available(module) || out == NULL \
			|| index >= module->field.count) { \
		return false; \
	} \
	records = ZEND_MIR_CORE_ITEMS(module, field, type); \
	*out = records[index]; \
	return true; \
}

ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_storage, value_storages, zend_mir_storage_ref)
ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_payload, value_payloads, zend_mir_payload_ref)
ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_reference_cell, value_reference_cells,
	zend_mir_reference_cell_ref)
ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_alias_relation, value_alias_relations,
	zend_mir_alias_relation_ref)
ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_ownership_event, value_ownership_events,
	zend_mir_ownership_event_ref)
ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_separation_plan, value_separation_plans,
	zend_mir_separation_plan_ref)
ZEND_MIR_VALUE_VIEW_ACCESSORS(
	zend_mir_value_view_call_transfer, value_call_transfers,
	zend_mir_call_transfer_ref)

#undef ZEND_MIR_VALUE_VIEW_ACCESSORS

void zend_mir_module_init_value_view(zend_mir_module *module)
{
	memset(&module->value_view, 0, sizeof(module->value_view));
	module->value_view.contract_version = ZEND_MIR_W06_CONTRACT_VERSION;
	module->value_view.context = module;
	module->value_view.storage_count = zend_mir_value_view_storage_count;
	module->value_view.storage_at = zend_mir_value_view_storage_at;
	module->value_view.payload_count = zend_mir_value_view_payload_count;
	module->value_view.payload_at = zend_mir_value_view_payload_at;
	module->value_view.reference_cell_count =
		zend_mir_value_view_reference_cell_count;
	module->value_view.reference_cell_at =
		zend_mir_value_view_reference_cell_at;
	module->value_view.alias_relation_count =
		zend_mir_value_view_alias_relation_count;
	module->value_view.alias_relation_at =
		zend_mir_value_view_alias_relation_at;
	module->value_view.ownership_event_count =
		zend_mir_value_view_ownership_event_count;
	module->value_view.ownership_event_at =
		zend_mir_value_view_ownership_event_at;
	module->value_view.separation_plan_count =
		zend_mir_value_view_separation_plan_count;
	module->value_view.separation_plan_at =
		zend_mir_value_view_separation_plan_at;
	module->value_view.call_transfer_count =
		zend_mir_value_view_call_transfer_count;
	module->value_view.call_transfer_at =
		zend_mir_value_view_call_transfer_at;
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
	module->view.frame_state_count = zend_mir_view_frame_state_count;
	module->view.frame_state_at = zend_mir_view_frame_state_at;
	module->view.source_position_count = zend_mir_view_source_position_count;
	module->view.source_position_at = zend_mir_view_source_position_at;
	module->view.frame_slot_count = zend_mir_view_frame_slot_count;
	module->view.frame_slot_at = zend_mir_view_frame_slot_at;
	module->view.root_count = zend_mir_view_root_count;
	module->view.root_at = zend_mir_view_root_at;
	module->view.cleanup_count = zend_mir_view_cleanup_count;
	module->view.cleanup_at = zend_mir_view_cleanup_at;
	module->view.instruction_operand_count = zend_mir_view_instruction_operand_count;
	module->view.instruction_operand_at = zend_mir_view_instruction_operand_at;
	module->view.successor_count = zend_mir_view_successor_count;
	module->view.successor_at = zend_mir_view_successor_at;
	module->view.predecessor_count = zend_mir_view_predecessor_count;
	module->view.predecessor_at = zend_mir_view_predecessor_at;
	module->view.source_map_count = zend_mir_view_source_map_count;
	module->view.source_map_at = zend_mir_view_source_map_at;
	module->view.value_fact_count = zend_mir_view_value_fact_count;
	module->view.value_fact_at = zend_mir_view_value_fact_at;
}
