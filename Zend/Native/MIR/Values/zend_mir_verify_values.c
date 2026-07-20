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
#include <string.h>

#include "../Core/zend_mir_module_internal.h"

#define ZEND_MIR_W06_VIEW_LIMIT UINT32_C(1048576)

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
		&& values->call_transfer_at != NULL
		&& values->verifier_receipt_count != NULL
		&& values->verifier_receipt_at != NULL;
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
			<= ZEND_MIR_W06_VIEW_LIMIT
		&& values->verifier_receipt_count(values->context) <= 5;
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
				|| cell.ownership >= ZEND_MIR_OWNERSHIP_STATE_COUNT) {
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
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_alias_relation_ref relation;
		uint32_t previous_index;
		if (!values->alias_relation_at(
				values->context, index, &relation)
				|| !zend_mir_id_is_valid(relation.left_id)
				|| !zend_mir_id_is_valid(relation.right_id)
				|| relation.relation < ZEND_MIR_ALIAS_MUST
				|| relation.relation > ZEND_MIR_ALIAS_NONE
				|| (relation.left_id == relation.right_id
					&& relation.relation != ZEND_MIR_ALIAS_MUST)
				|| (relation.relation == ZEND_MIR_ALIAS_NONE
					&& relation.proof_id == 0)) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH,
				ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH,
				"invalid alias proof");
		}
		for (previous_index = 0; previous_index < index;
				previous_index++) {
			zend_mir_alias_relation_ref previous;
			bool same_pair;
			if (!values->alias_relation_at(values->context,
					previous_index, &previous)) {
				return zend_mir_w06_emit(view, diagnostics,
					ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH,
					ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH,
					"alias callback failed");
			}
			same_pair =
				(previous.left_id == relation.left_id
					&& previous.right_id == relation.right_id)
				|| (previous.left_id == relation.right_id
					&& previous.right_id == relation.left_id);
			if (same_pair && previous.relation != relation.relation) {
				return zend_mir_w06_emit(view, diagnostics,
					ZEND_MIR_VERIFY_W06_ALIAS_MISMATCH,
					ZEND_MIRV_TOKEN_W06_ALIAS_MISMATCH,
					"contradictory alias relation");
			}
		}
	}
	return true;
}

static bool zend_mir_w06_prior_event_consumes(
	const zend_mir_value_view *values, uint32_t before,
	zend_mir_storage_id storage_id, bool *borrowed)
{
	uint32_t index;

	*borrowed = false;
	for (index = 0; index < before; index++) {
		zend_mir_ownership_event_ref event;
		if (!values->ownership_event_at(values->context, index, &event)) {
			return true;
		}
		if (event.source_storage_id != storage_id) {
			continue;
		}
		if (event.action == ZEND_MIR_TRANSFER_BORROW) {
			*borrowed = true;
		}
		if (event.action == ZEND_MIR_TRANSFER_MOVE
				|| event.action == ZEND_MIR_TRANSFER_RELEASE
				|| event.action == ZEND_MIR_TRANSFER_TO_CALLEE) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w06_verify_events(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t count = values->ownership_event_count(values->context);
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t payload_count = values->payload_count(values->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_ownership_event_ref event;
		bool borrowed;
		bool target_required;
		if (!values->ownership_event_at(values->context, index, &event)) {
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
				|| event.before_state < ZEND_MIR_REFCOUNT_IMMORTAL
				|| event.before_state > ZEND_MIR_REFCOUNT_UNKNOWN
				|| event.after_state < ZEND_MIR_REFCOUNT_IMMORTAL
				|| event.after_state > ZEND_MIR_REFCOUNT_UNKNOWN
				|| (target_required
					&& (event.target_storage_id >= storage_count
						|| event.target_storage_id
							== event.source_storage_id))
				|| zend_mir_w06_prior_event_consumes(
					values, index, event.source_storage_id,
					&borrowed)
				|| ((event.action == ZEND_MIR_TRANSFER_RELEASE
						|| event.action == ZEND_MIR_TRANSFER_MOVE
						|| event.action
							== ZEND_MIR_TRANSFER_TO_CALLEE)
					&& borrowed)) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_TRANSITION_MISMATCH,
				ZEND_MIRV_TOKEN_W06_TRANSITION_MISMATCH,
				"invalid ownership transition");
		}
	}
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
			&& plan.required <= ZEND_MIR_SEPARATION_REQUIRED_UNKNOWN;
		if (plan.required == ZEND_MIR_SEPARATION_REQUIRED_YES) {
			valid = valid && plan.result_payload_id < payload_count
				&& plan.result_payload_id != plan.source_payload_id
				&& plan.container_execution_debt
					== ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION;
		} else {
			valid = valid
				&& !zend_mir_id_is_valid(plan.result_payload_id)
				&& (!zend_mir_id_is_valid(
						plan.container_execution_debt)
					|| plan.container_execution_debt
						== ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION);
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
	uint32_t count = values->call_transfer_count(values->context);
	uint32_t storage_count = values->storage_count(values->context);
	uint32_t reference_count =
		values->reference_cell_count(values->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_call_transfer_ref transfer;
		if (!values->call_transfer_at(values->context, index, &transfer)
				|| !zend_mir_id_is_valid(transfer.call_site_id)
				|| transfer.parameter_modes.count == 0
				|| transfer.parameter_modes.offset
					> UINT32_MAX - transfer.parameter_modes.count
				|| transfer.resolved_debt_ids.offset
					> UINT32_MAX - transfer.resolved_debt_ids.count
				|| transfer.argument_storage_id >= storage_count
				|| (zend_mir_id_is_valid(
						transfer.argument_reference_cell_id)
					&& transfer.argument_reference_cell_id
						>= reference_count)
				|| transfer.argument_action < ZEND_MIR_TRANSFER_BORROW
				|| transfer.argument_action
					> ZEND_MIR_TRANSFER_FROM_CALLEE
				|| transfer.return_storage_id >= storage_count
				|| (zend_mir_id_is_valid(
						transfer.return_reference_cell_id)
					&& transfer.return_reference_cell_id
						>= reference_count)
				|| transfer.return_action < ZEND_MIR_TRANSFER_BORROW
				|| transfer.return_action
					> ZEND_MIR_TRANSFER_FROM_CALLEE) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_CALL_TRANSFER_MISMATCH,
				ZEND_MIRV_TOKEN_W06_CALL_TRANSFER_MISMATCH,
				"invalid call-transfer span or identity");
		}
	}
	return true;
}

static bool zend_mir_w06_verify_metadata(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	static const uint32_t expected_capabilities[] =
		{ 19, 20, 21, 22, 23, 24, 25, 26 };
	static const uint32_t expected_debts[] =
		{ 1001, 1008, 1009, 1010, 1011, 1012, 1013, 1014 };
	const zend_mir_module *module = zend_mir_module_from_value_view(values);

	if (module == NULL || module->value_capability_ids.count != 8
			|| module->value_semantic_debt_ids.count != 8
			|| memcmp(module->value_capability_ids.items,
				expected_capabilities,
				sizeof(expected_capabilities)) != 0
			|| memcmp(module->value_semantic_debt_ids.items,
				expected_debts, sizeof(expected_debts)) != 0) {
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_RECEIPT_MISMATCH,
			ZEND_MIRV_TOKEN_W06_RECEIPT_MISMATCH,
			"non-canonical capability or semantic-debt span");
	}
	return true;
}

static bool zend_mir_w06_verify_receipts(
	const zend_mir_view *view, const zend_mir_value_view *values,
	zend_mir_diagnostic_sink *diagnostics)
{
	bool seen[6] = { false, false, false, false, false, false };
	zend_mir_verifier_receipt_ref first;
	bool have_first = false;
	uint32_t count = values->verifier_receipt_count(values->context);
	uint32_t index;

	for (index = 0; index < count; index++) {
		zend_mir_value_verifier_receipt_ref wrapped;
		zend_mir_verifier_receipt_ref *receipt = &wrapped.receipt;
		uint32_t expected_version;
		if (!values->verifier_receipt_at(
				values->context, index, &wrapped)
				|| wrapped.id != index
				|| receipt->verifier_id < ZEND_MIR_VERIFIER_STRUCTURAL
				|| receipt->verifier_id
					> ZEND_MIR_VERIFIER_VALUE_REFERENCE
				|| seen[receipt->verifier_id]
				|| receipt->status != ZEND_MIR_VERIFIER_STATUS_PASS) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_RECEIPT_MISMATCH,
				ZEND_MIRV_TOKEN_W06_RECEIPT_MISMATCH,
				"invalid verifier receipt");
		}
		expected_version =
			receipt->verifier_id == ZEND_MIR_VERIFIER_CONTROL_FLOW
				? ZEND_MIR_W04_CONTRACT_VERSION
			: receipt->verifier_id == ZEND_MIR_VERIFIER_CALL_MODEL
				? ZEND_MIR_W05_CONTRACT_VERSION
			: receipt->verifier_id == ZEND_MIR_VERIFIER_VALUE_REFERENCE
				? ZEND_MIR_W06_CONTRACT_VERSION
				: ZEND_MIR_CONTRACT_VERSION;
		if (receipt->verifier_contract_version != expected_version
				|| (have_first
					&& (memcmp(first.module_fingerprint,
							receipt->module_fingerprint,
							sizeof(first.module_fingerprint)) != 0
						|| memcmp(first.source_fingerprint,
							receipt->source_fingerprint,
							sizeof(first.source_fingerprint)) != 0))) {
			return zend_mir_w06_emit(view, diagnostics,
				ZEND_MIR_VERIFY_W06_RECEIPT_MISMATCH,
				ZEND_MIRV_TOKEN_W06_RECEIPT_MISMATCH,
				"verifier receipt fingerprint mismatch");
		}
		if (!have_first) {
			first = *receipt;
			have_first = true;
		}
		seen[receipt->verifier_id] = true;
	}
	if (!seen[ZEND_MIR_VERIFIER_STRUCTURAL]
			|| !seen[ZEND_MIR_VERIFIER_SCALAR]
			|| !seen[ZEND_MIR_VERIFIER_CONTROL_FLOW]
			|| !seen[ZEND_MIR_VERIFIER_VALUE_REFERENCE]
			|| (values->call_transfer_count(values->context) != 0
				&& !seen[ZEND_MIR_VERIFIER_CALL_MODEL])) {
		return zend_mir_w06_emit(view, diagnostics,
			ZEND_MIR_VERIFY_W06_RECEIPT_MISMATCH,
			ZEND_MIRV_TOKEN_W06_RECEIPT_MISMATCH,
			"required verifier receipt absent");
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
		&& zend_mir_w06_verify_call_transfers(view, values, diagnostics)
		&& zend_mir_w06_verify_metadata(view, values, diagnostics)
		&& zend_mir_w06_verify_receipts(view, values, diagnostics);
}
