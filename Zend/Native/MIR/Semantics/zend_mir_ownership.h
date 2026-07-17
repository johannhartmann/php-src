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

#ifndef ZEND_MIR_OWNERSHIP_H
#define ZEND_MIR_OWNERSHIP_H

#include <stdbool.h>
#include <stdint.h>

#include "zend_mir_effect_summary.h"

typedef enum _zend_mir_cleanup_obligation {
	ZEND_MIR_CLEANUP_NONE = 0,
	ZEND_MIR_CLEANUP_EXACTLY_ONE_RELEASE = 1,
	ZEND_MIR_CLEANUP_INVALID = 2
} zend_mir_cleanup_obligation;

typedef enum _zend_mir_ownership_transition_status {
	ZEND_MIR_OWNERSHIP_TRANSITION_OK = 0,
	ZEND_MIR_OWNERSHIP_TRANSITION_INVALID_STATE = 1,
	ZEND_MIR_OWNERSHIP_TRANSITION_INVALID_ACTION = 2,
	ZEND_MIR_OWNERSHIP_TRANSITION_TERMINAL_STATE = 3,
	ZEND_MIR_OWNERSHIP_TRANSITION_NOT_ALLOWED = 4
} zend_mir_ownership_transition_status;

typedef enum _zend_mir_phi_merge_status {
	ZEND_MIR_PHI_MERGE_ALLOWED = 0,
	ZEND_MIR_PHI_MERGE_REQUIRES_CANONICALIZE = 1,
	ZEND_MIR_PHI_MERGE_REJECTED = 2
} zend_mir_phi_merge_status;

typedef struct _zend_mir_ownership_state_descriptor {
	zend_mir_ownership_state state;
	bool terminal;
	zend_mir_cleanup_obligation cleanup;
} zend_mir_ownership_state_descriptor;

typedef struct _zend_mir_ownership_action_descriptor {
	zend_mir_ownership_action action;
	uint8_t allowed_states;
	bool source_unchanged;
	zend_mir_ownership_state source_after;
	bool has_result;
	zend_mir_ownership_state result_state;
	zend_mir_effect_mask effects;
	zend_mir_memory_domain_mask reads;
	zend_mir_memory_domain_mask writes;
	zend_mir_barrier_mask barriers;
} zend_mir_ownership_action_descriptor;

typedef struct _zend_mir_ownership_transition {
	zend_mir_ownership_state source_after;
	bool has_result;
	zend_mir_ownership_state result_state;
	zend_mir_effect_summary summary;
} zend_mir_ownership_transition;

#ifdef __cplusplus
extern "C" {
#endif

const char *zend_mir_ownership_state_name(zend_mir_ownership_state state);
const char *zend_mir_ownership_action_name(zend_mir_ownership_action action);
const zend_mir_ownership_state_descriptor *zend_mir_ownership_state_descriptor_at(
	zend_mir_ownership_state state);
const zend_mir_ownership_action_descriptor *zend_mir_ownership_action_descriptor_at(
	zend_mir_ownership_action action);
bool zend_mir_ownership_state_is_usable(zend_mir_ownership_state state);
zend_mir_cleanup_obligation zend_mir_ownership_cleanup(
	zend_mir_ownership_state state);
zend_mir_ownership_transition_status zend_mir_ownership_apply(
	zend_mir_ownership_state state, zend_mir_ownership_action action,
	zend_mir_ownership_transition *transition);
zend_mir_phi_merge_status zend_mir_ownership_phi_merge(
	zend_mir_ownership_state left, zend_mir_ownership_state right,
	zend_mir_ownership_state *merged_state, zend_mir_cleanup_obligation *cleanup);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_OWNERSHIP_H */
