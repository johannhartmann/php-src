#include <assert.h>

#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"

struct _zend_op_array;
struct _zend_ssa;

zend_mir_lowering_result zend_mir_lower_w04_zend_op_array(
	const struct _zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

int main(void)
{
	zend_mir_lowering_result result =
		zend_mir_lower_w04_zend_op_array(NULL, NULL, NULL, NULL);

	assert(result.status == ZEND_MIR_LOWERING_DEFERRED);
	assert(result.diagnostic_code == ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED);
	assert(result.guarantees == 0);
	assert(result.module == NULL);
	assert(zend_mir_lowering_result_is_w04_failure_atomic(&result));
	return 0;
}
