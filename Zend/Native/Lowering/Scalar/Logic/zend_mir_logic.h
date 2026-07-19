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

#ifndef ZEND_MIR_LOGIC_H
#define ZEND_MIR_LOGIC_H

#include "../../zend_mir_lowering.h"

#define ZEND_MIR_LOGIC_PROVIDER_ID UINT32_C(4)
#define ZEND_MIR_LOGIC_SEMANTIC_FAMILY_ID UINT32_C(4)

/* Stable source-opcode numbers from the frozen W01 matrix. */
enum {
	ZEND_MIR_LOGIC_ZEND_BOOL_NOT = 14,
	ZEND_MIR_LOGIC_ZEND_BOOL_XOR = 15,
	ZEND_MIR_LOGIC_ZEND_IS_IDENTICAL = 16,
	ZEND_MIR_LOGIC_ZEND_IS_NOT_IDENTICAL = 17,
	ZEND_MIR_LOGIC_ZEND_IS_EQUAL = 18,
	ZEND_MIR_LOGIC_ZEND_IS_NOT_EQUAL = 19,
	ZEND_MIR_LOGIC_ZEND_IS_SMALLER = 20,
	ZEND_MIR_LOGIC_ZEND_IS_SMALLER_OR_EQUAL = 21,
	ZEND_MIR_LOGIC_ZEND_CAST = 51,
	ZEND_MIR_LOGIC_ZEND_BOOL = 52,
	ZEND_MIR_LOGIC_ZEND_SPACESHIP = 170
};

/* ZEND_CAST target values carried by zend_mir_source_opcode_ref.extended_value. */
enum {
	ZEND_MIR_LOGIC_CAST_LONG = 4,
	ZEND_MIR_LOGIC_CAST_DOUBLE = 5
};

typedef uint32_t zend_mir_logic_proof_mask;

enum {
	ZEND_MIR_LOGIC_PROOF_SINGLE_REACHABLE_BLOCK = UINT32_C(1) << 0,
	ZEND_MIR_LOGIC_PROOF_NO_CALLS = UINT32_C(1) << 1,
	ZEND_MIR_LOGIC_PROOF_NO_REENTRY = UINT32_C(1) << 2,
	ZEND_MIR_LOGIC_PROOF_SAME_EXACT_TYPE = UINT32_C(1) << 3,
	ZEND_MIR_LOGIC_PROOF_FINITE_F64 = UINT32_C(1) << 4,
	ZEND_MIR_LOGIC_PROOF_SAFE_SCALAR_CAST = UINT32_C(1) << 5,
	ZEND_MIR_LOGIC_PROOF_NO_DESTRUCTOR = UINT32_C(1) << 6,
	ZEND_MIR_LOGIC_PROOF_NO_EXCEPTION = UINT32_C(1) << 7,
	ZEND_MIR_LOGIC_PROOF_SOURCE_CFG = UINT32_C(1) << 8,
	ZEND_MIR_LOGIC_PROOF_ALL = (UINT32_C(1) << 8) - UINT32_C(1)
};

typedef struct _zend_mir_logic_value_binding {
	zend_mir_source_operand_ref source;
	zend_mir_value_id value_id;
	bool has_fact;
	zend_mir_value_fact_ref fact;
} zend_mir_logic_value_binding;

/*
 * temporary_value_id is required only by not-identical and not-equal. It must
 * be a caller-reserved synthetic value ID so parallel providers never invent
 * colliding persistent identity.
 */
typedef struct _zend_mir_logic_opcode_proof {
	uint32_t opline_index;
	zend_mir_logic_proof_mask proofs;
	zend_mir_value_id temporary_value_id;
} zend_mir_logic_opcode_proof;

/*
 * This is process-local provider configuration. Its pointers and array
 * addresses never enter source records, MIR records, facts, or text dumps.
 */
typedef struct _zend_mir_logic_context {
	const zend_mir_logic_value_binding *bindings;
	uint32_t binding_count;
	const zend_mir_logic_opcode_proof *opcode_proofs;
	uint32_t opcode_proof_count;
	bool values_predeclared;
} zend_mir_logic_context;

void zend_mir_logic_provider_init(
	zend_mir_lowering_provider *provider,
	const zend_mir_logic_context *logic_context);

#endif /* ZEND_MIR_LOGIC_H */
