#ifndef ZEND_MIR_ID_INDEX_H
#define ZEND_MIR_ID_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _zend_mir_id_index_entry {
	uint32_t id;
	uint32_t index_plus_one;
} zend_mir_id_index_entry;

static inline uint32_t zend_mir_id_index_hash(uint32_t id)
{
	id ^= id >> 16;
	id *= UINT32_C(0x7feb352d);
	id ^= id >> 15;
	id *= UINT32_C(0x846ca68b);
	id ^= id >> 16;
	return id;
}

static inline bool zend_mir_id_index_capacity(
		uint32_t count, uint32_t *capacity_out)
{
	uint32_t capacity = 8;

	if (capacity_out == NULL || count > UINT32_MAX / 2) {
		return false;
	}
	while (count > capacity / 2) {
		if (capacity > UINT32_MAX / 2) {
			return false;
		}
		capacity *= 2;
	}
	*capacity_out = capacity;
	return true;
}

static inline bool zend_mir_id_index_insert(
		zend_mir_id_index_entry *entries, uint32_t capacity,
		uint32_t id, uint32_t index, bool *duplicate)
{
	uint32_t slot;
	uint32_t probe;

	if (entries == NULL || capacity == 0 || (capacity & (capacity - 1)) != 0
			|| index == UINT32_MAX) {
		return false;
	}
	slot = zend_mir_id_index_hash(id) & (capacity - 1);
	for (probe = 0; probe < capacity; probe++) {
		zend_mir_id_index_entry *entry = &entries[slot];
		if (entry->index_plus_one == 0) {
			entry->id = id;
			entry->index_plus_one = index + 1;
			return true;
		}
		if (entry->id == id) {
			if (duplicate != NULL) {
				*duplicate = true;
			}
			return true;
		}
		slot = (slot + 1) & (capacity - 1);
	}
	return false;
}

static inline int32_t zend_mir_id_index_find(
		const zend_mir_id_index_entry *entries, uint32_t capacity, uint32_t id)
{
	uint32_t slot;
	uint32_t probe;

	if (entries == NULL || capacity == 0 || (capacity & (capacity - 1)) != 0) {
		return -1;
	}
	slot = zend_mir_id_index_hash(id) & (capacity - 1);
	for (probe = 0; probe < capacity; probe++) {
		const zend_mir_id_index_entry *entry = &entries[slot];
		if (entry->index_plus_one == 0) {
			return -1;
		}
		if (entry->id == id) {
			return (int32_t) (entry->index_plus_one - 1);
		}
		slot = (slot + 1) & (capacity - 1);
	}
	return -1;
}

#endif /* ZEND_MIR_ID_INDEX_H */
