// SPDX-License-Identifier: PHP-3.01

/*
 * PHP value snippets for TPDE EncodeGen (Linux x86-64).
 *
 * Each snippet takes concrete values or addresses and returns concrete
 * values: no encoded opcode operand, no call, no global and no stack frame.
 * Layouts come from the Zend headers of a configured build. Probes change
 * nothing before they succeed, so a miss can repeat the whole operation in
 * the runtime helper. regenerate.sh compiles this file together with
 * zend_tpde_encodegen.c into ../LinuxX64/zend_tpde_encodegen_x64.hpp.
 */

#include "Zend/zend.h"
#include "Zend/zend_types.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_object_handlers.h"

/* The value of a zval as its two machine words. */
typedef struct _zend_native_boxed {
	uint64_t payload;
	uint64_t type_info;
} zend_native_boxed;

/*
 * The slot of a declared property of an object receiver whose class and
 * offset the VM run-time cache slot names, or NULL: a non-object receiver, a
 * different class, a dynamic property, or an undefined or reference slot.
 */
zval *zend_native_property_slot(const zval *receiver, void *const *cache_slot)
{
	const zend_object *object;
	uintptr_t offset;
	zval *property;

	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		return NULL;
	}
	object = Z_OBJ_P(receiver);
	if (cache_slot[0] != object->ce) {
		return NULL;
	}
	offset = (uintptr_t) cache_slot[1];
	if (!IS_VALID_PROPERTY_OFFSET(offset)) {
		return NULL;
	}
	property = OBJ_PROP(object, offset);
	return Z_TYPE_P(property) == IS_UNDEF
			|| Z_TYPE_P(property) == IS_REFERENCE
		? NULL : property;
}

/* ZVAL_COPY: the value with a new reference to a counted payload. */
zend_native_boxed zend_native_zval_copy(const zval *value)
{
	zend_native_boxed boxed;

	boxed.payload = (uint64_t) Z_LVAL_P(value);
	boxed.type_info = Z_TYPE_INFO_P(value);
	if (Z_TYPE_INFO_REFCOUNTED(Z_TYPE_INFO_P(value))) {
		GC_ADDREF(Z_COUNTED_P(value));
	}
	return boxed;
}

/* Whether a zval holds true, as a machine boolean. */
uint64_t zend_native_zval_is_true_type(const zval *value)
{
	return Z_TYPE_P(value) == IS_TRUE;
}

double zend_native_load_f64(const double *address)
{
	return *address;
}
