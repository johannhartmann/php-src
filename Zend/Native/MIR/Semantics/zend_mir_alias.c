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

#include "zend_mir_alias.h"

static bool zend_mir_memory_domain_is_valid(zend_mir_memory_domain domain)
{
	return (uint32_t) domain < ZEND_MIR_MEMORY_DOMAIN_COUNT;
}

zend_mir_alias_kind zend_mir_alias_relation(
	zend_mir_memory_domain left, zend_mir_memory_domain right)
{
	uint32_t index;

	if (!zend_mir_memory_domain_is_valid(left)
			|| !zend_mir_memory_domain_is_valid(right)) {
		return ZEND_MIR_ALIAS_UNKNOWN;
	}
	if (left == right) {
		return ZEND_MIR_ALIAS_MAY_ALIAS;
	}
	for (index = 0; index < zend_mir_alias_descriptor_count(); index++) {
		const zend_mir_alias_descriptor *descriptor =
			zend_mir_alias_descriptor_at(index);
		if (descriptor->left == left && descriptor->right == right) {
			return descriptor->kind;
		}
	}

	/* Absence from W01 is not evidence that two domains are disjoint. */
	return ZEND_MIR_ALIAS_MAY_ALIAS;
}

bool zend_mir_domains_may_alias(zend_mir_memory_domain left, zend_mir_memory_domain right)
{
	/* UNKNOWN is deliberately conservative too. */
	(void) zend_mir_alias_relation(left, right);
	return true;
}

bool zend_mir_domain_masks_may_alias(
	zend_mir_memory_domain_mask left, zend_mir_memory_domain_mask right)
{
	const zend_mir_memory_domain_mask all_domains =
		(UINT32_C(1) << ZEND_MIR_MEMORY_DOMAIN_COUNT) - 1;
	uint32_t left_index;
	uint32_t right_index;

	if (left == 0 || right == 0) {
		return false;
	}
	if ((left & ~all_domains) != 0 || (right & ~all_domains) != 0) {
		return true;
	}
	for (left_index = 0; left_index < ZEND_MIR_MEMORY_DOMAIN_COUNT; left_index++) {
		if ((left & ZEND_MIR_MEMORY_DOMAIN_MASK(left_index)) == 0) {
			continue;
		}
		for (right_index = 0; right_index < ZEND_MIR_MEMORY_DOMAIN_COUNT; right_index++) {
			if ((right & ZEND_MIR_MEMORY_DOMAIN_MASK(right_index)) != 0
					&& zend_mir_domains_may_alias(
						(zend_mir_memory_domain) left_index,
						(zend_mir_memory_domain) right_index)) {
				return true;
			}
		}
	}
	return false;
}
