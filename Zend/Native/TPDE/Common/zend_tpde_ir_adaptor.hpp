// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Optimizer/zend_ssa.h"

#include <tpde/IRAdaptor.hpp>
#include <tpde/ValLocalIdx.hpp>

#include <algorithm>
#include <array>
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
	static constexpr uint32_t EXECUTION_CONTEXT_VALUE = 3;
	static constexpr uint32_t MIR_VALUE_BASE = 4;

	enum class InstKind : uint8_t {
		LoadFrame,
		LoadExecutionContext,
		UserOpcodeLanding,
		UserOpcodeGateway,
		UserOpcodeDispatch,
		UserOpcodeCallFragment,
		GeneratorGateway,
		GeneratorResume,
		ZvalTypeLoad,
		ZvalPayloadLoad,
		ZvalCopy,
		ZvalMove,
		ZvalStore,
		ZvalReleaseFast,
		ZvalGuardArguments,
		ZvalGuardType,
		SlowPathCall,
		BoxedCondGuard,
		BoxedCondCold,
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
	};

	struct DerivedValue {
		zend_mir_representation representation;
		zend_mir_scalar_type_mask exact_type;
		zend_mir_storage_id storage_id;
		bool constant = false;
		uint64_t constant_bits = 0;
	};

	struct InlinedBody {
		bool valid = false;
		IRValueRef value = INVALID_VALUE_REF;
		IRValueRef checked_left = INVALID_VALUE_REF;
		IRValueRef checked_right = INVALID_VALUE_REF;
		uint32_t checked_source_opcode = UINT32_MAX;

		bool checked() const {
			return checked_source_opcode != UINT32_MAX;
		}
	};

	struct ArgumentGuard {
		uint32_t argument_index;
		zend_mir_storage_id storage_id;
		zend_mir_scalar_type_mask exact_type;
		zend_tpde_machine_value_kind machine_kind;
	};

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
	std::array<IRFuncRef, 1> functions_{IRFuncRef{0}};
	std::array<IRValueRef, 2> arguments_{
		IRValueRef{EXECUTE_DATA_VALUE},
		IRValueRef{EXECUTION_CONTEXT_ARGUMENT}};
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
	std::vector<uint8_t> fused_instructions_;
	std::vector<IRValueRef> operands_;
	std::vector<uint8_t> phi_values_;
	std::vector<uint8_t> register_values_;
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	std::vector<DerivedValue> derived_values_;
	std::vector<IRValueRef> frame_slot_references_;
	std::vector<ArgumentGuard> argument_guards_;
	std::vector<uint32_t> user_opcode_next_landings_;
	std::vector<uint32_t> user_opcode_dispatch_to_sources_;
	zend_tpde_instruction synthetic_instruction_{};
	bool valid_ = true;

	int32_t block_index(zend_mir_block_id id) const {
		return zend_tpde_block_index(plan_, id);
	}

	IRValueRef value_ref(zend_mir_value_id id) const {
		int32_t index = zend_tpde_value_index(plan_, id);
		return index < 0 ? INVALID_VALUE_REF
			: IRValueRef{MIR_VALUE_BASE + static_cast<uint32_t>(index)};
	}

	IRValueRef source_operand_value_ref(
			const zend_mir_source_operand_ref &operand) const {
		zend_mir_value_id value_id;

		switch (operand.kind) {
			case ZEND_MIR_SOURCE_OPERAND_LITERAL:
				value_id = zend_mir_value_from_synthetic(operand.index);
				break;
			case ZEND_MIR_SOURCE_OPERAND_SLOT:
			case ZEND_MIR_SOURCE_OPERAND_SSA:
				if (operand.ssa_variable_id == ZEND_MIR_ID_INVALID) {
					return INVALID_VALUE_REF;
				}
				value_id = zend_mir_value_from_original_ssa(
					operand.ssa_variable_id);
				break;
			default:
				return INVALID_VALUE_REF;
		}
		return value_ref(value_id);
	}

	bool long_binary_machine_operands(
			const zend_tpde_instruction &instruction,
			IRValueRef &left, IRValueRef &right) const {
		zend_tpde_long_binary layout;

		if (!zend_tpde_long_binary_at(instruction, &layout)) {
			return false;
		}
		left = source_operand_value_ref(instruction.value_operation.op1);
		right = source_operand_value_ref(instruction.value_operation.op2);
		return left != INVALID_VALUE_REF
			&& right != INVALID_VALUE_REF
			&& exact_type(left) == ZEND_MIR_SCALAR_TYPE_I64
			&& exact_type(right) == ZEND_MIR_SCALAR_TYPE_I64
			&& machine_value_is_register_authoritative(left)
			&& machine_value_is_register_authoritative(right);
	}

	IRValueRef add_derived_value(
			zend_mir_representation representation,
			zend_mir_scalar_type_mask exact_type,
			zend_mir_storage_id storage_id,
			bool constant = false, uint64_t constant_bits = 0) {
		if (derived_values_.size()
				>= UINT32_MAX - MIR_VALUE_BASE - plan_->value_count) {
			valid_ = false;
			return INVALID_VALUE_REF;
		}
		const IRValueRef value{
			MIR_VALUE_BASE + plan_->value_count
				+ static_cast<uint32_t>(derived_values_.size())};
		derived_values_.push_back({
			representation, exact_type, storage_id, constant, constant_bits});
		if (representation == ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				&& zend_mir_id_is_valid(storage_id)) {
			frame_slot_references_.push_back(value);
		}
		return value;
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
			return InstKind::ZvalStore;
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
				return instruction.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_TMP
						|| instruction.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_VAR
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

	static bool is_boxed_cond_branch(
			const zend_tpde_instruction &instruction,
			const zend_mir_instruction_record &record) {
		if (!instruction.has_value_operation) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_COND_BRANCH) {
			return instruction.value_operation.opcode
				== ZEND_MIR_OPCODE_VALUE_COND_BRANCH;
		}
		return (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				|| record.opcode
					== ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH)
			&& instruction.value_operation.opcode == record.opcode;
	}

	zend_mir_instruction_record instruction_record_at(uint32_t index) const {
		return zend_tpde_instruction_record_at(
			plan_, zend_tpde_instruction_at(plan_, index));
	}

	void add_node(
			std::vector<BlockItem<IRInstRef>> &block_instructions,
			uint32_t block, InstNode node) {
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

	static void flatten_unique_successors(
			uint32_t block_count,
			const std::vector<BlockItem<IRBlockRef>> &items,
			std::vector<Slice> &slices,
			std::vector<IRBlockRef> &values) {
		flatten_block_items(block_count, items, slices, values);
		std::vector<uint32_t> seen(block_count, UINT32_MAX);
		uint32_t write = 0;
		for (uint32_t block = 0; block < block_count; ++block) {
			const Slice source = slices[block];
			Slice &result = slices[block];
			result.offset = write;
			result.count = 0;
			for (uint32_t n = 0; n < source.count; ++n) {
				IRBlockRef target = values[source.offset + n];
				uint32_t target_index = static_cast<uint32_t>(target);
				if (seen[target_index] == block) {
					continue;
				}
				seen[target_index] = block;
				values[write++] = target;
				++result.count;
			}
		}
		values.resize(write);
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
			IRValueRef caller_result,
			uint32_t caller_block,
			std::vector<BlockItem<IRInstRef>> &block_instructions) {
		if (call.direct_call == nullptr
				|| call.component_target_index == UINT32_MAX
				|| call.component_target_index >= component_plans_.size()
				|| (call.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) == 0
				|| call.direct_call->receiver_kind
					!= ZEND_NATIVE_INTERNAL_RECEIVER_NONE
				|| (call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I1
					&& call.direct_call->result_type
						!= ZEND_MIR_SCALAR_TYPE_I64)) {
			return {};
		}
		const zend_tpde_plan *callee =
			component_plans_[call.component_target_index];
		if (callee == nullptr || callee == plan_
				|| callee->block_count != 1
				|| callee->argument_count != call.call_argument_count
				|| callee->generator_resume_count != 0
				|| callee->user_opcode_callbacks) {
			return {};
		}

		std::vector<IRValueRef> values(
			callee->value_count, INVALID_VALUE_REF);
		const size_t derived_value_checkpoint = derived_values_.size();
		const size_t frame_slot_reference_checkpoint =
			frame_slot_references_.size();
		const size_t operand_checkpoint = operands_.size();
		const size_t node_checkpoint = nodes_.size();
		const size_t fused_checkpoint = fused_instructions_.size();
		const size_t block_instruction_checkpoint =
			block_instructions.size();
		auto fail_inline = [&]() -> InlinedBody {
			derived_values_.erase(
				derived_values_.begin() + derived_value_checkpoint,
				derived_values_.end());
			frame_slot_references_.erase(
				frame_slot_references_.begin()
					+ frame_slot_reference_checkpoint,
				frame_slot_references_.end());
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
			IRValueRef caller_value = INVALID_VALUE_REF;
			if (zend_mir_id_is_valid(argument.value_id)) {
				caller_value = value_ref(argument.value_id);
			} else {
				const zend_native_direct_call_argument &descriptor_argument =
					call.direct_call->arguments[argument_index];
				if (!zend_mir_scalar_type_is_exact(
						descriptor_argument.exact_type)
						|| descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_NULL
						|| descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_F64) {
					return fail_inline();
				}
				caller_value = add_derived_value(
					callee->values[callee_value_index].representation,
					descriptor_argument.exact_type,
					ZEND_MIR_ID_INVALID, true,
					descriptor_argument.scalar_bits);
			}
			if (caller_value == INVALID_VALUE_REF
					|| !zend_mir_scalar_type_is_exact(
						exact_type(caller_value))
					|| exact_type(caller_value)
						!= callee->values[callee_value_index].exact_type
					|| exact_type(caller_value)
						== ZEND_MIR_SCALAR_TYPE_F64) {
				return fail_inline();
			}
			values[static_cast<uint32_t>(callee_value_index)] =
				caller_value;
		}

		auto mapped_value = [&](zend_mir_value_id value_id) -> IRValueRef {
			const int32_t value_index =
				zend_tpde_value_index(callee, value_id);
			if (value_index < 0) {
				return INVALID_VALUE_REF;
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

		IRValueRef returned = INVALID_VALUE_REF;
		bool saw_return = false;
		IRValueRef checked_left = INVALID_VALUE_REF;
		IRValueRef checked_right = INVALID_VALUE_REF;
		uint32_t checked_source_opcode = UINT32_MAX;
		for (uint32_t index = 0; index < callee->instruction_count; ++index) {
			const zend_tpde_instruction &instruction =
				callee->instructions[index];
			const zend_mir_instruction_record record =
				zend_tpde_instruction_record_at(callee, &instruction);
			if (record.block_id != callee->block_ids[0]) {
				return fail_inline();
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
			if (record.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
					&& instruction.runtime_helper
						== ZEND_NATIVE_HELPER_COUNT) {
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
				returned = mapped_value(returned_id);
				saw_return = returned != INVALID_VALUE_REF;
				if (!saw_return) {
					return fail_inline();
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_RETURN) {
				if (saw_return || instruction.operand_count != 1) {
					return fail_inline();
				}
				returned = mapped_value(zend_tpde_operand_at(
					callee, &instruction, 0));
				saw_return = returned != INVALID_VALUE_REF;
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					|| record.opcode
						== ZEND_MIR_OPCODE_CANONICALIZE) {
				if (checked_source_opcode != UINT32_MAX) {
					return fail_inline();
				}
				if (!zend_mir_id_is_valid(record.result_id)
						|| instruction.operand_count != 1) {
					return fail_inline();
				}
				const int32_t result_index =
					zend_tpde_value_index(callee, record.result_id);
				const IRValueRef input = mapped_value(
					zend_tpde_operand_at(callee, &instruction, 0));
				if (result_index < 0 || input == INVALID_VALUE_REF
						|| exact_type(input)
							!= callee->values[result_index].exact_type) {
					return fail_inline();
				}
				values[static_cast<uint32_t>(result_index)] = input;
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
				if (checked_source_opcode != UINT32_MAX
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
				auto mapped_source_operand =
					[&](const zend_mir_source_operand_ref &source)
						-> IRValueRef {
						zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
						if (source.kind
								== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
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
						return zend_mir_id_is_valid(value_id)
							? mapped_value(value_id)
							: INVALID_VALUE_REF;
					};
				checked_left = mapped_source_operand(operation.op1);
				checked_right = mapped_source_operand(operation.op2);
				if (result_index < 0
						|| callee->values[result_index].exact_type
							!= ZEND_MIR_SCALAR_TYPE_I64
						|| checked_left == INVALID_VALUE_REF
						|| checked_right == INVALID_VALUE_REF
						|| exact_type(checked_left)
							!= ZEND_MIR_SCALAR_TYPE_I64
						|| exact_type(checked_right)
							!= ZEND_MIR_SCALAR_TYPE_I64) {
					return fail_inline();
				}
				checked_source_opcode = operation.source_opcode;
				values[static_cast<uint32_t>(result_index)] =
					caller_result;
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
		if (!saw_return || returned == INVALID_VALUE_REF
				|| exact_type(returned) != call.direct_call->result_type) {
			return fail_inline();
		}
		if (checked_source_opcode != UINT32_MAX) {
			if (returned != caller_result) {
				return fail_inline();
			}
			return {
				true,
				caller_result,
				checked_left,
				checked_right,
				checked_source_opcode,
			};
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
	explicit ZendIRAdaptor(const zend_tpde_plan *plan)
		: ZendIRAdaptor(plan,
			std::span<const zend_tpde_plan *const>{&plan, 1}) {}

	explicit ZendIRAdaptor(const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans)
		: plan_(plan), component_plans_(component_plans) {
		std::vector<BlockItem<IRBlockRef>> block_successors;
		std::vector<BlockItem<IRInstRef>> block_instructions;
		std::vector<BlockItem<IRValueRef>> block_phis;
		std::vector<uint32_t> finally_return_blocks;
		std::vector<IRBlockRef> finally_targets;
		std::vector<uint8_t> generator_resume_emitted;
		std::vector<uint8_t> source_landing_emitted;
		std::vector<uint32_t> source_landing_blocks;
		std::vector<uint32_t> source_block_next;
		std::vector<uint32_t> boxed_cond_cold_blocks(
			plan_->instruction_count, UINT32_MAX);
		std::vector<uint32_t> boxed_cond_cold_by_predecessor(
			plan_->block_count, UINT32_MAX);
		bool source_call_fragments = false;

		blocks_.reserve(plan_->block_count);
		block_successors.reserve(plan_->block_count * 2);
		block_instructions.reserve(plan_->instruction_count + plan_->value_count + 1);
		block_phis.reserve(plan_->value_count);
		phi_input_slices_.resize(MIR_VALUE_BASE + plan_->value_count);
		phi_values_.resize(MIR_VALUE_BASE + plan_->value_count);
		register_values_.resize(MIR_VALUE_BASE + plan_->value_count);
		for (uint32_t i = 0; i < plan_->value_count; ++i) {
			const zend_tpde_value &value = plan_->values[i];
			register_values_[MIR_VALUE_BASE + i] =
				!value.constant
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER
				&& ((zend_mir_scalar_type_is_exact(value.exact_type)
						&& value.exact_type
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		}
		/*
		 * A direct user call publishes its canonical result for Zend
		 * observability, but that store is also the definition of the same SSA
		 * value.  Keep arbitrary boxed results as two-part TPDE values when a
		 * later machine consumer exists instead of forcing that consumer to
		 * reload through the frame.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER
					|| record.representation
						!= ZEND_MIR_REPRESENTATION_ZVAL
					|| !zend_mir_id_is_valid(record.result_id)) {
				continue;
			}
			const IRValueRef result = value_ref(record.result_id);
			if (result != INVALID_VALUE_REF
					&& machine_kind(result)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				register_values_[static_cast<uint32_t>(result)] = 1;
			}
		}
		/*
		 * COPY/Pi and PHI are representation-preserving identities.  Propagate
		 * boxed register authority to a fixed point before constructing TPDE
		 * uses, so loop-header PHIs do not depend on persistent instruction
		 * order and call results consumed only through such identities remain
		 * visible to the allocator.
		 */
		bool register_values_changed;
		do {
			register_values_changed = false;
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				if ((record.opcode != ZEND_MIR_OPCODE_COPY
							&& record.opcode != ZEND_MIR_OPCODE_PHI)
						|| record.representation
							!= ZEND_MIR_REPRESENTATION_ZVAL
						|| !zend_mir_id_is_valid(record.result_id)
						|| instruction.operand_count == 0) {
					continue;
				}
				const IRValueRef result = value_ref(record.result_id);
				if (result == INVALID_VALUE_REF
						|| machine_kind(result)
							!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| machine_value_is_register_authoritative(result)) {
					continue;
				}
				bool all_inputs_register = true;
				for (uint32_t n = 0;
						n < instruction.operand_count; ++n) {
					const IRValueRef input = value_ref(
						zend_tpde_operand_at(plan_, &instruction, n));
					if (input == INVALID_VALUE_REF
							|| machine_kind(input)
								!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							|| !machine_value_is_register_authoritative(
								input)) {
						all_inputs_register = false;
						break;
					}
				}
				if (all_inputs_register) {
					register_values_[
						static_cast<uint32_t>(result)] = 1;
					register_values_changed = true;
				}
			}
		} while (register_values_changed);
		uint32_t synthetic_block_count = 0;
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			zend_tpde_value_condition condition;
			if (!is_boxed_cond_branch(instruction, record)
					|| !zend_tpde_value_condition_at(
						instruction, &condition)) {
				continue;
			}
			const int32_t predecessor = block_index(record.block_id);
			if (predecessor < 0
					|| boxed_cond_cold_by_predecessor[
						static_cast<uint32_t>(predecessor)] != UINT32_MAX) {
				valid_ = false;
				continue;
			}
			const uint32_t cold_block =
				plan_->block_count + synthetic_block_count++;
			boxed_cond_cold_blocks[i] = cold_block;
			boxed_cond_cold_by_predecessor[
				static_cast<uint32_t>(predecessor)] = cold_block;
		}
		const uint32_t tpde_block_count =
			plan_->block_count + synthetic_block_count;
		blocks_.reserve(tpde_block_count);
		block_info_.resize(tpde_block_count);
		block_info2_.resize(tpde_block_count);
		for (uint32_t i = 0; i < plan_->block_count; ++i) {
			blocks_.push_back(IRBlockRef{i});
			uint32_t count = plan_->view->successor_count(
				plan_->view->context, plan_->block_ids[i]);
			for (uint32_t n = 0; n < count; ++n) {
				zend_mir_block_id target;
				if (!plan_->view->successor_at(plan_->view->context,
						plan_->block_ids[i], n, &target)) {
					valid_ = false;
					continue;
				}
				int32_t target_index = block_index(target);
				if (target_index < 0) {
					valid_ = false;
					continue;
				}
				block_successors.push_back({i, IRBlockRef{
					static_cast<uint32_t>(target_index)}});
			}
			if (boxed_cond_cold_by_predecessor[i] != UINT32_MAX) {
				block_successors.push_back(
					{i, IRBlockRef{boxed_cond_cold_by_predecessor[i]}});
			}
		}
		for (uint32_t predecessor = 0;
				predecessor < plan_->block_count; ++predecessor) {
			const uint32_t cold_block =
				boxed_cond_cold_by_predecessor[predecessor];
			if (cold_block == UINT32_MAX) {
				continue;
			}
			blocks_.push_back(IRBlockRef{cold_block});
			const uint32_t successor_count =
				plan_->view->successor_count(
					plan_->view->context, plan_->block_ids[predecessor]);
			if (successor_count != 2) {
				valid_ = false;
				continue;
			}
			for (uint32_t n = 0; n < successor_count; ++n) {
				zend_mir_block_id target;
				const int32_t target_index =
					plan_->view->successor_at(
						plan_->view->context,
						plan_->block_ids[predecessor], n, &target)
					? block_index(target) : -1;
				if (target_index < 0) {
					valid_ = false;
					continue;
				}
				block_successors.push_back(
					{cold_block, IRBlockRef{
						static_cast<uint32_t>(target_index)}});
			}
		}
		/*
		 * Zend's CFG records FAST_CALL's continuation on the call block while
		 * the executable edge is selected by FAST_RET. Add those dynamic
		 * destinations to TPDE's internal CFG so liveness sees every machine
		 * branch without changing persistent source identity. Collect every
		 * continuation, handler, return block, and exception edge in one MIR
		 * pass; the former return-by-instruction rescans were quadratic.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			source_call_fragments =
				source_call_fragments
				|| instruction.user_opcode_call_fragments;
			const bool boxed_cond_branch =
				is_boxed_cond_branch(instruction, record);
			int32_t record_block = block_index(record.block_id);
			if (record_block < 0) {
				valid_ = false;
				continue;
			}
			if (zend_mir_id_is_valid(instruction.exception_block_id)) {
				int32_t exception_block =
					block_index(instruction.exception_block_id);
				if (exception_block < 0) {
					valid_ = false;
				} else {
					block_successors.push_back({
						static_cast<uint32_t>(record_block),
						IRBlockRef{static_cast<uint32_t>(exception_block)}});
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_FINALLY_RETURN) {
				finally_return_blocks.push_back(
					static_cast<uint32_t>(record_block));
			} else if (record.opcode == ZEND_MIR_OPCODE_FINALLY_CALL) {
				zend_mir_block_id continuation;
				if (plan_->view->successor_count(
							plan_->view->context, record.block_id) != 2
						|| !plan_->view->successor_at(
							plan_->view->context, record.block_id, 1,
							&continuation)) {
					valid_ = false;
					continue;
				}
				int32_t continuation_block = block_index(continuation);
				if (continuation_block < 0) {
					valid_ = false;
					continue;
				}
				finally_targets.push_back(
					IRBlockRef{static_cast<uint32_t>(continuation_block)});
			} else if ((record.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
						|| record.opcode == ZEND_MIR_OPCODE_FINALLY_ENTER)
					&& record.block_id != plan_->function.entry_block_id) {
				finally_targets.push_back(
					IRBlockRef{static_cast<uint32_t>(record_block)});
			}
		}
		generator_resume_emitted.resize(
			plan_->generator_resume_count, 0);
		const bool source_landings =
			plan_->user_opcode_callbacks || source_call_fragments;
		if (source_landings && plan_->source_op_array != nullptr) {
			source_landing_emitted.resize(plan_->source_op_array->last, 0);
			source_landing_blocks.resize(
				plan_->source_op_array->last, UINT32_MAX);
			if (plan_->user_opcode_callbacks) {
				user_opcode_next_landings_.resize(
					plan_->source_op_array->last, UINT32_MAX);
			}
			if (plan_->source_ssa == nullptr
					|| plan_->source_ssa->cfg.blocks == nullptr
					|| plan_->source_ssa->cfg.map == nullptr) {
				valid_ = false;
			} else {
				const zend_cfg &cfg = plan_->source_ssa->cfg;
				source_block_next.resize(cfg.blocks_count, UINT32_MAX);
				for (uint32_t instruction = 0;
						instruction < plan_->instruction_count;
						++instruction) {
					const zend_mir_instruction_record record =
						instruction_record_at(instruction);
					if (record.source_position_id
							>= plan_->source_op_array->last) {
						continue;
					}
					const uint32_t source_block =
						cfg.map[record.source_position_id];
					const int32_t mir_block =
						block_index(record.block_id);
					if (source_block >= cfg.blocks_count
							|| mir_block < 0) {
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
							static_cast<uint32_t>(mir_block);
					}
				}
				for (uint32_t source_block = 0;
						source_block < cfg.blocks_count; ++source_block) {
					const zend_basic_block &block =
						cfg.blocks[source_block];
					if ((block.flags & ZEND_BB_REACHABLE) == 0
							|| block.start > plan_->source_op_array->last
							|| block.len
								> plan_->source_op_array->last - block.start) {
						continue;
					}
					source_block_next[source_block] = block.start;
					const uint32_t block_end = block.start + block.len;
					uint32_t next_mir_block = UINT32_MAX;
					for (uint32_t source = block_end;
							source-- > block.start;) {
						if (plan_->source_op_array->opcodes[source].opcode
								== ZEND_OP_DATA) {
							continue;
						}
						if (source_landing_blocks[source] != UINT32_MAX) {
							next_mir_block = source_landing_blocks[source];
						} else if (next_mir_block != UINT32_MAX) {
							source_landing_blocks[source] = next_mir_block;
						}
					}
					uint32_t previous_mir_block = UINT32_MAX;
					for (uint32_t source = block.start;
							source < block_end; ++source) {
						if (plan_->source_op_array->opcodes[source].opcode
								== ZEND_OP_DATA) {
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
				for (uint32_t source = plan_->source_op_array->last;
						source-- > 0;) {
					if (source_landing_blocks[source] != UINT32_MAX) {
						next = source;
					}
					user_opcode_next_landings_[source] = next;
				}
				for (uint32_t source = 0;
						source < plan_->source_op_array->last; ++source) {
					if (source_landing_blocks[source] == UINT32_MAX
							|| source
								>= plan_->user_opcode_source_operation_count) {
						continue;
					}
					user_opcode_dispatch_to_sources_.push_back(source);
				}
			}
		}
		for (uint32_t return_block : finally_return_blocks) {
			for (IRBlockRef target : finally_targets) {
				block_successors.push_back({return_block, target});
			}
		}
		flatten_unique_successors(tpde_block_count, block_successors,
			successor_slices_, successors_);

		int32_t entry = block_index(plan_->function.entry_block_id);
		if (entry < 0) {
			valid_ = false;
			return;
		}
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
		uint32_t context_operand_offset =
			static_cast<uint32_t>(operands_.size());
		operands_.push_back(IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
			InstKind::LoadExecutionContext,
			UINT32_MAX,
			UINT32_MAX,
			IRValueRef{EXECUTION_CONTEXT_VALUE},
			{},
			context_operand_offset,
			1,
			true});
		if (plan_->generator_resume_count != 0) {
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			operands_.push_back(IRValueRef{EXECUTION_CONTEXT_VALUE});
			add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
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
		for (uint32_t i = 0; i < plan_->value_count; ++i) {
			const zend_tpde_value &value = plan_->values[i];
			const bool exact_scalar =
				zend_mir_scalar_type_is_exact(value.exact_type)
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL;
			const bool unboxed_pointer =
				value.machine_kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| value.machine_kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
				|| value.machine_kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
				|| value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
			const bool boxed_register_value =
				value.machine_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER;
			if (value.argument_index < 0
					|| (!exact_scalar && !unboxed_pointer
						&& !boxed_register_value)) {
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
					value.exact_type,
					value.machine_kind});
			}
			const IRValueRef payload_address = add_derived_value(
				ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
				ZEND_MIR_SCALAR_TYPE_NONE, storage_id);
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
				value.exact_type});
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
		auto mark_machine_use = [&](zend_mir_value_id id) {
			const int32_t index = zend_tpde_value_index(plan_, id);
			if (index >= 0) {
				machine_value_used[static_cast<uint32_t>(index)] = 1;
			}
		};
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
				for (uint32_t value = 0;
						value < plan_->value_count; ++value) {
					if (zend_tpde_generator_resume_value_live(
							plan_, resume_index, value)) {
						operands_.push_back(IRValueRef{
							MIR_VALUE_BASE + value});
					}
				}
				add_node(block_instructions, block, InstNode{
					InstKind::GeneratorResume,
					UINT32_MAX,
					resume_index,
					INVALID_VALUE_REF,
					{},
					operand_offset,
					static_cast<uint32_t>(operands_.size())
						- operand_offset,
					false});
			}
		};
		auto emit_user_opcode_landing = [&](uint32_t block,
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
						plan_->source_op_array->opcodes[
							source_position].opcode) != nullptr) {
					const uint32_t operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(IRValueRef{FRAME_VALUE});
					operands_.push_back(IRValueRef{EXECUTION_CONTEXT_VALUE});
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
			for (uint32_t instruction_index = 0;
					instruction_index < plan_->instruction_count;
					++instruction_index) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[instruction_index];
				if (!instruction.user_opcode_call_fragments
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

		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const bool boxed_cond_branch =
				is_boxed_cond_branch(instruction, record);
			const IRValueRef result = value_ref(record.result_id);
			const bool result_is_machine =
				result != INVALID_VALUE_REF
				&& ((zend_mir_scalar_type_is_exact(exact_type(result))
						&& exact_type(result)
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| (machine_kind(result)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& machine_value_is_register_authoritative(
							result)));
			IRValueRef long_left = INVALID_VALUE_REF;
			IRValueRef long_right = INVALID_VALUE_REF;
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
					&& long_binary_machine_operands(
						instruction, long_left, long_right)) {
				mark_machine_use(
					plan_->values[
						static_cast<uint32_t>(long_left)
							- MIR_VALUE_BASE].id);
				mark_machine_use(
					plan_->values[
						static_cast<uint32_t>(long_right)
							- MIR_VALUE_BASE].id);
			}
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				if (result_is_machine) {
					for (uint32_t n = 0;
							n < instruction.operand_count; ++n) {
						mark_machine_use(zend_tpde_operand_at(
							plan_, &instruction, n));
					}
				}
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				if (result_is_machine
						&& instruction.operand_count == 1) {
					mark_machine_use(zend_tpde_operand_at(
						plan_, &instruction, 0));
				}
				continue;
			}
			if (!result_is_machine
					&& (record.opcode == ZEND_MIR_OPCODE_COPY
						|| record.opcode
							== ZEND_MIR_OPCODE_CANONICALIZE
						|| record.opcode
							== ZEND_MIR_OPCODE_I1_TO_I64)) {
				continue;
			}
			const uint32_t data_operand_count =
				record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
					? 1
				: boxed_cond_branch
					? 0
				: record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					|| record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| (record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& instruction.direct_call != nullptr)
					? 0 : instruction.operand_count;
			for (uint32_t n = 0; n < data_operand_count; ++n) {
				mark_machine_use(zend_tpde_operand_at(
					plan_, &instruction, n));
			}
			if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& instruction.direct_call != nullptr
					&& (instruction.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0) {
				for (uint32_t n = 0;
						n < instruction.call_argument_count; ++n) {
					zend_mir_call_argument_ref argument;
					if (!zend_tpde_call_argument_at(plan_,
							instruction.call_argument_offset + n,
							&argument)) {
						valid_ = false;
						continue;
					}
					if (zend_mir_id_is_valid(argument.value_id)) {
						mark_machine_use(argument.value_id);
					}
				}
			}
		}

		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const bool boxed_cond_branch =
				is_boxed_cond_branch(instruction, record);
			if (!zend_mir_id_is_valid(record.id)) {
				valid_ = false;
				continue;
			}
			int32_t block = block_index(record.block_id);
			if (block < 0) {
				valid_ = false;
				continue;
			}
			if (source_landings && plan_->source_ssa != nullptr
					&& record.source_position_id
						< plan_->source_op_array->last) {
				const uint32_t source_block =
					plan_->source_ssa->cfg.map[
						record.source_position_id];
				if (source_block < source_block_next.size()) {
					const zend_basic_block &source =
						plan_->source_ssa->cfg.blocks[source_block];
					uint32_t &next_source =
						source_block_next[source_block];
					while (next_source != UINT32_MAX
							&& next_source <= record.source_position_id
							&& next_source < source.start + source.len) {
						emit_user_opcode_landing(
							static_cast<uint32_t>(block), next_source++);
					}
				}
			}
			emit_generator_resume(static_cast<uint32_t>(block),
				record.source_position_id, false);
			if ((record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
						|| record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
					&& instruction.user_opcode_call_fragments) {
				continue;
			}
			IRValueRef result = value_ref(record.result_id);
			if (record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
				continue;
			}
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				if (result == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				if (representation(result) == ZEND_MIR_REPRESENTATION_ZVAL) {
					const uint32_t predecessors =
						plan_->view->predecessor_count(
							plan_->view->context, record.block_id);
					const zend_mir_storage_id result_storage =
						canonical_storage(result);
					if (predecessors != instruction.operand_count) {
						valid_ = false;
						continue;
					}
					bool register_phi =
						machine_kind(result)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
					bool shared_storage =
						zend_mir_id_is_valid(result_storage);
					for (uint32_t n = 0; n < predecessors; ++n) {
						const IRValueRef input = value_ref(
							zend_tpde_operand_at(plan_, &instruction, n));
						if (input == INVALID_VALUE_REF
								|| representation(input)
									!= ZEND_MIR_REPRESENTATION_ZVAL) {
							valid_ = false;
							register_phi = false;
							shared_storage = false;
							break;
						}
						register_phi &=
							machine_kind(input)
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
								&& machine_value_is_register_authoritative(
									input);
						shared_storage &=
							canonical_storage(input) == result_storage;
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
							zend_mir_block_id predecessor;
							const IRValueRef input = value_ref(
								zend_tpde_operand_at(
									plan_, &instruction, n));
							const int32_t predecessor_index =
								plan_->view->predecessor_at(
										plan_->view->context,
										record.block_id, n,
										&predecessor)
									? block_index(predecessor) : -1;
							if (predecessor_index < 0) {
								valid_ = false;
								continue;
							}
							phi_inputs_.push_back({input,
								IRBlockRef{static_cast<uint32_t>(
									predecessor_index)}});
							++input_slice.count;
							const uint32_t cold_predecessor =
								boxed_cond_cold_by_predecessor[
									static_cast<uint32_t>(
										predecessor_index)];
							if (cold_predecessor != UINT32_MAX) {
								phi_inputs_.push_back(
									{input, IRBlockRef{cold_predecessor}});
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
				block_phis.push_back(
					{static_cast<uint32_t>(block), result});
				phi_values_[static_cast<uint32_t>(result)] = 1;
				uint32_t predecessors = plan_->view->predecessor_count(
					plan_->view->context, record.block_id);
				if (predecessors != instruction.operand_count) {
					valid_ = false;
					continue;
				}
				Slice &input_slice =
					phi_input_slices_[static_cast<uint32_t>(result)];
				input_slice.offset =
					static_cast<uint32_t>(phi_inputs_.size());
				for (uint32_t n = 0; n < predecessors; ++n) {
					zend_mir_block_id predecessor;
					int32_t predecessor_index;
					IRValueRef input = value_ref(zend_tpde_operand_at(
						plan_, &instruction, n));
					if (!plan_->view->predecessor_at(plan_->view->context,
							record.block_id, n, &predecessor)
							|| (predecessor_index = block_index(predecessor)) < 0
							|| input == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
					phi_inputs_.push_back(
						{input, IRBlockRef{static_cast<uint32_t>(predecessor_index)}});
					++input_slice.count;
					const uint32_t cold_predecessor =
						boxed_cond_cold_by_predecessor[
							static_cast<uint32_t>(predecessor_index)];
					if (cold_predecessor != UINT32_MAX) {
						phi_inputs_.push_back(
							{input, IRBlockRef{cold_predecessor}});
						++input_slice.count;
					}
				}
				continue;
			}

			IRValueRef copy_input = INVALID_VALUE_REF;
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& result != INVALID_VALUE_REF
					&& instruction.operand_count == 1) {
				copy_input = value_ref(zend_tpde_operand_at(
					plan_, &instruction, 0));
			}
			bool machine_result = result != INVALID_VALUE_REF
				&& ((zend_mir_scalar_type_is_exact(exact_type(result))
						&& exact_type(result)
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| (machine_kind(result)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& machine_value_is_register_authoritative(
							result)));
			if (machine_result
					&& (record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						|| record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)) {
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
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					? inline_component_scalar_body(instruction,
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
			/* W09 Pi nodes over canonical zvals preserve source SSA topology.
			 * They are registerless only after the same physical-location proof
			 * used for boxed PHIs. */
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				if (machine_result
						&& copy_input != INVALID_VALUE_REF
						&& machine_value_is_register_authoritative(
							copy_input)) {
					/* Emit the two-part machine copy below. */
				} else {
					if ((plan_->value_model_flags
							& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0) {
						continue;
					}
					const zend_mir_storage_id result_storage =
						canonical_storage(result);
					const IRValueRef input = instruction.operand_count == 1
						? value_ref(zend_tpde_operand_at(
							plan_, &instruction, 0))
						: INVALID_VALUE_REF;
					if (result == INVALID_VALUE_REF
							|| input == INVALID_VALUE_REF
							|| representation(input)
								!= ZEND_MIR_REPRESENTATION_ZVAL
							|| !zend_mir_id_is_valid(result_storage)
							|| canonical_storage(input) != result_storage) {
						valid_ = false;
					}
					continue;
				}
			}
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			IRValueRef long_left = INVALID_VALUE_REF;
			IRValueRef long_right = INVALID_VALUE_REF;
			const bool explicit_long_operands =
				record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
				&& machine_result
				&& long_binary_machine_operands(
					instruction, long_left, long_right);
			if (explicit_long_operands) {
				operands_.push_back(long_left);
				operands_.push_back(long_right);
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
					? 1
				: boxed_cond_branch
					? 0
				: record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					|| record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| (record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& instruction.direct_call != nullptr)
					? 0 : instruction.operand_count;
			for (uint32_t n = 0; n < data_operand_count; ++n) {
				IRValueRef operand = value_ref(zend_tpde_operand_at(
					plan_, &instruction, n));
				if (operand == INVALID_VALUE_REF) {
					valid_ = false;
				}
				operands_.push_back(operand);
			}
			if (record.opcode == ZEND_MIR_OPCODE_RETURN
					|| record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE
					|| instruction.source_effect != 0) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					&& (record.effects & ZEND_MIR_EFFECT_MASK(
						ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
				operands_.push_back(
					IRValueRef{EXECUTION_CONTEXT_VALUE});
			}
			if (zend_mir_opcode_is_executable_value(record.opcode)
					&& !boxed_cond_branch
					&& !(record.opcode
							== ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE
						&& instruction.runtime_helper
							== ZEND_NATIVE_HELPER_COUNT)) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (boxed_cond_branch) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
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
							IRValueRef value =
								zend_mir_id_is_valid(argument.value_id)
								? value_ref(argument.value_id)
								: IRValueRef{FRAME_VALUE};
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
							: 6 + machine_result;
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
							IRValueRef{EXECUTION_CONTEXT_VALUE});
					}
					if (inlined_user_body.valid) {
						if (inlined_user_body.checked()) {
							operands_.push_back(
								inlined_user_body.checked_left);
							operands_.push_back(
								inlined_user_body.checked_right);
						} else {
							operands_.push_back(
								inlined_user_body.value);
						}
					}
				} else if (dynamic_direct_call) {
					for (uint32_t n = 0; n < 3; ++n) {
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_VALUE});
					}
				}
			} else if (record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
				/* One direct Zend-runtime boundary returns status and payload. */
				operands_.push_back(IRValueRef{FRAME_VALUE});
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
			const uint32_t inlined_operand_index =
				inlined_user_body.valid
					? operand_count
						- (inlined_user_body.checked() ? 2 : 1)
					: UINT32_MAX;
			const uint32_t materialization_operand_index =
				instruction.materialization_count == 0
					? UINT32_MAX : operand_count;
			if (instruction.materialization_offset
						> plan_->materialization_count
					|| instruction.materialization_count
						> plan_->materialization_count
							- instruction.materialization_offset) {
				valid_ = false;
			} else {
				for (uint32_t n = 0;
						n < instruction.materialization_count; ++n) {
					const zend_tpde_materialization &materialization =
						plan_->materializations[
							instruction.materialization_offset + n];
					if (materialization.value_index >= plan_->value_count) {
						valid_ = false;
						operands_.push_back(INVALID_VALUE_REF);
					} else {
						operands_.push_back(IRValueRef{
							MIR_VALUE_BASE
								+ materialization.value_index});
					}
					++operand_count;
				}
			}
			const uint32_t boxed_cond_cold_block =
				boxed_cond_cold_blocks[i];
			if (boxed_cond_cold_block != UINT32_MAX) {
				const uint32_t semantic_operand_count =
					materialization_operand_index == UINT32_MAX
						? operand_count
						: materialization_operand_index;
				add_node(block_instructions,
					static_cast<uint32_t>(block), InstNode{
						InstKind::BoxedCondGuard, i,
						boxed_cond_cold_block, result, {},
						operand_offset, semantic_operand_count,
						false});
				const uint32_t cold_operand_offset =
					static_cast<uint32_t>(operands_.size());
				for (uint32_t n = 0; n < semantic_operand_count; ++n) {
					operands_.push_back(
						operands_[operand_offset + n]);
				}
				for (uint32_t n = 0;
						n < instruction.materialization_count; ++n) {
					operands_.push_back(
						operands_[operand_offset
							+ materialization_operand_index + n]);
				}
				add_node(block_instructions, boxed_cond_cold_block, InstNode{
					InstKind::BoxedCondCold, i,
					boxed_cond_cold_block, INVALID_VALUE_REF, {},
					cold_operand_offset,
					semantic_operand_count
						+ instruction.materialization_count,
					false,
					ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
					false, {}, false, UINT32_MAX, UINT32_MAX,
					instruction.materialization_count == 0
						? UINT32_MAX : semantic_operand_count,
					instruction.materialization_count});
				continue;
			}
			add_node(block_instructions, static_cast<uint32_t>(block), InstNode{
				executable_kind(instruction, record), i, UINT32_MAX, result, {},
				operand_offset, operand_count, machine_result,
				ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
				false, {}, inlined_user_body.valid,
				inlined_operand_index,
				inlined_user_body.checked_source_opcode,
				materialization_operand_index,
				instruction.materialization_count});
		}
		if (std::find(generator_resume_emitted.begin(),
				generator_resume_emitted.end(), 0)
				!= generator_resume_emitted.end()) {
			valid_ = false;
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
							>= argument_value_indices.size()
						|| argument_value_indices[
							static_cast<uint32_t>(argument_index)] >= 0) {
					valid_ = false;
					continue;
				}
				argument_value_indices[
					static_cast<uint32_t>(argument_index)] =
					static_cast<int32_t>(value_index);
			}
		}
		if (source_landings && plan_->source_ssa != nullptr) {
			for (uint32_t source_block = 0;
					source_block < source_block_next.size(); ++source_block) {
				uint32_t &next_source = source_block_next[source_block];
				if (next_source == UINT32_MAX) {
					continue;
				}
				const zend_basic_block &source =
					plan_->source_ssa->cfg.blocks[source_block];
				while (next_source < source.start + source.len) {
					const uint32_t block =
						source_landing_blocks[next_source];
					if (block != UINT32_MAX) {
						emit_user_opcode_landing(block, next_source);
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
			const IRValueRef value{
				MIR_VALUE_BASE + static_cast<uint32_t>(
					argument_value_indices[argument_index])};
			return std::find(operands_.begin(), operands_.end(), value)
						!= operands_.end()
				|| std::any_of(phi_inputs_.begin(), phi_inputs_.end(),
					[&](const PhiInput &input) {
						return input.value == value;
					});
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
					switch (node.kind) {
						case InstKind::ZvalGuardArguments:
							return argument_guards_.empty();
						case InstKind::ZvalTypeLoad:
						case InstKind::ZvalPayloadLoad:
						case InstKind::ZvalGuardType:
							return !argument_machine_value_used(
								node.argument_index);
						default:
							return false;
					}
				}),
			block_instructions.end());
		for (InstNode &node : nodes_) {
			node.liveness_operands =
				std::span<const IRValueRef>{operands_}.subspan(
				node.operand_offset, node.operand_count);
			const uint32_t semantic_operand_count =
				node.materialization_operand_index == UINT32_MAX
					? node.operand_count
					: node.materialization_operand_index;
			node.operands = node.liveness_operands.first(
				semantic_operand_count);
		}
		flatten_block_items(tpde_block_count, block_instructions,
			instruction_slices_, instructions_);
		flatten_block_items(tpde_block_count, block_phis,
			phi_slices_, phis_);
	}

	bool valid() const { return valid_; }
	uint64_t inlined_user_body_count() const {
		return static_cast<uint64_t>(std::count_if(
			nodes_.begin(), nodes_.end(),
			[](const InstNode &node) { return node.inlined_user_body; }));
	}
	const zend_tpde_plan *plan() const { return plan_; }
	const zend_tpde_plan *component_plan(uint32_t index) const {
		return index < component_plans_.size()
			? component_plans_[index] : nullptr;
	}
	bool component_compiled_variable_used(
			uint32_t component_index, uint32_t variable_index) const {
		const zend_tpde_plan *component = component_plan(component_index);
		if (component == nullptr || component->source_ssa == nullptr
				|| component->source_ssa->vars == nullptr
				|| component->source_ssa->vars_count < 0) {
			return true;
		}
		bool found = false;
		for (int index = 0;
				index < component->source_ssa->vars_count; ++index) {
			const zend_ssa_var &variable =
				component->source_ssa->vars[index];
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
				return true;
			}
		}
		/*
		 * An SSA value that is neither defined nor consumed cannot become
		 * observable and cannot require cleanup.  Omit its canonical zval
		 * from direct-frame initialization and release.
		 */
		return !found;
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
	std::span<const uint32_t> generator_resume_targets() const {
		return {plan_->generator_resume_targets,
			plan_->generator_resume_count};
	}
	std::span<const zend_mir_block_id>
	generator_resume_exception_blocks() const {
		return {plan_->generator_resume_exception_blocks,
			plan_->generator_resume_count};
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
		if (is_boxed_cond_branch(
				mir_instruction(inst), record)) {
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
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_MIR_SCALAR_TYPE_NONE
			: plan_->values[index - MIR_VALUE_BASE].exact_type;
	}
	zend_tpde_machine_value_kind machine_kind(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (const DerivedValue *derived = derived_value(value)) {
			switch (derived->exact_type) {
				case ZEND_MIR_SCALAR_TYPE_I1:
					return ZEND_TPDE_MACHINE_VALUE_BOOL;
				case ZEND_MIR_SCALAR_TYPE_F64:
					return ZEND_TPDE_MACHINE_VALUE_F64;
				default:
					return ZEND_TPDE_MACHINE_VALUE_I64;
			}
		}
		return index < MIR_VALUE_BASE
				|| index - MIR_VALUE_BASE >= plan_->value_count
			? ZEND_TPDE_MACHINE_VALUE_I64
			: plan_->values[index - MIR_VALUE_BASE].machine_kind;
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
		return index < register_values_.size()
			&& register_values_[index] != 0;
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
		return static_cast<uint32_t>(frame_slot_references_.size());
	}
	IRValueRef frame_slot_reference(uint32_t index) const {
		return index < frame_slot_references_.size()
			? frame_slot_references_[index]
			: INVALID_VALUE_REF;
	}
	bool frame_slot_reference(
			IRValueRef value, zend_mir_storage_id *storage_id) const {
		const DerivedValue *derived = derived_value(value);
		if (derived == nullptr
				|| derived->representation
					!= ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
				|| !zend_mir_id_is_valid(derived->storage_id)) {
			return false;
		}
		if (storage_id != nullptr) {
			*storage_id = derived->storage_id;
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
		/* Null has no runtime payload. Treat every exact-null value as the
		 * canonical zero constant, including arguments and call results, while
		 * retaining the instruction that produces its observable call effects. */
		if (plan_->values[index - MIR_VALUE_BASE].exact_type
				== ZEND_MIR_SCALAR_TYPE_NULL) {
			*bits = 0;
			return true;
		}
		if (!plan_->values[index - MIR_VALUE_BASE].constant) {
			return false;
		}
		*bits = plan_->values[index - MIR_VALUE_BASE].constant_bits;
		return true;
	}

	uint32_t func_count() const { return 1; }
	const auto &funcs() const { return functions_; }
	const auto &funcs_to_compile() const { return functions_; }
	std::string_view func_link_name(IRFuncRef) const { return "zend_native_entry"; }
	bool func_extern(IRFuncRef) const { return false; }
	bool func_only_local(IRFuncRef) const { return false; }
	bool func_has_weak_linkage(IRFuncRef) const { return false; }
	bool cur_needs_unwind_info() const { return plan_->may_emit_calls; }
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
			|| frame_slot_reference(value, nullptr);
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
	auto inst_results(IRInstRef inst) const {
		const InstNode &current = node(inst);
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
	static constexpr uint32_t EXECUTION_CONTEXT_VALUE =
		ZendIRAdaptor::EXECUTION_CONTEXT_VALUE;
	static constexpr uint32_t MIR_VALUE_BASE = ZendIRAdaptor::MIR_VALUE_BASE;

private:
	std::vector<std::unique_ptr<ZendIRAdaptor>> members_;
	std::vector<IRFuncRef> functions_;
	std::vector<std::string> link_names_;
	ZendIRAdaptor *active_ = nullptr;
	uint32_t active_index_ = 0;

public:
	explicit ZendComponentIRAdaptor(const zend_tpde_plan *plan)
		: ZendComponentIRAdaptor(
			std::span<const zend_tpde_plan *const>{&plan, 1}) {}

	explicit ZendComponentIRAdaptor(
			std::span<const zend_tpde_plan *const> plans) {
		members_.reserve(plans.size());
		functions_.reserve(plans.size());
		link_names_.reserve(plans.size());
		for (uint32_t index = 0; index < plans.size(); ++index) {
			members_.push_back(
				std::make_unique<ZendIRAdaptor>(plans[index], plans));
			functions_.push_back(IRFuncRef{index});
			link_names_.push_back(index == 0
				? "zend_native_entry"
				: "zend_native_component_" + std::to_string(index));
		}
		if (!members_.empty()) {
			active_ = members_[0].get();
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
	uint32_t current_function_index() const { return active_index_; }
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
	std::span<const uint32_t> generator_resume_targets() const {
		return active_->generator_resume_targets();
	}
	std::span<const zend_mir_block_id>
	generator_resume_exception_blocks() const {
		return active_->generator_resume_exception_blocks();
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
	bool constant(IRValueRef value, uint64_t *bits) const {
		return active_->constant(value, bits);
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
	bool func_only_local(IRFuncRef) const { return false; }
	bool func_has_weak_linkage(IRFuncRef) const { return false; }
	bool cur_needs_unwind_info() const {
		return active_->cur_needs_unwind_info();
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
		if (index >= members_.size()) {
			return false;
		}
		active_index_ = index;
		active_ = members_[index].get();
		return true;
	}
	void reset() {
		for (auto &member : members_) {
			member->reset();
		}
		active_index_ = 0;
		active_ = members_.empty() ? nullptr : members_[0].get();
	}
};

static_assert(::tpde::IRAdaptor<ZendComponentIRAdaptor>);

} // namespace zend::native::tpde
