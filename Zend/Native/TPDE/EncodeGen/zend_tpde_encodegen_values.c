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
	if (Z_TYPE_P(container) != IS_ARRAY) {
		return NULL;
	}
	/* An array zval never holds a NULL table: the NULL above alone means
	 * "not an array", without a second test of the table. */
	__builtin_assume(Z_ARRVAL_P(container) != NULL);
	return Z_ARRVAL_P(container);
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
 * Whether two key strings of the same hash and a length of at most 16 bytes
 * hold the same bytes: one or two 8-byte words each; below 8 bytes the
 * lowest differing byte of the first words must lie past the key.
 * A zend_string's value starts 8-byte aligned and its allocation is rounded
 * up to 8 bytes, so the words stay inside both strings.
 */
ZEND_NATIVE_SNIPPET_INLINE bool zend_native_short_key_equal(
	const zend_string *left, const zend_string *right, size_t length)
{
	const char *a = ZSTR_VAL(left);
	const char *b = ZSTR_VAL(right);
	const uint64_t first = *(const uint64_t *) (const void *) a
		^ *(const uint64_t *) (const void *) b;

	if (length < 8) {
		/* The lowest differing byte must lie past the key. */
		return first == 0
			|| (size_t) __builtin_ctzll(first) >= length * 8;
	}
	return first == 0
		&& *(const uint64_t *) (const void *) (a + length - 8)
			== *(const uint64_t *) (const void *) (b + length - 8);
}

/*
 * The element under a non-numeric string key: identity with the bucket key
 * decides, and a bucket key of the same hash and length up to 16 bytes is
 * compared by content; a longer one is left to the helper.
 */
/* The same with the key's hash known, which is never zero. */
ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_string_h(
	const HashTable *table, const zend_string *name, zend_ulong h)
{
	uint32_t index;

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
			/* A true collision of the full hash is left to the helper. */
			return ZSTR_LEN(bucket->key) == ZSTR_LEN(name)
					&& ZSTR_LEN(name) <= 16
					&& zend_native_short_key_equal(
						bucket->key, name, ZSTR_LEN(name))
				? zend_native_probe_element(&bucket->val)
				: ZEND_NATIVE_ELEMENT_UNKNOWN;
		}
		index = Z_NEXT(bucket->val);
	}
	return ZEND_NATIVE_ELEMENT_ABSENT;
}

ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_literal_string(
	const HashTable *table, const zend_string *name)
{
	zend_ulong h = ZSTR_H(name);

	if (h == 0) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	return zend_native_find_string_h(table, name, h);
}

/*
 * The element under a runtime string key that cannot be numeric (starting
 * with neither a digit nor '-') is decided like a literal's, interned or
 * not; keys without their hash are left to the helper.
 */
ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_find_string(
	const HashTable *table, const zend_string *name)
{
	unsigned char first;

	if (ZSTR_LEN(name) == 0) {
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
 * The element isset(), empty() and ?? test: as zend_native_array_find_*(),
 * but an undefined or null container, through a reference, has no element
 * (FETCH_DIM_IS and ISSET_ISEMPTY_DIM_OBJ answer it without a notice about
 * the container; an undefined key is still reported).
 */
ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_test_value(
	const zval *container, const zval *key, bool literal)
{
	if (Z_TYPE_P(container) == IS_REFERENCE) {
		container = &Z_REF_P(container)->val;
	}
	if (Z_TYPE_P(container) <= IS_NULL) {
		/* An undefined key CV still warns in the helper. */
		return Z_TYPE_P(key) == IS_UNDEF
			? ZEND_NATIVE_ELEMENT_UNKNOWN : ZEND_NATIVE_ELEMENT_ABSENT;
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	__builtin_assume(Z_ARRVAL_P(container) != NULL);
	return zend_native_find_value(Z_ARRVAL_P(container),
		(uint64_t) Z_LVAL_P(key), Z_TYPE_P(key), literal);
}

uintptr_t zend_native_array_test_literal(
	const zval *container, const zval *key)
{
	return zend_native_test_value(container, key, true);
}

uintptr_t zend_native_array_test_key(const zval *container, const zval *key)
{
	if (Z_TYPE_P(key) == IS_REFERENCE) {
		key = &Z_REF_P(key)->val;
	}
	return zend_native_test_value(container, key, false);
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
	if (Z_TYPE_P(container) != IS_ARRAY
			|| GC_REFCOUNT(Z_ARRVAL_P(container)) != 1) {
		return NULL;
	}
	__builtin_assume(Z_ARRVAL_P(container) != NULL);
	return Z_ARRVAL_P(container);
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

/*
 * The lookups of the forms above for a literal key whose kind the compiler
 * read: a non-numeric string with its (non-zero) hash, or an integer. The
 * key is never undefined.
 */
ZEND_NATIVE_SNIPPET_INLINE uintptr_t zend_native_test_table(
	const zval *container, const HashTable **table)
{
	if (Z_TYPE_P(container) == IS_REFERENCE) {
		container = &Z_REF_P(container)->val;
	}
	if (Z_TYPE_P(container) <= IS_NULL) {
		return ZEND_NATIVE_ELEMENT_ABSENT;
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		return ZEND_NATIVE_ELEMENT_UNKNOWN;
	}
	__builtin_assume(Z_ARRVAL_P(container) != NULL);
	*table = Z_ARRVAL_P(container);
	return 0;
}

uintptr_t zend_native_array_find_str(
	const zval *container, const zend_string *name, uint64_t h)
{
	const HashTable *table = zend_native_probe_array(container);

	return table != NULL ? zend_native_find_string_h(table, name, h)
		: ZEND_NATIVE_ELEMENT_UNKNOWN;
}

uintptr_t zend_native_array_find_idx(const zval *container, uint64_t h)
{
	const HashTable *table = zend_native_probe_array(container);

	return table != NULL ? zend_native_find_index(table, h)
		: ZEND_NATIVE_ELEMENT_UNKNOWN;
}

uintptr_t zend_native_array_test_str(
	const zval *container, const zend_string *name, uint64_t h)
{
	const HashTable *table = NULL;
	uintptr_t answer = zend_native_test_table(container, &table);

	return table != NULL ? zend_native_find_string_h(table, name, h) : answer;
}

uintptr_t zend_native_array_test_idx(const zval *container, uint64_t h)
{
	const HashTable *table = NULL;
	uintptr_t answer = zend_native_test_table(container, &table);

	return table != NULL ? zend_native_find_index(table, h) : answer;
}

uintptr_t zend_native_array_find_str_w(
	const zval *container, const zend_string *name, uint64_t h)
{
	const HashTable *table = zend_native_probe_array_w(container);

	return table != NULL ? zend_native_find_string_h(table, name, h)
		: ZEND_NATIVE_ELEMENT_UNKNOWN;
}

uintptr_t zend_native_array_find_idx_w(const zval *container, uint64_t h)
{
	const HashTable *table = zend_native_probe_array_w(container);

	return table != NULL ? zend_native_find_index(table, h)
		: ZEND_NATIVE_ELEMENT_UNKNOWN;
}

uintptr_t zend_native_indirect_find_str_w(
	const zval *var, const zend_string *name, uint64_t h)
{
	const HashTable *table = zend_native_probe_indirect_w(var);

	return table != NULL ? zend_native_find_string_h(table, name, h)
		: ZEND_NATIVE_ELEMENT_UNKNOWN;
}

uintptr_t zend_native_indirect_find_idx_w(const zval *var, uint64_t h)
{
	const HashTable *table = zend_native_probe_indirect_w(var);

	return table != NULL ? zend_native_find_index(table, h)
		: ZEND_NATIVE_ELEMENT_UNKNOWN;
}

/*
 * isset() of an element in one step: 1 when set, 0 when absent or null,
 * ZEND_NATIVE_ISSET_UNKNOWN when the helper decides (see
 * zend_native_array_test_*()).
 */
ZEND_NATIVE_SNIPPET_INLINE uint64_t zend_native_element_isset(
	uintptr_t element)
{
	const zval *value = (const zval *) element;

	if (element == ZEND_NATIVE_ELEMENT_UNKNOWN) {
		return ZEND_NATIVE_ISSET_UNKNOWN;
	}
	if (element == ZEND_NATIVE_ELEMENT_ABSENT) {
		return 0;
	}
	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	return Z_TYPE_P(value) > IS_NULL;
}

uint64_t zend_native_array_isset_key(const zval *container, const zval *key)
{
	return zend_native_element_isset(
		zend_native_test_value(container, zend_native_key_deref(key), false));
}

uint64_t zend_native_array_isset_literal(
	const zval *container, const zval *key)
{
	return zend_native_element_isset(
		zend_native_test_value(container, key, true));
}

uint64_t zend_native_array_isset_str(
	const zval *container, const zend_string *name, uint64_t h)
{
	const HashTable *table = NULL;
	uintptr_t answer = zend_native_test_table(container, &table);

	return zend_native_element_isset(table != NULL
		? zend_native_find_string_h(table, name, h) : answer);
}

uint64_t zend_native_array_isset_idx(const zval *container, uint64_t h)
{
	const HashTable *table = NULL;
	uintptr_t answer = zend_native_test_table(container, &table);

	return zend_native_element_isset(table != NULL
		? zend_native_find_index(table, h) : answer);
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
 * 1 when an assignment may overwrite a CV without the runtime: it holds no
 * reference, and its old value is not counted, or has another owner and
 * needs no new GC root (GC_MAY_LEAK is false, as for strings). The caller
 * then drops one reference itself.
 */
uint64_t zend_native_cv_overwritable(const zval *variable)
{
	if (!Z_REFCOUNTED_P(variable)) {
		return 1;
	}
	if (Z_TYPE_P(variable) == IS_REFERENCE || Z_REFCOUNT_P(variable) == 1) {
		return 0;
	}
	return !GC_MAY_LEAK(Z_COUNTED_P(variable));
}

/*
 * zend_is_identical() of a value, through a reference, and a literal: 1 or
 * 0 for null, bools, integers and strings, ZEND_NATIVE_IDENTICAL_UNKNOWN for
 * an undefined value, doubles (signed zeros, NaN), arrays and objects.
 */
uint64_t zend_native_zval_identical(const zval *value, const zval *literal)
{
	const char *left;
	const char *right;
	size_t length;

	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	if (Z_TYPE_P(value) == IS_UNDEF) {
		return ZEND_NATIVE_IDENTICAL_UNKNOWN;
	}
	if (Z_TYPE_P(value) != Z_TYPE_P(literal)) {
		return 0;
	}
	if (Z_TYPE_P(value) <= IS_TRUE) {
		return 1;
	}
	if (Z_TYPE_P(value) == IS_LONG) {
		return Z_LVAL_P(value) == Z_LVAL_P(literal);
	}
	if (Z_TYPE_P(value) != IS_STRING) {
		return ZEND_NATIVE_IDENTICAL_UNKNOWN;
	}
	if (Z_STR_P(value) == Z_STR_P(literal)) {
		return 1;
	}
	length = Z_STRLEN_P(value);
	if (length != Z_STRLEN_P(literal)) {
		return 0;
	}
	/* Word by word, then the last bytes; no library call and few
	 * registers: two cursors and the remaining length. */
	left = Z_STRVAL_P(value);
	right = Z_STRVAL_P(literal);
	for (; length >= sizeof(uint64_t); length -= sizeof(uint64_t)) {
		if (*(const uint64_t *) left != *(const uint64_t *) right) {
			return 0;
		}
		left += sizeof(uint64_t);
		right += sizeof(uint64_t);
	}
	for (; length != 0; length--) {
		if (*left++ != *right++) {
			return 0;
		}
	}
	return 1;
}

/*
 * count() of an array, through a reference, or ZEND_NATIVE_COUNT_UNKNOWN
 * for any other value (Countable objects, warnings and errors).
 */
uint64_t zend_native_zval_array_count(const zval *value)
{
	if (Z_TYPE_P(value) == IS_REFERENCE) {
		value = &Z_REF_P(value)->val;
	}
	return Z_TYPE_P(value) == IS_ARRAY
		? zend_hash_num_elements(Z_ARRVAL_P(value))
		: ZEND_NATIVE_COUNT_UNKNOWN;
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

/*
 * 1 when FE_FREE of a foreach holder is one dropped reference: an array
 * that another owner keeps alive. Object iterators and a last reference
 * need the runtime.
 */
uint64_t zend_native_iterator_shared(const zval *holder)
{
	return Z_TYPE_P(holder) == IS_ARRAY
		&& (!Z_REFCOUNTED_P(holder) || Z_REFCOUNT_P(holder) > 1);
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
