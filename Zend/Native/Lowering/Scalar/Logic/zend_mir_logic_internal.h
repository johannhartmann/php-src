/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_LOGIC_INTERNAL_H
#define ZEND_MIR_LOGIC_INTERNAL_H

#include <stddef.h>

#include "zend_mir_logic.h"

#define ZEND_MIR_LOGIC_MAX_PLAN_STEPS UINT32_C(2)
#define ZEND_MIR_LOGIC_MAX_OPERANDS UINT32_C(2)

#define ZEND_MIR_LOGIC_COMMON_PROOFS \
	(ZEND_MIR_LOGIC_PROOF_SINGLE_REACHABLE_BLOCK \
		| ZEND_MIR_LOGIC_PROOF_NO_CALLS \
		| ZEND_MIR_LOGIC_PROOF_NO_REENTRY)

typedef struct _zend_mir_logic_input {
	zend_mir_value_id value_id;
	zend_mir_value_fact_ref fact;
} zend_mir_logic_input;

typedef struct _zend_mir_logic_plan_step {
	zend_mir_opcode opcode;
	zend_mir_representation representation;
	zend_mir_value_id result_id;
	zend_mir_value_fact_ref result_fact;
	zend_mir_value_id operands[ZEND_MIR_LOGIC_MAX_OPERANDS];
	uint32_t operand_count;
} zend_mir_logic_plan_step;

typedef struct _zend_mir_logic_plan {
	zend_mir_block_id block_id;
	zend_mir_source_position_id source_position_id;
	zend_mir_logic_plan_step steps[ZEND_MIR_LOGIC_MAX_PLAN_STEPS];
	uint32_t step_count;
} zend_mir_logic_plan;

/*
 * W03-A owns these context accessors. Declaring the narrow integration seam
 * here keeps this specialist branch independently compilable without copying
 * or exposing the Core context layout.
 */
const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context);
zend_mir_block_id zend_mir_lowering_context_block_id(
	const zend_mir_lowering_context *context);
bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic);

zend_mir_lowering_status zend_mir_logic_fail(
	zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic);

zend_mir_lowering_status zend_mir_logic_prepare(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	const zend_mir_logic_context **logic_context_out,
	const zend_mir_logic_opcode_proof **proof_out,
	zend_mir_value_id *result_id_out,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_logic_input_at(
	const zend_mir_logic_context *context,
	const zend_mir_source_operand_ref *source,
	zend_mir_logic_input *out,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_logic_require_proofs(
	const zend_mir_logic_opcode_proof *proof,
	zend_mir_logic_proof_mask required,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_logic_require_finite(
	const zend_mir_logic_opcode_proof *proof,
	const zend_mir_logic_input *input,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_logic_add_step(
	zend_mir_logic_plan *plan,
	zend_mir_opcode opcode,
	zend_mir_representation representation,
	zend_mir_value_id result_id,
	zend_mir_scalar_type_mask exact_type,
	zend_mir_value_fact_flags fact_flags,
	int64_t integer_min,
	int64_t integer_max,
	const zend_mir_value_id *operands,
	uint32_t operand_count);

zend_mir_lowering_status zend_mir_logic_build_compare_plan(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
zend_mir_lowering_status zend_mir_logic_build_boolean_plan(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
zend_mir_lowering_status zend_mir_logic_build_cast_plan(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_logic_plan *plan,
	zend_mir_lowering_diagnostic_code *diagnostic_out);

#endif /* ZEND_MIR_LOGIC_INTERNAL_H */
