// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"
#include "Zend/Native/Lowering/zend_mir_dominator_forest.h"
#include "Zend/Native/MIR/zend_mir_call.h"
#include "Zend/Native/MIR/zend_mir_values.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_operands.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_execute.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

struct _zend_native_direct_call_descriptor;
struct _zend_native_direct_internal_call_descriptor;
struct _zend_native_user_call_descriptor;

extern "C" zend_mir_opcode zend_mir_executable_opcode(uint32_t opcode);

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

struct zend_tpde_machine_part_desc {
	zend_tpde_machine_part_role semantic_role;
	uint16_t bit_width;
	zend_tpde_machine_register_bank register_bank;
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

/* The Z_TYPE() of an exact scalar type, or IS_UNDEF. */
inline uint32_t zend_tpde_zval_type(zend_mir_scalar_type_mask type)
{
	switch (type) {
		case ZEND_MIR_SCALAR_TYPE_NULL: return IS_NULL;
		case ZEND_MIR_SCALAR_TYPE_I1: return IS_FALSE;
		case ZEND_MIR_SCALAR_TYPE_I64: return IS_LONG;
		case ZEND_MIR_SCALAR_TYPE_F64: return IS_DOUBLE;
		default: return IS_UNDEF;
	}
}

inline bool operator==(const zend_tpde_local_abi_type &left,
	const zend_tpde_local_abi_type &right)
{
	return left.valid == right.valid
		&& left.representation == right.representation
		&& left.exact_type == right.exact_type
		&& left.machine_kind == right.machine_kind
		&& left.transfer == right.transfer;
}

/* A scalar value of ABI value that boxes into the boxed zval ABI callee. */
inline bool zend_tpde_abi_boxes_into(const zend_tpde_local_abi_type &value,
	const zend_tpde_local_abi_type &callee)
{
	return value.valid && callee.valid
		&& callee.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
		&& callee.representation == ZEND_MIR_REPRESENTATION_ZVAL
		&& value.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE
		&& ((value.exact_type == ZEND_MIR_SCALAR_TYPE_I64
				&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_I64)
			|| (value.exact_type == ZEND_MIR_SCALAR_TYPE_F64
				&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_F64)
			|| (value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL));
}

/* A long, a double or a boolean held unboxed. */
inline bool zend_tpde_machine_kind_is_scalar(zend_tpde_machine_value_kind kind)
{
	return kind == ZEND_TPDE_MACHINE_VALUE_I64
		|| kind == ZEND_TPDE_MACHINE_VALUE_F64
		|| kind == ZEND_TPDE_MACHINE_VALUE_BOOL;
}

/* A long or a double held unboxed. */
inline bool zend_tpde_machine_kind_is_number(zend_tpde_machine_value_kind kind)
{
	return kind == ZEND_TPDE_MACHINE_VALUE_I64
		|| kind == ZEND_TPDE_MACHINE_VALUE_F64;
}

/* A long or a boolean held unboxed in a general-purpose register. */
inline bool zend_tpde_machine_kind_is_long_or_bool(
	zend_tpde_machine_value_kind kind)
{
	return kind == ZEND_TPDE_MACHINE_VALUE_I64
		|| kind == ZEND_TPDE_MACHINE_VALUE_BOOL;
}

/* A pointer to a string, an array, an object or a resource. */
inline bool zend_tpde_machine_kind_is_value_pointer(
	zend_tpde_machine_value_kind kind)
{
	return kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR;
}

/* A value pointer or a pointer to a zend_reference. */
inline bool zend_tpde_machine_kind_is_pointer(zend_tpde_machine_value_kind kind)
{
	return zend_tpde_machine_kind_is_value_pointer(kind)
		|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
}

/* The unboxed representation of an exact scalar type other than null. */
inline zend_mir_representation zend_tpde_scalar_representation(
	zend_mir_scalar_type_mask exact_type)
{
	return exact_type == ZEND_MIR_SCALAR_TYPE_I1
			? ZEND_MIR_REPRESENTATION_I1
		: exact_type == ZEND_MIR_SCALAR_TYPE_F64
			? ZEND_MIR_REPRESENTATION_DOUBLE
			: ZEND_MIR_REPRESENTATION_I64;
}

/* The machine kind of an exact scalar type other than null. */
inline zend_tpde_machine_value_kind zend_tpde_scalar_machine_kind(
	zend_mir_scalar_type_mask exact_type)
{
	return exact_type == ZEND_MIR_SCALAR_TYPE_I1
			? ZEND_TPDE_MACHINE_VALUE_BOOL
		: exact_type == ZEND_MIR_SCALAR_TYPE_F64
			? ZEND_TPDE_MACHINE_VALUE_F64
			: ZEND_TPDE_MACHINE_VALUE_I64;
}

/* The local ABI of an exact scalar other than null, passed unboxed. */
inline zend_tpde_local_abi_type zend_tpde_scalar_local_abi(
	zend_mir_scalar_type_mask exact_type)
{
	return {
		zend_tpde_scalar_representation(exact_type),
		exact_type,
		zend_tpde_scalar_machine_kind(exact_type),
		ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE,
		true,
	};
}

static inline zend_tpde_machine_representation_desc
zend_tpde_machine_representation(
	zend_tpde_machine_value_kind kind, bool register_authoritative)
{
	static constexpr zend_tpde_machine_part_desc gp_value[] = {{
		ZEND_TPDE_MACHINE_PART_VALUE, 64, ZEND_TPDE_MACHINE_REGISTER_GP}};
	static constexpr zend_tpde_machine_part_desc fp_value[] = {{
		ZEND_TPDE_MACHINE_PART_VALUE, 64, ZEND_TPDE_MACHINE_REGISTER_FP}};
	static constexpr zend_tpde_machine_part_desc boxed_zval[] = {
		{ZEND_TPDE_MACHINE_PART_PAYLOAD, 64,
			ZEND_TPDE_MACHINE_REGISTER_GP},
		{ZEND_TPDE_MACHINE_PART_TYPE_INFO, 32,
			ZEND_TPDE_MACHINE_REGISTER_GP}};

	if (kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& register_authoritative) {
		return {2, boxed_zval};
	}
	return {1, kind == ZEND_TPDE_MACHINE_VALUE_F64 ? fp_value : gp_value};
}

enum zend_tpde_machine_location : uint8_t {
	ZEND_TPDE_MACHINE_LOCATION_REGISTER = 0,
	ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT = 2,
};

enum zend_tpde_canonical_slot_state : uint8_t {
	ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED = 0,
	ZEND_TPDE_CANONICAL_SLOT_CLEAN = 1,
	ZEND_TPDE_CANONICAL_SLOT_DIRTY = 2,
};

uint32_t zend_tpde_machine_value_zval_type(
	zend_tpde_machine_value_kind kind);

uint32_t zend_tpde_machine_value_zval_type_info(
	zend_tpde_machine_value_kind kind);

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

/*
 * A source operand as runtime helpers take it (zend_native_operands.h):
 * the VM operand type in bits 0-7 and the znode_op value in bits 8-39, the
 * frame byte offset of a variable, the literal index of a constant or the
 * payload of an unused operand. Temporaries follow the
 * frame_variable_count CVs.
 */
uint64_t zend_tpde_encode_value_operand(
	const zend_mir_source_operand_ref &operand, uint32_t frame_variable_count,
	uint32_t unused_payload);

/*
 * Runtime-helper argument shape is target-neutral: Darwin and Linux share
 * this classification so they emit the same C ABI for a MIR operation.
 */
bool zend_tpde_helper_has_explicit_operands(
	zend_native_runtime_helper_id helper);

bool zend_tpde_helper_has_unused_operand_payloads(
	zend_native_runtime_helper_id helper);

bool zend_tpde_helper_has_explicit_auxiliary(
	zend_native_runtime_helper_id helper);

/*
 * The bits an explicit auxiliary operand carries above the operand
 * encoding: the OP_DATA's run-time cache offset of ASSIGN_OBJ_OP and
 * ASSIGN_STATIC_PROP_OP (ZEND_NATIVE_AUXILIARY_CACHE_OFFSET), so their
 * helpers do not read the OP_DATA.
 */
uint64_t zend_tpde_auxiliary_cache_bits(
	zend_native_runtime_helper_id helper,
	const zend_mir_executable_value_ref &operation);

/*
 * Selective frame initialization leaves an unobserved TMP/VAR slot
 * uninitialized until a boundary materializes a result there, so explicit
 * helpers that define a new Zend result need a fresh result zval. Not so
 * ADD_ARRAY_*: their result is the accumulator INIT_ARRAY created. A result
 * storage aliasing a source is an in-place operation and keeps its input.
 */
bool zend_tpde_helper_requires_undef_result(
	zend_native_runtime_helper_id helper,
	const zend_mir_executable_value_ref &operation);

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
	 * Source operations such as ZEND_RECV and a proven return-type check
	 * create a new Zend SSA identity without changing the machine value; the
	 * adaptor follows this alias instead of reloading through the frame.
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
	 * ZNMIR result identity; its source definition connects it to its
	 * canonical slot at the first observing runtime boundary.
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
 * Pointer-free address-selection facts for the adaptor. Values name ZNMIR
 * identities; UINT32_MAX denotes a synthetic base such as the canonical
 * frame or the execution context.
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
 * transition; the adaptor must not infer a scalar representation from an
 * opcode or from source SSA.
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

/* The two machine functions of a member: Zend entry and typed body. */
inline constexpr uint8_t ZEND_TPDE_MACHINE_MODE_ENTRY = 1u << 0;
inline constexpr uint8_t ZEND_TPDE_MACHINE_MODE_TYPED_BODY = 1u << 1;

constexpr uint8_t zend_tpde_machine_mode(bool typed_body) {
	return typed_body
		? ZEND_TPDE_MACHINE_MODE_TYPED_BODY : ZEND_TPDE_MACHINE_MODE_ENTRY;
}

struct zend_tpde_instruction {
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
	 * Compile-time type guards for inline-frame arguments, never serialized
	 * into the direct-call descriptor. By-reference entries describe the
	 * referent; by-value entries prove a canonical boxed source before it is
	 * transported as an exact scalar.
	 */
	zend_mir_scalar_type_mask *direct_call_argument_guard_types;
	_zend_native_direct_internal_call_descriptor *direct_internal_call;
	zend_mir_scalar_type_mask source_effect_exact_type;
	bool debug_probe;
	zend_mir_storage_id zval_store_storage_id;
	bool zval_store_direct_scalar;
	bool zval_store_lazy_scalar;
	/* The store needs no release of the destination's previous value: an
	 * unguarded payload and type store. */
	bool zval_store_plain;
	/* Both operands of this binary operation are numbers: known from type
	 * inference or results of component members that return numbers. */
	bool numeric_operands_proven;
	/* A direct call whose target has a numeric variant: 1 + the variant's
	 * component index, the general target and the checked arguments. The
	 * component decides the target once it knows which arguments are
	 * numbers. */
	uint32_t numeric_variant_plus_one;
	uint32_t numeric_variant_general;
	uint32_t numeric_variant_mask;
	/* A numeric comparison whose only consumer is the JMPZ/JMPNZ right
	 * after it: in a Zend entry the branch evaluates it and jumps to its
	 * targets directly (1 + the comparison's index on the branch). */
	bool fused_into_branch;
	uint32_t fused_compare_plus_one;
	/* A property or element read whose only consumer is the array read
	 * right after it, which uses the value in place: in a Zend entry the
	 * consumer evaluates it and its temporary only exists on the way to
	 * the helpers (1 + the producer's index on the consumer). */
	bool fused_into_container;
	uint32_t fused_container_plus_one;
	/* A Zend-entry typed call whose body may fail: a failed call takes the
	 * canonical cold call instead. */
	bool typed_call_may_fail;
	/* A specialized member's guarded operation whose failed guard
	 * deoptimizes into the generic copy: 1 + its resume index there. */
	uint32_t deopt_exit_resume_plus_one;
	/* ZEND_TPDE_MACHINE_MODE_* bits of the modes whose conditional branch
	 * tests the register condition instead of the boxed value. */
	uint8_t register_cond_branch_modes;
	/*
	 * A slot-authoritative exact scalar may still need a short-lived machine
	 * definition for the ZVAL_STORE that publishes it. This is kept apart
	 * from register authority so alias-observable CVs are never treated as
	 * register-resident after publication.
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
	/* The op1 CV can never hold a reference: it has no alias, no call can
	 * rebind it (see zend_tpde_ssa_variable_rebindable) and inference
	 * excludes MAY_BE_REF, so its value lives in the slot itself. */
	bool source_op1_reference_free;
	/* The SSA value the op1/op2 CV operand reads may be a reference (bound by
	 * global, static or &, or rebindable by any call), so its slot holds
	 * the reference rather than the value. */
	bool source_op1_may_reference;
	bool source_op2_may_reference;
	uint32_t source_op1_reference_index;
	uint32_t source_op2_reference_index;
	uint32_t operation_reference_index;
	bool local_abi_transport;
	uint8_t machine_control_flow_flags;
	/* A REGISTER_MERGE branch: 1 + the instruction defining its temporary
	 * at the end of each of its two predecessors, in predecessor order. */
	uint32_t register_merge_producer_plus_one[2];
};

/* The slot of a value operation's CV operand that may be a reference holds
 * that reference, which the assignment of the value already wrote through:
 * a cold path consumes the operand without publishing it into the slot. */
bool zend_tpde_reference_cv_operand_slot(
	const zend_tpde_instruction &instruction, zend_mir_storage_id storage);

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
	/* The container is a VAR holding an INDIRECT to the array's zval, as
	 * a write fetch leaves it ($a[$k][] = $v); the append consumes it. */
	bool indirect_container;
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
	bool indirect;
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
	bool source_literal;
	/* A temporary array moves into the holder. */
	bool source_temporary;
	uint32_t source_literal_index;
};

struct zend_tpde_object_property_read {
	uint32_t receiver_offset;
	uint32_t result_offset;
	uint32_t cache_offset;
};

struct zend_tpde_object_property_write {
	uint32_t receiver_offset;
	/* The value's frame offset, or its literal table offset. */
	uint32_t value_offset;
	uint32_t cache_offset;
	bool move_value;
	bool literal_value;
};

struct zend_tpde_dynamic_fetch_read {
	uint32_t name_offset;
	uint32_t result_offset;
	uint32_t cv_index;
	bool direct_long;
};

/* Known operand type: a long or a double, never undefined or a reference. */
inline constexpr uint8_t ZEND_TPDE_KNOWN_NUMBER = 0xfe;
/* Known operand type: false or true. */
inline constexpr uint8_t ZEND_TPDE_KNOWN_BOOL = 0xfd;

struct zend_tpde_source_opcode {
	uint8_t opcode;
	uint8_t op1_type;
	uint8_t op2_type;
	uint8_t result_type;
	/* IS_LONG or IS_DOUBLE when a literal or Zend's type inference fixes
	 * the operand's type, ZEND_TPDE_KNOWN_NUMBER when inference proves a
	 * long or a double, IS_UNDEF otherwise. */
	uint8_t op1_known_type;
	uint8_t op2_known_type;
	/* Zend's inferred MAY_BE_* mask of op1 (literals: the bit of their
	 * type), UINT32_MAX when unknown. */
	uint32_t op1_may_be;
	/* The same for op2. */
	uint32_t op2_may_be;
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

zend_tpde_integer_dispatch_kind
zend_tpde_integer_dispatch(
	const zend_tpde_multi_branch_case *branch_cases,
	uint32_t branch_case_count,
	std::vector<zend_tpde_integer_case> *cases,
	int64_t *low,
	uint64_t *range);

/*
 * Which MIR shapes may skip the generic dimension primitive is decided here,
 * target-neutrally; target backends only encode the guards and loads.
 */
bool zend_tpde_array_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_read *out,
	bool allow_temporary_key = false);

/*
 * A frameless internal call whose result and arguments are frame slots or
 * literals calls zend_native_call_frameless_direct() with precomputed
 * offsets instead of encoded operands. A CV result is overwritten without a
 * release, as ZEND_FRAMELESS_ICALL_* does: the optimizer only names a CV
 * that holds no counted value. VAR arguments may be indirect and keep
 * the general helper.
 */
struct zend_tpde_frameless_direct {
	uint64_t descriptor;
	uint64_t slots;
	uint64_t more_slots;
};

bool zend_tpde_frameless_direct_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_frameless_direct *out);

/*
 * The operands of a comparison fused into its branch: a CV, a temporary or
 * a literal, and a temporary result distinct from both.
 */
struct zend_tpde_fused_operand {
	uint32_t offset;
	bool literal;
	bool temporary;
};

bool zend_tpde_fused_operand_at(
	const zend_mir_source_operand_ref &operand, zend_mir_storage_id storage,
	zend_mir_storage_id result_storage, zend_tpde_fused_operand *out);

bool zend_tpde_fused_compare_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_fused_operand *left, zend_tpde_fused_operand *right);

/*
 * A TYPE_CHECK of a CV into a temporary, fused into its JMPZ/JMPNZ like a
 * comparison: the branch tests the type mask itself. A resource check,
 * which asks the resource list, keeps its own form.
 */
bool zend_tpde_fused_type_check_at(
	const zend_tpde_instruction &instruction, zend_tpde_fused_operand *value);

/*
 * isset($cv) into a temporary, fused into its JMPZ/JMPNZ: the branch tests
 * the CV's type itself. isset() never warns.
 */
bool zend_tpde_fused_isset_cv_at(
	const zend_tpde_instruction &instruction, zend_tpde_fused_operand *value);

/*
 * === and !== of a CV or temporary and a literal or another CV or
 * temporary, fused into its JMPZ/JMPNZ like a comparison.
 */
bool zend_tpde_fused_identical_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_fused_operand *left, zend_tpde_fused_operand *right);

/*
 * isset($a[$k]) of an array CV or literal under a literal, CV or temporary
 * key, fused into its JMPZ/JMPNZ: the branch probes the element itself.
 */
bool zend_tpde_fused_isset_dim_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_fused_operand *container, zend_tpde_fused_operand *key);

/*
 * $cv[$key] into a temporary, and $cv[$key] = $value (or $cv[] = $value)
 * with an unused result, call the _direct helpers with precomputed offsets
 * (ZEND_NATIVE_DIM_DIRECT_*) instead of encoded operands.
 */
struct zend_tpde_dim_direct {
	uint64_t descriptor;
	uint64_t slots;
	uint64_t more_slots;
};

bool zend_tpde_dim_direct_operand(
	const zend_mir_source_operand_ref &operand, zend_mir_storage_id storage,
	bool allow_unused, uint64_t *kind, uint64_t *offset);

bool zend_tpde_dim_direct_at(
	const zend_tpde_instruction &instruction, zend_tpde_dim_direct *out);

/*
 * $cv .= value with a literal, CV or temporary value and an unused result
 * calls zend_native_value_concat_assign_direct() with precomputed offsets.
 */
struct zend_tpde_concat_assign_direct {
	uint64_t descriptor;
	uint64_t slots;
};

bool zend_tpde_concat_assign_direct_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_concat_assign_direct *out);

/*
 * ZEND_CONCAT/ZEND_FAST_CONCAT of literal, CV or temporary operands into a
 * temporary, or a CV the optimizer named (no counted old value), calls
 * zend_native_value_concat_direct() with precomputed kinds
 * (ZEND_NATIVE_DIM_DIRECT_*) and offsets.
 */
struct zend_tpde_concat_direct {
	uint64_t descriptor;
	uint64_t slots;
	uint64_t result_offset;
};

bool zend_tpde_concat_direct_operand(
	const zend_mir_source_operand_ref &operand, zend_mir_storage_id storage,
	uint64_t *kind, uint64_t *offset);

bool zend_tpde_concat_direct_at(
	const zend_tpde_instruction &instruction, zend_tpde_concat_direct *out);

/*
 * ZEND_IS_IDENTICAL/ZEND_IS_NOT_IDENTICAL of literal, CV or temporary
 * operands into a temporary or an optimizer-named CV calls
 * zend_native_value_identical_direct(), laid out as the direct
 * concatenation.
 */
bool zend_tpde_identical_direct_at(
	const zend_tpde_instruction &instruction, zend_tpde_concat_direct *out);

bool zend_tpde_packed_array_append_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_packed_array_append *out);

bool zend_tpde_array_isset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_isset *out,
	bool allow_empty = false);

bool zend_tpde_string_length_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_string_length *out,
	bool allow_ssa_result = false);

bool zend_tpde_bool_unary_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_bool_unary *out);

bool zend_tpde_value_condition_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_value_condition *out);

bool zend_tpde_string_identity_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_string_identity *out);

bool zend_tpde_long_operand_at(
	const zend_mir_source_operand_ref &operand,
	zend_mir_storage_id storage_id,
	zend_tpde_long_operand *out);

bool zend_tpde_long_binary_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_binary *out);

bool zend_tpde_long_assign_op_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_assign_op *out);

bool zend_tpde_long_incdec_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_incdec *out, bool allow_indirect = false);

bool zend_tpde_slot_isset_empty_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_slot_isset_empty *out);

bool zend_tpde_packed_iterator_fetch_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_packed_iterator_fetch *out,
	bool allow_key = false);

bool zend_tpde_array_iterator_reset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_iterator_reset *out,
	bool allow_literal = false);

bool zend_tpde_object_property_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out);

/*
 * FETCH_OBJ_IS of a literal property into a temporary: a cached declared
 * property holding a value reads like FETCH_OBJ_R; anything else, which
 * may be missing, magic or a reference, keeps the helper.
 */
bool zend_tpde_object_property_isset_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out);

/*
 * FETCH_OBJ_FUNC_ARG of a literal property into a VAR: an argument sent by
 * value reads the property like FETCH_OBJ_R; the emitted form checks the
 * call's ZEND_CALL_SEND_ARG_BY_REF and leaves a by-reference send to the
 * helper.
 */
bool zend_tpde_object_property_func_arg_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out);

/*
 * FETCH_OBJ_W of a literal property into a VAR: a cached untyped declared
 * property is addressed by an IS_INDIRECT result; anything else keeps the
 * helper.
 */
bool zend_tpde_object_property_write_fetch_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out);

bool zend_tpde_object_property_write_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_write *out);

bool zend_tpde_dynamic_fetch_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_dynamic_fetch_read *out);

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
	/*
	 * The immediate-dominator forest of the blocks: the entry's tree holds
	 * the blocks it reaches, every other block is a root of its own.
	 */
	zend_mir_dominator_forest dominators;
};

/*
 * Linux x64 lowers forms DarwinA64 does not: array reads through temporary
 * or literal keys, and inline call frames that move temporary arguments.
 * Each host builds only its own target backend.
 */
inline constexpr bool zend_tpde_linux_inline_forms =
	ZEND_NATIVE_HOST_TARGET == ZEND_NATIVE_TARGET_LINUX_AMD64;

namespace zend::native::tpde {
class ZendIRAdaptor;
}

/*
 * An array a plan owns, allocated with malloc(), calloc() or realloc() and
 * released with free() when the plan is destroyed. Reads see the plain
 * pointer; storing a new array goes through reset().
 */
template <typename T>
class zend_tpde_owned {
public:
	zend_tpde_owned() = default;
	zend_tpde_owned(const zend_tpde_owned &) = delete;
	zend_tpde_owned &operator=(const zend_tpde_owned &) = delete;
	zend_tpde_owned(zend_tpde_owned &&other) noexcept
		: pointer_{std::exchange(other.pointer_, nullptr)} {}
	zend_tpde_owned &operator=(zend_tpde_owned &&other) noexcept {
		reset(std::exchange(other.pointer_, nullptr));
		return *this;
	}
	~zend_tpde_owned() { std::free(pointer_); }

	operator T *() const { return pointer_; }
	T *get() const { return pointer_; }
	/* Frees the array held so far and takes pointer. */
	void reset(T *pointer = nullptr) {
		std::free(pointer_);
		pointer_ = pointer;
	}
	/* Takes pointer, which realloc() returned for the array held so far. */
	void reallocated(T *pointer) { pointer_ = pointer; }

private:
	T *pointer_ = nullptr;
};

struct zend_tpde_plan {
	uint32_t source_ssa_variable_count;
	uint32_t source_opcode_count;
	zend_tpde_owned<zend_tpde_source_opcode> source_opcodes;
	zend_tpde_owned<zend_tpde_source_multi_branch> source_multi_branches;
	zend_tpde_owned<zend_tpde_multi_branch_case> source_multi_branch_cases;
	uint32_t source_multi_branch_case_count;
	uint32_t source_frame_variable_count;
	uint32_t source_temporary_count;
	uint32_t source_block_count;
	zend_tpde_owned<uint32_t> source_opcode_block_indices;
	zend_tpde_owned<uint8_t> source_opcode_is_data;
	zend_tpde_owned<uint32_t> source_block_starts;
	zend_tpde_owned<uint32_t> source_block_ends;
	zend_tpde_owned<zend_tpde_source_call_phase_entry> source_call_phases;
	uint32_t source_call_phase_count;
	zend_tpde_owned<uint8_t> compiled_variables_used;
	uint32_t compiled_variable_count;
	uint32_t symbol_namespace;
	uint32_t wrapper_function_index;
	/* See zend_native_component_member.entry_variant_member_plus_one. */
	uint32_t entry_variant_member_plus_one;
	/* See zend_native_component_member.deopt_generic_member_plus_one. */
	uint32_t deopt_generic_member_plus_one;
	bool deopt_landings;
	/* Tier 2 call counting (zend_native_component_member). */
	zend_native_entry_cell *call_count_cell;
	/* Tier-2 argument guards (zend_native_component_member). */
	uint8_t tier2_arg_guards[ZEND_NATIVE_TIER2_ARG_GUARDS];
	const void *tier2_fallback_entry;
	/* The resume IDs are deoptimization stress targets. */
	bool deopt_stress;
	uint32_t entry_variant_long_mask;
	bool entry_variant_numeric;
	/* Every return of this member yields a long or a double. */
	bool returns_number;
	/*
	 * The typed body may fail: an operation needs Zend semantics, such as a
	 * double returned as int or an integer overflow the plan excludes, or a
	 * typed call it makes failed. It then returns a zero status (the type
	 * part of a boxed result, otherwise an extra register) and the outermost
	 * Zend-entry caller repeats the effect-free call through its canonical
	 * path, which throws or coerces exactly.
	 */
	bool typed_body_may_fail;
	/* Parameters declared with a type a boxed zval does not prove; a
	 * typed call skips their receive checks. Numbers pass those in
	 * number_argument_mask unchanged. */
	uint32_t checked_boxed_argument_mask;
	uint32_t number_argument_mask;
	uint32_t typed_body_function_index;
	/*
	 * The native call entry (ADR 0025 section 3,
	 * zend_native_code_fast_call_entry()): frameless code placed before this
	 * member's Zend entry, with zend_native_call_fast_do()'s signature. It
	 * checks the pushed frame against the facts below, initializes it as
	 * i_init_func_execute_data() and the RECV/RECV_INIT of its parameters
	 * do, calls the Zend entry and leaves inline; any other frame continues
	 * in zend_native_call_fast_do(). Only for functions with at most
	 * ZEND_NATIVE_CALL_FAST_RECEIVE_MAX parameters, which are neither
	 * generators nor variadic by reference or with a type, and whose
	 * defaults are literals the runtime's inline receive takes too.
	 */
	bool fast_call_eligible;
	/* The layout profile saw the member run rarely
	 * (zend_tpde_layout_rare_function()): what of its code to keep small,
	 * ZEND_TPDE_RARE_* bits. */
	uint32_t rare;
	/* The entry collects extra arguments into an untyped, by-value
	 * variadic parameter (zend_native_call_receive_variadic()). */
	bool fast_call_variadic;
	uint32_t source_num_args;
	/* The declared return type's type_mask (arg_info[-1]), 0 without one:
	 * immutable for the compiled op_array, so VERIFY_RETURN_TYPE tests it
	 * as an immediate. */
	uint32_t source_return_type_mask;
	/* The fewest arguments the entry takes: one past the last parameter
	 * without a default. */
	uint32_t fast_call_min_arguments;
	/* Per parameter: the literal index of its default, UINT32_MAX for none,
	 * and the type bits an argument takes without a check or coercion
	 * (UINT32_MAX untyped). */
	uint32_t fast_call_default_literals[ZEND_NATIVE_CALL_FAST_RECEIVE_MAX];
	uint32_t fast_call_type_masks[ZEND_NATIVE_CALL_FAST_RECEIVE_MAX];
	/* Bit n: parameter n names a class; an object argument of the class
	 * the parameter last accepted (the target cell's fast_receive) fits. */
	uint32_t fast_call_class_mask;
	/* Bit n: CV n may hold a counted value when the function returns (by
	 * Zend's type inference); the entry releases only those. */
	uint64_t fast_call_counted_cvs;
	zend_mir_function_record function;
	zend_tpde_owned<zend_mir_block_id> block_ids;
	uint32_t block_count;
	zend_tpde_owned<zend_tpde_id_index_entry> block_index;
	uint32_t block_index_capacity;
	zend_tpde_owned<uint32_t> block_successor_offsets;
	zend_tpde_owned<uint32_t> block_successors;
	uint32_t block_successor_count;
	zend_tpde_owned<uint32_t> block_predecessor_offsets;
	zend_tpde_owned<uint32_t> block_predecessors;
	uint32_t block_predecessor_count;
	zend_tpde_owned<zend_tpde_value> values;
	uint32_t value_count;
	zend_tpde_owned<int32_t> argument_value_indices;
	zend_tpde_owned<zend_tpde_local_abi_type> argument_abi;
	zend_tpde_local_abi_type return_abi;
	zend_tpde_local_abi_type typed_body_return_abi;
	zend_tpde_owned<uint8_t> typed_component_call_eligible;
	zend_tpde_owned<uint8_t> effect_closed_inline_eligible;
	/* Per argument: whether an effect-closed inline clone of this member
	 * needs it, because its body reads it or its receive checks its type.
	 * The clone neither checks nor transports any other argument. */
	zend_tpde_owned<uint8_t> effect_closed_argument_used;
	bool typed_body_eligible;
	bool has_register_component_results;
	zend_tpde_machine_cfg entry_machine_cfg;
	zend_tpde_machine_cfg typed_body_machine_cfg;
	zend_tpde_owned<zend_tpde_id_index_entry> value_index;
	uint32_t value_index_capacity;
	zend_tpde_owned<zend_tpde_instruction> instructions;
	uint32_t instruction_count;
	zend_tpde_owned<zend_mir_value_id> instruction_operands;
	zend_tpde_owned<zend_tpde_operand_transport> instruction_operand_transports;
	uint32_t instruction_operand_count;
	zend_tpde_owned<zend_tpde_id_index_entry> instruction_index;
	uint32_t instruction_index_capacity;
	zend_tpde_owned<int32_t> value_definition_instructions;
	zend_tpde_owned<int32_t> source_value_definition_instructions;
	zend_tpde_owned<uint32_t> value_consumer_offsets;
	zend_tpde_owned<zend_tpde_machine_use> value_consumers;
	zend_tpde_owned<uint8_t> entry_value_required;
	zend_tpde_owned<uint8_t> typed_body_value_required;
	zend_tpde_owned<zend_tpde_id_index_entry> call_site_instruction_index;
	uint32_t call_site_instruction_index_capacity;
	uint32_t call_site_count;
	zend_tpde_owned<zend_mir_call_site_ref> call_sites;
	zend_tpde_owned<zend_tpde_id_index_entry> call_target_index;
	uint32_t call_target_index_capacity;
	uint32_t call_target_count;
	uint32_t call_argument_count;
	zend_tpde_owned<zend_mir_call_argument_ref> call_arguments;
	zend_tpde_owned<zend_tpde_source_value_binding> call_argument_bindings;
	zend_tpde_owned<zend_tpde_id_index_entry> user_binding_index;
	uint32_t user_binding_index_capacity;
	zend_tpde_owned<zend_tpde_id_index_entry> internal_binding_index;
	uint32_t internal_binding_index_capacity;
	_zend_native_direct_call_descriptor **direct_calls;
	uint32_t direct_call_count;
	_zend_native_direct_internal_call_descriptor **direct_internal_calls;
	uint32_t direct_internal_call_count;
	_zend_native_user_call_descriptor **user_calls;
	uint32_t user_call_count;
	uint32_t argument_count;
	uint64_t required_runtime_helpers[ZEND_NATIVE_RUNTIME_HELPER_WORD_COUNT];
	zend_tpde_owned<zend_mir_executable_value_ref> user_opcode_source_operations;
	uint32_t user_opcode_source_operation_count;
	zend_tpde_owned<uint32_t> user_opcode_source_op1_targets;
	zend_tpde_owned<uint32_t> user_opcode_source_op2_targets;
	zend_tpde_owned<uint32_t> user_opcode_source_extended_targets;
	zend_tpde_user_opcode_target
		user_opcode_targets[ZEND_VM_LAST_OPCODE + 1];
	uint32_t user_opcode_target_count;
	uint32_t generator_resume_count;
	uint32_t generator_resume_live_word_count;
	zend_tpde_owned<uint32_t> generator_resume_targets;
	zend_tpde_owned<uint32_t> generator_resume_landings;
	zend_tpde_owned<zend_mir_block_id> generator_resume_exception_blocks;
	zend_tpde_owned<uint64_t> generator_resume_live_values;
	/*
	 * Deoptimization resume IDs (ADR 0025 section 4): the source positions
	 * a deoptimizing activation continues at, with the MIR instruction that
	 * starts each.
	 */
	uint32_t deopt_resume_count;
	zend_tpde_owned<uint32_t> deopt_resume_targets;
	zend_tpde_owned<uint32_t> deopt_resume_instructions;
	/*
	 * A generic copy's Zend-entry adaptor, built to decide which resume IDs
	 * land when its specialized member's exits are frozen. The component
	 * adaptor adopts it instead of building the member again.
	 */
	mutable zend::native::tpde::ZendIRAdaptor *deopt_landing_adaptor;
	/*
	 * Per CV, nonzero when its lazy scalar stores are observable only by an
	 * operation that names the CV: no symbol table, parameter, catch,
	 * generator or user-opcode observer can read the slot otherwise.
	 */
	zend_tpde_owned<uint8_t> operand_observed_storages;
	uint32_t operand_observed_storage_count;
	zend_tpde_owned<zend_tpde_materialization> materializations;
	uint32_t materialization_count;
	zend_tpde_owned<zend_tpde_machine_reference> machine_references;
	uint32_t machine_reference_count;
	zend_tpde_owned<uint32_t> entry_undef_temporary_indices;
	/* Per index: only a branch-fused operation's helper needs it, and the
	 * branch stores a placeholder there first (Zend entry only). */
	zend_tpde_owned<uint8_t> entry_undef_temporary_fused_only;
	uint32_t entry_undef_temporary_count;
	/* The source op array is a generator: only then can an entry frame be
	 * a generator frame. */
	bool source_generator;
	/* The source op array is a function, not the code of a script or an
	 * eval, whose CVs a symbol table may share. */
	bool source_function;
	/* The literal table of the source op array, which outlives the
	 * compilation: lets the emitter specialize code on literal keys. */
	const zval *source_literals;
	uint32_t source_literal_count;
	bool may_emit_calls;
	bool zend_entry_may_emit_calls;
	bool typed_body_may_emit_calls;
	bool user_opcode_callbacks;
};

uint32_t zend_tpde_block_successor_count(
	const zend_tpde_plan *plan, zend_mir_block_id id);
bool zend_tpde_block_successor_at(
	const zend_tpde_plan *plan,
	zend_mir_block_id id,
	uint32_t successor_index,
	zend_mir_block_id *out);

/*
 * The source SSA variable, plus one, of the CV definition a long ASSIGN_OP or
 * INCDEC without a result mutates in its register, or 0. slot_incdec admits
 * an INCDEC only on a slot operand: lazy mutation also replaces the
 * loop-carried source identity, which SSA component boundaries do not keep.
 */
uint32_t zend_tpde_register_mutation_ssa_plus_one(
	const zend_tpde_instruction &instruction, bool slot_incdec);

/* Whether planning required the runtime helper (require_runtime_helper()). */
static inline bool zend_tpde_runtime_helper_required(
	const zend_tpde_plan *plan, zend_native_runtime_helper_id helper)
{
	return (plan->required_runtime_helpers[helper / 64u]
		& (UINT64_C(1) << (helper % 64u))) != 0;
}

/* Appends the storages ASSIGN_REF binds to storages, then sorts them and
 * drops duplicates. */
void zend_tpde_collect_reference_assigned_storages(
	const zend_tpde_plan *plan, std::vector<zend_mir_storage_id> &storages);

const zend_tpde_source_call_phase_entry *
zend_tpde_source_call_phase_at(
	const zend_tpde_plan *plan, uint32_t source_position);

/* Calls visit(value_index) for the live values of a generator resume in
 * ascending order; stops at, and returns, the first false. */
template <typename Visit>
static inline bool zend_tpde_visit_generator_resume_values(
	const zend_tpde_plan *plan, uint32_t resume_index, Visit &&visit)
{
	if (resume_index >= plan->generator_resume_count
			|| plan->generator_resume_live_values == nullptr) {
		return true;
	}
	const uint64_t *words =
		plan->generator_resume_live_values
			+ static_cast<size_t>(resume_index)
				* plan->generator_resume_live_word_count;
	for (uint32_t word = 0;
			word < plan->generator_resume_live_word_count; ++word) {
		for (uint64_t bits = words[word]; bits != 0; bits &= bits - 1) {
			const uint32_t value_index = word * 64
				+ static_cast<uint32_t>(std::countr_zero(bits));
			if (value_index >= plan->value_count) {
				return true;
			}
			if (!visit(value_index)) {
				return false;
			}
		}
	}
	return true;
}

/*
 * Whether a value has a machine result of its own, for the planner and the
 * adaptor alike: an exact scalar that is a constant or held in registers, or
 * a pointer or boxed zval held in registers. A null is payload-free and has
 * none.
 */
bool zend_tpde_machine_result_representation(
	zend_mir_scalar_type_mask exact_type,
	zend_tpde_machine_value_kind machine_kind,
	bool constant, bool register_authoritative);

/*
 * A direct user call with an argument whose type is checked only at run time:
 * a by-value boxed source or the referent of a by-reference one. Such a call
 * keeps its frame; an effect-closed inline body would emit no check.
 */
bool zend_tpde_direct_call_has_argument_guard(
	const zend_tpde_instruction &instruction);

/* The arithmetic of two numbers that cannot fail: overflow continues in
 * double precision. */
static inline bool zend_tpde_numeric_arithmetic(uint32_t opcode)
{
	return opcode == ZEND_ADD || opcode == ZEND_SUB || opcode == ZEND_MUL;
}

/* The comparisons of two numbers that cannot fail: NaN compares unordered. */
static inline bool zend_tpde_numeric_comparison(uint32_t opcode)
{
	return opcode == ZEND_IS_SMALLER || opcode == ZEND_IS_SMALLER_OR_EQUAL
		|| opcode == ZEND_IS_EQUAL || opcode == ZEND_IS_NOT_EQUAL;
}

static inline bool zend_tpde_known_numeric_type(uint8_t known)
{
	return known == IS_LONG || known == IS_DOUBLE
		|| known == ZEND_TPDE_KNOWN_NUMBER;
}

/*
 * A binary operation of the numeric opcodes whose source opcode the plan
 * still holds at its source position, with inline forms enabled.
 */
bool zend_tpde_numeric_binary_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction);

/*
 * Arithmetic other than division and every ordering or equality comparison
 * of two numbers cannot fail: integer overflow continues in double precision
 * and NaN compares unordered. Such a binary operation needs no slow path.
 */
bool zend_tpde_numeric_binary_proven(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction);

/*
 * A proven operation whose temporary result a typed body keeps in registers:
 * a boxed number for arithmetic, a boolean for comparisons.
 */
bool zend_tpde_typed_numeric_binary(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction);

/*
 * A QM_ASSIGN into a temporary, as a ternary makes, that a typed body forwards
 * as a register value: its source must name a register value itself.
 */
bool zend_tpde_typed_forward_assign(
	const zend_tpde_instruction &instruction);

bool zend_tpde_user_multi_branch_at(
	const zend_tpde_plan *plan,
	const zend_mir_executable_value_ref &operation,
	uint32_t target_opcode,
	zend_tpde_user_multi_branch *out);

bool zend_tpde_multi_branch_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction,
	const zend_mir_instruction_record &record,
	zend_tpde_multi_branch *out);

enum zend_native_image_symbol_kind : uint32_t {
	ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER = 1,
	ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL = 2,
	ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL = 3,
	ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR = 5,
	ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR = 7,
	ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR = 8,
};

struct zend_native_image_symbol {
	uint32_t kind;
	uint32_t id;
	uint32_t symbol_namespace;
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
	uint64_t build_id;
	unsigned char *text;
	size_t text_size;
	size_t text_capacity;
	zend_native_image_symbol *symbols;
	uint32_t symbol_count;
	uint32_t symbol_capacity;
	zend_native_image_symbol_binding *symbol_bindings;
	uint32_t symbol_binding_count;
	uint32_t symbol_binding_capacity;
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
	/* Transient lookup index over symbols and bindings, built on demand
	 * and never serialized. */
	void *symbol_lookup;
};

struct zend_native_code {
	zend_native_target target;
	zend_native_code *owner;
	uint32_t owner_refcount;
	void *mapping;
	size_t mapping_size;
	zend_native_frame_entry_t entry;
	zend_native_frame_entry_t *component_entries;
	/* Each component's native call entry (zend_native_call_fast_do()'s
	 * signature), which checks the frame, and the same entry past the
	 * checks for a frame the call site proved; NULL where the image has
	 * none. */
	void *fast_call_entry;
	void *exact_call_entry;
	void **component_fast_entries;
	void **component_exact_entries;
	zend_native_component_entry *component_metadata;
	uint32_t component_entry_count;
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
/* What of a member's code the layout profile saw run rarely keeps small:
 * no native call entry of its own. 0 when the profile (the host backend's;
 * none on others) did not see the member's generic body run rarely. */
inline constexpr uint32_t ZEND_TPDE_RARE_NO_CALL_ENTRY = 1u << 0;
uint32_t zend_tpde_layout_rare_function(
	uint32_t index, const zend_tpde_plan *plan);
zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index);

/*
 * The machine kind of a typed body's proven numeric binary result
 * (zend_tpde_typed_numeric_binary): a comparison's boolean, an exact long
 * where inference proved one (its range excludes overflow), else a boxed
 * number.
 */
zend_tpde_machine_value_kind zend_tpde_typed_numeric_result_kind(
	const zend_tpde_plan *plan, const zend_tpde_instruction &instruction);

struct zend_tpde_scalar_diamond {
	uint32_t entry;
	uint32_t true_block;
	uint32_t false_block;
	uint32_t merge;
};

/*
 * The smallest acyclic multi-block scalar body: an entry branching to two
 * blocks that join in a final merge block. COND_BRANCH reaches successor
 * zero when true and successor one when false; PHI operands follow
 * predecessor order.
 */
bool zend_tpde_scalar_diamond_at(
		const zend_tpde_plan *plan,
		zend_tpde_scalar_diamond *out);

/*
 * A self-identical store of an unaliased, non-refcounted scalar into its own
 * zval carrier, which an inlined scalar diamond drops with the callee frame.
 * Assignments and reference-visible stores stay observable and reject
 * inlining.
 */
bool zend_tpde_scalar_diamond_frame_transport(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction);

/*
 * A typed body has no frame either: the same self-identical scalar store
 * into an unaliased carrier disappears, as the value travels in registers.
 */
static inline bool zend_tpde_typed_body_frame_transport(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction)
{
	return zend_tpde_scalar_diamond_frame_transport(plan, instruction);
}

/*
 * The shape of a proof-closed scalar callee body that a direct call may clone
 * instead of calling through the local component ABI: one block, or a scalar
 * diamond of PHIs, of effect-free integer and boolean instructions, up to
 * eight checked additions and subtractions or one string length, and one
 * return. The machine plan screens bodies with it and freezes the
 * eligibility; the adaptor follows it while it clones a body.
 */
enum zend_tpde_inline_body_step : uint8_t {
	ZEND_TPDE_INLINE_BODY_REJECT,
	/* Transport, constant, statepoint or proven return-type check. */
	ZEND_TPDE_INLINE_BODY_SKIP,
	ZEND_TPDE_INLINE_BODY_PHI,
	ZEND_TPDE_INLINE_BODY_COND_BRANCH,
	ZEND_TPDE_INLINE_BODY_BRANCH,
	ZEND_TPDE_INLINE_BODY_RETURN_SOURCE_ZVAL,
	ZEND_TPDE_INLINE_BODY_RETURN,
	ZEND_TPDE_INLINE_BODY_COPY,
	ZEND_TPDE_INLINE_BODY_CHECKED_BINARY,
	ZEND_TPDE_INLINE_BODY_STRING_LENGTH,
	ZEND_TPDE_INLINE_BODY_SCALAR,
};

struct zend_tpde_inline_body_shape {
	/*
	 * What only the machine plan's screen rejects: bodies of more semantic
	 * instructions, a string length or scalar instruction after a string
	 * length, and diamonds without exactly one branch per arm.
	 */
	uint32_t semantic_instruction_limit;
	bool string_length_ends_body;
	bool counts_arm_branches;

	bool scalar_diamond;
	zend_tpde_scalar_diamond diamond;
	/* Set by each return step; the cloner keeps its own answer. */
	bool saw_return;
	bool saw_string_length;
	uint32_t checked_binary_count;
	uint32_t semantic_instruction_count;
	uint32_t conditional_branch_count;
	uint32_t arm_branch_count;
	uint32_t phi_count;
};

/*
 * Whether call's target and result and callee's form admit an inline body;
 * sets up shape for zend_tpde_inline_body_step_at().
 */
bool zend_tpde_inline_body_begin(
		const zend_tpde_instruction &call,
		const zend_tpde_plan *callee,
		zend_tpde_inline_body_shape *shape);

/* Classifies the next callee instruction of an inline body, in order. */
zend_tpde_inline_body_step zend_tpde_inline_body_step_at(
		const zend_tpde_instruction &call,
		const zend_tpde_plan *callee,
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record,
		zend_tpde_inline_body_shape *shape);

/*
 * The setup area a user call's Init reserves below the callee frame: the
 * frame header, the activation, then a placement per argument and its
 * target slots. False for an unsupported result operand or an offset above
 * limit (the target's immediate range).
 */
struct zend_tpde_user_call_setup {
	uint64_t argument_count;
	uint64_t frame_header_size;
	uint64_t activation_offset;
	uint64_t placement_offset;
	uint64_t setup_size;
	uint64_t result_offset;
	bool uses_discarded_return;
};

bool zend_tpde_user_call_setup_layout(
	const zend_tpde_plan *plan,
	const zend_native_user_call_descriptor *call,
	uint64_t limit,
	zend_tpde_user_call_setup *out);

int32_t zend_tpde_instruction_index(
	const zend_tpde_plan *plan, zend_mir_instruction_id id);
const zend_mir_instruction_record &zend_tpde_instruction_record_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction);
bool zend_tpde_call_argument_at(
	const zend_tpde_plan *plan,
	uint32_t index,
	zend_mir_call_argument_ref *out);
bool zend_tpde_image_append(
	zend_native_image *image, const void *bytes, size_t length);
const zend_native_image_symbol *zend_tpde_image_symbol_find(
	const zend_native_image *image,
	zend_native_image_symbol_kind kind,
	uint32_t id,
	uint32_t symbol_namespace);
/*
 * The image symbol of a runtime helper an emitter calls. Planning must
 * require every such helper; a debug build asserts that it did, a release
 * build returns nullptr as zend_tpde_image_symbol_find() does.
 */
const zend_native_image_symbol *zend_tpde_required_runtime_symbol(
	const zend_native_image *image,
	zend_native_runtime_helper_id helper);
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

/* The backend of the host target; only that one is built. */
#if defined(__APPLE__) && defined(__aarch64__)
# define zend_tpde_emit_host zend_tpde_emit_darwin_arm64
# define zend_native_publish_host zend_native_publish_darwin_arm64
# define zend_native_unmap_host zend_native_unmap_darwin_arm64
#else
# define zend_tpde_emit_host zend_tpde_emit_linux_x64
# define zend_native_publish_host zend_native_publish_linux_x64
# define zend_native_unmap_host zend_native_unmap_linux_x64
#endif
