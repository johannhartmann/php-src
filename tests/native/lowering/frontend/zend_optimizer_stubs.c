#include <stdlib.h>

#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/zend_compile.h"

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
