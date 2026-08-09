// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/zend_type_info.h"

#include <tpde/IRAdaptor.hpp>
#include <tpde/ValLocalIdx.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zend::native::tpde {

class IRValueRef {
	uint32_t value_;

public:
	explicit constexpr IRValueRef(uint32_t value) : value_(value) {}
	explicit constexpr operator uint32_t() const { return value_; }
	constexpr bool operator==(const IRValueRef &) const = default;
};

class IRInstRef {
	uint32_t value_;

public:
	explicit constexpr IRInstRef(uint32_t value) : value_(value) {}
	explicit constexpr operator uint32_t() const { return value_; }
	constexpr bool operator==(const IRInstRef &) const = default;
};

enum class IRBlockRef : uint32_t {};
enum class IRFuncRef : uint32_t {};

class ZendIRAdaptor {
public:
	using IRValueRef = zend::native::tpde::IRValueRef;
	using IRInstRef = zend::native::tpde::IRInstRef;
	using IRBlockRef = zend::native::tpde::IRBlockRef;
	using IRFuncRef = zend::native::tpde::IRFuncRef;

	static constexpr IRValueRef INVALID_VALUE_REF{UINT32_MAX};
	static constexpr IRBlockRef INVALID_BLOCK_REF =
		static_cast<IRBlockRef>(UINT32_MAX);
	static constexpr IRFuncRef INVALID_FUNC_REF =
		static_cast<IRFuncRef>(UINT32_MAX);
	static constexpr bool TPDE_PROVIDES_HIGHEST_VAL_IDX = true;
	static constexpr bool TPDE_LIVENESS_VISIT_ARGS = true;

	static constexpr uint32_t EXECUTE_DATA_VALUE = 0;
	static constexpr uint32_t EXECUTION_CONTEXT_ARGUMENT = 1;
	static constexpr uint32_t FRAME_VALUE = 2;
	static constexpr uint32_t MIR_VALUE_BASE = 4;

	enum class FunctionMode : uint8_t {
		ZendEntry,
		TypedBody,
	};

	enum class InstKind : uint8_t {
		LoadFrame,
		UserOpcodeLanding,
		UserOpcodeGateway,
		UserOpcodeDispatch,
		UserOpcodeCallFragment,
		UserCallInit,
		UserCallSend,
		UserCallCheck,
		UserCallExpand,
		UserCallDo,
		GeneratorGateway,
		GeneratorResume,
		ZvalTypeLoad,
		ZvalPayloadLoad,
		ZvalBoxedStore,
		ZvalCopy,
		ZvalMove,
		ZvalStore,
		ZvalReleaseFast,
		ZvalGuardArguments,
		ZvalGuardType,
		BoxScalar,
		UnboxScalar,
		UnboxPointer,
		UnboxReferenceScalar,
		ZvalReferenceResolve,
		SlowPathCall,
		TypedCallGuard,
		GuardedFast,
		GuardedCold,
		BoxedCondGuard,
		BoxedCondCold,
		BoxedCondColdBranch,
		StringLengthValue,
		ScalarSelect,
		MIR,
	};

	struct InstNode {
		InstKind kind;
		uint32_t mir_instruction_index;
		uint32_t argument_index;
		IRValueRef result;
		std::span<const IRValueRef> operands;
		uint32_t operand_offset;
		uint32_t operand_count;
		bool has_result;
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
		zend_mir_scalar_type_mask exact_type =
			ZEND_MIR_SCALAR_TYPE_NONE;
		bool synthetic = false;
		zend_mir_instruction_record synthetic_record{};
		bool inlined_user_body = false;
		uint32_t inlined_operand_index = UINT32_MAX;
		uint32_t inlined_checked_source_opcode = UINT32_MAX;
		uint32_t materialization_operand_index = UINT32_MAX;
		uint32_t materialization_count = 0;
		std::span<const IRValueRef> liveness_operands{};
		uint32_t control_block = UINT32_MAX;
		uint32_t continuation_block = UINT32_MAX;
		uint32_t machine_reference_operand_index = UINT32_MAX;
		uint32_t assign_op_right_operand_index = UINT32_MAX;
		uint32_t assign_op_left_operand_index = UINT32_MAX;
		uint32_t packed_append_value_operand_index = UINT32_MAX;
		uint32_t property_write_value_operand_index = UINT32_MAX;
		bool mutation_result = false;
		bool direct_internal_argument_transport = false;
		uint32_t source_position = UINT32_MAX;
		uint32_t generator_resume_value_offset = 0;
		uint32_t generator_resume_value_count = 0;
		uint32_t semantic_operand_count = UINT32_MAX;
		uint32_t boxed_op1_boundary_operand_index = UINT32_MAX;
		uint32_t boxed_op2_boundary_operand_index = UINT32_MAX;
		uint32_t inlined_checked_step_offset = UINT32_MAX;
		uint32_t inlined_checked_step_count = 0;
		uint32_t inlined_checked_operand_count = 0;
	};

	struct DerivedValue {
		zend_mir_representation representation;
		zend_mir_scalar_type_mask exact_type;
		zend_mir_storage_id storage_id;
		zend_tpde_machine_value_kind machine_kind;
		zend_mir_ownership_state ownership;
		zend_mir_refcount_state refcount_state;
		uint32_t machine_reference_index = UINT32_MAX;
		bool constant = false;
		uint64_t constant_bits = 0;
		bool known_string_literal = false;
		uint8_t known_string_first_byte = 0;
		uint64_t known_string_length = 0;
	};

	struct InlinedCheckedStep {
		uint32_t source_opcode = UINT32_MAX;
		bool accumulator_is_left = true;
	};

	struct InlinedBody {
		bool valid = false;
		IRValueRef value = INVALID_VALUE_REF;
		IRValueRef checked_left = INVALID_VALUE_REF;
		IRValueRef checked_right = INVALID_VALUE_REF;
		uint32_t checked_source_opcode = UINT32_MAX;
		std::vector<IRValueRef> checked_operands{};
		std::vector<InlinedCheckedStep> checked_steps{};

		bool checked() const {
			return !checked_steps.empty()
				|| checked_source_opcode != UINT32_MAX;
		}

		uint32_t operand_count() const {
			return checked()
				? static_cast<uint32_t>(checked_operands.empty()
					? 2 : checked_operands.size())
				: 1;
		}
	};

	struct ArgumentGuard {
		uint32_t argument_index;
		zend_mir_storage_id storage_id;
		zend_mir_scalar_type_mask exact_type;
		zend_tpde_machine_value_kind machine_kind;
	};

	struct TypedBodyAbiType {
		zend_mir_representation representation =
			ZEND_MIR_REPRESENTATION_VOID;
		zend_mir_scalar_type_mask exact_type =
			ZEND_MIR_SCALAR_TYPE_NONE;
		zend_tpde_machine_value_kind machine_kind =
			ZEND_TPDE_MACHINE_VALUE_I64;
		bool valid = false;
		zend_tpde_local_abi_transfer transfer =
			ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE;

		bool operator==(const TypedBodyAbiType &other) const {
			return valid == other.valid
				&& representation == other.representation
				&& exact_type == other.exact_type
				&& machine_kind == other.machine_kind
				&& transfer == other.transfer;
		}

		bool same_shape(const TypedBodyAbiType &other) const {
			return valid && other.valid
				&& representation == other.representation
				&& exact_type == other.exact_type
				&& machine_kind == other.machine_kind;
		}

		bool can_supply_argument(const TypedBodyAbiType &callee) const {
			if (!same_shape(callee)) {
				return false;
			}
			switch (callee.transfer) {
				case ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE:
					return transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE;
				case ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED:
					return transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED
						|| transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED
						|| transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL;
				case ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED:
				case ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED:
					return transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED;
				case ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL:
					return transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL;
			}
			return false;
		}
	};

	static bool typed_body_call_argument_can_supply(
			const zend_tpde_plan *plan,
			const zend_mir_call_argument_ref &argument,
			const TypedBodyAbiType &caller,
			const TypedBodyAbiType &callee) {
		if (!caller.can_supply_argument(callee)) {
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

	static zend_mir_ownership_state local_abi_ownership(
			zend_tpde_local_abi_transfer transfer,
			zend_mir_ownership_state fallback) {
		switch (transfer) {
			case ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED:
			case ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL:
				return ZEND_MIR_OWNERSHIP_STATE_BORROWED;
			case ZEND_TPDE_LOCAL_ABI_TRANSFER_OWNED:
				return ZEND_MIR_OWNERSHIP_STATE_OWNED;
			case ZEND_TPDE_LOCAL_ABI_TRANSFER_MOVED:
				return ZEND_MIR_OWNERSHIP_STATE_MOVED;
			case ZEND_TPDE_LOCAL_ABI_TRANSFER_NONE:
				return fallback;
		}
		return ZEND_MIR_OWNERSHIP_STATE_INVALID;
	}

	static zend_mir_refcount_state local_abi_refcount(
			zend_tpde_local_abi_transfer transfer,
			zend_mir_refcount_state fallback) {
		return transfer == ZEND_TPDE_LOCAL_ABI_TRANSFER_IMMORTAL
			? ZEND_MIR_REFCOUNT_IMMORTAL : fallback;
	}

	static bool machine_pointer_kind(zend_tpde_machine_value_kind kind) {
		return kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
	}

	struct PhiInput {
		IRValueRef value;
		IRBlockRef block;
	};

	struct Slice {
		uint32_t offset = 0;
		uint32_t count = 0;
	};

	template <typename T>
	struct BlockItem {
		uint32_t block;
		T value;
	};

	class PhiRef {
		const ZendIRAdaptor *adaptor_;
		IRValueRef value_;

		std::span<const PhiInput> inputs() const {
			const Slice &slice =
				adaptor_->phi_input_slices_[static_cast<uint32_t>(value_)];
			return std::span<const PhiInput>{adaptor_->phi_inputs_}.subspan(
				slice.offset, slice.count);
		}

	public:
		PhiRef(const ZendIRAdaptor *adaptor, IRValueRef value)
			: adaptor_(adaptor), value_(value) {}

		uint32_t incoming_count() const {
			return static_cast<uint32_t>(inputs().size());
		}
		IRValueRef incoming_val_for_slot(uint32_t slot) const {
			return inputs()[slot].value;
		}
		IRBlockRef incoming_block_for_slot(uint32_t slot) const {
			return inputs()[slot].block;
		}
		IRValueRef incoming_val_for_block(IRBlockRef block) const {
			for (const PhiInput &input : inputs()) {
				if (input.block == block) {
					return input.value;
				}
			}
			return INVALID_VALUE_REF;
		}
	};

private:
	const zend_tpde_plan *plan_;
	std::span<const zend_tpde_plan *const> component_plans_;
	FunctionMode function_mode_;
	std::array<IRFuncRef, 1> functions_{IRFuncRef{0}};
	std::vector<IRValueRef> arguments_;
	std::array<IRValueRef, 0> no_values_;
	std::vector<IRBlockRef> blocks_;
	std::vector<Slice> successor_slices_;
	std::vector<IRBlockRef> successors_;
	std::vector<Slice> instruction_slices_;
	std::vector<IRInstRef> instructions_;
	std::vector<Slice> phi_slices_;
	std::vector<IRValueRef> phis_;
	std::vector<Slice> phi_input_slices_;
	std::vector<PhiInput> phi_inputs_;
	std::vector<InstNode> nodes_;
	std::vector<InlinedCheckedStep> inlined_checked_steps_;
	std::vector<uint8_t> fused_instructions_;
	std::vector<IRValueRef> operands_;
	std::vector<IRValueRef> generator_resume_values_;
	std::vector<uint8_t> phi_values_;
	std::vector<IRValueRef> typed_body_value_overrides_;
	std::vector<IRValueRef> typed_body_source_ssa_overrides_;
	std::vector<IRValueRef> typed_body_instruction_results_;
	std::vector<IRValueRef> component_value_overrides_;
	std::vector<IRValueRef> component_source_ssa_overrides_;
	std::vector<IRValueRef> component_instruction_results_;
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	std::vector<DerivedValue> derived_values_;
	std::vector<IRValueRef> machine_reference_values_;
	std::vector<ArgumentGuard> argument_guards_;
	std::vector<uint32_t> user_opcode_next_landings_;
	std::vector<uint32_t> user_opcode_dispatch_to_sources_;
	std::vector<uint8_t> user_opcode_result_reload_sources_;
	zend_tpde_instruction synthetic_instruction_{};
	bool valid_ = true;

	static IRValueRef plan_source_operand_value_ref(
			const zend_tpde_plan *plan,
			const zend_mir_source_operand_ref &operand) {
		zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
		if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			value_id = zend_mir_value_from_synthetic(operand.index);
		} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
				&& operand.ssa_variable_id != ZEND_MIR_ID_INVALID) {
			value_id = zend_mir_value_from_original_ssa(
				operand.ssa_variable_id);
		}
		const int32_t index = zend_mir_id_is_valid(value_id)
			? zend_tpde_value_index(plan, value_id) : -1;
		return index < 0 ? INVALID_VALUE_REF
			: IRValueRef{MIR_VALUE_BASE + static_cast<uint32_t>(index)};
	}

	const std::vector<IRValueRef> &active_value_overrides() const {
		return function_mode_ == FunctionMode::TypedBody
			? typed_body_value_overrides_ : component_value_overrides_;
	}

	std::vector<IRValueRef> &active_value_overrides() {
		return function_mode_ == FunctionMode::TypedBody
			? typed_body_value_overrides_ : component_value_overrides_;
	}

	const std::vector<IRValueRef> &active_source_ssa_overrides() const {
		return function_mode_ == FunctionMode::TypedBody
			? typed_body_source_ssa_overrides_
			: component_source_ssa_overrides_;
	}

	std::vector<IRValueRef> &active_source_ssa_overrides() {
		return function_mode_ == FunctionMode::TypedBody
			? typed_body_source_ssa_overrides_
			: component_source_ssa_overrides_;
	}

	const std::vector<IRValueRef> &active_instruction_results() const {
		return function_mode_ == FunctionMode::TypedBody
			? typed_body_instruction_results_
			: component_instruction_results_;
	}

	std::vector<IRValueRef> &active_instruction_results() {
		return function_mode_ == FunctionMode::TypedBody
			? typed_body_instruction_results_
			: component_instruction_results_;
	}

	bool frozen_typed_component_call(uint32_t instruction_index) const {
		return plan_ != nullptr
			&& plan_->typed_component_call_eligible != nullptr
			&& instruction_index < plan_->instruction_count
			&& plan_->typed_component_call_eligible[instruction_index] != 0;
	}

	bool frozen_effect_closed_inline(uint32_t instruction_index) const {
		return plan_ != nullptr
			&& plan_->effect_closed_inline_eligible != nullptr
			&& instruction_index < plan_->instruction_count
			&& plan_->effect_closed_inline_eligible[instruction_index] != 0;
	}

	IRValueRef source_binding_value_ref(
			const zend_tpde_source_value_binding &binding) const {
		const auto &source_overrides = active_source_ssa_overrides();
		const auto &instruction_results = active_instruction_results();
		const bool has_definition =
			binding.definition_instruction_index >= 0
				&& static_cast<uint32_t>(
					binding.definition_instruction_index)
					< instruction_results.size();
		if (has_definition) {
			const IRValueRef result = instruction_results[
				static_cast<uint32_t>(
					binding.definition_instruction_index)];
			if (result != INVALID_VALUE_REF) {
				return result;
			}
		}
		if (binding.value_index >= 0
				&& static_cast<uint32_t>(binding.value_index)
					< plan_->value_count) {
			const zend_tpde_value &value =
				plan_->values[
					static_cast<uint32_t>(binding.value_index)];
			/*
			 * A binding with an explicit definition must not inherit a later
			 * source-SSA override when that definition stayed canonical.  Such an
			 * override belongs to another producer of the reused SSA identity and
			 * would expose a TPDE value before its defining instruction.
			 */
			if (!has_definition
					&& zend_mir_value_is_original_ssa(value.id)
					&& value.id
						< source_overrides.size()) {
				const IRValueRef override =
					source_overrides[value.id];
				if (override != INVALID_VALUE_REF) {
					return override;
				}
			}
			return value_ref(value.id);
		}
		return INVALID_VALUE_REF;
	}

	int32_t block_index(zend_mir_block_id id) const {
		return zend_tpde_block_index(plan_, id);
	}

	IRValueRef value_ref(zend_mir_value_id id) const {
		int32_t index = zend_tpde_value_index(plan_, id);
		if (index < 0) {
			return INVALID_VALUE_REF;
		}
		IRValueRef value{
			MIR_VALUE_BASE + static_cast<uint32_t>(index)};
		const auto &overrides = active_value_overrides();
		for (uint32_t depth = 0;
				depth <= overrides.size(); ++depth) {
			const uint32_t current = static_cast<uint32_t>(value);
			if (current < MIR_VALUE_BASE
					|| current - MIR_VALUE_BASE
						>= overrides.size()) {
				return value;
			}
			const IRValueRef override =
				overrides[current - MIR_VALUE_BASE];
			if (override == INVALID_VALUE_REF || override == value) {
				return value;
			}
			value = override;
		}
		return INVALID_VALUE_REF;
	}

	IRValueRef source_operand_value_ref(
			const zend_mir_source_operand_ref &operand) const {
		zend_mir_value_id value_id;
		const auto &source_overrides = active_source_ssa_overrides();
		const auto &value_overrides = active_value_overrides();

		switch (operand.kind) {
			case ZEND_MIR_SOURCE_OPERAND_LITERAL:
				value_id = zend_mir_value_from_synthetic(operand.index);
				break;
			case ZEND_MIR_SOURCE_OPERAND_SLOT:
			case ZEND_MIR_SOURCE_OPERAND_SSA:
				if (operand.ssa_variable_id == ZEND_MIR_ID_INVALID) {
					return INVALID_VALUE_REF;
				}
				if (operand.ssa_variable_id
							< source_overrides.size()) {
					const IRValueRef override =
						source_overrides[
							operand.ssa_variable_id];
					if (override != INVALID_VALUE_REF) {
						return override;
					}
				}
				value_id = zend_mir_value_from_original_ssa(
					operand.ssa_variable_id);
				break;
			default:
				return INVALID_VALUE_REF;
		}
		const IRValueRef value = value_ref(value_id);
		const uint32_t index = static_cast<uint32_t>(value);
		if (value != INVALID_VALUE_REF
				&& index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE
					< value_overrides.size()) {
			const IRValueRef override =
				value_overrides[index - MIR_VALUE_BASE];
			if (override != INVALID_VALUE_REF) {
				return override;
			}
		}
		return value;
	}

	zend_mir_storage_id source_operand_storage_id(
			const zend_mir_source_operand_ref &operand) const {
		if (operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
			return ZEND_MIR_ID_INVALID;
		}
		if (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			return operand.index < plan_->source_frame_variable_count
				? operand.index : ZEND_MIR_ID_INVALID;
		}
		if ((operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
					|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
				&& operand.index < plan_->source_temporary_count
				&& plan_->source_frame_variable_count
					<= ZEND_MIR_ID_MAX - operand.index) {
			return plan_->source_frame_variable_count + operand.index;
		}
		return ZEND_MIR_ID_INVALID;
	}

	bool direct_internal_source_argument_stable(
			const zend_mir_call_argument_ref &argument,
			uint32_t call_source_position) const {
		const zend_mir_storage_id storage =
			source_operand_storage_id(argument.source_operand);
		if (!zend_mir_id_is_valid(storage)
				|| plan_->source_opcodes == nullptr
				|| argument.send_opline_index >= call_source_position
				|| call_source_position > plan_->source_opcode_count) {
			return false;
		}
		const uint32_t encoded_storage = EX_NUM_TO_VAR(storage);
		for (uint32_t source = argument.send_opline_index + 1;
				source < call_source_position; ++source) {
			const zend_tpde_source_opcode &opline =
				plan_->source_opcodes[source];
			const bool op1_slot = opline.op1_type == IS_CV
				|| opline.op1_type == IS_TMP_VAR
				|| opline.op1_type == IS_VAR;
			const bool op2_slot = opline.op2_type == IS_CV
				|| opline.op2_type == IS_TMP_VAR
				|| opline.op2_type == IS_VAR;
			const bool result_slot = opline.result_type == IS_CV
				|| opline.result_type == IS_TMP_VAR
				|| opline.result_type == IS_VAR;
			/*
			 * Treat every later mention as unstable, including a read. This is
			 * deliberately stronger than a write-only test: it also prevents an
			 * earlier delayed TMP/VAR argument from being consumed after a later
			 * SEND has observed or reused the same physical slot.
			 */
			if ((op1_slot && opline.op1_var == encoded_storage)
					|| (op2_slot && opline.op2_var == encoded_storage)
					|| (result_slot
						&& opline.result_var == encoded_storage)) {
				return false;
			}
		}
		return true;
	}

	IRValueRef mutation_value_ref(
			const zend_tpde_instruction &instruction) const {
		zend_tpde_long_assign_op long_assign{};
		zend_tpde_long_incdec long_incdec{};

		if (!instruction.has_value_operation
				|| !((zend_tpde_long_assign_op_at(
							instruction, &long_assign)
							&& !long_assign.has_result)
						|| (zend_tpde_long_incdec_at(
							instruction, &long_incdec)
							&& !long_incdec.has_result))
				|| instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one == 0) {
			return INVALID_VALUE_REF;
		}
		return value_ref(zend_mir_value_from_original_ssa(
			instruction.value_operation
				.op1_definition_ssa_variable_id_plus_one - 1));
	}

	bool storage_assigned_by_reference(
			zend_mir_storage_id storage_id) const {
		if (!zend_mir_id_is_valid(storage_id)) {
			return false;
		}
		for (uint32_t index = 0; index < plan_->instruction_count; ++index) {
			const zend_tpde_instruction &candidate =
				plan_->instructions[index];
			if (candidate.has_value_operation
					&& candidate.record.opcode
						== ZEND_MIR_OPCODE_VALUE_ASSIGN_REF
					&& (candidate.value_operation.op1_storage_id
							== storage_id
						|| candidate.value_operation.op2_storage_id
							== storage_id)) {
				return true;
			}
		}
		return false;
	}

	bool long_binary_machine_operands(
			const zend_tpde_instruction &instruction,
			IRValueRef &left, IRValueRef &right) const {
		if (!instruction.has_value_operation
				|| instruction.value_operation.opcode
					!= ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
			return false;
		}
		const uint32_t source_opcode =
			instruction.value_operation.source_opcode;
		if (source_opcode != ZEND_ADD
				&& source_opcode != ZEND_SUB
				&& source_opcode != ZEND_BW_OR
				&& source_opcode != ZEND_BW_AND
				&& source_opcode != ZEND_BW_XOR
				&& source_opcode != ZEND_SPACESHIP
				&& source_opcode != ZEND_IS_IDENTICAL
				&& source_opcode != ZEND_IS_NOT_IDENTICAL
				&& source_opcode != ZEND_IS_EQUAL
				&& source_opcode != ZEND_IS_NOT_EQUAL
				&& source_opcode != ZEND_IS_SMALLER
				&& source_opcode != ZEND_IS_SMALLER_OR_EQUAL) {
			return false;
		}
		/*
		 * Full DFA may coalesce the source SSA identity of a fetch result
		 * back into its canonical zval slot.  The frozen source binding still
		 * names the exact defining instruction and therefore preserves its
		 * register-authoritative boxed result.  Prefer that def-use edge and
		 * retain the source-operand lookup for ordinary scalar plans.
		 */
		left = source_binding_value_ref(instruction.source_op1_binding);
		if (left == INVALID_VALUE_REF) {
			left = source_operand_value_ref(
				instruction.value_operation.op1);
		}
		right = source_binding_value_ref(instruction.source_op2_binding);
		if (right == INVALID_VALUE_REF) {
			right = source_operand_value_ref(
				instruction.value_operation.op2);
		}
		auto register_or_constant = [&](IRValueRef value) {
			if (machine_value_is_register_authoritative(value)) {
				/*
				 * A frozen machine kind alone does not prove that this adaptor
				 * emits a TPDE definition.  In particular, an unselected direct
				 * user call publishes its scalar result through the canonical
				 * Zend slot even though its value plan remains register-
				 * authoritative.  Require the actual definition for every
				 * non-constant operand before selecting the register fast path.
				 */
				return machine_value_has_register_definition(value);
			}
			if (const DerivedValue *derived = derived_value(value)) {
				return derived->constant;
			}
			const uint32_t index = static_cast<uint32_t>(value);
			return index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count
				&& plan_->values[index - MIR_VALUE_BASE].constant;
		};
		auto long_register_value = [&](IRValueRef value) {
			return exact_type(value) == ZEND_MIR_SCALAR_TYPE_I64
				|| machine_kind(value)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
		};
		const bool selected = left != INVALID_VALUE_REF
			&& right != INVALID_VALUE_REF
			&& long_register_value(left)
			&& long_register_value(right)
			&& register_or_constant(left)
			&& register_or_constant(right);
		return selected;
	}

	IRValueRef add_derived_value(
			zend_mir_representation representation,
			zend_mir_scalar_type_mask exact_type,
			zend_mir_storage_id storage_id,
			bool constant = false, uint64_t constant_bits = 0,
			uint8_t explicit_machine_kind = UINT8_MAX,
			zend_mir_ownership_state ownership =
				ZEND_MIR_OWNERSHIP_STATE_OWNED,
			zend_mir_refcount_state refcount_state =
				ZEND_MIR_REFCOUNT_UNKNOWN,
			uint32_t machine_reference_index = UINT32_MAX,
			bool known_string_literal = false,
			uint8_t known_string_first_byte = 0,
			uint64_t known_string_length = 0) {
		if (derived_values_.size()
				>= UINT32_MAX - MIR_VALUE_BASE - plan_->value_count) {
			valid_ = false;
			return INVALID_VALUE_REF;
		}
		zend_tpde_machine_value_kind kind = ZEND_TPDE_MACHINE_VALUE_I64;
		if (explicit_machine_kind != UINT8_MAX) {
			kind = static_cast<zend_tpde_machine_value_kind>(
				explicit_machine_kind);
		} else if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			kind = ZEND_TPDE_MACHINE_VALUE_BOOL;
		} else if (exact_type == ZEND_MIR_SCALAR_TYPE_F64
				|| representation == ZEND_MIR_REPRESENTATION_DOUBLE) {
			kind = ZEND_TPDE_MACHINE_VALUE_F64;
		}
		const IRValueRef value{
			MIR_VALUE_BASE + plan_->value_count
				+ static_cast<uint32_t>(derived_values_.size())};
		derived_values_.push_back({
			representation, exact_type, storage_id, kind, ownership,
			refcount_state, machine_reference_index, constant, constant_bits,
			known_string_literal, known_string_first_byte,
			known_string_length});
		const uint32_t required_value_count =
			static_cast<uint32_t>(value) + 1;
		if (phi_input_slices_.size() < required_value_count) {
			phi_input_slices_.resize(required_value_count);
			phi_values_.resize(required_value_count);
		}
		if (machine_reference_index != UINT32_MAX) {
			machine_reference_values_.push_back(value);
		}
		return value;
	}

	uint32_t machine_reference_index(
			zend_tpde_machine_reference_kind kind,
			uint32_t stable_storage_or_layout_id) const {
		for (uint32_t index = 0;
				index < plan_->machine_reference_count; ++index) {
			const zend_tpde_machine_reference &reference =
				plan_->machine_references[index];
			if (reference.kind == kind
					&& reference.stable_storage_or_layout_id
						== stable_storage_or_layout_id) {
				return index;
			}
		}
		return UINT32_MAX;
	}

	const DerivedValue *derived_value(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		const uint32_t base = MIR_VALUE_BASE + plan_->value_count;
		return index < base || index - base >= derived_values_.size()
			? nullptr : &derived_values_[index - base];
	}

	static InstKind executable_kind(
			const zend_tpde_instruction &instruction,
			const zend_mir_instruction_record &record) {
		if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
			return instruction.zval_store_lazy_scalar
				? InstKind::MIR : InstKind::ZvalStore;
		}
		if (!instruction.has_value_operation) {
			return InstKind::MIR;
		}
		switch (instruction.value_operation.opcode) {
			case ZEND_MIR_OPCODE_VALUE_ASSIGN:
				if (instruction.value_operation.op1.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV) {
					return InstKind::SlowPathCall;
				}
				return instruction.value_operation.op2.slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP
					? InstKind::ZvalMove : InstKind::ZvalCopy;
			case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
				if (instruction.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_VAR) {
					return InstKind::SlowPathCall;
				}
				return instruction.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_TMP
					? InstKind::ZvalMove : InstKind::ZvalCopy;
			case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
				return InstKind::ZvalCopy;
			case ZEND_MIR_OPCODE_VALUE_FREE:
				return InstKind::ZvalReleaseFast;
			case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
			case ZEND_MIR_OPCODE_VALUE_SEPARATE:
			case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
			case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
			case ZEND_MIR_OPCODE_VALUE_CONCAT:
			case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
			case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
			case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
			case ZEND_MIR_OPCODE_VALUE_ROPE_END:
			case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
			case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
			case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK:
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
			case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP:
			case ZEND_MIR_OPCODE_VALUE_UNSET_DIM:
			case ZEND_MIR_OPCODE_VALUE_FE_FREE:
			case ZEND_MIR_OPCODE_VALUE_CAST:
			case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
			case ZEND_MIR_OPCODE_VALUE_ECHO:
			case ZEND_MIR_OPCODE_FUNC_GET_ARGS:
			case ZEND_MIR_OPCODE_VALUE_COUNT:
			case ZEND_MIR_OPCODE_VALUE_GET_TYPE:
			case ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS:
			case ZEND_MIR_OPCODE_VALUE_IN_ARRAY:
			case ZEND_MIR_OPCODE_VALUE_ISSET_THIS:
			case ZEND_MIR_OPCODE_VALUE_GET_CALLED_CLASS:
			case ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE:
			case ZEND_MIR_OPCODE_VALUE_END_SILENCE:
			case ZEND_MIR_OPCODE_VALUE_MATCH_ERROR:
			case ZEND_MIR_OPCODE_VALUE_VERIFY_NEVER_TYPE:
			case ZEND_MIR_OPCODE_VALUE_DEFINED:
			case ZEND_MIR_OPCODE_VALUE_TICKS:
			case ZEND_MIR_OPCODE_VALUE_TYPE_ASSERT:
			case ZEND_MIR_OPCODE_VALUE_EXT_STMT:
			case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_BEGIN:
			case ZEND_MIR_OPCODE_VALUE_EXT_FCALL_END:
			case ZEND_MIR_OPCODE_VALUE_EXT_NOP:
			case ZEND_MIR_OPCODE_VALUE_DISCARD_EXCEPTION:
			case ZEND_MIR_OPCODE_VALUE_CHECK_FUNC_ARG:
			case ZEND_MIR_OPCODE_VALUE_CHECK_UNDEF_ARGS:
				return InstKind::SlowPathCall;
			case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
			case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
			case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
			case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
			case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
			case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
			case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			case ZEND_MIR_OPCODE_VALUE_INCDEC:
			case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
			case ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH:
			case ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH:
			case ZEND_MIR_OPCODE_OBJECT_FETCH_R:
			case ZEND_MIR_OPCODE_OBJECT_ASSIGN:
			case ZEND_MIR_OPCODE_DYNAMIC_FETCH_R:
				return InstKind::MIR;
			default:
				return InstKind::MIR;
		}
	}

	enum class ScalarTypeCheckSelection : uint8_t {
		Invalid,
		CopyInput,
		NotInput,
		ConstantFalse,
		ConstantTrue,
	};

	static ScalarTypeCheckSelection scalar_type_check_selection(
			const zend_tpde_plan *plan,
			const zend_tpde_instruction &instruction,
			IRValueRef *input_out = nullptr,
			IRValueRef *result_out = nullptr) {
		if (plan == nullptr || !instruction.has_value_operation
				|| instruction.value_operation.opcode
					!= ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
				|| instruction.value_operation.source_opcode
					!= ZEND_TYPE_CHECK) {
			return ScalarTypeCheckSelection::Invalid;
		}
		const IRValueRef input = plan_source_operand_value_ref(
			plan, instruction.value_operation.op1);
		const IRValueRef result = plan_source_operand_value_ref(
			plan, instruction.value_operation.result);
		const uint32_t input_index = static_cast<uint32_t>(input);
		const uint32_t result_index = static_cast<uint32_t>(result);
		if (input == INVALID_VALUE_REF
				|| input_index < MIR_VALUE_BASE
				|| input_index - MIR_VALUE_BASE >= plan->value_count
				|| instruction.value_operation.result.ssa_variable_id
					== ZEND_MIR_ID_INVALID
				|| (result != INVALID_VALUE_REF
					&& (result_index < MIR_VALUE_BASE
						|| result_index - MIR_VALUE_BASE
							>= plan->value_count))) {
			return ScalarTypeCheckSelection::Invalid;
		}
		if (input_out != nullptr) {
			*input_out = input;
		}
		if (result_out != nullptr) {
			*result_out = result;
		}
		const zend_mir_scalar_type_mask input_type =
			plan->values[input_index - MIR_VALUE_BASE].exact_type;
		const uint32_t expected = instruction.value_operation.extended_value;
		if (input_type == ZEND_MIR_SCALAR_TYPE_I1) {
			const uint32_t bool_mask = expected & MAY_BE_BOOL;
			if (bool_mask == MAY_BE_TRUE) {
				return ScalarTypeCheckSelection::CopyInput;
			}
			if (bool_mask == MAY_BE_FALSE) {
				return ScalarTypeCheckSelection::NotInput;
			}
			return bool_mask == MAY_BE_BOOL
				? ScalarTypeCheckSelection::ConstantTrue
				: ScalarTypeCheckSelection::ConstantFalse;
		}
		uint32_t input_mask;
		switch (input_type) {
			case ZEND_MIR_SCALAR_TYPE_NULL:
				input_mask = MAY_BE_NULL;
				break;
			case ZEND_MIR_SCALAR_TYPE_I64:
				input_mask = MAY_BE_LONG;
				break;
			case ZEND_MIR_SCALAR_TYPE_F64:
				input_mask = MAY_BE_DOUBLE;
				break;
			default:
				return ScalarTypeCheckSelection::Invalid;
		}
		return (expected & input_mask) != 0
			? ScalarTypeCheckSelection::ConstantTrue
			: ScalarTypeCheckSelection::ConstantFalse;
	}

	bool is_register_cond_branch(
			const zend_tpde_instruction &instruction,
			IRValueRef *condition_out = nullptr) const {
		const bool frozen_register_branch =
			(instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH) != 0;
		if (!frozen_register_branch) {
			return false;
		}
		IRValueRef condition = INVALID_VALUE_REF;
		condition = source_binding_value_ref(
			instruction.source_op1_binding);
		if (condition == INVALID_VALUE_REF) {
			condition = source_operand_value_ref(
				instruction.value_operation.op1);
		}
		if (condition == INVALID_VALUE_REF) {
			return function_mode_ == FunctionMode::TypedBody
				&& condition_out == nullptr
				&& instruction.value_operation.op1.ssa_variable_id
					!= ZEND_MIR_ID_INVALID;
		}
		const bool machine_condition =
			(exact_type(condition) == ZEND_MIR_SCALAR_TYPE_I1
				&& machine_kind(condition) == ZEND_TPDE_MACHINE_VALUE_BOOL)
			|| (exact_type(condition) == ZEND_MIR_SCALAR_TYPE_I64
				&& machine_kind(condition) == ZEND_TPDE_MACHINE_VALUE_I64);
		if (!machine_condition
				|| !machine_value_is_register_authoritative(condition)
				|| !machine_value_has_register_definition(condition)) {
			return false;
		}
		if (condition_out != nullptr) {
			*condition_out = condition;
		}
		return true;
	}

	bool is_boxed_cond_branch(
			const zend_tpde_instruction &instruction) const {
		return (instruction.machine_control_flow_flags
				& ZEND_TPDE_MACHINE_CONTROL_FLOW_BOXED_BRANCH) != 0
			&& !is_register_cond_branch(instruction);
	}

	bool machine_value_has_frozen_use(uint32_t value_index) const {
		if (value_index >= plan_->value_count) {
			return false;
		}
		const uint8_t *required =
			function_mode_ == FunctionMode::TypedBody
				? plan_->typed_body_value_required
				: plan_->entry_value_required;
		return required != nullptr && required[value_index] != 0;
	}

	zend_mir_instruction_record instruction_record_at(uint32_t index) const {
		return zend_tpde_instruction_record_at(
			plan_, zend_tpde_instruction_at(plan_, index));
	}

	void add_node(
			std::vector<BlockItem<IRInstRef>> &block_instructions,
			uint32_t block, InstNode node) {
		if (node.control_block == UINT32_MAX) {
			node.control_block = block;
		}
		uint32_t index = static_cast<uint32_t>(nodes_.size());
		nodes_.push_back(std::move(node));
		fused_instructions_.push_back(0);
		block_instructions.push_back({block, IRInstRef{index}});
	}

	template <typename T>
	static void flatten_block_items(
			uint32_t block_count,
			const std::vector<BlockItem<T>> &items,
			std::vector<Slice> &slices,
			std::vector<T> &values) {
		slices.assign(block_count, {});
		for (const BlockItem<T> &item : items) {
			++slices[item.block].count;
		}
		uint32_t offset = 0;
		for (Slice &slice : slices) {
			slice.offset = offset;
			offset += slice.count;
			slice.count = 0;
		}
		values.clear();
		if (offset != 0) {
			values.assign(offset, items.front().value);
		}
		for (const BlockItem<T> &item : items) {
			Slice &slice = slices[item.block];
			values[slice.offset + slice.count++] = item.value;
		}
	}

	/*
	 * Clone a proof-closed scalar callee body into the caller graph from the
	 * frozen ZNMIR plans.  This deliberately operates after every component
	 * member has been lowered: source opcodes are not consulted and the target
	 * backends see ordinary TPDE instructions and values.
	 *
	 * The observer-enabled path still executes the complete Zend-frame call.
	 * Pure scalar instructions may therefore execute speculatively before the
	 * observer guard, but no effectful, trapping, branching, or frame-observing
	 * instruction is admitted here.
	 */
	InlinedBody inline_component_scalar_body(
			const zend_tpde_instruction &call,
			uint32_t caller_instruction_index,
			IRValueRef caller_result,
			uint32_t caller_block,
			std::vector<BlockItem<IRInstRef>> &block_instructions) {
		if (call.direct_call == nullptr
				|| call.component_target_index == UINT32_MAX
				|| call.component_target_index >= component_plans_.size()
				|| (call.direct_call->receiver_kind
						!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE
					&& (call.direct_call->receiver_kind
							!= ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
						|| (call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0))
				|| (call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I1
					&& call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I64)) {
			return {};
		}
		const zend_tpde_plan *callee =
			component_plans_[call.component_target_index];
		zend_tpde_scalar_diamond diamond{};
		if (callee == nullptr || callee == plan_
				|| callee->argument_count != call.call_argument_count
				|| callee->generator_resume_count != 0
				|| callee->user_opcode_callbacks) {
			return {};
		}
		const bool scalar_diamond = callee->block_count != 1;
		if (callee->block_count != 1
				&& !zend_tpde_scalar_diamond_at(callee, &diamond)) {
			return {};
		}
		const bool effect_closed_inline =
			frozen_effect_closed_inline(caller_instruction_index);
		if (scalar_diamond && !effect_closed_inline) {
			return {};
		}

		std::vector<IRValueRef> values(
			callee->value_count, INVALID_VALUE_REF);
		std::vector<int32_t> checked_value_steps(
			callee->value_count, -1);
		const size_t derived_value_checkpoint = derived_values_.size();
		const size_t machine_reference_checkpoint =
			machine_reference_values_.size();
		const size_t operand_checkpoint = operands_.size();
		const size_t node_checkpoint = nodes_.size();
		const size_t fused_checkpoint = fused_instructions_.size();
		const size_t block_instruction_checkpoint =
			block_instructions.size();
		auto fail_inline = [&]() -> InlinedBody {
			derived_values_.erase(
				derived_values_.begin() + derived_value_checkpoint,
				derived_values_.end());
			machine_reference_values_.erase(
				machine_reference_values_.begin()
					+ machine_reference_checkpoint,
				machine_reference_values_.end());
			operands_.erase(operands_.begin() + operand_checkpoint,
				operands_.end());
			nodes_.erase(nodes_.begin() + node_checkpoint, nodes_.end());
			fused_instructions_.erase(
				fused_instructions_.begin() + fused_checkpoint,
				fused_instructions_.end());
			block_instructions.erase(
				block_instructions.begin() + block_instruction_checkpoint,
				block_instructions.end());
			return {};
		};
		for (uint32_t argument_index = 0;
				argument_index < callee->argument_count; ++argument_index) {
			const int32_t callee_value_index =
				callee->argument_value_indices == nullptr
				? -1 : callee->argument_value_indices[argument_index];
			zend_mir_call_argument_ref argument;
			if (callee_value_index < 0
					|| static_cast<uint32_t>(callee_value_index)
						>= callee->value_count
					|| !zend_tpde_call_argument_at(plan_,
						call.call_argument_offset + argument_index,
						&argument)) {
				return fail_inline();
			}
			const zend_tpde_value &callee_value =
				callee->values[static_cast<uint32_t>(callee_value_index)];
			const TypedBodyAbiType callee_argument_abi =
				typed_body_value_abi(callee,
					static_cast<uint32_t>(callee_value_index));
			const zend_mir_representation expected_representation =
				callee_argument_abi.valid
					? callee_argument_abi.representation
					: callee_value.representation;
			const zend_mir_scalar_type_mask expected_exact_type =
				callee_argument_abi.valid
					? callee_argument_abi.exact_type
					: callee_value.exact_type;
			const zend_tpde_machine_value_kind expected_machine_kind =
				callee_argument_abi.valid
					? callee_argument_abi.machine_kind
					: callee_value.machine_kind;
			IRValueRef caller_value = INVALID_VALUE_REF;
			if (effect_closed_inline
					&& plan_->call_argument_bindings != nullptr) {
				caller_value = source_binding_value_ref(
					plan_->call_argument_bindings[
						call.call_argument_offset + argument_index]);
			}
			if (caller_value == INVALID_VALUE_REF) {
				caller_value =
					source_operand_value_ref(argument.source_operand);
			}
			if (zend_mir_id_is_valid(argument.value_id)
					&& (!effect_closed_inline
						|| caller_value == INVALID_VALUE_REF)) {
				caller_value = value_ref(argument.value_id);
			} else if (caller_value == INVALID_VALUE_REF) {
				const zend_native_direct_call_argument &descriptor_argument =
					call.direct_call->arguments[argument_index];
				if (!zend_mir_scalar_type_is_exact(
						descriptor_argument.exact_type)
						|| descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_NULL
						|| descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_F64
						|| descriptor_argument.exact_type
							!= expected_exact_type) {
					continue;
				}
				caller_value = add_derived_value(
					expected_representation,
					descriptor_argument.exact_type,
					ZEND_MIR_ID_INVALID, true,
					descriptor_argument.scalar_bits);
			}
			if (caller_value != INVALID_VALUE_REF
					&& machine_kind(caller_value)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& (expected_machine_kind
							== ZEND_TPDE_MACHINE_VALUE_I64
						|| expected_machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOOL)
					&& (exact_type(caller_value) == expected_exact_type
						|| exact_type(caller_value)
							== ZEND_MIR_SCALAR_TYPE_NONE)
					&& machine_value_is_register_authoritative(
						caller_value)) {
				const IRValueRef transported = add_derived_value(
					expected_representation, expected_exact_type,
					canonical_storage(caller_value), false, 0,
					expected_machine_kind,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN);
				if (transported == INVALID_VALUE_REF) {
					return fail_inline();
				}
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(caller_value);
				add_node(block_instructions, caller_block, InstNode{
					InstKind::UnboxScalar, UINT32_MAX, UINT32_MAX,
					transported, {}, operand_offset, 1, true,
					canonical_storage(caller_value),
					expected_exact_type});
				caller_value = transported;
			}
			if (effect_closed_inline
					&& caller_value != INVALID_VALUE_REF
					&& machine_kind(caller_value)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& (expected_machine_kind
							== ZEND_TPDE_MACHINE_VALUE_I64
						|| expected_machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOOL)
					&& (exact_type(caller_value) == expected_exact_type
						|| exact_type(caller_value)
							== ZEND_MIR_SCALAR_TYPE_NONE)
					&& !machine_value_is_register_authoritative(
						caller_value)) {
				const zend_mir_storage_id storage_id =
					canonical_storage(caller_value);
				const uint32_t reference = zend_mir_id_is_valid(storage_id)
					? machine_reference_index(
						ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT, storage_id)
					: UINT32_MAX;
				const IRValueRef address = reference != UINT32_MAX
					? add_derived_value(
						ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
						ZEND_MIR_SCALAR_TYPE_NONE, storage_id,
						false, 0, UINT8_MAX,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN, reference)
					: INVALID_VALUE_REF;
				const IRValueRef transported = address != INVALID_VALUE_REF
					? add_derived_value(
						expected_representation, expected_exact_type,
						storage_id, false, 0, expected_machine_kind,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN)
					: INVALID_VALUE_REF;
				if (transported == INVALID_VALUE_REF) {
					return fail_inline();
				}
				const uint32_t load_operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(address);
				add_node(block_instructions, caller_block, InstNode{
					InstKind::ZvalPayloadLoad, UINT32_MAX, UINT32_MAX,
					transported, {}, load_operand_offset, 1, true,
					storage_id, expected_exact_type});
				caller_value = transported;
			}
			if (caller_value != INVALID_VALUE_REF
					&& machine_kind(caller_value)
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
					&& expected_machine_kind
						== ZEND_TPDE_MACHINE_VALUE_I64
					&& call.direct_call->arguments[argument_index].exact_type
						== expected_exact_type
					&& machine_value_is_register_authoritative(caller_value)) {
				const IRValueRef transported = add_derived_value(
					expected_representation, expected_exact_type,
					canonical_storage(caller_value), false, 0,
					expected_machine_kind,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN);
				if (transported == INVALID_VALUE_REF) {
					return fail_inline();
				}
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(caller_value);
				add_node(block_instructions, caller_block, InstNode{
					InstKind::UnboxReferenceScalar, UINT32_MAX, UINT32_MAX,
					transported, {}, operand_offset, 1, true,
					canonical_storage(caller_value), expected_exact_type});
				caller_value = transported;
			}
			if (caller_value != INVALID_VALUE_REF
					&& machine_kind(caller_value)
						== expected_machine_kind
					&& machine_pointer_kind(expected_machine_kind)
					&& !machine_value_is_register_authoritative(
						caller_value)) {
				const zend_mir_storage_id storage_id =
					canonical_storage(caller_value);
				const uint32_t reference =
					zend_mir_id_is_valid(storage_id)
						? machine_reference_index(
							ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
							storage_id)
						: UINT32_MAX;
				const IRValueRef address =
					reference != UINT32_MAX
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE,
							storage_id, false, 0, UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN,
							reference)
						: INVALID_VALUE_REF;
				const IRValueRef transported =
					address != INVALID_VALUE_REF
						? add_derived_value(
							expected_representation,
							expected_exact_type,
							storage_id, false, 0,
							expected_machine_kind,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							callee_value.refcount_state)
						: INVALID_VALUE_REF;
				if (transported == INVALID_VALUE_REF) {
					return fail_inline();
				}
				const uint32_t load_operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(address);
				add_node(block_instructions, caller_block, InstNode{
					InstKind::ZvalPayloadLoad, UINT32_MAX, UINT32_MAX,
					transported, {}, load_operand_offset, 1, true,
					storage_id, callee_value.exact_type});
				caller_value = transported;
			}
			if (caller_value == INVALID_VALUE_REF
					|| exact_type(caller_value)
						!= expected_exact_type
					|| machine_kind(caller_value)
						!= expected_machine_kind
					|| representation(caller_value)
						!= expected_representation
					|| exact_type(caller_value)
						== ZEND_MIR_SCALAR_TYPE_F64) {
				/*
				 * An effect-closed body need not transport arguments it never
				 * consumes.  Leave an ABI-incompatible unused parameter
				 * unmapped; any real use below still fails the clone through
				 * mapped_value().
				 */
				continue;
			}
			values[static_cast<uint32_t>(callee_value_index)] =
				caller_value;
		}

		auto mapped_value = [&](zend_mir_value_id value_id) -> IRValueRef {
			int32_t value_index =
				zend_tpde_value_index(callee, value_id);
			if (value_index < 0) {
				return INVALID_VALUE_REF;
			}
			for (uint32_t depth = 0;
					depth < callee->value_count; ++depth) {
				IRValueRef &mapped =
					values[static_cast<uint32_t>(value_index)];
				if (mapped != INVALID_VALUE_REF) {
					return mapped;
				}
				const int32_t alias =
					callee->values[static_cast<uint32_t>(value_index)]
						.register_alias_value_index;
				if (alias < 0 || alias == value_index) {
					break;
				}
				if (static_cast<uint32_t>(alias)
						>= callee->value_count) {
					return INVALID_VALUE_REF;
				}
				value_index = alias;
			}
			IRValueRef &mapped = values[static_cast<uint32_t>(value_index)];
			if (mapped != INVALID_VALUE_REF) {
				return mapped;
			}
			const zend_tpde_value &value =
				callee->values[static_cast<uint32_t>(value_index)];
			if (!value.constant
					|| !zend_mir_scalar_type_is_exact(value.exact_type)
					|| value.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				return INVALID_VALUE_REF;
			}
			mapped = add_derived_value(value.representation,
				value.exact_type, ZEND_MIR_ID_INVALID, true,
				value.constant_bits);
			return mapped;
		};
		auto source_value_id =
			[](const zend_mir_source_operand_ref &source)
				-> zend_mir_value_id {
				zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
				if (source.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
					value_id =
						zend_mir_value_from_synthetic(source.index);
				} else if ((source.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| source.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& source.ssa_variable_id
							!= ZEND_MIR_ID_INVALID) {
					value_id = zend_mir_value_from_original_ssa(
						source.ssa_variable_id);
				}
				return value_id;
			};
		auto mapped_source_operand =
			[&](const zend_mir_source_operand_ref &source)
				-> IRValueRef {
				const zend_mir_value_id value_id = source_value_id(source);
				return zend_mir_id_is_valid(value_id)
					? mapped_value(value_id) : INVALID_VALUE_REF;
			};
		auto checked_step_for_index = [&](int32_t value_index) -> int32_t {
				for (uint32_t depth = 0;
						value_index >= 0 && depth < callee->value_count;
						++depth) {
					if (static_cast<uint32_t>(value_index)
							>= checked_value_steps.size()) {
						return -1;
					}
					if (checked_value_steps[
							static_cast<uint32_t>(value_index)] >= 0) {
						return checked_value_steps[
							static_cast<uint32_t>(value_index)];
					}
					const int32_t alias = callee->values[
						static_cast<uint32_t>(value_index)]
						.register_alias_value_index;
					if (alias < 0 || alias == value_index) {
						break;
					}
					value_index = alias;
				}
				return -1;
			};
		auto checked_step_for_value =
			[&](zend_mir_value_id value_id) -> int32_t {
				return checked_step_for_index(
					zend_tpde_value_index(callee, value_id));
			};
		auto checked_step_for_binding =
			[&](const zend_tpde_source_value_binding &binding) -> int32_t {
				return checked_step_for_index(binding.value_index);
			};
		auto record_checked_step = [&](int32_t value_index, int32_t step) {
			if (value_index >= 0
					&& static_cast<uint32_t>(value_index)
						< checked_value_steps.size()) {
				checked_value_steps[static_cast<uint32_t>(value_index)] = step;
			}
		};

		IRValueRef returned = INVALID_VALUE_REF;
		int32_t returned_checked_step = -1;
		bool saw_return = false;
		zend_mir_value_id deferred_return_id = ZEND_MIR_ID_INVALID;
		zend_tpde_source_value_binding deferred_return_binding{};
		bool deferred_return_has_binding = false;
		IRValueRef diamond_condition = INVALID_VALUE_REF;
		std::vector<uint32_t> pending_phis;
		IRValueRef checked_left = INVALID_VALUE_REF;
		IRValueRef checked_right = INVALID_VALUE_REF;
		uint32_t checked_source_opcode = UINT32_MAX;
		std::vector<IRValueRef> checked_operands;
		std::vector<InlinedCheckedStep> checked_steps;
		bool cloned_string_length = false;
		for (uint32_t index = 0; index < callee->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				callee->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(callee, &instruction);
			const int32_t instruction_block =
				zend_tpde_block_index(callee, record.block_id);
			if (instruction_block < 0
					|| (!scalar_diamond
						&& record.block_id != callee->block_ids[0])) {
				return fail_inline();
			}
			if (instruction.local_abi_transport) {
				continue;
			}
			if (scalar_diamond
					&& zend_tpde_scalar_diamond_frame_transport(
						callee, instruction)) {
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
				/*
				 * Constants are mapped lazily when a cloned machine
				 * instruction consumes them.  Lowering also retains
				 * registerless NULL/UNDEF topology constants, which are not
				 * part of an exact scalar return body and must not reject an
				 * otherwise effect-closed inline candidate.
				 */
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
				if (record.effects != 0) {
					return fail_inline();
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				if (!scalar_diamond
						|| static_cast<uint32_t>(instruction_block)
							!= diamond.merge
						|| instruction.operand_count != 2
						|| !zend_mir_id_is_valid(record.result_id)) {
					return fail_inline();
				}
				pending_phis.push_back(index);
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH) {
				if (!scalar_diamond
						|| diamond_condition != INVALID_VALUE_REF
						|| static_cast<uint32_t>(instruction_block)
							!= diamond.entry
						|| instruction.operand_count != 1) {
					return fail_inline();
				}
				diamond_condition = mapped_value(
					zend_tpde_operand_at(callee, &instruction, 0));
				if (diamond_condition == INVALID_VALUE_REF
						|| exact_type(diamond_condition)
							!= ZEND_MIR_SCALAR_TYPE_I1
						|| machine_kind(diamond_condition)
							!= ZEND_TPDE_MACHINE_VALUE_BOOL) {
					return fail_inline();
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_BRANCH) {
				const uint32_t block =
					static_cast<uint32_t>(instruction_block);
				if (!scalar_diamond
						|| (block != diamond.true_block
							&& block != diamond.false_block)) {
					return fail_inline();
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
					&& (instruction.runtime_helper
							== ZEND_NATIVE_HELPER_COUNT
						|| cloned_string_length
						|| (!checked_steps.empty()
							&& call.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_I64)
						|| (callee->typed_body_return_abi.valid
							&& callee->typed_body_return_abi.exact_type
								== call.direct_call->result_type))) {
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
				zend_mir_value_id returned_id = ZEND_MIR_ID_INVALID;
				const zend_mir_source_operand_ref &source =
					instruction.value_operation.op1;
				if (saw_return || !instruction.has_value_operation
						|| instruction.value_operation.source_opcode
							!= ZEND_RETURN
						|| (source.kind
								== ZEND_MIR_SOURCE_OPERAND_LITERAL
							? ((returned_id =
									zend_mir_value_from_synthetic(
										source.index)),
								!zend_mir_id_is_valid(returned_id))
							: ((source.kind
										!= ZEND_MIR_SOURCE_OPERAND_SLOT
									&& source.kind
										!= ZEND_MIR_SOURCE_OPERAND_SSA)
								|| source.ssa_variable_id
									== ZEND_MIR_ID_INVALID
								|| ((returned_id =
										zend_mir_value_from_original_ssa(
											source.ssa_variable_id)),
									!zend_mir_id_is_valid(returned_id))))) {
					return fail_inline();
				}
				if (scalar_diamond) {
					if (saw_return
							|| static_cast<uint32_t>(instruction_block)
								!= diamond.merge) {
						return fail_inline();
					}
					deferred_return_id = returned_id;
					deferred_return_binding =
						instruction.source_op1_binding;
					deferred_return_has_binding = true;
					saw_return = true;
				} else {
					returned = mapped_value(returned_id);
					returned_checked_step = returned == INVALID_VALUE_REF
						? checked_step_for_value(returned_id) : -1;
					if (returned_checked_step < 0) {
						returned_checked_step = checked_step_for_binding(
							instruction.source_op1_binding);
					}
					saw_return = returned != INVALID_VALUE_REF
						|| returned_checked_step >= 0;
					if (!saw_return) {
						return fail_inline();
					}
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
				if (saw_return || instruction.operand_count != 1
						|| (scalar_diamond
							&& static_cast<uint32_t>(instruction_block)
								!= diamond.merge)) {
					return fail_inline();
				}
				const zend_mir_value_id returned_id =
					zend_tpde_operand_at(callee, &instruction, 0);
				if (scalar_diamond) {
					deferred_return_id = returned_id;
					saw_return = true;
				} else {
					returned = mapped_value(returned_id);
					returned_checked_step = returned == INVALID_VALUE_REF
						? checked_step_for_value(returned_id) : -1;
					saw_return = returned != INVALID_VALUE_REF
						|| returned_checked_step >= 0;
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					|| record.opcode
						== ZEND_MIR_OPCODE_CANONICALIZE) {
				if (!zend_mir_id_is_valid(record.result_id)
						|| instruction.operand_count != 1) {
					return fail_inline();
				}
				const int32_t result_index =
					zend_tpde_value_index(callee, record.result_id);
				const zend_mir_value_id input_id = zend_tpde_operand_at(
					callee, &instruction, 0);
				const IRValueRef input = mapped_value(input_id);
				const int32_t input_checked_step = input == INVALID_VALUE_REF
					? checked_step_for_value(input_id) : -1;
				if (result_index < 0
						|| (input == INVALID_VALUE_REF
							&& input_checked_step < 0)
						|| (input != INVALID_VALUE_REF
							&& exact_type(input)
								!= callee->values[result_index].exact_type)) {
					return fail_inline();
				}
				if (input_checked_step >= 0) {
					checked_value_steps[
						static_cast<uint32_t>(result_index)] =
						input_checked_step;
				} else {
					values[static_cast<uint32_t>(result_index)] = input;
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
				const zend_mir_executable_value_ref &operation =
					instruction.value_operation;
				zend_mir_value_id result_id = ZEND_MIR_ID_INVALID;
				if (operation.result.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
					result_id = zend_mir_value_from_synthetic(
						operation.result.index);
				} else if ((operation.result.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| operation.result.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& operation.result.ssa_variable_id
							!= ZEND_MIR_ID_INVALID) {
					result_id = zend_mir_value_from_original_ssa(
						operation.result.ssa_variable_id);
				}
				if (scalar_diamond || checked_steps.size() >= 8
						|| caller_result == INVALID_VALUE_REF
						|| !instruction.has_value_operation
						|| (operation.source_opcode != ZEND_ADD
							&& operation.source_opcode != ZEND_SUB)
						|| !zend_mir_id_is_valid(result_id)
						|| call.direct_call->result_type
							!= ZEND_MIR_SCALAR_TYPE_I64) {
					return fail_inline();
				}
				const int32_t result_index =
					zend_tpde_value_index(callee, result_id);
				const zend_mir_value_id left_id =
					source_value_id(operation.op1);
				const zend_mir_value_id right_id =
					source_value_id(operation.op2);
				const int32_t left_checked_step =
					zend_mir_id_is_valid(left_id)
						? checked_step_for_value(left_id) : -1;
				const int32_t right_checked_step =
					zend_mir_id_is_valid(right_id)
						? checked_step_for_value(right_id) : -1;
				const int32_t bound_left_checked_step =
					left_checked_step >= 0 ? left_checked_step
						: checked_step_for_binding(
							instruction.source_op1_binding);
				const int32_t bound_right_checked_step =
					right_checked_step >= 0 ? right_checked_step
						: checked_step_for_binding(
							instruction.source_op2_binding);
				const IRValueRef left = bound_left_checked_step < 0
					? mapped_source_operand(operation.op1)
					: INVALID_VALUE_REF;
				const IRValueRef right = bound_right_checked_step < 0
					? mapped_source_operand(operation.op2)
					: INVALID_VALUE_REF;
				if (result_index < 0
						|| (bound_left_checked_step < 0
							&& (left == INVALID_VALUE_REF
								|| exact_type(left)
									!= ZEND_MIR_SCALAR_TYPE_I64))
						|| (bound_right_checked_step < 0
							&& (right == INVALID_VALUE_REF
								|| exact_type(right)
									!= ZEND_MIR_SCALAR_TYPE_I64))) {
					return fail_inline();
				}
				if (checked_steps.empty()) {
					if (bound_left_checked_step >= 0
							|| bound_right_checked_step >= 0) {
						return fail_inline();
					}
					checked_left = left;
					checked_right = right;
					checked_source_opcode = operation.source_opcode;
					checked_operands.push_back(left);
					checked_operands.push_back(right);
					checked_steps.push_back(
						{operation.source_opcode, true});
				} else {
					const int32_t accumulator_step =
						static_cast<int32_t>(checked_steps.size() - 1);
					const bool accumulator_is_left =
						bound_left_checked_step == accumulator_step
						&& bound_right_checked_step < 0;
					const bool accumulator_is_right =
						bound_right_checked_step == accumulator_step
						&& bound_left_checked_step < 0;
					if (!accumulator_is_left && !accumulator_is_right) {
						return fail_inline();
					}
					checked_operands.push_back(
						accumulator_is_left ? right : left);
					checked_steps.push_back({operation.source_opcode,
						accumulator_is_left});
				}
				const int32_t checked_step =
					static_cast<int32_t>(checked_steps.size() - 1);
				record_checked_step(result_index, checked_step);
				record_checked_step(
					instruction.source_result_binding.value_index,
					checked_step);
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP) {
				const zend_mir_executable_value_ref &operation =
					instruction.value_operation;
				zend_mir_value_id result_id = ZEND_MIR_ID_INVALID;
				if (operation.result.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
					result_id = zend_mir_value_from_synthetic(
						operation.result.index);
				} else if ((operation.result.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| operation.result.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& operation.result.ssa_variable_id
							!= ZEND_MIR_ID_INVALID) {
					result_id = zend_mir_value_from_original_ssa(
						operation.result.ssa_variable_id);
				}
				if (scalar_diamond
						|| checked_source_opcode != UINT32_MAX
						|| !instruction.has_value_operation
						|| operation.source_opcode != ZEND_STRLEN
						|| !zend_mir_id_is_valid(result_id)
						|| call.direct_call->result_type
							!= ZEND_MIR_SCALAR_TYPE_I64) {
					return fail_inline();
				}
				const int32_t result_index =
					zend_tpde_value_index(callee, result_id);
				const IRValueRef input =
					mapped_source_operand(operation.op1);
				if (result_index < 0
						|| input == INVALID_VALUE_REF
						|| machine_kind(input)
							!= ZEND_TPDE_MACHINE_VALUE_STRING_PTR
						|| !machine_value_is_register_authoritative(input)
						|| !machine_value_has_register_definition(input)) {
					return fail_inline();
				}
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(input);
				const IRValueRef result = add_derived_value(
					ZEND_MIR_REPRESENTATION_I64,
					ZEND_MIR_SCALAR_TYPE_I64,
					ZEND_MIR_ID_INVALID, false, 0,
					ZEND_TPDE_MACHINE_VALUE_I64);
				if (result == INVALID_VALUE_REF) {
					return fail_inline();
				}
				values[static_cast<uint32_t>(result_index)] = result;
				add_node(block_instructions, caller_block, InstNode{
					InstKind::StringLengthValue, UINT32_MAX, UINT32_MAX,
					result, {}, operand_offset, 1, true,
					ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_I64,
					true, record});
				cloned_string_length = true;
				continue;
			}
			const bool integer_or_boolean_opcode =
				(record.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
					&& record.opcode
						<= ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW)
				|| (record.opcode >= ZEND_MIR_OPCODE_I64_BIT_OR
					&& record.opcode <= ZEND_MIR_OPCODE_I64_CMP)
				|| record.opcode == ZEND_MIR_OPCODE_I1_NOT
				|| record.opcode == ZEND_MIR_OPCODE_I1_XOR
				|| record.opcode == ZEND_MIR_OPCODE_I1_EQ
				|| record.opcode == ZEND_MIR_OPCODE_I64_TO_I1
				|| record.opcode == ZEND_MIR_OPCODE_I1_TO_I64;
			if (checked_source_opcode != UINT32_MAX
					|| !integer_or_boolean_opcode
					|| record.effects != 0
					|| !zend_mir_id_is_valid(record.result_id)
					|| instruction.operand_count > 2) {
				return fail_inline();
			}
			const int32_t result_index =
				zend_tpde_value_index(callee, record.result_id);
			if (result_index < 0
					|| !zend_mir_scalar_type_is_exact(
						callee->values[result_index].exact_type)
					|| callee->values[result_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_NULL
					|| callee->values[result_index].exact_type
						== ZEND_MIR_SCALAR_TYPE_F64) {
				return fail_inline();
			}
			const uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			for (uint32_t operand_index = 0;
					operand_index < instruction.operand_count;
					++operand_index) {
				const IRValueRef operand = mapped_value(
					zend_tpde_operand_at(
						callee, &instruction, operand_index));
				if (operand == INVALID_VALUE_REF) {
					return fail_inline();
				}
				operands_.push_back(operand);
			}
			const IRValueRef result = add_derived_value(
				callee->values[result_index].representation,
				callee->values[result_index].exact_type,
				ZEND_MIR_ID_INVALID);
			if (result == INVALID_VALUE_REF) {
				return fail_inline();
			}
			values[static_cast<uint32_t>(result_index)] = result;
			add_node(block_instructions, caller_block, InstNode{
				InstKind::MIR, UINT32_MAX, UINT32_MAX, result, {},
				operand_offset, instruction.operand_count, true,
				ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
				true, record});
		}
		if (scalar_diamond) {
			if (diamond_condition == INVALID_VALUE_REF
					|| pending_phis.empty()) {
				return fail_inline();
			}
			const uint32_t predecessor_begin =
				callee->block_predecessor_offsets[diamond.merge];
			for (uint32_t instruction_index : pending_phis) {
				const zend_tpde_instruction &instruction =
					callee->instructions[instruction_index];
				const zend_mir_instruction_record record =
					zend_tpde_instruction_record_at(callee, &instruction);
				const int32_t result_index =
					zend_tpde_value_index(callee, record.result_id);
				if (result_index < 0 || instruction.operand_count != 2) {
					return fail_inline();
				}
				IRValueRef true_value = INVALID_VALUE_REF;
				IRValueRef false_value = INVALID_VALUE_REF;
				for (uint32_t operand = 0; operand < 2; ++operand) {
					const uint32_t predecessor = callee->block_predecessors[
						predecessor_begin + operand];
					const IRValueRef input = mapped_value(
						zend_tpde_operand_at(callee, &instruction, operand));
					if (input == INVALID_VALUE_REF) {
						return fail_inline();
					}
					if (predecessor == diamond.true_block) {
						true_value = input;
					} else if (predecessor == diamond.false_block) {
						false_value = input;
					} else {
						return fail_inline();
					}
				}
				const zend_tpde_value &callee_result =
					callee->values[static_cast<uint32_t>(result_index)];
				if (true_value == INVALID_VALUE_REF
						|| false_value == INVALID_VALUE_REF
						|| exact_type(true_value) != callee_result.exact_type
						|| exact_type(false_value) != callee_result.exact_type
						|| representation(true_value)
							!= callee_result.representation
						|| representation(false_value)
							!= callee_result.representation
						|| machine_kind(true_value) != callee_result.machine_kind
						|| machine_kind(false_value)
							!= callee_result.machine_kind
						|| (callee_result.machine_kind
								!= ZEND_TPDE_MACHINE_VALUE_I64
							&& callee_result.machine_kind
								!= ZEND_TPDE_MACHINE_VALUE_BOOL)) {
					return fail_inline();
				}
				const IRValueRef result = add_derived_value(
					callee_result.representation, callee_result.exact_type,
					ZEND_MIR_ID_INVALID, false, 0,
					callee_result.machine_kind);
				if (result == INVALID_VALUE_REF) {
					return fail_inline();
				}
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(diamond_condition);
				operands_.push_back(true_value);
				operands_.push_back(false_value);
				values[static_cast<uint32_t>(result_index)] = result;
				add_node(block_instructions, caller_block, InstNode{
					InstKind::ScalarSelect, UINT32_MAX, UINT32_MAX,
					result, {}, operand_offset, 3, true,
					ZEND_MIR_ID_INVALID, callee_result.exact_type,
					true, record});
			}
			returned = mapped_value(deferred_return_id);
			if (returned == INVALID_VALUE_REF
					&& deferred_return_has_binding
					&& deferred_return_binding.value_index >= 0
					&& static_cast<uint32_t>(
						deferred_return_binding.value_index)
						< callee->value_count) {
				returned = mapped_value(callee->values[static_cast<uint32_t>(
					deferred_return_binding.value_index)].id);
			}
		}
		if (!saw_return
				|| (returned == INVALID_VALUE_REF
					&& returned_checked_step < 0)
				|| (returned != INVALID_VALUE_REF
					&& exact_type(returned)
						!= call.direct_call->result_type)) {
			return fail_inline();
		}
		if (!checked_steps.empty()) {
			if (returned != INVALID_VALUE_REF
					|| returned_checked_step
						!= static_cast<int32_t>(checked_steps.size() - 1)) {
				return fail_inline();
			}
			InlinedBody body;
			body.valid = true;
			body.value = caller_result;
			body.checked_left = checked_left;
			body.checked_right = checked_right;
			body.checked_source_opcode = checked_source_opcode;
			body.checked_operands = std::move(checked_operands);
			body.checked_steps = std::move(checked_steps);
			return body;
		}
		if (cloned_string_length) {
			/*
			 * The cloned unary operation already owns a fresh SSA result.
			 * Feeding it directly to CALL_DIRECT_USER gives that assignment
			 * exactly one consumer.  The extra path-separating copy below is
			 * only needed when the fast result aliases an existing caller
			 * value also observed by the materialized call path.
			 */
			return {true, returned};
		}
		/*
		 * Keep the inlined fast result distinct from the same caller value
		 * consumed by the materialized observer path.  TPDE then owns one
		 * ordinary SSA use on each side instead of two path-sensitive uses of
		 * a single ValueAssignment hidden inside CALL_DIRECT_USER.
		 */
		const uint32_t result_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(returned);
		const IRValueRef inline_result = add_derived_value(
			representation(returned), exact_type(returned),
			ZEND_MIR_ID_INVALID);
		if (inline_result == INVALID_VALUE_REF) {
			return fail_inline();
		}
		zend_mir_instruction_record copy_record{};
		copy_record.opcode = ZEND_MIR_OPCODE_COPY;
		copy_record.block_id = callee->block_ids[0];
		add_node(block_instructions, caller_block, InstNode{
			InstKind::MIR, UINT32_MAX, UINT32_MAX, inline_result, {},
			result_operand_offset, 1, true,
			ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
			true, copy_record});
		return {true, inline_result};
	}

public:
	static TypedBodyAbiType typed_body_plan_abi(
			const zend_tpde_local_abi_type &type) {
		return {
			type.representation,
			type.exact_type,
			type.machine_kind,
			type.valid,
			type.transfer,
		};
	}

	static TypedBodyAbiType typed_body_value_abi(
			const zend_tpde_plan *plan, uint32_t value_index) {
		if (plan == nullptr || value_index >= plan->value_count) {
			return {};
		}
		return typed_body_plan_abi(plan->values[value_index].local_abi);
	}

	static bool typed_body_signature(
			const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans,
			std::span<const uint8_t> typed_body_candidates,
			TypedBodyAbiType *return_type) {
		if (plan == nullptr || return_type == nullptr
				|| plan->generator_resume_count != 0
				|| plan->user_opcode_callbacks
				|| (plan->argument_count != 0
					&& plan->argument_value_indices == nullptr)) {
			return false;
		}
		for (uint32_t argument = 0;
				argument < plan->argument_count; ++argument) {
			const int32_t value_index =
				plan->argument_value_indices[argument];
			if (value_index < 0
					|| static_cast<uint32_t>(value_index)
						>= plan->value_count
					|| !typed_body_value_abi(plan,
						static_cast<uint32_t>(value_index)).valid) {
				return false;
			}
		}

		TypedBodyAbiType result_type{};
		std::vector<TypedBodyAbiType> call_result_types(plan->value_count);
		std::vector<TypedBodyAbiType> instruction_result_types(
			plan->instruction_count);
		std::vector<TypedBodyAbiType> register_source_ssa(
			plan->source_ssa_variable_count);
		bool saw_return = false;
		for (uint32_t index = 0;
				index < plan->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				plan->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(plan, &instruction);
			if (instruction.local_abi_transport) {
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
				const zend_tpde_plan *callee =
					instruction.component_target_index
							< component_plans.size()
						? component_plans[
							instruction.component_target_index]
						: nullptr;
				if (instruction.direct_call == nullptr
						|| instruction.component_target_index
							>= component_plans.size()
						|| instruction.component_target_index
							>= typed_body_candidates.size()
						|| typed_body_candidates[
							instruction.component_target_index] == 0
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
					if (!zend_tpde_call_argument_at(plan,
							instruction.call_argument_offset + argument,
							&source_argument)
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
						plan->call_argument_bindings[
							instruction.call_argument_offset + argument];
					const IRValueRef caller_ref =
						caller_binding.value_index < 0
							? INVALID_VALUE_REF
							: IRValueRef{MIR_VALUE_BASE
								+ static_cast<uint32_t>(
									caller_binding.value_index)};
					const uint32_t caller_ref_index =
						static_cast<uint32_t>(caller_ref);
					TypedBodyAbiType caller_abi =
						caller_ref != INVALID_VALUE_REF
								&& caller_ref_index >= MIR_VALUE_BASE
								&& caller_ref_index - MIR_VALUE_BASE
									< plan->value_count
							? typed_body_value_abi(plan,
								caller_ref_index - MIR_VALUE_BASE)
							: TypedBodyAbiType{};
					const zend_mir_value_id caller_ssa =
						source_argument.source_operand.ssa_variable_id;
					if (!caller_abi.valid
							&& caller_binding
									.definition_instruction_index >= 0
							&& static_cast<uint32_t>(
								caller_binding
									.definition_instruction_index)
								< instruction_result_types.size()) {
						caller_abi = instruction_result_types[
							static_cast<uint32_t>(
								caller_binding
									.definition_instruction_index)];
					}
					if (!caller_abi.valid
							&& caller_ssa < register_source_ssa.size()) {
						caller_abi = register_source_ssa[caller_ssa];
					}
					const TypedBodyAbiType callee_abi =
						typed_body_value_abi(
							callee, static_cast<uint32_t>(callee_value));
					const bool by_reference =
						source_argument.ownership
							== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE;
						if (!typed_body_call_argument_can_supply(
								plan, source_argument, caller_abi, callee_abi)
								|| by_reference
									!= (callee_abi.machine_kind
										== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
							return false;
						}
				}
				const IRValueRef call_result =
					plan_source_operand_value_ref(
						plan, instruction.direct_call->result_operand);
				const uint32_t call_result_index =
					static_cast<uint32_t>(call_result);
				const zend_mir_value_id call_result_ssa =
					instruction.direct_call->result_operand
						.ssa_variable_id;
				TypedBodyAbiType call_result_type =
					typed_body_plan_abi(callee->return_abi);
				if (!call_result_type.valid
						&& call_result != INVALID_VALUE_REF
						&& call_result_index >= MIR_VALUE_BASE
						&& call_result_index - MIR_VALUE_BASE
							< plan->value_count) {
					call_result_type = typed_body_value_abi(
						plan, call_result_index - MIR_VALUE_BASE);
				} else if (!call_result_type.valid
						&& zend_mir_scalar_type_is_exact(
						instruction.direct_call->result_type)
						&& instruction.direct_call->result_type
							!= ZEND_MIR_SCALAR_TYPE_NULL) {
					call_result_type = {
						instruction.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_I1
							? ZEND_MIR_REPRESENTATION_I1
						: instruction.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_F64
							? ZEND_MIR_REPRESENTATION_DOUBLE
							: ZEND_MIR_REPRESENTATION_I64,
						instruction.direct_call->result_type,
						instruction.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_I1
							? ZEND_TPDE_MACHINE_VALUE_BOOL
						: instruction.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_F64
							? ZEND_TPDE_MACHINE_VALUE_F64
							: ZEND_TPDE_MACHINE_VALUE_I64,
						true,
					};
				}
					if (!call_result_type.valid) {
						return false;
					}
				instruction_result_types[index] = call_result_type;
				if (call_result != INVALID_VALUE_REF
						&& call_result_index >= MIR_VALUE_BASE
						&& call_result_index - MIR_VALUE_BASE
							< call_result_types.size()) {
					call_result_types[
						call_result_index - MIR_VALUE_BASE] =
							call_result_type;
				} else if (call_result_ssa
						< register_source_ssa.size()) {
					register_source_ssa[call_result_ssa] =
						call_result_type;
				} else if (instruction.source_result_binding
								.definition_instruction_index
							== static_cast<int32_t>(index)) {
					/* The result remains a private local-ABI value. */
					} else {
						return false;
					}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
				/*
				 * The observable Zend entry may still need its helper for values
				 * arriving through the boxed ABI.  A private typed body may elide
				 * that helper only when its incoming value already has exactly the
				 * declared return ABI.
				 */
				TypedBodyAbiType verified_type{};
				const zend_tpde_source_value_binding verified_binding =
					instruction.source_op1_binding;
				if (verified_binding
							.definition_instruction_index >= 0
						&& static_cast<uint32_t>(verified_binding
							.definition_instruction_index)
							< instruction_result_types.size()) {
					verified_type = instruction_result_types[
						static_cast<uint32_t>(verified_binding
							.definition_instruction_index)];
				}
				const zend_mir_value_id verified_ssa =
					instruction.value_operation.op1.ssa_variable_id;
				if (!verified_type.valid
						&& verified_ssa < register_source_ssa.size()) {
					verified_type = register_source_ssa[verified_ssa];
				}
				if (!verified_type.valid
						&& verified_binding.value_index >= 0
						&& static_cast<uint32_t>(
							verified_binding.value_index)
							< plan->value_count) {
					verified_type = typed_body_value_abi(
						plan, static_cast<uint32_t>(
							verified_binding.value_index));
				}
				if (!verified_type.same_shape(
						typed_body_plan_abi(plan->return_abi))) {
					return false;
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK) {
				const ScalarTypeCheckSelection selection =
					scalar_type_check_selection(plan, instruction);
				if (selection == ScalarTypeCheckSelection::Invalid) {
					return false;
				}
				const zend_mir_value_id result_ssa =
					instruction.value_operation.result.ssa_variable_id;
				if (result_ssa >= register_source_ssa.size()) {
					return false;
				}
				register_source_ssa[result_ssa] =
					{ZEND_MIR_REPRESENTATION_I1,
						ZEND_MIR_SCALAR_TYPE_I1,
						ZEND_TPDE_MACHINE_VALUE_BOOL, true};
				continue;
			}
			/*
			 * Echo is an observability boundary: its helper consumes the
			 * active Zend frame and may invoke output handlers.  Keep the
			 * private typed body reserved for operations whose complete ABI
			 * is represented by its explicit TPDE arguments.
			 */
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
				const IRValueRef condition =
					plan_source_operand_value_ref(
						plan, instruction.value_operation.op1);
				const uint32_t condition_index =
					static_cast<uint32_t>(condition);
				const zend_mir_value_id source_ssa =
					instruction.value_operation.op1.ssa_variable_id;
				const bool source_override =
					source_ssa < register_source_ssa.size()
						&& register_source_ssa[source_ssa].valid
						&& register_source_ssa[source_ssa].exact_type
							== ZEND_MIR_SCALAR_TYPE_I1;
				if (!source_override
						&& (condition == INVALID_VALUE_REF
							|| condition_index < MIR_VALUE_BASE
							|| condition_index - MIR_VALUE_BASE
								>= plan->value_count
							|| plan->values[
								condition_index - MIR_VALUE_BASE].exact_type
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
			IRValueRef returned = INVALID_VALUE_REF;
			TypedBodyAbiType returned_source_type{};
			if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
				if (instruction.operand_count != 1) {
					return false;
				}
				const int32_t value_index = zend_tpde_value_index(
					plan, zend_tpde_operand_at(plan, &instruction, 0));
				if (value_index >= 0) {
					returned = IRValueRef{
						MIR_VALUE_BASE
							+ static_cast<uint32_t>(value_index)};
				}
			} else if (record.opcode
					== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
					if (!instruction.has_value_operation
							|| instruction.value_operation.source_opcode
								!= ZEND_RETURN) {
						return false;
					}
				const zend_tpde_source_value_binding returned_binding =
					instruction.source_op1_binding;
				returned = returned_binding.value_index < 0
					? INVALID_VALUE_REF
					: IRValueRef{MIR_VALUE_BASE
						+ static_cast<uint32_t>(
							returned_binding.value_index)};
				const zend_mir_value_id returned_ssa =
					instruction.value_operation.op1.ssa_variable_id;
				if (returned_binding
								.definition_instruction_index >= 0
						&& static_cast<uint32_t>(
							returned_binding
								.definition_instruction_index)
							< instruction_result_types.size()) {
					returned_source_type = instruction_result_types[
						static_cast<uint32_t>(
							returned_binding
								.definition_instruction_index)];
				}
				if (!returned_source_type.valid
						&& returned_ssa < register_source_ssa.size()) {
					returned_source_type =
						register_source_ssa[returned_ssa];
				}
			} else {
				const bool pure =
					record.opcode == ZEND_MIR_OPCODE_CONSTANT
					|| record.opcode == ZEND_MIR_OPCODE_PHI
					|| record.opcode == ZEND_MIR_OPCODE_COPY
					|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
					|| record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					|| record.opcode == ZEND_MIR_OPCODE_BRANCH
					|| record.opcode == ZEND_MIR_OPCODE_COND_BRANCH
					|| record.opcode == ZEND_MIR_OPCODE_UNREACHABLE
					|| (record.opcode
							>= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
						&& record.opcode
							<= ZEND_MIR_OPCODE_SCALAR_DROP);
				if (!pure
						|| (record.representation
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& record.result_id != ZEND_MIR_ID_INVALID
						&& zend_tpde_value_index(
							plan, record.result_id) >= 0
						&& !typed_body_value_abi(plan,
							static_cast<uint32_t>(
								zend_tpde_value_index(
									plan, record.result_id))).valid
						&& !(record.opcode
							== ZEND_MIR_OPCODE_CONSTANT
								&& plan->values[zend_tpde_value_index(
									plan, record.result_id)].exact_type
								== ZEND_MIR_SCALAR_TYPE_NULL))) {
						return false;
					}
				continue;
			}
			const uint32_t returned_index =
				static_cast<uint32_t>(returned);
				if (returned == INVALID_VALUE_REF
						&& !returned_source_type.valid) {
					return false;
				}
				if (returned != INVALID_VALUE_REF
						&& (returned_index < MIR_VALUE_BASE
							|| returned_index - MIR_VALUE_BASE
								>= plan->value_count)) {
					return false;
				}
			TypedBodyAbiType returned_type =
				returned_source_type.valid
					? returned_source_type
					: typed_body_value_abi(
						plan, returned_index - MIR_VALUE_BASE);
			if (returned != INVALID_VALUE_REF
					&& !returned_type.valid) {
				returned_type = call_result_types[
					returned_index - MIR_VALUE_BASE];
			}
			if (!returned_type.valid
					|| (saw_return
						&& !result_type.same_shape(returned_type))) {
				return false;
			}
			result_type = returned_type;
			saw_return = true;
		}
		if (!saw_return) {
			return false;
		}
		const TypedBodyAbiType declared_return =
			typed_body_plan_abi(plan->return_abi);
		if (!declared_return.valid
				|| !result_type.same_shape(declared_return)) {
			return false;
		}
		*return_type = result_type;
		return true;
	}

	explicit ZendIRAdaptor(const zend_tpde_plan *plan)
		: ZendIRAdaptor(plan,
			std::span<const zend_tpde_plan *const>{&plan, 1},
			FunctionMode::ZendEntry) {}

	explicit ZendIRAdaptor(const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans,
			FunctionMode function_mode = FunctionMode::ZendEntry)
		: plan_(plan), component_plans_(component_plans),
		  function_mode_(function_mode) {
		typed_body_value_overrides_.assign(
			plan_ == nullptr ? 0 : plan_->value_count,
			INVALID_VALUE_REF);
		typed_body_source_ssa_overrides_.assign(
			plan_ == nullptr ? 0 : plan_->source_ssa_variable_count,
			INVALID_VALUE_REF);
		typed_body_instruction_results_.assign(
			plan_ == nullptr ? 0 : plan_->instruction_count,
			INVALID_VALUE_REF);
		component_value_overrides_.assign(
			plan_ == nullptr ? 0 : plan_->value_count,
			INVALID_VALUE_REF);
		component_source_ssa_overrides_.assign(
			plan_ == nullptr ? 0 : plan_->source_ssa_variable_count,
			INVALID_VALUE_REF);
		component_instruction_results_.assign(
			plan_ == nullptr ? 0 : plan_->instruction_count,
			INVALID_VALUE_REF);
		if (function_mode_ == FunctionMode::ZendEntry) {
			arguments_.push_back(IRValueRef{EXECUTE_DATA_VALUE});
			arguments_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		} else {
			if (plan_ == nullptr || !plan_->typed_body_eligible
					|| !plan_->typed_body_return_abi.valid) {
				valid_ = false;
				return;
			}
			arguments_.reserve(plan_->argument_count);
			for (uint32_t argument = 0;
					argument < plan_->argument_count; ++argument) {
				arguments_.push_back(IRValueRef{
					MIR_VALUE_BASE + static_cast<uint32_t>(
						plan_->argument_value_indices[argument])});
			}
			for (uint32_t value = 0;
					value < plan_->value_count; ++value) {
				const int32_t alias =
					plan_->values[value].register_alias_value_index;
				if (alias >= 0
						&& static_cast<uint32_t>(alias)
							< plan_->value_count) {
					typed_body_value_overrides_[value] = IRValueRef{
						MIR_VALUE_BASE + static_cast<uint32_t>(alias)};
				}
			}
		}
		std::vector<BlockItem<IRInstRef>> block_instructions;
		std::vector<BlockItem<IRValueRef>> block_phis;
		std::vector<uint8_t> generator_resume_emitted;
		std::vector<uint8_t> source_landing_emitted;
		std::vector<uint32_t> source_landing_blocks;
		std::vector<uint32_t> source_block_next;
		const zend_tpde_machine_cfg &machine_cfg =
			function_mode_ == FunctionMode::TypedBody
				? plan_->typed_body_machine_cfg
				: plan_->entry_machine_cfg;
		const uint32_t *boxed_cond_cold_blocks =
			machine_cfg.boxed_cond_cold_blocks;
		const uint32_t *boxed_cond_cold_by_predecessor =
			machine_cfg.boxed_cond_cold_by_predecessor;
		const uint32_t *instruction_blocks =
			machine_cfg.instruction_blocks;
		const uint32_t *guarded_cold_blocks =
			machine_cfg.guarded_cold_blocks;
		const uint32_t *guarded_hot_blocks =
			machine_cfg.guarded_hot_blocks;
		const uint32_t *guarded_continuation_blocks =
			machine_cfg.guarded_continuation_blocks;
		const uint32_t *final_blocks = machine_cfg.final_blocks;
		bool source_call_fragments = false;
		IRValueRef observers_enabled_reference = INVALID_VALUE_REF;
		if (function_mode_ == FunctionMode::ZendEntry
				&& plan_->observers_enabled_reference_index
					< plan_->machine_reference_count) {
			observers_enabled_reference = add_derived_value(
				ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
				ZEND_MIR_SCALAR_TYPE_NONE,
				ZEND_MIR_ID_INVALID, false, 0,
				UINT8_MAX, ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_UNKNOWN,
				plan_->observers_enabled_reference_index);
		}

		blocks_.reserve(plan_->block_count);
		block_instructions.reserve(plan_->instruction_count + plan_->value_count + 1);
		block_phis.reserve(plan_->value_count);
		phi_input_slices_.resize(MIR_VALUE_BASE + plan_->value_count);
		phi_values_.resize(MIR_VALUE_BASE + plan_->value_count);
		const uint32_t tpde_block_count = machine_cfg.block_count;
		if (tpde_block_count < plan_->block_count
				|| machine_cfg.successor_offsets == nullptr
				|| (plan_->instruction_count != 0
					&& (instruction_blocks == nullptr
						|| guarded_cold_blocks == nullptr
						|| guarded_hot_blocks == nullptr
						|| guarded_continuation_blocks == nullptr
						|| boxed_cond_cold_blocks == nullptr))
				|| (plan_->block_count != 0
					&& (final_blocks == nullptr
						|| boxed_cond_cold_by_predecessor == nullptr))) {
			valid_ = false;
			return;
		}
		blocks_.reserve(tpde_block_count);
		block_info_.resize(tpde_block_count);
		block_info2_.resize(tpde_block_count);
		for (uint32_t i = 0; i < tpde_block_count; ++i) {
			blocks_.push_back(IRBlockRef{i});
		}
		successor_slices_.resize(tpde_block_count);
		successors_.reserve(machine_cfg.successor_count);
		for (uint32_t block = 0; block < tpde_block_count; ++block) {
			const uint32_t begin =
				machine_cfg.successor_offsets[block];
			const uint32_t end =
				machine_cfg.successor_offsets[block + 1];
			if (begin > end || end > machine_cfg.successor_count) {
				valid_ = false;
				return;
			}
			successor_slices_[block] = {begin, end - begin};
		}
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			if (instruction_blocks[i] >= tpde_block_count
					|| (guarded_cold_blocks[i] != UINT32_MAX
						&& guarded_cold_blocks[i] >= tpde_block_count)
					|| (guarded_hot_blocks[i] != UINT32_MAX
						&& guarded_hot_blocks[i] >= tpde_block_count)
					|| (guarded_continuation_blocks[i] != UINT32_MAX
						&& guarded_continuation_blocks[i]
							>= tpde_block_count)
					|| (boxed_cond_cold_blocks[i] != UINT32_MAX
						&& boxed_cond_cold_blocks[i]
							>= tpde_block_count)) {
				valid_ = false;
				return;
			}
		}
		for (uint32_t block = 0; block < plan_->block_count; ++block) {
			if (final_blocks[block] >= tpde_block_count
					|| (boxed_cond_cold_by_predecessor[block]
							!= UINT32_MAX
						&& boxed_cond_cold_by_predecessor[block]
							>= tpde_block_count)) {
				valid_ = false;
				return;
			}
		}
		const int32_t entry = block_index(plan_->function.entry_block_id);
		if (entry < 0) {
			valid_ = false;
			return;
		}
		for (uint32_t edge = 0;
				edge < machine_cfg.successor_count; ++edge) {
			if (machine_cfg.successors == nullptr
					|| machine_cfg.successors[edge]
						>= tpde_block_count) {
				valid_ = false;
				return;
			}
			successors_.push_back(
				IRBlockRef{machine_cfg.successors[edge]});
		}
		std::vector<uint8_t> machine_block_reachable(tpde_block_count, 0);
		if (tpde_block_count != 0) {
			std::vector<uint32_t> pending{
				static_cast<uint32_t>(entry)};
			machine_block_reachable[static_cast<uint32_t>(entry)] = 1;
			while (!pending.empty()) {
				const uint32_t block = pending.back();
				pending.pop_back();
				const uint32_t begin = machine_cfg.successor_offsets[block];
				const uint32_t end = machine_cfg.successor_offsets[block + 1];
				for (uint32_t edge = begin; edge < end; ++edge) {
					const uint32_t successor = machine_cfg.successors[edge];
					if (machine_block_reachable[successor] == 0) {
						machine_block_reachable[successor] = 1;
						pending.push_back(successor);
					}
				}
			}
		}
		auto machine_block_reachable_without = [&](uint32_t target,
				uint32_t excluded) {
			if (target >= tpde_block_count
					|| excluded == static_cast<uint32_t>(entry)) {
				return false;
			}
			if (excluded == UINT32_MAX) {
				return machine_block_reachable[target] != 0;
			}
			std::vector<uint8_t> visited(tpde_block_count);
			std::vector<uint32_t> pending{
				static_cast<uint32_t>(entry)};
			visited[static_cast<uint32_t>(entry)] = 1;
			while (!pending.empty()) {
				const uint32_t block = pending.back();
				pending.pop_back();
				if (block == target) {
					return true;
				}
				const uint32_t begin = machine_cfg.successor_offsets[block];
				const uint32_t end = machine_cfg.successor_offsets[block + 1];
				for (uint32_t edge = begin; edge < end; ++edge) {
					const uint32_t successor = machine_cfg.successors[edge];
					if (successor != excluded && visited[successor] == 0) {
						visited[successor] = 1;
						pending.push_back(successor);
					}
				}
			}
			return false;
		};
		auto machine_block_dominates = [&](uint32_t dominator,
				uint32_t block) {
			if (dominator >= tpde_block_count || block >= tpde_block_count) {
				return false;
			}
			if (dominator == block) {
				return true;
			}
			if (!machine_block_reachable_without(block, UINT32_MAX)) {
				return false;
			}
			return !machine_block_reachable_without(block, dominator);
		};
		auto phased_source_call = [&](uint32_t instruction_index) {
			if (function_mode_ != FunctionMode::ZendEntry
					|| plan_->source_call_phase_count == 0
					|| instruction_index >= plan_->instruction_count) {
				return false;
			}
			const zend_tpde_instruction &instruction =
				plan_->instructions[instruction_index];
			const zend_mir_instruction_record record =
				instruction_record_at(instruction_index);
			return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.user_call != nullptr
				&& instruction.direct_call == nullptr
				&& instruction.user_call->do_opcode != ZEND_CALLABLE_CONVERT
				&& instruction.user_call->do_opcode
					!= ZEND_CALLABLE_CONVERT_PARTIAL;
		};
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			source_call_fragments =
				source_call_fragments
				|| (instruction.user_opcode_call_fragments
					&& !phased_source_call(i));
		}
		generator_resume_emitted.resize(
			plan_->generator_resume_count, 0);
		const bool source_landings =
			plan_->user_opcode_callbacks || source_call_fragments
				|| (function_mode_ == FunctionMode::ZendEntry
					&& plan_->source_call_phase_count != 0);
		if (source_landings && plan_->source_opcodes != nullptr) {
			source_landing_emitted.resize(plan_->source_opcode_count, 0);
			source_landing_blocks.resize(
				plan_->source_opcode_count, UINT32_MAX);
			if (plan_->user_opcode_callbacks) {
				user_opcode_next_landings_.resize(
					plan_->source_opcode_count, UINT32_MAX);
				user_opcode_result_reload_sources_.resize(
					plan_->source_opcode_count, 0);
			}
			if (plan_->source_opcode_block_indices == nullptr
					|| plan_->source_opcode_is_data == nullptr
					|| plan_->source_block_starts == nullptr
					|| plan_->source_block_ends == nullptr) {
				valid_ = false;
			} else {
				source_block_next.resize(
					plan_->source_block_count, UINT32_MAX);
				for (uint32_t instruction = 0;
						instruction < plan_->instruction_count;
						++instruction) {
					const zend_mir_instruction_record record =
						instruction_record_at(instruction);
					if (record.source_position_id
							>= plan_->source_opcode_count) {
						continue;
					}
					const uint32_t source_block =
						plan_->source_opcode_block_indices[
							record.source_position_id];
					const uint32_t mir_block =
						instruction_blocks[instruction];
					if (source_block >= plan_->source_block_count
							|| mir_block == UINT32_MAX) {
						valid_ = false;
						continue;
					}
					/*
					 * Lowering may split one Zend basic block into multiple MIR
					 * blocks.  Keep the first executable landing for each exact
					 * source position; gaps are assigned to the next executable
					 * position below so call INIT/SEND fragments run before the
					 * operation that consumes their state.
					 */
					if (source_landing_blocks[record.source_position_id]
							== UINT32_MAX) {
						source_landing_blocks[record.source_position_id] =
							mir_block;
					}
				}
				for (uint32_t source_block = 0;
						source_block < plan_->source_block_count;
						++source_block) {
					const uint32_t block_start =
						plan_->source_block_starts[source_block];
					const uint32_t block_end =
						plan_->source_block_ends[source_block];
					if (block_start == UINT32_MAX
							|| block_end == UINT32_MAX) {
						continue;
					}
					source_block_next[source_block] = block_start;
					uint32_t next_mir_block = UINT32_MAX;
					for (uint32_t source = block_end;
							source-- > block_start;) {
						if (plan_->source_opcode_is_data[source] != 0) {
							continue;
						}
						if (source_landing_blocks[source] != UINT32_MAX) {
							next_mir_block = source_landing_blocks[source];
						} else if (next_mir_block != UINT32_MAX) {
							source_landing_blocks[source] = next_mir_block;
						}
					}
					uint32_t previous_mir_block = UINT32_MAX;
					for (uint32_t source = block_start;
							source < block_end; ++source) {
						if (plan_->source_opcode_is_data[source] != 0) {
							continue;
						}
						if (source_landing_blocks[source] != UINT32_MAX) {
							previous_mir_block =
								source_landing_blocks[source];
						} else if (previous_mir_block != UINT32_MAX) {
							source_landing_blocks[source] =
								previous_mir_block;
						} else {
							valid_ = false;
						}
					}
				}
			}
			if (plan_->user_opcode_callbacks) {
				uint32_t next = UINT32_MAX;
				for (uint32_t source = plan_->source_opcode_count;
						source-- > 0;) {
					if (source_landing_blocks[source] != UINT32_MAX) {
						next = source;
					}
					user_opcode_next_landings_[source] = next;
				}
				for (uint32_t source = 0;
						source < plan_->source_opcode_count; ++source) {
					if (source_landing_blocks[source] == UINT32_MAX
							|| source
								>= plan_->user_opcode_source_operation_count) {
						continue;
					}
					user_opcode_dispatch_to_sources_.push_back(source);
				}
			}
		}
		if (function_mode_ == FunctionMode::ZendEntry) {
			operands_.push_back(IRValueRef{EXECUTE_DATA_VALUE});
			add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
				InstKind::LoadFrame,
				UINT32_MAX,
				UINT32_MAX,
				IRValueRef{FRAME_VALUE},
				{},
				0,
				1,
				true});
			if (plan_->generator_resume_count != 0) {
				uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(IRValueRef{FRAME_VALUE});
				operands_.push_back(
					IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
				add_node(block_instructions, static_cast<uint32_t>(entry),
					InstNode{
						InstKind::GeneratorGateway,
						UINT32_MAX,
						UINT32_MAX,
						INVALID_VALUE_REF,
						{},
						operand_offset,
						2,
						false});
			}
			uint32_t guard_operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
				InstKind::ZvalGuardArguments,
				UINT32_MAX,
				UINT32_MAX,
				INVALID_VALUE_REF,
				{},
				guard_operand_offset,
				1,
				false});
		}
		for (uint32_t i = 0; i < plan_->value_count; ++i) {
			const zend_tpde_value &value = plan_->values[i];
			const TypedBodyAbiType argument_abi =
				value.argument_index >= 0
					? typed_body_value_abi(plan_, i)
					: TypedBodyAbiType{};
			const bool exact_scalar =
				argument_abi.valid
				&& zend_mir_scalar_type_is_exact(
					argument_abi.exact_type)
				&& argument_abi.exact_type
					!= ZEND_MIR_SCALAR_TYPE_NULL;
			const bool unboxed_pointer =
				argument_abi.valid
				&& (argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR);
			const bool boxed_register_value =
				argument_abi.valid
				&& argument_abi.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
			if (value.argument_index < 0
					|| (!exact_scalar && !unboxed_pointer
						&& !boxed_register_value)) {
				continue;
			}
			if (function_mode_ == FunctionMode::TypedBody) {
				continue;
			}
			/*
			 * W11P supplies canonical Zend-frame locations explicitly.  The
			 * older W03-W10 scalar contracts predate that table, but their
			 * argument values are still defined by the stable frame argument
			 * ordinal.  Keep those compatibility entry points executable
			 * without weakening the W11P location contract.
			 */
			const zend_mir_storage_id storage_id =
				zend_mir_id_is_valid(
					value.canonical_storage_id)
				? value.canonical_storage_id
				: (plan_->value_model_flags
						& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0
					? static_cast<zend_mir_storage_id>(
						value.argument_index)
					: ZEND_MIR_ID_INVALID;
			if (!zend_mir_id_is_valid(storage_id)) {
				valid_ = false;
				continue;
			}
			if (!boxed_register_value) {
				argument_guards_.push_back({
					static_cast<uint32_t>(value.argument_index),
					storage_id,
					argument_abi.exact_type,
					argument_abi.machine_kind});
			}
			const uint32_t payload_reference = machine_reference_index(
				ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT, storage_id);
			if (payload_reference == UINT32_MAX) {
				valid_ = false;
				continue;
			}
			const IRValueRef payload_address = add_derived_value(
				ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
				ZEND_MIR_SCALAR_TYPE_NONE, storage_id, false, 0,
				UINT8_MAX, ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_UNKNOWN,
				payload_reference);
			if (payload_address == INVALID_VALUE_REF) {
				continue;
			}
			const uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(payload_address);
			add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
				InstKind::ZvalPayloadLoad,
				UINT32_MAX,
				static_cast<uint32_t>(value.argument_index),
				IRValueRef{MIR_VALUE_BASE + i},
				{},
				operand_offset,
				1,
				true,
				storage_id,
				argument_abi.exact_type});
		}

		/*
		 * A call always publishes its canonical result zval, but it only needs
		 * a second, register-resident scalar result when a later TPDE
		 * instruction or PHI consumes that scalar identity.  Compute those
		 * uses once for the complete function.  This keeps the construction
		 * linear and avoids manufacturing zero-reference ValueAssignments for
		 * calls whose result is observed exclusively through Zend storage.
		 */
			std::vector<uint8_t> machine_value_used(plan_->value_count);
			for (uint32_t value = 0;
					value < plan_->value_count; ++value) {
				machine_value_used[value] =
					machine_value_has_frozen_use(value);
			}
			std::vector<IRValueRef> transient_scalar_results(
				plan_->instruction_count, INVALID_VALUE_REF);
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				if (!instruction.transient_scalar_result) {
					continue;
				}
				transient_scalar_results[i] = add_derived_value(
					instruction.transient_result_representation,
					instruction.transient_result_exact_type,
					instruction.transient_result_storage_id,
					false, 0,
					instruction.transient_result_machine_kind,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_IMMORTAL);
				if (transient_scalar_results[i] == INVALID_VALUE_REF) {
					valid_ = false;
				}
			}
			/*
			 * Component-call results may feed PHIs that precede the call in MIR
			 * instruction order (most notably loop-carried direct-call
			 * results). Select their local-ABI machine identities before
			 * visiting any PHI. Otherwise the PHI captures the canonical plan
			 * value while the call later defines a derived override, leaving
			 * the captured value without a TPDE definition on the backedge.
			 */
			std::vector<IRValueRef> register_component_results(
				plan_->instruction_count, INVALID_VALUE_REF);
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				const bool register_component_call =
					record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& instruction.direct_call != nullptr
					&& (frozen_typed_component_call(i)
						|| frozen_effect_closed_inline(i));
				if (!register_component_call) {
					continue;
				}

				/* A full-DFA result may reuse the destination CV's canonical
				 * binding even though the source operand still carries the exact
				 * defining SSA version.  Prefer that definition so a loop PHI's
				 * result is not globally replaced by its backedge call result. */
				IRValueRef canonical_result =
					plan_source_operand_value_ref(
						plan_, instruction.direct_call->result_operand);
				if (canonical_result == INVALID_VALUE_REF) {
					canonical_result = source_binding_value_ref(
						instruction.source_result_binding);
				}
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				const zend_mir_value_id result_ssa =
					instruction.direct_call->result_operand
						.ssa_variable_id;
				TypedBodyAbiType result_abi =
					instruction.component_target_index
							< component_plans_.size()
						? typed_body_plan_abi(
							component_plans_[
								instruction.component_target_index]
								->return_abi)
						: TypedBodyAbiType{};
				if (canonical_result != INVALID_VALUE_REF
						&& canonical_index >= MIR_VALUE_BASE
						&& canonical_index - MIR_VALUE_BASE
							< plan_->value_count
						&& !result_abi.valid) {
					result_abi = typed_body_value_abi(
						plan_, canonical_index - MIR_VALUE_BASE);
				}
				if (!result_abi.valid
						&& zend_mir_scalar_type_is_exact(
							instruction.direct_call->result_type)
						&& instruction.direct_call->result_type
							!= ZEND_MIR_SCALAR_TYPE_NULL) {
					const zend_mir_scalar_type_mask exact_type =
						instruction.direct_call->result_type;
					result_abi = {
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
						true,
					};
				}
				if (!result_abi.valid) {
					valid_ = false;
					continue;
				}
				/*
				 * An exact null return has a valid local ABI, but no payload to
				 * carry in a register. Keep the canonical null value instead of
				 * manufacturing a register result that liveness deliberately
				 * ignores.
				 */
				if (result_abi.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
					continue;
				}

				const IRValueRef result = add_derived_value(
					result_abi.representation,
					result_abi.exact_type, ZEND_MIR_ID_INVALID,
					false, 0, result_abi.machine_kind,
					local_abi_ownership(
						result_abi.transfer,
						canonical_result != INVALID_VALUE_REF
							&& canonical_index >= MIR_VALUE_BASE
							&& canonical_index - MIR_VALUE_BASE
								< plan_->value_count
						? plan_->values[
							canonical_index - MIR_VALUE_BASE].ownership
						: ZEND_MIR_OWNERSHIP_STATE_OWNED),
					local_abi_refcount(
						result_abi.transfer,
						canonical_result != INVALID_VALUE_REF
							&& canonical_index >= MIR_VALUE_BASE
							&& canonical_index - MIR_VALUE_BASE
								< plan_->value_count
						? plan_->values[
							canonical_index - MIR_VALUE_BASE].refcount_state
						: ZEND_MIR_REFCOUNT_UNKNOWN));
				if (result == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				register_component_results[i] = result;
				active_instruction_results()[i] = result;
				if (canonical_result != INVALID_VALUE_REF
						&& canonical_index >= MIR_VALUE_BASE
						&& canonical_index - MIR_VALUE_BASE
							< active_value_overrides().size()) {
					active_value_overrides()[
						canonical_index - MIR_VALUE_BASE] = result;
				}
				if (result_ssa < active_source_ssa_overrides().size()) {
					active_source_ssa_overrides()[result_ssa] = result;
				} else if (canonical_result == INVALID_VALUE_REF) {
					valid_ = false;
				}
			}
			/*
			 * A source assignment can rename a private component-call result
			 * into a loop-carried CV.  The corresponding PHI precedes both the
			 * call and the assignment in MIR order, so select that rename now,
			 * while the call's local-ABI identity is already known.  Waiting
			 * until VALUE_ASSIGN is visited would leave the PHI backedge bound
			 * to the old canonical-slot value and every iteration would call
			 * the callee with the loop's initial argument.
			 *
			 * The assignment itself remains a real guarded value operation:
			 * both targets publish the register value to the canonical CV on
			 * the hot edge and materialize the source temporary before the
			 * generic cold helper.  This preselection only makes the new source
			 * SSA identity authoritative soon enough for PHI construction.
			 */
			std::vector<IRValueRef> preselected_phi_results(
				plan_->instruction_count, INVALID_VALUE_REF);
			std::vector<zend_mir_storage_id> early_pointer_phi_storages;
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				if (record.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN
						|| !instruction.has_value_operation
						|| instruction.value_operation.op1.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_CV
						|| guarded_cold_blocks[i] == UINT32_MAX) {
					continue;
				}
				const IRValueRef assigned = source_binding_value_ref(
					instruction.source_op2_binding);
				if (assigned == INVALID_VALUE_REF
						|| !machine_pointer_kind(machine_kind(assigned))
						|| !machine_value_is_register_authoritative(
							assigned)
						|| !machine_value_has_register_definition(
							assigned)) {
					continue;
				}
				const zend_mir_storage_id source_storage =
					canonical_storage(assigned);
				if (zend_mir_id_is_valid(source_storage)) {
					early_pointer_phi_storages.push_back(source_storage);
				}
				const zend_mir_storage_id destination_storage =
					instruction.value_operation.op1_storage_id;
				if (zend_mir_id_is_valid(destination_storage)) {
					early_pointer_phi_storages.push_back(
						destination_storage);
				}
			}
			std::ranges::sort(early_pointer_phi_storages);
			early_pointer_phi_storages.erase(
				std::unique(early_pointer_phi_storages.begin(),
					early_pointer_phi_storages.end()),
				early_pointer_phi_storages.end());
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_mir_instruction_record phi =
					instruction_record_at(i);
				if (phi.opcode != ZEND_MIR_OPCODE_PHI
						|| phi.representation
							!= ZEND_MIR_REPRESENTATION_ZVAL) {
					continue;
				}
				const int32_t result_index =
					zend_tpde_value_index(plan_, phi.result_id);
				const IRValueRef canonical =
					result_index < 0 ? INVALID_VALUE_REF
						: IRValueRef{MIR_VALUE_BASE
							+ static_cast<uint32_t>(result_index)};
				const bool pointer_phi =
					canonical != INVALID_VALUE_REF
					&& machine_pointer_kind(machine_kind(canonical))
					&& machine_value_is_register_authoritative(canonical);
				if (canonical == INVALID_VALUE_REF || !pointer_phi) {
					continue;
				}
				const zend_mir_storage_id storage_id =
					canonical_storage(canonical);
				if (pointer_phi && !std::binary_search(
						early_pointer_phi_storages.begin(),
						early_pointer_phi_storages.end(), storage_id)) {
					continue;
				}
				const IRValueRef selected = add_derived_value(
					ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
					exact_type(canonical), storage_id, false, 0,
					machine_kind(canonical),
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN);
				if (!zend_mir_id_is_valid(storage_id)
						|| selected == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				active_value_overrides()[
					static_cast<uint32_t>(result_index)] = selected;
				preselected_phi_results[i] = selected;
				if (zend_mir_value_is_original_ssa(
							plan_->values[result_index].id)
						&& plan_->values[result_index].id
							< active_source_ssa_overrides().size()) {
					active_source_ssa_overrides()[
						plan_->values[result_index].id] = selected;
				}
			}
			std::vector<IRValueRef> register_assignment_sources(
				plan_->instruction_count, INVALID_VALUE_REF);
			std::vector<IRValueRef> register_assignment_results(
				plan_->instruction_count, INVALID_VALUE_REF);
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				if (record.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN
						|| !instruction.has_value_operation
						|| instruction.value_operation.op1.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_CV
						|| guarded_cold_blocks[i] == UINT32_MAX) {
					continue;
				}
				const IRValueRef assigned = source_binding_value_ref(
					instruction.source_op2_binding);
				const uint32_t definition_plus_one =
					instruction.value_operation
						.op1_definition_ssa_variable_id_plus_one;
				if (assigned == INVALID_VALUE_REF
						|| definition_plus_one == 0
						|| !machine_value_is_register_authoritative(
							assigned)
						|| !machine_value_has_register_definition(assigned)
						|| !zend_tpde_machine_value_is_register_authoritative(
							machine_kind(assigned))) {
					continue;
				}
				const uint32_t definition = definition_plus_one - 1;
				auto &source_overrides =
					active_source_ssa_overrides();
				if (definition >= source_overrides.size()) {
					valid_ = false;
					continue;
				}
				const int32_t value_index = zend_tpde_value_index(
					plan_,
					zend_mir_value_from_original_ssa(definition));
				if (value_index < 0
						|| static_cast<uint32_t>(value_index)
							>= active_value_overrides().size()) {
					valid_ = false;
					continue;
				}
				IRValueRef assignment_result = assigned;
				if (machine_pointer_kind(machine_kind(assigned))) {
					uint64_t literal_length = 0;
					bool literal_truthy = false;
					const bool string_literal = known_string_literal(
						assigned, &literal_length, &literal_truthy);
					const uint8_t literal_first_byte = string_literal
							&& literal_length == 1 && !literal_truthy
						? '0' : 0;
					assignment_result = add_derived_value(
						ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
						exact_type(assigned),
						instruction.value_operation.op1_storage_id,
						false, 0, machine_kind(assigned),
						ownership(assigned), refcount_state(assigned),
						UINT32_MAX, string_literal, literal_first_byte,
						literal_length);
					if (assignment_result == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
				}
				register_assignment_sources[i] = assigned;
				register_assignment_results[i] = assignment_result;
				source_overrides[definition] = assignment_result;
				active_value_overrides()[
					static_cast<uint32_t>(value_index)] =
						assignment_result;
			}
			struct RegisterAssignmentPhiSource {
				zend_mir_storage_id storage_id;
				IRValueRef value;
			};
			std::vector<RegisterAssignmentPhiSource>
				register_assignment_phi_candidates;
			register_assignment_phi_candidates.reserve(
				register_assignment_results.size());
			for (uint32_t i = 0;
					i < register_assignment_results.size(); ++i) {
				const IRValueRef candidate =
					register_assignment_results[i];
				if (candidate == INVALID_VALUE_REF
						|| !machine_pointer_kind(machine_kind(candidate))
						|| !plan_->instructions[i].has_value_operation) {
					continue;
				}
				register_assignment_phi_candidates.push_back({
					plan_->instructions[i].value_operation.op1_storage_id,
					candidate});
			}
			std::stable_sort(register_assignment_phi_candidates.begin(),
				register_assignment_phi_candidates.end(),
				[](const RegisterAssignmentPhiSource &left,
						const RegisterAssignmentPhiSource &right) {
					return left.storage_id < right.storage_id;
				});
			std::vector<RegisterAssignmentPhiSource>
				register_assignment_phi_sources;
			register_assignment_phi_sources.reserve(
				register_assignment_phi_candidates.size());
			for (uint32_t begin = 0;
					begin < register_assignment_phi_candidates.size();) {
				uint32_t end = begin + 1;
				IRValueRef selected =
					register_assignment_phi_candidates[begin].value;
				bool conflicting_kind = false;
				while (end < register_assignment_phi_candidates.size()
						&& register_assignment_phi_candidates[end].storage_id
							== register_assignment_phi_candidates[begin]
								.storage_id) {
					const IRValueRef candidate =
						register_assignment_phi_candidates[end].value;
					if (machine_kind(selected) != machine_kind(candidate)) {
						conflicting_kind = true;
					}
					selected = candidate;
					++end;
				}
				register_assignment_phi_sources.push_back({
					register_assignment_phi_candidates[begin].storage_id,
					conflicting_kind ? INVALID_VALUE_REF : selected});
				begin = end;
			}
			auto register_assignment_phi_source =
				[&](zend_mir_storage_id storage_id) {
					const auto source = std::lower_bound(
						register_assignment_phi_sources.begin(),
						register_assignment_phi_sources.end(), storage_id,
						[](const RegisterAssignmentPhiSource &candidate,
								zend_mir_storage_id id) {
							return candidate.storage_id < id;
						});
					return source != register_assignment_phi_sources.end()
							&& source->storage_id == storage_id
						? source->value : INVALID_VALUE_REF;
				};
			std::vector<uint8_t> source_result_used(
				plan_->instruction_count);
			std::vector<int32_t> source_result_consumer(
				plan_->instruction_count, -1);
			std::vector<uint8_t> source_result_materialized_for_helper(
				plan_->instruction_count);
			std::vector<int32_t> source_producer_by_value(
				plan_->value_count, -1);
			for (uint32_t consumer_index = 0;
					consumer_index < plan_->instruction_count;
					++consumer_index) {
			const zend_tpde_instruction &consumer =
					plan_->instructions[consumer_index];
			if (!consumer.has_value_operation) {
				continue;
			}
			if (consumer.source_result_binding.value_index >= 0
					&& static_cast<uint32_t>(
						consumer.source_result_binding.value_index)
							< source_producer_by_value.size()) {
					source_producer_by_value[
					static_cast<uint32_t>(
						consumer.source_result_binding.value_index)] =
					static_cast<int32_t>(consumer_index);
			}
				/*
				 * FE_FETCH defines its destination through op2 rather than the
				 * ordinary result operand.  Record that source SSA producer so an
				 * optimized binary consumer can retain the fetched zval in the
		 * machine def-use graph.
				 */
				if (consumer.record.opcode
						== ZEND_MIR_OPCODE_ITERATOR_BRANCH
						&& consumer.source_op2_definition_binding.value_index >= 0
						&& static_cast<uint32_t>(
							consumer.source_op2_definition_binding.value_index)
							< source_producer_by_value.size()) {
					source_producer_by_value[
						static_cast<uint32_t>(
							consumer.source_op2_definition_binding.value_index)] =
						static_cast<int32_t>(consumer_index);
				}
				auto mark_source_producer =
					[&](const zend_tpde_source_value_binding &binding) {
						int32_t producer =
							binding.definition_instruction_index;
						if (producer < 0 && binding.value_index >= 0
								&& static_cast<uint32_t>(binding.value_index)
									< source_producer_by_value.size()) {
							producer = source_producer_by_value[
								static_cast<uint32_t>(binding.value_index)];
						}
						if (producer >= 0
								&& static_cast<uint32_t>(producer)
									< source_result_used.size()) {
							const uint32_t producer_index =
								static_cast<uint32_t>(producer);
							source_result_used[producer_index] = 1;
							int32_t &recorded_consumer =
								source_result_consumer[producer_index];
							if (recorded_consumer == -1) {
								recorded_consumer =
									static_cast<int32_t>(consumer_index);
							} else if (recorded_consumer
									!= static_cast<int32_t>(consumer_index)) {
								recorded_consumer = -2;
							}
						}
					};
				/*
				 * A source-backed statepoint materialization is itself a machine
				 * consumer.  Mark its producer before result selection so a scalar
				 * source operation such as STRLEN receives the derived register
				 * result that the materialization operand references.
				 */
				if (consumer.materialization_offset
						<= plan_->materialization_count
						&& consumer.materialization_count
							<= plan_->materialization_count
								- consumer.materialization_offset) {
					for (uint32_t materialization_index = 0;
							materialization_index
								< consumer.materialization_count;
							++materialization_index) {
						const zend_tpde_materialization &materialization =
							plan_->materializations[
								consumer.materialization_offset
									+ materialization_index];
						if (materialization.value_index == UINT32_MAX
								&& materialization
									.source_definition_instruction_index >= 0) {
							mark_source_producer({
								materialization.source_value_index,
								materialization
									.source_definition_instruction_index,
							});
						}
					}
				}
					switch (consumer.record.opcode) {
					case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
					case ZEND_MIR_OPCODE_VALUE_ASSIGN:
						if (consumer.source_op2_binding.value_index >= 0
								|| consumer.source_op2_binding
									.definition_instruction_index >= 0) {
							mark_source_producer(
								consumer.source_op2_binding);
						} else {
							mark_source_producer(
								consumer.source_op1_binding);
						}
						break;
					case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
						mark_source_producer(
							consumer.source_op1_binding);
						break;
					case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
						/*
						 * Full OPcache DFA rewrites "$cv += fetch()" into a
						 * binary operation whose result directly defines the
						 * CV.  Both operands are then real consumers, rather
						 * than an intervening VALUE_ASSIGN_OP.  Preserve a
						 * source result in TPDE registers so the binary
						 * selector can consume it on the fast CFG.
						 */
						mark_source_producer(
							consumer.source_op1_binding);
						mark_source_producer(
							consumer.source_op2_binding);
						break;
					case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
					case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
						/*
						 * Guarded reads and isset/empty consume both the array
						 * payload and the dimension key directly. Keep both
						 * producers in the machine def-use graph.
						 */
						mark_source_producer(
							consumer.source_op1_binding);
						mark_source_producer(
							consumer.source_op2_binding);
						break;
					case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
					case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
					case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL:
						mark_source_producer(
							consumer.source_op1_binding);
						break;
					case ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL:
					case ZEND_MIR_OPCODE_CALL_DIRECT_USER:
						/*
						 * A direct call consumes its SEND operands through the
						 * frozen call-argument bindings rather than source_op1 or
						 * source_op2.  Record those uses before selecting scalar
						 * producer results so an immediately consumed value can
						 * remain register-authoritative through the call boundary.
						 */
						if (plan_->call_argument_bindings != nullptr
								&& consumer.call_argument_offset
									<= plan_->call_argument_count
								&& consumer.call_argument_count
									<= plan_->call_argument_count
										- consumer.call_argument_offset) {
							for (uint32_t argument_index = 0;
									argument_index
										< consumer.call_argument_count;
									++argument_index) {
								mark_source_producer(
									plan_->call_argument_bindings[
										consumer.call_argument_offset
											+ argument_index]);
							}
						}
						break;
				default:
					break;
			}
		}
		/*
		 * Calls do not carry executable value descriptors, so the value-operation
		 * consumer scan above cannot reach its CALL cases. Record their frozen SEND
		 * definitions separately before selecting register results. This is required
		 * when OPcache compacts several temporary SSA versions into one Zend slot.
		 */
		for (uint32_t consumer_index = 0;
				consumer_index < plan_->instruction_count; ++consumer_index) {
			const zend_tpde_instruction &consumer =
				plan_->instructions[consumer_index];
			if ((consumer.record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
					&& consumer.record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER)
					|| plan_->call_argument_bindings == nullptr
					|| consumer.call_argument_offset > plan_->call_argument_count
					|| consumer.call_argument_count > plan_->call_argument_count
						- consumer.call_argument_offset) {
				continue;
			}
			for (uint32_t argument = 0;
					argument < consumer.call_argument_count; ++argument) {
				const zend_tpde_source_value_binding &binding =
					plan_->call_argument_bindings[
						consumer.call_argument_offset + argument];
				int32_t producer = binding.definition_instruction_index;
				if (producer < 0 && binding.value_index >= 0
						&& static_cast<uint32_t>(binding.value_index)
							< source_producer_by_value.size()) {
					producer = source_producer_by_value[
						static_cast<uint32_t>(binding.value_index)];
				}
				if (producer < 0
						|| static_cast<uint32_t>(producer)
							>= source_result_used.size()) {
					continue;
				}
				const uint32_t producer_index =
					static_cast<uint32_t>(producer);
				source_result_used[producer_index] = 1;
				int32_t &recorded_consumer =
					source_result_consumer[producer_index];
				if (recorded_consumer == -1) {
					recorded_consumer = static_cast<int32_t>(consumer_index);
				} else if (recorded_consumer
						!= static_cast<int32_t>(consumer_index)) {
					recorded_consumer = -2;
				}
			}
		}
		/*
		 * Calls have no value-operation descriptor, so their statepoint
		 * materializations are skipped by the scan above as well.  Retain the
		 * exact source producer named by each materialization before selecting
		 * machine results; otherwise a compacted temporary may be reloaded only
		 * after its Zend slot has already been reused.
		 */
		for (uint32_t consumer_index = 0;
				consumer_index < plan_->instruction_count; ++consumer_index) {
			const zend_tpde_instruction &consumer =
				plan_->instructions[consumer_index];
			if (consumer.materialization_offset > plan_->materialization_count
					|| consumer.materialization_count
						> plan_->materialization_count
							- consumer.materialization_offset) {
				continue;
			}
			for (uint32_t materialization_index = 0;
					materialization_index < consumer.materialization_count;
					++materialization_index) {
				const zend_tpde_materialization &materialization =
					plan_->materializations[consumer.materialization_offset
						+ materialization_index];
				const int32_t producer =
					materialization.value_index == UINT32_MAX
						? materialization.source_definition_instruction_index
						: -1;
				if (producer < 0
						|| static_cast<uint32_t>(producer)
							>= source_result_used.size()) {
					continue;
				}
				const uint32_t producer_index =
					static_cast<uint32_t>(producer);
				source_result_used[producer_index] = 1;
				int32_t &recorded_consumer =
					source_result_consumer[producer_index];
				if (recorded_consumer == -1) {
					recorded_consumer = static_cast<int32_t>(consumer_index);
				} else if (recorded_consumer
						!= static_cast<int32_t>(consumer_index)) {
					recorded_consumer = -2;
				}
			}
		}
			auto source_result_has_direct_consumer =
				[&](uint32_t producer_index) {
					if (producer_index >= source_result_consumer.size()) {
						return false;
					}
					const int32_t signed_consumer =
						source_result_consumer[producer_index];
					if (signed_consumer <= static_cast<int32_t>(producer_index)
							|| static_cast<uint32_t>(signed_consumer)
								>= plan_->instruction_count) {
						return false;
					}
					const uint32_t consumer_index =
						static_cast<uint32_t>(signed_consumer);
					const uint32_t consumer_block =
						instruction_blocks[consumer_index];
					const uint32_t producer_continuation =
						guarded_continuation_blocks[producer_index];
					/*
					 * A guarded producer joins its fast and cold results in a
					 * generated continuation.  The next source instruction lives
					 * there even though no source operation intervenes.
					 */
					uint32_t active_block = producer_continuation != UINT32_MAX
						? producer_continuation
						: instruction_blocks[producer_index];
					for (uint32_t intermediate = producer_index + 1;
							intermediate < consumer_index; ++intermediate) {
						const zend_mir_opcode opcode =
							instruction_record_at(intermediate).opcode;
						if (opcode == ZEND_MIR_OPCODE_CONSTANT
								|| opcode == ZEND_MIR_OPCODE_STATEPOINT) {
							continue;
						}
						const bool composable_boxed_read =
							(opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
								|| opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
								|| opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
							&& source_result_consumer[intermediate]
								== signed_consumer
							&& guarded_continuation_blocks[intermediate]
								!= UINT32_MAX
							&& instruction_blocks[intermediate] == active_block;
						if (!composable_boxed_read) {
							return false;
						}
						active_block =
							guarded_continuation_blocks[intermediate];
					}
					return consumer_block == active_block;
				};
		/*
		 * A full-DFA ADD/SUB may define a canonical ZVAL whose inferred type is
		 * long|double.  Keeping that definition frame-authoritative prevents a
		 * loop PHI from becoming a TPDE value, even though the long fast path
		 * and the overflow cold path can both produce the complete
		 * payload/type-info pair.  Select those results before PHIs so the
		 * backedge is already a boxed machine value when the loop join is
		 * formed.
		 */
		std::vector<IRValueRef> register_binary_results(
			plan_->instruction_count, INVALID_VALUE_REF);
		std::vector<IRValueRef> register_branch_results(
			plan_->instruction_count, INVALID_VALUE_REF);
		std::vector<IRValueRef> register_boolean_results(
			plan_->instruction_count, INVALID_VALUE_REF);
		std::vector<zend_mir_storage_id> register_boxed_storages;
		/*
		 * Short-circuit PHIs precede their source operations in the frozen
		 * instruction order.  Preselect exact boolean results before visiting
		 * those PHIs, including the edge result of JMPZ_EX/JMPNZ_EX.  The latter
		 * is the condition itself on the only edge where the source opcode
		 * defines its result, so it needs no separate machine instruction.
		 */
		if (function_mode_ == FunctionMode::ZendEntry) {
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				if (!instruction.has_value_operation) {
					continue;
				}
				const bool register_boolean =
					(instruction.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
					&& (record.opcode
							== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
						|| (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
							&& (instruction.value_operation.source_opcode
									== ZEND_BOOL
								|| instruction.value_operation.source_opcode
									== ZEND_BOOL_NOT)));
				if (register_boolean) {
					const int32_t result_index =
						instruction.source_result_binding.value_index;
					const zend_mir_value_id result_ssa =
						instruction.value_operation.result.ssa_variable_id;
					const zend_mir_storage_id result_storage =
						instruction.value_operation.result_storage_id;
					if (result_index < 0
							|| static_cast<uint32_t>(result_index)
								>= plan_->value_count
							|| result_ssa
								>= active_source_ssa_overrides().size()
							|| !zend_mir_id_is_valid(result_storage)) {
						valid_ = false;
						continue;
					}
					const bool result_alias =
						(instruction.machine_control_flow_flags
							& ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS) != 0;
					const IRValueRef selected = result_alias
						? source_binding_value_ref(
							instruction.source_op1_binding)
						: add_derived_value(
							ZEND_MIR_REPRESENTATION_I1,
							ZEND_MIR_SCALAR_TYPE_I1, result_storage,
							false, 0, ZEND_TPDE_MACHINE_VALUE_BOOL,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_IMMORTAL);
					if (selected == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
					if (result_alias
							&& (exact_type(selected) != ZEND_MIR_SCALAR_TYPE_I1
								|| machine_kind(selected)
									!= ZEND_TPDE_MACHINE_VALUE_BOOL)) {
						valid_ = false;
						continue;
					}
					register_boolean_results[i] = selected;
					active_instruction_results()[i] = selected;
					active_value_overrides()[
						static_cast<uint32_t>(result_index)] = selected;
					active_source_ssa_overrides()[result_ssa] = selected;
					continue;
				}
				const uint32_t source_opcode =
					instruction.value_operation.source_opcode;
				if (record.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						|| (source_opcode != ZEND_JMPZ_EX
							&& source_opcode != ZEND_JMPNZ_EX)) {
					continue;
				}
				const IRValueRef condition = source_binding_value_ref(
					instruction.source_op1_binding);
				const int32_t result_index =
					instruction.source_result_binding.value_index;
				const zend_mir_value_id result_ssa =
					instruction.value_operation.result.ssa_variable_id;
				if (condition == INVALID_VALUE_REF
						|| exact_type(condition) != ZEND_MIR_SCALAR_TYPE_I1
						|| machine_kind(condition)
							!= ZEND_TPDE_MACHINE_VALUE_BOOL
						|| !machine_value_has_register_definition(condition)) {
					continue;
				}
				if (result_index < 0
						|| static_cast<uint32_t>(result_index)
							>= plan_->value_count
						|| result_ssa >= active_source_ssa_overrides().size()) {
					valid_ = false;
					continue;
				}
				active_instruction_results()[i] = condition;
				active_value_overrides()[
					static_cast<uint32_t>(result_index)] = condition;
				active_source_ssa_overrides()[result_ssa] = condition;
			}
			/*
			 * Select only the canonical two-edge short-circuit join: one input
			 * must be the register edge result of JMPZ_EX/JMPNZ_EX, every input
			 * must already be an exact machine boolean, and the joined value may
			 * only feed a following condition.  This intentionally excludes
			 * loop-carried and general-purpose boolean PHIs.
			 */
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				if (record.opcode != ZEND_MIR_OPCODE_PHI
						|| instruction.operand_count != 2
						|| instruction.operand_offset
							> plan_->instruction_operand_count
						|| instruction.operand_count
							> plan_->instruction_operand_count
								- instruction.operand_offset) {
					continue;
				}
				const int32_t result_index =
					zend_tpde_value_index(plan_, record.result_id);
				if (result_index < 0
						|| static_cast<uint32_t>(result_index)
							>= plan_->value_count
						|| plan_->value_consumer_offsets == nullptr
						|| plan_->value_consumers == nullptr) {
					continue;
				}
				bool conditional_consumer = false;
				bool consumers_supported = true;
				const uint32_t consumer_begin =
					plan_->value_consumer_offsets[result_index];
				const uint32_t consumer_end =
					plan_->value_consumer_offsets[result_index + 1];
				for (uint32_t use_index = consumer_begin;
						use_index < consumer_end; ++use_index) {
					const zend_tpde_machine_use &use =
						plan_->value_consumers[use_index];
					if (use.kind != ZEND_TPDE_MACHINE_USE_SOURCE_OPERAND
							|| use.operand_index != 0
							|| use.instruction_index
								>= plan_->instruction_count) {
						consumers_supported = false;
						break;
					}
					const zend_tpde_instruction &consumer =
						plan_->instructions[use.instruction_index];
					const zend_mir_instruction_record consumer_record =
						instruction_record_at(use.instruction_index);
					if (!consumer.has_value_operation
							|| consumer_record.opcode
								!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
							|| consumer.source_op1_binding.value_index
								!= result_index) {
						consumers_supported = false;
						break;
					}
					conditional_consumer = true;
				}
				bool exact_register_inputs = true;
				bool short_circuit_edge = false;
				for (uint32_t n = 0; n < instruction.operand_count; ++n) {
					const zend_mir_value_id input_id =
						zend_tpde_operand_at(plan_, &instruction, n);
					const IRValueRef input = value_ref(input_id);
					if (input == INVALID_VALUE_REF
							|| exact_type(input) != ZEND_MIR_SCALAR_TYPE_I1
							|| machine_kind(input)
								!= ZEND_TPDE_MACHINE_VALUE_BOOL
							|| !machine_value_has_register_definition(input)) {
						exact_register_inputs = false;
						break;
					}
					const int32_t input_index =
						zend_tpde_value_index(plan_, input_id);
					for (uint32_t producer_index = 0;
							input_index >= 0
								&& producer_index < plan_->instruction_count;
							++producer_index) {
						const zend_tpde_instruction &producer =
							plan_->instructions[producer_index];
						const uint32_t source_opcode =
							producer.has_value_operation
								? producer.value_operation.source_opcode : 0;
						if (producer.source_result_binding.value_index
								== input_index
								&& producer.record.opcode
									== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
								&& (source_opcode == ZEND_JMPZ_EX
									|| source_opcode == ZEND_JMPNZ_EX)
								&& (producer.machine_control_flow_flags
									& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_BRANCH)
									!= 0) {
							short_circuit_edge = true;
							break;
						}
					}
				}
				if (!conditional_consumer || !consumers_supported
						|| !exact_register_inputs || !short_circuit_edge) {
					continue;
				}
				const IRValueRef canonical_result{
					MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
				const zend_mir_storage_id result_storage =
					canonical_storage(canonical_result);
				if (!zend_mir_id_is_valid(result_storage)) {
					continue;
				}
				const IRValueRef selected = add_derived_value(
					ZEND_MIR_REPRESENTATION_I1,
					ZEND_MIR_SCALAR_TYPE_I1, result_storage,
					false, 0, ZEND_TPDE_MACHINE_VALUE_BOOL,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_IMMORTAL);
				if (selected == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				register_boolean_results[i] = selected;
				preselected_phi_results[i] = selected;
				active_instruction_results()[i] = selected;
				active_value_overrides()[result_index] = selected;
				if (zend_mir_value_is_original_ssa(
						plan_->values[result_index].id)
						&& plan_->values[result_index].id
							< active_source_ssa_overrides().size()) {
					active_source_ssa_overrides()[
						plan_->values[result_index].id] = selected;
				}
			}
		}
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			if (machine_block_reachable[instruction_blocks[i]] == 0) {
				continue;
			}
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const bool typed_call =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr
				&& frozen_typed_component_call(i);
			const bool scalar_inline =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr
				&& frozen_effect_closed_inline(i);
			const zend_tpde_plan *callee =
				instruction.component_target_index
						< component_plans_.size()
					? component_plans_[
						instruction.component_target_index]
					: nullptr;
			if ((!typed_call && !scalar_inline)
					|| callee == nullptr
					|| callee->argument_value_indices == nullptr
					|| plan_->call_argument_bindings == nullptr) {
				continue;
			}
			for (uint32_t argument_index = 0;
					argument_index < instruction.call_argument_count;
					++argument_index) {
				zend_mir_call_argument_ref argument{};
				const uint32_t call_argument_index =
					instruction.call_argument_offset + argument_index;
				const int32_t callee_value_index =
					argument_index < callee->argument_count
						? callee->argument_value_indices[argument_index]
						: -1;
				if (callee_value_index < 0
						|| static_cast<uint32_t>(callee_value_index)
							>= callee->value_count
						|| !zend_tpde_call_argument_at(plan_,
							call_argument_index, &argument)) {
					continue;
				}
				IRValueRef value = source_binding_value_ref(
					plan_->call_argument_bindings[
						call_argument_index]);
				if (value == INVALID_VALUE_REF) {
					value = source_operand_value_ref(
						argument.source_operand);
				}
				if (value == INVALID_VALUE_REF
						&& zend_mir_id_is_valid(argument.value_id)) {
					value = value_ref(argument.value_id);
				}
				if (value == INVALID_VALUE_REF
						|| machine_value_is_register_authoritative(value)
						|| !zend_mir_id_is_valid(
							canonical_storage(value))) {
					continue;
				}
				const TypedBodyAbiType transport_abi =
					typed_body_value_abi(
						callee,
						static_cast<uint32_t>(callee_value_index));
				const bool boxed_transport =
					typed_call && transport_abi.valid
					&& transport_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
				const bool scalar_transport =
					scalar_inline && transport_abi.valid
					&& representation(value)
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& (exact_type(value)
							== transport_abi.exact_type
						|| (exact_type(value)
								== ZEND_MIR_SCALAR_TYPE_NONE
							&& argument_index
								< instruction.direct_call
									->argument_count
							&& instruction.direct_call
									->arguments[argument_index].exact_type
								== transport_abi.exact_type))
					&& (transport_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_I64
						|| transport_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_BOOL);
				if (boxed_transport || scalar_transport) {
					register_boxed_storages.push_back(
						canonical_storage(value));
				}
			}
		}
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (function_mode_ != FunctionMode::ZendEntry
					|| record.opcode
						!= ZEND_MIR_OPCODE_VALUE_BINARY_OP
					|| !instruction.has_value_operation
					|| guarded_cold_blocks[i] == UINT32_MAX) {
				continue;
			}
			const uint32_t source_opcode =
				instruction.value_operation.source_opcode;
			const bool boolean_result =
				source_opcode == ZEND_IS_IDENTICAL
				|| source_opcode == ZEND_IS_NOT_IDENTICAL
				|| source_opcode == ZEND_IS_EQUAL
				|| source_opcode == ZEND_IS_NOT_EQUAL
				|| source_opcode == ZEND_IS_SMALLER
				|| source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
			if (!boolean_result
					&& source_opcode != ZEND_ADD
					&& source_opcode != ZEND_SUB
					&& source_opcode != ZEND_BW_OR
					&& source_opcode != ZEND_BW_AND
					&& source_opcode != ZEND_BW_XOR
					&& source_opcode != ZEND_SPACESHIP) {
				continue;
			}
			const int32_t result_index =
				instruction.source_result_binding.value_index;
			const zend_mir_storage_id result_storage =
				instruction.value_operation.result_storage_id;
			const zend_mir_value_id result_ssa =
				instruction.value_operation.result.ssa_variable_id;
			if (result_index < 0
					|| static_cast<uint32_t>(result_index)
						>= plan_->value_count
					|| !zend_mir_id_is_valid(result_storage)
					|| result_ssa
						>= active_source_ssa_overrides().size()) {
				continue;
			}
			const IRValueRef canonical{
				MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
			const bool scalar_result =
				(representation(canonical)
						== ZEND_MIR_REPRESENTATION_I64
					&& exact_type(canonical)
						== ZEND_MIR_SCALAR_TYPE_I64
					&& machine_kind(canonical)
						== ZEND_TPDE_MACHINE_VALUE_I64)
				|| (representation(canonical)
						== ZEND_MIR_REPRESENTATION_I1
					&& exact_type(canonical)
						== ZEND_MIR_SCALAR_TYPE_I1
					&& machine_kind(canonical)
						== ZEND_TPDE_MACHINE_VALUE_BOOL);
			const bool boxed_result =
				representation(canonical)
					== ZEND_MIR_REPRESENTATION_ZVAL;
			if ((!scalar_result && !boxed_result)
					|| (scalar_result
						? machine_value_has_register_definition(canonical)
						: machine_value_has_result_representation(canonical))) {
				continue;
			}
			const IRValueRef selected = add_derived_value(
				scalar_result
					? representation(canonical)
					: ZEND_MIR_REPRESENTATION_ZVAL,
				scalar_result
					? exact_type(canonical)
					: ZEND_MIR_SCALAR_TYPE_NONE,
				result_storage, false, 0,
				scalar_result
					? machine_kind(canonical)
					: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				ownership(canonical), refcount_state(canonical));
			if (selected == INVALID_VALUE_REF) {
				valid_ = false;
				continue;
			}
			register_binary_results[i] = selected;
			active_instruction_results()[i] = selected;
			active_value_overrides()[
				static_cast<uint32_t>(result_index)] = selected;
			active_source_ssa_overrides()[result_ssa] = selected;
			if (boxed_result) {
				source_result_used[i] = 1;
				register_boxed_storages.push_back(result_storage);
			}
		}
		/*
		 * Result-producing source branches define their result only on
		 * the taken edge.  When that result is an exact scalar or semantic
		 * pointer feeding a PHI, make the branch instruction its TPDE
		 * definition and let the target capture the helper-produced canonical
		 * payload before branching.  This avoids an eager predecessor load
		 * before the source operation has written the result slot. Exact boolean
		 * short-circuit results were selected by the earlier boolean prepass;
		 * this loop handles the remaining JMP_SET/COALESCE result shapes.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			if (machine_block_reachable[instruction_blocks[i]] == 0) {
				continue;
			}
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const int32_t result_index =
				instruction.source_result_binding.value_index;
			if (record.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					|| !instruction.has_value_operation
					|| (instruction.value_operation.source_opcode
							!= ZEND_JMP_SET
						&& instruction.value_operation.source_opcode
							!= ZEND_COALESCE)
					|| result_index < 0
					|| static_cast<uint32_t>(result_index)
						>= plan_->value_count
					|| !zend_mir_id_is_valid(
						instruction.value_operation.result_storage_id)) {
				continue;
			}
			const IRValueRef canonical{
				MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
			const bool scalar_result =
				(representation(canonical)
						== ZEND_MIR_REPRESENTATION_I64
					&& exact_type(canonical)
						== ZEND_MIR_SCALAR_TYPE_I64
					&& machine_kind(canonical)
						== ZEND_TPDE_MACHINE_VALUE_I64)
				|| (representation(canonical)
						== ZEND_MIR_REPRESENTATION_I1
					&& exact_type(canonical)
						== ZEND_MIR_SCALAR_TYPE_I1
					&& machine_kind(canonical)
						== ZEND_TPDE_MACHINE_VALUE_BOOL);
			const bool pointer_result =
				machine_pointer_kind(machine_kind(canonical));
			if ((!scalar_result && !pointer_result)
					|| machine_value_has_register_definition(canonical)) {
				continue;
			}
			const zend_mir_value_id result_ssa =
				instruction.value_operation.result.ssa_variable_id;
			if (result_ssa >= active_source_ssa_overrides().size()) {
				valid_ = false;
				continue;
			}
			const IRValueRef selected = add_derived_value(
				pointer_result
					? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
					: representation(canonical),
				exact_type(canonical),
				instruction.value_operation.result_storage_id,
				false, 0, machine_kind(canonical),
				ownership(canonical), refcount_state(canonical));
			if (selected == INVALID_VALUE_REF) {
				valid_ = false;
				continue;
			}
			register_branch_results[i] = selected;
			active_instruction_results()[i] = selected;
			active_value_overrides()[
				static_cast<uint32_t>(result_index)] = selected;
			active_source_ssa_overrides()[result_ssa] = selected;
		}
		/*
		 * A source FE_FETCH writes its destination on the taken edge.  Full
		 * OPcache DFA can feed that definition directly into a binary operation,
		 * bypassing the assignment opcode which would otherwise preserve the
		 * register-authoritative value.  Publish a boxed branch result for the
		 * packed-fetch shape; both target backends reload the canonical zval after
		 * either the guarded fast path or the semantic helper has completed.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			zend_tpde_packed_iterator_fetch iterator{};
			const int32_t result_index =
				instruction.source_op2_definition_binding.value_index;
			if (function_mode_ != FunctionMode::ZendEntry
					|| instruction.record.opcode
						!= ZEND_MIR_OPCODE_ITERATOR_BRANCH
					|| source_result_used[i] == 0
					|| result_index < 0
					|| static_cast<uint32_t>(result_index)
						>= plan_->value_count
					|| !zend_tpde_packed_iterator_fetch_at(
						instruction, &iterator)
					|| !zend_mir_id_is_valid(
						instruction.value_operation.op2_storage_id)
					|| instruction
						.source_op2_definition_ssa_variable_id_plus_one == 0) {
				continue;
			}
			const IRValueRef canonical{
				MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
			if (representation(canonical) != ZEND_MIR_REPRESENTATION_ZVAL
					|| machine_value_has_register_definition(canonical)) {
				continue;
			}
			const zend_mir_value_id result_ssa =
				instruction
					.source_op2_definition_ssa_variable_id_plus_one - 1;
			if (result_ssa >= active_source_ssa_overrides().size()) {
				valid_ = false;
				continue;
			}
			const IRValueRef selected = add_derived_value(
				iterator.destination_scalar_only
					? ZEND_MIR_REPRESENTATION_I64
					: ZEND_MIR_REPRESENTATION_ZVAL,
				iterator.destination_scalar_only
					? ZEND_MIR_SCALAR_TYPE_I64
					: ZEND_MIR_SCALAR_TYPE_NONE,
				instruction.value_operation.op2_storage_id,
				false, 0, iterator.destination_scalar_only
					? ZEND_TPDE_MACHINE_VALUE_I64
					: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				iterator.destination_scalar_only
					? ZEND_MIR_OWNERSHIP_STATE_BORROWED
					: ownership(canonical),
				iterator.destination_scalar_only
					? ZEND_MIR_REFCOUNT_IMMORTAL
					: refcount_state(canonical));
			if (selected == INVALID_VALUE_REF) {
				valid_ = false;
				continue;
			}
			register_branch_results[i] = selected;
			active_instruction_results()[i] = selected;
			active_value_overrides()[
				static_cast<uint32_t>(result_index)] = selected;
			active_source_ssa_overrides()[result_ssa] = selected;
		}
		for (uint32_t pass = 0;
				pass < plan_->instruction_count; ++pass) {
			bool changed = false;
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				if (record.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN
						|| !instruction.has_value_operation
						|| std::ranges::find(
							register_boxed_storages,
							instruction.value_operation.op1_storage_id)
							== register_boxed_storages.end()) {
					continue;
				}
				IRValueRef source = source_binding_value_ref(
					instruction.source_op2_binding);
				if (source == INVALID_VALUE_REF) {
					source = source_operand_value_ref(
						instruction.value_operation.op2);
				}
				const zend_mir_storage_id source_storage =
					source == INVALID_VALUE_REF
						? ZEND_MIR_ID_INVALID
						: canonical_storage(source);
				if (zend_mir_id_is_valid(source_storage)
						&& std::ranges::find(
							register_boxed_storages,
							source_storage)
							== register_boxed_storages.end()) {
					register_boxed_storages.push_back(source_storage);
					changed = true;
				}
			}
			if (!changed) {
				break;
			}
		}
		/*
		 * Binary and branch results are selected after the first assignment pass.
		 * Revisit assignments whose source only became register-authoritative in
		 * those passes so later PHI-edge copies retain that machine definition.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (record.opcode != ZEND_MIR_OPCODE_VALUE_ASSIGN
					|| !instruction.has_value_operation
					|| instruction.value_operation.op1.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV
					|| guarded_cold_blocks[i] == UINT32_MAX
					|| register_assignment_results[i]
						!= INVALID_VALUE_REF) {
				continue;
			}
			const IRValueRef assigned = source_binding_value_ref(
				instruction.source_op2_binding);
			const uint32_t definition_plus_one =
				instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one;
			if (assigned == INVALID_VALUE_REF
					|| definition_plus_one == 0
					|| !machine_value_is_register_authoritative(assigned)
					|| !machine_value_has_register_definition(assigned)
					|| !zend_tpde_machine_value_is_register_authoritative(
						machine_kind(assigned))) {
				continue;
			}
			const uint32_t definition = definition_plus_one - 1;
			auto &source_overrides = active_source_ssa_overrides();
			const int32_t value_index = zend_tpde_value_index(
				plan_, zend_mir_value_from_original_ssa(definition));
			if (definition >= source_overrides.size()
					|| value_index < 0
					|| static_cast<uint32_t>(value_index)
						>= active_value_overrides().size()) {
				valid_ = false;
				continue;
			}
			IRValueRef assignment_result = assigned;
			if (machine_pointer_kind(machine_kind(assigned))) {
				uint64_t literal_length = 0;
				bool literal_truthy = false;
				const bool string_literal = known_string_literal(
					assigned, &literal_length, &literal_truthy);
				const uint8_t literal_first_byte = string_literal
						&& literal_length == 1 && !literal_truthy
					? '0' : 0;
				assignment_result = add_derived_value(
					ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
					exact_type(assigned),
					instruction.value_operation.op1_storage_id,
					false, 0, machine_kind(assigned),
					ownership(assigned), refcount_state(assigned),
					UINT32_MAX, string_literal, literal_first_byte,
					literal_length);
				if (assignment_result == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
			}
			register_assignment_sources[i] = assigned;
			register_assignment_results[i] = assignment_result;
			source_overrides[definition] = assignment_result;
			active_value_overrides()[static_cast<uint32_t>(value_index)] =
				assignment_result;
		}

		/*
		 * Guarded in-place mutations are discovered after their loop PHIs in
		 * MIR instruction order.  Select their boxed machine results first so
		 * a loop-carried zval identity can be represented as a real TPDE PHI
		 * instead of remaining a canonical-frame reload cycle.  An earlier
		 * result-less mutation of the same non-reference-bound storage must use
		 * that identity too, otherwise the PHI observes the guarded result on
		 * only one edge.
		 */
		std::vector<IRValueRef> mutation_results(
			plan_->instruction_count, INVALID_VALUE_REF);
		std::vector<zend_mir_storage_id> lazy_mutation_storages;
		std::vector<zend_mir_storage_id> related_mutation_storages;
		auto select_mutation_result = [&](uint32_t i) {
			if (mutation_results[i] != INVALID_VALUE_REF) {
				return;
			}
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const IRValueRef canonical =
				mutation_value_ref(instruction);
			if (canonical == INVALID_VALUE_REF) {
				return;
			}
			if (representation(canonical)
						!= ZEND_MIR_REPRESENTATION_ZVAL
					|| !zend_mir_id_is_valid(
						canonical_storage(canonical))
					|| canonical_storage(canonical)
						!= instruction.value_operation.op1_storage_id
					|| instruction.value_operation
						.op1_definition_ssa_variable_id_plus_one == 0) {
				valid_ = false;
				return;
			}
			const zend_mir_value_id mutation_ssa =
				instruction.value_operation
					.op1_definition_ssa_variable_id_plus_one - 1;
			const int32_t mutation_index = zend_tpde_value_index(
				plan_, zend_mir_value_from_original_ssa(mutation_ssa));
			const IRValueRef mutation = add_derived_value(
				ZEND_MIR_REPRESENTATION_ZVAL,
				ZEND_MIR_SCALAR_TYPE_NONE,
				instruction.value_operation.op1_storage_id,
				false, 0, ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				ownership(canonical), refcount_state(canonical));
			if (mutation == INVALID_VALUE_REF || mutation_index < 0
					|| static_cast<uint32_t>(mutation_index)
						>= active_value_overrides().size()
					|| mutation_ssa
						>= active_source_ssa_overrides().size()) {
				valid_ = false;
				return;
			}
			active_value_overrides()[
				static_cast<uint32_t>(mutation_index)] = mutation;
			active_source_ssa_overrides()[mutation_ssa] = mutation;
			active_instruction_results()[i] = mutation;
			mutation_results[i] = mutation;
			lazy_mutation_storages.push_back(
				instruction.value_operation.op1_storage_id);
		};
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			if (instruction.mutation_lazy_scalar
					&& guarded_cold_blocks[i] != UINT32_MAX
					&& mutation_value_ref(instruction)
						!= INVALID_VALUE_REF) {
				related_mutation_storages.push_back(
					instruction.value_operation.op1_storage_id);
			}
		}
		std::ranges::sort(related_mutation_storages);
		related_mutation_storages.erase(
			std::unique(related_mutation_storages.begin(),
				related_mutation_storages.end()),
			related_mutation_storages.end());
		std::erase_if(related_mutation_storages,
			[&](zend_mir_storage_id storage_id) {
				return storage_assigned_by_reference(storage_id);
			});
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const bool guarded_mutation =
				instruction.mutation_lazy_scalar
				&& guarded_cold_blocks[i] != UINT32_MAX;
			const bool related_mutation =
				instruction.has_value_operation
					&& std::ranges::binary_search(
						related_mutation_storages,
						instruction.value_operation.op1_storage_id);
			if (guarded_mutation || related_mutation) {
				select_mutation_result(i);
			}
		}
		lazy_mutation_storages.insert(
			lazy_mutation_storages.end(),
			register_boxed_storages.begin(),
			register_boxed_storages.end());
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			if (instruction.zval_store_lazy_scalar
					&& zend_mir_id_is_valid(
						instruction.zval_store_storage_id)) {
				lazy_mutation_storages.push_back(
					instruction.zval_store_storage_id);
			}
		}
		std::ranges::sort(lazy_mutation_storages);
		lazy_mutation_storages.erase(
			std::unique(lazy_mutation_storages.begin(),
				lazy_mutation_storages.end()),
			lazy_mutation_storages.end());

		struct PendingPhiInput {
			InstKind kind;
			uint32_t block;
			IRValueRef source;
			IRValueRef result;
			zend_mir_storage_id storage_id;
			zend_mir_scalar_type_mask exact_type;
			bool emitted = false;
		};
		std::vector<IRValueRef> boxed_phi_input_overrides(
			plan_->instruction_operand_count, INVALID_VALUE_REF);
		std::vector<IRValueRef> boxed_phi_cold_input_overrides(
			plan_->instruction_operand_count, INVALID_VALUE_REF);
		std::vector<PendingPhiInput> pending_phi_inputs;
		std::vector<IRValueRef> selected_phi_results =
			preselected_phi_results;
		auto boxed_phi_needs_scalar_transport = [&](
				const zend_tpde_instruction &instruction,
				zend_mir_storage_id result_storage) {
			if (!zend_mir_id_is_valid(result_storage)) {
				return false;
			}
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const zend_mir_value_id input_id =
					zend_tpde_operand_at(plan_, &instruction, operand);
				const IRValueRef input = value_ref(input_id);
				if (input == INVALID_VALUE_REF
						|| canonical_storage(input) == result_storage) {
					continue;
				}
				const zend_mir_scalar_type_mask input_type = exact_type(input);
				const bool boxable_scalar =
					(input_type == ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(input)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (input_type == ZEND_MIR_SCALAR_TYPE_I1
						&& machine_kind(input)
							== ZEND_TPDE_MACHINE_VALUE_BOOL)
					|| (input_type == ZEND_MIR_SCALAR_TYPE_F64
						&& machine_kind(input)
							== ZEND_TPDE_MACHINE_VALUE_F64);
				uint64_t constant_bits;
				if (boxable_scalar
						&& ((machine_value_is_register_authoritative(input)
								&& machine_value_has_register_definition(input))
							|| constant(input, &constant_bits))) {
					return true;
				}
			}
			return false;
		};
		std::vector<uint8_t> runtime_short_circuit_result_values(
			plan_->value_count, 0);
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &producer = plan_->instructions[i];
			const int32_t result_index =
				producer.source_result_binding.value_index;
			if (producer.record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					&& producer.has_value_operation
					&& result_index >= 0
					&& static_cast<uint32_t>(result_index)
						< plan_->value_count
					&& (producer.value_operation.source_opcode
							== ZEND_COALESCE
						|| producer.value_operation.source_opcode
							== ZEND_JMP_SET)) {
				runtime_short_circuit_result_values[
					static_cast<uint32_t>(result_index)] = 1;
			}
		}
		auto phi_uses_runtime_short_circuit_slot = [&] (
				const zend_tpde_instruction &instruction,
				zend_mir_storage_id result_storage) {
			if (!zend_mir_id_is_valid(result_storage)) {
				return false;
			}
			bool has_runtime_short_circuit_input = false;
			for (uint32_t operand = 0;
					operand < instruction.operand_count; ++operand) {
				const zend_mir_value_id input_id =
					zend_tpde_operand_at(plan_, &instruction, operand);
				const int32_t input_index =
					zend_tpde_value_index(plan_, input_id);
				const IRValueRef input = value_ref(input_id);
				if (input_index < 0 || input == INVALID_VALUE_REF
						|| canonical_storage(input) != result_storage) {
					return false;
				}
				const int32_t definition =
					plan_->value_definition_instructions != nullptr
					? plan_->value_definition_instructions[input_index] : -1;
				bool runtime_short_circuit_input = false;
				if (definition >= 0
						&& static_cast<uint32_t>(definition)
							< plan_->instruction_count) {
					const zend_tpde_instruction &producer =
						plan_->instructions[static_cast<uint32_t>(definition)];
					runtime_short_circuit_input =
						producer.record.opcode
							== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						&& producer.has_value_operation
						&& (producer.value_operation.source_opcode
								== ZEND_COALESCE
							|| producer.value_operation.source_opcode
								== ZEND_JMP_SET);
				}
				if (!runtime_short_circuit_input) {
					runtime_short_circuit_input =
						runtime_short_circuit_result_values[
							static_cast<uint32_t>(input_index)] != 0;
				}
				has_runtime_short_circuit_input |=
					runtime_short_circuit_input;
			}
			return has_runtime_short_circuit_input;
		};
		/*
		 * Register-authoritative assignments and lazy scalar mutations may close
		 * an outer loop through one or more nested ZVAL PHIs.  Those PHIs are not
		 * emitted in def-use order, so give every PHI for that CV its final
		 * machine identity before resolving any incoming edge.  Otherwise an
		 * inner PHI encountered before its outer input PHI reloads the stale
		 * canonical slot instead of consuming the loop-carried register value.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (record.opcode != ZEND_MIR_OPCODE_PHI
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL) {
				continue;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan_, record.result_id);
			const IRValueRef canonical_result =
				result_index < 0 ? INVALID_VALUE_REF
					: IRValueRef{
						MIR_VALUE_BASE
							+ static_cast<uint32_t>(result_index)};
			const zend_mir_storage_id storage_id =
				canonical_result == INVALID_VALUE_REF
					? ZEND_MIR_ID_INVALID
					: canonical_storage(canonical_result);
			const IRValueRef assignment_source =
				register_assignment_phi_source(storage_id);
			const bool scalar_transport_phi =
				boxed_phi_needs_scalar_transport(
					plan_->instructions[i], storage_id);
			const bool runtime_short_circuit_slot_phi =
				phi_uses_runtime_short_circuit_slot(
					plan_->instructions[i], storage_id);
			const bool boxed_phi =
				zend_mir_id_is_valid(storage_id)
				&& !runtime_short_circuit_slot_phi
				&& (scalar_transport_phi
					|| std::binary_search(
						lazy_mutation_storages.begin(),
						lazy_mutation_storages.end(), storage_id));
			const bool pointer_phi =
				assignment_source != INVALID_VALUE_REF
				&& canonical_result != INVALID_VALUE_REF
				&& machine_pointer_kind(
					machine_kind(canonical_result))
				&& machine_kind(canonical_result)
					== machine_kind(assignment_source)
				&& machine_value_is_register_authoritative(
					canonical_result);
			if (!zend_mir_id_is_valid(storage_id)
					|| canonical_result == INVALID_VALUE_REF
					|| exact_type(canonical_result)
						== ZEND_MIR_SCALAR_TYPE_NULL
					|| (!pointer_phi && !boxed_phi)) {
				continue;
			}
			const IRValueRef selected_phi = add_derived_value(
				pointer_phi
					? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
					: ZEND_MIR_REPRESENTATION_ZVAL,
				pointer_phi
					? exact_type(assignment_source)
					: ZEND_MIR_SCALAR_TYPE_NONE,
				storage_id, false, 0,
				pointer_phi
					? machine_kind(assignment_source)
					: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
				pointer_phi
					? ZEND_MIR_OWNERSHIP_STATE_BORROWED
					: ownership(canonical_result),
				pointer_phi
					? ZEND_MIR_REFCOUNT_UNKNOWN
					: refcount_state(canonical_result));
			if (selected_phi == INVALID_VALUE_REF
					|| result_index < 0
					|| static_cast<uint32_t>(result_index)
						>= active_value_overrides().size()) {
				valid_ = false;
				continue;
			}
			selected_phi_results[i] = selected_phi;
		}
		auto value_ref_for_block = [&](zend_mir_value_id value_id,
				uint32_t use_block, uint32_t use_instruction) {
			const int32_t value_index = zend_tpde_value_index(plan_, value_id);
			if (value_index < 0) {
				return INVALID_VALUE_REF;
			}
			const IRValueRef canonical{
				MIR_VALUE_BASE + static_cast<uint32_t>(value_index)};
			const IRValueRef selected = value_ref(value_id);
			if (selected == INVALID_VALUE_REF || selected == canonical) {
				return selected;
			}
			for (uint32_t definition = 0;
					definition < active_instruction_results().size(); ++definition) {
				if (active_instruction_results()[definition] != selected
						|| !machine_block_dominates(
							instruction_blocks[definition], use_block)
						|| (instruction_blocks[definition] == use_block
							&& use_instruction != UINT32_MAX
							&& definition > use_instruction)) {
					continue;
				}
				return selected;
			}
			for (uint32_t definition = 0;
					definition < selected_phi_results.size(); ++definition) {
				if (selected_phi_results[definition] == selected
						&& machine_block_dominates(
							instruction_blocks[definition], use_block)) {
					return selected;
				}
			}
			return canonical;
		};
		auto entry_argument_has_register_definition =
				[&](IRValueRef value) {
			if (function_mode_ != FunctionMode::ZendEntry) {
				return false;
			}
			const uint32_t raw = static_cast<uint32_t>(value);
			if (raw < MIR_VALUE_BASE
					|| raw - MIR_VALUE_BASE >= plan_->value_count) {
				return false;
			}
			const uint32_t value_index = raw - MIR_VALUE_BASE;
			const zend_tpde_value &plan_value = plan_->values[value_index];
			if (plan_value.argument_index < 0
					|| plan_value.representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				return false;
			}
			const TypedBodyAbiType argument_abi =
				typed_body_value_abi(plan_, value_index);
			return argument_abi.valid
				&& ((zend_mir_scalar_type_is_exact(argument_abi.exact_type)
						&& argument_abi.exact_type
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
					|| argument_abi.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		};
		auto resolve_materializable_scalar = [&](
				uint32_t value_index, uint32_t use_block,
				uint32_t use_instruction, uint32_t depth,
				auto &&self) -> IRValueRef {
			if (value_index >= plan_->value_count
					|| depth > plan_->value_count) {
				return INVALID_VALUE_REF;
			}
			const zend_tpde_value &plan_value = plan_->values[value_index];
			IRValueRef value = value_ref_for_block(
				plan_value.id, use_block, use_instruction);
			const int32_t definition =
				plan_->value_definition_instructions == nullptr
					? -1
					: plan_->value_definition_instructions[value_index];
			if (definition >= 0
					&& static_cast<uint32_t>(definition)
						< plan_->instruction_count) {
				const zend_tpde_instruction &copy =
					plan_->instructions[static_cast<uint32_t>(definition)];
				const zend_mir_instruction_record copy_record =
					instruction_record_at(static_cast<uint32_t>(definition));
				if (copy_record.opcode == ZEND_MIR_OPCODE_COPY
						&& copy.operand_count == 1
						&& !plan_value.canonical_alias_observable
						&& plan_value.representation
							== ZEND_MIR_REPRESENTATION_ZVAL) {
					const zend_mir_value_id input_id =
						zend_tpde_operand_at(plan_, &copy, 0);
					const int32_t input_index =
						zend_tpde_value_index(plan_, input_id);
					const IRValueRef copied_input = value_ref(input_id);
					if (input_index >= 0
							&& copied_input != INVALID_VALUE_REF
							&& plan_->values[input_index].representation
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& zend_mir_id_is_valid(
								plan_value.canonical_storage_id)
							&& plan_->values[input_index].canonical_storage_id
								== plan_value.canonical_storage_id
							&& std::ranges::find(register_assignment_results,
								copied_input)
								!= register_assignment_results.end()) {
						return self(static_cast<uint32_t>(input_index),
							use_block, use_instruction, depth + 1, self);
					}
				}
			}
			if (machine_value_has_result_representation(value)
					&& (machine_value_has_register_definition(value)
						|| entry_argument_has_register_definition(value))) {
				bool assignment_result = false;
				bool assignment_dominates = false;
				for (uint32_t assignment_index = 0;
						assignment_index < register_assignment_results.size();
						++assignment_index) {
					if (register_assignment_results[assignment_index] != value) {
						continue;
					}
					assignment_result = true;
					const uint32_t assignment_block =
						guarded_continuation_blocks[assignment_index]
								!= UINT32_MAX
							? guarded_continuation_blocks[assignment_index]
							: instruction_blocks[assignment_index];
					assignment_dominates |= machine_block_dominates(
						assignment_block, use_block);
				}
				if (assignment_result && !assignment_dominates) {
					return INVALID_VALUE_REF;
				}
				return value;
			}
			if (definition < 0
					|| static_cast<uint32_t>(definition)
						>= plan_->instruction_count) {
				return INVALID_VALUE_REF;
			}
			const uint32_t definition_index =
				static_cast<uint32_t>(definition);
			if (definition_index < transient_scalar_results.size()
					&& transient_scalar_results[definition_index]
						!= INVALID_VALUE_REF) {
				return transient_scalar_results[definition_index];
			}
			/*
			 * Boxed PHIs are selected before the instruction stream is built so
			 * their incoming-edge conversions can be frozen. A statepoint may use
			 * that PHI before active_instruction_results records its later linear
			 * instruction. The selected value is nevertheless defined at block
			 * entry and is the authoritative materialization source.
			 */
			if (definition_index < selected_phi_results.size()) {
				const IRValueRef selected =
					selected_phi_results[definition_index];
				if (selected != INVALID_VALUE_REF
						&& machine_value_has_result_representation(selected)
						&& machine_value_has_register_definition(selected)
						&& machine_block_dominates(
							instruction_blocks[definition_index], use_block)) {
					return selected;
				}
			}
			if (definition_index < active_instruction_results().size()) {
				const IRValueRef selected =
					active_instruction_results()[definition_index];
				if (selected != value
						&& machine_value_has_result_representation(selected)
						&& machine_value_has_register_definition(selected)
						&& machine_block_dominates(
							instruction_blocks[definition_index], use_block)
						&& (instruction_blocks[definition_index] != use_block
							|| use_instruction == UINT32_MAX
							|| definition_index <= use_instruction)) {
					return selected;
				}
			}
			const zend_tpde_instruction &instruction =
				plan_->instructions[definition_index];
			const zend_mir_instruction_record record =
				instruction_record_at(definition_index);
			if (record.opcode != ZEND_MIR_OPCODE_COPY
					|| instruction.operand_count != 1
					|| plan_value.canonical_alias_observable
					|| !zend_mir_scalar_type_is_exact(plan_value.exact_type)
					|| plan_value.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				return INVALID_VALUE_REF;
			}
			const zend_mir_value_id input_id =
				zend_tpde_operand_at(plan_, &instruction, 0);
			const int32_t input_index =
				zend_tpde_value_index(plan_, input_id);
			if (input_index < 0
					|| plan_->values[input_index].exact_type
					!= plan_value.exact_type) {
				return INVALID_VALUE_REF;
			}
			/*
			 * A COPY may move an exact scalar from a literal or register into a
			 * different canonical slot. Scalar-definition freezing can also
			 * promote a formerly boxed COPY before adaptor construction. Trace
			 * either form so forward statepoint materializations reach the actual
			 * PHI, argument, or constant definition.
			 */
			const IRValueRef resolved = self(
				static_cast<uint32_t>(input_index), use_block,
				use_instruction, depth + 1, self);
			if (resolved == INVALID_VALUE_REF
					|| (!machine_value_has_result_representation(resolved)
						&& plan_->values[input_index].canonical_storage_id
							!= plan_value.canonical_storage_id)) {
				return INVALID_VALUE_REF;
			}
			return resolved;
		};
		auto resolve_copy_input = [&](zend_mir_value_id value_id,
				uint32_t use_block, uint32_t use_instruction) {
				const int32_t value_index =
					zend_tpde_value_index(plan_, value_id);
				if (value_index < 0) {
					return value_ref(value_id);
				}
				return resolve_materializable_scalar(
					static_cast<uint32_t>(value_index), use_block,
					use_instruction, 0,
					resolve_materializable_scalar);
			};
		auto phi_edge_value_ref = [&](zend_mir_value_id value_id,
				uint32_t predecessor_block) {
			const int32_t value_index = zend_tpde_value_index(plan_, value_id);
			const int32_t definition = value_index < 0
					|| plan_->value_definition_instructions == nullptr
				? -1
				: plan_->value_definition_instructions[value_index];
			if (definition >= 0
					&& static_cast<uint32_t>(definition)
						< selected_phi_results.size()) {
				const IRValueRef selected = selected_phi_results[
					static_cast<uint32_t>(definition)];
				if (selected != INVALID_VALUE_REF
						&& machine_block_dominates(
							instruction_blocks[definition], predecessor_block)) {
					return selected;
				}
			}
			return value_ref(value_id);
		};
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (record.opcode != ZEND_MIR_OPCODE_PHI
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL) {
				continue;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan_, record.result_id);
			const IRValueRef canonical_result =
				result_index < 0 ? INVALID_VALUE_REF
					: IRValueRef{
						MIR_VALUE_BASE
							+ static_cast<uint32_t>(result_index)};
			const zend_mir_storage_id storage_id =
				canonical_result == INVALID_VALUE_REF
					? ZEND_MIR_ID_INVALID
					: canonical_storage(canonical_result);
			IRValueRef selected_phi = selected_phi_results[i];
			const zend_tpde_machine_value_kind phi_machine_kind =
				selected_phi != INVALID_VALUE_REF
					? machine_kind(selected_phi)
				: canonical_result == INVALID_VALUE_REF
					? ZEND_TPDE_MACHINE_VALUE_I64
					: machine_kind(canonical_result);
			const bool pointer_phi =
				(selected_phi != INVALID_VALUE_REF
					&& machine_pointer_kind(phi_machine_kind))
				|| (selected_phi == INVALID_VALUE_REF
					&& canonical_result != INVALID_VALUE_REF
					&& machine_pointer_kind(phi_machine_kind)
					&& machine_value_is_register_authoritative(
						canonical_result));
			const bool boxed_phi =
				!phi_uses_runtime_short_circuit_slot(instruction, storage_id)
				&& ((selected_phi != INVALID_VALUE_REF
					&& machine_kind(selected_phi)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
				|| (canonical_result != INVALID_VALUE_REF
					&& std::binary_search(
						lazy_mutation_storages.begin(),
						lazy_mutation_storages.end(), storage_id)));
			if (!zend_mir_id_is_valid(storage_id)
					|| (selected_phi == INVALID_VALUE_REF && result_index >= 0
						&& machine_value_used[
							static_cast<uint32_t>(result_index)] == 0)
					|| (!pointer_phi && !boxed_phi)) {
				continue;
			}
			const int32_t source_block = block_index(record.block_id);
			if (source_block < 0) {
				valid_ = false;
				continue;
			}
			const uint32_t predecessor_begin =
				plan_->block_predecessor_offsets[source_block];
			const uint32_t predecessor_count =
				plan_->block_predecessor_offsets[source_block + 1]
					- predecessor_begin;
			if (predecessor_count != instruction.operand_count
					|| instruction.operand_offset
						> plan_->instruction_operand_count
					|| instruction.operand_count
						> plan_->instruction_operand_count
							- instruction.operand_offset) {
				valid_ = false;
				continue;
			}

			std::vector<IRValueRef> inputs(
				predecessor_count, INVALID_VALUE_REF);
			std::vector<IRValueRef> cold_inputs(
				predecessor_count, INVALID_VALUE_REF);
			std::vector<PendingPhiInput> conversions;
			bool supported = true;
			for (uint32_t n = 0; n < predecessor_count; ++n) {
				const uint32_t predecessor =
					plan_->block_predecessors[predecessor_begin + n];
				IRValueRef input = predecessor < plan_->block_count
					? phi_edge_value_ref(
						zend_tpde_operand_at(plan_, &instruction, n),
						final_blocks[predecessor])
					: INVALID_VALUE_REF;
				const int32_t input_index = zend_tpde_value_index(
					plan_, zend_tpde_operand_at(plan_, &instruction, n));
				if (input_index >= 0) {
					const IRValueRef materializable =
						resolve_materializable_scalar(
							static_cast<uint32_t>(input_index),
							final_blocks[predecessor], UINT32_MAX, 0,
							resolve_materializable_scalar);
					if (materializable != INVALID_VALUE_REF) {
						input = materializable;
					}
				}
				if (input == INVALID_VALUE_REF
						|| predecessor >= plan_->block_count) {
					supported = false;
					break;
				}
				if (machine_value_is_register_authoritative(input)
						&& machine_kind(input)
							== (pointer_phi
								? phi_machine_kind
								: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
						&& machine_value_has_register_definition(input)) {
					inputs[n] = input;
					cold_inputs[n] = input;
					continue;
				}

				InstKind conversion_kind;
				IRValueRef conversion_source = input;
				const zend_mir_scalar_type_mask input_type =
					exact_type(input);
				uint64_t constant_bits;
				const bool scalar_input =
					(input_type == ZEND_MIR_SCALAR_TYPE_I64
							&& machine_kind(input)
								== ZEND_TPDE_MACHINE_VALUE_I64)
						|| (input_type == ZEND_MIR_SCALAR_TYPE_I1
							&& machine_kind(input)
								== ZEND_TPDE_MACHINE_VALUE_BOOL)
						|| (input_type == ZEND_MIR_SCALAR_TYPE_F64
							&& machine_kind(input)
								== ZEND_TPDE_MACHINE_VALUE_F64);
				if (!pointer_phi && scalar_input
						&& ((machine_value_is_register_authoritative(input)
								&& (machine_value_has_register_definition(input)
									|| entry_argument_has_register_definition(input)))
							|| constant(input, &constant_bits))) {
					conversion_kind = InstKind::BoxScalar;
				} else {
					const uint32_t reference = machine_reference_index(
						ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
						storage_id);
					if (canonical_storage(input) != storage_id
							|| reference == UINT32_MAX) {
						supported = false;
						break;
					}
					conversion_source = add_derived_value(
						ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
						ZEND_MIR_SCALAR_TYPE_NONE, storage_id,
						false, 0, UINT8_MAX,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN, reference);
					conversion_kind = InstKind::ZvalPayloadLoad;
				}
				if (conversion_source == INVALID_VALUE_REF) {
					supported = false;
					break;
				}
				const IRValueRef boxed = add_derived_value(
					pointer_phi
						? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
						: ZEND_MIR_REPRESENTATION_ZVAL,
					input_type,
					storage_id, false, 0,
					pointer_phi
						? phi_machine_kind
						: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
					ownership(input), refcount_state(input));
				if (boxed == INVALID_VALUE_REF) {
					supported = false;
					break;
				}
				inputs[n] = boxed;
				cold_inputs[n] = boxed;
				conversions.push_back({
					conversion_kind, final_blocks[predecessor],
					conversion_source, boxed,
					storage_id, input_type, false});
				const uint32_t cold_predecessor =
					boxed_cond_cold_by_predecessor[predecessor];
				if (cold_predecessor != UINT32_MAX) {
						const IRValueRef cold_boxed = add_derived_value(
							pointer_phi
								? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
								: ZEND_MIR_REPRESENTATION_ZVAL,
						input_type,
						storage_id, false, 0,
						pointer_phi
							? phi_machine_kind
							: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
						ownership(input), refcount_state(input));
					if (cold_boxed == INVALID_VALUE_REF) {
						supported = false;
						break;
					}
					cold_inputs[n] = cold_boxed;
					conversions.push_back({
						conversion_kind, cold_predecessor,
						conversion_source, cold_boxed,
						storage_id, input_type, false});
				}
			}
			if (!supported) {
				continue;
			}
			if (selected_phi == INVALID_VALUE_REF) {
				selected_phi = add_derived_value(
					pointer_phi
						? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
						: ZEND_MIR_REPRESENTATION_ZVAL,
					pointer_phi
						? exact_type(canonical_result)
						: ZEND_MIR_SCALAR_TYPE_NONE,
					storage_id, false, 0,
					pointer_phi
						? phi_machine_kind
						: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
					pointer_phi
						? ZEND_MIR_OWNERSHIP_STATE_BORROWED
						: ownership(canonical_result),
					pointer_phi
						? ZEND_MIR_REFCOUNT_UNKNOWN
						: refcount_state(canonical_result));
				if (selected_phi == INVALID_VALUE_REF
						|| result_index < 0
						|| static_cast<uint32_t>(result_index)
							>= active_value_overrides().size()) {
					valid_ = false;
					continue;
				}
				selected_phi_results[i] = selected_phi;
			}
			for (uint32_t n = 0; n < predecessor_count; ++n) {
				boxed_phi_input_overrides[
					instruction.operand_offset + n] = inputs[n];
				boxed_phi_cold_input_overrides[
					instruction.operand_offset + n] = cold_inputs[n];
			}
			pending_phi_inputs.insert(pending_phi_inputs.end(),
				conversions.begin(), conversions.end());
		}
		/*
		 * An exact scalar can remain canonical-slot-backed when its defining
		 * source opcode did not produce a register value.  TPDE PHIs require a
		 * real machine definition on every incoming edge, so load such payloads
		 * in their predecessor blocks before constructing the scalar PHI.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (record.opcode != ZEND_MIR_OPCODE_PHI) {
				continue;
			}
			const int32_t result_index =
				zend_tpde_value_index(plan_, record.result_id);
			const IRValueRef result = result_index < 0
				? INVALID_VALUE_REF
				: value_ref(record.result_id);
			if (result == INVALID_VALUE_REF
					|| representation(result)
						== ZEND_MIR_REPRESENTATION_ZVAL
					|| machine_pointer_kind(machine_kind(result))
					|| !zend_mir_scalar_type_is_exact(exact_type(result))
					|| exact_type(result) == ZEND_MIR_SCALAR_TYPE_NULL
					|| !machine_value_has_result_representation(result)) {
				continue;
			}
			const int32_t source_block = block_index(record.block_id);
			if (source_block < 0) {
				valid_ = false;
				continue;
			}
			const uint32_t predecessor_begin =
				plan_->block_predecessor_offsets[source_block];
			const uint32_t predecessor_count =
				plan_->block_predecessor_offsets[source_block + 1]
					- predecessor_begin;
			if (predecessor_count != instruction.operand_count) {
				valid_ = false;
				continue;
			}
			for (uint32_t n = 0; n < predecessor_count; ++n) {
				const uint32_t operand_index = instruction.operand_offset + n;
				IRValueRef input = value_ref(
					zend_tpde_operand_at(plan_, &instruction, n));
				const int32_t input_index = zend_tpde_value_index(
					plan_, zend_tpde_operand_at(plan_, &instruction, n));
				const bool registerless_source = input_index >= 0
					&& (plan_->value_definition_instructions == nullptr
						|| plan_->value_definition_instructions[input_index] < 0);
				if (input_index >= 0) {
					const IRValueRef materializable =
						resolve_materializable_scalar(
							static_cast<uint32_t>(input_index),
							final_blocks[plan_->block_predecessors[
								predecessor_begin + n]], UINT32_MAX, 0,
							resolve_materializable_scalar);
					if (materializable != INVALID_VALUE_REF) {
						input = materializable;
					}
				}
				uint64_t constant_bits;
				if (input == INVALID_VALUE_REF
						|| constant(input, &constant_bits)
						|| machine_value_has_register_definition(input)) {
					continue;
				}
				const bool matching_scalar =
					exact_type(input) == exact_type(result)
					&& machine_kind(input) == machine_kind(result);
				const bool canonical_boxed =
					representation(input) == ZEND_MIR_REPRESENTATION_ZVAL
					&& machine_kind(input)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& canonical_storage(input)
						== canonical_storage(result);
				if ((!matching_scalar && !canonical_boxed)
						|| (!registerless_source && !canonical_boxed)) {
					continue;
				}
				const zend_mir_storage_id storage_id =
					canonical_storage(input);
				const uint32_t reference = zend_mir_id_is_valid(storage_id)
					? machine_reference_index(
						ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT, storage_id)
					: UINT32_MAX;
				const IRValueRef address = reference != UINT32_MAX
					? add_derived_value(
						ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
						ZEND_MIR_SCALAR_TYPE_NONE, storage_id, false, 0,
						UINT8_MAX, ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN, reference)
					: INVALID_VALUE_REF;
				const IRValueRef loaded = address != INVALID_VALUE_REF
					? add_derived_value(
						representation(result), exact_type(result), storage_id,
						false, 0, machine_kind(result),
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN)
					: INVALID_VALUE_REF;
				const uint32_t predecessor =
					plan_->block_predecessors[predecessor_begin + n];
				if (loaded == INVALID_VALUE_REF
						|| predecessor >= plan_->block_count) {
					continue;
				}
				boxed_phi_input_overrides[operand_index] = loaded;
				pending_phi_inputs.push_back({
					InstKind::ZvalPayloadLoad, final_blocks[predecessor],
					address, loaded, storage_id, exact_type(result), false});
				const uint32_t cold_predecessor =
					boxed_cond_cold_by_predecessor[predecessor];
				if (cold_predecessor != UINT32_MAX) {
					const IRValueRef cold_loaded = add_derived_value(
						representation(result), exact_type(result), storage_id,
						false, 0, machine_kind(result),
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN);
					if (cold_loaded == INVALID_VALUE_REF) {
						continue;
					}
					boxed_phi_cold_input_overrides[operand_index] = cold_loaded;
					pending_phi_inputs.push_back({
						InstKind::ZvalPayloadLoad, cold_predecessor,
						address, cold_loaded, storage_id,
						exact_type(result), false});
				}
			}
		}
		/*
		 * Source short-circuit temporaries have no MIR PHI record.  A frozen
		 * register merge identifies the two exact boolean definitions reaching
		 * the terminal source branch; publish their merge as an ordinary TPDE
		 * PHI so the branch never consults the unmaterialized zval slot.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[i];
			if (machine_block_reachable[instruction_blocks[i]] == 0) {
				continue;
			}
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const int32_t result_index =
				instruction.source_op1_binding.value_index;
			if ((instruction.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_MERGE) == 0
					|| result_index < 0
					|| static_cast<uint32_t>(result_index)
						>= plan_->value_count
					|| !zend_mir_id_is_valid(
						instruction.value_operation.op1_storage_id)) {
				continue;
			}
			const int32_t source_block = block_index(record.block_id);
			if (source_block < 0) {
				valid_ = false;
				continue;
			}
			const uint32_t predecessor_begin =
				plan_->block_predecessor_offsets[
					static_cast<uint32_t>(source_block)];
			const uint32_t predecessor_end =
				plan_->block_predecessor_offsets[
					static_cast<uint32_t>(source_block) + 1];
			if (predecessor_end - predecessor_begin != 2) {
				valid_ = false;
				continue;
			}
			std::array<IRValueRef, 2> inputs{
				INVALID_VALUE_REF, INVALID_VALUE_REF};
			bool supported = true;
			for (uint32_t n = 0; n < inputs.size(); ++n) {
				const uint32_t predecessor =
					plan_->block_predecessors[predecessor_begin + n];
				for (uint32_t candidate = plan_->instruction_count;
						candidate-- > 0;) {
					const zend_tpde_instruction &producer =
						plan_->instructions[candidate];
					const zend_mir_instruction_record producer_record =
						instruction_record_at(candidate);
					if (!producer.has_value_operation
							|| block_index(producer_record.block_id)
								!= static_cast<int32_t>(predecessor)
							|| producer.value_operation.result_storage_id
								!= instruction.value_operation.op1_storage_id) {
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
						&& register_boolean_results[candidate]
							!= INVALID_VALUE_REF;
					if (!register_edge && !register_result) {
						continue;
					}
					const IRValueRef selected =
						active_instruction_results()[candidate];
					if (selected != INVALID_VALUE_REF
							&& exact_type(selected)
								== ZEND_MIR_SCALAR_TYPE_I1
							&& machine_kind(selected)
								== ZEND_TPDE_MACHINE_VALUE_BOOL
							&& machine_value_has_register_definition(selected)) {
						inputs[n] = selected;
					}
					break;
				}
				if (inputs[n] == INVALID_VALUE_REF) {
					supported = false;
					break;
				}
			}
			if (!supported) {
				valid_ = false;
				continue;
			}
			const IRValueRef merged = add_derived_value(
				ZEND_MIR_REPRESENTATION_I1,
				ZEND_MIR_SCALAR_TYPE_I1,
				instruction.value_operation.op1_storage_id,
				false, 0, ZEND_TPDE_MACHINE_VALUE_BOOL,
				ZEND_MIR_OWNERSHIP_STATE_BORROWED,
				ZEND_MIR_REFCOUNT_IMMORTAL);
			if (merged == INVALID_VALUE_REF) {
				valid_ = false;
				continue;
			}
			active_value_overrides()[
				static_cast<uint32_t>(result_index)] = merged;
			block_phis.push_back({instruction_blocks[i], merged});
			phi_values_[static_cast<uint32_t>(merged)] = 1;
			Slice &input_slice =
				phi_input_slices_[static_cast<uint32_t>(merged)];
			input_slice.offset =
				static_cast<uint32_t>(phi_inputs_.size());
			for (uint32_t n = 0; n < inputs.size(); ++n) {
				const uint32_t predecessor =
					plan_->block_predecessors[predecessor_begin + n];
				phi_inputs_.push_back(
					{inputs[n], IRBlockRef{final_blocks[predecessor]}});
				++input_slice.count;
			}
		}
		auto emit_generator_resume = [&](uint32_t block,
				uint32_t source_position, bool exact_source_position) {
			for (uint32_t resume_index = 0;
					resume_index < plan_->generator_resume_count;
					++resume_index) {
				const uint32_t expected_position = exact_source_position
					? plan_->generator_resume_targets[resume_index]
					: plan_->generator_resume_landings[resume_index];
				if (generator_resume_emitted[resume_index] != 0
						|| expected_position != source_position) {
					continue;
				}
				generator_resume_emitted[resume_index] = 1;
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(IRValueRef{FRAME_VALUE});
				const uint32_t resume_value_offset =
					static_cast<uint32_t>(generator_resume_values_.size());
				const auto &source_overrides =
					active_source_ssa_overrides();
				for (uint32_t value = 0;
						value < plan_->value_count; ++value) {
					if (!zend_tpde_generator_resume_value_live(
							plan_, resume_index, value)) {
						continue;
					}
					const zend_tpde_value &plan_value = plan_->values[value];
					IRValueRef candidate = value_ref(plan_value.id);
					if (zend_mir_value_is_original_ssa(plan_value.id)
							&& plan_value.id < source_overrides.size()
							&& source_overrides[plan_value.id]
								!= INVALID_VALUE_REF) {
						candidate = source_overrides[plan_value.id];
					}
					uint64_t constant_bits = 0;
					if (candidate == INVALID_VALUE_REF
							|| !zend_mir_id_is_valid(
								canonical_storage(candidate))
							|| constant(candidate, &constant_bits)
							|| !machine_value_is_register_authoritative(candidate)) {
						continue;
					}
					bool duplicate = false;
					for (uint32_t operand = resume_value_offset;
							operand < generator_resume_values_.size(); ++operand) {
						duplicate = duplicate
							|| generator_resume_values_[operand] == candidate;
					}
					if (!duplicate) {
						generator_resume_values_.push_back(candidate);
					}
				}
				InstNode resume_node{
					InstKind::GeneratorResume,
					UINT32_MAX,
					resume_index,
					INVALID_VALUE_REF,
					{},
					operand_offset,
					static_cast<uint32_t>(operands_.size()) - operand_offset,
					false};
				resume_node.generator_resume_value_offset = resume_value_offset;
				resume_node.generator_resume_value_count =
					static_cast<uint32_t>(generator_resume_values_.size())
						- resume_value_offset;
				add_node(block_instructions, block, std::move(resume_node));
			}
		};
		auto emit_source_landing = [&](uint32_t block,
				uint32_t source_position) {
			if (source_position >= source_landing_emitted.size()
					|| source_landing_emitted[source_position] != 0
					|| source_landing_blocks[source_position] != block) {
				return;
			}
			source_landing_emitted[source_position] = 1;
			/*
			 * A generator continuation names the next Zend source opcode,
			 * which may be implemented by a source call fragment preceding
			 * the next MIR record. Place the machine landing before that
			 * fragment so resume executes the complete source operation.
			 */
			emit_generator_resume(block, source_position, true);
			if (plan_->user_opcode_callbacks) {
				add_node(block_instructions, block, InstNode{
					InstKind::UserOpcodeLanding,
					UINT32_MAX,
					source_position,
					INVALID_VALUE_REF,
					{},
					0,
					0,
					false});
				if (zend_get_user_opcode_handler(
						plan_->source_opcodes[source_position].opcode)
						!= nullptr) {
					const uint32_t operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(IRValueRef{FRAME_VALUE});
					operands_.push_back(
						IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
					operands_.push_back(IRValueRef{FRAME_VALUE});
					operands_.push_back(IRValueRef{FRAME_VALUE});
					for (size_t dispatch_source = 0;
							dispatch_source
								< user_opcode_dispatch_to_sources_.size();
							++dispatch_source) {
						for (uint32_t target = 0;
								target < plan_->user_opcode_target_count;
								++target) {
							for (uint32_t use = 0;
									use < zend_tpde_user_opcode_target_frame_uses(
										plan_->user_opcode_targets[target].kind);
									++use) {
								operands_.push_back(IRValueRef{FRAME_VALUE});
							}
						}
					}
					add_node(block_instructions, block, InstNode{
						InstKind::UserOpcodeGateway,
						UINT32_MAX,
						source_position,
						INVALID_VALUE_REF,
						{},
						operand_offset,
						static_cast<uint32_t>(
							operands_.size() - operand_offset),
						false});
				}
				add_node(block_instructions, block, InstNode{
					InstKind::UserOpcodeDispatch,
					UINT32_MAX,
					source_position,
					INVALID_VALUE_REF,
					{},
					0,
					0,
					false});
			}
			if (const zend_tpde_source_call_phase_entry *phase =
					zend_tpde_source_call_phase_at(plan_, source_position)) {
					auto append_phase = [&](InstKind kind,
							uint32_t argument_index,
							IRValueRef operand = INVALID_VALUE_REF,
							IRValueRef result = INVALID_VALUE_REF,
							bool has_result = false) {
						const uint32_t operand_offset =
							static_cast<uint32_t>(operands_.size());
						operands_.push_back(IRValueRef{FRAME_VALUE});
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
						if (operand != INVALID_VALUE_REF) {
							operands_.push_back(operand);
						}
						InstNode node{
							kind,
							phase->instruction_index,
							argument_index,
							result,
							{},
							operand_offset,
							operand == INVALID_VALUE_REF ? 2u : 3u,
							has_result};
						node.source_position = source_position;
						add_node(block_instructions, block, std::move(node));
					};
					if ((phase->phases
							& ZEND_TPDE_SOURCE_CALL_PHASE_INIT) != 0) {
						append_phase(
							InstKind::UserCallInit, UINT32_MAX);
					}
					if ((phase->phases
							& ZEND_TPDE_SOURCE_CALL_PHASE_SEND) != 0) {
						IRValueRef operand = INVALID_VALUE_REF;
						if ((phase->operand_flags
								& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE)
								!= 0) {
							if (phase->instruction_index
									>= plan_->instruction_count
									|| plan_->call_argument_bindings == nullptr) {
								valid_ = false;
								return;
							}
							const zend_tpde_instruction &instruction =
								plan_->instructions[phase->instruction_index];
							if (phase->argument_index
									>= instruction.call_argument_count) {
								valid_ = false;
								return;
							}
							const uint32_t binding_index =
								instruction.call_argument_offset
								+ phase->argument_index;
							if (binding_index < instruction.call_argument_offset
									|| binding_index
										>= plan_->call_argument_count) {
								valid_ = false;
								return;
							}
							const zend_tpde_source_value_binding &binding =
								plan_->call_argument_bindings[binding_index];
							if (binding.value_index < 0
									|| static_cast<uint32_t>(binding.value_index)
										>= plan_->value_count) {
								valid_ = false;
								return;
							}
							const zend_mir_scalar_type_mask exact_type =
								plan_->values[static_cast<uint32_t>(
									binding.value_index)].exact_type;
							operand = source_binding_value_ref(binding);
							if (operand == INVALID_VALUE_REF
									|| !zend_mir_scalar_type_is_exact(exact_type)
									|| (exact_type != ZEND_MIR_SCALAR_TYPE_I1
										&& exact_type != ZEND_MIR_SCALAR_TYPE_I64
										&& exact_type != ZEND_MIR_SCALAR_TYPE_F64)) {
								valid_ = false;
								return;
							}
						}
						append_phase(InstKind::UserCallSend,
							phase->argument_index, operand);
					}
					if ((phase->phases
							& ZEND_TPDE_SOURCE_CALL_PHASE_CHECK) != 0) {
						append_phase(
							InstKind::UserCallCheck, phase->argument_index);
					}
					if ((phase->phases
							& ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND) != 0) {
						append_phase(
							InstKind::UserCallExpand, UINT32_MAX);
					}
					if ((phase->phases
							& ZEND_TPDE_SOURCE_CALL_PHASE_DO) != 0) {
						const zend_mir_instruction_record record =
							instruction_record_at(phase->instruction_index);
						const IRValueRef result = value_ref(record.result_id);
						const int32_t result_index = zend_tpde_value_index(
							plan_, record.result_id);
						const bool machine_result =
							machine_value_has_result_representation(result)
							&& result_index >= 0
							&& machine_value_used[
								static_cast<uint32_t>(result_index)] != 0;
						append_phase(InstKind::UserCallDo, UINT32_MAX,
							INVALID_VALUE_REF, result, machine_result);
					}
			}
			for (uint32_t instruction_index = 0;
					instruction_index < plan_->instruction_count;
					++instruction_index) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[instruction_index];
				if (!instruction.user_opcode_call_fragments
						|| phased_source_call(instruction_index)
						|| instruction.user_call == nullptr) {
					continue;
				}
				const zend_native_user_call_descriptor *descriptor =
					instruction.user_call;
				bool fragment =
					source_position == descriptor->init_source_position
					|| source_position == descriptor->do_source_position;
				for (uint32_t argument = 0;
						!fragment && argument < descriptor->argument_count;
						++argument) {
					fragment = source_position
						== descriptor->arguments[argument].source_position;
				}
				if (!fragment) {
					continue;
				}
				const bool finish =
					source_position == descriptor->do_source_position;
				const zend_mir_instruction_record record =
					instruction_record_at(instruction_index);
				const IRValueRef result = finish
					? value_ref(record.result_id) : INVALID_VALUE_REF;
				const int32_t result_index = finish
					? zend_tpde_value_index(plan_, record.result_id) : -1;
				const bool machine_result = result != INVALID_VALUE_REF
					&& result_index >= 0
					&& zend_mir_scalar_type_is_exact(exact_type(result))
					&& exact_type(result) != ZEND_MIR_SCALAR_TYPE_NULL
					&& machine_value_used[
						static_cast<uint32_t>(result_index)] != 0;
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(IRValueRef{FRAME_VALUE});
				add_node(block_instructions, block, InstNode{
					InstKind::UserOpcodeCallFragment,
					instruction_index,
					source_position,
					result,
					{},
					operand_offset,
					1,
					machine_result});
				break;
			}
		};
		auto emit_pending_phi_inputs = [&](uint32_t block,
				uint32_t terminator_instruction) {
			for (PendingPhiInput &pending : pending_phi_inputs) {
				if (pending.emitted || pending.block != block) {
					continue;
				}
				const uint32_t operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(pending.source);
				add_node(block_instructions, block, InstNode{
					pending.kind,
					terminator_instruction,
					UINT32_MAX,
					pending.result,
					{},
					operand_offset,
					1,
					true,
					pending.storage_id,
					pending.exact_type,
					true});
				pending.emitted = true;
			}
		};
		auto has_pending_phi_inputs = [&](uint32_t block) {
			return std::ranges::any_of(pending_phi_inputs,
				[&](const PendingPhiInput &pending) {
					return !pending.emitted && pending.block == block;
				});
		};
		std::vector<uint8_t> elided_silence_instructions(
			plan_->instruction_count, 0);
		/*
		 * Constant-folded source expressions can leave an immediately adjacent
		 * BEGIN_SILENCE/END_SILENCE pair with no operation between them.  Saving
		 * and restoring error_reporting is then an identity operation, but two
		 * generic runtime calls per pair make large generated functions exceed
		 * the target section limit.  Remove only closed pairs whose saved value
		 * has no other consumer and whose source positions cannot be observed by
		 * an opcode callback, source-call fragment, debug probe, or generator
		 * continuation.
		 */
		if (!source_landings
				&& plan_->generator_resume_count == 0
				&& plan_->source_opcodes != nullptr
				&& plan_->value_consumer_offsets != nullptr
				&& plan_->value_consumers != nullptr) {
			for (uint32_t i = 0; i + 1 < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &begin = plan_->instructions[i];
				const zend_tpde_instruction &end = plan_->instructions[i + 1];
				const zend_mir_instruction_record begin_record =
					instruction_record_at(i);
				const zend_mir_instruction_record end_record =
					instruction_record_at(i + 1);
				if (begin_record.opcode
						!= ZEND_MIR_OPCODE_VALUE_BEGIN_SILENCE
						|| end_record.opcode
							!= ZEND_MIR_OPCODE_VALUE_END_SILENCE
						|| instruction_blocks[i] == UINT32_MAX
						|| instruction_blocks[i] != instruction_blocks[i + 1]
						|| begin.debug_probe || end.debug_probe
						|| begin.source_effect != 0 || end.source_effect != 0
						|| begin.user_opcode_call_fragments
						|| end.user_opcode_call_fragments
						|| begin_record.source_position_id == UINT32_MAX
						|| end_record.source_position_id
							!= begin_record.source_position_id + 1
						|| end_record.source_position_id
							>= plan_->source_opcode_count
						|| plan_->source_opcodes[
							begin_record.source_position_id].opcode
							!= ZEND_BEGIN_SILENCE
						|| plan_->source_opcodes[
							end_record.source_position_id].opcode
							!= ZEND_END_SILENCE) {
					continue;
				}
				const int32_t saved_value =
					begin.source_result_binding.value_index;
				if (saved_value < 0
						|| static_cast<uint32_t>(saved_value)
							>= plan_->value_count
						|| end.source_op1_binding.value_index != saved_value) {
					continue;
				}
				const uint32_t consumer_begin =
					plan_->value_consumer_offsets[
						static_cast<uint32_t>(saved_value)];
				const uint32_t consumer_end =
					plan_->value_consumer_offsets[
						static_cast<uint32_t>(saved_value) + 1];
				bool closed_pair = consumer_begin != consumer_end;
				for (uint32_t use = consumer_begin;
						closed_pair && use < consumer_end; ++use) {
					closed_pair = plan_->value_consumers[use].instruction_index
						== i + 1;
				}
				if (!closed_pair) {
					continue;
				}
				elided_silence_instructions[i] = 1;
				elided_silence_instructions[i + 1] = 1;
				++i;
			}
		}
		std::vector<int32_t> latest_source_producer_by_ir_value;
		auto record_source_producer = [&](uint32_t instruction_index) {
			if (instruction_index >= active_instruction_results().size()) {
				return;
			}
			const IRValueRef result =
				active_instruction_results()[instruction_index];
			if (result == INVALID_VALUE_REF) {
				return;
			}
			const uint32_t value_index = static_cast<uint32_t>(result);
			if (value_index >= latest_source_producer_by_ir_value.size()) {
				latest_source_producer_by_ir_value.resize(value_index + 1, -1);
			}
			latest_source_producer_by_ir_value[value_index] =
				static_cast<int32_t>(instruction_index);
		};
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			if (i != 0) {
				record_source_producer(i - 1);
			}
			if (elided_silence_instructions[i] != 0) {
				continue;
			}
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const bool boxed_cond_branch =
				is_boxed_cond_branch(instruction);
			if (!zend_mir_id_is_valid(record.id)) {
				valid_ = false;
				continue;
			}
			if (function_mode_ == FunctionMode::TypedBody
					&& instruction.local_abi_transport) {
				continue;
			}
			const uint32_t block = instruction_blocks[i];
			if (block == UINT32_MAX) {
				valid_ = false;
				continue;
			}
			if (machine_block_reachable[block] == 0) {
				continue;
			}
			if (source_landings
					&& plan_->source_opcode_block_indices != nullptr
					&& record.source_position_id
						< plan_->source_opcode_count) {
				const uint32_t source_block =
					plan_->source_opcode_block_indices[
						record.source_position_id];
				if (source_block < source_block_next.size()) {
					const uint32_t source_end =
						plan_->source_block_ends[source_block];
					uint32_t &next_source =
						source_block_next[source_block];
					while (next_source != UINT32_MAX
							&& next_source <= record.source_position_id
							&& next_source < source_end) {
						emit_source_landing(
							static_cast<uint32_t>(block), next_source++);
					}
				}
			}
			emit_generator_resume(static_cast<uint32_t>(block),
				record.source_position_id, false);
			if ((instruction.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_RESULT_ALIAS) != 0) {
				continue;
			}
			if (zend_mir_opcode_is_terminator(record.opcode)) {
				emit_pending_phi_inputs(block, i);
			}
			if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
						|| record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
					&& (instruction.user_opcode_call_fragments
						|| phased_source_call(i))) {
				continue;
			}
			IRValueRef result = value_ref(record.result_id);
			if (i < transient_scalar_results.size()
					&& transient_scalar_results[i] != INVALID_VALUE_REF) {
				result = transient_scalar_results[i];
			}
			if (i < register_boolean_results.size()
					&& register_boolean_results[i] != INVALID_VALUE_REF) {
				result = register_boolean_results[i];
			}
			IRValueRef type_check_input = INVALID_VALUE_REF;
			IRValueRef type_check_result = INVALID_VALUE_REF;
			const ScalarTypeCheckSelection type_check_selection =
				function_mode_ == FunctionMode::TypedBody
						&& record.opcode
							== ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
					? scalar_type_check_selection(
						plan_, instruction,
						&type_check_input, &type_check_result)
					: ScalarTypeCheckSelection::Invalid;
			if (type_check_selection
					!= ScalarTypeCheckSelection::Invalid) {
				const IRValueRef canonical_result = type_check_result;
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				if (type_check_result == INVALID_VALUE_REF
						|| exact_type(type_check_result)
							!= ZEND_MIR_SCALAR_TYPE_I1
						|| representation(type_check_result)
							!= ZEND_MIR_REPRESENTATION_I1) {
					const zend_mir_value_id result_ssa =
						instruction.value_operation.result
							.ssa_variable_id;
					if (result_ssa
							>= typed_body_source_ssa_overrides_.size()) {
						valid_ = false;
					} else {
						type_check_result = add_derived_value(
							ZEND_MIR_REPRESENTATION_I1,
							ZEND_MIR_SCALAR_TYPE_I1,
							instruction.value_operation
								.result_storage_id);
						if (canonical_result != INVALID_VALUE_REF
								&& canonical_index >= MIR_VALUE_BASE
								&& canonical_index - MIR_VALUE_BASE
									< typed_body_value_overrides_.size()) {
							typed_body_value_overrides_[
								canonical_index - MIR_VALUE_BASE] =
									type_check_result;
						}
						typed_body_source_ssa_overrides_[result_ssa] =
							type_check_result;
					}
				}
				result = type_check_result;
			}
			IRValueRef register_condition = INVALID_VALUE_REF;
			const bool register_cond_branch =
				is_register_cond_branch(instruction, &register_condition);
			zend_mir_scalar_type_mask canonical_bool_unary_exact_type =
				ZEND_MIR_SCALAR_TYPE_NONE;
			IRValueRef register_bool_unary_operand = INVALID_VALUE_REF;
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
					&& instruction.has_value_operation
					&& (instruction.machine_control_flow_flags
						& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT) != 0
					&& (instruction.value_operation.source_opcode == ZEND_BOOL
						|| instruction.value_operation.source_opcode
							== ZEND_BOOL_NOT)) {
				IRValueRef candidate = source_binding_value_ref(
					instruction.source_op1_binding);
				if ((candidate == INVALID_VALUE_REF
						|| exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_NONE)
						&& instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one != 0) {
					candidate = value_ref(zend_mir_value_from_original_ssa(
						instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one - 1));
				}
				if (candidate == INVALID_VALUE_REF) {
					candidate = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				if (candidate != INVALID_VALUE_REF
						&& exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_I1
						&& machine_value_is_register_authoritative(candidate)
						&& machine_value_has_register_definition(candidate)
						&& machine_kind(candidate)
							!= ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
					canonical_bool_unary_exact_type =
						ZEND_MIR_SCALAR_TYPE_I1;
					register_bool_unary_operand = candidate;
				}
			}
			IRValueRef register_boxed_condition_operand = INVALID_VALUE_REF;
			IRValueRef register_string_condition_operand = INVALID_VALUE_REF;
			IRValueRef register_string_length_operand = INVALID_VALUE_REF;
			bool constant_string_length = false;
			uint64_t constant_string_length_bits = 0;
			if (instruction.has_value_operation
					&& (boxed_cond_branch
						|| (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
							&& instruction.value_operation.source_opcode
								== ZEND_STRLEN))) {
				IRValueRef candidate = source_binding_value_ref(
					instruction.source_op1_binding);
				if (candidate == INVALID_VALUE_REF) {
					candidate = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				if (candidate != INVALID_VALUE_REF
						&& machine_value_is_register_authoritative(candidate)
						&& machine_value_has_register_definition(candidate)) {
					if (boxed_cond_branch
							&& machine_kind(candidate)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
						register_boxed_condition_operand = candidate;
					} else if (boxed_cond_branch
							&& machine_kind(candidate)
								== ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
						register_string_condition_operand = candidate;
					} else if (!boxed_cond_branch
							&& machine_kind(candidate)
								== ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
						register_string_length_operand = candidate;
						constant_string_length = known_string_literal(
							candidate, &constant_string_length_bits, nullptr);
					}
				}
			}
			const bool typed_component_call =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr
				&& frozen_typed_component_call(i);
			const bool register_component_call =
				typed_component_call
					|| (record.opcode
						== ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& instruction.direct_call != nullptr
					&& frozen_effect_closed_inline(i));
			if (register_component_call) {
				if (i >= register_component_results.size()) {
					valid_ = false;
					continue;
				}
				if (register_component_results[i] != INVALID_VALUE_REF) {
					result = register_component_results[i];
				} else if (result == INVALID_VALUE_REF
						|| exact_type(result)
							!= ZEND_MIR_SCALAR_TYPE_NULL) {
					valid_ = false;
					continue;
				}
			}
			if (i < register_binary_results.size()
					&& register_binary_results[i] != INVALID_VALUE_REF) {
				result = register_binary_results[i];
			}
			if (i < register_branch_results.size()
					&& register_branch_results[i] != INVALID_VALUE_REF) {
				result = register_branch_results[i];
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode
						== ZEND_MIR_OPCODE_VALUE_BINARY_OP
					&& instruction.has_value_operation
					&& source_result_used[i] != 0
					&& !machine_value_has_result_representation(result)) {
				IRValueRef left = INVALID_VALUE_REF;
				IRValueRef right = INVALID_VALUE_REF;
				if (long_binary_machine_operands(instruction, left, right)) {
					const uint32_t source_opcode =
						instruction.value_operation.source_opcode;
					const bool boolean_result =
						source_opcode == ZEND_IS_IDENTICAL
						|| source_opcode == ZEND_IS_NOT_IDENTICAL
						|| source_opcode == ZEND_IS_EQUAL
						|| source_opcode == ZEND_IS_NOT_EQUAL
						|| source_opcode == ZEND_IS_SMALLER
						|| source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
					if (boolean_result
							|| source_opcode == ZEND_ADD
							|| source_opcode == ZEND_SUB
							|| source_opcode == ZEND_BW_OR
							|| source_opcode == ZEND_BW_AND
							|| source_opcode == ZEND_BW_XOR
							|| source_opcode == ZEND_SPACESHIP) {
						auto &source_overrides =
							active_source_ssa_overrides();
						auto &instruction_results =
							active_instruction_results();
						const IRValueRef canonical_result =
							source_operand_value_ref(
								instruction.value_operation.result);
						const zend_mir_value_id result_ssa =
							instruction.value_operation.result
								.ssa_variable_id;
						result = add_derived_value(
							ZEND_MIR_REPRESENTATION_ZVAL,
							boolean_result
								? ZEND_MIR_SCALAR_TYPE_I1
								: ZEND_MIR_SCALAR_TYPE_I64,
							instruction.value_operation
								.result_storage_id,
							false, 0,
							ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
						instruction_results[i] = result;
						if (result_ssa < source_overrides.size()) {
							source_overrides[result_ssa] = result;
						} else if (canonical_result
								== INVALID_VALUE_REF) {
							valid_ = false;
						}
					}
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				const int32_t result_index =
					zend_tpde_value_index(plan_, record.result_id);
				if (result_index < 0
						|| static_cast<uint32_t>(result_index)
							>= active_value_overrides().size()) {
					valid_ = false;
					continue;
				}
				result = IRValueRef{
					MIR_VALUE_BASE + static_cast<uint32_t>(result_index)};
				if (i < selected_phi_results.size()
						&& selected_phi_results[i] != INVALID_VALUE_REF) {
					result = selected_phi_results[i];
					active_instruction_results()[i] = result;
					active_value_overrides()[
						static_cast<uint32_t>(result_index)] = result;
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				if (result == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				auto phi_input_ref = [&](uint32_t operand) {
					const uint32_t operand_index =
						instruction.operand_offset + operand;
					if (operand_index
								< boxed_phi_input_overrides.size()
							&& boxed_phi_input_overrides[operand_index]
								!= INVALID_VALUE_REF) {
						return boxed_phi_input_overrides[operand_index];
					}
					const int32_t source_block = block_index(record.block_id);
					if (source_block < 0) {
						return INVALID_VALUE_REF;
					}
					const uint32_t predecessor_begin =
						plan_->block_predecessor_offsets[source_block];
					const uint32_t predecessor_count =
						plan_->block_predecessor_offsets[source_block + 1]
							- predecessor_begin;
					if (operand >= predecessor_count) {
						return INVALID_VALUE_REF;
					}
					const uint32_t predecessor = plan_->block_predecessors[
						predecessor_begin + operand];
					return predecessor < plan_->block_count
						? phi_edge_value_ref(zend_tpde_operand_at(
							plan_, &instruction, operand),
							final_blocks[predecessor])
						: INVALID_VALUE_REF;
				};
				if (representation(result) == ZEND_MIR_REPRESENTATION_ZVAL
						|| machine_pointer_kind(machine_kind(result))) {
					/*
					 * An exact-null PHI carries neither payload nor type
					 * entropy.  All incoming values denote the same immediate
					 * PHP value, so allocating a two-part boxed PHI would
					 * manufacture a definition that has no machine use.
					 */
					if (exact_type(result)
							== ZEND_MIR_SCALAR_TYPE_NULL) {
						continue;
					}
					const int32_t source_block =
						block_index(record.block_id);
					if (source_block < 0) {
						valid_ = false;
						continue;
					}
					const uint32_t predecessor_begin =
						plan_->block_predecessor_offsets[source_block];
					const uint32_t predecessors =
						plan_->block_predecessor_offsets[source_block + 1]
							- predecessor_begin;
					const zend_mir_storage_id result_storage =
						canonical_storage(result);
					if (predecessors != instruction.operand_count) {
						valid_ = false;
						continue;
					}
					const zend_tpde_machine_value_kind result_kind =
						machine_kind(result);
					const bool boxed_result =
						result_kind
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
					const bool pointer_result =
						machine_pointer_kind(result_kind);
					bool register_phi =
						boxed_result || pointer_result;
					bool shared_storage =
						zend_mir_id_is_valid(result_storage);
					for (uint32_t n = 0; n < predecessors; ++n) {
						const int32_t canonical_input_index =
							zend_tpde_value_index(plan_,
								zend_tpde_operand_at(
									plan_, &instruction, n));
						const IRValueRef canonical_input =
							canonical_input_index < 0
								? INVALID_VALUE_REF
								: IRValueRef{MIR_VALUE_BASE
									+ static_cast<uint32_t>(canonical_input_index)};
						const IRValueRef input = phi_input_ref(n);
						if (input == INVALID_VALUE_REF) {
							valid_ = false;
							register_phi = false;
							shared_storage = false;
							break;
						}
						register_phi &=
							machine_value_is_register_authoritative(input)
								&& machine_value_has_register_definition(input)
								&& machine_kind(input) == result_kind
								&& (pointer_result
									|| representation(input)
										== ZEND_MIR_REPRESENTATION_ZVAL);
						shared_storage &=
							canonical_input != INVALID_VALUE_REF
							&& canonical_storage(canonical_input)
								== result_storage;
					}
					if (register_phi) {
						block_phis.push_back(
							{static_cast<uint32_t>(block), result});
						phi_values_[
							static_cast<uint32_t>(result)] = 1;
						Slice &input_slice =
							phi_input_slices_[
								static_cast<uint32_t>(result)];
						input_slice.offset =
							static_cast<uint32_t>(phi_inputs_.size());
						for (uint32_t n = 0; n < predecessors; ++n) {
							const IRValueRef input = phi_input_ref(n);
							const uint32_t source_predecessor_index =
								plan_->block_predecessors[
									predecessor_begin + n];
							const uint32_t predecessor_index =
								final_blocks[source_predecessor_index];
							phi_inputs_.push_back({input,
								IRBlockRef{predecessor_index}});
							++input_slice.count;
							const uint32_t cold_predecessor =
								boxed_cond_cold_by_predecessor[
									source_predecessor_index];
							if (cold_predecessor != UINT32_MAX) {
								const uint32_t operand_index =
									instruction.operand_offset + n;
								const IRValueRef cold_input =
									operand_index
											< boxed_phi_cold_input_overrides.size()
										&& boxed_phi_cold_input_overrides[
											operand_index]
											!= INVALID_VALUE_REF
										? boxed_phi_cold_input_overrides[
											operand_index]
										: input;
								phi_inputs_.push_back(
									{cold_input,
										IRBlockRef{cold_predecessor}});
								++input_slice.count;
							}
						}
						continue;
					}
					/*
					 * Preserve the old registerless form only when every
					 * identity is already the same clean physical Zend slot.
					 */
				if ((plan_->value_model_flags
						& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0
						|| !shared_storage) {
					valid_ = false;
				}
					continue;
				}
				if (!zend_mir_scalar_type_is_exact(exact_type(result))
						|| exact_type(result) == ZEND_MIR_SCALAR_TYPE_NULL) {
					continue;
				}
				if (!machine_value_has_result_representation(result)) {
					const zend_mir_storage_id result_storage =
						canonical_storage(result);
					bool shared_storage =
						(plan_->value_model_flags
							& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) != 0
						&& zend_mir_id_is_valid(result_storage);
					for (uint32_t n = 0;
							shared_storage && n < instruction.operand_count;
							++n) {
						const IRValueRef input = phi_input_ref(n);
						shared_storage = input != INVALID_VALUE_REF
							&& canonical_storage(input) == result_storage;
					}
					if (!shared_storage) {
						valid_ = false;
					}
					continue;
				}
				block_phis.push_back(
					{static_cast<uint32_t>(block), result});
				phi_values_[static_cast<uint32_t>(result)] = 1;
				const int32_t source_block =
					block_index(record.block_id);
				if (source_block < 0) {
					valid_ = false;
					continue;
				}
				const uint32_t predecessor_begin =
					plan_->block_predecessor_offsets[source_block];
				const uint32_t predecessors =
					plan_->block_predecessor_offsets[source_block + 1]
						- predecessor_begin;
				if (predecessors != instruction.operand_count) {
					valid_ = false;
					continue;
				}
				Slice &input_slice =
					phi_input_slices_[static_cast<uint32_t>(result)];
				input_slice.offset =
					static_cast<uint32_t>(phi_inputs_.size());
				for (uint32_t n = 0; n < predecessors; ++n) {
					IRValueRef input = phi_input_ref(n);
					const int32_t input_index = zend_tpde_value_index(
						plan_, zend_tpde_operand_at(plan_, &instruction, n));
					if (input_index >= 0) {
						const IRValueRef materializable =
							resolve_materializable_scalar(
								static_cast<uint32_t>(input_index),
								final_blocks[plan_->block_predecessors[
									predecessor_begin + n]], UINT32_MAX, 0,
								resolve_materializable_scalar);
						if (materializable != INVALID_VALUE_REF) {
							input = materializable;
						}
					}
					if (input == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
					const uint32_t source_predecessor_index =
						plan_->block_predecessors[
							predecessor_begin + n];
					const uint32_t predecessor_index =
						final_blocks[source_predecessor_index];
					phi_inputs_.push_back(
						{input, IRBlockRef{predecessor_index}});
					++input_slice.count;
					const uint32_t cold_predecessor =
						boxed_cond_cold_by_predecessor[
							source_predecessor_index];
					if (cold_predecessor != UINT32_MAX) {
						const uint32_t operand_index =
							instruction.operand_offset + n;
						const IRValueRef cold_input =
							operand_index
									< boxed_phi_cold_input_overrides.size()
								&& boxed_phi_cold_input_overrides[
									operand_index] != INVALID_VALUE_REF
							? boxed_phi_cold_input_overrides[operand_index]
							: input;
						phi_inputs_.push_back(
							{cold_input, IRBlockRef{cold_predecessor}});
						++input_slice.count;
					}
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& zend_mir_scalar_type_is_exact(exact_type(result))
					&& exact_type(result) != ZEND_MIR_SCALAR_TYPE_NULL) {
				const int32_t result_index =
					zend_tpde_value_index(plan_, record.result_id);
				if (result_index >= 0
						&& machine_value_used[
							static_cast<uint32_t>(result_index)] == 0) {
					continue;
				}
			}

			zend_tpde_slot_isset_empty source_scalar_isset_layout{};
			const bool source_scalar_slot_isset =
				record.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
				&& instruction.has_value_operation
				&& zend_tpde_slot_isset_empty_at(
					instruction, &source_scalar_isset_layout);
			const bool source_scalar_result_machine_eligible =
				function_mode_ == FunctionMode::ZendEntry
					&& instruction.has_value_operation
					&& source_result_used[i] != 0
					&& ((record.opcode
							== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
						&& source_scalar_slot_isset
						&& (instruction.machine_control_flow_flags
								& ZEND_TPDE_MACHINE_CONTROL_FLOW_REGISTER_RESULT)
								!= 0)
						|| (record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
							&& (register_bool_unary_operand
									!= INVALID_VALUE_REF
								|| (instruction.value_operation.source_opcode
										== ZEND_STRLEN
									&& register_string_length_operand
										!= INVALID_VALUE_REF))));
			if (source_scalar_result_machine_eligible
					&& !machine_value_has_result_representation(result)) {
				auto &value_overrides = active_value_overrides();
				auto &source_overrides = active_source_ssa_overrides();
				auto &instruction_results = active_instruction_results();
				const IRValueRef canonical_result =
					source_binding_value_ref(
						instruction.source_result_binding);
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				const zend_mir_scalar_type_mask result_type =
					record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
						&& instruction.value_operation.source_opcode == ZEND_STRLEN
						? ZEND_MIR_SCALAR_TYPE_I64
						: ZEND_MIR_SCALAR_TYPE_I1;
				const zend_mir_value_id result_ssa =
					instruction.value_operation.result.ssa_variable_id;
				const zend_mir_storage_id result_storage =
					instruction.value_operation.result_storage_id;
				if (canonical_result != INVALID_VALUE_REF
						&& canonical_index >= MIR_VALUE_BASE
						&& canonical_index - MIR_VALUE_BASE
							< plan_->value_count
						&& zend_mir_id_is_valid(result_storage)) {
					result = add_derived_value(
						result_type == ZEND_MIR_SCALAR_TYPE_I1
							? ZEND_MIR_REPRESENTATION_I1
							: ZEND_MIR_REPRESENTATION_I64,
						result_type, result_storage,
						constant_string_length,
						constant_string_length
							? constant_string_length_bits : 0,
						result_type == ZEND_MIR_SCALAR_TYPE_I1
							? ZEND_TPDE_MACHINE_VALUE_BOOL
							: ZEND_TPDE_MACHINE_VALUE_I64,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_IMMORTAL);
					if (result == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
					instruction_results[i] = result;
					value_overrides[
						canonical_index - MIR_VALUE_BASE] = result;
					if (result_ssa < source_overrides.size()) {
						source_overrides[result_ssa] = result;
					}
				}
			}
			bool source_boxed_result_machine_eligible = false;
			bool source_dynamic_direct_long = false;
			bool source_array_constant_key = false;
			const IRValueRef canonical_source_result =
				instruction.has_value_operation
					? source_binding_value_ref(
						instruction.source_result_binding)
					: INVALID_VALUE_REF;
			const int32_t direct_source_consumer =
				i < source_result_consumer.size()
					? source_result_consumer[i] : -1;
			IRValueRef source_array_key = INVALID_VALUE_REF;
			bool source_array_key_entry_register = false;
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					&& instruction.has_value_operation) {
				source_array_key = source_binding_value_ref(
					instruction.source_op2_binding);
				if (source_array_key == INVALID_VALUE_REF) {
					source_array_key = source_operand_value_ref(
						instruction.value_operation.op2);
				}
				uint64_t key_constant_bits = 0;
				source_array_constant_key =
					instruction.value_operation.op2.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| (source_array_key != INVALID_VALUE_REF
						&& (known_string_literal(
							source_array_key, nullptr, nullptr)
						|| (machine_kind(source_array_key)
								== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
							&& !zend_mir_id_is_valid(
								canonical_storage(source_array_key)))
						|| (exact_type(source_array_key)
								== ZEND_MIR_SCALAR_TYPE_I64
							&& constant(source_array_key,
								&key_constant_bits))));
				const uint32_t raw_key =
					static_cast<uint32_t>(source_array_key);
				if (function_mode_ == FunctionMode::ZendEntry
						&& raw_key >= MIR_VALUE_BASE
						&& raw_key - MIR_VALUE_BASE < plan_->value_count
						&& plan_->values[raw_key - MIR_VALUE_BASE]
							.argument_index >= 0) {
					const TypedBodyAbiType key_abi = typed_body_value_abi(
						plan_, raw_key - MIR_VALUE_BASE);
					source_array_key_entry_register = key_abi.valid
						&& key_abi.machine_kind
							== ZEND_TPDE_MACHINE_VALUE_STRING_PTR;
				}
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& instruction.has_value_operation
					&& source_result_used[i] != 0
					&& source_result_has_direct_consumer(i)) {
				const zend_tpde_machine_reference *reference = nullptr;
				if (record.opcode
						== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R) {
					zend_tpde_array_read layout{};
					source_boxed_result_machine_eligible =
						zend_tpde_array_read_at(instruction, &layout)
						&& operation_machine_reference(i, &reference)
						&& reference != nullptr
						&& reference->kind
							== ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT
						&& (layout.container_literal
							|| zend_mir_id_is_valid(
								reference->base_value_id))
						&& zend_mir_id_is_valid(
							reference->index_value_id)
						&& reference->scale == sizeof(zval)
						&& reference->access_width == sizeof(zval)
						&& (source_array_constant_key
							|| (source_array_key != INVALID_VALUE_REF
								&& ((exact_type(source_array_key)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& representation(source_array_key)
									== ZEND_MIR_REPRESENTATION_I64
								&& machine_kind(source_array_key)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| (machine_kind(source_array_key)
									== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
								&& machine_value_is_register_authoritative(
									source_array_key))
							|| (representation(source_array_key)
									== ZEND_MIR_REPRESENTATION_ZVAL
								&& machine_kind(source_array_key)
										== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
								&& (machine_value_has_register_definition(
										source_array_key)
									|| source_array_key_entry_register)));
				} else if (record.opcode
						== ZEND_MIR_OPCODE_OBJECT_FETCH_R) {
					zend_tpde_object_property_read layout{};
					source_boxed_result_machine_eligible =
						zend_tpde_object_property_read_at(
							instruction, &layout)
						&& operation_machine_reference(i, &reference)
						&& reference != nullptr
						&& reference->kind
							== ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT
						&& reference->stable_storage_or_layout_id
							== layout.cache_offset
						&& reference->access_width == sizeof(zval);
				} else if (record.opcode
						== ZEND_MIR_OPCODE_DYNAMIC_FETCH_R) {
					zend_tpde_dynamic_fetch_read layout{};
					source_boxed_result_machine_eligible =
						zend_tpde_dynamic_fetch_read_at(
							instruction, &layout);
					source_dynamic_direct_long =
						source_boxed_result_machine_eligible
							&& layout.direct_long;
				}
			}
			const bool unknown_reference_result =
				canonical_source_result != INVALID_VALUE_REF
				&& machine_kind(canonical_source_result)
					== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
				&& !zend_mir_scalar_type_is_exact(
					exact_type(canonical_source_result));
			const bool complete_boxed_source_result =
				(record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					&& source_array_constant_key)
				|| record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
				|| record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				|| record.opcode == ZEND_MIR_OPCODE_VALUE_COUNT;
			const bool direct_internal_argument_result =
				unknown_reference_result
				&& complete_boxed_source_result
				&& instruction.has_value_operation
				&& source_result_used[i] != 0
				&& direct_source_consumer > static_cast<int32_t>(i)
				&& static_cast<uint32_t>(direct_source_consumer)
					< plan_->instruction_count
				&& plan_->instructions[static_cast<uint32_t>(
					direct_source_consumer)].record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL;
			const bool register_complete_array_result =
				record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				&& source_array_constant_key
				&& source_result_used[i] != 0
				&& source_result_has_direct_consumer(i)
				&& direct_source_consumer > static_cast<int32_t>(i)
				&& static_cast<uint32_t>(direct_source_consumer)
					< plan_->instruction_count
				&& plan_->instructions[static_cast<uint32_t>(
					direct_source_consumer)].record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL;
			const bool boxed_source_result =
				((record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
						|| record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
						|| record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
					&& source_boxed_result_machine_eligible)
				|| direct_internal_argument_result
				|| register_complete_array_result;
			const bool boxed_helper_boundary_result =
				(record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					|| record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R)
				&& boxed_source_result
				&& direct_source_consumer > static_cast<int32_t>(i)
				&& static_cast<uint32_t>(direct_source_consumer)
					< plan_->instruction_count
				&& (plan_->instructions[static_cast<uint32_t>(
						direct_source_consumer)].record.opcode
						== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
					|| (plan_->instructions[static_cast<uint32_t>(
							direct_source_consumer)].record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& !frozen_typed_component_call(
							static_cast<uint32_t>(direct_source_consumer))));
			const bool typed_boxed_call_result =
				direct_source_consumer > static_cast<int32_t>(i)
				&& static_cast<uint32_t>(direct_source_consumer)
					< plan_->instruction_count
				&& frozen_typed_component_call(
					static_cast<uint32_t>(direct_source_consumer));
			if (function_mode_ == FunctionMode::ZendEntry
					&& instruction.has_value_operation
					&& boxed_source_result
					&& (!machine_value_has_result_representation(result)
						|| unknown_reference_result
						|| register_complete_array_result
						|| (typed_boxed_call_result
							&& machine_kind(result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& ownership(result)
								!= ZEND_MIR_OWNERSHIP_STATE_OWNED))) {
				auto &value_overrides = active_value_overrides();
				auto &source_overrides =
					active_source_ssa_overrides();
				auto &instruction_results =
					active_instruction_results();
				const IRValueRef canonical_result = canonical_source_result;
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				const zend_mir_value_id result_ssa =
					instruction.value_operation.result
						.ssa_variable_id;
				const zend_mir_storage_id result_storage =
					instruction.value_operation.result_storage_id;
				if (canonical_result == INVALID_VALUE_REF
						|| canonical_index < MIR_VALUE_BASE
						|| canonical_index - MIR_VALUE_BASE
							>= plan_->value_count
						|| !zend_mir_id_is_valid(result_storage)) {
					valid_ = false;
					continue;
				}
				/*
				 * Source value operations retain a void ZNMIR record because
				 * their full PHP semantics live in the executable descriptor.
				 * The successful fast path, or its existing cold continuation,
				 * nevertheless produces a real zval. Keep its payload and
				 * type-info as a two-part TPDE value and join it with the cold
				 * helper result at the continuation PHI.
				 *
				 * An unknown source result may otherwise inherit the value model's
				 * one-part reference-pointer carrier. That carrier
				 * cannot snapshot the complete value when an optimized op array
				 * reuses the result TMP before a direct internal call executes.
				 */
				result = source_dynamic_direct_long
						&& !unknown_reference_result
						&& !register_complete_array_result
					? add_derived_value(
						ZEND_MIR_REPRESENTATION_I64,
						ZEND_MIR_SCALAR_TYPE_I64,
						result_storage, false, 0,
						ZEND_TPDE_MACHINE_VALUE_I64)
					: add_derived_value(
						ZEND_MIR_REPRESENTATION_ZVAL,
						exact_type(canonical_result),
						result_storage, false, 0,
						ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
						ZEND_MIR_OWNERSHIP_STATE_OWNED,
						ZEND_MIR_REFCOUNT_UNKNOWN);
				if (result == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				instruction_results[i] = result;
				value_overrides[
					canonical_index - MIR_VALUE_BASE] = result;
				if (result_ssa < source_overrides.size()) {
					source_overrides[result_ssa] = result;
				}
				if (boxed_helper_boundary_result
						&& machine_kind(result)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					source_result_materialized_for_helper[i] = 1;
				}
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
					&& instruction.direct_internal_call != nullptr
					&& source_result_used[i] != 0
					&& !machine_value_has_result_representation(result)) {
				const zend_mir_scalar_type_mask result_type =
					instruction.direct_internal_call->result_type;
				const IRValueRef canonical_result = source_binding_value_ref(
					instruction.source_result_binding);
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				const zend_mir_storage_id result_storage =
					canonical_storage(canonical_result);
				const zend_mir_value_id result_ssa =
					instruction.direct_internal_call->result_operand.ssa_variable_id;
				if ((result_type == ZEND_MIR_SCALAR_TYPE_I1
						|| result_type == ZEND_MIR_SCALAR_TYPE_I64
						|| result_type == ZEND_MIR_SCALAR_TYPE_F64)
						&& canonical_result != INVALID_VALUE_REF
						&& canonical_index >= MIR_VALUE_BASE
						&& canonical_index - MIR_VALUE_BASE
							< plan_->value_count
						&& zend_mir_id_is_valid(result_storage)) {
					result = add_derived_value(
						result_type == ZEND_MIR_SCALAR_TYPE_I1
							? ZEND_MIR_REPRESENTATION_I1
							: result_type == ZEND_MIR_SCALAR_TYPE_F64
								? ZEND_MIR_REPRESENTATION_DOUBLE
								: ZEND_MIR_REPRESENTATION_I64,
						result_type, result_storage, false, 0,
						result_type == ZEND_MIR_SCALAR_TYPE_I1
							? ZEND_TPDE_MACHINE_VALUE_BOOL
							: result_type == ZEND_MIR_SCALAR_TYPE_F64
								? ZEND_TPDE_MACHINE_VALUE_F64
								: ZEND_TPDE_MACHINE_VALUE_I64,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_IMMORTAL);
					if (result == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
					active_instruction_results()[i] = result;
					active_value_overrides()[
						canonical_index - MIR_VALUE_BASE] = result;
					if (result_ssa < active_source_ssa_overrides().size()) {
						active_source_ssa_overrides()[result_ssa] = result;
					}
				}
			}

			IRValueRef copy_input = INVALID_VALUE_REF;
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& result != INVALID_VALUE_REF
					&& instruction.operand_count == 1) {
				copy_input = resolve_copy_input(zend_tpde_operand_at(
					plan_, &instruction, 0), block, i);
			}
			if (i < mutation_results.size()
					&& mutation_results[i] != INVALID_VALUE_REF) {
				result = mutation_results[i];
			}
			bool machine_result =
				machine_value_needs_result_assignment(result);
			/*
			 * A register BOOL/BOOL_NOT operand is only useful when this source
			 * operation also publishes a machine result.  The same applies to a
			 * known string length: canonical-only consumers still need the source
			 * operation to write the result zval in the frame.
			 */
			if (!machine_result) {
				register_bool_unary_operand = INVALID_VALUE_REF;
				register_string_length_operand = INVALID_VALUE_REF;
				constant_string_length = false;
			}
			if (machine_result
					&& (record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						|| record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
					&& !register_component_call) {
				const int32_t result_index =
					zend_tpde_value_index(plan_, record.result_id);
				machine_result = result_index >= 0
					&& machine_value_used[
						static_cast<uint32_t>(result_index)] != 0;
			}
			if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& instruction.direct_call != nullptr) {
				instruction.direct_call->flags &=
					~ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT;
				if (machine_result
						&& zend_mir_scalar_type_is_exact(
							exact_type(result))
						&& exact_type(result)
							!= ZEND_MIR_SCALAR_TYPE_NULL) {
					instruction.direct_call->flags |=
						ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT;
				}
			} else if (record.opcode
						== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL
					&& instruction.direct_internal_call != nullptr) {
				instruction.direct_internal_call->flags &=
					~ZEND_NATIVE_DIRECT_INTERNAL_CALL_REQUIRE_SCALAR_RESULT;
				if (machine_result) {
					instruction.direct_internal_call->flags |=
						ZEND_NATIVE_DIRECT_INTERNAL_CALL_REQUIRE_SCALAR_RESULT;
				}
			}
			const InlinedBody inlined_user_body =
				function_mode_ == FunctionMode::ZendEntry
					&& record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& machine_result
					? inline_component_scalar_body(instruction,
						i,
						result,
						static_cast<uint32_t>(block),
						block_instructions)
					: InlinedBody{};
			/*
			 * Scalar identity operations without an exact machine result are
			 * registerless source-SSA topology.  They have no observable
			 * target-side effect, and exposing their (possibly constant)
			 * operand to TPDE would create a use without a corresponding
			 * result definition.
			 */
			if (!machine_result
					&& (record.opcode == ZEND_MIR_OPCODE_COPY
						|| record.opcode == ZEND_MIR_OPCODE_CANONICALIZE
						|| record.opcode == ZEND_MIR_OPCODE_I1_TO_I64)) {
				continue;
			}
			/*
			 * Constant-folded scalar results have no TPDE assignment. Their
			 * consumers use the frozen constant directly, so retaining the pure
			 * defining instruction would ask the allocator for an ignored value.
			 */
			if (!machine_result && !register_cond_branch
					&& record.opcode
						>= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
					&& record.opcode < ZEND_MIR_OPCODE_SCALAR_DROP) {
				continue;
			}
			/* W09 Pi nodes over canonical zvals preserve source SSA topology.
			 * They are registerless only after the same physical-location proof
			 * used for boxed PHIs. */
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				uint64_t copy_constant_bits;
				if (machine_result
						&& copy_input != INVALID_VALUE_REF
							&& ((machine_value_is_register_authoritative(
									copy_input)
								&& machine_value_has_register_definition(
									copy_input))
								|| entry_argument_has_register_definition(
									copy_input)
								|| constant(copy_input, &copy_constant_bits))) {
					/* Emit the selected machine copy below. */
				} else {
					if ((plan_->value_model_flags
							& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0) {
						continue;
					}
					const zend_mir_storage_id result_storage =
						canonical_storage(result);
					const zend_mir_value_id input_id =
						instruction.operand_count == 1
							? zend_tpde_operand_at(plan_, &instruction, 0)
							: ZEND_MIR_ID_INVALID;
					const int32_t input_index = zend_tpde_value_index(
						plan_, input_id);
					const IRValueRef input = input_index >= 0
						? value_ref(input_id)
						: INVALID_VALUE_REF;
					if (result == INVALID_VALUE_REF
							|| input_index < 0
							|| input == INVALID_VALUE_REF
							|| plan_->values[static_cast<uint32_t>(input_index)]
								.representation
							!= ZEND_MIR_REPRESENTATION_ZVAL
							|| !zend_mir_id_is_valid(result_storage)
							|| canonical_storage(input) != result_storage) {
						valid_ = false;
					}
					if (machine_result
							&& result != INVALID_VALUE_REF
							&& input != INVALID_VALUE_REF
							&& machine_pointer_kind(machine_kind(result))
							&& machine_kind(input)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& representation(input)
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& zend_mir_id_is_valid(result_storage)
							&& canonical_storage(input) == result_storage) {
						const uint32_t reference = machine_reference_index(
							ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
							result_storage);
						const IRValueRef address = reference != UINT32_MAX
							? add_derived_value(
								ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
								ZEND_MIR_SCALAR_TYPE_NONE, result_storage,
								false, 0, UINT8_MAX,
								ZEND_MIR_OWNERSHIP_STATE_BORROWED,
								ZEND_MIR_REFCOUNT_UNKNOWN, reference)
							: INVALID_VALUE_REF;
						if (address == INVALID_VALUE_REF) {
							valid_ = false;
							continue;
						}
						const uint32_t load_operand_offset =
							static_cast<uint32_t>(operands_.size());
						operands_.push_back(address);
						add_node(block_instructions, block, InstNode{
							InstKind::ZvalPayloadLoad, i, UINT32_MAX,
							result, {}, load_operand_offset, 1, true,
							result_storage, exact_type(result)});
						active_instruction_results()[i] = result;
					}
					continue;
				}
			}
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			std::vector<std::pair<IRValueRef, uint32_t>>
				typed_call_value_guards;
			std::vector<IRValueRef> typed_call_owned_boxed_arguments;
			uint32_t assign_op_right_operand_index = UINT32_MAX;
			uint32_t assign_op_left_operand_index = UINT32_MAX;
			uint32_t packed_append_value_operand_index = UINT32_MAX;
			uint32_t property_write_value_operand_index = UINT32_MAX;
			if ((record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
						|| record.opcode
							== ZEND_MIR_OPCODE_VALUE_QM_ASSIGN)
					&& instruction.has_value_operation) {
				IRValueRef assigned = record.opcode
						== ZEND_MIR_OPCODE_VALUE_QM_ASSIGN
					? source_binding_value_ref(
						instruction.source_op1_binding)
					: i < register_assignment_sources.size()
							&& register_assignment_sources[i]
								!= INVALID_VALUE_REF
						? register_assignment_sources[i]
						: source_binding_value_ref(
							instruction.source_op2_binding);
				if (assigned == INVALID_VALUE_REF
						&& record.opcode != ZEND_MIR_OPCODE_VALUE_QM_ASSIGN) {
					assigned = source_binding_value_ref(
						instruction.source_op1_binding);
				}
				if (assigned != INVALID_VALUE_REF
						&& machine_value_is_register_authoritative(
							assigned)
						&& machine_value_has_register_definition(
							assigned)
						&& zend_tpde_machine_value_is_register_authoritative(
							machine_kind(assigned))) {
					/*
					 * A register-produced temporary stays authoritative
					 * through assignment.  The target writes the destination
					 * directly from its scalar, pointer, or boxed parts;
					 * subsequent source SSA users consume the same value
					 * instead of reloading the CV.
					 */
					operands_.push_back(assigned);
					const uint32_t definition_plus_one =
						instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one;
						if (definition_plus_one != 0) {
							const uint32_t definition =
								definition_plus_one - 1;
							const IRValueRef assignment_result =
								i < register_assignment_results.size()
										&& register_assignment_results[i]
											!= INVALID_VALUE_REF
									? register_assignment_results[i]
									: assigned;
							auto &source_overrides =
								active_source_ssa_overrides();
							if (definition >= source_overrides.size()) {
								valid_ = false;
							} else {
								source_overrides[definition] =
									assignment_result;
							const int32_t value_index =
								zend_tpde_value_index(
									plan_,
									zend_mir_value_from_original_ssa(
										definition));
							if (value_index >= 0
									&& static_cast<uint32_t>(value_index)
										< active_value_overrides().size()) {
									active_value_overrides()[
										static_cast<uint32_t>(value_index)] =
											assignment_result;
							}
						}
					}
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					&& instruction.has_value_operation) {
				IRValueRef key = source_binding_value_ref(
					instruction.source_op2_binding);
				if (key == INVALID_VALUE_REF) {
					key = source_operand_value_ref(
						instruction.value_operation.op2);
				}
					if (key != INVALID_VALUE_REF
							&& ((exact_type(key)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& representation(key)
									== ZEND_MIR_REPRESENTATION_I64
								&& machine_kind(key)
									== ZEND_TPDE_MACHINE_VALUE_I64)
								|| (machine_kind(key)
									== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
									&& machine_value_is_register_authoritative(
										key))
								|| (representation(key)
									== ZEND_MIR_REPRESENTATION_ZVAL
								&& machine_kind(key)
										== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
						&& machine_value_has_register_definition(key)) {
					/*
						 * Keep a proven integer or string dimension key in TPDE SSA.
						 * The target can then select a compact packed lookup or a
						 * pointer-identity mixed lookup without reloading and
						 * retagging the canonical key zval.
					 */
					operands_.push_back(key);
					const IRValueRef receiver =
						source_binding_value_ref(
							instruction.source_op1_binding);
					if (receiver != INVALID_VALUE_REF
							&& (machine_kind(receiver)
									== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
								|| (representation(receiver)
										== ZEND_MIR_REPRESENTATION_ZVAL
									&& machine_kind(receiver)
										== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
							&& machine_value_has_register_definition(
								receiver)) {
						/*
						 * Array-producing source operations publish their
						 * canonical zval for PHP observability and retain the
						 * payload pointer as the authoritative TPDE value.
						 * Keep it after the key so existing key-only guarded
						 * layouts remain stable.
						 */
						operands_.push_back(receiver);
					}
				}
			}
			if (record.opcode
						== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
					&& instruction.has_value_operation) {
				IRValueRef key = source_binding_value_ref(
					instruction.source_op2_binding);
				if (key == INVALID_VALUE_REF) {
					key = source_operand_value_ref(
						instruction.value_operation.op2);
				}
				if (key != INVALID_VALUE_REF
						&& ((exact_type(key) == ZEND_MIR_SCALAR_TYPE_I64
								&& representation(key)
									== ZEND_MIR_REPRESENTATION_I64
								&& machine_kind(key)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| (machine_kind(key)
									== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
								&& machine_value_is_register_authoritative(key))
							|| (representation(key)
									== ZEND_MIR_REPRESENTATION_ZVAL
								&& machine_kind(key)
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
						&& machine_value_has_register_definition(key)) {
					operands_.push_back(key);
					const IRValueRef receiver = source_binding_value_ref(
						instruction.source_op1_binding);
					if (receiver != INVALID_VALUE_REF
							&& (machine_kind(receiver)
									== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
								|| (representation(receiver)
										== ZEND_MIR_REPRESENTATION_ZVAL
									&& machine_kind(receiver)
										== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
							&& machine_value_has_register_definition(receiver)) {
						operands_.push_back(receiver);
					}
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
					&& instruction.has_value_operation) {
				IRValueRef name = source_binding_value_ref(
					instruction.source_op1_binding);
				if (name == INVALID_VALUE_REF) {
					name = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				if (name != INVALID_VALUE_REF
						&& machine_kind(name)
							== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
						&& machine_value_is_register_authoritative(name)
						&& machine_value_has_register_definition(name)) {
					/*
					 * A dynamic CV lookup consumes the variable-name string on
					 * every execution. Preserve an authoritative zend_string
					 * pointer for the guarded fast node; the cold helper observes
					 * the already materialized canonical frame instead.
					 */
					operands_.push_back(name);
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
						&& instruction.has_value_operation) {
					const IRValueRef right = source_binding_value_ref(
						instruction.source_op2_binding);
					const IRValueRef left = source_binding_value_ref(
						instruction.source_op1_binding);
					const bool register_right =
						plan_->generator_resume_count == 0
						&& !storage_assigned_by_reference(
							instruction.value_operation.op2_storage_id)
						&& right != INVALID_VALUE_REF
						&& ((representation(right)
									== ZEND_MIR_REPRESENTATION_I64
								&& exact_type(right)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& machine_kind(right)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| machine_kind(right)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
						&& machine_value_is_register_authoritative(right)
						&& machine_value_has_register_definition(right);
					const bool register_left =
						plan_->generator_resume_count == 0
						&& !storage_assigned_by_reference(
							instruction.value_operation.op1_storage_id)
						&& left != INVALID_VALUE_REF
						&& ((representation(left)
									== ZEND_MIR_REPRESENTATION_I64
								&& exact_type(left)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& machine_kind(left)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| machine_kind(left)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
						&& machine_value_is_register_authoritative(left)
						&& machine_value_has_register_definition(left);
					if (register_right) {
						/*
						 * Preserve a preceding register result across compound
						 * assignment.  The target can consume the payload/type
						 * pair directly; only the cold helper materializes it.
						 */
						assign_op_right_operand_index =
							static_cast<uint32_t>(operands_.size())
								- operand_offset;
						operands_.push_back(right);
					}
					if (register_left) {
						assign_op_left_operand_index =
							static_cast<uint32_t>(operands_.size())
								- operand_offset;
						operands_.push_back(left);
					}
				}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_INCDEC
					&& instruction.has_value_operation
					&& instruction.mutation_lazy_scalar) {
				const IRValueRef operand = source_binding_value_ref(
					instruction.source_op1_binding);
				if (operand != INVALID_VALUE_REF
						&& machine_value_is_register_authoritative(operand)
						&& machine_value_has_register_definition(operand)
						&& ((representation(operand)
									== ZEND_MIR_REPRESENTATION_I64
								&& exact_type(operand)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& machine_kind(operand)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| machine_kind(operand)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
					operands_.push_back(operand);
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
					&& instruction.has_value_operation) {
				zend_tpde_packed_array_append append_layout{};
				IRValueRef value = source_binding_value_ref(
					instruction.source_auxiliary_binding);
				if (value == INVALID_VALUE_REF) {
					value = source_operand_value_ref(
						instruction.value_operation.auxiliary);
				}
				if (zend_tpde_packed_array_append_at(
						instruction, &append_layout)
						&& value != INVALID_VALUE_REF
						&& representation(value)
							== ZEND_MIR_REPRESENTATION_I64
						&& exact_type(value) == ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(value)
							== ZEND_TPDE_MACHINE_VALUE_I64
						&& machine_value_is_register_authoritative(value)
						&& machine_value_has_register_definition(value)) {
					/*
					 * The append helper already records the auxiliary source as
					 * a semantic input.  Preserve its exact machine payload for
					 * the guarded fast node so it need not reload, classify, and
					 * refcount the canonical zval.  The cold node skips this extra
					 * SSA use and observes the materialized Zend frame.
					 */
					packed_append_value_operand_index =
						static_cast<uint32_t>(operands_.size())
							- operand_offset;
					operands_.push_back(value);
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN
					&& instruction.has_value_operation) {
				zend_tpde_object_property_write property_layout{};
				IRValueRef value = source_binding_value_ref(
					instruction.source_auxiliary_binding);
				if (value == INVALID_VALUE_REF) {
					value = source_operand_value_ref(
						instruction.value_operation.auxiliary);
				}
				if (zend_tpde_object_property_write_at(
						instruction, &property_layout)
						&& value != INVALID_VALUE_REF
						&& representation(value)
							== ZEND_MIR_REPRESENTATION_I64
						&& exact_type(value) == ZEND_MIR_SCALAR_TYPE_I64
						&& machine_kind(value)
							== ZEND_TPDE_MACHINE_VALUE_I64
						&& machine_value_is_register_authoritative(value)
						&& machine_value_has_register_definition(value)) {
					property_write_value_operand_index =
						static_cast<uint32_t>(operands_.size())
							- operand_offset;
					operands_.push_back(value);
				}
			}
			if (type_check_selection
					!= ScalarTypeCheckSelection::Invalid) {
				switch (type_check_selection) {
					case ScalarTypeCheckSelection::CopyInput:
					case ScalarTypeCheckSelection::NotInput:
						operands_.push_back(type_check_input);
						break;
					case ScalarTypeCheckSelection::ConstantFalse:
					case ScalarTypeCheckSelection::ConstantTrue:
						operands_.push_back(add_derived_value(
							ZEND_MIR_REPRESENTATION_I1,
							ZEND_MIR_SCALAR_TYPE_I1,
							ZEND_MIR_ID_INVALID, true,
							type_check_selection
									== ScalarTypeCheckSelection::ConstantTrue
								? 1 : 0));
						break;
					default:
						break;
				}
			}
			if (register_cond_branch) {
				operands_.push_back(register_condition);
			}
			if (register_bool_unary_operand != INVALID_VALUE_REF) {
				/*
				 * BOOL and BOOL_NOT over a proven machine boolean do not need
				 * to round-trip through the canonical temporary.  Retain the
				 * source value for the fast node; the cold helper still observes
				 * only the materialized Zend frame.
				 */
				operands_.push_back(register_bool_unary_operand);
			}
			if (register_string_length_operand != INVALID_VALUE_REF) {
				/*
				 * Keep the authoritative zend_string pointer ahead of the frame
				 * operand.  The normal or guarded fast node reads the length
				 * directly; a generic cold node skips this extra SSA use and
				 * observes only the canonical source zval.
				 */
				operands_.push_back(register_string_length_operand);
			}
			IRValueRef static_slot_isset_operand = INVALID_VALUE_REF;
			bool static_slot_isset_exact = false;
			bool static_slot_isset_needs_value = false;
			zend_mir_scalar_type_mask static_slot_isset_exact_type =
				ZEND_MIR_SCALAR_TYPE_NONE;
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
					&& instruction.has_value_operation) {
				zend_tpde_slot_isset_empty isset_layout{};
				const bool has_isset_layout =
					zend_tpde_slot_isset_empty_at(instruction, &isset_layout);
				IRValueRef candidate = source_binding_value_ref(
					instruction.source_op1_binding);
				if ((candidate == INVALID_VALUE_REF
						|| exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_NONE)
						&& instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one != 0) {
					candidate = value_ref(zend_mir_value_from_original_ssa(
						instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one - 1));
				}
				if (candidate == INVALID_VALUE_REF
						|| exact_type(candidate) == ZEND_MIR_SCALAR_TYPE_NONE) {
					candidate = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				uint64_t constant_bits = 0;
				const zend_mir_scalar_type_mask type =
					candidate == INVALID_VALUE_REF
						? ZEND_MIR_SCALAR_TYPE_NONE
						: exact_type(candidate);
				const bool supported =
					type == ZEND_MIR_SCALAR_TYPE_NULL
					|| type == ZEND_MIR_SCALAR_TYPE_I1
					|| type == ZEND_MIR_SCALAR_TYPE_I64;
				static_slot_isset_exact = supported && has_isset_layout
					&& machine_value_has_stable_exact_type(candidate);
				static_slot_isset_needs_value = static_slot_isset_exact
					&& machine_result
					&& isset_layout.is_empty
					&& type != ZEND_MIR_SCALAR_TYPE_NULL;
				bool available = candidate != INVALID_VALUE_REF
					&& (constant(candidate, &constant_bits)
						|| (machine_value_is_register_authoritative(candidate)
							&& machine_value_has_register_definition(candidate)));
				if (static_slot_isset_needs_value && !available
						&& type != ZEND_MIR_SCALAR_TYPE_NULL
						&& function_mode_ == FunctionMode::ZendEntry) {
					const zend_mir_storage_id storage_id =
						canonical_storage(candidate);
					const uint32_t reference =
						zend_mir_id_is_valid(storage_id)
						? machine_reference_index(
							ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
							storage_id)
						: UINT32_MAX;
					const IRValueRef address = reference != UINT32_MAX
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE, storage_id, false, 0,
							UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN, reference)
						: INVALID_VALUE_REF;
					const IRValueRef transported = address != INVALID_VALUE_REF
						? add_derived_value(
							representation(candidate), type, storage_id, false, 0,
							machine_kind(candidate),
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							refcount_state(candidate))
						: INVALID_VALUE_REF;
					if (transported != INVALID_VALUE_REF) {
						const std::vector<IRValueRef> semantic_prefix(
							operands_.begin() + operand_offset, operands_.end());
						const uint32_t load_operand_offset =
							static_cast<uint32_t>(operands_.size());
						operands_.push_back(address);
						const uint32_t transport_block =
							guarded_hot_blocks[i] != UINT32_MAX
							? guarded_hot_blocks[i]
							: static_cast<uint32_t>(block);
						add_node(block_instructions, transport_block,
							InstNode{InstKind::ZvalPayloadLoad, i, UINT32_MAX,
								transported, {}, load_operand_offset, 1, true,
								storage_id, type});
						operand_offset = static_cast<uint32_t>(operands_.size());
						operands_.insert(operands_.end(), semantic_prefix.begin(),
							semantic_prefix.end());
						candidate = transported;
						available = true;
					}
				}
				if (static_slot_isset_exact) {
					static_slot_isset_exact_type = type;
					if (static_slot_isset_needs_value && available) {
						static_slot_isset_operand = candidate;
						operands_.push_back(candidate);
					}
				}
			}
			const bool static_slot_isset_machine_fast =
				static_slot_isset_exact
				&& (!static_slot_isset_needs_value
					|| static_slot_isset_operand != INVALID_VALUE_REF);
			IRValueRef long_left = INVALID_VALUE_REF;
			IRValueRef long_right = INVALID_VALUE_REF;
			bool explicit_long_operands =
				record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
				&& machine_result
				&& long_binary_machine_operands(
					instruction, long_left, long_right);
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
					&& machine_result && !explicit_long_operands) {
				long_left = source_binding_value_ref(
					instruction.source_op1_binding);
				if (long_left == INVALID_VALUE_REF) {
					long_left = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				long_right = source_binding_value_ref(
					instruction.source_op2_binding);
				if (long_right == INVALID_VALUE_REF) {
					long_right = source_operand_value_ref(
						instruction.value_operation.op2);
				}
				auto transport_canonical_long = [&](IRValueRef &value) {
					if (value == INVALID_VALUE_REF
							|| machine_value_has_register_definition(value)) {
						return value != INVALID_VALUE_REF;
					}
					if (exact_type(value) != ZEND_MIR_SCALAR_TYPE_I64
							|| machine_kind(value)
								!= ZEND_TPDE_MACHINE_VALUE_I64) {
						return false;
					}
					const zend_mir_storage_id storage_id =
						canonical_storage(value);
					const uint32_t reference =
						zend_mir_id_is_valid(storage_id)
							? machine_reference_index(
								ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
								storage_id)
							: UINT32_MAX;
					const IRValueRef address = reference != UINT32_MAX
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE, storage_id,
							false, 0, UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN, reference)
						: INVALID_VALUE_REF;
					const IRValueRef transported = address != INVALID_VALUE_REF
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_I64,
							ZEND_MIR_SCALAR_TYPE_I64, storage_id,
							false, 0, ZEND_TPDE_MACHINE_VALUE_I64,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_IMMORTAL)
						: INVALID_VALUE_REF;
					if (transported == INVALID_VALUE_REF) {
						return false;
					}
					const std::vector<IRValueRef> semantic_prefix(
						operands_.begin() + operand_offset, operands_.end());
					const uint32_t load_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(address);
					add_node(block_instructions, block, InstNode{
						InstKind::ZvalPayloadLoad, i, UINT32_MAX,
						transported, {}, load_operand_offset, 1, true,
						storage_id, ZEND_MIR_SCALAR_TYPE_I64});
					operand_offset = static_cast<uint32_t>(operands_.size());
					operands_.insert(operands_.end(), semantic_prefix.begin(),
						semantic_prefix.end());
					value = transported;
					return true;
				};
				const uint32_t source_opcode =
					instruction.value_operation.source_opcode;
				const bool supported_opcode = source_opcode == ZEND_ADD
					|| source_opcode == ZEND_SUB
					|| source_opcode == ZEND_BW_OR
					|| source_opcode == ZEND_BW_AND
					|| source_opcode == ZEND_BW_XOR
					|| source_opcode == ZEND_SPACESHIP
					|| source_opcode == ZEND_IS_IDENTICAL
					|| source_opcode == ZEND_IS_NOT_IDENTICAL
					|| source_opcode == ZEND_IS_EQUAL
					|| source_opcode == ZEND_IS_NOT_EQUAL
					|| source_opcode == ZEND_IS_SMALLER
					|| source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
				const bool left_supported = long_left != INVALID_VALUE_REF
					&& (exact_type(long_left) == ZEND_MIR_SCALAR_TYPE_I64
						|| machine_kind(long_left)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
				const bool right_supported = long_right != INVALID_VALUE_REF
					&& (exact_type(long_right) == ZEND_MIR_SCALAR_TYPE_I64
						|| machine_kind(long_right)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
				explicit_long_operands = supported_opcode
					&& left_supported && right_supported
					&& transport_canonical_long(long_left)
					&& transport_canonical_long(long_right)
					&& machine_value_has_register_definition(long_left)
					&& machine_value_has_register_definition(long_right);
			}
			if (explicit_long_operands) {
				operands_.push_back(long_left);
				operands_.push_back(long_right);
			}
			uint32_t machine_reference_operand_index = UINT32_MAX;
			uint32_t direct_call_context_operand = UINT32_MAX;
			bool direct_internal_argument_transport = false;
			uint32_t direct_internal_boxed_argument_count = 0;
			const bool literal_assign_operand =
				record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& instruction.source_op2_reference_index
						< plan_->machine_reference_count
					&& plan_->machine_references[
						instruction.source_op2_reference_index].kind
						== ZEND_TPDE_MACHINE_REFERENCE_LITERAL;
			const bool literal_array_receiver =
				record.opcode == ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
				&& instruction.value_operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& instruction.source_op1_reference_index
					< plan_->machine_reference_count
				&& plan_->machine_references[
					instruction.source_op1_reference_index].kind
					== ZEND_TPDE_MACHINE_REFERENCE_LITERAL;
			if (literal_assign_operand || literal_array_receiver) {
				const uint32_t reference_index = literal_array_receiver
					? instruction.source_op1_reference_index
					: instruction.source_op2_reference_index;
				const IRValueRef literal_address = add_derived_value(
					ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
					ZEND_MIR_SCALAR_TYPE_NONE,
					ZEND_MIR_ID_INVALID, false, 0, UINT8_MAX,
					ZEND_MIR_OWNERSHIP_STATE_BORROWED,
					ZEND_MIR_REFCOUNT_UNKNOWN,
					reference_index);
				if (literal_address == INVALID_VALUE_REF) {
					valid_ = false;
				} else {
					machine_reference_operand_index =
						static_cast<uint32_t>(operands_.size())
							- operand_offset;
					operands_.push_back(literal_address);
				}
			}
			/*
			 * RETURN_SOURCE_ZVAL transfers the canonical zval directly from the
			 * Zend frame, selected by its source opline.  Its MIR value operand
			 * carries dependency/type information only; asking TPDE to allocate a
			 * machine value for it leaves an unconsumed ValueAssignment and, more
			 * importantly, would tempt target code to treat a scalar payload as a
			 * complete zval.  The runtime helper needs only the frame pointer.
			 */
			uint32_t data_operand_count =
				record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
					? instruction.zval_store_lazy_scalar ? 0 : 1
				: static_slot_isset_operand != INVALID_VALUE_REF
					? 0
				: register_bool_unary_operand != INVALID_VALUE_REF
					? 0
				: explicit_long_operands
					? 0
				: type_check_selection
							!= ScalarTypeCheckSelection::Invalid
						|| register_cond_branch
					? 0
				: boxed_cond_branch
					? 0
				: (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						&& function_mode_ == FunctionMode::ZendEntry)
					? 0
				: record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					|| (record.opcode
							== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
						&& function_mode_
							== FunctionMode::ZendEntry)
					|| record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| (record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& instruction.direct_call != nullptr)
					? 0 : instruction.operand_count;
			const uint32_t semantic_prefix_end =
				static_cast<uint32_t>(operands_.size());
			std::vector<IRValueRef> data_operands;
			data_operands.reserve(data_operand_count);
			bool data_operand_helpers = false;
			for (uint32_t n = 0; n < data_operand_count; ++n) {
				IRValueRef operand = value_ref(zend_tpde_operand_at(
					plan_, &instruction, n));
				const uint32_t transport_index =
					instruction.operand_offset + n;
				const zend_tpde_operand_transport *transport =
					plan_->instruction_operand_transports != nullptr
						&& transport_index
							< plan_->instruction_operand_count
					? &plan_->instruction_operand_transports[
						transport_index]
						: nullptr;
				const bool canonical_scalar_operand =
					transport != nullptr
					&& transport->kind
						== ZEND_TPDE_OPERAND_TRANSPORT_CANONICAL_SCALAR_LOAD;
				if (transport != nullptr
						&& transport->kind
							== ZEND_TPDE_OPERAND_TRANSPORT_DIRECT
						&& operand != INVALID_VALUE_REF
						&& !machine_value_is_register_authoritative(operand)) {
					const uint32_t raw = static_cast<uint32_t>(operand);
					if (raw >= MIR_VALUE_BASE
							&& raw - MIR_VALUE_BASE < plan_->value_count) {
						const IRValueRef materializable =
							resolve_materializable_scalar(
								raw - MIR_VALUE_BASE, block, i, 0,
								resolve_materializable_scalar);
						if (materializable != INVALID_VALUE_REF) {
							operand = materializable;
						}
					}
				}
				if (canonical_scalar_operand) {
					uint64_t constant_bits;
					if (!constant(operand, &constant_bits)) {
						/*
						 * Representation selection froze this canonical-slot
						 * transition before TPDE. Execute it without
						 * reinterpreting the source opcode here.
						 */
						const zend_mir_storage_id storage_id =
							transport->storage_id;
						const uint32_t reference =
							zend_mir_id_is_valid(storage_id)
								? machine_reference_index(
									ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
									storage_id)
								: UINT32_MAX;
						const IRValueRef address =
							reference != UINT32_MAX
								? add_derived_value(
									ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
									ZEND_MIR_SCALAR_TYPE_NONE,
									storage_id, false, 0, UINT8_MAX,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_UNKNOWN,
									reference)
								: INVALID_VALUE_REF;
						const IRValueRef resolved =
							address != INVALID_VALUE_REF
								&& transport->resolve_reference
								? add_derived_value(
									ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
									ZEND_MIR_SCALAR_TYPE_NONE,
									storage_id, false, 0, UINT8_MAX,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_UNKNOWN)
								: address;
						const IRValueRef transported =
							resolved != INVALID_VALUE_REF
								? add_derived_value(
									transport->representation,
									transport->exact_type,
									storage_id, false, 0,
									transport->machine_kind,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_IMMORTAL)
								: INVALID_VALUE_REF;
						if (transported == INVALID_VALUE_REF) {
							valid_ = false;
						} else {
							uint32_t load_operand_offset =
								static_cast<uint32_t>(operands_.size());
							operands_.push_back(address);
							data_operand_helpers = true;
							if (transport->resolve_reference) {
								add_node(block_instructions, block, InstNode{
									InstKind::ZvalReferenceResolve,
									i, UINT32_MAX, resolved, {},
									load_operand_offset, 1, true,
									storage_id,
									ZEND_MIR_SCALAR_TYPE_NONE});
							}
							load_operand_offset =
								static_cast<uint32_t>(operands_.size());
							operands_.push_back(resolved);
							add_node(block_instructions, block, InstNode{
								InstKind::ZvalPayloadLoad,
								i, UINT32_MAX, transported, {},
								load_operand_offset, 1, true,
								storage_id,
								transport->exact_type});
							operand = transported;
						}
					}
				}
				if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
						&& n == 0
						&& operand != INVALID_VALUE_REF
						&& !machine_value_is_register_authoritative(operand)) {
					const uint32_t operand_index =
						static_cast<uint32_t>(operand);
					if (operand_index >= MIR_VALUE_BASE
							&& operand_index - MIR_VALUE_BASE
								< plan_->value_count
							&& plan_->value_definition_instructions
								!= nullptr) {
						const int32_t definition =
							plan_->value_definition_instructions[
								operand_index - MIR_VALUE_BASE];
						if (definition >= 0
								&& static_cast<uint32_t>(definition)
									< plan_->instruction_count) {
							const zend_tpde_instruction &defining =
								plan_->instructions[
									static_cast<uint32_t>(definition)];
							const zend_mir_instruction_record
								defining_record =
									instruction_record_at(
										static_cast<uint32_t>(definition));
							if ((defining_record.opcode
										== ZEND_MIR_OPCODE_COPY
									|| defining_record.opcode
										== ZEND_MIR_OPCODE_CANONICALIZE)
									&& defining.operand_count == 1) {
								const IRValueRef input = value_ref(
									zend_tpde_operand_at(
										plan_, &defining, 0));
								uint64_t constant_bits;
								if (input != INVALID_VALUE_REF
										&& representation(input)
											== representation(operand)
										&& exact_type(input)
											== exact_type(operand)
										&& zend_tpde_machine_value_is_register_authoritative(
											machine_kind(input))
										&& (constant(input, &constant_bits)
											|| (machine_value_is_register_authoritative(
													input)
												&& machine_value_has_register_definition(
													input)))) {
									/*
									 * Registerless COPY/CANONICALIZE nodes can
									 * still feed the immediately following
									 * canonical ZVAL_STORE.  Use their machine
									 * input for that store; reloading the
									 * destination slot here would read its old
									 * payload before the assignment publishes
									 * the new value.
									 */
									operand = input;
								}
							}
						}
					}
				}
				if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
						&& n == 0
						&& operand != INVALID_VALUE_REF
						&& !machine_value_is_register_authoritative(operand)
						&& !machine_value_has_register_definition(operand)
						&& zend_mir_scalar_type_is_exact(
							exact_type(operand))
						&& exact_type(operand)
							!= ZEND_MIR_SCALAR_TYPE_NULL) {
					uint64_t constant_bits;
					if (!constant(operand, &constant_bits)) {
						/*
						 * A canonical-slot-authoritative scalar has no TPDE
						 * definition of its own.  ZVAL_STORE nevertheless needs
						 * the old payload after its release slow path, so capture
						 * it before the fast/cold split and make that transport
						 * the explicit SSA operand of both branches.
						 */
						const zend_mir_storage_id storage_id =
							canonical_storage(operand);
						const uint32_t reference =
							zend_mir_id_is_valid(storage_id)
								? machine_reference_index(
									ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
									storage_id)
								: UINT32_MAX;
						const IRValueRef address =
							reference != UINT32_MAX
								? add_derived_value(
									ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
									ZEND_MIR_SCALAR_TYPE_NONE,
									storage_id, false, 0, UINT8_MAX,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_UNKNOWN,
									reference)
								: INVALID_VALUE_REF;
						const IRValueRef transported =
							address != INVALID_VALUE_REF
								? add_derived_value(
									representation(operand),
									exact_type(operand),
									storage_id, false, 0,
									machine_kind(operand),
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									refcount_state(operand))
								: INVALID_VALUE_REF;
						if (transported == INVALID_VALUE_REF) {
							valid_ = false;
						} else {
							const uint32_t load_operand_offset =
								static_cast<uint32_t>(operands_.size());
							operands_.push_back(address);
							data_operand_helpers = true;
							add_node(block_instructions, block, InstNode{
								InstKind::ZvalPayloadLoad,
								i, UINT32_MAX, transported, {},
								load_operand_offset, 1, true,
								storage_id, exact_type(operand)});
							operand = transported;
						}
					}
				}
				if (operand == INVALID_VALUE_REF) {
					valid_ = false;
				}
				data_operands.push_back(operand);
			}
			if (data_operand_helpers) {
				const std::vector<IRValueRef> semantic_prefix(
					operands_.begin() + operand_offset,
					operands_.begin() + semantic_prefix_end);
				operand_offset = static_cast<uint32_t>(operands_.size());
				operands_.insert(operands_.end(),
					semantic_prefix.begin(), semantic_prefix.end());
			}
			operands_.insert(operands_.end(),
				data_operands.begin(), data_operands.end());
			if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
				/*
				 * A proven return-type check emits no machine instruction, but
				 * it remains a real SSA use. Keep that dependency visible to
				 * TPDE so a loop/merge PHI feeding the proof is released at the
				 * check instead of being mistaken for a live value at the block
				 * boundary.
				 */
				IRValueRef verified = source_binding_value_ref(
					instruction.source_op1_binding);
				if (verified == INVALID_VALUE_REF) {
					verified = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				if (verified != INVALID_VALUE_REF
						&& (machine_value_has_register_definition(
								verified)
							|| val_is_phi(verified))) {
					operands_.push_back(verified);
				}
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
				const IRValueRef returned = source_binding_value_ref(
					instruction.source_op1_binding);
				const uint32_t return_source_position =
					instruction.value_operation.source_position_id;
				const uint8_t return_producer_opcode =
					return_source_position != 0
						&& return_source_position <= plan_->source_opcode_count
					? plan_->source_opcodes[return_source_position - 1].opcode
					: ZEND_NOP;
				const bool immediate_call_result =
					return_producer_opcode == ZEND_DO_UCALL
					|| return_producer_opcode == ZEND_DO_FCALL
					|| return_producer_opcode == ZEND_DO_FCALL_BY_NAME
					|| return_producer_opcode == ZEND_DO_ICALL;
				const int32_t return_definition =
					instruction.source_op1_binding
						.definition_instruction_index;
				const bool immediate_register_call_result =
					return_definition >= 0
					&& static_cast<uint32_t>(return_definition)
						< plan_->instruction_count
					&& (frozen_typed_component_call(
							static_cast<uint32_t>(return_definition))
						|| frozen_effect_closed_inline(
							static_cast<uint32_t>(return_definition)));
				/* An ordinary Zend-entry call writes its canonical result slot.
				 * Optimized TMP reuse may otherwise expose the register for that
				 * slot's prior producer. A frozen local-ABI call instead defines the
				 * register result directly, both in a typed body and at Zend entry. */
				bool stale_canonical_return = false;
				if (function_mode_ == FunctionMode::ZendEntry
						&& returned != INVALID_VALUE_REF
						&& zend_mir_id_is_valid(
							instruction.value_operation.op1_storage_id)) {
					const uint32_t begin =
						instruction.source_op1_binding
									.definition_instruction_index < 0
							? 0
							: static_cast<uint32_t>(
								instruction.source_op1_binding
									.definition_instruction_index + 1);
					for (uint32_t candidate_index = begin;
							candidate_index < i; ++candidate_index) {
						const zend_tpde_instruction &candidate =
							plan_->instructions[candidate_index];
						if (candidate.has_value_operation
								&& ((candidate.record.opcode
										== ZEND_MIR_OPCODE_VALUE_BINARY_OP
									|| candidate.record.opcode
										== ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
									|| candidate.record.opcode
										== ZEND_MIR_OPCODE_VALUE_INCDEC
									|| candidate.record.opcode
										== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM
									|| candidate.record.opcode
										== ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP)
									|| (candidate.record.opcode
											== ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
										&& candidate.runtime_helper
											!= ZEND_NATIVE_HELPER_COUNT))
								&& candidate.value_operation.op1_storage_id
									== instruction.value_operation
										.op1_storage_id) {
							stale_canonical_return = true;
							break;
						}
					}
				}
				if (function_mode_ == FunctionMode::TypedBody
						&& returned == INVALID_VALUE_REF) {
					valid_ = false;
				}
				if (instruction.value_operation.source_opcode == ZEND_RETURN
					&& returned != INVALID_VALUE_REF
					&& (function_mode_ == FunctionMode::TypedBody
							|| !immediate_call_result
							|| immediate_register_call_result)
						&& !stale_canonical_return
						/*
						 * ARRAY_PTR values in the Zend entry may expose a
						 * borrowed payload view of the canonical result slot.
						 * Returning that pointer directly would copy/addref a
						 * TMP/VAR source instead of transferring and undefining
						 * its zval. Keep those entry returns on the
						 * ownership-aware helper path. A private typed body on a
						 * target with register-authoritative zval extensions has
						 * no canonical frame and must return its ABI value.
						 */
						&& ((function_mode_ == FunctionMode::TypedBody)
							|| machine_kind(returned)
								!= ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR)
						&& machine_value_has_register_definition(
							returned)) {
					operands_.push_back(returned);
				}
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& (record.opcode == ZEND_MIR_OPCODE_RETURN
					|| record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
					|| instruction.source_effect != 0)
					&& !(record.opcode
							== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
						&& static_slot_isset_machine_fast
						&& machine_result
						&& guarded_cold_blocks[i] == UINT32_MAX)) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					&& (record.effects & ZEND_MIR_EFFECT_MASK(
						ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
				operands_.push_back(
					IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& zend_mir_opcode_is_executable_value(record.opcode)
					&& !boxed_cond_branch
					&& !register_cond_branch
					&& (register_bool_unary_operand == INVALID_VALUE_REF
						|| guarded_cold_blocks[i] != UINT32_MAX)
					&& !(record.opcode
							== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
						&& static_slot_isset_machine_fast
						&& machine_result
						&& guarded_cold_blocks[i] == UINT32_MAX)
					&& !(record.opcode
							== ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
						&& instruction.runtime_helper
							== ZEND_NATIVE_HELPER_COUNT)) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode
					== ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& boxed_cond_branch) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
				if (boxed_cond_cold_blocks[i] != UINT32_MAX) {
					if (register_boxed_condition_operand != INVALID_VALUE_REF) {
						operands_.push_back(register_boxed_condition_operand);
					} else if (register_string_condition_operand
							!= INVALID_VALUE_REF) {
						operands_.push_back(register_string_condition_operand);
					}
				} else if (register_boxed_condition_operand
							!= INVALID_VALUE_REF) {
					operands_.push_back(register_boxed_condition_operand);
				}
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
				const bool typed_local_call =
					frozen_typed_component_call(i);
				if (typed_local_call) {
					std::vector<IRValueRef> typed_call_values;
					typed_call_values.reserve(
						instruction.call_argument_count);
					for (uint32_t n = 0;
							n < instruction.call_argument_count; ++n) {
						zend_mir_call_argument_ref argument{};
						if (!zend_tpde_call_argument_at(plan_,
								instruction.call_argument_offset + n,
								&argument)) {
							valid_ = false;
							typed_call_values.push_back(
								INVALID_VALUE_REF);
							continue;
						}
						const uint32_t call_argument_index =
							instruction.call_argument_offset + n;
						IRValueRef value = source_binding_value_ref(
							plan_->call_argument_bindings[
								call_argument_index]);
						if (value == INVALID_VALUE_REF) {
							value = source_operand_value_ref(
								argument.source_operand);
						}
						const zend_tpde_plan *callee =
							instruction.component_target_index
									< component_plans_.size()
								? component_plans_[
									instruction.component_target_index]
								: nullptr;
						const int32_t callee_value =
							callee != nullptr
									&& callee->argument_value_indices != nullptr
									&& n < callee->argument_count
								? callee->argument_value_indices[n] : -1;
						const TypedBodyAbiType transport_abi =
							callee_value >= 0
									&& static_cast<uint32_t>(callee_value)
										< callee->value_count
								? typed_body_value_abi(
									callee,
									static_cast<uint32_t>(callee_value))
								: TypedBodyAbiType{};
						/*
						 * A Zend-entry value can remain canonical-slot
						 * authoritative until a proven local ABI call needs
						 * its machine representation. Materialize scalar,
						 * pointer, and two-part boxed values only in the
						 * observer-free hot block; the cold call continues to
						 * consume the canonical frame.
						 */
						const bool transport_scalar =
							transport_abi.valid
							&& zend_mir_scalar_type_is_exact(
								transport_abi.exact_type)
							&& transport_abi.exact_type
								!= ZEND_MIR_SCALAR_TYPE_NULL;
						const bool transport_pointer =
							transport_abi.valid
							&& machine_pointer_kind(
								transport_abi.machine_kind);
						const bool transport_boxed =
							transport_abi.valid
							&& transport_abi.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
						const bool matching_register_value =
							value != INVALID_VALUE_REF
							&& representation(value)
								== transport_abi.representation
							&& exact_type(value)
								== transport_abi.exact_type
							&& machine_kind(value)
								== transport_abi.machine_kind
							&& machine_value_is_register_authoritative(
								value)
							&& machine_value_has_register_definition(value);
						const bool guarded_boxed_pointer =
							function_mode_ == FunctionMode::ZendEntry
							&& transport_pointer
							&& value != INVALID_VALUE_REF
							&& representation(value)
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& machine_kind(value)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& ownership(value)
								== ZEND_MIR_OWNERSHIP_STATE_OWNED
							&& machine_value_is_register_authoritative(value)
							&& machine_value_has_register_definition(value)
							&& n < instruction.direct_call->argument_count
							&& instruction.direct_call->arguments[n].exact_type
								== transport_abi.exact_type
							&& guarded_hot_blocks[i] != UINT32_MAX
							&& guarded_cold_blocks[i] != UINT32_MAX;
						if (guarded_boxed_pointer) {
							const IRValueRef boxed = value;
							const IRValueRef transported = add_derived_value(
								transport_abi.representation,
								transport_abi.exact_type,
								canonical_storage(boxed), false, 0,
								transport_abi.machine_kind,
								ZEND_MIR_OWNERSHIP_STATE_BORROWED,
								ZEND_MIR_REFCOUNT_UNKNOWN);
							if (transported == INVALID_VALUE_REF) {
								valid_ = false;
							} else {
								const uint32_t unbox_operand_offset =
									static_cast<uint32_t>(operands_.size());
								operands_.push_back(boxed);
								add_node(block_instructions,
									guarded_hot_blocks[i], InstNode{
										InstKind::UnboxPointer, i, UINT32_MAX,
										transported, {}, unbox_operand_offset,
										1, true, canonical_storage(boxed),
										transport_abi.exact_type});
								const uint32_t expected_zval_type =
									zend_tpde_machine_value_zval_type(
										transport_abi.machine_kind);
								if (expected_zval_type == IS_UNDEF) {
									valid_ = false;
								} else {
									typed_call_value_guards.emplace_back(
										boxed, expected_zval_type);
									typed_call_owned_boxed_arguments.push_back(
										boxed);
									value = transported;
								}
							}
						}
						if (function_mode_ == FunctionMode::ZendEntry
								&& (transport_scalar
									|| transport_pointer
									|| transport_boxed)
								&& !matching_register_value
								&& !guarded_boxed_pointer) {
							const zend_mir_storage_id storage_id =
								canonical_storage(value);
							const uint32_t reference =
								zend_mir_id_is_valid(storage_id)
									? machine_reference_index(
										ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
										storage_id)
									: UINT32_MAX;
							const IRValueRef address =
								reference != UINT32_MAX
									? add_derived_value(
										ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
										ZEND_MIR_SCALAR_TYPE_NONE,
										storage_id, false, 0, UINT8_MAX,
										ZEND_MIR_OWNERSHIP_STATE_BORROWED,
										ZEND_MIR_REFCOUNT_UNKNOWN,
										reference)
									: INVALID_VALUE_REF;
							const IRValueRef transported =
								address != INVALID_VALUE_REF
									? add_derived_value(
										transport_abi.representation,
										transport_abi.exact_type,
										storage_id, false, 0,
										transport_abi.machine_kind,
										ZEND_MIR_OWNERSHIP_STATE_BORROWED,
										ZEND_MIR_REFCOUNT_UNKNOWN)
									: INVALID_VALUE_REF;
							if (transported != INVALID_VALUE_REF) {
								const uint32_t load_operand_offset =
									static_cast<uint32_t>(operands_.size());
								operands_.push_back(address);
								const uint32_t transport_block =
									guarded_hot_blocks[i] != UINT32_MAX
										? guarded_hot_blocks[i]
										: static_cast<uint32_t>(block);
								add_node(block_instructions,
									transport_block, InstNode{
										InstKind::ZvalPayloadLoad,
										i, UINT32_MAX, transported, {},
										load_operand_offset, 1, true,
										storage_id,
										transport_abi.exact_type});
								value = transported;
							}
						}
						if (!machine_value_has_result_representation(value)
								|| !machine_value_has_register_definition(value)) {
							valid_ = false;
						}
						typed_call_values.push_back(value);
					}
					operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.insert(operands_.end(),
						typed_call_values.begin(),
						typed_call_values.end());
					if (function_mode_ == FunctionMode::ZendEntry) {
						operands_.push_back(
							IRValueRef{FRAME_VALUE});
						operands_.push_back(
							IRValueRef{FRAME_VALUE});
						direct_call_context_operand =
							static_cast<uint32_t>(operands_.size())
								- operand_offset;
						for (uint32_t use = 0; use < 3; ++use) {
							operands_.push_back(
								IRValueRef{
									EXECUTION_CONTEXT_ARGUMENT});
						}
					}
				} else {
				/*
				 * A proven inline descriptor materializes exact scalar payloads
				 * and boxed zvals in the generated Zend frame. Keep repeated
				 * frame/context uses explicit so TPDE's reference counts match
				 * both generated paths; a non-inline boxed result uses one
				 * additional caller-frame reference to load both value parts
				 * after the call boundary.
				 */
				const bool dynamic_direct_call =
					instruction.direct_call == nullptr
					&& instruction.user_call != nullptr
					&& instruction.user_call->do_opcode
						!= ZEND_CALLABLE_CONVERT
					&& instruction.user_call->do_opcode
						!= ZEND_CALLABLE_CONVERT_PARTIAL;
				uint32_t frame_use_count;
				if (instruction.direct_call != nullptr) {
					if ((instruction.direct_call->flags
							& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0) {
						for (uint32_t n = 0;
								n < instruction.call_argument_count; ++n) {
							zend_mir_call_argument_ref argument;
							if (!zend_tpde_call_argument_at(plan_,
									instruction.call_argument_offset + n,
									&argument)) {
								valid_ = false;
								operands_.push_back(INVALID_VALUE_REF);
								continue;
							}
							IRValueRef value = source_binding_value_ref(
								plan_->call_argument_bindings[
									instruction.call_argument_offset + n]);
							if (value == INVALID_VALUE_REF
									&& zend_mir_id_is_valid(
										argument.value_id)) {
								value = value_ref(argument.value_id);
							}
							if (value != INVALID_VALUE_REF
									&& !machine_value_has_result_representation(
										value)) {
								/*
								 * A canonical-only source is not an SSA
								 * machine value.  The generated frame path
								 * consumes the caller frame plus the frozen
								 * byte offset and performs the zval copy
								 * directly.
								 */
								value = IRValueRef{FRAME_VALUE};
							}
							if (value == INVALID_VALUE_REF) {
								value = IRValueRef{FRAME_VALUE};
							}
							if (value == INVALID_VALUE_REF) {
								valid_ = false;
							}
							operands_.push_back(value);
						}
						frame_use_count =
							(instruction.direct_call->flags
									& ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME)
								!= 0
							? 3
							: 6 + machine_result
								+ (((instruction.direct_call->expected_function
										->common.fn_flags & ZEND_ACC_VARIADIC) != 0)
									? 2 : 0);
					} else {
						frame_use_count = 2
							+ (machine_result
								&& machine_kind(result)
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
					}
				} else if (dynamic_direct_call) {
					frame_use_count = 2;
				} else {
					uint32_t setter_count = instruction.operand_count == 0
						? instruction.call_argument_count
						: instruction.operand_count;
					frame_use_count = setter_count + 2 + machine_result;
				}
				for (uint32_t n = 0; n < frame_use_count; ++n) {
					operands_.push_back(IRValueRef{FRAME_VALUE});
				}
				if (instruction.direct_call != nullptr) {
					direct_call_context_operand =
						static_cast<uint32_t>(operands_.size())
							- operand_offset;
					const uint32_t context_use_count =
						(instruction.direct_call->flags
							& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0
						? (instruction.direct_call->flags
							& ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME)
								!= 0
							? inlined_user_body.valid
								? 5
								: 5 + machine_result
							: 7
						: 3;
					for (uint32_t n = 0; n < context_use_count; ++n) {
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
					}
					if (inlined_user_body.valid) {
						if (inlined_user_body.checked()) {
							if (!inlined_user_body.checked_operands.empty()) {
								operands_.insert(operands_.end(),
									inlined_user_body.checked_operands.begin(),
									inlined_user_body.checked_operands.end());
							} else {
								operands_.push_back(
									inlined_user_body.checked_left);
								operands_.push_back(
									inlined_user_body.checked_right);
							}
						} else {
							operands_.push_back(
								inlined_user_body.value);
						}
					}
				} else if (dynamic_direct_call) {
					for (uint32_t n = 0; n < 3; ++n) {
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
					}
				}
				}
			} else if (record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
				std::vector<IRValueRef> argument_operands;
				argument_operands.reserve(instruction.call_argument_count);
				direct_internal_argument_transport =
					function_mode_ == FunctionMode::ZendEntry
					&& instruction.direct_internal_call != nullptr
					&& plan_->generator_resume_count == 0
					&& instruction.call_argument_count != 0;
				for (uint32_t n = 0;
						direct_internal_argument_transport
							&& n < instruction.call_argument_count; ++n) {
					const uint32_t argument_index =
						instruction.call_argument_offset + n;
					zend_mir_call_argument_ref argument{};
					if (argument_index >= plan_->call_argument_count
							|| !zend_tpde_call_argument_at(
								plan_, argument_index, &argument)) {
						direct_internal_argument_transport = false;
						break;
					}
					/*
					 * A source operation may replace the canonical plan value with a
					 * register-authoritative result after the call binding was frozen.
					 * Prefer the exact SEND SSA version so an immediately consumed
					 * scalar (for example strlen($value) passed to var_dump()) is
					 * placed from that register rather than from its still-undefined
					 * temporary Zend slot.
					 */
					const zend_tpde_source_value_binding &binding =
						plan_->call_argument_bindings[argument_index];
					IRValueRef value = source_binding_value_ref(binding);
					if (!machine_value_has_result_representation(value)
							&& instruction.materialization_offset
								<= plan_->materialization_count
							&& instruction.materialization_count
								<= plan_->materialization_count
									- instruction.materialization_offset) {
						for (uint32_t materialization_index = 0;
								materialization_index
									< instruction.materialization_count;
								++materialization_index) {
							const zend_tpde_materialization &materialization =
								plan_->materializations[
									instruction.materialization_offset
										+ materialization_index];
							if (materialization.value_index != UINT32_MAX
									|| materialization.source_value_index
										!= binding.value_index
									|| materialization
										.source_definition_instruction_index < 0) {
								continue;
							}
							const IRValueRef materialized =
								source_binding_value_ref({
									materialization.source_value_index,
									materialization
										.source_definition_instruction_index,
								});
							if (machine_value_has_result_representation(
									materialized)) {
								value = materialized;
								break;
							}
						}
					}
					if (value == INVALID_VALUE_REF
							&& zend_mir_id_is_valid(argument.value_id)) {
						value = value_ref(argument.value_id);
					}
					if (value == INVALID_VALUE_REF) {
						value = source_operand_value_ref(
							argument.source_operand);
					}
					const zend_mir_scalar_type_mask type =
						value != INVALID_VALUE_REF
							? exact_type(value)
							: ZEND_MIR_SCALAR_TYPE_NONE;
					const zend_tpde_machine_value_kind kind =
						value != INVALID_VALUE_REF
							? machine_kind(value)
							: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
					const bool direct_scalar_representation =
						(type == ZEND_MIR_SCALAR_TYPE_I1
							&& kind == ZEND_TPDE_MACHINE_VALUE_BOOL)
						|| (type == ZEND_MIR_SCALAR_TYPE_I64
							&& kind == ZEND_TPDE_MACHINE_VALUE_I64)
						|| (type == ZEND_MIR_SCALAR_TYPE_F64
							&& kind == ZEND_TPDE_MACHINE_VALUE_F64);
					const bool direct_scalar =
						instruction.direct_internal_call->arguments[n].mode
							== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
						&& value != INVALID_VALUE_REF
						&& machine_value_has_result_representation(value)
						&& direct_scalar_representation
						&& zend_mir_scalar_type_is_exact(type)
						&& (type == ZEND_MIR_SCALAR_TYPE_NULL
							|| type == ZEND_MIR_SCALAR_TYPE_I1
							|| type == ZEND_MIR_SCALAR_TYPE_I64
							|| type == ZEND_MIR_SCALAR_TYPE_F64);
					const bool direct_boxed_temporary =
						instruction.direct_internal_call->arguments[n].mode
							== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
						&& value != INVALID_VALUE_REF
						&& machine_value_has_result_representation(value)
						&& machine_value_has_register_definition(value)
						&& kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& (argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| argument.source_operand.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& (argument.source_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_TMP
							|| argument.source_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_VAR)
						&& zend_mir_id_is_valid(
							source_operand_storage_id(argument.source_operand));
					const bool stable_source =
						direct_internal_source_argument_stable(
							argument, record.source_position_id);
					if (direct_scalar || direct_boxed_temporary) {
						argument_operands.push_back(value);
						if (direct_boxed_temporary) {
							++direct_internal_boxed_argument_count;
						}
						continue;
					}
					/*
					 * Immutable literals and source slots proven untouched after their
					 * SEND remain safe to decode after the other arguments have been
					 * frozen. A reused slot must instead use an exact transported SSA
					 * scalar above, or keep the whole call on the conservative path.
					 */
					if (instruction.direct_internal_call->arguments[n].mode
							== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
							&& (argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL
								|| stable_source)) {
						argument_operands.push_back(IRValueRef{FRAME_VALUE});
						continue;
					}
					direct_internal_argument_transport = false;
					break;
				}
				if (direct_internal_argument_transport) {
					operands_.insert(operands_.end(),
						argument_operands.begin(), argument_operands.end());
					/*
					 * begin, each setter, finish, and an optional result read
					 * are independent calls. Keep every frame use explicit for
					 * TPDE liveness across those runtime boundaries.
					 */
					const uint32_t frame_uses =
						instruction.call_argument_count + 2
						+ (machine_result ? 1 : 0)
						+ direct_internal_boxed_argument_count;
					for (uint32_t n = 0; n < frame_uses; ++n) {
						operands_.push_back(IRValueRef{FRAME_VALUE});
					}
				} else {
					/* One direct Zend-runtime boundary returns status and payload. */
					operands_.push_back(IRValueRef{FRAME_VALUE});
				}
			} else if (record.opcode
					== ZEND_MIR_OPCODE_CATCH_ENTER
					|| record.opcode
						== ZEND_MIR_OPCODE_FINALLY_ENTER
					|| record.opcode
						== ZEND_MIR_OPCODE_FINALLY_CALL) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			} else if (record.opcode
					== ZEND_MIR_OPCODE_FINALLY_RETURN) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			uint32_t operand_count =
				static_cast<uint32_t>(operands_.size()) - operand_offset;
			uint32_t inlined_operand_index =
				inlined_user_body.valid
					? operand_count - inlined_user_body.operand_count()
					: UINT32_MAX;
			const uint32_t materialization_count =
				function_mode_ == FunctionMode::ZendEntry
					? instruction.materialization_count : 0;
			const uint32_t materialization_operand_index =
				materialization_count == 0
					? UINT32_MAX : operand_count;
			if (instruction.materialization_offset
						> plan_->materialization_count
					|| instruction.materialization_count
						> plan_->materialization_count
							- instruction.materialization_offset) {
				valid_ = false;
			} else {
				for (uint32_t n = 0;
						n < materialization_count; ++n) {
					const zend_tpde_materialization &materialization =
						plan_->materializations[
							instruction.materialization_offset + n];
					IRValueRef value = INVALID_VALUE_REF;
					if (materialization.value_index < plan_->value_count) {
						value = resolve_materializable_scalar(
							materialization.value_index, block, i, 0,
							resolve_materializable_scalar);
						if (value == INVALID_VALUE_REF
								|| !machine_value_has_result_representation(value)
								|| !machine_value_has_register_definition(value)) {
							const IRValueRef canonical{
								MIR_VALUE_BASE + materialization.value_index};
							const zend_mir_storage_id source_storage =
								canonical_storage(canonical);
							const uint32_t reference =
								zend_mir_id_is_valid(source_storage)
									? machine_reference_index(
										ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
										source_storage)
									: UINT32_MAX;
							const IRValueRef address = reference != UINT32_MAX
								? add_derived_value(
									ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
									ZEND_MIR_SCALAR_TYPE_NONE, source_storage,
									false, 0, UINT8_MAX,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_UNKNOWN, reference)
								: INVALID_VALUE_REF;
							const IRValueRef loaded = address != INVALID_VALUE_REF
								? add_derived_value(
									representation(canonical), exact_type(canonical),
									source_storage, false, 0,
									machine_kind(canonical),
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									refcount_state(canonical))
								: INVALID_VALUE_REF;
							if (loaded != INVALID_VALUE_REF) {
								const std::vector<IRValueRef> semantic_prefix(
									operands_.begin() + operand_offset,
									operands_.end());
								const uint32_t load_operand_offset =
									static_cast<uint32_t>(operands_.size());
								operands_.push_back(address);
								add_node(block_instructions, block, InstNode{
									InstKind::ZvalPayloadLoad, i, UINT32_MAX,
									loaded, {}, load_operand_offset, 1, true,
									source_storage, exact_type(canonical)});
								operand_offset =
									static_cast<uint32_t>(operands_.size());
								operands_.insert(operands_.end(),
									semantic_prefix.begin(), semantic_prefix.end());
								value = loaded;
							}
						}
					} else if (materialization.value_index == UINT32_MAX) {
						value = source_binding_value_ref({
							materialization.source_value_index,
							materialization
								.source_definition_instruction_index,
						});
						if ((value == INVALID_VALUE_REF
								|| !machine_value_has_result_representation(value))
								&& materialization.machine_kind
									== ZEND_TPDE_MACHINE_VALUE_I64
								&& materialization
									.source_definition_instruction_index >= 0
								&& static_cast<uint32_t>(materialization
									.source_definition_instruction_index)
									< plan_->instruction_count) {
							const zend_tpde_instruction &definition =
								plan_->instructions[static_cast<uint32_t>(
									materialization
										.source_definition_instruction_index)];
							const uint32_t reference =
								definition.has_value_operation
									&& definition.value_operation.source_opcode
										== ZEND_STRLEN
								? machine_reference_index(
									ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
									materialization.storage_id)
								: UINT32_MAX;
							const IRValueRef address = reference != UINT32_MAX
								? add_derived_value(
									ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
									ZEND_MIR_SCALAR_TYPE_NONE,
									materialization.storage_id, false, 0,
									UINT8_MAX,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_UNKNOWN, reference)
								: INVALID_VALUE_REF;
							const IRValueRef loaded = address != INVALID_VALUE_REF
								? add_derived_value(
									ZEND_MIR_REPRESENTATION_I64,
									ZEND_MIR_SCALAR_TYPE_I64,
									materialization.storage_id, false, 0,
									ZEND_TPDE_MACHINE_VALUE_I64,
									ZEND_MIR_OWNERSHIP_STATE_BORROWED,
									ZEND_MIR_REFCOUNT_IMMORTAL)
								: INVALID_VALUE_REF;
							if (loaded != INVALID_VALUE_REF) {
								const std::vector<IRValueRef> semantic_prefix(
									operands_.begin() + operand_offset,
									operands_.end());
								const uint32_t load_operand_offset =
									static_cast<uint32_t>(operands_.size());
								operands_.push_back(address);
								add_node(block_instructions, block, InstNode{
									InstKind::ZvalPayloadLoad, i, UINT32_MAX,
									loaded, {}, load_operand_offset, 1, true,
									materialization.storage_id,
									ZEND_MIR_SCALAR_TYPE_I64});
								operand_offset =
									static_cast<uint32_t>(operands_.size());
								operands_.insert(operands_.end(),
									semantic_prefix.begin(), semantic_prefix.end());
								value = loaded;
							}
						}
					}
					if (value == INVALID_VALUE_REF
								|| !machine_value_has_result_representation(value)) {
						valid_ = false;
						operands_.push_back(INVALID_VALUE_REF);
					} else {
						operands_.push_back(value);
					}
					++operand_count;
				}
			}
			const uint32_t boxed_cond_cold_block =
				boxed_cond_cold_blocks[i];
			const uint32_t guarded_cold_block =
				guarded_cold_blocks[i];
			IRValueRef boxed_op1_boundary = INVALID_VALUE_REF;
			IRValueRef boxed_op2_boundary = INVALID_VALUE_REF;
			uint32_t boxed_op1_boundary_operand_index = UINT32_MAX;
			uint32_t boxed_op2_boundary_operand_index = UINT32_MAX;
			uint32_t boundary_semantic_operand_count = UINT32_MAX;
			if (function_mode_ == FunctionMode::ZendEntry
					&& instruction.has_value_operation
					&& zend_mir_opcode_is_executable_value(record.opcode)) {
				auto direct_boxed_source = [&](const auto &binding,
						const zend_mir_source_operand_ref &source,
						zend_mir_storage_id storage,
						uint32_t definition_ssa_variable_id_plus_one) {
					IRValueRef candidate = source_binding_value_ref(binding);
					if ((candidate == INVALID_VALUE_REF
							|| machine_kind(candidate)
								!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)
							&& definition_ssa_variable_id_plus_one != 0) {
						candidate = value_ref(zend_mir_value_from_original_ssa(
							definition_ssa_variable_id_plus_one - 1));
					}
					if (candidate == INVALID_VALUE_REF) {
						candidate = source_operand_value_ref(source);
					}
					int32_t definition = binding.definition_instruction_index;
					if (definition < 0 && binding.value_index >= 0
							&& static_cast<uint32_t>(binding.value_index)
								< source_producer_by_value.size()) {
						definition = source_producer_by_value[
							static_cast<uint32_t>(binding.value_index)];
					}
					if (definition < 0 && candidate != INVALID_VALUE_REF) {
						const uint32_t candidate_index =
							static_cast<uint32_t>(candidate);
						definition = candidate_index
								< latest_source_producer_by_ir_value.size()
							? latest_source_producer_by_ir_value[candidate_index] : -1;
					}
					const bool direct_definition = definition >= 0
						&& static_cast<uint32_t>(definition)
							< active_instruction_results().size()
						&& static_cast<uint32_t>(definition)
							< plan_->instruction_count
						&& (plan_->instructions[
							static_cast<uint32_t>(definition)].record.opcode
								== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
							|| plan_->instructions[
								static_cast<uint32_t>(definition)].record.opcode
								== ZEND_MIR_OPCODE_OBJECT_FETCH_R)
						&& static_cast<uint32_t>(definition)
							< source_result_consumer.size()
						&& source_result_consumer[
							static_cast<uint32_t>(definition)]
							== static_cast<int32_t>(i);
					const IRValueRef produced = direct_definition
						? active_instruction_results()[
							static_cast<uint32_t>(definition)]
						: INVALID_VALUE_REF;
					return direct_definition
							&& produced != INVALID_VALUE_REF
							&& canonical_storage(produced) == storage
							&& machine_kind(produced)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& machine_value_is_register_authoritative(produced)
							&& machine_value_has_register_definition(produced)
						? produced : INVALID_VALUE_REF;
				};
				boxed_op1_boundary = direct_boxed_source(
					instruction.source_op1_binding,
					instruction.value_operation.op1,
					instruction.value_operation.op1_storage_id,
					instruction.value_operation
						.op1_definition_ssa_variable_id_plus_one);
				boxed_op2_boundary = direct_boxed_source(
					instruction.source_op2_binding,
					instruction.value_operation.op2,
					instruction.value_operation.op2_storage_id,
					instruction
						.source_op2_definition_ssa_variable_id_plus_one);
			}
			auto append_boundary_operand = [&](IRValueRef candidate,
					uint32_t segment_offset, uint32_t semantic_count,
					uint32_t &segment_count) {
					if (candidate == INVALID_VALUE_REF) {
						return UINT32_MAX;
					}
					const auto begin = operands_.begin() + segment_offset;
					const auto existing = std::find(
						begin, begin + semantic_count, candidate);
					if (existing != begin + semantic_count) {
						return UINT32_MAX;
					}
					const uint32_t index = segment_count++;
					operands_.push_back(candidate);
					return index;
			};
			if (boxed_cond_cold_block == UINT32_MAX
					&& guarded_cold_block == UINT32_MAX) {
				const uint32_t semantic_count =
					materialization_operand_index == UINT32_MAX
						? operand_count : materialization_operand_index;
				boxed_op1_boundary_operand_index = append_boundary_operand(
					boxed_op1_boundary, operand_offset, semantic_count,
					operand_count);
				if (boxed_op2_boundary
						== (boxed_op1_boundary_operand_index == UINT32_MAX
						? INVALID_VALUE_REF
						: operands_[operand_offset
							+ boxed_op1_boundary_operand_index])) {
					boxed_op2_boundary_operand_index =
						boxed_op1_boundary_operand_index;
				} else {
					boxed_op2_boundary_operand_index =
						append_boundary_operand(boxed_op2_boundary,
							operand_offset, semantic_count, operand_count);
				}
				if (boxed_op1_boundary_operand_index != UINT32_MAX
						|| boxed_op2_boundary_operand_index != UINT32_MAX) {
					boundary_semantic_operand_count = semantic_count;
				}
			}
			auto reload_slot_authoritative_source_result =
					[&](uint32_t reload_block) {
				if (function_mode_ != FunctionMode::ZendEntry
						|| source_result_used[i] == 0
						|| record.opcode == ZEND_MIR_OPCODE_VALUE_INIT_ARRAY
						|| machine_value_has_result_representation(
							active_instruction_results()[i])) {
					return;
				}
				const IRValueRef canonical_result = source_binding_value_ref(
					instruction.source_result_binding);
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				const zend_mir_storage_id storage_id =
					canonical_storage(canonical_result);
				if (canonical_result == INVALID_VALUE_REF
						|| canonical_index < MIR_VALUE_BASE
						|| canonical_index - MIR_VALUE_BASE >= plan_->value_count
						|| !zend_mir_id_is_valid(storage_id)
						|| exact_type(canonical_result)
							== ZEND_MIR_SCALAR_TYPE_NULL) {
					return;
				}
				const uint32_t reference =
					machine_reference_index(
							ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
							storage_id);
				const IRValueRef address = reference != UINT32_MAX
					? add_derived_value(
						ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
						ZEND_MIR_SCALAR_TYPE_NONE, storage_id, false, 0,
						UINT8_MAX, ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						ZEND_MIR_REFCOUNT_UNKNOWN, reference)
					: INVALID_VALUE_REF;
				const bool source_strlen = instruction.has_value_operation
					&& record.opcode == ZEND_MIR_OPCODE_VALUE_UNARY_OP
					&& instruction.value_operation.source_opcode == ZEND_STRLEN;
				const zend_mir_representation reload_representation =
					source_strlen
						? ZEND_MIR_REPRESENTATION_I64
						: representation(canonical_result);
				const zend_mir_scalar_type_mask reload_type =
					source_strlen
						? ZEND_MIR_SCALAR_TYPE_I64
						: exact_type(canonical_result);
				const zend_tpde_machine_value_kind reload_kind =
					source_strlen
						? ZEND_TPDE_MACHINE_VALUE_I64
						: machine_kind(canonical_result);
				const IRValueRef loaded = address != INVALID_VALUE_REF
					? add_derived_value(
						reload_representation, reload_type, storage_id, false, 0,
						reload_kind,
						ZEND_MIR_OWNERSHIP_STATE_BORROWED,
						source_strlen
							? ZEND_MIR_REFCOUNT_IMMORTAL
							: refcount_state(canonical_result))
					: INVALID_VALUE_REF;
				if (loaded == INVALID_VALUE_REF) {
					valid_ = false;
					return;
				}
				const uint32_t load_operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(address);
				InstNode load{InstKind::ZvalPayloadLoad, i, UINT32_MAX,
					loaded, {}, load_operand_offset, 1, true,
					storage_id, reload_type};
				load.control_block = reload_block;
				add_node(block_instructions, reload_block, std::move(load));
				active_instruction_results()[i] = loaded;
			};
			auto materialize_boxed_result_for_helper = [&](
					uint32_t materialization_block, IRValueRef value) {
				if (source_result_materialized_for_helper[i] == 0) {
					return;
				}
				const zend_mir_storage_id storage = canonical_storage(value);
				if (value == INVALID_VALUE_REF
						|| machine_kind(value)
							!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| !zend_mir_id_is_valid(storage)) {
					valid_ = false;
					return;
				}
				const uint32_t store_operand_offset =
					static_cast<uint32_t>(operands_.size());
				operands_.push_back(value);
				operands_.push_back(IRValueRef{FRAME_VALUE});
				InstNode store{InstKind::ZvalBoxedStore, i, UINT32_MAX,
					INVALID_VALUE_REF, {}, store_operand_offset, 2, false,
					storage};
				store.control_block = materialization_block;
				add_node(block_instructions, materialization_block,
					std::move(store));
			};
			if (guarded_cold_block != UINT32_MAX) {
				const uint32_t guarded_hot_block =
					guarded_hot_blocks[i];
				const uint32_t continuation_block =
					guarded_continuation_blocks[i];
				const IRValueRef canonical_mutation_result =
					mutation_results[i] != INVALID_VALUE_REF
						? mutation_results[i]
						: instruction.mutation_lazy_scalar
							? mutation_value_ref(instruction)
							: INVALID_VALUE_REF;
				const bool scalar_mutation_result =
					canonical_mutation_result != INVALID_VALUE_REF
					&& representation(canonical_mutation_result)
						== ZEND_MIR_REPRESENTATION_I64
					&& exact_type(canonical_mutation_result)
						== ZEND_MIR_SCALAR_TYPE_I64
					&& machine_kind(canonical_mutation_result)
						== ZEND_TPDE_MACHINE_VALUE_I64;
				const bool boxed_mutation_result =
					canonical_mutation_result != INVALID_VALUE_REF
					&& representation(canonical_mutation_result)
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& zend_mir_id_is_valid(
						canonical_storage(canonical_mutation_result))
					&& canonical_storage(canonical_mutation_result)
						== instruction.value_operation.op1_storage_id;
				const bool register_mutation_result =
					scalar_mutation_result || boxed_mutation_result;
				IRValueRef mutation_result =
					mutation_results[i] != INVALID_VALUE_REF
						? mutation_results[i]
						: canonical_mutation_result;
				if (register_mutation_result && boxed_mutation_result
						&& mutation_results[i]
							== INVALID_VALUE_REF) {
					const uint32_t mutation_ssa =
						instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one - 1;
					const int32_t mutation_index =
						zend_tpde_value_index(
							plan_,
							zend_mir_value_from_original_ssa(
								mutation_ssa));
					mutation_result = add_derived_value(
						ZEND_MIR_REPRESENTATION_ZVAL,
						ZEND_MIR_SCALAR_TYPE_NONE,
						instruction.value_operation.op1_storage_id,
						false, 0,
						ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL,
						ownership(canonical_mutation_result),
						refcount_state(canonical_mutation_result));
					if (mutation_result == INVALID_VALUE_REF
							|| mutation_index < 0
							|| static_cast<uint32_t>(mutation_index)
								>= active_value_overrides().size()
							|| mutation_ssa
								>= active_source_ssa_overrides().size()
							|| i >= active_instruction_results().size()) {
						valid_ = false;
					} else {
						active_value_overrides()[
							static_cast<uint32_t>(mutation_index)] =
							mutation_result;
						active_source_ssa_overrides()[mutation_ssa] =
							mutation_result;
						active_instruction_results()[i] = mutation_result;
					}
				} else if (register_mutation_result && scalar_mutation_result
						&& i < active_instruction_results().size()) {
					active_instruction_results()[i] = mutation_result;
				}
				const IRValueRef guarded_result =
					machine_result ? result
					: register_mutation_result
						? mutation_result : INVALID_VALUE_REF;
				const IRValueRef fast_result =
					guarded_result != INVALID_VALUE_REF
					? add_derived_value(
						representation(guarded_result),
						exact_type(guarded_result),
						ZEND_MIR_ID_INVALID, false, 0,
						machine_kind(guarded_result))
					: INVALID_VALUE_REF;
				const IRValueRef cold_result =
					guarded_result != INVALID_VALUE_REF
					? add_derived_value(
						representation(guarded_result),
						exact_type(guarded_result),
						ZEND_MIR_ID_INVALID, false, 0,
						machine_kind(guarded_result))
					: INVALID_VALUE_REF;
				uint32_t fast_operand_offset = operand_offset;
				uint32_t fast_operand_count = operand_count;
				uint32_t fast_materialization_operand_index =
					materialization_operand_index;
				uint32_t fast_materialization_count =
					materialization_count;
				uint32_t fast_block = block;
				const bool value_assign_source_operand =
					(record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN
						|| record.opcode
							== ZEND_MIR_OPCODE_VALUE_QM_ASSIGN)
					&& operand_count != 0
					&& operands_[operand_offset]
						!= IRValueRef{FRAME_VALUE}
					&& !machine_reference(
						operands_[operand_offset], nullptr);
				const bool value_assign_register_source =
					value_assign_source_operand
					&& machine_value_is_register_authoritative(
						operands_[operand_offset])
					&& machine_value_has_register_definition(
						operands_[operand_offset])
					&& zend_tpde_machine_value_is_register_authoritative(
						machine_kind(operands_[operand_offset]));
				if (value_assign_source_operand
						&& !value_assign_register_source) {
					/*
					 * copy_slot() reads canonical sources directly from the
					 * frame. Do not manufacture an allocator-visible SSA use
					 * for a value that the target selector never reads.
					 */
					++fast_operand_offset;
					--fast_operand_count;
					if (fast_materialization_operand_index
							!= UINT32_MAX) {
						--fast_materialization_operand_index;
					}
					if (machine_reference_operand_index != UINT32_MAX) {
						--machine_reference_operand_index;
					}
				}
				if (record.opcode
						== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
						&& static_slot_isset_machine_fast
						&& machine_result) {
					/*
					 * The exact scalar fast path consumes only its register
					 * operand.  Keep the Zend frame and any materializations on
					 * the semantic cold node, where the generic helper needs them.
					 */
					fast_operand_count =
						static_slot_isset_needs_value ? 2 : 1;
					fast_materialization_operand_index = UINT32_MAX;
					fast_materialization_count = 0;
				}
				if (register_bool_unary_operand != INVALID_VALUE_REF) {
					/*
					 * The synthetic register BOOL/BOOL_NOT consumes only its I1
					 * input. Keep the canonical frame and materialization operands
					 * on the split cold node for the generic Zend helper.
					 */
					fast_operand_count = 1;
					fast_materialization_operand_index = UINT32_MAX;
					fast_materialization_count = 0;
				}
				if (guarded_hot_block != UINT32_MAX) {
					const uint32_t context_operand =
						direct_call_context_operand;
					const IRValueRef guard_context =
						context_operand != UINT32_MAX
								&& context_operand < operand_count
							? operands_[operand_offset + context_operand]
							: INVALID_VALUE_REF;
					if (guard_context == INVALID_VALUE_REF
							|| observers_enabled_reference
								== INVALID_VALUE_REF) {
						valid_ = false;
					}
					const uint32_t guard_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(guard_context);
					operands_.push_back(observers_enabled_reference);
					const bool guarded_boxed_values =
						!typed_call_value_guards.empty();
					if (guarded_boxed_values) {
						operands_.push_back(IRValueRef{FRAME_VALUE});
					}
					for (uint32_t n = 0;
							n < materialization_count; ++n) {
						const uint32_t materialization_operand =
							materialization_operand_index + n;
						if (materialization_operand >= operand_count) {
							valid_ = false;
							operands_.push_back(INVALID_VALUE_REF);
						} else {
							operands_.push_back(
								operands_[operand_offset
									+ materialization_operand]);
						}
					}
					for (const auto &[guarded_value, expected_type] :
							typed_call_value_guards) {
						operands_.push_back(guarded_value);
						operands_.push_back(add_derived_value(
							ZEND_MIR_REPRESENTATION_I64,
							ZEND_MIR_SCALAR_TYPE_I64,
							ZEND_MIR_ID_INVALID, true, expected_type));
					}
					InstNode guard{
						InstKind::TypedCallGuard, i,
						guarded_cold_block, INVALID_VALUE_REF, {},
						guard_operand_offset,
						2 + static_cast<uint32_t>(guarded_boxed_values)
							+ materialization_count
							+ static_cast<uint32_t>(
								typed_call_value_guards.size()) * 2,
						false,
						ZEND_MIR_ID_INVALID,
						ZEND_MIR_SCALAR_TYPE_NONE,
						false, {}, false, UINT32_MAX, UINT32_MAX,
						materialization_count == 0
							? UINT32_MAX
							: 2 + static_cast<uint32_t>(
								guarded_boxed_values),
						materialization_count};
					guard.control_block = block;
					guard.continuation_block = guarded_hot_block;
					add_node(block_instructions, block, std::move(guard));

					fast_operand_offset =
						static_cast<uint32_t>(operands_.size());
					const uint32_t fast_call_argument_count =
						frozen_typed_component_call(i)
							|| (instruction.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0
							? instruction.call_argument_count : 0;
					for (uint32_t n = 0;
							n < fast_call_argument_count; ++n) {
						operands_.push_back(
							operands_[operand_offset + n]);
					}
					for (IRValueRef owned_boxed :
							typed_call_owned_boxed_arguments) {
						operands_.push_back(owned_boxed);
					}
					fast_operand_count = fast_call_argument_count
						+ static_cast<uint32_t>(
							typed_call_owned_boxed_arguments.size());
					uint32_t fast_inlined_operand_index = UINT32_MAX;
					if (inlined_user_body.valid) {
						fast_inlined_operand_index = fast_operand_count;
						const uint32_t inlined_operand_count =
							inlined_user_body.operand_count();
						for (uint32_t n = 0;
								n < inlined_operand_count; ++n) {
							operands_.push_back(
								operands_[operand_offset
									+ inlined_operand_index + n]);
						}
						fast_operand_count += inlined_operand_count;
					}
					fast_materialization_operand_index = UINT32_MAX;
					fast_materialization_count = 0;
					fast_block = guarded_hot_block;
					inlined_operand_index =
						fast_inlined_operand_index;
				}
				InstNode fast{
					InstKind::GuardedFast, i, guarded_cold_block,
					fast_result, {}, fast_operand_offset,
					fast_operand_count,
					fast_result != INVALID_VALUE_REF,
					ZEND_MIR_ID_INVALID, static_slot_isset_exact_type,
					false, {}, false, UINT32_MAX, UINT32_MAX,
					fast_materialization_operand_index,
					fast_materialization_count};
				fast.control_block = fast_block;
				fast.continuation_block = continuation_block;
				fast.machine_reference_operand_index =
					machine_reference_operand_index;
				fast.assign_op_right_operand_index =
					assign_op_right_operand_index;
				fast.assign_op_left_operand_index =
					assign_op_left_operand_index;
				fast.packed_append_value_operand_index =
					packed_append_value_operand_index;
				fast.property_write_value_operand_index =
					property_write_value_operand_index;
				fast.inlined_user_body = inlined_user_body.valid;
				fast.inlined_operand_index = inlined_operand_index;
				fast.inlined_checked_source_opcode =
					inlined_user_body.checked_source_opcode;
				if (!inlined_user_body.checked_steps.empty()) {
					fast.inlined_checked_step_offset =
						static_cast<uint32_t>(inlined_checked_steps_.size());
					fast.inlined_checked_step_count = static_cast<uint32_t>(
						inlined_user_body.checked_steps.size());
					fast.inlined_checked_operand_count =
						inlined_user_body.operand_count();
					inlined_checked_steps_.insert(
						inlined_checked_steps_.end(),
						inlined_user_body.checked_steps.begin(),
						inlined_user_body.checked_steps.end());
				}
				fast.mutation_result = register_mutation_result;
				if (register_bool_unary_operand != INVALID_VALUE_REF) {
					fast.synthetic = true;
					fast.synthetic_record = record;
					fast.synthetic_record.opcode =
						instruction.value_operation.source_opcode
							== ZEND_BOOL_NOT
						? ZEND_MIR_OPCODE_I1_NOT
						: ZEND_MIR_OPCODE_COPY;
					fast.synthetic_record.representation =
						ZEND_MIR_REPRESENTATION_I1;
					fast.synthetic_record.effects = 0;
					fast.synthetic_record.reads = 0;
					fast.synthetic_record.writes = 0;
					fast.synthetic_record.barriers = 0;
					fast.synthetic_record.ownership_actions = 0;
				}
				add_node(block_instructions, fast_block, std::move(fast));

				const uint32_t cold_operand_offset =
					static_cast<uint32_t>(operands_.size());
				uint32_t cold_operand_count = operand_count;
				uint32_t cold_materialization_operand_index =
					materialization_operand_index;
				if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
					/*
					 * The split direct-call cold block enters and leaves through
					 * the runtime boundary. It observes no fast-path arguments or
					 * inline-body values: exposing those as cold operands creates
					 * false SSA uses and can make a chained call use a value before
					 * its continuation PHI defines it.
					 */
					/*
					 * The frame has a canonical fixed assignment. The execution
					 * context is a normal SSA value and the cold runtime path
					 * consumes it independently for ENTER, entry and LEAVE.
					 * Keep all three context uses explicit so TPDE owns its complete
					 * liveness instead of relying on a hidden pinned register.
					 */
					operands_.push_back(IRValueRef{FRAME_VALUE});
					for (uint32_t use = 0; use < 3; ++use) {
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
					}
					cold_operand_count =
						4 + materialization_count;
					cold_materialization_operand_index =
						materialization_count == 0
							? UINT32_MAX : 4;
					for (uint32_t n = 0;
							n < materialization_count; ++n) {
						operands_.push_back(
							operands_[operand_offset
								+ materialization_operand_index + n]);
					}
				} else {
					/*
					 * VALUE_ASSIGN's register-authoritative boxed source is a
					 * fast-path operand.  copy_slot() publishes it to the
					 * canonical temporary before selecting the cold edge, so
					 * the generic helper observes the frame and must not claim
					 * a second SSA use of the same value.
					 */
					uint32_t cold_operand_skip = 0;
					if (record.opcode
							== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV
						&& static_slot_isset_machine_fast
						&& machine_result) {
						cold_operand_skip =
							static_slot_isset_needs_value ? 1 : 0;
						while (cold_operand_skip + 1 < operand_count
								&& operands_[operand_offset
									+ cold_operand_skip]
									== IRValueRef{FRAME_VALUE}
								&& operands_[operand_offset
									+ cold_operand_skip + 1]
									== IRValueRef{FRAME_VALUE}) {
							++cold_operand_skip;
						}
					} else if (packed_append_value_operand_index != UINT32_MAX
							|| property_write_value_operand_index != UINT32_MAX) {
						cold_operand_skip = 1;
					} else if (register_bool_unary_operand != INVALID_VALUE_REF
							|| register_string_length_operand
								!= INVALID_VALUE_REF) {
						cold_operand_skip = 1;
					} else if (record.opcode
								== ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
							&& operand_count != 0
							&& operands_[operand_offset]
								!= IRValueRef{FRAME_VALUE}
							&& machine_kind(operands_[operand_offset])
								== ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
						cold_operand_skip = 1;
					} else if (value_assign_source_operand) {
						cold_operand_skip = 1;
					} else if (record.opcode
								== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
							&& materialization_operand_index
								!= UINT32_MAX) {
						while (cold_operand_skip
									< materialization_operand_index
								&& operands_[operand_offset
										+ cold_operand_skip]
									!= IRValueRef{FRAME_VALUE}) {
							++cold_operand_skip;
						}
					} else if (record.opcode
								== ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R) {
						while (cold_operand_skip < operand_count
								&& operands_[operand_offset
										+ cold_operand_skip]
									!= IRValueRef{FRAME_VALUE}) {
							++cold_operand_skip;
						}
					} else if (record.opcode
								== ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM) {
						while (cold_operand_skip < operand_count
								&& operands_[operand_offset
									+ cold_operand_skip]
									!= IRValueRef{FRAME_VALUE}) {
							++cold_operand_skip;
						}
					}
					for (uint32_t n = cold_operand_skip;
							n < operand_count; ++n) {
						operands_.push_back(
							operands_[operand_offset + n]);
					}
					cold_operand_count -= cold_operand_skip;
					if (cold_materialization_operand_index != UINT32_MAX) {
						cold_materialization_operand_index -=
							cold_operand_skip;
					}
				}
				const uint32_t cold_semantic_operand_count =
					cold_materialization_operand_index == UINT32_MAX
						? cold_operand_count
						: cold_materialization_operand_index;
				uint32_t cold_boxed_op1_boundary_operand_index =
					append_boundary_operand(boxed_op1_boundary,
						cold_operand_offset, cold_semantic_operand_count,
						cold_operand_count);
				uint32_t cold_boxed_op2_boundary_operand_index = UINT32_MAX;
				if (boxed_op2_boundary
						== (cold_boxed_op1_boundary_operand_index == UINT32_MAX
							? INVALID_VALUE_REF
							: operands_[cold_operand_offset
								+ cold_boxed_op1_boundary_operand_index])) {
					cold_boxed_op2_boundary_operand_index =
						cold_boxed_op1_boundary_operand_index;
				} else {
					cold_boxed_op2_boundary_operand_index =
						append_boundary_operand(boxed_op2_boundary,
							cold_operand_offset, cold_semantic_operand_count,
							cold_operand_count);
				}
				InstNode cold{
					InstKind::GuardedCold, i, guarded_cold_block,
					cold_result, {}, cold_operand_offset,
					cold_operand_count, cold_result != INVALID_VALUE_REF,
					ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
					false, {}, false, UINT32_MAX, UINT32_MAX,
					cold_materialization_operand_index,
					materialization_count};
				cold.control_block = guarded_cold_block;
				cold.continuation_block = continuation_block;
				cold.assign_op_right_operand_index =
					assign_op_right_operand_index;
				cold.assign_op_left_operand_index =
					assign_op_left_operand_index;
				cold.mutation_result = register_mutation_result;
				add_node(block_instructions, guarded_cold_block,
					std::move(cold));
				if (cold_boxed_op1_boundary_operand_index != UINT32_MAX
						|| cold_boxed_op2_boundary_operand_index != UINT32_MAX) {
					nodes_.back().semantic_operand_count =
						cold_semantic_operand_count;
				}
				nodes_.back().boxed_op1_boundary_operand_index =
					cold_boxed_op1_boundary_operand_index;
				nodes_.back().boxed_op2_boundary_operand_index =
					cold_boxed_op2_boundary_operand_index;
				if (cold_result != INVALID_VALUE_REF
						&& record.source_position_id
							< user_opcode_result_reload_sources_.size()) {
					user_opcode_result_reload_sources_[
						record.source_position_id] = 1;
				}

				if (mutation_result != INVALID_VALUE_REF
						&& !register_mutation_result) {
					const zend_mir_storage_id mutation_storage =
						canonical_storage(mutation_result);
					const uint32_t mutation_reference =
						zend_mir_id_is_valid(mutation_storage)
							? machine_reference_index(
								ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
								mutation_storage)
							: UINT32_MAX;
					const IRValueRef mutation_slot =
						mutation_reference != UINT32_MAX
							? add_derived_value(
								ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
								ZEND_MIR_SCALAR_TYPE_NONE,
								mutation_storage, false, 0, UINT8_MAX,
								ZEND_MIR_OWNERSHIP_STATE_BORROWED,
								ZEND_MIR_REFCOUNT_UNKNOWN,
								mutation_reference)
							: INVALID_VALUE_REF;
					if (mutation_slot == INVALID_VALUE_REF
							|| mutation_storage
								!= instruction.value_operation
									.op1_storage_id) {
						valid_ = false;
					} else {
						const uint32_t reload_operand_offset =
							static_cast<uint32_t>(operands_.size());
						operands_.push_back(mutation_slot);
						InstNode reload{
							InstKind::ZvalPayloadLoad, i, UINT32_MAX,
							mutation_result, {}, reload_operand_offset,
							1, true, mutation_storage,
							exact_type(mutation_result)};
						reload.control_block = continuation_block;
						add_node(block_instructions, continuation_block,
							std::move(reload));
					}
				}
				if (guarded_result != INVALID_VALUE_REF) {
					if (fast_result == INVALID_VALUE_REF
							|| cold_result == INVALID_VALUE_REF
							|| static_cast<uint32_t>(guarded_result)
								>= phi_input_slices_.size()
							|| phi_values_[
								static_cast<uint32_t>(
									guarded_result)] != 0) {
						valid_ = false;
					}
					const uint32_t phi_input_offset =
						static_cast<uint32_t>(phi_inputs_.size());
					phi_inputs_.push_back(
						{fast_result, IRBlockRef{
							guarded_hot_block == UINT32_MAX
								? block : guarded_hot_block}});
					phi_inputs_.push_back(
						{cold_result, IRBlockRef{guarded_cold_block}});
					phi_input_slices_[
						static_cast<uint32_t>(guarded_result)] = {
							phi_input_offset, 2};
					phi_values_[
						static_cast<uint32_t>(guarded_result)] = 1;
					block_phis.push_back(
						{continuation_block, guarded_result});
				}
				materialize_boxed_result_for_helper(
					continuation_block, guarded_result);
				if (function_mode_ == FunctionMode::ZendEntry
						&& record.opcode
							== ZEND_MIR_OPCODE_VALUE_ASSIGN
						&& instruction.has_value_operation) {
					/*
					 * A refcounted assignment must first update the canonical
					 * Zend slot, including replacement cleanup and ownership.
					 * After that semantic boundary the assigned payload pointer
					 * is stable and can become the authoritative TPDE value.
					 * Publish it at the joined continuation so both the inline
					 * copy and the cold helper produce the same register SSA
					 * definition.  This is what keeps literal arrays/strings
					 * out of repeated frame reloads in following loops.
					 */
					const uint32_t result_ssa =
						instruction.value_operation
							.op1_definition_ssa_variable_id_plus_one;
					const IRValueRef canonical =
						result_ssa != 0
							? value_ref(
								zend_mir_value_from_original_ssa(
									result_ssa - 1))
							: INVALID_VALUE_REF;
					const int32_t canonical_value_index =
						result_ssa != 0
							? zend_tpde_value_index(
								plan_,
								zend_mir_value_from_original_ssa(
									result_ssa - 1))
							: -1;
					const uint8_t pointer_kind =
						canonical != INVALID_VALUE_REF
							? machine_kind(canonical) : UINT8_MAX;
					uint64_t literal_length = 0;
					bool literal_truthy = false;
					const bool string_literal = canonical
							!= INVALID_VALUE_REF
						&& known_string_literal(canonical,
							&literal_length, &literal_truthy);
					const uint8_t literal_first_byte = string_literal
							&& literal_length == 1 && !literal_truthy
						? '0' : 0;
					const zend_mir_storage_id storage_id =
						instruction.value_operation.op1_storage_id;
					const uint32_t reference =
						zend_mir_id_is_valid(storage_id)
							? machine_reference_index(
								ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
								storage_id)
							: UINT32_MAX;
					if ((pointer_kind
								== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
							|| pointer_kind
								== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
							|| pointer_kind
								== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
							|| pointer_kind
								== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR)
							&& canonical_value_index >= 0
							&& static_cast<uint32_t>(
								canonical_value_index)
								< plan_->value_count
							&& reference != UINT32_MAX) {
						const IRValueRef address = add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE,
							storage_id, false, 0, UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN,
							reference);
						const IRValueRef pointer =
							i < register_assignment_results.size()
									&& register_assignment_results[i]
										!= INVALID_VALUE_REF
									&& machine_pointer_kind(machine_kind(
										register_assignment_results[i]))
								? register_assignment_results[i]
								: address != INVALID_VALUE_REF
									? add_derived_value(
										ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
										ZEND_MIR_SCALAR_TYPE_NONE,
										storage_id, false, 0, pointer_kind,
										ownership(canonical),
										refcount_state(canonical), UINT32_MAX,
										string_literal, literal_first_byte,
										literal_length)
									: INVALID_VALUE_REF;
						if (pointer == INVALID_VALUE_REF) {
							valid_ = false;
						} else {
							const uint32_t load_operand_offset =
								static_cast<uint32_t>(
									operands_.size());
							operands_.push_back(address);
							InstNode load{
								InstKind::ZvalPayloadLoad, i,
								UINT32_MAX, pointer, {},
								load_operand_offset, 1, true,
								storage_id,
								ZEND_MIR_SCALAR_TYPE_NONE};
							load.control_block =
								continuation_block;
							add_node(block_instructions,
								continuation_block,
								std::move(load));
							auto &value_overrides =
								active_value_overrides();
							auto &source_overrides =
								active_source_ssa_overrides();
							auto &instruction_results =
								active_instruction_results();
							instruction_results[i] = pointer;
							value_overrides[
								static_cast<uint32_t>(
									canonical_value_index)] =
									pointer;
							if (result_ssa != 0
									&& result_ssa - 1
										< source_overrides.size()) {
								source_overrides[result_ssa - 1] =
									pointer;
							}
						}
					}
				}
				reload_slot_authoritative_source_result(continuation_block);
				continue;
			}
			if (boxed_cond_cold_block != UINT32_MAX) {
				const uint32_t semantic_operand_count =
					materialization_operand_index == UINT32_MAX
						? operand_count
						: materialization_operand_index;
				add_node(block_instructions,
					static_cast<uint32_t>(block), InstNode{
						InstKind::BoxedCondGuard, i,
						boxed_cond_cold_block, result, {},
						operand_offset, operand_count,
						false,
						ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
						false, {}, false, UINT32_MAX, UINT32_MAX,
						materialization_operand_index,
						materialization_count});
				nodes_.back().control_block = block;
				IRValueRef boxed_condition = source_binding_value_ref(
					instruction.source_op1_binding);
				if (boxed_condition == INVALID_VALUE_REF) {
					boxed_condition = source_operand_value_ref(
						instruction.value_operation.op1);
				}
				if (boxed_condition != INVALID_VALUE_REF) {
					nodes_.back().exact_type = exact_type(boxed_condition);
				}
				const uint32_t cold_operand_offset =
					static_cast<uint32_t>(operands_.size());
					const uint32_t cold_semantic_operand_count =
						semantic_operand_count
							- (register_boxed_condition_operand
									!= INVALID_VALUE_REF
								|| register_string_condition_operand
									!= INVALID_VALUE_REF);
					for (uint32_t n = 0;
							n < cold_semantic_operand_count; ++n) {
						operands_.push_back(
							operands_[operand_offset + n]);
				}
				for (uint32_t n = 0;
						n < materialization_count; ++n) {
					operands_.push_back(
						operands_[operand_offset
							+ materialization_operand_index + n]);
				}
				const bool split_cold_branch =
					has_pending_phi_inputs(boxed_cond_cold_block);
				const IRValueRef cold_decision = split_cold_branch
					? add_derived_value(
						ZEND_MIR_REPRESENTATION_I1,
						ZEND_MIR_SCALAR_TYPE_I1,
						ZEND_MIR_ID_INVALID, false, 0,
						ZEND_TPDE_MACHINE_VALUE_BOOL)
					: INVALID_VALUE_REF;
				if (split_cold_branch
						&& cold_decision == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				add_node(block_instructions, boxed_cond_cold_block, InstNode{
					InstKind::BoxedCondCold, i,
					boxed_cond_cold_block, cold_decision, {},
					cold_operand_offset,
					cold_semantic_operand_count
						+ materialization_count,
					split_cold_branch,
					ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
					false, {}, false, UINT32_MAX, UINT32_MAX,
					materialization_count == 0
						? UINT32_MAX : cold_semantic_operand_count,
					materialization_count});
				nodes_.back().control_block = boxed_cond_cold_block;
				if (split_cold_branch) {
					emit_pending_phi_inputs(boxed_cond_cold_block, i);
					const uint32_t branch_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(cold_decision);
					add_node(block_instructions,
						boxed_cond_cold_block, InstNode{
							InstKind::BoxedCondColdBranch, i,
							boxed_cond_cold_block,
							INVALID_VALUE_REF, {},
							branch_operand_offset, 1, false});
					nodes_.back().control_block =
						boxed_cond_cold_block;
				}
				continue;
			}
			add_node(block_instructions, static_cast<uint32_t>(block), InstNode{
				type_check_selection
							!= ScalarTypeCheckSelection::Invalid
						|| register_cond_branch
						|| register_bool_unary_operand != INVALID_VALUE_REF
					? InstKind::MIR
					: executable_kind(instruction, record),
				i, UINT32_MAX, result, {},
				operand_offset, operand_count, machine_result,
				ZEND_MIR_ID_INVALID,
				static_slot_isset_machine_fast
					? static_slot_isset_exact_type
					: canonical_bool_unary_exact_type,
				false, {}, inlined_user_body.valid,
				inlined_operand_index,
				inlined_user_body.checked_source_opcode,
				materialization_operand_index,
				materialization_count});
			nodes_.back().direct_internal_argument_transport =
				direct_internal_argument_transport;
			nodes_.back().assign_op_right_operand_index =
				assign_op_right_operand_index;
			nodes_.back().assign_op_left_operand_index =
				assign_op_left_operand_index;
			nodes_.back().packed_append_value_operand_index =
				packed_append_value_operand_index;
			nodes_.back().property_write_value_operand_index =
				property_write_value_operand_index;
			nodes_.back().semantic_operand_count =
				boundary_semantic_operand_count;
			nodes_.back().boxed_op1_boundary_operand_index =
				boxed_op1_boundary_operand_index;
			nodes_.back().boxed_op2_boundary_operand_index =
				boxed_op2_boundary_operand_index;
			if (!inlined_user_body.checked_steps.empty()) {
				nodes_.back().inlined_checked_step_offset =
					static_cast<uint32_t>(inlined_checked_steps_.size());
				nodes_.back().inlined_checked_step_count =
					static_cast<uint32_t>(
						inlined_user_body.checked_steps.size());
				nodes_.back().inlined_checked_operand_count =
					inlined_user_body.operand_count();
				inlined_checked_steps_.insert(
					inlined_checked_steps_.end(),
					inlined_user_body.checked_steps.begin(),
					inlined_user_body.checked_steps.end());
			}
			nodes_.back().mutation_result =
				i < mutation_results.size()
					&& mutation_results[i] != INVALID_VALUE_REF;
			materialize_boxed_result_for_helper(
				static_cast<uint32_t>(block), result);
			reload_slot_authoritative_source_result(
				static_cast<uint32_t>(block));
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode
						== ZEND_MIR_OPCODE_VALUE_INIT_ARRAY
					&& instruction.has_value_operation) {
				auto &value_overrides = active_value_overrides();
				auto &source_overrides =
					active_source_ssa_overrides();
				auto &instruction_results =
					active_instruction_results();
				const IRValueRef canonical_result =
					source_binding_value_ref(
						instruction.source_result_binding);
				const uint32_t canonical_index =
					static_cast<uint32_t>(canonical_result);
				const zend_mir_storage_id storage_id =
					instruction.value_operation.result_storage_id;
				const uint32_t reference =
					zend_mir_id_is_valid(storage_id)
						? machine_reference_index(
							ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT,
							storage_id)
						: UINT32_MAX;
				const IRValueRef address =
					reference != UINT32_MAX
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE,
							storage_id, false, 0, UINT8_MAX,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN,
							reference)
						: INVALID_VALUE_REF;
				const IRValueRef array =
					address != INVALID_VALUE_REF
						? add_derived_value(
							ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
							ZEND_MIR_SCALAR_TYPE_NONE,
							storage_id, false, 0,
							ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR,
							ZEND_MIR_OWNERSHIP_STATE_BORROWED,
							ZEND_MIR_REFCOUNT_UNKNOWN)
						: INVALID_VALUE_REF;
				if (canonical_result == INVALID_VALUE_REF
						|| canonical_index < MIR_VALUE_BASE
						|| canonical_index - MIR_VALUE_BASE
							>= plan_->value_count
						|| array == INVALID_VALUE_REF) {
					valid_ = false;
				} else {
					const uint32_t load_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(address);
					add_node(block_instructions,
						static_cast<uint32_t>(block), InstNode{
							InstKind::ZvalPayloadLoad, i, UINT32_MAX,
							array, {}, load_operand_offset, 1, true,
							storage_id, ZEND_MIR_SCALAR_TYPE_NONE});
					instruction_results[i] = array;
					value_overrides[
						canonical_index - MIR_VALUE_BASE] = array;
					const uint32_t result_ssa =
						instruction.value_operation.result
							.ssa_variable_id;
					if (result_ssa < source_overrides.size()) {
						source_overrides[result_ssa] = array;
					}
				}
			}
			if (type_check_selection
					!= ScalarTypeCheckSelection::Invalid
					|| register_cond_branch
					|| register_bool_unary_operand != INVALID_VALUE_REF) {
				InstNode &selected = nodes_.back();
				selected.synthetic = true;
				selected.synthetic_record = record;
				selected.synthetic_record.opcode =
					register_cond_branch
						? ZEND_MIR_OPCODE_COND_BRANCH
					: register_bool_unary_operand != INVALID_VALUE_REF
						? instruction.value_operation.source_opcode
								== ZEND_BOOL_NOT
							? ZEND_MIR_OPCODE_I1_NOT
							: ZEND_MIR_OPCODE_COPY
					: type_check_selection
							== ScalarTypeCheckSelection::NotInput
						? ZEND_MIR_OPCODE_I1_NOT
						: ZEND_MIR_OPCODE_COPY;
				selected.synthetic_record.effects = 0;
				selected.synthetic_record.reads = 0;
				selected.synthetic_record.writes = 0;
				selected.synthetic_record.barriers = 0;
				selected.synthetic_record.ownership_actions = 0;
				if (!register_cond_branch) {
					selected.synthetic_record.representation =
						ZEND_MIR_REPRESENTATION_I1;
					selected.synthetic_record.result_id =
						ZEND_MIR_ID_INVALID;
				}
			}
		}
		if (std::ranges::any_of(pending_phi_inputs,
				[](const PendingPhiInput &pending) {
					return !pending.emitted;
				})) {
			valid_ = false;
		}
		if (std::find(generator_resume_emitted.begin(),
				generator_resume_emitted.end(), 0)
				!= generator_resume_emitted.end()) {
			valid_ = false;
		}
		/*
		 * Reloaded generator values are definitions at the continuation entry,
		 * not uses there.  Reporting them as GeneratorResume operands makes a
		 * loop-carried PHI input appear dead immediately after the reload,
		 * because TPDE accounts PHI-edge uses before instructions in the
		 * predecessor block.  Keep the values known to liveness through the end
		 * of that block without exposing them as semantic target operands.
		 */
		for (const BlockItem<IRInstRef> &resume_item : block_instructions) {
			const InstNode &resume_node =
				nodes_[static_cast<uint32_t>(resume_item.value)];
			if (resume_node.kind != InstKind::GeneratorResume
					|| resume_node.generator_resume_value_count == 0) {
				continue;
			}
			auto terminal = std::find_if(
				block_instructions.rbegin(), block_instructions.rend(),
				[&](const BlockItem<IRInstRef> &item) {
					return item.block == resume_item.block;
				});
			if (terminal == block_instructions.rend()) {
				valid_ = false;
				continue;
			}
			InstNode &terminal_node =
				nodes_[static_cast<uint32_t>(terminal->value)];
			const uint32_t old_offset = terminal_node.operand_offset;
			const uint32_t old_count = terminal_node.operand_count;
			const uint32_t old_liveness_operand_index =
				terminal_node.materialization_operand_index == UINT32_MAX
					? old_count
					: terminal_node.materialization_operand_index;
			const uint32_t new_offset =
				static_cast<uint32_t>(operands_.size());
			for (uint32_t operand = 0; operand < old_count; ++operand) {
				operands_.push_back(operands_[old_offset + operand]);
			}
			const uint32_t liveness_operand_offset =
				new_offset + old_liveness_operand_index;
			if (&terminal_node != &resume_node
					&& std::find(
						operands_.begin() + liveness_operand_offset,
						operands_.end(),
						IRValueRef{EXECUTION_CONTEXT_ARGUMENT})
						== operands_.end()) {
				/* The terminal reload resolves the resumed frame through the
				 * execution context, so keep that argument assigned even when
				 * no semantic instruction in the block otherwise consumes it. */
				operands_.push_back(
					IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
			}
			const auto resume_value_span = std::span<const IRValueRef>{
				generator_resume_values_}.subspan(
					resume_node.generator_resume_value_offset,
					resume_node.generator_resume_value_count);
			const std::vector<IRValueRef> resume_values{
				resume_value_span.begin(), resume_value_span.end()};
			for (const IRValueRef value : resume_values) {
				if (std::find(
						operands_.begin() + liveness_operand_offset,
						operands_.end(), value) == operands_.end()) {
					operands_.push_back(value);
				}
			}
			if (terminal_node.materialization_operand_index == UINT32_MAX) {
				terminal_node.materialization_operand_index = old_count;
			}
			terminal_node.operand_offset = new_offset;
			terminal_node.operand_count =
				static_cast<uint32_t>(operands_.size()) - new_offset;
			if (&terminal_node != &resume_node) {
				std::vector<IRValueRef> terminal_resume_values;
				if (terminal_node.generator_resume_value_count != 0) {
					const auto existing = std::span<const IRValueRef>{
						generator_resume_values_}.subspan(
							terminal_node.generator_resume_value_offset,
							terminal_node.generator_resume_value_count);
					terminal_resume_values.assign(
						existing.begin(), existing.end());
				}
				for (const IRValueRef value : resume_values) {
					if (std::find(terminal_resume_values.begin(),
							terminal_resume_values.end(), value)
							== terminal_resume_values.end()) {
						terminal_resume_values.push_back(value);
					}
				}
				terminal_node.generator_resume_value_offset =
					static_cast<uint32_t>(generator_resume_values_.size());
				terminal_node.generator_resume_value_count =
					static_cast<uint32_t>(terminal_resume_values.size());
				generator_resume_values_.insert(
					generator_resume_values_.end(),
					terminal_resume_values.begin(),
					terminal_resume_values.end());
			}
		}
		/*
		 * A source argument may have an exact inferred type while remaining a
		 * boxed-only value (notably a by-reference parameter).  Loading and
		 * guarding such a slot as a scalar would inspect the reference wrapper
		 * rather than a machine value that the generated body actually uses.
		 * Keep argument materialization demand-driven by the finalized TPDE
		 * operand/PHI graph.
		 */
		std::vector<int32_t> argument_value_indices(
			plan_->argument_count, -1);
		if (plan_->argument_value_indices != nullptr) {
			std::copy_n(plan_->argument_value_indices,
				plan_->argument_count, argument_value_indices.begin());
		}
		if ((plan_->value_model_flags
				& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0) {
			for (uint32_t value_index = 0;
					value_index < plan_->value_count; ++value_index) {
				const int32_t argument_index =
					plan_->values[value_index].argument_index;
				if (argument_index < 0) {
					continue;
				}
				if (static_cast<uint32_t>(argument_index)
							>= argument_value_indices.size()) {
					valid_ = false;
					continue;
				}
				if (argument_value_indices[
						static_cast<uint32_t>(argument_index)] >= 0) {
					/* Later SSA versions of an argument can retain the same
					 * materialized frame slot. The first value is the incoming
					 * argument used to decide whether its entry load is needed. */
					continue;
				}
				argument_value_indices[
					static_cast<uint32_t>(argument_index)] =
					static_cast<int32_t>(value_index);
			}
		}
		if (source_landings && plan_->source_block_ends != nullptr) {
			for (uint32_t source_block = 0;
					source_block < source_block_next.size(); ++source_block) {
				uint32_t &next_source = source_block_next[source_block];
				if (next_source == UINT32_MAX) {
					continue;
				}
				const uint32_t source_end =
					plan_->source_block_ends[source_block];
				while (next_source < source_end) {
					const uint32_t block =
						source_landing_blocks[next_source];
					if (block != UINT32_MAX) {
						emit_source_landing(block, next_source);
					}
					++next_source;
				}
			}
		}
		auto argument_machine_value_used = [&](uint32_t argument_index) {
			if (argument_index >= argument_value_indices.size()
					|| argument_value_indices[argument_index] < 0) {
				return false;
			}
			const uint32_t value_index = static_cast<uint32_t>(
				argument_value_indices[argument_index]);
			const IRValueRef argument_value{
				MIR_VALUE_BASE + value_index};
			for (const BlockItem<IRInstRef> &item : block_instructions) {
				const InstNode &node =
					nodes_[static_cast<uint32_t>(item.value)];
				for (uint32_t operand = 0;
						operand < node.operand_count; ++operand) {
					if (operands_[node.operand_offset + operand]
							== argument_value) {
						return true;
					}
				}
			}
			for (const PhiInput &input : phi_inputs_) {
				if (input.value == argument_value) {
					return true;
				}
			}
			return machine_value_has_frozen_use(value_index);
		};
		std::erase_if(argument_guards_,
			[&](const ArgumentGuard &guard) {
				return !argument_machine_value_used(
					guard.argument_index);
			});
		block_instructions.erase(
			std::remove_if(block_instructions.begin(), block_instructions.end(),
				[&](const BlockItem<IRInstRef> &item) {
					if (item.block != static_cast<uint32_t>(entry)) {
						return false;
					}
					const InstNode &node =
						nodes_[static_cast<uint32_t>(item.value)];
					if (node.mir_instruction_index != UINT32_MAX) {
						return false;
					}
					switch (node.kind) {
						case InstKind::ZvalGuardArguments:
							return argument_guards_.empty();
						case InstKind::ZvalTypeLoad:
						case InstKind::ZvalPayloadLoad:
						case InstKind::ZvalGuardType:
							return node.argument_index != UINT32_MAX
								&& !argument_machine_value_used(
									node.argument_index);
						default:
							return false;
					}
				}),
			block_instructions.end());
		/* The adaptor remains live for the complete backend compilation.  Release
		 * geometric growth slack before installing the operand spans and handing
		 * the graph to TPDE, where large source functions otherwise retain a
		 * second, mostly empty node allocation throughout code generation. */
		operands_.shrink_to_fit();
		for (InstNode &node : nodes_) {
			const auto all_operands =
				std::span<const IRValueRef>{operands_}.subspan(
					node.operand_offset, node.operand_count);
			const uint32_t semantic_operand_count =
				node.semantic_operand_count != UINT32_MAX
					? node.semantic_operand_count
					: node.materialization_operand_index == UINT32_MAX
						? node.operand_count
						: node.materialization_operand_index;
			node.operands = all_operands.first(semantic_operand_count);
			node.liveness_operands = all_operands;
		}
		flatten_block_items(tpde_block_count, block_instructions,
			instruction_slices_, instructions_);
		flatten_block_items(tpde_block_count, block_phis,
			phi_slices_, phis_);
		block_instructions.clear();
		block_instructions.shrink_to_fit();
		block_phis.clear();
		block_phis.shrink_to_fit();
		nodes_.shrink_to_fit();
		fused_instructions_.shrink_to_fit();
		instructions_.shrink_to_fit();
		phis_.shrink_to_fit();
		}

	bool valid() const { return valid_; }
	bool compact_guarded_value_operation(IRInstRef instruction) const {
		static constexpr uint32_t compact_instruction_threshold = 1u << 16;
		const InstNode &current = node(instruction);
		if (plan_->instruction_count < compact_instruction_threshold
				|| current.kind != InstKind::GuardedFast
				|| current.mir_instruction_index >= plan_->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &mir =
			plan_->instructions[current.mir_instruction_index];
		return mir.has_value_operation
			&& mir.record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER;
	}
	bool typed_component_call(IRInstRef instruction) const {
		const InstNode &current = node(instruction);
		return frozen_typed_component_call(
			current.mir_instruction_index);
	}
	uint64_t inlined_user_body_count() const {
		return static_cast<uint64_t>(std::count_if(
			nodes_.begin(), nodes_.end(),
			[](const InstNode &node) {
				return node.kind == InstKind::GuardedFast
					&& node.inlined_user_body;
			}));
	}
	const zend_tpde_plan *plan() const { return plan_; }
	const zend_tpde_plan *component_plan(uint32_t index) const {
		return index < component_plans_.size()
			? component_plans_[index] : nullptr;
	}
	bool component_compiled_variable_used(
			uint32_t component_index, uint32_t variable_index) const {
		const zend_tpde_plan *component = component_plan(component_index);
		if (component == nullptr
				|| variable_index >= component->compiled_variable_count
				|| component->compiled_variables_used == nullptr) {
			return true;
		}
		return component->compiled_variables_used[variable_index] != 0;
	}
	const void *runtime_helper(zend_native_runtime_helper_id id) const {
		const zend_native_runtime_helper *helper =
			zend_native_runtime_helper_find(plan_->runtime, id);
		return helper != nullptr ? helper->address : nullptr;
	}
	IRBlockRef block_ref(zend_mir_block_id id) const {
		int32_t index = block_index(id);
		return index < 0 ? INVALID_BLOCK_REF
			: IRBlockRef{static_cast<uint32_t>(index)};
	}
	const InstNode &node(IRInstRef inst) const {
		return nodes_[static_cast<uint32_t>(inst)];
	}
	std::span<const InlinedCheckedStep> inlined_checked_steps(
			const InstNode &instruction) const {
		if (instruction.inlined_checked_step_count == 0) {
			return {};
		}
		if (instruction.inlined_checked_step_offset
					> inlined_checked_steps_.size()
				|| instruction.inlined_checked_step_count
					> inlined_checked_steps_.size()
						- instruction.inlined_checked_step_offset) {
			return {};
		}
		return std::span<const InlinedCheckedStep>{inlined_checked_steps_}
			.subspan(instruction.inlined_checked_step_offset,
				instruction.inlined_checked_step_count);
	}
	const zend_tpde_instruction &mir_instruction(IRInstRef inst) const {
		const InstNode &instruction_node = node(inst);
		return instruction_node.synthetic
			? synthetic_instruction_
			: plan_->instructions[instruction_node.mir_instruction_index];
	}
	std::span<const ArgumentGuard> argument_guards() const {
		return argument_guards_;
	}
	std::span<const uint32_t> user_opcode_next_landings() const {
		return user_opcode_next_landings_;
	}
	std::span<const uint32_t> user_opcode_dispatch_to_sources() const {
		return user_opcode_dispatch_to_sources_;
	}
	bool user_opcode_result_reload_source(uint32_t source) const {
		return source < user_opcode_result_reload_sources_.size()
			&& user_opcode_result_reload_sources_[source] != 0;
	}
	std::span<const uint32_t> generator_resume_targets() const {
		return {plan_->generator_resume_targets,
			plan_->generator_resume_count};
	}
	std::span<const zend_mir_block_id>
	generator_resume_exception_blocks() const {
		return {plan_->generator_resume_exception_blocks,
			plan_->generator_resume_count};
	}
	std::span<const IRValueRef> generator_resume_values(IRInstRef inst) const {
		const InstNode &current = node(inst);
		return std::span<const IRValueRef>{generator_resume_values_}.subspan(
			current.generator_resume_value_offset,
			current.generator_resume_value_count);
	}
	std::span<const zend_tpde_materialization>
	materializations(IRInstRef inst) const {
		const InstNode &current = node(inst);
		if (current.materialization_count == 0
				|| current.synthetic
				|| current.mir_instruction_index >= plan_->instruction_count) {
			return {};
		}
		const zend_tpde_instruction &instruction =
			plan_->instructions[current.mir_instruction_index];
		return std::span<const zend_tpde_materialization>{
			plan_->materializations, plan_->materialization_count}.subspan(
				instruction.materialization_offset,
				instruction.materialization_count);
	}
	zend_mir_instruction_record instruction_record(IRInstRef inst) const {
		const InstNode &instruction_node = node(inst);
		if (instruction_node.synthetic) {
			return instruction_node.synthetic_record;
		}
		zend_mir_instruction_record record =
			instruction_record_at(instruction_node.mir_instruction_index);
		if (is_boxed_cond_branch(mir_instruction(inst))) {
			record.opcode =
				mir_instruction(inst).value_operation.opcode;
		}
		return record;
	}
	zend_mir_representation representation(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->representation;
		}
		if (index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count) {
			const TypedBodyAbiType abi =
				typed_body_value_abi(plan_, index - MIR_VALUE_BASE);
			if (plan_->values[index - MIR_VALUE_BASE].argument_index >= 0
					&& abi.valid) {
				return abi.representation;
			}
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
			: plan_->values[index - MIR_VALUE_BASE].representation;
	}
	zend_mir_scalar_type_mask exact_type(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->exact_type;
		}
		if (index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count) {
			const TypedBodyAbiType abi =
				typed_body_value_abi(plan_, index - MIR_VALUE_BASE);
			if (plan_->values[index - MIR_VALUE_BASE].argument_index >= 0
					&& abi.valid) {
				return abi.exact_type;
			}
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_MIR_SCALAR_TYPE_NONE
			: plan_->values[index - MIR_VALUE_BASE].exact_type;
	}
	zend_tpde_machine_value_kind machine_kind(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->machine_kind;
		}
		if (index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count) {
			const TypedBodyAbiType abi =
				typed_body_value_abi(plan_, index - MIR_VALUE_BASE);
			if (plan_->values[index - MIR_VALUE_BASE].argument_index >= 0
					&& abi.valid) {
				return abi.machine_kind;
			}
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_TPDE_MACHINE_VALUE_I64
			: plan_->values[index - MIR_VALUE_BASE].machine_kind;
	}
	zend_mir_ownership_state ownership(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->ownership;
		}
		if (index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count) {
			const zend_tpde_value &plan_value =
				plan_->values[index - MIR_VALUE_BASE];
			if (function_mode_ == FunctionMode::TypedBody
					&& plan_value.argument_index >= 0
					&& static_cast<uint32_t>(
						plan_value.argument_index)
						< plan_->argument_count
					&& plan_->argument_abi != nullptr
					&& plan_->argument_abi[
						plan_value.argument_index].valid) {
				return local_abi_ownership(
					plan_->argument_abi[
						plan_value.argument_index].transfer,
					plan_value.ownership);
			}
			return plan_value.ownership;
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_MIR_OWNERSHIP_STATE_INVALID
			: plan_->values[index - MIR_VALUE_BASE].ownership;
	}
	zend_mir_refcount_state refcount_state(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->refcount_state;
		}
		if (index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count) {
			const zend_tpde_value &plan_value =
				plan_->values[index - MIR_VALUE_BASE];
			if (function_mode_ == FunctionMode::TypedBody
					&& plan_value.argument_index >= 0
					&& static_cast<uint32_t>(
						plan_value.argument_index)
						< plan_->argument_count
					&& plan_->argument_abi != nullptr
					&& plan_->argument_abi[
						plan_value.argument_index].valid) {
				return local_abi_refcount(
					plan_->argument_abi[
						plan_value.argument_index].transfer,
					plan_value.refcount_state);
			}
			return plan_value.refcount_state;
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_MIR_REFCOUNT_STATE_INVALID
			: plan_->values[index - MIR_VALUE_BASE].refcount_state;
	}
	bool machine_value_has_result_representation(IRValueRef value) const {
		if (value == INVALID_VALUE_REF) {
			return false;
		}
		if (exact_type(value) == ZEND_MIR_SCALAR_TYPE_NULL) {
			return false;
		}
		if (zend_mir_scalar_type_is_exact(exact_type(value))) {
			uint64_t constant_bits;
			return constant(value, &constant_bits)
				|| machine_value_is_register_authoritative(value);
		}
		switch (machine_kind(value)) {
			case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
			case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
			case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
			case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
			case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
			case ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL:
				return machine_value_is_register_authoritative(value);
			default:
				return false;
		}
	}
	bool machine_value_needs_result_assignment(IRValueRef value) const {
		uint64_t constant_bits;
		return machine_value_has_result_representation(value)
			&& !constant(value, &constant_bits);
	}
	bool machine_value_has_register_definition(IRValueRef value) const {
		if (value == INVALID_VALUE_REF
				|| exact_type(value) == ZEND_MIR_SCALAR_TYPE_NULL
				|| !zend_tpde_machine_value_is_register_authoritative(
					machine_kind(value))) {
			return false;
		}
		if (derived_value(value) != nullptr) {
			return true;
		}
		const uint32_t raw = static_cast<uint32_t>(value);
		if (raw < MIR_VALUE_BASE
				|| raw - MIR_VALUE_BASE >= plan_->value_count) {
			return true;
		}
		const uint32_t index = raw - MIR_VALUE_BASE;
		const zend_tpde_value &plan_value = plan_->values[index];
		if (plan_value.constant
				|| (function_mode_ == FunctionMode::TypedBody
					&& plan_value.argument_index >= 0
					&& plan_->argument_abi != nullptr
					&& static_cast<uint32_t>(plan_value.argument_index)
						< plan_->argument_count
					&& plan_->argument_abi[
						plan_value.argument_index].valid)) {
			return true;
		}
		const int32_t definition =
			plan_->value_definition_instructions == nullptr
				? -1
				: plan_->value_definition_instructions[index];
		if (definition < 0
				|| static_cast<uint32_t>(definition)
					>= plan_->instruction_count) {
			return false;
		}
		/*
		 * A scalar PHI is defined at its block entry even when its instruction
		 * appears later in the plan's linear order. COPY nodes on an outer-loop
		 * edge may therefore reference it before construction reaches that PHI.
		 * The frozen scalar-candidate closure guarantees that every incoming edge
		 * has a compatible machine definition before a ZVAL PHI is promoted.
		 */
		const zend_mir_instruction_record definition_record =
			instruction_record_at(static_cast<uint32_t>(definition));
		if (definition_record.opcode == ZEND_MIR_OPCODE_PHI
				&& plan_value.register_authoritative
				&& plan_value.representation
					!= ZEND_MIR_REPRESENTATION_ZVAL
				&& zend_mir_scalar_type_is_exact(plan_value.exact_type)
				&& plan_value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL) {
			return true;
		}
		if (static_cast<uint32_t>(definition)
					< active_instruction_results().size()
				&& active_instruction_results()[
					static_cast<uint32_t>(definition)]
					!= INVALID_VALUE_REF) {
			return true;
		}
		return false;
	}
	bool machine_value_is_register_authoritative(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (derived_value(value) != nullptr) {
			return true;
		}
		if (index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count) {
			return true;
		}
		return plan_->values[index - MIR_VALUE_BASE]
			.register_authoritative;
	}
	bool machine_value_has_stable_exact_type(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (derived_value(value) != nullptr
				|| index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count) {
			return true;
		}
		const zend_tpde_value &plan_value =
			plan_->values[index - MIR_VALUE_BASE];
		return plan_value.constant
			|| !plan_value.canonical_alias_observable;
	}
	bool plan_value_is_register_authoritative(
			zend_mir_value_id value_id) const {
		if (!zend_mir_id_is_valid(value_id)) {
			return false;
		}
		const IRValueRef value = value_ref(value_id);
		return value != INVALID_VALUE_REF
			&& machine_value_is_register_authoritative(value);
	}
	zend_mir_storage_id canonical_storage(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->storage_id;
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_MIR_ID_INVALID
			: plan_->values[index - MIR_VALUE_BASE].canonical_storage_id;
	}
	uint32_t frame_slot_reference_count() const {
		return static_cast<uint32_t>(machine_reference_values_.size());
	}
	IRValueRef frame_slot_reference(uint32_t index) const {
		return index < machine_reference_values_.size()
			? machine_reference_values_[index]
			: INVALID_VALUE_REF;
	}
	bool machine_reference(
			IRValueRef value,
			const zend_tpde_machine_reference **reference) const {
		const DerivedValue *derived = derived_value(value);
		if (derived == nullptr
				|| derived->machine_reference_index
					>= plan_->machine_reference_count) {
			return false;
		}
		if (reference != nullptr) {
			*reference = &plan_->machine_references[
				derived->machine_reference_index];
		}
		return true;
	}
	bool operation_machine_reference(
			uint32_t instruction_index,
			const zend_tpde_machine_reference **reference) const {
		if (instruction_index >= plan_->instruction_count) {
			return false;
		}
		const uint32_t reference_index =
			plan_->instructions[
				instruction_index].operation_reference_index;
		if (reference_index >= plan_->machine_reference_count) {
			return false;
		}
		if (reference != nullptr) {
			*reference = &plan_->machine_references[reference_index];
		}
		return true;
	}
	bool frame_slot_reference(
			IRValueRef value, zend_mir_storage_id *storage_id) const {
		const zend_tpde_machine_reference *reference = nullptr;
		if (!machine_reference(value, &reference)
				|| reference->kind
					!= ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT) {
			return false;
		}
		if (storage_id != nullptr) {
			*storage_id = reference->stable_storage_or_layout_id;
		}
		return true;
	}
	bool constant(IRValueRef value, uint64_t *bits) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			if (!derived->constant) {
				return false;
			}
			*bits = derived->constant_bits;
			return true;
		}
		if (index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count) {
			return false;
		}
		/*
		 * The frozen local ABI overrides the initial source-SSA fact for
		 * invocation-local arguments.  In particular, Zend may retain a
		 * null/undef fact for the pre-RECV CV identity even though that
		 * identity is loaded as a string, array, object, or boxed argument.
		 * Only the effective machine type may therefore collapse a value to
		 * the payload-free null constant.
		 */
		if (exact_type(value) == ZEND_MIR_SCALAR_TYPE_NULL) {
			*bits = 0;
			return true;
		}
		if (!plan_->values[index - MIR_VALUE_BASE].constant) {
			return false;
		}
		*bits = plan_->values[index - MIR_VALUE_BASE].constant_bits;
		return true;
	}
	bool known_string_literal(
			IRValueRef value, uint64_t *length, bool *truthy) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			if (!derived->known_string_literal) {
				return false;
			}
			if (length != nullptr) {
				*length = derived->known_string_length;
			}
			if (truthy != nullptr) {
				*truthy = derived->known_string_length != 0
					&& (derived->known_string_length != 1
						|| derived->known_string_first_byte != '0');
			}
			return true;
		}
		if (index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count) {
			return false;
		}
		index -= MIR_VALUE_BASE;
		for (uint32_t depth = 0; depth < plan_->value_count; ++depth) {
			const zend_tpde_value &candidate = plan_->values[index];
			if (candidate.known_string_literal) {
				if (length != nullptr) {
					*length = candidate.known_string_length;
				}
				if (truthy != nullptr) {
					*truthy = candidate.known_string_length != 0
						&& (candidate.known_string_length != 1
							|| candidate.known_string_first_byte != '0');
				}
				return true;
			}
			if (candidate.register_alias_value_index < 0
					|| static_cast<uint32_t>(
						candidate.register_alias_value_index)
						>= plan_->value_count
					|| static_cast<uint32_t>(
						candidate.register_alias_value_index) == index) {
				break;
			}
			index = static_cast<uint32_t>(
				candidate.register_alias_value_index);
		}
		return false;
	}
	bool typed_body() const {
		return function_mode_ == FunctionMode::TypedBody;
	}

	uint32_t func_count() const { return 1; }
	const auto &funcs() const { return functions_; }
	const auto &funcs_to_compile() const { return functions_; }
	std::string_view func_link_name(IRFuncRef) const { return "zend_native_entry"; }
	bool func_extern(IRFuncRef) const { return false; }
	bool func_only_local(IRFuncRef) const { return false; }
	bool func_has_weak_linkage(IRFuncRef) const { return false; }
	bool cur_func_may_emit_calls() const {
		return function_mode_ == FunctionMode::TypedBody
			? plan_->typed_body_may_emit_calls
			: plan_->zend_entry_may_emit_calls;
	}
	bool cur_needs_unwind_info() const {
		return function_mode_ == FunctionMode::TypedBody
			? plan_->typed_body_needs_unwind
			: plan_->zend_entry_needs_unwind;
	}
	bool cur_is_vararg() const { return false; }
	uint32_t cur_highest_val_idx() const {
		return MIR_VALUE_BASE + plan_->value_count
			+ static_cast<uint32_t>(derived_values_.size()) - 1;
	}
	const auto &cur_args() const { return arguments_; }
	static bool cur_arg_is_byval(uint32_t) { return false; }
	static uint32_t cur_arg_byval_align(uint32_t) { return 0; }
	static uint32_t cur_arg_byval_size(uint32_t) { return 0; }
	static bool cur_arg_is_sret(uint32_t) { return false; }
	const auto &cur_static_allocas() const { return no_values_; }
	static bool cur_has_dynamic_alloca() { return false; }
	IRBlockRef cur_entry_block() const {
		return IRBlockRef{static_cast<uint32_t>(block_index(
			plan_->function.entry_block_id))};
	}
	const auto &cur_blocks() const { return blocks_; }
	std::span<const IRBlockRef> block_succs(IRBlockRef block) const {
		const Slice &slice = successor_slices_[static_cast<uint32_t>(block)];
		return std::span<const IRBlockRef>{successors_}.subspan(
			slice.offset, slice.count);
	}
	std::span<const IRInstRef> block_insts(IRBlockRef block) const {
		const Slice &slice = instruction_slices_[static_cast<uint32_t>(block)];
		return std::span<const IRInstRef>{instructions_}.subspan(
			slice.offset, slice.count);
	}
	std::span<const IRValueRef> block_phis(IRBlockRef block) const {
		const Slice &slice = phi_slices_[static_cast<uint32_t>(block)];
		return std::span<const IRValueRef>{phis_}.subspan(
			slice.offset, slice.count);
	}
	uint32_t block_info(IRBlockRef block) const {
		return block_info_[static_cast<uint32_t>(block)];
	}
	void block_set_info(IRBlockRef block, uint32_t value) {
		block_info_[static_cast<uint32_t>(block)] = value;
	}
	uint32_t block_info2(IRBlockRef block) const {
		return block_info2_[static_cast<uint32_t>(block)];
	}
	void block_set_info2(IRBlockRef block, uint32_t value) {
		block_info2_[static_cast<uint32_t>(block)] = value;
	}
	std::string_view block_fmt_ref(IRBlockRef) const { return "znmir-block"; }
	::tpde::ValLocalIdx val_local_idx(IRValueRef value) const {
		return ::tpde::ValLocalIdx{static_cast<uint32_t>(value)};
	}
	bool val_ignore_in_liveness_analysis(IRValueRef value) const {
		uint64_t bits;
		return constant(value, &bits)
			|| machine_reference(value, nullptr);
	}
	bool val_is_phi(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		return index < phi_values_.size() && phi_values_[index] != 0;
	}
	PhiRef val_as_phi(IRValueRef value) const { return PhiRef{this, value}; }
	static uint32_t val_alloca_size(IRValueRef) { return 0; }
	static uint32_t val_alloca_align(IRValueRef) { return 1; }
	std::string_view value_fmt_ref(IRValueRef) const { return "znmir-value"; }
	std::span<const IRValueRef> inst_operands(IRInstRef inst) const {
		return node(inst).liveness_operands;
	}
	std::span<const IRValueRef> inst_results(IRInstRef inst) const {
		const InstNode &current = node(inst);
		if (current.kind == InstKind::GeneratorResume) {
			return std::span<const IRValueRef>{generator_resume_values_}.subspan(
				current.generator_resume_value_offset,
				current.generator_resume_value_count);
		}
		return std::span<const IRValueRef>{&current.result,
			current.has_result ? size_t{1} : size_t{0}};
	}
	bool inst_fused(IRInstRef inst) const {
		const uint32_t index = static_cast<uint32_t>(inst);
		return index < fused_instructions_.size()
			&& fused_instructions_[index] != 0;
	}
	void mark_fused(IRInstRef inst) {
		const uint32_t index = static_cast<uint32_t>(inst);
		if (index < fused_instructions_.size()) {
			fused_instructions_[index] = 1;
		} else {
			valid_ = false;
		}
	}
	std::string_view inst_fmt_ref(IRInstRef) const { return "znmir-inst"; }
	void start_compile() const {}
	void end_compile() const {}
	bool switch_func(IRFuncRef function) {
		return function == IRFuncRef{0};
	}
	void reset() {
		std::fill(block_info_.begin(), block_info_.end(), 0);
		std::fill(block_info2_.begin(), block_info2_.end(), 0);
		std::fill(fused_instructions_.begin(), fused_instructions_.end(), 0);
	}
};

static_assert(::tpde::IRAdaptor<ZendIRAdaptor>);

/*
 * TPDE's function switch is the natural component boundary: value, block and
 * instruction references are local to the selected function, while the
 * compiler owns one symbol table, one allocator run and one object image for
 * the complete component.  Keep the mature single-function view as the
 * per-function implementation and expose it through this zero-copy
 * multi-function adaptor instead of flattening all ZNMIR into a second IR.
 */
class ZendComponentIRAdaptor {
public:
	using IRValueRef = zend::native::tpde::IRValueRef;
	using IRInstRef = zend::native::tpde::IRInstRef;
	using IRBlockRef = zend::native::tpde::IRBlockRef;
	using IRFuncRef = zend::native::tpde::IRFuncRef;
	using InstKind = ZendIRAdaptor::InstKind;
	using InstNode = ZendIRAdaptor::InstNode;
	using ArgumentGuard = ZendIRAdaptor::ArgumentGuard;
	using PhiRef = ZendIRAdaptor::PhiRef;

	static constexpr IRValueRef INVALID_VALUE_REF =
		ZendIRAdaptor::INVALID_VALUE_REF;
	static constexpr IRBlockRef INVALID_BLOCK_REF =
		ZendIRAdaptor::INVALID_BLOCK_REF;
	static constexpr IRFuncRef INVALID_FUNC_REF =
		ZendIRAdaptor::INVALID_FUNC_REF;
	static constexpr bool TPDE_PROVIDES_HIGHEST_VAL_IDX =
		ZendIRAdaptor::TPDE_PROVIDES_HIGHEST_VAL_IDX;
	static constexpr bool TPDE_LIVENESS_VISIT_ARGS =
		ZendIRAdaptor::TPDE_LIVENESS_VISIT_ARGS;

	static constexpr uint32_t EXECUTE_DATA_VALUE =
		ZendIRAdaptor::EXECUTE_DATA_VALUE;
	static constexpr uint32_t EXECUTION_CONTEXT_ARGUMENT =
		ZendIRAdaptor::EXECUTION_CONTEXT_ARGUMENT;
	static constexpr uint32_t FRAME_VALUE = ZendIRAdaptor::FRAME_VALUE;
	static constexpr uint32_t MIR_VALUE_BASE = ZendIRAdaptor::MIR_VALUE_BASE;

private:
	std::vector<std::unique_ptr<ZendIRAdaptor>> members_;
	std::vector<ZendIRAdaptor *> function_views_;
	std::vector<IRFuncRef> functions_;
	std::vector<std::string> link_names_;
	ZendIRAdaptor *active_ = nullptr;
	uint64_t typed_body_call_site_count_ = 0;
	uint64_t typed_body_frame_bytes_elided_ = 0;

public:
	explicit ZendComponentIRAdaptor(const zend_tpde_plan *plan)
		: ZendComponentIRAdaptor(
			std::span<const zend_tpde_plan *const>{&plan, 1}) {}

	explicit ZendComponentIRAdaptor(
			std::span<const zend_tpde_plan *const> plans) {
		members_.reserve(plans.size());
		function_views_.reserve(plans.size() * 2);
		functions_.reserve(plans.size() * 2);
		link_names_.reserve(plans.size() * 2);
		for (uint32_t index = 0; index < plans.size(); ++index) {
			members_.push_back(
				std::make_unique<ZendIRAdaptor>(plans[index], plans,
					ZendIRAdaptor::FunctionMode::ZendEntry));
			functions_.push_back(
				IRFuncRef{plans[index]->wrapper_function_index});
			function_views_.push_back(members_.back().get());
			link_names_.push_back(index == 0
				? "zend_native_entry"
				: "zend_native_component_" + std::to_string(index));
		}
		for (uint32_t index = 0; index < plans.size(); ++index) {
			if (!plans[index]->typed_body_eligible) {
				continue;
			}
			const uint32_t function_index =
				plans[index]->typed_body_function_index;
			members_.push_back(
				std::make_unique<ZendIRAdaptor>(plans[index], plans,
					ZendIRAdaptor::FunctionMode::TypedBody));
			functions_.push_back(IRFuncRef{function_index});
			function_views_.push_back(members_.back().get());
			link_names_.push_back(
				"zend_native_typed_body_" + std::to_string(index));
		}
		if (!members_.empty()) {
			active_ = function_views_[0];
		}
	}

	bool valid() const {
		return active_ != nullptr
			&& std::all_of(members_.begin(), members_.end(),
				[](const auto &member) { return member->valid(); });
	}
	uint64_t inlined_user_body_count() const {
		uint64_t count = 0;
		for (const auto &member : members_) {
			count += member->inlined_user_body_count();
		}
		return count;
	}
	void mark_typed_body_call(uint64_t frame_bytes) {
		++typed_body_call_site_count_;
		typed_body_frame_bytes_elided_ += frame_bytes;
	}
	uint64_t typed_body_call_site_count() const {
		return typed_body_call_site_count_;
	}
	uint64_t typed_body_frame_bytes_elided() const {
		return typed_body_frame_bytes_elided_;
	}
	uint32_t current_function_index() const {
		return active_->plan()->symbol_namespace;
	}
	bool typed_component_call(IRInstRef instruction) const {
		return active_->typed_component_call(instruction);
	}
	bool compact_guarded_value_operation(IRInstRef instruction) const {
		return active_->compact_guarded_value_operation(instruction);
	}
	ZendIRAdaptor::TypedBodyAbiType typed_body_return_type(
			uint32_t component_index) const {
		const zend_tpde_plan *body = component_plan(component_index);
		return body != nullptr && body->typed_body_eligible
			? ZendIRAdaptor::typed_body_plan_abi(
				body->typed_body_return_abi)
			: ZendIRAdaptor::TypedBodyAbiType{};
	}
	bool typed_body_arguments_match(
			uint32_t component_index,
			std::span<const IRValueRef> operands) const {
		const zend_tpde_plan *body = component_plan(component_index);
		if (body == nullptr
				|| body->argument_abi == nullptr
				|| body->argument_value_indices == nullptr
				|| operands.size() < body->argument_count) {
			return false;
		}
		for (uint32_t argument = 0;
				argument < body->argument_count; ++argument) {
			const int32_t body_value =
				body->argument_value_indices[argument];
			const auto &abi = body->argument_abi[argument];
			const IRValueRef operand = operands[argument];
			if (body_value < 0
					|| static_cast<uint32_t>(body_value)
						>= body->value_count
					|| !abi.valid
					|| operand == INVALID_VALUE_REF
					|| !active_->machine_value_has_result_representation(
						operand)
					|| active_->exact_type(operand) != abi.exact_type
					|| active_->machine_kind(operand)
						!= abi.machine_kind) {
				return false;
			}
		}
		return true;
	}
	bool typed_body() const { return active_->typed_body(); }
	const zend_tpde_plan *plan() const { return active_->plan(); }
	const zend_tpde_plan *component_plan(uint32_t index) const {
		return active_->component_plan(index);
	}
	bool component_compiled_variable_used(
			uint32_t component_index, uint32_t variable_index) const {
		return active_->component_compiled_variable_used(
			component_index, variable_index);
	}
	const void *runtime_helper(zend_native_runtime_helper_id id) const {
		return active_->runtime_helper(id);
	}
	IRBlockRef block_ref(zend_mir_block_id id) const {
		return active_->block_ref(id);
	}
	const InstNode &node(IRInstRef inst) const { return active_->node(inst); }
	std::span<const ZendIRAdaptor::InlinedCheckedStep>
	inlined_checked_steps(const InstNode &instruction) const {
		return active_->inlined_checked_steps(instruction);
	}
	const zend_tpde_instruction &mir_instruction(IRInstRef inst) const {
		return active_->mir_instruction(inst);
	}
	std::span<const ArgumentGuard> argument_guards() const {
		return active_->argument_guards();
	}
	std::span<const uint32_t> user_opcode_next_landings() const {
		return active_->user_opcode_next_landings();
	}
	std::span<const uint32_t> user_opcode_dispatch_to_sources() const {
		return active_->user_opcode_dispatch_to_sources();
	}
	bool user_opcode_result_reload_source(uint32_t source) const {
		return active_->user_opcode_result_reload_source(source);
	}
	std::span<const uint32_t> generator_resume_targets() const {
		return active_->generator_resume_targets();
	}
	std::span<const zend_mir_block_id>
	generator_resume_exception_blocks() const {
		return active_->generator_resume_exception_blocks();
	}
	std::span<const IRValueRef> generator_resume_values(IRInstRef inst) const {
		return active_->generator_resume_values(inst);
	}
	std::span<const zend_tpde_materialization>
	materializations(IRInstRef inst) const {
		return active_->materializations(inst);
	}
	zend_mir_instruction_record instruction_record(IRInstRef inst) const {
		return active_->instruction_record(inst);
	}
	zend_mir_representation representation(IRValueRef value) const {
		return active_->representation(value);
	}
	zend_mir_scalar_type_mask exact_type(IRValueRef value) const {
		return active_->exact_type(value);
	}
	zend_tpde_machine_value_kind machine_kind(IRValueRef value) const {
		return active_->machine_kind(value);
	}
	zend_mir_ownership_state ownership(IRValueRef value) const {
		return active_->ownership(value);
	}
	zend_mir_refcount_state refcount_state(IRValueRef value) const {
		return active_->refcount_state(value);
	}
	bool machine_value_is_register_authoritative(IRValueRef value) const {
		return active_->machine_value_is_register_authoritative(value);
	}
	bool plan_value_is_register_authoritative(
			zend_mir_value_id value_id) const {
		return active_->plan_value_is_register_authoritative(value_id);
	}
	zend_mir_storage_id canonical_storage(IRValueRef value) const {
		return active_->canonical_storage(value);
	}
	uint32_t frame_slot_reference_count() const {
		return active_->frame_slot_reference_count();
	}
	IRValueRef frame_slot_reference(uint32_t index) const {
		return active_->frame_slot_reference(index);
	}
	bool frame_slot_reference(
			IRValueRef value, zend_mir_storage_id *storage_id) const {
		return active_->frame_slot_reference(value, storage_id);
	}
	bool machine_reference(
			IRValueRef value,
			const zend_tpde_machine_reference **reference) const {
		return active_->machine_reference(value, reference);
	}
	bool operation_machine_reference(
			uint32_t instruction_index,
			const zend_tpde_machine_reference **reference) const {
		return active_->operation_machine_reference(
			instruction_index, reference);
	}
	bool constant(IRValueRef value, uint64_t *bits) const {
		return active_->constant(value, bits);
	}
	bool known_string_literal(
			IRValueRef value, uint64_t *length, bool *truthy) const {
		return active_->known_string_literal(value, length, truthy);
	}

	uint32_t func_count() const {
		return static_cast<uint32_t>(functions_.size());
	}
	const auto &funcs() const { return functions_; }
	const auto &funcs_to_compile() const { return functions_; }
	std::string_view func_link_name(IRFuncRef function) const {
		const uint32_t index = static_cast<uint32_t>(function);
		return index < link_names_.size()
			? std::string_view{link_names_[index]} : std::string_view{};
	}
	bool func_extern(IRFuncRef) const { return false; }
	bool func_only_local(IRFuncRef function) const {
		const uint32_t index = static_cast<uint32_t>(function);
		return index < function_views_.size()
			&& function_views_[index]->typed_body();
	}
	bool func_has_weak_linkage(IRFuncRef) const { return false; }
	bool cur_needs_unwind_info() const {
		return active_->cur_needs_unwind_info();
	}
	bool cur_func_may_emit_calls() const {
		return active_->cur_func_may_emit_calls();
	}
	bool cur_is_vararg() const { return false; }
	uint32_t cur_highest_val_idx() const {
		return active_->cur_highest_val_idx();
	}
	const auto &cur_args() const { return active_->cur_args(); }
	static bool cur_arg_is_byval(uint32_t index) {
		return ZendIRAdaptor::cur_arg_is_byval(index);
	}
	static uint32_t cur_arg_byval_align(uint32_t index) {
		return ZendIRAdaptor::cur_arg_byval_align(index);
	}
	static uint32_t cur_arg_byval_size(uint32_t index) {
		return ZendIRAdaptor::cur_arg_byval_size(index);
	}
	static bool cur_arg_is_sret(uint32_t index) {
		return ZendIRAdaptor::cur_arg_is_sret(index);
	}
	const auto &cur_static_allocas() const {
		return active_->cur_static_allocas();
	}
	static bool cur_has_dynamic_alloca() { return false; }
	IRBlockRef cur_entry_block() const {
		return active_->cur_entry_block();
	}
	const auto &cur_blocks() const { return active_->cur_blocks(); }
	std::span<const IRBlockRef> block_succs(IRBlockRef block) const {
		return active_->block_succs(block);
	}
	std::span<const IRInstRef> block_insts(IRBlockRef block) const {
		return active_->block_insts(block);
	}
	std::span<const IRValueRef> block_phis(IRBlockRef block) const {
		return active_->block_phis(block);
	}
	uint32_t block_info(IRBlockRef block) const {
		return active_->block_info(block);
	}
	void block_set_info(IRBlockRef block, uint32_t value) {
		active_->block_set_info(block, value);
	}
	uint32_t block_info2(IRBlockRef block) const {
		return active_->block_info2(block);
	}
	void block_set_info2(IRBlockRef block, uint32_t value) {
		active_->block_set_info2(block, value);
	}
	std::string_view block_fmt_ref(IRBlockRef block) const {
		return active_->block_fmt_ref(block);
	}
	::tpde::ValLocalIdx val_local_idx(IRValueRef value) const {
		return active_->val_local_idx(value);
	}
	bool val_ignore_in_liveness_analysis(IRValueRef value) const {
		return active_->val_ignore_in_liveness_analysis(value);
	}
	bool val_is_phi(IRValueRef value) const {
		return active_->val_is_phi(value);
	}
	PhiRef val_as_phi(IRValueRef value) const {
		return active_->val_as_phi(value);
	}
	static uint32_t val_alloca_size(IRValueRef value) {
		return ZendIRAdaptor::val_alloca_size(value);
	}
	static uint32_t val_alloca_align(IRValueRef value) {
		return ZendIRAdaptor::val_alloca_align(value);
	}
	std::string_view value_fmt_ref(IRValueRef value) const {
		return active_->value_fmt_ref(value);
	}
	std::span<const IRValueRef> inst_operands(IRInstRef inst) const {
		return active_->inst_operands(inst);
	}
	auto inst_results(IRInstRef inst) const {
		return active_->inst_results(inst);
	}
	bool inst_fused(IRInstRef inst) const {
		return active_->inst_fused(inst);
	}
	void mark_fused(IRInstRef inst) { active_->mark_fused(inst); }
	std::string_view inst_fmt_ref(IRInstRef inst) const {
		return active_->inst_fmt_ref(inst);
	}
	void start_compile() const {}
	void end_compile() const {}
	bool switch_func(IRFuncRef function) {
		const uint32_t index = static_cast<uint32_t>(function);
		if (index >= function_views_.size()) {
			return false;
		}
		active_ = function_views_[index];
		return true;
	}
	void reset() {
		for (auto &member : members_) {
			member->reset();
		}
		active_ = function_views_.empty() ? nullptr : function_views_[0];
	}
};

static_assert(::tpde::IRAdaptor<ZendComponentIRAdaptor>);

} // namespace zend::native::tpde
