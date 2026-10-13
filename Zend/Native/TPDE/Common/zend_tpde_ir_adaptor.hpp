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
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
		ZvalPayloadLoad,
		ZvalBoxedStore,
		ZvalCopy,
		ZvalMove,
		ZvalStore,
		ZvalReleaseFast,
		ZvalGuardArguments,
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
		/* A BoxedCondGuard whose cold block carries the PHI inputs of the
		 * branch: only the cold edges define them. */
		bool cold_phi_inputs = false;
		/* A GeneratorGateway of deoptimization resume IDs instead of
		 * generator continuations. */
		bool deopt_resume = false;
		/* The first node of a deoptimization landing's block: the resume
		 * index whose landing stub precedes it. */
		uint32_t deopt_landing = UINT32_MAX;
		/* A GuardedCold node that deoptimizes into the generic copy
		 * (zend_tpde_instruction::deopt_exit_resume_plus_one). */
		bool deopt_exit = false;
		/* The operands a deoptimizing slow node stores to their slots
		 * before anything else (add_deopt_stores()). */
		uint32_t deopt_store_operand_index = UINT32_MAX;
		uint32_t deopt_store_count = 0;
	};

	/*
	 * The slots of a result vector each value was written to, newest first,
	 * so a query for a value's definitions need not scan the whole vector.
	 * A slot overwritten since keeps its stale link; visit() skips it.
	 */
	struct ResultDefinitions {
		std::vector<uint32_t> heads;
		std::vector<std::pair<uint32_t, uint32_t>> links;

		void add(uint32_t slot, IRValueRef value) {
			const uint32_t raw = static_cast<uint32_t>(value);
			if (value == INVALID_VALUE_REF) {
				return;
			}
			if (raw >= heads.size()) {
				heads.resize(static_cast<size_t>(raw) + 1, UINT32_MAX);
			}
			links.emplace_back(slot, heads[raw]);
			heads[raw] = static_cast<uint32_t>(links.size() - 1);
		}
		bool contains(const std::vector<IRValueRef> &results,
				IRValueRef value) const {
			bool found = false;
			visit(results, value, [&](uint32_t) { found = true; });
			return found;
		}
		/* Calls f(slot) for every slot of results that holds value. */
		template <typename F>
		void visit(const std::vector<IRValueRef> &results, IRValueRef value,
				F &&f) const {
			const uint32_t raw = static_cast<uint32_t>(value);
			if (value == INVALID_VALUE_REF || raw >= heads.size()) {
				return;
			}
			for (uint32_t link = heads[raw]; link != UINT32_MAX;
					link = links[link].second) {
				const uint32_t slot = links[link].first;
				if (slot < results.size() && results[slot] == value) {
					f(slot);
				}
			}
		}
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
		std::vector<IRValueRef> checked_operands{};
		std::vector<InlinedCheckedStep> checked_steps{};

		bool checked() const {
			return !checked_steps.empty();
		}

		uint32_t checked_source_opcode() const {
			return checked() ? checked_steps[0].source_opcode : UINT32_MAX;
		}

		uint32_t operand_count() const {
			return checked()
				? static_cast<uint32_t>(checked_operands.size())
				: 1;
		}
	};

	struct ArgumentGuard {
		uint32_t argument_index;
		zend_mir_storage_id storage_id;
		zend_mir_scalar_type_mask exact_type;
		zend_tpde_machine_value_kind machine_kind;
	};

	/* A typed body's value ABI is the plan's. */
	using TypedBodyAbiType = zend_tpde_local_abi_type;

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

	static bool machine_pointer_kind(zend_tpde_machine_value_kind kind);

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
	/* Per value, the frame slot a ZvalPayloadLoad read it from. */
	std::vector<zend_mir_storage_id> slot_load_storage_;
	std::vector<InlinedCheckedStep> inlined_checked_steps_;
	std::vector<uint8_t> fused_instructions_;
	/* inline_component_scalar_body's per-callee-value scratch. */
	std::vector<IRValueRef> inline_body_values_;
	std::vector<int32_t> inline_body_checked_value_steps_;
	/* Per MIR instruction, whether node construction shaped it as a boxed
	 * conditional branch (is_boxed_cond_branch): 0 or 1, or
	 * BOXED_COND_BRANCH_UNKNOWN when construction did not reach it. */
	static constexpr uint8_t BOXED_COND_BRANCH_UNKNOWN = 2;
	std::vector<uint8_t> boxed_cond_branches_;
	std::vector<IRValueRef> operands_;
	std::vector<IRValueRef> generator_resume_values_;
	std::vector<uint8_t> phi_values_;
	/* The function's value, source SSA and instruction result
	 * replacements (a typed body's register forms, for example). */
	std::vector<IRValueRef> value_overrides_;
	std::vector<IRValueRef> source_ssa_overrides_;
	std::vector<IRValueRef> instruction_results_;
	ResultDefinitions instruction_result_definitions_;
	/* Per storage below the largest frame-slot storage: the first frame-slot
	 * machine reference, UINT32_MAX if none. */
	std::vector<uint32_t> frame_slot_references_;
	/* Sorted storages an ASSIGN_REF names as either operand. */
	std::vector<zend_mir_storage_id> reference_assigned_storages_;
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	std::vector<DerivedValue> derived_values_;
	std::vector<IRValueRef> machine_reference_values_;
	std::vector<ArgumentGuard> argument_guards_;
	std::vector<uint32_t> user_opcode_next_landings_;
	std::vector<uint32_t> user_opcode_dispatch_to_sources_;
	std::vector<uint8_t> user_opcode_result_reload_sources_;
	/* Per deoptimization resume ID: its landing exists in this function,
	 * and the frame-state map of that landing. */
	std::vector<uint8_t> deopt_resume_landed_;
	/* Per MIR instruction: the first resume ID with a landing whose
	 * operation starts there, UINT32_MAX if none. */
	std::vector<uint32_t> deopt_resume_by_instruction_;
	/* Per TPDE block: it holds a guard's or boxed branch's cold node. */
	std::vector<uint8_t> block_cold_;
	std::vector<Slice> deopt_landing_slices_;
	std::vector<IRValueRef> deopt_landing_values_;
	zend_tpde_instruction synthetic_instruction_{};
	bool valid_ = true;

	static IRValueRef plan_source_operand_value_ref(
			const zend_tpde_plan *plan,
			const zend_mir_source_operand_ref &operand);

	const std::vector<IRValueRef> &active_value_overrides() const {
		return value_overrides_;
	}
	std::vector<IRValueRef> &active_value_overrides() {
		return value_overrides_;
	}
	const std::vector<IRValueRef> &active_source_ssa_overrides() const {
		return source_ssa_overrides_;
	}
	std::vector<IRValueRef> &active_source_ssa_overrides() {
		return source_ssa_overrides_;
	}
	const std::vector<IRValueRef> &active_instruction_results() const {
		return instruction_results_;
	}
	std::vector<IRValueRef> &active_instruction_results() {
		return instruction_results_;
	}
	void set_instruction_result(uint32_t index, IRValueRef value);

	bool frozen_typed_component_call(uint32_t instruction_index) const {
		return plan_ != nullptr
			&& plan_->typed_component_call_eligible != nullptr
			&& instruction_index < plan_->instruction_count
			&& plan_->typed_component_call_eligible[instruction_index] != 0;
	}

	bool frozen_effect_closed_inline(uint32_t instruction_index) const;

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
			/*
			 * The bound producer stayed canonical and has no TPDE result.
			 * Bypass the global override chain here: a reused SSA identity may
			 * already point at a derived value whose defining node is later in
			 * this function.
			 */
			if (valid_value_index(binding.value_index)) {
				return IRValueRef{MIR_VALUE_BASE
					+ static_cast<uint32_t>(binding.value_index)};
			}
			return INVALID_VALUE_REF;
		}
		if (valid_value_index(binding.value_index)) {
			const zend_tpde_value &value =
				plan_->values[
					static_cast<uint32_t>(binding.value_index)];
			if (zend_mir_value_is_original_ssa(value.id)
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

	/* Whether a plan value index names one of the plan's values. */
	bool valid_value_index(int32_t index) const {
		return index >= 0
			&& static_cast<uint32_t>(index) < plan_->value_count;
	}

	/* Whether an IR value number is one of the plan's MIR values, rather
	 * than a fixed argument or a derived value. */
	bool is_mir_value(uint32_t index) const {
		return index >= MIR_VALUE_BASE
			&& index - MIR_VALUE_BASE < plan_->value_count;
	}

	void override_ssa_value(uint32_t ssa_variable_id, IRValueRef value);

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
		/* value_ref() already follows the value overrides to their end. */
		return value_ref(value_id);
	}

	zend_mir_storage_id source_operand_storage_id(
			const zend_mir_source_operand_ref &operand) const;

	bool direct_internal_source_argument_stable(
			const zend_mir_call_argument_ref &argument,
			uint32_t call_source_position) const;

	IRValueRef mutation_value_ref(
			const zend_tpde_instruction &instruction) const;

	bool storage_assigned_by_reference(
			zend_mir_storage_id storage_id) const;

	/* The comparisons a long binary operation produces a boolean for. */
	static constexpr bool long_comparison_source_opcode(uint32_t opcode);

	/* The source opcodes of a binary operation on two longs. */
	static constexpr bool long_binary_source_opcode(uint32_t opcode);

	bool long_binary_machine_operands(
			const zend_tpde_instruction &instruction,
			IRValueRef &left, IRValueRef &right) const;

	/* The address of a canonical frame slot, as a derived value, or
	 * INVALID_VALUE_REF. */
	IRValueRef frame_slot_address(zend_mir_storage_id storage);
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
			uint64_t known_string_length = 0);

	uint32_t machine_reference_index(
			zend_tpde_machine_reference_kind kind,
			uint32_t stable_storage_or_layout_id) const {
		if (kind == ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT
				&& stable_storage_or_layout_id
					< frame_slot_references_.size()) {
			return frame_slot_references_[stable_storage_or_layout_id];
		}
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
			const zend_mir_instruction_record &record);

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
			IRValueRef *result_out = nullptr);

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

	bool machine_value_has_frozen_use(uint32_t value_index) const;

	const zend_mir_instruction_record &instruction_record_at(
			uint32_t index) const {
		static const zend_mir_instruction_record invalid = [] {
			zend_mir_instruction_record record{};
			record.id = ZEND_MIR_ID_INVALID;
			return record;
		}();
		return index < plan_->instruction_count
			? plan_->instructions[index].record : invalid;
	}

	/*
	 * The frame-state map of each deoptimization landing (ADR 0025 section
	 * 1): every machine value live into the landing, which its stub loads
	 * from the value's canonical frame slot into the place TPDE keeps it at
	 * the block start. Machine liveness, not MIR liveness: values the MIR
	 * does not name (unboxed arguments, loaded payloads) belong to the map
	 * too. A landing with a live value that its slot cannot reproduce is
	 * dropped: no transfer resumes there.
	 */
	void freeze_deopt_landing_values(
			const std::vector<BlockItem<IRInstRef>> &block_instructions,
			const std::vector<BlockItem<IRValueRef>> &block_phis,
			uint32_t block_count);

	/*
	 * A register scalar's slot may be stale where an operation deoptimizes
	 * (a lazily written CV, a typed-tier value). The operation's slow node,
	 * which runs the transfer, first stores every scalar of the landing's
	 * map: a slot whose value is a scalar holds no counted value, so the
	 * store is exact. Pointer values stay with their slots, which a
	 * separating helper may have replaced. cold_nodes are the operation's
	 * cold nodes in node order.
	 */
	void add_deopt_stores(const std::vector<uint32_t> &cold_nodes,
			std::span<const IRValueRef> landing_values);

	/* Appends stores as liveness operands a node stores first
	 * (deopt_store_operand_index). */
	void attach_deopt_stores(InstNode &current,
			const std::vector<IRValueRef> &stores);

	/*
	 * Moves the operands appended since operand_offset behind a new
	 * operand range: their old entries stay unused.
	 */
	void move_operand_prefix(uint32_t &operand_offset, uint32_t end);

	/*
	 * Adds a load from the frame slot at address before the operation whose
	 * operands start at operand_offset: the load takes the next operand, and
	 * the operation's operands so far move behind it.
	 */
	void add_payload_load_before(
			std::vector<BlockItem<IRInstRef>> &block_instructions,
			uint32_t block, uint32_t instruction, IRValueRef address,
			IRValueRef loaded, zend_mir_storage_id storage_id,
			zend_mir_scalar_type_mask type, uint32_t &operand_offset);

	void add_node(
			std::vector<BlockItem<IRInstRef>> &block_instructions,
			uint32_t block, InstNode node);

	template <typename T>
	static void flatten_block_items(
			uint32_t block_count,
			const std::vector<BlockItem<T>> &items,
			std::vector<Slice> &slices,
			std::vector<T> &values);

	/*
	 * Clones a proof-closed scalar callee body into the caller graph after
	 * every component member was lowered: source opcodes are not consulted, and
	 * the target backends see ordinary TPDE instructions and values. The
	 * observer-enabled path still runs the complete Zend-frame call, so pure
	 * scalar instructions may run speculatively before the observer guard; no
	 * effectful, trapping, branching or frame-observing instruction is
	 * admitted.
	 */
	InlinedBody inline_component_scalar_body(
			const zend_tpde_instruction &call,
			uint32_t caller_instruction_index,
			IRValueRef caller_result,
			uint32_t caller_block,
			std::vector<BlockItem<IRInstRef>> &block_instructions);

	/*
	 * Construction runs in a Builder (zend_tpde_ir_adaptor.cpp): the adaptor
	 * being built plus the state only its construction needs. It starts from
	 * an unbuilt adaptor that holds just the plan and the mode.
	 */
	class Builder;
	struct Unbuilt {};
	ZendIRAdaptor(Unbuilt, const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans,
			FunctionMode function_mode);

public:
	static TypedBodyAbiType typed_body_value_abi(
			const zend_tpde_plan *plan, uint32_t value_index) {
		if (value_index >= plan->value_count) {
			return {};
		}
		return plan->values[value_index].local_abi;
	}

	explicit ZendIRAdaptor(const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans,
			FunctionMode function_mode = FunctionMode::ZendEntry);

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
	IRBlockRef block_ref(zend_mir_block_id id) const {
		int32_t index = block_index(id);
		return index < 0 ? INVALID_BLOCK_REF
			: IRBlockRef{static_cast<uint32_t>(index)};
	}
	/*
	 * The source-call phase entry a UserCall* node lowers, or nullptr when
	 * its plan lacks that phase or the node's operands do not fit it.
	 */
	const zend_tpde_source_call_phase_entry *source_call_phase(
			const InstNode &node) const {
		uint8_t required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_NONE;
		switch (node.kind) {
			case InstKind::UserCallInit:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_INIT;
				break;
			case InstKind::UserCallSend:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_SEND;
				break;
			case InstKind::UserCallCheck:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_CHECK;
				break;
			case InstKind::UserCallExpand:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND;
				break;
			case InstKind::UserCallDo:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_DO;
				break;
			default:
				return nullptr;
		}
		const zend_tpde_source_call_phase_entry *phase =
			zend_tpde_source_call_phase_at(plan_, node.source_position);
		const bool send_phase = node.kind == InstKind::UserCallSend;
		const bool indexed_phase = send_phase
			|| node.kind == InstKind::UserCallCheck;
		if (phase == nullptr
				|| phase->instruction_index != node.mir_instruction_index
				|| (phase->phases & required_phase) == 0
				|| (indexed_phase
					? node.argument_index != phase->argument_index
					: node.argument_index != UINT32_MAX)
				|| (send_phase
					? node.operands.size()
						!= (((phase->operand_flags
							& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) != 0)
							? 3 : 2)
					: node.operands.size() != 2)) {
			return nullptr;
		}
		return phase;
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
	/* The deoptimization resume ID whose operation starts at MIR
	 * instruction `index` and has a landing, or UINT32_MAX. */
	uint32_t deopt_resume_index(uint32_t index) const {
		return index < deopt_resume_by_instruction_.size()
			? deopt_resume_by_instruction_[index] : UINT32_MAX;
	}
	bool deopt_resume_landed(uint32_t resume) const {
		return resume < deopt_resume_landed_.size()
			&& deopt_resume_landed_[resume] != 0;
	}
	std::span<const IRValueRef> deopt_landing_values(uint32_t resume) const {
		if (resume >= deopt_landing_slices_.size()) {
			return {};
		}
		return std::span<const IRValueRef>{deopt_landing_values_}.subspan(
			deopt_landing_slices_[resume].offset,
			deopt_landing_slices_[resume].count);
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
	/* The frame slot a value was loaded from, or ZEND_MIR_ID_INVALID. */
	zend_mir_storage_id slot_load_storage(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		return index < slot_load_storage_.size()
			? slot_load_storage_[index] : ZEND_MIR_ID_INVALID;
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
		const uint32_t index = instruction_node.mir_instruction_index;
		zend_mir_instruction_record record = instruction_record_at(index);
		const uint8_t shaped = index < boxed_cond_branches_.size()
			? boxed_cond_branches_[index] : BOXED_COND_BRANCH_UNKNOWN;
		const bool boxed_cond_branch = shaped == BOXED_COND_BRANCH_UNKNOWN
			? is_boxed_cond_branch(mir_instruction(inst)) : shaped != 0;
		ZEND_ASSERT(shaped == BOXED_COND_BRANCH_UNKNOWN
			|| boxed_cond_branch == is_boxed_cond_branch(mir_instruction(inst)));
		if (boxed_cond_branch) {
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
		if (is_mir_value(index)) {
			const TypedBodyAbiType abi =
				typed_body_value_abi(plan_, index - MIR_VALUE_BASE);
			if (plan_->values[index - MIR_VALUE_BASE].argument_index >= 0
					&& abi.valid) {
				return abi.representation;
			}
		}
		return !is_mir_value(index)
			? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
			: plan_->values[index - MIR_VALUE_BASE].representation;
	}
	zend_mir_scalar_type_mask exact_type(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->exact_type;
		}
		if (is_mir_value(index)) {
			const TypedBodyAbiType abi =
				typed_body_value_abi(plan_, index - MIR_VALUE_BASE);
			if (plan_->values[index - MIR_VALUE_BASE].argument_index >= 0
					&& abi.valid) {
				return abi.exact_type;
			}
		}
		return !is_mir_value(index)
			? ZEND_MIR_SCALAR_TYPE_NONE
			: plan_->values[index - MIR_VALUE_BASE].exact_type;
	}
	zend_tpde_machine_value_kind machine_kind(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->machine_kind;
		}
		if (is_mir_value(index)) {
			const TypedBodyAbiType abi =
				typed_body_value_abi(plan_, index - MIR_VALUE_BASE);
			if (plan_->values[index - MIR_VALUE_BASE].argument_index >= 0
					&& abi.valid) {
				return abi.machine_kind;
			}
		}
		return !is_mir_value(index)
			? ZEND_TPDE_MACHINE_VALUE_I64
			: plan_->values[index - MIR_VALUE_BASE].machine_kind;
	}
	zend_mir_ownership_state ownership(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->ownership;
		}
		if (is_mir_value(index)) {
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
		return !is_mir_value(index)
			? ZEND_MIR_OWNERSHIP_STATE_INVALID
			: plan_->values[index - MIR_VALUE_BASE].ownership;
	}
	zend_mir_refcount_state refcount_state(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->refcount_state;
		}
		if (is_mir_value(index)) {
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
		return !is_mir_value(index)
			? ZEND_MIR_REFCOUNT_STATE_INVALID
			: plan_->values[index - MIR_VALUE_BASE].refcount_state;
	}
	bool machine_value_has_result_representation(IRValueRef value) const {
		if (value == INVALID_VALUE_REF) {
			return false;
		}
		const zend_mir_scalar_type_mask type = exact_type(value);
		uint64_t constant_bits;
		return zend_tpde_machine_result_representation(type,
			machine_kind(value),
			zend_mir_scalar_type_is_exact(type)
				&& constant(value, &constant_bits),
			machine_value_is_register_authoritative(value));
	}
	bool machine_value_needs_result_assignment(IRValueRef value) const {
		uint64_t constant_bits;
		return machine_value_has_result_representation(value)
			&& !constant(value, &constant_bits);
	}
	bool machine_value_has_register_definition(IRValueRef value) const {
		if (value == INVALID_VALUE_REF
				|| exact_type(value) == ZEND_MIR_SCALAR_TYPE_NULL) {
			return false;
		}
		if (derived_value(value) != nullptr) {
			return true;
		}
		const uint32_t raw = static_cast<uint32_t>(value);
		if (!is_mir_value(raw)) {
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
		 * comes later in plan order, so COPY nodes on an outer-loop edge may
		 * reference it first. The scalar-candidate closure gives every incoming
		 * edge a compatible machine definition before a ZVAL PHI is promoted.
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
		/* A phi of register values that TPDE materializes. */
		if (function_mode_ == FunctionMode::TypedBody
				&& definition_record.opcode == ZEND_MIR_OPCODE_PHI
				&& raw < phi_values_.size() && phi_values_[raw] != 0) {
			return true;
		}
		/* A typed body has no frame: its scalar operations and copies,
		 * and copies of register zvals, define their results in
		 * registers. */
		if (function_mode_ == FunctionMode::TypedBody
				&& (definition_record.opcode == ZEND_MIR_OPCODE_COPY
					|| definition_record.opcode
						== ZEND_MIR_OPCODE_CANONICALIZE)
				&& plan_value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& plan_value.register_authoritative) {
			return true;
		}
		if (function_mode_ == FunctionMode::TypedBody
				&& ((definition_record.opcode
							>= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
						&& definition_record.opcode
							<= ZEND_MIR_OPCODE_SCALAR_DROP)
					|| definition_record.opcode == ZEND_MIR_OPCODE_COPY
					|| definition_record.opcode
						== ZEND_MIR_OPCODE_CANONICALIZE)
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
		if (!is_mir_value(index)) {
			return true;
		}
		return plan_->values[index - MIR_VALUE_BASE]
			.register_authoritative;
	}
	bool machine_value_has_stable_exact_type(IRValueRef value) const {
		const uint32_t index = static_cast<uint32_t>(value);
		if (derived_value(value) != nullptr
				|| !is_mir_value(index)) {
			return true;
		}
		const zend_tpde_value &plan_value =
			plan_->values[index - MIR_VALUE_BASE];
		return plan_value.constant
			|| !plan_value.canonical_alias_observable;
	}
	zend_mir_storage_id canonical_storage(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			return derived->storage_id;
		}
		return !is_mir_value(index)
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
		if (!is_mir_value(index)) {
			return false;
		}
		/*
		 * The local ABI overrides the initial source-SSA fact for
		 * invocation-local arguments: Zend may keep a null/undef fact for the
		 * pre-RECV CV identity although it is loaded as a string, array, object
		 * or boxed argument. Only the effective machine type may collapse a
		 * value to the null constant.
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
		if (!is_mir_value(index)) {
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
			if (!valid_value_index(candidate.register_alias_value_index)
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

	bool cur_func_may_emit_calls() const {
		return function_mode_ == FunctionMode::TypedBody
			? plan_->typed_body_may_emit_calls
			: plan_->zend_entry_may_emit_calls;
	}
	bool cur_needs_unwind_info() const {
		return cur_func_may_emit_calls();
	}
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
	/* A guard's cold block: TPDE writes it to the cold area of the
	 * function, behind the hot code. Only the x64 compiler appends that
	 * area to the function; arm64 keeps every block in line. */
	bool block_is_cold(IRBlockRef block) const {
		if constexpr (ZEND_NATIVE_HOST_TARGET
				!= ZEND_NATIVE_TARGET_LINUX_AMD64) {
			(void) block;
			return false;
		}
		return block_cold_[static_cast<uint32_t>(block)] != 0;
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
	void reset() {
		std::fill(block_info_.begin(), block_info_.end(), 0);
		std::fill(block_info2_.begin(), block_info2_.end(), 0);
		std::fill(fused_instructions_.begin(), fused_instructions_.end(), 0);
	}
};

/*
 * TPDE's function switch is the component boundary: value, block and
 * instruction references are local to the selected function, while the compiler
 * owns one symbol table, allocator run and object image for the component. The
 * single-function adaptor stays the per-function implementation behind this
 * zero-copy multi-function view, instead of flattening all ZNMIR into a second
 * IR.
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
	std::vector<uint8_t> profile_cold_blocks_;
	uint64_t typed_body_call_site_count_ = 0;
	uint64_t typed_body_frame_bytes_elided_ = 0;

public:
	explicit ZendComponentIRAdaptor(
			std::span<const zend_tpde_plan *const> plans) {
		members_.reserve(plans.size());
		function_views_.reserve(plans.size() * 2);
		functions_.reserve(plans.size() * 2);
		link_names_.reserve(plans.size() * 2);
		for (uint32_t index = 0; index < plans.size(); ++index) {
			/* Freezing the deoptimization exits already built a generic
			 * copy's Zend entry from the same plans. */
			if (plans[index]->deopt_landing_adaptor != nullptr) {
				members_.emplace_back(std::exchange(
					plans[index]->deopt_landing_adaptor, nullptr));
			} else {
				members_.push_back(
					std::make_unique<ZendIRAdaptor>(plans[index], plans,
						ZendIRAdaptor::FunctionMode::ZendEntry));
			}
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
			/* Metrics count source sites: a specialized function's generic
			 * copy repeats them. */
			if (!member->plan()->deopt_landings) {
				count += member->inlined_user_body_count();
			}
		}
		return count;
	}
	void mark_typed_body_call(uint64_t frame_bytes) {
		if (active_->plan()->deopt_landings) {
			return;
		}
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
			? body->typed_body_return_abi
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
	IRBlockRef block_ref(zend_mir_block_id id) const {
		return active_->block_ref(id);
	}
	const InstNode &node(IRInstRef inst) const { return active_->node(inst); }
	const zend_tpde_source_call_phase_entry *source_call_phase(
			const InstNode &node) const {
		return active_->source_call_phase(node);
	}
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
	uint32_t deopt_resume_index(uint32_t index) const {
		return active_->deopt_resume_index(index);
	}
	bool deopt_resume_landed(uint32_t resume) const {
		return active_->deopt_resume_landed(resume);
	}
	std::span<const IRValueRef> deopt_landing_values(uint32_t resume) const {
		return active_->deopt_landing_values(resume);
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
	zend_mir_storage_id slot_load_storage(IRValueRef value) const {
		return active_->slot_load_storage(value);
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
	bool block_is_cold(IRBlockRef block) const {
		const uint32_t index = static_cast<uint32_t>(block);
		if (index < profile_cold_blocks_.size()
				&& profile_cold_blocks_[index] != profile_block_default) {
			return profile_cold_blocks_[index] == profile_block_cold;
		}
		return active_->block_is_cold(block);
	}
	/* A layout profile's placement of each block: rarely entered blocks
	 * go to the cold area (Linux x86-64), as guarded cold blocks do, and
	 * often entered guarded cold blocks stay in the hot stream. */
	static constexpr uint8_t profile_block_default = 0;
	static constexpr uint8_t profile_block_cold = 1;
	static constexpr uint8_t profile_block_hot = 2;
	void set_profile_cold_blocks(std::vector<uint8_t> blocks) {
		profile_cold_blocks_ = std::move(blocks);
	}
	/* The block kinds placed in the cold area without a profile. */
	bool block_kind_is_cold(IRBlockRef block) const {
		return active_->block_is_cold(block);
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
