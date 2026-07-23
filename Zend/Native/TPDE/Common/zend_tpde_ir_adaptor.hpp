// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

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
	static constexpr uint32_t FRAME_VALUE = 1;
	static constexpr uint32_t MIR_VALUE_BASE = 2;

	enum class InstKind : uint8_t {
		LoadFrame,
		LoadArgument,
		MIR,
	};

	struct InstNode {
		InstKind kind;
		uint32_t mir_instruction_index;
		uint32_t argument_index;
		IRValueRef result;
		std::vector<IRValueRef> operands;
		bool has_result;
	};

	struct PhiInput {
		IRValueRef value;
		IRBlockRef block;
	};

	class PhiRef {
		const ZendIRAdaptor *adaptor_;
		IRValueRef value_;

		const std::vector<PhiInput> &inputs() const {
			return adaptor_->phi_inputs_[static_cast<uint32_t>(value_)];
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
	std::array<IRValueRef, 1> arguments_{IRValueRef{EXECUTE_DATA_VALUE}};
	std::vector<IRValueRef> no_values_;
	std::vector<IRBlockRef> blocks_;
	std::vector<std::vector<IRBlockRef>> successors_;
	std::vector<std::vector<IRInstRef>> instructions_;
	std::vector<std::vector<IRValueRef>> phis_;
	std::vector<std::vector<PhiInput>> phi_inputs_;
	std::vector<InstNode> nodes_;
	std::vector<uint8_t> phi_values_;
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	bool valid_ = true;

	int32_t block_index(zend_mir_block_id id) const {
		for (uint32_t i = 0; i < plan_->block_count; ++i) {
			if (plan_->blocks[i].id == id) {
				return static_cast<int32_t>(i);
			}
		}
		return -1;
	}

	IRValueRef value_ref(zend_mir_value_id id) const {
		int32_t index = zend_tpde_value_index(plan_, id);
		return index < 0 ? INVALID_VALUE_REF
			: IRValueRef{MIR_VALUE_BASE + static_cast<uint32_t>(index)};
	}

	void add_node(uint32_t block, InstNode node) {
		uint32_t index = static_cast<uint32_t>(nodes_.size());
		nodes_.push_back(std::move(node));
		instructions_[block].push_back(IRInstRef{index});
	}

public:
	explicit ZendIRAdaptor(const zend_tpde_plan *plan) : plan_(plan) {
		blocks_.reserve(plan_->block_count);
		successors_.resize(plan_->block_count);
		instructions_.resize(plan_->block_count);
		phis_.resize(plan_->block_count);
		phi_inputs_.resize(MIR_VALUE_BASE + plan_->value_count);
		phi_values_.resize(MIR_VALUE_BASE + plan_->value_count);
		block_info_.resize(plan_->block_count);
		block_info2_.resize(plan_->block_count);
		for (uint32_t i = 0; i < plan_->block_count; ++i) {
			blocks_.push_back(IRBlockRef{i});
			uint32_t count = plan_->view->successor_count(
				plan_->view->context, plan_->blocks[i].id);
			for (uint32_t n = 0; n < count; ++n) {
				zend_mir_block_id target;
				if (!plan_->view->successor_at(plan_->view->context,
						plan_->blocks[i].id, n, &target)) {
					valid_ = false;
					continue;
				}
				int32_t target_index = block_index(target);
				if (target_index < 0) {
					valid_ = false;
					continue;
				}
				successors_[i].push_back(IRBlockRef{
					static_cast<uint32_t>(target_index)});
			}
		}
		/*
		 * Zend's CFG records FAST_CALL's continuation on the call block while
		 * the executable edge is selected by FAST_RET. Add those dynamic
		 * destinations to TPDE's internal CFG so liveness sees every machine
		 * branch without changing persistent source identity.
		 */
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			if (plan_->instructions[i].record.opcode
					!= ZEND_MIR_OPCODE_FINALLY_RETURN) {
				continue;
			}
			int32_t ret_block = block_index(
				plan_->instructions[i].record.block_id);
			if (ret_block < 0) {
				valid_ = false;
				continue;
			}
			for (uint32_t n = 0; n < plan_->instruction_count; ++n) {
				const zend_mir_instruction_record &call =
					plan_->instructions[n].record;
				zend_mir_block_id continuation;
				if (call.opcode != ZEND_MIR_OPCODE_FINALLY_CALL
						|| plan_->view->successor_count(
							plan_->view->context, call.block_id) != 2
						|| !plan_->view->successor_at(
							plan_->view->context, call.block_id, 1,
							&continuation)) {
					continue;
				}
				int32_t continuation_block = block_index(continuation);
				if (continuation_block < 0) {
					valid_ = false;
					continue;
				}
				IRBlockRef continuation_ref{
					static_cast<uint32_t>(continuation_block)};
				auto &ret_successors = successors_[
					static_cast<uint32_t>(ret_block)];
				if (std::find(ret_successors.begin(), ret_successors.end(),
						continuation_ref) == ret_successors.end()) {
					ret_successors.push_back(continuation_ref);
				}
			}
			for (uint32_t n = 0; n < plan_->instruction_count; ++n) {
				const zend_mir_instruction_record &handler =
					plan_->instructions[n].record;
				if (handler.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
						&& handler.opcode != ZEND_MIR_OPCODE_FINALLY_ENTER) {
					continue;
				}
				if (handler.block_id == plan_->function.entry_block_id) {
					continue;
				}
				int32_t handler_block = block_index(handler.block_id);
				if (handler_block < 0) {
					valid_ = false;
					continue;
				}
				IRBlockRef handler_ref{static_cast<uint32_t>(handler_block)};
				auto &ret_successors = successors_[
					static_cast<uint32_t>(ret_block)];
				if (std::find(ret_successors.begin(), ret_successors.end(),
						handler_ref) == ret_successors.end()) {
					ret_successors.push_back(handler_ref);
				}
			}
		}

		int32_t entry = block_index(plan_->function.entry_block_id);
		if (entry < 0) {
			valid_ = false;
			return;
		}
		add_node(static_cast<uint32_t>(entry), InstNode{
			InstKind::LoadFrame,
			UINT32_MAX,
			UINT32_MAX,
			IRValueRef{FRAME_VALUE},
			{IRValueRef{EXECUTE_DATA_VALUE}},
			true});
		for (uint32_t i = 0; i < plan_->value_count; ++i) {
			if (plan_->values[i].argument_index < 0
					|| !zend_mir_scalar_type_is_exact(plan_->values[i].exact_type)
					|| plan_->values[i].exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				continue;
			}
			add_node(static_cast<uint32_t>(entry), InstNode{
				InstKind::LoadArgument,
				UINT32_MAX,
				static_cast<uint32_t>(plan_->values[i].argument_index),
				IRValueRef{MIR_VALUE_BASE + i},
				{IRValueRef{FRAME_VALUE}},
				true});
		}

		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			int32_t block = block_index(instruction.record.block_id);
			if (block < 0) {
				valid_ = false;
				continue;
			}
			if (zend_mir_id_is_valid(instruction.exception_block_id)) {
				int32_t exception_block = block_index(
					instruction.exception_block_id);
				if (exception_block < 0) {
					valid_ = false;
					continue;
				}
				IRBlockRef exception_ref{
					static_cast<uint32_t>(exception_block)};
				auto &block_successors = successors_[
					static_cast<uint32_t>(block)];
				if (std::find(block_successors.begin(), block_successors.end(),
						exception_ref) == block_successors.end()) {
					block_successors.push_back(exception_ref);
				}
			}
			IRValueRef result = value_ref(instruction.record.result_id);
			if (instruction.record.opcode == ZEND_MIR_OPCODE_CONSTANT) {
				continue;
			}
			if (instruction.record.opcode == ZEND_MIR_OPCODE_PHI) {
				if (result == INVALID_VALUE_REF) {
					valid_ = false;
					continue;
				}
				/*
				 * Canonical zval PHIs describe Zend-frame state.  W09 value and
				 * iterator helpers read and update that state by source slot, so
				 * these PHIs must remain in ZNMIR without becoming TPDE register
				 * assignments.  Scalar PHIs still carry machine values and retain
				 * the normal TPDE parallel-copy semantics.
				 */
				if (!zend_mir_scalar_type_is_exact(exact_type(result))
						|| exact_type(result) == ZEND_MIR_SCALAR_TYPE_NULL) {
					continue;
				}
				phis_[static_cast<uint32_t>(block)].push_back(result);
				phi_values_[static_cast<uint32_t>(result)] = 1;
				uint32_t predecessors = plan_->view->predecessor_count(
					plan_->view->context, instruction.record.block_id);
				if (predecessors != instruction.operand_count) {
					valid_ = false;
					continue;
				}
				for (uint32_t n = 0; n < predecessors; ++n) {
					zend_mir_block_id predecessor;
					int32_t predecessor_index;
					IRValueRef input = value_ref(zend_tpde_operand_at(
						plan_, &instruction, n));
					if (!plan_->view->predecessor_at(plan_->view->context,
							instruction.record.block_id, n, &predecessor)
							|| (predecessor_index = block_index(predecessor)) < 0
							|| input == INVALID_VALUE_REF) {
						valid_ = false;
						continue;
					}
					phi_inputs_[static_cast<uint32_t>(result)].push_back(
						{input, IRBlockRef{static_cast<uint32_t>(predecessor_index)}});
				}
				continue;
			}

			std::vector<IRValueRef> operands;
			bool machine_result = result != INVALID_VALUE_REF
				&& zend_mir_scalar_type_is_exact(exact_type(result))
				&& exact_type(result) != ZEND_MIR_SCALAR_TYPE_NULL;
			/* W09 Pi nodes over canonical zvals preserve source SSA topology,
			 * but the authoritative value remains in the Zend frame slot.  They
			 * are not machine copies and must not create a TPDE use-before-def
			 * dependency on another source-only zval identity. */
			if (instruction.record.opcode == ZEND_MIR_OPCODE_COPY
					&& instruction.record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				continue;
			}
			operands.reserve(instruction.operand_count +
				(instruction.record.opcode == ZEND_MIR_OPCODE_RETURN
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL));
			/*
			 * RETURN_SOURCE_ZVAL transfers the canonical zval directly from the
			 * Zend frame, selected by its source opline.  Its MIR value operand
			 * carries dependency/type information only; asking TPDE to allocate a
			 * machine value for it leaves an unconsumed ValueAssignment and, more
			 * importantly, would tempt target code to treat a scalar payload as a
			 * complete zval.  The runtime helper needs only the frame pointer.
			 */
			uint32_t data_operand_count =
				instruction.record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| (instruction.record.opcode
							== ZEND_MIR_OPCODE_CALL_DIRECT_USER
						&& instruction.direct_call != nullptr)
				? 0 : instruction.operand_count;
			for (uint32_t n = 0; n < data_operand_count; ++n) {
				IRValueRef operand = value_ref(zend_tpde_operand_at(
					plan_, &instruction, n));
				if (operand == INVALID_VALUE_REF) {
					valid_ = false;
				}
				operands.push_back(operand);
			}
			if (instruction.record.opcode == ZEND_MIR_OPCODE_RETURN
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
					|| instruction.source_effect != 0) {
				operands.push_back(IRValueRef{FRAME_VALUE});
			}
			if (instruction.record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					&& (instruction.record.effects & ZEND_MIR_EFFECT_MASK(
						ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				operands.push_back(IRValueRef{FRAME_VALUE});
			}
			if (zend_mir_opcode_is_executable_value(
					instruction.record.opcode)) {
				operands.push_back(IRValueRef{FRAME_VALUE});
			}
			if (instruction.record.opcode
					== ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				operands.push_back(IRValueRef{FRAME_VALUE});
			}
			if (instruction.record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
				operands.push_back(IRValueRef{FRAME_VALUE});
			}
			if (instruction.record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
				/*
				 * The descriptor path is one call with explicit source-backed
				 * operands and a two-register status/payload result.  The
				 * compatibility path still exposes one frame use per helper
				 * invocation so TPDE's reference counts match generated code.
				 */
				uint32_t frame_use_count;
				if (instruction.direct_call != nullptr) {
					frame_use_count = 1;
				} else {
					uint32_t setter_count = instruction.operand_count == 0
						? instruction.call_argument_count
						: instruction.operand_count;
					frame_use_count = setter_count + 2 + machine_result;
				}
				for (uint32_t n = 0; n < frame_use_count; ++n) {
					operands.push_back(IRValueRef{FRAME_VALUE});
				}
			} else if (instruction.record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
				/* begin + source setters + finish + optional scalar read */
				for (uint32_t n = 0;
						n < instruction.call_argument_count + 2
							+ machine_result; ++n) {
					operands.push_back(IRValueRef{FRAME_VALUE});
				}
			} else if (instruction.record.opcode
					== ZEND_MIR_OPCODE_CATCH_ENTER
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_FINALLY_ENTER
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_FINALLY_CALL
					|| instruction.record.opcode
						== ZEND_MIR_OPCODE_FINALLY_RETURN) {
				operands.push_back(IRValueRef{FRAME_VALUE});
			}
			add_node(static_cast<uint32_t>(block), InstNode{
				InstKind::MIR, i, UINT32_MAX, result, std::move(operands),
				machine_result});
		}
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
	zend_mir_representation representation(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		return index < MIR_VALUE_BASE ? ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
			: plan_->values[index - MIR_VALUE_BASE].representation;
	}
	zend_mir_scalar_type_mask exact_type(IRValueRef value) const {
		uint32_t index = static_cast<uint32_t>(value);
		return index < MIR_VALUE_BASE ? ZEND_MIR_SCALAR_TYPE_NONE
			: plan_->values[index - MIR_VALUE_BASE].exact_type;
	}
	bool constant(IRValueRef value, uint64_t *bits) const {
		uint32_t index = static_cast<uint32_t>(value);
		if (index < MIR_VALUE_BASE) {
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
		return MIR_VALUE_BASE + plan_->value_count - 1;
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
		return successors_[static_cast<uint32_t>(block)];
	}
	std::span<const IRInstRef> block_insts(IRBlockRef block) const {
		return instructions_[static_cast<uint32_t>(block)];
	}
	std::span<const IRValueRef> block_phis(IRBlockRef block) const {
		return phis_[static_cast<uint32_t>(block)];
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
	const auto &inst_operands(IRInstRef inst) const { return node(inst).operands; }
	auto inst_results(IRInstRef inst) const {
		const InstNode &current = node(inst);
		return std::span<const IRValueRef>{&current.result,
			current.has_result ? size_t{1} : size_t{0}};
	}
	static bool inst_fused(IRInstRef) { return false; }
	std::string_view inst_fmt_ref(IRInstRef) const { return "znmir-inst"; }
	void start_compile() const {}
	void end_compile() const {}
	bool switch_func(IRFuncRef function) {
		return function == IRFuncRef{0};
	}
	void reset() {
		std::fill(block_info_.begin(), block_info_.end(), 0);
		std::fill(block_info2_.begin(), block_info2_.end(), 0);
	}
};

static_assert(::tpde::IRAdaptor<ZendIRAdaptor>);

} // namespace zend::native::tpde
