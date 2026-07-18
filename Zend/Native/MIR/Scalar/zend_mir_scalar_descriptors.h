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

#ifndef ZEND_MIR_SCALAR_DESCRIPTORS_H
#define ZEND_MIR_SCALAR_DESCRIPTORS_H

#include "Zend/Native/MIR/zend_mir.h"

#define ZEND_MIR_SCALAR_MAX_OPERANDS UINT32_C(2)
#define ZEND_MIR_SCALAR_OPCODE_COUNT \
	((uint32_t) (ZEND_MIR_OPCODE_COUNT - ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW))

typedef uint16_t zend_mir_scalar_proof_mask;

enum {
	ZEND_MIR_SCALAR_PROOF_NONE = 0,
	ZEND_MIR_SCALAR_PROOF_NO_OVERFLOW = UINT16_C(1) << 0,
	ZEND_MIR_SCALAR_PROOF_NONZERO_DIVISOR = UINT16_C(1) << 1,
	ZEND_MIR_SCALAR_PROOF_VALID_SHIFT_COUNT = UINT16_C(1) << 2,
	ZEND_MIR_SCALAR_PROOF_RESULT_RANGE = UINT16_C(1) << 3
};

typedef struct _zend_mir_scalar_value_requirement {
	zend_mir_representation representation;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_value_fact_flags required_flags;
	zend_mir_ownership_state ownership;
} zend_mir_scalar_value_requirement;

typedef struct _zend_mir_scalar_descriptor {
	zend_mir_opcode opcode;
	const char *label;
	uint32_t operand_count;
	zend_mir_scalar_value_requirement operands[ZEND_MIR_SCALAR_MAX_OPERANDS];
	bool has_result;
	zend_mir_scalar_value_requirement result;
	zend_mir_scalar_proof_mask proofs;
	zend_mir_effect_mask effects;
	zend_mir_memory_domain_mask reads;
	zend_mir_memory_domain_mask writes;
	zend_mir_barrier_mask barriers;
	zend_mir_ownership_action_mask ownership_actions;
	bool requires_source;
	bool requires_frame;
} zend_mir_scalar_descriptor;

#ifdef __cplusplus
extern "C" {
#endif

const zend_mir_scalar_descriptor *zend_mir_scalar_descriptor_at(
	zend_mir_opcode opcode);
bool zend_mir_scalar_opcode_is_registered(zend_mir_opcode opcode);
bool zend_mir_scalar_fact_is_well_formed(const zend_mir_value_fact_ref *fact);
zend_mir_representation zend_mir_scalar_type_representation(
	zend_mir_scalar_type_mask type);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_SCALAR_DESCRIPTORS_H */
