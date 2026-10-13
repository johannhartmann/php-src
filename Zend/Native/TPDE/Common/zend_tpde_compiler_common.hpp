// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"

#include <tpde/CompilerBase.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zend::native::tpde {

/*
 * The target-independent part of the Zend compilers. A backend passes it to
 * TPDE as the compiler base (BaseTy), so it sits between ::tpde::CompilerBase
 * and the architecture's compiler and implements the TPDE hooks that depend
 * only on the adaptor and the value assignments. The backend (Derived)
 * declares it a friend and supplies the target code it uses:
 *  - JUMP_ALWAYS, JUMP_EQUAL, JUMP_NOT_EQUAL: its jump conditions;
 *  - emit_decision_test(reg): flags for a test of the 32-bit reg against 0;
 *  - patch_direct_call_stack_guard(offset, destination, frame_size).
 */
template <typename Adaptor, typename Derived, typename Config>
struct ZendCompilerCommon : ::tpde::CompilerBase<Adaptor, Derived, Config> {
	using Base = ::tpde::CompilerBase<Adaptor, Derived, Config>;
	using AsmReg = typename Base::AsmReg;
	using ValuePart = typename Base::ValuePart;
	using ValuePartRef = typename Base::ValuePartRef;
	using ScratchReg = typename Base::ScratchReg;

	using Base::Base;

	struct TargetBranchAssignment {
		::tpde::ValLocalIdx local_idx;
		uint32_t part;
	};
	using TargetBranchState = std::vector<TargetBranchAssignment>;

	TargetBranchState spill_target_branch_state() {
		const auto release = this->derived()->spill_before_branch(true);
		TargetBranchState state;

		for (auto reg_id : ::tpde::util::BitSetIterator<>{
				release & this->register_file.used}) {
			const ::tpde::Reg reg{reg_id};
			const auto local_idx = this->register_file.reg_local_idx(reg);
			if (local_idx == Base::INVALID_VAL_LOCAL_IDX
					|| this->register_file.is_fixed(reg)) {
				continue;
			}
			state.push_back(TargetBranchAssignment{
				local_idx, this->register_file.reg_part(reg)});
		}
		return state;
	}

	void reconcile_target_branch_state(const TargetBranchState &state) {
		for (const TargetBranchAssignment &entry : state) {
			::tpde::ValueAssignment *assignment =
				this->val_assignment(entry.local_idx);
			if (assignment == nullptr) {
				continue;
			}
			::tpde::AssignmentPartRef part{assignment, entry.part};
			if (!part.register_valid() || part.fixed_assignment()) {
				continue;
			}
			ZEND_ASSERT(part.stack_valid() || part.variable_ref()
				|| assignment->references_left == 0);
			const ::tpde::Reg reg = part.get_reg();
			part.set_register_valid(false);
			this->register_file.unmark_used(reg);
		}
	}

	struct ValRefSpecial {
		uint8_t mode = 4;
		uint8_t bank = 0;
		uint8_t padding[6]{};
		uint64_t bits = 0;
	};

	struct ValueParts {
		::tpde::RegBank bank;
		zend_tpde_machine_representation_desc representation;
		uint32_t count() const { return representation.part_count; }
		uint32_t size_bytes(uint32_t part) const {
			ZEND_ASSERT(part < representation.part_count);
			return representation.parts[part].bit_width / 8;
		}
		::tpde::RegBank reg_bank(uint32_t part) const {
			ZEND_ASSERT(part < representation.part_count);
			return representation.parts[part].register_bank
					== ZEND_TPDE_MACHINE_REGISTER_FP
				? Config::FP_BANK : Config::GP_BANK;
		}
	};

	/*
	 * Direct calls into a component-local function check the C stack against
	 * the callee's projected low-water mark, SP - frame, instead of SP alone.
	 * The frame is known only once the callee is compiled, so the guard is
	 * emitted with a placeholder the backend patches then; callees compiled
	 * later (forward calls, recursion) are patched when they finish.
	 */
	struct DirectCallStackGuardLabelPatch {
		::tpde::Label label;
		uint32_t target_function;
		AsmReg destination;
	};
	struct DirectCallStackGuardOffsetPatch {
		uint32_t offset;
		uint32_t target_function;
		AsmReg destination;
	};
	std::vector<DirectCallStackGuardLabelPatch>
		current_direct_call_stack_guard_patches_;
	std::vector<DirectCallStackGuardOffsetPatch>
		pending_direct_call_stack_guard_patches_;
	std::vector<uint32_t> function_stack_frame_sizes_;

	/* Records the guard placeholder label locates. */
	void add_direct_call_stack_guard(::tpde::Label label,
			uint32_t target_function, AsmReg destination) {
		current_direct_call_stack_guard_patches_.push_back({
			label, target_function, destination});
	}

	/*
	 * After ::finish_func() of function index, whose machine frame is
	 * frame_size bytes: patches its guards whose callee is compiled, keeps
	 * the others, and patches the kept guards that call index.
	 */
	void resolve_direct_call_stack_guards(uint32_t index, uint32_t frame_size) {
		for (const auto &patch : current_direct_call_stack_guard_patches_) {
			const uint32_t offset = this->text_writer.label_offset(patch.label);
			const uint32_t target_frame_size =
				function_stack_frame_sizes_[patch.target_function];
			if (target_frame_size == UINT32_MAX) {
				pending_direct_call_stack_guard_patches_.push_back({
					offset, patch.target_function, patch.destination});
			} else {
				this->derived()->patch_direct_call_stack_guard(
					offset, patch.destination, target_frame_size);
			}
		}
		current_direct_call_stack_guard_patches_.clear();

		ZEND_ASSERT(index < function_stack_frame_sizes_.size());
		function_stack_frame_sizes_[index] = frame_size;
		for (size_t patch_index = 0;
				patch_index < pending_direct_call_stack_guard_patches_.size();) {
			const auto &patch =
				pending_direct_call_stack_guard_patches_[patch_index];
			if (patch.target_function != index) {
				++patch_index;
				continue;
			}
			this->derived()->patch_direct_call_stack_guard(
				patch.offset, patch.destination, frame_size);
			pending_direct_call_stack_guard_patches_.erase(
				pending_direct_call_stack_guard_patches_.begin()
					+ static_cast<std::ptrdiff_t>(patch_index));
		}
	}

	void reset() {
		Base::reset();
		current_direct_call_stack_guard_patches_.clear();
		pending_direct_call_stack_guard_patches_.clear();
		function_stack_frame_sizes_.clear();
	}

	bool hook_post_func_sym_init() {
		function_stack_frame_sizes_.assign(
			this->func_syms.size(), UINT32_MAX);
		return true;
	}

	AsmReg canonical_value_register(IRValueRef value) {
		const auto local_idx = this->adaptor->val_local_idx(value);
		::tpde::ValueAssignment *assignment = this->val_assignment(local_idx);
		ZEND_ASSERT(assignment != nullptr);
		::tpde::AssignmentPartRef part{assignment, 0};
		if (!part.register_valid()) {
			ZEND_ASSERT(part.stack_valid());
			ValuePartRef reload{this, local_idx, assignment, 0, false};
			const AsmReg reg = reload.load_to_reg();
			reload.reset();
			return reg;
		}
		return AsmReg{part.get_reg().id()};
	}
	AsmReg canonical_frame_register() {
		return canonical_value_register(
			IRValueRef{Adaptor::FRAME_VALUE});
	}

	void generate_exception_branch(IRBlockRef target) {
		auto index = static_cast<uint32_t>(this->analyzer.block_idx(target));
		this->derived()->generate_raw_jump(
			Derived::JUMP_ALWAYS, this->block_labels[index]);
	}
	void generate_guarded_decision_branch(
			ScratchReg &&decision, IRBlockRef nonzero_target,
			IRBlockRef zero_target) {
		Derived *const self = this->derived();
		const IRBlockRef next = this->analyzer.block_ref(self->next_block());
		const bool nonzero_needs_split =
			self->branch_needs_split(nonzero_target);
		const bool zero_needs_split = self->branch_needs_split(zero_target);
		const auto spilled = self->spill_before_branch();

		self->begin_branch_region();
		self->emit_decision_test(decision.cur_reg());
		if (next == nonzero_target
				|| (next != zero_target && nonzero_needs_split)) {
			self->generate_branch_to_block(
				Derived::JUMP_EQUAL, zero_target, zero_needs_split, false);
			self->generate_branch_to_block(
				Derived::JUMP_ALWAYS, nonzero_target, false, true);
		} else if (next == zero_target) {
			self->generate_branch_to_block(
				Derived::JUMP_NOT_EQUAL, nonzero_target,
				nonzero_needs_split, false);
			self->generate_branch_to_block(
				Derived::JUMP_ALWAYS, zero_target, false, true);
		} else {
			ZEND_ASSERT(!nonzero_needs_split);
			self->generate_branch_to_block(
				Derived::JUMP_NOT_EQUAL, nonzero_target, false, false);
			self->generate_branch_to_block(
				Derived::JUMP_ALWAYS, zero_target, false, true);
		}
		self->end_branch_region();
		self->release_spilled_regs(spilled);
	}

	/*
	 * Calls dispatch(handler_block, flagged_position) for each catch or
	 * finally handler an exception enters, with the handler's source
	 * position flagged with ZEND_NATIVE_FINALLY_EXCEPTION_FLAG. With
	 * skip_unplaced, a handler whose block TPDE does not compile is left out.
	 */
	template <typename Dispatch>
	void for_each_exception_handler(bool skip_unplaced, Dispatch &&dispatch) {
		const zend_tpde_plan *plan = this->adaptor->plan();
		for (uint32_t i = 0; i < plan->instruction_count; ++i) {
			const zend_mir_instruction_record &handler =
				plan->instructions[i].record;
			if ((handler.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
					&& handler.opcode != ZEND_MIR_OPCODE_FINALLY_ENTER)
					|| handler.block_id == plan->function.entry_block_id
					|| !zend_mir_id_is_valid(handler.source_position_id)) {
				continue;
			}
			const IRBlockRef handler_block =
				this->adaptor->block_ref(handler.block_id);
			if (skip_unplaced
					&& static_cast<uint32_t>(
						this->analyzer.block_idx(handler_block))
						>= this->block_labels.size()) {
				continue;
			}
			dispatch(handler_block,
				ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
					| handler.source_position_id);
		}
	}

	/*
	 * Calls dispatch(source_position, continuation) for each FINALLY_CALL
	 * with a continuation block, which a FINALLY_RETURN reached from the
	 * call at source_position continues at.
	 */
	template <typename Dispatch>
	void for_each_finally_continuation(Dispatch &&dispatch) {
		const zend_tpde_plan *plan = this->adaptor->plan();
		for (uint32_t i = 0; i < plan->instruction_count; ++i) {
			const zend_mir_instruction_record &call =
				plan->instructions[i].record;
			zend_mir_block_id target;
			if (call.opcode != ZEND_MIR_OPCODE_FINALLY_CALL
					|| zend_tpde_block_successor_count(
						plan, call.block_id) != 2
					|| !zend_tpde_block_successor_at(
						plan, call.block_id, 1, &target)) {
				continue;
			}
			dispatch(call.source_position_id, this->adaptor->block_ref(target));
		}
	}

	bool try_force_fixed_assignment(IRValueRef value) const {
		const zend_tpde_machine_value_kind kind =
			this->adaptor->machine_kind(value);
		const bool pointer_value =
			kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
		return value == IRValueRef{Adaptor::FRAME_VALUE}
			|| (!this->adaptor->typed_body()
				&& value == IRValueRef{
					Adaptor::EXECUTION_CONTEXT_ARGUMENT})
			|| (pointer_value
				&& this->adaptor->machine_value_is_register_authoritative(value))
			|| (this->adaptor->val_is_phi(value)
				&& kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& this->adaptor->machine_value_is_register_authoritative(value));
	}
	ValueParts val_parts(IRValueRef value) const {
		const zend_tpde_machine_value_kind kind =
			this->adaptor->machine_kind(value);
		const zend_tpde_machine_representation_desc representation =
			zend_tpde_machine_representation(
				kind, this->adaptor->machine_value_is_register_authoritative(value));
		return {
			representation.parts[0].register_bank
					== ZEND_TPDE_MACHINE_REGISTER_FP
				? Config::FP_BANK : Config::GP_BANK,
			representation};
	}
	std::optional<ValRefSpecial> val_ref_special(IRValueRef value) {
		uint64_t bits;
		if (!this->adaptor->constant(value, &bits)) {
			return {};
		}
		return ValRefSpecial{
			.mode = 4,
			.bank = static_cast<uint8_t>(val_parts(value).bank.id()),
			.bits = bits};
	}
	ValuePart val_part_ref_special(ValRefSpecial &value, uint32_t) {
		return ValuePart{value.bits, 8, ::tpde::RegBank{value.bank}};
	}
	void define_func_idx(IRFuncRef function, uint32_t index) {
		(void) function;
		(void) index;
	}
	void setup_var_ref_assignments() {
		for (uint32_t index = 0;
				index < this->adaptor->frame_slot_reference_count(); ++index) {
			this->init_variable_ref(
				this->adaptor->frame_slot_reference(index), index);
		}
	}
};

} // namespace zend::native::tpde
