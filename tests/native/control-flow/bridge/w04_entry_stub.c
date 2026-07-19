/*
 * Branch-local link stub for W04-B compile and schema tests.
 *
 * This file is test evidence only. It is intentionally absent from config.m4
 * and cannot publish a module, guarantees, or control-flow semantics.
 */
#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"

struct _zend_op_array;
struct _zend_ssa;

zend_mir_lowering_result zend_mir_lower_w04_zend_op_array(
	const struct _zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_lowering_result result;

	(void) op_array;
	(void) ssa;
	(void) module_ops;
	(void) diagnostics;
	result.status = ZEND_MIR_LOWERING_DEFERRED;
	result.diagnostic_code = ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED;
	result.guarantees = 0;
	result.module = NULL;
	return result;
}
