#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/zend_compile.h"
#include "Zend/Optimizer/zend_dfg.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/zend_alloc.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"

#undef _emalloc
#undef _efree

void *ZEND_FASTCALL _emalloc(size_t size)
{
	return malloc(size);
}

void ZEND_FASTCALL _efree(void *pointer)
{
	free(pointer);
}

int ZEND_FASTCALL zend_binary_strcasecmp(
	const char *left, size_t left_length,
	const char *right, size_t right_length)
{
	size_t index;
	size_t length = left_length < right_length ? left_length : right_length;

	for (index = 0; index < length; index++) {
		unsigned char left_character = (unsigned char) left[index];
		unsigned char right_character = (unsigned char) right[index];

		if (left_character >= 'A' && left_character <= 'Z') {
			left_character = (unsigned char) (left_character + ('a' - 'A'));
		}
		if (right_character >= 'A' && right_character <= 'Z') {
			right_character =
				(unsigned char) (right_character + ('a' - 'A'));
		}
		if (left_character != right_character) {
			return (int) left_character - (int) right_character;
		}
	}
	return left_length == right_length ? 0
		: left_length < right_length ? -1 : 1;
}

bool zend_check_protected(
	const zend_class_entry *declaring_scope,
	const zend_class_entry *calling_scope)
{
	const zend_class_entry *scope;

	for (scope = declaring_scope; scope != NULL; scope = scope->parent) {
		if (scope == calling_scope) {
			return true;
		}
	}
	for (scope = calling_scope; scope != NULL; scope = scope->parent) {
		if (scope == declaring_scope) {
			return true;
		}
	}
	return false;
}

zend_class_entry *zend_optimizer_get_class_entry_from_op1(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_op *opline)
{
	uint32_t fetch_type;

	(void) script;
	if (op_array == NULL || opline == NULL || op_array->scope == NULL
			|| opline->op1_type != IS_UNUSED
			|| (op_array->scope->ce_flags & ZEND_ACC_TRAIT) != 0) {
		return NULL;
	}
	fetch_type = opline->op1.num & ZEND_FETCH_CLASS_MASK;
	if (fetch_type == ZEND_FETCH_CLASS_SELF
			|| (fetch_type == ZEND_FETCH_CLASS_STATIC
				&& (op_array->scope->ce_flags & ZEND_ACC_FINAL) != 0)) {
		return op_array->scope;
	}
	return NULL;
}

void zend_dfg_add_use_def_op(
	const zend_op_array *op_array, const zend_op *opline,
	uint32_t build_flags, zend_bitset use, zend_bitset def)
{
	uint32_t variable;

	(void) opline;
	(void) build_flags;
	(void) use;
	if (op_array == NULL || op_array->last_var > UINT32_MAX - op_array->T) {
		return;
	}
	/* The W03-only harness has no runtime class table. Conservatively treating
	 * every slot as redefined keeps W08 catch-receiver discovery fail-closed. */
	for (variable = 0; variable < op_array->last_var + op_array->T; variable++) {
		zend_bitset_incl(def, variable);
	}
}

#if defined(__GNUC__) && (defined(__i386__) || (defined(__x86_64__) && !defined(__ILP32__)))
bool ZEND_FASTCALL zend_string_equal_val(const zend_string *s1, const zend_string *s2)
{
	return memcmp(ZSTR_VAL(s1), ZSTR_VAL(s2), ZSTR_LEN(s1)) == 0;
}
#endif

zval *ZEND_FASTCALL zend_hash_find(const HashTable *ht, zend_string *key)
{
	(void) ht;
	(void) key;
	return NULL;
}

zend_function *zend_optimizer_get_called_func(
	const zend_script *script, const zend_op_array *op_array,
	zend_op *opline, bool *is_prototype)
{
	(void) script;
	(void) op_array;
	(void) opline;
	*is_prototype = false;
	return NULL;
}

const zend_class_constant *zend_fetch_class_const_info(
	const zend_script *script, const zend_op_array *op_array,
	const zend_op *opline, bool *is_prototype)
{
	(void) script;
	(void) op_array;
	(void) opline;
	*is_prototype = false;
	return NULL;
}

zend_function *zend_std_get_method(
	zend_object **object, zend_string *method, const zval *key)
{
	(void) object;
	(void) method;
	(void) key;
	return NULL;
}

zend_class_entry *zend_fetch_class_by_name(
	zend_string *class_name, zend_string *lcname, uint32_t fetch_type)
{
	(void) class_name;
	(void) lcname;
	(void) fetch_type;
	return NULL;
}
