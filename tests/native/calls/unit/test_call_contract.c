#include <assert.h>
#include <stdint.h>

#include "Zend/Native/Calls/Contracts/zend_mir_call_plan.h"
#include "Zend/Native/Calls/Contracts/zend_mir_call_source.h"
#include "Zend/Native/MIR/zend_mir_call.h"
#include "Zend/Native/MIR/zend_mir_opcodes.h"

int main(void)
{
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
	return 0;
}
