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

#ifndef ZEND_MIR_ALIAS_H
#define ZEND_MIR_ALIAS_H

#include <stdbool.h>
#include <stdint.h>

#include "../zend_mir_effects.h"

typedef enum _zend_mir_alias_kind {
	ZEND_MIR_ALIAS_MAY_ALIAS = 0,
	ZEND_MIR_ALIAS_CONTAINS_REFERENCE = 1,
	ZEND_MIR_ALIAS_INDIRECT_ACCESS = 2,
	ZEND_MIR_ALIAS_SHARES_POINTEE = 3,
	ZEND_MIR_ALIAS_UNKNOWN = 4
} zend_mir_alias_kind;

typedef struct _zend_mir_alias_descriptor {
	zend_mir_memory_domain left;
	zend_mir_memory_domain right;
	zend_mir_alias_kind kind;
} zend_mir_alias_descriptor;

#ifdef __cplusplus
extern "C" {
#endif

uint32_t zend_mir_alias_descriptor_count(void);
const zend_mir_alias_descriptor *zend_mir_alias_descriptor_at(uint32_t index);
zend_mir_alias_kind zend_mir_alias_relation(
	zend_mir_memory_domain left, zend_mir_memory_domain right);
bool zend_mir_domains_may_alias(zend_mir_memory_domain left, zend_mir_memory_domain right);
bool zend_mir_domain_masks_may_alias(
	zend_mir_memory_domain_mask left, zend_mir_memory_domain_mask right);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_ALIAS_H */
