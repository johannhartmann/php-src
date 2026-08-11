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
	ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR = 8,
};

enum zend_tpde_machine_register_bank : uint8_t {
	ZEND_TPDE_MACHINE_REGISTER_GP = 0,
	ZEND_TPDE_MACHINE_REGISTER_FP = 1,
};

enum zend_tpde_machine_part_role : uint8_t {
	ZEND_TPDE_MACHINE_PART_VALUE = 0,
	ZEND_TPDE_MACHINE_PART_PAYLOAD = 1,
	ZEND_TPDE_MACHINE_PART_TYPE_INFO = 2,
};

enum zend_tpde_machine_abi_extension : uint8_t {
	ZEND_TPDE_MACHINE_ABI_EXTENSION_NONE = 0,
	ZEND_TPDE_MACHINE_ABI_EXTENSION_ZERO = 1,
	ZEND_TPDE_MACHINE_ABI_EXTENSION_SIGN = 2,
};

enum zend_tpde_machine_part_ownership_role : uint8_t {
	ZEND_TPDE_MACHINE_PART_OWNERSHIP_NONE = 0,
	ZEND_TPDE_MACHINE_PART_OWNERSHIP_VALUE = 1,
	ZEND_TPDE_MACHINE_PART_OWNERSHIP_METADATA = 2,
};

struct zend_tpde_machine_part_desc {
	zend_tpde_machine_part_role semantic_role;
	uint16_t bit_width;
	zend_tpde_machine_register_bank register_bank;
	zend_tpde_machine_abi_extension abi_extension;
	zend_tpde_machine_part_ownership_role ownership_role;
};

struct zend_tpde_machine_representation_desc {
	uint32_t part_count;
	const zend_tpde_machine_part_desc *parts;
};

enum zend_tpde_local_abi_transfer : uint8_t {
	ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE = 0,
	ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED = 1,
	ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED = 2,
	ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED = 3,
	ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL = 4,
};

struct zend_tpde_local_abi_type {
	zend_mir_representation representation;
	zend_mir_scalar_type_mask exact_type;
	zend_tpde_machine_value_kind machine_kind;
	zend_tpde_local_abi_transfer transfer;
	bool valid;
};

static inline zend_tpde_machine_representation_desc
zend_tpde_machine_representation(
	zend_tpde_machine_value_kind kind, bool register_authoritative)
{
	static constexpr zend_tpde_machine_part_desc gp_value[] = {{
		ZEND_TPDE_MACHINE_PART_VALUE, 64, ZEND_TPDE_MACHINE_REGISTER_GP,
		ZEND_TPDE_MACHINE_ABI_EXTENSION_NONE,
		ZEND_TPDE_MACHINE_PART_OWNERSHIP_NONE}};
	static constexpr zend_tpde_machine_part_desc fp_value[] = {{
		ZEND_TPDE_MACHINE_PART_VALUE, 64, ZEND_TPDE_MACHINE_REGISTER_FP,
		ZEND_TPDE_MACHINE_ABI_EXTENSION_NONE,
		ZEND_TPDE_MACHINE_PART_OWNERSHIP_NONE}};
	static constexpr zend_tpde_machine_part_desc pointer_value[] = {{
		ZEND_TPDE_MACHINE_PART_VALUE, 64, ZEND_TPDE_MACHINE_REGISTER_GP,
		ZEND_TPDE_MACHINE_ABI_EXTENSION_NONE,
		ZEND_TPDE_MACHINE_PART_OWNERSHIP_VALUE}};
	static constexpr zend_tpde_machine_part_desc boxed_zval[] = {
		{ZEND_TPDE_MACHINE_PART_PAYLOAD, 64,
			ZEND_TPDE_MACHINE_REGISTER_GP,
			ZEND_TPDE_MACHINE_ABI_EXTENSION_NONE,
			ZEND_TPDE_MACHINE_PART_OWNERSHIP_VALUE},
		{ZEND_TPDE_MACHINE_PART_TYPE_INFO, 32,
			ZEND_TPDE_MACHINE_REGISTER_GP,
			ZEND_TPDE_MACHINE_ABI_EXTENSION_ZERO,
			ZEND_TPDE_MACHINE_PART_OWNERSHIP_METADATA}};

	if (kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& register_authoritative) {
		return {2, boxed_zval};
	}
	if (kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
		return {1, pointer_value};
	}
	return {1, kind == ZEND_TPDE_MACHINE_VALUE_F64 ? fp_value : gp_value};
}

enum zend_tpde_machine_location : uint8_t {
	ZEND_TPDE_MACHINE_LOCATION_REGISTER = 0,
	ZEND_TPDE_MACHINE_LOCATION_SPILL = 1,
	ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT = 2,
};

static inline bool zend_tpde_machine_value_is_register_authoritative(
	zend_tpde_machine_value_kind kind)
{
	switch (kind) {
		case ZEND_TPDE_MACHINE_VALUE_I64:
		case ZEND_TPDE_MACHINE_VALUE_F64:
		case ZEND_TPDE_MACHINE_VALUE_BOOL:
		case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
		case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
		case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
		case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
		case ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL:
		case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
			return true;
	}
	return false;
}

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
		case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
			return IS_RESOURCE;
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
		case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
			return IS_RESOURCE_EX;
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

/*
 * Selective frame initialization leaves an unobserved TMP/VAR slot
 * uninitialized until a boundary actually materializes a result there.
 * Explicit runtime helpers that define a new Zend result require a fresh
 * result zval.  Array-construction continuation opcodes are different: their
 * result operand names the array accumulator created by INIT_ARRAY and must
 * survive every ADD_ARRAY_* helper call.  Prepare only a distinct temporary
 * result slot for the defining operations; an aliased source/result storage
 * is an in-place operation and must retain its input.
 */
static inline bool zend_tpde_helper_requires_undef_result(
	zend_native_runtime_helper_id helper,
	const zend_mir_executable_value_ref &operation)
{
	if (!zend_tpde_helper_has_explicit_operands(helper)
			|| helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT
			|| helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_UNPACK
			|| !zend_mir_id_is_valid(operation.result_storage_id)
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)) {
		return false;
	}
	return operation.result_storage_id != operation.op1_storage_id
		&& operation.result_storage_id != operation.op2_storage_id
		&& operation.result_storage_id != operation.auxiliary_storage_id;
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
	/*
	 * Source operations such as ZEND_RECV and a proven return-type check can
	 * create a new Zend SSA identity without changing the machine value.
	 * Freeze that def-edge so the adaptor does not reconstruct it from Zend
	 * SSA or reload it through the canonical frame.
	 */
	int32_t register_alias_value_index;
	zend_tpde_machine_value_kind machine_kind;
	zend_tpde_machine_location location;
	zend_tpde_canonical_slot_state slot_state;
	zend_tpde_local_abi_type local_abi;
	bool register_authoritative;
	bool canonical_alias_observable;
	bool constant;
	uint64_t constant_bits;
	bool known_string_literal;
	uint8_t known_string_first_byte;
	uint64_t known_string_length;
};

struct zend_tpde_materialization {
	uint32_t value_index;
	zend_mir_storage_id storage_id;
	zend_tpde_machine_value_kind machine_kind;
	/*
	 * A typed component call may produce a private machine value without a
	 * persistent ZNMIR result identity. Preserve that source definition so
	 * the adaptor can connect it to its canonical source slot at the first
	 * observing runtime boundary.
	 */
	int32_t source_value_index;
	int32_t source_definition_instruction_index;
};

enum zend_tpde_machine_reference_kind : uint8_t {
	ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT = 0,
	ZEND_TPDE_MACHINE_REFERENCE_CONTEXT_FIELD = 1,
	ZEND_TPDE_MACHINE_REFERENCE_LITERAL = 2,
	ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT = 3,
	ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT = 4,
};

/*
 * Pointer-free address-selection facts consumed directly by the TPDE
 * adaptor. Values name frozen ZNMIR identities; UINT32_MAX denotes a
 * synthetic base such as the canonical frame or execution context.
 */
struct zend_tpde_machine_reference {
	zend_tpde_machine_reference_kind kind;
	zend_mir_value_id base_value_id;
	zend_mir_value_id index_value_id;
	uint32_t stable_storage_or_layout_id;
	uint32_t scale;
	int64_t displacement;
	uint32_t access_width;
};

enum zend_tpde_machine_use_kind : uint8_t {
	ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND = 0,
	ZEND_TPDE_MACHINE_USE_PHI_EDGE = 1,
	ZEND_TPDE_MACHINE_USE_CALL_ARGUMENT = 2,
	ZEND_TPDE_MACHINE_USE_LOCAL_ABI_ARGUMENT = 3,
	ZEND_TPDE_MACHINE_USE_STATEPOINT_MATERIALIZATION = 4,
	ZEND_TPDE_MACHINE_USE_SUSPEND_LIVE = 5,
	ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND = 6,
};

struct zend_tpde_machine_use {
	uint32_t instruction_index;
	uint32_t operand_index;
	uint32_t auxiliary;
	zend_tpde_machine_use_kind kind;
};

enum zend_tpde_operand_transport_kind : uint8_t {
	ZEND_TPDE_OPERAND_TRANSPORT_DIRECT = 0,
	ZEND_TPDE_OPERAND_TRANSPORT_CANONICAL_SCALAR_LOAD = 1,
};

/*
 * Representation selection owns every canonical-slot to machine-value
 * transition.  The adaptor consumes this frozen record mechanically; it must
 * not infer a scalar representation from an opcode or from source SSA.
 */
struct zend_tpde_operand_transport {
	zend_tpde_operand_transport_kind kind;
	zend_mir_representation representation;
	zend_mir_scalar_type_mask exact_type;
	zend_tpde_machine_value_kind machine_kind;
	zend_mir_storage_id storage_id;
	bool resolve_reference;
};

struct zend_tpde_id_index_entry {
	uint32_t id;
	uint32_t index;
};

struct zend_tpde_source_value_binding {
	int32_t value_index;
	int32_t definition_instruction_index;
};

enum zend_tpde_source_call_phase : uint8_t {
	ZEND_TPDE_SOURCE_CALL_PHASE_NONE = 0,
	ZEND_TPDE_SOURCE_CALL_PHASE_INIT = 1u << 0,
	ZEND_TPDE_SOURCE_CALL_PHASE_SEND = 1u << 1,
	ZEND_TPDE_SOURCE_CALL_PHASE_CHECK = 1u << 2,
	ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND = 1u << 3,
	ZEND_TPDE_SOURCE_CALL_PHASE_DO = 1u << 4,
};

enum zend_tpde_source_call_operand_flag : uint8_t {
	ZEND_TPDE_SOURCE_CALL_OPERAND_NONE = 0,
	ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE = 1u << 0,
	ZEND_TPDE_SOURCE_CALL_OPERAND_SOURCE = 1u << 1,
	ZEND_TPDE_SOURCE_CALL_OPERAND_RUNTIME_EXPANSION = 1u << 2,
};

struct zend_tpde_source_call_phase_entry {
	uint32_t instruction_index;
	uint32_t argument_index;
	int32_t value_index;
	uint8_t phases;
	uint8_t operand_flags;
};

enum zend_tpde_machine_control_flow_flag : uint8_t {
	ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD = 1u << 0,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_TYPED_COMPONENT_CALL = 1u << 1,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH = 1u << 2,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH = 1u << 3,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT = 1u << 4,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_INVERT_RESULT = 1u << 5,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS = 1u << 6,
	ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE = 1u << 7,
};

struct zend_tpde_instruction {
	zend_mir_instruction_id id;
	zend_mir_instruction_record record;
	uint32_t operand_offset;
	uint32_t operand_count;
	uint32_t component_target_index;
	uint32_t component_body_function_index;
	zend_native_entry_cell *entry_cell;
	zend_native_internal_call_cell *internal_call_cell;
	const zend_mir_call_site_ref *call_site;
	zend_mir_block_id exception_block_id;
	uint32_t call_argument_offset;
	uint32_t call_argument_count;
	_zend_native_user_call_descriptor *user_call;
	_zend_native_direct_call_descriptor *direct_call;
	/*
	 * Compile-time-only type guards for inline-frame arguments. By-reference
	 * entries describe the referent; by-value entries prove a canonical boxed
	 * source before it is transported as an exact scalar. These facts are
	 * consumed while lowering an inline frame and are never copied into the
	 * serialized direct-call descriptor.
	 */
	zend_mir_scalar_type_mask *direct_call_argument_guard_types;
	_zend_native_direct_internal_call_descriptor *direct_internal_call;
	zend_native_source_effect_kind source_effect;
	zend_mir_scalar_type_mask source_effect_exact_type;
	bool debug_probe;
	zend_mir_storage_id zval_store_storage_id;
	bool zval_store_direct_scalar;
	bool zval_store_lazy_scalar;
	/*
	 * A slot-authoritative exact scalar may still need a short-lived machine
	 * definition for the ZVAL_STORE that publishes it.  This transport is
	 * frozen separately from long-term register authority so alias-observable
	 * CVs are never treated as register-resident after publication.
	 */
	bool transient_scalar_result;
	zend_mir_representation transient_result_representation;
	zend_mir_scalar_type_mask transient_result_exact_type;
	zend_tpde_machine_value_kind transient_result_machine_kind;
	zend_mir_storage_id transient_result_storage_id;
	zend_mir_storage_id mutation_storage_id;
	bool mutation_lazy_scalar;
	zend_native_runtime_helper_id runtime_helper;
	zend_mir_executable_value_ref value_operation;
	bool has_value_operation;
	bool user_opcode_call_fragments;
	bool user_call_no_call;
	bool direct_scalar_return;
	zend_mir_scalar_type_mask direct_scalar_return_type;
	uint32_t direct_scalar_return_offset;
	uint32_t source_opline_index;
	uint32_t dynamic_fetch_cv_index;
	bool dynamic_fetch_direct_long;
	uint32_t materialization_offset;
	uint32_t materialization_count;
	zend_tpde_source_value_binding source_op1_binding;
	zend_tpde_source_value_binding source_op2_binding;
	zend_tpde_source_value_binding source_op2_definition_binding;
	zend_tpde_source_value_binding source_result_binding;
	zend_tpde_source_value_binding source_auxiliary_binding;
	uint32_t source_op2_definition_ssa_variable_id_plus_one;
	bool source_op2_canonical_scalar_only;
	uint32_t source_op1_reference_index;
	uint32_t source_op2_reference_index;
	uint32_t source_result_reference_index;
	uint32_t source_auxiliary_reference_index;
	uint32_t operation_reference_index;
	bool local_abi_transport;
	uint8_t machine_control_flow_flags;
};

struct zend_tpde_array_read {
	uint32_t container_offset;
	uint32_t key_offset;
	uint32_t result_offset;
	bool container_literal;
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
	bool is_empty;
};

struct zend_tpde_string_length {
	uint32_t operand_offset;
	uint32_t result_offset;
};

struct zend_tpde_bool_unary {
	uint32_t operand_offset;
	uint32_t result_offset;
	bool negate;
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
	uint32_t result_offset;
	uint32_t source_opcode;
	bool has_result;
};

struct zend_tpde_slot_isset_empty {
	uint32_t operand_offset;
	uint32_t result_offset;
	bool is_empty;
};

struct zend_tpde_packed_iterator_fetch {
	uint32_t holder_offset;
	uint32_t destination_offset;
	uint32_t key_offset;
	bool has_key;
	bool destination_scalar_only;
};

struct zend_tpde_array_iterator_reset {
	uint32_t source_offset;
	uint32_t holder_offset;
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
	uint32_t cv_index;
	bool direct_long;
};

struct zend_tpde_source_opcode {
	uint8_t opcode;
	uint8_t op1_type;
	uint8_t op2_type;
	uint8_t result_type;
	uint32_t op1_var;
	uint32_t op2_var;
	uint32_t result_var;
	uint32_t extended_value;
};

struct zend_tpde_multi_branch_case {
	int64_t integer_key;
	char *string_key;
	uint32_t string_length;
	uint32_t target;
};

struct zend_tpde_source_multi_branch {
	uint32_t case_offset;
	uint32_t case_count;
	uint32_t default_target;
	uint32_t fallback_target;
	uint32_t constant_successor;
	uint8_t source_opcode;
	bool valid;
};

struct zend_tpde_multi_branch {
	uint32_t operand_offset;
	uint32_t source_opcode;
	uint32_t successor_count;
	uint32_t constant_successor;
	const zend_tpde_multi_branch_case *cases;
	uint32_t case_count;
};

struct zend_tpde_user_multi_branch {
	uint32_t operand_offset;
	uint32_t target_opcode;
	uint32_t default_target;
	uint32_t fallback_target;
	const zend_tpde_multi_branch_case *cases;
	uint32_t case_count;
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
	const zend_tpde_multi_branch_case *branch_cases,
	uint32_t branch_case_count,
	std::vector<zend_tpde_integer_case> *cases,
	int64_t *low,
	uint64_t *range)
{
	if ((branch_cases == nullptr && branch_case_count != 0) || cases == nullptr
			|| low == nullptr || range == nullptr) {
		return ZEND_TPDE_INTEGER_DISPATCH_LINEAR;
	}
	cases->clear();
	cases->reserve(branch_case_count);
	for (uint32_t label_index = 0;
			label_index < branch_case_count; ++label_index) {
		if (branch_cases[label_index].string_key == nullptr) {
			cases->push_back({
				branch_cases[label_index].integer_key, label_index});
		}
	}
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
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.kind
					!= ZEND_MIR_SOURCE_OPERAND_LITERAL)
			|| operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| (operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1_storage_id == ZEND_MIR_ID_INVALID)
			|| (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.op1_storage_id != ZEND_MIR_ID_INVALID)
			|| operation.op2_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| (operation.op1_storage_id != ZEND_MIR_ID_INVALID
				&& operation.op1_storage_id == operation.op2_storage_id)
			|| (operation.op1_storage_id != ZEND_MIR_ID_INVALID
				&& operation.op1_storage_id
					== operation.result_storage_id)
			|| operation.op2_storage_id == operation.result_storage_id) {
		return false;
	}
	out->container_literal =
		operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
	container_offset = out->container_literal
		? 0
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
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
	zend_tpde_array_isset *out,
	bool allow_empty = false)
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
			|| (!allow_empty
				&& (operation.extended_value & ZEND_ISEMPTY) != 0)
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
	out->is_empty = (operation.extended_value & ZEND_ISEMPTY) != 0;
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
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
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

static inline bool zend_tpde_bool_unary_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_bool_unary *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
			|| (operation.source_opcode != ZEND_BOOL
				&& operation.source_opcode != ZEND_BOOL_NOT)
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
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
	out->negate = operation.source_opcode == ZEND_BOOL_NOT;
	return true;
}

static inline bool zend_tpde_value_condition_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_value_condition *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset = 0;
	const bool has_result = operation.source_opcode == ZEND_JMPZ_EX
		|| operation.source_opcode == ZEND_JMPNZ_EX;

	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
			|| (operation.source_opcode != ZEND_JMPZ
				&& operation.source_opcode != ZEND_JMPNZ
				&& operation.source_opcode != ZEND_JMPZ_EX
				&& operation.source_opcode != ZEND_JMPNZ_EX)
			|| (!has_result
				&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV)
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| (has_result
				&& operation.result_storage_id == ZEND_MIR_ID_INVALID)) {
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
	out->source_opcode = operation.source_opcode;
	out->has_result = has_result;
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
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| !zend_tpde_long_operand_at(
				operation.op1, operation.op1_storage_id, &out->left)
			|| !zend_tpde_long_operand_at(
				operation.op2, operation.op2_storage_id, &out->right)
			/* DFA may rewrite an in-place ASSIGN_OP into a binary opcode whose
			 * CV result aliases one input. The guarded fast path loads both inputs
			 * and verifies the aliased destination before writing its canonical
			 * slot. Other CV destinations retain the complete runtime primitive. */
			|| (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				&& (out->left.literal
					|| operation.op1_storage_id
						!= operation.result_storage_id)
				&& (out->right.literal
					|| operation.op2_storage_id
						!= operation.result_storage_id))
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& ((!out->left.literal
						&& operation.op1_storage_id
							== operation.result_storage_id)
					|| (!out->right.literal
						&& operation.op2_storage_id
							== operation.result_storage_id)))) {
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
			&& operation.op2_storage_id != ZEND_MIR_ID_INVALID) {
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
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID) {
		return false;
	}
	has_result = operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED;
	if (has_result
			&& ((operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& operation.result.kind
						!= ZEND_MIR_SOURCE_OPERAND_SSA)
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

static inline bool zend_tpde_packed_iterator_fetch_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_packed_iterator_fetch *out,
	bool allow_key = false)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t holder_offset;
	uint64_t destination_offset;
	uint64_t key_offset = 0;
	const bool has_key = operation.result.kind
		!= ZEND_MIR_SOURCE_OPERAND_UNUSED;

	/*
	 * The default layout deliberately covers only the value-only, by-value
	 * Zend foreach shape.  Targets may opt into the key temporary after they
	 * implement the ownership rules for string keys.
	 */
	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_ITERATOR_BRANCH
			|| operation.source_opcode != ZEND_FE_FETCH_R
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| (operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| (has_key
				&& (!allow_key
					|| (operation.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operation.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| (operation.result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_TMP
						&& operation.result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_VAR)))
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2_storage_id == ZEND_MIR_ID_INVALID
			|| (has_key
				? operation.result_storage_id == ZEND_MIR_ID_INVALID
				: operation.result_storage_id != ZEND_MIR_ID_INVALID)
			|| operation.op1_storage_id == operation.op2_storage_id) {
		return false;
	}
	if (has_key
			&& (operation.result_storage_id == operation.op1_storage_id
				|| operation.result_storage_id == operation.op2_storage_id)) {
		return false;
	}
	holder_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	destination_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
			* sizeof(zval);
	if (has_key) {
		key_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
	}
	if (holder_offset > UINT32_MAX || destination_offset > UINT32_MAX
			|| key_offset > UINT32_MAX) {
		return false;
	}
	out->holder_offset = static_cast<uint32_t>(holder_offset);
	out->destination_offset = static_cast<uint32_t>(destination_offset);
	out->key_offset = static_cast<uint32_t>(key_offset);
	out->has_key = has_key;
	out->destination_scalar_only =
		instruction.source_op2_canonical_scalar_only;
	return true;
}

static inline bool zend_tpde_array_iterator_reset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_iterator_reset *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t source_offset;
	uint64_t holder_offset;

	/*
	 * A direct CV array can use the ordinary FE_RESET_R copy semantics in the
	 * generated entry. References, temporaries, objects and by-reference
	 * iteration retain the complete runtime primitive.
	 */
	if (out == nullptr || !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_ITERATOR_BRANCH
			|| operation.source_opcode != ZEND_FE_RESET_R
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op1_storage_id == operation.result_storage_id) {
		return false;
	}
	source_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	holder_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (source_offset > UINT32_MAX || holder_offset > UINT32_MAX) {
		return false;
	}
	out->source_offset = static_cast<uint32_t>(source_offset);
	out->holder_offset = static_cast<uint32_t>(holder_offset);
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
	out->cv_index = instruction.dynamic_fetch_cv_index;
	out->direct_long = instruction.dynamic_fetch_direct_long;
	return true;
}

struct zend_tpde_machine_cfg {
	uint32_t block_count;
	uint32_t successor_count;
	uint32_t *successor_offsets;
	uint32_t *successors;
	uint32_t *instruction_blocks;
	uint32_t *guarded_cold_blocks;
	uint32_t *guarded_hot_blocks;
	uint32_t *guarded_continuation_blocks;
	uint32_t *final_blocks;
	uint32_t *boxed_cond_cold_blocks;
	uint32_t *boxed_cond_cold_by_predecessor;
};

struct zend_tpde_plan {
	const zend_native_runtime_api *runtime;
	uint32_t source_ssa_variable_count;
	uint32_t source_opcode_count;
	zend_tpde_source_opcode *source_opcodes;
	zend_tpde_source_multi_branch *source_multi_branches;
	zend_tpde_multi_branch_case *source_multi_branch_cases;
	uint32_t source_multi_branch_case_count;
	uint32_t source_frame_variable_count;
	uint32_t source_temporary_count;
	uint32_t source_block_count;
	uint32_t *source_opcode_block_indices;
	uint8_t *source_opcode_is_data;
	uint32_t *source_block_starts;
	uint32_t *source_block_ends;
	zend_tpde_source_call_phase_entry *source_call_phases;
	uint32_t source_call_phase_count;
	uint8_t *compiled_variables_used;
	uint32_t compiled_variable_count;
	uint32_t symbol_namespace;
	uint32_t wrapper_function_index;
	uint32_t typed_body_function_index;
	zend_mir_function_record function;
	zend_mir_block_id *block_ids;
	uint32_t block_count;
	zend_tpde_id_index_entry *block_index;
	uint32_t block_index_capacity;
	uint32_t *block_successor_offsets;
	uint32_t *block_successors;
	uint32_t block_successor_count;
	uint32_t *block_predecessor_offsets;
	uint32_t *block_predecessors;
	uint32_t block_predecessor_count;
	zend_tpde_value *values;
	uint32_t value_count;
	int32_t *argument_value_indices;
	zend_tpde_local_abi_type *argument_abi;
	zend_tpde_local_abi_type return_abi;
	zend_tpde_local_abi_type typed_body_return_abi;
	uint8_t *typed_component_call_eligible;
	uint8_t *effect_closed_inline_eligible;
	bool typed_body_eligible;
	bool has_register_component_results;
	zend_tpde_machine_cfg entry_machine_cfg;
	zend_tpde_machine_cfg typed_body_machine_cfg;
	zend_tpde_id_index_entry *value_index;
	uint32_t value_index_capacity;
	zend_tpde_instruction *instructions;
	uint32_t instruction_count;
	zend_mir_value_id *instruction_operands;
	zend_tpde_operand_transport *instruction_operand_transports;
	uint32_t instruction_operand_count;
	zend_tpde_id_index_entry *instruction_index;
	uint32_t instruction_index_capacity;
	int32_t *value_definition_instructions;
	int32_t *source_value_definition_instructions;
	uint32_t *value_consumer_offsets;
	zend_tpde_machine_use *value_consumers;
	uint32_t value_consumer_count;
	uint8_t *entry_value_required;
	uint8_t *typed_body_value_required;
	zend_tpde_id_index_entry *call_site_instruction_index;
	uint32_t call_site_instruction_index_capacity;
	uint32_t call_site_count;
	zend_mir_call_site_ref *call_sites;
	zend_tpde_id_index_entry *call_target_index;
	uint32_t call_target_index_capacity;
	uint32_t call_target_count;
	uint32_t call_argument_count;
	zend_mir_call_argument_ref *call_arguments;
	zend_tpde_source_value_binding *call_argument_bindings;
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
	zend_tpde_machine_reference *machine_references;
	uint32_t machine_reference_count;
	uint32_t observers_enabled_reference_index;
	uint32_t *entry_undef_temporary_indices;
	uint32_t entry_undef_temporary_count;
	bool may_emit_calls;
	bool zend_entry_may_emit_calls;
	bool typed_body_may_emit_calls;
	bool zend_entry_needs_unwind;
	bool typed_body_needs_unwind;
	bool user_opcode_callbacks;
};

uint32_t zend_tpde_block_successor_count(
	const zend_tpde_plan *plan, zend_mir_block_id id);
bool zend_tpde_block_successor_at(
	const zend_tpde_plan *plan,
	zend_mir_block_id id,
	uint32_t successor_index,
	zend_mir_block_id *out);

static inline const zend_tpde_source_call_phase_entry *
zend_tpde_source_call_phase_at(
	const zend_tpde_plan *plan, uint32_t source_position)
{
	if (plan == nullptr || plan->source_call_phases == nullptr
			|| source_position >= plan->source_opcode_count) {
		return nullptr;
	}
	const zend_tpde_source_call_phase_entry *entry =
		&plan->source_call_phases[source_position];
	return entry->phases == ZEND_TPDE_SOURCE_CALL_PHASE_NONE ? nullptr : entry;
}

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
	if (plan == nullptr || out == nullptr
			|| plan->source_multi_branches == nullptr
			|| operation.source_position_id >= plan->source_opcode_count
			|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| (target_opcode != ZEND_SWITCH_LONG
				&& target_opcode != ZEND_SWITCH_STRING
				&& target_opcode != ZEND_MATCH)) {
		return false;
	}
	const zend_tpde_source_multi_branch &branch =
		plan->source_multi_branches[operation.source_position_id];
	if (!branch.valid) {
		return false;
	}
	const uint64_t operand_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (operand_offset > UINT32_MAX || branch.default_target == UINT32_MAX
			|| (target_opcode != ZEND_MATCH
				&& branch.fallback_target == UINT32_MAX)) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->target_opcode = target_opcode;
	out->default_target = branch.default_target;
	out->fallback_target = branch.fallback_target;
	out->cases = branch.case_count == 0
		? nullptr
		: plan->source_multi_branch_cases + branch.case_offset;
	out->case_count = branch.case_count;
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
	uint64_t operand_offset;
	uint32_t expected_successors;

	if (plan == nullptr || out == nullptr
			|| !instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
			|| record.opcode != ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
			|| plan->source_multi_branches == nullptr
			|| operation.source_position_id >= plan->source_opcode_count
			|| operation.source_position_id != record.source_position_id
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return false;
	}
	const zend_tpde_source_multi_branch &branch =
		plan->source_multi_branches[operation.source_position_id];
	if (!branch.valid || branch.source_opcode != operation.source_opcode) {
		return false;
	}
	expected_successors = branch.case_count
		+ (branch.source_opcode == ZEND_MATCH ? 1 : 2);
	const bool constant_operand =
		operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
		&& branch.constant_successor < expected_successors;
	if (expected_successors < 2
			|| (!constant_operand
				&& operation.op1_storage_id == ZEND_MIR_ID_INVALID)
			|| zend_tpde_block_successor_count(
				plan, record.block_id) != expected_successors) {
		return false;
	}
	operand_offset = constant_operand ? 0
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (operand_offset > UINT32_MAX) {
		return false;
	}
	out->operand_offset = static_cast<uint32_t>(operand_offset);
	out->source_opcode = branch.source_opcode;
	out->successor_count = expected_successors;
	out->constant_successor = constant_operand
		? branch.constant_successor : UINT32_MAX;
	out->cases = branch.case_count == 0
		? nullptr
		: plan->source_multi_branch_cases + branch.case_offset;
	out->case_count = branch.case_count;
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
zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index);

struct zend_tpde_scalar_diamond {
	uint32_t entry;
	uint32_t true_block;
	uint32_t false_block;
	uint32_t merge;
};

/*
 * Recognize the smallest acyclic multi-block scalar body.  Successor order is
 * semantically significant: COND_BRANCH reaches successor zero when true and
 * successor one when false, while PHI operands follow predecessor order.
 */
static inline bool zend_tpde_scalar_diamond_at(
		const zend_tpde_plan *plan,
		zend_tpde_scalar_diamond *out)
{
	if (plan == nullptr || out == nullptr || plan->block_count != 4
			|| plan->block_ids == nullptr
			|| plan->block_successor_offsets == nullptr
			|| plan->block_successors == nullptr
			|| plan->block_predecessor_offsets == nullptr
			|| plan->block_predecessors == nullptr) {
		return false;
	}
	const int32_t entry_index =
		zend_tpde_block_index(plan, plan->function.entry_block_id);
	if (entry_index < 0) {
		return false;
	}
	const uint32_t entry = static_cast<uint32_t>(entry_index);
	const uint32_t entry_successor_begin =
		plan->block_successor_offsets[entry];
	if (plan->block_successor_offsets[entry + 1]
				- entry_successor_begin != 2) {
		return false;
	}
	const uint32_t true_block =
		plan->block_successors[entry_successor_begin];
	const uint32_t false_block =
		plan->block_successors[entry_successor_begin + 1];
	if (true_block >= plan->block_count || false_block >= plan->block_count
			|| true_block == false_block || true_block == entry
			|| false_block == entry) {
		return false;
	}
	const uint32_t true_successor_begin =
		plan->block_successor_offsets[true_block];
	const uint32_t false_successor_begin =
		plan->block_successor_offsets[false_block];
	if (plan->block_successor_offsets[true_block + 1]
				- true_successor_begin != 1
			|| plan->block_successor_offsets[false_block + 1]
				- false_successor_begin != 1) {
		return false;
	}
	const uint32_t merge = plan->block_successors[true_successor_begin];
	if (merge >= plan->block_count
			|| plan->block_successors[false_successor_begin] != merge
			|| merge == entry || merge == true_block || merge == false_block
			|| plan->block_successor_offsets[merge + 1]
				!= plan->block_successor_offsets[merge]) {
		return false;
	}
	auto has_single_predecessor = [&](uint32_t block, uint32_t predecessor) {
		const uint32_t begin = plan->block_predecessor_offsets[block];
		return plan->block_predecessor_offsets[block + 1] - begin == 1
			&& plan->block_predecessors[begin] == predecessor;
	};
	if (!has_single_predecessor(true_block, entry)
			|| !has_single_predecessor(false_block, entry)) {
		return false;
	}
	const uint32_t merge_predecessor_begin =
		plan->block_predecessor_offsets[merge];
	if (plan->block_predecessor_offsets[merge + 1]
				- merge_predecessor_begin != 2) {
		return false;
	}
	const uint32_t first =
		plan->block_predecessors[merge_predecessor_begin];
	const uint32_t second =
		plan->block_predecessors[merge_predecessor_begin + 1];
	if (!((first == true_block && second == false_block)
			|| (first == false_block && second == true_block))) {
		return false;
	}
	*out = {entry, true_block, false_block, merge};
	return true;
}

/*
 * A scalar diamond may materialize an unaliased scalar SSA identity into its
 * source-level zval carrier.  The cloned register body has no callee frame,
 * so this transport must disappear with that frame.  Restrict the omission
 * to self-identical, unaliased, non-refcounted scalar stores; assignments and
 * reference-visible stores remain observable and reject inlining.
 */
static inline bool zend_tpde_scalar_diamond_frame_transport(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction)
{
	if (plan == nullptr
			|| instruction.record.opcode != ZEND_MIR_OPCODE_ZVAL_STORE
			|| instruction.operand_count != 2
			|| !zend_mir_id_is_valid(instruction.zval_store_storage_id)) {
		return false;
	}
	const zend_mir_value_id source_id =
		zend_tpde_operand_at(plan, &instruction, 0);
	const zend_mir_value_id destination_id =
		zend_tpde_operand_at(plan, &instruction, 1);
	if (source_id != destination_id) {
		return false;
	}
	const int32_t source_index = zend_tpde_value_index(
		plan, source_id);
	const int32_t destination_index = zend_tpde_value_index(
		plan, destination_id);
	if (source_index < 0 || destination_index != source_index) {
		return false;
	}
	const zend_tpde_value &source = plan->values[source_index];
	const zend_tpde_value &destination = plan->values[destination_index];
	return (source.exact_type == ZEND_MIR_SCALAR_TYPE_I1
			|| source.exact_type == ZEND_MIR_SCALAR_TYPE_I64)
		&& destination.exact_type == source.exact_type
		&& source.category == ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
		&& destination.category == ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
		&& !source.canonical_alias_observable
		&& !destination.canonical_alias_observable
		&& destination.canonical_storage_id
			== instruction.zval_store_storage_id;
}

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
