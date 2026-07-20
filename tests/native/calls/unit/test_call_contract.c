#include <assert.h>
#include <stdint.h>

#include "Zend/Native/Calls/Contracts/zend_mir_call_plan.h"
#include "Zend/Native/Calls/Contracts/zend_mir_call_source.h"
#include "Zend/Native/MIR/zend_mir_call.h"
#include "Zend/Native/MIR/zend_mir_opcodes.h"

int main(void)
{
	zend_mir_call_capability_receipt_ref receipt = {0};
	zend_mir_source_call_argument_ref argument = {0};
	zend_mir_source_parameter_mode_ref parameter = {0};

	assert(ZEND_MIR_OPCODE_CALL_DIRECT_USER == 41);
	assert(ZEND_MIR_W05_OPCODE_COUNT == 42);
	assert(ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE == 0);
	assert(ZEND_MIR_SOURCE_PARAMETER_BY_VALUE == 0);
	assert(ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE == 1);
	assert(ZEND_MIR_CALL_PLAN_ACCEPTED == 0);
	argument.ordinal = 0;
	argument.mode = ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE;
	argument.flags = 0;
	assert(argument.ordinal == 0);
	assert(argument.mode == ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE);
	assert(argument.flags == 0);
	parameter.target_id = 7;
	parameter.ordinal = 65;
	parameter.mode = ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE;
	assert(parameter.target_id == 7);
	assert(parameter.ordinal == 65);
	assert(parameter.mode == ZEND_MIR_SOURCE_PARAMETER_BY_REFERENCE);
	receipt.canonical.capability_ids.count = 6;
	receipt.canonical.semantic_debt_ids.count = 8;
	receipt.capabilities = ZEND_MIR_W05_REQUIRED_CAPABILITIES;
	receipt.semantic_debts = ZEND_MIR_W05_REQUIRED_DEBTS;
	receipt.modeled = true;
	receipt.codegen_eligible = false;
	assert(receipt.capabilities == ZEND_MIR_W05_REQUIRED_CAPABILITIES);
	assert(receipt.semantic_debts == ZEND_MIR_W05_REQUIRED_DEBTS);
	assert(receipt.canonical.capability_ids.count == 6);
	assert(receipt.canonical.semantic_debt_ids.count == 8);
	assert(receipt.modeled && !receipt.codegen_eligible);
	return 0;
}
