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

#include "Zend/Native/Values/Core/zend_mir_value_core.h"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ALLOCATION_LIMIT 512
#define TEST_TEXT_CAPACITY 32768

static bool test_quiet_checks;

#define CHECK(condition) do { \
	if (!(condition)) { \
		if (!test_quiet_checks) { \
			fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
				__FILE__, __LINE__, #condition); \
		} \
		return false; \
	} \
} while (0)

typedef struct _test_allocator {
	void *raw[TEST_ALLOCATION_LIMIT];
	uint32_t raw_count;
	uint32_t allocation_count;
	uint32_t fail_at;
	uint32_t reset_count;
} test_allocator;

typedef struct _test_diagnostics {
	uint32_t count;
	char last_message[ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY];
} test_diagnostics;

typedef struct _test_text {
	char bytes[TEST_TEXT_CAPACITY];
	size_t length;
} test_text;

typedef enum _test_case {
	TEST_VALID = 0,
	TEST_INDIRECT_CYCLE,
	TEST_ALIAS_CONTRADICTION,
	TEST_DOUBLE_RELEASE,
	TEST_DESTROY_BORROWED,
	TEST_INVALID_SEPARATION,
	TEST_FINGERPRINT_MISMATCH,
	TEST_CALL_SPAN_OVERFLOW,
	TEST_DEBT_SPAN_OVERFLOW,
	TEST_MISSING_CAPABILITY,
	TEST_VERIFIER_INDIRECT_MUTATION,
	TEST_VERIFIER_FINGERPRINT_MUTATION,
	TEST_VERIFIER_PAYLOAD_MUTATION,
	TEST_COPY_ADDREF_UNIQUE_STAYS_UNIQUE,
	TEST_RELEASE_LOST_CLEANUP,
	TEST_EVENT_PAYLOAD_MISMATCH,
	TEST_REFERENCE_LOST_CLEANUP,
	TEST_ALIAS_TRANSITIVE_CONTRADICTION,
	TEST_SEPARATION_SOURCE_MISMATCH,
	TEST_SEPARATION_RESULT_CATEGORY_MISMATCH,
	TEST_STORAGE_ID_OVERFLOW,
	TEST_RELEASE_UNIQUE_STAYS_UNIQUE,
	TEST_ALIAS_STORAGE_USE_AFTER_MOVE,
	TEST_TABLE_COUNT_OVERFLOW,
	TEST_VALID_CALL_TRANSFER
} test_case;

static const zend_mir_capability_id test_capabilities[] = {
	ZEND_MIR_CAP_ZVAL_STORAGE_MODEL,
	ZEND_MIR_CAP_REFERENCE_CELL_MODEL,
	ZEND_MIR_CAP_INDIRECT_SLOT_MODEL,
	ZEND_MIR_CAP_REFCOUNT_TRANSFER_MODEL,
	ZEND_MIR_CAP_ALIAS_PARTITION_MODEL,
	ZEND_MIR_CAP_SEPARATION_PROTOCOL_MODEL,
	ZEND_MIR_CAP_DIRECT_USER_CALL_REFERENCE_TRANSFER_MODEL,
	ZEND_MIR_CAP_REFCOUNTED_CALL_RESULT_MODEL
};

static const zend_mir_semantic_debt_id test_debts[] = {
	ZEND_MIR_DEBT_CALL_EXECUTION,
	ZEND_MIR_DEBT_INTERNAL_C_ABI_INTEROP,
	ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION,
	ZEND_MIR_DEBT_STRING_AND_ARRAY_OPERATION_SEMANTICS,
	ZEND_MIR_DEBT_OBJECT_LIFECYCLE,
	ZEND_MIR_DEBT_DESTRUCTOR_EXCEPTION_CLEANUP,
	ZEND_MIR_DEBT_RUNTIME_REFERENCE_BINDING,
	ZEND_MIR_DEBT_DYNAMIC_SYMBOL_TABLE_ALIASING
};

static void *test_allocate(void *context, size_t size, size_t alignment)
{
	test_allocator *allocator = (test_allocator *) context;
	uintptr_t address;
	void *raw;

	allocator->allocation_count++;
	if (allocator->allocation_count == allocator->fail_at
			|| allocator->raw_count >= TEST_ALLOCATION_LIMIT
			|| alignment == 0 || (alignment & (alignment - 1)) != 0
			|| size > SIZE_MAX - alignment) {
		return NULL;
	}
	raw = malloc(size + alignment);
	if (raw == NULL) {
		return NULL;
	}
	address = ((uintptr_t) raw + alignment - 1)
		& ~(uintptr_t) (alignment - 1);
	allocator->raw[allocator->raw_count++] = raw;
	return (void *) address;
}

static void test_reset(void *context)
{
	test_allocator *allocator = (test_allocator *) context;
	uint32_t index;

	for (index = 0; index < allocator->raw_count; index++) {
		free(allocator->raw[index]);
	}
	allocator->raw_count = 0;
	allocator->reset_count++;
}

static zend_mir_allocator test_allocator_vtable(test_allocator *allocator)
{
	zend_mir_allocator result;

	result.context = allocator;
	result.allocate = test_allocate;
	result.reset = test_reset;
	return result;
}

static bool test_emit_diagnostic(
	void *context, const zend_mir_diagnostic *diagnostic)
{
	test_diagnostics *diagnostics = (test_diagnostics *) context;

	diagnostics->count++;
	memcpy(diagnostics->last_message, diagnostic->message,
		sizeof(diagnostics->last_message));
	return true;
}

static zend_mir_diagnostic_sink test_diagnostic_sink(
	test_diagnostics *diagnostics)
{
	zend_mir_diagnostic_sink result;

	memset(&result, 0, sizeof(result));
	result.context = diagnostics;
	result.emit = test_emit_diagnostic;
	result.limit = 32;
	return result;
}

static bool test_write(void *context, const char *bytes, size_t length)
{
	test_text *text = (test_text *) context;

	if (length > sizeof(text->bytes) - text->length - 1) {
		return false;
	}
	memcpy(text->bytes + text->length, bytes, length);
	text->length += length;
	text->bytes[text->length] = '\0';
	return true;
}

static bool test_stage_model(
	zend_mir_value_mutator *mutator, test_case mode)
{
	zend_mir_payload_ref payload;
	zend_mir_storage_ref storage;
	zend_mir_reference_cell_ref cell;
	zend_mir_alias_relation_ref alias;
	zend_mir_ownership_event_ref event;
	zend_mir_separation_plan_ref separation;

	memset(&payload, 0, sizeof(payload));
	payload.id = 0;
	payload.category = ZEND_MIR_VALUE_REFCOUNTED_STRING;
	payload.refcount_state =
		mode == TEST_COPY_ADDREF_UNIQUE_STAYS_UNIQUE
				|| mode == TEST_RELEASE_UNIQUE_STAYS_UNIQUE
			? ZEND_MIR_REFCOUNT_UNIQUE : ZEND_MIR_REFCOUNT_SHARED;
	payload.cleanup_obligation = true;
	CHECK(mutator->add_payload(mutator->context, &payload));
	payload.id = 1;
	payload.refcount_state = ZEND_MIR_REFCOUNT_UNIQUE;
	CHECK(mutator->add_payload(mutator->context, &payload));
	if (mode == TEST_SEPARATION_RESULT_CATEGORY_MISMATCH) {
		payload.id = 2;
		payload.category = ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT;
		payload.refcount_state = ZEND_MIR_REFCOUNT_UNIQUE;
		CHECK(mutator->add_payload(mutator->context, &payload));
	}

	memset(&storage, 0, sizeof(storage));
	storage.id = 0;
	storage.kind = ZEND_MIR_STORAGE_REFERENCE_PAYLOAD_SLOT;
	storage.state = ZEND_MIR_STORAGE_DIRECT;
	storage.category = ZEND_MIR_VALUE_REFCOUNTED_STRING;
	storage.payload_id = 0;
	storage.reference_cell_id = ZEND_MIR_ID_INVALID;
	storage.indirect_target_id = ZEND_MIR_ID_INVALID;
	CHECK(mutator->add_storage(mutator->context, &storage));
	storage.id = 1;
	storage.kind = ZEND_MIR_STORAGE_FRAME_SLOT;
	storage.state = ZEND_MIR_STORAGE_REFERENCE;
	storage.category = ZEND_MIR_VALUE_REFERENCE_CELL;
	storage.payload_id = ZEND_MIR_ID_INVALID;
	storage.reference_cell_id = 0;
	CHECK(mutator->add_storage(mutator->context, &storage));
	storage.id = 2;
	storage.kind = ZEND_MIR_STORAGE_INDIRECT_SLOT;
	storage.state = ZEND_MIR_STORAGE_INDIRECT;
	storage.reference_cell_id = ZEND_MIR_ID_INVALID;
	storage.indirect_target_id =
		mode == TEST_INDIRECT_CYCLE ? 4 : 1;
	CHECK(mutator->add_storage(mutator->context, &storage));
	storage.id = 3;
	storage.kind = ZEND_MIR_STORAGE_TEMPORARY;
	storage.state = ZEND_MIR_STORAGE_DIRECT;
	storage.category = ZEND_MIR_VALUE_REFCOUNTED_STRING;
	storage.payload_id = 0;
	storage.indirect_target_id = ZEND_MIR_ID_INVALID;
	CHECK(mutator->add_storage(mutator->context, &storage));
	if (mode == TEST_SEPARATION_SOURCE_MISMATCH) {
		storage.id = 4;
		storage.payload_id = 1;
		CHECK(mutator->add_storage(mutator->context, &storage));
	}
	if (mode == TEST_STORAGE_ID_OVERFLOW) {
		storage.id = ZEND_MIR_ID_INVALID;
		CHECK(mutator->add_storage(mutator->context, &storage));
	}
	if (mode == TEST_INDIRECT_CYCLE) {
		storage.id = 4;
		storage.kind = ZEND_MIR_STORAGE_INDIRECT_SLOT;
		storage.state = ZEND_MIR_STORAGE_INDIRECT;
		storage.category = ZEND_MIR_VALUE_CATEGORY_UNKNOWN;
		storage.payload_id = ZEND_MIR_ID_INVALID;
		storage.reference_cell_id = ZEND_MIR_ID_INVALID;
		storage.indirect_target_id = 2;
		CHECK(mutator->add_storage(mutator->context, &storage));
	}

	memset(&cell, 0, sizeof(cell));
	cell.id = 0;
	cell.payload_storage_id = 0;
	cell.alias_class_id = 1;
	cell.creation_source_id = 0;
	cell.ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
	cell.cleanup_obligation = mode != TEST_REFERENCE_LOST_CLEANUP;
	CHECK(mutator->add_reference_cell(mutator->context, &cell));

	memset(&alias, 0, sizeof(alias));
	alias.left_id = 1;
	alias.right_id = 1;
	alias.relation = ZEND_MIR_ALIAS_MUST;
	CHECK(mutator->add_alias_relation(mutator->context, &alias));
	if (mode == TEST_ALIAS_CONTRADICTION) {
		alias.right_id = 2;
		alias.relation = ZEND_MIR_ALIAS_NONE;
		alias.proof_id = 9;
		CHECK(mutator->add_alias_relation(mutator->context, &alias));
		alias.left_id = 2;
		alias.right_id = 1;
		alias.relation = ZEND_MIR_ALIAS_MAY;
		alias.proof_id = 0;
		CHECK(mutator->add_alias_relation(mutator->context, &alias));
	} else if (mode == TEST_ALIAS_TRANSITIVE_CONTRADICTION) {
		alias.left_id = 1;
		alias.right_id = 2;
		alias.relation = ZEND_MIR_ALIAS_MUST;
		alias.proof_id = 0;
		CHECK(mutator->add_alias_relation(mutator->context, &alias));
		alias.left_id = 2;
		alias.right_id = 3;
		CHECK(mutator->add_alias_relation(mutator->context, &alias));
		alias.left_id = 1;
		alias.right_id = 3;
		alias.relation = ZEND_MIR_ALIAS_NONE;
		alias.proof_id = 9;
		CHECK(mutator->add_alias_relation(mutator->context, &alias));
	}

	memset(&event, 0, sizeof(event));
	event.id = 0;
	event.source_storage_id = 0;
	event.target_storage_id = 3;
	event.payload_id = 0;
	event.action =
		mode == TEST_DOUBLE_RELEASE ? ZEND_MIR_TRANSFER_RELEASE
		: ZEND_MIR_TRANSFER_COPY_ADDREF;
	event.before_state = ZEND_MIR_REFCOUNT_SHARED;
	event.after_state = ZEND_MIR_REFCOUNT_SHARED;
	event.cleanup_obligation = true;
	if (mode == TEST_COPY_ADDREF_UNIQUE_STAYS_UNIQUE) {
		event.before_state = ZEND_MIR_REFCOUNT_UNIQUE;
		event.after_state = ZEND_MIR_REFCOUNT_UNIQUE;
	} else if (mode == TEST_RELEASE_UNIQUE_STAYS_UNIQUE) {
		event.action = ZEND_MIR_TRANSFER_RELEASE;
		event.target_storage_id = ZEND_MIR_ID_INVALID;
		event.before_state = ZEND_MIR_REFCOUNT_UNIQUE;
		event.after_state = ZEND_MIR_REFCOUNT_UNIQUE;
	} else if (mode == TEST_ALIAS_STORAGE_USE_AFTER_MOVE) {
		event.action = ZEND_MIR_TRANSFER_MOVE;
		event.cleanup_obligation = false;
	} else if (mode == TEST_DOUBLE_RELEASE) {
		event.target_storage_id = ZEND_MIR_ID_INVALID;
	} else if (mode == TEST_RELEASE_LOST_CLEANUP) {
		event.action = ZEND_MIR_TRANSFER_RELEASE;
		event.target_storage_id = ZEND_MIR_ID_INVALID;
		event.cleanup_obligation = false;
	} else if (mode == TEST_EVENT_PAYLOAD_MISMATCH) {
		event.payload_id = 1;
	}
	if (mode == TEST_DESTROY_BORROWED) {
		event.action = ZEND_MIR_TRANSFER_BORROW;
		event.target_storage_id = ZEND_MIR_ID_INVALID;
		event.cleanup_obligation = false;
	}
	CHECK(mutator->add_ownership_event(mutator->context, &event));
	if (mode == TEST_DOUBLE_RELEASE || mode == TEST_DESTROY_BORROWED
			|| mode == TEST_ALIAS_STORAGE_USE_AFTER_MOVE) {
		event.id = 1;
		event.action = ZEND_MIR_TRANSFER_RELEASE;
		event.target_storage_id = ZEND_MIR_ID_INVALID;
		event.cleanup_obligation = true;
		if (mode == TEST_ALIAS_STORAGE_USE_AFTER_MOVE) {
			event.source_storage_id = 3;
		}
		CHECK(mutator->add_ownership_event(mutator->context, &event));
	}

	memset(&separation, 0, sizeof(separation));
	separation.id = 0;
	separation.source_payload_id = 0;
	separation.source_storage_id = 0;
	separation.reason = ZEND_MIR_SEPARATION_WRITE;
	separation.uniqueness_fact = ZEND_MIR_REFCOUNT_SHARED;
	separation.required = ZEND_MIR_SEPARATION_REQUIRED_YES;
	separation.result_payload_id =
		mode == TEST_INVALID_SEPARATION ? 0 : 1;
	if (mode == TEST_SEPARATION_SOURCE_MISMATCH) {
		separation.source_storage_id = 4;
	} else if (mode == TEST_SEPARATION_RESULT_CATEGORY_MISMATCH) {
		separation.result_payload_id = 2;
	}
	separation.container_execution_debt =
		ZEND_MIR_DEBT_CONTAINER_CLONE_EXECUTION;
	if (mode == TEST_COPY_ADDREF_UNIQUE_STAYS_UNIQUE
			|| mode == TEST_RELEASE_UNIQUE_STAYS_UNIQUE) {
		separation.uniqueness_fact = ZEND_MIR_REFCOUNT_UNIQUE;
		separation.required = ZEND_MIR_SEPARATION_REQUIRED_NO;
		separation.result_payload_id = ZEND_MIR_ID_INVALID;
		separation.container_execution_debt = ZEND_MIR_ID_INVALID;
	}
	CHECK(mutator->add_separation_plan(mutator->context, &separation));

	if (mode == TEST_CALL_SPAN_OVERFLOW
			|| mode == TEST_DEBT_SPAN_OVERFLOW
			|| mode == TEST_VALID_CALL_TRANSFER) {
		zend_mir_call_transfer_ref transfer;
		memset(&transfer, 0, sizeof(transfer));
		transfer.call_site_id = 1;
		transfer.parameter_modes.offset =
			mode == TEST_CALL_SPAN_OVERFLOW ? UINT32_MAX : 0;
		transfer.parameter_modes.count =
			mode == TEST_CALL_SPAN_OVERFLOW ? 2 : 1;
		transfer.argument_storage_id =
			mode == TEST_VALID_CALL_TRANSFER ? 1 : 0;
		transfer.argument_reference_cell_id =
			mode == TEST_VALID_CALL_TRANSFER
				? 0 : ZEND_MIR_ID_INVALID;
		transfer.argument_action = ZEND_MIR_TRANSFER_BORROW;
		transfer.return_storage_id =
			mode == TEST_VALID_CALL_TRANSFER ? 1 : 3;
		transfer.return_reference_cell_id =
			mode == TEST_VALID_CALL_TRANSFER
				? 0 : ZEND_MIR_ID_INVALID;
		transfer.return_action = ZEND_MIR_TRANSFER_FROM_CALLEE;
		if (mode == TEST_DEBT_SPAN_OVERFLOW) {
			transfer.resolved_debt_ids.offset = UINT32_MAX;
			transfer.resolved_debt_ids.count = 2;
		}
		CHECK(mutator->add_call_transfer(mutator->context, &transfer));
	}

	return true;
}

static bool test_build_case(
	test_case mode, uint32_t fail_at, test_text *before_verify,
	test_text *after_verify, uint32_t *allocation_count,
	test_diagnostics *diagnostics_out)
{
	test_allocator allocator = { { 0 }, 0, 0, fail_at, 0 };
	test_diagnostics diagnostics = { 0, { 0 } };
	zend_mir_allocator vtable = test_allocator_vtable(&allocator);
	zend_mir_diagnostic_sink sink = test_diagnostic_sink(&diagnostics);
	zend_mir_module *module =
		zend_mir_module_create(60, &vtable, 128, NULL, &sink);
	zend_mir_mutator *core_mutator;
	zend_mir_value_mutator *value_mutator;
	zend_mir_function_id function;
	zend_mir_block_id block;
	zend_mir_source_position_ref source;
	zend_mir_source_position_id source_id;
	uint32_t module_fingerprint[4];
	const uint32_t source_fingerprint[4] = {
		UINT32_C(0x50607080), UINT32_C(0x50607081),
		UINT32_C(0x50607082), UINT32_C(0x50607083)
	};
	uint32_t verified_facets =
		ZEND_MIR_W06_VERIFIED_STRUCTURAL
		| ZEND_MIR_W06_VERIFIED_SCALAR
		| ZEND_MIR_W06_VERIFIED_CONTROL_FLOW
		| ZEND_MIR_W06_VERIFIED_VALUE_REFERENCE;
	bool committed;
	bool success = false;

	test_quiet_checks = fail_at != 0;
	if (module == NULL) {
		goto done;
	}
	core_mutator = zend_mir_module_get_mutator(module);
	value_mutator = zend_mir_module_get_value_mutator(module);
	if (core_mutator == NULL || value_mutator == NULL
			|| !core_mutator->add_function(
				core_mutator->context, 7, &function)
			|| !core_mutator->add_block(
				core_mutator->context, function, &block)
			|| !core_mutator->set_entry_block(
				core_mutator->context, function, block)) {
		goto destroy;
	}
	memset(&source, 0, sizeof(source));
	source.id = 0;
	source.file_symbol_id = 8;
	source.line = 1;
	source.column_start = 1;
	source.column_end = 2;
	if (!core_mutator->add_source_position(
			core_mutator->context, &source, &source_id)
			|| source_id != 0 || !test_stage_model(value_mutator, mode)) {
		goto destroy;
	}
	if (mode == TEST_TABLE_COUNT_OVERFLOW) {
		module->value_staging.payload_count = UINT32_C(1048577);
	}
	committed = zend_mir_module_commit_value_model(
		module, test_capabilities,
		mode == TEST_MISSING_CAPABILITY ? 7 : 8,
		test_debts, 8);
	if (!committed) {
		goto destroy;
	}
	if (!core_mutator->seal_function(core_mutator->context, function)
			|| !zend_mir_module_finalize(module)) {
		goto destroy;
	}
	if (mode == TEST_VALID_CALL_TRANSFER) {
		verified_facets |= ZEND_MIR_W06_VERIFIED_CALL_MODEL;
	}
	if (!zend_mir_value_compute_module_fingerprint(
			zend_mir_module_get_view(module), &sink, module_fingerprint)
			|| !zend_mir_module_publish_w06_verifier_receipts(
				module, module_fingerprint, source_fingerprint,
				verified_facets)) {
		goto destroy;
	}
	if (mode == TEST_VERIFIER_INDIRECT_MUTATION) {
		zend_mir_storage_ref *records = ZEND_MIR_CORE_ITEMS(
			module, value_storages, zend_mir_storage_ref);
		records[2].indirect_target_id = 2;
	} else if (mode == TEST_VERIFIER_FINGERPRINT_MUTATION
			|| mode == TEST_FINGERPRINT_MISMATCH) {
		zend_mir_value_verifier_receipt_ref *records =
			ZEND_MIR_CORE_ITEMS(module, value_verifier_receipts,
				zend_mir_value_verifier_receipt_ref);
		records[3].receipt.module_fingerprint[3]++;
	} else if (mode == TEST_VERIFIER_PAYLOAD_MUTATION) {
		zend_mir_payload_ref *payloads = ZEND_MIR_CORE_ITEMS(
			module, value_payloads, zend_mir_payload_ref);
		payloads[1].cleanup_obligation = false;
	}
	if (before_verify != NULL) {
		zend_mir_text_writer writer = { before_verify, test_write };
		if (!zend_mir_dump_text(
				zend_mir_module_get_view(module), &writer, &sink)) {
			goto destroy;
		}
	}
	if (!zend_mir_verify_w06_values(
			zend_mir_module_get_view(module),
			zend_mir_module_get_value_view(module), &sink)) {
		if (mode == TEST_VERIFIER_INDIRECT_MUTATION
				|| mode == TEST_VERIFIER_FINGERPRINT_MUTATION
				|| mode == TEST_VERIFIER_PAYLOAD_MUTATION
				|| mode == TEST_FINGERPRINT_MISMATCH) {
			success = true;
		}
		goto destroy;
	}
	if (mode == TEST_VERIFIER_INDIRECT_MUTATION
			|| mode == TEST_VERIFIER_FINGERPRINT_MUTATION
			|| mode == TEST_VERIFIER_PAYLOAD_MUTATION
			|| mode == TEST_FINGERPRINT_MISMATCH) {
		goto destroy;
	}
	if (after_verify != NULL) {
		zend_mir_text_writer writer = { after_verify, test_write };
		if (!zend_mir_dump_text(
				zend_mir_module_get_view(module), &writer, &sink)) {
			goto destroy;
		}
	}
	success = true;

destroy:
	zend_mir_module_destroy(module);
done:
	if (allocation_count != NULL) {
		*allocation_count = allocator.allocation_count;
	}
	if (diagnostics_out != NULL) {
		*diagnostics_out = diagnostics;
	}
	CHECK(allocator.raw_count == 0);
	CHECK(allocator.reset_count == 1);
	test_quiet_checks = false;
	return success;
}

static bool test_valid_model_and_read_only_verifier(void)
{
	test_text before = { { 0 }, 0 };
	test_text after = { { 0 }, 0 };
	test_diagnostics diagnostics;

	CHECK(test_build_case(
		TEST_VALID, 0, &before, &after, NULL, &diagnostics));
	CHECK(before.length == after.length);
	CHECK(memcmp(before.bytes, after.bytes, before.length) == 0);
	CHECK(strstr(before.bytes, "value-storage vs0") != NULL);
	CHECK(strstr(before.bytes, "reference-cell rc0") != NULL);
	CHECK(strstr(before.bytes, "separation-plan sp0") != NULL);
	CHECK(strstr(before.bytes, "value-capability 19") != NULL);
	CHECK(strstr(before.bytes, "value-debt 1009") != NULL);
	CHECK(diagnostics.count == 0);
	return true;
}

static bool test_rejections(void)
{
	static const test_case rejected[] = {
		TEST_INDIRECT_CYCLE,
		TEST_ALIAS_CONTRADICTION,
		TEST_DOUBLE_RELEASE,
		TEST_DESTROY_BORROWED,
		TEST_INVALID_SEPARATION,
		TEST_CALL_SPAN_OVERFLOW,
		TEST_DEBT_SPAN_OVERFLOW,
		TEST_COPY_ADDREF_UNIQUE_STAYS_UNIQUE,
		TEST_RELEASE_LOST_CLEANUP,
		TEST_EVENT_PAYLOAD_MISMATCH,
		TEST_REFERENCE_LOST_CLEANUP,
		TEST_ALIAS_TRANSITIVE_CONTRADICTION,
		TEST_SEPARATION_SOURCE_MISMATCH,
		TEST_SEPARATION_RESULT_CATEGORY_MISMATCH,
		TEST_STORAGE_ID_OVERFLOW,
		TEST_RELEASE_UNIQUE_STAYS_UNIQUE,
		TEST_ALIAS_STORAGE_USE_AFTER_MOVE,
		TEST_TABLE_COUNT_OVERFLOW,
		TEST_MISSING_CAPABILITY
	};
	uint32_t index;

	for (index = 0; index < sizeof(rejected) / sizeof(rejected[0]);
			index++) {
		test_diagnostics diagnostics;
		CHECK(!test_build_case(
			rejected[index], 0, NULL, NULL, NULL, &diagnostics));
		CHECK(diagnostics.count != 0);
	}
	return true;
}

static bool test_merge_rules(void)
{
	zend_mir_storage_ref left;
	zend_mir_storage_ref right;
	zend_mir_storage_ref merged;

	CHECK(zend_mir_value_merge_alias_relation(
		ZEND_MIR_ALIAS_MUST, ZEND_MIR_ALIAS_NONE) == ZEND_MIR_ALIAS_MAY);
	CHECK(zend_mir_value_merge_alias_relation(
		ZEND_MIR_ALIAS_NONE, ZEND_MIR_ALIAS_NONE) == ZEND_MIR_ALIAS_MAY);
	CHECK(zend_mir_value_merge_refcount_state(
		ZEND_MIR_REFCOUNT_UNIQUE,
		ZEND_MIR_REFCOUNT_SHARED) == ZEND_MIR_REFCOUNT_SHARED);
	memset(&left, 0, sizeof(left));
	left.id = 1;
	left.kind = ZEND_MIR_STORAGE_TEMPORARY;
	left.state = ZEND_MIR_STORAGE_DIRECT;
	left.category = ZEND_MIR_VALUE_REFCOUNTED_STRING;
	left.payload_id = 1;
	left.reference_cell_id = ZEND_MIR_ID_INVALID;
	left.indirect_target_id = ZEND_MIR_ID_INVALID;
	right = left;
	right.id = 2;
	right.payload_id = 2;
	CHECK(!zend_mir_value_merge_storage_state(&left, &right, &merged));
	right.state = ZEND_MIR_STORAGE_REFERENCE;
	CHECK(!zend_mir_value_merge_storage_state(&left, &right, &merged));
	return true;
}

static bool test_verifier_rejections(void)
{
	test_diagnostics diagnostics;

	CHECK(test_build_case(TEST_VERIFIER_INDIRECT_MUTATION,
		0, NULL, NULL, NULL, &diagnostics));
	CHECK(strstr(diagnostics.last_message, "MIRV0802") != NULL);
	CHECK(test_build_case(TEST_VERIFIER_FINGERPRINT_MUTATION,
		0, NULL, NULL, NULL, &diagnostics));
	CHECK(strstr(diagnostics.last_message, "MIRV0807") != NULL);
	CHECK(test_build_case(TEST_FINGERPRINT_MISMATCH,
		0, NULL, NULL, NULL, &diagnostics));
	CHECK(strstr(diagnostics.last_message, "MIRV0807") != NULL);
	CHECK(test_build_case(TEST_VERIFIER_PAYLOAD_MUTATION,
		0, NULL, NULL, NULL, &diagnostics));
	CHECK(strstr(diagnostics.last_message, "MIRV0807") != NULL);
	return true;
}

static bool test_call_transfer_dump_is_complete(void)
{
	test_text text = { { 0 }, 0 };
	test_diagnostics diagnostics;

	CHECK(test_build_case(
		TEST_VALID_CALL_TRANSFER, 0, &text, NULL, NULL, &diagnostics));
	CHECK(strstr(text.bytes, "argument-reference rc0") != NULL);
	CHECK(strstr(text.bytes, "return-reference rc0") != NULL);
	return true;
}

static bool test_every_arena_allocation_failure(void)
{
	uint32_t allocation_count;
	uint32_t fail_at;

	CHECK(test_build_case(
		TEST_VALID, 0, NULL, NULL, &allocation_count, NULL));
	CHECK(allocation_count > 1);
	for (fail_at = 1; fail_at <= allocation_count; fail_at++) {
		CHECK(!test_build_case(
			TEST_VALID, fail_at, NULL, NULL, NULL, NULL));
	}
	return true;
}

int main(void)
{
	if (!test_valid_model_and_read_only_verifier()
			|| !test_rejections()
			|| !test_merge_rules()
			|| !test_verifier_rejections()
			|| !test_call_transfer_dump_is_complete()
			|| !test_every_arena_allocation_failure()) {
		return 1;
	}
	puts("W06 value core tests passed");
	return 0;
}
