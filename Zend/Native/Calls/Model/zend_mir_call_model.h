#ifndef ZEND_MIR_CALL_MODEL_H
#define ZEND_MIR_CALL_MODEL_H

#include "../../Lowering/zend_mir_lowering_zend.h"

typedef enum _zend_mir_w05_test_fault {
	ZEND_MIR_W05_TEST_FAULT_NONE = 0,
	ZEND_MIR_W05_TEST_FAULT_PLANNER_ALLOCATION,
	ZEND_MIR_W05_TEST_FAULT_TARGET_SNAPSHOT,
	ZEND_MIR_W05_TEST_FAULT_ARGUMENT_TABLE,
	ZEND_MIR_W05_TEST_FAULT_FRAME_STATE,
	ZEND_MIR_W05_TEST_FAULT_CALL_RECORD,
	ZEND_MIR_W05_TEST_FAULT_STRUCTURAL_VERIFIER,
	ZEND_MIR_W05_TEST_FAULT_SCALAR_VERIFIER,
	ZEND_MIR_W05_TEST_FAULT_CONTROL_FLOW_VERIFIER,
	ZEND_MIR_W05_TEST_FAULT_CALL_VERIFIER,
	ZEND_MIR_W05_TEST_FAULT_FINGERPRINT_RECOMPUTE
} zend_mir_w05_test_fault;

#ifdef ZEND_MIR_W05_TEST_FAULTS
void zend_mir_w05_test_set_fault(zend_mir_w05_test_fault fault);
#endif

zend_mir_w05_lowering_result zend_mir_lower_w05_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

/* W07 executes the same verified direct-user call records, permits proven
 * scalar result chaining, and accepts omitted trailing scalar defaults. */
zend_mir_w05_lowering_result zend_mir_lower_w07_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	zend_mir_control_flow_map *control_flow_map,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

/*
 * W06 reuses the frozen W05 record layouts for its scalar prerequisite
 * projection, but must commit the call and value tables in one final module.
 * This helper performs only the already-frozen W05 planning and emission
 * steps. The caller owns finalization and every named verifier.
 */
zend_mir_lowering_diagnostic_code zend_mir_w05_plan_and_emit_calls(
	zend_mir_lowering_context *context,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver,
	zend_mir_call_mutator *call_mutator);

zend_mir_lowering_diagnostic_code zend_mir_w05_validate_call_plan(
	zend_mir_lowering_context *context,
	const zend_mir_source_call_view *source_calls,
	const zend_mir_source_call_target_resolver *resolver);

#endif /* ZEND_MIR_CALL_MODEL_H */
