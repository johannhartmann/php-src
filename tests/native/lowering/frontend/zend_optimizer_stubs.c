#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/zend_compile.h"

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

zend_function *zend_std_get_method(
	zend_object **object, zend_string *method, const zval *key)
{
	(void) object;
	(void) method;
	(void) key;
	return NULL;
}
