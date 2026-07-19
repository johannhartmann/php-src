#ifndef ZEND_MIR_LOWERING_H
#define ZEND_MIR_LOWERING_H

#include <stdbool.h>
#include <stdint.h>

#include "../MIR/zend_mir.h"
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

zend_mir_lowering_result zend_mir_lower_source(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator);

#endif /* ZEND_MIR_LOWERING_H */
