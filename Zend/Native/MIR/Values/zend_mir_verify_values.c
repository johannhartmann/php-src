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

#include "../zend_mir_values.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Core/zend_mir_module_internal.h"

#define ZEND_MIR_VIEW_LIMIT UINT32_C(1048576)

typedef struct _zend_mir_alias_key {
	uint32_t left_id;
	uint32_t right_id;
	zend_mir_alias_relation relation;
} zend_mir_alias_key;
