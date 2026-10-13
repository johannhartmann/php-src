// SPDX-License-Identifier: PHP-3.01
#pragma once

#include "Zend/Native/TPDE/Common/zend_tpde_direct_user_call.hpp"
#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"

#include <tpde/CompilerBase.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace zend::native::tpde {

/* The offset of a CV or temporary frame slot from the frame. */
constexpr uint64_t frame_slot_offset(uint64_t storage) {
	return (uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
}

/*
 * The target-independent part of the Zend compilers. A backend passes it to
 * TPDE as the compiler base (BaseTy), so it sits between ::tpde::CompilerBase
 * and the architecture's compiler and implements the TPDE hooks that depend
 * only on the adaptor and the value assignments. The backend (Derived)
 * declares it a friend and supplies the target code it uses:
 *  - JUMP_ALWAYS, JUMP_EQUAL, JUMP_NOT_EQUAL: its jump conditions;
 *  - emit_decision_test(reg): flags for a test of the 32-bit reg against 0;
 *  - patch_direct_call_stack_guard(offset, destination, frame_size);
 *  - FRAME_OFFSET_LIMIT: the largest frame displacement its loads and
 *    stores encode;
 *  - load_slot_part(reg, base, offset, kind, role): loads a value part from
 *    the zval at base + offset.
 */
template <typename Adaptor, typename Derived, typename Config>
struct ZendCompilerCommon : ::tpde::CompilerBase<Adaptor, Derived, Config> {
	using Base = ::tpde::CompilerBase<Adaptor, Derived, Config>;
	using AsmReg = typename Base::AsmReg;
	using ValuePart = typename Base::ValuePart;
	using ValuePartRef = typename Base::ValuePartRef;
	using ScratchReg = typename Base::ScratchReg;
	using ValueRef = typename Base::ValueRef;

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

	/*
	 * Calls store(frame_reg, value, storage, offset) for each value the
	 * statepoint of instruction materializes into its frame slot, at offset
	 * from the frame in frame_reg. The canonical frame assignment stays
	 * locked meanwhile: loading a later operand may otherwise evict the frame
	 * and reuse its raw register before the next store. A check of
	 * interrupt boundaries, unless interrupt_slow_path, and a typed body
	 * materialize nothing.
	 */
	template <typename Store>
	bool for_each_materialization(IRInstRef instruction,
			bool interrupt_slow_path, Store &&store) {
		if (this->adaptor->typed_body()) {
			return true;
		}
		const typename Adaptor::InstNode &node = this->adaptor->node(instruction);
		const auto materializations = this->adaptor->materializations(instruction);
		if (materializations.empty()) {
			return true;
		}
		const zend_mir_instruction_record record =
			this->adaptor->mir_instruction(instruction).record;
		if (!interrupt_slow_path
				&& record.opcode == ZEND_MIR_OPCODE_STATEPOINT
				&& (record.effects & ZEND_MIR_EFFECT_MASK(
					ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
			return true;
		}
		if (node.materialization_operand_index == UINT32_MAX
				|| node.materialization_count != materializations.size()
				|| node.materialization_operand_index
					> node.liveness_operands.size()
				|| materializations.size() > node.liveness_operands.size()
					- node.materialization_operand_index) {
			return false;
		}
		const auto frame_local = this->adaptor->val_local_idx(
			IRValueRef{Adaptor::FRAME_VALUE});
		auto *frame_assignment = this->val_assignment(frame_local);
		ZEND_ASSERT(frame_assignment != nullptr);
		ValuePartRef frame_value{
			this, frame_local, frame_assignment, 0, false};
		const AsmReg frame_reg = frame_value.load_to_reg();
		for (uint32_t index = 0; index < materializations.size(); ++index) {
			const zend_tpde_materialization &materialization =
				materializations[index];
			const uint64_t offset =
				frame_slot_offset(materialization.storage_id);
			if (!zend_mir_id_is_valid(materialization.storage_id)
					|| offset > Derived::FRAME_OFFSET_LIMIT - sizeof(zval)) {
				return false;
			}
			const IRValueRef value =
				node.liveness_operands[
					node.materialization_operand_index + index];
			if (!store(frame_reg, value, materialization.storage_id,
					offset)) {
				return false;
			}
		}
		return true;
	}

	/*
	 * Whether a value to materialize into storage is there already: a value
	 * loaded from its own CV slot is, as a write to the CV defines a new
	 * value, and the frame stands in for a value the adaptor found in its
	 * slot.
	 */
	bool materialized_in_own_slot(IRValueRef value,
			zend_mir_storage_id storage) const {
		return value == IRValueRef{Adaptor::FRAME_VALUE}
			|| (storage < this->adaptor->plan()->source_frame_variable_count
				&& this->adaptor->slot_load_storage(value) == storage);
	}

	/*
	 * Redefines each value live across the generator resume instruction
	 * from its canonical frame slot, frame_reg holding the frame, and hands
	 * its reference to reloaded(). A resume landing defines the values
	 * (result_ref); a resume through the gateway uses them and also stores
	 * each reloaded part to its spill slot.
	 */
	template <typename Reloaded>
	bool reload_generator_resume_values(IRInstRef instruction,
			AsmReg frame_reg, bool define, Reloaded &&reloaded) {
		for (const IRValueRef operand :
				this->adaptor->generator_resume_values(instruction)) {
			const zend_mir_storage_id storage =
				this->adaptor->canonical_storage(operand);
			const zend_tpde_machine_value_kind machine_kind =
				this->adaptor->machine_kind(operand);
			const uint64_t offset = frame_slot_offset(storage);
			if (!zend_mir_id_is_valid(storage)
					|| offset + offsetof(zval, u1.type_info)
						> Derived::FRAME_OFFSET_LIMIT) {
				return false;
			}
			auto value = define
				? this->result_ref(operand) : this->val_ref(operand);
			if (!value.has_assignment() || value.variable_ref()) {
				return false;
			}
			/*
			 * The reload redefines the machine value from its canonical
			 * Zend slot. Invalidate TPDE's register copy before allocating
			 * so no stale reload is emitted ahead of the authoritative frame
			 * load.
			 */
			const ValueParts parts = val_parts(operand);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto assignment = value.part_unowned(part).assignment();
				if (!assignment.register_valid()) {
					continue;
				}
				const auto stale_reg = assignment.get_reg();
				const bool owns_register =
					this->register_file.is_used(stale_reg)
					&& this->register_file.reg_local_idx(stale_reg)
						== this->adaptor->val_local_idx(operand)
					&& this->register_file.reg_part(stale_reg) == part;
				if (owns_register) {
					if (assignment.fixed_assignment()) {
						this->register_file.dec_lock_count_must_zero(stale_reg);
						--this->assignments.cur_fixed_assignment_count[
							assignment.bank().id()];
					} else if (this->register_file.is_fixed(stale_reg)) {
						return false;
					}
					this->register_file.unmark_used(stale_reg);
				}
				assignment.set_fixed_assignment(false);
				assignment.set_register_valid(false);
			}
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto value_part = value.part_unowned(part);
				value_part.assignment().set_modified(true);
				auto value_reg = value_part.cur_reg_or_alloc();
				const zend_tpde_machine_part_role role =
					parts.representation.parts[part].semantic_role;
				if (!this->derived()->load_slot_part(value_reg, frame_reg,
						offset, machine_kind, role)) {
					return false;
				}
				value_part.set_modified();
				if (!define) {
					/* A resume continuation may branch to an earlier-compiled
					 * block. Publish the authoritative frame reload before
					 * that block restores the value from TPDE's canonical
					 * spill slot. */
					this->derived()->spill(value_part.assignment());
				}
			}
			reloaded(std::move(value));
		}
		return true;
	}

	/*
	 * A ZEND_USER_OPCODE_DISPATCH_TO case of a user opcode gateway: the
	 * handler of the opline at source asked to run it as target_opcode.
	 */
	struct UserOpcodeDispatchCase {
		const zend_tpde_instruction *instruction;
		const zend_mir_executable_value_ref *operation;
		uint32_t source;
		uint32_t target_opcode;
		zend_tpde_user_opcode_target_kind kind;
		zend_native_runtime_helper_id helper;
		uint32_t frame_operand;
		uint32_t slow_frame_operand;
	};

	/*
	 * Checks the operands of a user opcode gateway node, whose function has
	 * landing_count opline landings, and plans its dispatch cases: for each
	 * dispatch source, one per user opcode target, in order. Operands 0 to 3
	 * are the gateway's own; each case then uses the frame operands its
	 * target kind takes (zend_tpde_user_opcode_target_frame_uses()), the
	 * last for its slow path. False for a node the gateway cannot compile.
	 */
	bool plan_user_opcode_dispatch(const typename Adaptor::InstNode &node,
			size_t landing_count,
			std::vector<UserOpcodeDispatchCase> &dispatch_cases) const {
		const zend_tpde_plan *plan = this->adaptor->plan();
		const auto dispatch_sources =
			this->adaptor->user_opcode_dispatch_to_sources();
		const size_t dispatch_case_count =
			dispatch_sources.size()
				* plan->user_opcode_target_count;
		size_t dispatch_operand_count = 0;
		for (uint32_t target = 0;
				target < plan->user_opcode_target_count; ++target) {
			dispatch_operand_count += dispatch_sources.size()
				* zend_tpde_user_opcode_target_frame_uses(
					plan->user_opcode_targets[target].kind);
		}
		if (plan->source_opcodes == nullptr
				|| node.operands.size() != 4 + dispatch_operand_count
				|| node.argument_index >= plan->source_opcode_count
				|| landing_count < plan->source_opcode_count) {
			return false;
		}
		dispatch_cases.reserve(dispatch_case_count);
		uint32_t frame_operand = 4;
		for (size_t source_index = 0;
				source_index < dispatch_sources.size(); ++source_index) {
			const uint32_t source = dispatch_sources[source_index];
			const zend_tpde_instruction *source_instruction = nullptr;
			for (uint32_t instruction = 0;
					instruction < plan->instruction_count; ++instruction) {
				const zend_tpde_instruction &candidate =
					plan->instructions[instruction];
				if (candidate.has_value_operation
						&& candidate.value_operation.source_position_id
							== source) {
					source_instruction = &candidate;
					break;
				}
			}
			if (source >= plan->user_opcode_source_operation_count) {
				return false;
			}
			const zend_mir_executable_value_ref *source_operation =
				&plan->user_opcode_source_operations[source];
			for (size_t target_index = 0;
					target_index < plan->user_opcode_target_count;
					++target_index) {
				const zend_tpde_user_opcode_target &target_case =
					plan->user_opcode_targets[target_index];
				dispatch_cases.push_back({
					source_instruction,
					source_operation,
					source,
					target_case.opcode,
					target_case.kind,
					target_case.helper,
					frame_operand,
					frame_operand
						+ zend_tpde_user_opcode_target_frame_uses(
							target_case.kind)
						- 1});
				frame_operand +=
					zend_tpde_user_opcode_target_frame_uses(
						target_case.kind);
			}
		}
		return true;
	}

	/*
	 * The callee frame and operand layout of a CALL_DIRECT_USER node with a
	 * direct call descriptor, compiled other than as a typed body's own call.
	 */
	struct DirectUserCallShape {
		uint32_t callee_argument_count;
		bool variadic_frame;
		uint32_t fixed_argument_count;
		uint32_t first_extra_argument_slot;
		uint32_t compiled_variable_count;
		uint32_t owned_argument_variable_count;
		bool release_extra_arguments;
		bool split_cold;
		zend_tpde_direct_user_call_layout layout;
		bool typed_body_call;
	};

	/*
	 * The shape of node's direct user call. The fast node of a generated
	 * frame then drops its references to the frame and context uses only
	 * its slow path takes. False for a node the call cannot compile from.
	 */
	bool direct_user_call_shape(const typename Adaptor::InstNode &node,
			const zend_tpde_instruction &call, bool generated_fast_path,
			bool private_inline_body, uint32_t typed_body_function,
			uint32_t argument_count, DirectUserCallShape &shape) {
		const uint32_t callee_argument_count =
			generated_fast_path
				? call.direct_call->callee_argument_count
				: argument_count;
		const bool variadic_frame =
			generated_fast_path
			&& (call.direct_call->expected_function->common.fn_flags
				& ZEND_ACC_VARIADIC) != 0;
		const uint32_t fixed_argument_count =
			callee_argument_count;
		const uint32_t first_extra_argument_slot =
			generated_fast_path
				? call.direct_call->callee_compiled_variable_count
					+ call.direct_call->callee_temporary_count
				: argument_count;
		const uint32_t compiled_variable_count =
			generated_fast_path
				? call.direct_call
					->callee_compiled_variable_count
				: argument_count;
		const uint32_t owned_argument_variable_count =
			fixed_argument_count + (variadic_frame ? 1 : 0);
		if (generated_fast_path
				&& call.direct_call->default_literal_count
					!= callee_argument_count) {
			return false;
		}
		bool release_extra_arguments = false;
		for (uint32_t index = 0;
				generated_fast_path && index < argument_count; ++index) {
			if (call.direct_call->arguments[index].ordinal
					>= fixed_argument_count) {
				release_extra_arguments =
					release_extra_arguments
					|| !zend_mir_scalar_type_is_exact(
						call.direct_call->arguments[index].exact_type);
			}
		}
		const bool split_cold =
			node.kind == Adaptor::InstKind::GuardedCold;
		const zend_tpde_direct_user_call_layout layout =
			zend_tpde_direct_user_call_layout_of(split_cold,
				typed_body_function != UINT32_MAX, generated_fast_path,
				private_inline_body, variadic_frame, node.has_result,
				argument_count);
		const bool typed_body_call =
			typed_body_function != UINT32_MAX
			&& (node.kind == Adaptor::InstKind::GuardedFast
				|| node.kind == Adaptor::InstKind::MIR)
			&& !node.inlined_user_body
			&& this->adaptor->typed_body_return_type(
				call.component_target_index).valid
			&& this->adaptor->typed_body_arguments_match(
				call.component_target_index, node.operands);
		if (generated_fast_path
				&& node.kind == Adaptor::InstKind::GuardedFast
				&& !typed_body_call
				&& !node.inlined_user_body) {
			if (node.operands.size() < layout.context_operand) {
				return false;
			}
			const uint32_t context_use_count =
				static_cast<uint32_t>(node.operands.size())
				- layout.context_operand;
			auto discard_operand = [&](uint32_t index) {
				auto discarded = this->val_ref(node.operands[index]);
				(void) discarded;
			};
			if (private_inline_body) {
				discard_operand(layout.frame_operand + 1);
				discard_operand(layout.frame_operand + 2);
				for (uint32_t use = 2;
						use < context_use_count; ++use) {
					discard_operand(layout.context_operand + use);
				}
			} else {
				discard_operand(layout.frame_operand + 4);
				discard_operand(layout.frame_operand + 5);
				for (uint32_t use = 4;
						use < context_use_count; ++use) {
					discard_operand(layout.context_operand + use);
				}
			}
		}
		shape = {
			.callee_argument_count = callee_argument_count,
			.variadic_frame = variadic_frame,
			.fixed_argument_count = fixed_argument_count,
			.first_extra_argument_slot = first_extra_argument_slot,
			.compiled_variable_count = compiled_variable_count,
			.owned_argument_variable_count = owned_argument_variable_count,
			.release_extra_arguments = release_extra_arguments,
			.split_cold = split_cold,
			.layout = layout,
			.typed_body_call = typed_body_call,
		};
		return true;
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
