/* Source-backed zval and reference operations for generated native code. */

#ifndef ZEND_NATIVE_VALUES_H
#define ZEND_NATIVE_VALUES_H

#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Materialize a dereferenced request-local owner, duplicating persistent
 * arrays and strings.  target may equal source only when the zval is a
 * non-reference shallow transfer whose additional logical owner is not yet
 * reflected in its refcount.
 */
void zend_native_zval_copy_deref_or_dup(zval *target, const zval *source);

zend_native_status zend_native_value_make_ref(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_assign_ref(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_separate(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_copy_tmp(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_free(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_echo(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_func_num_args(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_func_get_args(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
#define ZEND_NATIVE_VALUE_HELPER(name) \
	zend_native_status name( \
		zend_execute_data *execute_data, \
		uint64_t op1, uint64_t op2, uint64_t result, \
		uint32_t extended_value, uint32_t source_opcode, \
		uint32_t source_position_id);

ZEND_NATIVE_VALUE_HELPER(zend_native_value_count)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_get_type)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_array_key_exists)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_in_array)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_isset_this)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_get_called_class)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_begin_silence)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_end_silence)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_match_error)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_verify_never_type)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_defined)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_ticks)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_type_assert)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_ext_stmt)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_ext_fcall_begin)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_ext_fcall_end)
ZEND_NATIVE_VALUE_HELPER(zend_native_value_ext_nop)

#undef ZEND_NATIVE_VALUE_HELPER
void zend_native_zval_store_integer(
	zval *slot, uint64_t payload, uint32_t exact_type);
void zend_native_zval_store_double(zval *slot, double value);
void zend_native_zval_release_slow(zval *slot);
zend_native_status zend_native_value_unset_cv(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_check_var(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_assign(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
/*
 * $cv .= value with the operands resolved to frame offsets: descriptor holds
 * the value kind (ZEND_NATIVE_CONCAT_DIRECT_*) in bits 0-1 and the source
 * position in bits 32-63; slots holds the CV offset (bits 0-31) and the value
 * offset or literal index (32-63). The result is unused.
 */
zend_native_status zend_native_value_identical_direct(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t result_offset);
zend_native_status zend_native_value_concat_direct(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t result_offset);
zend_native_status zend_native_value_concat_assign_direct(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots);
zend_native_status zend_native_value_assign_op(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_qm_assign(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_concat(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_fast_concat(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_rope_init(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_rope_add(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_rope_end(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
/*
 * The address forms of INIT_ARRAY and ADD_ARRAY_ELEMENT: the operands and
 * the result by address, their kinds, the source opcode and position in
 * the descriptor (see zend_native_value_init_array_address()).
 */
zend_native_status zend_native_value_init_array_address(
	zend_execute_data *execute_data, zval *op1, zval *op2, zval *result,
	uint32_t extended_value, uint64_t descriptor);
zend_native_status zend_native_value_add_array_element_address(
	zend_execute_data *execute_data, zval *op1, zval *op2, zval *result,
	uint32_t extended_value, uint64_t descriptor);
zend_native_status zend_native_value_init_array(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_add_array_element(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_add_array_unpack(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
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
zend_native_status zend_native_value_fetch_dim_r_direct(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t more_slots);
/* ASSIGN_DIM of a CV (or FETCH_OBJ_W VAR) container from addresses: the
 * container slot, the key (NULL to append) and the value, and the direct
 * form's descriptor (kinds, flags, source position). */
zend_native_status zend_native_value_assign_dim_address(
	zend_execute_data *execute_data, zval *container_slot, zval *key,
	zval *value, uint64_t descriptor);
/* The null element an assignment under integer key h inserts into the
 * unshared array of a CV container (possibly a reference) that has no
 * element there, as ZEND_ASSIGN_DIM's write fetch does. */
zval *zend_native_array_insert_index(zval *container, zend_ulong h);
zval *zend_native_array_assign_lookup(zval *container, const zval *key);
zend_native_status zend_native_value_isset_isempty_dim_direct(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t more_slots);
ZEND_NATIVE_EXPLICIT_DIM_ASSIGN_HELPER(zend_native_value_assign_dim_op)

#undef ZEND_NATIVE_EXPLICIT_DIM_ASSIGN_HELPER
zend_native_status zend_native_value_unset_dim(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_isset_isempty_dim(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_fe_free(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_binary_op(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_case(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_unary_op(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_type_check(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_verify_return_type(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_cast(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_isset_isempty_cv(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_fetch_list(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_status zend_native_value_incdec(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
void zend_native_incdec_property_zval(
	zend_execute_data *execute_data, zval *property,
	const struct _zend_property_info *property_info, zval *result,
	bool post, bool increment);

zend_native_iterator_branch_result zend_native_value_iterator_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_iterator_branch_result zend_native_value_cond_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_iterator_branch_result zend_native_value_bind_static_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);
zend_native_iterator_branch_result zend_native_value_frameless_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id);

#ifdef __cplusplus
}
#endif

#endif /* ZEND_NATIVE_VALUES_H */
