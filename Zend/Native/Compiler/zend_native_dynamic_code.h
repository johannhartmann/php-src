#ifndef ZEND_NATIVE_DYNAMIC_CODE_H
#define ZEND_NATIVE_DYNAMIC_CODE_H

#include "Zend/zend_compile.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

/*
 * The compiler takes ownership of each dynamic op_array before resolving it
 * through the active product reentry scope. A successful resolution is
 * atomically published in this request-local dynamic registry.
 */

typedef struct _zend_native_dynamic_entry {
	zend_op_array *op_array;
	zend_native_entry_cell *entry_cell;
} zend_native_dynamic_entry;

typedef struct _zend_native_dynamic_compiler {
	struct _zend_native_compiler *product_compiler;
	zend_op_array **owned_op_arrays;
	uint32_t owned_op_array_count;
	uint32_t owned_op_array_capacity;
	zend_native_dynamic_entry *entries;
	HashTable entries_by_op_array;
	uint32_t entry_count;
	uint32_t entry_capacity;
} zend_native_dynamic_compiler;

/*
 * The active compiler is request-/TSRM-local. It is installed only while a
 * native execution tree is active, so independent ZTS requests never share a
 * compile-on-demand lock or partially published unit.
 */
ZEND_API void zend_native_dynamic_compiler_init(
	zend_native_dynamic_compiler *compiler);
ZEND_API void zend_native_dynamic_compiler_bind_product(
	zend_native_dynamic_compiler *compiler,
	struct _zend_native_compiler *product_compiler);
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);

#endif /* ZEND_NATIVE_DYNAMIC_CODE_H */
