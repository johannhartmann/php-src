// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_plan_internal.hpp"

namespace zend::native::tpde::plan_detail {

bool source_descriptor_operand(
	const zend_op_array *op_array,
	const zend_op *opline,
	uint8_t operand_type,
	const znode_op &node,
	zend_mir_source_operand_ref *out) {
	if (op_array == nullptr || opline == nullptr || out == nullptr) {
		return false;
	}
	operand_type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	out->kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	out->slot_kind = ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	out->index = ZEND_MIR_ID_INVALID;
	out->ssa_variable_id = ZEND_MIR_ID_INVALID;
	if (operand_type == IS_UNUSED) {
		return true;
	}
	if (operand_type == IS_CONST) {
		const zval *literal = RT_CONSTANT(opline, node);
		if (literal < op_array->literals
				|| literal >= op_array->literals + op_array->last_literal) {
			return false;
		}
		out->kind = ZEND_MIR_SOURCE_OPERAND_LITERAL;
		out->index = static_cast<uint32_t>(literal - op_array->literals);
		return true;
	}
	if (operand_type != IS_CV && operand_type != IS_TMP_VAR
			&& operand_type != IS_VAR) {
		return false;
	}
	const uint32_t physical_slot = EX_VAR_TO_NUM(node.var);
	if (operand_type == IS_CV) {
		if (physical_slot >= static_cast<uint32_t>(op_array->last_var)) {
			return false;
		}
		out->slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
		out->index = physical_slot;
	} else {
		if (physical_slot < static_cast<uint32_t>(op_array->last_var)
				|| physical_slot - static_cast<uint32_t>(op_array->last_var)
					>= op_array->T) {
			return false;
		}
		out->slot_kind = operand_type == IS_TMP_VAR
			? ZEND_MIR_SOURCE_SLOT_TMP : ZEND_MIR_SOURCE_SLOT_VAR;
		out->index =
			physical_slot - static_cast<uint32_t>(op_array->last_var);
	}
	out->kind = ZEND_MIR_SOURCE_OPERAND_SLOT;
	return true;
}

zend_mir_storage_id source_descriptor_storage(
	const zend_op_array *op_array,
	const zend_mir_source_operand_ref &operand) {
	if (op_array == nullptr
			|| (operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)) {
		return ZEND_MIR_ID_INVALID;
	}
	if (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
		return operand.index < static_cast<uint32_t>(op_array->last_var)
			? operand.index : ZEND_MIR_ID_INVALID;
	}
	if ((operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
			&& operand.index < op_array->T
			&& static_cast<uint32_t>(op_array->last_var)
				<= ZEND_MIR_ID_MAX - operand.index) {
		return static_cast<uint32_t>(op_array->last_var) + operand.index;
	}
	return ZEND_MIR_ID_INVALID;
}

bool user_opcode_source_operation(
	const zend_op_array *op_array, uint32_t source_position,
	zend_mir_executable_value_ref *operation) {
	const zend_op *opline;

	if (op_array == nullptr || operation == nullptr
			|| source_position >= op_array->last) {
		return false;
	}
	opline = &op_array->opcodes[source_position];
	std::memset(operation, 0, sizeof(*operation));
	operation->id = ZEND_MIR_ID_INVALID;
	operation->block_id = ZEND_MIR_ID_INVALID;
	operation->opcode = ZEND_MIR_OPCODE_INVALID;
	operation->source_opcode = opline->opcode;
	if (!source_descriptor_operand(
			op_array, opline, opline->op1_type, opline->op1,
			&operation->op1)
			|| !source_descriptor_operand(
				op_array, opline, opline->op2_type, opline->op2,
				&operation->op2)
			|| !source_descriptor_operand(
				op_array, opline, opline->result_type, opline->result,
				&operation->result)) {
		return false;
	}
	operation->op1_unused_payload =
		opline->op1_type == IS_UNUSED ? opline->op1.num : 0;
	operation->op2_unused_payload =
		opline->op2_type == IS_UNUSED ? opline->op2.num : 0;
	operation->result_unused_payload =
		opline->result_type == IS_UNUSED ? opline->result.num : 0;
	operation->op1_storage_id =
		source_descriptor_storage(op_array, operation->op1);
	operation->op2_storage_id =
		source_descriptor_storage(op_array, operation->op2);
	operation->result_storage_id =
		source_descriptor_storage(op_array, operation->result);
	operation->auxiliary.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	operation->auxiliary.slot_kind = ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	operation->auxiliary.index = ZEND_MIR_ID_INVALID;
	operation->auxiliary.ssa_variable_id = ZEND_MIR_ID_INVALID;
	operation->auxiliary_storage_id = ZEND_MIR_ID_INVALID;
	if (source_position + 1 < op_array->last
			&& op_array->opcodes[source_position + 1].opcode == ZEND_OP_DATA) {
		const zend_op *data = &op_array->opcodes[source_position + 1];
		if (!source_descriptor_operand(
				op_array, data, data->op1_type, data->op1,
				&operation->auxiliary)) {
			return false;
		}
		operation->auxiliary_unused_payload =
			data->op1_type == IS_UNUSED ? data->op1.num : 0;
		operation->auxiliary_extended_value = data->extended_value;
		operation->auxiliary_storage_id =
			source_descriptor_storage(op_array, operation->auxiliary);
	}
	operation->extended_value = opline->extended_value;
	operation->source_position_id = source_position;
	operation->frame_state_id = ZEND_MIR_ID_INVALID;
	return true;
}

uint32_t user_opcode_source_absolute_target(
	const zend_op_array *op_array, uint32_t source_position,
	const znode_op &operand) {
	if (op_array == nullptr || source_position >= op_array->last) {
		return UINT32_MAX;
	}
#if ZEND_USE_ABS_JMP_ADDR
	const uintptr_t address = reinterpret_cast<uintptr_t>(operand.jmp_addr);
#else
	const intptr_t signed_address =
		reinterpret_cast<intptr_t>(&op_array->opcodes[source_position])
		+ static_cast<intptr_t>(operand.jmp_offset);
	const uintptr_t address = static_cast<uintptr_t>(signed_address);
#endif
	const uintptr_t first =
		reinterpret_cast<uintptr_t>(op_array->opcodes);
	const uintptr_t last =
		first + static_cast<uintptr_t>(op_array->last) * sizeof(zend_op);
	if (address < first || address >= last
			|| (address - first) % sizeof(zend_op) != 0) {
		return UINT32_MAX;
	}
	return static_cast<uint32_t>((address - first) / sizeof(zend_op));
}

uint32_t user_opcode_source_extended_target(
	const zend_op_array *op_array, uint32_t source_position,
	uint32_t extended_value) {
	if (op_array == nullptr || source_position >= op_array->last) {
		return UINT32_MAX;
	}
	const uintptr_t first =
		reinterpret_cast<uintptr_t>(op_array->opcodes);
	const uintptr_t address =
		reinterpret_cast<uintptr_t>(&op_array->opcodes[source_position])
		+ static_cast<uintptr_t>(extended_value);
	const uintptr_t last =
		first + static_cast<uintptr_t>(op_array->last) * sizeof(zend_op);
	if (address < first || address >= last
			|| (address - first) % sizeof(zend_op) != 0) {
		return UINT32_MAX;
	}
	return static_cast<uint32_t>((address - first) / sizeof(zend_op));
}

bool user_opcode_target(
	uint32_t opcode, zend_tpde_user_opcode_target *target) {
	if (target == nullptr || opcode > ZEND_VM_LAST_OPCODE) {
		return false;
	}
	target->opcode = static_cast<uint8_t>(opcode);
	target->helper = ZEND_NATIVE_HELPER_COUNT;
	switch (opcode) {
		case ZEND_NOP:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_NOP;
			return true;
		case ZEND_FAST_CALL:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_CALL;
			return true;
		case ZEND_FAST_RET:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_RETURN;
			target->helper =
				ZEND_NATIVE_HELPER_FINALLY_RETURN_EXPLICIT;
			return true;
		case ZEND_CATCH:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_CATCH;
			target->helper = ZEND_NATIVE_HELPER_CATCH_EXPLICIT;
			return true;
		case ZEND_RECV:
		case ZEND_RECV_INIT:
		case ZEND_RECV_VARIADIC:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_RECEIVE;
			target->helper = ZEND_NATIVE_HELPER_RECEIVE_EXPLICIT;
			return true;
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_PLACEHOLDER:
		case ZEND_CHECK_FUNC_ARG:
		case ZEND_CHECK_UNDEF_ARGS:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DO_ICALL:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			target->kind =
				ZEND_TPDE_USER_OPCODE_TARGET_CALL_FRAGMENT;
			target->helper =
				ZEND_NATIVE_HELPER_CALL_FRAGMENT_EXPLICIT;
			return true;
		case ZEND_JMP:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_JUMP_OP1;
			return true;
		case ZEND_JMPZ:
		case ZEND_JMPZ_EX:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_OP2;
			target->helper = ZEND_NATIVE_HELPER_VALUE_COND_BRANCH;
			return true;
		case ZEND_JMPNZ:
		case ZEND_JMPNZ_EX:
		case ZEND_JMP_SET:
		case ZEND_COALESCE:
		case ZEND_JMP_NULL:
		case ZEND_ASSERT_CHECK:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2;
			target->helper = ZEND_NATIVE_HELPER_VALUE_COND_BRANCH;
			return true;
		case ZEND_FE_RESET_R:
		case ZEND_FE_RESET_RW:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_OP2;
			target->helper = ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH;
			return true;
		case ZEND_FE_FETCH_R:
		case ZEND_FE_FETCH_RW:
			target->kind =
				ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_EXTENDED;
			target->helper = ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH;
			return true;
		case ZEND_BIND_INIT_STATIC_OR_JMP:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2;
			target->helper = ZEND_NATIVE_HELPER_VALUE_BIND_STATIC_BRANCH;
			return true;
		case ZEND_JMP_FRAMELESS:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2;
			target->helper = ZEND_NATIVE_HELPER_VALUE_FRAMELESS_BRANCH;
			return true;
		case ZEND_RETURN:
		case ZEND_RETURN_BY_REF:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_RETURN;
			target->helper = ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL;
			return true;
		case ZEND_THROW:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_THROW;
			target->helper = ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL;
			return true;
		case ZEND_SWITCH_LONG:
		case ZEND_SWITCH_STRING:
		case ZEND_MATCH:
			target->kind = ZEND_TPDE_USER_OPCODE_TARGET_MULTI_BRANCH;
			return true;
		default:
			break;
	}
	const zend_mir_opcode mapped = zend_mir_executable_opcode(opcode);
	const zend_native_runtime_helper_id helper =
		zend_mir_opcode_is_executable_value(mapped)
			? executable_value_helper(mapped) : ZEND_NATIVE_HELPER_COUNT;
	if (helper == ZEND_NATIVE_HELPER_COUNT
			|| !zend_tpde_helper_has_explicit_operands(helper)
			|| helper == ZEND_NATIVE_HELPER_VALUE_BIND_STATIC_BRANCH
			|| helper == ZEND_NATIVE_HELPER_VALUE_FRAMELESS_BRANCH) {
		return false;
	}
	target->kind = ZEND_TPDE_USER_OPCODE_TARGET_VALUE;
	target->helper = helper;
	return true;
}

bool source_descriptor_send_opcode(uint8_t opcode) {
	switch (opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_PLACEHOLDER:
			return true;
		default:
			return false;
	}
}

bool exact_scalar_satisfies_type(
	zend_mir_scalar_type_mask exact_type, const zend_type &type) {
	if (!ZEND_TYPE_IS_SET(type)) {
		return true;
	}
	if (!zend_mir_scalar_type_is_exact(exact_type)
			|| !ZEND_TYPE_IS_ONLY_MASK(type)) {
		return false;
	}
	const uint32_t accepted = ZEND_TYPE_PURE_MASK(type);
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			return (accepted & MAY_BE_NULL) != 0;
		case ZEND_MIR_SCALAR_TYPE_I1:
			/*
			 * The scalar fact distinguishes bool from other PHP types but
			 * does not distinguish true from false. Both values therefore
			 * have to satisfy the declared type.
			 */
			return (accepted & MAY_BE_BOOL) == MAY_BE_BOOL;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return (accepted & MAY_BE_LONG) != 0;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return (accepted & MAY_BE_DOUBLE) != 0;
		default:
			return false;
	}
}

bool literal_satisfies_type(const zval *literal, const zend_type &type) {
	if (!ZEND_TYPE_IS_SET(type)) {
		return true;
	}
	if (literal == nullptr || !ZEND_TYPE_IS_ONLY_MASK(type)) {
		return false;
	}
	const uint32_t accepted = ZEND_TYPE_PURE_MASK(type);
	switch (Z_TYPE_P(literal)) {
		case IS_NULL:
			return (accepted & MAY_BE_NULL) != 0;
		case IS_FALSE:
			return (accepted & MAY_BE_FALSE) != 0;
		case IS_TRUE:
			return (accepted & MAY_BE_TRUE) != 0;
		case IS_LONG:
			return (accepted & MAY_BE_LONG) != 0;
		case IS_DOUBLE:
			return (accepted & MAY_BE_DOUBLE) != 0;
		case IS_STRING:
			return (accepted & MAY_BE_STRING) != 0;
		case IS_ARRAY:
			return (accepted & MAY_BE_ARRAY) != 0;
		default:
			return false;
	}
}

zend_mir_scalar_type_mask exact_scalar_from_declared_type(
	const zend_type &type) {
	if (!ZEND_TYPE_IS_SET(type) || !ZEND_TYPE_IS_ONLY_MASK(type)) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}

	switch (ZEND_TYPE_PURE_MASK(type)) {
		case MAY_BE_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case MAY_BE_BOOL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case MAY_BE_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case MAY_BE_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

zend_tpde_local_abi_type zend_tpde_local_abi_from_declared_type(
	const zend_type *type, bool by_reference,
	zend_tpde_local_abi_transfer transfer)
{
	if (by_reference) {
		return {
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR,
			transfer,
			true,
		};
	}
	if (type == nullptr || !ZEND_TYPE_IS_SET(*type)) {
		return {
			ZEND_MIR_REPRESENTATION_ZVAL,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			transfer,
			true,
		};
	}

	const zend_mir_scalar_type_mask scalar =
		exact_scalar_from_declared_type(*type);
	if (zend_mir_scalar_type_is_exact(scalar)
			&& scalar != ZEND_MIR_SCALAR_TYPE_NULL) {
		return zend_tpde_scalar_local_abi(scalar);
	}

	const uint32_t value_types =
		ZEND_TYPE_FULL_MASK(*type) & MAY_BE_ANY;
	/* callable admits strings, arrays and objects outside its mask bit. */
	const bool exact_builtin_type = ZEND_TYPE_IS_ONLY_MASK(*type)
		&& (ZEND_TYPE_PURE_MASK(*type) & ~MAY_BE_ANY) == 0;
	if (exact_builtin_type && value_types == MAY_BE_STRING) {
		return {
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_STRING_PTR,
			transfer,
			true,
		};
	}
	if (exact_builtin_type && value_types == MAY_BE_ARRAY) {
		return {
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR,
			transfer,
			true,
		};
	}
	if (exact_builtin_type && value_types == MAY_BE_OBJECT) {
		return {
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR,
			transfer,
			true,
		};
	}
	if (exact_builtin_type && value_types == MAY_BE_RESOURCE) {
		return {
			ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR,
			transfer,
			true,
		};
	}
	if (value_types != 0) {
		return {
			ZEND_MIR_REPRESENTATION_ZVAL,
			ZEND_MIR_SCALAR_TYPE_NONE,
			ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			transfer,
			true,
		};
	}
	return {};
}

static zend_mir_scalar_type_mask exact_scalar_from_type_mask(uint32_t type) {
	switch (type) {
		case MAY_BE_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case MAY_BE_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case MAY_BE_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

static zend_tpde_machine_value_kind zend_tpde_machine_kind(
	zend_mir_representation representation,
	zend_mir_scalar_type_mask exact_type,
	zend_mir_value_category category)
{
	/*
	 * Storage identity wins over the payload type.  A by-reference CV may
	 * carry an exact long payload while the machine value itself is the
	 * observable zend_reference pointer.  Treating it as an I64 would cache
	 * the payload across aliases and generator suspension.
	 */
	if (category == ZEND_MIR_VALUE_REFERENCE_CELL) {
		return ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
	}
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_I1:
			return ZEND_TPDE_MACHINE_VALUE_BOOL;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return ZEND_TPDE_MACHINE_VALUE_I64;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return ZEND_TPDE_MACHINE_VALUE_F64;
		default:
			break;
	}
	switch (category) {
		case ZEND_MIR_VALUE_REFCOUNTED_STRING:
			return ZEND_TPDE_MACHINE_VALUE_STRING_PTR;
		case ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT:
			return ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR;
		case ZEND_MIR_VALUE_OBJECT_ABSTRACT:
			return ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR;
		case ZEND_MIR_VALUE_RESOURCE_ABSTRACT:
			return ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR;
		default:
			break;
	}
	if (representation == ZEND_MIR_REPRESENTATION_DOUBLE) {
		return ZEND_TPDE_MACHINE_VALUE_F64;
	}
	if (representation == ZEND_MIR_REPRESENTATION_I1
			|| representation == ZEND_MIR_REPRESENTATION_I8
			|| representation == ZEND_MIR_REPRESENTATION_I16
			|| representation == ZEND_MIR_REPRESENTATION_I32
			|| representation == ZEND_MIR_REPRESENTATION_I64) {
		return ZEND_TPDE_MACHINE_VALUE_I64;
	}
	return ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
}

void zend_tpde_apply_machine_value_facts(
	zend_tpde_value *value,
	bool register_definition)
{
	value->machine_kind = zend_tpde_machine_kind(
		value->representation, value->exact_type, value->category);
	if (value->canonical_alias_observable) {
		value->machine_kind = ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
		value->location = ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT;
		value->slot_state = ZEND_TPDE_CANONICAL_SLOT_CLEAN;
	} else if (value->constant) {
		value->location = ZEND_TPDE_MACHINE_LOCATION_REGISTER;
		value->slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED;
	} else if (zend_mir_id_is_valid(value->canonical_storage_id)) {
		value->location = register_definition || value->argument_index >= 0
			? ZEND_TPDE_MACHINE_LOCATION_REGISTER
			: ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT;
		value->slot_state = value->argument_index >= 0
			|| !register_definition
			? ZEND_TPDE_CANONICAL_SLOT_CLEAN
			: ZEND_TPDE_CANONICAL_SLOT_DIRTY;
	} else {
		value->location = ZEND_TPDE_MACHINE_LOCATION_REGISTER;
		value->slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED;
	}
}

/*
 * Zend SSA leaves global-scope CVs NO_ALIAS; like the JIT, treat them and
 * the CVs of a function with indirect variable access as rebindable by any
 * call, so NO_ALIAS does not prove them register-authoritative.
 */
bool zend_tpde_ssa_variable_rebindable(
	const zend_op_array *op_array, const zend_ssa *ssa, uint32_t variable)
{
	return op_array == nullptr
		|| ((op_array->function_name == nullptr
				|| (ssa->cfg.flags & ZEND_FUNC_INDIRECT_VAR_ACCESS) != 0)
			&& ssa->vars[variable].var < op_array->last_var);
}

void zend_tpde_refine_non_alias_scalar_values(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	const std::vector<uint8_t> &register_definitions)
{
	if (source_ssa == nullptr || source_ssa->vars == nullptr
			|| register_definitions.size() != plan->value_count) {
		return;
	}
	for (uint32_t ssa_variable = 0;
			ssa_variable < static_cast<uint32_t>(source_ssa->vars_count);
			++ssa_variable) {
		if (source_ssa->vars[ssa_variable].alias != NO_ALIAS
				|| zend_tpde_ssa_variable_rebindable(
					source_op_array, source_ssa, ssa_variable)) {
			continue;
		}
		const int32_t value_index = zend_tpde_value_index(
			plan, zend_mir_value_from_original_ssa(ssa_variable));
		if (value_index < 0) {
			continue;
		}
		zend_tpde_value &value =
			plan->values[static_cast<uint32_t>(value_index)];
		if (value.exact_type != ZEND_MIR_SCALAR_TYPE_I1
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_I64
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_F64) {
			continue;
		}
		/*
		 * Value lowering keeps MAY_BE_REF after variable sends. NO_ALIAS
		 * proves this definition is no observable zend_reference, so the
		 * scalar stays register-authoritative across calls, copies and PHIs.
		 */
		value.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
		value.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
		value.canonical_alias_observable = false;
		zend_tpde_apply_machine_value_facts(
			&value,
			register_definitions[static_cast<uint32_t>(value_index)] != 0);
	}
}

void zend_tpde_refine_literal_assignment_values(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa)
{
	if (source_op_array == nullptr || source_ssa == nullptr
			|| source_ssa->ops == nullptr || source_ssa->vars == nullptr) {
		return;
	}
	for (uint32_t ssa_variable = 0;
			ssa_variable < static_cast<uint32_t>(source_ssa->vars_count);
			++ssa_variable) {
		const int32_t value_index = zend_tpde_value_index(
			plan, zend_mir_value_from_original_ssa(ssa_variable));
		const int32_t definition = source_ssa->vars[ssa_variable].definition;
		if (value_index < 0 || definition < 0
				|| static_cast<uint32_t>(definition)
					>= source_op_array->last) {
			continue;
		}
		zend_tpde_value &value =
			plan->values[static_cast<uint32_t>(value_index)];
		const zend_op &op =
			source_op_array->opcodes[static_cast<uint32_t>(definition)];
		const zend_ssa_op &ssa_op =
			source_ssa->ops[static_cast<uint32_t>(definition)];
		const zval *literal =
			op.op2_type == IS_CONST
				? RT_CONSTANT(&op, op.op2) : nullptr;
		if (op.opcode != ZEND_ASSIGN
				|| ssa_op.op1_def != static_cast<int32_t>(ssa_variable)
				|| op.op2_type != IS_CONST
				|| (Z_TYPE_P(literal) != IS_STRING
					&& Z_TYPE_P(literal) != IS_ARRAY)) {
			continue;
		}
		/*
		 * Zend's SEND_VAR_EX analysis keeps the destination aliasable until
		 * the callee is known. A literal assignment into a non-reference
		 * predecessor still defines an exact string or array payload, even
		 * when a later by-value use widened the SSA type mask.
		 */
		if (source_ssa->vars[ssa_variable].alias != NO_ALIAS
				|| zend_tpde_ssa_variable_rebindable(
					source_op_array, source_ssa, ssa_variable)) {
			continue;
		}
		if (ssa_op.op1_use >= 0) {
			const int32_t predecessor = zend_tpde_value_index(
				plan, zend_mir_value_from_original_ssa(
					static_cast<uint32_t>(ssa_op.op1_use)));
			if (predecessor < 0
					|| source_ssa->vars[ssa_op.op1_use].alias != NO_ALIAS) {
				continue;
			}
		}
		value.category = Z_TYPE_P(literal) == IS_STRING
			? ZEND_MIR_VALUE_REFCOUNTED_STRING
			: ZEND_MIR_VALUE_REFCOUNTED_CONTAINER_ABSTRACT;
		value.refcount_state =
			Z_TYPE_P(literal) == IS_STRING
				&& ZSTR_IS_INTERNED(Z_STR_P(literal))
			? ZEND_MIR_REFCOUNT_IMMORTAL
			: ZEND_MIR_REFCOUNT_UNKNOWN;
		value.canonical_alias_observable = false;
		value.machine_kind = Z_TYPE_P(literal) == IS_STRING
			? ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			: ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR;
		if (Z_TYPE_P(literal) == IS_STRING) {
			value.known_string_literal = true;
			value.known_string_length = Z_STRLEN_P(literal);
			value.known_string_first_byte = Z_STRLEN_P(literal) == 0
				? 0 : static_cast<uint8_t>(Z_STRVAL_P(literal)[0]);
		}
		value.location =
			ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT;
		value.slot_state = ZEND_TPDE_CANONICAL_SLOT_CLEAN;
	}
}

void zend_tpde_refine_boxed_scalar_copies(zend_tpde_plan *plan)
{
	if (plan->values == nullptr
			|| plan->instructions == nullptr) {
		return;
	}
	/* The value consumers are not frozen yet: the COPYs per source. */
	std::unordered_map<uint32_t, std::vector<uint32_t>> copies_of;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		const int32_t source_index = zend_tpde_value_index(
			plan, zend_tpde_operand_at(plan, &instruction, 0));
		if (zend_tpde_instruction_record_at(plan, &instruction).opcode
					== ZEND_MIR_OPCODE_COPY
				&& instruction.operand_count == 1 && source_index >= 0) {
			copies_of[static_cast<uint32_t>(source_index)].push_back(index);
		}
	}
	sweep_to_fixed_point(plan->instruction_count,
		[&](uint32_t index) -> int32_t {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if (record.opcode != ZEND_MIR_OPCODE_COPY
					|| record.representation != ZEND_MIR_REPRESENTATION_ZVAL
					|| instruction.operand_count != 1) {
				return -1;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			const int32_t source_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, 0));
			if (result_index < 0 || source_index < 0) {
				return -1;
			}
			zend_tpde_value &result = plan->values[result_index];
			const zend_tpde_value &source = plan->values[source_index];
			if (result.exact_type != ZEND_MIR_SCALAR_TYPE_NONE
					|| result.canonical_alias_observable
					|| (result.category != ZEND_MIR_VALUE_CATEGORY_UNKNOWN
						&& result.category
							!= ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR)
					|| (source.exact_type != ZEND_MIR_SCALAR_TYPE_I1
						&& source.exact_type != ZEND_MIR_SCALAR_TYPE_I64
						&& source.exact_type != ZEND_MIR_SCALAR_TYPE_F64)) {
				return -1;
			}
			/*
			 * A COPY into a boxed PHI component keeps the source payload type
			 * although its MIR representation is ZVAL. The boxed value stays
			 * slot-authoritative; the adjacent ZVAL_STORE consumes the
			 * transient scalar definition.
			 */
			result.exact_type = source.exact_type;
			result.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
			result.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
			return result_index;
		},
		[&](uint32_t value_index, const auto &mark) {
			const auto copies = copies_of.find(value_index);
			if (copies != copies_of.end()) {
				for (const uint32_t copy : copies->second) {
					mark(copy);
				}
			}
		});
}

zend_mir_scalar_type_mask exact_scalar_from_zval(const zval *value) {
	if (value == nullptr) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	switch (Z_TYPE_P(value)) {
		case IS_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case IS_FALSE:
		case IS_TRUE:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case IS_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case IS_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

uint64_t scalar_bits_from_zval(const zval *value) {
	uint64_t bits = 0;

	if (value == nullptr) {
		return 0;
	}
	switch (Z_TYPE_P(value)) {
		case IS_TRUE:
			return 1;
		case IS_LONG:
			return static_cast<uint64_t>(Z_LVAL_P(value));
		case IS_DOUBLE:
			static_assert(sizeof(bits) == sizeof(Z_DVAL_P(value)));
			memcpy(&bits, &Z_DVAL_P(value), sizeof(bits));
			return bits;
		default:
			return 0;
	}
}

static zend_mir_scalar_type_mask exact_scalar_from_call_result(
	const zend_op_array *op_array,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	uint32_t opline_index) {
	if (op_array == nullptr || calls == nullptr) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	const uint32_t site_count = calls->call_site_count(calls->context);
	for (uint32_t i = 0; i < site_count; ++i) {
		zend_mir_call_site_ref site;
		if (!calls->call_site_at(calls->context, i, &site)) {
			return ZEND_MIR_SCALAR_TYPE_NONE;
		}
		if (site.source_do_opline_index != opline_index) {
			continue;
		}
		for (uint32_t n = 0; n < binding_count; ++n) {
			if (bindings[n].target_id != site.target_id
					|| bindings[n].entry_cell == nullptr
					|| bindings[n].entry_cell->function == nullptr
					|| !ZEND_USER_CODE(
						bindings[n].entry_cell->function->type)
					|| bindings[n].entry_cell->function->op_array.arg_info
						== nullptr
					|| (bindings[n].entry_cell->function->common.fn_flags
							& ZEND_ACC_HAS_RETURN_TYPE) == 0) {
				continue;
			}
			return exact_scalar_from_declared_type(
				bindings[n].entry_cell->function->op_array.arg_info[-1].type);
		}
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	return ZEND_MIR_SCALAR_TYPE_NONE;
}

static constexpr uint32_t EXACT_SCALAR_SSA_DEPTH_LIMIT = 64;

static zend_mir_scalar_type_mask exact_scalar_from_ssa_value_at(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	uint32_t ssa_variable_id,
	uint32_t depth,
	exact_scalar_ssa_memo *memo,
	uint32_t *height,
	bool *limited) {
	*height = 0;
	if (op_array == nullptr || ssa == nullptr || ssa->var_info == nullptr
			|| ssa->vars == nullptr || ssa->ops == nullptr
			|| ssa_variable_id >= static_cast<uint32_t>(ssa->vars_count)) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	if (depth > EXACT_SCALAR_SSA_DEPTH_LIMIT) {
		*limited = true;
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	if (memo != nullptr) {
		if (memo->types.empty()) {
			memo->types.assign(static_cast<uint32_t>(ssa->vars_count),
				ZEND_MIR_SCALAR_TYPE_NONE);
			memo->heights.assign(static_cast<uint32_t>(ssa->vars_count), 0);
		}
		const uint32_t known = memo->heights[ssa_variable_id];
		if (known != 0
				&& depth + (known - 1) <= EXACT_SCALAR_SSA_DEPTH_LIMIT) {
			*height = known - 1;
			return memo->types[ssa_variable_id];
		}
	}
	bool search_limited = false;
	const zend_mir_scalar_type_mask result = [&]() -> zend_mir_scalar_type_mask {
	auto exact_scalar_from_ssa_value = [&](
			const zend_op_array *, const zend_ssa *,
			const zend_mir_call_view *, const zend_native_call_binding *,
			uint32_t, uint32_t source, uint32_t source_depth) {
		uint32_t source_height = 0;
		const zend_mir_scalar_type_mask type =
			exact_scalar_from_ssa_value_at(op_array, ssa, calls, bindings,
				binding_count, source, source_depth, memo, &source_height,
				&search_limited);
		*height = std::max(*height, source_height + 1);
		return type;
	};
	zend_mir_scalar_type_mask exact =
		exact_scalar_from_type_mask(ssa->var_info[ssa_variable_id].type);
	if (zend_mir_scalar_type_is_exact(exact)) {
		return exact;
	}
	const zend_ssa_var &variable = ssa->vars[ssa_variable_id];
	if (variable.definition_phi != nullptr) {
		const zend_ssa_phi *phi = variable.definition_phi;
		if (ssa->cfg.blocks == nullptr
				|| phi->block >= static_cast<uint32_t>(ssa->cfg.blocks_count)) {
			return ZEND_MIR_SCALAR_TYPE_NONE;
		}
		const zend_basic_block &block = ssa->cfg.blocks[phi->block];
		exact = ZEND_MIR_SCALAR_TYPE_NONE;
		for (uint32_t i = 0; i < block.predecessors_count; ++i) {
			if (phi->sources[i] < 0) {
				return ZEND_MIR_SCALAR_TYPE_NONE;
			}
			const zend_mir_scalar_type_mask source =
				exact_scalar_from_ssa_value(
					op_array, ssa, calls, bindings, binding_count,
					static_cast<uint32_t>(phi->sources[i]), depth + 1);
			if (!zend_mir_scalar_type_is_exact(source)
					|| (zend_mir_scalar_type_is_exact(exact)
						&& exact != source)) {
				return ZEND_MIR_SCALAR_TYPE_NONE;
			}
			exact = source;
		}
		return exact;
	}
	if (variable.definition < 0
			|| static_cast<uint32_t>(variable.definition) >= op_array->last) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	const uint32_t definition = static_cast<uint32_t>(variable.definition);
	const zend_op &opline = op_array->opcodes[definition];
	const zend_ssa_op &ssa_op = ssa->ops[definition];
	switch (opline.opcode) {
		case ZEND_RECV:
		case ZEND_RECV_INIT:
			if (op_array->arg_info != nullptr && opline.op1.num != 0
					&& opline.op1.num <= op_array->num_args) {
				return exact_scalar_from_declared_type(
					op_array->arg_info[opline.op1.num - 1].type);
			}
			break;
		case ZEND_ASSIGN:
			if (ssa_op.op2_use >= 0) {
				return exact_scalar_from_ssa_value(
					op_array, ssa, calls, bindings, binding_count,
					static_cast<uint32_t>(ssa_op.op2_use), depth + 1);
			}
			if (opline.op2_type == IS_CONST) {
				return exact_scalar_from_zval(RT_CONSTANT(&opline, opline.op2));
			}
			break;
		case ZEND_QM_ASSIGN:
		case ZEND_VERIFY_RETURN_TYPE:
			if (ssa_op.op1_use >= 0) {
				return exact_scalar_from_ssa_value(
					op_array, ssa, calls, bindings, binding_count,
					static_cast<uint32_t>(ssa_op.op1_use), depth + 1);
			}
			if (opline.op1_type == IS_CONST) {
				return exact_scalar_from_zval(RT_CONSTANT(&opline, opline.op1));
			}
			break;
		case ZEND_DO_FCALL:
		case ZEND_DO_ICALL:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL_BY_NAME:
			return exact_scalar_from_call_result(
				op_array, calls, bindings, binding_count, definition);
		case ZEND_BOOL:
		case ZEND_BOOL_NOT:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case ZEND_CAST:
			switch (opline.extended_value) {
				case IS_NULL:
					return ZEND_MIR_SCALAR_TYPE_NULL;
				case _IS_BOOL:
					return ZEND_MIR_SCALAR_TYPE_I1;
				case IS_LONG:
					return ZEND_MIR_SCALAR_TYPE_I64;
				case IS_DOUBLE:
					return ZEND_MIR_SCALAR_TYPE_F64;
				default:
					break;
			}
			break;
		default:
			break;
	}
	return ZEND_MIR_SCALAR_TYPE_NONE;
	}();
	if (search_limited) {
		*limited = true;
	} else if (memo != nullptr) {
		memo->types[ssa_variable_id] = result;
		memo->heights[ssa_variable_id] = *height + 1;
	}
	return result;
}

static zend_mir_scalar_type_mask exact_scalar_from_ssa_value(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	uint32_t ssa_variable_id,
	exact_scalar_ssa_memo *memo) {
	uint32_t height = 0;
	bool limited = false;
	return exact_scalar_from_ssa_value_at(op_array, ssa, calls, bindings,
		binding_count, ssa_variable_id, 0, memo, &height, &limited);
}

/* The SSA variable a call argument sends: its operand's, or its SEND's
 * op1 use; ZEND_MIR_ID_INVALID if neither is known. */
static uint32_t source_argument_ssa_variable(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_argument_ref &argument) {
	uint32_t ssa_variable_id = argument.source_operand.ssa_variable_id;
	if (ssa_variable_id == ZEND_MIR_ID_INVALID
			&& argument.send_opline_index < op_array->last
			&& ssa->ops[argument.send_opline_index].op1_use >= 0) {
		ssa_variable_id = static_cast<uint32_t>(
			ssa->ops[argument.send_opline_index].op1_use);
	}
	return ssa_variable_id < static_cast<uint32_t>(ssa->vars_count)
		? ssa_variable_id : ZEND_MIR_ID_INVALID;
}

/*
 * The argument sends a parameter CV as its RECV defined it: the SSA value it
 * sends is the result of the parameter's RECV or RECV_INIT.
 */
bool source_argument_is_received_parameter(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_argument_ref &argument) {
	if (op_array == nullptr || ssa == nullptr || ssa->vars == nullptr
			|| ssa->ops == nullptr) {
		return false;
	}
	const uint32_t ssa_variable_id =
		source_argument_ssa_variable(op_array, ssa, argument);
	if (ssa_variable_id == ZEND_MIR_ID_INVALID) {
		return false;
	}
	const zend_ssa_var &variable = ssa->vars[ssa_variable_id];
	if (variable.definition < 0
			|| static_cast<uint32_t>(variable.definition) >= op_array->last
			|| variable.var < 0
			|| static_cast<uint32_t>(variable.var)
				!= argument.source_operand.index) {
		return false;
	}
	const zend_op &receive = op_array->opcodes[variable.definition];
	return (receive.opcode == ZEND_RECV || receive.opcode == ZEND_RECV_INIT)
		&& receive.op1.num == argument.source_operand.index + 1
		&& ssa->ops[variable.definition].result_def
			== static_cast<int>(ssa_variable_id);
}

zend_mir_scalar_type_mask exact_scalar_from_source_argument(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	const zend_mir_call_argument_ref &argument,
	exact_scalar_ssa_memo *memo) {
	if (op_array == nullptr) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	if (argument.source_operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		if (argument.source_operand.index >= op_array->last_literal) {
			return ZEND_MIR_SCALAR_TYPE_NONE;
		}
		return exact_scalar_from_zval(
			&op_array->literals[argument.source_operand.index]);
	}
	if (ssa == nullptr || ssa->var_info == nullptr || ssa->ops == nullptr) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	const uint32_t ssa_variable_id =
		source_argument_ssa_variable(op_array, ssa, argument);
	if (ssa_variable_id == ZEND_MIR_ID_INVALID) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	return exact_scalar_from_ssa_value(
		op_array, ssa, calls, bindings, binding_count, ssa_variable_id, memo);
}

bool source_call_argument_may_be_undefined(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_argument_ref &argument) {
	if (op_array == nullptr || ssa == nullptr || ssa->var_info == nullptr
			|| ssa->ops == nullptr) {
		return false;
	}
	const uint32_t ssa_variable_id =
		source_argument_ssa_variable(op_array, ssa, argument);
	return ssa_variable_id != ZEND_MIR_ID_INVALID
		&& (ssa->var_info[ssa_variable_id].type & MAY_BE_UNDEF) != 0;
}

bool source_call_argument_stable_after_send(
	const zend_op_array *op_array,
	const zend_mir_call_argument_ref &argument,
	uint32_t call_source_position) {
	const zend_mir_storage_id storage =
		source_descriptor_storage(op_array, argument.source_operand);

	if (op_array == nullptr || !zend_mir_id_is_valid(storage)
			|| argument.send_opline_index >= call_source_position
			|| call_source_position > op_array->last) {
		return false;
	}
	const uint32_t encoded_storage = EX_NUM_TO_VAR(storage);
	for (uint32_t source = argument.send_opline_index + 1;
			source < call_source_position; ++source) {
		const zend_op *opline = &op_array->opcodes[source];
		const bool op1_slot = opline->op1_type == IS_CV
			|| opline->op1_type == IS_TMP_VAR
			|| opline->op1_type == IS_VAR;
		const bool op2_slot = opline->op2_type == IS_CV
			|| opline->op2_type == IS_TMP_VAR
			|| opline->op2_type == IS_VAR;
		const bool result_slot = opline->result_type == IS_CV
			|| opline->result_type == IS_TMP_VAR
			|| opline->result_type == IS_VAR;
		/*
		 * A later read is unstable too: a delayed TMP/VAR argument must not be
		 * consumed after another SEND has observed or reused its physical slot.
		 */
		if ((op1_slot && opline->op1.var == encoded_storage)
				|| (op2_slot && opline->op2.var == encoded_storage)
				|| (result_slot && opline->result.var == encoded_storage)) {
			return false;
		}
	}
	return true;
}

bool direct_call_parameter_ordinal(
	const zend_op_array *source_op_array,
	const zend_function *callee,
	const zend_mir_call_argument_ref &argument,
	uint32_t *parameter_ordinal) {
	*parameter_ordinal = argument.ordinal;
	if (argument.source_mode != ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED) {
		return true;
	}
	if (source_op_array == nullptr
			|| argument.send_opline_index >= source_op_array->last
			|| callee == nullptr || callee->common.arg_info == nullptr) {
		return false;
	}
	const zend_op *send =
		&source_op_array->opcodes[argument.send_opline_index];
	const zval *name = send->op2_type == IS_CONST
		? RT_CONSTANT(send, send->op2) : nullptr;
	if (name == nullptr || Z_TYPE_P(name) != IS_STRING) {
		return false;
	}
	for (uint32_t parameter = 0;
			parameter < callee->common.num_args; ++parameter) {
		const zend_string *parameter_name =
			callee->common.arg_info[parameter].name;
		if (parameter_name != nullptr
				&& zend_string_equals(parameter_name, Z_STR_P(name))) {
			*parameter_ordinal = parameter;
			return true;
		}
	}
	return false;
}

bool source_operand_value_id(
	const zend_mir_source_operand_ref &operand, zend_mir_value_id &value_id) {
	switch (operand.kind) {
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			value_id = zend_mir_value_from_synthetic(operand.index);
			return zend_mir_id_is_valid(value_id);
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			if (operand.ssa_variable_id == ZEND_MIR_ID_INVALID) {
				return false;
			}
			value_id = zend_mir_value_from_original_ssa(
				operand.ssa_variable_id);
			return zend_mir_id_is_valid(value_id);
		default:
			return false;
	}
}

zend_mir_scalar_type_mask source_operand_exact_type(
	const zend_tpde_plan *plan,
	const zend_mir_source_operand_ref &operand) {
	zend_mir_value_id value_id;
	if (!source_operand_value_id(operand, value_id)) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	const int32_t index = zend_tpde_value_index(plan, value_id);
	return index >= 0
		? plan->values[index].exact_type : ZEND_MIR_SCALAR_TYPE_NONE;
}

zend_native_runtime_helper_id executable_value_helper(zend_mir_opcode opcode) {
	if (opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
			&& opcode <= ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME) {
		return static_cast<zend_native_runtime_helper_id>(
			static_cast<uint32_t>(opcode)
				- static_cast<uint32_t>(ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS)
				+ static_cast<uint32_t>(
					ZEND_NATIVE_HELPER_OBJECT_DECLARE_ANON_CLASS));
	}
	if (opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
			&& opcode <= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL) {
		return static_cast<zend_native_runtime_helper_id>(
			static_cast<uint32_t>(opcode)
				- static_cast<uint32_t>(ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
				+ static_cast<uint32_t>(ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R));
	}
	switch (opcode) {
		/* Each executable value operation has the helper of its name. */
#define EXECUTABLE_VALUE_HELPER(symbol, label, value) \
		case ZEND_MIR_OPCODE_##symbol: \
			return ZEND_NATIVE_HELPER_##symbol;
		ZEND_MIR_EXECUTABLE_VALUE_OPCODE_CATALOG(EXECUTABLE_VALUE_HELPER)
#undef EXECUTABLE_VALUE_HELPER
		case ZEND_MIR_OPCODE_VALUE_CASE:
			return ZEND_NATIVE_HELPER_VALUE_CASE;
		case ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH:
			return ZEND_NATIVE_HELPER_VALUE_BIND_STATIC_BRANCH;
		case ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH:
			return ZEND_NATIVE_HELPER_VALUE_FRAMELESS_BRANCH;
		case ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE:
			return ZEND_NATIVE_HELPER_VERIFY_RETURN_TYPE;
		case ZEND_MIR_OPCODE_VALUE_ECHO:
			return ZEND_NATIVE_HELPER_VALUE_ECHO;
		case ZEND_MIR_OPCODE_FUNC_NUM_ARGS:
			return ZEND_NATIVE_HELPER_VALUE_FUNC_NUM_ARGS;
		case ZEND_MIR_OPCODE_FUNC_GET_ARGS:
			return ZEND_NATIVE_HELPER_VALUE_FUNC_GET_ARGS;
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_FUNCTION:
			return ZEND_NATIVE_HELPER_OBJECT_DECLARE_FUNCTION;
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS:
			return ZEND_NATIVE_HELPER_OBJECT_DECLARE_CLASS;
		case ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED:
			return ZEND_NATIVE_HELPER_OBJECT_DECLARE_CLASS_DELAYED;
		case ZEND_MIR_OPCODE_GENERATOR_CREATE:
			return ZEND_NATIVE_HELPER_GENERATOR_CREATE;
		case ZEND_MIR_OPCODE_GENERATOR_YIELD:
			return ZEND_NATIVE_HELPER_GENERATOR_YIELD;
		case ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM:
			return ZEND_NATIVE_HELPER_GENERATOR_YIELD_FROM;
		case ZEND_MIR_OPCODE_GENERATOR_RETURN:
			return ZEND_NATIVE_HELPER_GENERATOR_RETURN;
		case ZEND_MIR_OPCODE_VALUE_COUNT:
			return ZEND_NATIVE_HELPER_VALUE_COUNT;
		case ZEND_MIR_OPCODE_VALUE_GET_TYPE:
			return ZEND_NATIVE_HELPER_VALUE_GET_TYPE;
		case ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS:
			return ZEND_NATIVE_HELPER_VALUE_ARRAY_KEY_EXISTS;
		case ZEND_MIR_OPCODE_VALUE_IN_ARRAY:
			return ZEND_NATIVE_HELPER_VALUE_IN_ARRAY;
		case ZEND_MIR_OPCODE_VALUE_ISSET_THIS:
			return ZEND_NATIVE_HELPER_VALUE_ISSET_THIS;
		case ZEND_MIR_OPCODE_VALUE_GET_CALLED_CLASS:
			return ZEND_NATIVE_HELPER_VALUE_GET_CALLED_CLASS;
		case ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE:
			return ZEND_NATIVE_HELPER_VALUE_BEGIN_SILENCE;
		case ZEND_MIR_OPCODE_VALUE_END_SILENCE:
			return ZEND_NATIVE_HELPER_VALUE_END_SILENCE;
		case ZEND_MIR_OPCODE_VALUE_MATCH_ERROR:
			return ZEND_NATIVE_HELPER_VALUE_MATCH_ERROR;
		case ZEND_MIR_OPCODE_VALUE_VERIFY_NEVER_TYPE:
			return ZEND_NATIVE_HELPER_VALUE_VERIFY_NEVER_TYPE;
		case ZEND_MIR_OPCODE_VALUE_DEFINED:
			return ZEND_NATIVE_HELPER_VALUE_DEFINED;
		case ZEND_MIR_OPCODE_VALUE_TICKS:
			return ZEND_NATIVE_HELPER_VALUE_TICKS;
		case ZEND_MIR_OPCODE_VALUE_TYPE_ASSERT:
			return ZEND_NATIVE_HELPER_VALUE_TYPE_ASSERT;
		case ZEND_MIR_OPCODE_VALUE_EXT_STMT:
			return ZEND_NATIVE_HELPER_VALUE_EXT_STMT;
		case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_BEGIN:
			return ZEND_NATIVE_HELPER_VALUE_EXT_FCALL_BEGIN;
		case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_END:
			return ZEND_NATIVE_HELPER_VALUE_EXT_FCALL_END;
		case ZEND_MIR_OPCODE_VALUE_EXT_NOP:
			return ZEND_NATIVE_HELPER_VALUE_EXT_NOP;
		case ZEND_MIR_OPCODE_VALUE_DISCARD_EXCEPTION:
			return ZEND_NATIVE_HELPER_VALUE_DISCARD_EXCEPTION;
		case ZEND_MIR_OPCODE_VALUE_CHECK_FUNC_ARG:
			return ZEND_NATIVE_HELPER_VALUE_CHECK_FUNC_ARG;
		case ZEND_MIR_OPCODE_VALUE_CHECK_UNDEF_ARGS:
			return ZEND_NATIVE_HELPER_VALUE_CHECK_UNDEF_ARGS;
		default:
			return ZEND_NATIVE_HELPER_COUNT;
	}
}

} // namespace zend::native::tpde::plan_detail
