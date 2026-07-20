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
ZEND_MIR_VALUE_STAGE(zend_mir_value_stage_verifier_receipt,
	zend_mir_value_verifier_receipt_ref, verifier_receipts,
	verifier_receipt_count, verifier_receipt_capacity)

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
				|| cell->ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_value_validate_aliases(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;
	uint32_t other;

	for (index = 0; index < staging->alias_relation_count; index++) {
		const zend_mir_alias_relation_ref *relation =
			&staging->alias_relations[index];
		if (!zend_mir_id_is_valid(relation->left_id)
				|| !zend_mir_id_is_valid(relation->right_id)
				|| relation->relation < ZEND_MIR_ALIAS_MUST
				|| relation->relation > ZEND_MIR_ALIAS_NONE
				|| (relation->left_id == relation->right_id
					&& relation->relation != ZEND_MIR_ALIAS_MUST)
				|| (relation->relation == ZEND_MIR_ALIAS_NONE
					&& relation->proof_id == 0)) {
			return false;
		}
		for (other = 0; other < index; other++) {
			const zend_mir_alias_relation_ref *previous =
				&staging->alias_relations[other];
			bool same_pair =
				(previous->left_id == relation->left_id
					&& previous->right_id == relation->right_id)
				|| (previous->left_id == relation->right_id
					&& previous->right_id == relation->left_id);
			if (same_pair && previous->relation != relation->relation) {
				return false;
			}
		}
	}
	return true;
}

static bool zend_mir_value_validate_events(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;

	for (index = 0; index < staging->ownership_event_count; index++) {
		const zend_mir_ownership_event_ref *event =
			&staging->ownership_events[index];
		bool target_required =
			event->action == ZEND_MIR_TRANSFER_COPY_ADDREF
			|| event->action == ZEND_MIR_TRANSFER_MOVE
			|| event->action == ZEND_MIR_TRANSFER_FROM_CALLEE;
		bool borrowed = false;
		bool consumed = false;
		uint32_t previous_index;

		for (previous_index = 0; previous_index < index;
				previous_index++) {
			const zend_mir_ownership_event_ref *previous =
				&staging->ownership_events[previous_index];
			if (previous->source_storage_id
					!= event->source_storage_id) {
				continue;
			}
			if (previous->action == ZEND_MIR_TRANSFER_BORROW) {
				borrowed = true;
			}
			if (previous->action == ZEND_MIR_TRANSFER_MOVE
					|| previous->action == ZEND_MIR_TRANSFER_RELEASE
					|| previous->action
						== ZEND_MIR_TRANSFER_TO_CALLEE) {
				consumed = true;
			}
		}
		if (event->id != index
				|| event->source_storage_id >= staging->storage_count
				|| event->payload_id >= staging->payload_count
				|| event->action < ZEND_MIR_TRANSFER_BORROW
				|| event->action > ZEND_MIR_TRANSFER_FROM_CALLEE
				|| !zend_mir_value_refcount_valid(event->before_state)
				|| !zend_mir_value_refcount_valid(event->after_state)
				|| consumed
				|| (target_required
					&& (event->target_storage_id
							>= staging->storage_count
						|| event->target_storage_id
							== event->source_storage_id))
				|| ((event->action == ZEND_MIR_TRANSFER_RELEASE
						|| event->action == ZEND_MIR_TRANSFER_MOVE
						|| event->action
							== ZEND_MIR_TRANSFER_TO_CALLEE)
					&& borrowed)) {
			return false;
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
		if (plan->id != index
				|| plan->source_payload_id >= staging->payload_count
				|| plan->source_storage_id >= staging->storage_count
				|| plan->reason < ZEND_MIR_SEPARATION_EXPLICIT
				|| plan->reason > ZEND_MIR_SEPARATION_CALL_BOUNDARY
				|| !zend_mir_value_refcount_valid(
					plan->uniqueness_fact)
				|| plan->required < ZEND_MIR_SEPARATION_REQUIRED_NO
				|| plan->required
					> ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN) {
			return false;
		}
		if (plan->required == ZEND_MIR_SEPARATION_REQUIRED_YES) {
			if (plan->result_payload_id >= staging->payload_count
					|| plan->result_payload_id
						== plan->source_payload_id
					|| plan->container_execution_debt
						!= ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION) {
				return false;
			}
		} else if (zend_mir_id_is_valid(plan->result_payload_id)
				|| (zend_mir_id_is_valid(plan->container_execution_debt)
					&& plan->container_execution_debt
						!= ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_value_validate_call_transfers(
	const zend_mir_core_value_staging *staging)
{
	uint32_t index;

	for (index = 0; index < staging->call_transfer_count; index++) {
		const zend_mir_call_transfer_ref *transfer =
			&staging->call_transfers[index];
		if (!zend_mir_id_is_valid(transfer->call_site_id)
				|| transfer->parameter_modes.count == 0
				|| transfer->parameter_modes.offset
					> UINT32_MAX - transfer->parameter_modes.count
				|| transfer->resolved_debt_ids.offset
					> UINT32_MAX - transfer->resolved_debt_ids.count
				|| transfer->argument_storage_id
					>= staging->storage_count
				|| (zend_mir_id_is_valid(
						transfer->argument_reference_cell_id)
					&& transfer->argument_reference_cell_id
						>= staging->reference_cell_count)
				|| transfer->argument_action < ZEND_MIR_TRANSFER_BORROW
				|| transfer->argument_action
					> ZEND_MIR_TRANSFER_FROM_CALLEE
				|| transfer->return_storage_id >= staging->storage_count
				|| (zend_mir_id_is_valid(
						transfer->return_reference_cell_id)
					&& transfer->return_reference_cell_id
						>= staging->reference_cell_count)
				|| transfer->return_action < ZEND_MIR_TRANSFER_BORROW
				|| transfer->return_action
					> ZEND_MIR_TRANSFER_FROM_CALLEE) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_value_validate_receipts(
	const zend_mir_core_value_staging *staging)
{
	bool seen[6] = { false, false, false, false, false, false };
	const zend_mir_verifier_receipt_ref *first = NULL;
	uint32_t index;

	for (index = 0; index < staging->verifier_receipt_count; index++) {
		const zend_mir_value_verifier_receipt_ref *wrapped =
			&staging->verifier_receipts[index];
		const zend_mir_verifier_receipt_ref *receipt = &wrapped->receipt;
		uint32_t expected_version;

		if (wrapped->id != index
				|| receipt->verifier_id < ZEND_MIR_VERIFIER_STRUCTURAL
				|| receipt->verifier_id
					> ZEND_MIR_VERIFIER_VALUE_REFERENCE
				|| receipt->status != ZEND_MIR_VERIFIER_STATUS_PASS
				|| seen[receipt->verifier_id]) {
			return false;
		}
		expected_version =
			receipt->verifier_id == ZEND_MIR_VERIFIER_CONTROL_FLOW
				? ZEND_MIR_W04_CONTRACT_VERSION
			: receipt->verifier_id == ZEND_MIR_VERIFIER_CALL_MODEL
				? ZEND_MIR_W05_CONTRACT_VERSION
			: receipt->verifier_id == ZEND_MIR_VERIFIER_VALUE_REFERENCE
				? ZEND_MIR_W06_CONTRACT_VERSION
				: ZEND_MIR_CONTRACT_VERSION;
		if (receipt->verifier_contract_version != expected_version) {
			return false;
		}
		if (first == NULL) {
			first = receipt;
		} else if (memcmp(first->module_fingerprint,
					receipt->module_fingerprint,
					sizeof(first->module_fingerprint)) != 0
				|| memcmp(first->source_fingerprint,
					receipt->source_fingerprint,
					sizeof(first->source_fingerprint)) != 0) {
			return false;
		}
		seen[receipt->verifier_id] = true;
	}
	return seen[ZEND_MIR_VERIFIER_STRUCTURAL]
		&& seen[ZEND_MIR_VERIFIER_SCALAR]
		&& seen[ZEND_MIR_VERIFIER_CONTROL_FLOW]
		&& seen[ZEND_MIR_VERIFIER_VALUE_REFERENCE]
		&& (staging->call_transfer_count == 0
			|| seen[ZEND_MIR_VERIFIER_CALL_MODEL]);
}

static bool zend_mir_value_validate_capabilities(
	const zend_mir_capability_id *capabilities, uint32_t capability_count,
	const zend_mir_semantic_debt_id *debts, uint32_t debt_count)
{
	static const zend_mir_capability_id expected_capabilities[] = {
		ZEND_MIR_CAP_ZVAL_STORAGE_MODEL,
		ZEND_MIR_CAP_REFERENCE_CELL_MODEL,
		ZEND_MIR_CAP_INDIRECT_SLOT_MODEL,
		ZEND_MIR_CAP_REFCOUNT_TRANSFER_MODEL,
		ZEND_MIR_CAP_ALIAS_PARTITION_MODEL,
		ZEND_MIR_CAP_SEPARATION_PROTOCOL_MODEL,
		ZEND_MIR_CAP_DIRECT_USER_CALL_REFERENCE_TRANSFER_MODEL,
		ZEND_MIR_CAP_REFCOUNTED_CALL_RESULT_MODEL
	};
	static const zend_mir_semantic_debt_id expected_debts[] = {
		ZEND_MIR_DEBT_CALL_EXECUTION,
		ZEND_MIR_DEBT_INTERNAL_C_ABI_INTEROP,
		ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION,
		ZEND_MIR_DEBT_STRING_AND_ARRAY_OPERATION_SEMANTICS,
		ZEND_MIR_DEBT_OBJECT_LIFECYCLE,
		ZEND_MIR_DEBT_DESTRUCTOR_EXCEPTION_CLEANUP,
		ZEND_MIR_DEBT_RUNTIME_REFERENCE_BINDING,
		ZEND_MIR_DEBT_DYNAMIC_SYMBOL_TABLE_ALIASING
	};

	return capability_count
			== sizeof(expected_capabilities)
				/ sizeof(expected_capabilities[0])
		&& debt_count
			== sizeof(expected_debts) / sizeof(expected_debts[0])
		&& capabilities != NULL && debts != NULL
		&& memcmp(capabilities, expected_capabilities,
			sizeof(expected_capabilities)) == 0
		&& memcmp(debts, expected_debts, sizeof(expected_debts)) == 0;
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

bool zend_mir_module_commit_value_model(
	zend_mir_module *module,
	const zend_mir_capability_id *capability_ids,
	uint32_t capability_count,
	const zend_mir_semantic_debt_id *semantic_debt_ids,
	uint32_t semantic_debt_count)
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
			|| !zend_mir_value_validate_capabilities(
				capability_ids, capability_count,
				semantic_debt_ids, semantic_debt_count)
			|| !zend_mir_value_validate_payloads(staging)
			|| !zend_mir_value_validate_storages(staging)
			|| !zend_mir_value_validate_references(staging)
			|| !zend_mir_value_validate_aliases(staging)
			|| !zend_mir_value_validate_events(staging)
			|| !zend_mir_value_validate_separations(staging)
			|| !zend_mir_value_validate_call_transfers(staging)
			|| !zend_mir_value_validate_receipts(staging)) {
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
	ZEND_MIR_VALUE_COPY(value_verifier_receipts, verifier_receipts,
		verifier_receipt_count, zend_mir_value_verifier_receipt_ref)
#undef ZEND_MIR_VALUE_COPY
	if (!zend_mir_value_copy_table(module, &module->value_capability_ids,
			capability_ids, capability_count,
			sizeof(*capability_ids), alignof(zend_mir_capability_id))
			|| !zend_mir_value_copy_table(
				module, &module->value_semantic_debt_ids,
				semantic_debt_ids, semantic_debt_count,
				sizeof(*semantic_debt_ids),
				alignof(zend_mir_semantic_debt_id))) {
		return false;
	}
	staging->committed = true;
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
	module->value_mutator.add_verifier_receipt =
		zend_mir_value_stage_verifier_receipt;
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
	return left == right ? left : ZEND_MIR_ALIAS_MAY;
}

zend_mir_refcount_state zend_mir_value_merge_refcount_state(
	zend_mir_refcount_state left, zend_mir_refcount_state right)
{
	if (!zend_mir_value_refcount_valid(left)
			|| !zend_mir_value_refcount_valid(right)) {
		return ZEND_MIR_REFCOUNT_STATE_INVALID;
	}
	return left == right ? left : ZEND_MIR_REFCOUNT_UNKNOWN;
}

bool zend_mir_value_merge_storage_state(
	const zend_mir_storage_ref *left,
	const zend_mir_storage_ref *right,
	zend_mir_storage_ref *out)
{
	if (left == NULL || right == NULL || out == NULL
			|| left->kind != right->kind
			|| left->state != right->state
			|| left->category != right->category) {
		return false;
	}
	*out = *left;
	out->id = ZEND_MIR_ID_INVALID;
	if (left->payload_id != right->payload_id) {
		out->payload_id = ZEND_MIR_ID_INVALID;
	}
	if (left->reference_cell_id != right->reference_cell_id) {
		out->reference_cell_id = ZEND_MIR_ID_INVALID;
	}
	if (left->indirect_target_id != right->indirect_target_id) {
		out->indirect_target_id = ZEND_MIR_ID_INVALID;
	}
	return true;
}
