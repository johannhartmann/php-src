#include "zend_mir_frame_internal.h"

#include <string.h>

#define ZEND_MIR_FNV_OFFSET UINT64_C(14695981039346656037)
#define ZEND_MIR_FNV_PRIME UINT64_C(1099511628211)

static uint64_t zend_mir_frame_hash_u32(uint64_t hash, uint32_t value)
{
	uint32_t i;

	for (i = 0; i < 4; i++) {
		hash ^= (uint8_t) (value >> (i * 8));
		hash *= ZEND_MIR_FNV_PRIME;
	}
	return hash;
}

static uint64_t zend_mir_frame_hash_bool(uint64_t hash, bool value)
{
	return zend_mir_frame_hash_u32(hash, value ? 1U : 0U);
}

static uint64_t zend_mir_frame_hash_continuation(
		uint64_t hash, zend_mir_frame_continuation_spec continuation)
{
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) continuation.kind);
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) continuation.target_kind);
	if (continuation.target_kind == ZEND_MIR_FRAME_TARGET_EXISTING) {
		hash = zend_mir_frame_hash_u32(hash, continuation.frame_state_id);
	}
	return zend_mir_frame_hash_u32(hash, continuation.opline_index);
}

uint64_t zend_mir_frame_hash_builder(const zend_mir_frame_builder *builder)
{
	const zend_mir_frame_state_ref *ref = &builder->record.ref;
	uint64_t hash = ZEND_MIR_FNV_OFFSET;
	uint32_t i;

	hash = zend_mir_frame_hash_u32(hash, ref->function_id);
	hash = zend_mir_frame_hash_u32(hash, builder->record.function_name_symbol_id);
	hash = zend_mir_frame_hash_u32(hash, builder->record.op_array_id);
	hash = zend_mir_frame_hash_u32(hash, ref->parent_id);
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) ref->function_kind);
	hash = zend_mir_frame_hash_u32(hash, ref->opline_index);
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) ref->opline_phase);
	for (i = 0; i < 3; i++) {
		hash = zend_mir_frame_hash_continuation(hash, builder->continuations[i]);
	}
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) ref->suspend_kind);
	hash = zend_mir_frame_hash_u32(hash, ref->suspend_state_id);
	hash = zend_mir_frame_hash_u32(hash, ref->code_version_id);
	hash = zend_mir_frame_hash_bool(hash, builder->record.code_version_immutable);
	hash = zend_mir_frame_hash_bool(hash, builder->record.code_version_active);
	hash = zend_mir_frame_hash_bool(hash, ref->resume.allowed);
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) ref->resume.entry_kind);
	hash = zend_mir_frame_hash_u32(hash, ref->resume.resume_id);
	hash = zend_mir_frame_hash_u32(hash, ref->resume.code_version_id);
	hash = zend_mir_frame_hash_u32(hash, ref->resume.target_opline_index);
	hash = zend_mir_frame_hash_u32(hash, (uint32_t) ref->safepoint_class);
	hash = zend_mir_frame_hash_bool(hash, ref->canonical);
	hash = zend_mir_frame_hash_u32(hash, builder->slot_count);
	for (i = 0; i < builder->slot_count; i++) {
		const zend_mir_frame_slot_ref *slot = &builder->slots[i];

		hash = zend_mir_frame_hash_u32(hash, slot->slot_id);
		hash = zend_mir_frame_hash_u32(hash, slot->value_id);
		hash = zend_mir_frame_hash_u32(hash, slot->index);
		hash = zend_mir_frame_hash_u32(hash, (uint32_t) slot->kind);
		hash = zend_mir_frame_hash_u32(hash, (uint32_t) slot->representation);
		hash = zend_mir_frame_hash_u32(hash, (uint32_t) slot->materialization);
		hash = zend_mir_frame_hash_u32(hash, (uint32_t) slot->ownership);
		hash = zend_mir_frame_hash_bool(hash, slot->rooted);
		hash = zend_mir_frame_hash_bool(hash, slot->cleanup_required);
	}
	hash = zend_mir_frame_hash_u32(hash, builder->root_count);
	for (i = 0; i < builder->root_count; i++) {
		hash = zend_mir_frame_hash_u32(hash, builder->roots[i]);
	}
	hash = zend_mir_frame_hash_u32(hash, builder->cleanup_count);
	for (i = 0; i < builder->cleanup_count; i++) {
		hash = zend_mir_frame_hash_u32(hash, builder->cleanups[i].slot_id);
		hash = zend_mir_frame_hash_u32(hash, (uint32_t) builder->cleanups[i].action);
		hash = zend_mir_frame_hash_u32(hash, (uint32_t) builder->cleanups[i].state);
	}
	return hash;
}

static bool zend_mir_frame_slot_equal(
		const zend_mir_frame_slot_ref *left, const zend_mir_frame_slot_ref *right)
{
	return left->slot_id == right->slot_id
		&& left->value_id == right->value_id
		&& left->index == right->index
		&& left->kind == right->kind
		&& left->representation == right->representation
		&& left->materialization == right->materialization
		&& left->ownership == right->ownership
		&& left->rooted == right->rooted
		&& left->cleanup_required == right->cleanup_required;
}

static bool zend_mir_frame_cleanup_equal(
		const zend_mir_cleanup_ref *left, const zend_mir_cleanup_ref *right)
{
	return left->slot_id == right->slot_id
		&& left->action == right->action
		&& left->state == right->state;
}

static bool zend_mir_frame_resume_equal(
		const zend_mir_resume_ref *left, const zend_mir_resume_ref *right)
{
	return left->allowed == right->allowed
		&& left->entry_kind == right->entry_kind
		&& left->resume_id == right->resume_id
		&& left->code_version_id == right->code_version_id
		&& left->target_opline_index == right->target_opline_index;
}

static bool zend_mir_frame_continuation_matches(
		const zend_mir_frame_entry *entry, uint32_t index,
		zend_mir_frame_continuation_spec continuation)
{
	const zend_mir_continuation_ref *stored;

	if (index == 0) {
		stored = &entry->record.ref.return_continuation;
	} else if (index == 1) {
		stored = &entry->record.ref.exception_continuation;
	} else {
		stored = &entry->record.ref.bailout_continuation;
	}
	if (stored->kind != continuation.kind
			|| stored->opline_index != continuation.opline_index
			|| entry->continuation_targets[index] != continuation.target_kind) {
		return false;
	}
	if (continuation.target_kind == ZEND_MIR_FRAME_TARGET_NONE) {
		return stored->frame_state_id == ZEND_MIR_ID_INVALID;
	}
	if (continuation.target_kind == ZEND_MIR_FRAME_TARGET_SELF) {
		return stored->frame_state_id == entry->record.ref.id;
	}
	return stored->frame_state_id == continuation.frame_state_id;
}

bool zend_mir_frame_entry_equals_builder(const zend_mir_frame_table *table,
		const zend_mir_frame_entry *entry, const zend_mir_frame_builder *builder)
{
	const zend_mir_frame_state_ref *left = &entry->record.ref;
	const zend_mir_frame_state_ref *right = &builder->record.ref;
	uint32_t i;

	if (left->function_id != right->function_id
			|| entry->record.function_name_symbol_id != builder->record.function_name_symbol_id
			|| entry->record.op_array_id != builder->record.op_array_id
			|| left->parent_id != right->parent_id
			|| left->function_kind != right->function_kind
			|| left->opline_index != right->opline_index
			|| left->opline_phase != right->opline_phase
			|| left->suspend_kind != right->suspend_kind
			|| left->suspend_state_id != right->suspend_state_id
			|| left->code_version_id != right->code_version_id
			|| entry->record.code_version_immutable != builder->record.code_version_immutable
			|| entry->record.code_version_active != builder->record.code_version_active
			|| !zend_mir_frame_resume_equal(&left->resume, &right->resume)
			|| left->safepoint_class != right->safepoint_class
			|| left->canonical != right->canonical
			|| left->slots.count != builder->slot_count
			|| left->roots.count != builder->root_count
			|| left->cleanup_obligations.count != builder->cleanup_count) {
		return false;
	}
	for (i = 0; i < 3; i++) {
		if (!zend_mir_frame_continuation_matches(entry, i, builder->continuations[i])) {
			return false;
		}
	}
	for (i = 0; i < builder->slot_count; i++) {
		if (!zend_mir_frame_slot_equal(
				&table->slots[left->slots.offset + i], &builder->slots[i])) {
			return false;
		}
	}
	for (i = 0; i < builder->root_count; i++) {
		if (table->roots[left->roots.offset + i] != builder->roots[i]) {
			return false;
		}
	}
	for (i = 0; i < builder->cleanup_count; i++) {
		if (!zend_mir_frame_cleanup_equal(
				&table->cleanups[left->cleanup_obligations.offset + i], &builder->cleanups[i])) {
			return false;
		}
	}
	return true;
}

static zend_mir_frame_status zend_mir_frame_table_reserve(zend_mir_frame_table *table,
		void **items, uint32_t *capacity, uint32_t count, uint32_t needed,
		size_t element_size, size_t alignment)
{
	uint32_t new_capacity;
	void *replacement;

	if (needed <= *capacity) {
		return ZEND_MIR_FRAME_STATUS_OK;
	}
	new_capacity = *capacity == 0 ? 4 : *capacity;
	while (new_capacity < needed) {
		if (new_capacity > ZEND_MIR_ID_MAX / 2) {
			new_capacity = ZEND_MIR_ID_MAX;
			break;
		}
		new_capacity *= 2;
	}
	if (new_capacity < needed || element_size > SIZE_MAX / new_capacity) {
		return ZEND_MIR_FRAME_STATUS_OVERFLOW;
	}
	replacement = zend_mir_frame_allocate(
		table->allocator, element_size * new_capacity, alignment);
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

zend_mir_frame_table *zend_mir_frame_table_create(
		zend_mir_allocator allocator, uint64_t hash_mask)
{
	zend_mir_frame_table *table = zend_mir_frame_allocate(
		allocator, sizeof(zend_mir_frame_table), _Alignof(zend_mir_frame_table));

	if (table == NULL) {
		return NULL;
	}
	memset(table, 0, sizeof(*table));
	table->allocator = allocator;
	table->hash_mask = hash_mask;
	return table;
}

static zend_mir_continuation_ref zend_mir_frame_resolve_continuation(
		zend_mir_frame_state_id self, zend_mir_frame_continuation_spec spec)
{
	zend_mir_continuation_ref resolved;

	resolved.kind = spec.kind;
	resolved.opline_index = spec.opline_index;
	if (spec.target_kind == ZEND_MIR_FRAME_TARGET_SELF) {
		resolved.frame_state_id = self;
	} else if (spec.target_kind == ZEND_MIR_FRAME_TARGET_EXISTING) {
		resolved.frame_state_id = spec.frame_state_id;
	} else {
		resolved.frame_state_id = ZEND_MIR_ID_INVALID;
	}
	return resolved;
}

zend_mir_frame_status zend_mir_frame_table_intern(zend_mir_frame_table *table,
		zend_mir_frame_builder *builder, zend_mir_frame_state_id *out)
{
	zend_mir_frame_status status;
	uint64_t hash;
	uint32_t new_count;
	uint32_t new_slot_count;
	uint32_t new_root_count;
	uint32_t new_cleanup_count;
	uint32_t i;
	zend_mir_frame_entry entry;

	if (table == NULL || builder == NULL || out == NULL) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	status = zend_mir_frame_validate_builder(table, builder);
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	hash = zend_mir_frame_hash_builder(builder) & table->hash_mask;
	for (i = 0; i < table->count; i++) {
		if (table->entries[i].hash == hash
				&& zend_mir_frame_entry_equals_builder(table, &table->entries[i], builder)) {
			builder->finalized = true;
			*out = table->entries[i].record.ref.id;
			return ZEND_MIR_FRAME_STATUS_OK;
		}
	}
	if (!zend_mir_frame_checked_add(table->count, 1, &new_count)
			|| !zend_mir_frame_checked_add(table->slot_count, builder->slot_count, &new_slot_count)
			|| !zend_mir_frame_checked_add(table->root_count, builder->root_count, &new_root_count)
			|| !zend_mir_frame_checked_add(
				table->cleanup_count, builder->cleanup_count, &new_cleanup_count)) {
		return ZEND_MIR_FRAME_STATUS_OVERFLOW;
	}
	status = zend_mir_frame_table_reserve(table, (void **) &table->entries,
		&table->capacity, table->count, new_count, sizeof(*table->entries),
		_Alignof(zend_mir_frame_entry));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	status = zend_mir_frame_table_reserve(table, (void **) &table->slots,
		&table->slot_capacity, table->slot_count, new_slot_count, sizeof(*table->slots),
		_Alignof(zend_mir_frame_slot_ref));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	status = zend_mir_frame_table_reserve(table, (void **) &table->roots,
		&table->root_capacity, table->root_count, new_root_count, sizeof(*table->roots),
		_Alignof(uint32_t));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	status = zend_mir_frame_table_reserve(table, (void **) &table->cleanups,
		&table->cleanup_capacity, table->cleanup_count, new_cleanup_count,
		sizeof(*table->cleanups), _Alignof(zend_mir_cleanup_ref));
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	memset(&entry, 0, sizeof(entry));
	entry.record = builder->record;
	entry.record.ref.id = table->count;
	entry.record.owner_frame_id = table->count;
	entry.record.ref.slots.offset = table->slot_count;
	entry.record.ref.slots.count = builder->slot_count;
	entry.record.ref.roots.offset = table->root_count;
	entry.record.ref.roots.count = builder->root_count;
	entry.record.ref.cleanup_obligations.offset = table->cleanup_count;
	entry.record.ref.cleanup_obligations.count = builder->cleanup_count;
	entry.record.ref.return_continuation = zend_mir_frame_resolve_continuation(
		table->count, builder->continuations[0]);
	entry.record.ref.exception_continuation = zend_mir_frame_resolve_continuation(
		table->count, builder->continuations[1]);
	entry.record.ref.bailout_continuation = zend_mir_frame_resolve_continuation(
		table->count, builder->continuations[2]);
	for (i = 0; i < 3; i++) {
		entry.continuation_targets[i] = builder->continuations[i].target_kind;
	}
	entry.hash = hash;
	if (builder->slot_count != 0) {
		memcpy(&table->slots[table->slot_count], builder->slots,
			sizeof(*table->slots) * builder->slot_count);
	}
	if (builder->root_count != 0) {
		memcpy(&table->roots[table->root_count], builder->roots,
			sizeof(*table->roots) * builder->root_count);
	}
	if (builder->cleanup_count != 0) {
		memcpy(&table->cleanups[table->cleanup_count], builder->cleanups,
			sizeof(*table->cleanups) * builder->cleanup_count);
	}
	table->entries[table->count] = entry;
	table->slot_count = new_slot_count;
	table->root_count = new_root_count;
	table->cleanup_count = new_cleanup_count;
	table->count = new_count;
	builder->finalized = true;
	*out = entry.record.ref.id;
	return ZEND_MIR_FRAME_STATUS_OK;
}

uint32_t zend_mir_frame_table_count(const zend_mir_frame_table *table)
{
	return table == NULL ? 0 : table->count;
}

bool zend_mir_frame_table_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, zend_mir_frame_state_record *out)
{
	if (table == NULL || out == NULL || id >= table->count) {
		return false;
	}
	*out = table->entries[id].record;
	return true;
}

bool zend_mir_frame_table_slot_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, uint32_t index, zend_mir_frame_slot_ref *out)
{
	zend_mir_span span;

	if (table == NULL || out == NULL || id >= table->count) {
		return false;
	}
	span = table->entries[id].record.ref.slots;
	if (index >= span.count) {
		return false;
	}
	*out = table->slots[span.offset + index];
	return true;
}

bool zend_mir_frame_table_root_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, uint32_t index, uint32_t *slot_id_out)
{
	zend_mir_span span;

	if (table == NULL || slot_id_out == NULL || id >= table->count) {
		return false;
	}
	span = table->entries[id].record.ref.roots;
	if (index >= span.count) {
		return false;
	}
	*slot_id_out = table->roots[span.offset + index];
	return true;
}

bool zend_mir_frame_table_cleanup_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, uint32_t index, zend_mir_cleanup_ref *out)
{
	zend_mir_span span;

	if (table == NULL || out == NULL || id >= table->count) {
		return false;
	}
	span = table->entries[id].record.ref.cleanup_obligations;
	if (index >= span.count) {
		return false;
	}
	*out = table->cleanups[span.offset + index];
	return true;
}

uint32_t zend_mir_frame_table_frame_state_count(const void *context)
{
	return zend_mir_frame_table_count((const zend_mir_frame_table *) context);
}

bool zend_mir_frame_table_frame_state_at(
		const void *context, uint32_t index, zend_mir_frame_state_ref *out)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	if (table == NULL || out == NULL || index >= table->count) {
		return false;
	}
	*out = table->entries[index].record.ref;
	return true;
}

uint32_t zend_mir_frame_table_frame_slot_count(const void *context)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	return table == NULL ? 0 : table->slot_count;
}

bool zend_mir_frame_table_frame_slot_at(
		const void *context, uint32_t index, zend_mir_frame_slot_ref *out)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	if (table == NULL || out == NULL || index >= table->slot_count) {
		return false;
	}
	*out = table->slots[index];
	return true;
}

uint32_t zend_mir_frame_table_root_count(const void *context)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	return table == NULL ? 0 : table->root_count;
}

bool zend_mir_frame_table_flat_root_at(
		const void *context, uint32_t index, uint32_t *slot_id_out)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	if (table == NULL || slot_id_out == NULL || index >= table->root_count) {
		return false;
	}
	*slot_id_out = table->roots[index];
	return true;
}

uint32_t zend_mir_frame_table_cleanup_count(const void *context)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	return table == NULL ? 0 : table->cleanup_count;
}

bool zend_mir_frame_table_flat_cleanup_at(
		const void *context, uint32_t index, zend_mir_cleanup_ref *out)
{
	const zend_mir_frame_table *table = (const zend_mir_frame_table *) context;

	if (table == NULL || out == NULL || index >= table->cleanup_count) {
		return false;
	}
	*out = table->cleanups[index];
	return true;
}
