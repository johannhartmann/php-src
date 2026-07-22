/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/zend_compile.h"
#include "Zend/zend_type_info.h"
#include "Zend/zend_vm_opcodes.h"
#include "Zend/Optimizer/zend_ssa.h"

#include "zend_mir_value_lowering.h"
#include "../../Calls/Model/zend_mir_call_model.h"
#include "../../Lowering/Core/zend_mir_lowering_internal.h"
#include "../../Lowering/StraightLine/zend_mir_straight_line_internal.h"
#include "../../Lowering/zend_mir_lowering_zend.h"
#include "../../MIR/Core/zend_mir_module_internal.h"
#include "../../MIR/Semantics/zend_mir_effect_summary.h"

#define ZEND_MIR_W06_SNAPSHOT_MAGIC UINT32_C(0x57365350)
#define ZEND_MIR_W06_LIMIT UINT32_C(1048576)

typedef struct _zend_mir_w06_records {
	uint32_t magic;
	zend_mir_source_storage_ref *source_storages;
	zend_mir_source_reference_ref *source_references;
	zend_mir_source_indirect_ref *source_indirects;
	zend_mir_source_opcode_ref *source_opcodes;
	zend_mir_source_call_site_ref *w05_sites;
	zend_mir_source_call_target_ref *w05_targets;
	zend_mir_source_call_argument_ref *w05_arguments;
	zend_mir_source_parameter_mode_ref *w05_parameter_modes;
	zend_mir_value_plan_entry *entries;
	zend_mir_storage_ref *storages;
	zend_mir_payload_ref *payloads;
	zend_mir_reference_cell_ref *references;
	zend_mir_alias_relation_ref *aliases;
	zend_mir_ownership_event_ref *events;
	zend_mir_separation_plan_ref *separations;
	zend_mir_call_transfer_ref *transfers;
	zend_mir_storage_id *call_return_storage_ids;
	zend_mir_storage_id *call_argument_storage_ids;
	uint32_t source_storage_count;
	uint32_t source_reference_count;
	uint32_t source_indirect_count;
	uint32_t source_opcode_count;
	uint32_t w05_site_count;
	uint32_t w05_target_count;
	uint32_t w05_argument_count;
	uint32_t w05_parameter_mode_count;
	uint32_t entry_count;
	uint32_t storage_count;
	uint32_t payload_count;
	uint32_t reference_count;
	uint32_t alias_count;
	uint32_t event_count;
	uint32_t separation_count;
	uint32_t transfer_count;
	zend_mir_source_call_view w05_calls;
	zend_mir_source_call_target_resolver w05_resolver;
} zend_mir_w06_records;

#ifdef ZEND_MIR_W06_TEST_FAULTS
static zend_mir_w06_test_fault zend_mir_w06_fault;

void zend_mir_w06_test_set_fault(zend_mir_w06_test_fault fault)
{
	zend_mir_w06_fault = fault;
}

static bool zend_mir_w06_fault_is(zend_mir_w06_test_fault fault)
{
	return zend_mir_w06_fault == fault;
}
#else
static bool zend_mir_w06_fault_is(zend_mir_w06_test_fault fault)
{
	(void) fault;
	return false;
}
#endif

static void *zend_mir_w06_calloc(uint32_t count, size_t size)
{
	if (count == 0) {
		return NULL;
	}
	if (count > ZEND_MIR_W06_LIMIT
			|| (size != 0 && count > SIZE_MAX / size)) {
		return NULL;
	}
	return calloc(count, size);
}

bool zend_mir_w06_opcode_is_accepted(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_ASSIGN:
		case ZEND_ASSIGN_REF:
		case ZEND_CHECK_VAR:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_REF:
		case ZEND_FREE:
		case ZEND_CHECK_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_RETURN_BY_REF:
		case ZEND_MAKE_REF:
		case ZEND_UNSET_CV:
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_SEPARATE:
		case ZEND_COPY_TMP:
		case ZEND_SEND_FUNC_ARG:
			return true;
		default:
			return false;
	}
}

static zend_mir_opcode zend_mir_w09_executable_opcode(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_DIV:
		case ZEND_MOD:
		case ZEND_POW:
		case ZEND_SL:
		case ZEND_SR:
		case ZEND_BW_OR:
		case ZEND_BW_AND:
		case ZEND_BW_XOR:
		case ZEND_BOOL_XOR:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_SPACESHIP:
			return ZEND_MIR_OPCODE_VALUE_BINARY_OP;
		case ZEND_BW_NOT:
		case ZEND_BOOL_NOT:
		case ZEND_BOOL:
			return ZEND_MIR_OPCODE_VALUE_UNARY_OP;
		case ZEND_CAST:
			return ZEND_MIR_OPCODE_VALUE_CAST;
		case ZEND_ISSET_ISEMPTY_CV:
			return ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV;
		case ZEND_FETCH_LIST_R:
		case ZEND_FETCH_LIST_W:
			return ZEND_MIR_OPCODE_VALUE_FETCH_LIST;
		case ZEND_ASSIGN:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN;
		case ZEND_ASSIGN_OP:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_OP;
		case ZEND_QM_ASSIGN:
			return ZEND_MIR_OPCODE_VALUE_QM_ASSIGN;
		case ZEND_CONCAT:
			return ZEND_MIR_OPCODE_VALUE_CONCAT;
		case ZEND_FAST_CONCAT:
			return ZEND_MIR_OPCODE_VALUE_FAST_CONCAT;
		case ZEND_ROPE_INIT:
			return ZEND_MIR_OPCODE_VALUE_ROPE_INIT;
		case ZEND_ROPE_ADD:
			return ZEND_MIR_OPCODE_VALUE_ROPE_ADD;
		case ZEND_ROPE_END:
			return ZEND_MIR_OPCODE_VALUE_ROPE_END;
		case ZEND_INIT_ARRAY:
			return ZEND_MIR_OPCODE_VALUE_INIT_ARRAY;
		case ZEND_ADD_ARRAY_ELEMENT:
			return ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT;
		case ZEND_ADD_ARRAY_UNPACK:
			return ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK;
		case ZEND_FETCH_DIM_R:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R;
		case ZEND_FETCH_DIM_W:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W;
		case ZEND_FETCH_DIM_RW:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW;
		case ZEND_FETCH_DIM_IS:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS;
		case ZEND_FETCH_DIM_FUNC_ARG:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG;
		case ZEND_FETCH_DIM_UNSET:
			return ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET;
		case ZEND_ASSIGN_DIM:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM;
		case ZEND_ASSIGN_DIM_OP:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP;
		case ZEND_UNSET_DIM:
			return ZEND_MIR_OPCODE_VALUE_UNSET_DIM;
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
			return ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM;
		case ZEND_FE_FREE:
			return ZEND_MIR_OPCODE_VALUE_FE_FREE;
		case ZEND_MAKE_REF:
			return ZEND_MIR_OPCODE_VALUE_MAKE_REF;
		case ZEND_ASSIGN_REF:
			return ZEND_MIR_OPCODE_VALUE_ASSIGN_REF;
		case ZEND_SEPARATE:
			return ZEND_MIR_OPCODE_VALUE_SEPARATE;
		case ZEND_COPY_TMP:
			return ZEND_MIR_OPCODE_VALUE_COPY_TMP;
		case ZEND_FREE:
			return ZEND_MIR_OPCODE_VALUE_FREE;
		case ZEND_UNSET_CV:
			return ZEND_MIR_OPCODE_VALUE_UNSET_CV;
		case ZEND_CHECK_VAR:
			return ZEND_MIR_OPCODE_VALUE_CHECK_VAR;
		default:
			return ZEND_MIR_OPCODE_INVALID;
	}
}

bool zend_mir_w09_opcode_is_executable(uint32_t opcode)
{
	return opcode == ZEND_OP_DATA
		|| opcode == ZEND_FE_RESET_R || opcode == ZEND_FE_FETCH_R
		|| opcode == ZEND_FE_RESET_RW || opcode == ZEND_FE_FETCH_RW
		|| zend_mir_w09_executable_opcode(opcode) != ZEND_MIR_OPCODE_INVALID;
}

static bool zend_mir_w09_add_effect(
	zend_mir_effect_summary *summary, zend_mir_effect effect)
{
	zend_mir_effect_summary atomic;
	zend_mir_effect_summary composed;

	if (!zend_mir_effect_summary_from_effect(effect, &atomic)
			|| !zend_mir_effect_summary_compose(
				&composed, summary, &atomic)) {
		return false;
	}
	*summary = composed;
	return true;
}

static bool zend_mir_w09_operation_semantics(
	zend_mir_opcode opcode, zend_mir_executable_value_ref *operation,
	zend_mir_safepoint_class *frame_class)
{
	zend_mir_effect_summary summary;
	const zend_mir_memory_domain_mask frame_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_LOCALS)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_TEMPS);
	const zend_mir_memory_domain_mask reference_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_REFERENCE)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_GC_METADATA);

	zend_mir_effect_summary_empty(&summary);
	if (!zend_mir_w09_add_effect(&summary, ZEND_MIR_EFFECT_READ_MEMORY)
			|| !zend_mir_w09_add_effect(&summary, ZEND_MIR_EFFECT_WRITE_MEMORY)) {
		return false;
	}
	switch (opcode) {
		case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			if (!zend_mir_w09_add_effect(
					&summary, ZEND_MIR_EFFECT_ALLOCATE)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
		case ZEND_MIR_OPCODE_VALUE_CONCAT:
		case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
		case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
		case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
		case ZEND_MIR_OPCODE_VALUE_ROPE_END:
		case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP:
		case ZEND_MIR_OPCODE_VALUE_UNSET_DIM:
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
		case ZEND_MIR_OPCODE_VALUE_FE_FREE:
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
		case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
		case ZEND_MIR_OPCODE_VALUE_CAST:
		case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
			if (!zend_mir_w09_add_effect(&summary, ZEND_MIR_EFFECT_ALLOCATE)
					|| !zend_mir_w09_add_effect(
						&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_w09_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			if (!zend_mir_w09_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_FREE:
		case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
			if (!zend_mir_w09_add_effect(
					&summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
					|| !zend_mir_w09_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
			if (!zend_mir_w09_add_effect(
					&summary, ZEND_MIR_EFFECT_OBSERVE_FRAME)
					|| !zend_mir_w09_add_effect(
						&summary, ZEND_MIR_EFFECT_REENTER_PHP)
					|| !zend_mir_w09_add_effect(
						&summary, ZEND_MIR_EFFECT_THROW)) {
				return false;
			}
			break;
		case ZEND_MIR_OPCODE_VALUE_SEPARATE:
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			break;
		default:
			return false;
	}
	summary.reads |= frame_domains | reference_domains;
	summary.writes |= frame_domains | reference_domains;
	if (!zend_mir_effect_summary_init(&summary, summary.effects,
			summary.reads, summary.writes, summary.barriers, 0, 0)) {
		return false;
	}
	operation->effects = summary.effects;
	operation->reads = summary.reads;
	operation->writes = summary.writes;
	operation->barriers = summary.barriers;
	operation->ownership_actions = 0;
	if ((summary.barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_DESTRUCTOR)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR;
	} else if ((summary.barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_EXCEPTION)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_EXCEPTION_EDGE;
	} else if ((summary.barriers
			& ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_OBSERVER;
	} else if ((summary.effects
			& ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_ALLOCATE)) != 0) {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_ALLOCATION;
	} else {
		*frame_class = ZEND_MIR_SAFEPOINT_CLASS_INVALID;
	}
	return true;
}

static bool zend_mir_w09_source_block(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out)
{
	uint32_t index;

	for (index = 0; index < source->block_count(source->context); index++) {
		if (!source->block_at(source->context, index, out)) {
			return false;
		}
		if (out->id == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w09_mir_block(
	const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map, const zend_mir_view *view,
	zend_mir_source_block_id source_id, zend_mir_block_id *out)
{
	uint32_t index;
	uint32_t reachable_index = 0;

	if (source == NULL || view == NULL || out == NULL
			|| source->block_count == NULL || source->block_at == NULL
			|| view->block_count == NULL || view->block_at == NULL) {
		return false;
	}
	if (map != NULL && map->block_count != NULL && map->block_at != NULL) {
		for (index = 0; index < map->block_count(map->context); index++) {
			zend_mir_control_flow_block_mapping mapping;
			if (!map->block_at(map->context, index, &mapping)) {
				return false;
			}
			if (mapping.source_block_id == source_id) {
				*out = mapping.mir_block_id;
				return true;
			}
		}
	}
	/*
	 * W04 intentionally invalidates its process-local map after stage 3.
	 * Its persistent block order is nevertheless source-backed: reachable
	 * source blocks are created once, in source-table order, before calls or
	 * value operations are appended. Reconstruct only that stable ordinal.
	 */
	for (index = 0; index < source->block_count(source->context); index++) {
		zend_mir_source_block_ref source_block;
		zend_mir_block_record mir_block;

		if (!source->block_at(source->context, index, &source_block)) {
			return false;
		}
		if ((source_block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if (source_block.id == source_id) {
			if (reachable_index >= view->block_count(view->context)
					|| !view->block_at(
						view->context, reachable_index, &mir_block)) {
				return false;
			}
			*out = mir_block.id;
			return true;
		}
		reachable_index++;
	}
	return false;
}

static int zend_mir_w09_compare_operations(const void *left, const void *right)
{
	const zend_mir_executable_value_ref *a = left;
	const zend_mir_executable_value_ref *b = right;

	if (a->block_id != b->block_id) {
		return a->block_id < b->block_id ? -1 : 1;
	}
	if (a->source_position_id != b->source_position_id) {
		return a->source_position_id < b->source_position_id ? -1 : 1;
	}
	return 0;
}

bool zend_mir_w09_emit_executable_values(
	const zend_op_array *op_array,
	zend_mir_lowering_context *lowering_context,
	zend_mir_module *module,
	const zend_mir_control_flow_map *control_flow_map,
	zend_mir_straight_line_provider_context *frame_context)
{
	const zend_mir_lowering_source_view *source;
	const zend_mir_view *view;
	zend_mir_executable_value_ref *operations;
	zend_mir_value_mutator *value_mutator;
	zend_mir_mutator *mutator;
	uint32_t operation_count = 0;
	uint32_t index;
	bool success = false;

	if (op_array == NULL || lowering_context == NULL || module == NULL
			|| control_flow_map == NULL || frame_context == NULL
			|| op_array->last > ZEND_MIR_W06_LIMIT) {
		return false;
	}
	source = lowering_context->source;
	view = lowering_context->module_ops.view(
		lowering_context->module_ops.context, module);
	if (source == NULL || view == NULL
			|| source->opcode_count(source->context) != op_array->last) {
		return false;
	}
	operations = zend_mir_w06_calloc(op_array->last, sizeof(*operations));
	if (op_array->last != 0 && operations == NULL) {
		return false;
	}
	mutator = lowering_context->module_ops.mutator(
		lowering_context->module_ops.context, module);
	value_mutator = zend_mir_module_get_value_mutator(module);
	if (mutator == NULL || value_mutator == NULL
			|| value_mutator->add_executable_operation == NULL) {
		goto done;
	}
	for (index = 0; index < op_array->last; index++) {
		zend_mir_source_opcode_ref source_opcode;
		zend_mir_source_block_ref source_block;
		zend_mir_executable_value_ref *operation;
		zend_mir_safepoint_class frame_class;
		zend_mir_opcode opcode = zend_mir_w09_executable_opcode(
			op_array->opcodes[index].opcode);

		if (opcode == ZEND_MIR_OPCODE_INVALID) {
			continue;
		}
		if (!source->opcode_at(source->context, index, &source_opcode)
				|| source_opcode.opline_index != index
				|| !zend_mir_w09_source_block(
					source, source_opcode.block_id, &source_block)) {
			goto done;
		}
		if ((source_block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		operation = &operations[operation_count];
		memset(operation, 0, sizeof(*operation));
		operation->id = ZEND_MIR_ID_INVALID;
		operation->opcode = opcode;
		operation->source_position_id = source_opcode.source_position_id;
		operation->frame_state_id = ZEND_MIR_ID_INVALID;
		if (!zend_mir_w09_mir_block(source, control_flow_map, view,
				source_opcode.block_id, &operation->block_id)
				|| !zend_mir_w09_operation_semantics(
					opcode, operation, &frame_class)) {
			goto done;
		}
		if (frame_class != ZEND_MIR_SAFEPOINT_CLASS_INVALID) {
			zend_mir_source_position_id emitted_source;
			if (!zend_mir_straight_line_emit_frame_for_class(
					lowering_context, &source_opcode, mutator, frame_context,
					frame_class, &operation->frame_state_id, &emitted_source)
					|| emitted_source != operation->source_position_id) {
				goto done;
			}
		}
		operation_count++;
	}
	if (operation_count == 0) {
		success = true;
		goto done;
	}
	qsort(operations, operation_count, sizeof(*operations),
		zend_mir_w09_compare_operations);
	for (index = 0; index < operation_count; index++) {
		if (!value_mutator->add_executable_operation(
				value_mutator->context, &operations[index])) {
			goto done;
		}
	}
	success = zend_mir_module_commit_value_model(module);

done:
	free(operations);
	return success;
}

static bool zend_mir_w06_profile_opcode_is_accepted(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_ASSIGN:
		case ZEND_ASSIGN_REF:
		case ZEND_QM_ASSIGN:
		case ZEND_CHECK_VAR:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_DO_FCALL:
		case ZEND_INIT_FCALL:
		case ZEND_RETURN:
		case ZEND_RECV:
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_FREE:
		case ZEND_CHECK_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_RETURN_BY_REF:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_DO_UCALL:
		case ZEND_MAKE_REF:
		case ZEND_UNSET_CV:
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_SEPARATE:
		case ZEND_COPY_TMP:
		case ZEND_SEND_FUNC_ARG:
			return true;
		default:
			return false;
	}
}

static uint32_t zend_mir_w06_source_storage_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->source_storage_count;
}

static bool zend_mir_w06_source_storage_at(
	const void *context, uint32_t index, zend_mir_source_storage_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->source_storage_count) {
		return false;
	}
	*out = records->source_storages[index];
	return true;
}

static uint32_t zend_mir_w06_source_reference_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->source_reference_count;
}

static bool zend_mir_w06_source_reference_at(
	const void *context, uint32_t index, zend_mir_source_reference_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->source_reference_count) {
		return false;
	}
	*out = records->source_references[index];
	return true;
}

static uint32_t zend_mir_w06_source_indirect_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->source_indirect_count;
}

static bool zend_mir_w06_source_indirect_at(
	const void *context, uint32_t index, zend_mir_source_indirect_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->source_indirect_count) {
		return false;
	}
	*out = records->source_indirects[index];
	return true;
}

static uint32_t zend_mir_w06_source_opcode_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->source_opcode_count;
}

static bool zend_mir_w06_source_opcode_at(
	const void *context, uint32_t index, zend_mir_source_opcode_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->source_opcode_count) {
		return false;
	}
	*out = records->source_opcodes[index];
	return true;
}

static bool zend_mir_w06_operand_storage(
	const zend_op_array *op_array,
	const zend_mir_source_operand_ref *operand,
	uint32_t *storage_id)
{
	uint32_t base;

	if (op_array == NULL || operand == NULL || storage_id == NULL
			|| operand->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operand->slot_kind < ZEND_MIR_SOURCE_SLOT_CV
			|| operand->slot_kind > ZEND_MIR_SOURCE_SLOT_VAR) {
		return false;
	}
	base = operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV
		? 0 : (uint32_t) op_array->last_var;
	if (operand->index > ZEND_MIR_ID_MAX - base) {
		return false;
	}
	*storage_id = base + operand->index;
	return *storage_id < (uint32_t) op_array->last_var + op_array->T;
}

static bool zend_mir_w06_ensure_reference(
	const zend_op_array *op_array,
	const zend_mir_source_operand_ref *operand,
	uint32_t source_position_id,
	uint32_t *reference_origins,
	uint32_t *creation_sources)
{
	uint32_t storage_id;
	if (!zend_mir_w06_operand_storage(op_array, operand, &storage_id)) {
		return false;
	}
	if (!zend_mir_id_is_valid(reference_origins[storage_id])) {
		reference_origins[storage_id] = storage_id;
		creation_sources[storage_id] = source_position_id;
	}
	return true;
}

static bool zend_mir_w06_adopt_reference(
	const zend_op_array *op_array,
	const zend_mir_source_operand_ref *target,
	const zend_mir_source_operand_ref *source,
	bool optional_target,
	uint32_t *reference_origins)
{
	uint32_t source_storage;
	uint32_t target_storage;

	if (optional_target
			&& target->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return true;
	}
	if (!zend_mir_w06_operand_storage(
			op_array, source, &source_storage)
			|| !zend_mir_w06_operand_storage(
				op_array, target, &target_storage)
			|| !zend_mir_id_is_valid(
				reference_origins[source_storage])) {
		return false;
	}
	reference_origins[target_storage] =
		reference_origins[source_storage];
	return true;
}

static bool zend_mir_w06_inventory_opcode(
	const zend_op_array *op_array,
	const zend_mir_source_opcode_ref *opcode,
	uint32_t *reference_origins,
	uint32_t *creation_sources)
{
	switch (opcode->zend_opcode_number) {
		case ZEND_MAKE_REF:
			return zend_mir_w06_ensure_reference(
					op_array, &opcode->op1, opcode->source_position_id,
					reference_origins, creation_sources)
				&& zend_mir_w06_adopt_reference(
					op_array, &opcode->result, &opcode->op1, true,
					reference_origins);
		case ZEND_ASSIGN_REF:
			return zend_mir_w06_ensure_reference(
					op_array, &opcode->op2, opcode->source_position_id,
					reference_origins, creation_sources)
				&& zend_mir_w06_adopt_reference(
					op_array, &opcode->op1, &opcode->op2, false,
					reference_origins)
				&& zend_mir_w06_adopt_reference(
					op_array, &opcode->result, &opcode->op2, true,
					reference_origins);
		case ZEND_SEND_REF:
		case ZEND_RETURN_BY_REF:
			return zend_mir_w06_ensure_reference(
				op_array, &opcode->op1, opcode->source_position_id,
				reference_origins, creation_sources);
		case ZEND_COPY_TMP:
		{
			uint32_t source_storage;
			if (!zend_mir_w06_operand_storage(
					op_array, &opcode->op1, &source_storage)
					|| !zend_mir_id_is_valid(
						reference_origins[source_storage])) {
				return true;
			}
			return zend_mir_w06_adopt_reference(
				op_array, &opcode->result, &opcode->op1, false,
				reference_origins);
		}
		default:
			return true;
	}
}

static bool zend_mir_w06_inventory_parameters(
	const zend_op_array *op_array,
	const zend_mir_w06_records *records,
	uint32_t *reference_origins,
	uint32_t *creation_sources)
{
	uint32_t parameter;

	if (op_array->num_args > (uint32_t) op_array->last_var
			|| (op_array->num_args != 0 && op_array->arg_info == NULL)) {
		return false;
	}
	for (parameter = 0; parameter < op_array->num_args; parameter++) {
		uint32_t opcode_index;
		if ((ZEND_ARG_SEND_MODE(&op_array->arg_info[parameter])
				& ZEND_SEND_BY_REF) == 0) {
			continue;
		}
		reference_origins[parameter] = parameter;
		for (opcode_index = 0;
				opcode_index < records->source_opcode_count;
				opcode_index++) {
			const zend_mir_source_opcode_ref *opcode =
				&records->source_opcodes[opcode_index];
			uint32_t storage_id;
			if ((opcode->zend_opcode_number == ZEND_RECV
					|| opcode->zend_opcode_number == ZEND_RECV_INIT)
					&& zend_mir_w06_operand_storage(
						op_array, &opcode->result, &storage_id)
					&& storage_id == parameter) {
				creation_sources[parameter] =
					opcode->source_position_id;
				break;
			}
		}
		if (!zend_mir_id_is_valid(creation_sources[parameter])) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w06_finalize_reference_ids(
	uint32_t storage_count,
	const uint32_t *reference_origins,
	const uint32_t *creation_sources,
	uint32_t *reference_ids)
{
	uint32_t next_id = 0;
	uint32_t storage_id;

	for (storage_id = 0; storage_id < storage_count; storage_id++) {
		uint32_t origin = reference_origins[storage_id];
		if (!zend_mir_id_is_valid(origin)) {
			continue;
		}
		if (origin >= storage_count
				|| !zend_mir_id_is_valid(creation_sources[origin])) {
			return false;
		}
		if (!zend_mir_id_is_valid(reference_ids[origin])) {
			if (next_id == ZEND_MIR_ID_INVALID) {
				return false;
			}
			reference_ids[origin] = next_id++;
		}
	}
	return true;
}

static zend_mir_value_category zend_mir_w06_type_category(uint32_t type)
{
	const uint32_t scalar_types =
		MAY_BE_NULL | MAY_BE_FALSE | MAY_BE_TRUE
		| MAY_BE_LONG | MAY_BE_DOUBLE;
	uint32_t concrete = type & MAY_BE_ANY;
	uint32_t categories = 0;

	/*
	 * UNDEF and refcount/cardinality bits qualify a concrete value; they do
	 * not create another value category. Multiple scalar alternatives remain
	 * one non-refcounted category, while a scalar/refcounted union stays
	 * deliberately unknown.
	 */
	if ((concrete & scalar_types) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	if ((concrete & MAY_BE_STRING) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_REFCOUNTED_STRING;
	}
	if ((concrete & MAY_BE_ARRAY) != 0) {
		categories |=
			UINT32_C(1) << ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT;
	}
	if ((concrete & MAY_BE_OBJECT) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_OBJECT_ABSTRACT;
	}
	if ((concrete & MAY_BE_RESOURCE) != 0) {
		categories |= UINT32_C(1) << ZEND_MIR_VALUE_RESOURCE_ABSTRACT;
	}
	if (categories == 0 && (type & MAY_BE_UNDEF) != 0) {
		categories =
			UINT32_C(1) << ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	if (categories == 0 || (categories & (categories - 1)) != 0) {
		return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	}
	if ((categories
			& (UINT32_C(1) << ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR)) != 0) {
		return ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	if ((categories
			& (UINT32_C(1) << ZEND_MIR_VALUE_REFCOUNTED_STRING)) != 0) {
		return ZEND_MIR_VALUE_REFCOUNTED_STRING;
	}
	if ((categories
			& (UINT32_C(1)
				<< ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT)) != 0) {
		return ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT;
	}
	if ((categories
			& (UINT32_C(1) << ZEND_MIR_VALUE_OBJECT_ABSTRACT)) != 0) {
		return ZEND_MIR_VALUE_OBJECT_ABSTRACT;
	}
	return ZEND_MIR_VALUE_RESOURCE_ABSTRACT;
}

static zend_mir_value_category zend_mir_w06_ssa_category(
	const zend_ssa *ssa, uint32_t ssa_variable_id)
{
	if (ssa == NULL || ssa->vars_count < 0 || ssa->var_info == NULL
			|| ssa_variable_id >= (uint32_t) ssa->vars_count) {
		return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	}
	return zend_mir_w06_type_category(ssa->var_info[ssa_variable_id].type);
}

static uint32_t zend_mir_w06_storage_ssa_variable(
	const zend_ssa *ssa, uint32_t storage_id)
{
	uint32_t result = ZEND_MIR_ID_INVALID;
	uint32_t index;

	if (ssa == NULL || ssa->vars_count < 0 || ssa->vars == NULL) {
		return result;
	}
	for (index = 0; index < (uint32_t) ssa->vars_count; index++) {
		if (ssa->vars[index].var >= 0
				&& (uint32_t) ssa->vars[index].var == storage_id) {
			result = index;
		}
	}
	return result;
}

static zend_mir_value_category zend_mir_w06_storage_category(
	const zend_ssa *ssa, uint32_t storage_id)
{
	zend_mir_value_category category = ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	uint32_t index;

	if (ssa == NULL || ssa->vars_count < 0 || ssa->vars == NULL
			|| ssa->var_info == NULL) {
		return category;
	}
	for (index = 0; index < (uint32_t) ssa->vars_count; index++) {
		zend_mir_value_category candidate;
		if (ssa->vars[index].var < 0
				|| (uint32_t) ssa->vars[index].var != storage_id) {
			continue;
		}
		candidate = zend_mir_w06_type_category(ssa->var_info[index].type);
		if (candidate == ZEND_MIR_VALUE_CATEGORY_UNKNOWN) {
			continue;
		}
		if (category != ZEND_MIR_VALUE_CATEGORY_UNKNOWN
				&& category != candidate) {
			return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
		}
		category = candidate;
	}
	return category;
}

static zend_mir_value_category zend_mir_w06_storage_call_return_category(
	const zend_ssa *ssa,
	const zend_mir_zend_source *zend_source,
	const zend_mir_source_call_view *source_calls,
	uint32_t storage_id)
{
	zend_mir_value_category category = ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	uint32_t count =
		source_calls->call_site_count(source_calls->context);
	uint32_t index;

	if (ssa == NULL || ssa->vars_count < 0 || ssa->vars == NULL) {
		return category;
	}
	for (index = 0; index < count; index++) {
		zend_mir_source_call_site_ref site;
		zend_mir_value_category candidate;
		uint32_t return_type;

		if (!source_calls->call_site_at(
				source_calls->context, index, &site)
				|| !zend_mir_id_is_valid(site.result_ssa_variable_id)
				|| site.result_ssa_variable_id
					>= (uint32_t) ssa->vars_count
				|| ssa->vars[site.result_ssa_variable_id].var < 0
				|| (uint32_t) ssa->vars[
					site.result_ssa_variable_id].var != storage_id
				|| !zend_mir_zend_source_w06_call_return_type(
					zend_source, site.target_id, &return_type)) {
			continue;
		}
		candidate = zend_mir_w06_type_category(return_type);
		if (category != ZEND_MIR_VALUE_CATEGORY_UNKNOWN
				&& category != candidate) {
			return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
		}
		category = candidate;
	}
	return category;
}

static zend_mir_value_category zend_mir_w06_operand_category(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_source_operand_ref *operand)
{
	uint32_t storage_id;
	zend_mir_value_category category;

	if (zend_mir_id_is_valid(operand->ssa_variable_id)) {
		category = zend_mir_w06_ssa_category(
			ssa, operand->ssa_variable_id);
		if (category != ZEND_MIR_VALUE_CATEGORY_UNKNOWN) {
			return category;
		}
	}
	if (zend_mir_w06_operand_storage(
			op_array, operand, &storage_id)) {
		return zend_mir_w06_storage_category(ssa, storage_id);
	}
	return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
}

static zend_mir_lowering_diagnostic_code
zend_mir_w06_preflight_transitions(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_mir_w06_records *records)
{
	uint32_t index;

	for (index = 0; index < records->source_opcode_count; index++) {
		const zend_mir_source_opcode_ref *opcode =
			&records->source_opcodes[index];
		zend_mir_value_category source_category;
		zend_mir_value_category target_category;

		switch (opcode->zend_opcode_number) {
			case ZEND_ASSIGN:
				target_category = zend_mir_w06_operand_category(
					op_array, ssa, &opcode->op1);
				source_category = zend_mir_w06_operand_category(
					op_array, ssa, &opcode->op2);
				/*
				 * A refcounted overwrite requires versioned old/new
				 * storage identities. A1 deliberately has no such
				 * record, so only the destructor-free scalar case is
				 * complete in W06.
				 */
				if (target_category
							!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						|| source_category
							!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR) {
					return
						ZEND_MIRL_W06_INVALID_OWNERSHIP_TRANSITION;
				}
				break;
			case ZEND_UNSET_CV:
			case ZEND_COPY_TMP:
			case ZEND_FREE:
				source_category = zend_mir_w06_operand_category(
					op_array, ssa, &opcode->op1);
				if (source_category
							!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						&& source_category
							!= ZEND_MIR_VALUE_REFCOUNTED_STRING) {
					return
						ZEND_MIRL_W06_INVALID_OWNERSHIP_TRANSITION;
				}
				break;
			case ZEND_MAKE_REF:
			case ZEND_ASSIGN_REF:
				source_category = zend_mir_w06_operand_category(
					op_array, ssa, &opcode->op1);
				if (source_category
							!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						&& source_category
							!= ZEND_MIR_VALUE_REFCOUNTED_STRING) {
					return
						ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
				}
				break;
			case ZEND_RETURN_BY_REF:
				/*
				 * The VM accepts CONST/TMP and some VAR returns only by
				 * creating a reference at runtime after emitting a notice.
				 * W06 does not perform runtime reference binding, so its
				 * exact proof is limited to a source-backed CV lvalue.
				 */
				if (opcode->op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV) {
					return
						ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
				}
				source_category = zend_mir_w06_operand_category(
					op_array, ssa, &opcode->op1);
				if (source_category
							!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						&& source_category
							!= ZEND_MIR_VALUE_REFCOUNTED_STRING) {
					return
						ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
				}
				break;
			case ZEND_SEPARATE:
				source_category = zend_mir_w06_operand_category(
					op_array, ssa, &opcode->op1);
				if (source_category
						!= ZEND_MIR_VALUE_REFCOUNTED_STRING) {
					return
						ZEND_MIRL_W06_SEPARATION_DEFERRED;
				}
				break;
			default:
				break;
		}
	}
	return ZEND_MIRL_OK;
}

static zend_mir_lowering_diagnostic_code
zend_mir_w06_preflight_call_returns(
	const zend_ssa *ssa,
	const zend_mir_zend_source *zend_source,
	const zend_mir_source_call_view *source_calls)
{
	uint32_t count =
		source_calls->call_site_count(source_calls->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_source_call_site_ref site;
		zend_mir_value_category category;
		uint32_t return_type;

		if (!source_calls->call_site_at(
				source_calls->context, index, &site)) {
			return ZEND_MIRL_INVALID_SOURCE;
		}
		category = zend_mir_id_is_valid(site.result_ssa_variable_id)
			? zend_mir_w06_ssa_category(
				ssa, site.result_ssa_variable_id)
			: ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
		if (category == ZEND_MIR_VALUE_CATEGORY_UNKNOWN
				&& zend_mir_zend_source_w06_call_return_type(
					zend_source, site.target_id, &return_type)) {
			category = zend_mir_w06_type_category(return_type);
		}
		if (category != ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				&& category != ZEND_MIR_VALUE_REFCOUNTED_STRING) {
			return ZEND_MIRL_W06_CALL_TRANSFER_DEFERRED;
		}
	}
	return ZEND_MIRL_OK;
}

static void zend_mir_w06_release_records(zend_mir_w06_records *records)
{
	if (records == NULL) {
		return;
	}
	free(records->call_return_storage_ids);
	free(records->call_argument_storage_ids);
	free(records->transfers);
	free(records->separations);
	free(records->events);
	free(records->aliases);
	free(records->references);
	free(records->payloads);
	free(records->storages);
	free(records->entries);
	free(records->w05_parameter_modes);
	free(records->w05_arguments);
	free(records->w05_targets);
	free(records->w05_sites);
	free(records->source_opcodes);
	free(records->source_indirects);
	free(records->source_references);
	free(records->source_storages);
	free(records);
}

static uint32_t zend_mir_w06_w05_site_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->w05_site_count;
}

static bool zend_mir_w06_w05_site_at(
	const void *context, uint32_t index, zend_mir_source_call_site_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->w05_site_count) {
		return false;
	}
	*out = records->w05_sites[index];
	return true;
}

static uint32_t zend_mir_w06_w05_target_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->w05_target_count;
}

static bool zend_mir_w06_w05_target_at(
	const void *context, uint32_t index, zend_mir_source_call_target_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->w05_target_count) {
		return false;
	}
	*out = records->w05_targets[index];
	return true;
}

static uint32_t zend_mir_w06_w05_argument_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->w05_argument_count;
}

static bool zend_mir_w06_w05_argument_at(
	const void *context, uint32_t index,
	zend_mir_source_call_argument_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->w05_argument_count) {
		return false;
	}
	*out = records->w05_arguments[index];
	return true;
}

static uint32_t zend_mir_w06_w05_parameter_mode_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->w05_parameter_mode_count;
}

static bool zend_mir_w06_w05_parameter_mode_at(
	const void *context, uint32_t index,
	zend_mir_source_parameter_mode_ref *out)
{
	const zend_mir_w06_records *records = context;
	if (out == NULL || index >= records->w05_parameter_mode_count) {
		return false;
	}
	*out = records->w05_parameter_modes[index];
	return true;
}

static uint32_t zend_mir_w06_w05_source_opcode_count(const void *context)
{
	const zend_mir_w06_records *records = context;
	return records->source_opcode_count;
}

static bool zend_mir_w06_w05_source_opcode_at(
	const void *context, uint32_t index, zend_mir_source_opcode_ref *out)
{
	const zend_mir_w06_records *records = context;
	uint32_t argument_index;

	if (out == NULL || index >= records->source_opcode_count) {
		return false;
	}
	*out = records->source_opcodes[index];
	for (argument_index = 0;
			argument_index < records->w05_argument_count;
			argument_index++) {
		const zend_mir_source_call_argument_ref *argument =
			&records->w05_arguments[argument_index];

		if (argument->send_opline_index != out->opline_index) {
			continue;
		}
		/*
		 * The frozen W05 grammar accepts only the scalar SEND_VAL/VAR
		 * spellings. W06 keeps the original opcode in its value inventory,
		 * while this private prerequisite projection exposes the equivalent
		 * scalar spelling. The additive W06 transfer table retains the
		 * original by-reference/refcounted mode and source operand.
		 */
		out->zend_opcode_number =
			argument->source_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL
			? ZEND_SEND_VAL : ZEND_SEND_VAR;
		break;
	}
	return true;
}

static bool zend_mir_w06_w05_resolve(
	const void *context, zend_mir_source_call_target_id target_id,
	zend_mir_source_call_target_ref *out)
{
	return zend_mir_w06_w05_target_at(context, target_id, out);
}

static bool zend_mir_w06_private_result_use(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
			return true;
		default:
			return zend_mir_w06_opcode_is_accepted(opcode);
	}
}

/*
 * W05's frozen records remain a scalar prerequisite. W06 therefore supplies
 * an explicit scalar projection for structural call planning while retaining
 * the unmodified source call table for the W06 transfer plan. Reference modes
 * and result ownership are represented only by the additive W06 records.
 */
static bool zend_mir_w06_build_w05_projection(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_source_call_view *source_calls,
	zend_mir_w06_records *records)
{
	uint32_t index;

	if (source_calls->call_site_count == NULL
			|| source_calls->call_target_count == NULL
			|| source_calls->parameter_mode_count == NULL
			|| source_calls->parameter_mode_at == NULL) {
		return false;
	}
	records->w05_site_count =
		source_calls->call_site_count(source_calls->context);
	records->w05_target_count =
		source_calls->call_target_count(source_calls->context);
	records->w05_argument_count =
		source_calls->call_argument_count(source_calls->context);
	records->w05_parameter_mode_count =
		source_calls->parameter_mode_count(source_calls->context);
	records->w05_sites = zend_mir_w06_calloc(
		records->w05_site_count, sizeof(*records->w05_sites));
	records->w05_targets = zend_mir_w06_calloc(
		records->w05_target_count, sizeof(*records->w05_targets));
	records->w05_arguments = zend_mir_w06_calloc(
		records->w05_argument_count, sizeof(*records->w05_arguments));
	records->w05_parameter_modes = zend_mir_w06_calloc(
		records->w05_parameter_mode_count,
		sizeof(*records->w05_parameter_modes));
	if ((records->w05_site_count != 0 && records->w05_sites == NULL)
			|| (records->w05_target_count != 0
				&& records->w05_targets == NULL)
			|| (records->w05_argument_count != 0
				&& records->w05_arguments == NULL)
			|| (records->w05_parameter_mode_count != 0
				&& records->w05_parameter_modes == NULL)) {
		return false;
	}
	for (index = 0; index < records->w05_site_count; index++) {
		zend_mir_source_call_site_ref *site = &records->w05_sites[index];
		bool private_result = false;
		uint32_t use_index;
		if (!source_calls->call_site_at(
				source_calls->context, index, site)
				|| site->id != index) {
			return false;
		}
		if (zend_mir_id_is_valid(site->result_ssa_variable_id)
				&& site->result_ssa_variable_id
					< (uint32_t) ssa->vars_count) {
			bool seen = false;
			private_result = true;
			for (use_index = 0; use_index < op_array->last; use_index++) {
				const zend_ssa_op *ssa_op = &ssa->ops[use_index];
				if (ssa_op->op1_use
							!= (int) site->result_ssa_variable_id
						&& ssa_op->op2_use
							!= (int) site->result_ssa_variable_id
						&& ssa_op->result_use
							!= (int) site->result_ssa_variable_id) {
					continue;
				}
				seen = true;
				if (!zend_mir_w06_private_result_use(
						op_array->opcodes[use_index].opcode)) {
					private_result = false;
					break;
				}
			}
			private_result = private_result && seen;
		}
		if (private_result) {
			site->result_ssa_variable_id = ZEND_MIR_ID_INVALID;
			site->flags &= ~(ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);
			site->flags |= ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED;
		} else if (zend_mir_id_is_valid(site->result_ssa_variable_id)) {
			/*
			 * The frozen W05 prerequisite needs a scalar definition for
			 * nested-call uses. Its private SSA facts have already been
			 * reduced to nullable scalar facts; the original W06 result
			 * category and per-callsite storage remain in the value plan.
			 */
			site->flags &= ~(ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED);
			site->flags |= ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR;
		} else {
			site->flags &= ~(ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR);
			site->flags |= ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED;
		}
	}
	for (index = 0; index < records->w05_target_count; index++) {
		zend_mir_source_call_target_ref *target =
			&records->w05_targets[index];
		if (!source_calls->call_target_at(
				source_calls->context, index, target)
				|| target->id != index
				|| target->kind
					!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
				|| target->variadic) {
			return false;
		}
		target->returns_by_reference = false;
	}
	for (index = 0; index < records->w05_argument_count; index++) {
		zend_mir_source_call_argument_ref *argument =
			&records->w05_arguments[index];
		uint32_t proxy_payload;

		if (!source_calls->call_argument_at(
				source_calls->context, index, argument)
				|| argument->id != index
				|| argument->flags != 0
				|| zend_mir_id_is_valid(argument->name_symbol_id)) {
			return false;
		}
		proxy_payload = ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX
			- records->w05_argument_count + 1 + index;
		argument->value_ssa_variable_id = ZEND_MIR_ID_INVALID;
		argument->source_operand.kind = ZEND_MIR_SOURCE_OPERAND_LITERAL;
		argument->source_operand.slot_kind =
			ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
		argument->source_operand.index = proxy_payload;
		argument->source_operand.ssa_variable_id = ZEND_MIR_ID_INVALID;
		argument->mode = ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE;
	}
	for (index = 0; index < records->w05_parameter_mode_count; index++) {
		zend_mir_source_parameter_mode_ref *mode =
			&records->w05_parameter_modes[index];
		if (!source_calls->parameter_mode_at(
				source_calls->context, index, mode)) {
			return false;
		}
		mode->mode = ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
	}
	memset(&records->w05_calls, 0, sizeof(records->w05_calls));
	records->w05_calls.contract_version = ZEND_MIR_W05_CONTRACT_VERSION;
	records->w05_calls.context = records;
	records->w05_calls.call_site_count = zend_mir_w06_w05_site_count;
	records->w05_calls.call_site_at = zend_mir_w06_w05_site_at;
	records->w05_calls.call_target_count = zend_mir_w06_w05_target_count;
	records->w05_calls.call_target_at = zend_mir_w06_w05_target_at;
	records->w05_calls.call_argument_count = zend_mir_w06_w05_argument_count;
	records->w05_calls.call_argument_at = zend_mir_w06_w05_argument_at;
	records->w05_calls.parameter_mode_count =
		zend_mir_w06_w05_parameter_mode_count;
	records->w05_calls.parameter_mode_at =
		zend_mir_w06_w05_parameter_mode_at;
	records->w05_calls.source_opcode_count =
		zend_mir_w06_w05_source_opcode_count;
	records->w05_calls.source_opcode_at =
		zend_mir_w06_w05_source_opcode_at;
	records->w05_resolver.context = records;
	records->w05_resolver.resolve_exact_direct_user =
		zend_mir_w06_w05_resolve;
	return true;
}

static bool zend_mir_w06_copy_source_opcodes(
	const zend_mir_source_call_view *source_calls,
	zend_mir_w06_records *records)
{
	uint32_t index;
	records->source_opcode_count =
		source_calls->source_opcode_count(source_calls->context);
	records->source_opcodes = zend_mir_w06_calloc(
		records->source_opcode_count, sizeof(*records->source_opcodes));
	if (records->source_opcode_count != 0
			&& records->source_opcodes == NULL) {
		return false;
	}
	for (index = 0; index < records->source_opcode_count; index++) {
		if (!source_calls->source_opcode_at(
				source_calls->context, index,
				&records->source_opcodes[index])) {
			return false;
		}
	}
	return true;
}

static zend_mir_lowering_diagnostic_code zend_mir_w06_literal_code(
	const zval *literal);

static zend_mir_value_category zend_mir_w06_literal_category(
	const zval *literal)
{
	if (literal == NULL) {
		return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	}
	switch (Z_TYPE_P(literal)) {
		case IS_UNDEF:
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
		case IS_LONG:
		case IS_DOUBLE:
			return ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
		case IS_STRING:
			return ZEND_MIR_VALUE_REFCOUNTED_STRING;
		default:
			return ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
	}
}

static zend_mir_lowering_diagnostic_code zend_mir_w06_literal_operand_code(
	const zend_op_array *op_array,
	const zend_mir_source_operand_ref *operand)
{
	const zval *literal;

	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		return ZEND_MIRL_OK;
	}
	if (operand->index >= op_array->last_literal
			|| op_array->literals == NULL) {
		return ZEND_MIRL_INVALID_SOURCE;
	}
	literal = &op_array->literals[operand->index];
	return zend_mir_w06_literal_code(literal);
}

static zend_mir_lowering_diagnostic_code zend_mir_w06_literal_code(
	const zval *literal)
{
	if (literal == NULL) {
		return ZEND_MIRL_INVALID_SOURCE;
	}
	switch (Z_TYPE_P(literal)) {
		case IS_UNDEF:
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
		case IS_LONG:
		case IS_DOUBLE:
		case IS_STRING:
			return ZEND_MIRL_OK;
		case IS_ARRAY:
			return ZEND_MIRL_W06_SEPARATION_DEFERRED;
		case IS_OBJECT:
		case IS_RESOURCE:
		case IS_REFERENCE:
			return ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
		default:
			return ZEND_MIRL_W06_INVALID_OWNERSHIP_TRANSITION;
	}
}

zend_mir_lowering_diagnostic_code zend_mir_w06_preflight_literals(
	const zend_op_array *op_array)
{
	uint32_t index;

	if (op_array == NULL
			|| (op_array->last != 0 && op_array->opcodes == NULL)
			|| (op_array->last_literal != 0 && op_array->literals == NULL)) {
		return ZEND_MIRL_INVALID_SOURCE;
	}
	for (index = 0; index < op_array->last; index++) {
		const zend_op *opline = &op_array->opcodes[index];
		zend_mir_lowering_diagnostic_code code;

		if (!zend_mir_w06_profile_opcode_is_accepted(opline->opcode)) {
			continue;
		}
		if (opline->op1_type == IS_CONST) {
			code = zend_mir_w06_literal_code(
				RT_CONSTANT(opline, opline->op1));
			if (code != ZEND_MIRL_OK) {
				return code;
			}
		}
		if (opline->op2_type == IS_CONST) {
			code = zend_mir_w06_literal_code(
				RT_CONSTANT(opline, opline->op2));
			if (code != ZEND_MIRL_OK) {
				return code;
			}
		}
	}
	return ZEND_MIRL_OK;
}

static zend_mir_lowering_diagnostic_code zend_mir_w06_validate_literals(
	const zend_op_array *op_array, const zend_mir_w06_records *records)
{
	uint32_t index;

	for (index = 0; index < records->source_opcode_count; index++) {
		const zend_mir_source_opcode_ref *opcode =
			&records->source_opcodes[index];
		zend_mir_lowering_diagnostic_code code;

		if (!zend_mir_w06_profile_opcode_is_accepted(
				opcode->zend_opcode_number)) {
			continue;
		}
		code = zend_mir_w06_literal_operand_code(op_array, &opcode->op1);
		if (code != ZEND_MIRL_OK) {
			return code;
		}
		code = zend_mir_w06_literal_operand_code(op_array, &opcode->op2);
		if (code != ZEND_MIRL_OK) {
			return code;
		}
	}
	return ZEND_MIRL_OK;
}

static bool zend_mir_w06_build_storages(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_zend_source *zend_source,
	const zend_mir_source_call_view *source_calls,
	zend_mir_w06_records *records,
	const uint32_t *reference_origins,
	const uint32_t *reference_ids,
	const uint32_t *creation_sources)
{
	uint32_t base_count =
		(uint32_t) op_array->last_var + op_array->T;
	uint32_t local_reference_count = 0;
	uint32_t return_reference_count = 0;
	uint32_t literal_argument_count = 0;
	uint32_t argument_count =
		source_calls->call_argument_count(source_calls->context);
	uint32_t site_count =
		source_calls->call_site_count(source_calls->context);
	uint32_t local_payload_base;
	uint32_t call_return_base;
	uint32_t call_reference_payload_base;
	uint32_t literal_argument_base;
	uint32_t literal_payload_base;
	uint32_t index;
	uint32_t reference_index = 0;

	for (index = 0; index < base_count; index++) {
		if (zend_mir_id_is_valid(reference_ids[index])) {
			if (reference_ids[index] != local_reference_count
					|| !zend_mir_id_is_valid(creation_sources[index])) {
				return false;
			}
			local_reference_count++;
		}
	}
	for (index = 0; index < site_count; index++) {
		zend_mir_source_call_site_ref site;
		zend_mir_source_call_target_ref target;
		if (!source_calls->call_site_at(source_calls->context, index, &site)
				|| site.id != index
				|| !source_calls->call_target_at(
					source_calls->context, site.target_id, &target)) {
			return false;
		}
		if (target.returns_by_reference) {
			return_reference_count++;
		}
	}
	for (index = 0; index < argument_count; index++) {
		zend_mir_source_call_argument_ref argument;
		if (!source_calls->call_argument_at(
				source_calls->context, index, &argument)
				|| argument.id != index) {
			return false;
		}
		if (argument.source_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			literal_argument_count++;
		}
	}
	if (base_count > ZEND_MIR_W06_LIMIT - local_reference_count
			|| base_count + local_reference_count
				> ZEND_MIR_W06_LIMIT - site_count
			|| base_count + local_reference_count + site_count
				> ZEND_MIR_W06_LIMIT - return_reference_count
			|| base_count + local_reference_count + site_count
					+ return_reference_count
				> ZEND_MIR_W06_LIMIT - literal_argument_count
			|| base_count > ZEND_MIR_W06_LIMIT - site_count
			|| base_count + site_count
				> ZEND_MIR_W06_LIMIT - literal_argument_count) {
		return false;
	}
	local_payload_base = base_count;
	call_return_base = local_payload_base + local_reference_count;
	call_reference_payload_base = call_return_base + site_count;
	literal_argument_base =
		call_reference_payload_base + return_reference_count;
	literal_payload_base = base_count + site_count;
	records->source_storage_count =
		literal_argument_base + literal_argument_count;
	records->storage_count = records->source_storage_count;
	records->payload_count =
		literal_payload_base + literal_argument_count;
	records->source_reference_count =
		local_reference_count + return_reference_count;
	records->reference_count = records->source_reference_count;
	records->source_storages = zend_mir_w06_calloc(
		records->source_storage_count, sizeof(*records->source_storages));
	records->storages = zend_mir_w06_calloc(
		records->storage_count, sizeof(*records->storages));
	records->payloads = zend_mir_w06_calloc(
		records->payload_count, sizeof(*records->payloads));
	records->source_references = zend_mir_w06_calloc(
		records->reference_count, sizeof(*records->source_references));
	records->references = zend_mir_w06_calloc(
		records->reference_count, sizeof(*records->references));
	records->call_return_storage_ids = zend_mir_w06_calloc(
		site_count, sizeof(*records->call_return_storage_ids));
	records->call_argument_storage_ids = zend_mir_w06_calloc(
		argument_count, sizeof(*records->call_argument_storage_ids));
	if ((records->source_storage_count != 0
				&& (records->source_storages == NULL
					|| records->storages == NULL))
			|| (records->payload_count != 0 && records->payloads == NULL)
			|| (records->reference_count != 0
				&& (records->source_references == NULL
					|| records->references == NULL))
			|| (site_count != 0
				&& records->call_return_storage_ids == NULL)
			|| (argument_count != 0
				&& records->call_argument_storage_ids == NULL)) {
		return false;
	}
	for (index = 0; index < argument_count; index++) {
		records->call_argument_storage_ids[index] = ZEND_MIR_ID_INVALID;
	}
	for (index = 0; index < base_count; index++) {
		zend_mir_source_storage_ref *source =
			&records->source_storages[index];
		zend_mir_storage_ref *storage = &records->storages[index];
		zend_mir_payload_ref *payload = &records->payloads[index];
		bool reference =
			zend_mir_id_is_valid(reference_origins[index]);
		zend_mir_value_category category =
			zend_mir_w06_storage_category(ssa, index);

		if (reference
				&& (reference_origins[index] >= base_count
					|| !zend_mir_id_is_valid(
						reference_ids[reference_origins[index]]))) {
			return false;
		}
		if (category == ZEND_MIR_VALUE_CATEGORY_UNKNOWN) {
			category = zend_mir_w06_storage_call_return_category(
				ssa, zend_source, source_calls, index);
		}
		source->id = index;
		source->slot_kind = index < (uint32_t) op_array->last_var
			? ZEND_MIR_SOURCE_SLOT_CV : ZEND_MIR_SOURCE_SLOT_TMP;
		source->slot_index = index < (uint32_t) op_array->last_var
			? index : index - (uint32_t) op_array->last_var;
		source->ssa_variable_id =
			zend_mir_w06_storage_ssa_variable(ssa, index);
		source->kind = ZEND_MIR_STORAGE_FRAME_SLOT;
		source->state = reference
			? ZEND_MIR_STORAGE_REFERENCE : ZEND_MIR_STORAGE_DIRECT;
		source->category = reference
			? ZEND_MIR_VALUE_REFERENCE_CELL
			: category;
		source->provenance = ZEND_MIR_SOURCE_VALUE_LOCAL;

		memset(storage, 0, sizeof(*storage));
		storage->id = index;
		storage->kind = ZEND_MIR_STORAGE_FRAME_SLOT;
		storage->state = source->state;
		storage->category = source->category;
		storage->payload_id = reference
			? ZEND_MIR_ID_INVALID : index;
		storage->reference_cell_id = reference
			? reference_ids[reference_origins[index]]
			: ZEND_MIR_ID_INVALID;
		storage->indirect_target_id = ZEND_MIR_ID_INVALID;

		payload->id = index;
		payload->category = category;
		payload->refcount_state =
			category == ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				? ZEND_MIR_REFCOUNT_IMMORTAL
				: ZEND_MIR_REFCOUNT_UNKNOWN;
		payload->cleanup_obligation =
			category != ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
	}
	for (index = 0; index < base_count; index++) {
		uint32_t payload_storage_id;
		uint32_t reference_id = reference_ids[index];
		zend_mir_source_storage_ref *payload_source;
		zend_mir_storage_ref *payload_storage;
		zend_mir_source_reference_ref *source_reference;
		zend_mir_reference_cell_ref *reference_record;

		if (!zend_mir_id_is_valid(reference_id)) {
			continue;
		}
		payload_storage_id = local_payload_base + reference_id;
		payload_source = &records->source_storages[payload_storage_id];
		payload_storage = &records->storages[payload_storage_id];
		source_reference = &records->source_references[reference_id];
		reference_record = &records->references[reference_id];

		*payload_source = records->source_storages[index];
		payload_source->id = payload_storage_id;
		payload_source->kind =
			ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT;
		payload_source->state = ZEND_MIR_STORAGE_DIRECT;
		payload_source->category = records->payloads[index].category;

		memset(payload_storage, 0, sizeof(*payload_storage));
		payload_storage->id = payload_storage_id;
		payload_storage->kind =
			ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT;
		payload_storage->state = ZEND_MIR_STORAGE_DIRECT;
		payload_storage->category = records->payloads[index].category;
		payload_storage->payload_id = index;
		payload_storage->reference_cell_id = ZEND_MIR_ID_INVALID;
		payload_storage->indirect_target_id = ZEND_MIR_ID_INVALID;

		source_reference->id = reference_id;
		source_reference->payload_storage_id = payload_storage_id;
		source_reference->alias_class_id = reference_id;
		source_reference->creation_opline_index =
			creation_sources[index];
		source_reference->ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
		source_reference->cleanup_obligation = true;

		reference_record->id = reference_id;
		reference_record->payload_storage_id = payload_storage_id;
		reference_record->alias_class_id = reference_id;
		reference_record->creation_source_id = creation_sources[index];
		reference_record->ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
		reference_record->cleanup_obligation = true;
		reference_index++;
	}
	if (reference_index != local_reference_count) {
		return false;
	}
	for (index = 0; index < site_count; index++) {
		zend_mir_source_call_site_ref site;
		zend_mir_source_call_target_ref target;
		zend_mir_source_storage_ref *source;
		zend_mir_storage_ref *storage;
		zend_mir_payload_ref *payload;
		zend_mir_storage_id storage_id = call_return_base + index;
		zend_mir_payload_id payload_id = base_count + index;
		zend_mir_value_category category;
		bool reference;

		if (!source_calls->call_site_at(source_calls->context, index, &site)
				|| !source_calls->call_target_at(
					source_calls->context, site.target_id, &target)) {
			return false;
		}
		category = zend_mir_id_is_valid(site.result_ssa_variable_id)
			? zend_mir_w06_ssa_category(
				ssa, site.result_ssa_variable_id)
			: ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
		if (category == ZEND_MIR_VALUE_CATEGORY_UNKNOWN) {
			uint32_t return_type;
			if (zend_mir_zend_source_w06_call_return_type(
					zend_source, site.target_id, &return_type)) {
				category = zend_mir_w06_type_category(return_type);
			}
		}
		if (category != ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				&& category != ZEND_MIR_VALUE_REFCOUNTED_STRING) {
			return false;
		}
		reference = target.returns_by_reference;
		records->call_return_storage_ids[index] = storage_id;
		source = &records->source_storages[storage_id];
		source->id = storage_id;
		source->slot_kind = ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
		source->slot_index = site.id;
		source->ssa_variable_id = site.result_ssa_variable_id;
		source->kind = ZEND_MIR_STORAGE_CALL_RETURN_SLOT;
		source->state = reference
			? ZEND_MIR_STORAGE_REFERENCE : ZEND_MIR_STORAGE_DIRECT;
		source->category = reference
			? ZEND_MIR_VALUE_REFERENCE_CELL : category;
		source->provenance = ZEND_MIR_SOURCE_VALUE_CALL_RESULT;

		storage = &records->storages[storage_id];
		storage->id = storage_id;
		storage->kind = ZEND_MIR_STORAGE_CALL_RETURN_SLOT;
		storage->state = source->state;
		storage->category = source->category;
		storage->payload_id =
			reference ? ZEND_MIR_ID_INVALID : payload_id;
		storage->reference_cell_id =
			reference ? reference_index : ZEND_MIR_ID_INVALID;
		storage->indirect_target_id = ZEND_MIR_ID_INVALID;

		payload = &records->payloads[payload_id];
		payload->id = payload_id;
		payload->category = category;
		payload->refcount_state =
			category == ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				? ZEND_MIR_REFCOUNT_IMMORTAL
				: ZEND_MIR_REFCOUNT_UNKNOWN;
		payload->cleanup_obligation =
			category != ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;

		if (reference) {
			uint32_t return_reference_index =
				reference_index - local_reference_count;
			uint32_t payload_storage_id =
				call_reference_payload_base + return_reference_index;
			zend_mir_source_storage_ref *payload_source =
				&records->source_storages[payload_storage_id];
			zend_mir_storage_ref *payload_storage =
				&records->storages[payload_storage_id];
			zend_mir_source_reference_ref *source_reference =
				&records->source_references[reference_index];
			zend_mir_reference_cell_ref *reference_record =
				&records->references[reference_index];

			*payload_source = *source;
			payload_source->id = payload_storage_id;
			payload_source->kind =
				ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT;
			payload_source->state = ZEND_MIR_STORAGE_DIRECT;
			payload_source->category = category;
			payload_storage->id = payload_storage_id;
			payload_storage->kind =
				ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT;
			payload_storage->state = ZEND_MIR_STORAGE_DIRECT;
			payload_storage->category = category;
			payload_storage->payload_id = payload_id;
			payload_storage->reference_cell_id = ZEND_MIR_ID_INVALID;
			payload_storage->indirect_target_id = ZEND_MIR_ID_INVALID;
			source_reference->id = reference_index;
			source_reference->payload_storage_id = payload_storage_id;
			source_reference->alias_class_id = reference_index;
			source_reference->creation_opline_index =
				site.do_opline_index;
			source_reference->ownership =
				ZEND_MIR_OWNERSHIP_STATE_OWNED;
			source_reference->cleanup_obligation = true;
			reference_record->id = reference_index;
			reference_record->payload_storage_id = payload_storage_id;
			reference_record->alias_class_id = reference_index;
			reference_record->creation_source_id =
				site.do_opline_index;
			reference_record->ownership =
				ZEND_MIR_OWNERSHIP_STATE_OWNED;
			reference_record->cleanup_obligation = true;
			reference_index++;
		}
		if (zend_mir_id_is_valid(site.result_ssa_variable_id)
				&& site.result_ssa_variable_id
					< (uint32_t) ssa->vars_count
				&& ssa->vars[site.result_ssa_variable_id].var >= 0
				&& (uint32_t) ssa->vars[
					site.result_ssa_variable_id].var < base_count) {
			uint32_t physical_storage =
				(uint32_t) ssa->vars[
					site.result_ssa_variable_id].var;
			/*
			 * A physical TMP may be reused by nested calls. Bind its
			 * snapshot only to the latest SSA representative; earlier
			 * results remain addressable through their call-return slots.
			 */
			if (zend_mir_w06_storage_ssa_variable(
					ssa, physical_storage)
					== site.result_ssa_variable_id) {
				zend_mir_source_storage_ref *physical_source =
					&records->source_storages[physical_storage];
				zend_mir_storage_ref *physical =
					&records->storages[physical_storage];
				physical_source->state = source->state;
				physical_source->category = source->category;
				physical->state = storage->state;
				physical->category = storage->category;
				physical->payload_id = storage->payload_id;
				physical->reference_cell_id =
					storage->reference_cell_id;
				physical->indirect_target_id =
					ZEND_MIR_ID_INVALID;
			}
		}
	}
	if (reference_index != records->reference_count) {
		return false;
	}
	{
		uint32_t literal_index = 0;
		for (index = 0; index < argument_count; index++) {
			zend_mir_source_call_argument_ref argument;
			uint32_t physical_storage;
			uint32_t site_index;
			if (!source_calls->call_argument_at(
					source_calls->context, index, &argument)) {
				return false;
			}
			if (zend_mir_id_is_valid(
					argument.value_ssa_variable_id)) {
				for (site_index = 0; site_index < site_count;
						site_index++) {
					zend_mir_source_call_site_ref producer;
					if (!source_calls->call_site_at(
							source_calls->context,
							site_index, &producer)) {
						return false;
					}
					if (producer.result_ssa_variable_id
							== argument.value_ssa_variable_id) {
						records->call_argument_storage_ids[index] =
							records->call_return_storage_ids[
								producer.id];
						break;
					}
				}
				if (site_index != site_count) {
					continue;
				}
			}
			if (zend_mir_w06_operand_storage(
					op_array, &argument.source_operand,
					&physical_storage)) {
				if (physical_storage >= base_count) {
					return false;
				}
				records->call_argument_storage_ids[index] =
					physical_storage;
				continue;
			}
			if (argument.source_operand.kind
					!= ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| argument.source_operand.index
						>= op_array->last_literal) {
				return false;
			}
			{
				zend_mir_storage_id storage_id =
					literal_argument_base + literal_index;
				zend_mir_payload_id payload_id =
					literal_payload_base + literal_index;
				zend_mir_value_category category =
					zend_mir_w06_literal_category(
						&op_array->literals[
							argument.source_operand.index]);
				zend_mir_source_storage_ref *source =
					&records->source_storages[storage_id];
				zend_mir_storage_ref *storage =
					&records->storages[storage_id];
				zend_mir_payload_ref *payload =
					&records->payloads[payload_id];
				if (category == ZEND_MIR_VALUE_CATEGORY_UNKNOWN) {
					return false;
				}
				source->id = storage_id;
				source->slot_kind =
					ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
				source->slot_index = argument.id;
				source->ssa_variable_id =
					ZEND_MIR_ID_INVALID;
				source->kind =
					ZEND_MIR_STORAGE_CALL_ARGUMENT_SLOT;
				source->state = ZEND_MIR_STORAGE_DIRECT;
				source->category = category;
				source->provenance =
					ZEND_MIR_SOURCE_VALUE_CALL_ARGUMENT;
				storage->id = storage_id;
				storage->kind =
					ZEND_MIR_STORAGE_CALL_ARGUMENT_SLOT;
				storage->state = ZEND_MIR_STORAGE_DIRECT;
				storage->category = category;
				storage->payload_id = payload_id;
				storage->reference_cell_id =
					ZEND_MIR_ID_INVALID;
				storage->indirect_target_id =
					ZEND_MIR_ID_INVALID;
				payload->id = payload_id;
				payload->category = category;
				payload->refcount_state =
					category
						== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
					? ZEND_MIR_REFCOUNT_IMMORTAL
					: ZEND_MIR_REFCOUNT_UNKNOWN;
				payload->cleanup_obligation =
					category
						!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
				records->call_argument_storage_ids[index] =
					storage_id;
				literal_index++;
			}
		}
		if (literal_index != literal_argument_count) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w06_build_aliases(zend_mir_w06_records *records)
{
	uint32_t index;

	records->alias_count = records->reference_count;
	records->aliases = zend_mir_w06_calloc(
		records->alias_count, sizeof(*records->aliases));
	if (records->alias_count != 0 && records->aliases == NULL) {
		return false;
	}
	for (index = 0; index < records->reference_count; index++) {
		records->aliases[index].left_id =
			records->references[index].alias_class_id;
		records->aliases[index].right_id =
			records->references[index].alias_class_id;
		records->aliases[index].relation = ZEND_MIR_ALIAS_MUST;
		records->aliases[index].proof_id =
			records->references[index].creation_source_id;
	}
	return true;
}

static bool zend_mir_w06_build_events(
	const zend_op_array *op_array, zend_mir_w06_records *records)
{
	uint32_t index;
	uint32_t count = 0;

	for (index = 0; index < records->source_opcode_count; index++) {
		const zend_mir_source_opcode_ref *opcode =
			&records->source_opcodes[index];
		uint32_t source_id;
		if ((opcode->zend_opcode_number == ZEND_UNSET_CV
					|| opcode->zend_opcode_number == ZEND_FREE
					|| opcode->zend_opcode_number == ZEND_RETURN
					|| opcode->zend_opcode_number == ZEND_COPY_TMP)
				&& zend_mir_w06_operand_storage(
					op_array, &opcode->op1, &source_id)
				&& source_id < records->storage_count
				&& records->storages[source_id].state
					== ZEND_MIR_STORAGE_DIRECT
				&& records->storages[source_id].category
					== ZEND_MIR_VALUE_REFCOUNTED_STRING) {
			count++;
		}
	}
	records->event_count = count;
	records->events = zend_mir_w06_calloc(
		count, sizeof(*records->events));
	if (count != 0 && records->events == NULL) {
		return false;
	}
	count = 0;
	for (index = 0; index < records->source_opcode_count; index++) {
		const zend_mir_source_opcode_ref *opcode =
			&records->source_opcodes[index];
		zend_mir_ownership_event_ref *event;
		zend_mir_storage_ref *storage;
		zend_mir_payload_ref *payload;
		uint32_t storage_id;
		uint32_t target_id = ZEND_MIR_ID_INVALID;
		zend_mir_transfer_action action;

		if (opcode->zend_opcode_number != ZEND_UNSET_CV
				&& opcode->zend_opcode_number != ZEND_FREE
				&& opcode->zend_opcode_number != ZEND_RETURN
				&& opcode->zend_opcode_number != ZEND_COPY_TMP) {
			continue;
		}
		if (!zend_mir_w06_operand_storage(
				op_array, &opcode->op1, &storage_id)
				|| storage_id >= records->storage_count) {
			return false;
		}
		storage = &records->storages[storage_id];
		if (storage->state != ZEND_MIR_STORAGE_DIRECT
				|| storage->category
					!= ZEND_MIR_VALUE_REFCOUNTED_STRING) {
			continue;
		}
		if (storage->payload_id >= records->payload_count) {
			return false;
		}
		payload = &records->payloads[storage->payload_id];
		switch (opcode->zend_opcode_number) {
			case ZEND_COPY_TMP:
				if (!zend_mir_w06_operand_storage(
						op_array, &opcode->result, &target_id)
						|| target_id >= records->storage_count) {
					return false;
				}
				action = ZEND_MIR_TRANSFER_COPY_ADDREF;
				break;
			case ZEND_RETURN:
				action = ZEND_MIR_TRANSFER_TO_CALLEE;
				break;
			default:
				action = ZEND_MIR_TRANSFER_RELEASE;
				break;
		}
		event = &records->events[count];
		event->id = count++;
		event->source_storage_id = storage_id;
		event->target_storage_id = target_id;
		event->payload_id = storage->payload_id;
		event->action = action;
		event->before_state = payload->refcount_state;
		event->after_state = payload->refcount_state;
		event->cleanup_obligation = true;
	}
	return true;
}

static bool zend_mir_w06_build_plan_entries(
	const zend_op_array *op_array, zend_mir_w06_records *records)
{
	uint32_t index;
	uint32_t output = 0;
	uint32_t event_offset = 0;
	uint32_t separation_offset = 0;
	for (index = 0; index < records->source_opcode_count; index++) {
		if (zend_mir_w06_profile_opcode_is_accepted(
				records->source_opcodes[index].zend_opcode_number)) {
			output++;
		}
	}
	records->entry_count = output;
	records->entries = zend_mir_w06_calloc(
		output, sizeof(*records->entries));
	if (output != 0 && records->entries == NULL) {
		return false;
	}
	output = 0;
	for (index = 0; index < records->source_opcode_count; index++) {
		zend_mir_source_opcode_ref *opcode = &records->source_opcodes[index];
		zend_mir_value_plan_entry *entry;
		if (!zend_mir_w06_profile_opcode_is_accepted(
				opcode->zend_opcode_number)) {
			continue;
		}
		entry = &records->entries[output++];
		entry->source_opline_index = opcode->opline_index;
		entry->decision = ZEND_MIR_VALUE_PLAN_ACCEPTED;
		entry->diagnostic_code = ZEND_MIRL_OK;
		entry->storage_span.offset = 0;
		entry->storage_span.count = records->storage_count;
		entry->ownership_event_span.offset = event_offset;
		entry->ownership_event_span.count = 0;
		if (opcode->zend_opcode_number == ZEND_UNSET_CV
				|| opcode->zend_opcode_number == ZEND_FREE
				|| opcode->zend_opcode_number == ZEND_RETURN
				|| opcode->zend_opcode_number == ZEND_COPY_TMP) {
			uint32_t storage_id;
			if (!zend_mir_w06_operand_storage(
					op_array, &opcode->op1, &storage_id)
					|| storage_id >= records->storage_count) {
				return false;
			}
			if (records->storages[storage_id].state
						== ZEND_MIR_STORAGE_DIRECT
					&& records->storages[storage_id].category
						== ZEND_MIR_VALUE_REFCOUNTED_STRING) {
				entry->ownership_event_span.count = 1;
			}
		}
		event_offset += entry->ownership_event_span.count;
		entry->separation_plan_span.offset = separation_offset;
		if (opcode->zend_opcode_number == ZEND_SEPARATE) {
			entry->separation_plan_span.count = 1;
		}
		separation_offset += entry->separation_plan_span.count;
	}
	return event_offset == records->event_count
		&& separation_offset == records->separation_count;
}

static bool zend_mir_w06_build_separations(
	const zend_op_array *op_array, zend_mir_w06_records *records)
{
	uint32_t index;
	uint32_t output = 0;
	for (index = 0; index < records->source_opcode_count; index++) {
		if (records->source_opcodes[index].zend_opcode_number
				== ZEND_SEPARATE) {
			output++;
		}
	}
	records->separation_count = output;
	records->separations = zend_mir_w06_calloc(
		output, sizeof(*records->separations));
	if (output != 0 && records->separations == NULL) {
		return false;
	}
	output = 0;
	for (index = 0; index < records->source_opcode_count; index++) {
		const zend_mir_source_opcode_ref *opcode =
			&records->source_opcodes[index];
		zend_mir_separation_plan_ref *plan;
		uint32_t storage_id;
		if (opcode->zend_opcode_number != ZEND_SEPARATE) {
			continue;
		}
		if (!zend_mir_w06_operand_storage(
				op_array, &opcode->op1, &storage_id)) {
			return false;
		}
		if (records->storages[storage_id].state
				== ZEND_MIR_STORAGE_REFERENCE) {
			storage_id = records->references[
				records->storages[storage_id].reference_cell_id]
					.payload_storage_id;
		}
		plan = &records->separations[output];
		plan->id = output++;
		plan->source_storage_id = storage_id;
		plan->source_payload_id = records->storages[storage_id].payload_id;
		plan->reason = ZEND_MIR_SEPARATION_EXPLICIT;
		plan->uniqueness_fact = ZEND_MIR_REFCOUNT_UNKNOWN;
		plan->required = ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN;
		plan->result_payload_id = ZEND_MIR_ID_INVALID;
		plan->clone_execution_required = true;
	}
	return true;
}

static bool zend_mir_w06_build_transfers(
	const zend_op_array *op_array,
	const zend_mir_source_call_view *source_calls,
	zend_mir_w06_records *records)
{
	uint32_t count =
		source_calls->call_argument_count(source_calls->context);
	uint32_t index;

	records->transfer_count = count;
	records->transfers = zend_mir_w06_calloc(
		count, sizeof(*records->transfers));
	if (count != 0 && records->transfers == NULL) {
		return false;
	}
	for (index = 0; index < count; index++) {
		zend_mir_source_call_argument_ref argument;
		zend_mir_source_call_site_ref site;
		zend_mir_source_call_target_ref target;
		zend_mir_source_parameter_mode_ref parameter_mode;
		zend_mir_call_transfer_ref *transfer = &records->transfers[index];
		uint32_t argument_storage;
		uint32_t return_storage;
		if (!source_calls->call_argument_at(
				source_calls->context, index, &argument)
				|| !source_calls->call_site_at(
					source_calls->context, argument.call_site_id, &site)
				|| !source_calls->call_target_at(
					source_calls->context, site.target_id, &target)
				|| target.kind
					!= ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER
				|| target.variadic
				|| source_calls->parameter_mode_count == NULL
				|| source_calls->parameter_mode_at == NULL
				|| argument.ordinal >= target.parameter_modes.count
				|| target.parameter_modes.offset
					> source_calls->parameter_mode_count(
						source_calls->context)
				|| target.parameter_modes.count
					> source_calls->parameter_mode_count(
						source_calls->context)
						- target.parameter_modes.offset
				|| !source_calls->parameter_mode_at(
					source_calls->context,
					target.parameter_modes.offset + argument.ordinal,
					&parameter_mode)
				|| parameter_mode.target_id != target.id
				|| parameter_mode.ordinal != argument.ordinal
				|| argument.id >= records->w05_argument_count
				|| records->call_argument_storage_ids == NULL
				|| site.id >= records->w05_site_count
				|| records->call_return_storage_ids == NULL) {
			return false;
		}
		argument_storage =
			records->call_argument_storage_ids[argument.id];
		if (argument_storage >= records->storage_count) {
			return false;
		}
		if ((parameter_mode.mode == ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE)
				!= (argument.mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE)) {
			return false;
		}
		return_storage = records->call_return_storage_ids[site.id];
		if (return_storage >= records->storage_count
				|| records->storages[return_storage].kind
					!= ZEND_MIR_STORAGE_CALL_RETURN_SLOT) {
			return false;
		}
		transfer->call_site_id = site.id;
		transfer->parameter_modes = target.parameter_modes;
		transfer->argument_ordinal = argument.ordinal;
		transfer->argument_storage_id = argument_storage;
		transfer->argument_reference_cell_id =
			records->storages[argument_storage].state
				== ZEND_MIR_STORAGE_REFERENCE
			? records->storages[argument_storage].reference_cell_id
			: ZEND_MIR_ID_INVALID;
		if (parameter_mode.mode == ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
				&& !zend_mir_id_is_valid(
					transfer->argument_reference_cell_id)) {
			return false;
		}
		transfer->argument_action =
			parameter_mode.mode == ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE
			? ZEND_MIR_TRANSFER_TO_CALLEE
			: records->storages[argument_storage].category
					== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				? ZEND_MIR_TRANSFER_BORROW
				: ZEND_MIR_TRANSFER_COPY_ADDREF;
		transfer->return_storage_id = return_storage;
		transfer->return_reference_cell_id =
			records->storages[return_storage].state
				== ZEND_MIR_STORAGE_REFERENCE
			? records->storages[return_storage].reference_cell_id
			: ZEND_MIR_ID_INVALID;
		if (target.returns_by_reference
				!= zend_mir_id_is_valid(
					transfer->return_reference_cell_id)) {
			return false;
		}
		transfer->return_action = ZEND_MIR_TRANSFER_FROM_CALLEE;
	}
	return true;
}

zend_mir_lowering_diagnostic_code zend_mir_w06_build_value_snapshot(
	const zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	const zend_mir_zend_source *zend_source,
	const zend_mir_source_call_view *source_calls,
	zend_mir_w06_value_snapshot *snapshot)
{
	zend_mir_w06_records *records;
	uint32_t *reference_origins;
	uint32_t *reference_ids;
	uint32_t *creation_sources;
	uint32_t base_count;
	uint32_t index;

	if (snapshot == NULL) {
		return ZEND_MIRL_INVALID_SOURCE;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	if (op_array == NULL || ssa == NULL || zend_source == NULL
			|| !zend_source->w05 || source_calls == NULL
			|| source_calls->contract_version != ZEND_MIR_W05_CONTRACT_VERSION
			|| source_calls->source_opcode_count == NULL
			|| source_calls->source_opcode_at == NULL
			|| source_calls->call_site_at == NULL
			|| source_calls->call_target_at == NULL
			|| source_calls->call_argument_count == NULL
			|| source_calls->call_argument_at == NULL
			|| op_array->last_var < 0
			|| op_array->T > ZEND_MIR_W06_LIMIT
				- (uint32_t) op_array->last_var) {
		return ZEND_MIRL_INVALID_SOURCE;
	}
	if (zend_mir_w06_fault_is(ZEND_MIR_W06_TEST_FAULT_INVENTORY)) {
		return ZEND_MIRL_W06_INVALID_STORAGE;
	}
	base_count = (uint32_t) op_array->last_var + op_array->T;
	records = calloc(1, sizeof(*records));
	reference_origins = zend_mir_w06_calloc(
		base_count, sizeof(*reference_origins));
	reference_ids = zend_mir_w06_calloc(
		base_count, sizeof(*reference_ids));
	creation_sources = zend_mir_w06_calloc(
		base_count, sizeof(*creation_sources));
	if (records == NULL || (base_count != 0
			&& (reference_origins == NULL || reference_ids == NULL
				|| creation_sources == NULL))) {
		free(reference_origins);
		free(reference_ids);
		free(creation_sources);
		zend_mir_w06_release_records(records);
		return ZEND_MIRL_W05_CALL_PLAN_FAILED;
	}
	for (index = 0; index < base_count; index++) {
		reference_origins[index] = ZEND_MIR_ID_INVALID;
		reference_ids[index] = ZEND_MIR_ID_INVALID;
		creation_sources[index] = ZEND_MIR_ID_INVALID;
	}
	if (!zend_mir_w06_copy_source_opcodes(source_calls, records)) {
		goto invalid;
	}
	{
		zend_mir_lowering_diagnostic_code literal_code =
			zend_mir_w06_validate_literals(op_array, records);
		if (literal_code != ZEND_MIRL_OK) {
			free(reference_origins);
			free(reference_ids);
			free(creation_sources);
			zend_mir_w06_release_records(records);
			return literal_code;
		}
	}
	if (!zend_mir_w06_build_w05_projection(
			op_array, ssa, source_calls, records)) {
		goto invalid;
	}
	{
		zend_mir_lowering_diagnostic_code transition_code =
			zend_mir_w06_preflight_transitions(
				op_array, ssa, records);
		if (transition_code != ZEND_MIRL_OK) {
			free(reference_origins);
			free(reference_ids);
			free(creation_sources);
			zend_mir_w06_release_records(records);
			return transition_code;
		}
	}
	{
		zend_mir_lowering_diagnostic_code return_code =
			zend_mir_w06_preflight_call_returns(
				ssa, zend_source, source_calls);
		if (return_code != ZEND_MIRL_OK) {
			free(reference_origins);
			free(reference_ids);
			free(creation_sources);
			zend_mir_w06_release_records(records);
			return return_code;
		}
	}
	if (!zend_mir_w06_inventory_parameters(
			op_array, records, reference_origins, creation_sources)) {
		goto invalid;
	}
	for (index = 0; index < records->source_opcode_count; index++) {
		if (!zend_mir_w06_inventory_opcode(
				op_array, &records->source_opcodes[index],
				reference_origins, creation_sources)) {
			goto invalid;
		}
	}
	if (!zend_mir_w06_finalize_reference_ids(
			base_count, reference_origins, creation_sources,
			reference_ids)) {
		goto invalid;
	}
	if (zend_mir_w06_fault_is(ZEND_MIR_W06_TEST_FAULT_PLAN)
			|| !zend_mir_w06_build_storages(
				op_array, ssa, zend_source, source_calls, records,
				reference_origins, reference_ids, creation_sources)
			|| !zend_mir_w06_build_aliases(records)
			|| !zend_mir_w06_build_events(op_array, records)
			|| !zend_mir_w06_build_separations(op_array, records)
			|| !zend_mir_w06_build_transfers(
				op_array, source_calls, records)
			|| !zend_mir_w06_build_plan_entries(op_array, records)) {
		goto invalid;
	}
	free(reference_origins);
	free(reference_ids);
	free(creation_sources);
	records->magic = ZEND_MIR_W06_SNAPSHOT_MAGIC;
	snapshot->records = records;
	snapshot->source_view.contract_version = ZEND_MIR_W06_CONTRACT_VERSION;
	snapshot->source_view.context = records;
	snapshot->source_view.storage_count =
		zend_mir_w06_source_storage_count;
	snapshot->source_view.storage_at = zend_mir_w06_source_storage_at;
	snapshot->source_view.reference_count =
		zend_mir_w06_source_reference_count;
	snapshot->source_view.reference_at =
		zend_mir_w06_source_reference_at;
	snapshot->source_view.indirect_count =
		zend_mir_w06_source_indirect_count;
	snapshot->source_view.indirect_at = zend_mir_w06_source_indirect_at;
	snapshot->source_view.opcode_count = zend_mir_w06_source_opcode_count;
	snapshot->source_view.opcode_at = zend_mir_w06_source_opcode_at;
	snapshot->plan.entries = records->entries;
	snapshot->plan.count = records->entry_count;
	snapshot->plan.complete = true;
	snapshot->plan.immutable = true;
	return ZEND_MIRL_OK;

invalid:
	free(reference_origins);
	free(reference_ids);
	free(creation_sources);
	zend_mir_w06_release_records(records);
	return ZEND_MIRL_W06_INVALID_STORAGE;
}

void zend_mir_w06_release_value_snapshot(
	zend_mir_w06_value_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	zend_mir_w06_release_records(snapshot->records);
	memset(snapshot, 0, sizeof(*snapshot));
}

static bool zend_mir_w06_emit_records(
	const zend_mir_w06_records *records,
	zend_mir_value_mutator *mutator)
{
	uint32_t index;
#define W06_EMIT(count_field, array_field, callback, fault) \
	if (zend_mir_w06_fault_is(fault)) { \
		return false; \
	} \
	for (index = 0; index < records->count_field; index++) { \
		if (!mutator->callback( \
					mutator->context, &records->array_field[index])) { \
			return false; \
		} \
	}
	W06_EMIT(storage_count, storages, add_storage,
		ZEND_MIR_W06_TEST_FAULT_STORAGE)
	W06_EMIT(payload_count, payloads, add_payload,
		ZEND_MIR_W06_TEST_FAULT_STORAGE)
	W06_EMIT(reference_count, references, add_reference_cell,
		ZEND_MIR_W06_TEST_FAULT_REFERENCE_CELL)
	W06_EMIT(alias_count, aliases, add_alias_relation,
		ZEND_MIR_W06_TEST_FAULT_ALIAS)
	W06_EMIT(event_count, events, add_ownership_event,
		ZEND_MIR_W06_TEST_FAULT_EVENT)
	W06_EMIT(separation_count, separations, add_separation_plan,
		ZEND_MIR_W06_TEST_FAULT_SEPARATION)
	W06_EMIT(transfer_count, transfers, add_call_transfer,
		ZEND_MIR_W06_TEST_FAULT_CALL_TRANSFER)
#undef W06_EMIT
	return true;
}

bool zend_mir_w06_emit_value_snapshot(
	const zend_mir_source_value_view *source_values,
	const zend_mir_value_plan *plan,
	zend_mir_module *module,
	zend_mir_value_mutator *mutator)
{
	const zend_mir_w06_records *records;

	if (source_values == NULL || plan == NULL || module == NULL
			|| source_values->contract_version != ZEND_MIR_W06_CONTRACT_VERSION
			|| !plan->complete || !plan->immutable
			|| source_values->context == NULL
			|| (records = source_values->context)->magic
				!= ZEND_MIR_W06_SNAPSHOT_MAGIC
			|| plan->entries != records->entries
			|| plan->count != records->entry_count) {
		return false;
	}
	if (mutator == NULL) {
		mutator = zend_mir_module_get_value_mutator(module);
	}
	return mutator != NULL
		&& zend_mir_w06_emit_records(records, mutator)
		&& zend_mir_module_commit_value_model(module);
}

static zend_mir_w06_lowering_result zend_mir_w06_failure(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code)
{
	zend_mir_w06_lowering_result result;
	memset(&result, 0, sizeof(result));
	result.prerequisite.lowering.status = status;
	result.prerequisite.lowering.diagnostic_code = code;
	return result;
}

static bool zend_mir_w06_source_fingerprint(
	const zend_mir_source_value_view *source_values,
	const zend_mir_value_plan *plan,
	uint32_t words[4])
{
	uint32_t index;
	words[0] = UINT32_C(2166136261);
	words[1] = UINT32_C(3339451269);
	words[2] = UINT32_C(2593831049);
	words[3] = UINT32_C(1268118805);
#define W06_MIX(value) \
	do { \
		uint32_t w06_value = (uint32_t) (value); \
		uint32_t w06_word; \
		for (w06_word = 0; w06_word < 4; w06_word++) { \
			words[w06_word] ^= w06_value + w06_word * UINT32_C(0x9e3779b9); \
			words[w06_word] *= UINT32_C(16777619); \
		} \
	} while (0)
	W06_MIX(source_values->storage_count(source_values->context));
	W06_MIX(source_values->reference_count(source_values->context));
	W06_MIX(source_values->indirect_count(source_values->context));
	W06_MIX(source_values->opcode_count(source_values->context));
	W06_MIX(plan->count);
	for (index = 0;
			index < source_values->storage_count(source_values->context);
			index++) {
		zend_mir_source_storage_ref storage;
		if (!source_values->storage_at(
				source_values->context, index, &storage)) {
			return false;
		}
		W06_MIX(storage.id);
		W06_MIX(storage.slot_kind);
		W06_MIX(storage.slot_index);
		W06_MIX(storage.ssa_variable_id);
		W06_MIX(storage.kind);
		W06_MIX(storage.state);
		W06_MIX(storage.category);
		W06_MIX(storage.provenance);
	}
	for (index = 0;
			index < source_values->reference_count(source_values->context);
			index++) {
		zend_mir_source_reference_ref reference;
		if (!source_values->reference_at(
				source_values->context, index, &reference)) {
			return false;
		}
		W06_MIX(reference.id);
		W06_MIX(reference.payload_storage_id);
		W06_MIX(reference.alias_class_id);
		W06_MIX(reference.creation_opline_index);
		W06_MIX(reference.ownership);
		W06_MIX(reference.cleanup_obligation);
	}
	for (index = 0;
			index < source_values->indirect_count(source_values->context);
			index++) {
		zend_mir_source_indirect_ref indirect;
		if (!source_values->indirect_at(
				source_values->context, index, &indirect)) {
			return false;
		}
		W06_MIX(indirect.indirect_storage_id);
		W06_MIX(indirect.target_storage_id);
		W06_MIX(indirect.source_opline_index);
	}
	for (index = 0;
			index < source_values->opcode_count(source_values->context);
			index++) {
		zend_mir_source_opcode_ref opcode;
		if (!source_values->opcode_at(
				source_values->context, index, &opcode)) {
			return false;
		}
		W06_MIX(opcode.opline_index);
		W06_MIX(opcode.zend_opcode_number);
#define W06_MIX_OPERAND(operand) \
		do { \
			W06_MIX((operand).kind); \
			W06_MIX((operand).slot_kind); \
			W06_MIX((operand).index); \
			W06_MIX((operand).ssa_variable_id); \
		} while (0)
		W06_MIX_OPERAND(opcode.op1);
		W06_MIX_OPERAND(opcode.op2);
		W06_MIX_OPERAND(opcode.result);
#undef W06_MIX_OPERAND
		W06_MIX(opcode.extended_value);
		W06_MIX(opcode.source_position_id);
		W06_MIX(opcode.block_id);
	}
	for (index = 0; index < plan->count; index++) {
		W06_MIX(plan->entries[index].source_opline_index);
		W06_MIX(plan->entries[index].decision);
		W06_MIX(plan->entries[index].diagnostic_code);
		W06_MIX(plan->entries[index].storage_span.offset);
		W06_MIX(plan->entries[index].storage_span.count);
		W06_MIX(plan->entries[index].ownership_event_span.offset);
		W06_MIX(plan->entries[index].ownership_event_span.count);
		W06_MIX(plan->entries[index].separation_plan_span.offset);
		W06_MIX(plan->entries[index].separation_plan_span.count);
	}
#undef W06_MIX
	return true;
}

static bool zend_mir_w06_fingerprint_equal(
	const uint32_t left[4], const uint32_t right[4])
{
	return left[0] == right[0]
		&& left[1] == right[1]
		&& left[2] == right[2]
		&& left[3] == right[3];
}

zend_mir_w06_lowering_result zend_mir_lower_w06_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator,
	const zend_mir_source_value_view *source_values,
	const zend_mir_value_plan *value_plan,
	zend_mir_value_mutator *value_mutator)
{
	zend_mir_lowering_result w04;
	zend_mir_w06_lowering_result result;
	const zend_mir_view *view;
	const zend_mir_call_view *calls;
	const zend_mir_value_view *values;
	const zend_mir_w06_records *records;
	zend_mir_lowering_diagnostic_code call_code;
	uint32_t module_fingerprint[4];
	uint32_t source_fingerprint[4];
	uint32_t recomputed_module_fingerprint[4];
	uint32_t recomputed_source_fingerprint[4];

	if (context == NULL || source_values == NULL || value_plan == NULL
			|| !value_plan->complete || !value_plan->immutable
			|| source_values->storage_count == NULL
			|| source_values->storage_at == NULL
			|| source_values->reference_count == NULL
			|| source_values->reference_at == NULL
			|| source_values->indirect_count == NULL
			|| source_values->indirect_at == NULL
			|| source_values->opcode_count == NULL
			|| source_values->opcode_at == NULL) {
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	records = source_values->context;
	if (records == NULL || records->magic != ZEND_MIR_W06_SNAPSHOT_MAGIC
			|| source_calls == NULL || resolver == NULL) {
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	if (records->w05_site_count != 0) {
		call_code = zend_mir_w05_validate_call_plan(
			context, &records->w05_calls, &records->w05_resolver);
		if (call_code != ZEND_MIRL_OK) {
			return zend_mir_w06_failure(
				call_code == ZEND_MIRL_W05_CALL_PLAN_FAILED
					? ZEND_MIR_LOWERING_FAILED
					: ZEND_MIR_LOWERING_DEFERRED,
				call_code);
		}
	}
	w04 = zend_mir_lower_w04_zend_source(
		context, mutator, control_flow_map);
	if (w04.status != ZEND_MIR_LOWERING_SUCCESS) {
		return zend_mir_w06_failure(w04.status, w04.diagnostic_code);
	}
	if (!zend_mir_lowering_result_is_w04_failure_atomic(&w04)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_VALUE_VERIFY_FAILED);
	}
	if (zend_mir_w06_fault_is(
				ZEND_MIR_W06_TEST_FAULT_STRUCTURAL_VERIFIER)
			|| zend_mir_w06_fault_is(
				ZEND_MIR_W06_TEST_FAULT_SCALAR_VERIFIER)
			|| zend_mir_w06_fault_is(
				ZEND_MIR_W06_TEST_FAULT_CONTROL_FLOW_VERIFIER)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_VALUE_VERIFY_FAILED);
	}
	if (records->w05_site_count != 0) {
		if (call_mutator == NULL) {
			call_mutator = zend_mir_module_get_call_mutator(w04.module);
		}
		call_code = zend_mir_w05_plan_and_emit_calls(
			context, &records->w05_calls, &records->w05_resolver,
			call_mutator);
		if (call_code != ZEND_MIRL_OK) {
			context->module_ops.destroy(
				context->module_ops.context, w04.module);
			return zend_mir_w06_failure(
				ZEND_MIR_LOWERING_FAILED, call_code);
		}
	}
	if (!zend_mir_w06_emit_value_snapshot(
					source_values, value_plan, w04.module, value_mutator)
			|| !context->module_ops.finalize(
				context->module_ops.context, w04.module)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_INVALID_STORAGE);
	}
	view = context->module_ops.view(
		context->module_ops.context, w04.module);
	calls = zend_mir_module_get_call_view(w04.module);
	values = zend_mir_module_get_value_view(w04.module);
	if (view == NULL || values == NULL
			|| (records->w05_site_count != 0
				&& (calls == NULL
					|| zend_mir_w06_fault_is(
						ZEND_MIR_W06_TEST_FAULT_CALL_VERIFIER)
					|| !zend_mir_verify_w05_calls(
						view, &records->w05_calls, calls,
						context->diagnostics)))
			|| !zend_mir_value_compute_module_fingerprint(
					view, context->diagnostics, module_fingerprint)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_VALUE_VERIFY_FAILED);
	}
	if (!zend_mir_w06_source_fingerprint(
			source_values, value_plan, source_fingerprint)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_VALUE_VERIFY_FAILED);
	}
	if (!zend_mir_value_compute_module_fingerprint(
				view, context->diagnostics,
				recomputed_module_fingerprint)
			|| !zend_mir_w06_source_fingerprint(
				source_values, value_plan,
				recomputed_source_fingerprint)
			|| zend_mir_w06_fault_is(
				ZEND_MIR_W06_TEST_FAULT_FINGERPRINT_RECOMPUTE)
			|| !zend_mir_w06_fingerprint_equal(
				module_fingerprint, recomputed_module_fingerprint)
			|| !zend_mir_w06_fingerprint_equal(
				source_fingerprint, recomputed_source_fingerprint)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_VALUE_VERIFY_FAILED);
	}
	if (zend_mir_w06_fault_is(
				ZEND_MIR_W06_TEST_FAULT_VALUE_VERIFIER)
			|| !zend_mir_verify_w06_values(
				view, values, context->diagnostics)) {
		context->module_ops.destroy(context->module_ops.context, w04.module);
		return zend_mir_w06_failure(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_W06_VALUE_VERIFY_FAILED);
	}
	memset(&result, 0, sizeof(result));
	result.prerequisite.lowering = w04;
	result.prerequisite.prerequisite_guarantees = w04.guarantees;
	result.prerequisite.lowering.guarantees =
		ZEND_MIR_LOWERING_GUARANTEE_FINALIZED;
	result.prerequisite.capabilities = ZEND_MIR_W05_REQUIRED_CAPABILITIES;
	result.prerequisite.semantic_debts = ZEND_MIR_W05_REQUIRED_DEBTS;
	result.prerequisite.modeled = true;
	result.prerequisite.codegen_eligible = false;
	result.values_verified = true;
	result.modeled = true;
	result.codegen_eligible = false;
	return result;
}
