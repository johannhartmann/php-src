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

/* Snippets may not call: shared logic is always inlined. */
#define ZEND_NATIVE_SNIPPET_INLINE static inline __attribute__((always_inline))
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

ZEND_NATIVE_SNIPPET_INLINE const HashTable *zend_native_probe_array(
	const zval *container)
{
	if (Z_TYPE_P(container) == IS_REFERENCE) {
		container = &Z_REF_P(container)->val;
	}
	return Z_TYPE_P(container) == IS_ARRAY ? Z_ARRVAL_P(container) : NULL;
}

ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_probe_element(zval *element)
{
	return Z_TYPE_P(element) == IS_INDIRECT
		? ZEND_NATIVE_ELEMENT_UNKNOWN : (uintptr_t) element;
}

ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_index(
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
 * The element under an interned non-numeric literal key: identity with the
 * bucket key decides; a bucket with the same hash but another key string is
 * left to the helper.
 */
ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_literal_string(
	const HashTable *table, const zend_string *name)
{
	zend_ulong h = ZSTR_H(name);
	uint32_t index;

	if (h == 0) {
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
 * The element under a runtime string key: an interned key (one that cannot
 * be numeric, starting with neither a digit nor '-') is decided by identity
 * like a literal. A key of another string, whose equal-content bucket key
 * would need a byte comparison, is left to the helper, as are keys without
 * their hash.
 */
ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_string(
	const HashTable *table, const zend_string *name)
{
	unsigned char first;

	if (!ZSTR_IS_INTERNED(name) || ZSTR_LEN(name) == 0) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	first = (unsigned char) ZSTR_VAL(name)[0];
	if ((first >= '0' && first <= '9') || first == '-') {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	return zend_native_find_literal_string(table, name);
}

ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_value(
	const HashTable *table, uint64_t payload, uint32_t type, bool literal)
{
	if (table == NULL) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	if (type == IS_LONG) {
		return zend_native_find_index(table, (zend_ulong) payload);
	}
	if (type != IS_STRING) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	return literal
		? zend_native_find_literal_string(
			table, (const zend_string *) (uintptr_t) payload)
		: zend_native_find_string(
			table, (const zend_string *) (uintptr_t) payload);
}

/*
 * The element under a literal key of a container zval. The compiler turned
 * numeric string literals into integers.
 */
uintptr_t zend_native_array_find_literal(
	const zval *container, const zval *key)
{
	return zend_native_find_value(zend_native_probe_array(container),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), true);
}

/* The element under a runtime key of a container zval (see above). */
uintptr_t zend_native_array_find_key(const zval *container, const zval *key)
{
	if (Z_TYPE_P(key) == IS_REFERENCE) {
		key = &Z_REF_P(key)->val;
	}
	return zend_native_find_value(zend_native_probe_array(container),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), false);
}

/*
 * The array a write fetch (FETCH_DIM_W, RW, UNSET) of a CV may change in
 * place: one, through a reference, with no other owner, which SEPARATE_ARRAY
 * leaves as it is. NULL otherwise.
 */
ZEND_NATIVE_SNIPPET_INLINE const HashTable *zend_native_probe_array_w(
	const zval *container)
{
	if (Z_TYPE_P(container) == IS_REFERENCE) {
		container = &Z_REF_P(container)->val;
	}
	return Z_TYPE_P(container) == IS_ARRAY
			&& GC_REFCOUNT(Z_ARRVAL_P(container)) == 1
		? Z_ARRVAL_P(container) : NULL;
}

/*
 * The same for a VAR container, which a write fetch may change only through
 * the INDIRECT of a previous fetch: a VAR that owns its value is released by
 * the fetch.
 */
ZEND_NATIVE_SNIPPET_INLINE const HashTable *zend_native_probe_indirect_w(
	const zval *var)
{
	return Z_TYPE_P(var) == IS_INDIRECT
		? zend_native_probe_array_w(Z_INDIRECT_P(var)) : NULL;
}

ZEND_NATIVE_SNIPPET_INLINE const zval *zend_native_key_deref(const zval *key)
{
	return Z_TYPE_P(key) == IS_REFERENCE ? &Z_REF_P(key)->val : key;
}

/* The element a write fetch of a CV under a literal key returns. */
uintptr_t zend_native_array_find_literal_w(
	const zval *container, const zval *key)
{
	return zend_native_find_value(zend_native_probe_array_w(container),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), true);
}

/* The element a write fetch of a CV under a runtime key returns. */
uintptr_t zend_native_array_find_key_w(
	const zval *container, const zval *key)
{
	key = zend_native_key_deref(key);
	return zend_native_find_value(zend_native_probe_array_w(container),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), false);
}

/* The element a write fetch of a VAR under a literal key returns. */
uintptr_t zend_native_indirect_find_literal_w(
	const zval *var, const zval *key)
{
	return zend_native_find_value(zend_native_probe_indirect_w(var),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), true);
}

/* The element a write fetch of a VAR under a runtime key returns. */
uintptr_t zend_native_indirect_find_key_w(const zval *var, const zval *key)
{
	key = zend_native_key_deref(key);
	return zend_native_find_value(zend_native_probe_indirect_w(var),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), false);
}

/* The array of a container zval, through a reference, or NULL. */
const HashTable *zend_native_zval_table(const zval *container)
{
	return zend_native_probe_array(container);
}

/* The array of a boxed value, or NULL. */
const HashTable *zend_native_boxed_table(uint64_t payload, uint64_t type_info)
{
	return (uint8_t) type_info == IS_ARRAY
		? (const HashTable *) (uintptr_t) payload : NULL;
}

/* The element of an array under an integer, string or boxed key value. */
uintptr_t zend_native_table_find_long(const HashTable *table, int64_t key)
{
	return zend_native_find_index(table, (zend_ulong) key);
}

uintptr_t zend_native_table_find_string(
	const HashTable *table, const zend_string *key)
{
	return zend_native_find_string(table, key);
}

uintptr_t zend_native_table_find_boxed(
	const HashTable *table, uint64_t payload, uint64_t type_info)
{
	return zend_native_find_value(
		table, payload, (uint8_t) type_info, false);
}

/* Whether a zval holds exactly an integer. */
uint64_t zend_native_zval_is_long(const zval *value)
{
	return Z_TYPE_INFO_P(value) == IS_LONG;
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

/*
 * TYPE_CHECK of a CV: 1 when its value, through a reference, has a type in
 * mask, 0 when not, ZEND_NATIVE_TYPE_CHECK_UNDEFINED for an undefined one.
 */
uint64_t zend_native_zval_type_check(const zval *value, uint64_t mask)
{
	if (Z_TYPE_P(value) == IS_UNDEF) {
		return ZEND_NATIVE_TYPE_CHECK_UNDEFINED;
	}
	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	return (mask >> Z_TYPE_P(value)) & 1;
}

/*
 * 1 when a container, through a reference, is undefined, null, a bool or a
 * number: FETCH_DIM_IS of it is null without a diagnostic.
 */
uint64_t zend_native_zval_is_scalar(const zval *container)
{
	if (Z_TYPE_P(container) == IS_REFERENCE) {
		container = &Z_REF_P(container)->val;
	}
	return Z_TYPE_P(container) <= IS_DOUBLE;
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
