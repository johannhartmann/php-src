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
 * callers must add the canonical capability/debt spans and commit before the
 * module is finalized. No operation executes a destructor or clones storage.
 */
zend_mir_value_mutator *zend_mir_module_get_value_mutator(
	zend_mir_module *module);
const zend_mir_value_view *zend_mir_module_get_value_view(
	const zend_mir_module *module);
bool zend_mir_module_commit_value_model(
	zend_mir_module *module,
	const zend_mir_capability_id *capability_ids,
	uint32_t capability_count,
	const zend_mir_semantic_debt_id *semantic_debt_ids,
	uint32_t semantic_debt_count);

zend_mir_alias_relation zend_mir_value_merge_alias_relation(
	zend_mir_alias_relation left, zend_mir_alias_relation right);
zend_mir_refcount_state zend_mir_value_merge_refcount_state(
	zend_mir_refcount_state left, zend_mir_refcount_state right);
bool zend_mir_value_merge_storage_state(
	const zend_mir_storage_ref *left,
	const zend_mir_storage_ref *right,
	zend_mir_storage_ref *out);

#endif /* ZEND_MIR_VALUE_CORE_H */
