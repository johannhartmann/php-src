#ifndef ZEND_MIR_LOWERING_ZEND_H
#define ZEND_MIR_LOWERING_ZEND_H

#include "../MIR/zend_mir_control_flow.h"
#include "../MIR/zend_mir_call.h"
#include "../Calls/Contracts/zend_mir_call_plan.h"
#include "../Values/Contracts/zend_mir_value_lowering_inventory.h"
#include "zend_mir_lowering.h"

typedef struct _zend_script zend_script;
typedef struct _zend_op_array zend_op_array;
typedef struct _zend_ssa zend_ssa;
typedef struct _zend_mir_lowering_module_ops zend_mir_lowering_module_ops;

/*
 * Internal control-flow lowering entry point. The source view owned by
 * context and the process-local map must remain alive through stage-3
 * verification.
 */
zend_mir_lowering_result zend_mir_lower_control_flow_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *map);

zend_mir_lowering_result zend_mir_lower_calls_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

/*
 * Product compiler entry. Source, SSA and module callbacks remain borrowed
 * through the synchronous failure-atomic lowering operation.
 */
zend_mir_lowering_result zend_mir_lower_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

/*
 * Product compiler entry for targets with the typed lowering tier (ADR
 * 0024): the same op_array lowering with whole-function typed scalar
 * operations enabled.
 */
zend_mir_lowering_result zend_mir_lower_typed_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

#endif /* ZEND_MIR_LOWERING_ZEND_H */
