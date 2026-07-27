// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"
#include "Zend/Native/MIR/zend_mir_call.h"
#include "Zend/Native/MIR/zend_mir_values.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_execute.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct _zend_native_direct_call_descriptor;
struct _zend_native_direct_internal_call_descriptor;
struct _zend_native_user_call_descriptor;

extern "C" zend_mir_opcode zend_mir_w12_executable_opcode(uint32_t opcode);

enum zend_tpde_machine_value_kind : uint8_t {
	ZEND_TPDE_MACHINE_VALUE_I64 = 0,
	ZEND_TPDE_MACHINE_VALUE_F64 = 1,
	ZEND_TPDE_MACHINE_VALUE_BOOL = 2,
	ZEND_TPDE_MACHINE_VALUE_STRING_PTR = 3,
	ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR = 4,
	ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR = 5,
	ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR = 6,
	ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL = 7,
};

enum zend_tpde_machine_location : uint8_t {
	ZEND_TPDE_MACHINE_LOCATION_REGISTER = 0,
	ZEND_TPDE_MACHINE_LOCATION_SPILL = 1,
	ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT = 2,
};

enum zend_tpde_canonical_slot_state : uint8_t {
	ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED = 0,
	ZEND_TPDE_CANONICAL_SLOT_CLEAN = 1,
	ZEND_TPDE_CANONICAL_SLOT_DIRTY = 2,
};

static inline uint32_t zend_tpde_machine_value_zval_type(
	zend_tpde_machine_value_kind kind)
{
	switch (kind) {
		case ZEND_TPDE_MACHINE_VALUE_BOOL:
			return IS_FALSE;
		case ZEND_TPDE_MACHINE_VALUE_I64:
			return IS_LONG;
		case ZEND_TPDE_MACHINE_VALUE_F64:
			return IS_DOUBLE;
		case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
			return IS_STRING;
		case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
			return IS_ARRAY;
		case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
			return IS_OBJECT;
		case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
			return IS_REFERENCE;
		case ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL:
			return IS_UNDEF;
	}
	return IS_UNDEF;
}

static inline uint32_t zend_tpde_machine_value_zval_type_info(
	zend_tpde_machine_value_kind kind)
{
	switch (kind) {
		case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
			return IS_STRING_EX;
		case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
			return IS_ARRAY_EX;
		case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
			return IS_OBJECT_EX;
		case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
			return IS_REFERENCE_EX;
		default:
			return zend_tpde_machine_value_zval_type(kind);
	}
}

enum zend_tpde_user_opcode_target_kind : uint8_t {
	ZEND_TPDE_USER_OPCODE_TARGET_VALUE = 0,
	ZEND_TPDE_USER_OPCODE_TARGET_JUMP_OP1 = 1,
	ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2 = 2,
	ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_OP2 = 3,
	ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_EXTENDED = 4,
	ZEND_TPDE_USER_OPCODE_TARGET_RETURN = 5,
	ZEND_TPDE_USER_OPCODE_TARGET_THROW = 6,
	ZEND_TPDE_USER_OPCODE_TARGET_MULTI_BRANCH = 7,
	ZEND_TPDE_USER_OPCODE_TARGET_NOP = 8,
	ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_CALL = 9,
	ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_RETURN = 10,
	ZEND_TPDE_USER_OPCODE_TARGET_CATCH = 11,
	ZEND_TPDE_USER_OPCODE_TARGET_RECEIVE = 12,
	ZEND_TPDE_USER_OPCODE_TARGET_CALL_FRAGMENT = 13,
};

static inline uint32_t zend_tpde_user_opcode_target_frame_uses(
	zend_tpde_user_opcode_target_kind kind)
{
	return kind == ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_RETURN ? 2 : 1;
}

struct zend_tpde_user_opcode_target {
	uint8_t opcode;
	zend_tpde_user_opcode_target_kind kind;
	zend_native_runtime_helper_id helper;
};

static inline uint64_t zend_tpde_encode_value_operand(
	const zend_mir_source_operand_ref &operand, uint32_t unused_payload)
{
	const uint32_t payload_or_index =
		operand.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			? unused_payload : operand.index;

	return (static_cast<uint64_t>(
			static_cast<uint32_t>(operand.kind) & UINT32_C(0xff)))
		| (static_cast<uint64_t>(
				static_cast<uint32_t>(operand.slot_kind) & UINT32_C(0xff))
			<< 8)
		| (static_cast<uint64_t>(payload_or_index) << 16);
}

static inline uint64_t zend_tpde_encode_value_operand(
	const zend_mir_source_operand_ref &operand)
{
	return zend_tpde_encode_value_operand(operand, ZEND_MIR_ID_INVALID);
}

/*
 * Runtime-helper argument shape is target-neutral. Keep this classification
 * beside the shared operand encoder so Darwin and Linux cannot silently emit
 * different C ABIs for the same MIR operation.
 */
static inline bool zend_tpde_helper_has_explicit_operands(
	zend_native_runtime_helper_id helper)
{
	return helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN
		|| helper == ZEND_NATIVE_HELPER_VALUE_QM_ASSIGN
		|| helper == ZEND_NATIVE_HELPER_VALUE_COPY_TMP
		|| helper == ZEND_NATIVE_HELPER_VALUE_FREE
		|| helper == ZEND_NATIVE_HELPER_VALUE_CONCAT
		|| helper == ZEND_NATIVE_HELPER_VALUE_FAST_CONCAT
		|| helper == ZEND_NATIVE_HELPER_VALUE_BINARY_OP
		|| helper == ZEND_NATIVE_HELPER_VALUE_CAST
		|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_OP
		|| helper == ZEND_NATIVE_HELPER_VALUE_INCDEC
		|| helper == ZEND_NATIVE_HELPER_VALUE_MAKE_REF
		|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_REF
		|| helper == ZEND_NATIVE_HELPER_VALUE_SEPARATE
		|| helper == ZEND_NATIVE_HELPER_VALUE_UNSET_CV
		|| helper == ZEND_NATIVE_HELPER_VALUE_CHECK_VAR
		|| helper == ZEND_NATIVE_HELPER_VALUE_TYPE_CHECK
		|| helper == ZEND_NATIVE_HELPER_VALUE_ROPE_INIT
		|| helper == ZEND_NATIVE_HELPER_VALUE_ROPE_ADD
		|| helper == ZEND_NATIVE_HELPER_VALUE_ROPE_END
		|| helper == ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY
		|| helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT
		|| helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_UNPACK
		|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV
		|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM
		|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
		|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP
		|| helper == ZEND_NATIVE_HELPER_VALUE_UNSET_DIM
		|| helper == ZEND_NATIVE_HELPER_VALUE_FE_FREE
		|| helper == ZEND_NATIVE_HELPER_VALUE_FETCH_LIST
		|| helper == ZEND_NATIVE_HELPER_VALUE_UNARY_OP
		|| helper == ZEND_NATIVE_HELPER_VERIFY_RETURN_TYPE
		|| helper == ZEND_NATIVE_HELPER_VALUE_ECHO
		|| helper == ZEND_NATIVE_HELPER_VALUE_FUNC_NUM_ARGS
		|| helper == ZEND_NATIVE_HELPER_VALUE_FUNC_GET_ARGS
		|| (helper >= ZEND_NATIVE_HELPER_VALUE_COUNT
			&& helper <= ZEND_NATIVE_HELPER_VALUE_EXT_NOP)
		|| helper == ZEND_NATIVE_HELPER_VALUE_DISCARD_EXCEPTION
		|| helper == ZEND_NATIVE_HELPER_VALUE_CASE
		|| helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL
		|| helper == ZEND_NATIVE_HELPER_CALL_FRAMELESS_INTERNAL
		|| (helper >= ZEND_NATIVE_HELPER_GENERATOR_CREATE
			&& helper < ZEND_NATIVE_HELPER_COUNT)
		|| (helper >= ZEND_NATIVE_HELPER_OBJECT_DECLARE_ANON_CLASS
			&& helper <= ZEND_NATIVE_HELPER_OBJECT_BIND_STATIC)
		|| (helper >= ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS_NAME
			&& helper <= ZEND_NATIVE_HELPER_OBJECT_DECLARE_CLASS_DELAYED)
		|| (helper >= ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R
			&& helper <= ZEND_NATIVE_HELPER_DYNAMIC_INCLUDE_OR_EVAL)
		|| (helper >= ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R
			&& helper <= ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_UNSET);
}

static inline bool zend_tpde_helper_has_unused_operand_payloads(
	zend_native_runtime_helper_id helper)
{
	return (helper >= ZEND_NATIVE_HELPER_OBJECT_DECLARE_ANON_CLASS
			&& helper <= ZEND_NATIVE_HELPER_OBJECT_BIND_STATIC)
		|| (helper >= ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS_NAME
			&& helper <= ZEND_NATIVE_HELPER_OBJECT_DECLARE_CLASS_DELAYED)
		|| (helper >= ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R
			&& helper <= ZEND_NATIVE_HELPER_DYNAMIC_INCLUDE_OR_EVAL)
		|| helper == ZEND_NATIVE_HELPER_VALUE_CHECK_FUNC_ARG
		|| helper == ZEND_NATIVE_HELPER_VALUE_CHECK_UNDEF_ARGS;
}

static inline bool zend_tpde_helper_has_explicit_auxiliary(
	zend_native_runtime_helper_id helper)
{
	return helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
		|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP
		|| (helper >= ZEND_NATIVE_HELPER_OBJECT_ASSIGN
			&& helper <= ZEND_NATIVE_HELPER_OBJECT_ASSIGN_OP)
		|| (helper >= ZEND_NATIVE_HELPER_STATIC_ASSIGN
			&& helper <= ZEND_NATIVE_HELPER_STATIC_ASSIGN_OP)
		|| (helper >= ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R
			&& helper
				<= ZEND_NATIVE_HELPER_DYNAMIC_DECLARE_ATTRIBUTED_CONSTANT)
		|| helper == ZEND_NATIVE_HELPER_CALL_FRAMELESS_INTERNAL;
}

struct zend_tpde_value {
	zend_mir_value_id id;
	zend_mir_representation representation;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_storage_id canonical_storage_id;
	zend_mir_ownership_state ownership;
	zend_mir_value_category category;
	zend_mir_refcount_state refcount_state;
	int32_t argument_index;
	zend_tpde_machine_value_kind machine_kind;
	zend_tpde_machine_location location;
	zend_tpde_canonical_slot_state slot_state;
	bool canonical_alias_observable;
	bool constant;
	uint64_t constant_bits;
};

struct zend_tpde_materialization {
	uint32_t value_index;
	zend_mir_storage_id storage_id;
	zend_tpde_machine_value_kind machine_kind;
};

struct zend_tpde_id_index_entry {
	uint32_t id;
	uint32_t index;
};

struct zend_tpde_instruction {
	zend_mir_instruction_id id;
	uint32_t view_index;
	uint32_t operand_count;
	uint32_t component_target_index;
	zend_native_entry_cell *entry_cell;
	zend_native_internal_call_cell *internal_call_cell;
	zend_mir_call_site_ref call_site;
	zend_mir_block_id exception_block_id;
	uint32_t call_argument_offset;
	uint32_t call_argument_count;
	_zend_native_user_call_descriptor *user_call;
	_zend_native_direct_call_descriptor *direct_call;
	_zend_native_direct_internal_call_descriptor *direct_internal_call;
	zend_native_source_effect_kind source_effect;
	zend_mir_scalar_type_mask source_effect_exact_type;
	bool debug_probe;
	zend_mir_storage_id zval_store_storage_id;
	zend_native_runtime_helper_id runtime_helper;
	zend_mir_executable_value_ref value_operation;
	bool has_value_operation;
	bool user_opcode_call_fragments;
	bool direct_scalar_return;
	zend_mir_scalar_type_mask direct_scalar_return_type;
	uint32_t direct_scalar_return_offset;
	uint32_t source_opline_index;
	uint32_t materialization_offset;
	uint32_t materialization_count;
};

struct zend_tpde_array_read {
	uint32_t container_offset;
	uint32_t key_offset;
	uint32_t result_offset;
};

struct zend_tpde_packed_array_append {
	uint32_t container_offset;
	uint32_t value_offset;
	uint32_t result_offset;
	bool move_value;
	bool has_result;
};

struct zend_tpde_array_isset {
	uint32_t container_offset;
	uint32_t key_offset;
	uint32_t result_offset;
};

struct zend_tpde_string_length {
	uint32_t operand_offset;
	uint32_t result_offset;
};

struct zend_tpde_string_identity {
	uint32_t left_offset;
	uint32_t right_offset;
	uint32_t result_offset;
	bool inverted;
};

struct zend_tpde_long_operand {
	uint32_t offset;
	bool literal;
};

struct zend_tpde_long_binary {
	zend_tpde_long_operand left;
	zend_tpde_long_operand right;
	uint32_t result_offset;
	uint32_t source_opcode;
};

struct zend_tpde_long_assign_op {
	uint32_t left_offset;
	zend_tpde_long_operand right;
	uint32_t result_offset;
	uint32_t source_opcode;
	bool has_result;
	bool consume_right;
};

struct zend_tpde_long_incdec {
	uint32_t operand_offset;
	uint32_t result_offset;
	bool has_result;
	bool increment;
	bool post;
};

struct zend_tpde_value_condition {
	uint32_t operand_offset;
};

struct zend_tpde_slot_isset_empty {
	uint32_t operand_offset;
	uint32_t result_offset;
	bool is_empty;
};

struct zend_tpde_object_property_read {
	uint32_t receiver_offset;
	uint32_t result_offset;
	uint32_t cache_offset;
};

struct zend_tpde_object_property_write {
	uint32_t receiver_offset;
	uint32_t value_offset;
	uint32_t cache_offset;
	bool move_value;
};

struct zend_tpde_dynamic_fetch_read {
	uint32_t name_offset;
	uint32_t result_offset;
};

struct zend_tpde_multi_branch {
	uint32_t operand_offset;
	uint32_t source_opcode;
	uint32_t successor_count;
	HashTable *jump_table;
};

struct zend_tpde_user_multi_branch {
	uint32_t operand_offset;
	uint32_t target_opcode;
	uint32_t default_target;
	uint32_t fallback_target;
	HashTable *jump_table;
};

struct zend_tpde_integer_case {
	int64_t value;
	uint32_t label_index;
};

enum zend_tpde_integer_dispatch_kind : uint8_t {
	ZEND_TPDE_INTEGER_DISPATCH_LINEAR = 0,
	ZEND_TPDE_INTEGER_DISPATCH_BALANCED = 1,
	ZEND_TPDE_INTEGER_DISPATCH_JUMP_TABLE = 2,
};

static inline zend_tpde_integer_dispatch_kind
zend_tpde_integer_dispatch(
	HashTable *jump_table,
	std::vector<zend_tpde_integer_case> *cases,
	int64_t *low,
	uint64_t *range)
{
	if (jump_table == nullptr || cases == nullptr
			|| low == nullptr || range == nullptr) {
		return ZEND_TPDE_INTEGER_DISPATCH_LINEAR;
	}
	cases->clear();
	cases->reserve(zend_hash_num_elements(jump_table));
	uint32_t label_index = 0;
	zend_ulong numeric_key;
	zend_string *string_key;
	zval *jump_value;
	ZEND_HASH_FOREACH_KEY_VAL(
			jump_table, numeric_key, string_key, jump_value) {
		if (string_key == nullptr) {
			cases->push_back({
				static_cast<int64_t>(numeric_key), label_index});
		}
		++label_index;
	} ZEND_HASH_FOREACH_END();
	if (cases->size() <= 4) {
		return ZEND_TPDE_INTEGER_DISPATCH_LINEAR;
	}
	std::ranges::sort(*cases, {}, &zend_tpde_integer_case::value);
	const int64_t first = cases->front().value;
	const int64_t last = cases->back().value;
	const uint64_t span = static_cast<uint64_t>(last)
		- static_cast<uint64_t>(first) + 1;
	if (span == 0) {
		return ZEND_TPDE_INTEGER_DISPATCH_LINEAR;
	}
	*low = first;
	*range = span;
	return span <= std::numeric_limits<uint32_t>::max()
			&& span / cases->size() < 8
		? ZEND_TPDE_INTEGER_DISPATCH_JUMP_TABLE
		: ZEND_TPDE_INTEGER_DISPATCH_BALANCED;
}

static inline uint32_t zend_tpde_relative_source_target(
	const zend_op_array *op_array, uint32_t source_position,
	zend_long byte_offset)
{
	if (op_array == nullptr || source_position >= op_array->last) {
		return UINT32_MAX;
	}
	const uintptr_t first = reinterpret_cast<uintptr_t>(op_array->opcodes);
	const intptr_t signed_address =
		reinterpret_cast<intptr_t>(&op_array->opcodes[source_position])
		+ static_cast<intptr_t>(byte_offset);
	const uintptr_t address = static_cast<uintptr_t>(signed_address);
	const uintptr_t last =
		first + static_cast<uintptr_t>(op_array->last) * sizeof(zend_op);
	if (address < first || address >= last
			|| (address - first) % sizeof(zend_op) != 0) {
		return UINT32_MAX;
	}
	return static_cast<uint32_t>((address - first) / sizeof(zend_op));
}

/*
 * Keep the semantic fast-path selection target-neutral.  Target backends only
 * encode the guards and loads; they do not independently decide which MIR
 * shape is safe to execute without the generic dimension primitive.
 */
static inline bool zend_tpde_array_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_read *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t container_offset;
	uint64_t key_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
			|| operation.source_opcode != ZEND_FETCH_DIM_R
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.op2_storage_id
			|| operation.op1_storage_id == operation.result_storage_id
			|| operation.op2_storage_id == operation.result_storage_id) {
		return false;
	}
	container_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	key_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (container_offset > UINT32_MAX || key_offset > UINT32_MAX
			|| result_offset > UINT32_MAX) {
		return false;
	}
	out->container_offset = static_cast<uint32_t>(container_offset);
	out->key_offset = static_cast<uint32_t>(key_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	return true;
}

static inline bool zend_tpde_packed_array_append_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_packed_array_append *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t container_offset;
	uint64_t value_offset;
	uint64_t result_offset = 0;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
			|| operation.source_opcode != ZEND_ASSIGN_DIM
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.auxiliary_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.auxiliary_storage_id
			|| (operation.auxiliary.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.auxiliary.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_TMP)
			|| (operation.result_storage_id != ZEND_MIR_ID_INVALID
				&& (operation.result_storage_id
						== operation.op1_storage_id
					|| operation.result_storage_id
						== operation.auxiliary_storage_id))) {
		return false;
	}
	container_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	value_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.auxiliary_storage_id)
			* sizeof(zval);
	if (operation.result_storage_id != ZEND_MIR_ID_INVALID) {
		result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
	}
	if (container_offset > UINT32_MAX || value_offset > UINT32_MAX
			|| result_offset > UINT32_MAX) {
		return false;
	}
	out->container_offset = static_cast<uint32_t>(container_offset);
	out->value_offset = static_cast<uint32_t>(value_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->move_value =
		operation.auxiliary.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
	out->has_result =
		operation.result_storage_id != ZEND_MIR_ID_INVALID;
	return true;
}

static inline bool zend_tpde_array_isset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_isset *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t container_offset;
	uint64_t key_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode
				!= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
			|| operation.source_opcode != ZEND_ISSET_ISEMPTY_DIM_OBJ
			|| (operation.extended_value & ZEND_ISEMPTY) != 0
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.op2_storage_id
			|| operation.op1_storage_id == operation.result_storage_id
			|| operation.op2_storage_id == operation.result_storage_id) {
		return false;
	}
	container_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	key_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (container_offset > UINT32_MAX || key_offset > UINT32_MAX
			|| result_offset > UINT32_MAX) {
		return false;
	}
	out->container_offset = static_cast<uint32_t>(container_offset);
	out->key_offset = static_cast<uint32_t>(key_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	return true;
}

static inline bool zend_tpde_string_length_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_string_length *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
			|| operation.source_opcode != ZEND_STRLEN
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.result_storage_id) {
		return false;
	}
	operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (operand_offset > UINT32_MAX || result_offset > UINT32_MAX) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	return true;
}

static inline bool zend_tpde_value_condition_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_value_condition *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
			|| (operation.source_opcode != ZEND_JMPZ
				&& operation.source_opcode != ZEND_JMPNZ)
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID) {
		return false;
	}
	operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (operand_offset > UINT32_MAX) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	return true;
}

static inline bool zend_tpde_string_identity_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_string_identity *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t left_offset;
	uint64_t right_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_BINARY_OP
			|| (operation.source_opcode != ZEND_IS_IDENTICAL
				&& operation.source_opcode != ZEND_IS_NOT_IDENTICAL)
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.op2_storage_id
			|| operation.op1_storage_id == operation.result_storage_id
			|| operation.op2_storage_id == operation.result_storage_id) {
		return false;
	}
	left_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	right_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (left_offset > UINT32_MAX || right_offset > UINT32_MAX
			|| result_offset > UINT32_MAX) {
		return false;
	}
	out->left_offset = static_cast<uint32_t>(left_offset);
	out->right_offset = static_cast<uint32_t>(right_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->inverted = operation.source_opcode == ZEND_IS_NOT_IDENTICAL;
	return true;
}

static inline bool zend_tpde_long_operand_at(
	const zend_mir_source_operand_ref &operand,
	zend_mir_storage_id storage_id,
	zend_tpde_long_operand *out)
{
	uint64_t offset;

	if (out == nullptr) {
		return false;
	}
	if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		offset = uint64_t{operand.index} * sizeof(zval);
		out->literal = true;
	} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& storage_id != ZEND_MIR_ID_INVALID) {
		offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval);
		out->literal = false;
	} else {
		return false;
	}
	if (offset > UINT32_MAX) {
		return false;
	}
	out->offset = static_cast<uint32_t>(offset);
	return true;
}

static inline bool zend_tpde_long_binary_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_binary *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_BINARY_OP
			|| (operation.source_opcode != ZEND_ADD
				&& operation.source_opcode != ZEND_SUB
				&& operation.source_opcode != ZEND_BW_OR
				&& operation.source_opcode != ZEND_BW_AND
				&& operation.source_opcode != ZEND_BW_XOR
				&& operation.source_opcode != ZEND_IS_IDENTICAL
				&& operation.source_opcode != ZEND_IS_NOT_IDENTICAL
				&& operation.source_opcode != ZEND_IS_EQUAL
				&& operation.source_opcode != ZEND_IS_NOT_EQUAL
				&& operation.source_opcode != ZEND_IS_SMALLER
				&& operation.source_opcode
					!= ZEND_IS_SMALLER_OR_EQUAL)
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| !zend_tpde_long_operand_at(
				operation.op1, operation.op1_storage_id, &out->left)
			|| !zend_tpde_long_operand_at(
				operation.op2, operation.op2_storage_id, &out->right)
			|| (!out->left.literal
				&& operation.op1_storage_id
					== operation.result_storage_id)
			|| (!out->right.literal
				&& operation.op2_storage_id
					== operation.result_storage_id)) {
		return false;
	}
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (result_offset > UINT32_MAX) {
		return false;
	}
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->source_opcode = operation.source_opcode;
	return true;
}

static inline bool zend_tpde_long_assign_op_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_assign_op *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t left_offset;
	uint64_t right_offset;
	uint64_t result_offset = 0;
	bool has_result;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
			|| (operation.extended_value != ZEND_ADD
				&& operation.extended_value != ZEND_SUB
				&& operation.extended_value != ZEND_BW_OR
				&& operation.extended_value != ZEND_BW_AND
				&& operation.extended_value != ZEND_BW_XOR)
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID) {
		return false;
	}
	left_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& operation.op2_storage_id != ZEND_MIR_ID_INVALID) {
		right_offset = operation.op2_storage_id * sizeof(zval);
		out->right.literal = true;
	} else if ((operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
				|| operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP)
			&& operation.op2_storage_id != ZEND_MIR_ID_INVALID
			&& operation.op2_storage_id != operation.op1_storage_id) {
		right_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
				* sizeof(zval);
		out->right.literal = false;
	} else {
		return false;
	}
	has_result = operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED;
	if (has_result
			&& ((operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& operation.result.kind
						!= ZEND_MIR_SOURCE_OPERAND_SSA)
				|| (operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_TMP)
				|| operation.result_storage_id == ZEND_MIR_ID_INVALID
				|| operation.result_storage_id
					== operation.op1_storage_id
				|| (!out->right.literal
					&& operation.result_storage_id
						== operation.op2_storage_id))) {
		return false;
	}
	if (has_result) {
		result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
	}
	if (left_offset > UINT32_MAX || right_offset > UINT32_MAX
			|| result_offset > UINT32_MAX) {
		return false;
	}
	out->left_offset = static_cast<uint32_t>(left_offset);
	out->right.offset = static_cast<uint32_t>(right_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->source_opcode = operation.extended_value;
	out->has_result = has_result;
	out->consume_right =
		!out->right.literal
		&& (operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
			|| operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP);
	return true;
}

static inline bool zend_tpde_long_incdec_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_incdec *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset = 0;
	bool has_result;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_INCDEC
			|| (operation.source_opcode != ZEND_PRE_INC
				&& operation.source_opcode != ZEND_PRE_DEC
				&& operation.source_opcode != ZEND_POST_INC
				&& operation.source_opcode != ZEND_POST_DEC)
			|| operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID) {
		return false;
	}
	has_result = operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED;
	if (has_result
			&& (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR)
				|| operation.result_storage_id == ZEND_MIR_ID_INVALID
				|| operation.result_storage_id
					== operation.op1_storage_id)) {
		return false;
	}
	if ((operation.source_opcode == ZEND_POST_INC
			|| operation.source_opcode == ZEND_POST_DEC)
			&& !has_result) {
		return false;
	}
	operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (has_result) {
		result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
	}
	if (operand_offset > UINT32_MAX || result_offset > UINT32_MAX) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->has_result = has_result;
	out->increment = operation.source_opcode == ZEND_PRE_INC
		|| operation.source_opcode == ZEND_POST_INC;
	out->post = operation.source_opcode == ZEND_POST_INC
		|| operation.source_opcode == ZEND_POST_DEC;
	return true;
}

static inline bool zend_tpde_slot_isset_empty_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_slot_isset_empty *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode
				!= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
			|| operation.source_opcode != ZEND_ISSET_ISEMPTY_CV
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.result_storage_id) {
		return false;
	}
	operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (operand_offset > UINT32_MAX || result_offset > UINT32_MAX) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->is_empty = (operation.extended_value & ZEND_ISEMPTY) != 0;
	return true;
}

static inline bool zend_tpde_object_property_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t receiver_offset;
	uint64_t result_offset;
	uint32_t cache_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_OBJECT_FETCH_R
			|| operation.source_opcode != ZEND_FETCH_OBJ_R
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED)
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (operation.op1_storage_id == ZEND_MIR_ID_INVALID
					|| operation.op1_storage_id
						== operation.result_storage_id))) {
		return false;
	}
	receiver_offset =
		operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
		? offsetof(zend_execute_data, This)
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	cache_offset = operation.extended_value & ~ZEND_FETCH_REF;
	if (receiver_offset > UINT32_MAX || result_offset > UINT32_MAX
			|| cache_offset > UINT32_MAX - 3 * sizeof(void *)) {
		return false;
	}
	out->receiver_offset = static_cast<uint32_t>(receiver_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	out->cache_offset = cache_offset;
	return true;
}

static inline bool zend_tpde_object_property_write_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_write *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t receiver_offset;
	uint64_t value_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_OBJECT_ASSIGN
			|| operation.source_opcode != ZEND_ASSIGN_OBJ
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED)
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| (operation.auxiliary.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV
				&& operation.auxiliary.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_TMP)
			|| operation.auxiliary_storage_id == ZEND_MIR_ID_INVALID
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (operation.op1_storage_id == ZEND_MIR_ID_INVALID
					|| operation.op1_storage_id
						== operation.auxiliary_storage_id))) {
		return false;
	}
	receiver_offset =
		operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
		? offsetof(zend_execute_data, This)
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	value_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.auxiliary_storage_id)
			* sizeof(zval);
	if (receiver_offset > UINT32_MAX || value_offset > UINT32_MAX
			|| operation.extended_value
				> UINT32_MAX - 3 * sizeof(void *)) {
		return false;
	}
	out->receiver_offset = static_cast<uint32_t>(receiver_offset);
	out->value_offset = static_cast<uint32_t>(value_offset);
	out->cache_offset = operation.extended_value;
	out->move_value =
		operation.auxiliary.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
	return true;
}

static inline bool zend_tpde_dynamic_fetch_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_dynamic_fetch_read *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t name_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
			|| operation.source_opcode != ZEND_FETCH_R
			|| operation.extended_value != ZEND_FETCH_LOCAL
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.result_storage_id) {
		return false;
	}
	name_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (name_offset > UINT32_MAX || result_offset > UINT32_MAX) {
		return false;
	}
	out->name_offset = static_cast<uint32_t>(name_offset);
	out->result_offset = static_cast<uint32_t>(result_offset);
	return true;
}

struct zend_tpde_plan {
	const zend_mir_view *view;
	const zend_mir_call_view *calls;
	const zend_native_runtime_api *runtime;
	const zend_op_array *source_op_array;
	const struct _zend_ssa *source_ssa;
	uint32_t symbol_namespace;
	zend_mir_function_record function;
	zend_mir_block_id *block_ids;
	uint32_t block_count;
	zend_tpde_id_index_entry *block_index;
	uint32_t block_index_capacity;
	zend_tpde_value *values;
	uint32_t value_count;
	int32_t *argument_value_indices;
	zend_tpde_id_index_entry *value_index;
	uint32_t value_index_capacity;
	zend_tpde_instruction *instructions;
	uint32_t instruction_count;
	zend_tpde_id_index_entry *instruction_index;
	uint32_t instruction_index_capacity;
	int32_t *value_definition_instructions;
	uint32_t *value_consumer_offsets;
	uint32_t *value_consumers;
	uint32_t value_consumer_count;
	zend_tpde_id_index_entry *call_site_instruction_index;
	uint32_t call_site_instruction_index_capacity;
	uint32_t call_site_count;
	zend_tpde_id_index_entry *call_target_index;
	uint32_t call_target_index_capacity;
	uint32_t call_target_count;
	uint32_t call_argument_count;
	zend_tpde_id_index_entry *user_binding_index;
	uint32_t user_binding_index_capacity;
	zend_tpde_id_index_entry *internal_binding_index;
	uint32_t internal_binding_index_capacity;
	_zend_native_direct_call_descriptor **direct_calls;
	uint32_t direct_call_count;
	_zend_native_direct_internal_call_descriptor **direct_internal_calls;
	uint32_t direct_internal_call_count;
	_zend_native_user_call_descriptor **user_calls;
	uint32_t user_call_count;
	uint32_t argument_count;
	uint32_t value_model_flags;
	uint64_t required_runtime_capabilities;
	uint64_t required_runtime_helpers[ZEND_NATIVE_RUNTIME_HELPER_WORD_COUNT];
	zend_mir_executable_value_ref *user_opcode_source_operations;
	uint32_t user_opcode_source_operation_count;
	uint32_t *user_opcode_source_op1_targets;
	uint32_t *user_opcode_source_op2_targets;
	uint32_t *user_opcode_source_extended_targets;
	zend_tpde_user_opcode_target
		user_opcode_targets[ZEND_VM_LAST_OPCODE + 1];
	uint32_t user_opcode_target_count;
	uint32_t generator_resume_count;
	uint32_t generator_resume_live_word_count;
	uint32_t *generator_resume_targets;
	uint32_t *generator_resume_landings;
	zend_mir_block_id *generator_resume_exception_blocks;
	uint64_t *generator_resume_live_values;
	zend_tpde_materialization *materializations;
	uint32_t materialization_count;
	uint32_t *entry_undef_temporary_indices;
	uint32_t entry_undef_temporary_count;
	bool may_emit_calls;
	bool user_opcode_callbacks;
};

static inline bool zend_tpde_generator_resume_value_live(
	const zend_tpde_plan *plan, uint32_t resume_index, uint32_t value_index)
{
	if (plan == nullptr
			|| resume_index >= plan->generator_resume_count
			|| value_index >= plan->value_count
			|| plan->generator_resume_live_values == nullptr) {
		return false;
	}
	const uint64_t *words =
		plan->generator_resume_live_values
			+ static_cast<size_t>(resume_index)
				* plan->generator_resume_live_word_count;
	return (words[value_index / 64]
			& (uint64_t{1} << (value_index % 64))) != 0;
}

static inline bool zend_tpde_user_multi_branch_at(
	const zend_tpde_plan *plan,
	const zend_mir_executable_value_ref &operation,
	uint32_t target_opcode,
	zend_tpde_user_multi_branch *out)
{
	if (plan == nullptr || out == nullptr || plan->source_op_array == nullptr
			|| operation.source_position_id >= plan->source_op_array->last
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| (target_opcode != ZEND_SWITCH_LONG
				&& target_opcode != ZEND_SWITCH_STRING
				&& target_opcode != ZEND_MATCH)) {
		return false;
	}
	const zend_op *opline =
		&plan->source_op_array->opcodes[operation.source_position_id];
	if (opline->op2_type != IS_CONST) {
		return false;
	}
	const zval *jump_table = RT_CONSTANT(opline, opline->op2);
	if (Z_TYPE_P(jump_table) != IS_ARRAY) {
		return false;
	}
	const uint64_t operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	const uint32_t default_target = zend_tpde_relative_source_target(
		plan->source_op_array, operation.source_position_id,
		static_cast<zend_long>(opline->extended_value));
	const uint32_t fallback_target =
		operation.source_position_id + 1 < plan->source_op_array->last
			? operation.source_position_id + 1 : UINT32_MAX;
	if (operand_offset > UINT32_MAX || default_target == UINT32_MAX
			|| (target_opcode != ZEND_MATCH
				&& fallback_target == UINT32_MAX)) {
		return false;
	}
	zval *jump_value;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(jump_table), jump_value) {
		if (Z_TYPE_P(jump_value) != IS_LONG
				|| zend_tpde_relative_source_target(
					plan->source_op_array, operation.source_position_id,
					Z_LVAL_P(jump_value)) == UINT32_MAX) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->target_opcode = target_opcode;
	out->default_target = default_target;
	out->fallback_target = fallback_target;
	out->jump_table = Z_ARRVAL_P(jump_table);
	return true;
}

static inline bool zend_tpde_multi_branch_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction,
	const zend_mir_instruction_record &record,
	zend_tpde_multi_branch *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	const zend_op *opline;
	const zval *jump_table;
	uint64_t operand_offset;
	uint32_t expected_successors;

	if (plan == nullptr || out == nullptr
			|| !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
			|| record.opcode != ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
			|| plan->source_op_array == nullptr
			|| operation.source_position_id >= plan->source_op_array->last
			|| operation.source_position_id != record.source_position_id
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return false;
	}
	opline =
		&plan->source_op_array->opcodes[operation.source_position_id];
	if (opline->opcode != operation.source_opcode
			|| (opline->opcode != ZEND_SWITCH_LONG
				&& opline->opcode != ZEND_SWITCH_STRING
				&& opline->opcode != ZEND_MATCH)
			|| opline->op2_type != IS_CONST) {
		return false;
	}
	jump_table = RT_CONSTANT(opline, opline->op2);
	if (Z_TYPE_P(jump_table) != IS_ARRAY) {
		return false;
	}
	expected_successors = zend_hash_num_elements(Z_ARRVAL_P(jump_table))
		+ (opline->opcode == ZEND_MATCH ? 1 : 2);
	if (expected_successors < 2
			|| plan->view->successor_count(
				plan->view->context, record.block_id)
				!= expected_successors) {
		return false;
	}
	operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (operand_offset > UINT32_MAX) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->source_opcode = opline->opcode;
	out->successor_count = expected_successors;
	out->jump_table = Z_ARRVAL_P(jump_table);
	return true;
}

enum zend_native_image_symbol_kind : uint32_t {
	ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER = 1,
	ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL = 2,
	ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL = 3,
	ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_API = 4,
	ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR = 5,
	ZEND_NATIVE_IMAGE_SYMBOL_SOURCE = 6,
	ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR = 7,
	ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR = 8,
};

struct zend_native_image_symbol {
	uint32_t kind;
	uint32_t id;
	uint32_t symbol_namespace;
	uint32_t abi_version;
	uint32_t effects;
	char name[64];
};

struct zend_native_image_symbol_binding {
	uint32_t symbol_index;
	const void *address;
};

struct zend_native_component_entry {
	uint32_t argument_count;
	uint32_t frame_variable_count;
	uint32_t frame_temporary_count;
};

struct zend_native_image {
	zend_native_target target;
	uint32_t abi_version;
	uint32_t runtime_abi_version;
	uint64_t build_id;
	uint64_t code_version;
	unsigned char *text;
	size_t text_size;
	size_t text_capacity;
	zend_native_image_symbol *symbols;
	uint32_t symbol_count;
	uint32_t symbol_capacity;
	zend_native_image_symbol_binding *symbol_bindings;
	uint32_t symbol_binding_count;
	uint32_t symbol_binding_capacity;
	uint32_t slot_count;
	uint32_t argument_count;
	uint32_t frame_variable_count;
	uint32_t frame_temporary_count;
	zend_native_component_entry *component_entries;
	uint32_t component_entry_count;
	zend_native_image_metrics metrics;
	void *target_state;
	void (*destroy_target_state)(void *);
	_zend_native_direct_call_descriptor **direct_calls;
	uint32_t direct_call_count;
	_zend_native_direct_internal_call_descriptor **direct_internal_calls;
	uint32_t direct_internal_call_count;
	_zend_native_user_call_descriptor **user_calls;
	uint32_t user_call_count;
	_zend_native_internal_call_cell **owned_internal_call_cells;
	uint32_t owned_internal_call_cell_count;
};

struct zend_native_code {
	zend_native_target target;
	zend_native_code *owner;
	uint32_t owner_refcount;
	void *mapping;
	size_t mapping_size;
	zend_native_frame_entry_t entry;
	zend_native_frame_entry_t *component_entries;
	zend_native_component_entry *component_metadata;
	uint32_t component_entry_count;
	uint32_t slot_count;
	uint32_t argument_count;
	uint32_t frame_variable_count;
	uint32_t frame_temporary_count;
	bool writable;
	bool executable;
	bool unwind_registered;
	void *target_state;
	void (*destroy_target_state)(void *);
	_zend_native_direct_call_descriptor **direct_calls;
	uint32_t direct_call_count;
	_zend_native_direct_internal_call_descriptor **direct_internal_calls;
	uint32_t direct_internal_call_count;
	_zend_native_user_call_descriptor **user_calls;
	uint32_t user_call_count;
	_zend_native_internal_call_cell **owned_internal_call_cells;
	uint32_t owned_internal_call_cell_count;
};

void zend_tpde_set_diagnostic(
	zend_native_diagnostic *diag,
	zend_native_diagnostic_code code,
	const char *message);
int32_t zend_tpde_value_index(
	const zend_tpde_plan *plan, zend_mir_value_id id);
int32_t zend_tpde_block_index(
	const zend_tpde_plan *plan, zend_mir_block_id id);
int32_t zend_tpde_instruction_index(
	const zend_tpde_plan *plan, zend_mir_instruction_id id);
const zend_tpde_instruction *zend_tpde_instruction_at(
	const zend_tpde_plan *plan, uint32_t index);
zend_mir_instruction_record zend_tpde_instruction_record_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction);
bool zend_tpde_call_argument_at(
	const zend_tpde_plan *plan,
	uint32_t index,
	zend_mir_call_argument_ref *out);
zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index);
bool zend_tpde_image_append(
	zend_native_image *image, const void *bytes, size_t length);
bool zend_tpde_image_u8(zend_native_image *image, uint8_t value);
bool zend_tpde_image_u32(zend_native_image *image, uint32_t value);
bool zend_tpde_image_u64(zend_native_image *image, uint64_t value);
const zend_native_image_symbol *zend_tpde_image_symbol_find(
	const zend_native_image *image,
	zend_native_image_symbol_kind kind,
	uint32_t id,
	uint32_t symbol_namespace);
bool zend_tpde_image_resolve_symbol(
	const zend_native_image *image,
	const char *name,
	const void **address);

zend_result zend_tpde_emit_darwin_arm64(
	const zend_tpde_plan *const *plans,
	uint32_t plan_count,
	zend_native_image *image,
	zend_native_diagnostic *diag);
zend_result zend_tpde_emit_linux_x64(
	const zend_tpde_plan *const *plans,
	uint32_t plan_count,
	zend_native_image *image,
	zend_native_diagnostic *diag);
zend_result zend_tpde_map_darwin_arm64(
	const zend_native_image *image,
	zend_native_code *code,
	zend_native_diagnostic *diag);

zend_result zend_native_publish_darwin_arm64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag);
zend_result zend_native_publish_linux_x64(
	const zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag);
void zend_native_unmap_darwin_arm64(zend_native_code *code);
void zend_native_unmap_linux_x64(zend_native_code *code);
