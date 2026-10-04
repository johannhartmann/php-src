#ifndef ZEND_MIR_CALL_H
#define ZEND_MIR_CALL_H

#include <stdbool.h>
#include <stdint.h>

#include "../Calls/Contracts/zend_mir_call_source.h"
#include "zend_mir.h"

typedef enum _zend_mir_call_target_kind {
	ZEND_MIR_CALL_TARGET_DIRECT_USER = 0,
	ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL = 1,
	ZEND_MIR_CALL_TARGET_METHOD_USER = 2,
	ZEND_MIR_CALL_TARGET_DYNAMIC = 3,
	ZEND_MIR_CALL_TARGET_KIND_INVALID = -1
} zend_mir_call_target_kind;

typedef enum _zend_mir_call_argument_ownership {
	ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR = 0,
	ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE = 1,
	ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE = 2,
	ZEND_MIR_CALL_ARGUMENT_OWNERSHIP_INVALID = -1
} zend_mir_call_argument_ownership;

typedef enum _zend_mir_call_continuation_kind {
	ZEND_MIR_CALL_CONTINUATION_NORMAL = 0,
	ZEND_MIR_CALL_CONTINUATION_EXCEPTION = 1,
	ZEND_MIR_CALL_CONTINUATION_BAILOUT_REENTRY = 2,
	ZEND_MIR_CALL_CONTINUATION_OBSERVER = 3,
	ZEND_MIR_CALL_CONTINUATION_KIND_INVALID = -1
} zend_mir_call_continuation_kind;

typedef struct _zend_mir_call_target_ref {
	zend_mir_call_target_id id;
	zend_mir_call_target_kind kind;
	zend_mir_symbol_id function_symbol_id;
	zend_mir_op_array_id op_array_id;
	uint32_t num_args;
	uint32_t required_num_args;
	uint32_t function_flags_snapshot;
} zend_mir_call_target_ref;

typedef struct _zend_mir_call_argument_ref {
	zend_mir_call_argument_id id;
	zend_mir_call_site_id call_site_id;
	uint32_t ordinal;
	zend_mir_value_id value_id;
	zend_mir_call_argument_ownership ownership;
	uint32_t send_opline_index;
	zend_mir_source_call_argument_mode source_mode;
	zend_mir_source_operand_ref source_operand;
} zend_mir_call_argument_ref;

/*
 * function_id identifies a function with a lowered MIR body. It is valid for
 * the caller and invalid for an unlowered callee declaration.
 * function_symbol_id and op_array_id always identify the logical source
 * function; together they are the stable callee identity.
 */
typedef struct _zend_mir_call_frame_descriptor {
	zend_mir_frame_state_id frame_state_id;
	zend_mir_function_id function_id;
	zend_mir_symbol_id function_symbol_id;
	zend_mir_op_array_id op_array_id;
	zend_mir_span slots;
	uint32_t pending_call_slot_id;
} zend_mir_call_frame_descriptor;

typedef struct _zend_mir_call_continuation_ref {
	zend_mir_call_continuation_id id;
	zend_mir_call_site_id call_site_id;
	zend_mir_call_continuation_kind kind;
	zend_mir_block_id block_id;
	uint32_t source_opline_index;
} zend_mir_call_continuation_ref;

/*
 * result_id is invalid iff the source result is unused. Otherwise it is the
 * exact non-refcounted scalar MIR value mapped from the source result SSA.
 */
typedef struct _zend_mir_call_site_ref {
	zend_mir_call_site_id id;
	zend_mir_source_call_site_id source_call_site_id;
	zend_mir_instruction_id instruction_id;
	zend_mir_call_target_id target_id;
	zend_mir_span arguments;
	zend_mir_value_id result_id;
	zend_mir_source_operand_ref result_operand;
	zend_mir_call_frame_descriptor caller_frame;
	zend_mir_call_frame_descriptor callee_entry_frame;
	zend_mir_span continuations;
	zend_mir_effect_mask effects;
	zend_mir_memory_domain_mask reads;
	zend_mir_memory_domain_mask writes;
	zend_mir_barrier_mask barriers;
	uint32_t source_init_opline_index;
	uint32_t source_do_opline_index;
} zend_mir_call_site_ref;

typedef struct _zend_mir_call_view {
	uint32_t contract_version;
	const void *context;
	uint32_t (*call_site_count)(const void *context);
	bool (*call_site_at)(const void *context, uint32_t index,
		zend_mir_call_site_ref *out);
	uint32_t (*call_argument_count)(const void *context);
	bool (*call_argument_at)(const void *context, uint32_t index,
		zend_mir_call_argument_ref *out);
	uint32_t (*call_target_count)(const void *context);
	bool (*call_target_at)(const void *context, uint32_t index,
		zend_mir_call_target_ref *out);
	uint32_t (*call_continuation_count)(const void *context);
	bool (*call_continuation_at)(const void *context, uint32_t index,
		zend_mir_call_continuation_ref *out);
} zend_mir_call_view;

typedef struct _zend_mir_call_mutator {
	uint32_t contract_version;
	void *context;
	bool (*add_call_target)(void *context, const zend_mir_call_target_ref *target);
	bool (*add_call_argument)(void *context, const zend_mir_call_argument_ref *argument);
	bool (*add_call_continuation)(void *context,
		const zend_mir_call_continuation_ref *continuation);
	bool (*add_call_site)(void *context, const zend_mir_call_site_ref *site);
	bool (*commit_call_model)(void *context);
} zend_mir_call_mutator;

typedef enum _zend_mir_verify_call_code {
	ZEND_MIR_VERIFY_CALL_OK = 0,
	ZEND_MIR_VERIFY_SITE_MISMATCH = 700,
	ZEND_MIR_VERIFY_TARGET_MISMATCH = 701,
	ZEND_MIR_VERIFY_ARGUMENT_MISMATCH = 702,
	ZEND_MIR_VERIFY_FRAME_MISMATCH = 703,
	ZEND_MIR_VERIFY_CONTINUATION_MISMATCH = 704,
	ZEND_MIR_VERIFY_CALL_CODE_INVALID = -1
} zend_mir_verify_call_code;

#define ZEND_MIRV_TOKEN_SITE_MISMATCH "[MIRV0700]"
#define ZEND_MIRV_TOKEN_TARGET_MISMATCH "[MIRV0701]"
#define ZEND_MIRV_TOKEN_ARGUMENT_MISMATCH "[MIRV0702]"
#define ZEND_MIRV_TOKEN_FRAME_MISMATCH "[MIRV0703]"
#define ZEND_MIRV_TOKEN_CONTINUATION_MISMATCH "[MIRV0704]"

#endif /* ZEND_MIR_CALL_H */
