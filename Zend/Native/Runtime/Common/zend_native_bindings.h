/* Source-backed dynamic binding operations for generated native code. */

#ifndef ZEND_NATIVE_BINDINGS_H
#define ZEND_NATIVE_BINDINGS_H

#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

zend_native_status zend_native_dynamic_fetch_r(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_w(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_rw(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_is(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_func_arg(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_unset(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_unset_var(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_isset_isempty_var(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_bind_global(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_globals(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_fetch_constant(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_declare_constant(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_dynamic_declare_attributed_constant(
	zend_execute_data *execute_data, uint32_t source_opline_index);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_BINDINGS_H */
