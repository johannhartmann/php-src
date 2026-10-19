// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

uint32_t zend_tpde_machine_value_zval_type(
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

uint32_t zend_tpde_machine_value_zval_type_info(
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

uint64_t zend_tpde_encode_value_operand(
	const zend_mir_source_operand_ref &operand, uint32_t frame_variable_count,
	uint32_t unused_payload)
{
	uint32_t type = ZEND_NATIVE_OPERAND_INVALID;
	uint64_t value = 0;

	switch (operand.kind) {
		case ZEND_MIR_SOURCE_OPERAND_UNUSED:
			type = IS_UNUSED;
			value = unused_payload == ZEND_MIR_ID_INVALID ? 0 : unused_payload;
			break;
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			type = IS_CONST;
			value = operand.index;
			break;
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			if (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
				type = IS_CV;
				value = (uint64_t{ZEND_CALL_FRAME_SLOT} + operand.index)
					* sizeof(zval);
			} else if (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
					|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR) {
				type = operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
					? IS_TMP_VAR : IS_VAR;
				value = (uint64_t{ZEND_CALL_FRAME_SLOT} + frame_variable_count
					+ operand.index) * sizeof(zval);
			}
			break;
		default:
			break;
	}
	return type | ((value & UINT32_MAX) << 8);
}

bool zend_tpde_helper_has_explicit_operands(
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

bool zend_tpde_helper_has_unused_operand_payloads(
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

bool zend_tpde_helper_has_explicit_auxiliary(
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

uint64_t zend_tpde_auxiliary_cache_bits(
	zend_native_runtime_helper_id helper,
	const zend_mir_executable_value_ref &operation)
{
	if (helper != ZEND_NATIVE_HELPER_OBJECT_ASSIGN_OP
			&& helper != ZEND_NATIVE_HELPER_STATIC_ASSIGN_OP) {
		return 0;
	}
	return uint64_t{operation.auxiliary_extended_value
				< ZEND_NATIVE_AUXILIARY_CACHE_NONE
			? operation.auxiliary_extended_value
			: ZEND_NATIVE_AUXILIARY_CACHE_NONE}
		<< ZEND_NATIVE_AUXILIARY_CACHE_SHIFT;
}

bool zend_tpde_helper_requires_undef_result(
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

bool zend_tpde_reference_cv_operand_slot(
	const zend_tpde_instruction &instruction, zend_mir_storage_id storage)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;

	return instruction.has_value_operation && zend_mir_id_is_valid(storage)
		&& ((instruction.source_op1_may_reference
				&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				&& storage == operation.op1_storage_id)
			|| (instruction.source_op2_may_reference
				&& operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				&& storage == operation.op2_storage_id));
}

zend_tpde_integer_dispatch_kind
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

bool zend_tpde_array_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_read *out,
	bool allow_temporary_key)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t container_offset;
	uint64_t key_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
			|| operation.source_opcode != ZEND_FETCH_DIM_R
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.kind
					!= ZEND_MIR_SOURCE_OPERAND_LITERAL)
			|| (operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& (!allow_temporary_key
					|| (operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
						&& operation.op2.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_VAR
						&& operation.op2.kind
							!= ZEND_MIR_SOURCE_OPERAND_LITERAL)))
			|| (operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1_storage_id == ZEND_MIR_ID_INVALID)
			|| (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.op1_storage_id != ZEND_MIR_ID_INVALID)
			|| (operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.op2_storage_id == ZEND_MIR_ID_INVALID)
			|| (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& operation.op2_storage_id != ZEND_MIR_ID_INVALID)
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
	/* A literal key has no frame slot; its fast path takes the key operand. */
	key_offset = operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
		? 0
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
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

bool zend_tpde_frameless_direct_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_frameless_direct *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;

	if (!instruction.has_value_operation
			|| operation.source_opcode < ZEND_FRAMELESS_ICALL_0
			|| operation.source_opcode > ZEND_FRAMELESS_ICALL_3
			|| operation.extended_value > 0xffff
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV)
			|| !zend_mir_id_is_valid(operation.result_storage_id)) {
		return false;
	}
	const uint32_t argument_count =
		operation.source_opcode - ZEND_FRAMELESS_ICALL_0;
	const zend_mir_source_operand_ref *arguments[3] = {
		&operation.op1, &operation.op2, &operation.auxiliary};
	const zend_mir_storage_id storages[3] = {
		operation.op1_storage_id, operation.op2_storage_id,
		operation.auxiliary_storage_id};
	uint64_t offsets[3] = {0, 0, 0};
	uint64_t descriptor = operation.extended_value
		| (uint64_t{argument_count} << 16)
		| (uint64_t{operation.source_position_id} << 32);
	for (uint32_t index = 0; index < argument_count; ++index) {
		const zend_mir_source_operand_ref &argument = *arguments[index];
		if (argument.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			offsets[index] = argument.index;
			descriptor |= uint64_t{1}
				<< (ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT + index);
			continue;
		}
		if ((argument.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& argument.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
				|| !zend_mir_id_is_valid(storages[index])
				|| (argument.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& argument.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP)
				|| storages[index] == operation.result_storage_id) {
			return false;
		}
		offsets[index] = (uint64_t{ZEND_CALL_FRAME_SLOT} + storages[index])
			* sizeof(zval);
		descriptor |= uint64_t{1} << (
			(argument.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				? ZEND_NATIVE_FRAMELESS_DIRECT_CV_SHIFT
				: ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT) + index);
	}
	const uint64_t result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (result_offset > UINT32_MAX || offsets[0] > UINT32_MAX
			|| offsets[1] > UINT32_MAX || offsets[2] > UINT32_MAX) {
		return false;
	}
	out->descriptor = descriptor;
	out->slots = result_offset | (offsets[0] << 32);
	out->more_slots = offsets[1] | (offsets[2] << 32);
	return true;
}

bool zend_tpde_fused_operand_at(
	const zend_mir_source_operand_ref &operand, zend_mir_storage_id storage,
	zend_mir_storage_id result_storage, zend_tpde_fused_operand *out)
{
	uint64_t offset;

	out->literal = operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
	out->temporary = false;
	if (out->literal) {
		offset = uint64_t{operand.index} * sizeof(zval);
	} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP)
			&& zend_mir_id_is_valid(storage) && storage != result_storage) {
		out->temporary = operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
		offset = (uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
	} else {
		return false;
	}
	if (offset > INT32_MAX - sizeof(zval)) {
		return false;
	}
	out->offset = static_cast<uint32_t>(offset);
	return true;
}

bool zend_tpde_fused_compare_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_fused_operand *left, zend_tpde_fused_operand *right)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return instruction.has_value_operation
		&& operation.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
		&& operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
		&& zend_mir_id_is_valid(operation.result_storage_id)
		&& zend_tpde_fused_operand_at(operation.op1, operation.op1_storage_id,
			operation.result_storage_id, left)
		&& zend_tpde_fused_operand_at(operation.op2, operation.op2_storage_id,
			operation.result_storage_id, right);
}

bool zend_tpde_fused_type_check_at(
	const zend_tpde_instruction &instruction, zend_tpde_fused_operand *value)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return instruction.has_value_operation
		&& operation.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
		&& operation.source_opcode == ZEND_TYPE_CHECK
		&& operation.extended_value != (UINT32_C(1) << IS_RESOURCE)
		&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
		&& operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
		&& zend_mir_id_is_valid(operation.result_storage_id)
		&& zend_tpde_fused_operand_at(operation.op1, operation.op1_storage_id,
			operation.result_storage_id, value)
		&& !value->literal;
}

bool zend_tpde_fused_isset_cv_at(
	const zend_tpde_instruction &instruction, zend_tpde_fused_operand *value)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return instruction.has_value_operation
		&& operation.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
		&& operation.source_opcode == ZEND_ISSET_ISEMPTY_CV
		&& (operation.extended_value & ZEND_ISEMPTY) == 0
		&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
		&& operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
		&& zend_mir_id_is_valid(operation.result_storage_id)
		&& zend_tpde_fused_operand_at(operation.op1, operation.op1_storage_id,
			operation.result_storage_id, value)
		&& !value->literal;
}

bool zend_tpde_fused_identical_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_fused_operand *left, zend_tpde_fused_operand *right)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return instruction.has_value_operation
		&& (operation.source_opcode == ZEND_IS_IDENTICAL
			|| operation.source_opcode == ZEND_IS_NOT_IDENTICAL)
		&& zend_tpde_fused_compare_at(instruction, left, right)
		&& !left->literal
		&& (right->literal || right->offset != left->offset);
}

bool zend_tpde_fused_isset_dim_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_fused_operand *container, zend_tpde_fused_operand *key)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return instruction.has_value_operation
		&& operation.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
		&& operation.source_opcode == ZEND_ISSET_ISEMPTY_DIM_OBJ
		&& (operation.extended_value & ZEND_ISEMPTY) == 0
		&& operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
		&& zend_mir_id_is_valid(operation.result_storage_id)
		&& zend_tpde_fused_operand_at(operation.op1, operation.op1_storage_id,
			operation.result_storage_id, container)
		&& !container->temporary
		&& zend_tpde_fused_operand_at(operation.op2, operation.op2_storage_id,
			operation.result_storage_id, key)
		&& (key->literal || container->literal
			|| key->offset != container->offset);
}

bool zend_tpde_dim_direct_operand(
	const zend_mir_source_operand_ref &operand, zend_mir_storage_id storage,
	bool allow_unused, uint64_t *kind, uint64_t *offset)
{
	if (operand.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		*kind = ZEND_NATIVE_DIM_DIRECT_UNUSED;
		*offset = 0;
		return allow_unused;
	}
	if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		*kind = ZEND_NATIVE_DIM_DIRECT_CONST;
		*offset = operand.index;
		return true;
	}
	if ((operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| !zend_mir_id_is_valid(storage)
			|| (operand.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operand.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP)) {
		return false;
	}
	*kind = operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
		? ZEND_NATIVE_DIM_DIRECT_CV : ZEND_NATIVE_DIM_DIRECT_TMP;
	*offset = (uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
	return *offset <= UINT32_MAX;
}

bool zend_tpde_dim_direct_at(
	const zend_tpde_instruction &instruction, zend_tpde_dim_direct *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	const bool read = operation.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
		&& operation.source_opcode == ZEND_FETCH_DIM_R;
	const bool write = operation.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
		&& operation.source_opcode == ZEND_ASSIGN_DIM;
	const bool test = operation.opcode
			== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
		&& operation.source_opcode == ZEND_ISSET_ISEMPTY_DIM_OBJ;
	uint64_t container_kind;
	uint64_t container_offset;
	uint64_t indirect_container = 0;
	uint64_t key_kind;
	uint64_t key_offset;
	uint64_t value_kind = ZEND_NATIVE_DIM_DIRECT_UNUSED;
	uint64_t value_offset = 0;
	uint64_t result_offset = 0;

	/* A write may address the property FETCH_OBJ_W left in a VAR. */
	if (write && out != nullptr && instruction.has_value_operation
			&& (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
			&& zend_mir_id_is_valid(operation.op1_storage_id)) {
		container_kind = ZEND_NATIVE_DIM_DIRECT_CV;
		container_offset = (uint64_t{ZEND_CALL_FRAME_SLOT}
			+ operation.op1_storage_id) * sizeof(zval);
		indirect_container = 1;
		if (container_offset > UINT32_MAX) {
			return false;
		}
	}
	if (!instruction.has_value_operation
			|| (!read && !write && !test)
			|| operation.extended_value > 0xffff
			|| (indirect_container == 0
				&& !zend_tpde_dim_direct_operand(operation.op1,
					operation.op1_storage_id, false,
					&container_kind, &container_offset))
			|| (container_kind != ZEND_NATIVE_DIM_DIRECT_CV
				&& !((test || read)
					&& container_kind == ZEND_NATIVE_DIM_DIRECT_TMP))
			|| !zend_tpde_dim_direct_operand(operation.op2,
				operation.op2_storage_id, write, &key_kind, &key_offset)) {
		return false;
	}
	if (test || read) {
		/* The container kind rides in the value kind field. */
		value_kind = container_kind;
	}
	bool result_cv = false;
	if (read || test) {
		/* A read may write a CV the optimizer named as its result, which
		 * holds no counted value; ZEND_FETCH_DIM_R overwrites it. */
		result_cv = read
			&& operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& operation.result_storage_id != operation.op1_storage_id
			&& operation.result_storage_id != operation.op2_storage_id;
		if ((operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& !result_cv)
				|| !zend_mir_id_is_valid(operation.result_storage_id)) {
			return false;
		}
		result_offset = (uint64_t{ZEND_CALL_FRAME_SLOT}
			+ operation.result_storage_id) * sizeof(zval);
	} else if (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| !zend_tpde_dim_direct_operand(operation.auxiliary,
				operation.auxiliary_storage_id, false,
				&value_kind, &value_offset)) {
		return false;
	}
	if (result_offset > UINT32_MAX) {
		return false;
	}
	out->descriptor = key_kind | (value_kind << 2)
		| (uint64_t{result_cv} << ZEND_NATIVE_DIM_DIRECT_RESULT_CV_SHIFT)
		| (indirect_container << ZEND_NATIVE_DIM_DIRECT_INDIRECT_CONTAINER_SHIFT)
		| (uint64_t{operation.extended_value} << 8)
		| (uint64_t{operation.source_position_id} << 32);
	out->slots = container_offset | (key_offset << 32);
	out->more_slots = value_offset | (result_offset << 32);
	return true;
}

bool zend_tpde_concat_assign_direct_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_concat_assign_direct *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t value_kind;
	uint64_t value_offset;

	if (!instruction.has_value_operation
			|| operation.source_opcode != ZEND_ASSIGN_OP
			|| operation.extended_value != ZEND_CONCAT
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| !zend_mir_id_is_valid(operation.op1_storage_id)) {
		return false;
	}
	if (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value_kind = ZEND_NATIVE_CONCAT_DIRECT_CONST;
		value_offset = operation.op2.index;
	} else if ((operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP)
			&& zend_mir_id_is_valid(operation.op2_storage_id)
			&& operation.op2_storage_id != operation.op1_storage_id) {
		value_kind = operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			? ZEND_NATIVE_CONCAT_DIRECT_CV : ZEND_NATIVE_CONCAT_DIRECT_TMP;
		value_offset = (uint64_t{ZEND_CALL_FRAME_SLOT}
			+ operation.op2_storage_id) * sizeof(zval);
	} else {
		return false;
	}
	const uint64_t variable_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	if (variable_offset > UINT32_MAX || value_offset > UINT32_MAX) {
		return false;
	}
	out->descriptor = value_kind
		| (uint64_t{operation.source_position_id} << 32);
	out->slots = variable_offset | (value_offset << 32);
	return true;
}

bool zend_tpde_concat_direct_at(
	const zend_tpde_instruction &instruction, zend_tpde_concat_direct *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t left_kind, left_offset, right_kind, right_offset;

	if (!instruction.has_value_operation
			|| (operation.opcode != ZEND_MIR_OPCODE_VALUE_CONCAT
				&& operation.opcode != ZEND_MIR_OPCODE_VALUE_FAST_CONCAT)
			|| (operation.source_opcode != ZEND_CONCAT
				&& operation.source_opcode != ZEND_FAST_CONCAT)
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV)
			|| !zend_mir_id_is_valid(operation.result_storage_id)
			|| operation.result_storage_id == operation.op1_storage_id
			|| operation.result_storage_id == operation.op2_storage_id
			|| !zend_tpde_dim_direct_operand(operation.op1,
				operation.op1_storage_id, false, &left_kind, &left_offset)
			|| !zend_tpde_dim_direct_operand(operation.op2,
				operation.op2_storage_id, false, &right_kind, &right_offset)) {
		return false;
	}
	const uint64_t result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (result_offset > UINT32_MAX) {
		return false;
	}
	out->descriptor = left_kind | (right_kind << 2)
		| (uint64_t{operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV}
			<< ZEND_NATIVE_DIM_DIRECT_RESULT_CV_SHIFT)
		| (uint64_t{operation.source_opcode} << 8)
		| (uint64_t{operation.source_position_id} << 32);
	out->slots = left_offset | (right_offset << 32);
	out->result_offset = result_offset;
	return true;
}

bool zend_tpde_identical_direct_at(
	const zend_tpde_instruction &instruction, zend_tpde_concat_direct *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t left_kind, left_offset, right_kind, right_offset;

	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_BINARY_OP
			|| (operation.source_opcode != ZEND_IS_IDENTICAL
				&& operation.source_opcode != ZEND_IS_NOT_IDENTICAL)
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV)
			|| !zend_mir_id_is_valid(operation.result_storage_id)
			|| (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				&& (operation.result_storage_id == operation.op1_storage_id
					|| operation.result_storage_id
						== operation.op2_storage_id))
			|| !zend_tpde_dim_direct_operand(operation.op1,
				operation.op1_storage_id, false, &left_kind, &left_offset)
			|| !zend_tpde_dim_direct_operand(operation.op2,
				operation.op2_storage_id, false, &right_kind, &right_offset)) {
		return false;
	}
	const uint64_t result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (result_offset > UINT32_MAX) {
		return false;
	}
	out->descriptor = left_kind | (right_kind << 2)
		| (uint64_t{operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV}
			<< ZEND_NATIVE_DIM_DIRECT_RESULT_CV_SHIFT)
		| (uint64_t{operation.source_opcode} << 8)
		| (uint64_t{operation.source_position_id} << 32);
	out->slots = left_offset | (right_offset << 32);
	out->result_offset = result_offset;
	return true;
}

bool zend_tpde_packed_array_append_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_packed_array_append *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t container_offset;
	uint64_t value_offset;
	uint64_t result_offset = 0;

	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
			|| operation.source_opcode != ZEND_ASSIGN_DIM
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
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
	out->indirect_container =
		operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR;
	return true;
}

bool zend_tpde_array_isset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_isset *out,
	bool allow_empty)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t container_offset;
	uint64_t key_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
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

bool zend_tpde_string_length_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_string_length *out,
	bool allow_ssa_result)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
			|| operation.source_opcode != ZEND_STRLEN
			|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& (!allow_ssa_result
					|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA))
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

bool zend_tpde_bool_unary_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_bool_unary *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
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

bool zend_tpde_value_condition_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_value_condition *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset = 0;
	const bool has_result = operation.source_opcode == ZEND_JMPZ_EX
		|| operation.source_opcode == ZEND_JMPNZ_EX;

	if (!instruction.has_value_operation
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

bool zend_tpde_string_identity_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_string_identity *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t left_offset;
	uint64_t right_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
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

bool zend_tpde_long_operand_at(
	const zend_mir_source_operand_ref &operand,
	zend_mir_storage_id storage_id,
	zend_tpde_long_operand *out)
{
	uint64_t offset;

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

bool zend_tpde_long_binary_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_binary *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t result_offset;

	if (!instruction.has_value_operation
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
			 * CV result aliases one input; the fast path loads both inputs and
			 * checks the destination before writing it. Other CV destinations
			 * keep the runtime primitive. */
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

bool zend_tpde_long_assign_op_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_assign_op *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t left_offset;
	uint64_t right_offset;
	uint64_t result_offset = 0;
	bool has_result;

	if (!instruction.has_value_operation
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
	if (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		/* A literal has no storage: its offset indexes the literal table. */
		right_offset = uint64_t{operation.op2.index} * sizeof(zval);
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

bool zend_tpde_long_incdec_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_long_incdec *out, bool allow_indirect)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset = 0;
	bool has_result;

	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_VALUE_INCDEC
			|| (operation.source_opcode != ZEND_PRE_INC
				&& operation.source_opcode != ZEND_PRE_DEC
				&& operation.source_opcode != ZEND_POST_INC
				&& operation.source_opcode != ZEND_POST_DEC)
			|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& (!allow_indirect
					|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR))
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
	/* A VAR holds the INDIRECT a write fetch (FETCH_DIM_RW) left. */
	out->indirect = operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR;
	out->increment = operation.source_opcode == ZEND_PRE_INC
		|| operation.source_opcode == ZEND_POST_INC;
	out->post = operation.source_opcode == ZEND_POST_INC
		|| operation.source_opcode == ZEND_POST_DEC;
	return true;
}

bool zend_tpde_slot_isset_empty_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_slot_isset_empty *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
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

bool zend_tpde_packed_iterator_fetch_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_packed_iterator_fetch *out,
	bool allow_key)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t holder_offset;
	uint64_t destination_offset;
	uint64_t key_offset = 0;
	const bool has_key = operation.result.kind
		!= ZEND_MIR_SOURCE_OPERAND_UNUSED;

	/*
	 * Only the value-only, by-value foreach shape, unless the target takes
	 * the key temporary too (allow_key), which needs the ownership rules for
	 * string keys.
	 */
	if (!instruction.has_value_operation
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

bool zend_tpde_array_iterator_reset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_array_iterator_reset *out,
	bool allow_literal)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t source_offset;
	uint64_t holder_offset;

	/*
	 * A CV array is copied into the holder as FE_RESET_R does. With
	 * allow_literal a temporary array also moves into the holder, as
	 * FE_RESET_R moves a TMP operand, and a literal array, which OPcache
	 * propagates into FE_RESET_R, is taken too. References, objects and
	 * by-reference iteration keep the runtime primitive.
	 */
	const bool source_literal = allow_literal
		&& operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
	const bool source_temporary = allow_literal
		&& !source_literal
		&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_ITERATOR_BRANCH
			|| operation.source_opcode != ZEND_FE_RESET_R
			|| (!source_literal
				&& ((operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
						&& !source_temporary)
					|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
					|| operation.op1_storage_id
						== operation.result_storage_id))
			|| (source_literal
				&& operation.op1_storage_id != ZEND_MIR_ID_INVALID)
			|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
				&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| operation.result_storage_id == ZEND_MIR_ID_INVALID) {
		return false;
	}
	source_offset = source_literal
		? 0
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
			* sizeof(zval);
	holder_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
			* sizeof(zval);
	if (source_offset > UINT32_MAX || holder_offset > UINT32_MAX) {
		return false;
	}
	out->source_offset = static_cast<uint32_t>(source_offset);
	out->holder_offset = static_cast<uint32_t>(holder_offset);
	out->source_literal = source_literal;
	out->source_temporary = source_temporary;
	out->source_literal_index = source_literal ? operation.op1.index : 0;
	return true;
}

bool zend_tpde_object_property_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t receiver_offset;
	uint64_t result_offset;
	uint32_t cache_offset;

	if (!instruction.has_value_operation
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

bool zend_tpde_object_property_isset_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out)
{
	zend_tpde_instruction read = instruction;

	if (!instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_OBJECT_FETCH_IS
			|| instruction.value_operation.source_opcode != ZEND_FETCH_OBJ_IS
			|| (instruction.value_operation.result.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_TMP
				&& instruction.value_operation.result.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_VAR)) {
		return false;
	}
	read.value_operation.opcode = ZEND_MIR_OPCODE_OBJECT_FETCH_R;
	read.value_operation.source_opcode = ZEND_FETCH_OBJ_R;
	return zend_tpde_object_property_read_at(read, out);
}

bool zend_tpde_object_property_func_arg_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out)
{
	zend_tpde_instruction read = instruction;

	if (!instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_OBJECT_FETCH_FUNC_ARG
			|| instruction.value_operation.source_opcode
				!= ZEND_FETCH_OBJ_FUNC_ARG
			|| instruction.value_operation.result.slot_kind
				!= ZEND_MIR_SOURCE_SLOT_VAR) {
		return false;
	}
	read.value_operation.opcode = ZEND_MIR_OPCODE_OBJECT_FETCH_R;
	read.value_operation.source_opcode = ZEND_FETCH_OBJ_R;
	read.value_operation.extended_value &= ~ZEND_FETCH_OBJ_FLAGS;
	return zend_tpde_object_property_read_at(read, out);
}

bool zend_tpde_object_property_write_fetch_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_read *out)
{
	zend_tpde_instruction read = instruction;

	if (!instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_OBJECT_FETCH_W
			|| instruction.value_operation.source_opcode != ZEND_FETCH_OBJ_W
			|| instruction.value_operation.result.slot_kind
				!= ZEND_MIR_SOURCE_SLOT_VAR) {
		return false;
	}
	/* The fetch flags only concern typed properties, which the inline
	 * form leaves to the helper. */
	read.value_operation.opcode = ZEND_MIR_OPCODE_OBJECT_FETCH_R;
	read.value_operation.source_opcode = ZEND_FETCH_OBJ_R;
	read.value_operation.extended_value &= ~ZEND_FETCH_OBJ_FLAGS;
	return zend_tpde_object_property_read_at(read, out);
}

bool zend_tpde_object_property_write_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_object_property_write *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t receiver_offset;
	uint64_t value_offset;

	if (!instruction.has_value_operation
			|| operation.opcode != ZEND_MIR_OPCODE_OBJECT_ASSIGN
			|| operation.source_opcode != ZEND_ASSIGN_OBJ
			|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
				&& operation.op1.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED)
			|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| (operation.auxiliary.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& ((operation.auxiliary.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV
					&& operation.auxiliary.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_TMP)
					|| operation.auxiliary_storage_id
						== ZEND_MIR_ID_INVALID))
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
	out->literal_value =
		operation.auxiliary.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
	value_offset = out->literal_value
		? uint64_t{operation.auxiliary.index} * sizeof(zval)
		: (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.auxiliary_storage_id)
			* sizeof(zval);
	if (receiver_offset > UINT32_MAX || value_offset > UINT32_MAX
			|| operation.extended_value
				> UINT32_MAX - 3 * sizeof(void *)) {
		return false;
	}
	out->receiver_offset = static_cast<uint32_t>(receiver_offset);
	out->value_offset = static_cast<uint32_t>(value_offset);
	out->cache_offset = operation.extended_value;
	out->move_value = !out->literal_value
		&& operation.auxiliary.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
	return true;
}

bool zend_tpde_dynamic_fetch_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_dynamic_fetch_read *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t name_offset;
	uint64_t result_offset;

	if (!instruction.has_value_operation
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

uint32_t zend_tpde_register_mutation_ssa_plus_one(
	const zend_tpde_instruction &instruction, bool slot_incdec)
{
	zend_tpde_long_assign_op long_assign{};
	zend_tpde_long_incdec long_incdec{};
	if (!((zend_tpde_long_assign_op_at(instruction, &long_assign)
				&& !long_assign.has_result)
			|| (zend_tpde_long_incdec_at(instruction, &long_incdec)
				&& !long_incdec.has_result
				&& (!slot_incdec
					|| instruction.value_operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT)))) {
		return 0;
	}
	return instruction.value_operation.op1_definition_ssa_variable_id_plus_one;
}

void zend_tpde_collect_reference_assigned_storages(
	const zend_tpde_plan *plan, std::vector<zend_mir_storage_id> &storages)
{
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &candidate = plan->instructions[index];
		if (candidate.has_value_operation
				&& candidate.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ASSIGN_REF) {
			for (const zend_mir_storage_id storage : {
					candidate.value_operation.op1_storage_id,
					candidate.value_operation.op2_storage_id}) {
				if (zend_mir_id_is_valid(storage)) {
					storages.push_back(storage);
				}
			}
		}
	}
	std::sort(storages.begin(), storages.end());
	storages.erase(
		std::unique(storages.begin(), storages.end()), storages.end());
}

const zend_tpde_source_call_phase_entry *
zend_tpde_source_call_phase_at(
	const zend_tpde_plan *plan, uint32_t source_position)
{
	if (plan->source_call_phases == nullptr
			|| source_position >= plan->source_opcode_count) {
		return nullptr;
	}
	const zend_tpde_source_call_phase_entry *entry =
		&plan->source_call_phases[source_position];
	return entry->phases == ZEND_TPDE_SOURCE_CALL_PHASE_NONE ? nullptr : entry;
}

bool zend_tpde_machine_result_representation(
	zend_mir_scalar_type_mask exact_type,
	zend_tpde_machine_value_kind machine_kind,
	bool constant, bool register_authoritative)
{
	if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
		return false;
	}
	if (zend_mir_scalar_type_is_exact(exact_type)) {
		return constant || register_authoritative;
	}
	switch (machine_kind) {
		case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
		case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
		case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
		case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
		case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
		case ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL:
			return register_authoritative;
		default:
			return false;
	}
}

bool zend_tpde_direct_call_has_argument_guard(
	const zend_tpde_instruction &instruction)
{
	if (instruction.direct_call_argument_guard_types == nullptr) {
		return false;
	}
	for (uint32_t argument = 0;
			argument < instruction.call_argument_count; ++argument) {
		if (zend_mir_scalar_type_is_exact(
				instruction.direct_call_argument_guard_types[argument])) {
			return true;
		}
	}
	return false;
}

/*
 * The reads a private frame runs: their fast paths read the frame, $this
 * and containers without an effect, and every other case is a helper,
 * which a private frame retries instead (zend_tpde_private_retry_body()).
 */
static bool zend_tpde_private_retry_opcode(const zend_op *opline)
{
	switch (opline->opcode) {
		case ZEND_NOP:
		case ZEND_EXT_STMT:
		case ZEND_RECV:
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
		case ZEND_ISSET_ISEMPTY_PROP_OBJ:
		case ZEND_FETCH_OBJ_R:
		case ZEND_FETCH_OBJ_IS:
		case ZEND_FETCH_DIM_R:
		case ZEND_FETCH_DIM_IS:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_BOOL:
		case ZEND_BOOL_NOT:
		case ZEND_TYPE_CHECK:
		case ZEND_QM_ASSIGN:
		case ZEND_RETURN:
			return true;
		case ZEND_JMP:
			return OP_JMP_ADDR(opline, opline->op1) > opline;
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
		case ZEND_COALESCE:
			return OP_JMP_ADDR(opline, opline->op2) > opline;
		default:
			return false;
	}
}

bool zend_tpde_private_retry_body(const zend_op_array *op_array)
{
	if (!zend_tpde_linux_inline_forms || op_array == nullptr
			|| op_array->type != ZEND_USER_FUNCTION
			|| op_array->function_name == nullptr
			|| (op_array->fn_flags & (ZEND_ACC_GENERATOR | ZEND_ACC_VARIADIC
				| ZEND_ACC_RETURN_REFERENCE | ZEND_ACC_HAS_FINALLY_BLOCK
				| ZEND_ACC_CALL_VIA_TRAMPOLINE | ZEND_ACC_DEPRECATED
				| ZEND_ACC_NODISCARD | ZEND_ACC_CLOSURE)) != 0
			|| op_array->last_try_catch != 0
			|| op_array->static_variables != nullptr) {
		return false;
	}
	for (uint32_t argument = 0; argument < op_array->num_args; ++argument) {
		if (op_array->arg_info != nullptr
				&& ZEND_ARG_SEND_MODE(&op_array->arg_info[argument]) != 0) {
			return false;
		}
	}
	for (uint32_t index = 0; index < op_array->last_live_range; ++index) {
		if ((op_array->live_range[index].var & ZEND_LIVE_MASK)
				!= ZEND_LIVE_TMPVAR) {
			return false;
		}
	}
	for (uint32_t index = 0; index < op_array->last; ++index) {
		const zend_op *opline = &op_array->opcodes[index];
		if (!zend_tpde_private_retry_opcode(opline)
				|| zend_get_user_opcode_handler(opline->opcode) != nullptr) {
			return false;
		}
	}
	return true;
}

bool zend_tpde_numeric_binary_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return zend_tpde_linux_inline_forms
		&& instruction.has_value_operation
		&& operation.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
		&& (zend_tpde_numeric_arithmetic(operation.source_opcode)
			|| zend_tpde_numeric_comparison(operation.source_opcode)
			|| zend_tpde_numeric_bitwise(operation.source_opcode))
		&& plan->source_opcodes != nullptr
		&& operation.source_position_id < plan->source_opcode_count
		&& plan->source_opcodes[operation.source_position_id].opcode
			== operation.source_opcode;
}

bool zend_tpde_numeric_binary_proven(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction)
{
	if (!zend_tpde_numeric_binary_at(plan, instruction)) {
		return false;
	}
	const zend_tpde_source_opcode &source =
		plan->source_opcodes[instruction.value_operation.source_position_id];
	return instruction.numeric_operands_proven
		|| (zend_tpde_known_numeric_type(source.op1_known_type)
			&& zend_tpde_known_numeric_type(source.op2_known_type));
}

bool zend_tpde_typed_numeric_binary(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return zend_tpde_numeric_binary_proven(plan, instruction)
		&& (operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
			|| operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
		&& (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
			|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
			|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV)
		&& operation.result.ssa_variable_id != ZEND_MIR_ID_INVALID
		&& operation.result.ssa_variable_id
			< plan->source_ssa_variable_count;
}

bool zend_tpde_typed_forward_assign(
	const zend_tpde_instruction &instruction)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	return instruction.has_value_operation
		&& operation.opcode == ZEND_MIR_OPCODE_VALUE_QM_ASSIGN
		&& (operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
			|| operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
		&& (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
			|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
			|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV)
		&& operation.result.ssa_variable_id != ZEND_MIR_ID_INVALID;
}

bool zend_tpde_user_multi_branch_at(
	const zend_tpde_plan *plan,
	const zend_mir_executable_value_ref &operation,
	uint32_t target_opcode,
	zend_tpde_user_multi_branch *out)
{
	if (plan->source_multi_branches == nullptr
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

bool zend_tpde_multi_branch_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction &instruction,
	const zend_mir_instruction_record &record,
	zend_tpde_multi_branch *out)
{
	const zend_mir_executable_value_ref &operation =
		instruction.value_operation;
	uint64_t operand_offset;
	uint32_t expected_successors;

	if (!instruction.has_value_operation
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

zend_tpde_machine_value_kind zend_tpde_typed_numeric_result_kind(
	const zend_tpde_plan *plan, const zend_tpde_instruction &instruction)
{
	if (zend_tpde_numeric_comparison(
			instruction.value_operation.source_opcode)) {
		return ZEND_TPDE_MACHINE_VALUE_BOOL;
	}
	if (zend_tpde_numeric_bitwise(
			instruction.value_operation.source_opcode)
			|| instruction.typed_overflow_speculated) {
		return ZEND_TPDE_MACHINE_VALUE_I64;
	}
	const zend_mir_value_id result_ssa =
		instruction.value_operation.result.ssa_variable_id;
	const int32_t result_value =
		instruction.source_result_binding.value_index >= 0
			? instruction.source_result_binding.value_index
		: result_ssa != ZEND_MIR_ID_INVALID
			? zend_tpde_value_index(plan,
				zend_mir_value_from_original_ssa(result_ssa))
			: -1;
	return result_value >= 0
			&& static_cast<uint32_t>(result_value) < plan->value_count
			&& plan->values[result_value].exact_type
				== ZEND_MIR_SCALAR_TYPE_I64
		? ZEND_TPDE_MACHINE_VALUE_I64 : ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
}

bool zend_tpde_scalar_diamond_at(
		const zend_tpde_plan *plan,
		zend_tpde_scalar_diamond *out)
{
	if (plan->block_count != 4
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

bool zend_tpde_scalar_diamond_frame_transport(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction)
{
	if (instruction.record.opcode != ZEND_MIR_OPCODE_ZVAL_STORE
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

bool zend_tpde_inline_body_begin(
		const zend_tpde_instruction &call,
		const zend_tpde_plan *callee,
		zend_tpde_inline_body_shape *shape)
{
	if (call.direct_call == nullptr || callee == nullptr
			|| (call.direct_call->receiver_kind
					!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE
				&& (call.direct_call->receiver_kind
						!= ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
					|| (call.direct_call->flags
							& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0))
			|| (call.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_I1
				&& call.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_I64)
			|| callee->generator_resume_count != 0
			|| callee->user_opcode_callbacks) {
		return false;
	}
	shape->scalar_diamond = callee->block_count != 1;
	return !shape->scalar_diamond
		|| zend_tpde_scalar_diamond_at(callee, &shape->diamond);
}

zend_tpde_inline_body_step zend_tpde_inline_body_step_at(
		const zend_tpde_instruction &call,
		const zend_tpde_plan *callee,
		const zend_tpde_instruction &instruction,
		const zend_mir_instruction_record &record,
		zend_tpde_inline_body_shape *shape)
{
	const bool scalar_diamond = shape->scalar_diamond;
	const zend_tpde_scalar_diamond &diamond = shape->diamond;
	const int32_t block_index =
		zend_tpde_block_index(callee, record.block_id);
	if (block_index < 0
			|| (!scalar_diamond
				&& record.block_id != callee->block_ids[0])) {
		return ZEND_TPDE_INLINE_BODY_REJECT;
	}
	const uint32_t block = static_cast<uint32_t>(block_index);
	if (instruction.local_abi_transport
			|| (scalar_diamond
				&& zend_tpde_scalar_diamond_frame_transport(
					callee, instruction))) {
		return ZEND_TPDE_INLINE_BODY_SKIP;
	}
	if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
		/*
		 * Constants are mapped when a cloned instruction consumes them.
		 * Registerless NULL/UNDEF topology constants are not part of an
		 * exact scalar return body and must not reject an effect-closed
		 * candidate.
		 */
		return ZEND_TPDE_INLINE_BODY_SKIP;
	}
	if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
		return record.effects != 0
			? ZEND_TPDE_INLINE_BODY_REJECT : ZEND_TPDE_INLINE_BODY_SKIP;
	}
	if (++shape->semantic_instruction_count
			> shape->semantic_instruction_limit) {
		return ZEND_TPDE_INLINE_BODY_REJECT;
	}
	if (record.opcode == ZEND_MIR_OPCODE_PHI) {
		if (!scalar_diamond || block != diamond.merge
				|| !zend_mir_id_is_valid(record.result_id)
				|| instruction.operand_count != 2) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		++shape->phi_count;
		return ZEND_TPDE_INLINE_BODY_PHI;
	}
	if (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH) {
		if (!scalar_diamond || block != diamond.entry
				|| instruction.operand_count != 1
				|| ++shape->conditional_branch_count != 1) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		return ZEND_TPDE_INLINE_BODY_COND_BRANCH;
	}
	if (record.opcode == ZEND_MIR_OPCODE_BRANCH) {
		if (!scalar_diamond
				|| (block != diamond.true_block
					&& block != diamond.false_block)
				|| (shape->counts_arm_branches
					&& ++shape->arm_branch_count > 2)) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		return ZEND_TPDE_INLINE_BODY_BRANCH;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
			&& (instruction.runtime_helper == ZEND_NATIVE_HELPER_COUNT
				|| shape->saw_string_length
				|| (shape->checked_binary_count != 0
					&& call.direct_call->result_type
						== ZEND_MIR_SCALAR_TYPE_I64)
				|| (callee->typed_body_return_abi.valid
					&& callee->typed_body_return_abi.exact_type
						== call.direct_call->result_type))) {
		/*
		 * The typed-body signature, derived from every return before
		 * component calls are classified, proves the cloned body cannot
		 * take the coercion or TypeError path of the Zend entry.
		 */
		return ZEND_TPDE_INLINE_BODY_SKIP;
	}
	if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
		if (shape->saw_return || !instruction.has_value_operation
				|| instruction.value_operation.source_opcode
					!= ZEND_RETURN
				|| (scalar_diamond && block != diamond.merge)) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		shape->saw_return = true;
		return ZEND_TPDE_INLINE_BODY_RETURN_SOURCE_ZVAL;
	}
	if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
		if (shape->saw_return || instruction.operand_count != 1
				|| (scalar_diamond && block != diamond.merge)) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		shape->saw_return = true;
		return ZEND_TPDE_INLINE_BODY_RETURN;
	}
	if (record.opcode == ZEND_MIR_OPCODE_COPY
			|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE) {
		if (!zend_mir_id_is_valid(record.result_id)
				|| instruction.operand_count != 1) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		return ZEND_TPDE_INLINE_BODY_COPY;
	}
	const bool after_string_length =
		shape->string_length_ends_body && shape->saw_string_length;
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
		if (scalar_diamond || shape->checked_binary_count >= 8
				|| !instruction.has_value_operation
				|| (instruction.value_operation.source_opcode != ZEND_ADD
					&& instruction.value_operation.source_opcode
						!= ZEND_SUB)
				|| call.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_I64) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		++shape->checked_binary_count;
		return ZEND_TPDE_INLINE_BODY_CHECKED_BINARY;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP) {
		if (scalar_diamond || shape->checked_binary_count != 0
				|| after_string_length
				|| !instruction.has_value_operation
				|| instruction.value_operation.source_opcode != ZEND_STRLEN
				|| call.direct_call->result_type
					!= ZEND_MIR_SCALAR_TYPE_I64) {
			return ZEND_TPDE_INLINE_BODY_REJECT;
		}
		shape->saw_string_length = true;
		return ZEND_TPDE_INLINE_BODY_STRING_LENGTH;
	}
	const bool integer_or_boolean_opcode =
		(record.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
			&& record.opcode <= ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW)
		|| (record.opcode >= ZEND_MIR_OPCODE_I64_BIT_OR
			&& record.opcode <= ZEND_MIR_OPCODE_I64_CMP)
		|| record.opcode == ZEND_MIR_OPCODE_I1_NOT
		|| record.opcode == ZEND_MIR_OPCODE_I1_XOR
		|| record.opcode == ZEND_MIR_OPCODE_I1_EQ
		|| record.opcode == ZEND_MIR_OPCODE_I64_TO_I1
		|| record.opcode == ZEND_MIR_OPCODE_I1_TO_I64;
	if (shape->checked_binary_count != 0 || after_string_length
			|| !integer_or_boolean_opcode
			|| record.effects != 0
			|| !zend_mir_id_is_valid(record.result_id)
			|| instruction.operand_count > 2) {
		return ZEND_TPDE_INLINE_BODY_REJECT;
	}
	return ZEND_TPDE_INLINE_BODY_SCALAR;
}

bool zend_tpde_user_call_setup_layout(
	const zend_tpde_plan *plan,
	const zend_native_user_call_descriptor *call,
	uint64_t limit,
	zend_tpde_user_call_setup *out)
{
	const uint64_t argument_count = call->argument_count;
	const uint64_t frame_header_size =
		static_cast<uint64_t>(ZEND_CALL_FRAME_SLOT) * sizeof(zval);
	const uint64_t activation_offset =
		(frame_header_size + alignof(zend_native_direct_activation) - 1)
		& ~(static_cast<uint64_t>(
			alignof(zend_native_direct_activation)) - 1);
	const uint64_t placement_offset = activation_offset
		+ sizeof(zend_native_direct_activation);
	const uint64_t target_count = argument_count * 2 + 1;
	const uint64_t raw_setup_size = placement_offset
		+ argument_count * sizeof(zend_native_user_call_placement)
		+ target_count * sizeof(uint32_t);
	const uint64_t setup_size =
		(raw_setup_size + sizeof(zval) - 1)
		& ~(static_cast<uint64_t>(sizeof(zval)) - 1);
	const zend_mir_source_operand_ref &result_operand = call->do_result;
	const bool uses_discarded_return = result_operand.kind
		== ZEND_MIR_SOURCE_OPERAND_UNUSED;
	uint64_t result_storage = 0;
	if (!uses_discarded_return) {
		if ((result_operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& result_operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
				|| (result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)) {
			return false;
		}
		result_storage = result_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			? result_operand.index
			: static_cast<uint64_t>(plan->source_frame_variable_count)
				+ result_operand.index;
	}
	const uint64_t result_offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
	if (setup_size > limit || activation_offset > limit
			|| placement_offset - activation_offset > limit
			|| argument_count > UINT32_MAX
			|| (!uses_discarded_return && result_offset > limit)) {
		return false;
	}
	*out = {argument_count, frame_header_size, activation_offset,
		placement_offset, setup_size, result_offset, uses_discarded_return};
	return true;
}
