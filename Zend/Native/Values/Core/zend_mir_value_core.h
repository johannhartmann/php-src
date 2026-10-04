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

#ifndef ZEND_MIR_VALUE_CORE_H
#define ZEND_MIR_VALUE_CORE_H

#include "../../MIR/Core/zend_mir_arena.h"
#include "../../MIR/zend_mir_values.h"

/*
 * The mutator stages pointer-free records. Publication is all-or-nothing:
 * callers commit the complete model before the module is finalized. No
 * operation executes a destructor or clones storage.
 */
zend_mir_value_mutator *zend_mir_module_get_value_mutator(
	zend_mir_module *module);
bool zend_mir_module_commit_value_model(zend_mir_module *module);

bool zend_mir_value_transition_valid(
	zend_mir_transfer_action action,
	zend_mir_refcount_state before_state,
	zend_mir_refcount_state after_state,
	bool cleanup_obligation);

#endif /* ZEND_MIR_VALUE_CORE_H */
