/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include "zend_mir_straight_line.h"

zend_mir_lowering_status zend_mir_lower_structural(
	zend_mir_straight_line_provider_context *provider_context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_lowering_diagnostic_code *diagnostic_out)
{
	const zend_mir_straight_line_proof_mask required =
		ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY;
	zend_mir_straight_line_hazard_mask observable =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_OBSERVER
		| ZEND_MIR_STRAIGHT_LINE_HAZARD_INTERRUPT;

	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_INVALID_SOURCE;
	}
	if (provider_context == NULL || source_opcode == NULL
			|| source_opcode->zend_opcode_number
				!= ZEND_MIR_STRAIGHT_LINE_OPCODE_NOP) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if ((provider_context->proofs & required) != required
			|| !zend_mir_straight_line_has_cfg_proof(
				provider_context->proofs)) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_MISSING_PROOF;
		}
		return ZEND_MIR_LOWERING_REJECTED;
	}
	if ((provider_context->hazards & observable) != 0) {
		if (diagnostic_out != NULL) {
			*diagnostic_out = ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
		}
		return ZEND_MIR_LOWERING_DEFERRED;
	}
	if (diagnostic_out != NULL) {
		*diagnostic_out = ZEND_MIRL_OK;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}
