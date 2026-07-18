#include "zend_mir_frame_internal.h"

#include <limits.h>
#include <string.h>

bool zend_mir_frame_checked_add(uint32_t left, uint32_t right, uint32_t *out)
{
	if (out == NULL || right > ZEND_MIR_ID_MAX || left > ZEND_MIR_ID_MAX - right) {
		return false;
	}
	*out = left + right;
	return true;
}

void *zend_mir_frame_allocate(zend_mir_allocator allocator, size_t size, size_t alignment)
{
	if (allocator.allocate == NULL || size == 0 || alignment == 0) {
		return NULL;
	}
	return allocator.allocate(allocator.context, size, alignment);
}

static zend_mir_frame_status zend_mir_frame_builder_mutable(zend_mir_frame_builder *builder)
{
	if (builder == NULL) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if (builder->finalized) {
		return ZEND_MIR_FRAME_STATUS_FINALIZED;
	}
	return ZEND_MIR_FRAME_STATUS_OK;
}

static bool zend_mir_frame_enum_is_valid(int value, int count)
{
	return value >= 0 && value < count;
}

static zend_mir_frame_status zend_mir_frame_builder_reserve(zend_mir_frame_builder *builder,
		void **items, uint32_t *capacity, uint32_t count, size_t element_size, size_t alignment)
{
	uint32_t new_capacity;
	void *replacement;

	if (count < *capacity) {
		return ZEND_MIR_FRAME_STATUS_OK;
	}
	if (*capacity == 0) {
		new_capacity = 4;
	} else if (*capacity > ZEND_MIR_ID_MAX / 2) {
		new_capacity = ZEND_MIR_ID_MAX;
	} else {
		new_capacity = *capacity * 2;
	}
	if (new_capacity <= count || element_size > SIZE_MAX / new_capacity) {
		return ZEND_MIR_FRAME_STATUS_OVERFLOW;
	}
	replacement = zend_mir_frame_allocate(
		builder->allocator, element_size * new_capacity, alignment);
	if (replacement == NULL) {
		return ZEND_MIR_FRAME_STATUS_OUT_OF_MEMORY;
	}
	if (count != 0) {
		memcpy(replacement, *items, element_size * count);
	}
	*items = replacement;
	*capacity = new_capacity;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_builder *zend_mir_frame_builder_create(zend_mir_allocator allocator)
{
	zend_mir_frame_builder *builder = zend_mir_frame_allocate(
		allocator, sizeof(zend_mir_frame_builder), _Alignof(zend_mir_frame_builder));

	if (builder == NULL) {
		return NULL;
	}
	memset(builder, 0, sizeof(*builder));
	builder->allocator = allocator;
	builder->record.ref.id = ZEND_MIR_ID_INVALID;
	builder->record.ref.function_id = ZEND_MIR_ID_INVALID;
	builder->record.ref.parent_id = ZEND_MIR_ID_INVALID;
	builder->record.function_name_symbol_id = ZEND_MIR_ID_INVALID;
	builder->record.op_array_id = ZEND_MIR_ID_INVALID;
	builder->record.owner_frame_id = ZEND_MIR_ID_INVALID;
	return builder;
}

zend_mir_frame_status zend_mir_frame_builder_set_function(zend_mir_frame_builder *builder,
		zend_mir_function_id function_id, zend_mir_symbol_id function_name_symbol_id,
		zend_mir_op_array_id op_array_id, zend_mir_function_kind function_kind)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.function_id = function_id;
	builder->record.function_name_symbol_id = function_name_symbol_id;
	builder->record.op_array_id = op_array_id;
	builder->record.ref.function_kind = function_kind;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_FUNCTION;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_opline(zend_mir_frame_builder *builder,
		uint32_t opline_index, zend_mir_opline_phase opline_phase)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.opline_index = opline_index;
	builder->record.ref.opline_phase = opline_phase;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_OPLINE;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_parent(zend_mir_frame_builder *builder,
		zend_mir_frame_state_id parent_id)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.parent_id = parent_id;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_PARENT;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_add_slot(zend_mir_frame_builder *builder,
		const zend_mir_frame_slot_ref *slot)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	if (slot == NULL) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if (builder->layout_finished) {
		return ZEND_MIR_FRAME_STATUS_FINALIZED;
	}
	status = zend_mir_frame_builder_reserve(builder, (void **) &builder->slots,
		&builder->slot_capacity, builder->slot_count, sizeof(*builder->slots),
		_Alignof(zend_mir_frame_slot_ref));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->slots[builder->slot_count++] = *slot;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_add_root(
		zend_mir_frame_builder *builder, uint32_t slot_id)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	if (builder->layout_finished) {
		return ZEND_MIR_FRAME_STATUS_FINALIZED;
	}
	status = zend_mir_frame_builder_reserve(builder, (void **) &builder->roots,
		&builder->root_capacity, builder->root_count, sizeof(*builder->roots),
		_Alignof(uint32_t));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->roots[builder->root_count++] = slot_id;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_add_cleanup(zend_mir_frame_builder *builder,
		const zend_mir_cleanup_ref *cleanup)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	if (cleanup == NULL) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if (builder->layout_finished) {
		return ZEND_MIR_FRAME_STATUS_FINALIZED;
	}
	status = zend_mir_frame_builder_reserve(builder, (void **) &builder->cleanups,
		&builder->cleanup_capacity, builder->cleanup_count, sizeof(*builder->cleanups),
		_Alignof(zend_mir_cleanup_ref));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->cleanups[builder->cleanup_count++] = *cleanup;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_finish_layout(zend_mir_frame_builder *builder)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->layout_finished = true;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_LAYOUT;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_continuations(zend_mir_frame_builder *builder,
		zend_mir_frame_continuation_spec return_continuation,
		zend_mir_frame_continuation_spec exception_continuation,
		zend_mir_frame_continuation_spec bailout_continuation)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->continuations[0] = return_continuation;
	builder->continuations[1] = exception_continuation;
	builder->continuations[2] = bailout_continuation;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_CONTINUATIONS;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_suspend(zend_mir_frame_builder *builder,
		zend_mir_suspend_kind suspend_kind, uint32_t suspend_state_id)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.suspend_kind = suspend_kind;
	builder->record.ref.suspend_state_id = suspend_state_id;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_SUSPEND;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_code_version(zend_mir_frame_builder *builder,
		uint32_t code_version_id, bool immutable, bool active)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.code_version_id = code_version_id;
	builder->record.code_version_immutable = immutable;
	builder->record.code_version_active = active;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_CODE_VERSION;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_resume(
		zend_mir_frame_builder *builder, zend_mir_resume_ref resume)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.resume = resume;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_RESUME;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_builder_set_safepoint(zend_mir_frame_builder *builder,
		zend_mir_safepoint_class safepoint_class, bool canonical)
{
	zend_mir_frame_status status = zend_mir_frame_builder_mutable(builder);

	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	builder->record.ref.safepoint_class = safepoint_class;
	builder->record.ref.canonical = canonical;
	builder->required |= ZEND_MIR_FRAME_REQUIRED_SAFEPOINT;
	return ZEND_MIR_FRAME_STATUS_OK;
}

static bool zend_mir_frame_slot_exists(const zend_mir_frame_builder *builder, uint32_t slot_id)
{
	uint32_t i;

	for (i = 0; i < builder->slot_count; i++) {
		if (builder->slots[i].slot_id == slot_id) {
			return true;
		}
	}
	return false;
}

static uint32_t zend_mir_frame_root_occurrences(
		const zend_mir_frame_builder *builder, uint32_t slot_id)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < builder->root_count; i++) {
		if (builder->roots[i] == slot_id) {
			count++;
		}
	}
	return count;
}

static uint32_t zend_mir_frame_cleanup_occurrences(
		const zend_mir_frame_builder *builder, uint32_t slot_id)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < builder->cleanup_count; i++) {
		if (builder->cleanups[i].slot_id == slot_id) {
			count++;
		}
	}
	return count;
}

static zend_mir_frame_status zend_mir_frame_validate_layout(
		const zend_mir_frame_builder *builder)
{
	uint32_t i;
	uint32_t j;

	for (i = 0; i < builder->slot_count; i++) {
		const zend_mir_frame_slot_ref *slot = &builder->slots[i];

		if (!zend_mir_id_is_valid(slot->slot_id)
				|| (slot->materialization != ZEND_MIR_MATERIALIZATION_UNDEF
					&& !zend_mir_id_is_valid(slot->value_id))) {
			return ZEND_MIR_FRAME_STATUS_INVALID_ID;
		}
		if (!zend_mir_frame_enum_is_valid(
				slot->kind, ZEND_MIR_FRAME_SLOT_KIND_COUNT)
				|| !zend_mir_frame_enum_is_valid(
					slot->representation, ZEND_MIR_FRAME_SLOT_REPRESENTATION_COUNT)
				|| !zend_mir_frame_enum_is_valid(
					slot->materialization, ZEND_MIR_MATERIALIZATION_COUNT)
				|| !zend_mir_frame_enum_is_valid(
					slot->ownership, ZEND_MIR_FRAME_SLOT_OWNERSHIP_COUNT)) {
			return ZEND_MIR_FRAME_STATUS_INVALID_ENUM;
		}
		for (j = 0; j < i; j++) {
			if (builder->slots[j].slot_id == slot->slot_id) {
				return ZEND_MIR_FRAME_STATUS_DUPLICATE_SLOT;
			}
		}
	}
	for (i = 0; i < builder->root_count; i++) {
		if (!zend_mir_frame_slot_exists(builder, builder->roots[i])) {
			return ZEND_MIR_FRAME_STATUS_ROOT_MISMATCH;
		}
		for (j = 0; j < i; j++) {
			if (builder->roots[j] == builder->roots[i]) {
				return ZEND_MIR_FRAME_STATUS_DUPLICATE_ROOT;
			}
		}
	}
	for (i = 0; i < builder->cleanup_count; i++) {
		const zend_mir_cleanup_ref *cleanup = &builder->cleanups[i];

		if (!zend_mir_frame_slot_exists(builder, cleanup->slot_id)) {
			return ZEND_MIR_FRAME_STATUS_CLEANUP_MISMATCH;
		}
		if (!zend_mir_frame_enum_is_valid(
				cleanup->action, ZEND_MIR_CLEANUP_ACTION_COUNT)
				|| !zend_mir_frame_enum_is_valid(
					cleanup->state, ZEND_MIR_CLEANUP_STATE_COUNT)) {
			return ZEND_MIR_FRAME_STATUS_INVALID_ENUM;
		}
		for (j = 0; j < i; j++) {
			if (builder->cleanups[j].slot_id == cleanup->slot_id) {
				return ZEND_MIR_FRAME_STATUS_DUPLICATE_CLEANUP;
			}
		}
	}
	for (i = 0; i < builder->slot_count; i++) {
		const zend_mir_frame_slot_ref *slot = &builder->slots[i];

		if (zend_mir_frame_root_occurrences(builder, slot->slot_id) != (slot->rooted ? 1U : 0U)) {
			return ZEND_MIR_FRAME_STATUS_ROOT_MISMATCH;
		}
		if (zend_mir_frame_cleanup_occurrences(builder, slot->slot_id)
				!= (slot->cleanup_required ? 1U : 0U)) {
			return ZEND_MIR_FRAME_STATUS_CLEANUP_MISMATCH;
		}
	}
	return ZEND_MIR_FRAME_STATUS_OK;
}

static zend_mir_frame_status zend_mir_frame_validate_continuation(
		const zend_mir_frame_table *table, zend_mir_frame_continuation_spec continuation)
{
	if (!zend_mir_frame_enum_is_valid(
			continuation.kind, ZEND_MIR_CONTINUATION_KIND_COUNT)
			|| !zend_mir_frame_enum_is_valid(
				continuation.target_kind, ZEND_MIR_FRAME_TARGET_EXISTING + 1)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION;
	}
	if (continuation.target_kind == ZEND_MIR_FRAME_TARGET_NONE) {
		if (continuation.frame_state_id != ZEND_MIR_ID_INVALID
				|| continuation.opline_index != ZEND_MIR_ID_INVALID
				|| (continuation.kind != ZEND_MIR_CONTINUATION_KIND_TERMINAL
					&& continuation.kind != ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT)) {
			return ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION;
		}
		return ZEND_MIR_FRAME_STATUS_OK;
	}
	if (continuation.kind == ZEND_MIR_CONTINUATION_KIND_TERMINAL
			|| continuation.kind == ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT
			|| continuation.opline_index == ZEND_MIR_ID_INVALID) {
		return ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION;
	}
	if (continuation.target_kind == ZEND_MIR_FRAME_TARGET_SELF) {
		return continuation.frame_state_id == ZEND_MIR_ID_INVALID
			? ZEND_MIR_FRAME_STATUS_OK : ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION;
	}
	return zend_mir_id_is_valid(continuation.frame_state_id)
			&& continuation.frame_state_id < table->count
		? ZEND_MIR_FRAME_STATUS_OK : ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION;
}

static zend_mir_frame_status zend_mir_frame_validate_suspend_resume(
		const zend_mir_frame_builder *builder)
{
	const zend_mir_frame_state_ref *ref = &builder->record.ref;
	const zend_mir_resume_ref *resume = &ref->resume;

	if (!zend_mir_frame_enum_is_valid(
			ref->suspend_kind, ZEND_MIR_SUSPEND_KIND_COUNT)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_SUSPEND;
	}
	if ((ref->suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE)
			!= (ref->suspend_state_id == ZEND_MIR_ID_INVALID)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_SUSPEND;
	}
	if (!resume->allowed) {
		if (resume->entry_kind != ZEND_MIR_RESUME_ENTRY_KIND_NONE
				|| resume->resume_id != ZEND_MIR_ID_INVALID
				|| resume->code_version_id != ZEND_MIR_ID_INVALID
				|| resume->target_opline_index != ZEND_MIR_ID_INVALID) {
			return ZEND_MIR_FRAME_STATUS_INVALID_RESUME;
		}
		if (ref->suspend_kind != ZEND_MIR_SUSPEND_KIND_NONE) {
			return ZEND_MIR_FRAME_STATUS_INVALID_RESUME;
		}
		return ref->opline_phase == ZEND_MIR_OPLINE_PHASE_SUSPENDED
			? ZEND_MIR_FRAME_STATUS_INVALID_SUSPEND : ZEND_MIR_FRAME_STATUS_OK;
	}
	if (resume->entry_kind != ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER
			|| !zend_mir_id_is_valid(resume->resume_id)
			|| !zend_mir_id_is_valid(resume->target_opline_index)
			|| resume->code_version_id != ref->code_version_id
			|| ref->suspend_kind == ZEND_MIR_SUSPEND_KIND_NONE
			|| ref->opline_phase != ZEND_MIR_OPLINE_PHASE_SUSPENDED
			|| !builder->record.code_version_active) {
		return ZEND_MIR_FRAME_STATUS_INVALID_RESUME;
	}
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_frame_validate_builder(
		const zend_mir_frame_table *table, const zend_mir_frame_builder *builder)
{
	zend_mir_frame_status status;
	uint32_t i;

	if (table == NULL || builder == NULL) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if (builder->finalized) {
		return ZEND_MIR_FRAME_STATUS_FINALIZED;
	}
	if (builder->required != ZEND_MIR_FRAME_REQUIRED_ALL) {
		return ZEND_MIR_FRAME_STATUS_MISSING_REQUIRED;
	}
	if (!zend_mir_id_is_valid(builder->record.ref.function_id)
			|| !zend_mir_id_is_valid(builder->record.function_name_symbol_id)
			|| !zend_mir_id_is_valid(builder->record.ref.code_version_id)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if (!zend_mir_frame_enum_is_valid(
			builder->record.ref.function_kind, ZEND_MIR_FUNCTION_KIND_COUNT)
			|| !zend_mir_frame_enum_is_valid(
				builder->record.ref.opline_phase, ZEND_MIR_OPLINE_PHASE_COUNT)
			|| !zend_mir_frame_enum_is_valid(
				builder->record.ref.safepoint_class, ZEND_MIR_SAFEPOINT_CLASS_COUNT)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ENUM;
	}
	if (builder->record.ref.function_kind == ZEND_MIR_FUNCTION_KIND_INTERNAL) {
		if (builder->record.op_array_id != ZEND_MIR_ID_INVALID
				|| builder->record.ref.opline_index != ZEND_MIR_ID_INVALID) {
			return ZEND_MIR_FRAME_STATUS_INVALID_ID;
		}
	} else if (!zend_mir_id_is_valid(builder->record.op_array_id)
			|| !zend_mir_id_is_valid(builder->record.ref.opline_index)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if (zend_mir_id_is_valid(builder->record.ref.parent_id)) {
		if (builder->record.ref.parent_id == table->count) {
			return ZEND_MIR_FRAME_STATUS_PARENT_CYCLE;
		}
		if (builder->record.ref.parent_id > table->count) {
			return ZEND_MIR_FRAME_STATUS_INVALID_PARENT;
		}
	}
	if (!builder->record.code_version_immutable || !builder->record.ref.canonical) {
		return ZEND_MIR_FRAME_STATUS_NONCANONICAL;
	}
	status = zend_mir_frame_validate_layout(builder);
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	for (i = 0; i < 3; i++) {
		status = zend_mir_frame_validate_continuation(table, builder->continuations[i]);
		if (status != ZEND_MIR_FRAME_STATUS_OK) {
			return status;
		}
	}
	if (builder->continuations[2].kind != ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT
			|| builder->continuations[2].target_kind != ZEND_MIR_FRAME_TARGET_NONE) {
		return ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION;
	}
	status = zend_mir_frame_validate_suspend_resume(builder);
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	if (builder->record.ref.safepoint_class == ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND
			&& builder->record.ref.suspend_kind != ZEND_MIR_SUSPEND_KIND_GENERATOR) {
		return ZEND_MIR_FRAME_STATUS_INVALID_SUSPEND;
	}
	if (builder->record.ref.safepoint_class == ZEND_MIR_SAFEPOINT_CLASS_FIBER_SWITCH
			&& builder->record.ref.suspend_kind != ZEND_MIR_SUSPEND_KIND_FIBER) {
		return ZEND_MIR_FRAME_STATUS_INVALID_SUSPEND;
	}
	return ZEND_MIR_FRAME_STATUS_OK;
}
