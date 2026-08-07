#ifndef ZEND_NATIVE_COMPILER_INTERNAL_H
#define ZEND_NATIVE_COMPILER_INTERNAL_H

#include "Zend/Native/Compiler/zend_native_compiler.h"

typedef struct _zend_native_op_array_identity {
	const zend_op *opcodes;
	uint32_t fn_flags;
	uint32_t last;
	uint32_t last_var;
	uint32_t T;
	uint32_t last_literal;
	uint32_t num_args;
	uint32_t required_num_args;
	uint32_t line_start;
	uint32_t line_end;
} zend_native_op_array_identity;

static zend_always_inline void zend_native_op_array_identity_capture(
	zend_native_op_array_identity *identity,
	const zend_op_array *op_array)
{
	ZEND_ASSERT(identity != NULL);
	ZEND_ASSERT(op_array != NULL);
	identity->opcodes = op_array->opcodes;
	identity->fn_flags = op_array->fn_flags;
	identity->last = op_array->last;
	identity->last_var = op_array->last_var;
	identity->T = op_array->T;
	identity->last_literal = op_array->last_literal;
	identity->num_args = op_array->num_args;
	identity->required_num_args = op_array->required_num_args;
	identity->line_start = op_array->line_start;
	identity->line_end = op_array->line_end;
}

static zend_always_inline bool zend_native_op_array_identity_matches(
	const zend_native_op_array_identity *identity,
	const zend_op_array *op_array)
{
	return identity != NULL && op_array != NULL
		&& identity->opcodes == op_array->opcodes
		&& identity->fn_flags == op_array->fn_flags
		&& identity->last == op_array->last
		&& identity->last_var == op_array->last_var
		&& identity->T == op_array->T
		&& identity->last_literal == op_array->last_literal
		&& identity->num_args == op_array->num_args
		&& identity->required_num_args == op_array->required_num_args
		&& identity->line_start == op_array->line_start
		&& identity->line_end == op_array->line_end;
}

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
	zend_native_compiler **component_compiler,
	uint32_t *first_compiled_function,
	zend_native_entry_cell **root_entry,
	zend_native_compile_diagnostic *diagnostic);

/* Release lowering-only state for a completed dynamic component while its
 * published code and entry cells remain available for request-local reentry. */
void zend_native_compiler_release_ready_transients(
	zend_native_compiler *compiler, uint32_t first_function_index);

/* Retire a just-completed, request-local dynamic component before its READY
 * publications become visible to the executor indexes. The range must be the
 * current registry and publication tail, and every entry cell must be
 * quiescent. This keeps short-lived eval/include code native without retaining
 * its code, source lease or stable entry cells until request shutdown. */
bool zend_native_compiler_retire_dynamic_component(
	zend_native_compiler *compiler, uint32_t first_function_index,
	const zend_op_array *root);

/*
 * Keep one compiler's reentry and dynamic-code scopes active across warm
 * executor entries. The executor switches this request-local scope only when
 * control moves to another code generation.
 */
zend_result zend_native_compiler_activate_session(
	zend_native_compiler *compiler);
void zend_native_compiler_deactivate_session(
	zend_native_compiler *compiler);

/* The executor owns the storage classification for published OPcache
 * bundles. Dynamic execution uses it to avoid mutating cache-owned op arrays. */
bool zend_native_executor_op_array_is_cache_owned(
	const zend_op_array *op_array);

/*
 * Snapshot the append-only READY-publication log beginning at first_index.
 * No allocation or callback occurs while the compiler mutation lock is held.
 * On insufficient capacity, FAILURE is returned and ready_count reports the
 * needed size. Atomic component publication appends only after every sibling
 * entry cell is ready, so cursors cannot skip or observe a partial component.
 */
zend_result zend_native_compiler_snapshot_publication_delta(
	const zend_native_compiler *compiler, uint32_t first_index,
	zend_op_array **op_arrays, uint32_t capacity, uint32_t *ready_count,
	uint32_t *next_index);

#endif /* ZEND_NATIVE_COMPILER_INTERNAL_H */
