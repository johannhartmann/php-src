#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "Zend/Native/Calls/Contracts/zend_mir_call_plan.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"

int main(void)
{
	zend_mir_w05_lowering_result result;
	zend_mir_call_capability_receipt_ref receipt;
	zend_mir_call_frame_descriptor callee;
	zend_mir_call_site_ref site;
	zend_mir_call_plan plan;
	zend_mir_source_parameter_mode_ref parameter_mode;
	zend_mir_verifier_receipt_ref verifier_receipt;

	memset(&result, 0, sizeof(result));
	memset(&receipt, 0, sizeof(receipt));
	memset(&callee, 0, sizeof(callee));
	memset(&site, 0, sizeof(site));
	memset(&plan, 0, sizeof(plan));
	memset(&parameter_mode, 0, sizeof(parameter_mode));
	memset(&verifier_receipt, 0, sizeof(verifier_receipt));

	assert(ZEND_MIR_CONTRACT_VERSION == UINT32_C(0x00010002));
	assert(ZEND_MIR_W04_CONTRACT_VERSION == UINT32_C(0x00010003));
	assert(ZEND_MIR_W05_CONTRACT_VERSION == UINT32_C(0x00010008));
	assert(ZEND_MIR_OPCODE_CALL_DIRECT_USER == 41);
	assert(ZEND_MIR_OPCODE_COUNT == 41);
	assert(ZEND_MIR_W05_OPCODE_COUNT == 42);

	assert(offsetof(zend_mir_source_call_site_ref, flags)
		> offsetof(zend_mir_source_call_site_ref, init_opline_index));
	assert(offsetof(zend_mir_source_call_argument_ref, source_operand)
		> offsetof(zend_mir_source_call_argument_ref, ordinal));
	assert(offsetof(zend_mir_source_call_argument_ref, flags)
		> offsetof(zend_mir_source_call_argument_ref, mode));
	parameter_mode.ordinal = 127;
	parameter_mode.mode = ZEND_MIR_SOURCE_PARAMETER_BY_VALUE;
	assert(parameter_mode.ordinal == 127);
	assert(parameter_mode.mode == ZEND_MIR_SOURCE_PARAMETER_BY_VALUE);
	assert(offsetof(zend_mir_call_frame_descriptor, function_symbol_id)
		> offsetof(zend_mir_call_frame_descriptor, function_id));
	assert(offsetof(zend_mir_call_site_ref, result_id)
		> offsetof(zend_mir_call_site_ref, arguments));
	site.result_id = ZEND_MIR_ID_INVALID;
	assert(!zend_mir_id_is_valid(site.result_id));
	callee.frame_state_id = ZEND_MIR_ID_INVALID;
	callee.function_id = ZEND_MIR_ID_INVALID;
	callee.function_symbol_id = 1;
	callee.op_array_id = 2;
	assert(!zend_mir_id_is_valid(callee.frame_state_id));
	assert(!zend_mir_id_is_valid(callee.function_id));
	assert(zend_mir_id_is_valid(callee.function_symbol_id));
	assert(zend_mir_id_is_valid(callee.op_array_id));

	plan.entries = NULL;
	plan.count = 0;
	plan.complete = false;
	plan.immutable = false;
	assert(plan.entries == NULL && plan.count == 0);

	receipt.capabilities = ZEND_MIR_W05_REQUIRED_CAPABILITIES;
	receipt.semantic_debts = ZEND_MIR_W05_REQUIRED_DEBTS;
	receipt.modeled = true;
	receipt.codegen_eligible = false;
	assert(receipt.modeled && !receipt.codegen_eligible);
	verifier_receipt.verifier_id = ZEND_MIR_VERIFIER_CALL_MODEL;
	verifier_receipt.status = ZEND_MIR_VERIFIER_STATUS_PASS;
	assert(verifier_receipt.verifier_id == ZEND_MIR_VERIFIER_CALL_MODEL);

	result.lowering.status = ZEND_MIR_LOWERING_SUCCESS;
	result.lowering.diagnostic_code = ZEND_MIRL_OK;
	result.lowering.guarantees = ZEND_MIR_LOWERING_GUARANTEE_FINALIZED;
	result.lowering.module = (zend_mir_module *)(uintptr_t) 1;
	result.prerequisite_guarantees = ZEND_MIR_LOWERING_GUARANTEE_W04_ALL;
	result.capabilities = ZEND_MIR_W05_REQUIRED_CAPABILITIES;
	result.semantic_debts = ZEND_MIR_W05_REQUIRED_DEBTS;
	result.modeled = true;
	result.codegen_eligible = false;
	assert(zend_mir_lowering_result_is_w05_failure_atomic(&result));
	result.lowering.guarantees = ZEND_MIR_LOWERING_GUARANTEE_W04_ALL;
	assert(!zend_mir_lowering_result_is_w05_failure_atomic(&result));

	result.lowering.status = ZEND_MIR_LOWERING_FAILED;
	result.lowering.diagnostic_code = ZEND_MIRL_W05_CALL_PLAN_FAILED;
	result.lowering.guarantees = 0;
	result.lowering.module = NULL;
	result.prerequisite_guarantees = 0;
	result.capabilities = 0;
	result.semantic_debts = 0;
	result.modeled = false;
	assert(zend_mir_lowering_result_is_w05_failure_atomic(&result));

	assert(ZEND_MIR_VERIFY_W05_SITE_MISMATCH == 700);
	assert(ZEND_MIR_VERIFY_W05_CAPABILITY_DEBT_MISMATCH == 705);
	return 0;
}
