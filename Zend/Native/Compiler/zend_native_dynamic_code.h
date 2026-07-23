#ifndef ZEND_NATIVE_DYNAMIC_CODE_H
#define ZEND_NATIVE_DYNAMIC_CODE_H

#include "Zend/zend_compile.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

typedef zend_result (*zend_native_dynamic_compile_t)(
	void *context, zend_op_array *op_array);

/*
 * The compiler takes ownership of each dynamic op_array before invoking
 * compile. A successful compile must atomically publish its entry through
 * zend_native_dynamic_compiler_publish().
 */

typedef struct _zend_native_dynamic_entry {
	zend_op_array *op_array;
	zend_native_entry_cell *entry_cell;
} zend_native_dynamic_entry;

typedef struct _zend_native_dynamic_compiler {
	void *context;
	zend_native_dynamic_compile_t compile;
	zend_op_array **owned_op_arrays;
	uint32_t owned_op_array_count;
	uint32_t owned_op_array_capacity;
	zend_native_dynamic_entry *entries;
	uint32_t entry_count;
	uint32_t entry_capacity;
} zend_native_dynamic_compiler;

/*
 * The active compiler is request-/TSRM-local. It is installed only while a
 * native execution tree is active, so independent ZTS requests never share a
 * compile-on-demand lock or partially published unit.
 */
ZEND_API void zend_native_dynamic_compiler_init(
	zend_native_dynamic_compiler *compiler,
	void *context,
	zend_native_dynamic_compile_t compile);
ZEND_API void zend_native_dynamic_compiler_destroy(
	zend_native_dynamic_compiler *compiler);
ZEND_API void zend_native_dynamic_compiler_activate(
	zend_native_dynamic_compiler *compiler);
ZEND_API void zend_native_dynamic_compiler_deactivate(
	const zend_native_dynamic_compiler *compiler);
ZEND_API zend_result zend_native_dynamic_compiler_publish(
	zend_native_dynamic_compiler *compiler,
	zend_op_array *op_array,
	zend_native_entry_cell *entry_cell);
ZEND_API zend_native_entry_cell *zend_native_dynamic_compiler_lookup(
	const zend_native_dynamic_compiler *compiler,
	const zend_op_array *op_array);

ZEND_API zend_native_status zend_native_execute_include_or_eval(
	zend_execute_data *execute_data, uint32_t source_opline_index);

#endif /* ZEND_NATIVE_DYNAMIC_CODE_H */
