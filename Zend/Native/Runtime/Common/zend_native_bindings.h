/* Source-backed dynamic binding operations for generated native code. */

#ifndef ZEND_NATIVE_BINDINGS_H
#define ZEND_NATIVE_BINDINGS_H

#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS \
	zend_execute_data *execute_data, \
	uint64_t op1, uint64_t op2, uint64_t result, uint64_t auxiliary, \
	uint32_t extended_value, uint32_t source_opcode, \
	uint32_t source_position_id

zend_native_status zend_native_dynamic_fetch_r(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_w(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_rw(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_is(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_func_arg(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_unset(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_unset_var(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_isset_isempty_var(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_bind_global(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_globals(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_fetch_constant(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_declare_constant(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);
zend_native_status zend_native_dynamic_declare_attributed_constant(
	ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS);

#undef ZEND_NATIVE_DYNAMIC_EXPLICIT_ARGS

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_BINDINGS_H */
