#include <assert.h>

#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"

int main(void)
{
	zend_mir_lowering_result result =
		zend_mir_lower_w04_zend_source(NULL, NULL, NULL);

	assert(result.status == ZEND_MIR_LOWERING_DEFERRED);
	assert(result.diagnostic_code == ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED);
	assert(result.guarantees == 0);
	assert(result.module == NULL);
	assert(zend_mir_lowering_result_is_w04_failure_atomic(&result));
	return 0;
}
