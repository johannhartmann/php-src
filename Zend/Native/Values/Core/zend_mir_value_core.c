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

#include "zend_mir_value_core.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "../../MIR/Core/zend_mir_module_internal.h"

static bool zend_mir_value_grow_staging(
	zend_mir_module *module, void **items, uint32_t count,
	uint32_t *capacity, size_t item_size, size_t alignment)
{
	uint32_t next;
	void *replacement;

	if (!zend_mir_module_require_building(module)
			|| items == NULL || capacity == NULL || item_size == 0
			|| count == UINT32_MAX) {
		return false;
	}
	if (count < *capacity) {
		return true;
	}
	next = *capacity == 0 ? UINT32_C(4) : *capacity;
	while (next <= count) {
		if (next > UINT32_MAX / 2) {
			return false;
		}
		next *= 2;
	}
	if ((size_t) next > SIZE_MAX / item_size) {
		return false;
	}
	replacement = zend_mir_arena_allocate(
		&module->arena, (size_t) next * item_size, alignment);
	if (replacement == NULL) {
		return false;
	}
	if (count != 0) {
		memcpy(replacement, *items, (size_t) count * item_size);
	}
	*items = replacement;
	*capacity = next;
	return true;
}

#define ZEND_MIR_VALUE_STAGE(name, type, field, count_field, capacity_field) \
static bool name(void *context, const type *record) \
{ \
	zend_mir_module *module = (zend_mir_module *) context; \
	zend_mir_core_value_staging *staging; \
	if (!zend_mir_module_require_building(module) || record == NULL) { \
		return false; \
	} \
	staging = &module->value_staging; \
	if (staging->committed || !zend_mir_value_grow_staging( \
			module, (void **) &staging->field, staging->count_field, \
			&staging->capacity_field, sizeof(*staging->field), \
			alignof(type))) { \
		return zend_mir_module_fail(module, \
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED, \
			"W06 value-model staging failed"); \
	} \
	staging->field[staging->count_field++] = *record; \
	return true; \
}

ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_storage,
	zend_mir_storage_ref, storages, storage_count, storage_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_payload,
	zend_mir_payload_ref, payloads, payload_count, payload_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_reference_cell,
	zend_mir_reference_cell_ref, reference_cells,
	reference_cell_count, reference_cell_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_alias_relation,
	zend_mir_alias_relation_ref, alias_relations,
	alias_relation_count, alias_relation_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_ownership_event,
	zend_mir_ownership_event_ref, ownership_events,
	ownership_event_count, ownership_event_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_separation_plan,
	zend_mir_separation_plan_ref, separation_plans,
	separation_plan_count, separation_plan_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_call_transfer,
	zend_mir_call_transfer_ref, call_transfers,
	call_transfer_count, call_transfer_capacity)
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_executable_operation,
	zend_mir_executable_value_ref, executable_operations,
	executable_operation_count, executable_operation_capacity)

#undef ZEND_MIR_VALUE_STAGE

static bool zend_mir_value_category_valid(zend_mir_value_category category)
{
	return category >= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
		&& category <= ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
}

static bool zend_mir_value_refcount_valid(zend_mir_refcount_state state)
{
	return state >= ZEND_MIR_REFCOUNT_IMMORTAL
		&& state <= ZEND_MIR_REFCOUNT_UNKNOWN;
}

static bool zend_mir_value_resolve_staged_payload(
	const zend_mir_core_value_staging *staging,
	zend_mir_storage_id storage_id,
	zend_mir_payload_id *payload_id)
{
	uint32_t hops;

	if (staging == NULL || payload_id == NULL
			|| storage_id >= staging->storage_count) {
		return false;
	}
	for (hops = 0; hops <= staging->storage_count; hops++) {
		const zend_mir_storage_ref *storage = &staging->storages[storage_id];
		if (storage->state == ZEND_MIR_STORAGE_DIRECT) {
			*payload_id = storage->payload_id;
			return *payload_id < staging->payload_count;
		}
		if (storage->state == ZEND_MIR_STORAGE_REFERENCE) {
			const zend_mir_reference_cell_ref *cell;
			if (storage->reference_cell_id >= staging->reference_cell_count) {
				return false;
			}
			cell = &staging->reference_cells[storage->reference_cell_id];
			storage_id = cell->payload_storage_id;
			continue;
		}
		if (storage->state != ZEND_MIR_STORAGE_INDIRECT
				|| storage->indirect_target_id
					>= staging->storage_count) {
			return false;
		}
		storage_id = storage->indirect_target_id;
	}
	return false;
}

bool zend_mir_value_transition_valid(
	zend_mir_transfer_action action,
	zend_mir_refcount_state before_state,
	zend_mir_refcount_state after_state,
	bool cleanup_obligation)
{
	if (!zend_mir_value_refcount_valid(before_state)
			|| !zend_mir_value_refcount_valid(after_state)) {
		return false;
	}
	switch (action) {
		case ZEND_MIR_TRANSFER_BORROW:
			return before_state == after_state && !cleanup_obligation;
		case ZEND_MIR_TRANSFER_COPY_ADDREF:
			return cleanup_obligation
				&& ((before_state == ZEND_MIR_REFCOUNT_IMMORTAL
						&& after_state == ZEND_MIR_REFCOUNT_IMMORTAL)
					|| (before_state == ZEND_MIR_REFCOUNT_UNIQUE
						&& after_state == ZEND_MIR_REFCOUNT_SHARED)
					|| (before_state == ZEND_MIR_REFCOUNT_SHARED
						&& after_state == ZEND_MIR_REFCOUNT_SHARED)
					|| (before_state == ZEND_MIR_REFCOUNT_UNKNOWN
						&& after_state == ZEND_MIR_REFCOUNT_UNKNOWN));
		case ZEND_MIR_TRANSFER_MOVE:
			return before_state != ZEND_MIR_REFCOUNT_IMMORTAL
				&& before_state == after_state && !cleanup_obligation;
		case ZEND_MIR_TRANSFER_RELEASE:
			return before_state != ZEND_MIR_REFCOUNT_IMMORTAL
				&& cleanup_obligation
				&& ((before_state == ZEND_MIR_REFCOUNT_UNIQUE
						&& after_state == ZEND_MIR_REFCOUNT_UNKNOWN)
					|| (before_state == ZEND_MIR_REFCOUNT_SHARED
						&& after_state == ZEND_MIR_REFCOUNT_SHARED)
					|| (before_state == ZEND_MIR_REFCOUNT_UNKNOWN
						&& after_state == ZEND_MIR_REFCOUNT_UNKNOWN));
		case ZEND_MIR_TRANSFER_TO_CALLEE:
			return cleanup_obligation && before_state == after_state;
		case ZEND_MIR_TRANSFER_FROM_CALLEE:
			return before_state != ZEND_MIR_REFCOUNT_IMMORTAL
				&& after_state != ZEND_MIR_REFCOUNT_IMMORTAL
				&& cleanup_obligation;
		default:
			return false;
	}
}

static int zend_mir_value_compare_u32(const void *left, const void *right)
{
	uint32_t lhs = *(const uint32_t *) left;
	uint32_t rhs = *(const uint32_t *) right;
	return lhs < rhs ? -1 : lhs > rhs;
}

static uint32_t zend_mir_value_find_id(
	const uint32_t *ids, uint32_t count, uint32_t id)
{
	const uint32_t *match = bsearch(
		&id, ids, count, sizeof(*ids), zend_mir_value_compare_u32);
	return match != NULL ? (uint32_t) (match - ids) : UINT32_MAX;
}

static uint32_t zend_mir_value_alias_root(uint32_t *parents, uint32_t id)
{
	while (parents[id] != id) {
		parents[id] = parents[parents[id]];
		id = parents[id];
	}
	return id;
}

typedef struct _zend_mir_value_alias_key {
	uint32_t left_id;
	uint32_t right_id;
	zend_mir_alias_relation relation;
} zend_mir_value_alias_key;

static int zend_mir_value_compare_alias_key(
	const void *left, const void *right)
{
	const zend_mir_value_alias_key *lhs = left;
	const zend_mir_value_alias_key *rhs = right;
	if (lhs->left_id != rhs->left_id) {
		return lhs->left_id < rhs->left_id ? -1 : 1;
	}
	if (lhs->right_id != rhs->right_id) {
		return lhs->right_id < rhs->right_id ? -1 : 1;
	}
	return 0;
}

static bool zend_mir_value_alias_relations_valid(
	zend_mir_module *module,
	const zend_mir_alias_relation_ref *relations, uint32_t count)
{
	uint32_t *allocation;
	uint32_t *ids;
	uint32_t *parents;
	zend_mir_value_alias_key *keys;
	uint32_t id_count = 0;
	uint32_t index;
	bool valid = false;

	if (module == NULL || (relations == NULL && count != 0)
			|| count > UINT32_C(1048576)) {
		return false;
	}
	if (count == 0) {
		return true;
	}
	allocation = zend_mir_arena_allocate(
		&module->arena, (size_t) count * sizeof(uint32_t) * 4,
		alignof(uint32_t));
	if (allocation == NULL) {
		return false;
	}
	keys = zend_mir_arena_allocate(
		&module->arena, (size_t) count * sizeof(*keys),
		alignof(zend_mir_value_alias_key));
	if (keys == NULL) {
		return false;
	}
	ids = allocation;
	parents = allocation + count * 2;
	for (index = 0; index < count; index++) {
		const zend_mir_alias_relation_ref *relation = &relations[index];
		if (!zend_mir_id_is_valid(relation->left_id)
				|| !zend_mir_id_is_valid(relation->right_id)
				|| relation->relation < ZEND_MIR_ALIAS_MUST
				|| relation->relation > ZEND_MIR_ALIAS_NONE
				|| (relation->left_id == relation->right_id
					&& relation->relation != ZEND_MIR_ALIAS_MUST)
				|| (relation->relation == ZEND_MIR_ALIAS_NONE
					&& relation->proof_id == 0)) {
			goto done;
		}
		ids[index * 2] = relation->left_id;
		ids[index * 2 + 1] = relation->right_id;
		keys[index].left_id = relation->left_id < relation->right_id
			? relation->left_id : relation->right_id;
		keys[index].right_id = relation->left_id < relation->right_id
			? relation->right_id : relation->left_id;
		keys[index].relation = relation->relation;
	}
	qsort(keys, count, sizeof(*keys), zend_mir_value_compare_alias_key);
	for (index = 1; index < count; index++) {
		if (keys[index - 1].left_id == keys[index].left_id
				&& keys[index - 1].right_id == keys[index].right_id
				&& keys[index - 1].relation
					!= keys[index].relation) {
			goto done;
		}
	}
	qsort(ids, count * 2, sizeof(*ids), zend_mir_value_compare_u32);
	for (index = 0; index < count * 2; index++) {
		if (id_count == 0 || ids[index] != ids[id_count - 1]) {
			ids[id_count++] = ids[index];
		}
	}
	for (index = 0; index < id_count; index++) {
		parents[index] = index;
	}
	for (index = 0; index < count; index++) {
		const zend_mir_alias_relation_ref *relation = &relations[index];
		if (relation->relation == ZEND_MIR_ALIAS_MUST) {
			uint32_t left = zend_mir_value_alias_root(
				parents, zend_mir_value_find_id(
					ids, id_count, relation->left_id));
			uint32_t right = zend_mir_value_alias_root(
				parents, zend_mir_value_find_id(
					ids, id_count, relation->right_id));
			parents[right] = left;
		}
	}
	for (index = 0; index < count; index++) {
		const zend_mir_alias_relation_ref *relation = &relations[index];
		if (relation->relation == ZEND_MIR_ALIAS_NONE
				&& zend_mir_value_alias_root(
					parents, zend_mir_value_find_id(
						ids, id_count, relation->left_id))
					== zend_mir_value_alias_root(
						parents, zend_mir_value_find_id(
							ids, id_count,
							relation->right_id))) {
			goto done;
		}
	}
	valid = true;
done:
	return valid;
}

static bool zend_mir_value_validate_payloads(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;

	for (index = 0; index < staging->payload_count; index++) {
		const zend_mir_payload_ref *payload = &staging->payloads[index];
		if (payload->id != index
				|| !zend_mir_value_category_valid(payload->category)
				|| !zend_mir_value_refcount_valid(
					payload->refcount_state)
				|| (payload->category
					== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
					&& payload->cleanup_obligation)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_value_validate_storages(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;

	for (index = 0; index < staging->storage_count; index++) {
		const zend_mir_storage_ref *storage = &staging->storages[index];
		bool valid = storage->id == index
			&& storage->kind >= ZEND_MIR_STORAGE_FRAME_SLOT
			&& storage->kind <= ZEND_MIR_STORAGE_CALL_RETURN_SLOT
			&& storage->state >= ZEND_MIR_STORAGE_UNDEF
			&& storage->state <= ZEND_MIR_STORAGE_INDIRECT
			&& zend_mir_value_category_valid(storage->category);

		switch (storage->state) {
			case ZEND_MIR_STORAGE_UNDEF:
				valid = valid
					&& !zend_mir_id_is_valid(storage->payload_id)
					&& !zend_mir_id_is_valid(
						storage->reference_cell_id)
					&& !zend_mir_id_is_valid(
						storage->indirect_target_id);
				break;
			case ZEND_MIR_STORAGE_DIRECT:
				valid = valid
					&& storage->payload_id < staging->payload_count
					&& !zend_mir_id_is_valid(
						storage->reference_cell_id)
					&& !zend_mir_id_is_valid(
						storage->indirect_target_id)
					&& storage->category
						== staging->payloads[
							storage->payload_id].category;
				break;
			case ZEND_MIR_STORAGE_REFERENCE:
				valid = valid
					&& !zend_mir_id_is_valid(storage->payload_id)
					&& storage->reference_cell_id
						< staging->reference_cell_count
					&& !zend_mir_id_is_valid(
						storage->indirect_target_id)
					&& storage->category
						== ZEND_MIR_VALUE_REFERENCE_CELL;
				break;
			case ZEND_MIR_STORAGE_INDIRECT:
				valid = valid
					&& !zend_mir_id_is_valid(storage->payload_id)
					&& !zend_mir_id_is_valid(
						storage->reference_cell_id)
					&& storage->indirect_target_id
						< staging->storage_count
					&& storage->indirect_target_id != index;
				break;
			default:
				valid = false;
				break;
		}
		if (!valid) {
			return false;
		}
	}
	/* Following an indirect chain for at most N hops proves acyclicity. */
	for (index = 0; index < staging->storage_count; index++) {
		uint32_t cursor = index;
		uint32_t hops;
		for (hops = 0; hops <= staging->storage_count; hops++) {
			const zend_mir_storage_ref *storage =
				&staging->storages[cursor];
			if (storage->state != ZEND_MIR_STORAGE_INDIRECT) {
				break;
			}
			cursor = storage->indirect_target_id;
			if (hops == staging->storage_count) {
				return false;
			}
		}
	}
	return true;
}

static bool zend_mir_value_validate_references(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;

	for (index = 0; index < staging->reference_cell_count; index++) {
		const zend_mir_reference_cell_ref *cell =
			&staging->reference_cells[index];
		if (cell->id != index
				|| cell->payload_storage_id >= staging->storage_count
				|| staging->storages[cell->payload_storage_id].kind
					!= ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT
				|| staging->storages[cell->payload_storage_id].state
					!= ZEND_MIR_STORAGE_DIRECT
				|| !zend_mir_id_is_valid(cell->alias_class_id)
				|| !zend_mir_id_is_valid(cell->creation_source_id)
				|| cell->ownership < 0
				|| cell->ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT
				|| !cell->cleanup_obligation) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_value_validate_aliases(
	zend_mir_module *module,
	const zend_mir_core_value_staging *staging)
{
	return zend_mir_value_alias_relations_valid(
		module, staging->alias_relations, staging->alias_relation_count);
}

static bool zend_mir_value_validate_events(
	zend_mir_module *module,
	const zend_mir_core_value_staging *staging)
{
	zend_mir_refcount_state *states;
	bool *borrowed;
	bool *consumed;
	size_t state_bytes;
	uint32_t index;

	if (staging->ownership_event_count == 0) {
		return true;
	}
	if (staging->payload_count == 0) {
		return false;
	}
	state_bytes = (size_t) staging->payload_count * sizeof(*states);
	states = zend_mir_arena_allocate(
		&module->arena, state_bytes, alignof(zend_mir_refcount_state));
	borrowed = zend_mir_arena_allocate(
		&module->arena, staging->payload_count, alignof(bool));
	consumed = zend_mir_arena_allocate(
		&module->arena, staging->payload_count, alignof(bool));
	if (states == NULL || borrowed == NULL || consumed == NULL) {
		return false;
	}
	memset(borrowed, 0, staging->payload_count);
	memset(consumed, 0, staging->payload_count);
	for (index = 0; index < staging->payload_count; index++) {
		states[index] = staging->payloads[index].refcount_state;
	}
	for (index = 0; index < staging->ownership_event_count; index++) {
		const zend_mir_ownership_event_ref *event =
			&staging->ownership_events[index];
		zend_mir_payload_id source_payload;
		zend_mir_payload_id target_payload = ZEND_MIR_ID_INVALID;
		bool target_required =
			event->action == ZEND_MIR_TRANSFER_COPY_ADDREF
			|| event->action == ZEND_MIR_TRANSFER_MOVE
			|| event->action == ZEND_MIR_TRANSFER_FROM_CALLEE;
		if (event->id != index
				|| event->source_storage_id >= staging->storage_count
				|| event->payload_id >= staging->payload_count
				|| event->action < ZEND_MIR_TRANSFER_BORROW
				|| event->action > ZEND_MIR_TRANSFER_FROM_CALLEE
				|| !zend_mir_value_resolve_staged_payload(
					staging, event->source_storage_id,
					&source_payload)
				|| source_payload != event->payload_id
				|| states[event->payload_id] != event->before_state
				|| !zend_mir_value_transition_valid(
					event->action, event->before_state,
					event->after_state, event->cleanup_obligation)
				|| (event->cleanup_obligation
					&& !staging->payloads[
						event->payload_id].cleanup_obligation)
				|| consumed[event->payload_id]
				|| (target_required
					&& (event->target_storage_id
							>= staging->storage_count
						|| event->target_storage_id
							== event->source_storage_id
						|| !zend_mir_value_resolve_staged_payload(
							staging, event->target_storage_id,
							&target_payload)
						|| target_payload != event->payload_id))
				|| (!target_required
					&& zend_mir_id_is_valid(
						event->target_storage_id))
				|| ((event->action == ZEND_MIR_TRANSFER_RELEASE
						|| event->action == ZEND_MIR_TRANSFER_MOVE
						|| event->action
							== ZEND_MIR_TRANSFER_TO_CALLEE)
					&& borrowed[event->payload_id])) {
			return false;
		}
		states[event->payload_id] = event->after_state;
		if (event->action == ZEND_MIR_TRANSFER_BORROW) {
			borrowed[event->payload_id] = true;
		}
		if (event->action == ZEND_MIR_TRANSFER_MOVE
				|| event->action == ZEND_MIR_TRANSFER_RELEASE
				|| event->action == ZEND_MIR_TRANSFER_TO_CALLEE) {
			consumed[event->payload_id] = true;
		}
	}
	return true;
}

static bool zend_mir_value_validate_separations(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;

	for (index = 0; index < staging->separation_plan_count; index++) {
		const zend_mir_separation_plan_ref *plan =
			&staging->separation_plans[index];
		const zend_mir_payload_ref *source;
		zend_mir_payload_id source_payload;
		if (plan->id != index
				|| plan->source_payload_id >= staging->payload_count
				|| plan->source_storage_id >= staging->storage_count
				|| plan->reason < ZEND_MIR_SEPARATION_EXPLICIT
				|| plan->reason > ZEND_MIR_SEPARATION_CALL_BOUNDARY
				|| !zend_mir_value_refcount_valid(
					plan->uniqueness_fact)
				|| plan->required < ZEND_MIR_SEPARATION_REQUIRED_NO
				|| plan->required
					> ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN
				|| !zend_mir_value_resolve_staged_payload(
					staging, plan->source_storage_id,
					&source_payload)
				|| source_payload != plan->source_payload_id) {
			return false;
		}
		source = &staging->payloads[plan->source_payload_id];
		if (source->refcount_state != plan->uniqueness_fact
				|| (plan->uniqueness_fact
						== ZEND_MIR_REFCOUNT_UNIQUE
					&& plan->required
						!= ZEND_MIR_SEPARATION_REQUIRED_NO)
				|| (plan->uniqueness_fact
						== ZEND_MIR_REFCOUNT_SHARED
					&& plan->required
						!= ZEND_MIR_SEPARATION_REQUIRED_YES)
				|| ((plan->uniqueness_fact
							== ZEND_MIR_REFCOUNT_UNKNOWN
						|| plan->uniqueness_fact
							== ZEND_MIR_REFCOUNT_IMMORTAL)
					&& plan->required
						!= ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN)) {
			return false;
		}
		if (plan->required == ZEND_MIR_SEPARATION_REQUIRED_YES) {
			if (plan->result_payload_id >= staging->payload_count
					|| plan->result_payload_id
						== plan->source_payload_id
					|| staging->payloads[
						plan->result_payload_id].category
						!= source->category
					|| staging->payloads[
						plan->result_payload_id].refcount_state
						!= ZEND_MIR_REFCOUNT_UNIQUE
					|| !plan->clone_execution_required) {
				return false;
			}
		} else if (plan->required == ZEND_MIR_SEPARATION_REQUIRED_NO
				&& ((zend_mir_id_is_valid(plan->result_payload_id)
						&& plan->result_payload_id
							!= plan->source_payload_id)
					|| plan->clone_execution_required)) {
			return false;
		} else if (plan->required
					== ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN
				&& (zend_mir_id_is_valid(plan->result_payload_id)
					|| !plan->clone_execution_required)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_value_validate_call_transfer_storage(
	const zend_mir_core_value_staging *staging,
	const zend_mir_call_transfer_ref *transfer)
{
	const zend_mir_storage_ref *argument_storage;
	const zend_mir_storage_ref *return_storage;
	bool argument_is_reference;
	bool return_is_reference;

	if (transfer->argument_storage_id >= staging->storage_count
			|| transfer->return_storage_id >= staging->storage_count) {
		return false;
	}
	argument_storage = &staging->storages[transfer->argument_storage_id];
	return_storage = &staging->storages[transfer->return_storage_id];
	argument_is_reference =
		argument_storage->state == ZEND_MIR_STORAGE_REFERENCE;
	return_is_reference = return_storage->state == ZEND_MIR_STORAGE_REFERENCE;
	return argument_is_reference
			== zend_mir_id_is_valid(transfer->argument_reference_cell_id)
		&& (!argument_is_reference
			|| (transfer->argument_reference_cell_id
					< staging->reference_cell_count
				&& argument_storage->reference_cell_id
					== transfer->argument_reference_cell_id))
		&& (argument_is_reference
			? transfer->argument_action == ZEND_MIR_TRANSFER_TO_CALLEE
			: argument_storage->category
					== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				? transfer->argument_action == ZEND_MIR_TRANSFER_BORROW
				: transfer->argument_action
					== ZEND_MIR_TRANSFER_COPY_ADDREF)
		&& return_storage->kind == ZEND_MIR_STORAGE_CALL_RETURN_SLOT
		&& return_is_reference
			== zend_mir_id_is_valid(transfer->return_reference_cell_id)
		&& (!return_is_reference
			|| (transfer->return_reference_cell_id
					< staging->reference_cell_count
				&& return_storage->reference_cell_id
					== transfer->return_reference_cell_id))
		&& transfer->return_action == ZEND_MIR_TRANSFER_FROM_CALLEE;
}

static bool zend_mir_value_validate_call_transfers(
	const zend_mir_module *module,
	const zend_mir_core_value_staging *staging)
{
	const zend_mir_call_site_ref *sites;
	const zend_mir_call_argument_ref *arguments;
	const zend_mir_call_target_ref *targets;
	uint32_t index;
	bool source_backed = staging->call_transfer_count == 0
		&& module->call_arguments.count != 0;

	if (!module->call_staging.committed) {
		return staging->call_transfer_count == 0;
	}
	if (module->call_sites.count == 0 && module->call_arguments.count == 0
			&& module->call_targets.count == 0
			&& module->call_continuations.count == 0) {
		return staging->call_transfer_count == 0;
	}
	if ((!source_backed
			&& staging->call_transfer_count != module->call_arguments.count)
			|| module->call_sites.items == NULL
			|| (module->call_arguments.count != 0
				&& module->call_arguments.items == NULL)
			|| module->call_targets.items == NULL) {
		return false;
	}
	sites = ZEND_MIR_CORE_ITEMS(
		module, call_sites, zend_mir_call_site_ref);
	arguments = ZEND_MIR_CORE_ITEMS(
		module, call_arguments, zend_mir_call_argument_ref);
	targets = ZEND_MIR_CORE_ITEMS(
		module, call_targets, zend_mir_call_target_ref);
	if (source_backed) {
		for (index = 0; index < module->call_arguments.count; index++) {
			if (arguments[index].id != index
					|| arguments[index].ownership
						== ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR) {
				return false;
			}
		}
		return true;
	}
	for (index = 0; index < staging->call_transfer_count; index++) {
		const zend_mir_call_transfer_ref *transfer =
			&staging->call_transfers[index];
		const zend_mir_call_argument_ref *argument = &arguments[index];
		const zend_mir_call_site_ref *site;
		const zend_mir_call_target_ref *target;
		uint32_t site_first_transfer;

		if (argument->id != index
				|| argument->call_site_id >= module->call_sites.count) {
			return false;
		}
		site = &sites[argument->call_site_id];
		if (site->id != argument->call_site_id
				|| site->target_id >= module->call_targets.count
				|| site->arguments.offset > index
				|| argument->ordinal >= site->arguments.count
				|| site->arguments.offset + argument->ordinal != index
				|| transfer->call_site_id != site->id
				|| transfer->argument_ordinal != argument->ordinal) {
			return false;
		}
		target = &targets[site->target_id];
		if (target->id != site->target_id
				|| transfer->parameter_modes.count != target->num_args
				|| argument->ordinal >= transfer->parameter_modes.count
				|| transfer->parameter_modes.offset
					> UINT32_MAX - transfer->parameter_modes.count
				|| !zend_mir_value_validate_call_transfer_storage(
					staging, transfer)) {
			return false;
		}
		site_first_transfer = index - argument->ordinal;
		if (argument->ordinal != 0) {
			const zend_mir_call_transfer_ref *first =
				&staging->call_transfers[site_first_transfer];
			if (first->call_site_id != transfer->call_site_id
					|| first->parameter_modes.offset
						!= transfer->parameter_modes.offset
					|| first->parameter_modes.count
						!= transfer->parameter_modes.count
					|| first->return_storage_id
						!= transfer->return_storage_id
					|| first->return_reference_cell_id
						!= transfer->return_reference_cell_id
					|| first->return_action != transfer->return_action) {
				return false;
			}
		}
	}
	return true;
}

static bool zend_mir_value_staging_counts_bounded(
	const zend_mir_core_value_staging *staging)
{
	const uint32_t limit = UINT32_C(1048576);
	return staging->storage_count <= limit
		&& staging->payload_count <= limit
		&& staging->reference_cell_count <= limit
		&& staging->alias_relation_count <= limit
		&& staging->ownership_event_count <= limit
		&& staging->separation_plan_count <= limit
		&& staging->call_transfer_count <= limit
		&& staging->executable_operation_count <= limit;
}

static bool zend_mir_value_validate_executable_operations(
	const zend_mir_module *module,
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;
	zend_mir_block_id previous_block = 0;
	zend_mir_source_position_id previous_source = 0;
	bool have_previous = false;

	for (index = 0; index < staging->executable_operation_count; index++) {
		const zend_mir_executable_value_ref *operation =
			&staging->executable_operations[index];
		const zend_mir_block_record *blocks = ZEND_MIR_CORE_ITEMS(
			module, blocks, zend_mir_block_record);

		if (zend_mir_id_is_valid(operation->id)
				|| operation->block_id >= module->blocks.count
				|| blocks[operation->block_id].id != operation->block_id
				|| !zend_mir_opcode_is_executable_value(operation->opcode)
				|| operation->source_position_id >= module->source_positions.count) {
			return false;
		}
		if (have_previous
				&& (operation->block_id < previous_block
					|| (operation->block_id == previous_block
						&& operation->source_position_id <= previous_source))) {
			return false;
		}
		previous_block = operation->block_id;
		previous_source = operation->source_position_id;
		have_previous = true;
	}
	return true;
}

static void zend_mir_value_emit_executable_operation(
	zend_mir_core_instruction *target,
	const zend_mir_executable_value_ref *operation,
	zend_mir_instruction_id id)
{
	memset(target, 0, sizeof(*target));
	target->record.id = id;
	target->record.block_id = operation->block_id;
	target->record.opcode = operation->opcode;
	target->record.representation = ZEND_MIR_REPRESENTATION_VOID;
	target->record.result_id = ZEND_MIR_ID_INVALID;
	target->record.frame_state_id = operation->frame_state_id;
	target->record.source_position_id = operation->source_position_id;
	target->record.effects = operation->effects;
	target->record.reads = operation->reads;
	target->record.writes = operation->writes;
	target->record.barriers = operation->barriers;
	target->record.ownership_actions = operation->ownership_actions;
}

static bool zend_mir_value_compose_executable_operations(
	zend_mir_module *module, zend_mir_core_value_staging *staging)
{
	zend_mir_core_instruction *old_instructions;
	zend_mir_core_instruction *new_instructions;
	zend_mir_call_site_ref *call_sites;
	uint32_t *old_to_new;
	unsigned char *operation_emitted;
	size_t instruction_bytes;
	size_t map_bytes;
	uint32_t old_count = module->instructions.count;
	uint32_t operation_count = staging->executable_operation_count;
	uint32_t total_count;
	uint32_t old_index = 0;
	uint32_t operation_index;
	uint32_t emitted_count = 0;
	uint32_t new_index = 0;
	uint32_t site_index;

	if (operation_count == 0) {
		return true;
	}
	if (old_count > module->limits.instructions
			|| operation_count > module->limits.instructions - old_count) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"executable value instruction count overflow");
	}
	total_count = old_count + operation_count;
	if ((size_t) total_count > SIZE_MAX / sizeof(*new_instructions)
			|| (size_t) old_count > SIZE_MAX / sizeof(*old_to_new)) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED,
			"executable value composition size overflow");
	}
	instruction_bytes = (size_t) total_count * sizeof(*new_instructions);
	map_bytes = (size_t) old_count * sizeof(*old_to_new);
	new_instructions = zend_mir_arena_allocate(
		&module->arena, instruction_bytes, alignof(zend_mir_core_instruction));
	old_to_new = zend_mir_arena_allocate(
		&module->arena, map_bytes == 0 ? sizeof(*old_to_new) : map_bytes,
		alignof(uint32_t));
	operation_emitted = zend_mir_arena_allocate(
		&module->arena, operation_count, alignof(unsigned char));
	if (new_instructions == NULL || old_to_new == NULL
			|| operation_emitted == NULL) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED,
			"executable value composition allocation failed");
	}
	memset(operation_emitted, 0, operation_count);
	old_instructions = ZEND_MIR_CORE_ITEMS(
		module, instructions, zend_mir_core_instruction);
	while (old_index < old_count) {
		const zend_mir_instruction_record *old =
			&old_instructions[old_index].record;

		/*
		 * CFG lowering emits PHIs for all blocks before it emits block bodies,
		 * so the global instruction table is intentionally not block-sorted.
		 * Merge against each owning block's non-PHI stream instead of assuming
		 * that the next global instruction belongs to the next block.  This
		 * also preserves the required PHI-first ordering within every block.
		 */
		if (old->opcode != ZEND_MIR_OPCODE_PHI) {
			for (operation_index = 0; operation_index < operation_count;
					operation_index++) {
				const zend_mir_executable_value_ref *operation =
					&staging->executable_operations[operation_index];

				if (operation_emitted[operation_index]
						|| operation->block_id != old->block_id
						|| operation->source_position_id
							> old->source_position_id) {
					continue;
				}
				zend_mir_value_emit_executable_operation(
					&new_instructions[new_index], operation, new_index);
				operation_emitted[operation_index] = 1;
				emitted_count++;
				new_index++;
			}
		}
		new_instructions[new_index] = old_instructions[old_index];
		new_instructions[new_index].record.id = new_index;
		old_to_new[old_index] = new_index;
		new_index++;
		old_index++;
	}
	if (emitted_count != operation_count || new_index != total_count) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_INVALID_CFG,
			"executable value operation has no ordered owning block instruction");
	}
	call_sites = ZEND_MIR_CORE_ITEMS(
		module, call_sites, zend_mir_call_site_ref);
	for (site_index = 0; site_index < module->call_sites.count; site_index++) {
		if (call_sites[site_index].instruction_id >= old_count) {
			return zend_mir_module_fail(module,
				ZEND_MIR_DIAGNOSTIC_INVALID_ID,
				"call site references an unknown pre-composition instruction");
		}
		call_sites[site_index].instruction_id =
			old_to_new[call_sites[site_index].instruction_id];
	}
	module->instructions.items = new_instructions;
	module->instructions.count = total_count;
	module->instructions.capacity = total_count;
	return true;
}

static bool zend_mir_value_copy_table(
	zend_mir_module *module, zend_mir_core_table *table,
	const void *source, uint32_t count, size_t item_size, size_t alignment)
{
	const unsigned char *bytes = (const unsigned char *) source;

	while (table->count < count) {
		if (!zend_mir_module_grow_table(
				module, table, item_size, alignment, UINT32_MAX)) {
			return false;
		}
		memcpy((unsigned char *) table->items
				+ (size_t) table->count * item_size,
			bytes + (size_t) table->count * item_size, item_size);
		table->count++;
	}
	return true;
}

bool zend_mir_module_commit_value_model(zend_mir_module *module)
{
	zend_mir_core_value_staging *staging;

	if (!zend_mir_module_require_building(module)) {
		return false;
	}
	if (module->value_mutator.contract_version == 0) {
		zend_mir_module_init_value_view(module);
		zend_mir_module_init_value_mutator(module);
	}
	staging = &module->value_staging;
	if (staging->committed
			|| !zend_mir_value_staging_counts_bounded(staging)
			|| !zend_mir_value_validate_payloads(staging)
			|| !zend_mir_value_validate_storages(staging)
			|| !zend_mir_value_validate_references(staging)
			|| !zend_mir_value_validate_aliases(module, staging)
			|| !zend_mir_value_validate_events(module, staging)
			|| !zend_mir_value_validate_separations(staging)
			|| !zend_mir_value_validate_call_transfers(module, staging)
			|| !zend_mir_value_validate_executable_operations(module, staging)
			|| !zend_mir_value_compose_executable_operations(module, staging)) {
		return zend_mir_module_fail(module,
			ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP,
			"invalid W06 value/reference model");
	}
#define ZEND_MIR_VALUE_COPY(field, source_field, count_field, type) \
	if (!zend_mir_value_copy_table(module, &module->field, \
			staging->source_field, staging->count_field, \
			sizeof(type), alignof(type))) { \
		return false; \
	}
	ZEND_MIR_VALUE_COPY(value_payloads, payloads, payload_count,
		zend_mir_payload_ref)
	ZEND_MIR_VALUE_COPY(value_reference_cells, reference_cells,
		reference_cell_count, zend_mir_reference_cell_ref)
	ZEND_MIR_VALUE_COPY(value_storages, storages, storage_count,
		zend_mir_storage_ref)
	ZEND_MIR_VALUE_COPY(value_alias_relations, alias_relations,
		alias_relation_count, zend_mir_alias_relation_ref)
	ZEND_MIR_VALUE_COPY(value_ownership_events, ownership_events,
		ownership_event_count, zend_mir_ownership_event_ref)
	ZEND_MIR_VALUE_COPY(value_separation_plans, separation_plans,
		separation_plan_count, zend_mir_separation_plan_ref)
	ZEND_MIR_VALUE_COPY(value_call_transfers, call_transfers,
		call_transfer_count, zend_mir_call_transfer_ref)
#undef ZEND_MIR_VALUE_COPY
	staging->committed = true;
	return true;
}

typedef struct _zend_mir_w06_fingerprint_writer {
	uint32_t words[4];
} zend_mir_w06_fingerprint_writer;

static void zend_mir_w06_fingerprint_mix(
	zend_mir_w06_fingerprint_writer *writer, uint32_t value)
{
	static const uint32_t domains[4] = {
		UINT32_C(0x243f6a88), UINT32_C(0x85a308d3),
		UINT32_C(0x13198a2e), UINT32_C(0x03707344)
	};
	uint32_t word;
	uint32_t byte;

	for (word = 0; word < 4; word++) {
		uint32_t domain_value = value ^ domains[word];
		for (byte = 0; byte < 4; byte++) {
			writer->words[word] ^= domain_value & UINT32_C(0xff);
			writer->words[word] *= UINT32_C(16777619);
			domain_value >>= 8;
		}
	}
}

static bool zend_mir_w06_fingerprint_write(
	void *context, const char *bytes, size_t length)
{
	zend_mir_w06_fingerprint_writer *writer = context;
	size_t index;

	if (writer == NULL || (bytes == NULL && length != 0)) {
		return false;
	}
	for (index = 0; index < length; index++) {
		zend_mir_w06_fingerprint_mix(
			writer, (unsigned char) bytes[index]);
	}
	return true;
}

bool zend_mir_value_compute_module_fingerprint(
	const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics,
	uint32_t fingerprint[4])
{
	zend_mir_w06_fingerprint_writer digest = {{
		UINT32_C(2166136261),
		UINT32_C(3339451269),
		UINT32_C(2593831049),
		UINT32_C(1268118805)
	}};
	zend_mir_text_writer writer = {
		&digest, zend_mir_w06_fingerprint_write
	};

	if (view == NULL || diagnostics == NULL || fingerprint == NULL
			|| !zend_mir_dump_text(view, &writer, diagnostics)) {
		return false;
	}
	memcpy(fingerprint, digest.words, sizeof(digest.words));
	return true;
}

void zend_mir_module_init_value_mutator(zend_mir_module *module)
{
	memset(&module->value_mutator, 0, sizeof(module->value_mutator));
	module->value_mutator.contract_version = ZEND_MIR_W06_CONTRACT_VERSION;
	module->value_mutator.context = module;
	module->value_mutator.add_storage = zend_mir_value_stage_storage;
	module->value_mutator.add_payload = zend_mir_value_stage_payload;
	module->value_mutator.add_reference_cell =
		zend_mir_value_stage_reference_cell;
	module->value_mutator.add_alias_relation =
		zend_mir_value_stage_alias_relation;
	module->value_mutator.add_ownership_event =
		zend_mir_value_stage_ownership_event;
	module->value_mutator.add_separation_plan =
		zend_mir_value_stage_separation_plan;
	module->value_mutator.add_call_transfer =
		zend_mir_value_stage_call_transfer;
	module->value_mutator.add_executable_operation =
		zend_mir_value_stage_executable_operation;
}

zend_mir_value_mutator *zend_mir_module_get_value_mutator(
	zend_mir_module *module)
{
	if (zend_mir_module_require_building(module)
			&& module->value_mutator.contract_version == 0) {
		zend_mir_module_init_value_view(module);
		zend_mir_module_init_value_mutator(module);
	}
	return zend_mir_module_require_building(module)
		&& !module->value_staging.committed
		? &module->value_mutator : NULL;
}

const zend_mir_value_view *zend_mir_module_get_value_view(
	const zend_mir_module *module)
{
	return module != NULL && module->state != ZEND_MIR_MODULE_FAILED
		&& module->value_staging.committed
		? &module->value_view : NULL;
}

zend_mir_alias_relation zend_mir_value_merge_alias_relation(
	zend_mir_alias_relation left, zend_mir_alias_relation right)
{
	if (left < ZEND_MIR_ALIAS_MUST || left > ZEND_MIR_ALIAS_NONE
			|| right < ZEND_MIR_ALIAS_MUST || right > ZEND_MIR_ALIAS_NONE) {
		return ZEND_MIR_ALIAS_RELATION_INVALID;
	}
	if (left == ZEND_MIR_ALIAS_MUST && right == ZEND_MIR_ALIAS_MUST) {
		return ZEND_MIR_ALIAS_MUST;
	}
	/* A no-alias merge needs a proof that this helper does not carry. */
	return ZEND_MIR_ALIAS_MAY;
}

zend_mir_refcount_state zend_mir_value_merge_refcount_state(
	zend_mir_refcount_state left, zend_mir_refcount_state right)
{
	if (!zend_mir_value_refcount_valid(left)
			|| !zend_mir_value_refcount_valid(right)) {
		return ZEND_MIR_REFCOUNT_STATE_INVALID;
	}
	if (left == right) {
		return left;
	}
	if ((left == ZEND_MIR_REFCOUNT_UNIQUE
			&& right == ZEND_MIR_REFCOUNT_SHARED)
			|| (left == ZEND_MIR_REFCOUNT_SHARED
				&& right == ZEND_MIR_REFCOUNT_UNIQUE)) {
		return ZEND_MIR_REFCOUNT_SHARED;
	}
	return ZEND_MIR_REFCOUNT_UNKNOWN;
}

bool zend_mir_value_merge_storage_state(
	const zend_mir_storage_ref *left,
	const zend_mir_storage_ref *right,
	zend_mir_storage_ref *out)
{
	if (left == NULL || right == NULL || out == NULL
			|| left->kind != right->kind
			|| left->state != right->state
			|| left->category != right->category
			|| left->payload_id != right->payload_id
			|| left->reference_cell_id != right->reference_cell_id
			|| left->indirect_target_id != right->indirect_target_id) {
		return false;
	}
	*out = *left;
	out->id = ZEND_MIR_ID_INVALID;
	return true;
}
