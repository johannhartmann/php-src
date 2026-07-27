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

	enum class FunctionMode : uint8_t {
		ZendEntry,
		TypedBody,
	};

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
		TypedCallGuard,
		GuardedFast,
		GuardedCold,
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
		uint32_t control_block = UINT32_MAX;
		uint32_t continuation_block = UINT32_MAX;
	};

	struct DerivedValue {
		zend_mir_representation representation;
		zend_mir_scalar_type_mask exact_type;
		zend_mir_storage_id storage_id;
		zend_tpde_machine_value_kind machine_kind;
		zend_mir_ownership_state ownership;
		zend_mir_refcount_state refcount_state;
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
	};

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
	std::span<const uint8_t> typed_body_eligible_;
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
	std::vector<uint8_t> fused_instructions_;
	std::vector<IRValueRef> operands_;
	std::vector<uint8_t> phi_values_;
	std::vector<uint8_t> register_values_;
	std::vector<IRValueRef> typed_body_value_overrides_;
	std::vector<IRValueRef> typed_body_source_ssa_overrides_;
	std::vector<IRValueRef> typed_body_instruction_results_;
	std::vector<uint8_t> frozen_call_argument_value_used_;
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	std::vector<DerivedValue> derived_values_;
	std::vector<IRValueRef> frame_slot_references_;
	std::vector<ArgumentGuard> argument_guards_;
	std::vector<uint32_t> user_opcode_next_landings_;
	std::vector<uint32_t> user_opcode_dispatch_to_sources_;
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

	IRValueRef source_binding_value_ref(
			const zend_tpde_source_value_binding &binding) const {
		if (binding.value_index >= 0
				&& static_cast<uint32_t>(binding.value_index)
					< plan_->value_count) {
			const zend_tpde_value &value =
				plan_->values[
					static_cast<uint32_t>(binding.value_index)];
			if (zend_mir_value_is_original_ssa(value.id)
					&& value.id
						< typed_body_source_ssa_overrides_.size()) {
				const IRValueRef override =
					typed_body_source_ssa_overrides_[value.id];
				if (override != INVALID_VALUE_REF) {
					return override;
				}
			}
			return value_ref(value.id);
		}
		if (binding.definition_instruction_index >= 0
				&& static_cast<uint32_t>(
					binding.definition_instruction_index)
					< typed_body_instruction_results_.size()) {
			return typed_body_instruction_results_[
				static_cast<uint32_t>(
					binding.definition_instruction_index)];
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
		for (uint32_t depth = 0;
				depth <= typed_body_value_overrides_.size(); ++depth) {
			const uint32_t current = static_cast<uint32_t>(value);
			if (current < MIR_VALUE_BASE
					|| current - MIR_VALUE_BASE
						>= typed_body_value_overrides_.size()) {
				return value;
			}
			const IRValueRef override =
				typed_body_value_overrides_[current - MIR_VALUE_BASE];
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
							< typed_body_source_ssa_overrides_.size()) {
					const IRValueRef override =
						typed_body_source_ssa_overrides_[
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
					< typed_body_value_overrides_.size()) {
			const IRValueRef override =
				typed_body_value_overrides_[index - MIR_VALUE_BASE];
			if (override != INVALID_VALUE_REF) {
				return override;
			}
		}
		return value;
	}

	IRValueRef guarded_mutation_value_ref(
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
				&& source_opcode != ZEND_IS_IDENTICAL
				&& source_opcode != ZEND_IS_NOT_IDENTICAL
				&& source_opcode != ZEND_IS_EQUAL
				&& source_opcode != ZEND_IS_NOT_EQUAL
				&& source_opcode != ZEND_IS_SMALLER
				&& source_opcode != ZEND_IS_SMALLER_OR_EQUAL) {
			return false;
		}
		left = source_operand_value_ref(instruction.value_operation.op1);
		right = source_operand_value_ref(instruction.value_operation.op2);
		auto register_or_constant = [&](IRValueRef value) {
			if (machine_value_is_register_authoritative(value)) {
				return true;
			}
			if (const DerivedValue *derived = derived_value(value)) {
				return derived->constant;
			}
			const uint32_t index = static_cast<uint32_t>(value);
			return index >= MIR_VALUE_BASE
				&& index - MIR_VALUE_BASE < plan_->value_count
				&& plan_->values[index - MIR_VALUE_BASE].constant;
		};
		return left != INVALID_VALUE_REF
			&& right != INVALID_VALUE_REF
			&& exact_type(left) == ZEND_MIR_SCALAR_TYPE_I64
			&& exact_type(right) == ZEND_MIR_SCALAR_TYPE_I64
			&& register_or_constant(left)
			&& register_or_constant(right);
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
				ZEND_MIR_REFCOUNT_UNKNOWN) {
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
			refcount_state, constant, constant_bits});
		const uint32_t required_value_count =
			static_cast<uint32_t>(value) + 1;
		if (phi_input_slices_.size() < required_value_count) {
			phi_input_slices_.resize(required_value_count);
			phi_values_.resize(required_value_count);
		}
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
			const zend_mir_instruction_record &record,
			IRValueRef *condition_out = nullptr) const {
		if (function_mode_ != FunctionMode::TypedBody
				|| !instruction.has_value_operation
				|| (record.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					&& record.opcode != ZEND_MIR_OPCODE_COND_BRANCH)
				|| instruction.value_operation.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				|| (instruction.value_operation.source_opcode != ZEND_JMPZ
					&& instruction.value_operation.source_opcode
						!= ZEND_JMPNZ)) {
			return false;
		}
		const IRValueRef condition = source_operand_value_ref(
			instruction.value_operation.op1);
		if (condition == INVALID_VALUE_REF) {
			return condition_out == nullptr
				&& instruction.value_operation.op1.ssa_variable_id
					!= ZEND_MIR_ID_INVALID;
		}
		if (exact_type(condition) != ZEND_MIR_SCALAR_TYPE_I1) {
			return false;
		}
		if (condition_out != nullptr) {
			*condition_out = condition;
		}
		return true;
	}

	bool is_boxed_cond_branch(
			const zend_tpde_instruction &instruction,
			const zend_mir_instruction_record &record) const {
		if (!instruction.has_value_operation) {
			return false;
		}
		if (is_register_cond_branch(instruction, record)) {
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

	bool machine_use_requires_tpde_value(
			const zend_tpde_machine_use &use) const {
		if (use.instruction_index >= plan_->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &instruction =
			plan_->instructions[use.instruction_index];
		const zend_mir_instruction_record record =
			instruction_record_at(use.instruction_index);
		switch (use.kind) {
			case ZEND_TPDE_MACHINE_USE_STATEPOINT_MATERIALIZATION:
			case ZEND_TPDE_MACHINE_USE_SUSPEND_LIVE:
			case ZEND_TPDE_MACHINE_USE_LOCAL_ABI_ARGUMENT:
				return true;
			case ZEND_TPDE_MACHINE_USE_CALL_ARGUMENT:
				return record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& instruction.direct_call != nullptr
					&& (instruction.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
			case ZEND_TPDE_MACHINE_USE_PHI_EDGE:
				return machine_value_has_result_representation(
					value_ref(record.result_id));
			case ZEND_TPDE_MACHINE_USE_INSTRUCTION_OPERAND:
				break;
		}

		const IRValueRef result = value_ref(record.result_id);
		const bool machine_result =
			machine_value_has_result_representation(result);
		if (record.opcode == ZEND_MIR_OPCODE_COPY
				&& record.representation
					== ZEND_MIR_REPRESENTATION_ZVAL) {
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
		if (record.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			/*
			 * The outer Zend ABI is now a materialization boundary, not a
			 * reason to suppress the producer's machine value.  Keep the
			 * returned payload live through that boundary.
			 */
			return use.operand_index == 0;
		}
		if (is_boxed_cond_branch(instruction, record)
				|| record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				|| record.opcode == ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
				|| (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
					&& instruction.direct_call != nullptr)) {
			return false;
		}
		return use.operand_index < instruction.operand_count;
	}

	bool machine_value_has_frozen_use(uint32_t value_index) const {
		if (value_index >= plan_->value_count) {
			return false;
		}
		if (value_index < frozen_call_argument_value_used_.size()
				&& frozen_call_argument_value_used_[value_index] != 0) {
			return true;
		}
		if (plan_->value_consumer_offsets == nullptr) {
			return false;
		}
		const uint32_t begin =
			plan_->value_consumer_offsets[value_index];
		const uint32_t end =
			plan_->value_consumer_offsets[value_index + 1];
		for (uint32_t use = begin; use < end; ++use) {
			if (machine_use_requires_tpde_value(
					plan_->value_consumers[use])) {
				return true;
			}
		}
		return false;
	}

	bool needs_explicit_cold_path(
			const zend_tpde_instruction &instruction,
			const zend_mir_instruction_record &record) {
		if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
			return instruction.runtime_helper
				== ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW;
		}
		if (record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
			if (instruction.component_target_index
						< typed_body_eligible_.size()
					&& typed_body_eligible_[
						instruction.component_target_index] != 0) {
				return function_mode_ == FunctionMode::ZendEntry;
			}
			return instruction.direct_call != nullptr
				&& (instruction.direct_call->flags
					& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
		}
		if (!instruction.has_value_operation
				|| executable_kind(instruction, record)
					== InstKind::SlowPathCall) {
			return false;
		}
		const zend_mir_executable_value_ref &operation =
			instruction.value_operation;
		switch (operation.opcode) {
			case ZEND_MIR_OPCODE_VALUE_ASSIGN:
			case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			case ZEND_MIR_OPCODE_VALUE_FREE:
			case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
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
				return true;
			default:
				return false;
		}
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
		if (value.argument_index >= 0
				&& plan->argument_abi != nullptr
				&& static_cast<uint32_t>(value.argument_index)
					< plan->argument_count) {
			return typed_body_plan_abi(
				plan->argument_abi[
					static_cast<uint32_t>(value.argument_index)]);
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
		return {
			exact_scalar
				? value.exact_type == ZEND_MIR_SCALAR_TYPE_I1
					? ZEND_MIR_REPRESENTATION_I1
				: value.exact_type == ZEND_MIR_SCALAR_TYPE_F64
					? ZEND_MIR_REPRESENTATION_DOUBLE
					: ZEND_MIR_REPRESENTATION_I64
				: value.representation,
			value.exact_type,
			abi_kind,
			true,
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
					: ZEND_TPDE_LOCAL_ABI_TRANSFER_BORROWED,
		};
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
						if (!caller_abi.same_shape(callee_abi)
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
				 * The typed body ABI is selected only after every return has
				 * been proven below to match the frozen declared return ABI.
				 * The Zend entry retains its ordinary runtime verification;
				 * the private body therefore does not need to materialize a
				 * zval merely to repeat that check.
				 */
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
				if (returned == INVALID_VALUE_REF
						&& returned_binding
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
				if (returned == INVALID_VALUE_REF
						&& !returned_source_type.valid
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
				returned == INVALID_VALUE_REF
					? returned_source_type
					: typed_body_value_abi(
						plan, returned_index - MIR_VALUE_BASE);
			if (returned != INVALID_VALUE_REF
					&& !returned_type.valid) {
				returned_type = call_result_types[
					returned_index - MIR_VALUE_BASE];
			}
			if (!returned_type.valid
					|| saw_return
						&& !result_type.same_shape(returned_type)) {
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
			FunctionMode::ZendEntry, {}) {}

	explicit ZendIRAdaptor(const zend_tpde_plan *plan,
			std::span<const zend_tpde_plan *const> component_plans,
			FunctionMode function_mode = FunctionMode::ZendEntry,
			std::span<const uint8_t> typed_body_eligible = {})
		: plan_(plan), component_plans_(component_plans),
		  function_mode_(function_mode),
		  typed_body_eligible_(typed_body_eligible) {
		typed_body_value_overrides_.assign(
			plan_ == nullptr ? 0 : plan_->value_count,
			INVALID_VALUE_REF);
		typed_body_source_ssa_overrides_.assign(
			plan_ == nullptr ? 0 : plan_->source_ssa_variable_count,
			INVALID_VALUE_REF);
		typed_body_instruction_results_.assign(
			plan_ == nullptr ? 0 : plan_->instruction_count,
			INVALID_VALUE_REF);
		frozen_call_argument_value_used_.assign(
			plan_ == nullptr ? 0 : plan_->value_count, 0);
		if (plan_ != nullptr
				&& plan_->call_argument_bindings != nullptr) {
			for (uint32_t argument = 0;
					argument < plan_->call_argument_count; ++argument) {
				const int32_t value_index =
					plan_->call_argument_bindings[argument].value_index;
				if (value_index >= 0
						&& static_cast<uint32_t>(value_index)
							< frozen_call_argument_value_used_.size()) {
					frozen_call_argument_value_used_[
						static_cast<uint32_t>(value_index)] = 1;
				}
			}
		}
		if (function_mode_ == FunctionMode::ZendEntry) {
			arguments_.push_back(IRValueRef{EXECUTE_DATA_VALUE});
			arguments_.push_back(
				IRValueRef{EXECUTION_CONTEXT_ARGUMENT});
		} else {
			TypedBodyAbiType return_type;
			if (!typed_body_signature(plan_, component_plans_,
					typed_body_eligible_, &return_type)) {
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
		std::vector<uint32_t> instruction_blocks(
			plan_->instruction_count, UINT32_MAX);
		std::vector<uint32_t> guarded_cold_blocks(
			plan_->instruction_count, UINT32_MAX);
		std::vector<uint32_t> guarded_hot_blocks(
			plan_->instruction_count, UINT32_MAX);
		std::vector<uint32_t> guarded_continuation_blocks(
			plan_->instruction_count, UINT32_MAX);
		std::vector<uint32_t> final_blocks(plan_->block_count);
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
			const TypedBodyAbiType argument_abi =
				value.argument_index >= 0
					? typed_body_value_abi(plan_, i)
					: TypedBodyAbiType{};
			register_values_[MIR_VALUE_BASE + i] =
				!value.constant
				&& value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL
				&& (argument_abi.valid
					|| value.location
						== ZEND_TPDE_MACHINE_LOCATION_REGISTER)
				&& (argument_abi.valid
					|| (zend_mir_scalar_type_is_exact(value.exact_type)
						&& value.exact_type
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
					|| value.machine_kind
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		}
		/*
		 * Zend SSA gives in-place CV mutations a distinct op1 definition even
		 * when the opcode has no PHP result.  The executable value record
		 * intentionally describes source operands rather than duplicating
		 * that process-local SSA edge.  Publish the definition to TPDE here so
		 * later loop conditions consume the post-mutation value instead of a
		 * register holding the preceding slot version.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const IRValueRef definition =
				guarded_mutation_value_ref(plan_->instructions[i]);
			if (definition == INVALID_VALUE_REF
					|| static_cast<uint32_t>(definition)
						>= register_values_.size()) {
				continue;
			}
			const zend_tpde_machine_value_kind kind =
				machine_kind(definition);
			if ((zend_mir_scalar_type_is_exact(exact_type(definition))
						&& exact_type(definition)
							!= ZEND_MIR_SCALAR_TYPE_NULL)
					|| kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
					|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
					|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
					|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
					|| kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				register_values_[static_cast<uint32_t>(definition)] = 1;
			}
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
		for (uint32_t block = 0; block < plan_->block_count; ++block) {
			final_blocks[block] = block;
		}
		/*
		 * Split every helper-capable fast operation before exposing the CFG to
		 * TPDE.  The original block is the first hot segment; every guarded
		 * operation gets an out-of-line cold block and a continuation block.
		 * This makes calls and their clobbers ordinary allocator-visible CFG
		 * state instead of target-local branches with saved assignments.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const int32_t source_block = block_index(record.block_id);
			if (source_block < 0) {
				valid_ = false;
				continue;
			}
			const uint32_t source = static_cast<uint32_t>(source_block);
			instruction_blocks[i] = final_blocks[source];
			if (!needs_explicit_cold_path(instruction, record)) {
				continue;
			}
			const bool typed_component_guard =
				function_mode_ == FunctionMode::ZendEntry
				&& record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.component_target_index
					< typed_body_eligible_.size()
				&& typed_body_eligible_[
					instruction.component_target_index] != 0;
			if (typed_component_guard) {
				guarded_hot_blocks[i] =
					plan_->block_count + synthetic_block_count++;
			}
			guarded_cold_blocks[i] =
				plan_->block_count + synthetic_block_count++;
			guarded_continuation_blocks[i] =
				plan_->block_count + synthetic_block_count++;
			final_blocks[source] = guarded_continuation_blocks[i];
		}
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
			const uint32_t predecessor = instruction_blocks[i];
			if (predecessor == UINT32_MAX
					|| predecessor >= plan_->block_count
						+ synthetic_block_count) {
				valid_ = false;
				continue;
			}
			const uint32_t cold_block =
				plan_->block_count + synthetic_block_count++;
			boxed_cond_cold_blocks[i] = cold_block;
			const int32_t source_predecessor =
				block_index(record.block_id);
			if (source_predecessor < 0) {
				valid_ = false;
			} else {
				boxed_cond_cold_by_predecessor[
					static_cast<uint32_t>(source_predecessor)] =
						cold_block;
			}
		}
		const uint32_t tpde_block_count =
			plan_->block_count + synthetic_block_count;
		blocks_.reserve(tpde_block_count);
		block_info_.resize(tpde_block_count);
		block_info2_.resize(tpde_block_count);
		for (uint32_t i = 0; i < plan_->block_count; ++i) {
			blocks_.push_back(IRBlockRef{i});
			const uint32_t begin = plan_->block_successor_offsets[i];
			const uint32_t end = plan_->block_successor_offsets[i + 1];
			for (uint32_t edge = begin; edge < end; ++edge) {
				const uint32_t target_index =
					plan_->block_successors[edge];
				block_successors.push_back({final_blocks[i], IRBlockRef{
					target_index}});
			}
		}
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const uint32_t cold_block = guarded_cold_blocks[i];
			if (cold_block == UINT32_MAX) {
				continue;
			}
			const uint32_t fast_block = instruction_blocks[i];
			const uint32_t hot_block = guarded_hot_blocks[i];
			const uint32_t continuation =
				guarded_continuation_blocks[i];
			if (hot_block != UINT32_MAX) {
				blocks_.push_back(IRBlockRef{hot_block});
			}
			blocks_.push_back(IRBlockRef{cold_block});
			blocks_.push_back(IRBlockRef{continuation});
			block_successors.push_back(
				{fast_block, IRBlockRef{
					hot_block == UINT32_MAX ? continuation : hot_block}});
			block_successors.push_back(
				{fast_block, IRBlockRef{cold_block}});
			if (hot_block != UINT32_MAX) {
				block_successors.push_back(
					{hot_block, IRBlockRef{continuation}});
			}
			block_successors.push_back(
				{cold_block, IRBlockRef{continuation}});
		}
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const uint32_t cold_block = boxed_cond_cold_blocks[i];
			if (cold_block == UINT32_MAX) {
				continue;
			}
			const uint32_t predecessor = instruction_blocks[i];
			block_successors.push_back(
				{predecessor, IRBlockRef{cold_block}});
			blocks_.push_back(IRBlockRef{cold_block});
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const int32_t source_predecessor =
				block_index(record.block_id);
			if (source_predecessor < 0) {
				valid_ = false;
				continue;
			}
			const uint32_t successor_begin =
				plan_->block_successor_offsets[source_predecessor];
			const uint32_t successor_end =
				plan_->block_successor_offsets[source_predecessor + 1];
			const uint32_t successor_count =
				successor_end - successor_begin;
			if (successor_count != 2) {
				valid_ = false;
				continue;
			}
			for (uint32_t edge = successor_begin;
					edge < successor_end; ++edge) {
				const uint32_t target_index =
					plan_->block_successors[edge];
				block_successors.push_back(
					{cold_block, IRBlockRef{
						target_index}});
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
			const uint32_t record_block = instruction_blocks[i];
			if (record_block == UINT32_MAX) {
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
						guarded_cold_blocks[i] == UINT32_MAX
							? record_block : guarded_cold_blocks[i],
						IRBlockRef{static_cast<uint32_t>(exception_block)}});
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_FINALLY_RETURN) {
				finally_return_blocks.push_back(record_block);
			} else if (record.opcode == ZEND_MIR_OPCODE_FINALLY_CALL) {
				const int32_t source_block =
					block_index(record.block_id);
				if (source_block < 0
						|| plan_->block_successor_offsets[source_block + 1]
							- plan_->block_successor_offsets[source_block]
							!= 2) {
					valid_ = false;
					continue;
				}
				const uint32_t continuation_block =
					plan_->block_successors[
						plan_->block_successor_offsets[source_block] + 1];
				finally_targets.push_back(
					IRBlockRef{continuation_block});
			} else if ((record.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
						|| record.opcode == ZEND_MIR_OPCODE_FINALLY_ENTER)
					&& record.block_id != plan_->function.entry_block_id) {
				finally_targets.push_back(
					IRBlockRef{record_block});
			}
		}
		generator_resume_emitted.resize(
			plan_->generator_resume_count, 0);
		const bool source_landings =
			plan_->user_opcode_callbacks || source_call_fragments;
		if (source_landings && plan_->source_opcodes != nullptr) {
			source_landing_emitted.resize(plan_->source_opcode_count, 0);
			source_landing_blocks.resize(
				plan_->source_opcode_count, UINT32_MAX);
			if (plan_->user_opcode_callbacks) {
				user_opcode_next_landings_.resize(
					plan_->source_opcode_count, UINT32_MAX);
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
						plan_->source_opcodes[source_position].opcode)
						!= nullptr) {
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
				is_register_cond_branch(
					instruction, record, &register_condition);
			const bool typed_component_call =
				record.opcode == ZEND_MIR_OPCODE_CALL_DIRECT_USER
				&& instruction.direct_call != nullptr
				&& instruction.component_target_index
					< typed_body_eligible_.size()
				&& typed_body_eligible_[
					instruction.component_target_index] != 0;
			if (typed_component_call) {
				IRValueRef canonical_result =
					source_binding_value_ref(
						instruction.source_result_binding);
				if (canonical_result == INVALID_VALUE_REF) {
					canonical_result = plan_source_operand_value_ref(
						plan_, instruction.direct_call->result_operand);
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
				result = add_derived_value(
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
				typed_body_instruction_results_[i] = result;
				if (canonical_result != INVALID_VALUE_REF
						&& canonical_index >= MIR_VALUE_BASE
						&& canonical_index - MIR_VALUE_BASE
							< typed_body_value_overrides_.size()) {
					typed_body_value_overrides_[
						canonical_index - MIR_VALUE_BASE] = result;
				} else if (result_ssa
						< typed_body_source_ssa_overrides_.size()) {
					typed_body_source_ssa_overrides_[
						result_ssa] = result;
				} else {
					valid_ = false;
				}
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode
						== ZEND_MIR_OPCODE_VALUE_BINARY_OP
					&& instruction.has_value_operation
					&& !machine_value_has_result_representation(result)) {
				IRValueRef left = INVALID_VALUE_REF;
				IRValueRef right = INVALID_VALUE_REF;
				if (long_binary_machine_operands(
						instruction, left, right)) {
					const uint32_t source_opcode =
						instruction.value_operation.source_opcode;
					const bool boolean_result =
						source_opcode == ZEND_IS_IDENTICAL
						|| source_opcode == ZEND_IS_NOT_IDENTICAL
						|| source_opcode == ZEND_IS_EQUAL
						|| source_opcode == ZEND_IS_NOT_EQUAL
						|| source_opcode == ZEND_IS_SMALLER
						|| source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
					const bool supported =
						boolean_result
						|| source_opcode == ZEND_ADD
						|| source_opcode == ZEND_SUB
						|| source_opcode == ZEND_BW_OR
						|| source_opcode == ZEND_BW_AND
						|| source_opcode == ZEND_BW_XOR;
					if (supported) {
						const IRValueRef canonical_result =
							source_operand_value_ref(
								instruction.value_operation.result);
						const uint32_t canonical_index =
							static_cast<uint32_t>(canonical_result);
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
						typed_body_instruction_results_[i] = result;
						if (canonical_result != INVALID_VALUE_REF
								&& canonical_index >= MIR_VALUE_BASE
								&& canonical_index - MIR_VALUE_BASE
									< typed_body_value_overrides_.size()) {
							typed_body_value_overrides_[
								canonical_index - MIR_VALUE_BASE] =
									result;
						}
						if (result_ssa
								< typed_body_source_ssa_overrides_.size()) {
							typed_body_source_ssa_overrides_[
								result_ssa] = result;
						} else if (canonical_result
								== INVALID_VALUE_REF) {
							valid_ = false;
						}
					}
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
				if (representation(result) == ZEND_MIR_REPRESENTATION_ZVAL) {
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
							const IRValueRef input = value_ref(
								zend_tpde_operand_at(
									plan_, &instruction, n));
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
					IRValueRef input = value_ref(zend_tpde_operand_at(
						plan_, &instruction, n));
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
			bool machine_result =
				machine_value_has_result_representation(result);
			if (machine_result
					&& (record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						|| record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL)
					&& !typed_component_call) {
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
				: type_check_selection
							!= ScalarTypeCheckSelection::Invalid
						|| register_cond_branch
					? 0
				: boxed_cond_branch
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
			for (uint32_t n = 0; n < data_operand_count; ++n) {
				IRValueRef operand = value_ref(zend_tpde_operand_at(
					plan_, &instruction, n));
				if (operand == INVALID_VALUE_REF) {
					valid_ = false;
				}
				operands_.push_back(operand);
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
				const IRValueRef returned = source_binding_value_ref(
					instruction.source_op1_binding);
				if (function_mode_ == FunctionMode::TypedBody
						&& returned == INVALID_VALUE_REF) {
					valid_ = false;
				}
				if (returned != INVALID_VALUE_REF
						&& machine_value_is_register_authoritative(
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
					|| instruction.source_effect != 0)) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					&& (record.effects & ZEND_MIR_EFFECT_MASK(
						ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
				operands_.push_back(
					IRValueRef{EXECUTION_CONTEXT_VALUE});
			}
			if (function_mode_ == FunctionMode::ZendEntry
					&& zend_mir_opcode_is_executable_value(record.opcode)
					&& !boxed_cond_branch
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
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
				const bool typed_local_call =
					instruction.component_target_index
						< typed_body_eligible_.size()
					&& typed_body_eligible_[
						instruction.component_target_index] != 0;
				if (typed_local_call) {
					for (uint32_t n = 0;
							n < instruction.call_argument_count; ++n) {
						zend_mir_call_argument_ref argument{};
						if (!zend_tpde_call_argument_at(plan_,
								instruction.call_argument_offset + n,
								&argument)) {
							valid_ = false;
							operands_.push_back(INVALID_VALUE_REF);
							continue;
						}
						const IRValueRef value =
							source_binding_value_ref(
								plan_->call_argument_bindings[
									instruction.call_argument_offset + n]);
						if (!machine_value_has_result_representation(value)) {
							valid_ = false;
						}
						operands_.push_back(value);
					}
					if (function_mode_ == FunctionMode::ZendEntry) {
						operands_.push_back(
							IRValueRef{FRAME_VALUE});
						operands_.push_back(
							IRValueRef{FRAME_VALUE});
						for (uint32_t use = 0; use < 3; ++use) {
							operands_.push_back(
								IRValueRef{
									EXECUTION_CONTEXT_VALUE});
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
			const uint32_t guarded_cold_block =
				guarded_cold_blocks[i];
			if (guarded_cold_block != UINT32_MAX) {
				const uint32_t guarded_hot_block =
					guarded_hot_blocks[i];
				const uint32_t continuation_block =
					guarded_continuation_blocks[i];
				const IRValueRef fast_result = machine_result
					? add_derived_value(
						representation(result), exact_type(result),
						ZEND_MIR_ID_INVALID, false, 0,
						machine_kind(result))
					: INVALID_VALUE_REF;
				const IRValueRef cold_result = machine_result
					? add_derived_value(
						representation(result), exact_type(result),
						ZEND_MIR_ID_INVALID, false, 0,
						machine_kind(result))
					: INVALID_VALUE_REF;
				uint32_t fast_operand_offset = operand_offset;
				uint32_t fast_operand_count = operand_count;
				uint32_t fast_materialization_operand_index =
					materialization_operand_index;
				uint32_t fast_materialization_count =
					instruction.materialization_count;
				uint32_t fast_block = block;
				if (guarded_hot_block != UINT32_MAX) {
					const uint32_t context_operand =
						instruction.call_argument_count + 2;
					const IRValueRef guard_context =
						context_operand < operand_count
							? operands_[operand_offset + context_operand]
							: INVALID_VALUE_REF;
					if (guard_context == INVALID_VALUE_REF) {
						valid_ = false;
					}
					const uint32_t guard_operand_offset =
						static_cast<uint32_t>(operands_.size());
					operands_.push_back(guard_context);
					for (uint32_t n = 0;
							n < instruction.materialization_count; ++n) {
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
					InstNode guard{
						InstKind::TypedCallGuard, i,
						guarded_cold_block, INVALID_VALUE_REF, {},
						guard_operand_offset,
						1 + instruction.materialization_count,
						false,
						ZEND_MIR_ID_INVALID,
						ZEND_MIR_SCALAR_TYPE_NONE,
						false, {}, false, UINT32_MAX, UINT32_MAX,
						instruction.materialization_count == 0
							? UINT32_MAX : 1,
						instruction.materialization_count};
					guard.control_block = block;
					guard.continuation_block = guarded_hot_block;
					add_node(block_instructions, block, std::move(guard));

					fast_operand_offset =
						static_cast<uint32_t>(operands_.size());
					for (uint32_t n = 0;
							n < instruction.call_argument_count; ++n) {
						operands_.push_back(
							operands_[operand_offset + n]);
					}
					fast_operand_count =
						instruction.call_argument_count;
					fast_materialization_operand_index = UINT32_MAX;
					fast_materialization_count = 0;
					fast_block = guarded_hot_block;
				}
				InstNode fast{
					InstKind::GuardedFast, i, guarded_cold_block,
					fast_result, {}, fast_operand_offset,
					fast_operand_count, machine_result,
					ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
					false, {}, false, UINT32_MAX, UINT32_MAX,
					fast_materialization_operand_index,
					fast_materialization_count};
				fast.control_block = fast_block;
				fast.continuation_block = continuation_block;
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
					operands_.push_back(IRValueRef{FRAME_VALUE});
					operands_.push_back(IRValueRef{FRAME_VALUE});
					operands_.push_back(
						IRValueRef{EXECUTION_CONTEXT_VALUE});
					operands_.push_back(
						IRValueRef{EXECUTION_CONTEXT_VALUE});
					operands_.push_back(
						IRValueRef{EXECUTION_CONTEXT_VALUE});
					cold_operand_count =
						5 + instruction.materialization_count;
					cold_materialization_operand_index =
						instruction.materialization_count == 0
							? UINT32_MAX : 5;
					for (uint32_t n = 0;
							n < instruction.materialization_count; ++n) {
						operands_.push_back(
							operands_[operand_offset
								+ materialization_operand_index + n]);
					}
				} else {
					for (uint32_t n = 0; n < operand_count; ++n) {
						operands_.push_back(
							operands_[operand_offset + n]);
					}
				}
				InstNode cold{
					InstKind::GuardedCold, i, guarded_cold_block,
					cold_result, {}, cold_operand_offset,
					cold_operand_count, cold_result != INVALID_VALUE_REF,
					ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
					false, {}, false, UINT32_MAX, UINT32_MAX,
					cold_materialization_operand_index,
					instruction.materialization_count};
				cold.control_block = guarded_cold_block;
				cold.continuation_block = continuation_block;
				add_node(block_instructions, guarded_cold_block,
					std::move(cold));

				const IRValueRef mutation_result =
					guarded_mutation_value_ref(instruction);
				if (mutation_result != INVALID_VALUE_REF) {
					const zend_mir_storage_id mutation_storage =
						canonical_storage(mutation_result);
					const IRValueRef mutation_slot =
						zend_mir_id_is_valid(mutation_storage)
							? add_derived_value(
								ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER,
								ZEND_MIR_SCALAR_TYPE_NONE,
								mutation_storage)
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
				if (machine_result) {
					if (fast_result == INVALID_VALUE_REF
							|| cold_result == INVALID_VALUE_REF
							|| static_cast<uint32_t>(result)
								>= phi_input_slices_.size()
							|| phi_values_[
								static_cast<uint32_t>(result)] != 0) {
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
					phi_input_slices_[static_cast<uint32_t>(result)] = {
						phi_input_offset, 2};
					phi_values_[static_cast<uint32_t>(result)] = 1;
					block_phis.push_back(
						{continuation_block, result});
				}
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
						operand_offset, semantic_operand_count,
						false});
				nodes_.back().control_block = block;
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
				nodes_.back().control_block = boxed_cond_cold_block;
				continue;
			}
			add_node(block_instructions, static_cast<uint32_t>(block), InstNode{
				type_check_selection
							!= ScalarTypeCheckSelection::Invalid
						|| register_cond_branch
					? InstKind::MIR
					: executable_kind(instruction, record),
				i, UINT32_MAX, result, {},
				operand_offset, operand_count, machine_result,
				ZEND_MIR_ID_INVALID, ZEND_MIR_SCALAR_TYPE_NONE,
				false, {}, inlined_user_body.valid,
				inlined_operand_index,
				inlined_user_body.checked_source_opcode,
				materialization_operand_index,
				instruction.materialization_count});
			if (type_check_selection
					!= ScalarTypeCheckSelection::Invalid
					|| register_cond_branch) {
				InstNode &selected = nodes_.back();
				selected.synthetic = true;
				selected.synthetic_record = record;
				selected.synthetic_record.opcode = register_cond_branch
					? ZEND_MIR_OPCODE_COND_BRANCH
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
		if (zend_mir_scalar_type_is_exact(exact_type(value))
				&& exact_type(value) != ZEND_MIR_SCALAR_TYPE_NULL) {
			return true;
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
	std::vector<ZendIRAdaptor *> function_views_;
	std::vector<uint32_t> function_component_indices_;
	std::vector<uint8_t> typed_body_eligible_;
	std::vector<uint32_t> typed_body_function_indices_;
	std::vector<ZendIRAdaptor::TypedBodyAbiType> typed_body_return_types_;
	std::vector<IRFuncRef> functions_;
	std::vector<std::string> link_names_;
	ZendIRAdaptor *active_ = nullptr;
	uint32_t active_index_ = 0;
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
		function_component_indices_.reserve(plans.size() * 2);
		typed_body_eligible_.assign(plans.size(), 1);
		typed_body_function_indices_.assign(plans.size(), UINT32_MAX);
		typed_body_return_types_.resize(plans.size());
		functions_.reserve(plans.size() * 2);
		link_names_.reserve(plans.size() * 2);
		bool eligibility_changed;
		do {
			eligibility_changed = false;
			for (uint32_t index = 0; index < plans.size(); ++index) {
				if (typed_body_eligible_[index] == 0) {
					continue;
				}
				ZendIRAdaptor::TypedBodyAbiType return_type;
				if (!ZendIRAdaptor::typed_body_signature(
						plans[index], plans, typed_body_eligible_,
						&return_type)) {
					typed_body_eligible_[index] = 0;
					eligibility_changed = true;
				}
			}
		} while (eligibility_changed);
		for (uint32_t index = 0; index < plans.size(); ++index) {
			if (typed_body_eligible_[index] == 0
					|| !ZendIRAdaptor::typed_body_signature(
						plans[index], plans, typed_body_eligible_,
						&typed_body_return_types_[index])) {
				typed_body_eligible_[index] = 0;
				typed_body_return_types_[index] = {};
			}
		}
		for (uint32_t index = 0; index < plans.size(); ++index) {
			members_.push_back(
				std::make_unique<ZendIRAdaptor>(plans[index], plans,
					ZendIRAdaptor::FunctionMode::ZendEntry,
					typed_body_eligible_));
			functions_.push_back(IRFuncRef{index});
			function_views_.push_back(members_.back().get());
			function_component_indices_.push_back(index);
			link_names_.push_back(index == 0
				? "zend_native_entry"
				: "zend_native_component_" + std::to_string(index));
		}
		for (uint32_t index = 0; index < plans.size(); ++index) {
			if (typed_body_eligible_[index] == 0) {
				continue;
			}
			const uint32_t function_index =
				static_cast<uint32_t>(functions_.size());
			members_.push_back(
				std::make_unique<ZendIRAdaptor>(plans[index], plans,
					ZendIRAdaptor::FunctionMode::TypedBody,
					typed_body_eligible_));
			functions_.push_back(IRFuncRef{function_index});
			function_views_.push_back(members_.back().get());
			function_component_indices_.push_back(index);
			typed_body_function_indices_[index] = function_index;
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
		return function_component_indices_[active_index_];
	}
	uint32_t typed_body_function_index(uint32_t component_index) const {
		return component_index < typed_body_function_indices_.size()
			? typed_body_function_indices_[component_index] : UINT32_MAX;
	}
	ZendIRAdaptor::TypedBodyAbiType typed_body_return_type(
			uint32_t component_index) const {
		return component_index < typed_body_return_types_.size()
			? typed_body_return_types_[component_index]
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
	bool func_only_local(IRFuncRef function) const {
		const uint32_t index = static_cast<uint32_t>(function);
		return index < function_views_.size()
			&& function_views_[index]->typed_body();
	}
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
		if (index >= function_views_.size()) {
			return false;
		}
		active_index_ = index;
		active_ = function_views_[index];
		return true;
	}
	void reset() {
		for (auto &member : members_) {
			member->reset();
		}
		active_index_ = 0;
		active_ = function_views_.empty() ? nullptr : function_views_[0];
	}
};

static_assert(::tpde::IRAdaptor<ZendComponentIRAdaptor>);

} // namespace zend::native::tpde
