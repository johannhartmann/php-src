#ifndef ZEND_MIR_VERIFY_INTERNAL_H
#define ZEND_MIR_VERIFY_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zend_mir_verify.h"

typedef struct _zend_mir_verify_function {
	zend_mir_function_record record;
} zend_mir_verify_function;

typedef struct _zend_mir_verify_block {
	zend_mir_block_record record;
	uint32_t successors_offset;
	uint32_t successors_count;
	uint32_t predecessors_offset;
	uint32_t predecessors_count;
} zend_mir_verify_block;

typedef struct _zend_mir_verify_instruction {
	zend_mir_instruction_record record;
	uint32_t operands_offset;
	uint32_t operands_count;
} zend_mir_verify_instruction;

typedef struct _zend_mir_verify_value {
	zend_mir_value_record record;
} zend_mir_verify_value;

typedef struct _zend_mir_verify_constant {
	zend_mir_constant_record record;
} zend_mir_verify_constant;

typedef struct _zend_mir_verify_frame {
	zend_mir_frame_state_ref record;
} zend_mir_verify_frame;

typedef struct _zend_mir_verify_source {
	zend_mir_source_position_ref record;
} zend_mir_verify_source;

typedef struct _zend_mir_verify_source_map {
	zend_mir_source_map_ref record;
} zend_mir_verify_source_map;

typedef struct _zend_mir_verify_allocation {
	struct _zend_mir_verify_allocation *next;
	max_align_t alignment;
	unsigned char data[];
} zend_mir_verify_allocation;

typedef struct _zend_mir_verify_pending_diagnostic {
	zend_mir_diagnostic diagnostic;
	zend_mir_verify_code verify_code;
	zend_mir_value_id operand_id;
	uint32_t sequence;
} zend_mir_verify_pending_diagnostic;

typedef struct _zend_mir_verify_context {
	const zend_mir_view *view;
	zend_mir_diagnostic_sink *diagnostics;
	zend_mir_module_id module_id;
	uint32_t diagnostic_capacity;
	uint32_t diagnostics_reported;
	zend_mir_verify_pending_diagnostic
		pending_diagnostics[ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT];
	bool valid;
	bool halted;
	bool identifiers_valid;

	zend_mir_verify_allocation *allocations;

	uint32_t function_count;
	uint32_t block_count;
	uint32_t instruction_count;
	uint32_t value_count;
	uint32_t constant_count;
	uint32_t frame_count;
	uint32_t source_count;
	uint32_t source_map_count;
	uint32_t slot_count;
	uint32_t root_count;
	uint32_t cleanup_count;

	zend_mir_verify_function *functions;
	zend_mir_verify_block *blocks;
	zend_mir_verify_instruction *instructions;
	zend_mir_verify_value *values;
	zend_mir_verify_constant *constants;
	zend_mir_verify_frame *frames;
	zend_mir_verify_source *sources;
	zend_mir_verify_source_map *source_maps;
	zend_mir_frame_slot_ref *slots;
	uint32_t *roots;
	zend_mir_cleanup_ref *cleanups;
	zend_mir_value_id *operands;
	zend_mir_block_id *successors;
	zend_mir_block_id *predecessors;
	uint32_t operand_count;
	uint32_t successor_count;
	uint32_t predecessor_count;
} zend_mir_verify_context;

void *zend_mir_verify_allocate(
	zend_mir_verify_context *context, uint32_t count, size_t element_size);
void zend_mir_verify_release(zend_mir_verify_context *context);

zend_mir_diagnostic_location zend_mir_verify_location(void);
zend_mir_diagnostic_location zend_mir_verify_function_location(
	const zend_mir_verify_context *context, zend_mir_function_id function_id);
zend_mir_diagnostic_location zend_mir_verify_block_location(
	const zend_mir_verify_context *context, zend_mir_block_id block_id);
zend_mir_diagnostic_location zend_mir_verify_instruction_location(
	const zend_mir_verify_context *context, const zend_mir_instruction_record *instruction);
zend_mir_diagnostic_location zend_mir_verify_frame_location(
	const zend_mir_verify_context *context, const zend_mir_frame_state_ref *frame);

void zend_mir_verify_emit(zend_mir_verify_context *context,
	zend_mir_verify_code verify_code, zend_mir_diagnostic_code generic_code,
	zend_mir_diagnostic_location location, zend_mir_value_id operand_id,
	const char *message);
void zend_mir_verify_emit_fatal(zend_mir_verify_context *context,
	zend_mir_verify_code verify_code, zend_mir_diagnostic_code generic_code,
	const char *message);

const zend_mir_verify_function *zend_mir_verify_find_function(
	const zend_mir_verify_context *context, zend_mir_function_id id);
const zend_mir_verify_block *zend_mir_verify_find_block(
	const zend_mir_verify_context *context, zend_mir_block_id id);
const zend_mir_verify_instruction *zend_mir_verify_find_instruction(
	const zend_mir_verify_context *context, zend_mir_instruction_id id);
const zend_mir_verify_value *zend_mir_verify_find_value(
	const zend_mir_verify_context *context, zend_mir_value_id id);
const zend_mir_verify_frame *zend_mir_verify_find_frame(
	const zend_mir_verify_context *context, zend_mir_frame_state_id id);
const zend_mir_verify_source *zend_mir_verify_find_source(
	const zend_mir_verify_context *context, zend_mir_source_position_id id);

bool zend_mir_verify_span_is_valid(zend_mir_span span, uint32_t count);
bool zend_mir_verify_mask_has_unknown(uint64_t mask, uint32_t count);

void zend_mir_verify_ids(zend_mir_verify_context *context);
void zend_mir_verify_cfg(zend_mir_verify_context *context);
void zend_mir_verify_dominance(zend_mir_verify_context *context);
void zend_mir_verify_semantics(zend_mir_verify_context *context);
void zend_mir_verify_frames(zend_mir_verify_context *context);

#ifdef ZEND_MIR_VERIFY_TESTING
void zend_mir_verify_test_fail_allocation_after(uint32_t successful_allocations);
#endif

#endif /* ZEND_MIR_VERIFY_INTERNAL_H */
