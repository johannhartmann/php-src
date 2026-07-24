// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_type_info.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t MAX_RECORDS = UINT32_C(1) << 20;
constexpr size_t MAX_NATIVE_IMAGE_BYTES = size_t{1} << 28;
constexpr uint32_t NATIVE_IMAGE_ABI_VERSION = 1;
std::atomic_uint32_t live_unwind_registrations{0};

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
	uint32_t abi_version,
	uint32_t effects,
	const void *address = nullptr) {
	if (image == nullptr || !zend_mir_id_is_valid(id)) {
		return false;
	}
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		const zend_native_image_symbol &symbol = image->symbols[index];
		if (symbol.kind == kind && symbol.id == id) {
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
	symbol.abi_version = abi_version;
	symbol.effects = effects;
	const int written = std::snprintf(symbol.name, sizeof(symbol.name),
		"__znmir_%u_%u", static_cast<uint32_t>(kind), id);
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
					instruction.id, NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.direct_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image direct-call symbol");
			return false;
		}
		if (instruction.direct_internal_call != nullptr
				&& !image_add_symbol(image,
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					instruction.id, NATIVE_IMAGE_ABI_VERSION, 0,
					instruction.direct_internal_call)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to create the native image direct internal-call symbol");
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
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			value_id = zend_mir_value_from_original_ssa(
				operand.ssa_variable_id);
			return zend_mir_id_is_valid(value_id);
		default:
			return false;
	}
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
		default:
			return ZEND_NATIVE_HELPER_COUNT;
	}
}

void destroy_plan(zend_tpde_plan *plan) {
	for (uint32_t index = 0; index < plan->direct_call_count; ++index) {
		std::free(plan->direct_calls[index]);
	}
	for (uint32_t index = 0;
			index < plan->direct_internal_call_count; ++index) {
		std::free(plan->direct_internal_calls[index]);
	}
	std::free(plan->block_ids);
	std::free(plan->block_index);
	std::free(plan->values);
	std::free(plan->value_index);
	std::free(plan->instructions);
	std::free(plan->instruction_index);
	std::free(plan->call_site_instruction_index);
	std::free(plan->call_target_index);
	std::free(plan->user_binding_index);
	std::free(plan->internal_binding_index);
	std::free(plan->direct_calls);
	std::free(plan->direct_internal_calls);
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
	if (plan->block_count == 0 || !checked_count(plan->block_count)
			|| !checked_count(plan->value_count)
			|| !checked_count(plan->instruction_count)
			|| !checked_count(plan->call_site_count)
			|| !checked_count(plan->call_target_count)
			|| !checked_count(plan->call_argument_count)
			|| !checked_count(constant_count)
			|| !checked_count(frame_slot_count)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"MIR record count is outside the W06 executable bound");
		return false;
	}

	plan->block_ids = static_cast<zend_mir_block_id *>(
		std::calloc(plan->block_count, sizeof(*plan->block_ids)));
	plan->block_index = allocate_id_index(
		plan->block_count, &plan->block_index_capacity);
	plan->values = static_cast<zend_tpde_value *>(
		std::calloc(plan->value_count, sizeof(*plan->values)));
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
	if (plan->block_ids == nullptr || plan->block_index == nullptr
			|| (plan->value_count != 0
				&& (plan->values == nullptr || plan->value_index == nullptr))
			|| (plan->instruction_count != 0
				&& (plan->instructions == nullptr
					|| plan->instruction_index == nullptr))
			|| (plan->call_site_count != 0
				&& (plan->call_site_instruction_index == nullptr
					|| plan->direct_calls == nullptr
					|| plan->direct_internal_calls == nullptr))
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
		plan->instructions[i].exception_block_id = ZEND_MIR_ID_INVALID;
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
		plan->values[i] = {value.id, value.representation,
			ZEND_MIR_SCALAR_TYPE_NONE, ZEND_MIR_ID_INVALID, -1, false, 0};
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

	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_mir_instruction_record record;
		if (!view->instruction_at(view->context, i, &record)
				|| record.id != plan->instructions[i].id) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction table is unreadable");
			return false;
		}
		const uint32_t count = plan->instructions[i].operand_count;
		if (zend_mir_opcode_is_executable_value(record.opcode)) {
			const bool semantic_echo =
				record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR;
			const zend_native_runtime_helper_id helper = semantic_echo
				? ZEND_NATIVE_HELPER_COUNT
				: executable_value_helper(record.opcode);
			if ((semantic_echo ? count != 1 : count != 0)
					|| !zend_mir_id_is_valid(record.source_position_id)
					|| (!semantic_echo && helper == ZEND_NATIVE_HELPER_COUNT)
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
				require_runtime_helper(plan,
					plan->values[value_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_F64
						? ZEND_NATIVE_HELPER_ECHO_DOUBLE
						: ZEND_NATIVE_HELPER_ECHO_INTEGER);
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
				const zend_mir_executable_value_ref &operation =
					plan->instructions[i].value_operation;
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
			const bool explicit_object_operands =
				(record.opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
					&& record.opcode <= ZEND_MIR_OPCODE_OBJECT_BIND_STATIC)
				|| (record.opcode
						>= ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME
					&& record.opcode
						<= ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED);
			if (!explicit_object_operands
					&& record.opcode
						!= ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
					&& (record.opcode < ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
						|| record.opcode
							> ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL)) {
				/*
				 * Kept temporarily for uncommon object slow paths while their
				 * helper ABI is migrated.
				 */
				plan->instructions[i].source_opline_index =
					record.source_position_id;
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
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL);
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
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH);
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
			const zend_mir_executable_value_ref &operation =
				plan->instructions[i].value_operation;
			const bool conditional_opcode =
				operation.source_opcode == ZEND_JMPZ
				|| operation.source_opcode == ZEND_JMPNZ
				|| operation.source_opcode == ZEND_JMPZ_EX
				|| operation.source_opcode == ZEND_JMPNZ_EX
				|| operation.source_opcode == ZEND_JMP_SET
				|| operation.source_opcode == ZEND_COALESCE
				|| operation.source_opcode == ZEND_JMP_NULL;
			if (count != 0 || !zend_mir_id_is_valid(record.source_position_id)
					|| !plan->instructions[i].has_value_operation
					|| operation.id != record.id
					|| operation.opcode != record.opcode
					|| operation.source_position_id
						!= record.source_position_id
					|| operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| !conditional_opcode) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"source value branch lacks exact source semantics");
				return false;
			}
			plan->required_runtime_capabilities |=
				ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
			require_runtime_helper(
				plan, ZEND_NATIVE_HELPER_VALUE_COND_BRANCH);
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
				plan->required_runtime_capabilities |=
					ZEND_NATIVE_RUNTIME_CAP_ZVAL_SLOT;
				require_runtime_helper(plan, ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL);
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
			plan->instructions[i].call_do_opcode =
				source_op_array->opcodes[site.source_do_opline_index].opcode;
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
				bool direct_descriptor =
					(target.kind == ZEND_MIR_CALL_TARGET_DIRECT_USER
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
						(!zend_mir_id_is_valid(record.result_id)
							&& descriptor->result_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_UNUSED)
						|| (zend_mir_id_is_valid(record.result_id)
							&& (descriptor->result_operand.kind
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
					}
					plan->instructions[i].direct_call = descriptor;
					plan->direct_calls[plan->direct_call_count++] = descriptor;
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_DIRECT_USER_CALL_ENTER);
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE);
				} else {
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_USER_CALL_BEGIN);
					require_runtime_helper(
						plan, ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE);
				}
				if (source_arguments) {
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
					if (!direct_descriptor) {
						require_runtime_helper(
							plan,
							ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT);
					}
				}
				for (uint32_t n = 0; n < count; ++n) {
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
			if (zend_mir_id_is_valid(record.result_id)
					&& plan->instructions[i].direct_call == nullptr
					&& plan->instructions[i].direct_internal_call == nullptr) {
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
					: ((effect.kind != ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR
							&& effect.kind
								!= ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE)
						|| !zend_mir_scalar_type_is_exact(effect.exact_type)))) {
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
							!= ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL) {
					continue;
				}
			} else if (candidate_record.opcode
						!= ZEND_MIR_OPCODE_ECHO_SCALAR
					&& candidate_record.opcode != ZEND_MIR_OPCODE_I1_NOT
					&& candidate_record.opcode != ZEND_MIR_OPCODE_I64_TO_I1
					&& candidate_record.opcode != ZEND_MIR_OPCODE_F64_TO_I1
					&& candidate_record.opcode != ZEND_MIR_OPCODE_SCALAR_DROP) {
				continue;
			}
			if (match != nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"W07 source effect maps to multiple MIR instructions");
				return false;
			}
			match = &candidate;
		}
		if (match == nullptr
				|| (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE
					? zend_mir_id_is_valid(match->exception_block_id)
					: (match->operand_count != 1
						|| match->source_effect != 0))) {
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
		match->source_effect = effect.kind;
		match->source_effect_exact_type = effect.exact_type;
		if (effect.kind == ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE) {
			require_runtime_helper(plan, ZEND_NATIVE_HELPER_ABI_CONFORMANCE);
		} else {
			require_runtime_helper(plan,
				effect.exact_type == ZEND_MIR_SCALAR_TYPE_F64
					? ZEND_NATIVE_HELPER_ECHO_DOUBLE
					: ZEND_NATIVE_HELPER_ECHO_INTEGER);
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

	if (frame_argument_count != UINT32_MAX) {
		plan->argument_count = frame_argument_count;
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
		if (frame_argument
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
	uint32_t id) {
	if (image == nullptr) {
		return nullptr;
	}
	for (uint32_t index = 0; index < image->symbol_count; ++index) {
		const zend_native_image_symbol &symbol = image->symbols[index];
		if (symbol.kind == kind && symbol.id == id) {
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
						!= ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR)) {
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
		effect_count, frame_argument_count, nullptr, out_image, diag);
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
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	return zend_tpde_compile_module_w08_with_runtime(
		target, module, user_bindings, user_binding_count,
		internal_bindings, internal_binding_count, effects, effect_count,
		frame_argument_count, source_op_array, zend_native_runtime_get(),
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
	const zend_native_runtime_api *runtime,
	zend_native_image **out_image,
	zend_native_diagnostic *diag) {
	if (diag != nullptr) {
		std::memset(diag, 0, sizeof(*diag));
	}
	if (module == nullptr || out_image == nullptr
			|| runtime == nullptr
			|| (user_binding_count != 0 && user_bindings == nullptr)
			|| (internal_binding_count != 0 && internal_bindings == nullptr)
			|| (effect_count != 0 && effects == nullptr)
			|| !checked_count(user_binding_count)
			|| !checked_count(internal_binding_count)
			|| !checked_count(effect_count)
			|| (frame_argument_count != UINT32_MAX
				&& !checked_count(frame_argument_count))) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"module and out_image are required");
		return FAILURE;
	}
	*out_image = nullptr;
	if (target != ZEND_NATIVE_TARGET_DARWIN_ARM64
			&& target != ZEND_NATIVE_TARGET_LINUX_AMD64) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_TARGET,
			"only darwin-arm64-dev and linux-amd64-prod are supported");
		return FAILURE;
	}

	zend_tpde_plan plan{};
	if (!initialize_plan(
			module, runtime, user_bindings, user_binding_count,
			internal_bindings, internal_binding_count, effects, effect_count,
			frame_argument_count, source_op_array,
			&plan, diag)) {
		destroy_plan(&plan);
		return FAILURE;
	}
	zend_native_image *image = static_cast<zend_native_image *>(
		std::calloc(1, sizeof(*image)));
	if (image == nullptr) {
		destroy_plan(&plan);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate a native image");
		return FAILURE;
	}
	image->target = target;
	image->abi_version = NATIVE_IMAGE_ABI_VERSION;
	image->runtime_abi_version = plan.runtime->abi_version;
	/* TPDE liveness and register allocation own temporaries; the reserved ABI
	 * pointer remains present for compatibility but no value-slot array is used. */
	image->slot_count = 0;
	image->argument_count = plan.argument_count;
	zend_result result = prepare_image_symbols(&plan, image, diag)
		? target == ZEND_NATIVE_TARGET_DARWIN_ARM64
			? zend_tpde_emit_darwin_arm64(&plan, image, diag)
			: zend_tpde_emit_linux_x64(&plan, image, diag)
		: FAILURE;
	if (result == SUCCESS) {
		image->direct_calls = plan.direct_calls;
		image->direct_call_count = plan.direct_call_count;
		plan.direct_calls = nullptr;
		plan.direct_call_count = 0;
		image->direct_internal_calls = plan.direct_internal_calls;
		image->direct_internal_call_count = plan.direct_internal_call_count;
		plan.direct_internal_calls = nullptr;
		plan.direct_internal_call_count = 0;
	}
	destroy_plan(&plan);
	if (result == FAILURE) {
		zend_native_image_destroy(image);
		return FAILURE;
	}
	*out_image = image;
	return SUCCESS;
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
	op_array.last_var = argument_count;
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
	for (uint32_t index = 0; index < code->direct_call_count; ++index) {
		std::free(code->direct_calls[index]);
	}
	std::free(code->direct_calls);
	for (uint32_t index = 0;
			index < code->direct_internal_call_count; ++index) {
		std::free(code->direct_internal_calls[index]);
	}
	std::free(code->direct_internal_calls);
	if (code->unwind_registered) {
		uint32_t previous = live_unwind_registrations.fetch_sub(
			1, std::memory_order_relaxed);
		ZEND_ASSERT(previous != 0);
		code->unwind_registered = false;
	}
	if (code->target == ZEND_NATIVE_TARGET_DARWIN_ARM64) {
		zend_native_unmap_darwin_arm64(code);
	} else if (code->target == ZEND_NATIVE_TARGET_LINUX_AMD64) {
		zend_native_unmap_linux_x64(code);
	}
	std::free(code);
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
