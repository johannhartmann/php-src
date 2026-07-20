#ifndef ZEND_MIR_LOWERING_H
#define ZEND_MIR_LOWERING_H

#include <stdbool.h>
#include <stdint.h>

#include "../MIR/zend_mir.h"
#include "../MIR/zend_mir_call.h"
#include "../MIR/zend_mir_values.h"
#include "zend_mir_lowering_diagnostic.h"
#include "zend_mir_lowering_registry.h"
#include "zend_mir_lowering_source.h"

enum {
	ZEND_MIR_LOWERING_GUARANTEE_FINALIZED = UINT32_C(1) << 0,
	ZEND_MIR_LOWERING_GUARANTEE_STAGE1_VERIFIED = UINT32_C(1) << 1,
	ZEND_MIR_LOWERING_GUARANTEE_STAGE2_VERIFIED = UINT32_C(1) << 2,
	ZEND_MIR_LOWERING_GUARANTEE_STAGE3_VERIFIED = UINT32_C(1) << 3,
	ZEND_MIR_LOWERING_GUARANTEE_W03_ALL =
		ZEND_MIR_LOWERING_GUARANTEE_FINALIZED
		| ZEND_MIR_LOWERING_GUARANTEE_STAGE1_VERIFIED
		| ZEND_MIR_LOWERING_GUARANTEE_STAGE2_VERIFIED,
	ZEND_MIR_LOWERING_GUARANTEE_ALL =
		ZEND_MIR_LOWERING_GUARANTEE_W03_ALL,
	ZEND_MIR_LOWERING_GUARANTEE_W04_ALL =
		ZEND_MIR_LOWERING_GUARANTEE_W03_ALL
		| ZEND_MIR_LOWERING_GUARANTEE_STAGE3_VERIFIED
};

typedef struct _zend_mir_lowering_result {
	zend_mir_lowering_status status;
	zend_mir_lowering_diagnostic_code diagnostic_code;
	uint32_t guarantees;
	zend_mir_module *module;
} zend_mir_lowering_result;

/*
 * W05 preserves the W03/W04 result layout and adds named capabilities and
 * debts in a wrapper. prerequisite_guarantees records the verified W04 input
 * projection. The nested lowering.guarantees field describes the final W05
 * module and therefore contains FINALIZED only: CALL_DIRECT_USER is outside
 * the frozen W03 generic opcode boundary and is verified by the named W05
 * verifier. W05 does not invent a generic Stage 4 guarantee.
 */
typedef struct _zend_mir_w05_lowering_result {
	zend_mir_lowering_result lowering;
	uint32_t prerequisite_guarantees;
	uint32_t capabilities;
	uint32_t semantic_debts;
	bool modeled;
	bool codegen_eligible;
} zend_mir_w05_lowering_result;

/*
 * W06 is a named-capability result, not a new generic stage bit. The W05
 * result remains an immutable prerequisite receipt. canonical contains sorted
 * registry transport IDs, verifier_receipts identifies the same final module
 * fingerprint for the required verifier pipeline, and codegen remains false
 * while any semantic debt is open.
 */
typedef struct _zend_mir_w06_lowering_result {
	zend_mir_w05_lowering_result prerequisite;
	zend_mir_capability_set_ref canonical;
	zend_mir_span verifier_receipts;
	bool modeled;
	bool codegen_eligible;
} zend_mir_w06_lowering_result;

static inline bool zend_mir_lowering_result_is_failure_atomic(
	const zend_mir_lowering_result *result)
{
	if (result == NULL) {
		return false;
	}
	if (result->status == ZEND_MIR_LOWERING_SUCCESS) {
		return result->diagnostic_code == ZEND_MIRL_OK
			&& result->guarantees == ZEND_MIR_LOWERING_GUARANTEE_ALL
			&& result->module != NULL;
	}
	return result->status != ZEND_MIR_LOWERING_STATUS_INVALID
		&& result->diagnostic_code != ZEND_MIRL_OK
		&& result->guarantees == 0
		&& result->module == NULL;
}

static inline bool zend_mir_lowering_result_is_w04_failure_atomic(
	const zend_mir_lowering_result *result)
{
	if (result == NULL) {
		return false;
	}
	if (result->status == ZEND_MIR_LOWERING_SUCCESS) {
		return result->diagnostic_code == ZEND_MIRL_OK
			&& result->guarantees == ZEND_MIR_LOWERING_GUARANTEE_W04_ALL
			&& result->module != NULL;
	}
	return result->status != ZEND_MIR_LOWERING_STATUS_INVALID
		&& result->diagnostic_code != ZEND_MIRL_OK
		&& result->guarantees == 0
		&& result->module == NULL;
}

static inline bool zend_mir_lowering_result_is_w05_failure_atomic(
	const zend_mir_w05_lowering_result *result)
{
	if (result == NULL) {
		return false;
	}
	if (result->lowering.status == ZEND_MIR_LOWERING_SUCCESS) {
		return result->lowering.diagnostic_code == ZEND_MIRL_OK
			&& result->lowering.guarantees
				== ZEND_MIR_LOWERING_GUARANTEE_FINALIZED
			&& result->lowering.module != NULL
			&& result->prerequisite_guarantees
				== ZEND_MIR_LOWERING_GUARANTEE_W04_ALL
			&& result->capabilities == ZEND_MIR_W05_REQUIRED_CAPABILITIES
			&& result->semantic_debts == ZEND_MIR_W05_REQUIRED_DEBTS
			&& result->modeled
			&& !result->codegen_eligible;
	}
	return result->lowering.status != ZEND_MIR_LOWERING_STATUS_INVALID
		&& result->lowering.diagnostic_code != ZEND_MIRL_OK
		&& result->lowering.guarantees == 0
		&& result->lowering.module == NULL
		&& result->prerequisite_guarantees == 0
		&& result->capabilities == 0
		&& result->semantic_debts == 0
		&& !result->modeled
		&& !result->codegen_eligible;
}

static inline bool zend_mir_lowering_result_is_w06_failure_atomic(
	const zend_mir_w06_lowering_result *result)
{
	if (result == NULL) {
		return false;
	}
	if (result->prerequisite.lowering.status == ZEND_MIR_LOWERING_SUCCESS) {
		return zend_mir_lowering_result_is_w05_failure_atomic(&result->prerequisite)
			&& result->canonical.capability_ids.count != 0
			&& result->canonical.semantic_debt_ids.count != 0
			&& !result->canonical.codegen_eligible
			&& result->verifier_receipts.count >= 4
			&& result->modeled
			&& !result->codegen_eligible;
	}
	return result->prerequisite.lowering.status
			!= ZEND_MIR_LOWERING_STATUS_INVALID
		&& result->prerequisite.lowering.diagnostic_code != ZEND_MIRL_OK
		&& result->prerequisite.lowering.guarantees == 0
		&& result->prerequisite.lowering.module == NULL
		&& result->prerequisite.prerequisite_guarantees == 0
		&& result->prerequisite.capabilities == 0
		&& result->prerequisite.semantic_debts == 0
		&& !result->prerequisite.modeled
		&& !result->prerequisite.codegen_eligible
		&& result->canonical.capability_ids.count == 0
		&& result->canonical.semantic_debt_ids.count == 0
		&& !result->canonical.codegen_eligible
		&& result->verifier_receipts.count == 0
		&& !result->modeled
		&& !result->codegen_eligible;
}

zend_mir_lowering_result zend_mir_lower_source(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator);

#endif /* ZEND_MIR_LOWERING_H */
