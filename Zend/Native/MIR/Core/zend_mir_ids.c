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

#include "zend_mir_module_internal.h"

bool zend_mir_core_id_validate(uint32_t id)
{
	return id <= ZEND_MIR_ID_MAX;
}

bool zend_mir_core_value_id_decode(zend_mir_value_id id, bool *synthetic,
		uint32_t *payload)
{
	if (!zend_mir_core_id_validate(id) || synthetic == NULL || payload == NULL) {
		return false;
	}
	*synthetic = (id & ZEND_MIR_VALUE_SYNTHETIC_BIT) != 0;
	*payload = *synthetic ? id & ~ZEND_MIR_VALUE_SYNTHETIC_BIT : id;
	return true;
}

bool zend_mir_core_next_id(uint32_t count, uint32_t *out)
{
	if (out == NULL || count > ZEND_MIR_ID_MAX) {
		return false;
	}
	*out = count;
	return true;
}

uint32_t zend_mir_core_hash_id(uint32_t id)
{
	id ^= id >> 16;
	id *= UINT32_C(0x7feb352d);
	id ^= id >> 15;
	id *= UINT32_C(0x846ca68b);
	id ^= id >> 16;
	return id;
}
