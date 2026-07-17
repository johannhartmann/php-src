#ifndef ZEND_MIR_FRAME_INTERNAL_H
#define ZEND_MIR_FRAME_INTERNAL_H

#include <stddef.h>

#include "zend_mir_frame_state.h"

#define ZEND_MIR_FRAME_REQUIRED_FUNCTION (UINT32_C(1) << 0)
#define ZEND_MIR_FRAME_REQUIRED_OPLINE (UINT32_C(1) << 1)
#define ZEND_MIR_FRAME_REQUIRED_PARENT (UINT32_C(1) << 2)
#define ZEND_MIR_FRAME_REQUIRED_LAYOUT (UINT32_C(1) << 3)
#define ZEND_MIR_FRAME_REQUIRED_CONTINUATIONS (UINT32_C(1) << 4)
#define ZEND_MIR_FRAME_REQUIRED_SUSPEND (UINT32_C(1) << 5)
#define ZEND_MIR_FRAME_REQUIRED_CODE_VERSION (UINT32_C(1) << 6)
#define ZEND_MIR_FRAME_REQUIRED_RESUME (UINT32_C(1) << 7)
#define ZEND_MIR_FRAME_REQUIRED_SAFEPOINT (UINT32_C(1) << 8)
#define ZEND_MIR_FRAME_REQUIRED_ALL ((UINT32_C(1) << 9) - 1)

struct _zend_mir_frame_builder {
	zend_mir_allocator allocator;
	zend_mir_frame_state_record record;
	zend_mir_frame_continuation_spec continuations[3];
	zend_mir_frame_slot_ref *slots;
	uint32_t *roots;
	zend_mir_cleanup_ref *cleanups;
	uint32_t slot_count;
	uint32_t slot_capacity;
	uint32_t root_count;
	uint32_t root_capacity;
	uint32_t cleanup_count;
	uint32_t cleanup_capacity;
	uint32_t required;
	bool layout_finished;
	bool finalized;
};

typedef struct _zend_mir_frame_entry {
	zend_mir_frame_state_record record;
	zend_mir_frame_target_kind continuation_targets[3];
	uint64_t hash;
} zend_mir_frame_entry;

struct _zend_mir_frame_table {
	zend_mir_allocator allocator;
	uint64_t hash_mask;
	zend_mir_frame_entry *entries;
	zend_mir_frame_slot_ref *slots;
	uint32_t *roots;
	zend_mir_cleanup_ref *cleanups;
	uint32_t count;
	uint32_t capacity;
	uint32_t slot_count;
	uint32_t slot_capacity;
	uint32_t root_count;
	uint32_t root_capacity;
	uint32_t cleanup_count;
	uint32_t cleanup_capacity;
};

bool zend_mir_frame_checked_add(uint32_t left, uint32_t right, uint32_t *out);
void *zend_mir_frame_allocate(zend_mir_allocator allocator, size_t size, size_t alignment);
zend_mir_frame_status zend_mir_frame_validate_builder(
		const zend_mir_frame_table *table, const zend_mir_frame_builder *builder);
uint64_t zend_mir_frame_hash_builder(const zend_mir_frame_builder *builder);
bool zend_mir_frame_entry_equals_builder(const zend_mir_frame_table *table,
		const zend_mir_frame_entry *entry, const zend_mir_frame_builder *builder);

#endif /* ZEND_MIR_FRAME_INTERNAL_H */
