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

#include "zend_mir_arena.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ALLOCATION_LIMIT 256

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
			__FILE__, __LINE__, #condition); \
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
	zend_mir_diagnostic_code last_code;
} test_diagnostics;

typedef struct _test_snapshot {
	uint32_t words[128];
	uint32_t count;
} test_snapshot;

static void *test_allocate(void *context, size_t size, size_t alignment)
{
	test_allocator *allocator = context;
	uintptr_t address;
	void *raw;

	allocator->allocation_count++;
	if (allocator->allocation_count == allocator->fail_at) {
		return NULL;
	}
	if (allocator->raw_count >= TEST_ALLOCATION_LIMIT || alignment == 0
			|| (alignment & (alignment - 1)) != 0
			|| size > SIZE_MAX - alignment) {
		return NULL;
	}
	raw = malloc(size + alignment);
	if (raw == NULL) {
		return NULL;
	}
	address = ((uintptr_t) raw + alignment - 1) & ~(uintptr_t) (alignment - 1);
	allocator->raw[allocator->raw_count++] = raw;
	return (void *) address;
}

static void test_reset(void *context)
{
	test_allocator *allocator = context;
	uint32_t index;

	for (index = 0; index < allocator->raw_count; index++) {
		free(allocator->raw[index]);
	}
	allocator->raw_count = 0;
	allocator->reset_count++;
}

static zend_mir_allocator test_allocator_vtable(test_allocator *allocator)
{
	zend_mir_allocator vtable;

	vtable.context = allocator;
	vtable.allocate = test_allocate;
	vtable.reset = test_reset;
	return vtable;
}

static bool test_emit_diagnostic(void *context,
		const zend_mir_diagnostic *diagnostic)
{
	test_diagnostics *diagnostics = context;

	diagnostics->count++;
	diagnostics->last_code = diagnostic->code;
	return true;
}

static zend_mir_diagnostic_sink test_diagnostic_sink(
		test_diagnostics *diagnostics)
{
	zend_mir_diagnostic_sink sink;

	sink.context = diagnostics;
	sink.emit = test_emit_diagnostic;
	sink.limit = 32;
	sink.emitted = 0;
	return sink;
}

static void snapshot_word(test_snapshot *snapshot, uint32_t value)
{
	if (snapshot->count < sizeof(snapshot->words) / sizeof(snapshot->words[0])) {
		snapshot->words[snapshot->count++] = value;
	}
}

static bool take_snapshot(const zend_mir_view *view, test_snapshot *snapshot)
{
	uint32_t index;

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot_word(snapshot, view->module_id(view->context));
	snapshot_word(snapshot, view->function_count(view->context));
	for (index = 0; index < view->function_count(view->context); index++) {
		zend_mir_function_record record;

		CHECK(view->function_at(view->context, index, &record));
		snapshot_word(snapshot, record.id);
		snapshot_word(snapshot, record.symbol_id);
		snapshot_word(snapshot, record.entry_block_id);
		snapshot_word(snapshot, record.flags);
	}
	snapshot_word(snapshot, view->block_count(view->context));
	for (index = 0; index < view->block_count(view->context); index++) {
		zend_mir_block_record record;

		CHECK(view->block_at(view->context, index, &record));
		snapshot_word(snapshot, record.id);
		snapshot_word(snapshot, record.function_id);
	}
	snapshot_word(snapshot, view->value_count(view->context));
	for (index = 0; index < view->value_count(view->context); index++) {
		zend_mir_value_record record;

		CHECK(view->value_at(view->context, index, &record));
		snapshot_word(snapshot, record.id);
		snapshot_word(snapshot, (uint32_t) record.representation);
		snapshot_word(snapshot, (uint32_t) record.ownership);
	}
	snapshot_word(snapshot, view->constant_count(view->context));
	for (index = 0; index < view->constant_count(view->context); index++) {
		zend_mir_constant_record record;

		CHECK(view->constant_at(view->context, index, &record));
		snapshot_word(snapshot, record.value_id);
		snapshot_word(snapshot, (uint32_t) record.representation);
		snapshot_word(snapshot, (uint32_t) record.kind);
		snapshot_word(snapshot, (uint32_t) record.payload_bits);
		snapshot_word(snapshot, (uint32_t) (record.payload_bits >> 32));
		snapshot_word(snapshot, record.symbol_id);
	}
	snapshot_word(snapshot, view->instruction_count(view->context));
	for (index = 0; index < view->instruction_count(view->context); index++) {
		zend_mir_instruction_record record;
		uint32_t operand;

		CHECK(view->instruction_at(view->context, index, &record));
		snapshot_word(snapshot, record.id);
		snapshot_word(snapshot, record.block_id);
		snapshot_word(snapshot, (uint32_t) record.opcode);
		snapshot_word(snapshot, (uint32_t) record.representation);
		snapshot_word(snapshot, record.result_id);
		snapshot_word(snapshot, record.frame_state_id);
		snapshot_word(snapshot, record.source_position_id);
		snapshot_word(snapshot, record.effects);
		snapshot_word(snapshot, record.reads);
		snapshot_word(snapshot, record.writes);
		snapshot_word(snapshot, record.barriers);
		snapshot_word(snapshot, record.ownership_actions);
		snapshot_word(snapshot,
			view->instruction_operand_count(view->context, record.id));
		for (operand = 0;
				operand < view->instruction_operand_count(view->context, record.id);
				operand++) {
			zend_mir_value_id value_id;

			CHECK(view->instruction_operand_at(
				view->context, record.id, operand, &value_id));
			snapshot_word(snapshot, value_id);
		}
	}
	return true;
}

static bool build_model(size_t chunk_size, uint32_t fail_at,
		test_snapshot *snapshot, uint32_t *allocation_count)
{
	test_allocator allocator = { { 0 }, 0, 0, fail_at, 0 };
	test_diagnostics diagnostics = { 0, ZEND_MIR_DIAGNOSTIC_NONE };
	zend_mir_allocator vtable = test_allocator_vtable(&allocator);
	zend_mir_diagnostic_sink sink = test_diagnostic_sink(&diagnostics);
	zend_mir_module *module;
	zend_mir_mutator *mutator;
	const zend_mir_view *view;
	zend_mir_function_id function0;
	zend_mir_function_id function1;
	zend_mir_block_id block0;
	zend_mir_block_id block1;
	zend_mir_instruction_id instruction0;
	zend_mir_instruction_id instruction1;
	zend_mir_value_id synthetic = zend_mir_value_from_synthetic(3);
	zend_mir_constant_record constant;
	zend_mir_instruction_record instruction;
	bool success = false;

	module = zend_mir_module_create(42, &vtable, chunk_size, NULL, &sink);
	if (module == NULL) {
		goto done;
	}
	mutator = zend_mir_module_get_mutator(module);
	if (mutator == NULL
			|| !mutator->add_function(mutator->context, 100, &function0)
			|| !mutator->add_function(mutator->context, 101, &function1)
			|| !mutator->add_block(mutator->context, function0, &block0)
			|| !mutator->add_block(mutator->context, function1, &block1)
			|| !mutator->set_entry_block(mutator->context, function0, block0)
			|| !mutator->set_entry_block(mutator->context, function1, block1)
			|| !mutator->add_value(mutator->context, 7,
				ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED)
			|| !mutator->add_value(mutator->context, synthetic,
				ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_OWNERSHIP_STATE_BORROWED)
			|| !mutator->add_value(mutator->context, 2,
				ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_OWNERSHIP_STATE_UNINITIALIZED)) {
		goto destroy;
	}
	memset(&constant, 0, sizeof(constant));
	constant.value_id = 7;
	constant.representation = ZEND_MIR_REPRESENTATION_I64;
	constant.kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	constant.payload_bits = UINT64_C(0x1122334455667788);
	constant.symbol_id = ZEND_MIR_ID_INVALID;
	if (!mutator->add_constant(mutator->context, &constant)) {
		goto destroy;
	}
	memset(&instruction, 0, sizeof(instruction));
	instruction.id = ZEND_MIR_ID_INVALID;
	instruction.block_id = block0;
	instruction.opcode = ZEND_MIR_OPCODE_COPY;
	instruction.representation = ZEND_MIR_REPRESENTATION_I64;
	instruction.result_id = 7;
	instruction.frame_state_id = ZEND_MIR_ID_INVALID;
	instruction.source_position_id = ZEND_MIR_ID_INVALID;
	if (!mutator->add_instruction(mutator->context, &instruction, &instruction0)
			|| !mutator->add_operand(mutator->context, instruction0, synthetic)) {
		goto destroy;
	}
	instruction.block_id = block1;
	instruction.opcode = ZEND_MIR_OPCODE_RETURN;
	instruction.representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction.result_id = ZEND_MIR_ID_INVALID;
	if (!mutator->add_instruction(mutator->context, &instruction, &instruction1)
			|| !mutator->add_operand(mutator->context, instruction1, 2)
			|| !mutator->seal_function(mutator->context, function0)
			|| !mutator->seal_function(mutator->context, function1)
			|| !zend_mir_module_finalize(module)) {
		goto destroy;
	}
	view = zend_mir_module_get_view(module);
	if (view == NULL || !take_snapshot(view, snapshot)) {
		goto destroy;
	}
	success = true;

destroy:
	zend_mir_module_destroy(module);
done:
	if (allocation_count != NULL) {
		*allocation_count = allocator.allocation_count;
	}
	CHECK(allocator.raw_count == 0);
	CHECK(allocator.reset_count == 1);
	if (!success && fail_at != 0) {
		CHECK(diagnostics.count != 0);
	}
	return success;
}

static bool test_ids(void)
{
	bool synthetic;
	uint32_t payload;
	zend_mir_value_id id;

	CHECK(zend_mir_core_id_validate(0));
	CHECK(zend_mir_core_id_validate(ZEND_MIR_ID_MAX));
	CHECK(!zend_mir_core_id_validate(ZEND_MIR_ID_INVALID));
	CHECK(zend_mir_value_from_original_ssa(0) == 0);
	CHECK(zend_mir_value_from_original_ssa(ZEND_MIR_VALUE_ORIGINAL_MAX)
		== ZEND_MIR_VALUE_ORIGINAL_MAX);
	CHECK(zend_mir_value_from_original_ssa(ZEND_MIR_VALUE_SYNTHETIC_BIT)
		== ZEND_MIR_ID_INVALID);
	id = zend_mir_value_from_synthetic(ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX);
	CHECK(id == ZEND_MIR_VALUE_SYNTHETIC_MAX);
	CHECK(zend_mir_core_value_id_decode(id, &synthetic, &payload));
	CHECK(synthetic);
	CHECK(payload == ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX);
	CHECK(zend_mir_core_value_id_decode(
		ZEND_MIR_VALUE_ORIGINAL_MAX, &synthetic, &payload));
	CHECK(!synthetic);
	CHECK(payload == ZEND_MIR_VALUE_ORIGINAL_MAX);
	CHECK(!zend_mir_core_value_id_decode(
		ZEND_MIR_ID_INVALID, &synthetic, &payload));
	CHECK(!zend_mir_core_value_id_decode(0, NULL, &payload));
	return true;
}

static bool test_arena_alignment_and_overflow(void)
{
	test_allocator allocator = { { 0 }, 0, 0, 0, 0 };
	zend_mir_allocator vtable = test_allocator_vtable(&allocator);
	zend_mir_arena arena;
	void *first;
	void *aligned;

	CHECK(zend_mir_arena_init(&arena, &vtable, 128));
	first = zend_mir_arena_allocate(&arena, 1, 1);
	aligned = zend_mir_arena_allocate(&arena, sizeof(max_align_t), alignof(max_align_t));
	CHECK(first != NULL);
	CHECK(aligned != NULL);
	CHECK(((uintptr_t) aligned & (alignof(max_align_t) - 1)) == 0);
	zend_mir_arena_release(&arena);
	CHECK(allocator.reset_count == 1);

	memset(&allocator, 0, sizeof(allocator));
	vtable = test_allocator_vtable(&allocator);
	CHECK(zend_mir_arena_init(&arena, &vtable, 64));
	CHECK(zend_mir_arena_allocate(&arena, SIZE_MAX, alignof(max_align_t)) == NULL);
	CHECK(arena.failed);
	CHECK(allocator.allocation_count == 0);
	zend_mir_arena_release(&arena);
	return true;
}

static bool test_chunk_determinism_and_oom(void)
{
	test_snapshot small;
	test_snapshot large;
	test_snapshot ignored;
	uint32_t allocations;
	uint32_t fail_at;

	CHECK(build_model(64, 0, &small, &allocations));
	CHECK(build_model(16384, 0, &large, NULL));
	CHECK(small.count == large.count);
	CHECK(memcmp(small.words, large.words,
		(size_t) small.count * sizeof(small.words[0])) == 0);
	for (fail_at = 1; fail_at <= allocations; fail_at++) {
		CHECK(!build_model(64, fail_at, &ignored, NULL));
	}
	return true;
}

static bool test_lifecycle_and_limits(void)
{
	test_allocator allocator = { { 0 }, 0, 0, 0, 0 };
	test_diagnostics diagnostics = { 0, ZEND_MIR_DIAGNOSTIC_NONE };
	zend_mir_allocator vtable = test_allocator_vtable(&allocator);
	zend_mir_diagnostic_sink sink = test_diagnostic_sink(&diagnostics);
	zend_mir_module *module = zend_mir_module_create(1, &vtable, 128, NULL, &sink);
	zend_mir_mutator *mutator;
	const zend_mir_view *view;
	zend_mir_function_id function;
	zend_mir_block_id block;

	CHECK(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	view = zend_mir_module_get_view(module);
	CHECK(mutator != NULL && view != NULL);
	CHECK(mutator->add_function(mutator->context, 1, &function));
	CHECK(mutator->add_block(mutator->context, function, &block));
	CHECK(mutator->set_entry_block(mutator->context, function, block));
	CHECK(mutator->seal_function(mutator->context, function));
	CHECK(zend_mir_module_finalize(module));
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FINALIZED);
	CHECK(!zend_mir_module_finalize(module));
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FINALIZED);
	CHECK(zend_mir_module_get_mutator(module) == NULL);
	CHECK(!mutator->add_function(mutator->context, 2, &function));
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FINALIZED);
	CHECK(view->function_count(view->context) == 1);
	zend_mir_module_destroy(module);

	memset(&allocator, 0, sizeof(allocator));
	memset(&diagnostics, 0, sizeof(diagnostics));
	vtable = test_allocator_vtable(&allocator);
	sink = test_diagnostic_sink(&diagnostics);
	module = zend_mir_module_create(2, &vtable, 128, NULL, &sink);
	CHECK(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	view = zend_mir_module_get_view(module);
	CHECK(mutator->add_value(mutator->context, 5,
		ZEND_MIR_REPRESENTATION_I32, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	CHECK(!mutator->add_value(mutator->context, 5,
		ZEND_MIR_REPRESENTATION_I32, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FAILED);
	CHECK(zend_mir_module_get_view(module) == NULL);
	CHECK(zend_mir_module_get_mutator(module) == NULL);
	CHECK(view->value_count(view->context) == 0);
	CHECK(!mutator->add_value(mutator->context, 6,
		ZEND_MIR_REPRESENTATION_I32, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	zend_mir_module_destroy(module);

	memset(&allocator, 0, sizeof(allocator));
	memset(&diagnostics, 0, sizeof(diagnostics));
	vtable = test_allocator_vtable(&allocator);
	sink = test_diagnostic_sink(&diagnostics);
	{
		zend_mir_core_limits limits = zend_mir_core_default_limits();
		zend_mir_function_id untouched = 99;

		limits.functions = 0;
		module = zend_mir_module_create(3, &vtable, 128, &limits, &sink);
		CHECK(module != NULL);
		mutator = zend_mir_module_get_mutator(module);
		CHECK(!mutator->add_function(mutator->context, 1, &untouched));
		CHECK(untouched == 99);
		CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FAILED);
		CHECK(diagnostics.last_code == ZEND_MIR_DIAGNOSTIC_CAPACITY_EXCEEDED);
		zend_mir_module_destroy(module);
	}

	memset(&allocator, 0, sizeof(allocator));
	memset(&diagnostics, 0, sizeof(diagnostics));
	allocator.fail_at = 2;
	vtable = test_allocator_vtable(&allocator);
	sink = test_diagnostic_sink(&diagnostics);
	module = zend_mir_module_create(4, &vtable, 128, NULL, &sink);
	CHECK(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	view = zend_mir_module_get_view(module);
	function = 77;
	CHECK(!mutator->add_function(mutator->context, 1, &function));
	CHECK(function == 77);
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FAILED);
	CHECK(view->function_count(view->context) == 0);
	CHECK(diagnostics.last_code == ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED);
	zend_mir_module_destroy(module);

	memset(&allocator, 0, sizeof(allocator));
	memset(&diagnostics, 0, sizeof(diagnostics));
	vtable = test_allocator_vtable(&allocator);
	sink = test_diagnostic_sink(&diagnostics);
	module = zend_mir_module_create(5, &vtable, 128, NULL, &sink);
	CHECK(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	CHECK(mutator->add_function(mutator->context, 1, &function));
	CHECK(!zend_mir_module_finalize(module));
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FAILED);
	CHECK(diagnostics.last_code == ZEND_MIR_DIAGNOSTIC_INVALID_ID);
	zend_mir_module_destroy(module);
	return true;
}

static bool test_empty_and_full_view_surface(void)
{
	test_allocator allocator = { { 0 }, 0, 0, 0, 0 };
	zend_mir_allocator vtable = test_allocator_vtable(&allocator);
	zend_mir_module *module = zend_mir_module_create(9, &vtable, 0, NULL, NULL);
	const zend_mir_view *view;
	zend_mir_frame_state_ref frame_state;

	CHECK(module != NULL);
	view = zend_mir_module_get_view(module);
	CHECK(view != NULL);
	CHECK(view->contract_version == ZEND_MIR_CONTRACT_VERSION);
	CHECK(view->function_count(view->context) == 0);
	CHECK(view->block_count(view->context) == 0);
	CHECK(view->instruction_count(view->context) == 0);
	CHECK(view->value_count(view->context) == 0);
	CHECK(view->constant_count(view->context) == 0);
	CHECK(view->frame_state_count(view->context) == 0);
	CHECK(!view->frame_state_at(view->context, 0, &frame_state));
	CHECK(view->source_position_count(view->context) == 0);
	CHECK(view->frame_slot_count(view->context) == 0);
	CHECK(view->root_count(view->context) == 0);
	CHECK(view->cleanup_count(view->context) == 0);
	CHECK(view->successor_count(view->context, 0) == 0);
	CHECK(view->predecessor_count(view->context, 0) == 0);
	CHECK(zend_mir_module_finalize(module));
	zend_mir_module_destroy(module);
	return true;
}

int main(void)
{
	if (!test_ids()
			|| !test_arena_alignment_and_overflow()
			|| !test_chunk_determinism_and_oom()
			|| !test_lifecycle_and_limits()
			|| !test_empty_and_full_view_surface()) {
		return EXIT_FAILURE;
	}
	puts("core MIR tests passed");
	return EXIT_SUCCESS;
}
