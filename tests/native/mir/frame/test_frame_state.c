#include "Zend/Native/MIR/Frame/zend_mir_frame_state.h"
#include "Zend/Native/MIR/Frame/zend_mir_source_map.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_ALLOCATIONS 512
#define TEST_NO_FAILURE UINT32_MAX

typedef struct _test_arena {
	void *blocks[TEST_MAX_ALLOCATIONS];
	uint32_t allocation_count;
	uint32_t fail_at;
} test_arena;

static void *test_allocate(void *context, size_t size, size_t alignment)
{
	test_arena *arena = (test_arena *) context;
	void *block;

	assert(alignment <= _Alignof(max_align_t));
	if (arena->allocation_count == arena->fail_at
			|| arena->allocation_count >= TEST_MAX_ALLOCATIONS) {
		return NULL;
	}
	block = malloc(size);
	if (block == NULL) {
		return NULL;
	}
	arena->blocks[arena->allocation_count++] = block;
	return block;
}

static void test_reset(void *context)
{
	test_arena *arena = (test_arena *) context;
	uint32_t i;

	for (i = 0; i < arena->allocation_count; i++) {
		free(arena->blocks[i]);
	}
	memset(arena, 0, sizeof(*arena));
	arena->fail_at = TEST_NO_FAILURE;
}

static zend_mir_allocator test_allocator(test_arena *arena)
{
	zend_mir_allocator allocator;

	memset(arena, 0, sizeof(*arena));
	arena->fail_at = TEST_NO_FAILURE;
	allocator.context = arena;
	allocator.allocate = test_allocate;
	allocator.reset = test_reset;
	return allocator;
}

static zend_mir_frame_continuation_spec self_continuation(
		zend_mir_continuation_kind kind, uint32_t opline_index)
{
	zend_mir_frame_continuation_spec continuation;

	continuation.kind = kind;
	continuation.target_kind = ZEND_MIR_FRAME_TARGET_SELF;
	continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	continuation.opline_index = opline_index;
	return continuation;
}

static zend_mir_frame_continuation_spec no_continuation(zend_mir_continuation_kind kind)
{
	zend_mir_frame_continuation_spec continuation;

	continuation.kind = kind;
	continuation.target_kind = ZEND_MIR_FRAME_TARGET_NONE;
	continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	continuation.opline_index = ZEND_MIR_ID_INVALID;
	return continuation;
}

typedef struct _test_frame_options {
	zend_mir_function_id function_id;
	zend_mir_symbol_id name_symbol_id;
	zend_mir_op_array_id op_array_id;
	uint32_t opline_index;
	zend_mir_opline_phase phase;
	zend_mir_frame_state_id parent_id;
	zend_mir_safepoint_class safepoint_class;
	zend_mir_suspend_kind suspend_kind;
	uint32_t slot_id;
	uint32_t root_copies;
	uint32_t cleanup_copies;
	bool duplicate_slot;
	bool canonical;
	bool immutable_code;
	bool malformed_resume;
} test_frame_options;

static test_frame_options normal_options(uint32_t discriminator)
{
	test_frame_options options;

	memset(&options, 0, sizeof(options));
	options.function_id = 10 + discriminator;
	options.name_symbol_id = 100 + discriminator;
	options.op_array_id = 200 + discriminator;
	options.opline_index = 4 + discriminator;
	options.phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	options.parent_id = ZEND_MIR_ID_INVALID;
	options.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_USER_CALL;
	options.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	options.slot_id = 300 + discriminator;
	options.root_copies = 1;
	options.cleanup_copies = 1;
	options.canonical = true;
	options.immutable_code = true;
	return options;
}

static zend_mir_frame_builder *build_frame(
		zend_mir_allocator allocator, test_frame_options options)
{
	zend_mir_frame_builder *builder = zend_mir_frame_builder_create(allocator);
	zend_mir_frame_slot_ref slot;
	zend_mir_cleanup_ref cleanup;
	zend_mir_resume_ref resume;
	zend_mir_frame_continuation_spec return_continuation;
	zend_mir_frame_continuation_spec exception_continuation;
	uint32_t code_version_id = 400 + options.function_id;
	uint32_t i;

	assert(builder != NULL);
	assert(zend_mir_frame_builder_set_function(builder, options.function_id,
		options.name_symbol_id, options.op_array_id, ZEND_MIR_FUNCTION_KIND_USER)
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_opline(builder, options.opline_index, options.phase)
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_parent(builder, options.parent_id)
		== ZEND_MIR_FRAME_STATUS_OK);
	memset(&slot, 0, sizeof(slot));
	slot.slot_id = options.slot_id;
	slot.value_id = 500 + options.function_id;
	slot.index = 0;
	slot.kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
	slot.representation = options.suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
		? ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL
		: ZEND_MIR_FRAME_SLOT_REPRESENTATION_PERSISTENT_SUSPEND_STATE;
	slot.materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	slot.ownership = options.suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
		? ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED
		: ZEND_MIR_FRAME_SLOT_OWNERSHIP_SUSPEND_STATE_OWNED;
	slot.rooted = true;
	slot.cleanup_required = true;
	assert(zend_mir_frame_builder_add_slot(builder, &slot) == ZEND_MIR_FRAME_STATUS_OK);
	if (options.duplicate_slot) {
		assert(zend_mir_frame_builder_add_slot(builder, &slot) == ZEND_MIR_FRAME_STATUS_OK);
	}
	for (i = 0; i < options.root_copies; i++) {
		assert(zend_mir_frame_builder_add_root(builder, slot.slot_id)
			== ZEND_MIR_FRAME_STATUS_OK);
	}
	cleanup.slot_id = slot.slot_id;
	cleanup.action = options.suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
		? ZEND_MIR_CLEANUP_ACTION_DESTROY : ZEND_MIR_CLEANUP_ACTION_RELEASE;
	cleanup.state = options.suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
		? ZEND_MIR_CLEANUP_STATE_PENDING : ZEND_MIR_CLEANUP_STATE_TRANSFERRED;
	for (i = 0; i < options.cleanup_copies; i++) {
		assert(zend_mir_frame_builder_add_cleanup(builder, &cleanup)
			== ZEND_MIR_FRAME_STATUS_OK);
	}
	assert(zend_mir_frame_builder_finish_layout(builder) == ZEND_MIR_FRAME_STATUS_OK);
	return_continuation = self_continuation(
		ZEND_MIR_CONTINUATION_KIND_NATIVE, options.opline_index + 1);
	exception_continuation = self_continuation(
		ZEND_MIR_CONTINUATION_KIND_ZEND_EXCEPTION, options.opline_index);
	assert(zend_mir_frame_builder_set_continuations(builder,
		return_continuation, exception_continuation,
		no_continuation(ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT))
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_suspend(builder, options.suspend_kind,
		options.suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
			? ZEND_MIR_ID_INVALID : 600 + options.function_id)
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_code_version(
		builder, code_version_id, options.immutable_code, true) == ZEND_MIR_FRAME_STATUS_OK);
	memset(&resume, 0, sizeof(resume));
	if (options.suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE) {
		resume.allowed = false;
		resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
		resume.resume_id = ZEND_MIR_ID_INVALID;
		resume.code_version_id = ZEND_MIR_ID_INVALID;
		resume.target_opline_index = ZEND_MIR_ID_INVALID;
	} else {
		resume.allowed = true;
		resume.entry_kind = options.malformed_resume
			? ZEND_MIR_RESUME_ENTRY_KIND_NONE
			: ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER;
		resume.resume_id = 700 + options.function_id;
		resume.code_version_id = options.malformed_resume ? code_version_id + 1 : code_version_id;
		resume.target_opline_index = options.opline_index + 1;
	}
	assert(zend_mir_frame_builder_set_resume(builder, resume) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_safepoint(
		builder, options.safepoint_class, options.canonical) == ZEND_MIR_FRAME_STATUS_OK);
	return builder;
}

static void test_w01_examples_and_deduplication(void)
{
	test_arena table_arena;
	test_arena builder_arena;
	zend_mir_allocator table_allocator = test_allocator(&table_arena);
	zend_mir_allocator builder_allocator = test_allocator(&builder_arena);
	zend_mir_frame_table *table = zend_mir_frame_table_create(table_allocator, UINT64_MAX);
	test_frame_options options = normal_options(0);
	zend_mir_frame_builder *first;
	zend_mir_frame_builder *duplicate;
	zend_mir_frame_state_record record;
	zend_mir_frame_slot_ref slot;
	zend_mir_cleanup_ref cleanup;
	zend_mir_frame_state_id first_id;
	zend_mir_frame_state_id duplicate_id;
	zend_mir_frame_state_id example_id;
	zend_mir_frame_builder *internal;
	zend_mir_resume_ref no_resume;
	uint32_t root;

	assert(table != NULL);
	first = build_frame(builder_allocator, options);
	duplicate = build_frame(builder_allocator, options);
	assert(zend_mir_frame_table_intern(table, first, &first_id) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_table_intern(table, duplicate, &duplicate_id) == ZEND_MIR_FRAME_STATUS_OK);
	assert(first_id == duplicate_id);
	assert(zend_mir_frame_table_count(table) == 1);
	assert(zend_mir_frame_table_at(table, first_id, &record));
	assert(record.owner_frame_id == first_id);
	assert(record.op_array_id == options.op_array_id);
	assert(record.code_version_immutable && record.code_version_active);
	assert(record.ref.return_continuation.frame_state_id == first_id);
	assert(zend_mir_frame_table_slot_at(table, first_id, 0, &slot));
	assert(slot.slot_id == options.slot_id);
	assert(zend_mir_frame_table_root_at(table, first_id, 0, &root));
	assert(root == options.slot_id);
	assert(zend_mir_frame_table_cleanup_at(table, first_id, 0, &cleanup));
	assert(cleanup.slot_id == options.slot_id);
	assert(zend_mir_frame_builder_set_parent(first, ZEND_MIR_ID_INVALID)
		== ZEND_MIR_FRAME_STATUS_FINALIZED);

	options = normal_options(1);
	options.phase = ZEND_MIR_OPLINE_PHASE_EXCEPTION;
	options.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_EXCEPTION_EDGE;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &example_id) == ZEND_MIR_FRAME_STATUS_OK);
	options = normal_options(2);
	options.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &example_id) == ZEND_MIR_FRAME_STATUS_OK);
	options = normal_options(3);
	options.phase = ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	options.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	options.suspend_kind = ZEND_MIR_SUSPEND_KIND_GENERATOR;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &example_id) == ZEND_MIR_FRAME_STATUS_OK);
	options = normal_options(4);
	options.phase = ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	options.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_FIBER_SWITCH;
	options.suspend_kind = ZEND_MIR_SUSPEND_KIND_FIBER;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &example_id) == ZEND_MIR_FRAME_STATUS_OK);

	internal = zend_mir_frame_builder_create(builder_allocator);
	assert(internal != NULL);
	assert(zend_mir_frame_builder_set_function(internal, 90, 91, ZEND_MIR_ID_INVALID,
		ZEND_MIR_FUNCTION_KIND_INTERNAL) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_opline(
		internal, ZEND_MIR_ID_INVALID, ZEND_MIR_OPLINE_PHASE_BEFORE) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_parent(internal, ZEND_MIR_ID_INVALID)
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_finish_layout(internal) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_continuations(internal,
		no_continuation(ZEND_MIR_CONTINUATION_KIND_TERMINAL),
		no_continuation(ZEND_MIR_CONTINUATION_KIND_TERMINAL),
		no_continuation(ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT))
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_suspend(
		internal, ZEND_MIR_SUSPEND_KIND_NONE, ZEND_MIR_ID_INVALID) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_code_version(internal, 92, true, true)
		== ZEND_MIR_FRAME_STATUS_OK);
	memset(&no_resume, 0, sizeof(no_resume));
	no_resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	no_resume.resume_id = ZEND_MIR_ID_INVALID;
	no_resume.code_version_id = ZEND_MIR_ID_INVALID;
	no_resume.target_opline_index = ZEND_MIR_ID_INVALID;
	assert(zend_mir_frame_builder_set_resume(internal, no_resume) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_safepoint(
		internal, ZEND_MIR_SAFEPOINT_CLASS_INTERNAL_CALL, true) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_table_intern(table, internal, &example_id) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_table_at(table, example_id, &record));
	assert(record.op_array_id == ZEND_MIR_ID_INVALID);
	assert(record.ref.opline_index == ZEND_MIR_ID_INVALID);
	assert(zend_mir_frame_table_count(table) == 6);
	test_reset(&builder_arena);
	test_reset(&table_arena);
}

static void test_collisions_parents_and_negative_cases(void)
{
	test_arena table_arena;
	test_arena builder_arena;
	zend_mir_allocator table_allocator = test_allocator(&table_arena);
	zend_mir_allocator builder_allocator = test_allocator(&builder_arena);
	zend_mir_frame_table *table = zend_mir_frame_table_create(table_allocator, 0);
	test_frame_options options = normal_options(10);
	zend_mir_frame_state_id first_id;
	zend_mir_frame_state_id second_id;
	zend_mir_frame_builder *builder;

	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &first_id) == ZEND_MIR_FRAME_STATUS_OK);
	options = normal_options(11);
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_OK);
	assert(first_id != second_id);
	assert(zend_mir_frame_table_count(table) == 2);

	options = normal_options(12);
	options.parent_id = first_id;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_OK);
	options = normal_options(13);
	options.parent_id = zend_mir_frame_table_count(table);
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_PARENT_CYCLE);
	options = normal_options(14);
	options.parent_id = 1000;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_INVALID_PARENT);

	options = normal_options(15);
	options.duplicate_slot = true;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_DUPLICATE_SLOT);
	options = normal_options(16);
	options.root_copies = 2;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_DUPLICATE_ROOT);
	options = normal_options(17);
	options.root_copies = 0;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_ROOT_MISMATCH);
	options = normal_options(18);
	options.cleanup_copies = 0;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_CLEANUP_MISMATCH);
	options = normal_options(22);
	options.cleanup_copies = 2;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_DUPLICATE_CLEANUP);
	options = normal_options(19);
	options.canonical = false;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_NONCANONICAL);
	options = normal_options(20);
	options.phase = ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	options.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	options.suspend_kind = ZEND_MIR_SUSPEND_KIND_GENERATOR;
	options.malformed_resume = true;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_INVALID_RESUME);
	options = normal_options(21);
	options.immutable_code = false;
	assert(zend_mir_frame_table_intern(table,
		build_frame(builder_allocator, options), &second_id) == ZEND_MIR_FRAME_STATUS_NONCANONICAL);

	builder = zend_mir_frame_builder_create(builder_allocator);
	assert(builder != NULL);
	assert(zend_mir_frame_table_intern(table, builder, &second_id)
		== ZEND_MIR_FRAME_STATUS_MISSING_REQUIRED);
	assert(zend_mir_frame_table_count(table) == 3);
	test_reset(&builder_arena);
	test_reset(&table_arena);
}

static void test_builder_failure_atomicity(void)
{
	test_arena table_arena;
	test_arena builder_arena;
	zend_mir_allocator table_allocator = test_allocator(&table_arena);
	zend_mir_allocator builder_allocator = test_allocator(&builder_arena);
	zend_mir_frame_table *table = zend_mir_frame_table_create(table_allocator, UINT64_MAX);
	zend_mir_frame_builder *builder = zend_mir_frame_builder_create(builder_allocator);
	zend_mir_frame_slot_ref slot;
	zend_mir_cleanup_ref cleanup;
	zend_mir_resume_ref resume;
	zend_mir_frame_state_id id;

	assert(table != NULL && builder != NULL);
	assert(zend_mir_frame_builder_set_function(
		builder, 80, 81, 82, ZEND_MIR_FUNCTION_KIND_USER) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_opline(
		builder, 3, ZEND_MIR_OPLINE_PHASE_BEFORE) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_parent(builder, ZEND_MIR_ID_INVALID)
		== ZEND_MIR_FRAME_STATUS_OK);
	memset(&slot, 0, sizeof(slot));
	slot.slot_id = 83;
	slot.value_id = 84;
	slot.kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
	slot.representation = ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	slot.materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	slot.ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	slot.rooted = true;
	slot.cleanup_required = true;
	builder_arena.fail_at = builder_arena.allocation_count;
	assert(zend_mir_frame_builder_add_slot(builder, &slot)
		== ZEND_MIR_FRAME_STATUS_OUT_OF_MEMORY);
	builder_arena.fail_at = TEST_NO_FAILURE;
	assert(zend_mir_frame_builder_add_slot(builder, &slot) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_add_root(builder, slot.slot_id) == ZEND_MIR_FRAME_STATUS_OK);
	cleanup.slot_id = slot.slot_id;
	cleanup.action = ZEND_MIR_CLEANUP_ACTION_DESTROY;
	cleanup.state = ZEND_MIR_CLEANUP_STATE_PENDING;
	assert(zend_mir_frame_builder_add_cleanup(builder, &cleanup) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_finish_layout(builder) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_continuations(builder,
		self_continuation(ZEND_MIR_CONTINUATION_KIND_NATIVE, 4),
		self_continuation(ZEND_MIR_CONTINUATION_KIND_ZEND_EXCEPTION, 3),
		no_continuation(ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT))
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_suspend(
		builder, ZEND_MIR_SUSPEND_KIND_NONE, ZEND_MIR_ID_INVALID) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_code_version(builder, 85, true, true)
		== ZEND_MIR_FRAME_STATUS_OK);
	memset(&resume, 0, sizeof(resume));
	resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	resume.resume_id = ZEND_MIR_ID_INVALID;
	resume.code_version_id = ZEND_MIR_ID_INVALID;
	resume.target_opline_index = ZEND_MIR_ID_INVALID;
	assert(zend_mir_frame_builder_set_resume(builder, resume) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_builder_set_safepoint(
		builder, ZEND_MIR_SAFEPOINT_CLASS_ALLOCATION, true) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_table_intern(table, builder, &id) == ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_frame_table_count(table) == 1);
	test_reset(&builder_arena);
	test_reset(&table_arena);
}

static void test_failure_atomicity(void)
{
	test_arena table_arena;
	test_arena builder_arena;
	zend_mir_allocator table_allocator = test_allocator(&table_arena);
	zend_mir_allocator builder_allocator = test_allocator(&builder_arena);
	zend_mir_frame_table *table = zend_mir_frame_table_create(table_allocator, UINT64_MAX);
	zend_mir_frame_builder *builder = build_frame(builder_allocator, normal_options(30));
	zend_mir_frame_state_id id;

	assert(table != NULL);
	table_arena.fail_at = table_arena.allocation_count + 1;
	assert(zend_mir_frame_table_intern(table, builder, &id)
		== ZEND_MIR_FRAME_STATUS_OUT_OF_MEMORY);
	assert(zend_mir_frame_table_count(table) == 0);
	table_arena.fail_at = TEST_NO_FAILURE;
	assert(zend_mir_frame_table_intern(table, builder, &id) == ZEND_MIR_FRAME_STATUS_OK);
	assert(id == 0 && zend_mir_frame_table_count(table) == 1);
	test_reset(&builder_arena);
	test_reset(&table_arena);
}

static void test_source_maps(void)
{
	test_arena frame_arena;
	test_arena builder_arena;
	test_arena source_arena;
	zend_mir_allocator frame_allocator = test_allocator(&frame_arena);
	zend_mir_allocator builder_allocator = test_allocator(&builder_arena);
	zend_mir_allocator source_allocator = test_allocator(&source_arena);
	zend_mir_frame_table *frames = zend_mir_frame_table_create(frame_allocator, UINT64_MAX);
	test_frame_options options = normal_options(40);
	zend_mir_source_map_table *maps;
	zend_mir_source_map_ref requested;
	zend_mir_source_map_ref observed;
	zend_mir_source_map_id first;
	zend_mir_source_map_id duplicate;
	zend_mir_source_map_id second;
	zend_mir_frame_state_id owner;

	assert(zend_mir_frame_table_intern(frames,
		build_frame(builder_allocator, options), &owner) == ZEND_MIR_FRAME_STATUS_OK);
	maps = zend_mir_source_map_table_create(source_allocator, frames, 0);
	assert(maps != NULL);
	memset(&requested, 0, sizeof(requested));
	requested.id = ZEND_MIR_ID_INVALID;
	requested.source_position_id = 7;
	requested.op_array_id = options.op_array_id;
	requested.opline_index = options.opline_index;
	requested.opline_phase = options.phase;
	requested.owner_frame_id = owner;
	source_arena.fail_at = source_arena.allocation_count;
	assert(zend_mir_source_map_table_intern(maps, &requested, &first)
		== ZEND_MIR_FRAME_STATUS_OUT_OF_MEMORY);
	assert(zend_mir_source_map_table_count(maps) == 0);
	source_arena.fail_at = TEST_NO_FAILURE;
	assert(zend_mir_source_map_table_intern(maps, &requested, &first)
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(zend_mir_source_map_table_intern(maps, &requested, &duplicate)
		== ZEND_MIR_FRAME_STATUS_OK);
	assert(first == duplicate);
	requested.source_position_id = 8;
	assert(zend_mir_source_map_table_add(maps, &requested, &second));
	assert(second != first);
	assert(zend_mir_source_map_table_count(maps) == 2);
	assert(zend_mir_source_map_table_at(maps, 0, &observed));
	assert(observed.source_position_id == 7);
	requested.opline_index++;
	assert(zend_mir_source_map_table_intern(maps, &requested, &second)
		== ZEND_MIR_FRAME_STATUS_NONCANONICAL);
	assert(zend_mir_source_map_table_count(maps) == 2);
	test_reset(&source_arena);
	test_reset(&builder_arena);
	test_reset(&frame_arena);
}

int main(void)
{
	test_w01_examples_and_deduplication();
	test_collisions_parents_and_negative_cases();
	test_builder_failure_atomicity();
	test_failure_atomicity();
	test_source_maps();
	return 0;
}
