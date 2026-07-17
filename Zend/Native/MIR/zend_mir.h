/* Canonical, target-neutral ZNMIR core contract. */

#ifndef ZEND_MIR_H
#define ZEND_MIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zend_mir_diagnostic.h"
#include "zend_mir_effects.h"
#include "zend_mir_frame_state.h"
#include "zend_mir_ids.h"
#include "zend_mir_opcodes.h"

typedef struct _zend_mir_module zend_mir_module;
typedef struct _zend_mir_function zend_mir_function;
typedef struct _zend_mir_block zend_mir_block;
typedef struct _zend_mir_instruction zend_mir_instruction;
typedef struct _zend_mir_value zend_mir_value;
typedef struct _zend_mir_frame_state zend_mir_frame_state;

typedef void *(*zend_mir_allocate_fn)(void *context, size_t size, size_t alignment);
typedef void (*zend_mir_reset_fn)(void *context);

typedef struct _zend_mir_allocator {
	void *context;
	zend_mir_allocate_fn allocate;
	zend_mir_reset_fn reset;
} zend_mir_allocator;

/* Record structs are immutable snapshots and contain no process pointers. */
typedef struct _zend_mir_function_record {
	zend_mir_function_id id;
	zend_mir_symbol_id symbol_id;
	zend_mir_block_id entry_block_id;
	uint32_t flags;
} zend_mir_function_record;

typedef struct _zend_mir_block_record {
	zend_mir_block_id id;
	zend_mir_function_id function_id;
} zend_mir_block_record;

typedef struct _zend_mir_value_record {
	zend_mir_value_id id;
	zend_mir_representation representation;
	zend_mir_ownership_state ownership;
} zend_mir_value_record;

/* Constant payloads are tagged scalars or symbol IDs, never host pointers. */
typedef struct _zend_mir_constant_record {
	zend_mir_value_id value_id;
	zend_mir_representation representation;
	zend_mir_constant_kind kind;
	uint64_t payload_bits;
	zend_mir_symbol_id symbol_id;
} zend_mir_constant_record;

typedef struct _zend_mir_instruction_record {
	zend_mir_instruction_id id;
	zend_mir_block_id block_id;
	zend_mir_opcode opcode;
	zend_mir_representation representation;
	zend_mir_value_id result_id;
	zend_mir_frame_state_id frame_state_id;
	zend_mir_source_position_id source_position_id;
	zend_mir_effect_mask effects;
	zend_mir_memory_domain_mask reads;
	zend_mir_memory_domain_mask writes;
	zend_mir_barrier_mask barriers;
	zend_mir_ownership_action_mask ownership_actions;
} zend_mir_instruction_record;

typedef struct _zend_mir_view {
	uint32_t contract_version;
	const void *context;
	zend_mir_module_id (*module_id)(const void *context);
	uint32_t (*function_count)(const void *context);
	bool (*function_at)(const void *context, uint32_t index, zend_mir_function_record *out);
	uint32_t (*block_count)(const void *context);
	bool (*block_at)(const void *context, uint32_t index, zend_mir_block_record *out);
	uint32_t (*instruction_count)(const void *context);
	bool (*instruction_at)(const void *context, uint32_t index, zend_mir_instruction_record *out);
	uint32_t (*value_count)(const void *context);
	bool (*value_at)(const void *context, uint32_t index, zend_mir_value_record *out);
	uint32_t (*constant_count)(const void *context);
	bool (*constant_at)(const void *context, uint32_t index, zend_mir_constant_record *out);
	uint32_t (*frame_state_count)(const void *context);
	bool (*frame_state_at)(const void *context, uint32_t index, zend_mir_frame_state_ref *out);
	uint32_t (*source_position_count)(const void *context);
	bool (*source_position_at)(const void *context, uint32_t index, zend_mir_source_position_ref *out);
	uint32_t (*frame_slot_count)(const void *context);
	bool (*frame_slot_at)(const void *context, uint32_t index, zend_mir_frame_slot_ref *out);
	uint32_t (*root_count)(const void *context);
	bool (*root_at)(const void *context, uint32_t index, uint32_t *slot_id_out);
	uint32_t (*cleanup_count)(const void *context);
	bool (*cleanup_at)(const void *context, uint32_t index, zend_mir_cleanup_ref *out);
	uint32_t (*instruction_operand_count)(const void *context, zend_mir_instruction_id instruction_id);
	bool (*instruction_operand_at)(const void *context, zend_mir_instruction_id instruction_id,
		uint32_t index, zend_mir_value_id *out);
	uint32_t (*successor_count)(const void *context, zend_mir_block_id block_id);
	bool (*successor_at)(const void *context, zend_mir_block_id block_id, uint32_t index,
		zend_mir_block_id *out);
	uint32_t (*predecessor_count)(const void *context, zend_mir_block_id block_id);
	bool (*predecessor_at)(const void *context, zend_mir_block_id block_id, uint32_t index,
		zend_mir_block_id *out);
} zend_mir_view;

typedef struct _zend_mir_mutator {
	uint32_t contract_version;
	void *context;
	zend_mir_diagnostic_sink *diagnostics;
	bool (*add_function)(void *context, zend_mir_symbol_id symbol_id, zend_mir_function_id *out);
	bool (*add_block)(void *context, zend_mir_function_id function_id, zend_mir_block_id *out);
	bool (*set_entry_block)(void *context, zend_mir_function_id function_id, zend_mir_block_id block_id);
	bool (*add_value)(void *context, zend_mir_value_id requested_id,
		zend_mir_representation representation, zend_mir_ownership_state ownership);
	bool (*add_constant)(void *context, const zend_mir_constant_record *constant);
	bool (*add_instruction)(void *context, const zend_mir_instruction_record *record,
		zend_mir_instruction_id *out);
	bool (*add_operand)(void *context, zend_mir_instruction_id instruction_id, zend_mir_value_id value_id);
	bool (*add_edge)(void *context, zend_mir_block_id from, zend_mir_block_id to);
	bool (*add_source_position)(void *context, const zend_mir_source_position_ref *source_position,
		zend_mir_source_position_id *out);
	bool (*add_frame_slot)(void *context, const zend_mir_frame_slot_ref *slot, uint32_t *index_out);
	bool (*add_root)(void *context, uint32_t slot_id, uint32_t *index_out);
	bool (*add_cleanup)(void *context, const zend_mir_cleanup_ref *cleanup, uint32_t *index_out);
	bool (*add_frame_state)(void *context, const zend_mir_frame_state_ref *frame_state,
		zend_mir_frame_state_id *out);
	bool (*seal_function)(void *context, zend_mir_function_id function_id);
} zend_mir_mutator;

/*
 * View order is semantic: PHI operand N belongs to predecessor N. BRANCH has
 * successor 0; COND_BRANCH has true successor 0 and false successor 1.
 * RETURN, THROW, and UNREACHABLE have no successors.
 */

typedef bool (*zend_mir_text_write_fn)(void *context, const char *bytes, size_t length);

/* Process-local output callback; no stream pointer becomes persistent identity. */
typedef struct _zend_mir_text_writer {
	void *context;
	zend_mir_text_write_fn write;
} zend_mir_text_writer;

/* W02-E implements the strict canonical format frozen by ADR 0012. */
bool zend_mir_dump_text(const zend_mir_view *view, zend_mir_text_writer *writer,
	zend_mir_diagnostic_sink *diagnostics);
bool zend_mir_parse_text(const char *text, size_t length, zend_mir_mutator *mutator,
	zend_mir_diagnostic_sink *diagnostics);

/* W02-F rejects malformed input before any target lowering. */
bool zend_mir_verify_stage1(const zend_mir_view *view, zend_mir_diagnostic_sink *diagnostics);

static inline bool zend_mir_contract_is_compatible(uint32_t version)
{
	return (version >> 16) == ZEND_MIR_CONTRACT_VERSION_MAJOR
		&& (version & UINT32_C(0xffff)) <= ZEND_MIR_CONTRACT_VERSION_MINOR;
}

#endif /* ZEND_MIR_H */
