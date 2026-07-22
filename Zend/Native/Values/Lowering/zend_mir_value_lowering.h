/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_VALUE_LOWERING_H
#define ZEND_MIR_VALUE_LOWERING_H

#include "../../Calls/Contracts/zend_mir_call_source.h"
#include "../../Lowering/Frontend/zend_mir_zend_source.h"
#include "../../MIR/zend_mir_control_flow.h"
#include "../Contracts/zend_mir_value_plan.h"
#include "../Core/zend_mir_value_core.h"

struct _zend_op_array;
struct _zend_ssa;
struct _zend_mir_lowering_context;
struct _zend_mir_straight_line_provider_context;

typedef enum _zend_mir_w06_test_fault {
	ZEND_MIR_W06_TEST_FAULT_NONE = 0,
	ZEND_MIR_W06_TEST_FAULT_INVENTORY,
	ZEND_MIR_W06_TEST_FAULT_PLAN,
	ZEND_MIR_W06_TEST_FAULT_STORAGE,
	ZEND_MIR_W06_TEST_FAULT_REFERENCE_CELL,
	ZEND_MIR_W06_TEST_FAULT_ALIAS,
	ZEND_MIR_W06_TEST_FAULT_EVENT,
	ZEND_MIR_W06_TEST_FAULT_SEPARATION,
	ZEND_MIR_W06_TEST_FAULT_CALL_TRANSFER,
	ZEND_MIR_W06_TEST_FAULT_STRUCTURAL_VERIFIER,
	ZEND_MIR_W06_TEST_FAULT_SCALAR_VERIFIER,
	ZEND_MIR_W06_TEST_FAULT_CONTROL_FLOW_VERIFIER,
	ZEND_MIR_W06_TEST_FAULT_CALL_VERIFIER,
	ZEND_MIR_W06_TEST_FAULT_FINGERPRINT_RECOMPUTE,
	ZEND_MIR_W06_TEST_FAULT_VALUE_VERIFIER
} zend_mir_w06_test_fault;

/*
 * Process-local owner for the immutable source inventory and atomic plan.
 * The opaque allocation contains only pointer-free records; Zend pointers are
 * borrowed by the builder and are never retained.
 */
typedef struct _zend_mir_w06_value_snapshot {
	zend_mir_source_value_view source_view;
	zend_mir_value_plan plan;
	void *records;
} zend_mir_w06_value_snapshot;

zend_mir_lowering_diagnostic_code zend_mir_w06_build_value_snapshot(
	const struct _zend_op_array *op_array,
	const struct _zend_ssa *ssa,
	const zend_mir_zend_source *zend_source,
	const zend_mir_source_call_view *source_calls,
	zend_mir_w06_value_snapshot *snapshot);

void zend_mir_w06_release_value_snapshot(
	zend_mir_w06_value_snapshot *snapshot);

bool zend_mir_w06_emit_value_snapshot(
	const zend_mir_source_value_view *source_values,
	const zend_mir_value_plan *plan,
	zend_mir_module *module,
	zend_mir_value_mutator *mutator);

bool zend_mir_w06_opcode_is_accepted(uint32_t opcode);
bool zend_mir_w09_opcode_is_executable(uint32_t opcode);

bool zend_mir_w09_emit_executable_values(
	const struct _zend_op_array *op_array,
	struct _zend_mir_lowering_context *lowering_context,
	zend_mir_module *module,
	const zend_mir_control_flow_map *control_flow_map,
	struct _zend_mir_straight_line_provider_context *frame_context);

zend_mir_lowering_diagnostic_code zend_mir_w06_preflight_literals(
	const struct _zend_op_array *op_array);

#ifdef ZEND_MIR_W06_TEST_FAULTS
void zend_mir_w06_test_set_fault(zend_mir_w06_test_fault fault);
#endif

#endif /* ZEND_MIR_VALUE_LOWERING_H */
