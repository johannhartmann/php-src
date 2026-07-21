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

#include "../zend_mir_values.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Core/zend_mir_module_internal.h"

#define ZEND_MIR_W06_VIEW_LIMIT UINT32_C(1048576)

static bool zend_mir_w06_transition_valid(
	zend_mir_transfer_action action,
	zend_mir_refcount_state before_state,
	zend_mir_refcount_state after_state,
	bool cleanup_obligation)
{
	if (before_state < ZEND_MIR_REFCOUNT_IMMORTAL
			|| before_state > ZEND_MIR_REFCOUNT_UNKNOWN
			|| after_state < ZEND_MIR_REFCOUNT_IMMORTAL
			|| after_state > ZEND_MIR_REFCOUNT_UNKNOWN) {
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

static int zend_mir_w06_compare_u32(const void *left, const void *right)
{
	uint32_t lhs = *(const uint32_t *) left;
	uint32_t rhs = *(const uint32_t *) right;
	return lhs < rhs ? -1 : lhs > rhs;
}

static uint32_t zend_mir_w06_find_id(
	const uint32_t *ids, uint32_t count, uint32_t id)
{
	const uint32_t *match = bsearch(
		&id, ids, count, sizeof(*ids), zend_mir_w06_compare_u32);
	return match != NULL ? (uint32_t) (match - ids) : UINT32_MAX;
}

static uint32_t zend_mir_w06_alias_root(uint32_t *parents, uint32_t id)
{
	while (parents[id] != id) {
		parents[id] = parents[parents[id]];
		id = parents[id];
	}
	return id;
}

typedef struct _zend_mir_w06_alias_key {
	uint32_t left_id;
	uint32_t right_id;
	zend_mir_alias_relation relation;
} zend_mir_w06_alias_key;

static int zend_mir_w06_compare_alias_key(
	const void *left, const void *right)
{
	const zend_mir_w06_alias_key *lhs = left;
	const zend_mir_w06_alias_key *rhs = right;
	if (lhs->left_id != rhs->left_id) {
		return lhs->left_id < rhs->left_id ? -1 : 1;
	}
	if (lhs->right_id != rhs->right_id) {
		return lhs->right_id < rhs->right_id ? -1 : 1;
	}
	return 0;
}

static bool zend_mir_w06_alias_relations_valid(
	const zend_mir_alias_relation_ref *relations, uint32_t count)
{
	uint32_t *allocation;
	uint32_t *ids;
	uint32_t *parents;
	zend_mir_w06_alias_key *keys;
	uint32_t id_count = 0;
	uint32_t index;
	bool valid = false;

	if ((relations == NULL && count != 0)
			|| count > ZEND_MIR_W06_VIEW_LIMIT) {
		return false;
	}
	if (count == 0) {
		return true;
	}
	allocation = malloc((size_t) count * sizeof(uint32_t) * 4);
	if (allocation == NULL) {
		return false;
	}
	keys = malloc((size_t) count * sizeof(*keys));
	if (keys == NULL) {
		free(allocation);
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
	qsort(keys, count, sizeof(*keys), zend_mir_w06_compare_alias_key);
	for (index = 1; index < count; index++) {
		if (keys[index - 1].left_id == keys[index].left_id
				&& keys[index - 1].right_id == keys[index].right_id
				&& keys[index - 1].relation
					!= keys[index].relation) {
			goto done;
		}
	}
	qsort(ids, count * 2, sizeof(*ids), zend_mir_w06_compare_u32);
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
			uint32_t left = zend_mir_w06_alias_root(
				parents, zend_mir_w06_find_id(
					ids, id_count, relation->left_id));
			uint32_t right = zend_mir_w06_alias_root(
				parents, zend_mir_w06_find_id(
					ids, id_count, relation->right_id));
			parents[right] = left;
		}
	}
	for (index = 0; index < count; index++) {
		const zend_mir_alias_relation_ref *relation = &relations[index];
		if (relation->relation == ZEND_MIR_ALIAS_NONE
				&& zend_mir_w06_alias_root(
					parents, zend_mir_w06_find_id(
						ids, id_count, relation->left_id))
					== zend_mir_w06_alias_root(
						parents, zend_mir_w06_find_id(
							ids, id_count,
							relation->right_id))) {
			goto done;
		}
	}
	valid = true;
done:
	free(keys);
	free(allocation);
	return valid;
}

static bool zend_mir_w06_emit(
	const zend_mir_view *view, zend_mir_diagnostic_sink *sink,
	zend_mir_verify_w06_code code, const char *token, const char *detail)
{
	zend_mir_diagnostic diagnostic;

	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = code == ZEND_MIR_VERIFY_W06_SEPARATION_MISMATCH
		? ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS
		: ZEND_MIR_DIAGNOSTIC_INVALID_OWNERSHIP;
	diagnostic.severity = ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id =
		view != NULL && view->module_id != NULL
			? view->module_id(view->context) : ZEND_MIR_ID_INVALID;
	diagnostic.location.function_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.block_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	(void) snprintf(diagnostic.message, sizeof(diagnostic.message),
		"%s %s", token, detail);
	(void) zend_mir_diagnostic_sink_emit(sink, &diagnostic);
	return false;
}

static bool zend_mir_w06_view_complete(
	const zend_mir_view *view, const zend_mir_value_view *values)
{
	return view != NULL && view->context != NULL
		&& view->module_id != NULL
		&& view->source_position_count != NULL
		&& view->source_position_at != NULL
		&& values != NULL && values->context != NULL
		&& values->contract_version == ZEND_MIR_W06_CONTRACT_VERSION
		&& values->storage_count != NULL && values->storage_at != NULL
		&& values->payload_count != NULL && values->payload_at != NULL
		&& values->reference_cell_count != NULL
		&& values->reference_cell_at != NULL
		&& values->alias_relation_count != NULL
		&& values->alias_relation_at != NULL
		&& values->ownership_event_count != NULL
		&& values->ownership_event_at != NULL
		&& values->separation_plan_count != NULL
		&& values->separation_plan_at != NULL
		&& values->call_transfer_count != NULL
		&& values->call_transfer_at != NULL;
}

static bool zend_mir_w06_counts_bounded(const zend_mir_value_view *values)
{
	return values->storage_count(values->context) <= ZEND_MIR_W06_VIEW_LIMIT
		&& values->payload_count(values->context) <= ZEND_MIR_W06_VIEW_LIMIT
		&& values->reference_cell_count(values->context)
			<= ZEND_MIR_W06_VIEW_LIMIT
		&& values->alias_relation_count(values->context)
			<= ZEND_MIR_W06_VIEW_LIMIT
		&& values->ownership_event_count(values->context)
			<= ZEND_MIR_W06_VIEW_LIMIT
		&& values->separation_plan_count(values->context)
			<= ZEND_MIR_W06_VIEW_LIMIT
		&& values->call_transfer_count(values->context)
			<= ZEND_MIR_W06_VIEW_LIMIT;
}

static bool zend_mir_w06_source_exists(
	const zend_mir_view *view, zend_mir_source_position_id id)
{
	uint32_t index;

	for (index = 0; index < view->source_position_count(view->context);
			index++) {
		zend_mir_source_position_ref source;
		if (!view->source_position_at(view->context, index, &source)) {
			return false;
		}
		if (source.id == id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w06_resolve_payload(
	const zend_mir_value_view *values,
	zend_mir_storage_id storage_id,
	zend_mir_payload_id *payload_id)
{
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t reference_count =
		values->reference_cell_count(values->context);
	uint32_t payload_count = values->payload_count(values->context);
	uint32_t hops;

	if (payload_id == NULL || storage_id >= storage_count) {
		return false;
	}
	for (hops = 0; hops <= storage_count; hops++) {
		zend_mir_storage_ref storage;
		if (!values->storage_at(values->context, storage_id, &storage)) {
			return false;
		}
		if (storage.state == ZEND_MIR_STORAGE_DIRECT) {
			*payload_id = storage.payload_id;
			return *payload_id < payload_count;
		}
		if (storage.state == ZEND_MIR_STORAGE_REFERENCE) {
			zend_mir_reference_cell_ref cell;
			if (storage.reference_cell_id >= reference_count
					|| !values->reference_cell_at(
						values->context,
						storage.reference_cell_id, &cell)) {
				return false;
			}
			storage_id = cell.payload_storage_id;
			continue;
		}
		if (storage.state != ZEND_MIR_STORAGE_INDIRECT
				|| storage.indirect_target_id >= storage_count) {
			return false;
		}
		storage_id = storage.indirect_target_id;
	}
	return false;
}

static bool zend_mir_w06_verify_payloads(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t count = values->payload_count(values->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_payload_ref payload;
		if (!values->payload_at(values->context, index, &payload)
				|| payload.id != index
				|| payload.category < ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
				|| payload.category > ZEND_MIR_VALUE_CATEGORY_UNKNOWN
				|| payload.refcount_state < ZEND_MIR_REFCOUNT_IMMORTAL
				|| payload.refcount_state > ZEND_MIR_REFCOUNT_UNKNOWN
				|| (payload.category
					== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
					&& payload.cleanup_obligation)) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_STORAGE_MISMATCH,
				ZEND_MIRV_TOKEN_W06_STORAGE_MISMATCH,
				"invalid payload table");
		}
	}
	return true;
}

static bool zend_mir_w06_verify_storages(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t payload_count = values->payload_count(values->context);
	uint32_t reference_count =
		values->reference_cell_count(values->context);
	uint32_t index;

	for (index = 0; index < storage_count; index++) {
		zend_mir_storage_ref storage;
		bool valid;
		if (!values->storage_at(values->context, index, &storage)) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_STORAGE_MISMATCH,
				ZEND_MIRV_TOKEN_W06_STORAGE_MISMATCH,
				"storage callback failed");
		}
		valid = storage.id == index
			&& storage.kind >= ZEND_MIR_STORAGE_FRAME_SLOT
			&& storage.kind <= ZEND_MIR_STORAGE_CALL_RETURN_SLOT
			&& storage.category >= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
			&& storage.category <= ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
		switch (storage.state) {
			case ZEND_MIR_STORAGE_UNDEF:
				valid = valid
					&& !zend_mir_id_is_valid(storage.payload_id)
					&& !zend_mir_id_is_valid(
						storage.reference_cell_id)
					&& !zend_mir_id_is_valid(
						storage.indirect_target_id);
				break;
			case ZEND_MIR_STORAGE_DIRECT: {
				zend_mir_payload_ref payload;
				valid = valid && storage.payload_id < payload_count
					&& !zend_mir_id_is_valid(
						storage.reference_cell_id)
					&& !zend_mir_id_is_valid(
						storage.indirect_target_id)
					&& values->payload_at(values->context,
						storage.payload_id, &payload)
					&& payload.category == storage.category;
				break;
			}
			case ZEND_MIR_STORAGE_REFERENCE:
				valid = valid
					&& !zend_mir_id_is_valid(storage.payload_id)
					&& storage.reference_cell_id < reference_count
					&& !zend_mir_id_is_valid(
						storage.indirect_target_id)
					&& storage.category
						== ZEND_MIR_VALUE_REFERENCE_CELL;
				break;
			case ZEND_MIR_STORAGE_INDIRECT:
				valid = valid
					&& !zend_mir_id_is_valid(storage.payload_id)
					&& !zend_mir_id_is_valid(
						storage.reference_cell_id)
					&& storage.indirect_target_id < storage_count;
				break;
			default:
				valid = false;
				break;
		}
		if (!valid) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_STORAGE_MISMATCH,
				ZEND_MIRV_TOKEN_W06_STORAGE_MISMATCH,
				"storage state does not match its identity fields");
		}
	}
	for (index = 0; index < storage_count; index++) {
		uint32_t cursor = index;
		uint32_t hops;
		for (hops = 0; hops <= storage_count; hops++) {
			zend_mir_storage_ref storage;
			if (!values->storage_at(values->context, cursor, &storage)) {
				return zend_mir_w06_emit(view, diagnostics,
					ZEND_MIR_VERIFY_W06_INDIRECT_MISMATCH,
					ZEND_MIRV_TOKEN_W06_INDIRECT_MISMATCH,
					"indirect target lookup failed");
			}
			if (storage.state != ZEND_MIR_STORAGE_INDIRECT) {
				break;
			}
			cursor = storage.indirect_target_id;
			if (hops == storage_count) {
				return zend_mir_w06_emit(view, diagnostics,
					ZEND_MIR_VERIFY_W06_INDIRECT_MISMATCH,
					ZEND_MIRV_TOKEN_W06_INDIRECT_MISMATCH,
					"indirect storage cycle");
			}
		}
	}
	return true;
}

static bool zend_mir_w06_verify_references(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t count = values->reference_cell_count(values->context);
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_reference_cell_ref cell;
		zend_mir_storage_ref storage;
		if (!values->reference_cell_at(values->context, index, &cell)
				|| cell.id != index
				|| cell.payload_storage_id >= storage_count
				|| !values->storage_at(values->context,
					cell.payload_storage_id, &storage)
				|| storage.kind
					!= ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT
				|| storage.state != ZEND_MIR_STORAGE_DIRECT
				|| !zend_mir_id_is_valid(cell.alias_class_id)
				|| !zend_mir_w06_source_exists(
					view, cell.creation_source_id)
				|| cell.ownership < 0
				|| cell.ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT
				|| !cell.cleanup_obligation) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_REFERENCE_MISMATCH,
				ZEND_MIRV_TOKEN_W06_REFERENCE_MISMATCH,
				"invalid reference-cell table");
		}
	}
	return true;
}

static bool zend_mir_w06_verify_aliases(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t count = values->alias_relation_count(values->context);
	zend_mir_alias_relation_ref *relations;
	uint32_t index;

	if (count == 0) {
		return true;
	}
	relations = malloc((size_t) count * sizeof(*relations));
	if (relations == NULL) {
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH,
			ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH,
			"alias table allocation failed");
	}
	for (index = 0; index < count; index++) {
		if (!values->alias_relation_at(
				values->context, index, &relations[index])) {
			free(relations);
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH,
				ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH,
				"alias callback failed");
		}
	}
	if (!zend_mir_w06_alias_relations_valid(relations, count)) {
		free(relations);
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH,
			ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH,
			"invalid or contradictory alias closure");
	}
	free(relations);
	return true;
}

static bool zend_mir_w06_verify_events(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t count = values->ownership_event_count(values->context);
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t payload_count = values->payload_count(values->context);
	zend_mir_refcount_state *states;
	bool *borrowed;
	bool *consumed;
	uint32_t index;

	if (count == 0) {
		return true;
	}
	if (payload_count == 0) {
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH,
			ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH,
			"ownership events require payloads");
	}
	states = malloc((size_t) payload_count * sizeof(*states));
	borrowed = calloc(payload_count, sizeof(*borrowed));
	consumed = calloc(payload_count, sizeof(*consumed));
	if (states == NULL || borrowed == NULL || consumed == NULL) {
		free(states);
		free(borrowed);
		free(consumed);
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH,
			ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH,
			"ownership-state allocation failed");
	}
	for (index = 0; index < payload_count; index++) {
		zend_mir_payload_ref payload;
		if (!values->payload_at(values->context, index, &payload)) {
			free(states);
			free(borrowed);
			free(consumed);
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH,
				ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH,
				"payload callback failed");
		}
		states[index] = payload.refcount_state;
	}
	for (index = 0; index < count; index++) {
		zend_mir_ownership_event_ref event;
		zend_mir_payload_ref payload;
		zend_mir_payload_id source_payload;
		zend_mir_payload_id target_payload = ZEND_MIR_ID_INVALID;
		bool target_required;
		if (!values->ownership_event_at(values->context, index, &event)) {
			free(states);
			free(borrowed);
			free(consumed);
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH,
				ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH,
				"ownership-event callback failed");
		}
		target_required =
			event.action == ZEND_MIR_TRANSFER_COPY_ADDREF
			|| event.action == ZEND_MIR_TRANSFER_MOVE
			|| event.action == ZEND_MIR_TRANSFER_FROM_CALLEE;
		if (event.id != index
				|| event.source_storage_id >= storage_count
				|| event.payload_id >= payload_count
				|| event.action < ZEND_MIR_TRANSFER_BORROW
				|| event.action > ZEND_MIR_TRANSFER_FROM_CALLEE
				|| !zend_mir_w06_resolve_payload(
					values, event.source_storage_id,
					&source_payload)
				|| source_payload != event.payload_id
				|| !values->payload_at(
					values->context, event.payload_id, &payload)
				|| states[event.payload_id] != event.before_state
				|| !zend_mir_w06_transition_valid(
					event.action, event.before_state,
					event.after_state, event.cleanup_obligation)
				|| (event.cleanup_obligation
					&& !payload.cleanup_obligation)
				|| (target_required
					&& (event.target_storage_id >= storage_count
						|| event.target_storage_id
							== event.source_storage_id
						|| !zend_mir_w06_resolve_payload(
							values, event.target_storage_id,
							&target_payload)
						|| target_payload != event.payload_id))
				|| (!target_required
					&& zend_mir_id_is_valid(
						event.target_storage_id))
				|| consumed[event.payload_id]
				|| ((event.action == ZEND_MIR_TRANSFER_RELEASE
						|| event.action == ZEND_MIR_TRANSFER_MOVE
						|| event.action
							== ZEND_MIR_TRANSFER_TO_CALLEE)
					&& borrowed[event.payload_id])) {
			free(states);
			free(borrowed);
			free(consumed);
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH,
				ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH,
				"invalid ownership transition");
		}
		states[event.payload_id] = event.after_state;
		if (event.action == ZEND_MIR_TRANSFER_BORROW) {
			borrowed[event.payload_id] = true;
		}
		if (event.action == ZEND_MIR_TRANSFER_MOVE
				|| event.action == ZEND_MIR_TRANSFER_RELEASE
				|| event.action == ZEND_MIR_TRANSFER_TO_CALLEE) {
			consumed[event.payload_id] = true;
		}
	}
	free(states);
	free(borrowed);
	free(consumed);
	return true;
}

static bool zend_mir_w06_verify_separations(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t count = values->separation_plan_count(values->context);
	uint32_t payload_count = values->payload_count(values->context);
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_separation_plan_ref plan;
		zend_mir_payload_ref source;
		zend_mir_payload_ref result;
		zend_mir_payload_id source_payload;
		bool valid;
		if (!values->separation_plan_at(
				values->context, index, &plan)) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_SEPARATION_MISMATCH,
				ZEND_MIRV_TOKEN_W06_SEPARATION_MISMATCH,
				"separation callback failed");
		}
		valid = plan.id == index
			&& plan.source_payload_id < payload_count
			&& plan.source_storage_id < storage_count
			&& plan.reason >= ZEND_MIR_SEPARATION_EXPLICIT
			&& plan.reason <= ZEND_MIR_SEPARATION_CALL_BOUNDARY
			&& plan.uniqueness_fact >= ZEND_MIR_REFCOUNT_IMMORTAL
			&& plan.uniqueness_fact <= ZEND_MIR_REFCOUNT_UNKNOWN
			&& plan.required >= ZEND_MIR_SEPARATION_REQUIRED_NO
			&& plan.required <= ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN
			&& zend_mir_w06_resolve_payload(
				values, plan.source_storage_id, &source_payload)
			&& source_payload == plan.source_payload_id
			&& values->payload_at(
				values->context, plan.source_payload_id, &source)
			&& source.refcount_state == plan.uniqueness_fact
			&& ((plan.uniqueness_fact == ZEND_MIR_REFCOUNT_UNIQUE
					&& plan.required
						== ZEND_MIR_SEPARATION_REQUIRED_NO)
				|| (plan.uniqueness_fact == ZEND_MIR_REFCOUNT_SHARED
					&& plan.required
						== ZEND_MIR_SEPARATION_REQUIRED_YES)
				|| ((plan.uniqueness_fact
							== ZEND_MIR_REFCOUNT_UNKNOWN
						|| plan.uniqueness_fact
							== ZEND_MIR_REFCOUNT_IMMORTAL)
					&& plan.required
						== ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN));
		if (plan.required == ZEND_MIR_SEPARATION_REQUIRED_YES) {
			valid = valid && plan.result_payload_id < payload_count
				&& plan.result_payload_id != plan.source_payload_id
				&& values->payload_at(
					values->context, plan.result_payload_id, &result)
				&& result.category == source.category
				&& result.refcount_state == ZEND_MIR_REFCOUNT_UNIQUE
				&& plan.clone_execution_required;
		} else if (plan.required == ZEND_MIR_SEPARATION_REQUIRED_NO) {
			valid = valid
				&& (!zend_mir_id_is_valid(plan.result_payload_id)
					|| plan.result_payload_id
						== plan.source_payload_id)
				&& !plan.clone_execution_required;
		} else {
			valid = valid
				&& !zend_mir_id_is_valid(plan.result_payload_id)
				&& plan.clone_execution_required;
		}
		if (!valid) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_SEPARATION_MISMATCH,
				ZEND_MIRV_TOKEN_W06_SEPARATION_MISMATCH,
				"invalid separation plan");
		}
	}
	return true;
}

static bool zend_mir_w06_verify_call_transfers(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	const zend_mir_module *module = zend_mir_module_from_value_view(values);
	const zend_mir_call_view *calls =
		zend_mir_module_call_view_from_view(view);
	uint32_t count = values->call_transfer_count(values->context);
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t reference_count =
		values->reference_cell_count(values->context);
	uint32_t index;

	if (module == NULL
			|| ((calls == NULL) != (module->call_sites.count == 0))
			|| (calls != NULL && calls != &module->call_view)
			|| count != module->call_arguments.count
			|| (module->call_staging.committed
				&& (module->call_sites.items == NULL
					|| (module->call_arguments.count != 0
						&& module->call_arguments.items == NULL)
					|| module->call_targets.items == NULL))
			|| (!module->call_staging.committed && count != 0)) {
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH,
			ZEND_MIRV_TOKEN_W06_CALL_TRANSFER_MISMATCH,
			"call-transfer table is not bound to the W05 call model");
	}
	for (index = 0; index < count; index++) {
		zend_mir_call_transfer_ref transfer;
		zend_mir_call_argument_ref argument;
		zend_mir_call_site_ref site;
		zend_mir_call_target_ref target;
		zend_mir_storage_ref argument_storage;
		zend_mir_storage_ref return_storage;
		bool argument_is_reference;
		bool return_is_reference;
		if (!values->call_transfer_at(values->context, index, &transfer)
				|| !calls->call_argument_at(
					calls->context, index, &argument)
				|| argument.id != index
				|| argument.call_site_id
					>= calls->call_site_count(calls->context)
				|| !calls->call_site_at(
					calls->context, argument.call_site_id, &site)
				|| site.id != argument.call_site_id
				|| site.target_id
					>= calls->call_target_count(calls->context)
				|| !calls->call_target_at(
					calls->context, site.target_id, &target)
				|| target.id != site.target_id
				|| site.arguments.offset > index
				|| argument.ordinal >= site.arguments.count
				|| site.arguments.offset + argument.ordinal != index
				|| transfer.call_site_id != site.id
				|| transfer.argument_ordinal != argument.ordinal
				|| transfer.parameter_modes.count != target.num_args
				|| argument.ordinal >= transfer.parameter_modes.count
				|| transfer.parameter_modes.offset
					> UINT32_MAX - transfer.parameter_modes.count
				|| transfer.argument_storage_id >= storage_count
				|| !values->storage_at(values->context,
					transfer.argument_storage_id, &argument_storage)
				|| transfer.return_storage_id >= storage_count
				|| !values->storage_at(values->context,
					transfer.return_storage_id, &return_storage)) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH,
				ZEND_MIRV_TOKEN_W06_CALL_TRANSFER_MISMATCH,
				"invalid call-transfer span or identity");
		}
		argument_is_reference =
			argument_storage.state == ZEND_MIR_STORAGE_REFERENCE;
		return_is_reference =
			return_storage.state == ZEND_MIR_STORAGE_REFERENCE;
		if (argument_is_reference
					!= zend_mir_id_is_valid(
						transfer.argument_reference_cell_id)
				|| (argument_is_reference
					&& (transfer.argument_reference_cell_id
							>= reference_count
						|| argument_storage.reference_cell_id
							!= transfer
								.argument_reference_cell_id))
				|| (argument_is_reference
					? transfer.argument_action
						!= ZEND_MIR_TRANSFER_TO_CALLEE
					: argument_storage.category
							== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						? transfer.argument_action
							!= ZEND_MIR_TRANSFER_BORROW
						: transfer.argument_action
							!= ZEND_MIR_TRANSFER_COPY_ADDREF)
				|| return_storage.kind
					!= ZEND_MIR_STORAGE_CALL_RETURN_SLOT
				|| return_is_reference
					!= zend_mir_id_is_valid(
						transfer.return_reference_cell_id)
				|| (return_is_reference
					&& (transfer.return_reference_cell_id
							>= reference_count
						|| return_storage.reference_cell_id
							!= transfer.return_reference_cell_id))
				|| transfer.return_action
					!= ZEND_MIR_TRANSFER_FROM_CALLEE) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH,
				ZEND_MIRV_TOKEN_W06_CALL_TRANSFER_MISMATCH,
				"call-transfer action does not match value storage");
		}
		if (argument.ordinal != 0) {
			zend_mir_call_transfer_ref first;
			if (!values->call_transfer_at(
					values->context, index - argument.ordinal, &first)
					|| first.call_site_id != transfer.call_site_id
					|| first.parameter_modes.offset
						!= transfer.parameter_modes.offset
					|| first.parameter_modes.count
						!= transfer.parameter_modes.count
					|| first.return_storage_id
						!= transfer.return_storage_id
					|| first.return_reference_cell_id
						!= transfer.return_reference_cell_id
					|| first.return_action != transfer.return_action) {
				return zend_mir_w06_emit(view, diagnostics,
					ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH,
					ZEND_MIRV_TOKEN_W06_CALL_TRANSFER_MISMATCH,
					"inconsistent per-site call-transfer projection");
			}
		}
	}
	return true;
}

bool zend_mir_verify_w06_values(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	if (!zend_mir_w06_view_complete(view, values)
			|| !zend_mir_w06_counts_bounded(values)) {
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_STORAGE_MISMATCH,
			ZEND_MIRV_TOKEN_W06_STORAGE_MISMATCH,
			"incomplete or unbounded W06 value view");
	}
	return zend_mir_w06_verify_payloads(view, values, diagnostics)
		&& zend_mir_w06_verify_storages(view, values, diagnostics)
		&& zend_mir_w06_verify_references(view, values, diagnostics)
		&& zend_mir_w06_verify_aliases(view, values, diagnostics)
		&& zend_mir_w06_verify_events(view, values, diagnostics)
		&& zend_mir_w06_verify_separations(view, values, diagnostics)
		&& zend_mir_w06_verify_call_transfers(view, values, diagnostics);
}
