/* Deterministic source maps over stable source, op-array, and frame IDs. */

#ifndef ZEND_MIR_FRAME_SOURCE_MAP_H
#define ZEND_MIR_FRAME_SOURCE_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "zend_mir_frame_state.h"

typedef struct _zend_mir_source_map_table zend_mir_source_map_table;

zend_mir_source_map_table *zend_mir_source_map_table_create(
		zend_mir_allocator allocator, const zend_mir_frame_table *frames, uint64_t hash_mask);
zend_mir_frame_status zend_mir_source_map_table_intern(zend_mir_source_map_table *table,
		const zend_mir_source_map_ref *requested, zend_mir_source_map_id *out);

uint32_t zend_mir_source_map_table_count(const void *context);
bool zend_mir_source_map_table_at(
		const void *context, uint32_t index, zend_mir_source_map_ref *out);

/* Callback-compatible mutator entry point; use intern() when status is needed. */
bool zend_mir_source_map_table_add(void *context,
		const zend_mir_source_map_ref *requested, zend_mir_source_map_id *out);

#endif /* ZEND_MIR_FRAME_SOURCE_MAP_H */
