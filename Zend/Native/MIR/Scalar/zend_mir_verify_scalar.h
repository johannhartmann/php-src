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

#ifndef ZEND_MIR_VERIFY_SCALAR_H
#define ZEND_MIR_VERIFY_SCALAR_H

#include "zend_mir_scalar_descriptors.h"

typedef enum _zend_mir_scalar_verify_code {
	ZEND_MIR_SCALAR_VERIFY_INVALID_ARGUMENT = 600,
	ZEND_MIR_SCALAR_VERIFY_INCOMPLETE_VIEW = 601,
	ZEND_MIR_SCALAR_VERIFY_CAPACITY_EXCEEDED = 602,
	ZEND_MIR_SCALAR_VERIFY_ALLOCATION_FAILED = 603,
	ZEND_MIR_SCALAR_VERIFY_CALLBACK_FAILED = 604,
	ZEND_MIR_SCALAR_VERIFY_INVALID_FACT = 610,
	ZEND_MIR_SCALAR_VERIFY_DUPLICATE_FACT_ID = 611,
	ZEND_MIR_SCALAR_VERIFY_DUPLICATE_VALUE_FACT = 612,
	ZEND_MIR_SCALAR_VERIFY_MISSING_FACT = 613,
	ZEND_MIR_SCALAR_VERIFY_INVALID_OPCODE = 620,
	ZEND_MIR_SCALAR_VERIFY_INVALID_OPERAND = 621,
	ZEND_MIR_SCALAR_VERIFY_INVALID_RESULT = 622,
	ZEND_MIR_SCALAR_VERIFY_MISSING_PROOF = 623,
	ZEND_MIR_SCALAR_VERIFY_INVALID_EFFECTS = 624,
	ZEND_MIR_SCALAR_VERIFY_INVALID_OWNERSHIP = 625,
	ZEND_MIR_SCALAR_VERIFY_INVALID_SOURCE = 626,
	ZEND_MIR_SCALAR_VERIFY_INVALID_SCOPE = 627,
	ZEND_MIR_SCALAR_VERIFY_CODE_INVALID = -1
} zend_mir_scalar_verify_code;

#ifdef __cplusplus
extern "C" {
#endif

const char *zend_mir_scalar_verify_code_name(zend_mir_scalar_verify_code code);

#ifdef ZEND_MIR_VERIFY_TESTING
void zend_mir_verify_scalar_test_fail_allocation_after(
	uint32_t successful_allocations);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZEND_MIR_VERIFY_SCALAR_H */
