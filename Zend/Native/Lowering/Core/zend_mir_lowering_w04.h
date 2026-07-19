#ifndef ZEND_MIR_LOWERING_W04_H
#define ZEND_MIR_LOWERING_W04_H

#include "zend_mir_lowering_internal.h"

struct _zend_op_array;
struct _zend_ssa;

/*
 * Process-local Zend integration wrapper for the frozen W04 worker entry.
 * The returned module owns no Zend pointers; all adapter state is released
 * after stage-3 verification completes.
 */
zend_mir_lowering_result zend_mir_lower_w04_zend_op_array(
	const struct _zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

#endif /* ZEND_MIR_LOWERING_W04_H */
