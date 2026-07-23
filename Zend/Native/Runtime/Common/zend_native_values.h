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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_assign_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_qm_assign(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
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
#define ZEND_NATIVE_EXPLICIT_VALUE_HELPER(name) \
	zend_native_status name( \
		zend_execute_data *execute_data, \
		uint64_t op1, uint64_t op2, uint64_t result, \
		uint32_t extended_value, uint32_t source_opcode, \
		uint32_t source_position_id);

ZEND_NATIVE_EXPLICIT_VALUE_HELPER(zend_native_value_fetch_dim_r)
ZEND_NATIVE_EXPLICIT_VALUE_HELPER(zend_native_value_fetch_dim_w)
ZEND_NATIVE_EXPLICIT_VALUE_HELPER(zend_native_value_fetch_dim_rw)
ZEND_NATIVE_EXPLICIT_VALUE_HELPER(zend_native_value_fetch_dim_is)
ZEND_NATIVE_EXPLICIT_VALUE_HELPER(zend_native_value_fetch_dim_func_arg)
ZEND_NATIVE_EXPLICIT_VALUE_HELPER(zend_native_value_fetch_dim_unset)

#undef ZEND_NATIVE_EXPLICIT_VALUE_HELPER
#define ZEND_NATIVE_EXPLICIT_DIM_ASSIGN_HELPER(name) \
	zend_native_status name( \
		zend_execute_data *execute_data, \
		uint64_t op1, uint64_t op2, uint64_t result, uint64_t auxiliary, \
		uint32_t extended_value, uint32_t source_opcode, \
		uint32_t source_position_id);

ZEND_NATIVE_EXPLICIT_DIM_ASSIGN_HELPER(zend_native_value_assign_dim)
ZEND_NATIVE_EXPLICIT_DIM_ASSIGN_HELPER(zend_native_value_assign_dim_op)

#undef ZEND_NATIVE_EXPLICIT_DIM_ASSIGN_HELPER
zend_native_status zend_native_value_unset_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_isset_isempty_dim(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_fe_free(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_binary_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_unary_op(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_type_check(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_verify_return_type(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_cast(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_isset_isempty_cv(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_fetch_list(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_status zend_native_value_incdec(
	zend_execute_data *execute_data, uint32_t source_opline_index);

zend_native_iterator_branch_result zend_native_value_iterator_branch(
	zend_execute_data *execute_data, uint32_t source_opline_index);
zend_native_iterator_branch_result zend_native_value_cond_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_VALUES_H */
