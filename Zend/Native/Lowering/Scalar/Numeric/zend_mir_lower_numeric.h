/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_LOWER_NUMERIC_H
#define ZEND_MIR_LOWER_NUMERIC_H

#include <stdbool.h>
#include <stdint.h>

#include "../../zend_mir_lowering.h"

#define ZEND_MIR_NUMERIC_FAMILY_ID UINT32_C(3)
#define ZEND_MIR_NUMERIC_ARITHMETIC_PROVIDER_ID UINT32_C(3)
#define ZEND_MIR_NUMERIC_INTEGER_PROVIDER_ID UINT32_C(103)
#define ZEND_MIR_NUMERIC_BITWISE_PROVIDER_ID UINT32_C(203)
#define ZEND_MIR_NUMERIC_PROVIDER_COUNT UINT32_C(3)

#define ZEND_MIR_NUMERIC_OPCODE_ADD UINT32_C(1)
#define ZEND_MIR_NUMERIC_OPCODE_SUB UINT32_C(2)
#define ZEND_MIR_NUMERIC_OPCODE_MUL UINT32_C(3)
#define ZEND_MIR_NUMERIC_OPCODE_DIV UINT32_C(4)
#define ZEND_MIR_NUMERIC_OPCODE_MOD UINT32_C(5)
#define ZEND_MIR_NUMERIC_OPCODE_SL UINT32_C(6)
#define ZEND_MIR_NUMERIC_OPCODE_SR UINT32_C(7)
#define ZEND_MIR_NUMERIC_OPCODE_CONCAT UINT32_C(8)
#define ZEND_MIR_NUMERIC_OPCODE_BW_OR UINT32_C(9)
#define ZEND_MIR_NUMERIC_OPCODE_BW_AND UINT32_C(10)
#define ZEND_MIR_NUMERIC_OPCODE_BW_XOR UINT32_C(11)
#define ZEND_MIR_NUMERIC_OPCODE_POW UINT32_C(12)
#define ZEND_MIR_NUMERIC_OPCODE_BW_NOT UINT32_C(13)

typedef uint32_t zend_mir_numeric_proof_mask;

enum {
	ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK = UINT32_C(1) << 0,
	ZEND_MIR_NUMERIC_PROOF_NO_CALLS = UINT32_C(1) << 1,
	ZEND_MIR_NUMERIC_PROOF_NO_REENTRY = UINT32_C(1) << 2,
	ZEND_MIR_NUMERIC_PROOF_NO_DESTRUCTOR = UINT32_C(1) << 3,
	ZEND_MIR_NUMERIC_PROOF_NO_EXCEPTION = UINT32_C(1) << 4,
	ZEND_MIR_NUMERIC_PROOF_SOURCE_CFG = UINT32_C(1) << 5
};

typedef uint32_t zend_mir_numeric_hazard_mask;

enum {
	ZEND_MIR_NUMERIC_HAZARD_REFERENCE = UINT32_C(1) << 0,
	ZEND_MIR_NUMERIC_HAZARD_COW = UINT32_C(1) << 1,
	ZEND_MIR_NUMERIC_HAZARD_STRING = UINT32_C(1) << 2,
	ZEND_MIR_NUMERIC_HAZARD_OBJECT = UINT32_C(1) << 3,
	ZEND_MIR_NUMERIC_HAZARD_ARRAY = UINT32_C(1) << 4,
	ZEND_MIR_NUMERIC_HAZARD_HELPER = UINT32_C(1) << 5,
	ZEND_MIR_NUMERIC_HAZARD_CALL = UINT32_C(1) << 6,
	ZEND_MIR_NUMERIC_HAZARD_REENTRY = UINT32_C(1) << 7,
	ZEND_MIR_NUMERIC_HAZARD_DESTRUCTOR = UINT32_C(1) << 8,
	ZEND_MIR_NUMERIC_HAZARD_EXCEPTION = UINT32_C(1) << 9
};

typedef struct _zend_mir_numeric_range {
	int64_t minimum;
	int64_t maximum;
} zend_mir_numeric_range;

typedef bool (*zend_mir_numeric_resolve_operand_fn)(
	const void *context, const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out);
typedef bool (*zend_mir_numeric_value_fact_fn)(
	const void *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *fact_out);
typedef bool (*zend_mir_numeric_source_position_fn)(
	const void *context, zend_mir_source_position_id requested_id,
	zend_mir_source_position_ref *position_out);

typedef struct _zend_mir_numeric_provider_context {
	const zend_mir_lowering_source_view *source;
	const void *source_context;
	zend_mir_numeric_resolve_operand_fn resolve_operand;
	zend_mir_numeric_value_fact_fn value_fact;
	zend_mir_numeric_source_position_fn source_position;
	zend_mir_numeric_proof_mask proofs;
	zend_mir_numeric_hazard_mask hazards;
	bool values_predeclared;
} zend_mir_numeric_provider_context;

typedef enum _zend_mir_numeric_provider_group {
	ZEND_MIR_NUMERIC_PROVIDER_GROUP_ARITHMETIC = 0,
	ZEND_MIR_NUMERIC_PROVIDER_GROUP_INTEGER = 1,
	ZEND_MIR_NUMERIC_PROVIDER_GROUP_BITWISE = 2,
	ZEND_MIR_NUMERIC_PROVIDER_GROUP_INVALID = -1
} zend_mir_numeric_provider_group;

typedef struct _zend_mir_numeric_provider_binding {
	zend_mir_numeric_provider_context *numeric;
	zend_mir_numeric_provider_group group;
} zend_mir_numeric_provider_binding;

typedef struct _zend_mir_numeric_provider_set {
	zend_mir_numeric_provider_binding bindings[ZEND_MIR_NUMERIC_PROVIDER_COUNT];
	zend_mir_lowering_provider providers[ZEND_MIR_NUMERIC_PROVIDER_COUNT];
} zend_mir_numeric_provider_set;

#ifdef __cplusplus
extern "C" {
#endif

/* Process-local accessors supplied by the W03 lowering core. */
const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context);
zend_mir_block_id zend_mir_lowering_context_block_id(
	const zend_mir_lowering_context *context);
bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context, zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic);
bool zend_mir_numeric_range_add(
	zend_mir_numeric_range left, zend_mir_numeric_range right,
	zend_mir_numeric_range *result);
bool zend_mir_numeric_range_subtract(
	zend_mir_numeric_range left, zend_mir_numeric_range right,
	zend_mir_numeric_range *result);
bool zend_mir_numeric_range_multiply(
	zend_mir_numeric_range left, zend_mir_numeric_range right,
	zend_mir_numeric_range *result);
bool zend_mir_numeric_range_modulo(
	zend_mir_numeric_range dividend, zend_mir_numeric_range divisor,
	zend_mir_numeric_range *result);
bool zend_mir_numeric_modulo_is_safe(
	zend_mir_numeric_range dividend, zend_mir_numeric_range divisor);
bool zend_mir_numeric_shift_left(
	zend_mir_numeric_range value, zend_mir_numeric_range count,
	zend_mir_numeric_range *result);
bool zend_mir_numeric_shift_right(
	zend_mir_numeric_range value, zend_mir_numeric_range count,
	zend_mir_numeric_range *result);

zend_mir_lowering_status zend_mir_lower_numeric(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_numeric_provider_context *provider_context,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_numeric_provider_set_init(
	zend_mir_numeric_provider_context *provider_context,
	zend_mir_numeric_provider_set *provider_set);
uint32_t zend_mir_numeric_provider_count(
	const zend_mir_numeric_provider_set *provider_set);
bool zend_mir_numeric_provider_at(
	const zend_mir_numeric_provider_set *provider_set, uint32_t index,
	zend_mir_lowering_provider *provider_out);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_LOWER_NUMERIC_H */
