/* Source-backed zval and reference operations for generated native code. */

#ifndef ZEND_NATIVE_VALUES_H
#define ZEND_NATIVE_VALUES_H

#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

zend_native_status zend_native_value_make_ref(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_assign_ref(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_separate(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_copy_tmp(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_free(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_unset_cv(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_check_var(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_assign(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_assign_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_qm_assign(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_concat(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fast_concat(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_rope_init(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_rope_add(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_rope_end(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_init_array(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_add_array_element(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_add_array_unpack(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_dim_r(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_dim_w(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_dim_rw(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_dim_is(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_dim_func_arg(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_dim_unset(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_assign_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_assign_dim_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_unset_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_isset_isempty_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fe_free(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_binary_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_unary_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_cast(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_isset_isempty_cv(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_fetch_list(
	zend_execute_data *execute_data, uint32_t source_opline_index);

zend_native_iterator_branch_result zend_native_value_iterator_branch(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_iterator_branch_result zend_native_value_cond_branch(
	zend_execute_data *execute_data, uint32_t source_opline_index);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_VALUES_H */
