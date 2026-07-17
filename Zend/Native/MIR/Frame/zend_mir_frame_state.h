/* Immutable, target-neutral ZNMIR frame-state construction contract. */

#ifndef ZEND_MIR_FRAME_FRAME_STATE_H
#define ZEND_MIR_FRAME_FRAME_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "Zend/Native/MIR/zend_mir.h"

typedef enum _zend_mir_frame_status {
	ZEND_MIR_FRAME_STATUS_OK = 0,
	ZEND_MIR_FRAME_STATUS_MISSING_REQUIRED = 1,
	ZEND_MIR_FRAME_STATUS_INVALID_ID = 2,
	ZEND_MIR_FRAME_STATUS_INVALID_ENUM = 3,
	ZEND_MIR_FRAME_STATUS_INVALID_PARENT = 4,
	ZEND_MIR_FRAME_STATUS_PARENT_CYCLE = 5,
	ZEND_MIR_FRAME_STATUS_DUPLICATE_SLOT = 6,
	ZEND_MIR_FRAME_STATUS_DUPLICATE_ROOT = 7,
	ZEND_MIR_FRAME_STATUS_DUPLICATE_CLEANUP = 8,
	ZEND_MIR_FRAME_STATUS_ROOT_MISMATCH = 9,
	ZEND_MIR_FRAME_STATUS_CLEANUP_MISMATCH = 10,
	ZEND_MIR_FRAME_STATUS_INVALID_CONTINUATION = 11,
	ZEND_MIR_FRAME_STATUS_INVALID_SUSPEND = 12,
	ZEND_MIR_FRAME_STATUS_INVALID_RESUME = 13,
	ZEND_MIR_FRAME_STATUS_NONCANONICAL = 14,
	ZEND_MIR_FRAME_STATUS_OVERFLOW = 15,
	ZEND_MIR_FRAME_STATUS_OUT_OF_MEMORY = 16,
	ZEND_MIR_FRAME_STATUS_FINALIZED = 17
} zend_mir_frame_status;

typedef enum _zend_mir_frame_target_kind {
	ZEND_MIR_FRAME_TARGET_NONE = 0,
	ZEND_MIR_FRAME_TARGET_SELF = 1,
	ZEND_MIR_FRAME_TARGET_EXISTING = 2
} zend_mir_frame_target_kind;

typedef struct _zend_mir_frame_continuation_spec {
	zend_mir_continuation_kind kind;
	zend_mir_frame_target_kind target_kind;
	zend_mir_frame_state_id frame_state_id;
	uint32_t opline_index;
} zend_mir_frame_continuation_spec;

/*
 * This is an immutable snapshot. The extra scalars retain W01 fields that do
 * not belong to the frozen 1.0 frame-state reference.
 */
typedef struct _zend_mir_frame_state_record {
	zend_mir_frame_state_ref ref;
	zend_mir_symbol_id function_name_symbol_id;
	zend_mir_op_array_id op_array_id;
	zend_mir_frame_state_id owner_frame_id;
	bool code_version_immutable;
	bool code_version_active;
} zend_mir_frame_state_record;

typedef struct _zend_mir_frame_builder zend_mir_frame_builder;
typedef struct _zend_mir_frame_table zend_mir_frame_table;

/* Allocators belong to their caller and must outlive every object using them. */
zend_mir_frame_builder *zend_mir_frame_builder_create(zend_mir_allocator allocator);

zend_mir_frame_status zend_mir_frame_builder_set_function(zend_mir_frame_builder *builder,
		zend_mir_function_id function_id, zend_mir_symbol_id function_name_symbol_id,
		zend_mir_op_array_id op_array_id, zend_mir_function_kind function_kind);
zend_mir_frame_status zend_mir_frame_builder_set_opline(zend_mir_frame_builder *builder,
		uint32_t opline_index, zend_mir_opline_phase opline_phase);
zend_mir_frame_status zend_mir_frame_builder_set_parent(zend_mir_frame_builder *builder,
		zend_mir_frame_state_id parent_id);
zend_mir_frame_status zend_mir_frame_builder_add_slot(zend_mir_frame_builder *builder,
		const zend_mir_frame_slot_ref *slot);
zend_mir_frame_status zend_mir_frame_builder_add_root(zend_mir_frame_builder *builder,
		uint32_t slot_id);
zend_mir_frame_status zend_mir_frame_builder_add_cleanup(zend_mir_frame_builder *builder,
		const zend_mir_cleanup_ref *cleanup);
zend_mir_frame_status zend_mir_frame_builder_finish_layout(zend_mir_frame_builder *builder);
zend_mir_frame_status zend_mir_frame_builder_set_continuations(zend_mir_frame_builder *builder,
		zend_mir_frame_continuation_spec return_continuation,
		zend_mir_frame_continuation_spec exception_continuation,
		zend_mir_frame_continuation_spec bailout_continuation);
zend_mir_frame_status zend_mir_frame_builder_set_suspend(zend_mir_frame_builder *builder,
		zend_mir_suspend_kind suspend_kind, uint32_t suspend_state_id);
zend_mir_frame_status zend_mir_frame_builder_set_code_version(zend_mir_frame_builder *builder,
		uint32_t code_version_id, bool immutable, bool active);
zend_mir_frame_status zend_mir_frame_builder_set_resume(zend_mir_frame_builder *builder,
		zend_mir_resume_ref resume);
zend_mir_frame_status zend_mir_frame_builder_set_safepoint(zend_mir_frame_builder *builder,
		zend_mir_safepoint_class safepoint_class, bool canonical);

/* hash_mask is normally UINT64_MAX; zero deliberately forces collision tests. */
zend_mir_frame_table *zend_mir_frame_table_create(
		zend_mir_allocator allocator, uint64_t hash_mask);
zend_mir_frame_status zend_mir_frame_table_intern(zend_mir_frame_table *table,
		zend_mir_frame_builder *builder, zend_mir_frame_state_id *out);

uint32_t zend_mir_frame_table_count(const zend_mir_frame_table *table);
bool zend_mir_frame_table_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, zend_mir_frame_state_record *out);
bool zend_mir_frame_table_slot_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, uint32_t index, zend_mir_frame_slot_ref *out);
bool zend_mir_frame_table_root_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, uint32_t index, uint32_t *slot_id_out);
bool zend_mir_frame_table_cleanup_at(const zend_mir_frame_table *table,
		zend_mir_frame_state_id id, uint32_t index, zend_mir_cleanup_ref *out);

/* Callback-compatible flat-pool views for zend_mir_view. */
uint32_t zend_mir_frame_table_frame_state_count(const void *context);
bool zend_mir_frame_table_frame_state_at(
		const void *context, uint32_t index, zend_mir_frame_state_ref *out);
uint32_t zend_mir_frame_table_frame_slot_count(const void *context);
bool zend_mir_frame_table_frame_slot_at(
		const void *context, uint32_t index, zend_mir_frame_slot_ref *out);
uint32_t zend_mir_frame_table_root_count(const void *context);
bool zend_mir_frame_table_flat_root_at(
		const void *context, uint32_t index, uint32_t *slot_id_out);
uint32_t zend_mir_frame_table_cleanup_count(const void *context);
bool zend_mir_frame_table_flat_cleanup_at(
		const void *context, uint32_t index, zend_mir_cleanup_ref *out);

#endif /* ZEND_MIR_FRAME_FRAME_STATE_H */
