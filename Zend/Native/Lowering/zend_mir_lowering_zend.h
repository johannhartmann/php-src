#ifndef ZEND_MIR_LOWERING_ZEND_H
#define ZEND_MIR_LOWERING_ZEND_H

#include "../MIR/zend_mir_control_flow.h"
#include "../MIR/zend_mir_call.h"
#include "../Calls/Contracts/zend_mir_call_plan.h"
#include "../Values/Contracts/zend_mir_value_plan.h"
#include "zend_mir_lowering.h"

/*
 * Internal W04 entry point. The source view owned by context and the
 * process-local map must remain alive through stage-3 verification.
 */
zend_mir_lowering_result zend_mir_lower_w04_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *map);

/*
 * Internal modeling-only W05 entry. The complete immutable plan is validated
 * before either mutator is called. Resolver and source views remain
 * process-local through named call verification.
 */
zend_mir_w05_lowering_result zend_mir_lower_w05_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

zend_mir_w05_lowering_result zend_mir_lower_w07_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

zend_mir_w08_lowering_result zend_mir_lower_w08_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

zend_mir_w08_lowering_result zend_mir_lower_w09_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

zend_mir_w08_lowering_result zend_mir_lower_w10_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

/*
 * Internal modeling-only W06 entry. Source inventory and the complete value
 * plan are validated before either mutator is called. All source, call and
 * value views remain process-local until every receipt is emitted for one
 * final module fingerprint.
 */
zend_mir_w06_lowering_result zend_mir_lower_w06_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator,
	const zend_mir_source_value_view *source_values,
	const zend_mir_value_plan *value_plan,
	zend_mir_value_mutator *value_mutator);

#endif /* ZEND_MIR_LOWERING_ZEND_H */
