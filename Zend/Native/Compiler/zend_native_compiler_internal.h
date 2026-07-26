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

/*
 * Keep one compiler's reentry and dynamic-code scopes active across warm
 * executor entries. The executor switches this request-local scope only when
 * control moves to another code generation.
 */
zend_result zend_native_compiler_activate_session(
	zend_native_compiler *compiler);
void zend_native_compiler_deactivate_session(
	zend_native_compiler *compiler);

#endif /* ZEND_NATIVE_COMPILER_INTERNAL_H */
