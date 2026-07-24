// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"
#include "Zend/Native/MIR/zend_mir_call.h"
#include "Zend/Native/MIR/zend_mir_values.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_execute.h"

#include <cstddef>
#include <cstdint>

struct _zend_native_direct_call_descriptor;
struct _zend_native_direct_internal_call_descriptor;

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

struct zend_tpde_value {
	zend_mir_value_id id;
	zend_mir_representation representation;
	zend_mir_scalar_type_mask exact_type;
	zend_mir_storage_id canonical_storage_id;
	int32_t argument_index;
	bool constant;
	uint64_t constant_bits;
};

struct zend_tpde_id_index_entry {
	uint32_t id;
	uint32_t index;
};

struct zend_tpde_instruction {
	zend_mir_instruction_id id;
	uint32_t view_index;
	uint32_t operand_count;
	zend_native_entry_cell *entry_cell;
	zend_native_internal_call_cell *internal_call_cell;
	zend_mir_call_site_ref call_site;
	zend_mir_block_id exception_block_id;
	uint32_t call_argument_offset;
	uint32_t call_argument_count;
	_zend_native_direct_call_descriptor *direct_call;
	_zend_native_direct_internal_call_descriptor *direct_internal_call;
	zend_native_source_effect_kind source_effect;
	zend_mir_scalar_type_mask source_effect_exact_type;
	zend_mir_executable_value_ref value_operation;
	bool has_value_operation;
	uint32_t source_opline_index;
};

struct zend_tpde_integer_array_read {
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

struct zend_tpde_integer_array_isset {
	uint32_t container_offset;
	uint32_t key_offset;
	uint32_t result_offset;
};

struct zend_tpde_string_length {
	uint32_t operand_offset;
	uint32_t result_offset;
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

/*
 * Keep the semantic fast-path selection target-neutral.  Target backends only
 * encode the guards and loads; they do not independently decide which MIR
 * shape is safe to execute without the generic dimension primitive.
 */
static inline bool zend_tpde_integer_array_read_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_integer_array_read *out)
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

static inline bool zend_tpde_integer_array_isset_at(
	const zend_tpde_instruction &instruction,
	zend_tpde_integer_array_isset *out)
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
			|| operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
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

struct zend_tpde_plan {
	const zend_mir_view *view;
	const zend_mir_call_view *calls;
	const zend_native_runtime_api *runtime;
	zend_mir_function_record function;
	zend_mir_block_id *block_ids;
	uint32_t block_count;
	zend_tpde_id_index_entry *block_index;
	uint32_t block_index_capacity;
	zend_tpde_value *values;
	uint32_t value_count;
	zend_tpde_id_index_entry *value_index;
	uint32_t value_index_capacity;
	zend_tpde_instruction *instructions;
	uint32_t instruction_count;
	zend_tpde_id_index_entry *instruction_index;
	uint32_t instruction_index_capacity;
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
	uint32_t argument_count;
	uint32_t value_model_flags;
	uint64_t required_runtime_capabilities;
	uint64_t required_runtime_helpers[ZEND_NATIVE_RUNTIME_HELPER_WORD_COUNT];
	bool may_emit_calls;
};

enum zend_native_image_symbol_kind : uint32_t {
	ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER = 1,
	ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL = 2,
	ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL = 3,
	ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_API = 4,
	ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR = 5,
	ZEND_NATIVE_IMAGE_SYMBOL_SOURCE = 6,
	ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR = 7,
};

struct zend_native_image_symbol {
	uint32_t kind;
	uint32_t id;
	uint32_t abi_version;
	uint32_t effects;
	char name[64];
};

struct zend_native_image_symbol_binding {
	uint32_t symbol_index;
	const void *address;
};

struct zend_native_image {
	zend_native_target target;
	uint32_t abi_version;
	uint32_t runtime_abi_version;
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
	void *target_state;
	void (*destroy_target_state)(void *);
	_zend_native_direct_call_descriptor **direct_calls;
	uint32_t direct_call_count;
	_zend_native_direct_internal_call_descriptor **direct_internal_calls;
	uint32_t direct_internal_call_count;
};

struct zend_native_code {
	zend_native_target target;
	void *mapping;
	size_t mapping_size;
	zend_native_frame_entry_t entry;
	uint32_t slot_count;
	uint32_t argument_count;
	bool writable;
	bool executable;
	bool unwind_registered;
	void *target_state;
	void (*destroy_target_state)(void *);
	_zend_native_direct_call_descriptor **direct_calls;
	uint32_t direct_call_count;
	_zend_native_direct_internal_call_descriptor **direct_internal_calls;
	uint32_t direct_internal_call_count;
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
	uint32_t id);
bool zend_tpde_image_resolve_symbol(
	const zend_native_image *image,
	const char *name,
	const void **address);

zend_result zend_tpde_emit_darwin_arm64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag);
zend_result zend_tpde_emit_linux_x64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag);
zend_result zend_tpde_map_linux_x64(
	const zend_native_image *image,
	zend_native_code *code,
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
