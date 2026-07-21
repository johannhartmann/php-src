// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_type_info.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t MAX_RECORDS = UINT32_C(1) << 20;
constexpr size_t MAX_NATIVE_IMAGE_BYTES = size_t{1} << 28;

bool checked_count(uint32_t count) {
	return count <= MAX_RECORDS;
}

void destroy_plan(zend_tpde_plan *plan) {
	std::free(plan->blocks);
	std::free(plan->values);
	std::free(plan->instructions);
	std::free(plan->operands);
	std::memset(plan, 0, sizeof(*plan));
}

bool initialize_plan(
	const zend_mir_view *view,
	const zend_native_call_binding *bindings,
	uint32_t binding_count,
	const zend_native_source_effect *effects,
	uint32_t effect_count,
	uint32_t frame_argument_count,
	zend_tpde_plan *plan,
	zend_native_diagnostic *diag) {
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
	const uint32_t constant_count = view->constant_count(view->context);
	const uint32_t frame_slot_count = view->frame_slot_count(view->context);
	if (plan->block_count == 0 || !checked_count(plan->block_count)
			|| !checked_count(plan->value_count)
			|| !checked_count(plan->instruction_count)
			|| !checked_count(constant_count)
			|| !checked_count(frame_slot_count)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"MIR record count is outside the W06 executable bound");
		return false;
	}

	plan->blocks = static_cast<zend_mir_block_record *>(
		std::calloc(plan->block_count, sizeof(*plan->blocks)));
	plan->values = static_cast<zend_tpde_value *>(
		std::calloc(plan->value_count, sizeof(*plan->values)));
	plan->instructions = static_cast<zend_tpde_instruction *>(
		std::calloc(plan->instruction_count, sizeof(*plan->instructions)));
	if (plan->blocks == nullptr || (plan->value_count != 0 && plan->values == nullptr)
			|| (plan->instruction_count != 0 && plan->instructions == nullptr)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate the TPDE adaptor plan");
		return false;
	}

	for (uint32_t i = 0; i < plan->block_count; ++i) {
		if (!view->block_at(view->context, i, &plan->blocks[i])
				|| plan->blocks[i].function_id != plan->function.id) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR block table is inconsistent");
			return false;
		}
	}
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		zend_mir_value_record value;
		if (!view->value_at(view->context, i, &value)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR value table is unreadable");
			return false;
		}
		plan->values[i] = {value.id, value.representation,
			ZEND_MIR_SCALAR_TYPE_NONE, -1, false, 0};
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

	uint64_t operands = 0;
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		zend_mir_instruction_record record;
		if (!view->instruction_at(view->context, i, &record)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR instruction table is unreadable");
			return false;
		}
		uint32_t count = view->instruction_operand_count(view->context, record.id);
		operands += count;
		if (operands > MAX_RECORDS) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"MIR operand count is outside the W06 executable bound");
			return false;
		}
		plan->instructions[i].record = record;
		plan->instructions[i].operand_offset = static_cast<uint32_t>(operands - count);
		plan->instructions[i].operand_count = count;
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			const zend_mir_call_view *calls =
				zend_mir_module_call_view_from_view(view);
			zend_mir_call_site_ref site{};
			bool found_site = false;
			if (calls == nullptr || calls->call_site_count == nullptr
					|| calls->call_site_at == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct call lacks its W05 call view");
				return false;
			}
			for (uint32_t n = 0; n < calls->call_site_count(calls->context); ++n) {
				zend_mir_call_site_ref candidate;
				if (!calls->call_site_at(calls->context, n, &candidate)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
						"direct call-site table is unreadable");
					return false;
				}
				if (candidate.instruction_id == record.id) {
					if (found_site) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
							"direct call has duplicate call sites");
						return false;
					}
					site = candidate;
					found_site = true;
				}
			}
			if (!found_site || site.arguments.count != count) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"direct call instruction and call site disagree");
				return false;
			}
			for (uint32_t n = 0; n < binding_count; ++n) {
				if (bindings[n].target_id == site.target_id) {
					if (plan->instructions[i].entry_cell != nullptr
							|| bindings[n].entry_cell == nullptr) {
						zend_tpde_set_diagnostic(diag,
							ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
							"direct call binding is duplicate or null");
						return false;
					}
					plan->instructions[i].entry_cell = bindings[n].entry_cell;
				}
			}
			if (plan->instructions[i].entry_cell == nullptr) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
					"direct user call has no native entry-cell binding");
				return false;
			}
			plan->may_emit_calls = true;
		}
	}
	for (uint32_t i = 0; i < effect_count; ++i) {
		const zend_native_source_effect &effect = effects[i];
		zend_tpde_instruction *match = nullptr;

		if (effect.kind != ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR
				|| !zend_mir_id_is_valid(effect.source_position_id)
				|| !zend_mir_scalar_type_is_exact(effect.exact_type)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"W07 source effect is invalid");
			return false;
		}
		for (uint32_t n = 0; n < plan->instruction_count; ++n) {
			zend_tpde_instruction &candidate = plan->instructions[n];
			if (candidate.record.source_position_id
					!= effect.source_position_id) {
				continue;
			}
			if (candidate.record.opcode != ZEND_MIR_OPCODE_I1_NOT
					&& candidate.record.opcode != ZEND_MIR_OPCODE_I64_TO_I1
					&& candidate.record.opcode != ZEND_MIR_OPCODE_F64_TO_I1
					&& candidate.record.opcode != ZEND_MIR_OPCODE_SCALAR_DROP) {
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
		if (match == nullptr || match->operand_count != 1
				|| match->source_effect != 0) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"W07 echo must map uniquely to a scalar value proof");
			return false;
		}
		match->source_effect = effect.kind;
		match->source_effect_exact_type = effect.exact_type;
		plan->may_emit_calls = true;
	}
	plan->operand_count = static_cast<uint32_t>(operands);
	plan->operands = static_cast<zend_mir_value_id *>(
		std::calloc(plan->operand_count, sizeof(*plan->operands)));
	if (plan->operand_count != 0 && plan->operands == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate the TPDE operand table");
		return false;
	}
	for (uint32_t i = 0; i < plan->instruction_count; ++i) {
		const zend_tpde_instruction &instruction = plan->instructions[i];
		for (uint32_t n = 0; n < instruction.operand_count; ++n) {
			if (!view->instruction_operand_at(view->context,
					instruction.record.id, n,
					&plan->operands[instruction.operand_offset + n])) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"MIR operand table is unreadable");
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
	for (uint32_t i = 0; i < plan->value_count; ++i) {
		if (plan->values[i].id == id) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

const zend_tpde_instruction *zend_tpde_instruction_at(
	const zend_tpde_plan *plan, uint32_t index) {
	return index < plan->instruction_count ? &plan->instructions[index] : nullptr;
}

zend_mir_value_id zend_tpde_operand_at(
	const zend_tpde_plan *plan,
	const zend_tpde_instruction *instruction,
	uint32_t index) {
	if (index >= instruction->operand_count) {
		return ZEND_MIR_ID_INVALID;
	}
	return plan->operands[instruction->operand_offset + index];
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
	if (diag != nullptr) {
		std::memset(diag, 0, sizeof(*diag));
	}
	if (module == nullptr || out_image == nullptr
			|| (binding_count != 0 && bindings == nullptr)
			|| (effect_count != 0 && effects == nullptr)
			|| !checked_count(binding_count) || !checked_count(effect_count)
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
			module, bindings, binding_count, effects, effect_count,
			frame_argument_count,
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
	/* TPDE liveness and register allocation own temporaries; the reserved ABI
	 * pointer remains present for compatibility but no value-slot array is used. */
	image->slot_count = 0;
	image->argument_count = plan.argument_count;
	zend_result result = target == ZEND_NATIVE_TARGET_DARWIN_ARM64
		? zend_tpde_emit_darwin_arm64(&plan, image, diag)
		: zend_tpde_emit_linux_x64(&plan, image, diag);
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
	return target == ZEND_NATIVE_TARGET_DARWIN_ARM64
		? zend_native_publish_darwin_arm64(image, out_code, diag)
		: target == ZEND_NATIVE_TARGET_LINUX_AMD64
			? zend_native_publish_linux_x64(image, out_code, diag)
			: FAILURE;
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

	zend_op_array op_array{};
	op_array.type = ZEND_USER_FUNCTION;
	op_array.num_args = argument_count;
	op_array.required_num_args = argument_count;
	op_array.last_var = argument_count;
	if (argument_count != 0) {
		op_array.arg_info = static_cast<zend_arg_info *>(
			std::calloc(argument_count, sizeof(zend_arg_info)));
		if (op_array.arg_info == nullptr) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"unable to allocate scalar execution argument metadata");
			return FAILURE;
		}
	}

	zend_execute_data *previous = EG(current_execute_data);
	zend_execute_data *frame = zend_vm_stack_push_call_frame(
		ZEND_CALL_NESTED_FUNCTION, reinterpret_cast<zend_function *>(&op_array),
		argument_count, nullptr);
	zval return_value;
	ZVAL_UNDEF(&return_value);
	zend_init_func_execute_data(frame, &op_array, &return_value);
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
				zend_vm_stack_free_args(frame);
				zend_vm_stack_free_call_frame(frame);
				std::free(op_array.arg_info);
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
					"scalar execution argument kind is invalid");
				return FAILURE;
		}
	}

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
	zend_vm_stack_free_args(frame);
	zend_vm_stack_free_call_frame(frame);
	std::free(op_array.arg_info);
	return status == ZEND_NATIVE_RETURNED ? SUCCESS : FAILURE;
}

extern "C" zend_native_status zend_native_execute_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diag) {
	if (code == nullptr || execute_data == nullptr || !code->executable
			|| execute_data->func == nullptr
			|| ZEND_CALL_NUM_ARGS(execute_data) > code->argument_count
			|| zend_native_frame_prepare(execute_data) == FAILURE) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Zend frame does not match the compiled native entry");
		return ZEND_NATIVE_EXCEPTION;
	}
	zval discarded_return;
	zval *original_return_value = execute_data->return_value;
	if (original_return_value == nullptr) {
		ZVAL_UNDEF(&discarded_return);
		execute_data->return_value = &discarded_return;
	}
	zend_native_status status = ZEND_NATIVE_BAILOUT;
	zend_try {
		status = code->entry(execute_data);
	} zend_catch {
		status = EG(exception) != nullptr
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	} zend_end_try();
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != nullptr) {
		status = ZEND_NATIVE_EXCEPTION;
	}
	if (status == ZEND_NATIVE_RETURNED
			&& (execute_data->func->common.fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
		const zend_arg_info *return_info =
			execute_data->func->common.arg_info - 1;
		uint32_t type_mask = ZEND_TYPE_FULL_MASK(return_info->type);
		zval *return_value = execute_data->return_value;

		if ((type_mask & MAY_BE_NEVER) != 0) {
			zend_verify_never_error(execute_data->func);
			status = ZEND_NATIVE_EXCEPTION;
		} else if ((type_mask & MAY_BE_VOID) == 0
				&& (Z_ISUNDEF_P(return_value)
					|| !zend_check_type_ex(
						&return_info->type, return_value, true, false))) {
			zend_verify_return_error(
				execute_data->func,
				Z_ISUNDEF_P(return_value) ? nullptr : return_value);
			status = ZEND_NATIVE_EXCEPTION;
		}
	}
	if (original_return_value == nullptr) {
		if (!Z_ISUNDEF(discarded_return)) {
			zval_ptr_dtor(&discarded_return);
		}
		execute_data->return_value = nullptr;
	}
	return status;
}

extern "C" void zend_native_image_destroy(zend_native_image *image) {
	if (image != nullptr) {
		if (image->destroy_target_state != nullptr) {
			image->destroy_target_state(image->target_state);
		}
		std::free(image->text);
		std::free(image);
	}
}

extern "C" void zend_native_code_destroy(zend_native_code *code) {
	if (code == nullptr) {
		return;
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
