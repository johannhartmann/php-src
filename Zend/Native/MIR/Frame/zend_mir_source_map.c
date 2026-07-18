#include "zend_mir_source_map.h"

#include <stddef.h>
#include <string.h>

typedef struct _zend_mir_source_map_entry {
	zend_mir_source_map_ref record;
	uint64_t hash;
} zend_mir_source_map_entry;

struct _zend_mir_source_map_table {
	zend_mir_allocator allocator;
	const zend_mir_frame_table *frames;
	uint64_t hash_mask;
	zend_mir_source_map_entry *entries;
	uint32_t count;
	uint32_t capacity;
};

#define ZEND_MIR_SOURCE_FNV_OFFSET UINT64_C(14695981039346656037)
#define ZEND_MIR_SOURCE_FNV_PRIME UINT64_C(1099511628211)

static void *zend_mir_source_map_allocate(
		zend_mir_allocator allocator, size_t size, size_t alignment)
{
	if (allocator.allocate == NULL || size == 0 || alignment == 0) {
		return NULL;
	}
	return allocator.allocate(allocator.context, size, alignment);
}

static uint64_t zend_mir_source_map_hash_u32(uint64_t hash, uint32_t value)
{
	uint32_t i;

	for (i = 0; i < 4; i++) {
		hash ^= (uint8_t) (value >> (i * 8));
		hash *= ZEND_MIR_SOURCE_FNV_PRIME;
	}
	return hash;
}

static uint64_t zend_mir_source_map_hash(const zend_mir_source_map_ref *record)
{
	uint64_t hash = ZEND_MIR_SOURCE_FNV_OFFSET;

	hash = zend_mir_source_map_hash_u32(hash, record->source_position_id);
	hash = zend_mir_source_map_hash_u32(hash, record->op_array_id);
	hash = zend_mir_source_map_hash_u32(hash, record->opline_index);
	hash = zend_mir_source_map_hash_u32(hash, (uint32_t) record->opline_phase);
	return zend_mir_source_map_hash_u32(hash, record->owner_frame_id);
}

static bool zend_mir_source_map_equal(
		const zend_mir_source_map_ref *left, const zend_mir_source_map_ref *right)
{
	return left->source_position_id == right->source_position_id
		&& left->op_array_id == right->op_array_id
		&& left->opline_index == right->opline_index
		&& left->opline_phase == right->opline_phase
		&& left->owner_frame_id == right->owner_frame_id;
}

static zend_mir_frame_status zend_mir_source_map_reserve(
		zend_mir_source_map_table *table, uint32_t needed)
{
	uint32_t capacity;
	zend_mir_source_map_entry *replacement;

	if (needed <= table->capacity) {
		return ZEND_MIR_FRAME_STATUS_OK;
	}
	capacity = table->capacity == 0 ? 4 : table->capacity;
	while (capacity < needed) {
		if (capacity > ZEND_MIR_ID_MAX / 2) {
			capacity = ZEND_MIR_ID_MAX;
			break;
		}
		capacity *= 2;
	}
	if (capacity < needed || sizeof(*replacement) > SIZE_MAX / capacity) {
		return ZEND_MIR_FRAME_STATUS_OVERFLOW;
	}
	replacement = zend_mir_source_map_allocate(table->allocator,
		sizeof(*replacement) * capacity, _Alignof(zend_mir_source_map_entry));
	if (replacement == NULL) {
		return ZEND_MIR_FRAME_STATUS_OUT_OF_MEMORY;
	}
	if (table->count != 0) {
		memcpy(replacement, table->entries, sizeof(*replacement) * table->count);
	}
	table->entries = replacement;
	table->capacity = capacity;
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_source_map_table *zend_mir_source_map_table_create(
		zend_mir_allocator allocator, const zend_mir_frame_table *frames, uint64_t hash_mask)
{
	zend_mir_source_map_table *table;

	if (frames == NULL) {
		return NULL;
	}
	table = zend_mir_source_map_allocate(
		allocator, sizeof(*table), _Alignof(zend_mir_source_map_table));
	if (table == NULL) {
		return NULL;
	}
	memset(table, 0, sizeof(*table));
	table->allocator = allocator;
	table->frames = frames;
	table->hash_mask = hash_mask;
	return table;
}

static zend_mir_frame_status zend_mir_source_map_validate(
		const zend_mir_source_map_table *table, const zend_mir_source_map_ref *requested)
{
	zend_mir_frame_state_record owner;

	if (!zend_mir_id_is_valid(requested->source_position_id)
			|| !zend_mir_id_is_valid(requested->op_array_id)
			|| !zend_mir_id_is_valid(requested->owner_frame_id)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	if ((int) requested->opline_phase < 0
			|| requested->opline_phase >= ZEND_MIR_OPLINE_PHASE_COUNT) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ENUM;
	}
	if (!zend_mir_frame_table_at(table->frames, requested->owner_frame_id, &owner)) {
		return ZEND_MIR_FRAME_STATUS_INVALID_PARENT;
	}
	if (owner.owner_frame_id != requested->owner_frame_id
			|| owner.op_array_id != requested->op_array_id
			|| owner.ref.opline_index != requested->opline_index
			|| owner.ref.opline_phase != requested->opline_phase) {
		return ZEND_MIR_FRAME_STATUS_NONCANONICAL;
	}
	return ZEND_MIR_FRAME_STATUS_OK;
}

zend_mir_frame_status zend_mir_source_map_table_intern(zend_mir_source_map_table *table,
		const zend_mir_source_map_ref *requested, zend_mir_source_map_id *out)
{
	zend_mir_frame_status status;
	uint64_t hash;
	uint32_t i;
	uint32_t new_count;
	zend_mir_source_map_entry entry;

	if (table == NULL || requested == NULL || out == NULL) {
		return ZEND_MIR_FRAME_STATUS_INVALID_ID;
	}
	status = zend_mir_source_map_validate(table, requested);
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	hash = zend_mir_source_map_hash(requested) & table->hash_mask;
	for (i = 0; i < table->count; i++) {
		if (table->entries[i].hash == hash
				&& zend_mir_source_map_equal(&table->entries[i].record, requested)) {
			*out = table->entries[i].record.id;
			return ZEND_MIR_FRAME_STATUS_OK;
		}
	}
	if (table->count >= ZEND_MIR_ID_MAX) {
		return ZEND_MIR_FRAME_STATUS_OVERFLOW;
	}
	new_count = table->count + 1;
	status = zend_mir_source_map_reserve(table, new_count);
	if (status != ZEND_MIR_FRAME_STATUS_OK) {
		return status;
	}
	entry.record = *requested;
	entry.record.id = table->count;
	entry.hash = hash;
	table->entries[table->count] = entry;
	table->count = new_count;
	*out = entry.record.id;
	return ZEND_MIR_FRAME_STATUS_OK;
}

uint32_t zend_mir_source_map_table_count(const void *context)
{
	const zend_mir_source_map_table *table = (const zend_mir_source_map_table *) context;

	return table == NULL ? 0 : table->count;
}

bool zend_mir_source_map_table_at(
		const void *context, uint32_t index, zend_mir_source_map_ref *out)
{
	const zend_mir_source_map_table *table = (const zend_mir_source_map_table *) context;

	if (table == NULL || out == NULL || index >= table->count) {
		return false;
	}
	*out = table->entries[index].record;
	return true;
}

bool zend_mir_source_map_table_add(void *context,
		const zend_mir_source_map_ref *requested, zend_mir_source_map_id *out)
{
	return zend_mir_source_map_table_intern(
		(zend_mir_source_map_table *) context, requested, out) == ZEND_MIR_FRAME_STATUS_OK;
}
