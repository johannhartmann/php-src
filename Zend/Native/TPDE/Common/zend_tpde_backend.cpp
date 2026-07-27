// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_system_id.h"
#include "Zend/zend_type_info.h"
#include "Zend/Optimizer/zend_ssa.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t MAX_RECORDS = UINT32_C(1) << 20;
constexpr size_t MAX_NATIVE_IMAGE_BYTES = size_t{1} << 28;
constexpr uint32_t NATIVE_IMAGE_ABI_VERSION = 5;
constexpr uint32_t NATIVE_IMAGE_SERIAL_FORMAT = 3;
constexpr uint64_t NATIVE_IMAGE_SERIAL_MAGIC = UINT64_C(0x003331474d494e5a);
constexpr uint64_t NATIVE_IMAGE_BUILD_ID_SEED =
	UINT64_C(0x5750313300000000)
	^ (static_cast<uint64_t>(NATIVE_IMAGE_ABI_VERSION) << 32)
	^ static_cast<uint64_t>(ZEND_NATIVE_RUNTIME_ABI_VERSION);
std::atomic_uint32_t live_unwind_registrations{0};
std::atomic_uint64_t next_native_code_version{1};

struct zend_native_serial_image_header {
	uint64_t magic;
	uint32_t format;
	uint32_t target;
	uint32_t image_abi;
	uint32_t runtime_abi;
	uint64_t build_id;
	uint64_t code_version;
	uint32_t slot_count;
	uint32_t argument_count;
	uint32_t frame_variable_count;
	uint32_t frame_temporary_count;
	zend_native_image_metrics metrics;
	uint64_t text_size;
	uint32_t symbol_count;
	uint32_t binding_count;
	uint32_t component_count;
	uint32_t reserved;
	uint64_t total_size;
	uint64_t checksum;
};

struct zend_native_serial_binding {
	uint32_t symbol_index;
	uint32_t payload_size;
	uint64_t primary_reference;
	uint64_t scope_reference;
	uint32_t receiver_kind;
	uint32_t reserved;
};

struct zend_native_byte_buffer {
	unsigned char *bytes;
	size_t size;
	size_t capacity;
};

uint64_t native_image_build_id(zend_native_target target) {
	uint64_t hash = NATIVE_IMAGE_BUILD_ID_SEED
		^ static_cast<uint64_t>(static_cast<uint32_t>(target));

	for (size_t index = 0; index < sizeof(zend_system_id); ++index) {
		hash ^= static_cast<unsigned char>(zend_system_id[index]);
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

bool checked_count(uint32_t count);

bool native_buffer_append(
	zend_native_byte_buffer *buffer, const void *bytes, size_t size) {
	if (buffer == nullptr || (size != 0 && bytes == nullptr)
			|| size > MAX_NATIVE_IMAGE_BYTES
			|| buffer->size > MAX_NATIVE_IMAGE_BYTES - size) {
		return false;
	}
	const size_t required = buffer->size + size;
	if (required > buffer->capacity) {
		size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
		while (capacity < required) {
			capacity = capacity > MAX_NATIVE_IMAGE_BYTES / 2
				? MAX_NATIVE_IMAGE_BYTES : capacity * 2;
		}
		void *resized = std::realloc(buffer->bytes, capacity);
		if (resized == nullptr) {
			return false;
		}
		buffer->bytes = static_cast<unsigned char *>(resized);
		buffer->capacity = capacity;
	}
	if (size != 0) {
		std::memcpy(buffer->bytes + buffer->size, bytes, size);
	}
	buffer->size = required;
	return true;
}

uint64_t native_serial_checksum(const unsigned char *bytes, size_t size) {
	uint64_t hash = UINT64_C(1469598103934665603);
	const size_t checksum_offset =
		offsetof(zend_native_serial_image_header, checksum);
	for (size_t index = 0; index < size; ++index) {
		const unsigned char value =
			index >= checksum_offset
				&& index < checksum_offset + sizeof(uint64_t)
			? 0 : bytes[index];
		hash ^= value;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

const zend_native_image_symbol_binding *native_image_binding(
	const zend_native_image *image, uint32_t symbol_index) {
	for (uint32_t index = 0; index < image->symbol_binding_count; ++index) {
		if (image->symbol_bindings[index].symbol_index == symbol_index) {
			return &image->symbol_bindings[index];
		}
	}
	return nullptr;
}

bool native_descriptor_size(
	uint32_t kind, const void *address, size_t *size) {
	if (address == nullptr || size == nullptr) {
		return false;
	}
	uint32_t argument_count;
	size_t base_size;
	size_t argument_size;
	switch (kind) {
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR:
			argument_count =
				static_cast<const zend_native_direct_call_descriptor *>(
					address)->argument_count;
			base_size = offsetof(
				zend_native_direct_call_descriptor, arguments);
			argument_size = sizeof(zend_native_direct_call_argument);
			break;
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR:
			argument_count =
				static_cast<const zend_native_direct_internal_call_descriptor *>(
					address)->argument_count;
			base_size = offsetof(
				zend_native_direct_internal_call_descriptor, arguments);
			argument_size =
				sizeof(zend_native_direct_internal_call_argument);
			break;
		case ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR:
			argument_count =
				static_cast<const zend_native_user_call_descriptor *>(
					address)->argument_count;
			base_size = offsetof(zend_native_user_call_descriptor, arguments);
			argument_size =
				sizeof(zend_native_direct_internal_call_argument);
			break;
		default:
			return false;
	}
	if (!checked_count(argument_count)
			|| argument_count > (MAX_NATIVE_IMAGE_BYTES - base_size)
				/ argument_size) {
		return false;
	}
	*size = base_size + static_cast<size_t>(argument_count) * argument_size;
	return true;
}

bool checked_count(uint32_t count) {
	return count <= MAX_RECORDS;
}

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
		operation->auxiliary_storage_id =
			source_descriptor_storage(op_array, operation->auxiliary);
	}
	operation->extended_value = opline->extended_value;
	operation->source_position_id = source_position;
	operation->frame_state_id = ZEND_MIR_ID_INVALID;
	return true;
}

zend_native_runtime_helper_id executable_value_helper(zend_mir_opcode opcode);

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
	const zend_mir_opcode mapped = zend_mir_w12_executable_opcode(opcode);
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

zend_mir_scalar_type_mask exact_scalar_from_type_mask(uint32_t type) {
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

zend_tpde_machine_value_kind zend_tpde_machine_kind(
	zend_mir_representation representation,
	zend_mir_scalar_type_mask exact_type,
	zend_mir_value_category category)
{
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
		case ZEND_MIR_VALUE_REFERENCE_CELL:
			return ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
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

bool zend_tpde_apply_machine_value_facts(
	const zend_mir_value_view *value_model,
	const zend_ssa *source_ssa,
	zend_tpde_value *value)
{
	zend_mir_storage_ref storage{};
	zend_mir_value_category category = ZEND_MIR_VALUE_CATEGORY_UNKNOWN;

	if (value_model != nullptr && source_ssa != nullptr
			&& source_ssa->vars != nullptr
			&& value->id < static_cast<uint32_t>(source_ssa->vars_count)
			&& source_ssa->vars[value->id].var >= 0
			&& value_model->storage_count != nullptr
			&& value_model->storage_at != nullptr) {
		const uint32_t storage_index =
			static_cast<uint32_t>(source_ssa->vars[value->id].var);
		const uint32_t storage_count =
			value_model->storage_count(value_model->context);
		if (storage_index < storage_count
				&& value_model->storage_at(
					value_model->context, storage_index, &storage)
				&& storage.id == storage_index) {
			category = storage.category;
		}
		if (category != ZEND_MIR_VALUE_CATEGORY_UNKNOWN
				&& zend_mir_id_is_valid(storage.payload_id)
				&& value_model->payload_count != nullptr
				&& value_model->payload_at != nullptr) {
			zend_mir_payload_ref payload{};
			const uint32_t payload_count =
				value_model->payload_count(value_model->context);
			if (storage.payload_id < payload_count
					&& value_model->payload_at(value_model->context,
						storage.payload_id, &payload)
					&& payload.id == storage.payload_id) {
				value->refcount_state = payload.refcount_state;
			}
		}
	}
	value->machine_kind = zend_tpde_machine_kind(
		value->representation, value->exact_type, category);
	if (value->constant) {
		value->location = ZEND_TPDE_MACHINE_LOCATION_REGISTER;
		value->slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED;
	} else if (zend_mir_id_is_valid(value->canonical_storage_id)) {
		value->location = value->machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& value->argument_index < 0
			? ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT
			: ZEND_TPDE_MACHINE_LOCATION_REGISTER;
		value->slot_state = value->argument_index >= 0
			|| value->machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			? ZEND_TPDE_CANONICAL_SLOT_CLEAN
			: ZEND_TPDE_CANONICAL_SLOT_DIRTY;
	} else {
		value->location = ZEND_TPDE_MACHINE_LOCATION_REGISTER;
		value->slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED;
	}
	return true;
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

zend_mir_scalar_type_mask exact_scalar_from_call_result(
	const zend_op_array *op_array,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	uint32_t opline_index) {
	if (op_array == nullptr || calls == nullptr
			|| calls->call_site_count == nullptr
			|| calls->call_site_at == nullptr) {
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

zend_mir_scalar_type_mask exact_scalar_from_ssa_value(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	uint32_t ssa_variable_id,
	uint32_t depth) {
	if (op_array == nullptr || ssa == nullptr || ssa->var_info == nullptr
			|| ssa->vars == nullptr || ssa->ops == nullptr
			|| ssa_variable_id >= static_cast<uint32_t>(ssa->vars_count)
			|| depth > 64) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
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
}

zend_mir_scalar_type_mask exact_scalar_from_source_argument(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_view *calls,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	const zend_mir_call_argument_ref &argument) {
	uint32_t ssa_variable_id = argument.source_operand.ssa_variable_id;

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
	if (ssa_variable_id == ZEND_MIR_ID_INVALID
			&& argument.send_opline_index < op_array->last
			&& ssa->ops[argument.send_opline_index].op1_use >= 0) {
		ssa_variable_id = static_cast<uint32_t>(
			ssa->ops[argument.send_opline_index].op1_use);
	}
	if (ssa_variable_id == ZEND_MIR_ID_INVALID
			|| ssa_variable_id >= static_cast<uint32_t>(ssa->vars_count)) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	return exact_scalar_from_ssa_value(
		op_array, ssa, calls, bindings, binding_count, ssa_variable_id, 0);
}

uint32_t id_index_capacity(uint32_t count) {
	uint32_t capacity = 8;

	while (capacity < count * 2) {
		capacity <<= 1;
	}
	return capacity;
}

uint32_t id_index_hash(uint32_t id) {
	id ^= id >> 16;
	id *= UINT32_C(0x7feb352d);
	id ^= id >> 15;
	id *= UINT32_C(0x846ca68b);
	return id ^ (id >> 16);
}

bool id_index_insert(
	zend_tpde_id_index_entry *entries,
	uint32_t capacity,
	uint32_t id,
	uint32_t index) {
	if (!zend_mir_id_is_valid(id)) {
		return false;
	}
	uint32_t slot = id_index_hash(id) & (capacity - 1);

	for (uint32_t probe = 0; probe < capacity; ++probe) {
		if (entries[slot].id == ZEND_MIR_ID_INVALID) {
			entries[slot] = {id, index};
			return true;
		}
		if (entries[slot].id == id) {
			return false;
		}
		slot = (slot + 1) & (capacity - 1);
	}
	return false;
}

int32_t id_index_find(
	const zend_tpde_id_index_entry *entries,
	uint32_t capacity,
	uint32_t id) {
	if (entries == nullptr || !zend_mir_id_is_valid(id)) {
		return -1;
	}
	uint32_t slot = id_index_hash(id) & (capacity - 1);
	for (uint32_t probe = 0; probe < capacity; ++probe) {
		if (entries[slot].id == id) {
			return static_cast<int32_t>(entries[slot].index);
		}
		if (entries[slot].id == ZEND_MIR_ID_INVALID) {
			return -1;
		}
		slot = (slot + 1) & (capacity - 1);
	}
	return -1;
}

zend_tpde_id_index_entry *allocate_id_index(
	uint32_t count, uint32_t *capacity_out) {
	if (count == 0) {
		*capacity_out = 0;
		return nullptr;
	}
	const uint32_t capacity = id_index_capacity(count);
	auto *entries = static_cast<zend_tpde_id_index_entry *>(
		std::malloc(static_cast<size_t>(capacity)
			* sizeof(zend_tpde_id_index_entry)));
	if (entries == nullptr) {
		return nullptr;
	}
	for (uint32_t i = 0; i < capacity; ++i) {
		entries[i] = {ZEND_MIR_ID_INVALID, 0};
	}
	*capacity_out = capacity;
	return entries;
}

void require_runtime_helper(
	zend_tpde_plan *plan, zend_native_runtime_helper_id helper) {
	plan->required_runtime_helpers[helper / 64u] |=
		UINT64_C(1) << (helper % 64u);
}

bool image_add_symbol(
	zend_native_image *image,
	zend_native_image_symbol_kind kind,
	uint32_t id,
	uint32_t symbol_namespace,
	uint32_t abi_version,
	uint32_t effects,
	const void *address = nullptr) {
	if (image == nullptr || !zend_mir_id_is_valid(id)) {
		return false;
	}
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		const zend_native_image_symbol &symbol = image->symbols[index];
		if (symbol.kind == kind && symbol.id == id
				&& symbol.symbol_namespace == symbol_namespace) {
			if (symbol.abi_version != abi_version
					|| symbol.effects != effects) {
				return false;
			}
			if (address == nullptr) {
				return true;
			}
			for (uint32_t binding_index = 0;
					binding_index < image->symbol_binding_count;
					++binding_index) {
				const zend_native_image_symbol_binding &binding =
					image->symbol_bindings[binding_index];
				if (binding.symbol_index == index) {
					return binding.address == address;
				}
			}
			return false;
		}
	}
	if (image->symbol_count == image->symbol_capacity) {
		uint32_t capacity = image->symbol_capacity == 0
			? 16 : image->symbol_capacity * 2;
		if (capacity > MAX_RECORDS) {
			return false;
		}
		void *resized = std::realloc(image->symbols,
			static_cast<size_t>(capacity) * sizeof(*image->symbols));
		if (resized == nullptr) {
			return false;
		}
		image->symbols = static_cast<zend_native_image_symbol *>(resized);
		image->symbol_capacity = capacity;
	}
	zend_native_image_symbol &symbol = image->symbols[image->symbol_count];
	std::memset(&symbol, 0, sizeof(symbol));
	symbol.kind = kind;
	symbol.id = id;
	symbol.symbol_namespace = symbol_namespace;
	symbol.abi_version = abi_version;
	symbol.effects = effects;
	const int written = std::snprintf(symbol.name, sizeof(symbol.name),
		"__znmir_%u_%u_%u", static_cast<uint32_t>(kind),
		symbol_namespace, id);
	if (written <= 0 || static_cast<size_t>(written) >= sizeof(symbol.name)) {
		return false;
	}
	const uint32_t symbol_index = image->symbol_count++;
	if (address != nullptr) {
		if (image->symbol_binding_count == image->symbol_binding_capacity) {
			uint32_t capacity = image->symbol_binding_capacity == 0
				? 16 : image->symbol_binding_capacity * 2;
			if (capacity > MAX_RECORDS) {
				return false;
			}
			void *resized = std::realloc(image->symbol_bindings,
				static_cast<size_t>(capacity)
					* sizeof(*image->symbol_bindings));
			if (resized == nullptr) {
				return false;
			}
			image->symbol_bindings =
				static_cast<zend_native_image_symbol_binding *>(resized);
			image->symbol_binding_capacity = capacity;
		}
		image->symbol_bindings[image->symbol_binding_count++] = {
			symbol_index, address};
	}
	return true;
}

bool prepare_image_symbols(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	for (uint32_t id = 1; id < ZEND_NATIVE_HELPER_COUNT; ++id) {
		if ((plan->required_runtime_helpers[id / 64u]
				& (UINT64_C(1) << (id % 64u))) == 0) {
			continue;
		}
		const zend_native_runtime_helper *helper =
			zend_native_runtime_helper_find(plan->runtime,
				static_cast<zend_native_runtime_helper_id>(id));
		if (helper == nullptr
				|| !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER, id,
					0,
					plan->runtime->abi_version, helper->effects)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image runtime symbol table");
			return false;
		}
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		if (instruction.entry_cell != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					instruction.call_site.target_id,
					plan->symbol_namespace,
					NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.entry_cell)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image entry-cell symbol");
			return false;
		}
		if (instruction.internal_call_cell != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
					instruction.call_site.target_id,
					plan->symbol_namespace,
					NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.internal_call_cell)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image internal-call symbol");
			return false;
		}
		if (instruction.direct_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
					instruction.id, plan->symbol_namespace,
					NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.direct_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image direct-call symbol");
			return false;
		}
		if (instruction.direct_internal_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					instruction.id, plan->symbol_namespace,
					NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.direct_internal_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image direct internal-call symbol");
			return false;
		}
		if (instruction.user_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
					instruction.id, plan->symbol_namespace,
					NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.user_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image user-call symbol");
			return false;
		}
	}
	return true;
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
	if (plan == nullptr || !source_operand_value_id(operand, value_id)) {
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
		case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			return ZEND_NATIVE_HELPER_VALUE_MAKE_REF;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
			return ZEND_NATIVE_HELPER_VALUE_ASSIGN_REF;
		case ZEND_MIR_OPCODE_VALUE_SEPARATE:
			return ZEND_NATIVE_HELPER_VALUE_SEPARATE;
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
			return ZEND_NATIVE_HELPER_VALUE_COPY_TMP;
		case ZEND_MIR_OPCODE_VALUE_FREE:
			return ZEND_NATIVE_HELPER_VALUE_FREE;
		case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
			return ZEND_NATIVE_HELPER_VALUE_UNSET_CV;
		case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
			return ZEND_NATIVE_HELPER_VALUE_CHECK_VAR;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
			return ZEND_NATIVE_HELPER_VALUE_ASSIGN;
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			return ZEND_NATIVE_HELPER_VALUE_QM_ASSIGN;
		case ZEND_MIR_OPCODE_VALUE_CONCAT:
			return ZEND_NATIVE_HELPER_VALUE_CONCAT;
		case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
			return ZEND_NATIVE_HELPER_VALUE_FAST_CONCAT;
		case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
			return ZEND_NATIVE_HELPER_VALUE_ROPE_INIT;
		case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
			return ZEND_NATIVE_HELPER_VALUE_ROPE_ADD;
		case ZEND_MIR_OPCODE_VALUE_ROPE_END:
			return ZEND_NATIVE_HELPER_VALUE_ROPE_END;
		case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
			return ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY;
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
			return ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT;
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK:
			return ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_UNPACK;
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R;
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_W;
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_RW;
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_IS;
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_FUNC_ARG;
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_UNSET;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
			return ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP:
			return ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP;
		case ZEND_MIR_OPCODE_VALUE_UNSET_DIM:
			return ZEND_NATIVE_HELPER_VALUE_UNSET_DIM;
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
			return ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM;
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
			return ZEND_NATIVE_HELPER_VALUE_ASSIGN_OP;
		case ZEND_MIR_OPCODE_VALUE_FE_FREE:
			return ZEND_NATIVE_HELPER_VALUE_FE_FREE;
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
			return ZEND_NATIVE_HELPER_VALUE_BINARY_OP;
		case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
			return ZEND_NATIVE_HELPER_VALUE_UNARY_OP;
		case ZEND_MIR_OPCODE_VALUE_CAST:
			return ZEND_NATIVE_HELPER_VALUE_CAST;
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			return ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV;
		case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
			return ZEND_NATIVE_HELPER_VALUE_FETCH_LIST;
		case ZEND_MIR_OPCODE_VALUE_INCDEC:
			return ZEND_NATIVE_HELPER_VALUE_INCDEC;
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

bool call_site_requires_source_fragments(
	const zend_tpde_plan *plan, const zend_mir_call_site_ref &site)
{
	uint32_t source_position;

	if (plan == nullptr || plan->source_op_array == nullptr
			|| site.source_init_opline_index >= plan->source_op_array->last
			|| site.source_do_opline_index >= plan->source_op_array->last
			|| site.source_init_opline_index
				>= site.source_do_opline_index) {
		return false;
	}
	if (zend_get_user_opcode_handler(
			plan->source_op_array->opcodes[
				site.source_init_opline_index].opcode) != nullptr
			|| zend_get_user_opcode_handler(
				plan->source_op_array->opcodes[
					site.source_do_opline_index].opcode) != nullptr) {
		return true;
	}
	/*
	 * NEW must materialize the object into its source result slot before its
	 * constructor arguments are sent.  The source-fragment path already
	 * performs that ordering and still binds a fixed internal constructor
	 * through the process-local call cell.
	 */
	if (plan->source_op_array->opcodes[
			site.source_init_opline_index].opcode == ZEND_NEW) {
		return true;
	}
	/*
	 * CALLABLE_CONVERT does not invoke its resolved target.  It consumes the
	 * pending source call frame to create a Closure, so it cannot use either
	 * the direct internal-call or direct native-call completion path.  Emit
	 * INIT/SEND/CONVERT at their source positions and let the fragment runtime
	 * preserve the resolved function and PHP's call-frame ownership exactly.
	 */
	const uint8_t finish_opcode = plan->source_op_array->opcodes[
		site.source_do_opline_index].opcode;
	if (finish_opcode == ZEND_CALLABLE_CONVERT
			|| finish_opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
		return true;
	}
	for (uint32_t index = 0; index < site.arguments.count; ++index) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
				plan, site.arguments.offset + index, &argument)
				|| argument.send_opline_index
					>= plan->source_op_array->last) {
			return false;
		}
		if (zend_get_user_opcode_handler(
				plan->source_op_array->opcodes[
					argument.send_opline_index].opcode) != nullptr) {
			return true;
		}
	}
	/*
	 * CHECK_FUNC_ARG and CHECK_UNDEF_ARGS observe the pending call frame at
	 * their exact source positions.  Keep the ordinary single-enter direct
	 * path for all other calls, but materialize INIT/SEND/DO fragments when
	 * either metadata operation is present so FETCH_*_FUNC_ARG sees the
	 * resolved callee's by-reference contract before it fetches its lvalue.
	 */
	for (source_position = site.source_init_opline_index + 1;
			source_position < site.source_do_opline_index;
			source_position++) {
		const uint8_t opcode =
			plan->source_op_array->opcodes[source_position].opcode;
		if (opcode == ZEND_CHECK_FUNC_ARG
				|| opcode == ZEND_CHECK_UNDEF_ARGS) {
			return true;
		}
	}
	return false;
}

zend_native_user_call_descriptor *build_user_call_descriptor(
	zend_tpde_plan *plan,
	const zend_mir_call_site_ref &site,
	const zend_mir_instruction_record &record,
	zend_native_diagnostic *diag)
{
	if (plan == nullptr || plan->source_op_array == nullptr
			|| site.source_init_opline_index >= plan->source_op_array->last
			|| site.source_do_opline_index >= plan->source_op_array->last) {
		return nullptr;
	}
	const zend_op *init = &plan->source_op_array->opcodes[
		site.source_init_opline_index];
	const zend_op *finish = &plan->source_op_array->opcodes[
		site.source_do_opline_index];
	if (init->extended_value > site.arguments.count) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"call source argument count is inconsistent");
		return nullptr;
	}
	const size_t descriptor_size =
		offsetof(zend_native_user_call_descriptor, arguments)
		+ static_cast<size_t>(site.arguments.count)
			* sizeof(zend_native_direct_internal_call_argument);
	auto *descriptor =
		static_cast<zend_native_user_call_descriptor *>(
			std::calloc(1, descriptor_size));
	if (descriptor == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate a source call descriptor");
		return nullptr;
	}
	descriptor->argument_count = site.arguments.count;
	descriptor->initial_argument_count = init->extended_value;
	descriptor->init_source_position = site.source_init_opline_index;
	descriptor->do_source_position = site.source_do_opline_index;
	descriptor->init_opcode = init->opcode;
	descriptor->do_opcode = finish->opcode;
	descriptor->init_op1_payload = init->op1.num;
	descriptor->init_op2_payload = init->op2.num;
	descriptor->init_result_payload = init->result.num;
	descriptor->init_extended_value = init->extended_value;
	descriptor->do_op1_payload = finish->op1.num;
	descriptor->do_op2_payload = finish->op2.num;
	descriptor->do_result_payload = finish->result.num;
	descriptor->do_extended_value = finish->extended_value;
	if (!source_descriptor_operand(
				plan->source_op_array, init, init->op1_type,
				init->op1, &descriptor->init_op1)
			|| !source_descriptor_operand(
				plan->source_op_array, init, init->op2_type,
				init->op2, &descriptor->init_op2)
			|| !source_descriptor_operand(
				plan->source_op_array, init, init->result_type,
				init->result, &descriptor->init_result)
			|| !source_descriptor_operand(
				plan->source_op_array, finish, finish->op1_type,
				finish->op1, &descriptor->do_op1)
			|| !source_descriptor_operand(
				plan->source_op_array, finish, finish->op2_type,
				finish->op2, &descriptor->do_op2)) {
		std::free(descriptor);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"call source operands are invalid");
		return nullptr;
	}
	descriptor->do_result = site.result_operand;
	if (zend_mir_id_is_valid(record.result_id)) {
		const int32_t result_index =
			zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0
				|| !zend_mir_scalar_type_is_exact(
					plan->values[result_index].exact_type)) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call result is not an exact scalar");
			return nullptr;
		}
		descriptor->result_type =
			plan->values[result_index].exact_type;
		descriptor->flags |=
			ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT;
	}
	for (uint32_t index = 0; index < site.arguments.count; ++index) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
					plan, site.arguments.offset + index, &argument)
				|| argument.send_opline_index
					>= plan->source_op_array->last) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call argument table is unreadable");
			return nullptr;
		}
		const zend_op *send = &plan->source_op_array->opcodes[
			argument.send_opline_index];
		if (!source_descriptor_send_opcode(send->opcode)) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call SEND opcode is invalid");
			return nullptr;
		}
		zend_native_direct_internal_call_argument &encoded =
			descriptor->arguments[index];
		encoded.ordinal = argument.ordinal;
		encoded.mode = argument.source_mode
				== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
			? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
			: argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
				? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
				: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
		encoded.source_opcode = send->opcode;
		encoded.source_position = argument.send_opline_index;
		encoded.source_operand = argument.source_operand;
		encoded.auxiliary_payload = send->op2.num;
		encoded.result_payload = send->result.num;
		encoded.extended_value = send->extended_value;
		if (!source_descriptor_operand(
				plan->source_op_array, send, send->op2_type,
				send->op2, &encoded.auxiliary_operand)) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call auxiliary operand is invalid");
			return nullptr;
		}
	}
	return descriptor;
}

class FrozenValueBitSet {
	std::vector<uint64_t> words_;

public:
	explicit FrozenValueBitSet(uint32_t value_count = 0)
		: words_((static_cast<size_t>(value_count) + 63) / 64) {}

	bool test(uint32_t value) const {
		return (words_[value / 64] & (uint64_t{1} << (value % 64))) != 0;
	}
	void set(uint32_t value) {
		words_[value / 64] |= uint64_t{1} << (value % 64);
	}
	void reset(uint32_t value) {
		words_[value / 64] &= ~(uint64_t{1} << (value % 64));
	}
	void union_without(
			const FrozenValueBitSet &other,
			const FrozenValueBitSet &excluded) {
		for (size_t word = 0; word < words_.size(); ++word) {
			words_[word] |= other.words_[word] & ~excluded.words_[word];
		}
	}
	void assign_use_and_out_without_def(
			const FrozenValueBitSet &use,
			const FrozenValueBitSet &out,
			const FrozenValueBitSet &def) {
		for (size_t word = 0; word < words_.size(); ++word) {
			words_[word] =
				use.words_[word] | (out.words_[word] & ~def.words_[word]);
		}
	}
	const uint64_t *data() const {
		return words_.data();
	}
	size_t size() const {
		return words_.size();
	}
	bool operator==(const FrozenValueBitSet &) const = default;
};

bool freeze_generator_resume_liveness(
	zend_tpde_plan *plan, zend_native_diagnostic *diag)
{
	if (plan->source_op_array == nullptr
			|| (plan->source_op_array->fn_flags & ZEND_ACC_GENERATOR) == 0) {
		return true;
	}

	std::vector<uint32_t> targets;
	if ((plan->source_op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) != 0) {
		for (uint32_t i = 0;
				i < plan->source_op_array->last_try_catch; ++i) {
			const zend_try_catch_element &region =
				plan->source_op_array->try_catch_array[i];
			if (region.finally_op != 0
					&& region.finally_op < plan->source_op_array->last
					&& region.finally_end < plan->source_op_array->last) {
				targets.push_back(region.finally_op);
			}
		}
	}
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (instruction.has_value_operation
				&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
					|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
					|| record.opcode
						== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
			const uint32_t source_position =
				instruction.value_operation.source_position_id;
			if (source_position == UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"generator resume source position is invalid");
				return false;
			}
			targets.push_back(source_position + 1);
		}
	}
	std::sort(targets.begin(), targets.end());
	targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
	if (targets.empty()) {
		return true;
	}
	if (!checked_count(static_cast<uint32_t>(targets.size()))) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"generator resume target count exceeds the executable bound");
		return false;
	}

	plan->generator_resume_count = static_cast<uint32_t>(targets.size());
	plan->generator_resume_live_word_count =
		(plan->value_count + 63) / 64;
	const uint64_t live_word_count_u64 =
		static_cast<uint64_t>(plan->generator_resume_count)
			* plan->generator_resume_live_word_count;
	if (live_word_count_u64 > MAX_RECORDS) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"generator resume liveness exceeds the executable bound");
		return false;
	}
	plan->generator_resume_targets = static_cast<uint32_t *>(
		std::malloc(targets.size()
			* sizeof(*plan->generator_resume_targets)));
	plan->generator_resume_landings = static_cast<uint32_t *>(
		std::malloc(targets.size()
			* sizeof(*plan->generator_resume_landings)));
	plan->generator_resume_exception_blocks =
		static_cast<zend_mir_block_id *>(std::malloc(
			targets.size()
				* sizeof(*plan->generator_resume_exception_blocks)));
	const size_t live_word_count =
		static_cast<size_t>(live_word_count_u64);
	plan->generator_resume_live_values =
		live_word_count == 0 ? nullptr : static_cast<uint64_t *>(
			std::calloc(live_word_count,
				sizeof(*plan->generator_resume_live_values)));
	if (plan->generator_resume_targets == nullptr
			|| plan->generator_resume_landings == nullptr
			|| plan->generator_resume_exception_blocks == nullptr
			|| (live_word_count != 0
				&& plan->generator_resume_live_values == nullptr)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze generator resume liveness");
		return false;
	}

	std::vector<uint32_t> landing_instructions(
		targets.size(), UINT32_MAX);
	for (uint32_t resume = 0;
			resume < plan->generator_resume_count; ++resume) {
		const uint32_t target = targets[resume];
		uint32_t landing = UINT32_MAX;
		zend_mir_block_id exception_block = ZEND_MIR_ID_INVALID;
		plan->generator_resume_targets[resume] = target;
		for (uint32_t i = 0; i < plan->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan->instructions[i];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			const uint32_t source_position = record.source_position_id;
			if (source_position >= target
					&& (landing == UINT32_MAX
						|| source_position < landing)) {
				landing = source_position;
				landing_instructions[resume] = i;
			}
			if (instruction.has_value_operation
					&& source_position != UINT32_MAX
					&& source_position + 1 == target
					&& (record.opcode
							== ZEND_MIR_OPCODE_GENERATOR_CREATE
						|| record.opcode
							== ZEND_MIR_OPCODE_GENERATOR_YIELD
						|| record.opcode
							== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
				if (zend_mir_id_is_valid(exception_block)
						&& exception_block
							!= instruction.exception_block_id) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"generator resume has inconsistent exception edges");
					return false;
				}
				exception_block = instruction.exception_block_id;
			}
		}
		if (landing == UINT32_MAX) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator resume target has no MIR landing");
			return false;
		}
		plan->generator_resume_landings[resume] = landing;
		plan->generator_resume_exception_blocks[resume] = exception_block;
	}

	const uint32_t block_count = plan->block_count;
	const uint32_t value_count = plan->value_count;
	std::vector<std::vector<uint32_t>> block_instructions(block_count);
	std::vector<FrozenValueBitSet> block_use(
		block_count, FrozenValueBitSet{value_count});
	std::vector<FrozenValueBitSet> block_def(
		block_count, FrozenValueBitSet{value_count});
	std::vector<FrozenValueBitSet> block_phi_def(
		block_count, FrozenValueBitSet{value_count});
	std::vector<FrozenValueBitSet> live_in(
		block_count, FrozenValueBitSet{value_count});
	std::vector<FrozenValueBitSet> live_out(
		block_count, FrozenValueBitSet{value_count});

	auto add_uses = [&](uint32_t instruction_index,
			FrozenValueBitSet &live) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_PHI) {
			return;
		}
		for (uint32_t n = 0; n < instruction.operand_count; ++n) {
			const int32_t index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, n));
			if (index >= 0) {
				live.set(static_cast<uint32_t>(index));
			}
		}
		for (uint32_t n = 0;
				n < instruction.call_argument_count; ++n) {
			zend_mir_call_argument_ref argument;
			if (zend_tpde_call_argument_at(plan,
					instruction.call_argument_offset + n, &argument)
					&& zend_mir_id_is_valid(argument.value_id)) {
				const int32_t index =
					zend_tpde_value_index(plan, argument.value_id);
				if (index >= 0) {
					live.set(static_cast<uint32_t>(index));
				}
			}
		}
	};

	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		const int32_t block = zend_tpde_block_index(plan, record.block_id);
		if (block < 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator liveness references an unknown block");
			return false;
		}
		const uint32_t block_number = static_cast<uint32_t>(block);
		block_instructions[block_number].push_back(i);
		FrozenValueBitSet uses{value_count};
		add_uses(i, uses);
		for (uint32_t value = 0; value < value_count; ++value) {
			if (uses.test(value)
					&& !block_def[block_number].test(value)) {
				block_use[block_number].set(value);
			}
		}
		const int32_t result =
			zend_tpde_value_index(plan, record.result_id);
		if (result >= 0) {
			block_def[block_number].set(static_cast<uint32_t>(result));
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				block_phi_def[block_number].set(
					static_cast<uint32_t>(result));
			}
		}
	}

	std::vector<uint32_t> worklist;
	std::vector<uint8_t> queued(block_count, 1);
	worklist.reserve(block_count);
	for (uint32_t block = block_count; block-- > 0;) {
		worklist.push_back(block);
	}
	while (!worklist.empty()) {
		const uint32_t block = worklist.back();
		worklist.pop_back();
		queued[block] = 0;
		FrozenValueBitSet next_out{value_count};
		const zend_mir_block_id block_id = plan->block_ids[block];
		const uint32_t successor_count =
			plan->view->successor_count(plan->view->context, block_id);
		for (uint32_t n = 0; n < successor_count; ++n) {
			zend_mir_block_id successor_id;
			if (!plan->view->successor_at(
					plan->view->context, block_id, n, &successor_id)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"generator liveness successor table is unreadable");
				return false;
			}
			const int32_t successor =
				zend_tpde_block_index(plan, successor_id);
			if (successor < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"generator liveness successor is unknown");
				return false;
			}
			next_out.union_without(
				live_in[static_cast<uint32_t>(successor)],
				block_phi_def[static_cast<uint32_t>(successor)]);
			uint32_t predecessor_index = UINT32_MAX;
			const uint32_t predecessor_count =
				plan->view->predecessor_count(
					plan->view->context, successor_id);
			for (uint32_t predecessor = 0;
					predecessor < predecessor_count; ++predecessor) {
				zend_mir_block_id predecessor_id;
				if (plan->view->predecessor_at(
						plan->view->context, successor_id,
						predecessor, &predecessor_id)
						&& predecessor_id == block_id) {
					predecessor_index = predecessor;
					break;
				}
			}
			if (predecessor_index == UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"generator liveness predecessor table is inconsistent");
				return false;
			}
			for (uint32_t instruction_index :
					block_instructions[
						static_cast<uint32_t>(successor)]) {
				const zend_tpde_instruction &instruction =
					plan->instructions[instruction_index];
				const zend_mir_instruction_record record =
					zend_tpde_instruction_record_at(plan, &instruction);
				if (record.opcode != ZEND_MIR_OPCODE_PHI) {
					continue;
				}
				if (predecessor_index >= instruction.operand_count) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"generator liveness PHI arity is invalid");
					return false;
				}
				const int32_t input = zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &instruction, predecessor_index));
				if (input >= 0) {
					next_out.set(static_cast<uint32_t>(input));
				}
			}
		}
		FrozenValueBitSet next_in{value_count};
		next_in.assign_use_and_out_without_def(
			block_use[block], next_out, block_def[block]);
		if (!(next_out == live_out[block])
				|| !(next_in == live_in[block])) {
			live_out[block] = std::move(next_out);
			live_in[block] = std::move(next_in);
			const uint32_t predecessor_count =
				plan->view->predecessor_count(
					plan->view->context, block_id);
			for (uint32_t predecessor = 0;
					predecessor < predecessor_count; ++predecessor) {
				zend_mir_block_id predecessor_id;
				if (!plan->view->predecessor_at(
						plan->view->context, block_id,
						predecessor, &predecessor_id)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"generator liveness predecessor table is unreadable");
					return false;
				}
				const int32_t predecessor_block =
					zend_tpde_block_index(plan, predecessor_id);
				if (predecessor_block < 0) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"generator liveness predecessor is unknown");
					return false;
				}
				const uint32_t predecessor_number =
					static_cast<uint32_t>(predecessor_block);
				if (queued[predecessor_number] == 0) {
					queued[predecessor_number] = 1;
					worklist.push_back(predecessor_number);
				}
			}
		}
	}

	for (uint32_t resume = 0;
			resume < plan->generator_resume_count; ++resume) {
		const uint32_t landing_instruction =
			landing_instructions[resume];
		const zend_mir_instruction_record landing_record =
			zend_tpde_instruction_record_at(
				plan, &plan->instructions[landing_instruction]);
		const int32_t block =
			zend_tpde_block_index(plan, landing_record.block_id);
		if (block < 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator resume landing block is unknown");
			return false;
		}
		const uint32_t block_number = static_cast<uint32_t>(block);
		FrozenValueBitSet live = live_out[block_number];
		const std::vector<uint32_t> &instructions =
			block_instructions[block_number];
		for (size_t n = instructions.size(); n-- > 0;) {
			const uint32_t instruction_index = instructions[n];
			const zend_tpde_instruction &instruction =
				plan->instructions[instruction_index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			const int32_t definition =
				zend_tpde_value_index(plan, record.result_id);
			if (definition >= 0) {
				live.reset(static_cast<uint32_t>(definition));
			}
			add_uses(instruction_index, live);
			if (instruction_index == landing_instruction) {
				break;
			}
		}
		for (uint32_t value = 0; value < value_count; ++value) {
			const zend_tpde_value &facts = plan->values[value];
			if (live.test(value)
					&& (!zend_mir_scalar_type_is_exact(facts.exact_type)
						|| facts.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
						|| !zend_mir_id_is_valid(
							facts.canonical_storage_id)
						|| facts.constant)) {
				live.reset(value);
			}
		}
		if (live.size() != plan->generator_resume_live_word_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"generator resume live-set width is inconsistent");
			return false;
		}
		if (live.size() != 0) {
			std::memcpy(
				plan->generator_resume_live_values
					+ static_cast<size_t>(resume)
						* plan->generator_resume_live_word_count,
				live.data(), live.size() * sizeof(uint64_t));
		}
	}
	return true;
}

void destroy_plan(zend_tpde_plan *plan) {
	for (uint32_t index = 0; index < plan->direct_call_count; ++index) {
		std::free(plan->direct_calls[index]);
	}
	for (uint32_t index = 0;
			index < plan->direct_internal_call_count; ++index) {
		std::free(plan->direct_internal_calls[index]);
	}
	for (uint32_t index = 0; index < plan->user_call_count; ++index) {
		std::free(plan->user_calls[index]);
	}
	std::free(plan->block_ids);
	std::free(plan->block_index);
	std::free(plan->values);
	std::free(plan->argument_value_indices);
	std::free(plan->value_index);
	std::free(plan->instructions);
	std::free(plan->instruction_index);
	std::free(plan->call_site_instruction_index);
	std::free(plan->call_target_index);
	std::free(plan->user_binding_index);
	std::free(plan->internal_binding_index);
	std::free(plan->user_opcode_source_operations);
	std::free(plan->user_opcode_source_op1_targets);
	std::free(plan->user_opcode_source_op2_targets);
	std::free(plan->user_opcode_source_extended_targets);
	std::free(plan->generator_resume_targets);
	std::free(plan->generator_resume_landings);
	std::free(plan->generator_resume_exception_blocks);
	std::free(plan->generator_resume_live_values);
	std::free(plan->direct_calls);
	std::free(plan->direct_internal_calls);
	std::free(plan->user_calls);
	std::memset(plan, 0, sizeof(*plan));
}

bool initialize_plan(
	const zend_mir_view *view,
	const zend_native_runtime_api *runtime,
	const zend_native_call_binding *user_bindings,
	uint32_t user_binding_count,
	const zend_native_internal_call_binding *internal_bindings,
	uint32_t internal_binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag) {
	plan->runtime = runtime;
	plan->required_runtime_capabilities =
		ZEND_NATIVE_RUNTIME_CAP_BAILOUT_BOUNDARY;
	if (zend_native_runtime_validate(plan->runtime,
			plan->required_runtime_capabilities, diag) == FAILURE) {
		return false;
	}
	if (!zend_mir_contract_is_compatible(view->contract_version)
			|| view->function_count(view->context) != 1) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"W06 requires one verified compatible MIR function");
		return false;
	}
	if (!view->function_at(view->context, 0, &plan->function)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"MIR function table is unreadable");
		return false;
	}

	plan->view = view;
	plan->source_op_array = source_op_array;
	plan->source_ssa = source_ssa;
	plan->block_count = view->block_count(view->context);
	plan->value_count = view->value_count(view->context);
	plan->instruction_count = view->instruction_count(view->context);
	plan->calls = zend_mir_module_call_view_from_view(view);
	const zend_mir_value_view *value_model =
		zend_mir_module_value_view_from_view(view);
	plan->call_site_count = plan->calls != nullptr
		&& plan->calls->call_site_count != nullptr
		? plan->calls->call_site_count(plan->calls->context) : 0;
	plan->call_target_count = plan->calls != nullptr
		&& plan->calls->call_target_count != nullptr
		? plan->calls->call_target_count(plan->calls->context) : 0;
	plan->call_argument_count = plan->calls != nullptr
		&& plan->calls->call_argument_count != nullptr
		? plan->calls->call_argument_count(plan->calls->context) : 0;
	const uint32_t constant_count = view->constant_count(view->context);
	const uint32_t frame_slot_count = view->frame_slot_count(view->context);
	if (source_op_array != nullptr) {
		for (uint32_t index = 0; index < source_op_array->last; ++index) {
			if (zend_get_user_opcode_handler(
					source_op_array->opcodes[index].opcode) != nullptr) {
				plan->user_opcode_callbacks = true;
				plan->user_opcode_source_operation_count =
					source_op_array->last;
				plan->user_opcode_source_operations =
					static_cast<zend_mir_executable_value_ref *>(
						std::calloc(source_op_array->last,
							sizeof(*plan->user_opcode_source_operations)));
				plan->user_opcode_source_op1_targets =
					static_cast<uint32_t *>(std::malloc(
						static_cast<size_t>(source_op_array->last)
							* sizeof(uint32_t)));
				plan->user_opcode_source_op2_targets =
					static_cast<uint32_t *>(std::malloc(
						static_cast<size_t>(source_op_array->last)
							* sizeof(uint32_t)));
				plan->user_opcode_source_extended_targets =
					static_cast<uint32_t *>(std::malloc(
						static_cast<size_t>(source_op_array->last)
							* sizeof(uint32_t)));
				if (source_op_array->last != 0
						&& (plan->user_opcode_source_operations == nullptr
							|| plan->user_opcode_source_op1_targets == nullptr
							|| plan->user_opcode_source_op2_targets == nullptr
							|| plan->user_opcode_source_extended_targets
								== nullptr)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
						"unable to allocate user-opcode source operations");
					return false;
				}
				for (uint32_t source = 0;
						source < source_op_array->last; ++source) {
					if (!user_opcode_source_operation(
							source_op_array, source,
							&plan->user_opcode_source_operations[source])) {
						char message[192];
						std::snprintf(message, sizeof(message),
							"user-opcode source operands are invalid at source %u"
							" (opcode %u, types %u/%u/%u)",
							source,
							source_op_array->opcodes[source].opcode,
							source_op_array->opcodes[source].op1_type,
							source_op_array->opcodes[source].op2_type,
							source_op_array->opcodes[source].result_type);
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							message);
						return false;
					}
					const zend_op &opline =
						source_op_array->opcodes[source];
					plan->user_opcode_source_op1_targets[source] =
						user_opcode_source_absolute_target(
							source_op_array, source, opline.op1);
					plan->user_opcode_source_op2_targets[source] =
						user_opcode_source_absolute_target(
							source_op_array, source, opline.op2);
					plan->user_opcode_source_extended_targets[source] =
						user_opcode_source_extended_target(
							source_op_array, source,
							opline.extended_value);
				}
				require_runtime_helper(
					plan, ZEND_NATIVE_HELPER_USER_OPCODE_INVOKE);
				require_runtime_helper(plan,
					ZEND_NATIVE_HELPER_GENERATOR_USER_OPCODE_RETURN);
				for (uint32_t opcode = 0;
						opcode <= ZEND_VM_LAST_OPCODE; ++opcode) {
					zend_tpde_user_opcode_target target;
					if (!user_opcode_target(opcode, &target)) {
						continue;
					}
					plan->user_opcode_targets[
						plan->user_opcode_target_count++] = target;
					if (target.helper != ZEND_NATIVE_HELPER_COUNT) {
						require_runtime_helper(plan, target.helper);
					}
				}
				break;
			}
		}
		const uint32_t observer_temporary_count =
			ZEND_OBSERVER_ENABLED ? 1 : 0;
		if (source_op_array->last_var < 0
				|| !checked_count(
					static_cast<uint32_t>(source_op_array->last_var))
				|| !checked_count(source_op_array->T)
				|| source_op_array->T < observer_temporary_count
				|| static_cast<uint64_t>(source_op_array->last_var)
						+ source_op_array->T
					> MAX_RECORDS) {
			zend_tpde_set_diagnostic(
				diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source frame storage is outside the executable bound");
			return false;
		}
	}
	if (plan->block_count == 0 || !checked_count(plan->block_count)
			|| !checked_count(plan->value_count)
			|| !checked_count(plan->instruction_count)
			|| !checked_count(plan->call_site_count)
			|| !checked_count(plan->call_target_count)
			|| !checked_count(plan->call_argument_count)
			|| !checked_count(constant_count)
			|| !checked_count(frame_slot_count)) {
		char message[192];
		std::snprintf(message, sizeof(message),
			"MIR record count exceeds executable bound"
			" (blocks=%u values=%u instructions=%u sites=%u targets=%u"
			" arguments=%u constants=%u frame_slots=%u)",
			plan->block_count, plan->value_count, plan->instruction_count,
			plan->call_site_count, plan->call_target_count,
			plan->call_argument_count, constant_count, frame_slot_count);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			message);
		return false;
	}

	plan->block_ids = static_cast<zend_mir_block_id *>(
		std::calloc(plan->block_count, sizeof(*plan->block_ids)));
	plan->block_index = allocate_id_index(
		plan->block_count, &plan->block_index_capacity);
	plan->values = static_cast<zend_tpde_value *>(
		std::calloc(plan->value_count, sizeof(*plan->values)));
	plan->argument_count =
		frame_argument_count == UINT32_MAX ? 0 : frame_argument_count;
	plan->argument_value_indices = static_cast<int32_t *>(
		std::malloc(
			static_cast<size_t>(plan->argument_count)
				* sizeof(*plan->argument_value_indices)));
	for (uint32_t i = 0;
			i < plan->argument_count && plan->argument_value_indices != nullptr;
			++i) {
		plan->argument_value_indices[i] = -1;
	}
	plan->value_index = allocate_id_index(
		plan->value_count, &plan->value_index_capacity);
	plan->instructions = static_cast<zend_tpde_instruction *>(
		std::calloc(plan->instruction_count, sizeof(*plan->instructions)));
	plan->instruction_index = allocate_id_index(
		plan->instruction_count, &plan->instruction_index_capacity);
	plan->call_site_instruction_index = allocate_id_index(
		plan->call_site_count, &plan->call_site_instruction_index_capacity);
	plan->call_target_index = allocate_id_index(
		plan->call_target_count, &plan->call_target_index_capacity);
	plan->user_binding_index = allocate_id_index(
		user_binding_count, &plan->user_binding_index_capacity);
	plan->internal_binding_index = allocate_id_index(
		internal_binding_count, &plan->internal_binding_index_capacity);
	plan->direct_calls = static_cast<zend_native_direct_call_descriptor **>(
		std::calloc(plan->call_site_count, sizeof(*plan->direct_calls)));
	plan->direct_internal_calls =
		static_cast<zend_native_direct_internal_call_descriptor **>(
			std::calloc(plan->call_site_count,
				sizeof(*plan->direct_internal_calls)));
	plan->user_calls = static_cast<zend_native_user_call_descriptor **>(
		std::calloc(plan->call_site_count, sizeof(*plan->user_calls)));
	if (plan->block_ids == nullptr || plan->block_index == nullptr
			|| (plan->value_count != 0
				&& (plan->values == nullptr || plan->value_index == nullptr))
			|| (plan->argument_count != 0
				&& plan->argument_value_indices == nullptr)
			|| (plan->instruction_count != 0
				&& (plan->instructions == nullptr
					|| plan->instruction_index == nullptr))
			|| (plan->call_site_count != 0
				&& (plan->call_site_instruction_index == nullptr
					|| plan->direct_calls == nullptr
					|| plan->direct_internal_calls == nullptr
					|| plan->user_calls == nullptr))
			|| (plan->call_target_count != 0
				&& plan->call_target_index == nullptr)
			|| (user_binding_count != 0 && plan->user_binding_index == nullptr)
			|| (internal_binding_count != 0
				&& plan->internal_binding_index == nullptr)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate the TPDE adaptor plan");
		return false;
	}
	uint64_t operands = 0;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_mir_instruction_record record;
		if (!view->instruction_at(view->context, i, &record)
				|| !id_index_insert(plan->instruction_index,
					plan->instruction_index_capacity, record.id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction table is unreadable or contains duplicate IDs");
			return false;
		}
		const uint32_t count =
			view->instruction_operand_count(view->context, record.id);
		operands += count;
		if (operands > MAX_RECORDS) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR operand count is outside the W06 executable bound");
			return false;
		}
		plan->instructions[i].id = record.id;
		plan->instructions[i].view_index = i;
		plan->instructions[i].operand_count = count;
		plan->instructions[i].component_target_index = UINT32_MAX;
		plan->instructions[i].exception_block_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].zval_store_storage_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].runtime_helper = ZEND_NATIVE_HELPER_COUNT;
		plan->instructions[i].source_opline_index = UINT32_MAX;
	}
	for (uint32_t i = 0; i < plan->call_site_count; ++i) {
		zend_mir_call_site_ref site;
		if (plan->calls == nullptr || plan->calls->call_site_at == nullptr
				|| !plan->calls->call_site_at(plan->calls->context, i, &site)
				|| zend_tpde_instruction_index(plan, site.instruction_id) < 0
				|| !id_index_insert(plan->call_site_instruction_index,
					plan->call_site_instruction_index_capacity,
					site.instruction_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-site table is unreadable, duplicated, or references an unknown instruction");
			return false;
		}
	}
	for (uint32_t i = 0; i < plan->call_target_count; ++i) {
		zend_mir_call_target_ref target;
		if (plan->calls == nullptr || plan->calls->call_target_at == nullptr
				|| !plan->calls->call_target_at(
					plan->calls->context, i, &target)
				|| !id_index_insert(plan->call_target_index,
					plan->call_target_index_capacity, target.id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-target table is unreadable or contains duplicate IDs");
			return false;
		}
	}
	for (uint32_t i = 0; i < plan->call_argument_count; ++i) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(plan, i, &argument)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-argument table is unreadable");
			return false;
		}
	}
	for (uint32_t i = 0; i < user_binding_count; ++i) {
		if (user_bindings[i].entry_cell == nullptr
				|| !id_index_insert(plan->user_binding_index,
					plan->user_binding_index_capacity,
					user_bindings[i].target_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"direct user-call binding table is invalid or duplicated");
			return false;
		}
	}
	for (uint32_t i = 0; i < internal_binding_count; ++i) {
		if (internal_bindings[i].call_cell == nullptr
				|| !id_index_insert(plan->internal_binding_index,
					plan->internal_binding_index_capacity,
					internal_bindings[i].target_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"direct internal-call binding table is invalid or duplicated");
			return false;
		}
	}
	if (value_model != nullptr) {
		if (value_model->contract_version != ZEND_MIR_W11P_CONTRACT_VERSION
				|| (value_model->model_flags
					& ~ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) != 0
				|| value_model->value_location_count == nullptr
				|| value_model->value_location_at == nullptr
				|| value_model->executable_operation_count == nullptr
				|| value_model->executable_operation_at == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"executable value model lacks explicit W11P operands or locations");
			return false;
		}
		plan->value_model_flags = value_model->model_flags;
		const uint32_t operation_count =
			value_model->executable_operation_count(value_model->context);
		if (!checked_count(operation_count)
				|| operation_count > plan->instruction_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"executable value operation count is outside the MIR bound");
			return false;
		}
		for (uint32_t i = 0; i < operation_count; ++i) {
			zend_mir_executable_value_ref operation{};
			int32_t instruction_index;
			if (!value_model->executable_operation_at(
					value_model->context, i, &operation)
					|| (instruction_index =
							zend_tpde_instruction_index(plan, operation.id)) < 0
					|| plan->instructions[instruction_index]
						.has_value_operation) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"executable value operation has an invalid instruction identity");
				return false;
			}
			plan->instructions[instruction_index].value_operation = operation;
			plan->instructions[instruction_index].has_value_operation = true;
		}
	}
	/*
	 * W08 predates the executable-value table, but its source-zval RETURN has
	 * the same runtime ABI as W11. Translate that legacy MIR once, while the
	 * immutable source and SSA views are available, so generated code never
	 * decodes a Zend opcode to recover operand semantics.
	 */
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_tpde_instruction &instruction = plan->instructions[i];
		zend_mir_instruction_record record;
		if (instruction.has_value_operation
				|| !view->instruction_at(
					view->context, instruction.view_index, &record)
				|| record.opcode != ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			continue;
		}
		if (source_op_array == nullptr
				|| record.source_position_id >= source_op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"legacy source-zval return has no source descriptor");
			return false;
		}
		const zend_op *opline =
			&source_op_array->opcodes[record.source_position_id];
		if ((opline->opcode != ZEND_RETURN
					&& opline->opcode != ZEND_RETURN_BY_REF)
				|| !source_descriptor_operand(
					source_op_array, opline, opline->op1_type,
					opline->op1, &instruction.value_operation.op1)
				|| !source_descriptor_operand(
					source_op_array, opline, IS_UNUSED, opline->op2,
					&instruction.value_operation.op2)
				|| !source_descriptor_operand(
					source_op_array, opline, IS_UNUSED, opline->result,
					&instruction.value_operation.result)
				|| !source_descriptor_operand(
					source_op_array, opline, IS_UNUSED, opline->result,
					&instruction.value_operation.auxiliary)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"legacy source-zval return operand is invalid");
			return false;
		}
		instruction.value_operation.id = record.id;
		instruction.value_operation.block_id = record.block_id;
		instruction.value_operation.opcode = record.opcode;
		instruction.value_operation.source_opcode = opline->opcode;
		instruction.value_operation.op1_storage_id =
			source_descriptor_storage(
				source_op_array, instruction.value_operation.op1);
		instruction.value_operation.op2_storage_id = ZEND_MIR_ID_INVALID;
		instruction.value_operation.result_storage_id = ZEND_MIR_ID_INVALID;
		instruction.value_operation.auxiliary_storage_id =
			ZEND_MIR_ID_INVALID;
		instruction.value_operation.extended_value = opline->extended_value;
		instruction.value_operation.source_position_id =
			record.source_position_id;
		instruction.value_operation.frame_state_id = record.frame_state_id;
		instruction.value_operation.effects = record.effects;
		instruction.value_operation.reads = record.reads;
		instruction.value_operation.writes = record.writes;
		instruction.value_operation.barriers = record.barriers;
		instruction.value_operation.ownership_actions =
			record.ownership_actions;
		if (source_ssa != nullptr && source_ssa->ops != nullptr
				&& source_ssa->ops[record.source_position_id].op1_use >= 0) {
			instruction.value_operation.op1.kind =
				ZEND_MIR_SOURCE_OPERAND_SSA;
			instruction.value_operation.op1.ssa_variable_id =
				static_cast<uint32_t>(
					source_ssa->ops[record.source_position_id].op1_use);
		}
		instruction.has_value_operation = true;
	}

	for (uint32_t i = 0; i < plan->block_count; ++i) {
		zend_mir_block_record block;
		if (!view->block_at(view->context, i, &block)
				|| block.function_id != plan->function.id
				|| !id_index_insert(plan->block_index,
					plan->block_index_capacity, block.id, i)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR block table is inconsistent");
			return false;
		}
		plan->block_ids[i] = block.id;
	}
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		zend_mir_value_record value;
		if (!view->value_at(view->context, i, &value)
				|| !id_index_insert(plan->value_index,
					plan->value_index_capacity, value.id, i)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR value table is unreadable or contains duplicate IDs");
			return false;
		}
		plan->values[i] = {
			.id = value.id,
			.representation = value.representation,
			.exact_type = ZEND_MIR_SCALAR_TYPE_NONE,
			.canonical_storage_id = ZEND_MIR_ID_INVALID,
			.ownership = value.ownership,
			.refcount_state = ZEND_MIR_REFCOUNT_UNKNOWN,
			.argument_index = -1,
			.machine_kind = ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			.location = ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT,
			.slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED,
			.constant = false,
			.constant_bits = 0,
		};
	}
	if (value_model != nullptr
			&& (value_model->model_flags
				& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) != 0) {
		const uint32_t location_count =
			value_model->value_location_count(value_model->context);
		if (!checked_count(location_count)
				|| location_count > plan->value_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"value-location table is outside the MIR value bound");
			return false;
		}
		for (uint32_t i = 0; i < location_count; ++i) {
			zend_mir_value_location_ref location{};
			int32_t value_index;
			if (!value_model->value_location_at(
					value_model->context, i, &location)
					|| !zend_mir_id_is_valid(location.storage_id)
					|| (value_index = zend_tpde_value_index(
							plan, location.value_id)) < 0
					|| zend_mir_id_is_valid(
						plan->values[value_index].canonical_storage_id)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"value-location table is unreadable, duplicated, or invalid");
				return false;
			}
			plan->values[value_index].canonical_storage_id =
				location.storage_id;
			if (location.frame_argument_ordinal_plus_one != 0) {
				const uint32_t argument_index =
					location.frame_argument_ordinal_plus_one - 1;
				if (frame_argument_count == UINT32_MAX
						|| argument_index >= plan->argument_count
						|| argument_index > static_cast<uint32_t>(INT32_MAX)
						|| plan->argument_value_indices[argument_index] >= 0) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"value-location table has an invalid or duplicate frame argument");
					return false;
				}
				plan->argument_value_indices[argument_index] = value_index;
				plan->values[value_index].argument_index =
					static_cast<int32_t>(argument_index);
				plan->values[value_index].constant = false;
			}
		}
	}
	for (uint32_t i = 0; i < constant_count; ++i) {
		zend_mir_constant_record constant;
		if (!view->constant_at(view->context, i, &constant)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR constant table is unreadable");
			return false;
		}
		int32_t index = zend_tpde_value_index(plan, constant.value_id);
		if (index < 0) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR constant references an unknown value");
			return false;
		}
		plan->values[index].constant = true;
		switch (constant.kind) {
			case ZEND_MIR_CONSTANT_KIND_NULL_VALUE:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_NULL;
				plan->values[index].constant_bits = 0;
				break;
			case ZEND_MIR_CONSTANT_KIND_FALSE_VALUE:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_I1;
				plan->values[index].constant_bits = 0;
				break;
			case ZEND_MIR_CONSTANT_KIND_TRUE_VALUE:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_I1;
				plan->values[index].constant_bits = 1;
				break;
			case ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_I64;
				plan->values[index].constant_bits = constant.payload_bits;
				break;
			case ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS:
				plan->values[index].exact_type = ZEND_MIR_SCALAR_TYPE_F64;
				plan->values[index].constant_bits = constant.payload_bits;
				break;
			default:
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
					"W06 does not execute pointer or string constants");
				return false;
		}
	}
	const uint32_t value_fact_count = view->value_fact_count(view->context);
	if (!checked_count(value_fact_count)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"MIR value-fact count is outside the executable bound");
		return false;
	}
	for (uint32_t i = 0; i < value_fact_count; ++i) {
		zend_mir_value_fact_ref fact;
		if (!view->value_fact_at(view->context, i, &fact)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR value-fact table is unreadable");
			return false;
		}
		int32_t index = zend_tpde_value_index(plan, fact.value_id);
		if (index >= 0 && zend_mir_scalar_type_is_exact(fact.exact_type)) {
			plan->values[index].exact_type = fact.exact_type;
		}
	}
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		if (!zend_tpde_apply_machine_value_facts(
				value_model, source_ssa, &plan->values[i])) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine value facts do not match source SSA storage");
			return false;
		}
	}

	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_mir_instruction_record record;
		if (!view->instruction_at(view->context, i, &record)
				|| record.id != plan->instructions[i].id) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction table is unreadable");
			return false;
		}
		const uint32_t count = plan->instructions[i].operand_count;
		if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
			if (count != 2) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"zval store requires a scalar source and destination identity");
				return false;
			}
			const zend_mir_value_id source_id = zend_tpde_operand_at(
				plan, &plan->instructions[i], 0);
			const zend_mir_value_id destination_id = zend_tpde_operand_at(
				plan, &plan->instructions[i], 1);
			const int32_t source_index =
				zend_tpde_value_index(plan, source_id);
			const int32_t destination_index =
				zend_tpde_value_index(plan, destination_id);
			if (source_index < 0 || destination_index < 0
					|| !zend_mir_scalar_type_is_exact(
						plan->values[source_index].exact_type)
					|| !zend_mir_id_is_valid(
						plan->values[destination_index]
							.canonical_storage_id)) {
				char message[192];
				std::snprintf(message, sizeof(message),
					"zval store %u operands source=%u index=%d type=%u "
					"destination=%u index=%d storage=%u lack metadata",
					record.id, source_id, source_index,
					source_index >= 0
						? static_cast<unsigned>(
							plan->values[source_index].exact_type)
						: 0,
					destination_id, destination_index,
					destination_index >= 0
						? plan->values[destination_index]
							.canonical_storage_id
						: ZEND_MIR_ID_INVALID);
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					message);
				return false;
			}
			plan->instructions[i].zval_store_storage_id =
				plan->values[destination_index].canonical_storage_id;
			plan->instructions[i].runtime_helper =
				ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW;
			require_runtime_helper(
				plan, plan->instructions[i].runtime_helper);
			continue;
		}
		if (zend_mir_opcode_is_executable_value(record.opcode)) {
			const bool semantic_echo =
				record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR;
			const bool multi_branch =
				record.opcode == ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH;
			const zend_native_runtime_helper_id helper = semantic_echo
				? ZEND_NATIVE_HELPER_COUNT
				: multi_branch
					? ZEND_NATIVE_HELPER_COUNT
				: executable_value_helper(record.opcode);
			plan->instructions[i].runtime_helper = helper;
			if ((semantic_echo ? count != 1 : count != 0)
					|| !zend_mir_id_is_valid(record.source_position_id)
					|| (!semantic_echo && !multi_branch
						&& helper == ZEND_NATIVE_HELPER_COUNT)
					|| (!semantic_echo && !multi_branch
						&& !zend_tpde_helper_has_explicit_operands(helper))
					|| !plan->instructions[i].has_value_operation
					|| plan->instructions[i].value_operation.id != record.id
					|| plan->instructions[i].value_operation.opcode
						!= record.opcode
					|| plan->instructions[i].value_operation.source_position_id
						!= record.source_position_id) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"executable value operation lacks exact source semantics");
				return false;
			}
			if (multi_branch) {
				zend_tpde_multi_branch layout;
				if (!zend_tpde_multi_branch_at(
						plan, plan->instructions[i], record, &layout)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"multiway branch lacks exact source-backed cases");
					return false;
				}
				continue;
			}
			if (semantic_echo) {
				zend_mir_value_id value_id;
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				if (operation.source_opcode != ZEND_ECHO
						|| !source_operand_value_id(operation.op1, value_id)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"semantic echo lacks an explicit scalar operand");
					return false;
				}
				const int32_t value_index =
					zend_tpde_value_index(plan, value_id);
				if (value_index < 0
						|| !zend_mir_scalar_type_is_exact(
							plan->values[value_index].exact_type)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
						"semantic echo operand has no exact scalar type");
					return false;
				}
				plan->instructions[i].source_effect_exact_type =
					plan->values[value_index].exact_type;
				plan->instructions[i].runtime_helper =
					plan->values[value_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_F64
						? ZEND_NATIVE_HELPER_ECHO_DOUBLE
						: ZEND_NATIVE_HELPER_ECHO_INTEGER;
				require_runtime_helper(plan,
					plan->instructions[i].runtime_helper);
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				zend_tpde_long_binary binary{};
				zend_mir_value_id result_id;
				const bool long_operands =
					source_operand_exact_type(plan, operation.op1)
						== ZEND_MIR_SCALAR_TYPE_I64
					&& source_operand_exact_type(plan, operation.op2)
						== ZEND_MIR_SCALAR_TYPE_I64;
				if (long_operands
						&& zend_tpde_long_binary_at(
							plan->instructions[i], &binary)
						&& source_operand_value_id(
							operation.result, result_id)) {
					const int32_t result_index =
						zend_tpde_value_index(plan, result_id);
					if (result_index >= 0) {
						plan->values[result_index].exact_type =
							operation.source_opcode == ZEND_ADD
								|| operation.source_opcode == ZEND_SUB
								|| operation.source_opcode == ZEND_BW_OR
								|| operation.source_opcode == ZEND_BW_AND
								|| operation.source_opcode == ZEND_BW_XOR
							? ZEND_MIR_SCALAR_TYPE_I64
							: ZEND_MIR_SCALAR_TYPE_I1;
					}
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				zend_mir_value_id verified_value_id = ZEND_MIR_ID_INVALID;
				if (operation.source_opcode != ZEND_VERIFY_RETURN_TYPE
						|| operation.op2.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| (operation.op1.kind
								== ZEND_MIR_SOURCE_OPERAND_LITERAL
							&& operation.result.kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"return type verification lacks explicit source operands");
					return false;
				}
				/*
				 * A statically exact scalar that already satisfies the declared
				 * return type cannot take Zend's coercion or TypeError path.
				 * Keep VERIFY_RETURN_TYPE as the semantic slow path for boxed or
				 * otherwise polymorphic values, but do not cross the C ABI merely
				 * to rediscover a proof already present in ZNMIR.
				 */
				if (source_op_array != nullptr
						&& source_op_array->arg_info != nullptr
						&& (source_op_array->fn_flags
								& ZEND_ACC_HAS_RETURN_TYPE) != 0
						&& source_operand_value_id(
							operation.op1, verified_value_id)) {
					const int32_t verified_value_index =
						zend_tpde_value_index(plan, verified_value_id);
					if (verified_value_index >= 0
							&& zend_mir_scalar_type_is_exact(
								plan->values[
									verified_value_index].exact_type)
							&& exact_scalar_satisfies_type(
								plan->values[
									verified_value_index].exact_type,
								source_op_array->arg_info[-1].type)) {
						plan->instructions[i].runtime_helper =
							ZEND_NATIVE_HELPER_COUNT;
						continue;
					}
				}
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				require_runtime_helper(
					plan, ZEND_NATIVE_HELPER_VERIFY_RETURN_TYPE);
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS
					|| record.opcode == ZEND_MIR_OPCODE_FUNC_GET_ARGS) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				const bool valid_op1 =
					record.opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS
						? operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						: (operation.op1.kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED
							|| operation.op1.kind
								== ZEND_MIR_SOURCE_OPERAND_LITERAL);
				if (!valid_op1
						|| operation.op2.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| operation.auxiliary.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| operation.source_opcode
							!= (record.opcode
									== ZEND_MIR_OPCODE_FUNC_NUM_ARGS
								? ZEND_FUNC_NUM_ARGS
								: ZEND_FUNC_GET_ARGS)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"argument introspection lacks explicit source operands");
					return false;
				}
				if (record.opcode == ZEND_MIR_OPCODE_FUNC_NUM_ARGS) {
					zend_mir_value_id result_id;
					const int32_t result_index =
						source_operand_value_id(operation.result, result_id)
							? zend_tpde_value_index(plan, result_id)
							: -1;
					if (result_index < 0 || result_id != record.result_id) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"func_num_args result identity is inconsistent");
						return false;
					}
					/*
					 * FUNC_NUM_ARGS is intrinsically an integer operation.  The
					 * source SSA overlay may intentionally omit a fact for this
					 * non-argument temporary, so derive the executable type from
					 * the opcode contract before validating and compiling it.
					 */
					plan->values[result_index].exact_type =
						ZEND_MIR_SCALAR_TYPE_I64;
					if (record.representation
							!= ZEND_MIR_REPRESENTATION_I64) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"func_num_args result representation is not i64");
						return false;
					}
					if (plan->values[result_index].exact_type
							!= ZEND_MIR_SCALAR_TYPE_I64) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"func_num_args result type is not exact i64");
						return false;
					}
					/*
					 * The count belongs to the active invocation even if source
					 * analysis saw only fixed-arity callers while compiling this
					 * function. Generated code must read the current frame.
					 */
					plan->values[result_index].constant = false;
				}
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				if (record.opcode == ZEND_MIR_OPCODE_FUNC_GET_ARGS) {
					require_runtime_helper(plan, helper);
				}
				continue;
			}
			if (record.opcode >= ZEND_MIR_OPCODE_GENERATOR_CREATE
					&& record.opcode <= ZEND_MIR_OPCODE_GENERATOR_RETURN) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				const uint32_t expected_source_opcode =
					record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
						? ZEND_GENERATOR_CREATE
						: record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
							? ZEND_YIELD
							: record.opcode
									== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM
								? ZEND_YIELD_FROM
								: ZEND_GENERATOR_RETURN;
				if (operation.source_opcode != expected_source_opcode
						|| source_op_array == nullptr
						|| (source_op_array->fn_flags
							& ZEND_ACC_GENERATOR) == 0) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"generator operation lacks a generator source frame");
					return false;
				}
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_SUSPEND;
				require_runtime_helper(plan, helper);
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				if (operation.source_opcode < ZEND_FRAMELESS_ICALL_0
						|| operation.source_opcode > ZEND_FRAMELESS_ICALL_3
						|| operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"frameless internal call lacks explicit operands");
					return false;
				}
				const uint32_t argument_count =
					operation.source_opcode - ZEND_FRAMELESS_ICALL_0;
				const zend_mir_source_operand_ref arguments[] = {
					operation.op1, operation.op2, operation.auxiliary};
				for (uint32_t argument = 0; argument < 3; ++argument) {
					if ((argument < argument_count)
							== (arguments[argument].kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"frameless internal call arity disagrees with operands");
						return false;
					}
				}
			}
			plan->required_runtime_capabilities |=
				ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
			if ((record.opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
						&& record.opcode
							<= ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED)
					|| (record.opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
						&& record.opcode
							<= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL)) {
				plan->required_runtime_capabilities |= record.opcode
						>= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
					? ZEND_NATIVE_RUNTIME_CAP_DYNAMIC_BINDING
					: ZEND_NATIVE_RUNTIME_CAP_OBJECT_OPERATION;
			}
			require_runtime_helper(plan, helper);
		}
		if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			zend_tpde_instruction &instruction = plan->instructions[i];
			const zend_mir_executable_value_ref &operation =
				instruction.value_operation;
			zend_mir_value_id value_id;
			if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
					|| !instruction.has_value_operation
					|| operation.id != record.id
					|| operation.opcode != record.opcode
					|| (operation.source_opcode != ZEND_RETURN
						&& operation.source_opcode != ZEND_RETURN_BY_REF)
					|| operation.source_position_id
						!= record.source_position_id
					|| (operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_LITERAL
						? operation.op1.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_KIND_INVALID
							|| operation.op1_storage_id
								!= ZEND_MIR_ID_INVALID
						: (operation.op1.kind
									!= ZEND_MIR_SOURCE_OPERAND_SLOT
								&& operation.op1.kind
									!= ZEND_MIR_SOURCE_OPERAND_SSA)
							|| operation.op1.slot_kind
								< ZEND_MIR_SOURCE_SLOT_CV
							|| operation.op1.slot_kind
								> ZEND_MIR_SOURCE_SLOT_VAR
							|| operation.op1_storage_id
								== ZEND_MIR_ID_INVALID)
					|| operation.op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| operation.result.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-zval return lacks explicit source semantics");
				return false;
			}
			const int32_t value_index =
				source_operand_value_id(operation.op1, value_id)
					? zend_tpde_value_index(plan, value_id) : -1;
			if (operation.source_opcode == ZEND_RETURN
					&& operation.op1.kind
						!= ZEND_MIR_SOURCE_OPERAND_LITERAL
					&& value_index >= 0
					/*
					 * A scalar fact attached to a frame argument describes the
					 * compiled source, not every invocation.  Until the return
					 * itself carries a retained entry-guard proof, copying the
					 * raw payload would skip zval ownership for a polymorphic
					 * array/object argument.  Keep argument returns on the
					 * ownership-correct helper path; locally produced exact
					 * scalars remain eligible for the direct return.
					 */
					&& plan->values[value_index].argument_index < 0
					&& (source_ssa == nullptr
						|| source_ssa->var_info == nullptr
						|| operation.op1.ssa_variable_id
							== ZEND_MIR_ID_INVALID
						|| operation.op1.ssa_variable_id
							>= static_cast<uint32_t>(source_ssa->vars_count)
						|| (source_ssa->var_info[
								operation.op1.ssa_variable_id].type
								& MAY_BE_UNDEF) == 0)
					&& zend_mir_scalar_type_is_exact(
						plan->values[value_index].exact_type)) {
				bool helper_mutates_return_storage = false;
				for (uint32_t previous = 0; previous < i; ++previous) {
					const zend_tpde_instruction &candidate =
						plan->instructions[previous];
					zend_mir_storage_id call_result_storage =
						ZEND_MIR_ID_INVALID;
					if (candidate.direct_call != nullptr) {
						call_result_storage = source_descriptor_storage(
							source_op_array,
							candidate.direct_call->result_operand);
					} else if (candidate.direct_internal_call != nullptr) {
						call_result_storage = source_descriptor_storage(
							source_op_array,
							candidate.direct_internal_call->result_operand);
					} else if (candidate.user_call != nullptr) {
						call_result_storage = source_descriptor_storage(
							source_op_array,
							candidate.user_call->do_result);
					}
					if (call_result_storage == operation.op1_storage_id) {
						helper_mutates_return_storage = true;
						break;
					}
					if (!candidate.has_value_operation
							|| candidate.runtime_helper
								== ZEND_NATIVE_HELPER_COUNT) {
						continue;
					}
					const zend_mir_executable_value_ref &candidate_operation =
						candidate.value_operation;
					if (candidate_operation.op1_storage_id
								== operation.op1_storage_id
							|| candidate_operation.op2_storage_id
								== operation.op1_storage_id
							|| candidate_operation.result_storage_id
								== operation.op1_storage_id
							|| candidate_operation.auxiliary_storage_id
								== operation.op1_storage_id) {
						helper_mutates_return_storage = true;
						break;
					}
				}
				const uint64_t source_offset =
					(uint64_t{ZEND_CALL_FRAME_SLOT}
						+ operation.op1_storage_id) * sizeof(zval);
				if (source_offset > UINT32_MAX) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source-zval return offset exceeds the frame ABI");
					return false;
				}
				if (!helper_mutates_return_storage) {
					instruction.direct_scalar_return = true;
					instruction.direct_scalar_return_type =
						plan->values[value_index].exact_type;
					instruction.direct_scalar_return_offset =
						static_cast<uint32_t>(source_offset);
					instruction.runtime_helper = ZEND_NATIVE_HELPER_COUNT;
				}
			}
		}
		if (record.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL) {
			const zend_mir_executable_value_ref &operation =
				plan->instructions[i].value_operation;
			if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
					|| !plan->instructions[i].has_value_operation
					|| operation.id != record.id
					|| operation.opcode != record.opcode
					|| operation.source_opcode != ZEND_THROW
					|| operation.source_position_id
						!= record.source_position_id
					|| operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| operation.op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| operation.result.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-zval throw lacks exact source semantics");
				return false;
			}
			plan->required_runtime_capabilities |=
				ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
			plan->instructions[i].runtime_helper =
				ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL;
			require_runtime_helper(
				plan, plan->instructions[i].runtime_helper);
		}
		if (record.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
			const zend_mir_executable_value_ref &operation =
				plan->instructions[i].value_operation;
			const bool iterator_opcode =
				operation.source_opcode == ZEND_FE_RESET_R
				|| operation.source_opcode == ZEND_FE_RESET_RW
				|| operation.source_opcode == ZEND_FE_FETCH_R
				|| operation.source_opcode == ZEND_FE_FETCH_RW;
			if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
					|| !plan->instructions[i].has_value_operation
					|| operation.id != record.id
					|| operation.opcode != record.opcode
					|| operation.source_position_id
						!= record.source_position_id
					|| !iterator_opcode) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"iterator branch lacks explicit source semantics");
				return false;
			}
			plan->required_runtime_capabilities |=
				ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
			plan->instructions[i].runtime_helper =
				ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH;
			require_runtime_helper(
				plan, plan->instructions[i].runtime_helper);
		}
		const bool boxed_cond_branch =
			(record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH)
			&& plan->instructions[i].has_value_operation
			&& (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				? plan->instructions[i].value_operation.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				: plan->instructions[i].value_operation.opcode
					== record.opcode);
		if (boxed_cond_branch) {
			const zend_mir_executable_value_ref &operation =
				plan->instructions[i].value_operation;
			const bool compatible_scalar_branch =
				record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				&& count == 1;
			const bool conditional_opcode =
				(record.opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
					? operation.source_opcode
						== ZEND_BIND_INIT_STATIC_OR_JMP
					: record.opcode
						== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
						? operation.source_opcode == ZEND_JMP_FRAMELESS
						: operation.source_opcode == ZEND_JMPZ
				|| operation.source_opcode == ZEND_JMPNZ
				|| operation.source_opcode == ZEND_JMPZ_EX
				|| operation.source_opcode == ZEND_JMPNZ_EX
				|| operation.source_opcode == ZEND_JMP_SET
				|| operation.source_opcode == ZEND_COALESCE
				|| operation.source_opcode == ZEND_JMP_NULL
				|| operation.source_opcode == ZEND_ASSERT_CHECK);
			const bool exact_operands =
				record.opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
					? (operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV
						&& operation.op2.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						&& operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
					: record.opcode
						== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
						? operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_LITERAL
							&& operation.op2.kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED
							&& operation.result.kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED
						: operation.op1.kind
								!= ZEND_MIR_SOURCE_OPERAND_UNUSED
							|| operation.source_opcode
								== ZEND_ASSERT_CHECK;
			if ((!compatible_scalar_branch && count != 0)
					|| !zend_mir_id_is_valid(record.source_position_id)
					|| !plan->instructions[i].has_value_operation
					|| operation.id != record.id
					|| (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
						? operation.opcode
							!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						: operation.opcode != record.opcode)
					|| operation.source_position_id
						!= record.source_position_id
					|| !exact_operands
					|| !conditional_opcode) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source value branch lacks exact source semantics");
				return false;
			}
			plan->required_runtime_capabilities |=
				ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
			plan->instructions[i].runtime_helper =
				record.opcode == ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
					? ZEND_NATIVE_HELPER_VALUE_BIND_STATIC_BRANCH
					: record.opcode
						== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
						? ZEND_NATIVE_HELPER_VALUE_FRAMELESS_BRANCH
						: ZEND_NATIVE_HELPER_VALUE_COND_BRANCH;
			require_runtime_helper(
				plan, plan->instructions[i].runtime_helper);
		}
		if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				&& (record.effects & ZEND_MIR_EFFECT_MASK(
					ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
			zend_mir_frame_state_ref frame{};
			bool found_frame = false;
			for (uint32_t n = 0; n < view->frame_state_count(view->context); ++n) {
				zend_mir_frame_state_ref candidate{};
				if (!view->frame_state_at(view->context, n, &candidate)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"MIR frame-state table is unreadable");
					return false;
				}
				if (candidate.id == record.frame_state_id) {
					frame = candidate;
					found_frame = true;
					break;
				}
			}
			if (!found_frame || frame.function_id != plan->function.id) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"interrupt statepoint lacks its source-backed frame");
				return false;
			}
			plan->instructions[i].source_opline_index = frame.opline_index;
			plan->required_runtime_capabilities |=
				ZEND_NATIVE_RUNTIME_CAP_INTERRUPT;
			require_runtime_helper(plan, ZEND_NATIVE_HELPER_INTERRUPT_POLL);
		}
		switch (record.opcode) {
			case ZEND_MIR_OPCODE_CATCH_ENTER:
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_CATCH_ENTER);
				break;
			case ZEND_MIR_OPCODE_FINALLY_ENTER:
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_FINALLY_ENTER);
				break;
			case ZEND_MIR_OPCODE_FINALLY_CALL:
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_FINALLY_CALL);
				break;
			case ZEND_MIR_OPCODE_FINALLY_RETURN:
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_FINALLY_RETURN);
				break;
			case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL:
				if (!plan->instructions[i].direct_scalar_return) {
					plan->required_runtime_capabilities |=
						ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL);
				}
				break;
			default:
				break;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				|| record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
			zend_mir_call_site_ref site{};
			zend_mir_call_target_ref target{};
			zend_mir_call_continuation_ref exception_continuation{};
			const int32_t site_index = id_index_find(
				plan->call_site_instruction_index,
				plan->call_site_instruction_index_capacity, record.id);
			if (plan->calls == nullptr || site_index < 0
					|| plan->calls->call_site_at == nullptr
					|| plan->calls->call_target_at == nullptr
					|| plan->calls->call_continuation_at == nullptr
					|| !plan->calls->call_site_at(
						plan->calls->context,
						static_cast<uint32_t>(site_index), &site)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct call lacks its W05 call view");
				return false;
			}
			const int32_t target_index = id_index_find(
				plan->call_target_index, plan->call_target_index_capacity,
				site.target_id);
			if (site.instruction_id != record.id || target_index < 0
					|| site.arguments.offset > plan->call_argument_count
					|| site.arguments.count > plan->call_argument_count
						- site.arguments.offset
					|| !plan->calls->call_target_at(
						plan->calls->context,
						static_cast<uint32_t>(target_index), &target)
					|| target.id != site.target_id
					|| site.continuations.count != 4
					|| !plan->calls->call_continuation_at(plan->calls->context,
						site.continuations.offset + 1,
						&exception_continuation)
					|| exception_continuation.kind
						!= ZEND_MIR_CALL_CONTINUATION_EXCEPTION_DEBT
					|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& ((target.kind != ZEND_MIR_CALL_TARGET_DIRECT_USER
								&& target.kind
									!= ZEND_MIR_CALL_TARGET_METHOD_USER
								&& target.kind
									!= ZEND_MIR_CALL_TARGET_DYNAMIC)
							|| (site.arguments.count != count
								&& count != 0)))
					|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
						&& (count != 0
							|| target.kind
								!= ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL))) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct call instruction and call site disagree");
				return false;
			}
			plan->instructions[i].call_site = site;
			plan->instructions[i].exception_block_id =
				exception_continuation.block_id;
			plan->instructions[i].call_argument_offset = site.arguments.offset;
			plan->instructions[i].call_argument_count = site.arguments.count;
			if (source_op_array == nullptr
					|| site.source_do_opline_index >= source_op_array->last) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct call has no explicit source completion descriptor");
				return false;
			}
			const bool fragment_call =
				call_site_requires_source_fragments(plan, site);
			plan->instructions[i].user_opcode_call_fragments =
				fragment_call;
			if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
				const bool source_arguments = count == 0
					&& site.arguments.count != 0;
				const int32_t binding_index = id_index_find(
					plan->user_binding_index,
					plan->user_binding_index_capacity, site.target_id);
				if (binding_index < 0) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
						"direct user call has no native entry-cell binding");
					return false;
				}
				plan->instructions[i].entry_cell =
					user_bindings[binding_index].entry_cell;
				plan->instructions[i].component_target_index =
					user_bindings[binding_index].component_target_index;
				bool direct_descriptor =
					!fragment_call
					&& (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_USER
						|| (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER
							&& user_bindings[binding_index].direct_native))
					&& site.arguments.count >= target.required_num_args;
				for (uint32_t n = 0;
						direct_descriptor && n < site.arguments.count; ++n) {
					zend_mir_call_argument_ref argument;
					if (!zend_tpde_call_argument_at(
							plan, site.arguments.offset + n, &argument)) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct user-call argument view changed during compilation");
						return false;
					}
					direct_descriptor = argument.ordinal == n
						&& (argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
							|| argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE)
						&& (argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_LITERAL
							|| argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA);
				}
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_USER_CALL
						| ZEND_NATIVE_RUNTIME_CAP_OBSERVER;
				if (direct_descriptor) {
					const zend_op *init = nullptr;
					if (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER) {
						if (source_op_array == nullptr
								|| site.source_init_opline_index
									>= source_op_array->last) {
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"direct native method has no source descriptor");
							return false;
						}
						init = &source_op_array->opcodes[
							site.source_init_opline_index];
					}
					const size_t descriptor_size =
						offsetof(zend_native_direct_call_descriptor, arguments)
						+ static_cast<size_t>(site.arguments.count)
							* sizeof(zend_native_direct_call_argument);
					auto *descriptor =
						static_cast<zend_native_direct_call_descriptor *>(
							std::calloc(1, descriptor_size));
					if (descriptor == nullptr) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
							"unable to allocate a direct user-call descriptor");
						return false;
					}
					descriptor->argument_count = site.arguments.count;
					descriptor->source_position =
						site.source_do_opline_index;
					descriptor->expected_function =
						plan->instructions[i].entry_cell != nullptr
							? plan->instructions[i].entry_cell->function : nullptr;
					if (plan->instructions[i].entry_cell != nullptr
							&& plan->instructions[i].entry_cell->lease_managed) {
						descriptor->flags |=
							ZEND_NATIVE_DIRECT_CALL_GENERATION_LEASED;
					}
					descriptor->receiver_kind =
						ZEND_NATIVE_INTERNAL_RECEIVER_NONE;
					descriptor->receiver_operand.kind =
						ZEND_MIR_SOURCE_OPERAND_UNUSED;
					descriptor->receiver_operand.slot_kind =
						ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
					descriptor->receiver_operand.index =
						ZEND_MIR_ID_INVALID;
					descriptor->receiver_operand.ssa_variable_id =
						ZEND_MIR_ID_INVALID;
					if (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER) {
						if (init->opcode == ZEND_INIT_STATIC_METHOD_CALL) {
							if (init->op2_type != IS_CONST
									|| descriptor->expected_function == nullptr
									|| descriptor->expected_function->common.scope
										== nullptr
									|| (descriptor->expected_function->common.fn_flags
										& ZEND_ACC_STATIC) == 0
									|| (init->op1_type != IS_CONST
										&& init->op1_type != IS_UNUSED)) {
								std::free(descriptor);
								zend_tpde_set_diagnostic(diag,
									ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
									"direct native static method has no fixed scope");
								return false;
							}
							descriptor->receiver_kind =
								ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE;
							if (init->op1_type == IS_UNUSED) {
								descriptor->flags |=
									ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE;
							} else {
								descriptor->called_scope =
									descriptor->expected_function->common.scope;
							}
						} else if (init->opcode != ZEND_INIT_METHOD_CALL) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"direct native method has an unsupported source init");
							return false;
						} else if (init->op1_type == IS_UNUSED) {
							descriptor->receiver_kind =
								ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS;
						} else if (init->op1_type == IS_CV
								|| init->op1_type == IS_VAR
								|| init->op1_type == IS_TMP_VAR) {
							if (!source_descriptor_operand(
									source_op_array, init, init->op1_type,
									init->op1,
									&descriptor->receiver_operand)) {
								std::free(descriptor);
								zend_tpde_set_diagnostic(diag,
									ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
									"direct native method receiver is not explicit");
								return false;
							}
							if (init->op1_type == IS_VAR
									|| init->op1_type == IS_TMP_VAR) {
								descriptor->flags |=
									ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER;
							}
							descriptor->receiver_kind =
								ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT;
						} else {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"direct native method receiver is not explicit");
							return false;
						}
					}
					descriptor->result_operand = site.result_operand;
					descriptor->result_type = ZEND_MIR_SCALAR_TYPE_NONE;
					bool trivial_frame =
						plan->instructions[i].entry_cell != nullptr
						&& plan->instructions[i].entry_cell->function != nullptr
						&& ZEND_USER_CODE(
							plan->instructions[i].entry_cell->function->type);
					zend_function *callee = trivial_frame
						? plan->instructions[i].entry_cell->function : nullptr;
					if (trivial_frame) {
						const zend_op_array &op_array = callee->op_array;
						const bool inline_receiver =
							(op_array.scope == nullptr
								&& descriptor->receiver_kind
									== ZEND_NATIVE_INTERNAL_RECEIVER_NONE)
							|| (op_array.scope != nullptr
								&& ((descriptor->receiver_kind
											== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS
										&& (op_array.fn_flags
											& ZEND_ACC_STATIC) == 0)
									|| (descriptor->receiver_kind
											== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE
										&& (op_array.fn_flags
											& ZEND_ACC_STATIC) != 0
										&& (((descriptor->flags
													& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
												!= 0
											&& descriptor->called_scope == nullptr)
											|| ((descriptor->flags
													& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
												== 0
											&& descriptor->called_scope != nullptr)))
									|| (descriptor->receiver_kind
											== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
										&& (op_array.fn_flags
											& ZEND_ACC_STATIC) == 0
										&& (descriptor->flags
											& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER)
											== 0
										&& (descriptor->receiver_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_SLOT
											|| descriptor->receiver_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_SSA)
										&& descriptor->receiver_operand.slot_kind
											== ZEND_MIR_SOURCE_SLOT_CV)));
						trivial_frame =
							inline_receiver
							&& site.arguments.count
								>= op_array.required_num_args
							&& (op_array.fn_flags
								& (ZEND_ACC_VARIADIC
									| ZEND_ACC_CALL_VIA_TRAMPOLINE)) == 0;
						for (uint32_t n = site.arguments.count;
								trivial_frame && n < op_array.num_args; ++n) {
							const zend_op &receive = op_array.opcodes[n];
							const zval *default_value =
								receive.opcode == ZEND_RECV_INIT
									&& receive.op1.num == n + 1
									&& receive.op2_type == IS_CONST
									&& EX_VAR_TO_NUM(receive.result.var) == n
								? RT_CONSTANT(&receive, receive.op2)
								: nullptr;
							trivial_frame = default_value != nullptr
								&& default_value >= op_array.literals
								&& default_value
									< op_array.literals + op_array.last_literal
								&& static_cast<size_t>(
									default_value - op_array.literals)
									<= static_cast<size_t>(INT32_MAX)
										/ sizeof(zval)
								&& Z_TYPE_P(default_value) != IS_CONSTANT_AST;
						}
						if (trivial_frame) {
							descriptor->frame_size = zend_vm_calc_used_stack(
								site.arguments.count, callee);
						}
					}
					if (zend_mir_id_is_valid(record.result_id)) {
						const int32_t result_index =
							zend_tpde_value_index(plan, record.result_id);
						if (result_index < 0) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"direct user-call result has no value record");
							return false;
						}
						if (zend_mir_scalar_type_is_exact(
								plan->values[result_index].exact_type)) {
							descriptor->result_type =
								plan->values[result_index].exact_type;
						}
					}
					if (!zend_mir_scalar_type_is_exact(
							descriptor->result_type)
							&& callee->op_array.arg_info != nullptr
							&& (callee->common.fn_flags
									& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
						descriptor->result_type =
							exact_scalar_from_declared_type(
								callee->op_array.arg_info[-1].type);
					}
					for (uint32_t n = 0; n < site.arguments.count; ++n) {
						zend_mir_call_argument_ref argument;
						if (!zend_tpde_call_argument_at(
								plan, site.arguments.offset + n, &argument)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"direct user-call argument view changed during compilation");
							return false;
						}
						descriptor->arguments[n].ordinal = argument.ordinal;
						descriptor->arguments[n].mode =
							argument.ownership
									== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
								? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
								: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
						const int32_t argument_value_index =
							zend_tpde_value_index(plan, argument.value_id);
						descriptor->arguments[n].exact_type =
							descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& argument_value_index >= 0
							? plan->values[argument_value_index].exact_type
							: ZEND_MIR_SCALAR_TYPE_NONE;
						descriptor->arguments[n].source_operand =
							argument.source_operand;
						descriptor->arguments[n].scalar_bits = 0;
						descriptor->arguments[n].source_frame_offset =
							UINT32_MAX;
						if (source_op_array != nullptr
								&& (argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_SLOT
									|| argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_SSA)) {
							uint64_t slot =
								argument.source_operand.index;
							if (argument.source_operand.slot_kind
									!= ZEND_MIR_SOURCE_SLOT_CV) {
								slot += source_op_array->last_var;
							}
							slot += ZEND_CALL_FRAME_SLOT;
							if (slot <= UINT32_MAX / sizeof(zval)) {
								descriptor->arguments[n]
									.source_frame_offset =
										static_cast<uint32_t>(
											slot * sizeof(zval));
							}
						}
						if (source_op_array != nullptr
								&& argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL
								&& argument.source_operand.index
									< source_op_array->last_literal) {
							/*
							 * The source literal is authoritative. A boxed
							 * literal may share a topology-only MIR value with
							 * a NULL placeholder, but it must never be
							 * materialized as that placeholder in an inline
							 * direct-call frame.
							 */
							descriptor->arguments[n].exact_type =
								exact_scalar_from_zval(
									&source_op_array->literals[
										argument.source_operand.index]);
							descriptor->arguments[n].scalar_bits =
								scalar_bits_from_zval(
									&source_op_array->literals[
										argument.source_operand.index]);
						}
						if (!zend_mir_scalar_type_is_exact(
								descriptor->arguments[n].exact_type)
								&& descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
							descriptor->arguments[n].exact_type =
								exact_scalar_from_source_argument(
									source_op_array, source_ssa, plan->calls,
									user_bindings, user_binding_count, argument);
						}
						if (!zend_mir_scalar_type_is_exact(
								descriptor->arguments[n].exact_type)
								&& descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& source_op_array != nullptr
								&& source_op_array->arg_info != nullptr
								&& (argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_SLOT
									|| argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_SSA)
								&& argument.source_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_CV
								&& argument.source_operand.index
									< source_op_array->num_args) {
							descriptor->arguments[n].exact_type =
								exact_scalar_from_declared_type(
									source_op_array->arg_info[
										argument.source_operand.index].type);
						}
						const bool inline_argument =
							(descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& (zend_mir_scalar_type_is_exact(
										descriptor->arguments[n].exact_type)
									|| ((argument.source_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_SLOT
											|| argument.source_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_SSA)
										&& argument.source_operand.slot_kind
											== ZEND_MIR_SOURCE_SLOT_CV)))
							|| (descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
								&& (argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_SLOT
									|| argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_SSA)
								&& argument.source_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_CV);
						const bool inline_parameter =
							!trivial_frame
							|| n >= callee->op_array.num_args
							|| callee->op_array.arg_info == nullptr
							|| !ZEND_TYPE_IS_SET(
								callee->op_array.arg_info[n].type)
							|| (descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& exact_scalar_satisfies_type(
									descriptor->arguments[n].exact_type,
									callee->op_array.arg_info[n].type));
						trivial_frame =
							trivial_frame && inline_argument && inline_parameter;
					}
					const bool inline_result =
						descriptor->result_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| ((descriptor->result_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_SLOT
								|| descriptor->result_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_SSA)
							&& (descriptor->result_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_CV
								|| descriptor->result_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_TMP
								|| descriptor->result_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_VAR));
					if (trivial_frame && inline_result) {
						descriptor->flags |=
							ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME;
						bool leaf_scalar_frame =
							user_bindings[binding_index].leaf_scalar_frame
							&& descriptor->receiver_kind
								== ZEND_NATIVE_INTERNAL_RECEIVER_NONE
							&& site.arguments.count
								== callee->op_array.num_args
							&& (descriptor->result_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_UNUSED
								|| zend_mir_scalar_type_is_exact(
									descriptor->result_type));
						for (uint32_t n = 0;
								leaf_scalar_frame
									&& n < site.arguments.count; ++n) {
							leaf_scalar_frame =
								descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& zend_mir_scalar_type_is_exact(
									descriptor->arguments[n].exact_type);
						}
						if (leaf_scalar_frame) {
							descriptor->flags |=
								ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME;
						}
					}
					plan->instructions[i].direct_call = descriptor;
					plan->direct_calls[plan->direct_call_count++] = descriptor;
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_DIRECT_USER_CALL_ENTER);
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE);
				} else {
					if (source_op_array == nullptr
							|| site.source_init_opline_index
								>= source_op_array->last) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"user call has no explicit source init descriptor");
						return false;
					}
					const zend_op *init = &source_op_array->opcodes[
						site.source_init_opline_index];
					const zend_op *finish = &source_op_array->opcodes[
						site.source_do_opline_index];
					if (init->extended_value > site.arguments.count) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"user call source argument count is inconsistent");
						return false;
					}
					const size_t descriptor_size =
						offsetof(zend_native_user_call_descriptor, arguments)
						+ static_cast<size_t>(site.arguments.count)
							* sizeof(zend_native_direct_internal_call_argument);
					auto *descriptor =
						static_cast<zend_native_user_call_descriptor *>(
							std::calloc(1, descriptor_size));
					if (descriptor == nullptr) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
							"unable to allocate a user-call descriptor");
						return false;
					}
					descriptor->argument_count = site.arguments.count;
					descriptor->initial_argument_count = init->extended_value;
					descriptor->init_source_position =
						site.source_init_opline_index;
					descriptor->do_source_position =
						site.source_do_opline_index;
					descriptor->init_opcode = init->opcode;
					descriptor->do_opcode = finish->opcode;
					descriptor->init_op1_payload = init->op1.num;
					descriptor->init_op2_payload = init->op2.num;
					descriptor->init_result_payload = init->result.num;
					descriptor->init_extended_value = init->extended_value;
					descriptor->do_op1_payload = finish->op1.num;
					descriptor->do_op2_payload = finish->op2.num;
					descriptor->do_result_payload = finish->result.num;
					descriptor->do_extended_value = finish->extended_value;
					if (!source_descriptor_operand(
								source_op_array, init, init->op1_type,
								init->op1, &descriptor->init_op1)
							|| !source_descriptor_operand(
								source_op_array, init, init->op2_type,
								init->op2, &descriptor->init_op2)
							|| !source_descriptor_operand(
								source_op_array, init, init->result_type,
								init->result, &descriptor->init_result)
							|| !source_descriptor_operand(
								source_op_array, finish, finish->op1_type,
								finish->op1, &descriptor->do_op1)
							|| !source_descriptor_operand(
								source_op_array, finish, finish->op2_type,
								finish->op2, &descriptor->do_op2)) {
						std::free(descriptor);
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"user call source operands are invalid");
						return false;
					}
					descriptor->do_result = site.result_operand;
					if (zend_mir_id_is_valid(record.result_id)) {
						const int32_t result_index =
							zend_tpde_value_index(plan, record.result_id);
						if (result_index < 0
								|| !zend_mir_scalar_type_is_exact(
									plan->values[result_index].exact_type)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"dynamic user-call result is not an exact scalar");
							return false;
						}
						descriptor->result_type =
							plan->values[result_index].exact_type;
						descriptor->flags |=
							ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT;
					}
					for (uint32_t n = 0; n < site.arguments.count; ++n) {
						zend_mir_call_argument_ref argument;
						if (!zend_tpde_call_argument_at(
									plan, site.arguments.offset + n, &argument)
								|| argument.send_opline_index
									>= source_op_array->last) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"user-call argument table is unreadable");
							return false;
						}
						const zend_op *send = &source_op_array->opcodes[
							argument.send_opline_index];
						if (!source_descriptor_send_opcode(send->opcode)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"user-call SEND opcode is invalid");
							return false;
						}
						zend_native_direct_internal_call_argument &encoded =
							descriptor->arguments[n];
						encoded.ordinal = argument.ordinal;
						encoded.mode = argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
							? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
							: argument.ownership
									== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
								? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
								: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
						encoded.source_opcode = send->opcode;
						encoded.source_position = argument.send_opline_index;
						encoded.source_operand = argument.source_operand;
						encoded.auxiliary_payload = send->op2.num;
						encoded.result_payload = send->result.num;
						encoded.extended_value = send->extended_value;
						if (!source_descriptor_operand(
								source_op_array, send, send->op2_type,
								send->op2, &encoded.auxiliary_operand)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"user-call auxiliary operand is invalid");
							return false;
						}
					}
					plan->instructions[i].user_call = descriptor;
					plan->user_calls[plan->user_call_count++] = descriptor;
					if (fragment_call) {
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_CALL_FRAGMENT);
					} else if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT
							|| descriptor->do_opcode
								== ZEND_CALLABLE_CONVERT_PARTIAL) {
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_USER_CALL_BEGIN);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE);
					} else {
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_DYNAMIC_USER_CALL_ENTER);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_DYNAMIC_USER_CALL_LEAVE);
					}
				}
				if (!fragment_call
						&& source_arguments && !direct_descriptor) {
					for (uint32_t n = 0; n < site.arguments.count; ++n) {
						zend_mir_call_argument_ref argument;
						if (!zend_tpde_call_argument_at(
								plan, site.arguments.offset + n, &argument)) {
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"direct user-call argument view changed during compilation");
							return false;
						}
						if (argument.ownership
								== ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR) {
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"source-backed user call has a scalar argument");
							return false;
						}
					}
					if (!direct_descriptor
							&& (plan->instructions[i].user_call->do_opcode
								== ZEND_CALLABLE_CONVERT
								|| plan->instructions[i].user_call->do_opcode
									== ZEND_CALLABLE_CONVERT_PARTIAL)) {
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT);
					}
				}
				for (uint32_t n = 0;
						!fragment_call && n < count; ++n) {
					zend_mir_value_id operand_id;
					if (!view->instruction_operand_at(
							view->context, record.id, n, &operand_id)) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct user call operand table is unreadable");
						return false;
					}
					int32_t value_index = zend_tpde_value_index(plan, operand_id);
					if (value_index < 0) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct user call operand is unknown");
						return false;
					}
					if (!direct_descriptor) {
						require_runtime_helper(plan,
							plan->values[value_index].exact_type
								== ZEND_MIR_SCALAR_TYPE_F64
								? ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE
								: ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER);
					}
				}
			} else {
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_INTERNAL_CALL
						| ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT
						| ZEND_NATIVE_RUNTIME_CAP_OBSERVER;
				const int32_t binding_index = id_index_find(
					plan->internal_binding_index,
					plan->internal_binding_index_capacity, site.target_id);
				if (binding_index < 0 || source_op_array == nullptr
						|| site.source_init_opline_index
							>= source_op_array->last
						|| site.source_do_opline_index
							>= source_op_array->last) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
						"direct internal call has no runtime binding or source descriptor");
					return false;
				}
				plan->instructions[i].internal_call_cell =
					internal_bindings[binding_index].call_cell;
				if (fragment_call) {
					zend_native_user_call_descriptor *descriptor =
						build_user_call_descriptor(
							plan, site, record, diag);
					if (descriptor == nullptr) {
						return false;
					}
					plan->instructions[i].user_call = descriptor;
					plan->user_calls[
						plan->user_call_count++] = descriptor;
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_CALL_FRAGMENT);
				} else {
					const zend_op *init = &source_op_array->opcodes[
					site.source_init_opline_index];
					const zend_op *finish = &source_op_array->opcodes[
					site.source_do_opline_index];
					if ((finish->opcode != ZEND_DO_ICALL
							&& finish->opcode != ZEND_DO_FCALL)
						|| init->extended_value > site.arguments.count) {
						zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"direct internal call source sequence is inconsistent");
						return false;
					}
					const size_t descriptor_size = offsetof(
						zend_native_direct_internal_call_descriptor, arguments)
					+ static_cast<size_t>(site.arguments.count)
						* sizeof(zend_native_direct_internal_call_argument);
					auto *descriptor =
					static_cast<zend_native_direct_internal_call_descriptor *>(
						std::calloc(1, descriptor_size));
					if (descriptor == nullptr) {
						zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
						"unable to allocate a direct internal-call descriptor");
						return false;
					}
					descriptor->argument_count = site.arguments.count;
					descriptor->initial_argument_count = init->extended_value;
					descriptor->init_source_position =
					site.source_init_opline_index;
					descriptor->do_source_position = site.source_do_opline_index;
					descriptor->result_operand = site.result_operand;
					descriptor->result_type = ZEND_MIR_SCALAR_TYPE_NONE;
					if (!source_descriptor_operand(
						source_op_array, init, init->op1_type, init->op1,
						&descriptor->receiver_operand)) {
						std::free(descriptor);
						zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"direct internal-call receiver operand is invalid");
						return false;
					}
					if (zend_mir_id_is_valid(record.result_id)) {
						const int32_t result_index =
						zend_tpde_value_index(plan, record.result_id);
						if (result_index < 0
							|| !zend_mir_scalar_type_is_exact(
								plan->values[result_index].exact_type)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct internal-call result has no exact scalar value");
							return false;
						}
						descriptor->result_type =
							plan->values[result_index].exact_type;
					}
					for (uint32_t n = 0; n < site.arguments.count; ++n) {
						zend_mir_call_argument_ref argument;
						if (!zend_tpde_call_argument_at(
								plan, site.arguments.offset + n, &argument)
							|| argument.send_opline_index
								>= source_op_array->last) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct internal-call argument table is unreadable");
							return false;
						}
						const zend_op *send = &source_op_array->opcodes[
						argument.send_opline_index];
						if (!source_descriptor_send_opcode(send->opcode)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct internal-call SEND opcode is invalid");
							return false;
						}
						zend_native_direct_internal_call_argument &encoded =
						descriptor->arguments[n];
						encoded.ordinal = argument.ordinal;
						encoded.mode = argument.source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
						? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
						: argument.ownership
								== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
							? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
							: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
						encoded.source_opcode = send->opcode;
						encoded.source_position = argument.send_opline_index;
						encoded.source_operand = argument.source_operand;
						encoded.auxiliary_payload = send->op2.num;
						encoded.result_payload = send->result.num;
						encoded.extended_value = send->extended_value;
						if (!source_descriptor_operand(
							source_op_array, send, send->op2_type, send->op2,
							&encoded.auxiliary_operand)) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct internal-call auxiliary operand is invalid");
							return false;
						}
					}
					plan->instructions[i].direct_internal_call = descriptor;
					plan->direct_internal_calls[
						plan->direct_internal_call_count++] = descriptor;
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL);
				}
			}
			if (zend_mir_id_is_valid(record.result_id)
					&& plan->instructions[i].direct_call == nullptr
					&& plan->instructions[i].direct_internal_call == nullptr
					&& (plan->instructions[i].user_call == nullptr
						|| plan->instructions[i].user_call->do_opcode
							== ZEND_CALLABLE_CONVERT
						|| plan->instructions[i].user_call->do_opcode
							== ZEND_CALLABLE_CONVERT_PARTIAL)) {
				require_runtime_helper(
					plan, ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR);
			}
		}
	}
	for (uint32_t i = 0; i < effect_count; ++i) {
		const zend_native_source_effect &effect = effects[i];
		zend_tpde_instruction *match = nullptr;

		if (!zend_mir_id_is_valid(effect.source_position_id)
				|| (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE
					? !zend_mir_id_is_valid(effect.target_block_id)
					: (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE
						? false
						: ((effect.kind
								!= ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR
							&& effect.kind
								!= ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE)
							|| !zend_mir_scalar_type_is_exact(
								effect.exact_type))))) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"W07 source effect is invalid");
			return false;
		}
		for (uint32_t n = 0; n < plan->instruction_count; ++n) {
			zend_tpde_instruction &candidate = plan->instructions[n];
			const zend_mir_instruction_record candidate_record =
				zend_tpde_instruction_record_at(plan, &candidate);
			if (candidate_record.source_position_id != effect.source_position_id) {
				continue;
			}
			if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE) {
				if (executable_value_helper(candidate_record.opcode)
						== ZEND_NATIVE_HELPER_COUNT
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_GENERATOR_CREATE
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_GENERATOR_YIELD
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM) {
					continue;
				}
			} else if (effect.kind != ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE
					&& candidate_record.opcode
						!= ZEND_MIR_OPCODE_ECHO_SCALAR
					&& candidate_record.opcode != ZEND_MIR_OPCODE_I1_NOT
					&& candidate_record.opcode != ZEND_MIR_OPCODE_I64_TO_I1
					&& candidate_record.opcode != ZEND_MIR_OPCODE_F64_TO_I1
					&& candidate_record.opcode != ZEND_MIR_OPCODE_SCALAR_DROP) {
				continue;
			}
			if (match != nullptr
					&& effect.kind
						!= ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"W07 source effect maps to multiple MIR instructions");
				return false;
			}
			if (match != nullptr) {
				continue;
			}
			match = &candidate;
		}
		if (match == nullptr
				&& effect.kind == ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE) {
			continue;
		}
		if (match == nullptr
				|| (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE
					? zend_mir_id_is_valid(match->exception_block_id)
					: (effect.kind
							== ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE
						? match->debug_probe
						: (match->operand_count != 1
						|| match->source_effect != 0)))) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE
					? "value exception route must map uniquely to an instruction"
					: "W07 echo must map uniquely to a scalar value proof");
			return false;
		}
		if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE) {
			match->exception_block_id = effect.target_block_id;
			continue;
		}
		if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE) {
			match->debug_probe = true;
			require_runtime_helper(plan, ZEND_NATIVE_HELPER_SOURCE_PROBE);
			continue;
		}
		match->source_effect = effect.kind;
		match->source_effect_exact_type = effect.exact_type;
		if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE) {
			require_runtime_helper(plan, ZEND_NATIVE_HELPER_ABI_CONFORMANCE);
		} else {
			match->runtime_helper =
				effect.exact_type == ZEND_MIR_SCALAR_TYPE_F64
					? ZEND_NATIVE_HELPER_ECHO_DOUBLE
					: ZEND_NATIVE_HELPER_ECHO_INTEGER;
			require_runtime_helper(plan, match->runtime_helper);
		}
	}
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		for (uint32_t n = 0; n < instruction.operand_count; ++n) {
			zend_mir_value_id operand;
			if (!view->instruction_operand_at(view->context,
					instruction.id, n, &operand)
					|| zend_tpde_value_index(plan, operand) < 0) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR operand table is unreadable or references an unknown value");
				return false;
			}
		}
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR) {
			zend_mir_value_id expected;
			if (!source_operand_value_id(
					instruction.value_operation.op1, expected)
					|| instruction.operand_count != 1
					|| zend_tpde_operand_at(plan, &instruction, 0) != expected) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"semantic echo MIR operand differs from source semantics");
				return false;
			}
		}
	}

	for (uint32_t i = 0; i < frame_slot_count; ++i) {
		zend_mir_frame_slot_ref slot;
		if (!view->frame_slot_at(view->context, i, &slot)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR frame-slot table is unreadable");
			return false;
		}
		bool frame_argument = frame_argument_count == UINT32_MAX
			? (slot.kind == ZEND_MIR_FRAME_SLOT_KIND_ARGUMENT
				|| slot.kind == ZEND_MIR_FRAME_SLOT_KIND_CV)
			: slot.kind == ZEND_MIR_FRAME_SLOT_KIND_CV
				&& slot.index < frame_argument_count;
		if ((plan->value_model_flags
					& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0
				&& frame_argument
				&& slot.materialization == ZEND_MIR_MATERIALIZATION_MATERIALIZED
				&& zend_mir_id_is_valid(slot.value_id)) {
			int32_t value_index = zend_tpde_value_index(plan, slot.value_id);
			if (value_index >= 0 && plan->values[value_index].argument_index < 0) {
				plan->values[value_index].argument_index = static_cast<int32_t>(slot.index);
				/* Frame arguments are invocation-local even when source analysis
				 * inferred a constant at a particular call site. Native code is
				 * compiled once per function and must load them from execute_data. */
				plan->values[value_index].constant = false;
				if (slot.index == UINT32_MAX) {
					zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"MIR argument index overflows");
					return false;
				}
				if (plan->argument_count <= slot.index) {
					plan->argument_count = slot.index + 1;
				}
			}
		}
	}
	if (!freeze_generator_resume_liveness(plan, diag)) {
		return false;
	}
	if (zend_native_runtime_validate(plan->runtime,
			plan->required_runtime_capabilities, diag) == FAILURE) {
		return false;
	}
	for (uint32_t id = 1; id < ZEND_NATIVE_HELPER_COUNT; ++id) {
		if ((plan->required_runtime_helpers[id / 64u]
				& (UINT64_C(1) << (id % 64u))) != 0) {
			const zend_native_runtime_helper *helper =
				zend_native_runtime_helper_find(plan->runtime,
					static_cast<zend_native_runtime_helper_id>(id));
			if (helper == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
					"native runtime lacks a required symbolic helper");
				return false;
			}
		}
	}
	plan->may_emit_calls = false;
	for (uint32_t index = 0;
			index < ZEND_NATIVE_RUNTIME_HELPER_WORD_COUNT; ++index) {
		plan->may_emit_calls = plan->may_emit_calls
			|| plan->required_runtime_helpers[index] != 0;
	}
	return true;
}

bool source_opline_decoding_helper(zend_native_runtime_helper_id helper) {
	switch (helper) {
		case ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT:
		case ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE:
		case ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR:
		case ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE:
			return true;
		default:
			return false;
	}
}

zend_native_image_metrics collect_plan_metrics(const zend_tpde_plan &plan) {
	zend_native_image_metrics metrics{};

	for (uint32_t index = 0; index < plan.instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan.instructions[index];
		const zend_native_runtime_helper_id helper =
			instruction.runtime_helper;
		bool guarded_fast_path = false;
		zend_tpde_array_read array_read{};
		zend_tpde_packed_array_append array_append{};
		zend_tpde_array_isset array_isset{};
		zend_tpde_string_length string_length{};
		zend_tpde_string_identity string_identity{};
		zend_tpde_value_condition value_condition{};
		zend_tpde_slot_isset_empty slot_isset{};
		zend_tpde_object_property_read property_read{};
		zend_tpde_object_property_write property_write{};

		if (helper != ZEND_NATIVE_HELPER_COUNT) {
			metrics.runtime_helper_sites++;
			if (source_opline_decoding_helper(helper)) {
				metrics.source_opline_decode_sites++;
			}
		}
		guarded_fast_path =
			zend_tpde_array_read_at(instruction, &array_read)
			|| zend_tpde_packed_array_append_at(
				instruction, &array_append)
			|| zend_tpde_array_isset_at(instruction, &array_isset)
			|| zend_tpde_string_length_at(instruction, &string_length)
			|| zend_tpde_string_identity_at(
				instruction, &string_identity)
			|| zend_tpde_value_condition_at(
				instruction, &value_condition)
			|| zend_tpde_slot_isset_empty_at(
				instruction, &slot_isset)
			|| zend_tpde_object_property_read_at(
				instruction, &property_read)
			|| zend_tpde_object_property_write_at(
				instruction, &property_write);
		if (guarded_fast_path) {
			metrics.guard_sites++;
			if (helper != ZEND_NATIVE_HELPER_COUNT) {
				metrics.slow_path_sites++;
			}
		}
	}
	metrics.direct_call_sites = plan.direct_call_count;
	for (uint32_t index = 0; index < plan.direct_call_count; ++index) {
		if (plan.direct_calls[index] != nullptr) {
			metrics.direct_call_frame_bytes +=
				plan.direct_calls[index]->frame_size;
		}
	}
	return metrics;
}
} // namespace

void zend_tpde_set_diagnostic(
	zend_native_diagnostic *diag,
	zend_native_diagnostic_code code,
	const char *message) {
	if (diag == nullptr) {
		return;
	}
	diag->code = code;
	std::snprintf(diag->message, sizeof(diag->message), "%s", message);
}

int32_t zend_tpde_value_index(const zend_tpde_plan *plan, zend_mir_value_id id) {
	return id_index_find(plan->value_index, plan->value_index_capacity, id);
}

int32_t zend_tpde_block_index(const zend_tpde_plan *plan, zend_mir_block_id id) {
	return id_index_find(plan->block_index, plan->block_index_capacity, id);
}

int32_t zend_tpde_instruction_index(
	const zend_tpde_plan *plan, zend_mir_instruction_id id) {
	return id_index_find(
		plan->instruction_index, plan->instruction_index_capacity, id);
}

const zend_tpde_instruction *zend_tpde_instruction_at(
	const zend_tpde_plan *plan, uint32_t index) {
	return index < plan->instruction_count ? &plan->instructions[index] : nullptr;
}

zend_mir_instruction_record zend_tpde_instruction_record_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction) {
	zend_mir_instruction_record record{};
	if (instruction == nullptr
			|| instruction->view_index >= plan->instruction_count
			|| !plan->view->instruction_at(plan->view->context,
				instruction->view_index, &record)
			|| record.id != instruction->id) {
		record.id = ZEND_MIR_ID_INVALID;
	}
	return record;
}

bool zend_tpde_call_argument_at(
	const zend_tpde_plan *plan,
	uint32_t index,
	zend_mir_call_argument_ref *out) {
	return plan != nullptr && plan->calls != nullptr && out != nullptr
		&& index < plan->call_argument_count
		&& plan->calls->call_argument_at != nullptr
		&& plan->calls->call_argument_at(plan->calls->context, index, out);
}

zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index) {
	if (index >= instruction->operand_count) {
		return ZEND_MIR_ID_INVALID;
	}
	zend_mir_value_id operand = ZEND_MIR_ID_INVALID;
	return plan->view->instruction_operand_at(plan->view->context,
			instruction->id, index, &operand)
		? operand
		: ZEND_MIR_ID_INVALID;
}

bool zend_tpde_image_append(
	zend_native_image *image, const void *bytes, size_t length) {
	if (length > MAX_NATIVE_IMAGE_BYTES
			|| image->text_size > MAX_NATIVE_IMAGE_BYTES - length) {
		return false;
	}
	size_t needed = image->text_size + length;
	if (needed > image->text_capacity) {
		size_t capacity = image->text_capacity == 0 ? 4096 : image->text_capacity;
		while (capacity < needed) {
			capacity = capacity > MAX_NATIVE_IMAGE_BYTES / 2
				? MAX_NATIVE_IMAGE_BYTES
				: capacity * 2;
		}
		void *resized = std::realloc(image->text, capacity);
		if (resized == nullptr) {
			return false;
		}
		image->text = static_cast<unsigned char *>(resized);
		image->text_capacity = capacity;
	}
	std::memcpy(image->text + image->text_size, bytes, length);
	image->text_size = needed;
	return true;
}

bool zend_tpde_image_u8(zend_native_image *image, uint8_t value) {
	return zend_tpde_image_append(image, &value, sizeof(value));
}
bool zend_tpde_image_u32(zend_native_image *image, uint32_t value) {
	return zend_tpde_image_append(image, &value, sizeof(value));
}
bool zend_tpde_image_u64(zend_native_image *image, uint64_t value) {
	return zend_tpde_image_append(image, &value, sizeof(value));
}

const zend_native_image_symbol *zend_tpde_image_symbol_find(
	const zend_native_image *image,
	zend_native_image_symbol_kind kind,
	uint32_t id,
	uint32_t symbol_namespace) {
	if (image == nullptr) {
		return nullptr;
	}
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		const zend_native_image_symbol &symbol = image->symbols[index];
		if (symbol.kind == kind && symbol.id == id
				&& symbol.symbol_namespace == symbol_namespace) {
			return &symbol;
		}
	}
	return nullptr;
}

bool zend_tpde_image_resolve_symbol(
	const zend_native_image *image,
	const char *name,
	const void **address) {
	if (image == nullptr || name == nullptr || address == nullptr
			|| image->abi_version != NATIVE_IMAGE_ABI_VERSION) {
		return false;
	}
	*address = nullptr;
	const zend_native_image_symbol *symbol = nullptr;
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		if (std::strcmp(image->symbols[index].name, name) == 0) {
			symbol = &image->symbols[index];
			break;
		}
	}
	if (symbol == nullptr) {
		return false;
	}
	if (symbol->kind != ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER) {
		if (symbol->abi_version != NATIVE_IMAGE_ABI_VERSION
				|| symbol->effects != 0
				|| (symbol->kind != ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR
					&& symbol->kind
						!= ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR)) {
			return false;
		}
		const uint32_t symbol_index =
			static_cast<uint32_t>(symbol - image->symbols);
		for (uint32_t index = 0;
				index < image->symbol_binding_count; ++index) {
			const zend_native_image_symbol_binding &binding =
				image->symbol_bindings[index];
			if (binding.symbol_index == symbol_index
					&& binding.address != nullptr) {
				*address = binding.address;
				return true;
			}
		}
		return false;
	}
	const zend_native_runtime_api *runtime = zend_native_runtime_get();
	if (runtime == nullptr
			|| runtime->abi_version != image->runtime_abi_version
			|| symbol->abi_version != runtime->abi_version) {
		return false;
	}
	const zend_native_runtime_helper *helper =
		zend_native_runtime_helper_find(runtime,
			static_cast<zend_native_runtime_helper_id>(symbol->id));
	if (helper == nullptr || helper->effects != symbol->effects
			|| helper->address == nullptr) {
		return false;
	}
	*address = helper->address;
	return true;
}

extern "C" zend_result zend_tpde_compile_module(
	zend_native_target target,
	const zend_mir_view *module,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	return zend_tpde_compile_module_w07(
		target, module, nullptr, 0, nullptr, 0, UINT32_MAX, out_image, diag);
}

extern "C" zend_result zend_tpde_compile_module_bound(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	return zend_tpde_compile_module_w07(
		target, module, bindings, binding_count, nullptr, 0, UINT32_MAX,
		out_image, diag);
}

extern "C" zend_result zend_tpde_compile_module_w07(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	return zend_tpde_compile_module_w08(
		target, module, bindings, binding_count, nullptr, 0, effects,
		effect_count, frame_argument_count, nullptr, nullptr, out_image, diag);
}

extern "C" zend_result zend_tpde_compile_module_w08(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *user_bindings,
	uint32_t user_binding_count,
	const zend_native_internal_call_binding *internal_bindings,
	uint32_t internal_binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	return zend_tpde_compile_module_w08_with_runtime(
		target, module, user_bindings, user_binding_count,
		internal_bindings, internal_binding_count, effects, effect_count,
		frame_argument_count, source_op_array, source_ssa,
		zend_native_runtime_get(),
		out_image, diag);
}

extern "C" zend_result zend_tpde_compile_module_w08_with_runtime(
	zend_native_target target,
	const zend_mir_view *module,
	const zend_native_call_binding *user_bindings,
	uint32_t user_binding_count,
	const zend_native_internal_call_binding *internal_bindings,
	uint32_t internal_binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	const zend_native_runtime_api *runtime,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	const zend_native_component_member member{
		.module = module,
		.user_bindings = user_bindings,
		.user_binding_count = user_binding_count,
		.internal_bindings = internal_bindings,
		.internal_binding_count = internal_binding_count,
		.effects = effects,
		.effect_count = effect_count,
		.frame_argument_count = frame_argument_count,
		.source_op_array = source_op_array,
		.source_ssa = source_ssa,
	};
	return zend_tpde_compile_component_w14_with_runtime(
		target, &member, 1, runtime, out_image, diag);
}

extern "C" zend_result zend_tpde_compile_component_w14_with_runtime(
	zend_native_target target,
	const zend_native_component_member *members,
	uint32_t member_count,
	const zend_native_runtime_api *runtime,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	if (diag != nullptr) {
		std::memset(diag, 0, sizeof(*diag));
	}
	if (members == nullptr || member_count == 0
			|| !checked_count(member_count)
			|| out_image == nullptr || runtime == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"component members, runtime and out_image are required");
		return FAILURE;
	}
	for (uint32_t index = 0; index < member_count; ++index) {
		const zend_native_component_member &member = members[index];
		if (member.module == nullptr
				|| (member.user_binding_count != 0
					&& member.user_bindings == nullptr)
				|| (member.internal_binding_count != 0
					&& member.internal_bindings == nullptr)
				|| (member.effect_count != 0 && member.effects == nullptr)
				|| !checked_count(member.user_binding_count)
				|| !checked_count(member.internal_binding_count)
				|| !checked_count(member.effect_count)
				|| (member.frame_argument_count != UINT32_MAX
					&& !checked_count(member.frame_argument_count))) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"native component member is invalid");
			return FAILURE;
		}
	}
	*out_image = nullptr;
	if (target != ZEND_NATIVE_TARGET_DARWIN_ARM64
			&& target != ZEND_NATIVE_TARGET_LINUX_AMD64) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_TARGET,
			"only darwin-arm64-dev and linux-amd64-prod are supported");
		return FAILURE;
	}

	auto *plans = static_cast<zend_tpde_plan *>(
		std::calloc(member_count, sizeof(zend_tpde_plan)));
	auto *plan_refs = static_cast<const zend_tpde_plan **>(
		std::calloc(member_count, sizeof(zend_tpde_plan *)));
	if (plans == nullptr || plan_refs == nullptr) {
		std::free(plans);
		std::free(plan_refs);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate native component plans");
		return FAILURE;
	}
	uint32_t initialized = 0;
	for (; initialized < member_count; ++initialized) {
		const zend_native_component_member &member = members[initialized];
		if (!initialize_plan(
				member.module, runtime,
				member.user_bindings, member.user_binding_count,
				member.internal_bindings, member.internal_binding_count,
				member.effects, member.effect_count,
				member.frame_argument_count,
				member.source_op_array, member.source_ssa,
				&plans[initialized], diag)) {
			break;
		}
		plans[initialized].symbol_namespace = initialized;
		plan_refs[initialized] = &plans[initialized];
	}
	if (initialized != member_count) {
		for (uint32_t index = 0; index <= initialized; ++index) {
			destroy_plan(&plans[index]);
		}
		std::free(plan_refs);
		std::free(plans);
		return FAILURE;
	}
	zend_native_image *image = static_cast<zend_native_image *>(
		std::calloc(1, sizeof(*image)));
	if (image == nullptr) {
		for (uint32_t index = 0; index < member_count; ++index) {
			destroy_plan(&plans[index]);
		}
		std::free(plan_refs);
		std::free(plans);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate a native image");
		return FAILURE;
	}
	image->target = target;
	image->abi_version = NATIVE_IMAGE_ABI_VERSION;
	image->runtime_abi_version = runtime->abi_version;
	image->build_id = native_image_build_id(target);
	image->code_version = next_native_code_version.fetch_add(
		1, std::memory_order_relaxed);
	/* TPDE liveness and register allocation own temporaries; the reserved ABI
	 * pointer remains present for compatibility but no value-slot array is used. */
	image->slot_count = 0;
	image->argument_count = plans[0].argument_count;
	image->frame_variable_count = members[0].source_op_array != nullptr
		? static_cast<uint32_t>(members[0].source_op_array->last_var)
		: plans[0].argument_count;
	image->frame_temporary_count =
		members[0].source_op_array != nullptr
			? members[0].source_op_array->T : 0;
	image->component_entries = static_cast<zend_native_component_entry *>(
		std::calloc(member_count, sizeof(*image->component_entries)));
	if (image->component_entries == nullptr) {
		for (uint32_t index = 0; index < member_count; ++index) {
			destroy_plan(&plans[index]);
		}
		std::free(plan_refs);
		std::free(plans);
		zend_native_image_destroy(image);
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate native component entry metadata");
		return FAILURE;
	}
	image->component_entry_count = member_count;
	for (uint32_t index = 0; index < member_count; ++index) {
		image->component_entries[index].argument_count =
			plans[index].argument_count;
		image->component_entries[index].frame_variable_count =
			members[index].source_op_array != nullptr
				? static_cast<uint32_t>(
					members[index].source_op_array->last_var)
				: plans[index].argument_count;
		image->component_entries[index].frame_temporary_count =
			members[index].source_op_array != nullptr
				? members[index].source_op_array->T : 0;
	}
	bool symbols_ready = true;
	for (uint32_t index = 0; index < member_count; ++index) {
		const zend_native_image_metrics metrics =
			collect_plan_metrics(plans[index]);
		image->metrics.runtime_helper_sites += metrics.runtime_helper_sites;
		image->metrics.source_opline_decode_sites +=
			metrics.source_opline_decode_sites;
		image->metrics.guard_sites += metrics.guard_sites;
		image->metrics.slow_path_sites += metrics.slow_path_sites;
		image->metrics.direct_call_sites += metrics.direct_call_sites;
		image->metrics.direct_leaf_scalar_sites +=
			metrics.direct_leaf_scalar_sites;
		image->metrics.direct_call_frame_bytes +=
			metrics.direct_call_frame_bytes;
		if (!prepare_image_symbols(&plans[index], image, diag)) {
			symbols_ready = false;
			break;
		}
	}
	zend_result result = symbols_ready
		? target == ZEND_NATIVE_TARGET_DARWIN_ARM64
			? zend_tpde_emit_darwin_arm64(
				plan_refs, member_count, image, diag)
			: zend_tpde_emit_linux_x64(
				plan_refs, member_count, image, diag)
		: FAILURE;
	if (result == SUCCESS) {
		uint32_t direct_count = 0;
		uint32_t direct_internal_count = 0;
		uint32_t user_count = 0;
		for (uint32_t index = 0; index < member_count; ++index) {
			if (plans[index].direct_call_count > MAX_RECORDS - direct_count
					|| plans[index].direct_internal_call_count
						> MAX_RECORDS - direct_internal_count
					|| plans[index].user_call_count
						> MAX_RECORDS - user_count) {
				result = FAILURE;
				break;
			}
			direct_count += plans[index].direct_call_count;
			direct_internal_count += plans[index].direct_internal_call_count;
			user_count += plans[index].user_call_count;
		}
		if (result == SUCCESS) {
			image->direct_calls = direct_count == 0 ? nullptr
				: static_cast<zend_native_direct_call_descriptor **>(
					std::calloc(direct_count, sizeof(*image->direct_calls)));
			image->direct_internal_calls =
				direct_internal_count == 0 ? nullptr
				: static_cast<zend_native_direct_internal_call_descriptor **>(
					std::calloc(direct_internal_count,
						sizeof(*image->direct_internal_calls)));
			image->user_calls = user_count == 0 ? nullptr
				: static_cast<zend_native_user_call_descriptor **>(
					std::calloc(user_count, sizeof(*image->user_calls)));
			if ((direct_count != 0 && image->direct_calls == nullptr)
					|| (direct_internal_count != 0
						&& image->direct_internal_calls == nullptr)
					|| (user_count != 0 && image->user_calls == nullptr)) {
				result = FAILURE;
			}
		}
		if (result == SUCCESS) {
			for (uint32_t index = 0; index < member_count; ++index) {
				if (plans[index].direct_call_count != 0) {
					std::memcpy(
						image->direct_calls + image->direct_call_count,
						plans[index].direct_calls,
						static_cast<size_t>(plans[index].direct_call_count)
							* sizeof(*image->direct_calls));
				}
				image->direct_call_count += plans[index].direct_call_count;
				plans[index].direct_call_count = 0;
				if (plans[index].direct_internal_call_count != 0) {
					std::memcpy(
						image->direct_internal_calls
							+ image->direct_internal_call_count,
						plans[index].direct_internal_calls,
						static_cast<size_t>(
							plans[index].direct_internal_call_count)
							* sizeof(*image->direct_internal_calls));
				}
				image->direct_internal_call_count +=
					plans[index].direct_internal_call_count;
				plans[index].direct_internal_call_count = 0;
				if (plans[index].user_call_count != 0) {
					std::memcpy(
						image->user_calls + image->user_call_count,
						plans[index].user_calls,
						static_cast<size_t>(plans[index].user_call_count)
							* sizeof(*image->user_calls));
				}
				image->user_call_count += plans[index].user_call_count;
				plans[index].user_call_count = 0;
			}
		}
	}
	for (uint32_t index = 0; index < member_count; ++index) {
		destroy_plan(&plans[index]);
	}
	std::free(plan_refs);
	std::free(plans);
	if (result == FAILURE) {
		if (diag != nullptr && diag->code == ZEND_NATIVE_DIAGNOSTIC_OK) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to retain native component metadata");
		}
		zend_native_image_destroy(image);
		return FAILURE;
	}
	*out_image = image;
	return SUCCESS;
}

extern "C" zend_result zend_native_image_serialize(
	const zend_native_image *image,
	zend_native_image_encode_reference_t encode_reference,
	void *reference_context,
	unsigned char **out_bytes,
	size_t *out_size,
	zend_native_diagnostic *diag) {
	zend_native_byte_buffer buffer{};
	zend_native_serial_image_header header{};

	if (out_bytes == nullptr || out_size == nullptr || image == nullptr
			|| encode_reference == nullptr
			|| image->abi_version != NATIVE_IMAGE_ABI_VERSION
			|| image->build_id != native_image_build_id(image->target)
			|| image->text_size > MAX_NATIVE_IMAGE_BYTES
			|| !checked_count(image->symbol_count)
			|| !checked_count(image->symbol_binding_count)
			|| image->component_entry_count == 0
			|| !checked_count(image->component_entry_count)
			|| image->component_entries == nullptr
			|| !checked_count(image->frame_variable_count)
			|| !checked_count(image->frame_temporary_count)
			|| image->frame_variable_count < image->argument_count
			|| image->frame_temporary_count
				> MAX_RECORDS - image->frame_variable_count) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native image cannot be serialized");
		return FAILURE;
	}
	*out_bytes = nullptr;
	*out_size = 0;
	for (uint32_t index = 0;
			index < image->component_entry_count; ++index) {
		const zend_native_component_entry &entry =
			image->component_entries[index];
		if (!checked_count(entry.frame_variable_count)
				|| !checked_count(entry.frame_temporary_count)
				|| entry.frame_variable_count < entry.argument_count
				|| entry.frame_temporary_count
					> MAX_RECORDS - entry.frame_variable_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"native component metadata cannot be serialized");
			return FAILURE;
		}
	}
	header.magic = NATIVE_IMAGE_SERIAL_MAGIC;
	header.format = NATIVE_IMAGE_SERIAL_FORMAT;
	header.target = static_cast<uint32_t>(image->target);
	header.image_abi = image->abi_version;
	header.runtime_abi = image->runtime_abi_version;
	header.build_id = image->build_id;
	header.code_version = image->code_version;
	header.slot_count = image->slot_count;
	header.argument_count = image->argument_count;
	header.frame_variable_count = image->frame_variable_count;
	header.frame_temporary_count = image->frame_temporary_count;
	header.metrics = image->metrics;
	header.text_size = image->text_size;
	header.symbol_count = image->symbol_count;
	header.binding_count = image->symbol_binding_count;
	header.component_count = image->component_entry_count;
	if (!native_buffer_append(&buffer, &header, sizeof(header))
			|| !native_buffer_append(
				&buffer, image->text, image->text_size)
			|| !native_buffer_append(
				&buffer, image->symbols,
				static_cast<size_t>(image->symbol_count)
					* sizeof(*image->symbols))
			|| !native_buffer_append(
				&buffer, image->component_entries,
				static_cast<size_t>(image->component_entry_count)
					* sizeof(*image->component_entries))) {
		goto allocation_failure;
	}
	for (uint32_t symbol_index = 0;
			symbol_index < image->symbol_count; ++symbol_index) {
		const zend_native_image_symbol &symbol =
			image->symbols[symbol_index];
		if (symbol.kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER) {
			continue;
		}
		const zend_native_image_symbol_binding *binding =
			native_image_binding(image, symbol_index);
		if (binding == nullptr || binding->address == nullptr) {
			goto invalid_image;
		}
		zend_native_serial_binding serialized{};
		serialized.symbol_index = symbol_index;
		const void *payload = nullptr;
		size_t payload_size = 0;
		const zend_native_internal_call_cell *internal_cell;

		switch (symbol.kind) {
			case ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL:
				if (!encode_reference(
						reference_context,
						ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL,
						binding->address,
						&serialized.primary_reference)
						|| serialized.primary_reference == 0) {
					goto invalid_image;
				}
				break;
			case ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL:
				internal_cell =
					static_cast<const zend_native_internal_call_cell *>(
						binding->address);
				if (internal_cell->function == nullptr
						|| !encode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
							internal_cell->function,
							&serialized.primary_reference)
						|| serialized.primary_reference == 0
						|| (internal_cell->called_scope != nullptr
							&& (!encode_reference(
								reference_context,
								ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
								internal_cell->called_scope,
								&serialized.scope_reference)
								|| serialized.scope_reference == 0))) {
					goto invalid_image;
				}
				serialized.receiver_kind =
					static_cast<uint32_t>(internal_cell->receiver_kind);
				break;
			case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR: {
				const auto *descriptor =
					static_cast<const zend_native_direct_call_descriptor *>(
						binding->address);
				if (!native_descriptor_size(
						symbol.kind, descriptor, &payload_size)
						|| payload_size > UINT32_MAX
						|| descriptor->expected_function == nullptr
						|| !encode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
							descriptor->expected_function,
							&serialized.primary_reference)
						|| serialized.primary_reference == 0
						|| (descriptor->called_scope != nullptr
							&& (!encode_reference(
								reference_context,
								ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
								descriptor->called_scope,
								&serialized.scope_reference)
								|| serialized.scope_reference == 0))) {
					goto invalid_image;
				}
				unsigned char *copy =
					static_cast<unsigned char *>(std::malloc(payload_size));
				if (copy == nullptr) {
					goto allocation_failure;
				}
				std::memcpy(copy, descriptor, payload_size);
				auto *copy_descriptor =
					reinterpret_cast<zend_native_direct_call_descriptor *>(
						copy);
				copy_descriptor->expected_function = nullptr;
				copy_descriptor->called_scope = nullptr;
				payload = copy;
				break;
			}
			case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR:
			case ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR:
				if (!native_descriptor_size(
						symbol.kind, binding->address, &payload_size)
						|| payload_size > UINT32_MAX) {
					goto invalid_image;
				}
				payload = binding->address;
				break;
			default:
				goto invalid_image;
		}
		serialized.payload_size = static_cast<uint32_t>(payload_size);
		if (!native_buffer_append(
				&buffer, &serialized, sizeof(serialized))
				|| !native_buffer_append(
					&buffer, payload, payload_size)) {
			if (symbol.kind
					== ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR) {
				std::free(const_cast<void *>(payload));
			}
			goto allocation_failure;
		}
		if (symbol.kind
				== ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR) {
			std::free(const_cast<void *>(payload));
		}
	}
	if (buffer.size > MAX_NATIVE_IMAGE_BYTES) {
		goto invalid_image;
	}
	header.total_size = buffer.size;
	std::memcpy(buffer.bytes, &header, sizeof(header));
	header.checksum = native_serial_checksum(buffer.bytes, buffer.size);
	std::memcpy(buffer.bytes, &header, sizeof(header));
	*out_bytes = buffer.bytes;
	*out_size = buffer.size;
	return SUCCESS;

invalid_image:
	std::free(buffer.bytes);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"native image contains a non-persistent binding");
	return FAILURE;
allocation_failure:
	std::free(buffer.bytes);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
		"unable to serialize native image");
	return FAILURE;
}

extern "C" zend_result zend_native_image_deserialize(
	const unsigned char *bytes,
	size_t size,
	zend_native_image_decode_reference_t decode_reference,
	void *reference_context,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	zend_native_serial_image_header header;
	zend_native_image *image = nullptr;
	size_t offset;
	size_t symbol_bytes = 0;
	size_t component_bytes = 0;
	bool *bound = nullptr;

	if (bytes == nullptr || out_image == nullptr
			|| decode_reference == nullptr
			|| size < sizeof(header) || size > MAX_NATIVE_IMAGE_BYTES) {
		goto invalid_image;
	}
	*out_image = nullptr;
	std::memcpy(&header, bytes, sizeof(header));
	if (header.magic != NATIVE_IMAGE_SERIAL_MAGIC
			|| header.format != NATIVE_IMAGE_SERIAL_FORMAT
			|| header.target > ZEND_NATIVE_TARGET_LINUX_AMD64
			|| header.image_abi != NATIVE_IMAGE_ABI_VERSION
			|| header.runtime_abi != ZEND_NATIVE_RUNTIME_ABI_VERSION
			|| header.build_id != native_image_build_id(
				static_cast<zend_native_target>(header.target))
			|| header.code_version == 0 || header.total_size != size
			|| header.checksum != native_serial_checksum(bytes, size)
			|| header.text_size > size
			|| !checked_count(header.symbol_count)
			|| !checked_count(header.binding_count)
			|| header.binding_count > header.symbol_count
			|| header.component_count == 0
			|| !checked_count(header.component_count)
			|| !checked_count(header.frame_variable_count)
			|| !checked_count(header.frame_temporary_count)
			|| header.frame_variable_count < header.argument_count
			|| header.frame_temporary_count
				> MAX_RECORDS - header.frame_variable_count) {
		goto invalid_image;
	}
	offset = sizeof(header);
	if (header.text_size > size - offset) {
		goto invalid_image;
	}
	symbol_bytes =
		static_cast<size_t>(header.symbol_count)
			* sizeof(zend_native_image_symbol);
	if (symbol_bytes > size - offset - header.text_size) {
		goto invalid_image;
	}
	component_bytes =
		static_cast<size_t>(header.component_count)
			* sizeof(zend_native_component_entry);
	if (component_bytes
			> size - offset - header.text_size - symbol_bytes) {
		goto invalid_image;
	}
	image = static_cast<zend_native_image *>(
		std::calloc(1, sizeof(*image)));
	if (image == nullptr) {
		goto allocation_failure;
	}
	image->target = static_cast<zend_native_target>(header.target);
	image->abi_version = header.image_abi;
	image->runtime_abi_version = header.runtime_abi;
	image->build_id = header.build_id;
	image->code_version = header.code_version;
	image->slot_count = header.slot_count;
	image->argument_count = header.argument_count;
	image->frame_variable_count = header.frame_variable_count;
	image->frame_temporary_count = header.frame_temporary_count;
	image->metrics = header.metrics;
	image->text_size = header.text_size;
	image->text_capacity = header.text_size;
	if (header.text_size != 0) {
		image->text = static_cast<unsigned char *>(
			std::malloc(header.text_size));
		if (image->text == nullptr) {
			goto allocation_failure;
		}
		std::memcpy(image->text, bytes + offset, header.text_size);
	}
	offset += header.text_size;
	image->symbol_count = header.symbol_count;
	image->symbol_capacity = header.symbol_count;
	if (symbol_bytes != 0) {
		image->symbols = static_cast<zend_native_image_symbol *>(
			std::malloc(symbol_bytes));
		bound = static_cast<bool *>(
			std::calloc(header.symbol_count, sizeof(bool)));
		if (image->symbols == nullptr || bound == nullptr) {
			goto allocation_failure;
		}
		std::memcpy(image->symbols, bytes + offset, symbol_bytes);
	}
	offset += symbol_bytes;
	image->component_entries = static_cast<zend_native_component_entry *>(
		std::malloc(component_bytes));
	if (image->component_entries == nullptr) {
		goto allocation_failure;
	}
	std::memcpy(
		image->component_entries, bytes + offset, component_bytes);
	image->component_entry_count = header.component_count;
	offset += component_bytes;
	for (uint32_t index = 0;
			index < image->component_entry_count; ++index) {
		const zend_native_component_entry &entry =
			image->component_entries[index];
		if (!checked_count(entry.frame_variable_count)
				|| !checked_count(entry.frame_temporary_count)
				|| entry.frame_variable_count < entry.argument_count
				|| entry.frame_temporary_count
					> MAX_RECORDS - entry.frame_variable_count) {
			goto invalid_image;
		}
	}
	if (header.binding_count != 0) {
		image->symbol_bindings =
			static_cast<zend_native_image_symbol_binding *>(
				std::calloc(header.binding_count,
					sizeof(*image->symbol_bindings)));
		if (image->symbol_bindings == nullptr) {
			goto allocation_failure;
		}
		image->symbol_binding_capacity = header.binding_count;
	}
	for (uint32_t binding_index = 0;
			binding_index < header.binding_count; ++binding_index) {
		zend_native_serial_binding serialized;
		if (sizeof(serialized) > size - offset) {
			goto invalid_image;
		}
		std::memcpy(&serialized, bytes + offset, sizeof(serialized));
		offset += sizeof(serialized);
		if (serialized.symbol_index >= image->symbol_count
				|| bound[serialized.symbol_index]
				|| serialized.payload_size > size - offset) {
			goto invalid_image;
		}
		const zend_native_image_symbol &symbol =
			image->symbols[serialized.symbol_index];
		const void *address = nullptr;
		switch (symbol.kind) {
			case ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL:
				if (serialized.payload_size != 0
						|| serialized.primary_reference == 0
						|| !decode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL,
							serialized.primary_reference, &address)
						|| address == nullptr) {
					goto invalid_image;
				}
				break;
			case ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL: {
				if (serialized.payload_size != 0
						|| serialized.primary_reference == 0
						|| serialized.receiver_kind
							> ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
					goto invalid_image;
				}
				auto *cell =
					static_cast<zend_native_internal_call_cell *>(
						std::calloc(
							1, sizeof(zend_native_internal_call_cell)));
				const void *resolved_function = nullptr;
				const void *resolved_scope = nullptr;
				if (cell == nullptr
						|| !decode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
							serialized.primary_reference,
							&resolved_function)
						|| resolved_function == nullptr
						|| (serialized.scope_reference != 0
							&& !decode_reference(
								reference_context,
								ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
								serialized.scope_reference,
								&resolved_scope))) {
					std::free(cell);
					goto invalid_image;
				}
				cell->function = const_cast<zend_function *>(
					static_cast<const zend_function *>(resolved_function));
				cell->called_scope = const_cast<zend_class_entry *>(
					static_cast<const zend_class_entry *>(resolved_scope));
				cell->receiver_kind =
					static_cast<zend_native_internal_receiver_kind>(
						serialized.receiver_kind);
				void *resized = std::realloc(
					image->owned_internal_call_cells,
					static_cast<size_t>(
						image->owned_internal_call_cell_count + 1)
						* sizeof(*image->owned_internal_call_cells));
				if (resized == nullptr) {
					std::free(cell);
					goto allocation_failure;
				}
				image->owned_internal_call_cells =
					static_cast<zend_native_internal_call_cell **>(resized);
				image->owned_internal_call_cells[
					image->owned_internal_call_cell_count++] = cell;
				address = cell;
				break;
			}
			case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR: {
				if (serialized.primary_reference == 0) {
					goto invalid_image;
				}
				auto *descriptor =
					static_cast<zend_native_direct_call_descriptor *>(
						std::malloc(serialized.payload_size));
				if (descriptor == nullptr) {
					goto allocation_failure;
				}
				std::memcpy(
					descriptor, bytes + offset, serialized.payload_size);
				size_t expected_size;
				const void *resolved_function = nullptr;
				const void *resolved_scope = nullptr;
				if (!native_descriptor_size(
						symbol.kind, descriptor, &expected_size)
						|| expected_size != serialized.payload_size
						|| descriptor->expected_function != nullptr
						|| descriptor->called_scope != nullptr
						|| !decode_reference(
							reference_context,
							ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION,
							serialized.primary_reference,
							&resolved_function)
						|| resolved_function == nullptr
						|| (serialized.scope_reference != 0
							&& !decode_reference(
								reference_context,
								ZEND_NATIVE_IMAGE_REFERENCE_CLASS,
								serialized.scope_reference,
								&resolved_scope))) {
					std::free(descriptor);
					goto invalid_image;
				}
				descriptor->expected_function =
					const_cast<zend_function *>(
						static_cast<const zend_function *>(
							resolved_function));
				descriptor->called_scope =
					const_cast<zend_class_entry *>(
						static_cast<const zend_class_entry *>(
							resolved_scope));
				void *resized = std::realloc(
					image->direct_calls,
					static_cast<size_t>(image->direct_call_count + 1)
						* sizeof(*image->direct_calls));
				if (resized == nullptr) {
					std::free(descriptor);
					goto allocation_failure;
				}
				image->direct_calls =
					static_cast<zend_native_direct_call_descriptor **>(
						resized);
				image->direct_calls[image->direct_call_count++] = descriptor;
				address = descriptor;
				break;
			}
			case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR:
			case ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR: {
				void *descriptor = std::malloc(serialized.payload_size);
				if (descriptor == nullptr) {
					goto allocation_failure;
				}
				std::memcpy(
					descriptor, bytes + offset, serialized.payload_size);
				size_t expected_size;
				if (!native_descriptor_size(
						symbol.kind, descriptor, &expected_size)
						|| expected_size != serialized.payload_size) {
					std::free(descriptor);
					goto invalid_image;
				}
				if (symbol.kind
						== ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR) {
					void *resized = std::realloc(
						image->direct_internal_calls,
						static_cast<size_t>(
							image->direct_internal_call_count + 1)
							* sizeof(*image->direct_internal_calls));
					if (resized == nullptr) {
						std::free(descriptor);
						goto allocation_failure;
					}
					image->direct_internal_calls =
						static_cast<
							zend_native_direct_internal_call_descriptor **>(
								resized);
					image->direct_internal_calls[
						image->direct_internal_call_count++] =
							static_cast<
								zend_native_direct_internal_call_descriptor *>(
									descriptor);
				} else {
					void *resized = std::realloc(
						image->user_calls,
						static_cast<size_t>(image->user_call_count + 1)
							* sizeof(*image->user_calls));
					if (resized == nullptr) {
						std::free(descriptor);
						goto allocation_failure;
					}
					image->user_calls =
						static_cast<zend_native_user_call_descriptor **>(
							resized);
					image->user_calls[image->user_call_count++] =
						static_cast<zend_native_user_call_descriptor *>(
							descriptor);
				}
				address = descriptor;
				break;
			}
			default:
				goto invalid_image;
		}
		offset += serialized.payload_size;
		image->symbol_bindings[image->symbol_binding_count++] = {
			serialized.symbol_index, address};
		bound[serialized.symbol_index] = true;
	}
	if (offset != size) {
		goto invalid_image;
	}
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		const zend_native_image_symbol &symbol = image->symbols[index];
		if (std::memchr(symbol.name, '\0', sizeof(symbol.name)) == nullptr
				|| (symbol.kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER
					? bound[index]
					: !bound[index])) {
			goto invalid_image;
		}
	}
	std::free(bound);
	*out_image = image;
	return SUCCESS;

invalid_image:
	std::free(bound);
	zend_native_image_destroy(image);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"serialized native image is incompatible or malformed");
	return FAILURE;
allocation_failure:
	std::free(bound);
	zend_native_image_destroy(image);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
		"unable to restore serialized native image");
	return FAILURE;
}

extern "C" void zend_native_serialized_image_destroy(
	unsigned char *bytes) {
	std::free(bytes);
}

extern "C" zend_result zend_native_publish_image(
	zend_native_target target,
	zend_native_image *image,
	zend_native_code **out_code,
	zend_native_diagnostic *diag) {
	if (image == nullptr || out_code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"image and out_code are required");
		return FAILURE;
	}
	*out_code = nullptr;
	if (target != image->target) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
			"publish target differs from compiled image target");
		return FAILURE;
	}
	zend_result result = target == ZEND_NATIVE_TARGET_DARWIN_ARM64
		? zend_native_publish_darwin_arm64(image, out_code, diag)
		: target == ZEND_NATIVE_TARGET_LINUX_AMD64
			? zend_native_publish_linux_x64(image, out_code, diag)
			: FAILURE;
	if (result == SUCCESS && *out_code != nullptr
			&& (*out_code)->unwind_registered) {
		live_unwind_registrations.fetch_add(1, std::memory_order_relaxed);
	}
	if (result == SUCCESS && *out_code != nullptr) {
		(*out_code)->owner = *out_code;
		(*out_code)->owner_refcount = 1;
		(*out_code)->component_metadata = image->component_entries;
		(*out_code)->component_entry_count = image->component_entry_count;
		image->component_entries = nullptr;
		image->component_entry_count = 0;
		/*
		 * Direct-call descriptors are resolved into process-local relocation
		 * slots at publication. Transfer their storage with the mapping so
		 * callers may destroy the intermediate image immediately afterward.
		 */
		(*out_code)->direct_calls = image->direct_calls;
		(*out_code)->direct_call_count = image->direct_call_count;
		image->direct_calls = nullptr;
		image->direct_call_count = 0;
		(*out_code)->direct_internal_calls = image->direct_internal_calls;
		(*out_code)->direct_internal_call_count =
			image->direct_internal_call_count;
		image->direct_internal_calls = nullptr;
		image->direct_internal_call_count = 0;
		(*out_code)->user_calls = image->user_calls;
		(*out_code)->user_call_count = image->user_call_count;
		image->user_calls = nullptr;
		image->user_call_count = 0;
		(*out_code)->owned_internal_call_cells =
			image->owned_internal_call_cells;
		(*out_code)->owned_internal_call_cell_count =
			image->owned_internal_call_cell_count;
		image->owned_internal_call_cells = nullptr;
		image->owned_internal_call_cell_count = 0;
	}
	return result;
}

extern "C" zend_result zend_native_execute(
	const zend_native_code *code,
	const zend_native_scalar *arguments,
	uint32_t argument_count,
	zend_native_scalar *result,
	zend_native_diagnostic *diag) {
	if (code == nullptr || result == nullptr || !code->executable
			|| (argument_count != 0 && arguments == nullptr)
			|| argument_count != code->argument_count) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native execution arguments do not match the compiled entry");
		return FAILURE;
	}
	for (uint32_t i = 0; i < argument_count; ++i) {
		if (arguments[i].kind > ZEND_NATIVE_SCALAR_DOUBLE) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"scalar execution argument kind is invalid");
			return FAILURE;
		}
	}

	zend_op_array op_array{};
	op_array.type = ZEND_USER_FUNCTION;
	op_array.num_args = argument_count;
	op_array.required_num_args = argument_count;
	op_array.last_var = code->frame_variable_count;
	op_array.T = code->frame_temporary_count;
	op_array.last = argument_count;
	op_array.opcodes = static_cast<zend_op *>(std::calloc(
		static_cast<size_t>(argument_count) + 1, sizeof(zend_op)));
	if (op_array.opcodes == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate scalar execution receive opcodes");
		return FAILURE;
	}
	if (argument_count != 0) {
		op_array.arg_info = static_cast<zend_arg_info *>(
			std::calloc(argument_count, sizeof(zend_arg_info)));
		if (op_array.arg_info == nullptr) {
			std::free(op_array.opcodes);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to allocate scalar execution argument metadata");
			return FAILURE;
		}
	}
	for (uint32_t i = 0; i < argument_count; ++i) {
		op_array.opcodes[i].opcode = ZEND_RECV;
		op_array.opcodes[i].op1.num = i + 1;
	}

	zend_execute_data *previous = EG(current_execute_data);
	zend_execute_data *frame = zend_vm_stack_push_call_frame(
		ZEND_CALL_NESTED_FUNCTION, reinterpret_cast<zend_function *>(&op_array),
		argument_count, nullptr);
	zval return_value;
	ZVAL_UNDEF(&return_value);
	for (uint32_t i = 0; i < argument_count; ++i) {
		zval *argument = ZEND_CALL_ARG(frame, i + 1);
		switch (arguments[i].kind) {
			case ZEND_NATIVE_SCALAR_NULL:
				ZVAL_NULL(argument);
				break;
			case ZEND_NATIVE_SCALAR_BOOL:
				ZVAL_BOOL(argument, arguments[i].payload_bits != 0);
				break;
			case ZEND_NATIVE_SCALAR_LONG:
				ZVAL_LONG(argument, static_cast<zend_long>(arguments[i].payload_bits));
				break;
			case ZEND_NATIVE_SCALAR_DOUBLE: {
				double value;
				std::memcpy(&value, &arguments[i].payload_bits, sizeof(value));
				ZVAL_DOUBLE(argument, value);
				break;
			}
			default:
				ZEND_UNREACHABLE();
		}
	}
	zend_init_func_execute_data(frame, &op_array, &return_value);

	EG(current_execute_data) = frame;
	zend_native_status status = zend_native_execute_frame(code, frame, diag);
	EG(current_execute_data) = previous;
	std::memset(result, 0, sizeof(*result));
	if (status == ZEND_NATIVE_RETURNED) {
		switch (Z_TYPE(return_value)) {
			case IS_NULL:
				result->kind = ZEND_NATIVE_SCALAR_NULL;
				break;
			case IS_FALSE:
			case IS_TRUE:
				result->kind = ZEND_NATIVE_SCALAR_BOOL;
				result->payload_bits = Z_TYPE(return_value) == IS_TRUE;
				break;
			case IS_LONG:
				result->kind = ZEND_NATIVE_SCALAR_LONG;
				result->payload_bits = static_cast<uint64_t>(Z_LVAL(return_value));
				break;
			case IS_DOUBLE:
				result->kind = ZEND_NATIVE_SCALAR_DOUBLE;
				std::memcpy(
					&result->payload_bits, &Z_DVAL(return_value),
					sizeof(result->payload_bits));
				break;
			default:
				status = ZEND_NATIVE_EXCEPTION;
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"native scalar execution returned a non-scalar value");
				break;
		}
	}
	if (!Z_ISUNDEF(return_value)) {
		zval_ptr_dtor(&return_value);
	}
	zend_vm_stack_free_call_frame(frame);
	std::free(op_array.arg_info);
	std::free(op_array.opcodes);
	return status == ZEND_NATIVE_RETURNED ? SUCCESS : FAILURE;
}

extern "C" void zend_native_image_destroy(zend_native_image *image) {
	if (image != nullptr) {
		for (uint32_t index = 0; index < image->direct_call_count; ++index) {
			std::free(image->direct_calls[index]);
		}
		std::free(image->direct_calls);
		for (uint32_t index = 0;
				index < image->direct_internal_call_count; ++index) {
			std::free(image->direct_internal_calls[index]);
		}
		std::free(image->direct_internal_calls);
		for (uint32_t index = 0; index < image->user_call_count; ++index) {
			std::free(image->user_calls[index]);
		}
		std::free(image->user_calls);
		for (uint32_t index = 0;
				index < image->owned_internal_call_cell_count; ++index) {
			std::free(image->owned_internal_call_cells[index]);
		}
		std::free(image->owned_internal_call_cells);
		std::free(image->component_entries);
		if (image->destroy_target_state != nullptr) {
			image->destroy_target_state(image->target_state);
		}
		std::free(image->symbol_bindings);
		std::free(image->symbols);
		std::free(image->text);
		std::free(image);
	}
}

extern "C" void zend_native_code_destroy(zend_native_code *code) {
	if (code == nullptr) {
		return;
	}
	zend_native_code *owner = code->owner != nullptr ? code->owner : code;
	const uint32_t previous = __atomic_fetch_sub(
		&owner->owner_refcount, 1, __ATOMIC_ACQ_REL);
	ZEND_ASSERT(previous != 0);
	if (code != owner) {
		std::free(code);
	}
	if (previous != 1) {
		return;
	}
	for (uint32_t index = 0; index < owner->direct_call_count; ++index) {
		std::free(owner->direct_calls[index]);
	}
	std::free(owner->direct_calls);
	for (uint32_t index = 0;
			index < owner->direct_internal_call_count; ++index) {
		std::free(owner->direct_internal_calls[index]);
	}
	std::free(owner->direct_internal_calls);
	for (uint32_t index = 0; index < owner->user_call_count; ++index) {
		std::free(owner->user_calls[index]);
	}
	std::free(owner->user_calls);
	for (uint32_t index = 0;
			index < owner->owned_internal_call_cell_count; ++index) {
		std::free(owner->owned_internal_call_cells[index]);
	}
	std::free(owner->owned_internal_call_cells);
	std::free(owner->component_entries);
	std::free(owner->component_metadata);
	if (owner->unwind_registered) {
		uint32_t unwind_previous = live_unwind_registrations.fetch_sub(
			1, std::memory_order_relaxed);
		ZEND_ASSERT(unwind_previous != 0);
		owner->unwind_registered = false;
	}
	if (owner->target == ZEND_NATIVE_TARGET_DARWIN_ARM64) {
		zend_native_unmap_darwin_arm64(owner);
	} else if (owner->target == ZEND_NATIVE_TARGET_LINUX_AMD64) {
		zend_native_unmap_linux_x64(owner);
	}
	std::free(owner);
}

extern "C" zend_result zend_native_code_component_view(
	zend_native_code *code,
	uint32_t component_index,
	zend_native_code **out_view,
	zend_native_diagnostic *diag) {
	if (code == nullptr || out_view == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native code and output view are required");
		return FAILURE;
	}
	zend_native_code *owner = code->owner != nullptr ? code->owner : code;
	if (component_index >= owner->component_entry_count
			|| owner->component_entries == nullptr
			|| owner->component_metadata == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native component entry index is outside the published image");
		return FAILURE;
	}
	if (component_index == 0 && code == owner) {
		*out_view = code;
		return SUCCESS;
	}
	zend_native_code *view = static_cast<zend_native_code *>(
		std::calloc(1, sizeof(*view)));
	if (view == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate native component code view");
		return FAILURE;
	}
	view->target = owner->target;
	view->owner = owner;
	view->mapping = owner->mapping;
	view->mapping_size = owner->mapping_size;
	view->entry = owner->component_entries[component_index];
	view->slot_count = owner->slot_count;
	view->argument_count =
		owner->component_metadata[component_index].argument_count;
	view->frame_variable_count =
		owner->component_metadata[component_index].frame_variable_count;
	view->frame_temporary_count =
		owner->component_metadata[component_index].frame_temporary_count;
	view->writable = owner->writable;
	view->executable = owner->executable;
	__atomic_fetch_add(&owner->owner_refcount, 1, __ATOMIC_RELAXED);
	*out_view = view;
	return SUCCESS;
}

extern "C" const char *zend_native_target_id(zend_native_target target) {
	return target == ZEND_NATIVE_TARGET_DARWIN_ARM64 ? "darwin-arm64-dev"
		: target == ZEND_NATIVE_TARGET_LINUX_AMD64 ? "linux-amd64-prod" : "invalid";
}
extern "C" const char *zend_native_target_triple(zend_native_target target) {
	return target == ZEND_NATIVE_TARGET_DARWIN_ARM64 ? "arm64-apple-darwin"
		: target == ZEND_NATIVE_TARGET_LINUX_AMD64 ? "x86_64-unknown-linux-gnu" : "invalid";
}
extern "C" size_t zend_native_image_size(const zend_native_image *image) {
	return image == nullptr ? 0 : image->text_size;
}
extern "C" const unsigned char *zend_native_image_bytes(const zend_native_image *image) {
	return image == nullptr ? nullptr : image->text;
}
extern "C" uint32_t zend_native_image_component_count(
		const zend_native_image *image) {
	return image == nullptr ? 0 : image->component_entry_count;
}
extern "C" void zend_native_image_get_metrics(
		const zend_native_image *image, zend_native_image_metrics *metrics) {
	if (metrics == nullptr) {
		return;
	}
	*metrics = image != nullptr
		? image->metrics : zend_native_image_metrics{};
}
extern "C" bool zend_native_code_is_writable(const zend_native_code *code) {
	return code != nullptr && code->writable;
}
extern "C" bool zend_native_code_is_executable(const zend_native_code *code) {
	return code != nullptr && code->executable;
}

extern "C" bool zend_native_code_has_unwind_info(const zend_native_code *code) {
	return code != nullptr && code->unwind_registered;
}

extern "C" uint32_t zend_native_live_unwind_registration_count(void) {
	return live_unwind_registrations.load(std::memory_order_relaxed);
}

extern "C" bool zend_native_code_contains_address(
		const zend_native_code *code, const void *address) {
	if (code == nullptr || code->mapping == nullptr || address == nullptr) {
		return false;
	}
	const auto begin = reinterpret_cast<uintptr_t>(code->mapping);
	const auto candidate = reinterpret_cast<uintptr_t>(address);
	return candidate >= begin && candidate - begin < code->mapping_size;
}

extern "C" zend_native_frame_entry_t zend_native_code_frame_entry(
	const zend_native_code *code) {
	return code != nullptr ? code->entry : nullptr;
}

extern "C" uint32_t zend_native_code_argument_count(
	const zend_native_code *code) {
	return code != nullptr ? code->argument_count : 0;
}
