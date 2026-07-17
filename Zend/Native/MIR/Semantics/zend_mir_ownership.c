/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the Modified BSD License that is      |
  | bundled with this package in the file LICENSE, and is available      |
  | through the World Wide Web at <https://www.php.net/license/>.        |
  |                                                                      |
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <stddef.h>

#include "zend_mir_ownership.h"

bool zend_mir_ownership_state_is_usable(zend_mir_ownership_state state)
{
	return state == ZEND_MIR_OWNERSHIP_STATE_BORROWED
		|| state == ZEND_MIR_OWNERSHIP_STATE_OWNED
		|| state == ZEND_MIR_OWNERSHIP_STATE_SHARED_OWNED;
}

zend_mir_cleanup_obligation zend_mir_ownership_cleanup(
	zend_mir_ownership_state state)
{
	const zend_mir_ownership_state_descriptor *descriptor =
		zend_mir_ownership_state_descriptor_at(state);
	return descriptor != NULL ? descriptor->cleanup : ZEND_MIR_CLEANUP_INVALID;
}

static void zend_mir_ownership_transition_reject(zend_mir_ownership_transition *transition)
{
	if (transition == NULL) {
		return;
	}
	transition->source_after = ZEND_MIR_OWNERSHIP_STATE_INVALID;
	transition->has_result = false;
	transition->result_state = ZEND_MIR_OWNERSHIP_STATE_INVALID;
	zend_mir_effect_summary_fail_closed(&transition->summary);
}

zend_mir_ownership_transition_status zend_mir_ownership_apply(
	zend_mir_ownership_state state, zend_mir_ownership_action action,
	zend_mir_ownership_transition *transition)
{
	const zend_mir_ownership_state_descriptor *state_descriptor;
	const zend_mir_ownership_action_descriptor *action_descriptor;

	if (transition == NULL) {
		return ZEND_MIR_OWNERSHIP_TRANSITION_NOT_ALLOWED;
	}
	state_descriptor = zend_mir_ownership_state_descriptor_at(state);
	if (state_descriptor == NULL) {
		zend_mir_ownership_transition_reject(transition);
		return ZEND_MIR_OWNERSHIP_TRANSITION_INVALID_STATE;
	}
	action_descriptor = zend_mir_ownership_action_descriptor_at(action);
	if (action_descriptor == NULL) {
		zend_mir_ownership_transition_reject(transition);
		return ZEND_MIR_OWNERSHIP_TRANSITION_INVALID_ACTION;
	}
	if (state_descriptor->terminal) {
		zend_mir_ownership_transition_reject(transition);
		return ZEND_MIR_OWNERSHIP_TRANSITION_TERMINAL_STATE;
	}
	if ((action_descriptor->allowed_states & (UINT8_C(1) << state)) == 0) {
		zend_mir_ownership_transition_reject(transition);
		return ZEND_MIR_OWNERSHIP_TRANSITION_NOT_ALLOWED;
	}

	transition->source_after = action_descriptor->source_unchanged
		? state : action_descriptor->source_after;
	transition->has_result = action_descriptor->has_result;
	transition->result_state = action_descriptor->has_result
		? action_descriptor->result_state : ZEND_MIR_OWNERSHIP_STATE_INVALID;
	if (!zend_mir_effect_summary_init(&transition->summary,
			action_descriptor->effects, action_descriptor->reads,
			action_descriptor->writes, action_descriptor->barriers, 0,
			ZEND_MIR_OWNERSHIP_ACTION_MASK(action))) {
		zend_mir_ownership_transition_reject(transition);
		return ZEND_MIR_OWNERSHIP_TRANSITION_NOT_ALLOWED;
	}
	return ZEND_MIR_OWNERSHIP_TRANSITION_OK;
}

zend_mir_phi_merge_status zend_mir_ownership_phi_merge(
	zend_mir_ownership_state left, zend_mir_ownership_state right,
	zend_mir_ownership_state *merged_state, zend_mir_cleanup_obligation *cleanup)
{
	if (merged_state != NULL) {
		*merged_state = ZEND_MIR_OWNERSHIP_STATE_INVALID;
	}
	if (cleanup != NULL) {
		*cleanup = ZEND_MIR_CLEANUP_NONE;
	}
	if (!zend_mir_ownership_state_is_usable(left)
			|| !zend_mir_ownership_state_is_usable(right)) {
		return ZEND_MIR_PHI_MERGE_REJECTED;
	}
	if (left != right) {
		return ZEND_MIR_PHI_MERGE_REQUIRES_CANONICALIZE;
	}
	if (merged_state != NULL) {
		*merged_state = left;
	}
	if (cleanup != NULL) {
		*cleanup = zend_mir_ownership_cleanup(left);
	}
	return ZEND_MIR_PHI_MERGE_ALLOWED;
}
