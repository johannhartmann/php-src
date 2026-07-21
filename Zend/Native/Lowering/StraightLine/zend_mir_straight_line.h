/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_STRAIGHT_LINE_H
#define ZEND_MIR_STRAIGHT_LINE_H

#include "../zend_mir_lowering.h"

#define ZEND_MIR_STRAIGHT_LINE_PROVIDER_ID UINT32_C(5)
#define ZEND_MIR_STRAIGHT_LINE_FAMILY_ID UINT32_C(5)

#define ZEND_MIR_STRAIGHT_LINE_OPCODE_NOP UINT32_C(0)
#define ZEND_MIR_STRAIGHT_LINE_OPCODE_ASSIGN UINT32_C(22)
#define ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN UINT32_C(31)
#define ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN UINT32_C(62)
#define ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE UINT32_C(70)
#define ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN_BY_REF UINT32_C(111)

typedef uint32_t zend_mir_straight_line_proof_mask;

enum {
	ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK = UINT32_C(1) << 0,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS = UINT32_C(1) << 1,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY = UINT32_C(1) << 2,
	ZEND_MIR_STRAIGHT_LINE_PROOF_EXACT_SCALAR = UINT32_C(1) << 3,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED = UINT32_C(1) << 4,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NOT_BY_REFERENCE = UINT32_C(1) << 5,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NO_OBSERVER = UINT32_C(1) << 6,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NO_DESTRUCTOR = UINT32_C(1) << 7,
	ZEND_MIR_STRAIGHT_LINE_PROOF_NO_EXCEPTION = UINT32_C(1) << 8,
	ZEND_MIR_STRAIGHT_LINE_PROOF_SOURCE_CFG = UINT32_C(1) << 9
};

static inline bool zend_mir_straight_line_has_cfg_proof(
	zend_mir_straight_line_proof_mask proofs)
{
	return (proofs & (ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK
		| ZEND_MIR_STRAIGHT_LINE_PROOF_SOURCE_CFG)) != 0;
}

typedef uint32_t zend_mir_straight_line_hazard_mask;

enum {
	ZEND_MIR_STRAIGHT_LINE_HAZARD_REFERENCE = UINT32_C(1) << 0,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_RETURN_BY_REFERENCE = UINT32_C(1) << 1,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_PENDING_CALL = UINT32_C(1) << 2,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_CLEANUP = UINT32_C(1) << 3,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_DESTRUCTOR = UINT32_C(1) << 4,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_OBSERVER = UINT32_C(1) << 5,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_INTERRUPT = UINT32_C(1) << 6,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_EXCEPTION = UINT32_C(1) << 7,
	ZEND_MIR_STRAIGHT_LINE_HAZARD_OLD_VALUE = UINT32_C(1) << 8
};

typedef struct _zend_mir_straight_line_value {
	zend_mir_value_id value_id;
	zend_mir_representation representation;
	zend_mir_ownership_state ownership;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_value_fact_flags fact_flags;
	int64_t integer_min;
	int64_t integer_max;
} zend_mir_straight_line_value;

typedef struct _zend_mir_straight_line_lifetime {
	zend_mir_straight_line_value *values;
	uint32_t capacity;
	uint32_t count;
	bool entry_emitted;
	zend_mir_frame_state_id entry_frame_state_id;
} zend_mir_straight_line_lifetime;

typedef struct _zend_mir_straight_line_slot {
	uint32_t slot_id;
	uint32_t index;
	zend_mir_frame_slot_kind kind;
	zend_mir_value_id value_id;
	zend_mir_representation value_representation;
	zend_mir_materialization materialization;
	zend_mir_frame_slot_ownership ownership;
} zend_mir_straight_line_slot;

typedef bool (*zend_mir_straight_line_source_position_fn)(
	const void *context, zend_mir_source_position_id requested_id,
	zend_mir_source_position_ref *out);

typedef bool (*zend_mir_straight_line_resolve_operand_fn)(
	const void *context, const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out);

typedef struct _zend_mir_straight_line_entry {
	zend_mir_function_kind function_kind;
	zend_mir_op_array_id op_array_id;
	uint32_t code_version_id;
	zend_mir_straight_line_slot *slots;
	uint32_t slot_count;
	const void *source_context;
	zend_mir_straight_line_source_position_fn source_position_at;
	zend_mir_straight_line_resolve_operand_fn resolve_operand;
} zend_mir_straight_line_entry;

typedef struct _zend_mir_straight_line_provider_context {
	const zend_mir_lowering_source_view *source;
	zend_mir_straight_line_lifetime *lifetime;
	zend_mir_straight_line_entry *entry;
	zend_mir_straight_line_proof_mask proofs;
	zend_mir_straight_line_hazard_mask hazards;
	bool values_predeclared;
} zend_mir_straight_line_provider_context;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * These process-local accessors are supplied by the lowering core.  Keeping
 * them out of the frozen public ABI lets the W03 integration track reconcile
 * provider-private context layouts without persisting a pointer in ZNMIR.
 */
const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context);
zend_mir_function_id zend_mir_lowering_context_function_id(
	const zend_mir_lowering_context *context);
zend_mir_block_id zend_mir_lowering_context_block_id(
	const zend_mir_lowering_context *context);
bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context, zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic);
bool zend_mir_straight_line_lifetime_init(
	zend_mir_straight_line_lifetime *lifetime,
	zend_mir_straight_line_value *storage, uint32_t capacity);
bool zend_mir_straight_line_track_value(
	zend_mir_straight_line_lifetime *lifetime,
	const zend_mir_straight_line_value *value);
bool zend_mir_straight_line_value_at(
	const zend_mir_straight_line_lifetime *lifetime,
	zend_mir_value_id value_id, zend_mir_straight_line_value *out);

zend_mir_lowering_status zend_mir_lower_structural(
	zend_mir_straight_line_provider_context *provider_context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
zend_mir_lowering_status zend_mir_lower_copy_move(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
zend_mir_lowering_status zend_mir_lower_entry_state(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
zend_mir_lowering_status zend_mir_lower_return(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
zend_mir_lowering_status zend_mir_lower_free(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_lifetime_provider_init(
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_provider *provider_out);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_STRAIGHT_LINE_H */
