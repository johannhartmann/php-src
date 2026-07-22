#ifndef ZEND_MIR_CALL_SOURCE_H
#define ZEND_MIR_CALL_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "../../Lowering/zend_mir_lowering_source.h"
#include "../../MIR/zend_mir_frame_state.h"

typedef enum _zend_mir_source_call_target_kind {
	ZEND_MIR_SOURCE_CALL_TARGET_DIRECT_USER = 0,
	ZEND_MIR_SOURCE_CALL_TARGET_DYNAMIC_USER = 1,
	ZEND_MIR_SOURCE_CALL_TARGET_METHOD = 2,
	ZEND_MIR_SOURCE_CALL_TARGET_INTERNAL = 3,
	ZEND_MIR_SOURCE_CALL_TARGET_KIND_INVALID = -1
} zend_mir_source_call_target_kind;

typedef enum _zend_mir_source_call_argument_mode {
	ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE = 0,
	ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE = 1,
	ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED = 2,
	ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK = 3,
	ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER = 4,
	ZEND_MIR_SOURCE_CALL_ARGUMENT_MODE_INVALID = -1
} zend_mir_source_call_argument_mode;

enum {
	ZEND_MIR_SOURCE_CALL_SITE_RESULT_UNUSED = UINT32_C(1) << 0,
	ZEND_MIR_SOURCE_CALL_SITE_RESULT_SCALAR = UINT32_C(1) << 1,
	ZEND_MIR_SOURCE_CALL_SITE_PROTECTED = UINT32_C(1) << 2,
	ZEND_MIR_SOURCE_CALL_SITE_NESTED = UINT32_C(1) << 3
};

typedef enum _zend_mir_source_parameter_mode {
	ZEND_MIR_SOURCE_PARAMETER_BY_VALUE = 0,
	ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE = 1,
	ZEND_MIR_SOURCE_PARAMETER_MODE_INVALID = -1
} zend_mir_source_parameter_mode;

typedef struct _zend_mir_source_parameter_mode_ref {
	zend_mir_source_call_target_id target_id;
	uint32_t ordinal;
	zend_mir_source_parameter_mode mode;
} zend_mir_source_parameter_mode_ref;

typedef struct _zend_mir_source_call_site_ref {
	zend_mir_source_call_site_id id;
	zend_mir_source_call_site_id parent_call_site_id;
	uint32_t init_opline_index;
	uint32_t do_opline_index;
	zend_mir_source_block_id source_block_id;
	zend_mir_source_call_target_id target_id;
	zend_mir_span argument_span;
	uint32_t result_ssa_variable_id;
	uint32_t flags;
} zend_mir_source_call_site_ref;

typedef struct _zend_mir_source_call_target_ref {
	zend_mir_source_call_target_id id;
	zend_mir_source_call_target_kind kind;
	zend_mir_symbol_id function_symbol_id;
	zend_mir_op_array_id op_array_id;
	uint32_t num_args;
	uint32_t required_num_args;
	uint32_t function_flags_snapshot;
	zend_mir_span parameter_modes;
	bool variadic;
	bool returns_by_reference;
} zend_mir_source_call_target_ref;

typedef struct _zend_mir_source_call_argument_ref {
	zend_mir_source_call_argument_id id;
	zend_mir_source_call_site_id call_site_id;
	uint32_t send_opline_index;
	uint32_t ordinal;
	zend_mir_symbol_id name_symbol_id;
	zend_mir_source_call_argument_mode mode;
	uint32_t flags;
	uint32_t value_ssa_variable_id;
	zend_mir_source_operand_ref source_operand;
} zend_mir_source_call_argument_ref;

/*
 * Table order is semantic. Call sites are ordered by INIT opline, arguments by
 * (call_site_id, ordinal), and targets by stable source ID. Parent IDs encode
 * nesting. A named call that the compiler normalized into a complete ordered
 * argument list is indistinguishable from its positional form and needs no
 * syntax-history side channel. No callback may expose a process address as
 * identity.
 */
typedef struct _zend_mir_source_call_view {
	uint32_t contract_version;
	const void *context;
	uint32_t (*call_site_count)(const void *context);
	bool (*call_site_at)(const void *context, uint32_t index,
		zend_mir_source_call_site_ref *out);
	uint32_t (*call_target_count)(const void *context);
	bool (*call_target_at)(const void *context, uint32_t index,
		zend_mir_source_call_target_ref *out);
	uint32_t (*call_argument_count)(const void *context);
	bool (*call_argument_at)(const void *context, uint32_t index,
		zend_mir_source_call_argument_ref *out);
	uint32_t (*parameter_mode_count)(const void *context);
	bool (*parameter_mode_at)(const void *context, uint32_t index,
		zend_mir_source_parameter_mode_ref *out);
	uint32_t (*source_opcode_count)(const void *context);
	bool (*source_opcode_at)(const void *context, uint32_t index,
		zend_mir_source_opcode_ref *out);
} zend_mir_source_call_view;

/*
 * Resolver storage is process-local. Implementations may inspect
 * zend_function internally, but return only the pointer-free target snapshot.
 */
typedef struct _zend_mir_source_call_target_resolver {
	const void *context;
	bool (*resolve_exact_direct_user)(
		const void *context,
		zend_mir_source_call_target_id target_id,
		zend_mir_source_call_target_ref *out);
	bool (*resolve_exact_internal)(
		const void *context,
		zend_mir_source_call_target_id target_id,
		zend_mir_source_call_target_ref *out);
} zend_mir_source_call_target_resolver;

#endif /* ZEND_MIR_CALL_SOURCE_H */
