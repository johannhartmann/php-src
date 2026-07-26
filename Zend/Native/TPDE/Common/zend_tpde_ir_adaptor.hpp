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
#include <span>
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
		FrameSlotAddress,
		ZvalTypeLoad,
		ZvalPayloadLoad,
		ZvalCopy,
		ZvalMove,
		ZvalStore,
		ZvalReleaseFast,
		ZvalGuardArguments,
		ZvalGuardType,
		SlowPathCall,
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
	};

	struct DerivedValue {
		zend_mir_representation representation;
		zend_mir_scalar_type_mask exact_type;
		zend_mir_storage_id storage_id;
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

	class ValueBitSet {
		std::vector<uint64_t> words_;

	public:
		explicit ValueBitSet(uint32_t value_count = 0)
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
				const ValueBitSet &other, const ValueBitSet &excluded) {
			for (size_t word = 0; word < words_.size(); ++word) {
				words_[word] |= other.words_[word] & ~excluded.words_[word];
			}
		}
		void assign_use_and_out_without_def(
				const ValueBitSet &use, const ValueBitSet &out,
				const ValueBitSet &def) {
			for (size_t word = 0; word < words_.size(); ++word) {
				words_[word] =
					use.words_[word] | (out.words_[word] & ~def.words_[word]);
			}
		}
		bool operator==(const ValueBitSet &) const = default;
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
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	std::vector<DerivedValue> derived_values_;
	std::vector<ArgumentGuard> argument_guards_;
	std::vector<uint32_t> generator_resume_targets_;
	std::vector<uint32_t> generator_resume_landings_;
	std::vector<zend_mir_block_id> generator_resume_exception_blocks_;
	std::vector<uint32_t> user_opcode_next_landings_;
	std::vector<uint32_t> user_opcode_dispatch_to_sources_;
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
			zend_mir_storage_id storage_id) {
		if (derived_values_.size()
				>= UINT32_MAX - MIR_VALUE_BASE - plan_->value_count) {
			valid_ = false;
			return INVALID_VALUE_REF;
		}
		const IRValueRef value{
			MIR_VALUE_BASE + plan_->value_count
				+ static_cast<uint32_t>(derived_values_.size())};
		derived_values_.push_back({representation, exact_type, storage_id});
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

	std::vector<ValueBitSet>
	compute_generator_resume_live_values(
			const std::vector<uint32_t> &landing_instructions) {
		const uint32_t block_count = plan_->block_count;
		const uint32_t value_count = plan_->value_count;
		std::vector<std::vector<uint32_t>> block_instructions(block_count);
		std::vector<ValueBitSet> block_use(
			block_count, ValueBitSet{value_count});
		std::vector<ValueBitSet> block_def(
			block_count, ValueBitSet{value_count});
		std::vector<ValueBitSet> block_phi_def(
			block_count, ValueBitSet{value_count});
		std::vector<ValueBitSet> live_in(
			block_count, ValueBitSet{value_count});
		std::vector<ValueBitSet> live_out(
			block_count, ValueBitSet{value_count});

		auto add_uses = [&](uint32_t instruction_index,
				ValueBitSet &live) {
			const zend_tpde_instruction &instruction =
				plan_->instructions[instruction_index];
			const zend_mir_instruction_record record =
				instruction_record_at(instruction_index);
			if (record.opcode == ZEND_MIR_OPCODE_PHI) {
				return;
			}
			for (uint32_t n = 0; n < instruction.operand_count; ++n) {
				const int32_t index = zend_tpde_value_index(
					plan_, zend_tpde_operand_at(plan_, &instruction, n));
				if (index >= 0) {
					live.set(static_cast<uint32_t>(index));
				}
			}
			for (uint32_t n = 0;
					n < instruction.call_argument_count; ++n) {
				zend_mir_call_argument_ref argument;
				if (zend_tpde_call_argument_at(plan_,
						instruction.call_argument_offset + n, &argument)
						&& zend_mir_id_is_valid(argument.value_id)) {
					const int32_t index =
						zend_tpde_value_index(plan_, argument.value_id);
					if (index >= 0) {
						live.set(static_cast<uint32_t>(index));
					}
				}
			}
		};

		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			const int32_t block = block_index(record.block_id);
			if (block < 0) {
				valid_ = false;
				continue;
			}
			const uint32_t block_number = static_cast<uint32_t>(block);
			block_instructions[block_number].push_back(i);
			ValueBitSet uses{value_count};
			add_uses(i, uses);
			for (uint32_t value = 0; value < value_count; ++value) {
				if (uses.test(value)
						&& !block_def[block_number].test(value)) {
					block_use[block_number].set(value);
				}
			}
			const int32_t result =
				zend_tpde_value_index(plan_, record.result_id);
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
			ValueBitSet next_out{value_count};
			const zend_mir_block_id block_id = plan_->block_ids[block];
			const uint32_t successor_count =
				plan_->view->successor_count(
					plan_->view->context, block_id);
			for (uint32_t n = 0; n < successor_count; ++n) {
				zend_mir_block_id successor_id;
				if (!plan_->view->successor_at(
						plan_->view->context, block_id, n,
						&successor_id)) {
					valid_ = false;
					continue;
				}
				const int32_t successor = block_index(successor_id);
				if (successor < 0) {
					valid_ = false;
					continue;
				}
				next_out.union_without(
					live_in[static_cast<uint32_t>(successor)],
					block_phi_def[static_cast<uint32_t>(successor)]);
				uint32_t predecessor_index = UINT32_MAX;
				const uint32_t predecessor_count =
					plan_->view->predecessor_count(
						plan_->view->context, successor_id);
				for (uint32_t predecessor = 0;
						predecessor < predecessor_count; ++predecessor) {
					zend_mir_block_id predecessor_id;
					if (plan_->view->predecessor_at(
							plan_->view->context, successor_id,
							predecessor, &predecessor_id)
							&& predecessor_id == block_id) {
						predecessor_index = predecessor;
						break;
					}
				}
				if (predecessor_index == UINT32_MAX) {
					valid_ = false;
					continue;
				}
				for (uint32_t instruction_index :
						block_instructions[
							static_cast<uint32_t>(successor)]) {
					const zend_tpde_instruction &instruction =
						plan_->instructions[instruction_index];
					const zend_mir_instruction_record record =
						instruction_record_at(instruction_index);
					if (record.opcode != ZEND_MIR_OPCODE_PHI) {
						continue;
					}
					if (predecessor_index
							>= instruction.operand_count) {
						valid_ = false;
						continue;
					}
					const int32_t input = zend_tpde_value_index(
						plan_, zend_tpde_operand_at(plan_,
							&instruction, predecessor_index));
					if (input >= 0) {
						next_out.set(static_cast<uint32_t>(input));
					}
				}
			}
			ValueBitSet next_in{value_count};
			next_in.assign_use_and_out_without_def(
				block_use[block], next_out, block_def[block]);
			if (!(next_out == live_out[block])
					|| !(next_in == live_in[block])) {
				live_out[block] = std::move(next_out);
				live_in[block] = std::move(next_in);
				const uint32_t predecessor_count =
					plan_->view->predecessor_count(
						plan_->view->context, block_id);
				for (uint32_t predecessor = 0;
						predecessor < predecessor_count; ++predecessor) {
					zend_mir_block_id predecessor_id;
					if (!plan_->view->predecessor_at(
							plan_->view->context, block_id, predecessor,
							&predecessor_id)) {
						valid_ = false;
						continue;
					}
					const int32_t predecessor_block =
						block_index(predecessor_id);
					if (predecessor_block < 0) {
						valid_ = false;
						continue;
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

		std::vector<ValueBitSet> result;
		result.reserve(landing_instructions.size());
		for (uint32_t landing_instruction : landing_instructions) {
			ValueBitSet live{value_count};
			if (landing_instruction == UINT32_MAX) {
				result.push_back(std::move(live));
				continue;
			}
			const zend_mir_instruction_record landing_record =
				instruction_record_at(landing_instruction);
			const int32_t block = block_index(landing_record.block_id);
			if (block < 0) {
				valid_ = false;
				result.push_back(std::move(live));
				continue;
			}
			const uint32_t block_number = static_cast<uint32_t>(block);
			live = live_out[block_number];
			const std::vector<uint32_t> &instructions =
				block_instructions[block_number];
			for (size_t n = instructions.size(); n-- > 0;) {
				const uint32_t instruction_index = instructions[n];
				const zend_mir_instruction_record record =
					instruction_record_at(instruction_index);
				const int32_t definition =
					zend_tpde_value_index(plan_, record.result_id);
				if (definition >= 0) {
					live.reset(static_cast<uint32_t>(definition));
				}
				add_uses(instruction_index, live);
				if (instruction_index == landing_instruction) {
					break;
				}
			}
			for (uint32_t value = 0; value < value_count; ++value) {
				uint64_t bits;
				const IRValueRef ref{MIR_VALUE_BASE + value};
				if (live.test(value)
						&& (!zend_mir_scalar_type_is_exact(exact_type(ref))
							|| exact_type(ref) == ZEND_MIR_SCALAR_TYPE_NULL
							|| !zend_mir_id_is_valid(
								canonical_storage(ref))
							|| constant(ref, &bits))) {
					live.reset(value);
				}
			}
			result.push_back(std::move(live));
		}
		return result;
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

public:
	explicit ZendIRAdaptor(const zend_tpde_plan *plan) : plan_(plan) {
		std::vector<BlockItem<IRBlockRef>> block_successors;
		std::vector<BlockItem<IRInstRef>> block_instructions;
		std::vector<BlockItem<IRValueRef>> block_phis;
		std::vector<uint32_t> finally_return_blocks;
		std::vector<IRBlockRef> finally_targets;
		std::vector<uint8_t> generator_resume_emitted;
		std::vector<uint32_t> generator_resume_landing_instructions;
		std::vector<ValueBitSet> generator_resume_live_values;
		std::vector<uint8_t> source_landing_emitted;
		std::vector<uint32_t> source_landing_blocks;
		std::vector<uint32_t> source_block_next;
		bool source_call_fragments = false;

		blocks_.reserve(plan_->block_count);
		block_successors.reserve(plan_->block_count * 2);
		block_instructions.reserve(plan_->instruction_count + plan_->value_count + 1);
		block_phis.reserve(plan_->value_count);
		phi_input_slices_.resize(MIR_VALUE_BASE + plan_->value_count);
		phi_values_.resize(MIR_VALUE_BASE + plan_->value_count);
		block_info_.resize(plan_->block_count);
		block_info2_.resize(plan_->block_count);
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
		}
		/*
		 * Zend's CFG records FAST_CALL's continuation on the call block while
		 * the executable edge is selected by FAST_RET. Add those dynamic
		 * destinations to TPDE's internal CFG so liveness sees every machine
		 * branch without changing persistent source identity. Collect every
		 * continuation, handler, return block, and exception edge in one MIR
		 * pass; the former return-by-instruction rescans were quadratic.
		 */
		if (plan_->source_op_array != nullptr
				&& (plan_->source_op_array->fn_flags
					& (ZEND_ACC_GENERATOR | ZEND_ACC_HAS_FINALLY_BLOCK))
					== (ZEND_ACC_GENERATOR | ZEND_ACC_HAS_FINALLY_BLOCK)) {
			for (uint32_t i = 0;
					i < plan_->source_op_array->last_try_catch; ++i) {
				const zend_try_catch_element &region =
					plan_->source_op_array->try_catch_array[i];
				if (region.finally_op != 0
						&& region.finally_op < plan_->source_op_array->last
						&& region.finally_end < plan_->source_op_array->last) {
					/*
					 * zend_generator_dtor_storage() resumes a suspended
					 * generator at finally_op when it is force-closed.
					 * Publish that exact source address as another local
					 * machine-code landing beside the ordinary yield
					 * continuations.
					 */
					generator_resume_targets_.push_back(region.finally_op);
				}
			}
		}
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			source_call_fragments =
				source_call_fragments
				|| instruction.user_opcode_call_fragments;
			if (instruction.has_value_operation
					&& (record.opcode == ZEND_MIR_OPCODE_GENERATOR_CREATE
						|| record.opcode == ZEND_MIR_OPCODE_GENERATOR_YIELD
						|| record.opcode
							== ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)) {
				const uint32_t source_position =
					instruction.value_operation.source_position_id;
				if (source_position == UINT32_MAX) {
					valid_ = false;
				} else {
					generator_resume_targets_.push_back(source_position + 1);
				}
			}
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
		std::sort(generator_resume_targets_.begin(),
			generator_resume_targets_.end());
		generator_resume_targets_.erase(
			std::unique(generator_resume_targets_.begin(),
				generator_resume_targets_.end()),
			generator_resume_targets_.end());
		generator_resume_landings_.reserve(
			generator_resume_targets_.size());
		generator_resume_exception_blocks_.reserve(
			generator_resume_targets_.size());
		generator_resume_landing_instructions.reserve(
			generator_resume_targets_.size());
		for (uint32_t target : generator_resume_targets_) {
			uint32_t landing = UINT32_MAX;
			uint32_t landing_instruction = UINT32_MAX;
			zend_mir_block_id exception_block = ZEND_MIR_ID_INVALID;
			for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
				const zend_tpde_instruction &instruction =
					plan_->instructions[i];
				const zend_mir_instruction_record record =
					instruction_record_at(i);
				const uint32_t source_position =
					record.source_position_id;
				if (source_position >= target
						&& (landing == UINT32_MAX
							|| source_position < landing)) {
					landing = source_position;
					landing_instruction = i;
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
						valid_ = false;
					}
					exception_block = instruction.exception_block_id;
				}
			}
			generator_resume_landings_.push_back(landing);
			generator_resume_landing_instructions.push_back(
				landing_instruction);
			generator_resume_exception_blocks_.push_back(exception_block);
			if (landing == UINT32_MAX) {
				valid_ = false;
			}
		}
		generator_resume_live_values =
			compute_generator_resume_live_values(
				generator_resume_landing_instructions);
		generator_resume_emitted.resize(
			generator_resume_targets_.size(), 0);
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
		flatten_unique_successors(plan_->block_count, block_successors,
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
		if (!generator_resume_targets_.empty()) {
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
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
				InstKind::FrameSlotAddress,
				UINT32_MAX,
				static_cast<uint32_t>(value.argument_index),
				payload_address,
				{},
				operand_offset,
				1,
				true,
				storage_id});
			operand_offset = static_cast<uint32_t>(operands_.size());
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
					resume_index < generator_resume_landings_.size();
					++resume_index) {
				const uint32_t expected_position = exact_source_position
					? generator_resume_targets_[resume_index]
					: generator_resume_landings_[resume_index];
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
					if (generator_resume_live_values[
							resume_index].test(value)) {
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
				&& zend_mir_scalar_type_is_exact(exact_type(result))
				&& exact_type(result) != ZEND_MIR_SCALAR_TYPE_NULL;
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
				/*
				 * A boxed PHI is registerless only when every incoming source-SSA
				 * identity names the exact same canonical Zend-frame location as
				 * its result.  This is a physical-location proof, not an assumption
				 * based on representation or source-opline decoding.
				 */
				if (representation(result) == ZEND_MIR_REPRESENTATION_ZVAL) {
					if ((plan_->value_model_flags
							& ZEND_MIR_VALUE_MODEL_CANONICAL_LOCATIONS) == 0) {
						continue;
					}
					const uint32_t predecessors =
						plan_->view->predecessor_count(
							plan_->view->context, record.block_id);
					const zend_mir_storage_id result_storage =
						canonical_storage(result);
					if (predecessors != instruction.operand_count
							|| !zend_mir_id_is_valid(result_storage)) {
						valid_ = false;
						continue;
					}
					for (uint32_t n = 0; n < predecessors; ++n) {
						const IRValueRef input = value_ref(
							zend_tpde_operand_at(plan_, &instruction, n));
						if (input == INVALID_VALUE_REF
								|| representation(input)
									!= ZEND_MIR_REPRESENTATION_ZVAL
								|| canonical_storage(input) != result_storage) {
							valid_ = false;
							break;
						}
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
				}
				continue;
			}

			bool machine_result = result != INVALID_VALUE_REF
				&& zend_mir_scalar_type_is_exact(exact_type(result))
				&& exact_type(result) != ZEND_MIR_SCALAR_TYPE_NULL;
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
				if (machine_result) {
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
				 * and boxed CV zvals in the generated Zend frame. Boxed CVs use
				 * a frame operand because their authoritative representation
				 * remains the canonical caller slot. Keep repeated frame/context
				 * uses explicit so TPDE's reference counts match both generated
				 * paths.
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
									& (ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME
										| ZEND_NATIVE_DIRECT_CALL_INLINE_BOXED_LEAF_BODY))
								!= 0
							? 3
							: 6 + machine_result;
					} else {
						frame_use_count = 2;
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
							& (ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME
								| ZEND_NATIVE_DIRECT_CALL_INLINE_BOXED_LEAF_BODY))
								!= 0
							? (instruction.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INLINE_LEAF_BODY)
									!= 0
								? 5
								: 5 + machine_result
							: 7
						: 3;
					for (uint32_t n = 0; n < context_use_count; ++n) {
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_VALUE});
					}
					if ((instruction.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INLINE_LEAF_BODY) != 0
							&& instruction.direct_call->inline_leaf_operation
								!= ZEND_NATIVE_INLINE_LEAF_VOID
							&& instruction.direct_call->inline_leaf_operation
								!= ZEND_NATIVE_INLINE_LEAF_SCALAR_CONSTANT
							&& instruction.direct_call->inline_leaf_argument
								< instruction.call_argument_count) {
						zend_mir_call_argument_ref inline_argument;
						if (!zend_tpde_call_argument_at(plan_,
								instruction.call_argument_offset
									+ instruction.direct_call
										->inline_leaf_argument,
								&inline_argument)) {
							valid_ = false;
						} else if (zend_mir_id_is_valid(
								inline_argument.value_id)) {
							IRValueRef value =
								value_ref(inline_argument.value_id);
							if (value == INVALID_VALUE_REF) {
								valid_ = false;
							}
							/*
							 * The inline body and the materialized slow path
							 * consume a register-authoritative value
							 * independently. Boxed string arguments remain
							 * frame-backed until their type has been proved.
							 * Keep both machine uses explicit so TPDE's
							 * reference count preserves the value until each
							 * generated path reads it.
							 */
							if (instruction.direct_call
										->inline_leaf_operation
									!= ZEND_NATIVE_INLINE_LEAF_STRING_LENGTH_ARGUMENT
									|| machine_value_is_register_authoritative(
										value)) {
								operands_.push_back(value);
							}
						}
					}
					if ((instruction.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INLINE_LEAF_BODY) != 0
							&& (instruction.direct_call
										->inline_leaf_operation
									== ZEND_NATIVE_INLINE_LEAF_LONG_ADD_ARGUMENT
								|| instruction.direct_call
										->inline_leaf_operation
									== ZEND_NATIVE_INLINE_LEAF_LONG_SUB_ARGUMENT)
							&& instruction.direct_call->inline_leaf_argument2
								< instruction.call_argument_count) {
						zend_mir_call_argument_ref inline_argument;
						if (!zend_tpde_call_argument_at(plan_,
								instruction.call_argument_offset
									+ instruction.direct_call
										->inline_leaf_argument2,
								&inline_argument)) {
							valid_ = false;
						} else if (zend_mir_id_is_valid(
								inline_argument.value_id)) {
							IRValueRef value =
								value_ref(inline_argument.value_id);
							if (value == INVALID_VALUE_REF) {
								valid_ = false;
							}
							operands_.push_back(value);
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
			add_node(block_instructions, static_cast<uint32_t>(block), InstNode{
				executable_kind(instruction, record), i, UINT32_MAX, result, {},
				operand_offset,
				operand_count, machine_result});
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
						case InstKind::FrameSlotAddress:
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
			node.operands = std::span<const IRValueRef>{operands_}.subspan(
				node.operand_offset, node.operand_count);
		}
		flatten_block_items(plan_->block_count, block_instructions,
			instruction_slices_, instructions_);
		flatten_block_items(plan_->block_count, block_phis,
			phi_slices_, phis_);
	}

	bool valid() const { return valid_; }
	const zend_tpde_plan *plan() const { return plan_; }
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
		return plan_->instructions[node(inst).mir_instruction_index];
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
		return generator_resume_targets_;
	}
	std::span<const zend_mir_block_id>
	generator_resume_exception_blocks() const {
		return generator_resume_exception_blocks_;
	}
	zend_mir_instruction_record instruction_record(IRInstRef inst) const {
		const InstNode &instruction_node = node(inst);
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
		const zend_tpde_value &mir_value =
			plan_->values[index - MIR_VALUE_BASE];
		return mir_value.location == ZEND_TPDE_MACHINE_LOCATION_REGISTER
			&& ((zend_mir_scalar_type_is_exact(mir_value.exact_type)
					&& mir_value.exact_type != ZEND_MIR_SCALAR_TYPE_NULL)
				|| mir_value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| mir_value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
				|| mir_value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
				|| mir_value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
				|| mir_value.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
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
	bool constant(IRValueRef value, uint64_t *bits) const {
		uint32_t index = static_cast<uint32_t>(value);
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
		return constant(value, &bits);
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
		return node(inst).operands;
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

} // namespace zend::native::tpde
