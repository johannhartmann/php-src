#ifndef ZEND_NATIVE_COMPILER_INTERNAL_H
#define ZEND_NATIVE_COMPILER_INTERNAL_H

#include "Zend/Native/Compiler/zend_native_compiler.h"

/*
 * Compile one dynamic Zend compilation instance as a single native
 * component. The bucket cursors are snapshots taken immediately before
 * zend_include_or_eval(); only symbols appended by that exact compilation
 * instance are adopted into the component.
 */
zend_result zend_native_compiler_compile_dynamic_component(
	zend_native_compiler *compiler,
	zend_op_array *root,
	uint32_t first_function_bucket,
	uint32_t first_class_bucket,
	zend_native_entry_cell **root_entry,
	zend_native_compile_diagnostic *diagnostic);

#endif /* ZEND_NATIVE_COMPILER_INTERNAL_H */
