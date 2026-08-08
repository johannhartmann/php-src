// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_system_id.h"
#include "Zend/zend_type_info.h"
#include "Zend/Optimizer/zend_ssa.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t MAX_RECORDS = UINT32_C(1) << 20;
constexpr size_t MAX_NATIVE_IMAGE_BYTES = size_t{1} << 28;
constexpr uint32_t NATIVE_IMAGE_ABI_VERSION = 6;
constexpr uint32_t NATIVE_IMAGE_SERIAL_FORMAT = 4;
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
	uint32_t trailing_word_count = 0;
	size_t base_size;
	size_t argument_size;
	switch (kind) {
		case ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR: {
			const auto *descriptor =
				static_cast<const zend_native_direct_call_descriptor *>(
					address);
			argument_count = descriptor->argument_count;
			trailing_word_count = descriptor->default_literal_count;
			base_size = offsetof(
				zend_native_direct_call_descriptor, arguments);
			argument_size = sizeof(zend_native_direct_call_argument);
			break;
		}
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
	const size_t arguments_size =
		static_cast<size_t>(argument_count) * argument_size;
	if (!checked_count(trailing_word_count)
			|| trailing_word_count
				> (MAX_NATIVE_IMAGE_BYTES - base_size - arguments_size)
					/ sizeof(uint32_t)) {
		return false;
	}
	*size = base_size + arguments_size
		+ static_cast<size_t>(trailing_word_count) * sizeof(uint32_t);
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
		return {
			scalar == ZEND_MIR_SCALAR_TYPE_I1
				? ZEND_MIR_REPRESENTATION_I1
			: scalar == ZEND_MIR_SCALAR_TYPE_F64
				? ZEND_MIR_REPRESENTATION_DOUBLE
				: ZEND_MIR_REPRESENTATION_I64,
			scalar,
			scalar == ZEND_MIR_SCALAR_TYPE_I1
				? ZEND_TPDE_MACHINE_VALUE_BOOL
			: scalar == ZEND_MIR_SCALAR_TYPE_F64
				? ZEND_TPDE_MACHINE_VALUE_F64
				: ZEND_TPDE_MACHINE_VALUE_I64,
			ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE,
			true,
		};
	}

	const uint32_t value_types =
		ZEND_TYPE_FULL_MASK(*type) & MAY_BE_ANY;
	const bool exact_builtin_type = ZEND_TYPE_IS_ONLY_MASK(*type);
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

bool zend_tpde_apply_machine_value_facts(
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
	return true;
}

static void zend_tpde_refine_non_alias_scalar_values(
	zend_tpde_plan *plan,
	const zend_ssa *source_ssa,
	const std::vector<uint8_t> &register_definitions)
{
	if (plan == nullptr || source_ssa == nullptr || source_ssa->vars == nullptr
			|| register_definitions.size() != plan->value_count) {
		return;
	}
	for (uint32_t ssa_variable = 0;
			ssa_variable < static_cast<uint32_t>(source_ssa->vars_count);
			++ssa_variable) {
		if (source_ssa->vars[ssa_variable].alias != NO_ALIAS) {
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
		 * The generic value lowering conservatively preserves MAY_BE_REF
		 * after variable sends.  Zend SSA's alias class is the stronger
		 * proof for this concrete SSA definition: NO_ALIAS means the exact
		 * scalar is not an observable zend_reference and can remain the
		 * register-authoritative value across calls, copies and PHIs.
		 */
		value.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
		value.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
		value.canonical_alias_observable = false;
		(void) zend_tpde_apply_machine_value_facts(
			&value,
			register_definitions[static_cast<uint32_t>(value_index)] != 0);
	}
}

static void zend_tpde_refine_literal_assignment_values(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa)
{
	if (plan == nullptr || source_op_array == nullptr || source_ssa == nullptr
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
		 * Zend's SEND_VAR_EX analysis deliberately keeps the destination
		 * aliasable until the eventual callee declaration is known.  A
		 * literal assignment into a non-reference predecessor is stronger:
		 * this SSA definition is an exact string or array payload even when
		 * a later by-value use made the generic SSA type mask conservative.
		 */
		if (source_ssa->vars[ssa_variable].alias != NO_ALIAS) {
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

static void zend_tpde_refine_boxed_scalar_copies(zend_tpde_plan *plan)
{
	uint32_t pass = 0;
	bool changed;

	if (plan == nullptr || plan->values == nullptr
			|| plan->instructions == nullptr) {
		return;
	}
	do {
		changed = false;
		for (uint32_t index = 0; index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if (record.opcode != ZEND_MIR_OPCODE_COPY
					|| record.representation != ZEND_MIR_REPRESENTATION_ZVAL
					|| instruction.operand_count != 1) {
				continue;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			const int32_t source_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, 0));
			if (result_index < 0 || source_index < 0) {
				continue;
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
				continue;
			}
			/*
			 * A COPY into a boxed PHI component preserves the source payload
			 * type even though its canonical MIR representation remains ZVAL.
			 * Freeze that semantic fact in the machine plan after MIR
			 * verification. The boxed value stays slot-authoritative; the
			 * adjacent ZVAL_STORE consumes the transient scalar definition.
			 */
			result.exact_type = source.exact_type;
			result.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
			result.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
			changed = true;
		}
		pass++;
	} while (changed && pass < plan->value_count);
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

bool source_call_argument_may_be_undefined(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_call_argument_ref &argument) {
	uint32_t ssa_variable_id = argument.source_operand.ssa_variable_id;

	if (op_array == nullptr || ssa == nullptr || ssa->var_info == nullptr
			|| ssa->ops == nullptr) {
		return false;
	}
	if (ssa_variable_id == ZEND_MIR_ID_INVALID
			&& argument.send_opline_index < op_array->last
			&& ssa->ops[argument.send_opline_index].op1_use >= 0) {
		ssa_variable_id = static_cast<uint32_t>(
			ssa->ops[argument.send_opline_index].op1_use);
	}
	return ssa_variable_id != ZEND_MIR_ID_INVALID
		&& ssa_variable_id < static_cast<uint32_t>(ssa->vars_count)
		&& (ssa->var_info[ssa_variable_id].type & MAY_BE_UNDEF) != 0;
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
					instruction.call_site->target_id,
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
					instruction.call_site->target_id,
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

	if (plan == nullptr || plan->source_opcodes == nullptr
			|| site.source_init_opline_index >= plan->source_opcode_count
			|| site.source_do_opline_index >= plan->source_opcode_count
			|| site.source_init_opline_index
				>= site.source_do_opline_index) {
		return false;
	}
	if (zend_get_user_opcode_handler(
			plan->source_opcodes[
				site.source_init_opline_index].opcode) != nullptr
			|| zend_get_user_opcode_handler(
				plan->source_opcodes[
					site.source_do_opline_index].opcode) != nullptr) {
		return true;
	}
	for (uint32_t index = 0; index < site.arguments.count; ++index) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
				plan, site.arguments.offset + index, &argument)
				|| argument.send_opline_index
					>= plan->source_opcode_count) {
			return false;
		}
		if (zend_get_user_opcode_handler(
				plan->source_opcodes[
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
			plan->source_opcodes[source_position].opcode;
		if (opcode == ZEND_CHECK_FUNC_ARG
				|| opcode == ZEND_CHECK_UNDEF_ARGS) {
			return true;
		}
	}
	return false;
}

bool call_site_participates_in_nested_call(
	const zend_mir_call_view *calls, const zend_mir_call_site_ref &site)
{
	if (calls == nullptr || calls->call_site_count == nullptr
			|| calls->call_site_at == nullptr) {
		return false;
	}
	const uint32_t call_site_count = calls->call_site_count(calls->context);
	for (uint32_t index = 0; index < call_site_count; ++index) {
		zend_mir_call_site_ref candidate{};

		if (!calls->call_site_at(calls->context, index, &candidate)) {
			continue;
		}
		if (candidate.id != site.id
				&& ((candidate.source_init_opline_index
						> site.source_init_opline_index
					&& candidate.source_do_opline_index
						< site.source_do_opline_index)
					|| (site.source_init_opline_index
						> candidate.source_init_opline_index
					&& site.source_do_opline_index
						< candidate.source_do_opline_index))) {
			return true;
		}
	}
	return false;
}

zend_native_user_call_descriptor *build_user_call_descriptor(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_call_site_ref &site,
	const zend_mir_instruction_record &record,
	zend_native_diagnostic *diag)
{
	if (plan == nullptr || source_op_array == nullptr
			|| site.source_init_opline_index >= source_op_array->last
			|| site.source_do_opline_index >= source_op_array->last) {
		return nullptr;
	}
	const zend_op *init = &source_op_array->opcodes[
		site.source_init_opline_index];
	const zend_op *finish = &source_op_array->opcodes[
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
			"call source operands are invalid");
		return nullptr;
	}
	descriptor->do_result = site.result_operand;
	if (finish->opcode != ZEND_CALLABLE_CONVERT
			&& finish->opcode != ZEND_CALLABLE_CONVERT_PARTIAL
			&& zend_mir_id_is_valid(record.result_id)) {
		const int32_t result_index =
			zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call result is unknown");
			return nullptr;
		}
		if (zend_mir_scalar_type_is_exact(
				plan->values[result_index].exact_type)) {
			descriptor->result_type =
				plan->values[result_index].exact_type;
			descriptor->flags |=
				ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT;
		}
	}
	for (uint32_t index = 0; index < site.arguments.count; ++index) {
		zend_mir_call_argument_ref argument;
		if (!zend_tpde_call_argument_at(
					plan, site.arguments.offset + index, &argument)
				|| argument.send_opline_index
					>= source_op_array->last) {
			std::free(descriptor);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source call argument table is unreadable");
			return nullptr;
		}
		const zend_op *send = &source_op_array->opcodes[
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
				source_op_array, send, send->op2_type,
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

bool freeze_source_call_phases(
	zend_tpde_plan *plan, zend_native_diagnostic *diag)
{
	if (plan == nullptr || plan->source_opcode_count == 0
			|| plan->source_opcodes == nullptr) {
		return true;
	}

	auto phase_candidate = [&](uint32_t instruction_index) {
		if (instruction_index >= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& instruction.user_call != nullptr
			&& instruction.direct_call == nullptr
			&& instruction.user_call->do_opcode != ZEND_CALLABLE_CONVERT
			&& instruction.user_call->do_opcode
				!= ZEND_CALLABLE_CONVERT_PARTIAL;
	};

	bool any_candidate = false;
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		any_candidate = any_candidate || phase_candidate(instruction_index);
	}
	if (!any_candidate) {
		return true;
	}

	plan->source_call_phases =
		static_cast<zend_tpde_source_call_phase_entry *>(std::calloc(
			plan->source_opcode_count, sizeof(*plan->source_call_phases)));
	if (plan->source_call_phases == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate source-call phase table");
		return false;
	}
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		plan->source_call_phases[source].instruction_index = UINT32_MAX;
		plan->source_call_phases[source].argument_index = UINT32_MAX;
		plan->source_call_phases[source].value_index = -1;
	}

	std::vector<uint32_t> init_events(plan->source_opcode_count, UINT32_MAX);
	std::vector<uint32_t> do_events(plan->source_opcode_count, UINT32_MAX);
	auto assign_event = [&](std::vector<uint32_t> &events,
			uint32_t source, uint32_t instruction_index) {
		if (source >= events.size() || events[source] != UINT32_MAX) {
			return false;
		}
		events[source] = instruction_index;
		return true;
	};
	auto assign_phase = [&](uint32_t source, uint32_t instruction_index,
			uint8_t phases) -> zend_tpde_source_call_phase_entry * {
		if (source >= plan->source_opcode_count) {
			return nullptr;
		}
		zend_tpde_source_call_phase_entry &entry =
			plan->source_call_phases[source];
		if (entry.instruction_index != UINT32_MAX
				&& entry.instruction_index != instruction_index) {
			return nullptr;
		}
		entry.instruction_index = instruction_index;
		entry.phases |= phases;
		return &entry;
	};

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
			continue;
		}
		const zend_mir_call_site_ref &site = *instruction.call_site;
		if (site.source_init_opline_index >= site.source_do_opline_index
				|| !assign_event(init_events,
					site.source_init_opline_index, instruction_index)
				|| !assign_event(do_events,
					site.source_do_opline_index, instruction_index)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-call intervals are invalid or overlap at an endpoint");
			return false;
		}
		if (!phase_candidate(instruction_index)) {
			continue;
		}
		if (assign_phase(site.source_init_opline_index, instruction_index,
				ZEND_TPDE_SOURCE_CALL_PHASE_INIT) == nullptr
				|| assign_phase(site.source_do_opline_index, instruction_index,
					ZEND_TPDE_SOURCE_CALL_PHASE_DO) == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-call phases collide at an INIT or DO opline");
			return false;
		}

		for (uint32_t argument = 0;
				argument < instruction.call_argument_count; ++argument) {
			const uint32_t argument_index =
				instruction.call_argument_offset + argument;
			zend_mir_call_argument_ref source_argument{};
			if (!zend_tpde_call_argument_at(
					plan, argument_index, &source_argument)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call argument phase is unreadable");
				return false;
			}
			zend_tpde_source_call_phase_entry *entry = assign_phase(
				source_argument.send_opline_index, instruction_index,
				ZEND_TPDE_SOURCE_CALL_PHASE_SEND);
			if (entry == nullptr || entry->argument_index != UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call SEND phases are duplicated or collide");
				return false;
			}
			/* Placement indices are descriptor-local, never plan-global. */
			entry->argument_index = argument;

			const zend_tpde_source_value_binding &binding =
				plan->call_argument_bindings[argument_index];
			bool direct_value = binding.value_index >= 0
				&& static_cast<uint32_t>(binding.value_index)
					< plan->value_count
				&& (source_argument.source_mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
					|| source_argument.source_mode
						== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED)
				&& source_argument.ownership
					!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
			if (direct_value) {
				const zend_tpde_value &value = plan->values[
					static_cast<uint32_t>(binding.value_index)];
				const zend_mir_scalar_type_mask exact_type = value.exact_type;
				direct_value = zend_mir_scalar_type_is_exact(exact_type)
					&& (exact_type == ZEND_MIR_SCALAR_TYPE_I1
						|| exact_type == ZEND_MIR_SCALAR_TYPE_I64
						|| exact_type == ZEND_MIR_SCALAR_TYPE_F64)
					&& (binding.definition_instruction_index >= 0
						|| value.constant || value.argument_index >= 0);
			}
			if (direct_value && binding.definition_instruction_index >= 0) {
				const uint32_t definition_index = static_cast<uint32_t>(
					binding.definition_instruction_index);
				if (definition_index >= plan->instruction_count) {
					direct_value = false;
				} else {
					const zend_mir_instruction_record definition =
						zend_tpde_instruction_record_at(
							plan, &plan->instructions[definition_index]);
					direct_value = definition.source_position_id
						< source_argument.send_opline_index
						&& plan->source_opcode_block_indices != nullptr
						&& definition.source_position_id
							< plan->source_opcode_count
						&& plan->source_opcode_block_indices[
							definition.source_position_id]
							== plan->source_opcode_block_indices[
								source_argument.send_opline_index];
				}
			}
			if (direct_value) {
				entry->value_index = binding.value_index;
				entry->operand_flags |=
					ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE;
			} else {
				const zend_mir_source_operand_ref &operand =
					source_argument.source_operand;
				const bool source_operand =
					operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| (operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA
						&& operand.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_KIND_INVALID)
					|| (operand.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
						&& source_argument.source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER);
				if (!source_operand) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source-call SEND value is defined after its landing");
					return false;
				}
				entry->operand_flags |=
					ZEND_TPDE_SOURCE_CALL_OPERAND_SOURCE;
			}

			if (source_argument.source_mode
					== ZEND_MIR_SOURCE_CALL_ARGUMENT_UNPACK) {
				entry->operand_flags |=
					ZEND_TPDE_SOURCE_CALL_OPERAND_RUNTIME_EXPANSION;
			}
		}
		/*
		 * The single expansion node is a generated conditional statepoint.
		 * It is a no-op for a completely direct placement plan and invokes the
		 * bounded helper only when resolution marked an unpack or a genuinely
		 * extra named tail for runtime expansion.
		 */
		if (assign_phase(site.source_do_opline_index, instruction_index,
					ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND) == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source-call expansion phase collides at its DO opline");
			return false;
		}
	}

	std::vector<uint32_t> active_calls;
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		if (init_events[source] != UINT32_MAX) {
			active_calls.push_back(init_events[source]);
		}
		const uint8_t opcode = plan->source_opcodes[source].opcode;
		if ((opcode == ZEND_CHECK_FUNC_ARG
				|| opcode == ZEND_CHECK_UNDEF_ARGS)
				&& !active_calls.empty()
				&& phase_candidate(active_calls.back())) {
			const uint32_t instruction_index = active_calls.back();
			zend_tpde_source_call_phase_entry *entry = assign_phase(
				source, instruction_index,
				ZEND_TPDE_SOURCE_CALL_PHASE_CHECK);
			if (entry == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call CHECK phase collides with another call");
				return false;
			}
			if (opcode == ZEND_CHECK_FUNC_ARG) {
				const zend_tpde_instruction &instruction =
					plan->instructions[instruction_index];
				uint32_t next_argument = UINT32_MAX;
				uint32_t next_send = UINT32_MAX;
				for (uint32_t argument = 0;
						argument < instruction.call_argument_count;
						++argument) {
					zend_mir_call_argument_ref candidate{};
					if (!zend_tpde_call_argument_at(plan,
							instruction.call_argument_offset + argument,
							&candidate)) {
						next_argument = UINT32_MAX;
						break;
					}
					if (candidate.send_opline_index > source
							&& candidate.send_opline_index < next_send) {
						next_send = candidate.send_opline_index;
						next_argument = argument;
					}
				}
				if (next_argument == UINT32_MAX) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"CHECK_FUNC_ARG has no following placement");
					return false;
				}
				entry->argument_index = next_argument;
			}
		}
		if (do_events[source] != UINT32_MAX) {
			if (active_calls.empty()
					|| active_calls.back() != do_events[source]) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source-call intervals are not correctly nested");
				return false;
			}
			active_calls.pop_back();
		}
	}
	if (!active_calls.empty()) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source-call interval remains open at function end");
		return false;
	}

	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		plan->source_call_phase_count +=
			plan->source_call_phases[source].phases != 0;
	}
	return true;
}

bool freeze_machine_plan_consumers(
	zend_tpde_plan *plan, zend_native_diagnostic *diag)
{
	const uint32_t value_count = plan->value_count;
	std::vector<uint32_t> counts(value_count, 0);

	plan->value_definition_instructions = value_count == 0
		? nullptr : static_cast<int32_t *>(std::malloc(
			static_cast<size_t>(value_count)
				* sizeof(*plan->value_definition_instructions)));
	plan->source_value_definition_instructions = value_count == 0
		? nullptr : static_cast<int32_t *>(std::malloc(
			static_cast<size_t>(value_count)
				* sizeof(*plan->source_value_definition_instructions)));
	plan->value_consumer_offsets = static_cast<uint32_t *>(std::calloc(
		static_cast<size_t>(value_count) + 1,
		sizeof(*plan->value_consumer_offsets)));
	if ((value_count != 0
				&& (plan->value_definition_instructions == nullptr
					|| plan->source_value_definition_instructions == nullptr))
			|| plan->value_consumer_offsets == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze machine-plan consumers");
		return false;
	}
	if (value_count != 0) {
		std::fill_n(plan->value_definition_instructions,
			value_count, int32_t{-1});
		std::fill_n(plan->source_value_definition_instructions,
			value_count, int32_t{-1});
	}

	auto visit_use = [&](zend_mir_value_id id,
			uint32_t instruction_index, uint32_t operand_index,
			uint32_t auxiliary, zend_tpde_machine_use_kind kind, bool fill,
			std::vector<uint32_t> *cursors) -> bool {
		if (!zend_mir_id_is_valid(id)) {
			return true;
		}
		const int32_t value_index = zend_tpde_value_index(plan, id);
		if (value_index < 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-plan consumer references an unknown value");
			return false;
		}
		const uint32_t value = static_cast<uint32_t>(value_index);
		if (!fill) {
			if (counts[value] == UINT32_MAX) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan consumer count overflows");
				return false;
			}
			++counts[value];
		} else {
			plan->value_consumers[(*cursors)[value]++] = {
				instruction_index,
				operand_index,
				auxiliary,
				kind,
			};
		}
		return true;
	};
	auto visit_instruction_uses = [&](uint32_t instruction_index,
			bool fill, std::vector<uint32_t> *cursors) -> bool {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		for (uint32_t n = 0; n < instruction.operand_count; ++n) {
			if (!visit_use(zend_tpde_operand_at(
					plan, &instruction, n), instruction_index, n,
					record.opcode == ZEND_MIR_OPCODE_PHI ? n : UINT32_MAX,
					record.opcode == ZEND_MIR_OPCODE_PHI
						? ZEND_TPDE_MACHINE_USE_PHI_EDGE
						: ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND,
					fill, cursors)) {
				return false;
			}
		}
		if (instruction.has_value_operation) {
			const zend_tpde_source_value_binding bindings[] = {
				instruction.source_op1_binding,
				instruction.source_op2_binding,
				instruction.source_auxiliary_binding,
			};
			for (uint32_t n = 0; n < 3; ++n) {
				const int32_t value_index = bindings[n].value_index;
				if (value_index < 0) {
					continue;
				}
				if (static_cast<uint32_t>(value_index) >= plan->value_count
						|| !visit_use(
							plan->values[static_cast<uint32_t>(value_index)].id,
							instruction_index, n, UINT32_MAX,
							ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND,
							fill, cursors)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source operand binding references an unknown value");
					return false;
				}
			}
		}
		for (uint32_t n = 0;
				n < instruction.call_argument_count; ++n) {
			zend_mir_call_argument_ref argument{};
			if (!zend_tpde_call_argument_at(plan,
					instruction.call_argument_offset + n, &argument)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan call argument table is unreadable");
				return false;
			}
			if (!visit_use(argument.value_id, instruction_index, n,
					instruction.component_target_index != UINT32_MAX
							&& record.opcode
								== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						? instruction.component_target_index
						: UINT32_MAX,
					instruction.component_target_index != UINT32_MAX
							&& record.opcode
								== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						? ZEND_TPDE_MACHINE_USE_LOCAL_ABI_ARGUMENT
						: ZEND_TPDE_MACHINE_USE_CALL_ARGUMENT,
					fill, cursors)) {
				return false;
			}
		}
		if (instruction.materialization_offset
					> plan->materialization_count
				|| instruction.materialization_count
					> plan->materialization_count
						- instruction.materialization_offset) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-plan materialization slice is invalid");
			return false;
		}
		for (uint32_t n = 0;
				n < instruction.materialization_count; ++n) {
			const zend_tpde_materialization &materialization =
				plan->materializations[
					instruction.materialization_offset + n];
			if (materialization.value_index == UINT32_MAX) {
				if (materialization
								.source_definition_instruction_index < 0
						|| static_cast<uint32_t>(materialization
							.source_definition_instruction_index)
							>= plan->instruction_count
						|| (materialization.source_value_index >= 0
							&& static_cast<uint32_t>(
								materialization.source_value_index)
								>= plan->value_count)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"machine-plan source materialization has no definition");
					return false;
				}
				continue;
			}
			if (materialization.value_index >= plan->value_count
					|| !visit_use(
						plan->values[materialization.value_index].id,
						instruction_index, n,
						materialization.storage_id,
						ZEND_TPDE_MACHINE_USE_STATEPOINT_MATERIALIZATION,
						fill, cursors)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan materialization references an unknown value");
				return false;
			}
		}
		if (instruction.has_value_operation
				&& record.source_position_id != UINT32_MAX
				&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
					|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
					|| record.opcode
						== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
			const uint32_t target = record.source_position_id + 1;
			for (uint32_t resume = 0;
					resume < plan->generator_resume_count; ++resume) {
				if (plan->generator_resume_targets[resume] != target) {
					continue;
				}
				for (uint32_t value = 0;
						value < plan->value_count; ++value) {
					if (zend_tpde_generator_resume_value_live(
							plan, resume, value)
							&& !visit_use(plan->values[value].id,
								instruction_index, value, resume,
								ZEND_TPDE_MACHINE_USE_SUSPEND_LIVE,
								fill, cursors)) {
						return false;
					}
				}
				break;
			}
		}
		return true;
	};

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(
				plan, &plan->instructions[instruction_index]);
		if (zend_mir_id_is_valid(record.result_id)) {
			const int32_t value_index =
				zend_tpde_value_index(plan, record.result_id);
			if (value_index < 0
					|| plan->value_definition_instructions[
						static_cast<uint32_t>(value_index)] >= 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine-plan value has an invalid or duplicate definition");
				return false;
			}
			plan->value_definition_instructions[
				static_cast<uint32_t>(value_index)] =
				static_cast<int32_t>(instruction_index);
		}
		const int32_t source_result =
			plan->instructions[instruction_index]
				.source_result_binding.value_index;
		if (source_result >= 0
				&& static_cast<uint32_t>(source_result) < value_count
				&& plan->source_value_definition_instructions[
					static_cast<uint32_t>(source_result)] < 0) {
			plan->source_value_definition_instructions[
				static_cast<uint32_t>(source_result)] =
				static_cast<int32_t>(instruction_index);
		}
		if (!visit_instruction_uses(instruction_index, false, nullptr)) {
			return false;
		}
	}

	uint64_t consumer_count = 0;
	for (uint32_t value = 0; value < value_count; ++value) {
		plan->value_consumer_offsets[value] =
			static_cast<uint32_t>(consumer_count);
		consumer_count += counts[value];
		if (consumer_count > MAX_RECORDS || consumer_count > UINT32_MAX) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-plan consumer table exceeds the executable bound");
			return false;
		}
	}
	plan->value_consumer_offsets[value_count] =
		static_cast<uint32_t>(consumer_count);
	plan->value_consumer_count = static_cast<uint32_t>(consumer_count);
	plan->value_consumers = consumer_count == 0
		? nullptr : static_cast<zend_tpde_machine_use *>(std::malloc(
			static_cast<size_t>(consumer_count)
				* sizeof(*plan->value_consumers)));
	if (consumer_count != 0 && plan->value_consumers == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate machine-plan consumer table");
		return false;
	}
	std::vector<uint32_t> cursors(
		plan->value_consumer_offsets,
		plan->value_consumer_offsets + value_count);
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		if (!visit_instruction_uses(
				instruction_index, true, &cursors)) {
			return false;
		}
	}
	return true;
}

static int32_t machine_source_binding_value_index(
		const zend_tpde_plan *plan,
		const zend_tpde_source_value_binding &binding) {
	if (binding.value_index >= 0
			&& static_cast<uint32_t>(binding.value_index)
				< plan->value_count) {
		return binding.value_index;
	}
	if (binding.definition_instruction_index < 0
			|| static_cast<uint32_t>(binding.definition_instruction_index)
				>= plan->instruction_count) {
		return -1;
	}
	const zend_tpde_source_value_binding &result =
		plan->instructions[static_cast<uint32_t>(
			binding.definition_instruction_index)].source_result_binding;
	return result.value_index >= 0
			&& static_cast<uint32_t>(result.value_index) < plan->value_count
		? result.value_index : -1;
}

static bool machine_short_circuit_boolean_phi_consumer(
		const zend_tpde_plan *plan,
		const zend_tpde_machine_use &use) {
	if (use.kind != ZEND_TPDE_MACHINE_USE_PHI_EDGE
			|| use.instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &phi =
		plan->instructions[use.instruction_index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &phi);
	if (record.opcode != ZEND_MIR_OPCODE_PHI
			|| phi.operand_count != 2
			|| use.operand_index >= phi.operand_count) {
		return false;
	}
	const uint32_t edge_operand = use.operand_index == 0 ? 1 : 0;
	const int32_t edge_value = zend_tpde_value_index(
		plan, zend_tpde_operand_at(plan, &phi, edge_operand));
	if (edge_value < 0) {
		return false;
	}
	int32_t producer = plan->value_definition_instructions == nullptr
		? -1 : plan->value_definition_instructions[edge_value];
	if (producer < 0) {
		for (uint32_t candidate = 0;
				candidate < plan->instruction_count; ++candidate) {
			if (plan->instructions[candidate]
					.source_result_binding.value_index == edge_value) {
				producer = static_cast<int32_t>(candidate);
				break;
			}
		}
	}
	if (producer < 0
			|| static_cast<uint32_t>(producer) >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &branch =
		plan->instructions[static_cast<uint32_t>(producer)];
	const zend_mir_instruction_record branch_record =
		zend_tpde_instruction_record_at(plan, &branch);
	return branch.has_value_operation
		&& branch_record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
		&& (branch.value_operation.source_opcode == ZEND_JMPZ_EX
			|| branch.value_operation.source_opcode == ZEND_JMPNZ_EX)
		&& (branch.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
}

static bool machine_register_boolean_consumer(
		const zend_tpde_plan *plan,
		const zend_tpde_machine_use &use,
		int32_t producer_value_index,
		uint32_t producer_instruction_index) {
	if (machine_short_circuit_boolean_phi_consumer(plan, use)) {
		return true;
	}
	if (use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
			|| use.instruction_index >= plan->instruction_count
			|| use.operand_index != 0) {
		return false;
	}
	const zend_tpde_instruction &consumer =
		plan->instructions[use.instruction_index];
	if (!consumer.has_value_operation
			|| machine_source_binding_value_index(
				plan, consumer.source_op1_binding)
				!= producer_value_index) {
		return false;
	}
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &consumer);
	if (producer_instruction_index >= plan->instruction_count
			|| zend_tpde_instruction_record_at(plan,
				&plan->instructions[producer_instruction_index]).block_id
				!= record.block_id) {
		return false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
		/*
		 * A terminal branch is converted to a register consumer when its producer
		 * is selected below.  A result-producing short-circuit branch must already
		 * have passed the stronger register-only edge check: otherwise selecting
		 * its producer would leave the branch reading an unmaterialized zval.
		 */
		if (consumer.value_operation.source_opcode == ZEND_JMPZ
				|| consumer.value_operation.source_opcode == ZEND_JMPNZ) {
			return true;
		}
		return (consumer.value_operation.source_opcode == ZEND_JMPZ_EX
				|| consumer.value_operation.source_opcode == ZEND_JMPNZ_EX)
			&& (consumer.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
	}
	if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP) {
		return consumer.value_operation.source_opcode == ZEND_BOOL
			|| consumer.value_operation.source_opcode == ZEND_BOOL_NOT;
	}
	return false;
}

static void freeze_register_boolean_terminal_consumers(
		zend_tpde_plan *plan, int32_t producer_value_index) {
	if (producer_value_index < 0
			|| static_cast<uint32_t>(producer_value_index) >= plan->value_count
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return;
	}
	const uint32_t value = static_cast<uint32_t>(producer_value_index);
	const uint32_t begin = plan->value_consumer_offsets[value];
	const uint32_t end = plan->value_consumer_offsets[value + 1];
	for (uint32_t use_index = begin; use_index < end; ++use_index) {
		const zend_tpde_machine_use &use = plan->value_consumers[use_index];
		if (use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
				|| use.operand_index != 0
				|| use.instruction_index >= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &consumer =
			plan->instructions[use.instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &consumer);
		if (consumer.has_value_operation
				&& record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& machine_source_binding_value_index(
					plan, consumer.source_op1_binding) == producer_value_index
				&& (consumer.value_operation.source_opcode == ZEND_JMPZ
					|| consumer.value_operation.source_opcode == ZEND_JMPNZ)) {
			consumer.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
		}
	}
}

static int32_t machine_single_source_consumer(
		const zend_tpde_plan *plan, int32_t value_index) {
	if (value_index < 0
			|| static_cast<uint32_t>(value_index) >= plan->value_count
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return -1;
	}
	const uint32_t begin =
		plan->value_consumer_offsets[static_cast<uint32_t>(value_index)];
	const uint32_t end =
		plan->value_consumer_offsets[static_cast<uint32_t>(value_index) + 1];
	int32_t consumer = -1;
	bool source_operand = false;
	for (uint32_t index = begin; index < end; ++index) {
		const zend_tpde_machine_use &use = plan->value_consumers[index];
		if ((use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
				&& use.kind != ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND)
				|| (use.kind == ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
					&& use.operand_index != 0)
				|| (consumer >= 0
					&& static_cast<uint32_t>(consumer)
						!= use.instruction_index)) {
			return -1;
		}
		consumer = static_cast<int32_t>(use.instruction_index);
		source_operand = source_operand
			|| use.kind == ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND;
	}
	return source_operand ? consumer : -1;
}

static bool machine_short_circuit_branch_is_register_only(
		const zend_tpde_plan *plan, uint32_t branch_index) {
	if (branch_index >= plan->instruction_count
			|| plan->value_consumer_offsets == nullptr
			|| plan->value_consumers == nullptr) {
		return false;
	}
	const zend_tpde_instruction &branch = plan->instructions[branch_index];
	const int32_t result_index = machine_source_binding_value_index(
		plan, branch.source_result_binding);
	if (result_index < 0) {
		return false;
	}
	const uint32_t value = static_cast<uint32_t>(result_index);
	const uint32_t begin = plan->value_consumer_offsets[value];
	const uint32_t end = plan->value_consumer_offsets[value + 1];
	if (begin == end) {
		return false;
	}
	for (uint32_t use_index = begin; use_index < end; ++use_index) {
		const zend_tpde_machine_use &use =
			plan->value_consumers[use_index];
		if (use.kind != ZEND_TPDE_MACHINE_USE_PHI_EDGE
				|| use.instruction_index >= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &phi =
			plan->instructions[use.instruction_index];
		const zend_mir_instruction_record phi_record =
			zend_tpde_instruction_record_at(plan, &phi);
		if (phi_record.opcode != ZEND_MIR_OPCODE_PHI
				|| phi.operand_count != 2) {
			return false;
		}
		const int32_t phi_result =
			zend_tpde_value_index(plan, phi_record.result_id);
		const int32_t consumer_index =
			machine_single_source_consumer(plan, phi_result);
		if (consumer_index < 0
				|| static_cast<uint32_t>(consumer_index)
					>= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &consumer =
			plan->instructions[static_cast<uint32_t>(consumer_index)];
		const zend_mir_instruction_record consumer_record =
			zend_tpde_instruction_record_at(plan, &consumer);
		if (!consumer.has_value_operation
				|| consumer_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (consumer.value_operation.source_opcode != ZEND_JMPZ
					&& consumer.value_operation.source_opcode != ZEND_JMPNZ)) {
			return false;
		}
	}
	return true;
}

static void freeze_register_boolean_results(zend_tpde_plan *plan)
{
	if (plan->value_consumer_offsets == nullptr) {
		return;
	}
	std::vector<int32_t> value_definitions(plan->value_count, -1);
	if (plan->value_definition_instructions != nullptr) {
		std::copy_n(plan->value_definition_instructions, plan->value_count,
			value_definitions.begin());
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const int32_t value_index =
			plan->instructions[index].source_result_binding.value_index;
		if (value_index >= 0
				&& static_cast<uint32_t>(value_index) < plan->value_count
				&& value_definitions[static_cast<uint32_t>(value_index)] < 0) {
			value_definitions[static_cast<uint32_t>(value_index)] =
				static_cast<int32_t>(index);
		}
	}
	auto value_definition = [&](int32_t value_index) {
		return value_index < 0
				|| static_cast<uint32_t>(value_index) >= plan->value_count
			? int32_t{-1}
			: value_definitions[static_cast<uint32_t>(value_index)];
	};
	std::vector<uint8_t> register_boolean_results(
		plan->instruction_count);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		register_boolean_results[index] =
			(plan->instructions[index].machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0;
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (instruction.has_value_operation
				&& record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& (instruction.value_operation.source_opcode == ZEND_JMPZ_EX
					|| instruction.value_operation.source_opcode == ZEND_JMPNZ_EX)
				&& machine_short_circuit_branch_is_register_only(plan, index)) {
			instruction.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
		}
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (!instruction.has_value_operation
				|| record.opcode
					!= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				|| instruction.value_operation.source_opcode
					!= ZEND_ISSET_ISEMPTY_CV
				|| instruction.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
			continue;
		}
		const int32_t input_index = machine_source_binding_value_index(
			plan, instruction.source_op1_binding);
		const int32_t result_index = machine_source_binding_value_index(
			plan, instruction.source_result_binding);
		if (input_index < 0 || result_index < 0) {
			continue;
		}
		const zend_tpde_value &input =
			plan->values[static_cast<uint32_t>(input_index)];
		if ((input.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& input.exact_type != ZEND_MIR_SCALAR_TYPE_I1
				&& input.exact_type != ZEND_MIR_SCALAR_TYPE_I64)
				|| input.canonical_alias_observable
				|| input.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
			continue;
		}
		zend_tpde_slot_isset_empty isset_layout{};
		if (!zend_tpde_slot_isset_empty_at(instruction, &isset_layout)) {
			continue;
		}
		const uint32_t value = static_cast<uint32_t>(result_index);
		const uint32_t begin = plan->value_consumer_offsets[value];
		const uint32_t end = plan->value_consumer_offsets[value + 1];
		if (begin == end) {
			continue;
		}
		bool all_register_consumers = true;
		for (uint32_t use = begin; use < end; ++use) {
			if (!machine_register_boolean_consumer(
					plan, plan->value_consumers[use], result_index, index)) {
				all_register_consumers = false;
				break;
			}
		}
		if (all_register_consumers) {
			instruction.machine_control_flow_flags &=
				~ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			instruction.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT;
			register_boolean_results[index] = 1;
			freeze_register_boolean_terminal_consumers(plan, result_index);
		}
	}

	/*
	 * BOOL and BOOL_NOT preserve the complete semantics of a proven machine
	 * boolean.  Propagate the register result through such chains so short
	 * circuit lowering does not create a guarded helper diamond for each
	 * logical negation between isset/empty and its branch consumer.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (!instruction.has_value_operation
				|| record.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
				|| (instruction.value_operation.source_opcode != ZEND_BOOL
					&& instruction.value_operation.source_opcode
						!= ZEND_BOOL_NOT)) {
			continue;
		}
		int32_t producer =
			instruction.source_op1_binding.definition_instruction_index;
		if (producer < 0) {
			const int32_t input = machine_source_binding_value_index(
				plan, instruction.source_op1_binding);
			producer = input >= 0
					&& plan->value_definition_instructions != nullptr
				? plan->value_definition_instructions[
					static_cast<uint32_t>(input)]
				: -1;
			if (producer < 0 && input >= 0) {
				producer = value_definition(input);
			}
		}
		if (producer < 0
				|| static_cast<uint32_t>(producer) >= index
				|| register_boolean_results[
					static_cast<uint32_t>(producer)] == 0) {
			continue;
		}
		const int32_t result_index = machine_source_binding_value_index(
			plan, instruction.source_result_binding);
		if (result_index < 0) {
			continue;
		}
		const uint32_t value = static_cast<uint32_t>(result_index);
		const uint32_t begin = plan->value_consumer_offsets[value];
		const uint32_t end = plan->value_consumer_offsets[value + 1];
		if (begin == end) {
			continue;
		}
		bool all_register_consumers = true;
		for (uint32_t use = begin; use < end; ++use) {
			if (!machine_register_boolean_consumer(
					plan, plan->value_consumers[use],
					result_index, index)) {
				all_register_consumers = false;
				break;
			}
		}
		if (all_register_consumers) {
			instruction.machine_control_flow_flags &=
				~ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			instruction.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT;
			register_boolean_results[index] = 1;
			freeze_register_boolean_terminal_consumers(plan, result_index);
		}
	}

	/*
	 * Fold the narrow empty(CV) -> BOOL_NOT form into the earlier empty result.
	 * The result alias is essential: every later consumer then uses a value
	 * defined by the producer, rather than a synthetic value whose definition
	 * may appear after a short-circuit PHI in TPDE's block schedule.  Requiring
	 * the producer to have one semantic consumer guarantees that its original
	 * polarity is not observable anywhere else.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &producer = plan->instructions[index];
		const zend_mir_instruction_record producer_record =
			zend_tpde_instruction_record_at(plan, &producer);
		zend_tpde_slot_isset_empty layout{};
		if (!producer.has_value_operation
				|| producer_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				|| (producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) == 0
				|| !zend_tpde_slot_isset_empty_at(producer, &layout)
				|| !layout.is_empty) {
			continue;
		}
		const int32_t input_index = machine_source_binding_value_index(
			plan, producer.source_op1_binding);
		const int32_t producer_result = machine_source_binding_value_index(
			plan, producer.source_result_binding);
		if (input_index < 0
				|| (plan->values[static_cast<uint32_t>(input_index)].exact_type
						!= ZEND_MIR_SCALAR_TYPE_I1
					&& plan->values[static_cast<uint32_t>(input_index)].exact_type
						!= ZEND_MIR_SCALAR_TYPE_I64)) {
			continue;
		}
		const int32_t unary_index =
			machine_single_source_consumer(plan, producer_result);
		if (unary_index <= static_cast<int32_t>(index)
				|| static_cast<uint32_t>(unary_index)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &unary =
			plan->instructions[static_cast<uint32_t>(unary_index)];
		const zend_mir_instruction_record unary_record =
			zend_tpde_instruction_record_at(plan, &unary);
		if (!unary.has_value_operation
				|| unary_record.opcode != ZEND_MIR_OPCODE_VALUE_UNARY_OP
				|| unary.value_operation.source_opcode != ZEND_BOOL_NOT
				|| unary_record.block_id != producer_record.block_id
				|| (unary.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) == 0) {
			continue;
		}
		const int32_t unary_result = machine_source_binding_value_index(
			plan, unary.source_result_binding);
		const int32_t branch_index =
			machine_single_source_consumer(plan, unary_result);
		if (branch_index <= unary_index
				|| static_cast<uint32_t>(branch_index)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &branch =
			plan->instructions[static_cast<uint32_t>(branch_index)];
		const zend_mir_instruction_record branch_record =
			zend_tpde_instruction_record_at(plan, &branch);
		if (!branch.has_value_operation
				|| branch_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| branch_record.block_id != unary_record.block_id
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)) {
			continue;
		}
		producer.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_INVERT_RESULT;
		unary.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS;
		branch.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
	}

	/*
	 * The adaptor already recognizes the canonical two-edge short-circuit PHI
	 * when both inputs are exact machine booleans.  Freeze only the corresponding
	 * terminal branch here; keep all intervening BOOL operations as real
	 * definitions so edge values retain their original polarity and liveness.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &phi = plan->instructions[index];
		const zend_mir_instruction_record phi_record =
			zend_tpde_instruction_record_at(plan, &phi);
		if (phi_record.opcode != ZEND_MIR_OPCODE_PHI
				|| phi.operand_count != 2) {
			continue;
		}
		bool exact_register_inputs = true;
		bool short_circuit_edge = false;
		for (uint32_t operand = 0; operand < phi.operand_count; ++operand) {
			const int32_t value_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &phi, operand));
			const int32_t producer_index =
				value_definition(value_index);
			if (producer_index < 0
					|| static_cast<uint32_t>(producer_index)
						>= plan->instruction_count) {
				exact_register_inputs = false;
				break;
			}
			const zend_tpde_instruction &producer =
				plan->instructions[static_cast<uint32_t>(producer_index)];
			const zend_mir_instruction_record producer_record =
				zend_tpde_instruction_record_at(plan, &producer);
			const bool register_result =
				(producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
				&& register_boolean_results[
					static_cast<uint32_t>(producer_index)] != 0;
			const bool register_edge = producer.has_value_operation
				&& producer_record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& (producer.value_operation.source_opcode == ZEND_JMPZ_EX
					|| producer.value_operation.source_opcode == ZEND_JMPNZ_EX)
				&& (producer.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
			if (!register_result && !register_edge) {
				exact_register_inputs = false;
				break;
			}
			short_circuit_edge = short_circuit_edge || register_edge;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan, phi_record.result_id);
		const int32_t branch_index = exact_register_inputs
			&& short_circuit_edge
			? machine_single_source_consumer(plan, result_index) : -1;
		if (branch_index < 0
				|| static_cast<uint32_t>(branch_index)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &branch =
			plan->instructions[static_cast<uint32_t>(branch_index)];
		const zend_mir_instruction_record branch_record =
			zend_tpde_instruction_record_at(plan, &branch);
		if (!branch.has_value_operation
				|| branch_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)) {
			continue;
		}
		phi.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT;
		branch.machine_control_flow_flags |=
			ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
	}

	/*
	 * Zend short-circuit temporaries are source-level values rather than MIR
	 * values.  When both incoming definitions stay as exact machine booleans,
	 * freeze the terminal branch as an implicit two-edge register merge.  The
	 * adaptor will materialize the corresponding TPDE PHI; keeping the shape in
	 * the plan also lets machine-CFG construction omit the boxed branch diamond.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &branch = plan->instructions[index];
		const zend_mir_instruction_record branch_record =
			zend_tpde_instruction_record_at(plan, &branch);
		const int32_t condition_index = machine_source_binding_value_index(
			plan, branch.source_op1_binding);
		if (!branch.has_value_operation
				|| branch_record.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (branch.value_operation.source_opcode != ZEND_JMPZ
					&& branch.value_operation.source_opcode != ZEND_JMPNZ)
				|| (branch.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) == 0
				|| condition_index < 0
				|| value_definition(condition_index) >= 0
				|| !zend_mir_id_is_valid(
					branch.value_operation.op1_storage_id)
				|| plan->block_predecessor_offsets == nullptr
				|| plan->block_predecessors == nullptr) {
			continue;
		}
		const int32_t branch_block =
			zend_tpde_block_index(plan, branch_record.block_id);
		if (branch_block < 0) {
			continue;
		}
		const uint32_t predecessor_begin =
			plan->block_predecessor_offsets[
				static_cast<uint32_t>(branch_block)];
		const uint32_t predecessor_end =
			plan->block_predecessor_offsets[
				static_cast<uint32_t>(branch_block) + 1];
		if (predecessor_end - predecessor_begin != 2) {
			continue;
		}
		bool supported = true;
		bool short_circuit_edge = false;
		for (uint32_t predecessor_offset = predecessor_begin;
				predecessor_offset < predecessor_end;
				++predecessor_offset) {
			const uint32_t predecessor =
				plan->block_predecessors[predecessor_offset];
			int32_t incoming = -1;
			for (uint32_t candidate = plan->instruction_count;
					candidate-- > 0;) {
				const zend_tpde_instruction &producer =
					plan->instructions[candidate];
				const zend_mir_instruction_record producer_record =
					zend_tpde_instruction_record_at(plan, &producer);
				if (!producer.has_value_operation
						|| zend_tpde_block_index(
							plan, producer_record.block_id)
							!= static_cast<int32_t>(predecessor)
						|| producer.value_operation.result_storage_id
							!= branch.value_operation.op1_storage_id) {
					continue;
				}
				const bool register_edge =
					producer_record.opcode
							== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					&& (producer.value_operation.source_opcode
							== ZEND_JMPZ_EX
						|| producer.value_operation.source_opcode
							== ZEND_JMPNZ_EX)
					&& (producer.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH)
						!= 0;
				const bool register_result =
					(producer.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT)
						!= 0
					&& register_boolean_results[candidate] != 0;
				if (!register_edge && !register_result) {
					continue;
				}
				incoming = static_cast<int32_t>(candidate);
				short_circuit_edge = short_circuit_edge || register_edge;
				break;
			}
			if (incoming < 0) {
				supported = false;
				break;
			}
		}
		if (supported && short_circuit_edge) {
			branch.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE;
		}
	}
}

struct zend_tpde_source_definition_index {
	const uint32_t *offsets;
	const uint32_t *instructions;
	const uint32_t *direct_offsets;
	const uint32_t *direct_instructions;
	uint32_t storage_count;
};

zend_tpde_source_value_binding freeze_source_value_binding(
	const zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_source_operand_ref &operand,
	zend_mir_storage_id storage_id,
	uint32_t consumer_source_position,
	uint32_t consumer_instruction_index,
	const zend_tpde_source_definition_index &definitions)
{
	zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
	int32_t value_index = -1;
	if (source_operand_value_id(operand, value_id)) {
		value_index = zend_tpde_value_index(plan, value_id);
	}
	if (value_index < 0
			&& (operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& operand.index < plan->argument_count
			&& plan->argument_value_indices != nullptr
			&& plan->argument_value_indices[operand.index] >= 0) {
		value_index = plan->argument_value_indices[operand.index];
	}
	if (!zend_mir_id_is_valid(storage_id)) {
		return {value_index, -1};
	}
	if (storage_id >= definitions.storage_count) {
		return {value_index, -1};
	}

	int32_t definition = -1;
	uint32_t definition_source_position = 0;
	const bool call_argument = consumer_instruction_index == UINT32_MAX;
	const bool operand_has_ssa =
		zend_mir_id_is_valid(operand.ssa_variable_id);
	const uint32_t *definition_offsets = call_argument
		? definitions.offsets : definitions.direct_offsets;
	const uint32_t *definition_instructions = call_argument
		? definitions.instructions : definitions.direct_instructions;
	const uint32_t definition_begin = definition_offsets[storage_id];
	const uint32_t definition_end = definition_offsets[storage_id + 1];
	for (uint32_t definition_offset = definition_begin;
			definition_offset < definition_end; ++definition_offset) {
		const uint32_t index = definition_instructions[definition_offset];
		const zend_tpde_instruction &candidate = plan->instructions[index];
		const zend_mir_instruction_record candidate_record =
			zend_tpde_instruction_record_at(plan, &candidate);
		zend_mir_source_operand_ref candidate_result{};
		zend_mir_storage_id candidate_storage = ZEND_MIR_ID_INVALID;
		uint32_t candidate_source_position = UINT32_MAX;
		bool has_candidate_result = false;

		if (candidate.direct_call != nullptr) {
			candidate_result = candidate.direct_call->result_operand;
			candidate_storage = source_descriptor_storage(
				source_op_array, candidate_result);
			candidate_source_position =
				candidate.call_site->source_do_opline_index;
			has_candidate_result = true;
		} else if (call_argument
				&& candidate.direct_internal_call != nullptr) {
			candidate_result = candidate.direct_internal_call->result_operand;
			candidate_storage = source_descriptor_storage(
				source_op_array, candidate_result);
			candidate_source_position =
				candidate.call_site->source_do_opline_index;
			has_candidate_result = true;
		} else if (call_argument && candidate.has_value_operation
				&& candidate.value_operation.result.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			candidate_result = candidate.value_operation.result;
			candidate_storage =
				candidate.value_operation.result_storage_id;
			candidate_source_position = candidate_record.source_position_id;
			has_candidate_result = true;
		}
		if (!has_candidate_result
				|| index >= consumer_instruction_index
				|| candidate_source_position > consumer_source_position
				|| candidate_storage != storage_id
				|| (call_argument && operand_has_ssa
					&& (!zend_mir_id_is_valid(
							candidate_result.ssa_variable_id)
						|| operand.ssa_variable_id
							!= candidate_result.ssa_variable_id))
				|| (!call_argument && operand_has_ssa
					&& zend_mir_id_is_valid(
						candidate_result.ssa_variable_id)
					&& operand.ssa_variable_id
						!= candidate_result.ssa_variable_id)
				|| (call_argument && !operand_has_ssa
					&& (consumer_source_position
							>= plan->source_opcode_count
						|| candidate_source_position
							>= plan->source_opcode_count
						|| plan->source_opcode_block_indices == nullptr
						|| plan->source_opcode_block_indices[
							consumer_source_position]
							!= plan->source_opcode_block_indices[
								candidate_source_position]))) {
			continue;
		}
		if (definition < 0
				|| candidate_source_position >= definition_source_position) {
			definition = static_cast<int32_t>(index);
			definition_source_position = candidate_source_position;
		}
	}
	return {value_index, definition};
}

int32_t freeze_reaching_phi_value(
	const zend_tpde_plan *plan,
	zend_mir_block_id consumer_block_id,
	zend_mir_storage_id storage_id)
{
	if (plan == nullptr || !zend_mir_id_is_valid(consumer_block_id)
			|| !zend_mir_id_is_valid(storage_id)
			|| plan->block_predecessor_offsets == nullptr) {
		return -1;
	}
	const int32_t consumer_block =
		zend_tpde_block_index(plan, consumer_block_id);
	if (consumer_block < 0) {
		return -1;
	}
	std::vector<uint8_t> visiting(plan->block_count, 0);
	auto resolve = [&](uint32_t block_index, auto &&self) -> int32_t {
		if (block_index >= plan->block_count || visiting[block_index] != 0) {
			return -1;
		}
		for (uint32_t index = 0; index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &candidate = plan->instructions[index];
			const zend_mir_instruction_record candidate_record =
				zend_tpde_instruction_record_at(plan, &candidate);
			if (candidate_record.block_id != plan->block_ids[block_index]
					|| candidate_record.opcode != ZEND_MIR_OPCODE_PHI
					|| !zend_mir_id_is_valid(candidate_record.result_id)) {
				continue;
			}
			const int32_t value =
				zend_tpde_value_index(plan, candidate_record.result_id);
			if (value >= 0
					&& plan->values[value].canonical_storage_id == storage_id) {
				return value;
			}
		}
		visiting[block_index] = 1;
		const uint32_t begin = plan->block_predecessor_offsets[block_index];
		const uint32_t end = plan->block_predecessor_offsets[block_index + 1];
		int32_t merged = -1;
		for (uint32_t edge = begin; edge < end; ++edge) {
			const int32_t incoming =
				self(plan->block_predecessors[edge], self);
			if (incoming < 0) {
				continue;
			}
			if (merged >= 0 && merged != incoming) {
				visiting[block_index] = 0;
				return -1;
			}
			merged = incoming;
		}
		visiting[block_index] = 0;
		return merged;
	};
	return resolve(static_cast<uint32_t>(consumer_block), resolve);
}

bool freeze_source_value_bindings(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa,
	zend_native_diagnostic *diag)
{
	if (plan == nullptr || source_op_array == nullptr) {
		return true;
	}
	const uint32_t storage_count =
		static_cast<uint32_t>(source_op_array->last_var)
		+ source_op_array->T;
	std::vector<uint32_t> definition_offsets(
		static_cast<size_t>(storage_count) + 1, 0);
	std::vector<uint32_t> direct_definition_offsets(
		static_cast<size_t>(storage_count) + 1, 0);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &candidate = plan->instructions[index];
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;

		if (candidate.direct_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array, candidate.direct_call->result_operand);
		} else if (candidate.direct_internal_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array,
				candidate.direct_internal_call->result_operand);
		} else if (candidate.has_value_operation
				&& candidate.value_operation.result.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			storage_id = candidate.value_operation.result_storage_id;
		}
		if (storage_id < storage_count) {
			definition_offsets[storage_id + 1]++;
			if (candidate.direct_call != nullptr) {
				direct_definition_offsets[storage_id + 1]++;
			}
		}
	}
	for (uint32_t storage = 0; storage < storage_count; ++storage) {
		definition_offsets[storage + 1] += definition_offsets[storage];
		direct_definition_offsets[storage + 1] +=
			direct_definition_offsets[storage];
	}
	std::vector<uint32_t> definition_instructions(
		definition_offsets[storage_count]);
	std::vector<uint32_t> direct_definition_instructions(
		direct_definition_offsets[storage_count]);
	std::vector<uint32_t> definition_cursors = definition_offsets;
	std::vector<uint32_t> direct_definition_cursors =
		direct_definition_offsets;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &candidate = plan->instructions[index];
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;

		if (candidate.direct_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array, candidate.direct_call->result_operand);
		} else if (candidate.direct_internal_call != nullptr) {
			storage_id = source_descriptor_storage(
				source_op_array,
				candidate.direct_internal_call->result_operand);
		} else if (candidate.has_value_operation
				&& candidate.value_operation.result.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			storage_id = candidate.value_operation.result_storage_id;
		}
		if (storage_id < storage_count) {
			definition_instructions[definition_cursors[storage_id]++] = index;
			if (candidate.direct_call != nullptr) {
				direct_definition_instructions[
					direct_definition_cursors[storage_id]++] = index;
			}
		}
	}
	const zend_tpde_source_definition_index definitions = {
		definition_offsets.data(), definition_instructions.data(),
		direct_definition_offsets.data(), direct_definition_instructions.data(),
		storage_count};
	for (uint32_t index = 0; index < plan->call_argument_count; ++index) {
		zend_mir_call_argument_ref argument{};
		if (!zend_tpde_call_argument_at(plan, index, &argument)
				|| argument.send_opline_index
					>= source_op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"unable to freeze a source call-argument value");
			return false;
		}
		plan->call_argument_bindings[index] =
			freeze_source_value_binding(
				plan, source_op_array, argument.source_operand,
				source_descriptor_storage(
					source_op_array, argument.source_operand),
				argument.send_opline_index, UINT32_MAX, definitions);
	}

	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (!zend_mir_id_is_valid(record.id)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"unable to freeze source values for an unknown instruction");
			return false;
		}
		const uint32_t source_position =
			zend_mir_id_is_valid(record.source_position_id)
				? record.source_position_id : UINT32_MAX;
		if (instruction.has_value_operation) {
			const zend_mir_executable_value_ref &operation =
				instruction.value_operation;
			instruction.source_op1_binding = freeze_source_value_binding(
				plan, source_op_array, operation.op1,
				operation.op1_storage_id,
				source_position, index, definitions);
			instruction.source_op2_binding = freeze_source_value_binding(
				plan, source_op_array, operation.op2,
				operation.op2_storage_id,
				source_position, index, definitions);
			if (source_ssa != nullptr && source_ssa->var_info != nullptr
					&& source_position < source_op_array->last
					&& source_ssa->ops[source_position].op2_def >= 0) {
				const zend_ssa_op &source_op =
					source_ssa->ops[source_position];
				const uint32_t definition = static_cast<uint32_t>(
					source_op.op2_def);
				if (definition >= static_cast<uint32_t>(source_ssa->vars_count)
						|| source_ssa->vars[definition].var < 0
						|| static_cast<uint32_t>(
							source_ssa->vars[definition].var)
							!= operation.op2_storage_id) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"source op2 definition does not match its storage");
					return false;
				}
				const int32_t value_index = zend_tpde_value_index(
					plan, zend_mir_value_from_original_ssa(definition));
				if (value_index >= 0) {
					instruction.source_op2_definition_binding =
						{value_index, -1};
					instruction
						.source_op2_definition_ssa_variable_id_plus_one =
						definition + 1;
					if (source_op.op2_use >= 0
							&& source_op.op2_use < source_ssa->vars_count
							&& source_ssa->vars[source_op.op2_use].alias
								== NO_ALIAS) {
						const uint32_t use_type =
							source_ssa->var_info[source_op.op2_use].type
								& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF);
						const uint32_t definition_type =
							source_ssa->var_info[definition].type
								& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF);
						/*
						 * An unaliased destination whose reaching value is only
						 * UNDEF or LONG cannot require reference resolution or a
						 * destructor.  Freeze this exact source proof so the packed
						 * LONG iterator path need not repeat a runtime type guard.
						 */
						instruction.source_op2_canonical_scalar_only =
							use_type != 0
							&& (use_type
								& ~(MAY_BE_UNDEF | MAY_BE_LONG)) == 0
							&& definition_type == MAY_BE_LONG;
					}
				}
			}
			instruction.source_result_binding = freeze_source_value_binding(
				plan, source_op_array, operation.result,
				operation.result_storage_id,
				source_position, index, definitions);
			instruction.source_auxiliary_binding =
				freeze_source_value_binding(
					plan, source_op_array, operation.auxiliary,
					operation.auxiliary_storage_id, source_position,
					index, definitions);
			/*
			 * Zend SSA intentionally omits the RETURN use when a preceding
			 * VERIFY_RETURN_TYPE already consumed the same physical carrier.
			 * Preserve that explicit MIR edge instead of reconstructing a
			 * reaching definition from the slot's linear instruction history;
			 * the latter is invalid for loop exits because a backedge call does
			 * not dominate the return block.
			 */
			if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					&& instruction.source_op1_binding.value_index < 0
					&& !zend_mir_id_is_valid(
						operation.op1.ssa_variable_id)
					&& zend_mir_id_is_valid(operation.op1_storage_id)) {
				for (uint32_t previous = index; previous != 0; --previous) {
					const zend_tpde_instruction &candidate =
						plan->instructions[previous - 1];
					const zend_mir_instruction_record candidate_record =
						zend_tpde_instruction_record_at(plan, &candidate);
					if (candidate_record.block_id != record.block_id) {
						break;
					}
					if (candidate_record.opcode
							!= ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
							|| candidate.operand_count != 1) {
						continue;
					}
					const int32_t verified = zend_tpde_value_index(
						plan, zend_tpde_operand_at(plan, &candidate, 0));
					if (verified >= 0
							&& plan->values[verified].canonical_storage_id
								== operation.op1_storage_id) {
						instruction.source_op1_binding = {verified, -1};
					}
					break;
				}
				if (instruction.source_op1_binding.value_index < 0) {
					const int32_t reaching_phi = freeze_reaching_phi_value(
						plan, record.block_id, operation.op1_storage_id);
					if (reaching_phi >= 0) {
						instruction.source_op1_binding = {reaching_phi, -1};
					}
				}
			}
			const uint8_t source_opcode =
				source_position < plan->source_opcode_count
					? plan->source_opcodes[source_position].opcode
					: ZEND_NOP;
			instruction.local_abi_transport =
				record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
				&& (source_opcode == ZEND_RECV
					|| source_opcode == ZEND_RECV_INIT
					|| source_opcode == ZEND_RECV_VARIADIC);
		}
		if (instruction.direct_call != nullptr) {
			instruction.source_result_binding = {-1,
				static_cast<int32_t>(index)};
		} else if (instruction.direct_internal_call != nullptr
				&& instruction.call_site->source_do_opline_index
					< source_op_array->last) {
			const zend_mir_source_operand_ref &result_operand =
				instruction.direct_internal_call->result_operand;
			instruction.source_result_binding = freeze_source_value_binding(
				plan, source_op_array, result_operand,
				source_descriptor_storage(source_op_array, result_operand),
				instruction.call_site->source_do_opline_index, index,
				definitions);
		}
	}
	return true;
}

void freeze_dynamic_fetch_cv_indices(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_ssa *source_ssa)
{
	if (plan == nullptr || source_op_array == nullptr || source_ssa == nullptr
			|| source_op_array->vars == nullptr
			|| source_op_array->opcodes == nullptr
			|| source_ssa->vars == nullptr || source_ssa->ops == nullptr) {
		return;
	}
	bool frame_aliases_stable = true;
	for (uint32_t source_index = 0;
			source_index < source_op_array->last; ++source_index) {
		switch (source_op_array->opcodes[source_index].opcode) {
			case ZEND_ASSIGN_REF:
			case ZEND_INIT_FCALL_BY_NAME:
			case ZEND_INIT_FCALL:
			case ZEND_SEND_REF:
			case ZEND_NEW:
			case ZEND_INIT_NS_FCALL_BY_NAME:
			case ZEND_INCLUDE_OR_EVAL:
			case ZEND_UNSET_VAR:
			case ZEND_FETCH_W:
			case ZEND_FETCH_RW:
			case ZEND_FETCH_FUNC_ARG:
			case ZEND_FETCH_UNSET:
			case ZEND_TICKS:
			case ZEND_INIT_METHOD_CALL:
			case ZEND_INIT_STATIC_METHOD_CALL:
			case ZEND_INIT_USER_CALL:
			case ZEND_FE_RESET_RW:
			case ZEND_FE_FETCH_RW:
			case ZEND_INIT_DYNAMIC_CALL:
			case ZEND_MAKE_REF:
			case ZEND_USER_OPCODE:
			case ZEND_YIELD:
			case ZEND_YIELD_FROM:
			case ZEND_BIND_GLOBAL:
			case ZEND_BIND_LEXICAL:
			case ZEND_BIND_STATIC:
				frame_aliases_stable = false;
				break;
			default:
				break;
		}
		if (!frame_aliases_stable) {
			break;
		}
	}
	auto source_position_dominates = [&](uint32_t definition,
			uint32_t use) -> bool {
		if (definition >= source_op_array->last || use >= source_op_array->last
				|| source_ssa->cfg.blocks == nullptr
				|| source_ssa->cfg.map == nullptr) {
			return false;
		}
		const uint32_t definition_block = source_ssa->cfg.map[definition];
		int32_t use_block = static_cast<int32_t>(source_ssa->cfg.map[use]);
		if (definition_block >= source_ssa->cfg.blocks_count
				|| use_block < 0
				|| static_cast<uint32_t>(use_block)
					>= source_ssa->cfg.blocks_count) {
			return false;
		}
		if (definition_block == static_cast<uint32_t>(use_block)) {
			return definition <= use;
		}
		while (use_block >= 0
				&& static_cast<uint32_t>(use_block) != definition_block) {
			use_block = source_ssa->cfg.blocks[use_block].idom;
		}
		return use_block >= 0
			&& static_cast<uint32_t>(use_block) == definition_block;
	};
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count; ++instruction_index) {
		zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode != ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				|| !instruction.has_value_operation) {
			continue;
		}
		const uint32_t ssa_variable =
			instruction.value_operation.op1.ssa_variable_id;
		if (!zend_mir_id_is_valid(ssa_variable)
				|| ssa_variable >= static_cast<uint32_t>(source_ssa->vars_count)
				|| !frame_aliases_stable) {
			continue;
		}
		const zend_ssa_var &name_variable = source_ssa->vars[ssa_variable];
		const int32_t definition = name_variable.definition;
		if (name_variable.definition_phi != nullptr || definition < 0
				|| static_cast<uint32_t>(definition) >= source_op_array->last) {
			continue;
		}
		const zend_op &op = source_op_array->opcodes[
			static_cast<uint32_t>(definition)];
		const zend_ssa_op &ssa_op = source_ssa->ops[
			static_cast<uint32_t>(definition)];
		if (op.opcode != ZEND_ASSIGN || op.op2_type != IS_CONST
				|| op.op1_type != IS_CV
				|| ssa_op.op1_def != static_cast<int32_t>(ssa_variable)
				|| !source_position_dominates(
					static_cast<uint32_t>(definition),
					record.source_position_id)) {
			continue;
		}
		const uint32_t name_cv = EX_VAR_TO_NUM(op.op1.var);
		uint32_t name_definition_count = 0;
		bool name_has_phi = false;
		for (int32_t candidate = 0;
				candidate < source_ssa->vars_count; ++candidate) {
			const zend_ssa_var &name = source_ssa->vars[candidate];
			if (name.var != static_cast<int32_t>(name_cv)) {
				continue;
			}
			name_has_phi = name_has_phi || name.definition_phi != nullptr;
			if (name.definition >= 0) {
				++name_definition_count;
			}
		}
		if (name_has_phi || name_definition_count != 1) {
			continue;
		}
		const zval *literal = RT_CONSTANT(&op, op.op2);
		if (Z_TYPE_P(literal) != IS_STRING) {
			continue;
		}
		for (uint32_t cv = 0; cv < source_op_array->last_var; ++cv) {
			if (zend_string_equals(
					source_op_array->vars[cv], Z_STR_P(literal))) {
				uint32_t target_definition_count = 0;
				bool target_is_dominating_long = false;
				bool target_has_phi = false;
				for (int32_t candidate = 0;
						candidate < source_ssa->vars_count; ++candidate) {
					const zend_ssa_var &target = source_ssa->vars[candidate];
					if (target.var != static_cast<int32_t>(cv)) {
						continue;
					}
					target_has_phi = target_has_phi
						|| target.definition_phi != nullptr;
					if (target.definition < 0) {
						continue;
					}
					++target_definition_count;
					const uint32_t target_definition =
						static_cast<uint32_t>(target.definition);
					if (target_definition >= source_op_array->last) {
						continue;
					}
					const zend_op &target_op =
						source_op_array->opcodes[target_definition];
					const zend_ssa_op &target_ssa_op =
						source_ssa->ops[target_definition];
					if (target_op.opcode == ZEND_ASSIGN
							&& target_op.op2_type == IS_CONST
							&& target_ssa_op.op1_def == candidate
							&& Z_TYPE_P(RT_CONSTANT(
								&target_op, target_op.op2)) == IS_LONG
							&& source_position_dominates(
								target_definition,
								record.source_position_id)) {
						target_is_dominating_long = true;
					}
				}
				const bool target_is_stable =
					!target_has_phi && target_definition_count == 1;
				if (target_is_stable) {
					instruction.dynamic_fetch_cv_index = cv;
					instruction.dynamic_fetch_direct_long =
						target_is_dominating_long;
				}
				break;
			}
		}
	}
}

bool freeze_machine_references(
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag)
{
	if (plan == nullptr) {
		return true;
	}

	std::vector<zend_tpde_machine_reference> references;
	auto add_reference =
		[&](const zend_tpde_machine_reference &reference) -> uint32_t {
			for (uint32_t index = 0; index < references.size(); ++index) {
				const zend_tpde_machine_reference &candidate =
					references[index];
				if (candidate.kind == reference.kind
						&& candidate.base_value_id
							== reference.base_value_id
						&& candidate.index_value_id
							== reference.index_value_id
						&& candidate.stable_storage_or_layout_id
							== reference.stable_storage_or_layout_id
						&& candidate.scale == reference.scale
						&& candidate.displacement == reference.displacement
						&& candidate.access_width == reference.access_width) {
					return index;
				}
			}
			if (references.size() >= MAX_RECORDS) {
				return UINT32_MAX;
			}
			references.push_back(reference);
			return static_cast<uint32_t>(references.size() - 1);
		};
	auto binding_value_id =
		[&](const zend_tpde_source_value_binding &binding) {
			return binding.value_index >= 0
					&& static_cast<uint32_t>(binding.value_index)
						< plan->value_count
				? plan->values[
					static_cast<uint32_t>(binding.value_index)].id
				: ZEND_MIR_ID_INVALID;
		};
	auto operand_reference =
		[&](const zend_mir_source_operand_ref &operand,
				zend_mir_storage_id storage_id,
				const zend_tpde_source_value_binding &binding) {
			if (zend_mir_id_is_valid(storage_id)) {
				return add_reference({
					ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
					ZEND_MIR_ID_INVALID,
					ZEND_MIR_ID_INVALID,
					storage_id,
					1,
					static_cast<int64_t>(
						(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id)
							* sizeof(zval)),
					sizeof(zval),
				});
			}
			if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
				return add_reference({
					ZEND_TPDE_MACHINE_REFERENCE_LITERAL,
					binding_value_id(binding),
					ZEND_MIR_ID_INVALID,
					operand.index,
					sizeof(zval),
					0,
					sizeof(zval),
				});
			}
			return UINT32_MAX;
		};

	plan->observers_enabled_reference_index = add_reference({
		ZEND_TPDE_MACHINE_REFERENCE_CONTEXT_FIELD,
		ZEND_MIR_ID_INVALID,
		ZEND_MIR_ID_INVALID,
		static_cast<uint32_t>(offsetof(
			zend_native_execution_context, observers_enabled)),
		1,
		static_cast<int64_t>(offsetof(
			zend_native_execution_context, observers_enabled)),
		sizeof(bool),
	});
	if (plan->observers_enabled_reference_index == UINT32_MAX) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"machine-reference table exceeds the executable bound");
		return false;
	}

	/*
	 * Argument payload loads and safepoint materialization consume canonical
	 * locations even when no source opcode names the slot.  Freeze those
	 * locations from the value table itself so the adaptor never has to
	 * rediscover an address from the live op_array.
	 */
	for (uint32_t index = 0; index < plan->value_count; ++index) {
		const zend_tpde_value &value = plan->values[index];
		const zend_mir_storage_id storage_id =
			zend_mir_id_is_valid(value.canonical_storage_id)
				? value.canonical_storage_id
				: value.argument_index >= 0
					&& (plan->value_model_flags
						& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0
					? static_cast<zend_mir_storage_id>(
						value.argument_index)
					: ZEND_MIR_ID_INVALID;
		if (!zend_mir_id_is_valid(storage_id)) {
			continue;
		}
		if (add_reference({
				ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
				ZEND_MIR_ID_INVALID,
				ZEND_MIR_ID_INVALID,
				storage_id,
				1,
				static_cast<int64_t>(
					(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id)
						* sizeof(zval)),
				sizeof(zval),
			}) == UINT32_MAX) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine-reference table exceeds the executable bound");
			return false;
		}
	}

	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		instruction.source_op1_reference_index = UINT32_MAX;
		instruction.source_op2_reference_index = UINT32_MAX;
		instruction.source_result_reference_index = UINT32_MAX;
		instruction.source_auxiliary_reference_index = UINT32_MAX;
		instruction.operation_reference_index = UINT32_MAX;
		if (!instruction.has_value_operation) {
			continue;
		}
		const zend_mir_executable_value_ref &operation =
			instruction.value_operation;
		instruction.source_op1_reference_index = operand_reference(
			operation.op1, operation.op1_storage_id,
			instruction.source_op1_binding);
		instruction.source_op2_reference_index = operand_reference(
			operation.op2, operation.op2_storage_id,
			instruction.source_op2_binding);
		instruction.source_result_reference_index = operand_reference(
			operation.result, operation.result_storage_id,
			instruction.source_result_binding);
		instruction.source_auxiliary_reference_index = operand_reference(
			operation.auxiliary, operation.auxiliary_storage_id,
			instruction.source_auxiliary_binding);

		if ((operation.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
					|| operation.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN)
				&& operation.op2.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			instruction.operation_reference_index = add_reference({
				ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT,
				binding_value_id(instruction.source_op1_binding),
				binding_value_id(instruction.source_op2_binding),
				operation.extended_value & ~ZEND_FETCH_REF,
				1,
				0,
				sizeof(zval),
			});
		} else if (operation.opcode
					== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				|| operation.opcode
					== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM) {
			instruction.operation_reference_index = add_reference({
				ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT,
				binding_value_id(instruction.source_op1_binding),
				operation.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					? binding_value_id(instruction.source_op2_binding)
					: ZEND_MIR_ID_INVALID,
				0,
				sizeof(zval),
				0,
				sizeof(zval),
			});
		}
		if ((instruction.operation_reference_index == UINT32_MAX
					&& (((operation.opcode
							== ZEND_MIR_OPCODE_OBJECT_FETCH_R
						|| operation.opcode
							== ZEND_MIR_OPCODE_OBJECT_ASSIGN)
						&& operation.op2.kind
							== ZEND_MIR_SOURCE_OPERAND_LITERAL)
						|| operation.opcode
							== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
						|| operation.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM))
				|| references.size() >= MAX_RECORDS) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"unable to freeze a machine address reference");
			return false;
		}
	}

	plan->machine_reference_count =
		static_cast<uint32_t>(references.size());
	if (references.empty()) {
		return true;
	}
	plan->machine_references = static_cast<zend_tpde_machine_reference *>(
		std::malloc(references.size()
			* sizeof(*plan->machine_references)));
	if (plan->machine_references == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate the machine-reference table");
		return false;
	}
	std::memcpy(plan->machine_references, references.data(),
		references.size() * sizeof(*plan->machine_references));
	return true;
}

bool freeze_generator_resume_liveness(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	const zend_mir_value_view *value_model,
	zend_native_diagnostic *diag)
{
	if (source_op_array == nullptr
			|| (source_op_array->fn_flags & ZEND_ACC_GENERATOR) == 0) {
		return true;
	}

	std::vector<uint32_t> targets;
	if ((source_op_array->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK) != 0) {
		for (uint32_t i = 0;
				i < source_op_array->last_try_catch; ++i) {
			const zend_try_catch_element &region =
				source_op_array->try_catch_array[i];
			if (region.finally_op != 0
					&& region.finally_op < source_op_array->last
					&& region.finally_end < source_op_array->last) {
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

	if (value_model == nullptr
			|| value_model->contract_version != ZEND_MIR_W14_CONTRACT_VERSION
			|| value_model->suspend_live_value_count == nullptr
			|| value_model->suspend_live_value_at == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"generator MIR lacks canonical suspend liveness");
		return false;
	}
	const uint32_t live_count =
		value_model->suspend_live_value_count(value_model->context);
	if (!checked_count(live_count)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"canonical suspend liveness exceeds the executable bound");
		return false;
	}
	for (uint32_t index = 0; index < live_count; ++index) {
		zend_mir_suspend_live_value_ref live{};
		if (!value_model->suspend_live_value_at(
				value_model->context, index, &live)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"canonical suspend liveness is unreadable");
			return false;
		}
		const auto target = std::lower_bound(
			targets.begin(), targets.end(),
			live.target_source_position_id);
		const int32_t value_index =
			zend_tpde_value_index(plan, live.value_id);
		if (target == targets.end()
				|| *target != live.target_source_position_id
				|| value_index < 0
				|| plan->values[value_index].canonical_storage_id
					!= live.storage_id) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"canonical suspend liveness does not match the machine plan");
			return false;
		}
		const uint32_t resume =
			static_cast<uint32_t>(target - targets.begin());
		/*
		 * The canonical table describes semantic liveness, including values
		 * whose authoritative copy already lives in the generator frame.
		 * Only register-authoritative values need a TPDE resume operand.
		 * Keeping this selection at the frozen machine-plan boundary avoids
		 * rebuilding SSA liveness in the adaptor while preserving boxed
		 * frame values without manufacturing unused TPDE definitions.
		 */
		if (plan->generator_resume_live_word_count == 0) {
			continue;
		}
		uint64_t *words =
			plan->generator_resume_live_values
				+ static_cast<size_t>(resume)
					* plan->generator_resume_live_word_count;
		const uint32_t value = static_cast<uint32_t>(value_index);
		const zend_tpde_value &semantic_value = plan->values[value];
		if (!semantic_value.constant
				&& zend_tpde_machine_value_is_register_authoritative(
					semantic_value.machine_kind)) {
			words[value / 64] |= uint64_t{1} << (value % 64);
		}
	}
	return true;
}

bool freeze_statepoint_materializations(
	zend_tpde_plan *plan,
	const zend_mir_view *view,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag)
{
	const uint32_t frame_count = view->frame_state_count(view->context);
	std::vector<zend_mir_frame_state_ref> frames(frame_count);
	std::vector<zend_tpde_materialization> materializations;
	std::vector<zend_mir_storage_id> lazy_scalar_storages;
	std::vector<uint32_t> block_instruction_begin(
		plan->block_count, plan->instruction_count);
	std::vector<uint32_t> block_instruction_end(plan->block_count, 0);

	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const int32_t block_index = zend_tpde_block_index(
			plan, instruction.record.block_id);
		if (block_index >= 0) {
			const uint32_t block = static_cast<uint32_t>(block_index);
			block_instruction_begin[block] =
				std::min(block_instruction_begin[block], index);
			block_instruction_end[block] =
				std::max(block_instruction_end[block], index + 1);
		}
		const zend_mir_storage_id storage_id =
			instruction.zval_store_lazy_scalar
				? instruction.zval_store_storage_id
				: instruction.mutation_lazy_scalar
					? instruction.mutation_storage_id
					: ZEND_MIR_ID_INVALID;
		if (zend_mir_id_is_valid(storage_id)) {
			lazy_scalar_storages.push_back(storage_id);
		}
	}
	std::ranges::sort(lazy_scalar_storages);
	lazy_scalar_storages.erase(
		std::unique(lazy_scalar_storages.begin(), lazy_scalar_storages.end()),
		lazy_scalar_storages.end());

	/*
	 * Resolve the register value that owns a canonical scalar slot at an
	 * observable boundary.  This is a sparse reaching-definition query over
	 * the frozen CFG, not a second block-by-value dataflow matrix.  Exact
	 * scalar PHIs terminate loop queries at their natural merge point.
	 */
	struct reaching_scalar_definition {
		int32_t value_index;
		int32_t instruction_index;

		bool operator==(const reaching_scalar_definition &other) const {
			return value_index == other.value_index
				&& instruction_index == other.instruction_index;
		}
	};
	constexpr reaching_scalar_definition no_reaching_scalar{-1, -1};
	constexpr reaching_scalar_definition cyclic_reaching_scalar{-2, -2};
	auto reaching_scalar = [&](uint32_t boundary_index,
			zend_mir_storage_id storage_id)
			-> reaching_scalar_definition {
		if (boundary_index >= plan->instruction_count) {
			return no_reaching_scalar;
		}
		const int32_t boundary_block = zend_tpde_block_index(
			plan, plan->instructions[boundary_index].record.block_id);
		if (boundary_block < 0) {
			return no_reaching_scalar;
		}
		std::vector<uint8_t> visiting(plan->block_count, 0);
		std::vector<uint8_t> memoized(plan->block_count, 0);
		std::vector<reaching_scalar_definition> memo(
			plan->block_count, no_reaching_scalar);
		auto resolve = [&](uint32_t block_index, uint32_t before,
				auto &&self) -> reaching_scalar_definition {
			if (block_index >= plan->block_count) {
				return no_reaching_scalar;
			}
			const bool whole_block = before == plan->instruction_count;
			if (whole_block && memoized[block_index] != 0) {
				return memo[block_index];
			}
			if (visiting[block_index] != 0) {
				return cyclic_reaching_scalar;
			}
			const uint32_t begin = block_instruction_begin[block_index];
			const uint32_t end = std::min(
				before, block_instruction_end[block_index]);
			for (uint32_t cursor = end; cursor > begin; --cursor) {
				const uint32_t instruction_index = cursor - 1;
				const zend_tpde_instruction &candidate =
					plan->instructions[instruction_index];
				if (candidate.record.block_id
						!= plan->block_ids[block_index]) {
					continue;
				}
				if (candidate.record.opcode
							== ZEND_MIR_OPCODE_ZVAL_STORE
						&& candidate.zval_store_storage_id
							== storage_id
						&& candidate.zval_store_lazy_scalar
						&& candidate.operand_count >= 1) {
					const reaching_scalar_definition found{
						zend_tpde_value_index(
							plan, zend_tpde_operand_at(
								plan, &candidate, 0)),
						-1,
					};
					if (whole_block) {
						memo[block_index] = found;
						memoized[block_index] = 1;
					}
					return found;
				}
				if (candidate.has_value_operation
						&& candidate.record.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN_REF
						&& (candidate.value_operation.op1_storage_id
								== storage_id
							|| candidate.value_operation.op2_storage_id
								== storage_id)) {
					return no_reaching_scalar;
				}
				if (candidate.has_value_operation
						&& (candidate.record.opcode
								== ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
							|| candidate.record.opcode
								== ZEND_MIR_OPCODE_VALUE_INCDEC)
						&& candidate.value_operation.op1_storage_id
							== storage_id) {
					if (!candidate.mutation_lazy_scalar
							|| candidate.mutation_storage_id != storage_id
							|| candidate.value_operation
								.op1_definition_ssa_variable_id_plus_one == 0) {
						return no_reaching_scalar;
					}
					const reaching_scalar_definition found{
						zend_tpde_value_index(
							plan, zend_mir_value_from_original_ssa(
								candidate.value_operation
									.op1_definition_ssa_variable_id_plus_one
									- 1)),
						static_cast<int32_t>(instruction_index),
					};
					if (whole_block) {
						memo[block_index] = found;
						memoized[block_index] = 1;
					}
					return found;
				}
				if (!zend_mir_id_is_valid(
							candidate.record.result_id)) {
					continue;
				}
				const int32_t result = zend_tpde_value_index(
					plan, candidate.record.result_id);
				if (result >= 0) {
					const zend_tpde_value &value =
						plan->values[static_cast<uint32_t>(result)];
					const bool lazy_scalar_join =
						(candidate.record.opcode == ZEND_MIR_OPCODE_PHI
							|| candidate.record.opcode == ZEND_MIR_OPCODE_COPY)
						&& candidate.record.representation
							== ZEND_MIR_REPRESENTATION_ZVAL;
					if (value.canonical_storage_id == storage_id
							&& (lazy_scalar_join
								|| (zend_mir_scalar_type_is_exact(
										value.exact_type)
									&& value.exact_type
										!= ZEND_MIR_SCALAR_TYPE_NULL))
							&& !value.constant
							&& !value.canonical_alias_observable) {
						const reaching_scalar_definition found{result, -1};
						if (whole_block) {
							memo[block_index] = found;
							memoized[block_index] = 1;
						}
						return found;
					}
				}
			}

			visiting[block_index] = 1;
			const uint32_t predecessor_begin =
				plan->block_predecessor_offsets[block_index];
			const uint32_t predecessor_end =
				plan->block_predecessor_offsets[block_index + 1];
			reaching_scalar_definition merged = no_reaching_scalar;
			bool saw_cycle = false;
			for (uint32_t predecessor = predecessor_begin;
					predecessor < predecessor_end; ++predecessor) {
				const reaching_scalar_definition incoming = self(
					plan->block_predecessors[predecessor],
					plan->instruction_count, self);
				if (incoming == cyclic_reaching_scalar) {
					saw_cycle = true;
					continue;
				}
				if (incoming.value_index < 0
						|| (merged.value_index >= 0
							&& !(incoming == merged))) {
					merged = no_reaching_scalar;
					break;
				}
				merged = incoming;
			}
			visiting[block_index] = 0;
			if (merged.value_index < 0 && saw_cycle) {
				return cyclic_reaching_scalar;
			}
			if (whole_block) {
				memo[block_index] = merged;
				memoized[block_index] = 1;
			}
			return merged;
		};
		const reaching_scalar_definition resolved = resolve(
			static_cast<uint32_t>(boundary_block),
			boundary_index, resolve);
		return resolved == cyclic_reaching_scalar
			? no_reaching_scalar : resolved;
	};

	for (uint32_t index = 0; index < frame_count; ++index) {
		if (!view->frame_state_at(view->context, index, &frames[index])) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"materialization frame-state table is unreadable");
			return false;
		}
	}
	const uint32_t frame_index_capacity = frame_count == 0
		? 0 : id_index_capacity(frame_count);
	std::vector<zend_tpde_id_index_entry> frame_index(
		frame_index_capacity, {ZEND_MIR_ID_INVALID, 0});
	for (uint32_t index = 0; index < frame_count; ++index) {
		if (zend_mir_id_is_valid(frames[index].id)
				&& id_index_find(frame_index.data(), frame_index_capacity,
					frames[index].id) < 0) {
			id_index_insert(frame_index.data(), frame_index_capacity,
				frames[index].id, index);
		}
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		const bool observable_boundary =
			(record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			|| record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
			|| record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			|| record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
			|| instruction.runtime_helper != ZEND_NATIVE_HELPER_COUNT
			|| instruction.source_effect != 0
			|| instruction.debug_probe)
			&& !(record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& instruction.zval_store_lazy_scalar);

		instruction.materialization_offset =
			static_cast<uint32_t>(materializations.size());
		instruction.materialization_count = 0;
		if (!observable_boundary) {
			continue;
		}

		auto duplicate_storage = [&](zend_mir_storage_id storage_id) {
			for (uint32_t materialization_index =
					instruction.materialization_offset;
					materialization_index < materializations.size();
					++materialization_index) {
				if (materializations[materialization_index].storage_id
						== storage_id) {
					return true;
				}
			}
			return false;
		};
		auto append_materialization = [&](
				uint32_t value_index,
				zend_mir_storage_id storage_id,
				zend_tpde_machine_value_kind machine_kind,
				int32_t source_value_index,
				int32_t source_definition_instruction_index) {
			if (!zend_mir_id_is_valid(storage_id)
					|| duplicate_storage(storage_id)) {
				return;
			}
			materializations.push_back({
				value_index,
				storage_id,
				machine_kind,
				source_value_index,
				source_definition_instruction_index,
			});
			++instruction.materialization_count;
		};
		auto append_source_materialization = [&](
				zend_mir_storage_id storage_id,
				zend_tpde_machine_value_kind machine_kind,
				int32_t source_value_index,
				int32_t source_definition_instruction_index) {
			for (uint32_t materialization_index =
					instruction.materialization_offset;
					materialization_index < materializations.size();
					++materialization_index) {
				zend_tpde_materialization &materialization =
					materializations[materialization_index];
				if (materialization.storage_id == storage_id) {
					materialization = {
						UINT32_MAX, storage_id, machine_kind,
						source_value_index,
						source_definition_instruction_index,
					};
					return;
				}
			}
			append_materialization(
				UINT32_MAX, storage_id, machine_kind,
				source_value_index, source_definition_instruction_index);
		};
		auto source_scalar_definition = [&](zend_mir_storage_id storage_id) {
			for (uint32_t cursor = index; cursor > 0; --cursor) {
				const uint32_t candidate_index = cursor - 1;
				const zend_tpde_instruction &candidate =
					plan->instructions[candidate_index];
				const zend_mir_instruction_record candidate_record =
					zend_tpde_instruction_record_at(plan, &candidate);
				if (candidate_record.opcode == ZEND_MIR_OPCODE_CONSTANT
						|| candidate_record.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
					continue;
				}
				if (candidate_record.opcode
						== ZEND_MIR_OPCODE_VALUE_UNARY_OP
						&& candidate.has_value_operation
						&& candidate.value_operation.result_storage_id
							== storage_id
						&& candidate.value_operation.source_opcode
							== ZEND_STRLEN) {
					return static_cast<int32_t>(candidate_index);
				}
				/*
				 * A source result is only a valid materialization input for its
				 * direct consumer.  Temporary storage ids are reused, so looking
				 * through another real instruction can bind an unrelated earlier
				 * producer and create an invalid machine def-use edge.
				 */
				return int32_t{-1};
			}
			return int32_t{-1};
		};

		if (zend_mir_id_is_valid(record.frame_state_id)) {
			const int32_t frame_index_value = id_index_find(
				frame_index.data(), frame_index_capacity,
				record.frame_state_id);
			const zend_mir_frame_state_ref *frame = frame_index_value < 0
				? nullptr
				: &frames[static_cast<uint32_t>(frame_index_value)];
			if (frame == nullptr || frame->function_id != plan->function.id
					|| frame->slots.offset
						> view->frame_slot_count(view->context)
					|| frame->slots.count
						> view->frame_slot_count(view->context)
							- frame->slots.offset) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"observable instruction lacks its materialization frame");
				return false;
			}

			const bool function_entry =
				record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				&& frame->safepoint_class
					== ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY;
			if (!function_entry) {
				for (uint32_t slot_index = 0;
						slot_index < frame->slots.count; ++slot_index) {
					zend_mir_frame_slot_ref slot{};
					if (!view->frame_slot_at(view->context,
							frame->slots.offset + slot_index, &slot)) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"materialization frame slot is unreadable");
						return false;
					}
					if (slot.materialization
								!= ZEND_MIR_MATERIALIZATION_MATERIALIZED
							|| !zend_mir_id_is_valid(slot.value_id)) {
						continue;
					}
					const int32_t value_index =
						zend_tpde_value_index(plan, slot.value_id);
					if (value_index < 0) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"materialization references an unknown value");
						return false;
					}
					const zend_tpde_value &value =
						plan->values[static_cast<uint32_t>(value_index)];
					if (value.slot_state
								!= ZEND_TPDE_CANONICAL_SLOT_DIRTY
							|| !zend_mir_id_is_valid(
								value.canonical_storage_id)
							|| value.constant) {
						continue;
					}
					append_materialization(
						static_cast<uint32_t>(value_index),
						value.canonical_storage_id,
						value.machine_kind, value_index, -1);
				}
			}
		}

		auto append_reaching_scalar = [&](zend_mir_storage_id storage_id) {
			const reaching_scalar_definition reaching =
				reaching_scalar(index, storage_id);
			if (reaching.value_index < 0) {
				return;
			}
			const zend_tpde_value &value =
				plan->values[
					static_cast<uint32_t>(reaching.value_index)];
			append_materialization(
				reaching.instruction_index < 0
					? static_cast<uint32_t>(reaching.value_index)
					: UINT32_MAX,
				storage_id, value.machine_kind,
				reaching.value_index, reaching.instruction_index);
		};
		for (zend_mir_storage_id storage_id : lazy_scalar_storages) {
			append_reaching_scalar(storage_id);
		}
		/*
		 * A native generator resumes register-authoritative values from their
		 * canonical Zend slots.  Publish the exact semantic suspend-live values
		 * before the helper transfers control back to the caller.  Sparse
		 * reaching definitions alone are insufficient here: a loop value may be
		 * copied from a PHI solely for use after the yield and therefore have no
		 * intervening ZVAL_STORE (the induction value is a common example).
		 */
		if (instruction.has_value_operation
				&& record.source_position_id != UINT32_MAX
				&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
					|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
					|| record.opcode
						== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
			const uint32_t target = record.source_position_id + 1;
			for (uint32_t resume = 0;
					resume < plan->generator_resume_count; ++resume) {
				if (plan->generator_resume_targets[resume] != target) {
					continue;
				}
				for (uint32_t value_index = 0;
						value_index < plan->value_count; ++value_index) {
					if (!zend_tpde_generator_resume_value_live(
							plan, resume, value_index)) {
						continue;
					}
					const zend_tpde_value &value =
						plan->values[value_index];
					append_materialization(
						value_index, value.canonical_storage_id,
						value.machine_kind, value_index, -1);
				}
				break;
			}
		}

		/*
		 * A local dynamic read may select any CV from its runtime name. In
		 * canonical-location modules frame states omit duplicated slots, so the
		 * cold helper boundary must publish every register-authoritative CV that
		 * has a sparse reaching definition, not only the explicit name operand.
		 */
		if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				&& instruction.has_value_operation
				&& instruction.value_operation.extended_value
					== ZEND_FETCH_LOCAL) {
			for (zend_mir_storage_id storage_id = 0;
					storage_id < source_op_array->last_var; ++storage_id) {
				append_reaching_scalar(storage_id);
			}
		}

		/*
		 * Canonical-location modules do not duplicate every source slot in
		 * each frame state. Runtime helpers still read explicit operands from
		 * the Zend frame, so publish authoritative inputs at that boundary.
		 * This also covers private typed-call results without a persistent MIR
		 * result identity.
		 */
		if (instruction.runtime_helper != ZEND_NATIVE_HELPER_COUNT
				&& instruction.has_value_operation) {
			const struct {
				zend_tpde_source_value_binding binding;
				zend_mir_storage_id storage_id;
			} inputs[] = {
				{instruction.source_op1_binding,
					instruction.value_operation.op1_storage_id},
				{instruction.source_op2_binding,
					instruction.value_operation.op2_storage_id},
				{instruction.source_auxiliary_binding,
					instruction.value_operation.auxiliary_storage_id},
			};
			for (const auto &input : inputs) {
				if (!zend_mir_id_is_valid(input.storage_id)) {
					continue;
				}
				const int32_t source_definition =
					source_scalar_definition(input.storage_id);
				if (source_definition >= 0) {
					append_source_materialization(
						input.storage_id,
						ZEND_TPDE_MACHINE_VALUE_I64,
						input.binding.value_index, source_definition);
					continue;
				}
				if (input.binding.definition_instruction_index >= 0) {
					append_materialization(
						UINT32_MAX, input.storage_id,
						ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
						input.binding.value_index,
						input.binding.definition_instruction_index);
					continue;
				}
				if (input.binding.value_index >= 0) {
					const uint32_t value_index =
						static_cast<uint32_t>(input.binding.value_index);
					if (value_index >= plan->value_count) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"source materialization references an unknown value");
						return false;
					}
					const zend_tpde_value &value =
						plan->values[value_index];
					if (!value.constant && value.register_authoritative
							&& value.slot_state
								== ZEND_TPDE_CANONICAL_SLOT_DIRTY) {
						append_materialization(
							value_index, input.storage_id,
							value.machine_kind,
							input.binding.value_index,
							input.binding.definition_instruction_index);
					}
				}
			}
		}
		/*
		 * Direct call helpers consume their source-backed arguments from the
		 * canonical Zend frame. A producer may nevertheless keep a newer
		 * pointer or boxed value in TPDE registers (for example FETCH_DIM_W
		 * immediately followed by SEND_REF). Publish precisely those dirty
		 * argument slots before entering the runtime boundary.
		 */
		if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					|| record.opcode
						== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
				&& instruction.call_argument_offset
					<= plan->call_argument_count
				&& instruction.call_argument_count
					<= plan->call_argument_count
						- instruction.call_argument_offset) {
			if (instruction.call_argument_count != 0
					&& plan->call_argument_bindings == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"call materialization binding table is missing");
				return false;
			}
			for (uint32_t argument_index = 0;
					argument_index < instruction.call_argument_count;
					++argument_index) {
				const uint32_t frozen_index =
					instruction.call_argument_offset + argument_index;
				zend_mir_call_argument_ref argument{};
				if (!zend_tpde_call_argument_at(
						plan, frozen_index, &argument)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"call materialization argument is unreadable");
					return false;
				}
				const zend_mir_storage_id storage_id =
					source_descriptor_storage(
						source_op_array, argument.source_operand);
				if (!zend_mir_id_is_valid(storage_id)) {
					continue;
				}
				const zend_tpde_source_value_binding &binding =
					plan->call_argument_bindings[frozen_index];
				const int32_t source_definition =
					source_scalar_definition(storage_id);
				if (source_definition >= 0) {
					append_source_materialization(
						storage_id,
						ZEND_TPDE_MACHINE_VALUE_I64,
						binding.value_index, source_definition);
					continue;
				}
				if (binding.definition_instruction_index >= 0) {
					append_materialization(
						UINT32_MAX, storage_id,
						ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
						binding.value_index,
						binding.definition_instruction_index);
					continue;
				}
				if (binding.value_index < 0
						|| static_cast<uint32_t>(binding.value_index)
							>= plan->value_count) {
					continue;
				}
				const uint32_t value_index =
					static_cast<uint32_t>(binding.value_index);
				const zend_tpde_value &value =
					plan->values[value_index];
				if (!value.constant && value.register_authoritative
						&& value.slot_state
							== ZEND_TPDE_CANONICAL_SLOT_DIRTY) {
					append_materialization(
						value_index, storage_id, value.machine_kind,
						binding.value_index,
						binding.definition_instruction_index);
				}
			}
		}
	}
	if (materializations.size() > MAX_RECORDS) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"statepoint materialization plan exceeds executable bound");
		return false;
	}
	plan->materialization_count =
		static_cast<uint32_t>(materializations.size());
	if (materializations.empty()) {
		return true;
	}
	plan->materializations = static_cast<zend_tpde_materialization *>(
		std::malloc(materializations.size()
			* sizeof(*plan->materializations)));
	if (plan->materializations == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate statepoint materialization plan");
		return false;
	}
	std::memcpy(plan->materializations, materializations.data(),
		materializations.size() * sizeof(*plan->materializations));
	return true;
}

void freeze_machine_control_flow(zend_tpde_plan *plan)
{
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record &record = instruction.record;
		uint8_t flags = 0;

		if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
			if (instruction.runtime_helper
					== ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW) {
				flags |= ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			}
		} else if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			if (instruction.direct_call != nullptr
					&& (instruction.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0) {
				flags |= ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			}
			if (instruction.component_target_index != UINT32_MAX) {
				flags |=
					ZEND_TPDE_MACHINE_CONTROL_FLOW_TYPED_COMPONENT_CALL;
			}
		} else if (instruction.has_value_operation) {
			const zend_mir_executable_value_ref &operation =
				instruction.value_operation;
			switch (operation.opcode) {
				case ZEND_MIR_OPCODE_VALUE_ASSIGN:
					if (operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV) {
						flags |=
							ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					}
					break;
				case ZEND_MIR_OPCODE_VALUE_UNARY_OP: {
					flags |=
						ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					break;
				}
				case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
				case ZEND_MIR_OPCODE_VALUE_FREE:
				case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
				case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
				case ZEND_MIR_OPCODE_VALUE_INCDEC:
				case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
				case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
				case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
				case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
				case ZEND_MIR_OPCODE_OBJECT_FETCH_R:
				case ZEND_MIR_OPCODE_OBJECT_ASSIGN:
				case ZEND_MIR_OPCODE_DYNAMIC_FETCH_R:
					flags |=
						ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
					break;
				default:
					break;
			}
		}

		const bool branch_shape =
			instruction.has_value_operation
			&& (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH)
			&& (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				? instruction.value_operation.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				: instruction.value_operation.opcode == record.opcode);
		if (branch_shape) {
			flags |= ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH;
			const bool follows_boolean_unary =
				record.source_position_id > 0
				&& record.source_position_id < plan->source_opcode_count
				&& plan->source_opcodes != nullptr
				&& (plan->source_opcodes[
						record.source_position_id - 1].opcode == ZEND_BOOL
					|| plan->source_opcodes[
						record.source_position_id - 1].opcode == ZEND_BOOL_NOT);
			if ((record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH)
					&& instruction.value_operation.opcode
						== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& !follows_boolean_unary
			&& (instruction.value_operation.source_opcode
						== ZEND_JMPZ
					|| instruction.value_operation.source_opcode
						== ZEND_JMPNZ)) {
				flags |=
					ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH;
			}
		}
		instruction.machine_control_flow_flags = flags;
	}
}

bool freeze_entry_undef_temporaries(
	zend_tpde_plan *plan,
	const zend_op_array *source_op_array,
	zend_native_diagnostic *diag)
{
	const zend_op_array *op_array = source_op_array;
	if (op_array == nullptr || op_array->T == 0) {
		return true;
	}
	if (op_array->last_live_range != 0 && op_array->live_range == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"source live-range table is unreadable");
		return false;
	}

	std::vector<uint8_t> required(op_array->T);
	for (uint32_t index = 0; index < op_array->last_live_range; ++index) {
		const zend_live_range &range = op_array->live_range[index];
		const uint32_t kind = range.var & ZEND_LIVE_MASK;
		const uint32_t physical_slot =
			EX_VAR_TO_NUM(range.var & ~ZEND_LIVE_MASK);
		if (kind > ZEND_LIVE_NEW
				|| physical_slot < static_cast<uint32_t>(op_array->last_var)
				|| physical_slot
					>= static_cast<uint32_t>(op_array->last_var)
						+ op_array->T
				|| range.start >= range.end
				|| range.end > op_array->last) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"source live range is outside the executable frame");
			return false;
		}
		required[physical_slot
			- static_cast<uint32_t>(op_array->last_var)] = 1;
	}
	const uint32_t first_temporary =
		static_cast<uint32_t>(op_array->last_var);
	const uint64_t temporary_limit =
		static_cast<uint64_t>(first_temporary) + op_array->T;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		if (instruction.record.opcode != ZEND_MIR_OPCODE_ZVAL_STORE) {
			continue;
		}
		const zend_mir_storage_id storage =
			instruction.zval_store_storage_id;
		if (!zend_mir_id_is_valid(storage)
				|| static_cast<uint64_t>(storage) >= temporary_limit) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"zval store destination is outside the executable frame");
			return false;
		}
		if (storage < first_temporary) {
			continue;
		}
		required[storage - first_temporary] = 1;
	}
	/*
	 * Explicit value helpers produce canonical Zend results and deliberately
	 * require a fresh destination before they allocate, invoke user code or
	 * consume an operand.  Keep those actual helper destinations valid without
	 * paying the old cost of clearing every temporary in the frame.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		if (!instruction.has_value_operation
				|| instruction.runtime_helper == ZEND_NATIVE_HELPER_COUNT
				|| !zend_tpde_helper_has_explicit_operands(
					instruction.runtime_helper)) {
			continue;
		}
		const zend_mir_storage_id storage =
			instruction.value_operation.result_storage_id;
		if (!zend_mir_id_is_valid(storage)) {
			continue;
		}
		if (static_cast<uint64_t>(storage) >= temporary_limit) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"value helper result is outside the temporary frame");
			return false;
		}
		if (storage >= first_temporary) {
			required[storage - first_temporary] = 1;
		}
	}
	if (plan->user_opcode_callbacks
			&& plan->user_opcode_source_operations != nullptr) {
		for (uint32_t source = 0;
				source < plan->user_opcode_source_operation_count; ++source) {
			if (zend_get_user_opcode_handler(
					op_array->opcodes[source].opcode) == nullptr) {
				continue;
			}
			const zend_mir_source_operand_ref &result =
				plan->user_opcode_source_operations[source].result;
			if ((result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
						|| result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
					&& (result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
						|| result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
					&& result.index < op_array->T) {
				required[result.index] = 1;
			}
		}
	}

	std::vector<uint32_t> indices;
	indices.reserve(op_array->T);
	for (uint32_t index = 0; index < op_array->T; ++index) {
		if (required[index] != 0) {
			indices.push_back(index);
		}
	}
	plan->entry_undef_temporary_count =
		static_cast<uint32_t>(indices.size());
	if (indices.empty()) {
		return true;
	}
	plan->entry_undef_temporary_indices = static_cast<uint32_t *>(
		std::malloc(indices.size()
			* sizeof(*plan->entry_undef_temporary_indices)));
	if (plan->entry_undef_temporary_indices == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate selective temporary initialization plan");
		return false;
	}
	std::memcpy(plan->entry_undef_temporary_indices, indices.data(),
		indices.size() * sizeof(*plan->entry_undef_temporary_indices));
	return true;
}

static void destroy_machine_cfg(zend_tpde_machine_cfg *cfg);

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
	for (uint32_t index = 0;
			index < plan->source_multi_branch_case_count; ++index) {
		std::free(plan->source_multi_branch_cases[index].string_key);
	}
	std::free(plan->block_ids);
	std::free(plan->block_index);
	std::free(plan->block_successor_offsets);
	std::free(plan->block_successors);
	std::free(plan->block_predecessor_offsets);
	std::free(plan->block_predecessors);
	std::free(plan->values);
	std::free(plan->argument_value_indices);
	std::free(plan->argument_abi);
	std::free(plan->typed_component_call_eligible);
	std::free(plan->effect_closed_inline_eligible);
	destroy_machine_cfg(&plan->entry_machine_cfg);
	destroy_machine_cfg(&plan->typed_body_machine_cfg);
	std::free(plan->value_index);
	std::free(plan->instructions);
	std::free(plan->instruction_operands);
	std::free(plan->instruction_operand_transports);
	std::free(plan->instruction_index);
	std::free(plan->value_definition_instructions);
	std::free(plan->source_value_definition_instructions);
	std::free(plan->value_consumer_offsets);
	std::free(plan->value_consumers);
	std::free(plan->entry_value_required);
	std::free(plan->typed_body_value_required);
	std::free(plan->source_opcodes);
	std::free(plan->source_multi_branches);
	std::free(plan->source_multi_branch_cases);
	std::free(plan->source_opcode_block_indices);
	std::free(plan->source_opcode_is_data);
	std::free(plan->source_block_starts);
	std::free(plan->source_block_ends);
	std::free(plan->source_call_phases);
	std::free(plan->compiled_variables_used);
	std::free(plan->call_site_instruction_index);
	std::free(plan->call_sites);
	std::free(plan->call_target_index);
	std::free(plan->call_arguments);
	std::free(plan->call_argument_bindings);
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
	std::free(plan->materializations);
	std::free(plan->machine_references);
	std::free(plan->entry_undef_temporary_indices);
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

	plan->source_ssa_variable_count =
		source_ssa != nullptr && source_ssa->vars_count > 0
			? static_cast<uint32_t>(source_ssa->vars_count) : 0;
	plan->source_opcode_count =
		source_op_array != nullptr ? source_op_array->last : 0;
	plan->source_frame_variable_count =
		source_op_array != nullptr ? source_op_array->last_var : 0;
	plan->source_temporary_count =
		source_op_array != nullptr ? source_op_array->T : 0;
	if (plan->source_opcode_count != 0) {
		plan->source_opcodes = static_cast<zend_tpde_source_opcode *>(
			std::calloc(plan->source_opcode_count,
				sizeof(*plan->source_opcodes)));
		if (plan->source_opcodes == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze source opcode metadata");
			return false;
		}
		for (uint32_t index = 0;
				index < plan->source_opcode_count; ++index) {
			const zend_op &source = source_op_array->opcodes[index];
			zend_tpde_source_opcode &frozen = plan->source_opcodes[index];
			frozen.opcode = source.opcode;
			frozen.op1_type = source.op1_type;
			frozen.op2_type = source.op2_type;
			frozen.result_type = source.result_type;
			frozen.op1_var =
				source.op1_type == IS_CV || source.op1_type == IS_VAR
					|| source.op1_type == IS_TMP_VAR
				? source.op1.var : UINT32_MAX;
			frozen.op2_var =
				source.op2_type == IS_CV || source.op2_type == IS_VAR
					|| source.op2_type == IS_TMP_VAR
				? source.op2.var : UINT32_MAX;
			frozen.result_var =
				source.result_type == IS_CV
					|| source.result_type == IS_VAR
					|| source.result_type == IS_TMP_VAR
				? source.result.var : UINT32_MAX;
			frozen.extended_value = source.extended_value;
		}
		plan->source_multi_branches =
			static_cast<zend_tpde_source_multi_branch *>(std::calloc(
				plan->source_opcode_count,
				sizeof(*plan->source_multi_branches)));
		if (plan->source_multi_branches == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze source multiway metadata");
			return false;
		}
		uint64_t branch_case_count = 0;
		for (uint32_t index = 0;
				index < plan->source_opcode_count; ++index) {
			const zend_op &source = source_op_array->opcodes[index];
			if ((source.opcode != ZEND_SWITCH_LONG
						&& source.opcode != ZEND_SWITCH_STRING
						&& source.opcode != ZEND_MATCH)
					|| source.op2_type != IS_CONST) {
				continue;
			}
			const zval *jump_table = RT_CONSTANT(&source, source.op2);
			if (Z_TYPE_P(jump_table) != IS_ARRAY) {
				continue;
			}
			branch_case_count +=
				zend_hash_num_elements(Z_ARRVAL_P(jump_table));
			if (branch_case_count > MAX_RECORDS) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source multiway case table exceeds executable bound");
				return false;
			}
		}
		plan->source_multi_branch_case_count =
			static_cast<uint32_t>(branch_case_count);
		if (branch_case_count != 0) {
			plan->source_multi_branch_cases =
				static_cast<zend_tpde_multi_branch_case *>(std::calloc(
					branch_case_count,
					sizeof(*plan->source_multi_branch_cases)));
			if (plan->source_multi_branch_cases == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
					"unable to freeze source multiway cases");
				return false;
			}
		}
		uint32_t case_offset = 0;
		for (uint32_t index = 0;
				index < plan->source_opcode_count; ++index) {
			const zend_op &source = source_op_array->opcodes[index];
			if ((source.opcode != ZEND_SWITCH_LONG
						&& source.opcode != ZEND_SWITCH_STRING
						&& source.opcode != ZEND_MATCH)
					|| source.op2_type != IS_CONST) {
				continue;
			}
			const zval *jump_table = RT_CONSTANT(&source, source.op2);
			if (Z_TYPE_P(jump_table) != IS_ARRAY) {
				continue;
			}
			zend_tpde_source_multi_branch &branch =
				plan->source_multi_branches[index];
			branch.case_offset = case_offset;
			branch.case_count =
				zend_hash_num_elements(Z_ARRVAL_P(jump_table));
			branch.constant_successor = UINT32_MAX;
			branch.source_opcode = source.opcode;
			branch.default_target = zend_tpde_relative_source_target(
				source_op_array, index,
				static_cast<zend_long>(source.extended_value));
			branch.fallback_target =
				index + 1 < plan->source_opcode_count
					? index + 1 : UINT32_MAX;
			branch.valid = branch.default_target != UINT32_MAX
				&& (source.opcode == ZEND_MATCH
					|| branch.fallback_target != UINT32_MAX);
			zend_ulong numeric_key;
			zend_string *string_key;
			zval *jump_value;
			const zval *constant_operand =
				source.opcode == ZEND_MATCH && source.op1_type == IS_CONST
					? RT_CONSTANT(&source, source.op1) : nullptr;
			uint32_t branch_case_index = 0;
			ZEND_HASH_FOREACH_KEY_VAL(
					Z_ARRVAL_P(jump_table),
					numeric_key, string_key, jump_value) {
				zend_tpde_multi_branch_case &frozen_case =
					plan->source_multi_branch_cases[case_offset++];
				if (Z_TYPE_P(jump_value) != IS_LONG) {
					branch.valid = false;
					branch_case_index++;
					continue;
				}
				if (constant_operand != nullptr
						&& branch.constant_successor == UINT32_MAX
						&& ((string_key == nullptr
								&& Z_TYPE_P(constant_operand) == IS_LONG
								&& Z_LVAL_P(constant_operand)
									== static_cast<zend_long>(numeric_key))
							|| (string_key != nullptr
								&& Z_TYPE_P(constant_operand) == IS_STRING
								&& zend_string_equals(
									Z_STR_P(constant_operand), string_key)))) {
					branch.constant_successor = branch_case_index;
				}
				frozen_case.target = zend_tpde_relative_source_target(
					source_op_array, index, Z_LVAL_P(jump_value));
				if (frozen_case.target == UINT32_MAX) {
					branch.valid = false;
				}
				if (string_key == nullptr) {
					frozen_case.integer_key =
						static_cast<int64_t>(numeric_key);
					branch_case_index++;
					continue;
				}
				if (ZSTR_LEN(string_key) > UINT32_MAX) {
					branch.valid = false;
					branch_case_index++;
					continue;
				}
				frozen_case.string_length =
					static_cast<uint32_t>(ZSTR_LEN(string_key));
				const size_t allocation_size =
					frozen_case.string_length == 0
						? 1 : frozen_case.string_length;
				frozen_case.string_key =
					static_cast<char *>(std::malloc(allocation_size));
				if (frozen_case.string_key == nullptr) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
						"unable to freeze source multiway string key");
					return false;
				}
				if (frozen_case.string_length != 0) {
					std::memcpy(frozen_case.string_key,
						ZSTR_VAL(string_key), frozen_case.string_length);
				}
				branch_case_index++;
			} ZEND_HASH_FOREACH_END();
			if (constant_operand != nullptr
					&& branch.constant_successor == UINT32_MAX) {
				branch.constant_successor = branch.case_count;
			}
		}
	}
	plan->compiled_variable_count =
		source_op_array != nullptr ? source_op_array->last_var : 0;
	if (plan->compiled_variable_count != 0) {
		plan->compiled_variables_used = static_cast<uint8_t *>(
			std::malloc(plan->compiled_variable_count));
		if (plan->compiled_variables_used == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze compiled-variable uses");
			return false;
		}
		std::memset(plan->compiled_variables_used, 1,
			plan->compiled_variable_count);
		if (source_ssa != nullptr && source_ssa->vars != nullptr
				&& source_ssa->vars_count >= 0) {
			for (uint32_t variable_index = 0;
					variable_index < plan->compiled_variable_count;
					++variable_index) {
				bool found = false;
				bool used = false;
				for (int index = 0;
						index < source_ssa->vars_count; ++index) {
					const zend_ssa_var &variable =
						source_ssa->vars[index];
					if (variable.var < 0
							|| static_cast<uint32_t>(variable.var)
								!= variable_index) {
						continue;
					}
					found = true;
					if (variable.definition >= 0
							|| variable.definition_phi != nullptr
							|| variable.use_chain >= 0
							|| variable.phi_use_chain != nullptr
							|| variable.sym_use_chain != nullptr) {
						used = true;
						break;
					}
				}
				plan->compiled_variables_used[variable_index] =
					!found || used;
			}
		}
	}
	if (source_op_array != nullptr && source_ssa != nullptr
			&& source_ssa->cfg.blocks != nullptr
			&& source_ssa->cfg.map != nullptr
			&& source_ssa->cfg.blocks_count > 0) {
		plan->source_block_count =
			static_cast<uint32_t>(source_ssa->cfg.blocks_count);
		plan->source_opcode_block_indices = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->source_opcode_count)
				* sizeof(*plan->source_opcode_block_indices)));
		plan->source_opcode_is_data = static_cast<uint8_t *>(
			std::malloc(plan->source_opcode_count));
		plan->source_block_starts = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->source_block_count)
				* sizeof(*plan->source_block_starts)));
		plan->source_block_ends = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->source_block_count)
				* sizeof(*plan->source_block_ends)));
		if ((plan->source_opcode_count != 0
					&& (plan->source_opcode_block_indices == nullptr
						|| plan->source_opcode_is_data == nullptr))
				|| plan->source_block_starts == nullptr
				|| plan->source_block_ends == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze source CFG landings");
			return false;
		}
		for (uint32_t source = 0;
				source < plan->source_opcode_count; ++source) {
			const int source_block = source_ssa->cfg.map[source];
			plan->source_opcode_block_indices[source] =
				source_block >= 0
						&& static_cast<uint32_t>(source_block)
							< plan->source_block_count
					? static_cast<uint32_t>(source_block) : UINT32_MAX;
			plan->source_opcode_is_data[source] =
				source_op_array->opcodes[source].opcode == ZEND_OP_DATA;
		}
		for (uint32_t source_block = 0;
				source_block < plan->source_block_count; ++source_block) {
			const zend_basic_block &block =
				source_ssa->cfg.blocks[source_block];
			if ((block.flags & ZEND_BB_REACHABLE) == 0
					|| block.start > plan->source_opcode_count
					|| block.len
						> plan->source_opcode_count
							- block.start) {
				plan->source_block_starts[source_block] = UINT32_MAX;
				plan->source_block_ends[source_block] = UINT32_MAX;
				continue;
			}
			plan->source_block_starts[source_block] =
				block.start;
			plan->source_block_ends[source_block] =
				block.start + block.len;
		}
	}
	plan->block_count = view->block_count(view->context);
	plan->value_count = view->value_count(view->context);
	plan->instruction_count = view->instruction_count(view->context);
	const zend_mir_call_view *calls =
		zend_mir_module_call_view_from_view(view);
	const zend_mir_value_view *value_model =
		zend_mir_module_value_view_from_view(view);
	plan->call_site_count = calls != nullptr
		&& calls->call_site_count != nullptr
		? calls->call_site_count(calls->context) : 0;
	plan->call_target_count = calls != nullptr
		&& calls->call_target_count != nullptr
		? calls->call_target_count(calls->context) : 0;
	plan->call_argument_count = calls != nullptr
		&& calls->call_argument_count != nullptr
		? calls->call_argument_count(calls->context) : 0;
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
	plan->argument_abi = static_cast<zend_tpde_local_abi_type *>(
		std::calloc(plan->argument_count, sizeof(*plan->argument_abi)));
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
	plan->call_sites = static_cast<zend_mir_call_site_ref *>(
		std::malloc(static_cast<size_t>(plan->call_site_count)
			* sizeof(*plan->call_sites)));
	plan->call_target_index = allocate_id_index(
		plan->call_target_count, &plan->call_target_index_capacity);
	plan->call_argument_bindings =
		static_cast<zend_tpde_source_value_binding *>(std::malloc(
			static_cast<size_t>(plan->call_argument_count)
				* sizeof(*plan->call_argument_bindings)));
	plan->call_arguments =
		static_cast<zend_mir_call_argument_ref *>(std::malloc(
			static_cast<size_t>(plan->call_argument_count)
				* sizeof(*plan->call_arguments)));
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
				&& (plan->argument_value_indices == nullptr
					|| plan->argument_abi == nullptr))
			|| (plan->instruction_count != 0
				&& (plan->instructions == nullptr
					|| plan->instruction_index == nullptr))
			|| (plan->call_site_count != 0
				&& (plan->call_site_instruction_index == nullptr
					|| plan->call_sites == nullptr
					|| plan->direct_calls == nullptr
					|| plan->direct_internal_calls == nullptr
					|| plan->user_calls == nullptr))
			|| (plan->call_target_count != 0
				&& plan->call_target_index == nullptr)
			|| (plan->call_argument_count != 0
				&& (plan->call_arguments == nullptr
					|| plan->call_argument_bindings == nullptr))
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
		plan->instructions[i].record = record;
		plan->instructions[i].operand_offset =
			static_cast<uint32_t>(operands - count);
		plan->instructions[i].operand_count = count;
		plan->instructions[i].component_target_index = UINT32_MAX;
		plan->instructions[i].component_body_function_index = UINT32_MAX;
		plan->instructions[i].exception_block_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].zval_store_storage_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].zval_store_direct_scalar = false;
		plan->instructions[i].zval_store_lazy_scalar = false;
		plan->instructions[i].transient_scalar_result = false;
		plan->instructions[i].transient_result_storage_id =
			ZEND_MIR_ID_INVALID;
		plan->instructions[i].mutation_storage_id = ZEND_MIR_ID_INVALID;
		plan->instructions[i].mutation_lazy_scalar = false;
		plan->instructions[i].runtime_helper = ZEND_NATIVE_HELPER_COUNT;
		plan->instructions[i].source_opline_index = UINT32_MAX;
		plan->instructions[i].dynamic_fetch_cv_index = UINT32_MAX;
		plan->instructions[i].dynamic_fetch_direct_long = false;
		plan->instructions[i].source_op1_binding = {-1, -1};
		plan->instructions[i].source_op2_binding = {-1, -1};
		plan->instructions[i].source_op2_definition_binding = {-1, -1};
		plan->instructions[i].source_op2_canonical_scalar_only = false;
		plan->instructions[i].source_result_binding = {-1, -1};
		plan->instructions[i].source_auxiliary_binding = {-1, -1};
	}
	plan->instruction_operand_count = static_cast<uint32_t>(operands);
	plan->instruction_operands = static_cast<zend_mir_value_id *>(
		std::malloc(static_cast<size_t>(plan->instruction_operand_count)
			* sizeof(*plan->instruction_operands)));
	if (plan->instruction_operand_count != 0
			&& plan->instruction_operands == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze MIR instruction operands");
		return false;
	}
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_tpde_instruction &instruction = plan->instructions[i];
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			zend_mir_value_id value = ZEND_MIR_ID_INVALID;
			if (!view->instruction_operand_at(view->context,
					instruction.id, operand, &value)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR instruction operand table is unreadable");
				return false;
			}
			plan->instruction_operands[
				instruction.operand_offset + operand] = value;
		}
	}
	for (uint32_t i = 0; i < plan->call_site_count; ++i) {
		zend_mir_call_site_ref site;
		if (calls == nullptr || calls->call_site_at == nullptr
				|| !calls->call_site_at(calls->context, i, &site)
				|| zend_tpde_instruction_index(plan, site.instruction_id) < 0
				|| !id_index_insert(plan->call_site_instruction_index,
					plan->call_site_instruction_index_capacity,
					site.instruction_id, i)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-site table is unreadable, duplicated, or references an unknown instruction");
			return false;
		}
		plan->call_sites[i] = site;
	}
	for (uint32_t i = 0; i < plan->call_target_count; ++i) {
		zend_mir_call_target_ref target;
		if (calls == nullptr || calls->call_target_at == nullptr
				|| !calls->call_target_at(calls->context, i, &target)
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
		plan->call_argument_bindings[i] = {-1, -1};
		if (calls == nullptr
				|| calls->call_argument_at == nullptr
				|| !calls->call_argument_at(calls->context, i, &argument)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR call-argument table is unreadable");
			return false;
		}
		plan->call_arguments[i] = argument;
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
		if (value_model->contract_version
					!= ZEND_MIR_W14_CONTRACT_VERSION
				|| (value_model->model_flags
					& ~ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) != 0
				|| value_model->value_location_count == nullptr
				|| value_model->value_location_at == nullptr
				|| value_model->executable_operation_count == nullptr
				|| value_model->executable_operation_at == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"executable value model lacks the frozen W14 machine-plan facts");
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
		const zend_mir_instruction_record &record = instruction.record;
		if (instruction.has_value_operation
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
	plan->block_successor_offsets = static_cast<uint32_t *>(
		std::calloc(static_cast<size_t>(plan->block_count) + 1,
			sizeof(*plan->block_successor_offsets)));
	plan->block_predecessor_offsets = static_cast<uint32_t *>(
		std::calloc(static_cast<size_t>(plan->block_count) + 1,
			sizeof(*plan->block_predecessor_offsets)));
	if (plan->block_successor_offsets == nullptr
			|| plan->block_predecessor_offsets == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze MIR CFG offsets");
		return false;
	}
	for (uint32_t block = 0; block < plan->block_count; ++block) {
		const uint32_t successor_count = view->successor_count(
			view->context, plan->block_ids[block]);
		const uint32_t predecessor_count = view->predecessor_count(
			view->context, plan->block_ids[block]);
		if (successor_count
					> UINT32_MAX - plan->block_successor_count
				|| predecessor_count
					> UINT32_MAX - plan->block_predecessor_count) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR CFG edge count overflows the machine plan");
			return false;
		}
		plan->block_successor_count += successor_count;
		plan->block_predecessor_count += predecessor_count;
		plan->block_successor_offsets[block + 1] =
			plan->block_successor_count;
		plan->block_predecessor_offsets[block + 1] =
			plan->block_predecessor_count;
	}
	if (plan->block_successor_count != 0) {
		plan->block_successors = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->block_successor_count)
				* sizeof(*plan->block_successors)));
	}
	if (plan->block_predecessor_count != 0) {
		plan->block_predecessors = static_cast<uint32_t *>(
			std::malloc(static_cast<size_t>(plan->block_predecessor_count)
				* sizeof(*plan->block_predecessors)));
	}
	if ((plan->block_successor_count != 0
				&& plan->block_successors == nullptr)
			|| (plan->block_predecessor_count != 0
				&& plan->block_predecessors == nullptr)) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze MIR CFG edges");
		return false;
	}
	for (uint32_t block = 0; block < plan->block_count; ++block) {
		const uint32_t successor_begin =
			plan->block_successor_offsets[block];
		const uint32_t successor_count =
			plan->block_successor_offsets[block + 1] - successor_begin;
		for (uint32_t edge = 0; edge < successor_count; ++edge) {
			zend_mir_block_id target;
			const int32_t target_index =
				view->successor_at(view->context, plan->block_ids[block],
						edge, &target)
					? id_index_find(plan->block_index,
						plan->block_index_capacity, target)
					: -1;
			if (target_index < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR successor is outside the machine plan");
				return false;
			}
			plan->block_successors[successor_begin + edge] =
				static_cast<uint32_t>(target_index);
		}
		const uint32_t predecessor_begin =
			plan->block_predecessor_offsets[block];
		const uint32_t predecessor_count =
			plan->block_predecessor_offsets[block + 1]
				- predecessor_begin;
		for (uint32_t edge = 0; edge < predecessor_count; ++edge) {
			zend_mir_block_id predecessor;
			const int32_t predecessor_index =
				view->predecessor_at(view->context, plan->block_ids[block],
						edge, &predecessor)
					? id_index_find(plan->block_index,
						plan->block_index_capacity, predecessor)
					: -1;
			if (predecessor_index < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR predecessor is outside the machine plan");
				return false;
			}
			plan->block_predecessors[predecessor_begin + edge] =
				static_cast<uint32_t>(predecessor_index);
		}
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
			.category = ZEND_MIR_VALUE_CATEGORY_UNKNOWN,
			.refcount_state = ZEND_MIR_REFCOUNT_UNKNOWN,
			.argument_index = -1,
			.register_alias_value_index = -1,
			.machine_kind = ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
			.location = ZEND_TPDE_MACHINE_LOCATION_CANONICAL_FRAME_SLOT,
			.slot_state = ZEND_TPDE_CANONICAL_SLOT_UNMATERIALIZED,
			.canonical_alias_observable = false,
			.constant = false,
			.constant_bits = 0,
			.known_string_literal = false,
			.known_string_first_byte = 0,
			.known_string_length = 0,
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
					|| (location.category
								< ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
							|| location.category
								> ZEND_MIR_VALUE_CATEGORY_UNKNOWN
							|| location.refcount_state
								< ZEND_MIR_REFCOUNT_IMMORTAL
							|| location.refcount_state
								> ZEND_MIR_REFCOUNT_UNKNOWN
							|| (location.alias_observable
								&& location.category
									!= ZEND_MIR_VALUE_REFERENCE_CELL))
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
			plan->values[value_index].category = location.category;
			plan->values[value_index].refcount_state =
				location.refcount_state;
			plan->values[value_index].canonical_alias_observable =
				location.alias_observable;
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
	std::vector<uint8_t> register_definitions(plan->value_count, 0);
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_mir_instruction_record record{};
		if (!view->instruction_at(view->context, i, &record)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction table is unreadable while classifying value definitions");
			return false;
		}
		if (!zend_mir_id_is_valid(record.result_id)) {
			continue;
		}
		const int32_t value_index =
			zend_tpde_value_index(plan, record.result_id);
		if (value_index < 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction result is absent from the value table");
			return false;
		}
		register_definitions[static_cast<uint32_t>(value_index)] = 1;
	}
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		if (!zend_tpde_apply_machine_value_facts(
				&plan->values[i],
				register_definitions[i] != 0)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"machine value facts do not match source SSA storage");
			return false;
		}
	}
	zend_tpde_refine_non_alias_scalar_values(
		plan, source_ssa, register_definitions);
	zend_tpde_refine_literal_assignment_values(
		plan, source_op_array, source_ssa);
	zend_tpde_refine_boxed_scalar_copies(plan);
	for (uint32_t argument = 0;
			argument < plan->argument_count; ++argument) {
		const zend_arg_info *arg_info =
			source_op_array != nullptr
				&& source_op_array->arg_info != nullptr
				&& argument < source_op_array->num_args
			? &source_op_array->arg_info[argument] : nullptr;
		const bool by_reference =
			arg_info != nullptr
			&& (ZEND_ARG_SEND_MODE(arg_info)
				& (ZEND_SEND_BY_REF | ZEND_SEND_PREFER_REF)) != 0;
		plan->argument_abi[argument] =
			zend_tpde_local_abi_from_declared_type(
				arg_info == nullptr ? nullptr : &arg_info->type,
				by_reference,
				ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED);
	}
	/*
	 * A receive opcode is an SSA definition, but not a machine operation: its
	 * result is exactly the incoming local-ABI argument. Keep that edge in the
	 * frozen plan. This avoids both an adapter-side Zend SSA analysis and a
	 * canonical-frame reload in the typed body.
	 */
	if (source_op_array != nullptr && source_ssa != nullptr
			&& source_ssa->ops != nullptr) {
		auto freeze_register_alias = [&](int32_t use_ssa, int32_t def_ssa) {
			if (use_ssa < 0 || def_ssa < 0) {
				return;
			}
			const int32_t source_index = zend_tpde_value_index(
				plan, zend_mir_value_from_original_ssa(
					static_cast<uint32_t>(use_ssa)));
			const int32_t result_index = zend_tpde_value_index(
				plan, zend_mir_value_from_original_ssa(
					static_cast<uint32_t>(def_ssa)));
			if (source_index < 0 || result_index < 0
					|| source_index == result_index) {
				return;
			}
			const zend_tpde_value &source = plan->values[source_index];
			const zend_tpde_value &result = plan->values[result_index];
			if (result.canonical_storage_id
						== source.canonical_storage_id
					&& result.machine_kind == source.machine_kind
					&& result.exact_type == source.exact_type) {
				plan->values[result_index].register_alias_value_index =
					source_index;
			}
		};
		for (uint32_t opline = 0;
				opline < source_op_array->last; ++opline) {
			const zend_op &op = source_op_array->opcodes[opline];
			if (op.opcode == ZEND_VERIFY_RETURN_TYPE) {
				freeze_register_alias(
					source_ssa->ops[opline].op1_use,
					source_ssa->ops[opline].op1_def);
				continue;
			}
			if (op.opcode != ZEND_RECV
					&& op.opcode != ZEND_RECV_INIT
					&& op.opcode != ZEND_RECV_VARIADIC) {
				continue;
			}
			const uint32_t argument = EX_VAR_TO_NUM(op.result.var);
			const int32_t result_ssa = source_ssa->ops[opline].result_def;
			if (argument >= plan->argument_count || result_ssa < 0
					|| plan->argument_value_indices == nullptr
					|| plan->argument_value_indices[argument] < 0) {
				continue;
			}
			const int32_t result_index = zend_tpde_value_index(
				plan, zend_mir_value_from_original_ssa(
					static_cast<uint32_t>(result_ssa)));
			if (result_index < 0) {
				continue;
			}
			const int32_t argument_index =
				plan->argument_value_indices[argument];
			const zend_tpde_value &incoming =
				plan->values[argument_index];
			const zend_tpde_value &received =
				plan->values[result_index];
			if (argument_index != result_index
					&& received.canonical_storage_id
						== incoming.canonical_storage_id
					&& received.machine_kind == incoming.machine_kind
					&& received.exact_type == incoming.exact_type) {
				plan->values[result_index].register_alias_value_index =
					argument_index;
			}
		}
	}
	if (source_op_array != nullptr
			&& source_op_array->arg_info != nullptr
			&& (source_op_array->fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
		plan->return_abi = zend_tpde_local_abi_from_declared_type(
			&source_op_array->arg_info[-1].type,
			(source_op_array->fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0,
			ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED);
	} else {
		plan->return_abi =
			zend_tpde_local_abi_from_declared_type(
				nullptr, false, ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED);
	}

	std::vector<uint8_t> cyclic_blocks(plan->block_count, 0);
	std::vector<uint8_t> visited_blocks(plan->block_count, 0);
	std::vector<uint32_t> finish_order;
	struct block_dfs_frame {
		uint32_t block;
		uint32_t next_edge;
	};
	std::vector<block_dfs_frame> dfs_stack;
	finish_order.reserve(plan->block_count);
	dfs_stack.reserve(plan->block_count);
	for (uint32_t start = 0; start < plan->block_count; ++start) {
		if (visited_blocks[start] != 0) {
			continue;
		}
		visited_blocks[start] = 1;
		dfs_stack.push_back({start, plan->block_successor_offsets[start]});
		while (!dfs_stack.empty()) {
			block_dfs_frame &frame = dfs_stack.back();
			const uint32_t end =
				plan->block_successor_offsets[frame.block + 1];
			if (frame.next_edge < end) {
				const uint32_t successor =
					plan->block_successors[frame.next_edge++];
				if (visited_blocks[successor] == 0) {
					visited_blocks[successor] = 1;
					dfs_stack.push_back({successor,
						plan->block_successor_offsets[successor]});
				}
				continue;
			}
			finish_order.push_back(frame.block);
			dfs_stack.pop_back();
		}
	}
	std::fill(visited_blocks.begin(), visited_blocks.end(), 0);
	std::vector<uint32_t> component;
	std::vector<uint32_t> component_stack;
	component.reserve(plan->block_count);
	component_stack.reserve(plan->block_count);
	for (auto order = finish_order.rbegin(); order != finish_order.rend();
			++order) {
		const uint32_t start = *order;
		if (visited_blocks[start] != 0) {
			continue;
		}
		component.clear();
		component_stack.push_back(start);
		visited_blocks[start] = 1;
		while (!component_stack.empty()) {
			const uint32_t block = component_stack.back();
			component_stack.pop_back();
			component.push_back(block);
			const uint32_t begin = plan->block_predecessor_offsets[block];
			const uint32_t end = plan->block_predecessor_offsets[block + 1];
			for (uint32_t edge = begin; edge < end; ++edge) {
				const uint32_t predecessor = plan->block_predecessors[edge];
				if (visited_blocks[predecessor] == 0) {
					visited_blocks[predecessor] = 1;
					component_stack.push_back(predecessor);
				}
			}
		}
		bool cyclic = component.size() > 1;
		if (!cyclic) {
			const uint32_t block = component[0];
			const uint32_t begin = plan->block_successor_offsets[block];
			const uint32_t end = plan->block_successor_offsets[block + 1];
			for (uint32_t edge = begin; edge < end; ++edge) {
				if (plan->block_successors[edge] == block) {
					cyclic = true;
					break;
				}
			}
		}
		if (cyclic) {
			for (const uint32_t block : component) {
				cyclic_blocks[block] = 1;
			}
		}
	}
	auto block_is_cyclic = [&](zend_mir_block_id block_id) {
		const int32_t block_index = zend_tpde_block_index(plan, block_id);
		return block_index >= 0
			&& cyclic_blocks[static_cast<uint32_t>(block_index)] != 0;
	};
	std::vector<zend_mir_storage_id> phi_storages;
	std::vector<zend_mir_storage_id> reference_assigned_storages;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &candidate = plan->instructions[i];
		if (candidate.record.opcode == ZEND_MIR_OPCODE_PHI
				&& zend_mir_id_is_valid(candidate.record.result_id)) {
			const int32_t result_index =
				zend_tpde_value_index(plan, candidate.record.result_id);
			if (result_index >= 0) {
				const zend_mir_storage_id storage_id =
					plan->values[result_index].canonical_storage_id;
				if (zend_mir_id_is_valid(storage_id)) {
					phi_storages.push_back(storage_id);
				}
			}
		}
		if (candidate.has_value_operation
				&& candidate.record.opcode
					== ZEND_MIR_OPCODE_VALUE_ASSIGN_REF) {
			const zend_mir_storage_id op1_storage =
				candidate.value_operation.op1_storage_id;
			const zend_mir_storage_id op2_storage =
				candidate.value_operation.op2_storage_id;
			if (zend_mir_id_is_valid(op1_storage)) {
				reference_assigned_storages.push_back(op1_storage);
			}
			if (zend_mir_id_is_valid(op2_storage)) {
				reference_assigned_storages.push_back(op2_storage);
			}
		}
	}
	auto sort_unique = [](std::vector<zend_mir_storage_id> &storages) {
		std::sort(storages.begin(), storages.end());
		storages.erase(
			std::unique(storages.begin(), storages.end()), storages.end());
	};
	sort_unique(phi_storages);
	sort_unique(reference_assigned_storages);
	auto storage_has_phi = [&](zend_mir_storage_id storage_id) {
		return std::binary_search(
			phi_storages.begin(), phi_storages.end(), storage_id);
	};
	auto storage_assigned_by_reference = [&](zend_mir_storage_id storage_id) {
		return std::binary_search(reference_assigned_storages.begin(),
			reference_assigned_storages.end(), storage_id);
	};
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
			bool loop_carried_integer_transport = false;
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
			/*
			 * ZVAL_STORE overwrites the value reaching op1 at its source
			 * assignment. When that exact reaching SSA value is an
			 * unaliased non-refcounted scalar, no reference resolution,
			 * destructor or refcount transition can be observable. Proving
			 * the reaching definition is both stronger and more useful than
			 * scanning the slot's entire history: an earlier refcounted value
			 * may already have been released by a dominating assignment.
			 */
			if (source_op_array != nullptr && source_ssa != nullptr
					&& source_ssa->ops != nullptr
					&& source_ssa->vars != nullptr
					&& record.source_position_id < source_op_array->last
					&& source_op_array->opcodes[
						record.source_position_id].opcode == ZEND_ASSIGN) {
				const int32_t previous_ssa =
					source_ssa->ops[
						record.source_position_id].op1_use;
				const int32_t previous_index =
					previous_ssa >= 0
					? zend_tpde_value_index(
						plan, zend_mir_value_from_original_ssa(
							static_cast<uint32_t>(previous_ssa)))
						: -1;
				if (previous_index >= 0
						&& source_ssa->vars[previous_ssa].alias == NO_ALIAS) {
					const zend_tpde_value &previous =
						plan->values[previous_index];
					plan->instructions[i].zval_store_direct_scalar =
						previous.canonical_storage_id
								== plan->instructions[i]
									.zval_store_storage_id
						&& previous.category
								== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						&& !previous.canonical_alias_observable;
					loop_carried_integer_transport =
						plan->instructions[i].zval_store_direct_scalar
						&& previous.exact_type == ZEND_MIR_SCALAR_TYPE_I64
						&& plan->values[source_index].category
							== ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR
						&& plan->values[source_index].exact_type
							== ZEND_MIR_SCALAR_TYPE_I64
						&& plan->values[source_index].canonical_storage_id
							== plan->instructions[i].zval_store_storage_id;
				}
			}
			if (plan->instructions[i].zval_store_direct_scalar) {
				const bool phi_storage = storage_has_phi(
					plan->instructions[i].zval_store_storage_id);
				/*
				 * An integer assignment which itself defines the canonical
				 * loop-carried value does not need to publish the preceding PHI
				 * to the frame. Other PHI-backed stores remain materialized: their
				 * selected source may not dominate every guarded continuation.
				 */
				plan->instructions[i].zval_store_lazy_scalar =
					block_is_cyclic(record.block_id)
					&& (!phi_storage || loop_carried_integer_transport);
			}
			if (!plan->instructions[i].zval_store_lazy_scalar) {
				plan->instructions[i].runtime_helper =
					ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW;
				require_runtime_helper(
					plan, plan->instructions[i].runtime_helper);
			}
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
			if ((record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
						|| record.opcode
							== ZEND_MIR_OPCODE_VALUE_INCDEC)
					&& block_is_cyclic(record.block_id)) {
				zend_tpde_long_assign_op long_assign{};
				zend_tpde_long_incdec long_incdec{};
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				const bool register_mutation =
					(zend_tpde_long_assign_op_at(
							plan->instructions[i], &long_assign)
						&& !long_assign.has_result)
					|| (zend_tpde_long_incdec_at(
							plan->instructions[i], &long_incdec)
						&& !long_incdec.has_result
						/*
						 * The direct target path accepts an SSA source with a
						 * concrete CV storage, but lazy mutation additionally
						 * replaces the loop-carried source identity.  Keep that
						 * stronger transport on its established slot form until
						 * SSA component boundaries can retain the replacement.
						 */
						&& operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT);
				const uint32_t mutation_ssa =
					operation.op1_definition_ssa_variable_id_plus_one;
				const int32_t mutation_value =
					mutation_ssa == 0 ? -1
					: zend_tpde_value_index(
						plan, zend_mir_value_from_original_ssa(
							mutation_ssa - 1));
				if (register_mutation
						&& source_op_array != nullptr
						&& source_op_array->function_name != nullptr
						&& zend_mir_id_is_valid(
							operation.op1_storage_id)
						&& !storage_assigned_by_reference(
							operation.op1_storage_id)
						&& mutation_value >= 0
						&& source_ssa != nullptr
						&& source_ssa->vars != nullptr
						&& mutation_ssa - 1
							< static_cast<uint32_t>(
								source_ssa->vars_count)
						&& source_ssa->vars[mutation_ssa - 1].alias
							== NO_ALIAS) {
					const zend_tpde_value &value =
						plan->values[
							static_cast<uint32_t>(mutation_value)];
					if (value.canonical_storage_id
							== operation.op1_storage_id) {
						/*
						 * Zend SSA's NO_ALIAS fact is the source-level proof
						 * that this concrete CV definition cannot be observed
						 * through a zend_reference.  The value-location model
						 * may conservatively retain canonical observability for
						 * the polymorphic cold result; that does not require the
						 * proven long fast path to publish on every loop edge.
						 */
						plan->instructions[i].mutation_storage_id =
							operation.op1_storage_id;
						plan->instructions[i].mutation_lazy_scalar = true;
					}
				}
			}
			if (multi_branch) {
				zend_tpde_multi_branch layout;
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				if (!zend_tpde_multi_branch_at(
						plan, plan->instructions[i], record, &layout)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"multiway branch lacks exact source-backed cases");
					return false;
				}
				if (layout.source_opcode == ZEND_MATCH
						&& (operation.op1.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| operation.op1.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV) {
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_VALUE_CHECK_VAR);
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
			if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
				if (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						&& (operation.extended_value == ZEND_INCLUDE_ONCE
							|| operation.extended_value
								== ZEND_REQUIRE_ONCE)) {
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_CONST_INCLUDE_ONCE);
				}
			}
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
			if (calls == nullptr || site_index < 0
					|| calls->call_site_at == nullptr
					|| calls->call_target_at == nullptr
					|| calls->call_continuation_at == nullptr
					|| !calls->call_site_at(
						calls->context,
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
					|| !calls->call_target_at(
						calls->context,
						static_cast<uint32_t>(target_index), &target)
					|| target.id != site.target_id
					|| site.continuations.count != 4
					|| !calls->call_continuation_at(calls->context,
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
			plan->instructions[i].call_site =
				&plan->call_sites[static_cast<uint32_t>(site_index)];
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
			/* NEW creates its object receiver while executing the INIT opcode.
			 * A direct-internal descriptor only carries a pre-existing receiver,
			 * so inherited internal constructors must retain the source phases. */
			const bool internal_constructor_call =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
				&& source_op_array != nullptr
				&& site.source_init_opline_index < source_op_array->last
				&& source_op_array->opcodes[
					site.source_init_opline_index].opcode == ZEND_NEW;
			/* The direct internal descriptor records the declaring scope in its
			 * binding cell, but INIT_STATIC_METHOD_CALL may name a derived called
			 * scope. Preserve the source resolver for late-static-binding-sensitive
			 * internal handlers instead of collapsing the INIT into that cell. */
			const bool internal_static_method_call =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
				&& source_op_array != nullptr
				&& site.source_init_opline_index < source_op_array->last
				&& source_op_array->opcodes[
					site.source_init_opline_index].opcode
					== ZEND_INIT_STATIC_METHOD_CALL;
			/* Internal handlers may deliberately perform semantic validation before
			 * their parameter parser reports too few arguments. Keep a statically
			 * under-arity call in source order so INIT/SEND/DO reaches the handler
			 * with the same frame state as ZEND_DO_ICALL. */
			const bool internal_under_arity_call =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
				&& site.arguments.count < target.required_num_args;
			const bool nested_source_call =
				call_site_participates_in_nested_call(calls, site);
			const bool fragment_call =
				internal_constructor_call
				|| internal_static_method_call
				|| internal_under_arity_call
				|| nested_source_call
				|| call_site_requires_source_fragments(plan, site);
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
				zend_function *expected_function =
					plan->instructions[i].entry_cell != nullptr
						? plan->instructions[i].entry_cell->function
						: nullptr;
				const zend_op_array *expected_op_array =
					expected_function != nullptr
							&& ZEND_USER_CODE(expected_function->type)
						? &expected_function->op_array : nullptr;
				bool direct_descriptor =
					!fragment_call
					&& source_op_array->opcodes[
						site.source_do_opline_index].opcode
						!= ZEND_CALLABLE_CONVERT
					&& source_op_array->opcodes[
						site.source_do_opline_index].opcode
						!= ZEND_CALLABLE_CONVERT_PARTIAL
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
					direct_descriptor =
						(argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_VALUE
							|| argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
							|| argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED)
						&& (argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_LITERAL
							|| argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_SSA);
					if (!direct_descriptor) {
						break;
					}
					uint32_t parameter_ordinal = argument.ordinal;
					if (!direct_call_parameter_ordinal(
							source_op_array, expected_function, argument,
							&parameter_ordinal)) {
						direct_descriptor = false;
						break;
					}
					const zend_op *send =
						argument.send_opline_index < source_op_array->last
							? &source_op_array->opcodes[
								argument.send_opline_index]
							: nullptr;
					const bool parameter_by_reference =
						expected_function != nullptr
						&& ARG_MUST_BE_SENT_BY_REF(
							expected_function, parameter_ordinal + 1);
					/*
					 * A direct descriptor transfers every argument while executing
					 * DO_FCALL. Reference arguments and possibly-undefined reads need
					 * their original SEND opcode timing and validation instead.
					 */
					if (parameter_by_reference
							|| argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_BY_REFERENCE
							|| argument.ownership
								== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
							|| (send != nullptr && send->opcode == ZEND_SEND_REF)
							|| source_call_argument_may_be_undefined(
								source_op_array, source_ssa, argument)) {
						direct_descriptor = false;
					}
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
					/*
					 * Keep one literal-map entry per fixed parameter.  UINT32_MAX
					 * marks a supplied slot.  A slot-indexed map is required for
					 * named calls, where omitted defaults are not necessarily a
					 * contiguous suffix of the source argument list.
					 */
					const uint32_t default_literal_count =
						expected_op_array != nullptr
							? expected_op_array->num_args : 0;
					const size_t descriptor_size =
						offsetof(zend_native_direct_call_descriptor, arguments)
						+ static_cast<size_t>(site.arguments.count)
							* sizeof(zend_native_direct_call_argument)
						+ static_cast<size_t>(default_literal_count)
							* sizeof(uint32_t);
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
					descriptor->frame_argument_count = site.arguments.count;
					descriptor->default_literal_count =
						default_literal_count;
					for (uint32_t n = 0; n < default_literal_count; ++n) {
						zend_native_direct_call_default_literals(descriptor)[n] =
							UINT32_MAX;
					}
					descriptor->source_position =
						site.source_do_opline_index;
					descriptor->expected_function = expected_function;
					if (expected_op_array != nullptr) {
						descriptor->callee_argument_count =
							expected_op_array->num_args;
						descriptor->callee_compiled_variable_count =
							expected_op_array->last_var;
						descriptor->callee_temporary_count =
							expected_op_array->T;
					}
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
					descriptor->receiver_source_frame_offset = UINT32_MAX;
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
							uint64_t receiver_slot =
								descriptor->receiver_operand.index;
							if (descriptor->receiver_operand.slot_kind
									!= ZEND_MIR_SOURCE_SLOT_CV) {
								receiver_slot += source_op_array->last_var;
							}
							receiver_slot += ZEND_CALL_FRAME_SLOT;
							if (receiver_slot > UINT32_MAX / sizeof(zval)) {
								std::free(descriptor);
								zend_tpde_set_diagnostic(diag,
									ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
									"direct native method receiver frame offset overflows");
								return false;
							}
							descriptor->receiver_source_frame_offset =
								static_cast<uint32_t>(receiver_slot * sizeof(zval));
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
					std::vector<uint8_t> supplied_parameters(
						callee != nullptr ? callee->common.num_args : 0, 0);
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
										&& (descriptor->receiver_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_SLOT
											|| descriptor->receiver_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_SSA)
										&& descriptor->receiver_source_frame_offset
											!= UINT32_MAX)));
						trivial_frame =
							inline_receiver
							&& (op_array.scope == nullptr
								|| (op_array.scope->ce_flags & ZEND_ACC_TRAIT) == 0)
							&& site.arguments.count
								>= op_array.required_num_args
							&& (op_array.fn_flags
								& (ZEND_ACC_CALL_VIA_TRAMPOLINE
									| ZEND_ACC_DEPRECATED
									| ZEND_ACC_NODISCARD)) == 0;
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
						uint32_t parameter_ordinal = argument.ordinal;
						if (argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED) {
							const zend_op *send = source_op_array != nullptr
									&& argument.send_opline_index
										< source_op_array->last
								? &source_op_array->opcodes[
									argument.send_opline_index]
								: nullptr;
							const zval *name = send != nullptr
									&& send->op2_type == IS_CONST
								? RT_CONSTANT(send, send->op2) : nullptr;
							bool found = false;
							if (callee != nullptr && name != nullptr
									&& Z_TYPE_P(name) == IS_STRING
									&& callee->common.arg_info != nullptr) {
								for (uint32_t parameter = 0;
										parameter < callee->common.num_args;
										++parameter) {
									const zend_string *parameter_name =
										callee->common.arg_info[parameter].name;
									if (parameter_name != nullptr
											&& zend_string_equals(
												parameter_name, Z_STR_P(name))) {
										parameter_ordinal = parameter;
										found = true;
										break;
									}
								}
							}
							/* Unknown named parameters require a variadic-name map. */
							trivial_frame = trivial_frame && found;
						}
						descriptor->arguments[n].ordinal = parameter_ordinal;
						if (parameter_ordinal < supplied_parameters.size()) {
							trivial_frame = trivial_frame
								&& supplied_parameters[parameter_ordinal] == 0;
							supplied_parameters[parameter_ordinal] = 1;
							descriptor->frame_argument_count = std::max(
								descriptor->frame_argument_count,
								parameter_ordinal + 1);
						}
						/*
						 * An open method target is deliberately frozen before
						 * request-local binding, so its MIR argument record can
						 * only preserve the SEND opcode's syntactic mode. Once
						 * the binding resolves a concrete user function, its
						 * parameter declaration is authoritative. In
						 * particular, SEND_VAR_EX to `array &$arg` must create
						 * and transfer a reference cell even though the source
						 * opcode itself is not SEND_REF.
						 */
						const bool parameter_by_reference =
							callee != nullptr
							&& ARG_MUST_BE_SENT_BY_REF(
								callee, parameter_ordinal + 1);
						descriptor->arguments[n].mode =
							parameter_by_reference
								|| argument.ownership
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
									source_op_array, source_ssa, calls,
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
						const zval *source_literal =
							source_op_array != nullptr
								&& argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL
								&& argument.source_operand.index
									< source_op_array->last_literal
							? &source_op_array->literals[
								argument.source_operand.index]
							: nullptr;
						const bool inline_literal =
							descriptor->arguments[n].mode
								== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
							&& source_literal != nullptr
							&& Z_TYPE_P(source_literal) != IS_CONSTANT_AST;
						const bool inline_argument =
							(descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& (zend_mir_scalar_type_is_exact(
										descriptor->arguments[n].exact_type)
									|| inline_literal
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
						const zend_type *parameter_type =
							callee->op_array.arg_info == nullptr
								? nullptr
								: parameter_ordinal < callee->op_array.num_args
									? &callee->op_array.arg_info[
										parameter_ordinal].type
									: (callee->common.fn_flags
											& ZEND_ACC_VARIADIC) != 0
										? &callee->op_array.arg_info[
											callee->op_array.num_args].type
										: nullptr;
						const bool inline_parameter =
							!trivial_frame
							|| parameter_type == nullptr
							|| !ZEND_TYPE_IS_SET(*parameter_type)
							|| (inline_literal
								&& literal_satisfies_type(
									source_literal, *parameter_type))
							|| (descriptor->arguments[n].mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& exact_scalar_satisfies_type(
									descriptor->arguments[n].exact_type,
									*parameter_type));
						trivial_frame =
							trivial_frame && inline_argument && inline_parameter;
					}
					if (trivial_frame) {
						const zend_op_array &op_array = callee->op_array;
						for (uint32_t parameter = 0;
								trivial_frame && parameter < op_array.num_args;
								++parameter) {
							if (supplied_parameters[parameter] != 0) {
								continue;
							}
							const zend_op &receive = op_array.opcodes[parameter];
							const zval *default_value =
								receive.opcode == ZEND_RECV_INIT
									&& receive.op1.num == parameter + 1
									&& receive.op2_type == IS_CONST
									&& EX_VAR_TO_NUM(receive.result.var) == parameter
								? RT_CONSTANT(&receive, receive.op2)
								: nullptr;
							trivial_frame = default_value != nullptr
								&& default_value >= op_array.literals
								&& default_value
									< op_array.literals + op_array.last_literal
								&& Z_TYPE_P(default_value) != IS_CONSTANT_AST;
							if (trivial_frame) {
								zend_native_direct_call_default_literals(
									descriptor)[parameter] =
										static_cast<uint32_t>(
											default_value - op_array.literals);
							}
						}
						if (trivial_frame) {
							descriptor->frame_size = zend_vm_calc_used_stack(
								descriptor->frame_argument_count, callee);
						}
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
						if ((callee->op_array.fn_flags & ZEND_ACC_VARIADIC) != 0) {
							require_runtime_helper(
								plan,
								ZEND_NATIVE_HELPER_RECEIVE_EXPLICIT_PENDING);
						}
						bool leaf_scalar_frame =
							user_bindings[binding_index].leaf_scalar_frame
							&& (callee->op_array.fn_flags & ZEND_ACC_VARIADIC) == 0
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
								descriptor->arguments[n].ordinal == n
								&& descriptor->arguments[n].mode
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
					plan->instructions[i].user_call_no_call =
						init->opcode == ZEND_NEW
						&& target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER
						&& target.num_args == 0
						&& target.required_num_args == 0
						&& plan->instructions[i].entry_cell != nullptr
						&& plan->instructions[i].entry_cell->function
							== reinterpret_cast<const zend_function *>(
								source_op_array);
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
					if (finish->opcode != ZEND_CALLABLE_CONVERT
							&& finish->opcode != ZEND_CALLABLE_CONVERT_PARTIAL
							&& zend_mir_id_is_valid(record.result_id)) {
						const int32_t result_index =
							zend_tpde_value_index(plan, record.result_id);
						if (result_index < 0) {
							std::free(descriptor);
							zend_tpde_set_diagnostic(diag,
								ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
								"dynamic user-call result is unknown");
							return false;
						}
						if (zend_mir_scalar_type_is_exact(
								plan->values[result_index].exact_type)) {
							descriptor->result_type =
								plan->values[result_index].exact_type;
							descriptor->flags |=
								ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT;
						}
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
					}
					if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT
							|| descriptor->do_opcode
								== ZEND_CALLABLE_CONVERT_PARTIAL) {
						require_runtime_helper(
								plan,
								ZEND_NATIVE_HELPER_CALL_CONVERT_EXPLICIT);
					} else {
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT);
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER);
						require_runtime_helper(
							plan, ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_USER_CALL_RESOLVE);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_USER_CALL_NORMALIZE_RESOLUTION);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_USER_CALL_RELEASE_RESOLUTION);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_CALL_RESERVE_DYNAMIC_FRAME);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_USER_CALL_EXPAND_ARGUMENTS);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_USER_CALL_SEND_RESOLVED_ARGUMENT);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_PREPARE);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_OBSERVER_BEGIN);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_OBSERVER_END);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_FINALIZE);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RESERVE);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_CHECK_FUNC_ARG_RESOLVED);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_ACTIVATION_POP);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE);
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION);
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
					if (!direct_descriptor
							&& plan->instructions[i].user_call->do_opcode
								!= ZEND_CALLABLE_CONVERT
							&& plan->instructions[i].user_call->do_opcode
								!= ZEND_CALLABLE_CONVERT_PARTIAL) {
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
							plan, source_op_array, site, record, diag);
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
							&& finish->opcode != ZEND_DO_FCALL
							&& finish->opcode != ZEND_DO_FCALL_BY_NAME
							&& finish->opcode != ZEND_CALLABLE_CONVERT
							&& finish->opcode
								!= ZEND_CALLABLE_CONVERT_PARTIAL)
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
					descriptor->do_opcode = finish->opcode;
					descriptor->do_op1_payload = finish->op1.num;
					descriptor->do_extended_value = finish->extended_value;
					descriptor->result_operand = site.result_operand;
					descriptor->result_type = ZEND_MIR_SCALAR_TYPE_NONE;
					zend_function *callee =
						plan->instructions[i].internal_call_cell != nullptr
							? plan->instructions[i].internal_call_cell->function
							: nullptr;
					if (!source_descriptor_operand(
						source_op_array, init, init->op1_type, init->op1,
						&descriptor->receiver_operand)
							|| !source_descriptor_operand(
								source_op_array, finish, finish->op2_type,
								finish->op2, &descriptor->do_op2)) {
						std::free(descriptor);
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct internal-call source operand is invalid");
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
					if (!zend_mir_scalar_type_is_exact(
							descriptor->result_type)) {
						if (callee != nullptr && callee->common.arg_info != nullptr
								&& (callee->common.fn_flags
									& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
							descriptor->result_type =
								exact_scalar_from_declared_type(
									callee->common.arg_info[-1].type);
						}
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
						bool send_by_reference = argument.ownership
							== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
						if (argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_NAMED) {
							uint32_t parameter_ordinal;
							send_by_reference =
								direct_call_parameter_ordinal(
									source_op_array, callee, argument,
									&parameter_ordinal)
								&& ARG_MUST_BE_SENT_BY_REF(
									callee, parameter_ordinal + 1);
						}
						encoded.mode = argument.source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
						? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
						: send_by_reference
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
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN);
					require_runtime_helper(plan,
						ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT);
					require_runtime_helper(plan,
						ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT);
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE);
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR);
					require_runtime_helper(plan,
						ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT);
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
	std::vector<uint32_t> source_instruction_offsets(
		static_cast<size_t>(plan->source_opcode_count) + 1, 0);
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[i]);

		if (zend_mir_id_is_valid(record.source_position_id)
				&& record.source_position_id < plan->source_opcode_count) {
			source_instruction_offsets[record.source_position_id + 1]++;
		}
	}
	for (uint32_t source = 0; source < plan->source_opcode_count; ++source) {
		source_instruction_offsets[source + 1] +=
			source_instruction_offsets[source];
	}
	std::vector<uint32_t> source_instruction_indexes(
		source_instruction_offsets[plan->source_opcode_count]);
	std::vector<uint32_t> source_instruction_cursors =
		source_instruction_offsets;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[i]);

		if (zend_mir_id_is_valid(record.source_position_id)
				&& record.source_position_id < plan->source_opcode_count) {
			source_instruction_indexes[
				source_instruction_cursors[record.source_position_id]++] = i;
		}
	}
	for (uint32_t i = 0; i < effect_count; ++i) {
		const zend_native_source_effect &effect = effects[i];
		zend_tpde_instruction *match = nullptr;
		bool exception_match = false;
		zend_mir_opcode source_opcode = ZEND_MIR_OPCODE_INVALID;
		uint32_t source_instruction_begin = 0;
		uint32_t source_instruction_end = 0;

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
		if (effect.source_position_id < plan->source_opcode_count) {
			source_instruction_begin =
				source_instruction_offsets[effect.source_position_id];
			source_instruction_end =
				source_instruction_offsets[effect.source_position_id + 1];
		}
		for (uint32_t source_instruction = source_instruction_begin;
				source_instruction < source_instruction_end;
				++source_instruction) {
			const uint32_t n =
				source_instruction_indexes[source_instruction];
			zend_tpde_instruction &candidate = plan->instructions[n];
			const zend_mir_instruction_record candidate_record =
				zend_tpde_instruction_record_at(plan, &candidate);
			const bool boxed_cond_branch = candidate.has_value_operation
				&& candidate_record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& candidate.value_operation.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH;
			if (candidate_record.source_position_id != effect.source_position_id) {
				continue;
			}
			source_opcode = candidate_record.opcode;
			if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE) {
				if (candidate_record.opcode
						== ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH) {
					/*
					 * The inline dispatch itself cannot raise. Its source-backed
					 * fallback is a separate successor instruction with its own
					 * exception route.
					 */
					exception_match = true;
					continue;
				}
				if (!boxed_cond_branch
						&& executable_value_helper(candidate_record.opcode)
						== ZEND_NATIVE_HELPER_COUNT
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_ITERATOR_BRANCH
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_CATCH_ENTER
						&& candidate_record.opcode
							!= ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
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
				/*
				 * A single source opcode may lower to multiple executable
				 * value helpers.  They share the source-level try region, so
				 * every one of them must use the same exception successor.
				 */
				if (zend_mir_id_is_valid(candidate.exception_block_id)
						&& candidate.exception_block_id
							!= effect.target_block_id) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"value exception routes disagree for one source opcode");
					return false;
				}
				candidate.exception_block_id = effect.target_block_id;
				exception_match = true;
				continue;
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
		if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE) {
			if (!exception_match) {
				char message[160];

				std::snprintf(message, sizeof(message),
					"value exception route at source %u has no executable instruction (opcode %u)",
					effect.source_position_id,
					static_cast<unsigned int>(source_opcode));
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					message);
				return false;
			}
			continue;
		}
		if (match == nullptr
				&& effect.kind == ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE) {
			continue;
		}
		if (match == nullptr
				|| (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE
						? match->debug_probe
						: (match->operand_count != 1
						|| match->source_effect != 0))) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"W07 echo must map uniquely to a scalar value proof");
			return false;
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
	if (!freeze_source_value_bindings(
			plan, source_op_array, source_ssa, diag)) {
		return false;
	}
	freeze_dynamic_fetch_cv_indices(plan, source_op_array, source_ssa);
	if (!freeze_source_call_phases(plan, diag)) {
		return false;
	}
	freeze_machine_control_flow(plan);
	if (!freeze_machine_references(plan, diag)) {
		return false;
	}
	if (!freeze_entry_undef_temporaries(plan, source_op_array, diag)) {
		return false;
	}
	if (!freeze_generator_resume_liveness(
			plan, source_op_array, value_model, diag)) {
		return false;
	}
	if (!freeze_statepoint_materializations(
			plan, view, source_op_array, diag)) {
		return false;
	}
	if (!freeze_machine_plan_consumers(plan, diag)) {
		return false;
	}
	freeze_register_boolean_results(plan);
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
		case ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT:
		case ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE:
		case ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR:
		case ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE:
			return true;
		default:
			return false;
	}
}

static zend_tpde_local_abi_type machine_plan_abi(
		zend_mir_representation representation,
		zend_mir_scalar_type_mask exact_type,
		zend_tpde_machine_value_kind machine_kind,
		zend_tpde_local_abi_transfer transfer) {
	zend_tpde_local_abi_type result{};
	result.representation = representation;
	result.exact_type = exact_type;
	result.machine_kind = machine_kind;
	result.transfer = transfer;
	result.valid = true;
	return result;
}

static bool machine_plan_abi_same_shape(
		const zend_tpde_local_abi_type &left,
		const zend_tpde_local_abi_type &right) {
	return left.valid && right.valid
		&& left.representation == right.representation
		&& left.exact_type == right.exact_type
		&& left.machine_kind == right.machine_kind;
}

static bool machine_plan_abi_can_supply_argument(
		const zend_tpde_local_abi_type &caller,
		const zend_tpde_local_abi_type &callee) {
	if (!machine_plan_abi_same_shape(caller, callee)) {
		return false;
	}
	switch (callee.transfer) {
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE:
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE;
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED:
			/* A typed body borrows its argument for the duration of the call.
			 * An existing owner or immortal root is therefore a valid source;
			 * no ownership operation is required at the call boundary. */
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED
				|| caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED
				|| caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL;
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED:
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED:
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED;
		case ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL:
			return caller.transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL;
	}
	return false;
}

static bool machine_plan_call_argument_can_supply(
		const zend_tpde_plan *plan,
		const zend_mir_call_argument_ref &argument,
		const zend_tpde_local_abi_type &caller,
		const zend_tpde_local_abi_type &callee) {
	if (!machine_plan_abi_can_supply_argument(caller, callee)) {
		return false;
	}
	if (callee.transfer != ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED
			|| caller.transfer != ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED) {
		return true;
	}
	if (plan == nullptr || plan->source_opcodes == nullptr
			|| argument.send_opline_index >= plan->source_opcode_count) {
		return false;
	}
	const uint8_t source_type =
		plan->source_opcodes[argument.send_opline_index].op1_type;
	return source_type == IS_CV || source_type == IS_CONST;
}

static zend_tpde_local_abi_type machine_plan_value_abi(
		const zend_tpde_plan *plan, uint32_t value_index) {
	if (plan == nullptr || value_index >= plan->value_count) {
		return {};
	}
	for (uint32_t depth = 0; depth < plan->value_count; ++depth) {
		const int32_t alias =
			plan->values[value_index].register_alias_value_index;
		if (alias < 0) {
			break;
		}
		if (static_cast<uint32_t>(alias) >= plan->value_count) {
			return {};
		}
		if (static_cast<uint32_t>(alias) == value_index) {
			break;
		}
		value_index = static_cast<uint32_t>(alias);
	}
	const zend_tpde_value &value = plan->values[value_index];
	if (value.local_abi.valid) {
		return value.local_abi;
	}
	if (value.argument_index >= 0
			&& plan->argument_abi != nullptr
			&& static_cast<uint32_t>(value.argument_index)
				< plan->argument_count) {
		return plan->argument_abi[
			static_cast<uint32_t>(value.argument_index)];
	}
	const bool exact_scalar =
		zend_mir_scalar_type_is_exact(value.exact_type)
		&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL;
	const bool native_pointer =
		value.machine_kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
		|| value.machine_kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
		|| value.machine_kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
		|| value.machine_kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
		|| value.machine_kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
	const bool boxed =
		value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	if ((!exact_scalar && !native_pointer && !boxed)
			|| (!exact_scalar && value.canonical_alias_observable)) {
		return {};
	}
	const zend_tpde_machine_value_kind abi_kind =
		exact_scalar
			? value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				? ZEND_TPDE_MACHINE_VALUE_BOOL
			: value.exact_type == ZEND_MIR_SCALAR_TYPE_F64
				? ZEND_TPDE_MACHINE_VALUE_F64
				: ZEND_TPDE_MACHINE_VALUE_I64
			: value.machine_kind;
	const zend_tpde_machine_representation_desc machine_rep =
		zend_tpde_machine_representation(abi_kind, true);
	if (machine_rep.part_count == 0 || machine_rep.parts == nullptr) {
		return {};
	}
	return machine_plan_abi(
		exact_scalar
			? value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				? ZEND_MIR_REPRESENTATION_I1
			: value.exact_type == ZEND_MIR_SCALAR_TYPE_F64
				? ZEND_MIR_REPRESENTATION_DOUBLE
				: ZEND_MIR_REPRESENTATION_I64
		: native_pointer
			? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
			: value.representation,
		value.exact_type,
		abi_kind,
		exact_scalar
			? ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE
			: value.refcount_state == ZEND_MIR_REFCOUNT_IMMORTAL
				? ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL
			: value.ownership == ZEND_MIR_OWNERSHIP_STATE_MOVED
				? ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED
			: value.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED
					|| value.ownership
						== ZEND_MIR_OWNERSHIP_STATE_SHARED_OWNED
				? ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED
				: ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED);
}

static int32_t machine_plan_source_value_index(
		const zend_tpde_plan *plan,
		const zend_mir_source_operand_ref &operand) {
	zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
	if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value_id = zend_mir_value_from_synthetic(operand.index);
	} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand.ssa_variable_id != ZEND_MIR_ID_INVALID) {
		value_id =
			zend_mir_value_from_original_ssa(operand.ssa_variable_id);
	}
	return zend_mir_id_is_valid(value_id)
		? zend_tpde_value_index(plan, value_id) : -1;
}

static bool machine_plan_type_check_supported(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction) {
	if (plan == nullptr || !instruction.has_value_operation
			|| instruction.value_operation.opcode
				!= ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			|| instruction.value_operation.source_opcode
				!= ZEND_TYPE_CHECK
			|| instruction.value_operation.result.ssa_variable_id
				== ZEND_MIR_ID_INVALID) {
		return false;
	}
	const int32_t input_index = machine_plan_source_value_index(
		plan, instruction.value_operation.op1);
	const int32_t result_index = machine_plan_source_value_index(
		plan, instruction.value_operation.result);
	if (input_index < 0
			|| (result_index >= 0
				&& static_cast<uint32_t>(result_index)
					>= plan->value_count)) {
		return false;
	}
	switch (plan->values[input_index].exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
		case ZEND_MIR_SCALAR_TYPE_I1:
		case ZEND_MIR_SCALAR_TYPE_I64:
		case ZEND_MIR_SCALAR_TYPE_F64:
			return true;
		default:
			return false;
	}
}

static bool freeze_typed_body_signature(
		const zend_tpde_plan *plan,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		const uint8_t *typed_body_candidates,
		bool reject_variadic_receive,
		zend_tpde_local_abi_type *return_type) {
	if (plan == nullptr || component_plans == nullptr
			|| typed_body_candidates == nullptr || return_type == nullptr
			|| plan->generator_resume_count != 0
			|| plan->user_opcode_callbacks
			|| (plan->argument_count != 0
				&& plan->argument_value_indices == nullptr)) {
		return false;
	}
	if (reject_variadic_receive && plan->source_opcodes != nullptr) {
		for (uint32_t source = 0;
				source < plan->source_opcode_count; ++source) {
			if (plan->source_opcodes[source].opcode == ZEND_RECV_VARIADIC) {
				/* A variadic receive constructs a packed array from a dynamic
				 * number of arguments. The fixed typed-body ABI does not carry
				 * enough information to reproduce that operation. */
				return false;
			}
		}
	}
	for (uint32_t argument = 0;
			argument < plan->argument_count; ++argument) {
		const int32_t value_index =
			plan->argument_value_indices[argument];
		if (value_index < 0
				|| static_cast<uint32_t>(value_index)
					>= plan->value_count
				|| !machine_plan_value_abi(
					plan, static_cast<uint32_t>(value_index)).valid) {
			return false;
		}
	}

	zend_tpde_local_abi_type result_type{};
	std::vector<zend_tpde_local_abi_type> call_result_types(
		plan->value_count);
	std::vector<zend_tpde_local_abi_type> instruction_result_types(
		plan->instruction_count);
	std::vector<zend_tpde_local_abi_type> register_source_ssa(
		plan->source_ssa_variable_count);
	auto value_has_typed_body_definition = [&](int32_t value_index) {
		for (uint32_t depth = 0;
				value_index >= 0 && depth < plan->value_count;
				++depth) {
			if (static_cast<uint32_t>(value_index) >= plan->value_count) {
				return false;
			}
			const zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(value_index)];
			if (value.constant || value.argument_index >= 0
					|| (plan->value_definition_instructions != nullptr
						&& plan->value_definition_instructions[
							static_cast<uint32_t>(value_index)] >= 0)) {
				return true;
			}
			if (value.register_alias_value_index < 0
					|| value.register_alias_value_index == value_index) {
				return false;
			}
			value_index = value.register_alias_value_index;
		}
		return false;
	};
	bool saw_return = false;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (instruction.local_abi_transport) {
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const uint32_t target = instruction.component_target_index;
			const zend_tpde_plan *callee =
				target < component_count ? component_plans[target] : nullptr;
			if (instruction.direct_call == nullptr
					|| target >= component_count
					|| typed_body_candidates[target] == 0
					|| instruction.direct_call->receiver_kind
						!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE
					|| callee == nullptr
					|| (callee->argument_count != 0
						&& callee->argument_value_indices == nullptr)
					|| instruction.call_argument_count
						!= callee->argument_count) {
				return false;
			}
			for (uint32_t argument = 0;
					argument < instruction.call_argument_count;
					++argument) {
				zend_mir_call_argument_ref source_argument{};
				const int32_t callee_value =
					callee->argument_value_indices[argument];
				const uint32_t argument_index =
					instruction.call_argument_offset + argument;
				if (!zend_tpde_call_argument_at(
						plan, argument_index, &source_argument)
						|| (source_argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_BORROWED_SCALAR
							&& source_argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_VALUE
							&& source_argument.ownership
								!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE)
						|| callee_value < 0
						|| static_cast<uint32_t>(callee_value)
							>= callee->value_count) {
					return false;
				}
				const zend_tpde_source_value_binding caller_binding =
					plan->call_argument_bindings[argument_index];
				zend_tpde_local_abi_type caller_abi{};
				if (caller_binding.value_index >= 0
						&& static_cast<uint32_t>(
							caller_binding.value_index)
							< plan->value_count) {
					caller_abi = machine_plan_value_abi(
						plan, static_cast<uint32_t>(
							caller_binding.value_index));
				}
				if (!caller_abi.valid
						&& caller_binding.definition_instruction_index >= 0
						&& static_cast<uint32_t>(
							caller_binding.definition_instruction_index)
							< instruction_result_types.size()) {
					caller_abi = instruction_result_types[
						static_cast<uint32_t>(
							caller_binding.definition_instruction_index)];
				}
				const zend_mir_value_id caller_ssa =
					source_argument.source_operand.ssa_variable_id;
				if (!caller_abi.valid
						&& caller_ssa < register_source_ssa.size()) {
					caller_abi = register_source_ssa[caller_ssa];
				}
				const zend_tpde_local_abi_type callee_abi =
					machine_plan_value_abi(
						callee, static_cast<uint32_t>(callee_value));
				const bool by_reference =
					source_argument.ownership
						== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
				if (!machine_plan_abi_can_supply_argument(
						caller_abi, callee_abi)
						|| by_reference
							!= (callee_abi.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
					return false;
				}
			}
			const int32_t call_result = machine_plan_source_value_index(
				plan, instruction.direct_call->result_operand);
			const zend_mir_value_id call_result_ssa =
				instruction.direct_call->result_operand.ssa_variable_id;
			zend_tpde_local_abi_type call_result_type =
				callee->return_abi;
			if (!call_result_type.valid && call_result >= 0) {
				call_result_type = machine_plan_value_abi(
					plan, static_cast<uint32_t>(call_result));
			} else if (!call_result_type.valid
					&& zend_mir_scalar_type_is_exact(
						instruction.direct_call->result_type)
					&& instruction.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_NULL) {
				const zend_mir_scalar_type_mask exact_type =
					instruction.direct_call->result_type;
				call_result_type = machine_plan_abi(
					exact_type == ZEND_MIR_SCALAR_TYPE_I1
						? ZEND_MIR_REPRESENTATION_I1
					: exact_type == ZEND_MIR_SCALAR_TYPE_F64
						? ZEND_MIR_REPRESENTATION_DOUBLE
						: ZEND_MIR_REPRESENTATION_I64,
					exact_type,
					exact_type == ZEND_MIR_SCALAR_TYPE_I1
						? ZEND_TPDE_MACHINE_VALUE_BOOL
					: exact_type == ZEND_MIR_SCALAR_TYPE_F64
						? ZEND_TPDE_MACHINE_VALUE_F64
						: ZEND_TPDE_MACHINE_VALUE_I64,
					ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE);
			}
			if (!call_result_type.valid) {
				return false;
			}
			instruction_result_types[index] = call_result_type;
			if (call_result >= 0) {
				call_result_types[static_cast<uint32_t>(call_result)] =
					call_result_type;
			} else if (call_result_ssa < register_source_ssa.size()) {
				register_source_ssa[call_result_ssa] = call_result_type;
			} else if (instruction.source_result_binding
							.definition_instruction_index
						!= static_cast<int32_t>(index)) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
			zend_tpde_local_abi_type verified_type{};
			const int32_t verified =
				instruction.source_op1_binding.value_index;
			if (instruction.source_op1_binding
						.definition_instruction_index >= 0
					&& static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)
						< instruction_result_types.size()) {
				verified_type = instruction_result_types[
					static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)];
			}
			const zend_mir_value_id verified_ssa =
				instruction.value_operation.op1.ssa_variable_id;
			if (!verified_type.valid
					&& verified_ssa < register_source_ssa.size()) {
				verified_type = register_source_ssa[verified_ssa];
			}
			if (!verified_type.valid && verified >= 0
					&& static_cast<uint32_t>(verified)
						< plan->value_count) {
				verified_type = machine_plan_value_abi(
					plan, static_cast<uint32_t>(verified));
			}
			if (!machine_plan_abi_same_shape(
					verified_type, plan->return_abi)) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK) {
			if (!machine_plan_type_check_supported(plan, instruction)) {
				return false;
			}
			const zend_mir_value_id result_ssa =
				instruction.value_operation.result.ssa_variable_id;
			if (result_ssa >= register_source_ssa.size()) {
				return false;
			}
			register_source_ssa[result_ssa] = machine_plan_abi(
				ZEND_MIR_REPRESENTATION_I1,
				ZEND_MIR_SCALAR_TYPE_I1,
				ZEND_TPDE_MACHINE_VALUE_BOOL,
				ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE);
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR
				|| instruction.source_effect
					== ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
			if (!instruction.has_value_operation
					|| instruction.value_operation.opcode
						!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					|| (instruction.value_operation.source_opcode
							!= ZEND_JMPZ
						&& instruction.value_operation.source_opcode
							!= ZEND_JMPNZ)) {
				return false;
			}
			const int32_t condition = machine_plan_source_value_index(
				plan, instruction.value_operation.op1);
			const zend_mir_value_id source_ssa =
				instruction.value_operation.op1.ssa_variable_id;
			const bool source_override =
				source_ssa < register_source_ssa.size()
				&& register_source_ssa[source_ssa].valid
				&& register_source_ssa[source_ssa].exact_type
					== ZEND_MIR_SCALAR_TYPE_I1;
			if (!source_override
					&& (condition < 0
						|| plan->values[condition].exact_type
							!= ZEND_MIR_SCALAR_TYPE_I1)) {
				return false;
			}
			continue;
		}
		if (record.effects != 0 || record.reads != 0
				|| record.writes != 0 || record.barriers != 0
				|| record.ownership_actions != 0) {
			return false;
		}
		int32_t returned = -1;
		zend_tpde_local_abi_type returned_source_type{};
		if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
			if (instruction.operand_count != 1) {
				return false;
			}
			returned = zend_tpde_value_index(
				plan, zend_tpde_operand_at(plan, &instruction, 0));
		} else if (record.opcode
				== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			if (!instruction.has_value_operation
					|| instruction.value_operation.source_opcode
						!= ZEND_RETURN) {
				return false;
			}
			returned = instruction.source_op1_binding.value_index;
			const zend_mir_value_id returned_ssa =
				instruction.value_operation.op1.ssa_variable_id;
			if (instruction.source_op1_binding
							.definition_instruction_index >= 0
					&& static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)
						< instruction_result_types.size()) {
				returned_source_type = instruction_result_types[
					static_cast<uint32_t>(
						instruction.source_op1_binding
							.definition_instruction_index)];
			}
			if (!returned_source_type.valid
					&& returned_ssa < register_source_ssa.size()) {
				returned_source_type = register_source_ssa[returned_ssa];
			}
		} else {
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				for (uint32_t operand = 0;
						operand < instruction.operand_count; ++operand) {
					if (!value_has_typed_body_definition(
							zend_tpde_value_index(plan,
								zend_tpde_operand_at(
									plan, &instruction, operand)))) {
						return false;
					}
				}
			}
			const bool pure =
				record.opcode == ZEND_MIR_OPCODE_CONSTANT
				|| record.opcode == ZEND_MIR_OPCODE_PHI
				|| record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
				|| record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				|| record.opcode == ZEND_MIR_OPCODE_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
				|| record.opcode == ZEND_MIR_OPCODE_UNREACHABLE
				|| (record.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
					&& record.opcode <= ZEND_MIR_OPCODE_SCALAR_DROP);
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			if (!pure
					|| (record.representation
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& record.result_id != ZEND_MIR_ID_INVALID
						&& result_index >= 0
						&& !machine_plan_value_abi(
							plan, static_cast<uint32_t>(
								result_index)).valid
						&& !(record.opcode
							== ZEND_MIR_OPCODE_CONSTANT
							&& plan->values[result_index].exact_type
								== ZEND_MIR_SCALAR_TYPE_NULL))) {
				return false;
			}
			continue;
		}
		if (returned < 0 && !returned_source_type.valid) {
			return false;
		}
		zend_tpde_local_abi_type current_return =
			returned_source_type.valid
				? returned_source_type
				: machine_plan_value_abi(
					plan, static_cast<uint32_t>(returned));
		if (returned >= 0 && !current_return.valid) {
			current_return =
				call_result_types[static_cast<uint32_t>(returned)];
		}
		if (!current_return.valid
				|| (saw_return
					&& !machine_plan_abi_same_shape(
						result_type, current_return))) {
			return false;
		}
		result_type = current_return;
		saw_return = true;
	}
	if (!saw_return
			|| !machine_plan_abi_same_shape(
				result_type, plan->return_abi)) {
		return false;
	}
	*return_type = result_type;
	return true;
}

/*
 * Prefer cloning a proof-closed scalar body over emitting even the local
 * component ABI call.  The adaptor performs the final value-mapping proof;
 * this structural screen keeps the frozen machine CFG in the inline-capable
 * form for exactly the instruction family that the generic ZNMIR cloner
 * accepts.
 */
static bool machine_plan_prefers_effect_closed_inline(
		const zend_tpde_instruction &call,
		const zend_tpde_plan *callee) {
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
			|| callee->block_count != 1
			|| callee->block_ids == nullptr
			|| callee->generator_resume_count != 0
			|| callee->user_opcode_callbacks) {
		return false;
	}

	bool saw_return = false;
	bool saw_checked_binary = false;
	bool saw_string_length = false;
	for (uint32_t index = 0; index < callee->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			callee->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(callee, &instruction);
		if (record.block_id != callee->block_ids[0]) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
			if (record.effects != 0) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
				&& (instruction.runtime_helper
						== ZEND_NATIVE_HELPER_COUNT
					|| saw_string_length
					|| (callee->typed_body_return_abi.valid
						&& callee->typed_body_return_abi.exact_type
							== call.direct_call->result_type))) {
			/*
			 * The typed-body signature is established from every frozen
			 * return before component calls are classified.  Reuse that
			 * proof here: the cloned register body cannot take the boxed
			 * coercion/TypeError path retained by the observable Zend entry.
			 */
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			if (saw_return || !instruction.has_value_operation
					|| instruction.value_operation.source_opcode
						!= ZEND_RETURN) {
				return false;
			}
			saw_return = true;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
			if (saw_return || instruction.operand_count != 1) {
				return false;
			}
			saw_return = true;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE) {
			if (!zend_mir_id_is_valid(record.result_id)
					|| instruction.operand_count != 1) {
				return false;
			}
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
			if (saw_checked_binary || !instruction.has_value_operation
					|| (instruction.value_operation.source_opcode
							!= ZEND_ADD
						&& instruction.value_operation.source_opcode
							!= ZEND_SUB)
					|| call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I64) {
				return false;
			}
			saw_checked_binary = true;
			continue;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP) {
			if (saw_checked_binary || saw_string_length
					|| !instruction.has_value_operation
					|| instruction.value_operation.source_opcode
						!= ZEND_STRLEN
					|| call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I64) {
				return false;
			}
			saw_string_length = true;
			continue;
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
		if (saw_checked_binary || saw_string_length
				|| !integer_or_boolean_opcode
				|| record.effects != 0
				|| !zend_mir_id_is_valid(record.result_id)
				|| instruction.operand_count > 2) {
			return false;
		}
	}
	return saw_return;
}

static bool machine_plan_effect_closed_argument_is_used(
		const zend_tpde_plan *callee, uint32_t argument) {
	if (callee == nullptr || argument >= callee->argument_count
			|| callee->argument_value_indices == nullptr) {
		return true;
	}
	const int32_t argument_value =
		callee->argument_value_indices[argument];
	if (argument_value < 0
			|| static_cast<uint32_t>(argument_value)
				>= callee->value_count) {
		return true;
	}
	auto reaches_argument = [&](int32_t value_index) {
		for (uint32_t depth = 0;
				value_index >= 0 && depth < callee->value_count;
				++depth) {
			if (value_index == argument_value) {
				return true;
			}
			if (static_cast<uint32_t>(value_index)
					>= callee->value_count) {
				return true;
			}
			const int32_t alias =
				callee->values[static_cast<uint32_t>(value_index)]
					.register_alias_value_index;
			if (alias < 0 || alias == value_index) {
				return false;
			}
			value_index = alias;
		}
		return value_index >= 0;
	};
	auto binding_reaches_argument =
		[&](const zend_tpde_source_value_binding &binding) {
			return binding.value_index >= 0
				&& reaches_argument(binding.value_index);
		};

	for (uint32_t index = 0;
			index < callee->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			callee->instructions[index];
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			const int32_t value_index = zend_tpde_value_index(
				callee, zend_tpde_operand_at(
					callee, &instruction, operand));
			if (value_index >= 0 && reaches_argument(value_index)) {
				return true;
			}
		}
		if (binding_reaches_argument(instruction.source_op1_binding)
				|| binding_reaches_argument(
					instruction.source_op2_binding)
				|| binding_reaches_argument(
					instruction.source_auxiliary_binding)) {
			return true;
		}
	}
	return false;
}

static bool machine_value_kind_can_be_register_authoritative(
	zend_tpde_machine_value_kind kind);

static bool freeze_typed_component_calls(
		zend_tpde_plan *plan,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		bool register_a64_value_transports) {
	plan->has_register_component_results = false;
	if (plan->instruction_count == 0) {
		return true;
	}
	plan->typed_component_call_eligible = static_cast<uint8_t *>(
		std::calloc(plan->instruction_count, sizeof(uint8_t)));
	plan->effect_closed_inline_eligible = static_cast<uint8_t *>(
		std::calloc(plan->instruction_count, sizeof(uint8_t)));
	if (plan->typed_component_call_eligible == nullptr
			|| plan->effect_closed_inline_eligible == nullptr) {
		return false;
	}
	if (plan->call_argument_bindings == nullptr) {
		return true;
	}
	std::vector<zend_tpde_local_abi_type> instruction_result_types(
		plan->instruction_count);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			if (result_index >= 0) {
				instruction_result_types[index] =
					machine_plan_value_abi(
						plan, static_cast<uint32_t>(result_index));
			}
			continue;
		}
		const uint32_t target = instruction.component_target_index;
		const zend_tpde_plan *callee =
			target < component_count ? component_plans[target] : nullptr;
		if (instruction.direct_call == nullptr
				|| callee == nullptr
				|| instruction.direct_call->expected_function == nullptr
				|| (instruction.direct_call->expected_function->common.fn_flags
					& (ZEND_ACC_DEPRECATED | ZEND_ACC_NODISCARD)) != 0
				|| (instruction.entry_cell != nullptr
					&& instruction.entry_cell->function != nullptr
					&& (instruction.entry_cell->function->common.fn_flags
						& (ZEND_ACC_DEPRECATED | ZEND_ACC_NODISCARD)) != 0)
				|| instruction.call_argument_count
					!= callee->argument_count
				|| (callee->argument_count != 0
					&& callee->argument_value_indices == nullptr)
				|| instruction.call_argument_offset
					> plan->call_argument_count
				|| instruction.call_argument_count
					> plan->call_argument_count
						- instruction.call_argument_offset) {
			continue;
		}
		const bool effect_closed_inline =
			machine_plan_prefers_effect_closed_inline(
				instruction, callee);
		bool compatible = true;
		for (uint32_t argument = 0;
				argument < instruction.call_argument_count; ++argument) {
			if (effect_closed_inline
					&& !machine_plan_effect_closed_argument_is_used(
						callee, argument)) {
				continue;
			}
			zend_mir_call_argument_ref source_argument{};
			const uint32_t argument_index =
				instruction.call_argument_offset + argument;
			const int32_t callee_value =
				callee->argument_value_indices[argument];
			if (!zend_tpde_call_argument_at(
					plan, argument_index, &source_argument)
					|| callee_value < 0
					|| static_cast<uint32_t>(callee_value)
						>= callee->value_count) {
				compatible = false;
				break;
			}
			const zend_tpde_source_value_binding &binding =
				plan->call_argument_bindings[argument_index];
			zend_tpde_local_abi_type caller_abi{};
			const int32_t source_value = machine_plan_source_value_index(
				plan, source_argument.source_operand);
			if (source_value >= 0) {
				caller_abi = machine_plan_value_abi(
					plan, static_cast<uint32_t>(source_value));
			}
			if (!caller_abi.valid && binding.value_index >= 0
					&& static_cast<uint32_t>(binding.value_index)
						< plan->value_count) {
				caller_abi = machine_plan_value_abi(
					plan, static_cast<uint32_t>(binding.value_index));
			}
			if (!caller_abi.valid
					&& binding.definition_instruction_index >= 0
					&& static_cast<uint32_t>(
						binding.definition_instruction_index)
						< instruction_result_types.size()) {
				caller_abi = instruction_result_types[
					static_cast<uint32_t>(
						binding.definition_instruction_index)];
			}
			const zend_tpde_local_abi_type callee_abi =
				machine_plan_value_abi(
					callee, static_cast<uint32_t>(callee_value));
			const zend_tpde_instruction *property_producer =
				binding.definition_instruction_index >= 0
						&& static_cast<uint32_t>(
							binding.definition_instruction_index)
							< plan->instruction_count
					? &plan->instructions[static_cast<uint32_t>(
						binding.definition_instruction_index)]
					: nullptr;
			const zend_tpde_machine_reference *property_reference =
				property_producer != nullptr
						&& property_producer->operation_reference_index
							< plan->machine_reference_count
					? &plan->machine_references[
						property_producer->operation_reference_index]
					: nullptr;
			zend_tpde_object_property_read property_layout{};
			const bool guarded_property_pointer_transport =
				register_a64_value_transports
				&& !effect_closed_inline
				&& callee_abi.valid
				&& machine_value_kind_can_be_register_authoritative(
					callee_abi.machine_kind)
				&& callee_abi.representation
					== ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				&& property_producer != nullptr
				&& property_producer->record.opcode
					== ZEND_MIR_OPCODE_OBJECT_FETCH_R
				&& zend_tpde_object_property_read_at(
					*property_producer, &property_layout)
				&& property_reference != nullptr
				&& property_reference->kind
					== ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT
				&& property_reference->stable_storage_or_layout_id
					== property_layout.cache_offset
				&& property_reference->access_width == sizeof(zval)
				&& argument < instruction.direct_call->argument_count
				&& instruction.direct_call->arguments[argument].exact_type
					== callee_abi.exact_type
				&& source_argument.ownership
					!= ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
			if (guarded_property_pointer_transport) {
				/*
				 * A cached property read produces a two-part register zval.
				 * The AArch64 entry guard checks its runtime type before the
				 * hot block extracts the refcounted payload.  Model the value
				 * as the callee's borrowed pointer only after that guard; the
				 * materialized cold call retains canonical Zend semantics.
				 */
				caller_abi = callee_abi;
			}
			if (effect_closed_inline
					&& source_value >= 0
					&& static_cast<uint32_t>(source_value)
						< plan->value_count) {
				const zend_tpde_value &source =
					plan->values[static_cast<uint32_t>(source_value)];
				/*
				 * A canonical refcounted value is a valid source for an
				 * effect-closed local ABI after one payload load.  Keep its
				 * Zend slot authoritative until the cloned body, but compare
				 * the transported pointer shape here so the whole call can be
				 * removed without manufacturing a private Zend frame.
				 */
				if (!caller_abi.valid
						&& source.machine_kind == callee_abi.machine_kind
						&& zend_mir_id_is_valid(
							source.canonical_storage_id)
						&& machine_value_kind_can_be_register_authoritative(
							source.machine_kind)) {
					caller_abi = callee_abi;
				} else if (source.representation
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& source.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& (source.exact_type
								== callee_abi.exact_type
							|| (source.exact_type
									== ZEND_MIR_SCALAR_TYPE_NONE
								&& argument
									< instruction.direct_call
										->argument_count
								&& instruction.direct_call
										->arguments[argument].exact_type
									== callee_abi.exact_type))
						&& (callee_abi.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_I64
							|| callee_abi.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_BOOL)
						&& zend_mir_id_is_valid(
							source.canonical_storage_id)) {
					caller_abi = callee_abi;
				} else if (!caller_abi.valid
						&& source.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
						&& callee_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_I64
						&& argument < instruction.direct_call->argument_count
						&& instruction.direct_call->arguments[argument].exact_type
							== callee_abi.exact_type
						&& zend_mir_id_is_valid(source.canonical_storage_id)
						&& source_argument.send_opline_index
							< plan->source_opcode_count
						&& plan->source_opcodes[
							source_argument.send_opline_index].op1_type == IS_CV) {
					/*
					 * By-value SEND dereferences a reference-cell CV. The direct-call
					 * descriptor freezes the exact scalar payload type, allowing an
					 * effect-closed clone to perform the same payload load locally.
					 */
					caller_abi = callee_abi;
				}
			}
			const bool by_reference =
				source_argument.ownership
					== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
			if (!machine_plan_call_argument_can_supply(
					plan, source_argument, caller_abi, callee_abi)
					|| by_reference
						!= (callee_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
				compatible = false;
				break;
			}
		}
		if (!compatible) {
			continue;
		}
		if (effect_closed_inline) {
			plan->effect_closed_inline_eligible[index] = 1;
			instruction.machine_control_flow_flags |=
				ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD;
			plan->has_register_component_results = true;
			continue;
		}
		if (instruction.direct_call->receiver_kind
				!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE) {
			continue;
		}
		if (!callee->typed_body_eligible
				|| !callee->typed_body_return_abi.valid) {
			continue;
		}
		plan->typed_component_call_eligible[index] = 1;
		instruction.component_body_function_index =
			callee->typed_body_function_index;
		instruction_result_types[index] =
			callee->typed_body_return_abi;
		plan->has_register_component_results = true;
	}
	return true;
}

static bool machine_plan_value_has_result_representation(
	const zend_tpde_plan *plan, zend_mir_value_id value_id);

static bool retain_typed_call_materializations(
		zend_tpde_plan *plan,
		zend_native_diagnostic *diag) {
	if (plan->materialization_count == 0) {
		return true;
	}
	std::vector<zend_mir_storage_id> lazy_scalar_storages;
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction = plan->instructions[index];
		const zend_mir_storage_id storage_id =
			instruction.zval_store_lazy_scalar
				? instruction.zval_store_storage_id
				: instruction.mutation_lazy_scalar
					? instruction.mutation_storage_id
					: ZEND_MIR_ID_INVALID;
		if (zend_mir_id_is_valid(storage_id)) {
			lazy_scalar_storages.push_back(storage_id);
		}
	}
	std::ranges::sort(lazy_scalar_storages);
	lazy_scalar_storages.erase(
		std::unique(lazy_scalar_storages.begin(), lazy_scalar_storages.end()),
		lazy_scalar_storages.end());
	auto lazy_join_has_machine_source = [&](uint32_t value_index) -> bool {
		if (value_index >= plan->value_count) {
			return false;
		}
		const int32_t definition =
			plan->value_definition_instructions == nullptr
				? -1 : plan->value_definition_instructions[value_index];
		if (definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[static_cast<uint32_t>(definition)];
		const zend_mir_opcode opcode =
			zend_tpde_instruction_record_at(plan, &instruction).opcode;
		if (opcode != ZEND_MIR_OPCODE_COPY
				&& opcode != ZEND_MIR_OPCODE_PHI) {
			return false;
		}
		for (uint32_t operand = 0;
				operand < instruction.operand_count; ++operand) {
			const zend_mir_value_id operand_id =
				zend_tpde_operand_at(plan, &instruction, operand);
			if (machine_plan_value_has_result_representation(
						plan, operand_id)) {
				return true;
			}
		}
		return false;
	};
	std::vector<zend_tpde_materialization> retained;
	retained.reserve(plan->materialization_count);
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		zend_tpde_instruction &instruction = plan->instructions[index];
		const uint32_t old_offset = instruction.materialization_offset;
		const uint32_t old_count = instruction.materialization_count;
		instruction.materialization_offset =
			static_cast<uint32_t>(retained.size());
		instruction.materialization_count = 0;
		if (old_offset > plan->materialization_count
				|| old_count > plan->materialization_count - old_offset) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"component materialization slice is out of bounds");
			return false;
		}
		for (uint32_t offset = 0; offset < old_count; ++offset) {
			const zend_tpde_materialization &materialization =
				plan->materializations[old_offset + offset];
			const bool lazy_scalar = std::binary_search(
				lazy_scalar_storages.begin(), lazy_scalar_storages.end(),
				materialization.storage_id);
			if (materialization.value_index == UINT32_MAX) {
				const int32_t definition =
					materialization.source_definition_instruction_index;
				if (definition < 0
						|| static_cast<uint32_t>(definition)
							>= plan->instruction_count) {
					continue;
				}
				const uint32_t definition_index =
					static_cast<uint32_t>(definition);
				const zend_tpde_instruction &definition_instruction =
					plan->instructions[definition_index];
				const bool machine_definition =
					(lazy_scalar
						&& definition_instruction.mutation_lazy_scalar)
					|| (definition_instruction.has_value_operation
						&& definition_instruction.value_operation.source_opcode
							== ZEND_STRLEN)
					|| (plan->typed_component_call_eligible != nullptr
						&& plan->typed_component_call_eligible[
							definition_index] != 0)
					|| (plan->effect_closed_inline_eligible != nullptr
						&& plan->effect_closed_inline_eligible[
							definition_index] != 0);
				if (!machine_definition) {
					continue;
				}
			} else {
				if (materialization.value_index >= plan->value_count) {
					continue;
				}
				const zend_tpde_value &value =
					plan->values[materialization.value_index];
				const int32_t definition =
					plan->value_definition_instructions == nullptr
						? -1
						: plan->value_definition_instructions[
							materialization.value_index];
				const zend_mir_opcode definition_opcode =
					definition >= 0
						&& static_cast<uint32_t>(definition)
							< plan->instruction_count
					? zend_tpde_instruction_record_at(
						plan, &plan->instructions[
							static_cast<uint32_t>(definition)]).opcode
					: ZEND_MIR_OPCODE_INVALID;
				const bool lazy_scalar_join = lazy_scalar
					&& value.representation == ZEND_MIR_REPRESENTATION_ZVAL
					&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& !value.constant
					&& !value.canonical_alias_observable
					&& (definition_opcode == ZEND_MIR_OPCODE_COPY
						|| definition_opcode == ZEND_MIR_OPCODE_PHI)
					&& lazy_join_has_machine_source(
						materialization.value_index);
				if (!lazy_scalar_join
						&& !machine_plan_value_has_result_representation(
							plan, value.id)) {
					continue;
				}
			}
			retained.push_back(materialization);
			++instruction.materialization_count;
		}
	}

	zend_tpde_materialization *replacement = nullptr;
	if (!retained.empty()) {
		replacement = static_cast<zend_tpde_materialization *>(
			std::malloc(retained.size() * sizeof(*replacement)));
		if (replacement == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to retain component materialization plan");
			return false;
		}
		std::memcpy(replacement, retained.data(),
			retained.size() * sizeof(*replacement));
	}
	std::free(plan->materializations);
	plan->materializations = replacement;
	plan->materialization_count =
		static_cast<uint32_t>(retained.size());
	return true;
}

static void destroy_machine_cfg(zend_tpde_machine_cfg *cfg) {
	std::free(cfg->successor_offsets);
	std::free(cfg->successors);
	std::free(cfg->instruction_blocks);
	std::free(cfg->guarded_cold_blocks);
	std::free(cfg->guarded_hot_blocks);
	std::free(cfg->guarded_continuation_blocks);
	std::free(cfg->final_blocks);
	std::free(cfg->boxed_cond_cold_blocks);
	std::free(cfg->boxed_cond_cold_by_predecessor);
	std::memset(cfg, 0, sizeof(*cfg));
}

static bool freeze_machine_cfg_array(
		uint32_t **out,
		const std::vector<uint32_t> &values) {
	if (values.empty()) {
		*out = nullptr;
		return true;
	}
	*out = static_cast<uint32_t *>(
		std::malloc(values.size() * sizeof(**out)));
	if (*out == nullptr) {
		return false;
	}
	std::memcpy(*out, values.data(), values.size() * sizeof(**out));
	return true;
}

static int32_t machine_cfg_register_cond_branch_value_index(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction) {
	int32_t value_index = instruction.source_op1_binding.value_index;
	if (value_index >= 0) {
		return value_index;
	}
	zend_mir_value_id value_id;
	if (!source_operand_value_id(
			instruction.value_operation.op1, value_id)) {
		return -1;
	}
	return zend_tpde_value_index(plan, value_id);
}

static bool machine_cfg_register_cond_branch(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction,
		bool typed_body) {
	if ((instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) == 0) {
		return false;
	}
	if ((instruction.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE) != 0) {
		return true;
	}
	const int32_t value_index =
		machine_cfg_register_cond_branch_value_index(plan, instruction);
	if (value_index < 0) {
		return typed_body
			&& instruction.value_operation.op1.ssa_variable_id
				!= ZEND_MIR_ID_INVALID;
	}
	const zend_tpde_value &value =
		plan->values[static_cast<uint32_t>(value_index)];
	const bool machine_condition =
		(value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
			&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL)
		|| (value.exact_type == ZEND_MIR_SCALAR_TYPE_I64
			&& value.machine_kind == ZEND_TPDE_MACHINE_VALUE_I64);
	if (machine_condition
			&& (typed_body || value.register_authoritative)) {
		return true;
	}
	/*
	 * Exact entry isset/empty and BOOL chains acquire their I1 result during
	 * adaptor construction.  Their canonical source value deliberately remains
	 * a zval, so recognize the frozen register-result producer before building
	 * the boxed conditional diamond.
	 */
	if (typed_body || plan->value_definition_instructions == nullptr) {
		return false;
	}
	int32_t definition =
		instruction.source_op1_binding.definition_instruction_index;
	if (definition < 0) {
		definition = plan->value_definition_instructions[
			static_cast<uint32_t>(value_index)];
	}
	if (definition < 0) {
		definition = plan->source_value_definition_instructions == nullptr
			? -1
			: plan->source_value_definition_instructions[
				static_cast<uint32_t>(value_index)];
	}
	if (definition < 0
			|| static_cast<uint32_t>(definition) >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &producer =
		plan->instructions[static_cast<uint32_t>(definition)];
	if ((producer.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) == 0) {
		return false;
	}
	const zend_mir_instruction_record producer_record =
		zend_tpde_instruction_record_at(plan, &producer);
	if (producer_record.opcode == ZEND_MIR_OPCODE_PHI) {
		return true;
	}
	if (producer_record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
			&& producer.direct_call != nullptr
			&& producer.direct_call->result_type
				== ZEND_MIR_SCALAR_TYPE_I1
			&& ((plan->typed_component_call_eligible != nullptr
					&& plan->typed_component_call_eligible[
						static_cast<uint32_t>(definition)] != 0)
				|| (plan->effect_closed_inline_eligible != nullptr
					&& plan->effect_closed_inline_eligible[
						static_cast<uint32_t>(definition)] != 0))) {
		return true;
	}
	if (!producer.has_value_operation) {
		return false;
	}
	return producer_record.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
		|| (producer_record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
			&& (producer.value_operation.source_opcode == ZEND_BOOL
				|| producer.value_operation.source_opcode == ZEND_BOOL_NOT));
}

static bool machine_value_kind_can_be_register_authoritative(
		zend_tpde_machine_value_kind kind) {
	return kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
		|| kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
}

static bool machine_plan_value_has_result_representation(
		const zend_tpde_plan *plan, zend_mir_value_id value_id) {
	const int32_t value_index = zend_tpde_value_index(plan, value_id);
	if (value_index < 0) {
		return false;
	}
	const zend_tpde_value &value =
		plan->values[static_cast<uint32_t>(value_index)];
	const int32_t definition =
		plan->value_definition_instructions == nullptr
			? -1 : plan->value_definition_instructions[value_index];
	if (definition >= 0
			&& static_cast<uint32_t>(definition) < plan->instruction_count
			&& plan->instructions[static_cast<uint32_t>(definition)]
				.transient_scalar_result) {
		return true;
	}
	if (zend_mir_scalar_type_is_exact(value.exact_type)
			&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL) {
		return value.constant || value.register_authoritative;
	}
	return machine_value_kind_can_be_register_authoritative(
			value.machine_kind)
		&& value.register_authoritative;
}

static bool machine_plan_value_needs_result_assignment(
		const zend_tpde_plan *plan, zend_mir_value_id value_id) {
	const int32_t value_index = zend_tpde_value_index(plan, value_id);
	return value_index >= 0
		&& !plan->values[static_cast<uint32_t>(value_index)].constant
		&& machine_plan_value_has_result_representation(plan, value_id);
}

static int32_t machine_plan_guarded_mutation_value_index(
		const zend_tpde_plan *plan,
		const zend_tpde_instruction &instruction) {
	zend_tpde_long_assign_op long_assign{};
	zend_tpde_long_incdec long_incdec{};
	if (!instruction.has_value_operation
			|| !instruction.mutation_lazy_scalar
			|| !((zend_tpde_long_assign_op_at(
						instruction, &long_assign)
						&& !long_assign.has_result)
					|| (zend_tpde_long_incdec_at(
						instruction, &long_incdec)
						&& !long_incdec.has_result))
			|| instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one == 0) {
		return -1;
	}
	return zend_tpde_value_index(
		plan, zend_mir_value_from_original_ssa(
			instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one - 1));
}

static void freeze_machine_scalar_definitions(zend_tpde_plan *plan) {
	if (plan == nullptr || plan->values == nullptr
			|| plan->instructions == nullptr) {
		return;
	}

	/*
	 * Scalar MIR opcodes define their descriptor-selected machine payload even
	 * when the corresponding source SSA identity still has a canonical ZVAL
	 * representation.  Freeze that payload identity before register authority
	 * and operand transports are computed.  COPY and PHI then propagate it to a
	 * fixed point so loop-carried scalar values have real backedge definitions
	 * instead of repeatedly reloading the loop's initial canonical slot.
	 */
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &plan->instructions[index]);
		const zend_mir_scalar_descriptor *descriptor =
			zend_mir_scalar_descriptor_at(record.opcode);
		if (descriptor == nullptr || !descriptor->has_result
				|| !zend_mir_scalar_type_is_exact(
				descriptor->result.exact_type)
				|| descriptor->result.exact_type
					== ZEND_MIR_SCALAR_TYPE_NULL) {
			continue;
		}
		const int32_t result_index =
			zend_tpde_value_index(plan, record.result_id);
		if (result_index < 0) {
			continue;
		}
		zend_tpde_value &result =
			plan->values[static_cast<uint32_t>(result_index)];
		if (result.constant || result.canonical_alias_observable) {
			continue;
		}
		result.representation = descriptor->result.representation;
		result.exact_type = descriptor->result.exact_type;
		result.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
		result.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
		(void) zend_tpde_apply_machine_value_facts(&result, true);
	}

	auto scalar_machine_definition = [](const zend_tpde_value &value) {
		return value.constant
			|| (value.representation != ZEND_MIR_REPRESENTATION_ZVAL
				&& zend_mir_scalar_type_is_exact(value.exact_type)
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& (value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER
					|| (value.argument_index >= 0
						&& value.local_abi.valid)));
	};

	/*
	 * A deferred COPY/PHI input is safe only when its entire same-slot
	 * dependency graph consists of other scalar-selectable COPY/PHI values.
	 * Prune candidates backwards from boxed or pointer boundaries before the
	 * fixed point below.  This prevents one scalar seed from promoting only
	 * half of a mixed scalar/zval loop cycle.
	 */
	std::vector<uint8_t> scalar_copy_phi_candidates(plan->value_count);
	for (uint32_t value_index = 0;
			value_index < plan->value_count; ++value_index) {
		const zend_tpde_value &value = plan->values[value_index];
		const int32_t definition =
			plan->value_definition_instructions == nullptr
				? -1 : plan->value_definition_instructions[value_index];
		if (definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count
				|| value.constant || value.canonical_alias_observable
				|| value.representation != ZEND_MIR_REPRESENTATION_ZVAL) {
			continue;
		}
		const zend_tpde_instruction &instruction =
			plan->instructions[static_cast<uint32_t>(definition)];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if ((record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_PHI)
				&& record.representation == ZEND_MIR_REPRESENTATION_ZVAL
				&& instruction.operand_count != 0) {
			scalar_copy_phi_candidates[value_index] = 1;
		}
	}

	bool changed;
	do {
		changed = false;
		for (uint32_t value_index = 0;
				value_index < plan->value_count; ++value_index) {
			if (scalar_copy_phi_candidates[value_index] == 0) {
				continue;
			}
			const zend_tpde_value &result = plan->values[value_index];
			const uint32_t definition = static_cast<uint32_t>(
				plan->value_definition_instructions[value_index]);
			const zend_tpde_instruction &instruction =
				plan->instructions[definition];
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const int32_t input_index = zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &instruction, operand));
				if (input_index < 0) {
					scalar_copy_phi_candidates[value_index] = 0;
					changed = true;
					break;
				}
				const zend_tpde_value &input =
					plan->values[static_cast<uint32_t>(input_index)];
				if (scalar_machine_definition(input)) {
					continue;
				}
				const bool deferred_candidate =
					input.representation == ZEND_MIR_REPRESENTATION_ZVAL
					&& !input.canonical_alias_observable
					&& input.canonical_storage_id
						== result.canonical_storage_id
					&& scalar_copy_phi_candidates[
						static_cast<uint32_t>(input_index)] != 0;
				if (!deferred_candidate) {
					scalar_copy_phi_candidates[value_index] = 0;
					changed = true;
					break;
				}
			}
		}
	} while (changed);

	do {
		changed = false;
		for (uint32_t index = 0; index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if ((record.opcode != ZEND_MIR_OPCODE_COPY
					&& record.opcode != ZEND_MIR_OPCODE_PHI)
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL
					|| instruction.operand_count == 0) {
				continue;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan, record.result_id);
			if (result_index < 0) {
				continue;
			}
			zend_tpde_value &result =
				plan->values[static_cast<uint32_t>(result_index)];
			if (result.constant || result.canonical_alias_observable
					|| result.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL) {
				continue;
			}

			zend_mir_scalar_type_mask input_type =
				ZEND_MIR_SCALAR_TYPE_NONE;
			zend_mir_representation input_representation =
				ZEND_MIR_REPRESENTATION_VOID;
			zend_tpde_machine_value_kind input_kind =
				ZEND_TPDE_MACHINE_VALUE_I64;
			bool compatible = true;
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const int32_t input_index = zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &instruction, operand));
				if (input_index < 0) {
					compatible = false;
					break;
				}
				const zend_tpde_value &input =
					plan->values[static_cast<uint32_t>(input_index)];
				const bool machine_definition =
					scalar_machine_definition(input);
				if (!machine_definition) {
					const int32_t input_definition =
						plan->value_definition_instructions == nullptr
							? -1
							: plan->value_definition_instructions[input_index];
					const zend_mir_instruction_record input_record =
						input_definition >= 0
								&& static_cast<uint32_t>(input_definition)
									< plan->instruction_count
							? zend_tpde_instruction_record_at(
								plan, &plan->instructions[
									static_cast<uint32_t>(input_definition)])
							: zend_mir_instruction_record{};
					/*
					 * A loop-carried ZVAL component can be cyclic, so one incoming
					 * COPY/PHI may not have been selected yet.  Defer that edge when
					 * it names the same private canonical slot; an arbitrary boxed
					 * argument or value-operation result remains a hard boundary.
					 */
					if (input.representation == ZEND_MIR_REPRESENTATION_ZVAL
							&& !input.canonical_alias_observable
							&& input.canonical_storage_id
								== result.canonical_storage_id
							&& (input_record.opcode == ZEND_MIR_OPCODE_COPY
								|| input_record.opcode == ZEND_MIR_OPCODE_PHI)
							&& scalar_copy_phi_candidates[
								static_cast<uint32_t>(input_index)] != 0) {
						continue;
					}
					compatible = false;
					break;
				}
				if (!zend_mir_scalar_type_is_exact(input.exact_type)
						|| input.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
						|| (input_type != ZEND_MIR_SCALAR_TYPE_NONE
							&& (input.exact_type != input_type
								|| input.representation
									!= input_representation
								|| input.machine_kind != input_kind))) {
					compatible = false;
					break;
				}
				if (input_type == ZEND_MIR_SCALAR_TYPE_NONE) {
					input_type = input.exact_type;
					input_representation = input.representation;
					input_kind = input.machine_kind;
				}
			}
			if (!compatible
					|| input_type == ZEND_MIR_SCALAR_TYPE_NONE
					|| input_representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				continue;
			}
			result.representation = input_representation;
			result.exact_type = input_type;
			result.category = ZEND_MIR_VALUE_NON_REFCOUNTED_SCALAR;
			result.refcount_state = ZEND_MIR_REFCOUNT_IMMORTAL;
			(void) zend_tpde_apply_machine_value_facts(&result, true);
			changed = true;
		}
	} while (changed);
}

static void freeze_machine_register_authority(
		zend_tpde_plan *plan, bool register_direct_scalar_results) {
	freeze_machine_scalar_definitions(plan);
	for (uint32_t index = 0; index < plan->value_count; ++index) {
		zend_tpde_value &value = plan->values[index];
		const int32_t definition =
			plan->value_definition_instructions != nullptr
				? plan->value_definition_instructions[index] : -1;
		const zend_mir_instruction_record definition_record =
			definition >= 0
					&& static_cast<uint32_t>(definition)
						< plan->instruction_count
				? zend_tpde_instruction_record_at(
					plan, &plan->instructions[
						static_cast<uint32_t>(definition)])
				: zend_mir_instruction_record{};
		const bool boxed_join =
			value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& value.representation == ZEND_MIR_REPRESENTATION_ZVAL
			&& (definition_record.opcode == ZEND_MIR_OPCODE_COPY
				|| definition_record.opcode == ZEND_MIR_OPCODE_PHI);
		const bool argument_abi =
			value.argument_index >= 0 && value.local_abi.valid;
		const zend_mir_scalar_type_mask exact_type =
			argument_abi ? value.local_abi.exact_type : value.exact_type;
		const zend_tpde_machine_value_kind machine_kind =
			argument_abi ? value.local_abi.machine_kind : value.machine_kind;
		value.register_authoritative =
			!value.constant
			&& !boxed_join
			&& exact_type != ZEND_MIR_SCALAR_TYPE_NULL
			&& (argument_abi
				|| value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER)
			&& (argument_abi
				|| (zend_mir_scalar_type_is_exact(exact_type)
					&& exact_type != ZEND_MIR_SCALAR_TYPE_NULL)
				|| machine_value_kind_can_be_register_authoritative(
					machine_kind));
	}

	for (uint32_t index = 0;
			index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const int32_t mutation =
			machine_plan_guarded_mutation_value_index(
				plan, instruction);
		if (mutation >= 0) {
			zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(mutation)];
			if ((zend_mir_scalar_type_is_exact(value.exact_type)
						&& value.exact_type
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| machine_value_kind_can_be_register_authoritative(
						value.machine_kind)) {
				value.register_authoritative = true;
			}
		}

		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const int32_t result =
				zend_tpde_value_index(plan, record.result_id);
			if (result >= 0) {
				zend_tpde_value &value = plan->values[result];
				if (value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| (register_direct_scalar_results
							&& zend_mir_scalar_type_is_exact(
								value.exact_type)
							&& value.exact_type
								!= ZEND_MIR_SCALAR_TYPE_NULL)) {
					value.register_authoritative = true;
				}
			}
		}
	}

	bool changed;
	do {
		changed = false;
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if ((record.opcode != ZEND_MIR_OPCODE_COPY
						&& record.opcode != ZEND_MIR_OPCODE_PHI)
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL
					|| instruction.operand_count == 0) {
				continue;
			}
			const int32_t result =
				zend_tpde_value_index(plan, record.result_id);
			if (result < 0
					|| plan->values[result].machine_kind
						!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					|| plan->values[result].register_authoritative) {
				continue;
			}
			bool all_inputs_register = true;
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const int32_t input = zend_tpde_value_index(
					plan, zend_tpde_operand_at(
						plan, &instruction, operand));
				if (input < 0
						|| plan->values[input].machine_kind
							!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| !plan->values[input]
							.register_authoritative) {
					all_inputs_register = false;
					break;
				}
			}
			if (all_inputs_register) {
				plan->values[result].register_authoritative = true;
				changed = true;
			}
		}
	} while (changed);
}

static bool machine_plan_local_abi_call_eligible(
		const zend_tpde_plan *plan, uint32_t instruction_index) {
	if (plan == nullptr || instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(
			plan, &plan->instructions[instruction_index]);
	return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		&& ((plan->typed_component_call_eligible != nullptr
				&& plan->typed_component_call_eligible[instruction_index] != 0)
			|| (plan->effect_closed_inline_eligible != nullptr
				&& plan->effect_closed_inline_eligible[instruction_index] != 0));
}

static bool machine_plan_call_arguments_require_values(
		const zend_tpde_plan *plan,
		uint32_t instruction_index,
		bool register_a64_value_transports) {
	if (machine_plan_local_abi_call_eligible(plan, instruction_index)) {
		return true;
	}
	if (!register_a64_value_transports
			|| plan == nullptr
			|| instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &instruction =
		plan->instructions[instruction_index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &instruction);
	return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
		&& instruction.direct_call != nullptr
		&& (instruction.direct_call->flags
			& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
}

static bool machine_plan_use_requires_value(
		const zend_tpde_plan *plan,
		const zend_tpde_machine_use &use,
		bool typed_body) {
	if (use.instruction_index >= plan->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &instruction =
		plan->instructions[use.instruction_index];
	const zend_mir_instruction_record record =
		zend_tpde_instruction_record_at(plan, &instruction);
	switch (use.kind) {
		case ZEND_TPDE_MACHINE_USE_STATEPOINT_MATERIALIZATION:
		case ZEND_TPDE_MACHINE_USE_SUSPEND_LIVE:
			return true;
		case ZEND_TPDE_MACHINE_USE_LOCAL_ABI_ARGUMENT:
			return machine_plan_local_abi_call_eligible(
				plan, use.instruction_index);
		case ZEND_TPDE_MACHINE_USE_CALL_ARGUMENT:
			return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr
				&& (instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
		case ZEND_TPDE_MACHINE_USE_PHI_EDGE:
			return machine_plan_value_needs_result_assignment(
				plan, record.result_id);
		case ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND:
			return false;
		case ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND:
			break;
	}

	const bool machine_result =
		machine_plan_value_needs_result_assignment(
			plan, record.result_id);
	if (record.opcode == ZEND_MIR_OPCODE_COPY
			&& record.representation == ZEND_MIR_REPRESENTATION_ZVAL) {
		return machine_result && use.operand_index == 0;
	}
	if (!machine_result
			&& (record.opcode == ZEND_MIR_OPCODE_COPY
				|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
				|| record.opcode == ZEND_MIR_OPCODE_I1_TO_I64)) {
		return false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
		return use.operand_index == 0;
	}
	if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
			&& (typed_body || plan->has_register_component_results)) {
		return use.operand_index == 0;
	}
	const bool boxed_cond_branch =
		(instruction.machine_control_flow_flags
			& ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH) != 0
		&& !machine_cfg_register_cond_branch(
			plan, instruction, typed_body);
	if (boxed_cond_branch
			|| record.opcode == ZEND_MIR_OPCODE_STATEPOINT
			|| (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
				&& !typed_body && !plan->has_register_component_results)
			|| record.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
			|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr)) {
		return false;
	}
	return use.operand_index < instruction.operand_count;
}

static bool freeze_machine_operand_transports(
		zend_tpde_plan *plan, zend_native_diagnostic *diag) {
	if (plan->instruction_operand_count == 0) {
		return true;
	}
	plan->instruction_operand_transports =
		static_cast<zend_tpde_operand_transport *>(std::calloc(
			plan->instruction_operand_count,
			sizeof(*plan->instruction_operand_transports)));
	if (plan->instruction_operand_transports == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze machine operand transports");
		return false;
	}

	/*
	 * Select transient definitions before classifying individual uses.  The
	 * source scalar is not authoritative after ZVAL_STORE, but the store must
	 * consume the newly computed payload rather than reload the old destination
	 * zval.  Keeping this in the frozen plan makes the adaptor a mechanical
	 * consumer of an explicit definition/use transport.
	 */
	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &store =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &store);
		if (record.opcode != ZEND_MIR_OPCODE_ZVAL_STORE
				|| store.operand_count == 0) {
			continue;
		}
		const int32_t value_index = zend_tpde_value_index(
			plan, zend_tpde_operand_at(plan, &store, 0));
		if (value_index < 0) {
			continue;
		}
		const zend_tpde_value &value =
			plan->values[static_cast<uint32_t>(value_index)];
		const int32_t definition =
			plan->value_definition_instructions == nullptr
				? -1 : plan->value_definition_instructions[value_index];
		if (value.constant || value.register_authoritative
				|| !zend_mir_scalar_type_is_exact(value.exact_type)
				|| value.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
				|| definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan->instruction_count) {
			continue;
		}
		zend_tpde_instruction &defining =
			plan->instructions[static_cast<uint32_t>(definition)];
		defining.transient_scalar_result = true;
		defining.transient_result_representation =
			value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				? ZEND_MIR_REPRESENTATION_I1
			: value.exact_type == ZEND_MIR_SCALAR_TYPE_F64
				? ZEND_MIR_REPRESENTATION_DOUBLE
				: ZEND_MIR_REPRESENTATION_I64;
		defining.transient_result_exact_type = value.exact_type;
		defining.transient_result_machine_kind =
			value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				? ZEND_TPDE_MACHINE_VALUE_BOOL
			: value.exact_type == ZEND_MIR_SCALAR_TYPE_F64
				? ZEND_TPDE_MACHINE_VALUE_F64
				: ZEND_TPDE_MACHINE_VALUE_I64;
		defining.transient_result_storage_id = value.canonical_storage_id;
	}

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		for (uint32_t operand_index = 0;
				operand_index < instruction.operand_count;
				++operand_index) {
			const zend_tpde_machine_use use{
				instruction_index,
				operand_index,
				record.opcode == ZEND_MIR_OPCODE_PHI
					? operand_index : UINT32_MAX,
				record.opcode == ZEND_MIR_OPCODE_PHI
					? ZEND_TPDE_MACHINE_USE_PHI_EDGE
					: ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND,
			};
			if (!machine_plan_use_requires_value(plan, use, false)
					&& !machine_plan_use_requires_value(plan, use, true)) {
				continue;
			}
			const int32_t value_index = zend_tpde_value_index(
				plan, zend_tpde_operand_at(
					plan, &instruction, operand_index));
			if (value_index < 0) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"machine operand transport references an unknown value");
				return false;
			}
			const zend_tpde_value &value =
				plan->values[static_cast<uint32_t>(value_index)];
			zend_mir_scalar_type_mask transport_type = value.exact_type;
			zend_mir_representation transport_representation =
				value.representation;
			const zend_mir_scalar_descriptor *scalar_descriptor =
				zend_mir_scalar_descriptor_at(record.opcode);
			if (!zend_mir_scalar_type_is_exact(transport_type)
					&& value.representation == ZEND_MIR_REPRESENTATION_ZVAL
					&& scalar_descriptor != nullptr
					&& operand_index < scalar_descriptor->operand_count
					&& zend_mir_scalar_type_is_exact(
						scalar_descriptor->operands[operand_index].exact_type)) {
				/*
				 * Scalar lowering may consume a canonical ZVAL identity using a
				 * stronger source proof than the merged MIR value fact.  Freeze the
				 * descriptor-required payload load here, while representation
				 * selection still owns the transition, rather than presenting the
				 * register allocator with an undefined ZVAL machine value.
				 */
				transport_type =
					scalar_descriptor->operands[operand_index].exact_type;
				transport_representation =
					scalar_descriptor->operands[operand_index].representation;
			}
			const int32_t definition =
				plan->value_definition_instructions == nullptr
					? -1 : plan->value_definition_instructions[value_index];
			const bool transient_store_use =
				record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
				&& operand_index == 0
				&& definition >= 0
				&& static_cast<uint32_t>(definition)
					< plan->instruction_count
				&& plan->instructions[static_cast<uint32_t>(definition)]
					.transient_scalar_result;
			if (value.constant || value.register_authoritative
					|| transient_store_use
					|| transport_type == ZEND_MIR_SCALAR_TYPE_NULL
					|| !zend_mir_scalar_type_is_exact(transport_type)) {
				continue;
			}
			if (!zend_mir_id_is_valid(value.canonical_storage_id)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"slot-authoritative scalar has no canonical storage");
				return false;
			}
			zend_tpde_operand_transport &transport =
				plan->instruction_operand_transports[
					instruction.operand_offset + operand_index];
			transport = {
				ZEND_TPDE_OPERAND_TRANSPORT_CANONICAL_SCALAR_LOAD,
				transport_representation,
				transport_type,
				transport_type == ZEND_MIR_SCALAR_TYPE_I1
					? ZEND_TPDE_MACHINE_VALUE_BOOL
				: transport_type == ZEND_MIR_SCALAR_TYPE_F64
					? ZEND_TPDE_MACHINE_VALUE_F64
					: ZEND_TPDE_MACHINE_VALUE_I64,
				value.canonical_storage_id,
				value.canonical_alias_observable
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR,
			};
		}
	}
	return true;
}

static bool freeze_machine_required_values(
		zend_tpde_plan *plan,
		bool register_a64_value_transports,
		zend_native_diagnostic *diag) {
	if (plan->value_count == 0) {
		return true;
	}
	plan->entry_value_required = static_cast<uint8_t *>(
		std::calloc(plan->value_count, sizeof(uint8_t)));
	plan->typed_body_value_required = static_cast<uint8_t *>(
		std::calloc(plan->value_count, sizeof(uint8_t)));
	if (plan->entry_value_required == nullptr
			|| plan->typed_body_value_required == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to freeze machine-plan value uses");
		return false;
	}

	if (plan->call_argument_bindings != nullptr) {
		for (uint32_t instruction_index = 0;
				instruction_index < plan->instruction_count;
				++instruction_index) {
			if (!machine_plan_call_arguments_require_values(
					plan, instruction_index,
					register_a64_value_transports)) {
				continue;
			}
			const zend_tpde_instruction &instruction =
				plan->instructions[instruction_index];
			if (instruction.call_argument_offset > plan->call_argument_count
					|| instruction.call_argument_count
						> plan->call_argument_count
							- instruction.call_argument_offset) {
				continue;
			}
			for (uint32_t argument = 0;
					argument < instruction.call_argument_count;
					++argument) {
				const uint32_t argument_index =
					instruction.call_argument_offset + argument;
				const int32_t value_index =
					plan->call_argument_bindings[argument_index].value_index;
				if (value_index < 0
						|| static_cast<uint32_t>(value_index)
							>= plan->value_count) {
					continue;
				}
				plan->entry_value_required[value_index] = 1;
				plan->typed_body_value_required[value_index] = 1;
			}
		}
	}
	if (plan->value_consumer_offsets != nullptr) {
		for (uint32_t value = 0; value < plan->value_count; ++value) {
			const uint32_t begin = plan->value_consumer_offsets[value];
			const uint32_t end = plan->value_consumer_offsets[value + 1];
			for (uint32_t use = begin; use < end; ++use) {
				const zend_tpde_machine_use &consumer =
					plan->value_consumers[use];
				plan->entry_value_required[value] |=
					machine_plan_use_requires_value(
						plan, consumer, false);
				plan->typed_body_value_required[value] |=
					machine_plan_use_requires_value(
						plan, consumer, true);
			}
		}
	}

	for (uint32_t instruction_index = 0;
			instruction_index < plan->instruction_count;
			++instruction_index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[instruction_index];
		const int32_t value_index =
			machine_cfg_register_cond_branch_value_index(
				plan, instruction);
		if (value_index < 0
				|| static_cast<uint32_t>(value_index)
					>= plan->value_count) {
			continue;
		}
		const uint32_t value = static_cast<uint32_t>(value_index);
		if (machine_cfg_register_cond_branch(
				plan, instruction, false)) {
			plan->entry_value_required[value] = 1;
		}
		if (machine_cfg_register_cond_branch(
				plan, instruction, true)) {
			plan->typed_body_value_required[value] = 1;
		}
	}

	/* A required TPDE value must have a machine representation in the
	 * corresponding function. Canonical-slot-only values remain available to
	 * source helpers, but cannot participate in register liveness. */
	for (uint32_t value = 0; value < plan->value_count; ++value) {
		if (machine_plan_value_has_result_representation(
				plan, plan->values[value].id)) {
			continue;
		}
		plan->entry_value_required[value] = 0;
		plan->typed_body_value_required[value] = 0;
	}

	return true;
}

static bool freeze_machine_cfg(
		zend_tpde_plan *plan,
		bool typed_body,
		zend_tpde_machine_cfg *cfg,
		zend_native_diagnostic *diag) {
	std::vector<uint32_t> instruction_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> guarded_cold_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> guarded_hot_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> guarded_continuation_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> boxed_cond_cold_blocks(
		plan->instruction_count, UINT32_MAX);
	std::vector<uint32_t> boxed_cond_cold_by_predecessor(
		plan->block_count, UINT32_MAX);
	std::vector<uint32_t> final_blocks(plan->block_count);
	uint32_t synthetic_block_count = 0;

	for (uint32_t block = 0; block < plan->block_count; ++block) {
		final_blocks[block] = block;
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(plan, &instruction);
		const int32_t source_block =
			zend_tpde_block_index(plan, record.block_id);
		if (source_block < 0) {
			goto malformed;
		}
		const uint32_t source = static_cast<uint32_t>(source_block);
		instruction_blocks[index] = final_blocks[source];
		const bool typed_component_call =
			(instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_TYPED_COMPONENT_CALL) != 0
			&& plan->typed_component_call_eligible != nullptr
			&& plan->typed_component_call_eligible[index] != 0;
		const bool guarded =
			typed_component_call
				? !typed_body
				: (instruction.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_GUARDED_COLD) != 0;
		if (!guarded) {
			continue;
		}
		if (typed_component_call && !typed_body) {
			guarded_hot_blocks[index] =
				plan->block_count + synthetic_block_count++;
		}
		guarded_cold_blocks[index] =
			plan->block_count + synthetic_block_count++;
		guarded_continuation_blocks[index] =
			plan->block_count + synthetic_block_count++;
		final_blocks[source] = guarded_continuation_blocks[index];
	}
	for (uint32_t index = 0; index < plan->instruction_count; ++index) {
		const zend_tpde_instruction &instruction =
			plan->instructions[index];
		zend_tpde_value_condition condition{};
		const bool boxed_branch =
			(instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH) != 0
			&& !machine_cfg_register_cond_branch(
				plan, instruction, typed_body)
			&& zend_tpde_value_condition_at(instruction, &condition);
		if (!boxed_branch) {
			continue;
		}
		const uint32_t predecessor = instruction_blocks[index];
		if (predecessor == UINT32_MAX
				|| predecessor >= plan->block_count
					+ synthetic_block_count) {
			goto malformed;
		}
		const uint32_t cold_block =
			plan->block_count + synthetic_block_count++;
		boxed_cond_cold_blocks[index] = cold_block;
		const int32_t source_predecessor =
			zend_tpde_block_index(plan, instruction.record.block_id);
		if (source_predecessor < 0) {
			goto malformed;
		}
		boxed_cond_cold_by_predecessor[
			static_cast<uint32_t>(source_predecessor)] = cold_block;
	}

	{
		const uint32_t block_count =
			plan->block_count + synthetic_block_count;
		std::vector<std::vector<uint32_t>> block_successors(block_count);
		std::vector<uint32_t> finally_return_blocks;
		std::vector<uint32_t> finally_targets;
		auto add_edge = [&](uint32_t from, uint32_t to) {
			if (from >= block_count || to >= block_count) {
				return false;
			}
			block_successors[from].push_back(to);
			return true;
		};

		for (uint32_t block = 0; block < plan->block_count; ++block) {
			for (uint32_t edge = plan->block_successor_offsets[block];
					edge < plan->block_successor_offsets[block + 1];
					++edge) {
				if (!add_edge(
						final_blocks[block],
						plan->block_successors[edge])) {
					goto malformed;
				}
			}
		}
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const uint32_t cold = guarded_cold_blocks[index];
			if (cold == UINT32_MAX) {
				continue;
			}
			const uint32_t hot = guarded_hot_blocks[index];
			const uint32_t continuation =
				guarded_continuation_blocks[index];
			if (!add_edge(
					instruction_blocks[index],
					hot == UINT32_MAX ? continuation : hot)
					|| !add_edge(instruction_blocks[index], cold)
					|| (hot != UINT32_MAX
						&& !add_edge(hot, continuation))
					|| !add_edge(cold, continuation)) {
				goto malformed;
			}
		}
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const uint32_t cold = boxed_cond_cold_blocks[index];
			if (cold == UINT32_MAX) {
				continue;
			}
			const int32_t source_predecessor = zend_tpde_block_index(
				plan, plan->instructions[index].record.block_id);
			if (source_predecessor < 0
					|| plan->block_successor_offsets[
							source_predecessor + 1]
						- plan->block_successor_offsets[
							source_predecessor]
						!= 2
					|| !add_edge(instruction_blocks[index], cold)) {
				goto malformed;
			}
			for (uint32_t edge =
						plan->block_successor_offsets[source_predecessor];
					edge < plan->block_successor_offsets[
						source_predecessor + 1];
					++edge) {
				if (!add_edge(cold, plan->block_successors[edge])) {
					goto malformed;
				}
			}
		}
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			const uint32_t record_block = instruction_blocks[index];
			if (record_block == UINT32_MAX) {
				goto malformed;
			}
			if (zend_mir_id_is_valid(instruction.exception_block_id)) {
				const int32_t exception_block = zend_tpde_block_index(
					plan, instruction.exception_block_id);
				if (exception_block < 0
						|| !add_edge(
							guarded_cold_blocks[index] == UINT32_MAX
								? record_block
								: guarded_cold_blocks[index],
							static_cast<uint32_t>(exception_block))) {
					goto malformed;
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_FINALLY_RETURN) {
				finally_return_blocks.push_back(record_block);
			} else if (record.opcode == ZEND_MIR_OPCODE_FINALLY_CALL) {
				const int32_t source_block =
					zend_tpde_block_index(plan, record.block_id);
				if (source_block < 0
						|| plan->block_successor_offsets[source_block + 1]
							- plan->block_successor_offsets[source_block]
							!= 2) {
					goto malformed;
				}
				finally_targets.push_back(
					plan->block_successors[
						plan->block_successor_offsets[source_block] + 1]);
			} else if ((record.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
						|| record.opcode == ZEND_MIR_OPCODE_FINALLY_ENTER)
					&& record.block_id
						!= plan->function.entry_block_id) {
				finally_targets.push_back(record_block);
			}
		}
		for (uint32_t return_block : finally_return_blocks) {
			for (uint32_t target : finally_targets) {
				if (!add_edge(return_block, target)) {
					goto malformed;
				}
			}
		}

		std::vector<uint32_t> successor_offsets(block_count + 1);
		std::vector<uint32_t> successors;
		std::vector<uint32_t> seen(block_count, UINT32_MAX);
		for (uint32_t block = 0; block < block_count; ++block) {
			const bool identical_branch_targets =
				block_successors[block].size() == 2
				&& block_successors[block][0] == block_successors[block][1];
			for (uint32_t target : block_successors[block]) {
				if (!identical_branch_targets && seen[target] == block) {
					continue;
				}
				seen[target] = block;
				successors.push_back(target);
			}
			if (successors.size() > MAX_RECORDS) {
				goto malformed;
			}
			successor_offsets[block + 1] =
				static_cast<uint32_t>(successors.size());
		}
		cfg->block_count = block_count;
		cfg->successor_count =
			static_cast<uint32_t>(successors.size());
		if (!freeze_machine_cfg_array(
					&cfg->successor_offsets, successor_offsets)
				|| !freeze_machine_cfg_array(
					&cfg->successors, successors)
				|| !freeze_machine_cfg_array(
					&cfg->instruction_blocks, instruction_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->guarded_cold_blocks, guarded_cold_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->guarded_hot_blocks, guarded_hot_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->guarded_continuation_blocks,
					guarded_continuation_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->final_blocks, final_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->boxed_cond_cold_blocks,
					boxed_cond_cold_blocks)
				|| !freeze_machine_cfg_array(
					&cfg->boxed_cond_cold_by_predecessor,
					boxed_cond_cold_by_predecessor)) {
			destroy_machine_cfg(cfg);
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to allocate frozen TPDE control-flow plan");
			return false;
		}
	}
	return true;

malformed:
	destroy_machine_cfg(cfg);
	zend_tpde_set_diagnostic(diag,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"unable to freeze TPDE control-flow plan");
	return false;
}

static bool freeze_component_machine_plan(
		zend_tpde_plan *plans,
		const zend_tpde_plan *const *component_plans,
		uint32_t component_count,
		bool register_a64_value_transports,
		zend_native_diagnostic *diag) {
	for (uint32_t component = 0;
			component < component_count; ++component) {
		for (uint32_t value = 0;
				value < plans[component].value_count; ++value) {
			plans[component].values[value].local_abi =
				machine_plan_value_abi(&plans[component], value);
		}
	}
	std::vector<uint8_t> candidates(component_count, 1);
	bool changed;
	do {
		changed = false;
		for (uint32_t index = 0; index < component_count; ++index) {
			if (candidates[index] == 0) {
				continue;
			}
			zend_tpde_local_abi_type return_type{};
			if (!freeze_typed_body_signature(
					&plans[index], component_plans, component_count,
					candidates.data(), register_a64_value_transports,
					&return_type)) {
				candidates[index] = 0;
				changed = true;
			}
		}
	} while (changed);
	for (uint32_t index = 0; index < component_count; ++index) {
		zend_tpde_local_abi_type return_type{};
		plans[index].typed_body_eligible =
			candidates[index] != 0
			&& freeze_typed_body_signature(
				&plans[index], component_plans, component_count,
				candidates.data(), register_a64_value_transports,
				&return_type);
		plans[index].typed_body_return_abi =
			plans[index].typed_body_eligible
				? return_type : zend_tpde_local_abi_type{};
	}
	uint32_t next_typed_body_function = component_count;
	for (uint32_t index = 0; index < component_count; ++index) {
		plans[index].wrapper_function_index = index;
		plans[index].typed_body_function_index =
			plans[index].typed_body_eligible
				? next_typed_body_function++ : UINT32_MAX;
	}
	for (uint32_t index = 0; index < component_count; ++index) {
		if (!freeze_typed_component_calls(
				&plans[index], component_plans, component_count,
				register_a64_value_transports)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to freeze component-local TPDE call plan");
			return false;
		}
		bool typed_body_may_emit_calls = false;
		for (uint32_t instruction = 0;
				instruction < plans[index].instruction_count; ++instruction) {
			typed_body_may_emit_calls = typed_body_may_emit_calls
				|| (plans[index].typed_component_call_eligible != nullptr
					&& plans[index].typed_component_call_eligible[instruction] != 0);
		}
		/*
		 * Typed-body eligibility rejects effectful and helper-backed
		 * instructions.  A frozen component-local typed call is therefore the
		 * only remaining call-producing instruction in that function view.
		 * Zend entries retain the aggregate helper fact and may also contain a
		 * direct typed-body call.  Keep unwind facts separate so future cold
		 * helper functions do not have to widen either hot function.
		 */
		plans[index].typed_body_may_emit_calls =
			plans[index].typed_body_eligible && typed_body_may_emit_calls;
		plans[index].zend_entry_may_emit_calls =
			plans[index].may_emit_calls || typed_body_may_emit_calls;
		plans[index].typed_body_needs_unwind =
			plans[index].typed_body_may_emit_calls;
		plans[index].zend_entry_needs_unwind =
			plans[index].zend_entry_may_emit_calls;
		freeze_machine_register_authority(
			&plans[index], register_a64_value_transports);
		if (!freeze_machine_operand_transports(&plans[index], diag)) {
			return false;
		}
		if (!retain_typed_call_materializations(&plans[index], diag)) {
			return false;
		}
		if (!freeze_machine_required_values(
				&plans[index], register_a64_value_transports, diag)) {
			return false;
		}
		if (!freeze_machine_cfg(
				&plans[index], false,
				&plans[index].entry_machine_cfg, diag)
				|| (plans[index].typed_body_eligible
					&& !freeze_machine_cfg(
						&plans[index], true,
						&plans[index].typed_body_machine_cfg,
						diag))) {
			return false;
		}
	}
	return true;
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
		zend_tpde_packed_iterator_fetch packed_iterator_fetch{};
		zend_tpde_array_iterator_reset array_iterator_reset{};
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
			|| zend_tpde_packed_iterator_fetch_at(
				instruction, &packed_iterator_fetch)
			|| zend_tpde_array_iterator_reset_at(
				instruction, &array_iterator_reset)
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
		const zend_mir_instruction_record record =
			zend_tpde_instruction_record_at(&plan, &instruction);
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& !instruction.user_call_no_call
				&& (instruction.direct_call != nullptr
					|| (instruction.user_call != nullptr
						&& instruction.user_call->do_opcode
							!= ZEND_CALLABLE_CONVERT
						&& instruction.user_call->do_opcode
							!= ZEND_CALLABLE_CONVERT_PARTIAL))) {
			/* Both fixed and runtime-resolved user targets use the universal
			 * direct Zend-frame path. Descriptor storage is an implementation
			 * detail and must not change the semantic site count. */
			metrics.direct_call_sites++;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr) {
			const bool typed_component_call =
				plan.typed_component_call_eligible != nullptr
				&& plan.typed_component_call_eligible[index] != 0;
			const bool effect_closed_inline =
				plan.effect_closed_inline_eligible != nullptr
				&& plan.effect_closed_inline_eligible[index] != 0;
			const bool inline_frame =
				(instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;

			/*
			 * Typed/effect-closed calls and an inline-frame call's successful
			 * steady-state path emit no runtime-helper call. A remaining
			 * descriptor uses the canonical user-call frame entry and finish
			 * sites around the native callee. Guarded stack growth and
			 * exception/interrupt completion are correctness slow paths, not
			 * per-inner-call steady-state sites.
			 *
			 * Direct-user codegen emits neither an unconditional heap allocator
			 * nor a C bailout catcher at the call site.  Consequently the heap
			 * and catcher counters remain zero unless a future plan/codegen
			 * shape adds such a steady-state site explicitly.
			 */
			if (!typed_component_call
					&& !effect_closed_inline
					&& !inline_frame) {
				metrics.inner_call_runtime_helper_calls += 2;
			}
		}
	}
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

uint32_t zend_tpde_block_successor_count(
	const zend_tpde_plan *plan, zend_mir_block_id id) {
	if (plan == nullptr || plan->block_successor_offsets == nullptr) {
		return 0;
	}
	const int32_t block_index = zend_tpde_block_index(plan, id);
	if (block_index < 0
			|| static_cast<uint32_t>(block_index) >= plan->block_count) {
		return 0;
	}
	const uint32_t index = static_cast<uint32_t>(block_index);
	return plan->block_successor_offsets[index + 1]
		- plan->block_successor_offsets[index];
}

bool zend_tpde_block_successor_at(
	const zend_tpde_plan *plan,
	zend_mir_block_id id,
	uint32_t successor_index,
	zend_mir_block_id *out) {
	if (plan == nullptr || out == nullptr
			|| plan->block_successor_offsets == nullptr
			|| plan->block_successors == nullptr) {
		return false;
	}
	const int32_t block_index = zend_tpde_block_index(plan, id);
	if (block_index < 0
			|| static_cast<uint32_t>(block_index) >= plan->block_count) {
		return false;
	}
	const uint32_t index = static_cast<uint32_t>(block_index);
	const uint32_t begin = plan->block_successor_offsets[index];
	const uint32_t count =
		plan->block_successor_offsets[index + 1] - begin;
	if (successor_index >= count) {
		return false;
	}
	const uint32_t target_index =
		plan->block_successors[begin + successor_index];
	if (target_index >= plan->block_count || plan->block_ids == nullptr) {
		return false;
	}
	*out = plan->block_ids[target_index];
	return true;
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
	if (plan == nullptr || instruction == nullptr
			|| instruction->record.id != instruction->id) {
		record.id = ZEND_MIR_ID_INVALID;
	} else {
		record = instruction->record;
	}
	return record;
}

bool zend_tpde_call_argument_at(
	const zend_tpde_plan *plan,
	uint32_t index,
	zend_mir_call_argument_ref *out) {
	return plan != nullptr && out != nullptr
		&& index < plan->call_argument_count
		&& plan->call_arguments != nullptr
		&& ((*out = plan->call_arguments[index]), true);
}

zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index) {
	if (plan == nullptr || instruction == nullptr
			|| index >= instruction->operand_count
			|| instruction->operand_offset > plan->instruction_operand_count
			|| instruction->operand_count
				> plan->instruction_operand_count - instruction->operand_offset
			|| plan->instruction_operands == nullptr) {
		return ZEND_MIR_ID_INVALID;
	}
	return plan->instruction_operands[instruction->operand_offset + index];
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
	if (!freeze_component_machine_plan(
			plans, plan_refs, member_count,
			target == ZEND_NATIVE_TARGET_DARWIN_ARM64, diag)) {
		for (uint32_t index = 0; index < member_count; ++index) {
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
		image->metrics.inner_call_runtime_helper_calls +=
			metrics.inner_call_runtime_helper_calls;
		image->metrics.inner_call_heap_allocations +=
			metrics.inner_call_heap_allocations;
		image->metrics.inner_call_catcher_boundaries +=
			metrics.inner_call_catcher_boundaries;
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
