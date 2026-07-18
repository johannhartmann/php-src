/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_STRAIGHT_LINE_INTERNAL_H
#define ZEND_MIR_STRAIGHT_LINE_INTERNAL_H

#include "zend_mir_straight_line.h"

bool zend_mir_straight_line_value_contract_is_valid(
	const zend_mir_straight_line_value *value);
void zend_mir_straight_line_restore_entry_state(
	zend_mir_straight_line_lifetime *lifetime, bool entry_emitted,
	zend_mir_frame_state_id entry_frame_state_id);
bool zend_mir_straight_line_emit_source_position(
	const zend_mir_straight_line_entry *entry,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_source_position_id *source_id_out);
bool zend_mir_straight_line_emit_observable_frame(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator,
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_frame_state_id *frame_id_out,
	zend_mir_source_position_id *source_id_out);

#endif /* ZEND_MIR_STRAIGHT_LINE_INTERNAL_H */
