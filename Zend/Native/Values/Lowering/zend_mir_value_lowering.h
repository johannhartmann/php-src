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
#include "../Contracts/zend_mir_value_lowering_inventory.h"
#include "../Core/zend_mir_value_core.h"

struct _zend_op_array;
struct _zend_ssa;
struct _zend_mir_lowering_context;
struct _zend_mir_straight_line_provider_context;

/*
 * Process-local owner for the immutable source inventory and atomic plan.
 * The opaque allocation contains only pointer-free records; Zend pointers are
 * borrowed by the builder and are never retained.
 */
typedef struct _zend_mir_value_snapshot {
	zend_mir_source_value_view source_view;
	zend_mir_value_lowering_inventory inventory;
	void *records;
} zend_mir_value_snapshot;

bool zend_mir_opcode_is_accepted(uint32_t opcode);
zend_mir_opcode zend_mir_executable_opcode(uint32_t opcode);
bool zend_mir_object_opcode_is_executable(uint32_t opcode);
bool zend_mir_overlay_opcode_is_executable(uint32_t opcode);

bool zend_mir_emit_executable_values(
	const struct _zend_op_array *op_array,
	struct _zend_mir_lowering_context *lowering_context,
	zend_mir_module *module,
	const zend_mir_control_flow_map *control_flow_map,
	struct _zend_mir_straight_line_provider_context *frame_context,
	const uint8_t *scalarized_opcodes,
	uint32_t scalarized_opcode_count);

#endif /* ZEND_MIR_VALUE_LOWERING_H */
