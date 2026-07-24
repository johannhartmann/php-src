// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

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
		LoadArgument,
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
	std::vector<IRValueRef> operands_;
	std::vector<uint8_t> phi_values_;
	std::vector<uint32_t> block_info_;
	std::vector<uint32_t> block_info2_;
	bool valid_ = true;

	int32_t block_index(zend_mir_block_id id) const {
		return zend_tpde_block_index(plan_, id);
	}

	IRValueRef value_ref(zend_mir_value_id id) const {
		int32_t index = zend_tpde_value_index(plan_, id);
		return index < 0 ? INVALID_VALUE_REF
			: IRValueRef{MIR_VALUE_BASE + static_cast<uint32_t>(index)};
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

public:
	explicit ZendIRAdaptor(const zend_tpde_plan *plan) : plan_(plan) {
		std::vector<BlockItem<IRBlockRef>> block_successors;
		std::vector<BlockItem<IRInstRef>> block_instructions;
		std::vector<BlockItem<IRValueRef>> block_phis;
		std::vector<uint32_t> finally_return_blocks;
		std::vector<IRBlockRef> finally_targets;

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
		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
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
		for (uint32_t i = 0; i < plan_->value_count; ++i) {
			if (plan_->values[i].argument_index < 0
					|| !zend_mir_scalar_type_is_exact(plan_->values[i].exact_type)
					|| plan_->values[i].exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				continue;
			}
			uint32_t operand_offset = static_cast<uint32_t>(operands_.size());
			operands_.push_back(IRValueRef{FRAME_VALUE});
			add_node(block_instructions, static_cast<uint32_t>(entry), InstNode{
				InstKind::LoadArgument,
				UINT32_MAX,
				static_cast<uint32_t>(plan_->values[i].argument_index),
				IRValueRef{MIR_VALUE_BASE + i},
				{},
				operand_offset,
				1,
				true});
		}

		for (uint32_t i = 0; i < plan_->instruction_count; ++i) {
			const zend_tpde_instruction &instruction = plan_->instructions[i];
			const zend_mir_instruction_record record =
				instruction_record_at(i);
			if (!zend_mir_id_is_valid(record.id)) {
				valid_ = false;
				continue;
			}
			int32_t block = block_index(record.block_id);
			if (block < 0) {
				valid_ = false;
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
			/* W09 Pi nodes over canonical zvals preserve source SSA topology,
			 * but the authoritative value remains in the Zend frame slot.  They
			 * are not machine copies and must not create a TPDE use-before-def
			 * dependency on another source-only zval identity. */
			if (record.opcode == ZEND_MIR_OPCODE_COPY
					&& record.representation
						== ZEND_MIR_REPRESENTATION_ZVAL) {
				continue;
			}
			uint32_t operand_offset =
				static_cast<uint32_t>(operands_.size());
			/*
			 * RETURN_SOURCE_ZVAL transfers the canonical zval directly from the
			 * Zend frame, selected by its source opline.  Its MIR value operand
			 * carries dependency/type information only; asking TPDE to allocate a
			 * machine value for it leaves an unconsumed ValueAssignment and, more
			 * importantly, would tempt target code to treat a scalar payload as a
			 * complete zval.  The runtime helper needs only the frame pointer.
			 */
			uint32_t data_operand_count =
				record.opcode == ZEND_MIR_OPCODE_STATEPOINT
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
					|| instruction.source_effect != 0) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (record.opcode == ZEND_MIR_OPCODE_STATEPOINT
					&& (record.effects & ZEND_MIR_EFFECT_MASK(
						ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (zend_mir_opcode_is_executable_value(
					record.opcode)) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			if (record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
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
								zend_mir_scalar_type_is_exact(
									instruction.direct_call
										->arguments[n].exact_type)
								? value_ref(argument.value_id)
								: IRValueRef{FRAME_VALUE};
							if (value == INVALID_VALUE_REF) {
								valid_ = false;
							}
							operands_.push_back(value);
						}
						frame_use_count = 6 + machine_result;
					} else {
						frame_use_count = 2;
					}
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
						? 7 : 3;
					for (uint32_t n = 0; n < context_use_count; ++n) {
						operands_.push_back(
							IRValueRef{EXECUTION_CONTEXT_VALUE});
					}
				}
			} else if (record.opcode
					== ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL) {
				/* begin + source setters + finish + optional scalar read */
				for (uint32_t n = 0;
						n < instruction.call_argument_count + 2
							+ machine_result; ++n) {
					operands_.push_back(IRValueRef{FRAME_VALUE});
				}
			} else if (record.opcode
					== ZEND_MIR_OPCODE_CATCH_ENTER
					|| record.opcode
						== ZEND_MIR_OPCODE_FINALLY_ENTER
					|| record.opcode
						== ZEND_MIR_OPCODE_FINALLY_CALL
					|| record.opcode
						== ZEND_MIR_OPCODE_FINALLY_RETURN) {
				operands_.push_back(IRValueRef{FRAME_VALUE});
			}
			uint32_t operand_count =
				static_cast<uint32_t>(operands_.size()) - operand_offset;
			add_node(block_instructions, static_cast<uint32_t>(block), InstNode{
				InstKind::MIR, i, UINT32_MAX, result, {}, operand_offset,
				operand_count, machine_result});
		}
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
	zend_mir_instruction_record instruction_record(IRInstRef inst) const {
		return instruction_record_at(node(inst).mir_instruction_index);
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
