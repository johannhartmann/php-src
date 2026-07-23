#ifndef ZEND_MIR_LOWERING_CONTROL_FLOW_H
#define ZEND_MIR_LOWERING_CONTROL_FLOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zend_mir_lowering_source.h"

typedef enum _zend_mir_w04_branch_kind {
	ZEND_MIR_W04_BRANCH_UNCONDITIONAL = 0,
	ZEND_MIR_W04_BRANCH_IF_FALSE = 1,
	ZEND_MIR_W04_BRANCH_IF_TRUE = 2,
	ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT = 3,
	ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT = 4,
	ZEND_MIR_W04_BRANCH_CATCH = 5,
	ZEND_MIR_W08_BRANCH_FINALLY_CALL = 6,
	ZEND_MIR_W08_BRANCH_FINALLY_RETURN = 7,
	ZEND_MIR_W09_BRANCH_ITERATOR = 8,
	ZEND_MIR_W09_BRANCH_COALESCE = 9,
	ZEND_MIR_W09_BRANCH_JMP_SET = 10,
	ZEND_MIR_W10_BRANCH_JMP_NULL = 11,
	ZEND_MIR_W10_BRANCH_THROW = 12,
	ZEND_MIR_W04_BRANCH_KIND_INVALID = -1
} zend_mir_w04_branch_kind;

typedef enum _zend_mir_w04_proof {
	ZEND_MIR_W04_PROOF_SOURCE_CFG_COMPLETE = UINT32_C(1) << 0,
	ZEND_MIR_W04_PROOF_REDUCIBLE_CFG = UINT32_C(1) << 1,
	ZEND_MIR_W04_PROOF_NO_PROTECTED_REGION = UINT32_C(1) << 2,
	ZEND_MIR_W04_PROOF_BRANCH_SUCCESSOR_ORDER = UINT32_C(1) << 3,
	ZEND_MIR_W04_PROOF_PHI_PREDECESSOR_ORDER = UINT32_C(1) << 4,
	ZEND_MIR_W04_PROOF_EDGE_STATEPOINTS = UINT32_C(1) << 5,
	ZEND_MIR_W04_PROOF_SCALAR_CONDITION = UINT32_C(1) << 6,
	ZEND_MIR_W04_PROOF_RESULT_SSA = UINT32_C(1) << 7
} zend_mir_w04_proof;

/*
 * ZNMIR COND_BRANCH successor 0 is true and successor 1 is false.
 * Zend JMPZ source successor 0 is the false jump target and successor 1 is
 * true fallthrough. Zend JMPNZ uses true jump target 0 and false fallthrough 1.
 * A non-final Zend CATCH uses source successor 0 for type mismatch and source
 * successor 1 for the matching catch body; ZNMIR keeps match at successor 0.
 */
static inline uint32_t zend_mir_w04_mir_successor_for_source(
	zend_mir_w04_branch_kind kind, uint32_t source_successor_index)
{
	if (source_successor_index > 1) {
		return UINT32_MAX;
	}
	switch (kind) {
		case ZEND_MIR_W04_BRANCH_IF_FALSE:
		case ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT:
		case ZEND_MIR_W04_BRANCH_CATCH:
		case ZEND_MIR_W09_BRANCH_ITERATOR:
			return source_successor_index == 0 ? 1 : 0;
		case ZEND_MIR_W08_BRANCH_FINALLY_CALL:
			return source_successor_index;
		case ZEND_MIR_W04_BRANCH_IF_TRUE:
		case ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT:
		case ZEND_MIR_W09_BRANCH_COALESCE:
		case ZEND_MIR_W09_BRANCH_JMP_SET:
		case ZEND_MIR_W10_BRANCH_JMP_NULL:
			return source_successor_index;
		default:
			return UINT32_MAX;
	}
}

static inline bool zend_mir_w04_edge_requires_statepoint(
	const zend_mir_source_edge_ref *edge)
{
	return edge != NULL
		&& (edge->flags & ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY) != 0;
}

#endif /* ZEND_MIR_LOWERING_CONTROL_FLOW_H */
