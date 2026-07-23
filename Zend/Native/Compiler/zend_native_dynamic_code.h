#ifndef ZEND_NATIVE_DYNAMIC_CODE_H
#define ZEND_NATIVE_DYNAMIC_CODE_H

#include "Zend/zend_compile.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

typedef zend_native_status (*zend_native_dynamic_compile_execute_t)(
	void *context, zend_op_array *op_array, zend_execute_data *execute_data);

/*
 * compile_execute takes ownership of op_array on every invocation. It must
 * release the complete codeunit only after its native entries and all
 * non-suspended frames and closures have released their owner references.
 */

typedef struct _zend_native_dynamic_compiler {
	void *context;
	zend_native_dynamic_compile_execute_t compile_execute;
	zend_op_array **owned_op_arrays;
	uint32_t owned_op_array_count;
	uint32_t owned_op_array_capacity;
} zend_native_dynamic_compiler;

/*
 * The active compiler is request-/TSRM-local. It is installed only while a
 * native execution tree is active, so independent ZTS requests never share a
 * compile-on-demand lock or partially published unit.
 */
ZEND_API void zend_native_dynamic_compiler_init(
	zend_native_dynamic_compiler *compiler,
	void *context,
	zend_native_dynamic_compile_execute_t compile_execute);
ZEND_API void zend_native_dynamic_compiler_destroy(
	zend_native_dynamic_compiler *compiler);
ZEND_API void zend_native_dynamic_compiler_activate(
	zend_native_dynamic_compiler *compiler);
ZEND_API void zend_native_dynamic_compiler_deactivate(
	const zend_native_dynamic_compiler *compiler);

ZEND_API zend_native_status zend_native_execute_include_or_eval(
	zend_execute_data *execute_data, uint32_t source_opline_index);

#endif /* ZEND_NATIVE_DYNAMIC_CODE_H */
