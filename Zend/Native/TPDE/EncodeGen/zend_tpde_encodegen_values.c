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
#include "Zend/Native/TPDE/EncodeGen/zend_tpde_encodegen_values.h"

/*
 * Refcounts change through the field itself: GC_ADDREF and GC_DELREF add the
 * debug build's RC checks, a global access and a call, and would make the
 * generated header depend on the build configuration.
 */
#define ZEND_NATIVE_ADDREF(counted) ((void) ++(counted)->gc.refcount)
#define ZEND_NATIVE_DELREF(counted) ((void) --(counted)->gc.refcount)

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
		ZEND_NATIVE_ADDREF(Z_COUNTED_P(value));
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

/*
 * Array element probes return the element's zval, ZEND_NATIVE_ELEMENT_ABSENT,
 * or ZEND_NATIVE_ELEMENT_UNKNOWN when they cannot decide: not an array, a key
 * of another type, an indirect element, or a bucket whose key has the
 * literal's hash but is another string.
 */

static zend_always_inline const HashTable *zend_native_probe_array(
	const zval *container)
{
	if (Z_TYPE_P(container) == IS_REFERENCE) {
		container = &Z_REF_P(container)->val;
	}
	return Z_TYPE_P(container) == IS_ARRAY ? Z_ARRVAL_P(container) : NULL;
}

static zend_always_inline uintptr_t zend_native_probe_element(zval *element)
{
	return Z_TYPE_P(element) == IS_INDIRECT
		? ZEND_NATIVE_ELEMENT_UNKNOWN : (uintptr_t) element;
}

static zend_always_inline uintptr_t zend_native_find_index(
	const HashTable *table, zend_ulong h)
{
	uint32_t index;

	if (HT_IS_PACKED(table)) {
		if (h >= table->nNumUsed
				|| Z_TYPE(table->arPacked[h]) == IS_UNDEF) {
			return ZEND_NATIVE_ELEMENT_ABSENT;
		}
		return zend_native_probe_element(&table->arPacked[h]);
	}
	index = HT_HASH_EX(table->arData, (uint32_t) h | table->nTableMask);
	while (index != HT_INVALID_IDX) {
		Bucket *bucket = HT_HASH_TO_BUCKET_EX(table->arData, index);

		if (bucket->h == h && bucket->key == NULL) {
			return zend_native_probe_element(&bucket->val);
		}
		index = Z_NEXT(bucket->val);
	}
	return ZEND_NATIVE_ELEMENT_ABSENT;
}

/*
 * The element under a literal key. The compiler already turned numeric
 * string literals into integers and interned the others, so identity with
 * the bucket key decides string equality; a bucket with the same hash but
 * another key string is left to the helper.
 */
uintptr_t zend_native_array_find_literal(
	const zval *container, const zval *key)
{
	const HashTable *table = zend_native_probe_array(container);
	const zend_string *name;
	zend_ulong h;
	uint32_t index;

	if (table == NULL) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	if (Z_TYPE_P(key) == IS_LONG) {
		return zend_native_find_index(table, (zend_ulong) Z_LVAL_P(key));
	}
	if (Z_TYPE_P(key) != IS_STRING) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	name = Z_STR_P(key);
	h = ZSTR_H(name);
	if (h == 0 || !ZSTR_IS_INTERNED(name)) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	if (HT_IS_PACKED(table)) {
		return ZEND_NATIVE_ELEMENT_ABSENT;
	}
	index = HT_HASH_EX(table->arData, (uint32_t) h | table->nTableMask);
	while (index != HT_INVALID_IDX) {
		Bucket *bucket = HT_HASH_TO_BUCKET_EX(table->arData, index);

		if (bucket->key == name) {
			return zend_native_probe_element(&bucket->val);
		}
		if (bucket->h == h && bucket->key != NULL) {
			return ZEND_NATIVE_ELEMENT_UNKNOWN;
		}
		index = Z_NEXT(bucket->val);
	}
	return ZEND_NATIVE_ELEMENT_ABSENT;
}

/*
 * The element under a runtime key, as FETCH_DIM_R with a CV key sees it: an
 * integer, or a string that cannot be numeric (it does not start with a
 * digit or '-') and already carries its hash. Other strings are undecided,
 * so the helper handles numeric conversion and computes the hash, which the
 * string then keeps.
 */
uintptr_t zend_native_array_find_key(const zval *container, const zval *key)
{
	const HashTable *table = zend_native_probe_array(container);
	const zend_string *name;
	zend_ulong h;
	size_t length;
	uint32_t index;
	unsigned char first;

	if (table == NULL) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	if (Z_TYPE_P(key) == IS_REFERENCE) {
		key = &Z_REF_P(key)->val;
	}
	if (Z_TYPE_P(key) == IS_LONG) {
		return zend_native_find_index(table, (zend_ulong) Z_LVAL_P(key));
	}
	if (Z_TYPE_P(key) != IS_STRING) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	name = Z_STR_P(key);
	h = ZSTR_H(name);
	length = ZSTR_LEN(name);
	if (h == 0 || length == 0) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	first = (unsigned char) ZSTR_VAL(name)[0];
	if ((first >= '0' && first <= '9') || first == '-') {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	if (HT_IS_PACKED(table)) {
		return ZEND_NATIVE_ELEMENT_ABSENT;
	}
	index = HT_HASH_EX(table->arData, (uint32_t) h | table->nTableMask);
	while (index != HT_INVALID_IDX) {
		Bucket *bucket = HT_HASH_TO_BUCKET_EX(table->arData, index);

		if (bucket->key == name) {
			return zend_native_probe_element(&bucket->val);
		}
		if (bucket->h == h && bucket->key != NULL
				&& ZSTR_LEN(bucket->key) == length) {
			const unsigned char *left =
				(const unsigned char *) ZSTR_VAL(bucket->key);
			const unsigned char *right =
				(const unsigned char *) ZSTR_VAL(name);
			size_t offset = 0;

			while (offset < length && left[offset] == right[offset]) {
				offset++;
			}
			if (offset == length) {
				return zend_native_probe_element(&bucket->val);
			}
		}
		index = Z_NEXT(bucket->val);
	}
	return ZEND_NATIVE_ELEMENT_ABSENT;
}

/*
 * Whether consuming a temporary container only drops a reference: an
 * uncounted value, or a counted one with another owner. A sole owner would
 * destroy the container, which the helper does.
 */
uint64_t zend_native_container_shared(const zval *container)
{
	return !Z_REFCOUNTED_P(container) || Z_REFCOUNT_P(container) > 1;
}

/* Drop one reference of a shared value (see zend_native_container_shared). */
void zend_native_release_shared(const zval *container)
{
	if (Z_REFCOUNTED_P(container)) {
		ZEND_NATIVE_DELREF(Z_COUNTED_P(container));
	}
}

/* ZVAL_COPY_DEREF as two machine words. */
zend_native_boxed zend_native_zval_copy_deref(const zval *value)
{
	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	return zend_native_zval_copy(value);
}

/* isset() of an element: set and not null, looking through a reference. */
uint64_t zend_native_zval_isset(const zval *value)
{
	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	return Z_TYPE_P(value) > IS_NULL;
}

/*
 * empty() of an element: 1 when empty, 0 when not, ZEND_NATIVE_EMPTY_UNKNOWN
 * when deciding needs the helper (an object's cast or count handler, or
 * another type).
 */
uint64_t zend_native_zval_empty(const zval *value)
{
	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	switch (Z_TYPE_P(value)) {
		case IS_UNDEF:
		case IS_NULL:
		case IS_FALSE:
			return 1;
		case IS_TRUE:
			return 0;
		case IS_LONG:
			return Z_LVAL_P(value) == 0;
		case IS_DOUBLE:
			return Z_DVAL_P(value) == 0.0;
		case IS_STRING:
			return Z_STRLEN_P(value) == 0
				|| (Z_STRLEN_P(value) == 1 && Z_STRVAL_P(value)[0] == '0');
		case IS_ARRAY:
			return zend_hash_num_elements(Z_ARRVAL_P(value)) == 0;
		default:
			return ZEND_NATIVE_EMPTY_UNKNOWN;
	}
}
