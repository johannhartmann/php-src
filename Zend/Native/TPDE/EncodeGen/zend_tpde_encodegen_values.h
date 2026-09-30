// SPDX-License-Identifier: PHP-3.01

/* Result codes shared by the PHP value snippets and the code calling them. */

#ifndef ZEND_TPDE_ENCODEGEN_VALUES_H
#define ZEND_TPDE_ENCODEGEN_VALUES_H

/*
 * The array element probes return the element's zval, or: the probe cannot
 * decide and the helper must, or the key is certainly absent.
 */
#define ZEND_NATIVE_ELEMENT_UNKNOWN ((uintptr_t) 0)
#define ZEND_NATIVE_ELEMENT_ABSENT ((uintptr_t) 1)

/* zend_native_zval_empty(): the helper decides (objects, other types). */
#define ZEND_NATIVE_EMPTY_UNKNOWN 2

/* zend_native_zval_type_check(): an undefined variable, which warns. */
#define ZEND_NATIVE_TYPE_CHECK_UNDEFINED 2

/* zend_native_zval_identical(): the helper compares (doubles, arrays,
 * objects, undefined variables). */
#define ZEND_NATIVE_IDENTICAL_UNKNOWN 2

/* zend_native_zval_array_count(): not an array; the helper counts. */
#define ZEND_NATIVE_COUNT_UNKNOWN UINT64_MAX

#endif
