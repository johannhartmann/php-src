// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/LinuxX64/zend_tpde_encodegen_x64.hpp"
#include "Zend/Native/TPDE/EncodeGen/zend_tpde_encodegen_values.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"
#include "Zend/zend_observer.h"

#include <tpde/x64/CompilerX64.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

namespace {

using Adaptor = zend::native::tpde::ZendComponentIRAdaptor;
using IRValueRef = zend::native::tpde::IRValueRef;
using IRInstRef = zend::native::tpde::IRInstRef;
using IRBlockRef = zend::native::tpde::IRBlockRef;
using IRFuncRef = zend::native::tpde::IRFuncRef;

struct ZendX64Config : tpde::x64::PlatformConfig {
	static constexpr bool DEFAULT_VAR_REF_HANDLING = false;
};

class ZendCompilerX64 final
	: public tpde::x64::CompilerX64<Adaptor, ZendCompilerX64,
		::tpde::CompilerBase, ZendX64Config>,
	  public tpde_encodegen::EncodeCompiler<Adaptor, ZendCompilerX64,
		::tpde::CompilerBase, ZendX64Config> {
	using Base = tpde::x64::CompilerX64<Adaptor, ZendCompilerX64,
		::tpde::CompilerBase, ZendX64Config>;
	using EncodeBase = tpde_encodegen::EncodeCompiler<Adaptor, ZendCompilerX64,
		::tpde::CompilerBase, ZendX64Config>;
	zend_native_image *image_;
	/* A source operand in the runtime helpers' encoding. */
	uint64_t encode_source_operand(const zend_mir_source_operand_ref &operand,
			uint32_t unused_payload = ZEND_MIR_ID_INVALID) const {
		return zend_tpde_encode_value_operand(operand,
			adaptor->plan()->source_frame_variable_count, unused_payload);
	}
	std::array<tpde::SymRef, ZEND_NATIVE_HELPER_COUNT> runtime_symbols_{};
	std::vector<tpde::SymRef> image_symbols_;
	std::vector<tpde::SymRef> image_slots_;
	std::vector<tpde::Label> generator_resume_labels_;
	/* Deoptimization resume IDs (ADR 0025 section 4): the landing stubs'
	 * labels and the stack slot that disarms the stress transfer points of
	 * a resumed activation. */
	std::vector<tpde::Label> deopt_resume_labels_;
	int32_t deopt_disarmed_slot_ = 0;
	int32_t deopt_frame_slot_ = 0;
	int32_t deopt_context_slot_ = 0;
	int32_t lookup_reuse_slot_ = 0;
	std::vector<uint32_t> lookup_reuse_reads_;
	uint32_t current_function_index_ = 0;
	std::vector<tpde::Label> user_opcode_labels_;
	std::vector<tpde::Label> user_opcode_dispatch_labels_;
	/* Per source call (MIR instruction), the stack slot recording whether
	 * its Init took the native fast path (ADR 0025 section 3). */
	std::vector<std::pair<uint32_t, int32_t>> fast_call_slots_;
	std::vector<std::pair<uint32_t, int32_t>> fast_entry_slots_;
	std::vector<std::pair<uint32_t, int32_t>> fast_do_entry_slots_;
	std::vector<tpde::Label> user_opcode_result_reload_labels_;
	std::optional<tpde::Label> catch_dispatch_label_;
	/* The zero-status exit of a typed body that may fail. */
	std::optional<tpde::Label> typed_failure_label_;
	uint32_t current_continuation_block_ = UINT32_MAX;
	bool continuation_edge_emitted_ = false;
	/* The fast node of a deoptimization exit: TPDE compiles the exit's
	 * slow block, which leaves the function, between the fast path and
	 * the continuation, so the fast path's edge has no PHI move or
	 * register state to carry its values there. */
	bool deopt_exit_fast_ = false;
	/* TPDE tracks its branch regions in debug builds only. */
	bool in_branch_region_ = false;

	struct TargetBranchAssignment {
		::tpde::ValLocalIdx local_idx;
		uint32_t part;
	};
	using TargetBranchState = std::vector<TargetBranchAssignment>;
	TargetBranchState generator_gateway_state_;

	/*
	 * The entry of a function with deoptimization resume IDs: an entry the
	 * transfer makes (the context's deopt_resume) continues at the landing
	 * its frame's opline names, with the stress transfer points disarmed.
	 * Every other entry arms them and runs the function from the start.
	 */
	bool emit_deopt_gateway(const Adaptor::InstNode &node) {
		const zend_tpde_plan *plan = adaptor->plan();
		if (node.operands.size() != 2
				|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				|| node.operands[1]
					!= IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}
				|| plan->deopt_resume_count == 0) {
			return false;
		}
		while (deopt_resume_labels_.size() < plan->deopt_resume_count) {
			deopt_resume_labels_.push_back(text_writer.label_create());
		}
		deopt_disarmed_slot_ = allocate_stack_slot(sizeof(uint32_t));
		deopt_frame_slot_ = allocate_stack_slot(sizeof(void *));
		deopt_context_slot_ = allocate_stack_slot(sizeof(void *));
		auto normal = text_writer.label_create();
		auto invalid = text_writer.label_create();
		{
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto [context_ref, context] = val_ref_single(node.operands[1]);
			auto frame_reg = frame.load_to_reg();
			auto context_reg = context.load_to_reg();
			/* A landing stub restores every other value; the frame and
			 * context stay where the entry put them. */
			frame.set_modified();
			spill(frame.assignment());
			context.set_modified();
			spill(context.assignment());
			const int32_t resume_offset = static_cast<int32_t>(
				offsetof(zend_native_execution_context, deopt_resume));
			ASM(CMP8mi, FE_MEM(context_reg, 0, FE_NOREG, resume_offset), 0);
			generate_raw_jump(Jump::je, normal);
			ASM(MOV8mi, FE_MEM(context_reg, 0, FE_NOREG, resume_offset), 0);
			ASM(MOV32mi, FE_MEM(FE_BP, 0, FE_NOREG, deopt_disarmed_slot_), 1);
			/* A landing enters an operation's cold block, past the fast
			 * path that sets the isset lookup a later read reuses. */
			ASM(MOV64mi, FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()), 0);
			/* The landing stubs restore the frame and context from here. */
			ASM(MOV64mr, FE_MEM(FE_BP, 0, FE_NOREG, deopt_frame_slot_),
				frame_reg);
			ASM(MOV64mr, FE_MEM(FE_BP, 0, FE_NOREG, deopt_context_slot_),
				context_reg);
			ScratchReg opline{this};
			ScratchReg target{this};
			auto opline_reg = opline.alloc_gp();
			auto target_reg = target.alloc_gp();
			ASM(MOV64rm, opline_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, opline))));
			ASM(MOV64rm, target_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, target_reg,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_function, op_array.opcodes))));
			ASM(SUB64rr, opline_reg, target_reg);
			for (uint32_t index = 0; index < plan->deopt_resume_count;
					++index) {
				const uint64_t byte_offset =
					uint64_t{plan->deopt_resume_targets[index]}
						* sizeof(zend_op);
				if (!adaptor->deopt_resume_landed(index)) {
					continue;
				}
				if (byte_offset > INT32_MAX) {
					return false;
				}
				ASM(CMP64ri, opline_reg, static_cast<int32_t>(byte_offset));
				generate_raw_jump(Jump::je, deopt_resume_labels_[index]);
			}
			generate_raw_jump(Jump::jmp, invalid);
		}
		label_place(invalid);
		emit_status_return(ZEND_NATIVE_EXCEPTION);
		label_place(normal);
		ASM(MOV32mi, FE_MEM(FE_BP, 0, FE_NOREG, deopt_disarmed_slot_), 0);
		return true;
	}

	/*
	 * A stress transfer point before the helper of a guarded operation's
	 * cold path, after its statepoint materializations: an armed point may
	 * hand the frame to a new activation that resumes before the operation
	 * (zend_native_deopt_stress_reenter()); its status is then the
	 * function's. The call is unconditional, so the values it spills are
	 * spilled on both paths.
	 */
	bool emit_deopt_stress_transfer(uint32_t mir_instruction_index) {
		const uint32_t resume =
			adaptor->deopt_resume_index(mir_instruction_index);
		if (resume == UINT32_MAX || !adaptor->plan()->deopt_stress) {
			return true;
		}
		if (deopt_disarmed_slot_ == 0
				|| current_function_index_ >= this->func_syms.size()) {
			return false;
		}
		auto stay = text_writer.label_create();
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		{
			ScratchReg frame{this};
			mov(frame.alloc_gp(), canonical_frame_register(), 8);
			ValuePart frame_part{tpde::x64::PlatformConfig::GP_BANK, 8};
			frame_part.set_value(this, std::move(frame));
			builder.add_arg(std::move(frame_part), tpde::CCAssignment{});
		}
		add_const_arg(builder,
			adaptor->plan()->deopt_resume_targets[resume], 4);
		{
			ScratchReg self{this};
			const AsmReg self_reg = self.alloc_gp();
			emit_symbol_address(self_reg, this->func_syms[current_function_index_]);
			ValuePart self_part{tpde::x64::PlatformConfig::GP_BANK, 8};
			self_part.set_value(this, std::move(self));
			builder.add_arg(std::move(self_part), tpde::CCAssignment{});
		}
		{
			ScratchReg disarmed{this};
			ASM(MOV32rm, disarmed.alloc_gp(),
				FE_MEM(FE_BP, 0, FE_NOREG, deopt_disarmed_slot_));
			ValuePart disarmed_part{tpde::x64::PlatformConfig::GP_BANK, 4};
			disarmed_part.set_value(this, std::move(disarmed));
			builder.add_arg(std::move(disarmed_part), tpde::CCAssignment{});
		}
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_DEOPT_STRESS_REENTER));
		ValuePart transfer{tpde::x64::PlatformConfig::GP_BANK, 8};
		builder.add_ret(transfer, tpde::CCAssignment{});
		const AsmReg transfer_reg = transfer.cur_reg_or_load(this);
		ASM(BT64ri, transfer_reg, 32);
		generate_raw_jump(Jump::jae, stay);
		{
			ScratchReg status{this};
			mov(status.alloc_gp(), transfer_reg, 4);
			transfer.reset(this);
			ValuePart status_part{tpde::x64::PlatformConfig::GP_BANK, 4};
			status_part.set_value(this, std::move(status));
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status_part), tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(stay);
		return true;
	}

	/*
	 * The landing stub of a deoptimization resume ID at the start of its
	 * operation's cold block, out of the path that reaches the block: each
	 * value of the landing's map is loaded from its canonical frame slot
	 * into the register or stack slot TPDE holds it in at the block start;
	 * the stub then falls into the block.
	 */
	bool emit_deopt_landing(uint32_t resume) {
		if (resume >= deopt_resume_labels_.size() || deopt_frame_slot_ == 0) {
			return false;
		}
		auto block = text_writer.label_create();
		generate_raw_jump(Jump::jmp, block);
		label_place(deopt_resume_labels_[resume]);
		/* The stub uses scratch registers and explicit stores only: the
		 * path that reaches the block does not run it, so TPDE's value
		 * state must stay as the block start has it. */
		ScratchReg frame{this};
		const AsmReg frame_reg = frame.alloc_gp();
		ASM(MOV64rm, frame_reg,
			FE_MEM(FE_BP, 0, FE_NOREG, deopt_frame_slot_));
		/* Write a part's value to each copy TPDE holds at the block start.
		 * A value without an assignment is read by nothing after it; one
		 * pending free keeps its place until its loop ends. */
		auto place = [&](IRValueRef value, uint32_t part, AsmReg source) {
			auto *assignment = val_assignment(adaptor->val_local_idx(value));
			if (assignment == nullptr) {
				return true;
			}
			tpde::AssignmentPartRef location{assignment, part};
			if (location.variable_ref()) {
				return false;
			}
			if (location.register_valid()) {
				const AsmReg target{location.get_reg()};
				if (target != source) {
					if (location.bank() == tpde::x64::PlatformConfig::FP_BANK) {
						ASM(SSE_MOVAPDrr, target, source);
					} else {
						mov(target, source, 8);
					}
				}
			}
			if (location.stack_valid()) {
				spill_reg(source, location.frame_off(), location.part_size());
			}
			return location.register_valid() || location.stack_valid();
		};
		if (!place(IRValueRef{Adaptor::FRAME_VALUE}, 0, frame_reg)) {
			return false;
		}
		{
			ScratchReg context{this};
			const AsmReg context_reg = context.alloc_gp();
			ASM(MOV64rm, context_reg,
				FE_MEM(FE_BP, 0, FE_NOREG, deopt_context_slot_));
			if (!place(IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}, 0,
					context_reg)) {
				return false;
			}
		}
		for (const IRValueRef value : adaptor->deopt_landing_values(resume)) {
			const zend_mir_storage_id storage =
				adaptor->canonical_storage(value);
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (!zend_mir_id_is_valid(storage)
					|| offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			const int32_t payload = static_cast<int32_t>(offset);
			const int32_t type_info = static_cast<int32_t>(
				offset + offsetof(zval, u1.type_info));
			const zend_tpde_machine_value_kind kind =
				adaptor->machine_kind(value);
			const ValueParts parts = val_parts(value);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				const zend_tpde_machine_part_role role =
					parts.representation.parts[part].semantic_role;
				ScratchReg loaded{this};
				const bool fp = kind == ZEND_TPDE_MACHINE_VALUE_F64
					&& role == ZEND_TPDE_MACHINE_PART_VALUE;
				const AsmReg target = fp
					? loaded.alloc(tpde::x64::PlatformConfig::FP_BANK)
					: loaded.alloc_gp();
				if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL
						&& role == ZEND_TPDE_MACHINE_PART_VALUE) {
					ASM(MOV32rm, target,
						FE_MEM(frame_reg, 0, FE_NOREG, type_info));
					ASM(CMP32ri, target, IS_TRUE);
					generate_raw_set(Jump::je, target);
				} else if (fp) {
					ASM(SSE_MOVSDrm, target,
						FE_MEM(frame_reg, 0, FE_NOREG, payload));
				} else if (role == ZEND_TPDE_MACHINE_PART_VALUE
						|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					ASM(MOV64rm, target,
						FE_MEM(frame_reg, 0, FE_NOREG, payload));
				} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					ASM(MOV32rm, target,
						FE_MEM(frame_reg, 0, FE_NOREG, type_info));
				} else {
					return false;
				}
				if (!place(value, part, target)) {
					return false;
				}
			}
		}
		frame.reset();
		label_place(block);
		return true;
	}

	/*
	 * A specialized member's failed guard (ADR 0025 section 4): the cold
	 * block, its frame completed by the stores before it, hands the frame
	 * to the generic copy at the operation's resume ID and returns its
	 * status.
	 */
	bool emit_deopt_exit(IRInstRef instruction) {
		const Adaptor::InstNode &node = adaptor->node(instruction);
		const zend_tpde_plan *plan = adaptor->plan();
		const uint32_t generic = plan->deopt_generic_member_plus_one;
		const uint32_t position =
			adaptor->mir_instruction(instruction).record.source_position_id;
		if (generic == 0 || generic - 1 >= this->func_syms.size()
				|| position == UINT32_MAX) {
			return false;
		}
		const AsmReg frame_reg = canonical_frame_register();
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		{
			ScratchReg frame{this};
			mov(frame.alloc_gp(), frame_reg, 8);
			ValuePart frame_part{tpde::x64::PlatformConfig::GP_BANK, 8};
			frame_part.set_value(this, std::move(frame));
			builder.add_arg(std::move(frame_part), tpde::CCAssignment{});
		}
		/* The exit uses none of its operands but the stored ones. */
		for (size_t index = 0; index < node.liveness_operands.size();
				++index) {
			if (!materialized_operand(instruction, index)) {
				auto consumed = val_ref(node.liveness_operands[index]);
				(void) consumed;
			}
		}
		add_const_arg(builder, position, 4);
		{
			ScratchReg entry{this};
			const AsmReg entry_reg = entry.alloc_gp();
			emit_symbol_address(entry_reg, this->func_syms[generic - 1]);
			ValuePart entry_part{tpde::x64::PlatformConfig::GP_BANK, 8};
			entry_part.set_value(this, std::move(entry));
			builder.add_arg(std::move(entry_part), tpde::CCAssignment{});
		}
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_DEOPT_TRANSFER));
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
		builder.add_ret(status, tpde::CCAssignment{});
		/* The machine CFG keeps the edge to the continuation, which TPDE
		 * compiles next with the state this block leaves: every value in
		 * its stack slot, as after the branch of an ordinary slow path. */
		const auto spilled = spill_before_branch(true);
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status), tpde::CCAssignment{});
		return_builder.ret();
		release_spilled_regs(spilled);
		return true;
	}

	/* The scalars a deoptimizing slow node stores to their slots before
	 * its transfer point (Adaptor::add_deopt_stores()). */
	bool emit_deopt_stores(IRInstRef instruction) {
		const Adaptor::InstNode &node = adaptor->node(instruction);
		if (node.deopt_store_operand_index == UINT32_MAX) {
			return true;
		}
		if (node.deopt_store_operand_index + node.deopt_store_count
				> node.liveness_operands.size()) {
			return false;
		}
		const AsmReg frame_reg = canonical_frame_register();
		for (uint32_t index = 0; index < node.deopt_store_count; ++index) {
			const IRValueRef value = node.liveness_operands[
				node.deopt_store_operand_index + index];
			const zend_mir_storage_id storage =
				adaptor->canonical_storage(value);
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (!zend_mir_id_is_valid(storage)
					|| offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			const zend_tpde_machine_value_kind kind =
				adaptor->machine_kind(value);
			const zend_mir_scalar_type_mask type =
				kind == ZEND_TPDE_MACHINE_VALUE_F64 ? ZEND_MIR_SCALAR_TYPE_F64
				: kind == ZEND_TPDE_MACHINE_VALUE_I64 ? ZEND_MIR_SCALAR_TYPE_I64
				: kind == ZEND_TPDE_MACHINE_VALUE_BOOL ? ZEND_MIR_SCALAR_TYPE_I1
				: ZEND_MIR_SCALAR_TYPE_NONE;
			if (type == ZEND_MIR_SCALAR_TYPE_NONE) {
				return false;
			}
			auto value_ref = val_ref(value);
			auto payload = value_ref.part(0);
			store_exact_scalar(frame_reg, static_cast<int32_t>(offset), type,
				payload.load_to_reg());
		}
		return true;
	}

	int32_t site_slot(std::vector<std::pair<uint32_t, int32_t>> &slots,
			uint32_t call_instruction, uint32_t size) {
		for (const auto &[instruction, slot] : slots) {
			if (instruction == call_instruction) {
				return slot;
			}
		}
		const int32_t slot = allocate_stack_slot(size);
		slots.emplace_back(call_instruction, slot);
		return slot;
	}
	int32_t fast_call_slot(uint32_t call_instruction) {
		return site_slot(fast_call_slots_, call_instruction, sizeof(uint32_t));
	}
	/* The entry a dynamic fast call site's Do calls, from its Init. */
	int32_t fast_do_entry_slot(uint32_t call_instruction) {
		return site_slot(
			fast_do_entry_slots_, call_instruction, sizeof(void *));
	}
	/* The native entry a dynamic fast call site resolved in its Init. */
	int32_t fast_entry_slot(uint32_t call_instruction) {
		return site_slot(fast_entry_slots_, call_instruction, sizeof(void *));
	}
	/* A stack slot used only within one instruction's code: compile_inst()
	 * frees it after the instruction. */
	std::vector<std::pair<int32_t, uint32_t>> inst_stack_slots_;
	int32_t inst_stack_slot(uint32_t size) {
		const int32_t slot = allocate_stack_slot(size);
		inst_stack_slots_.emplace_back(slot, size);
		return slot;
	}

	/*
	 * Whether a call_user_func*() or $f(...) site takes the dynamic fast
	 * path: zend_native_call_fast_dynamic_init() pushes the frame of a
	 * recorded native target, every argument is sent to EX(call) by
	 * zend_native_call_fast_send() and the frame is prepared generically.
	 * Sends of register values and named or by-reference-checked
	 * arguments keep the universal protocol.
	 */
	bool source_call_fast_dynamic(const zend_tpde_instruction &call) const {
		const zend_native_user_call_descriptor *descriptor = call.user_call;
		const zend_tpde_plan *plan = adaptor->plan();
		if (!adaptor->generator_resume_targets().empty()
				|| descriptor == nullptr || descriptor->flags != 0
				|| (descriptor->init_opcode != ZEND_INIT_USER_CALL
					&& descriptor->init_opcode != ZEND_INIT_DYNAMIC_CALL)
				|| (descriptor->do_opcode != ZEND_DO_UCALL
					&& descriptor->do_opcode != ZEND_DO_FCALL
					&& descriptor->do_opcode != ZEND_DO_FCALL_BY_NAME)
				|| call.call_argument_count != descriptor->argument_count) {
			return false;
		}
		if (descriptor->do_result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& ((descriptor->do_result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
						&& descriptor->do_result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| (descriptor->do_result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_TMP
						&& descriptor->do_result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_VAR))) {
			return false;
		}
		for (uint32_t index = 0; index < descriptor->argument_count;
				++index) {
			const zend_native_direct_internal_call_argument &argument =
				descriptor->arguments[index];
			const zend_tpde_source_call_phase_entry *phase =
				zend_tpde_source_call_phase_at(
					plan, argument.source_position);
			if ((argument.source_opcode != ZEND_SEND_VAL
						&& argument.source_opcode != ZEND_SEND_VAL_EX
						&& argument.source_opcode != ZEND_SEND_VAR
						&& argument.source_opcode != ZEND_SEND_VAR_EX
						&& argument.source_opcode != ZEND_SEND_USER
						&& argument.source_opcode != ZEND_SEND_ARRAY)
					|| (argument.source_opcode != ZEND_SEND_ARRAY
						&& argument.auxiliary_operand.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED)
					|| phase == nullptr
					|| (phase->operand_flags
						& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) != 0
					|| (phase->phases & ZEND_TPDE_SOURCE_CALL_PHASE_CHECK)
						!= 0) {
				return false;
			}
		}
		return true;
	}

	/*
	 * Whether a source call may take the native call-site fast path of ADR
	 * 0025 (see zend_native_user_call_site_header): a function call or an
	 * instance-method call on $this or a CV, with a literal method name, whose
	 * arguments are all positional by-value sends of CVs, temporaries or
	 * literals kept in their frame slots, and whose result is unused or a
	 * temporary. The runtime publishes a site's target only when that target
	 * takes exactly these arguments by value.
	 */
	bool source_call_fast_eligible(const zend_tpde_instruction &call) const {
		return source_call_fast_static(call) || source_call_fast_dynamic(call);
	}

	bool source_call_fast_static(const zend_tpde_instruction &call) const {
		const zend_native_user_call_descriptor *descriptor = call.user_call;
		const zend_tpde_plan *plan = adaptor->plan();
		auto frame_slot = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
		};
		const bool scalar_result = descriptor != nullptr
			&& (descriptor->flags
				& ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT) != 0;
		/* The fast-path flag lives in a native stack slot, which a
		 * generator resume does not restore. */
		if (!adaptor->generator_resume_targets().empty()) {
			return false;
		}
		if (descriptor == nullptr
				|| (descriptor->flags
					& ~ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT) != 0
				|| (scalar_result
					&& (descriptor->do_result.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| (descriptor->result_type
								!= ZEND_MIR_SCALAR_TYPE_I1
							&& descriptor->result_type
								!= ZEND_MIR_SCALAR_TYPE_I64
							&& descriptor->result_type
								!= ZEND_MIR_SCALAR_TYPE_F64)))
				|| descriptor->argument_count
					!= descriptor->initial_argument_count
				|| descriptor->argument_count != call.call_argument_count
				|| (descriptor->do_opcode != ZEND_DO_UCALL
					&& descriptor->do_opcode != ZEND_DO_FCALL
					&& descriptor->do_opcode != ZEND_DO_FCALL_BY_NAME)) {
			return false;
		}
		switch (descriptor->init_opcode) {
			case ZEND_INIT_FCALL:
			case ZEND_INIT_FCALL_BY_NAME:
			case ZEND_INIT_NS_FCALL_BY_NAME:
				break;
			case ZEND_NEW:
				/* new of a literal class; the Init creates the object in
				 * the result slot. */
				if (descriptor->init_op1.kind
						!= ZEND_MIR_SOURCE_OPERAND_LITERAL
						|| !frame_slot(descriptor->init_result)
						|| (descriptor->init_result.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_TMP
							&& descriptor->init_result.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_VAR)) {
					return false;
				}
				break;
			case ZEND_INIT_STATIC_METHOD_CALL: {
				/* A named class, self::, parent:: or static::. */
				const uint32_t fetch =
					descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK;
				if (descriptor->init_op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_LITERAL
						|| (descriptor->init_op1.kind
								!= ZEND_MIR_SOURCE_OPERAND_LITERAL
							&& (descriptor->init_op1.kind
									!= ZEND_MIR_SOURCE_OPERAND_UNUSED
								|| (fetch != ZEND_FETCH_CLASS_SELF
									&& fetch != ZEND_FETCH_CLASS_PARENT
									&& fetch != ZEND_FETCH_CLASS_STATIC)))) {
					return false;
				}
				break;
			}
			case ZEND_INIT_METHOD_CALL:
				if (descriptor->init_op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_LITERAL) {
					return false;
				}
				if (descriptor->init_op1.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED) {
					break;
				}
				if (!frame_slot(descriptor->init_op1)
						|| (descriptor->init_op1.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_CV
							&& descriptor->init_op1.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_TMP
							&& descriptor->init_op1.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_VAR)) {
					return false;
				}
				break;
			default:
				return false;
		}
		if (descriptor->do_result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (!frame_slot(descriptor->do_result)
					|| (descriptor->do_result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_TMP
						&& descriptor->do_result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_VAR))) {
			return false;
		}
		for (uint32_t index = 0; index < descriptor->argument_count;
				++index) {
			const zend_native_direct_internal_call_argument &argument =
				descriptor->arguments[index];
			const zend_mir_source_operand_ref &source =
				argument.source_operand;
			if (argument.mode != ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
					|| argument.auxiliary_operand.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| argument.ordinal != index
					|| (argument.source_opcode != ZEND_SEND_VAL
						&& argument.source_opcode != ZEND_SEND_VAL_EX
						&& argument.source_opcode != ZEND_SEND_VAR
						&& argument.source_opcode != ZEND_SEND_VAR_EX
						&& argument.source_opcode != ZEND_SEND_FUNC_ARG
						&& argument.source_opcode != ZEND_SEND_VAR_NO_REF_EX)
					|| (source.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& (!frame_slot(source)
							|| (source.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
								&& source.slot_kind
									!= ZEND_MIR_SOURCE_SLOT_TMP
								/* A by-value FUNC_ARG fetch reads its
								 * operand: the VAR holds a plain value
								 * the send moves. A call result sent with
								 * NO_REF_EX, and any VAR sent with
								 * SEND_VAR(_EX), moves, or is
								 * dereferenced. */
								&& (source.slot_kind
										!= ZEND_MIR_SOURCE_SLOT_VAR
									|| (argument.source_opcode
											!= ZEND_SEND_FUNC_ARG
										&& argument.source_opcode
											!= ZEND_SEND_VAR_NO_REF_EX
										&& argument.source_opcode
											!= ZEND_SEND_VAR
										&& argument.source_opcode
											!= ZEND_SEND_VAR_EX)))))) {
				return false;
			}
			if (zend_tpde_source_call_phase_at(
					plan, argument.source_position) == nullptr) {
				return false;
			}
		}
		return true;
	}

	TargetBranchState spill_target_branch_state() {
		const auto release = spill_before_branch(true);
		TargetBranchState state;

		for (auto reg_id : ::tpde::util::BitSetIterator<>{
				release & register_file.used}) {
			const ::tpde::Reg reg{reg_id};
			const auto local_idx = register_file.reg_local_idx(reg);
			if (local_idx == INVALID_VAL_LOCAL_IDX
					|| register_file.is_fixed(reg)) {
				continue;
			}
			state.push_back(TargetBranchAssignment{
				local_idx, register_file.reg_part(reg)});
		}
		return state;
	}

	void reconcile_target_branch_state(const TargetBranchState &state) {
		for (const TargetBranchAssignment &entry : state) {
			::tpde::ValueAssignment *assignment =
				val_assignment(entry.local_idx);
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
			register_file.unmark_used(reg);
		}
	}

public:
	struct ValRefSpecial {
		uint8_t mode = 4;
		uint8_t bank = 0;
		uint8_t padding[6]{};
		uint64_t bits = 0;
	};

	struct ValueParts {
		tpde::RegBank bank;
		zend_tpde_machine_representation_desc representation;
		uint32_t count() const { return representation.part_count; }
		uint32_t size_bytes(uint32_t part) const {
			ZEND_ASSERT(part < representation.part_count);
			return representation.parts[part].bit_width / 8;
		}
		tpde::RegBank reg_bank(uint32_t part) const {
			ZEND_ASSERT(part < representation.part_count);
			return representation.parts[part].register_bank
					== ZEND_TPDE_MACHINE_REGISTER_FP
				? tpde::x64::PlatformConfig::FP_BANK
				: tpde::x64::PlatformConfig::GP_BANK;
		}
	};

	explicit ZendCompilerX64(Adaptor *adaptor, zend_native_image *image)
		: Base{adaptor},
		  image_{image},
		  image_symbols_(image->symbol_count),
		  image_slots_(image->symbol_count) {}

	/*
	 * Direct calls into a component-local function check the C stack against
	 * the callee's projected low-water mark, RSP - frame, instead of RSP
	 * alone. The frame is known only once the callee is compiled, so the guard
	 * encodes SUB with a 32-bit immediate and is patched then; callees
	 * compiled later (forward calls, recursion) are patched when they finish.
	 */
	struct DirectCallStackGuardLabelPatch {
		tpde::Label label;
		uint32_t target_function;
	};
	struct DirectCallStackGuardOffsetPatch {
		uint32_t offset;
		uint32_t target_function;
	};
	std::vector<DirectCallStackGuardLabelPatch>
		current_direct_call_stack_guard_patches_;
	std::vector<DirectCallStackGuardOffsetPatch>
		pending_direct_call_stack_guard_patches_;
	std::vector<uint32_t> function_stack_frame_sizes_;

	void emit_direct_call_stack_guard_position(
			AsmReg destination, uint32_t target_function) {
		ASM(MOV64rr, destination, FE_SP);
		/* Any placeholder outside int8 selects the imm32 encoding. */
		ASM(SUB64ri, destination, INT32_C(0x10000));
		const auto patch_label = text_writer.label_create();
		label_place(patch_label);
		current_direct_call_stack_guard_patches_.push_back({
			patch_label, target_function});
	}

	void patch_direct_call_stack_guard(uint32_t offset, uint32_t frame_size) {
		ZEND_ASSERT(offset >= sizeof(int32_t));
		ZEND_ASSERT(frame_size <= INT32_MAX);
		const int32_t immediate = static_cast<int32_t>(frame_size);
		std::memcpy(text_writer.begin_ptr() + offset - sizeof(int32_t),
			&immediate, sizeof(immediate));
	}

	/* Bytes the function's machine frame occupies below the caller's RSP. */
	uint32_t machine_frame_size() {
		const uint64_t saved_registers = this->register_file.clobbered
			& cur_cc_assigner()->get_ccinfo().callee_saved_regs;
		const bool needs_stack_frame = this->stack.frame_used
			|| this->stack.generated_call || this->stack.has_dynamic_alloca
			|| saved_registers != 0;
		const uint32_t return_address = sizeof(void *);
		if (!needs_stack_frame) {
			return return_address;
		}
		/* return address + saved RBP + saved registers and locals */
		return return_address + sizeof(void *)
			+ tpde::util::align_up(
				this->stack.frame_size + this->max_callee_stack_arg_size, 16);
	}

	void reset() {
		Base::reset();
		EncodeBase::reset();
		current_direct_call_stack_guard_patches_.clear();
		pending_direct_call_stack_guard_patches_.clear();
		function_stack_frame_sizes_.clear();
	}

	bool hook_post_func_sym_init() {
		function_stack_frame_sizes_.assign(
			this->func_syms.size(), UINT32_MAX);
		return true;
	}

	tpde::SymRef runtime_symbol(zend_native_runtime_helper_id id) {
		tpde::SymRef &reference =
			runtime_symbols_[static_cast<uint32_t>(id)];
		if (!reference.valid()) {
			const zend_native_image_symbol *symbol = zend_tpde_image_symbol_find(
				image_, ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
				static_cast<uint32_t>(id), 0);
			if (symbol == nullptr) {
				return {};
			}
			reference = assembler.sym_add_undef(symbol->name,
				tpde::Assembler::SymBinding::GLOBAL);
		}
		return reference;
	}

	AsmReg canonical_value_register(IRValueRef value) {
		const auto local_idx = adaptor->val_local_idx(value);
		::tpde::ValueAssignment *assignment = val_assignment(local_idx);
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
	/*
	 * The frame for an inline form: its fixed register when it has one,
	 * which no form writes, else a scratch copy, as into_scratch() makes.
	 */
	struct FrameRegister {
		std::optional<ScratchReg> scratch;
		AsmReg fixed{};
		bool is_fixed = false;
		AsmReg cur_reg() const {
			return scratch.has_value() ? scratch->cur_reg() : fixed;
		}
		void reset() {
			if (scratch.has_value()) {
				scratch->reset();
			}
		}
	};
	/* A ScratchReg holding the frame, to hand on as a call argument: the
	 * scratch copy itself, or a copy of the fixed register made here, on
	 * the path that passes it. */
	ScratchReg take_frame(FrameRegister &frame) {
		if (!frame.is_fixed) {
			ScratchReg taken = std::move(*frame.scratch);
			frame.scratch.reset();
			return taken;
		}
		ScratchReg copy{this};
		const AsmReg reg = copy.alloc_gp();
		ASM(MOV64rr, reg, frame.fixed);
		return copy;
	}
	FrameRegister frame_register(ValuePartRef &&frame) {
		FrameRegister result;
		if (frame.has_assignment() && frame.assignment().fixed_assignment()) {
			result.fixed = frame.load_to_reg();
			result.is_fixed = true;
			frame.reset();
		} else {
			result.scratch.emplace(std::move(frame).into_scratch());
		}
		return result;
	}
	AsmReg canonical_frame_register() {
		return canonical_value_register(
			IRValueRef{Adaptor::FRAME_VALUE});
	}
	/*
	 * Lookup reuse: isset($a[$k]) whose true edge leads, through a block
	 * holding only its branch, to a block that only this branch enters and
	 * that starts with $a[$k] (FETCH_DIM_R of the same CV container and the
	 * same CV or literal key). Nothing runs between the two, so the element
	 * the isset found is the element the read finds. Returns the IRInstRef
	 * of that read, or an invalid ref.
	 */
	IRInstRef lookup_reuse_consumer(IRInstRef producer_inst) {
		const Adaptor::InstNode &producer = adaptor->node(producer_inst);
		const zend_tpde_instruction &producer_mir =
			adaptor->mir_instruction(producer_inst);
		const zend_mir_executable_value_ref &isset =
			producer_mir.value_operation;
		auto slot_kind = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA
				? operand.slot_kind : ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
		};
		/* isset($a[$k]) names the container first, array_key_exists($k,
		 * $a) the key. */
		const bool key_exists = producer_mir.has_value_operation
			&& isset.source_opcode == ZEND_ARRAY_KEY_EXISTS;
		const zend_mir_source_operand_ref &container =
			key_exists ? isset.op2 : isset.op1;
		const zend_mir_source_operand_ref &key =
			key_exists ? isset.op1 : isset.op2;
		const zend_mir_storage_id container_storage =
			key_exists ? isset.op2_storage_id : isset.op1_storage_id;
		const zend_mir_storage_id key_storage =
			key_exists ? isset.op1_storage_id : isset.op2_storage_id;
		if (producer.kind != Adaptor::InstKind::GuardedFast
				|| producer.continuation_block == UINT32_MAX
				|| !producer_mir.has_value_operation
				|| (!key_exists
					&& (isset.opcode != ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM
						|| (isset.extended_value & ZEND_ISEMPTY) != 0))
				|| slot_kind(container) != ZEND_MIR_SOURCE_SLOT_CV
				|| (key.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
					&& slot_kind(key) != ZEND_MIR_SOURCE_SLOT_CV)) {
			return IRInstRef{UINT32_MAX};
		}
		const IRBlockRef branch_block{producer.continuation_block};
		const auto branch_insts = adaptor->block_insts(branch_block);
		const auto branch_succs = adaptor->block_succs(branch_block);
		/* The branch block holds the loads of the isset's result and the
		 * branch on it. */
		if (branch_insts.empty() || branch_succs.size() != 2) {
			return IRInstRef{UINT32_MAX};
		}
		for (size_t index = 0; index < branch_insts.size(); ++index) {
			const Adaptor::InstNode &branch_node =
				adaptor->node(branch_insts[index]);
			const bool last = index + 1 == branch_insts.size();
			if (last ? branch_node.kind != Adaptor::InstKind::MIR
					|| adaptor->instruction_record(branch_insts[index]).opcode
						!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				: branch_node.kind != Adaptor::InstKind::ZvalPayloadLoad
					|| branch_node.mir_instruction_index
						!= producer.mir_instruction_index) {
				return IRInstRef{UINT32_MAX};
			}
		}
		/* Successor 0 is the branch's true edge; only it may enter. */
		const IRBlockRef read_block = branch_succs[0];
		if (read_block == branch_succs[1]) {
			return IRInstRef{UINT32_MAX};
		}
		uint32_t entries = 0;
		for (IRBlockRef block : adaptor->cur_blocks()) {
			for (IRBlockRef succ : adaptor->block_succs(block)) {
				entries += succ == read_block;
			}
		}
		const auto read_insts = adaptor->block_insts(read_block);
		if (entries != 1 || read_insts.empty()) {
			return IRInstRef{UINT32_MAX};
		}
		const IRInstRef consumer_inst = read_insts[0];
		const Adaptor::InstNode &consumer = adaptor->node(consumer_inst);
		const zend_tpde_instruction &consumer_mir =
			adaptor->mir_instruction(consumer_inst);
		const zend_mir_executable_value_ref &read =
			consumer_mir.value_operation;
		/* Literal keys match by value: the same string or integer. */
		auto same_literal = [&](uint32_t left, uint32_t right) {
			const zend_tpde_plan *plan = adaptor->plan();
			if (plan->source_literals == nullptr
					|| left >= plan->source_literal_count
					|| right >= plan->source_literal_count) {
				return false;
			}
			const zval *a = &plan->source_literals[left];
			const zval *b = &plan->source_literals[right];
			return Z_TYPE_P(a) == Z_TYPE_P(b)
				&& ((Z_TYPE_P(a) == IS_LONG && Z_LVAL_P(a) == Z_LVAL_P(b))
					|| (Z_TYPE_P(a) == IS_STRING
						&& zend_string_equals(Z_STR_P(a), Z_STR_P(b))));
		};
		if (consumer.kind != Adaptor::InstKind::GuardedFast
				|| consumer.synthetic || !consumer_mir.has_value_operation
				|| (read.opcode != ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R
					&& read.opcode != ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW
					&& read.opcode != ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W)
				|| slot_kind(read.op1) != ZEND_MIR_SOURCE_SLOT_CV
				|| read.op1_storage_id != container_storage
				|| read.op2.kind != key.kind
				|| (key.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
					? !same_literal(read.op2.index, key.index)
					: slot_kind(read.op2) != ZEND_MIR_SOURCE_SLOT_CV
						|| read.op2_storage_id != key_storage)) {
			return IRInstRef{UINT32_MAX};
		}
		return consumer_inst;
	}
	/* Runs the function's tier-1 code on this frame and returns its status:
	 * the stack alignment of an entry is unknown, so align, call, restore. */
	void emit_tier1_fallback_call(
			AsmReg frame_reg, AsmReg context_reg, const void *entry,
			uint32_t member_plus_one = 0) {
		this->stack.generated_call = true;
		/* Either register may be the other's argument register. */
		ASM(MOV64rr, FE_AX, context_reg);
		ASM(LEA64rm, FE_DI, FE_MEM(frame_reg, 0, FE_NOREG, 0));
		ASM(MOV64rr, FE_SI, FE_AX);
		ASM(MOV64rr, FE_AX, FE_SP);
		ASM(AND64ri, FE_SP, -16);
		ASM(PUSHr, FE_AX);
		ASM(PUSHr, FE_AX);
		if (member_plus_one != 0) {
			/* A member of this component. */
			emit_symbol_address(FE_AX, this->func_syms[member_plus_one - 1]);
		} else {
			ASM(MOV64ri, FE_AX, static_cast<int64_t>(
				reinterpret_cast<uintptr_t>(entry)));
		}
		ASM(CALLr, FE_AX);
		ASM(MOV64rm, FE_SP, FE_MEM(FE_SP, 0, FE_NOREG, 0));
		ScratchReg status{this};
		const AsmReg status_reg = status.alloc_gp();
		ASM(MOV32rr, status_reg, FE_AX);
		ValuePart status_part{tpde::x64::PlatformConfig::GP_BANK, 4};
		status_part.set_value(this, std::move(status));
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status_part), tpde::CCAssignment{});
		return_builder.ret_local_path();
	}
	/*
	 * The entry of a tier-2 copy specialized by argument type feedback
	 * (zend_native_compiler_tier2_specialize_arguments()): arguments of
	 * other types, or missing ones, run the function's tier-1 code.
	 */
	bool emit_tier2_arg_guards(AsmReg frame_reg, AsmReg context_reg) {
		const zend_tpde_plan *plan = adaptor->plan();
		if (plan->tier2_fallback_entry == nullptr) {
			return true;
		}
		auto fallback = text_writer.label_create();
		auto passed = text_writer.label_create();
		{
			ScratchReg type{this};
			const AsmReg type_reg = type.alloc_gp();
			for (uint32_t index = 0; index < ZEND_NATIVE_TIER2_ARG_GUARDS;
					++index) {
				const uint8_t guard = plan->tier2_arg_guards[index];
				const int32_t type_offset = static_cast<int32_t>(
					(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval)
						+ offsetof(zval, u1.v.type));
				uint8_t exact = 0;
				switch (guard) {
					case ZEND_NATIVE_ARG_GUARD_LONG: exact = IS_LONG; break;
					case ZEND_NATIVE_ARG_GUARD_DOUBLE: exact = IS_DOUBLE; break;
					case ZEND_NATIVE_ARG_GUARD_STRING: exact = IS_STRING; break;
					case ZEND_NATIVE_ARG_GUARD_ARRAY: exact = IS_ARRAY; break;
					case ZEND_NATIVE_ARG_GUARD_OBJECT: exact = IS_OBJECT; break;
					default: break;
				}
				if (exact != 0) {
					ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG, type_offset),
						exact);
					generate_raw_jump(Jump::jne, fallback);
				} else if (guard == ZEND_NATIVE_ARG_GUARD_NUMBER) {
					auto number = text_writer.label_create();
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG, type_offset));
					ASM(CMP32ri, type_reg, IS_LONG);
					generate_raw_jump(Jump::je, number);
					ASM(CMP32ri, type_reg, IS_DOUBLE);
					generate_raw_jump(Jump::jne, fallback);
					label_place(number);
				} else if (guard == ZEND_NATIVE_ARG_GUARD_BOOL) {
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG, type_offset));
					ASM(SUB32ri, type_reg, IS_FALSE);
					ASM(CMP32ri, type_reg, IS_TRUE - IS_FALSE);
					generate_raw_jump(Jump::ja, fallback);
				}
			}
		}
		const bool cold = !text_writer.in_cold_area();
		if (cold) {
			cold_begin();
		} else {
			generate_raw_jump(Jump::jmp, passed);
		}
		label_place(fallback);
		emit_tier1_fallback_call(
			frame_reg, context_reg, plan->tier2_fallback_entry);
		if (cold) {
			cold_end();
		}
		label_place(passed);
		return true;
	}
	/* The stack slot holding the element an isset found for its read, and
	 * the reads whose isset stores it (lookup_reuse_consumer()). */
	int32_t lookup_reuse_slot() {
		if (lookup_reuse_slot_ == 0) {
			lookup_reuse_slot_ = allocate_stack_slot(sizeof(void *));
		}
		return lookup_reuse_slot_;
	}
	bool lookup_reuse_armed(IRInstRef inst) const {
		return std::find(lookup_reuse_reads_.begin(),
			lookup_reuse_reads_.end(), static_cast<uint32_t>(inst))
			!= lookup_reuse_reads_.end();
	}
	/*
	 * A typed body with an untyped result returns a scalar as a boxed zval:
	 * payload and type register. Returns false for any other value.
	 */
	bool add_boxed_scalar_return(RetBuilder &return_builder, IRValueRef value) {
		const zend_tpde_machine_value_kind kind = adaptor->machine_kind(value);
		if (adaptor->plan()->typed_body_return_abi.machine_kind
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| (kind != ZEND_TPDE_MACHINE_VALUE_I64
					&& kind != ZEND_TPDE_MACHINE_VALUE_F64
					&& kind != ZEND_TPDE_MACHINE_VALUE_BOOL)) {
			return false;
		}
		/* Build the parts in their return registers, so that neither
		 * blocks the other's assignment. */
		ScratchReg payload{this};
		ScratchReg type_info{this};
		auto payload_reg = payload.alloc_specific(
			tpde::x64::AsmReg{tpde::x64::AsmReg::AX});
		auto type_info_reg = type_info.alloc_specific(
			tpde::x64::AsmReg{tpde::x64::AsmReg::DX});
		auto [scalar_ref, scalar] = val_ref_single(value);
		auto scalar_reg = scalar.load_to_reg();
		if (kind == ZEND_TPDE_MACHINE_VALUE_F64) {
			ASM(SSE_MOVQ_X2Grr, payload_reg, scalar_reg);
			ASM(MOV32ri, type_info_reg, IS_DOUBLE);
		} else if (kind == ZEND_TPDE_MACHINE_VALUE_I64) {
			ASM(MOV64rr, payload_reg, scalar_reg);
			ASM(MOV32ri, type_info_reg, IS_LONG);
		} else {
			ASM(MOV64rr, payload_reg, scalar_reg);
			ASM(MOVZXr32r8, type_info_reg, scalar_reg);
			ASM(ADD32ri, type_info_reg, IS_FALSE);
		}
		ValuePart payload_part{
			tpde::x64::PlatformConfig::GP_BANK, sizeof(zend_value)};
		payload_part.set_value(this, std::move(payload));
		ValuePart type_info_part{tpde::x64::PlatformConfig::GP_BANK, 4};
		type_info_part.set_value(this, std::move(type_info));
		return_builder.add(std::move(payload_part), tpde::CCAssignment{});
		return_builder.add(std::move(type_info_part), tpde::CCAssignment{});
		return true;
	}
	tpde::Label typed_failure_label() {
		if (!typed_failure_label_.has_value()) {
			typed_failure_label_ = text_writer.label_create();
		}
		return *typed_failure_label_;
	}
	/*
	 * A typed body that may fail returns a status besides a value that is no
	 * boxed zval (whose type part is the status): 1 in the first free integer
	 * return register. Reserve it before the value parts are assigned.
	 */
	bool typed_return_needs_status() const {
		return adaptor->typed_body()
			&& adaptor->plan()->typed_body_may_fail
			&& adaptor->plan()->typed_body_return_abi.machine_kind
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	}
	void reserve_typed_return_status(ScratchReg &status) {
		if (!typed_return_needs_status()) {
			return;
		}
		auto status_reg = status.alloc_specific(
			adaptor->plan()->typed_body_return_abi.machine_kind
					== ZEND_TPDE_MACHINE_VALUE_F64
				? tpde::x64::AsmReg{tpde::x64::AsmReg::AX}
				: tpde::x64::AsmReg{tpde::x64::AsmReg::DX});
		ASM(MOV32ri, status_reg, 1);
	}
	void add_typed_return_status(
			RetBuilder &return_builder, ScratchReg &&status) {
		if (!typed_return_needs_status()) {
			return;
		}
		ValuePart status_part{tpde::x64::PlatformConfig::GP_BANK, 4};
		status_part.set_value(this, std::move(status));
		return_builder.add(std::move(status_part), tpde::CCAssignment{});
	}
	/*
	 * After a typed call to a body that may fail, test its status: the type
	 * part of a boxed result, otherwise an extra integer return. Without a
	 * decision register a zero status leaves through this body's failure
	 * exit; with one, the register receives the status for the caller.
	 */
	/*
	 * Calls the typed body of component member target with the node's
	 * first argument_count operands and defines the node's result from the
	 * returned parts. check_typed_call_status() handles a failed call.
	 */
	bool emit_typed_body_call(const Adaptor::InstNode &node,
			uint32_t function, uint32_t target, uint32_t argument_count,
			ScratchReg *failure_decision) {
		tpde::x64::CCAssignerSysV body_assigner{false};
		CallBuilder body_builder{*this, body_assigner};
		for (uint32_t argument = 0; argument < argument_count; ++argument) {
			body_builder.add_arg(CallArg{node.operands[argument]});
		}
		body_builder.call(this->func_syms[function]);
		const auto return_type = adaptor->typed_body_return_type(target);
		if (!return_type.valid) {
			return false;
		}
		const auto return_representation =
			zend_tpde_machine_representation(return_type.machine_kind, true);
		std::vector<ValuePart> body_results;
		body_results.reserve(return_representation.part_count);
		for (uint32_t part = 0; part < return_representation.part_count;
				++part) {
			const auto &part_desc = return_representation.parts[part];
			body_results.emplace_back(
				part_desc.register_bank == ZEND_TPDE_MACHINE_REGISTER_FP
					? tpde::x64::PlatformConfig::FP_BANK
					: tpde::x64::PlatformConfig::GP_BANK,
				part_desc.bit_width / 8);
			body_builder.add_ret(body_results.back(), tpde::CCAssignment{});
		}
		if (!check_typed_call_status(
				target, body_builder, body_results, failure_decision)) {
			return false;
		}
		if (!node.has_result) {
			for (auto &body_result : body_results) {
				body_result.reset(this);
			}
			return true;
		}
		if (val_parts(node.result).count() != body_results.size()) {
			return false;
		}
		auto destination = result_ref(node.result);
		for (uint32_t part = 0; part < body_results.size(); ++part) {
			destination.part(part).set_value(std::move(body_results[part]));
		}
		return true;
	}

	bool check_typed_call_status(uint32_t target, CallBuilder &builder,
			std::vector<ValuePart> &results, ScratchReg *decision) {
		const zend_tpde_plan *body = adaptor->component_plan(target);
		if (body == nullptr || !body->typed_body_may_fail) {
			return true;
		}
		const bool boxed = body->typed_body_return_abi.machine_kind
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
		ValuePart status_part{tpde::x64::PlatformConfig::GP_BANK, 4};
		if (!boxed) {
			builder.add_ret(status_part, tpde::CCAssignment{});
		} else if (results.size() != 2) {
			return false;
		}
		ValuePart &status = boxed ? results[1] : status_part;
		auto status_reg = status.cur_reg_or_load(this);
		if (decision != nullptr) {
			auto decision_reg = decision->alloc_gp();
			ASM(MOV32rr, decision_reg, status_reg);
		} else {
			ASM(TEST32rr, status_reg, status_reg);
			generate_raw_jump(Jump::je, typed_failure_label());
		}
		if (!boxed) {
			status_part.reset(this);
		}
		return true;
	}
	/* Return a number as int (failing for a double) or as float. */
	bool emit_number_return(IRValueRef value) {
		const bool as_long = adaptor->plan()->typed_body_return_abi.machine_kind
			== ZEND_TPDE_MACHINE_VALUE_I64;
		ScratchReg result{this};
		{
			auto number = val_ref(value);
			auto payload = number.part(0);
			auto type_info = number.part(1);
			auto payload_reg = payload.load_to_reg();
			auto type_info_reg = type_info.load_to_reg();
			if (as_long) {
				auto result_reg = result.alloc_gp();
				ASM(MOV64rr, result_reg, payload_reg);
				ASM(CMP32ri, type_info_reg, IS_LONG);
				generate_raw_jump(Jump::jne, typed_failure_label());
			} else {
				auto result_reg = result.alloc(
					tpde::x64::PlatformConfig::FP_BANK);
				auto is_double = text_writer.label_create();
				auto done = text_writer.label_create();
				ASM(CMP32ri, type_info_reg, IS_LONG);
				generate_raw_jump(Jump::jne, is_double);
				ASM(SSE_CVTSI2SD64rr, result_reg, payload_reg);
				generate_raw_jump(Jump::jmp, done);
				label_place(is_double);
				ASM(SSE_MOVQ_G2Xrr, result_reg, payload_reg);
				label_place(done);
			}
		}
		const bool needs_status = typed_return_needs_status();
		const auto status_target = as_long
			? tpde::x64::AsmReg{tpde::x64::AsmReg::DX}
			: tpde::x64::AsmReg{tpde::x64::AsmReg::AX};
		if (needs_status && result.cur_reg() == status_target) {
			ScratchReg moved{this};
			auto moved_reg = moved.alloc_gp();
			ASM(MOV64rr, moved_reg, result.cur_reg());
			result = std::move(moved);
		}
		ScratchReg status{this};
		if (needs_status) {
			auto status_reg = status.alloc_specific(status_target);
			ASM(MOV32ri, status_reg, 1);
		}
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		ValuePart result_part{as_long
				? tpde::x64::PlatformConfig::GP_BANK
				: tpde::x64::PlatformConfig::FP_BANK, 8};
		result_part.set_value(this, std::move(result));
		return_builder.add(std::move(result_part), tpde::CCAssignment{});
		add_typed_return_status(return_builder, std::move(status));
		return_builder.ret();
		return true;
	}
	/*
	 * Load the HashTable of an array container slot, through a reference
	 * as for by-reference array parameters; anything else goes to slow.
	 */
	void load_array_container(AsmReg base_reg, int32_t offset,
			AsmReg type_reg, AsmReg array_reg, tpde::Label slow) {
		auto loaded = text_writer.label_create();
		auto direct = text_writer.label_create();
		ASM(MOVZXr32m8, type_reg,
			FE_MEM(base_reg, 0, FE_NOREG,
				offset + static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::je, direct);
		ASM(CMP32ri, type_reg, IS_REFERENCE);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg, FE_MEM(base_reg, 0, FE_NOREG, offset));
		ASM(CMP8mi,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_reference, val)
					+ offsetof(zval, u1.type_info))),
			IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_reference, val))));
		generate_raw_jump(Jump::jmp, loaded);
		label_place(direct);
		ASM(MOV64rm, array_reg, FE_MEM(base_reg, 0, FE_NOREG, offset));
		label_place(loaded);
	}
	/*
	 * Whether a liveness operand of a node was already consumed before its
	 * own code: a statepoint materialization (emit_materializations()) or
	 * a generator resume value (reload_generator_values()).
	 */
	bool materialized_operand(IRInstRef instruction, size_t index) {
		const Adaptor::InstNode &node = adaptor->node(instruction);
		if (node.deopt_store_operand_index != UINT32_MAX
				&& index >= node.deopt_store_operand_index
				&& index < node.deopt_store_operand_index
					+ node.deopt_store_count) {
			return true;
		}
		const auto resumed_values =
			adaptor->generator_resume_values(instruction);
		if (node.kind != Adaptor::InstKind::GeneratorResume
				&& !resumed_values.empty()) {
			/* The reload also took the execution context's use. */
			if (node.liveness_operands[index]
					== IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}) {
				return true;
			}
			for (const IRValueRef resumed : resumed_values) {
				if (resumed == node.liveness_operands[index]) {
					return true;
				}
			}
		}
		if (adaptor->typed_body()
				|| node.materialization_operand_index == UINT32_MAX) {
			return false;
		}
		const size_t count = adaptor->materializations(instruction).size();
		return index >= node.materialization_operand_index
			&& index < node.materialization_operand_index + count;
	}

	/*
	 * A fused comparison's result temporary still receives a boolean on the
	 * hot path: a later definition of the same slot may release its old
	 * value, which would otherwise be whatever the slot held before.
	 */
	bool store_fused_result_placeholder(
			const zend_tpde_instruction *compare, AsmReg frame_reg) {
		const uint64_t result_offset = (uint64_t{ZEND_CALL_FRAME_SLOT}
			+ compare->value_operation.result_storage_id) * sizeof(zval);
		if (!zend_mir_id_is_valid(compare->value_operation.result_storage_id)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		ASM(MOV32mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
			IS_FALSE);
		return true;
	}

	/*
	 * The hot part of a comparison fused into its JMPZ/JMPNZ: two numbers
	 * (through a CV's reference) compare and jump to the branch's truthy or
	 * falsey label; any other operand jumps to slow.
	 */
	bool emit_fused_compare(
			const zend_tpde_instruction *compare, AsmReg frame_reg,
			AsmReg left_type, AsmReg left_value, tpde::Label truthy,
			tpde::Label falsey, tpde::Label slow) {
		struct {
			zend_tpde_fused_operand left;
			zend_tpde_fused_operand right;
		} layout{};
		if (compare != nullptr
				&& zend_tpde_fused_type_check_at(*compare, &layout.left)) {
			/* TYPE_CHECK: the mask bit of the type, or of a reference's
			 * referent; an undefined CV, which warns, takes the helper. */
			if (!store_fused_result_placeholder(compare, frame_reg)) {
				return false;
			}
			const int32_t offset = static_cast<int32_t>(layout.left.offset);
			ScratchReg mask{this};
			const AsmReg mask_reg = mask.alloc_gp();
			ASM(MOV32ri, mask_reg, static_cast<int32_t>(
				compare->value_operation.extended_value));
			ASM(MOVZXr32m8, left_type,
				FE_MEM(frame_reg, 0, FE_NOREG,
					offset + static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(BT32rr, mask_reg, left_type);
			generate_raw_jump(Jump::jb, truthy);
			ASM(TEST32rr, left_type, left_type);
			generate_raw_jump(Jump::je, slow);
			ASM(CMP32ri, left_type, IS_REFERENCE);
			/* A reference's referent is tested out of the hot code. */
			auto reference = text_writer.label_create();
			generate_raw_jump(Jump::je, reference);
			generate_raw_jump(Jump::jmp, falsey);
			const bool cold_reference = !text_writer.in_cold_area();
			if (cold_reference) {
				cold_begin();
			}
			label_place(reference);
			ASM(MOV64rm, left_value, FE_MEM(frame_reg, 0, FE_NOREG, offset));
			ASM(MOVZXr32m8, left_type,
				FE_MEM(left_value, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_reference, val)
						+ offsetof(zval, u1.type_info))));
			ASM(BT32rr, mask_reg, left_type);
			generate_raw_jump(Jump::jb, truthy);
			generate_raw_jump(Jump::jmp, falsey);
			if (cold_reference) {
				cold_end();
			}
			return true;
		}
		if (compare == nullptr
				|| !zend_tpde_fused_compare_at(
					*compare, &layout.left, &layout.right)
				|| !store_fused_result_placeholder(compare, frame_reg)) {
			return false;
		}
		const uint32_t opcode = compare->value_operation.source_opcode;
		ScratchReg literals{this};
		ScratchReg right_type_scratch{this};
		ScratchReg right_value_scratch{this};
		ScratchReg left_double{this};
		ScratchReg right_double{this};
		const AsmReg right_type = right_type_scratch.alloc_gp();
		const AsmReg right_value = right_value_scratch.alloc_gp();
		AsmReg literals_reg = frame_reg;
		if (layout.left.literal || layout.right.literal) {
			literals_reg = literals.alloc_gp();
			ASM(MOV64rm, literals_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, literals_reg,
				FE_MEM(literals_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_op_array, literals))));
		}
		auto load = [&](const zend_tpde_fused_operand &operand,
				AsmReg type_reg, AsmReg value_reg) {
			const AsmReg base = operand.literal ? literals_reg : frame_reg;
			const int32_t offset = static_cast<int32_t>(operand.offset);
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(base, 0, FE_NOREG,
					offset + static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(MOV64rm, value_reg, FE_MEM(base, 0, FE_NOREG, offset));
			if (!operand.literal && !operand.temporary) {
				auto plain = text_writer.label_create();
				ASM(CMP32ri, type_reg, IS_REFERENCE);
				generate_raw_jump(Jump::jne, plain);
				ASM(MOVZXr32m8, type_reg,
					FE_MEM(value_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_reference, val)
							+ offsetof(zval, u1.type_info))));
				ASM(MOV64rm, value_reg,
					FE_MEM(value_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_reference, val))));
				label_place(plain);
			}
		};
		load(layout.left, left_type, left_value);
		load(layout.right, right_type, right_value);
		auto not_longs = text_writer.label_create();
		ASM(CMP32ri, left_type, IS_LONG);
		generate_raw_jump(Jump::jne, not_longs);
		ASM(CMP32ri, right_type, IS_LONG);
		generate_raw_jump(Jump::jne, not_longs);
		ASM(CMP64rr, left_value, right_value);
		switch (opcode) {
			case ZEND_IS_SMALLER:
				generate_raw_jump(Jump::jl, truthy);
				break;
			case ZEND_IS_SMALLER_OR_EQUAL:
				generate_raw_jump(Jump::jle, truthy);
				break;
			case ZEND_IS_EQUAL:
				generate_raw_jump(Jump::je, truthy);
				break;
			default:
				generate_raw_jump(Jump::jne, truthy);
				break;
		}
		generate_raw_jump(Jump::jmp, falsey);

		/* long or double, at least one double: out of the hot code */
		const bool cold_doubles = !text_writer.in_cold_area();
		if (cold_doubles) {
			cold_begin();
		}
		label_place(not_longs);
		const auto left_fp =
			left_double.alloc(tpde::x64::PlatformConfig::FP_BANK);
		const auto right_fp =
			right_double.alloc(tpde::x64::PlatformConfig::FP_BANK);
		auto to_double = [&](AsmReg type_reg, AsmReg value_reg, AsmReg fp) {
			auto is_long = text_writer.label_create();
			auto converted = text_writer.label_create();
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::je, is_long);
			ASM(CMP32ri, type_reg, IS_DOUBLE);
			generate_raw_jump(Jump::jne, slow);
			ASM(SSE_MOVQ_G2Xrr, fp, value_reg);
			generate_raw_jump(Jump::jmp, converted);
			label_place(is_long);
			ASM(SSE_CVTSI2SD64rr, fp, value_reg);
			label_place(converted);
		};
		to_double(left_type, left_value, left_fp);
		to_double(right_type, right_value, right_fp);
		/* Unordered (NaN) operands compare false, except !=. */
		switch (opcode) {
			case ZEND_IS_SMALLER:
				ASM(SSE_UCOMISDrr, right_fp, left_fp);
				generate_raw_jump(Jump::ja, truthy);
				break;
			case ZEND_IS_SMALLER_OR_EQUAL:
				ASM(SSE_UCOMISDrr, right_fp, left_fp);
				generate_raw_jump(Jump::jae, truthy);
				break;
			case ZEND_IS_EQUAL:
				ASM(SSE_UCOMISDrr, left_fp, right_fp);
				generate_raw_jump(Jump::jp, falsey);
				generate_raw_jump(Jump::je, truthy);
				break;
			default:
				ASM(SSE_UCOMISDrr, left_fp, right_fp);
				generate_raw_jump(Jump::jp, truthy);
				generate_raw_jump(Jump::jne, truthy);
				break;
		}
		generate_raw_jump(Jump::jmp, falsey);
		if (cold_doubles) {
			cold_end();
		}
		return true;
	}

	/*
	 * The cold part: the comparison's helper computes its temporary, which
	 * the branch then tests. The frame scratch is passed to the call and
	 * recreated afterwards, as no scratch register may live across it.
	 */
	bool emit_fused_compare_helper(
			const zend_tpde_instruction *compare,
			const zend_tpde_instruction &branch, FrameRegister &frame_scratch) {
		const zend_mir_executable_value_ref &operation =
			compare->value_operation;
		{
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart frame_argument{tpde::x64::PlatformConfig::GP_BANK, 8};
			frame_argument.set_value(this, take_frame(frame_scratch));
			builder.add_arg(std::move(frame_argument), tpde::CCAssignment{});
			call_value_operation(builder, operation,
				compare->runtime_helper);
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			auto returned = text_writer.label_create();
			generate_raw_jump(Jump::je, returned);
			status.reset(this);
			const zend_mir_block_id exception_block =
				zend_mir_id_is_valid(compare->exception_block_id)
					? compare->exception_block_id
					: branch.exception_block_id;
			emit_exception_exit(exception_block);
			label_place(returned);
		}
		if (!frame_scratch.is_fixed) {
			frame_scratch.scratch.emplace(this);
			const AsmReg frame_reg = frame_scratch.scratch->alloc_gp();
			ASM(MOV64rr, frame_reg, canonical_frame_register());
		}
		return true;
	}

	/*
	 * Calls a value helper after its frame argument: the operation's
	 * encoded operands, extended value, source opcode and position.
	 */
	void call_value_operation(CallBuilder &builder,
			const zend_mir_executable_value_ref &operation,
			zend_native_runtime_helper_id helper) {
		for (const zend_mir_source_operand_ref *operand :
				{&operation.op1, &operation.op2, &operation.result}) {
			add_const_arg(builder, encode_source_operand(*operand), 8);
		}
		for (uint32_t value : {operation.extended_value,
				operation.source_opcode, operation.source_position_id}) {
			add_const_arg(builder, value, 4);
		}
		builder.call(runtime_symbol(helper));
	}

	/*
	 * A branch helper's decision of ITERATOR_EXCEPTION or above reports a
	 * pending exception: branch to the exception block, or return
	 * EXCEPTION. release() frees the decision on that path only; the
	 * register keeps the decision where the check falls through.
	 */
	template <typename Release>
	void emit_decision_exception_check(AsmReg decision_reg,
			zend_mir_block_id exception_block, Release &&release) {
		ASM(CMP32ri, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
		auto valid = text_writer.label_create();
		generate_raw_jump(Jump::jl, valid);
		release();
		emit_exception_exit(exception_block);
		label_place(valid);
	}

	/*
	 * Calls the native entry an enter helper resolved, with the callee
	 * frame and the context add_context() passes, and returns the entry's
	 * status in CX, the leave helper's fourth argument register.
	 */
	template <typename AddContext>
	ValuePart call_resolved_entry(ValuePart &&callee, ValuePart &&entry,
			AddContext &&add_context) {
		ScratchReg entry_copy{this};
		auto entry_copy_reg =
			entry_copy.alloc_specific(tpde::x64::AsmReg::R11);
		mov(entry_copy_reg, entry.cur_reg_or_load(this), sizeof(void *));
		entry.reset(this);
		ValuePart entry_target{tpde::x64::PlatformConfig::GP_BANK, 8};
		entry_target.set_value(this, std::move(entry_copy));
		ValuePart entry_status{tpde::x64::PlatformConfig::GP_BANK, 4};
		{
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(std::move(callee), tpde::CCAssignment{});
			add_context(builder, assigner);
			builder.call(std::move(entry_target));
			builder.add_ret(entry_status, tpde::CCAssignment{});
		}
		ScratchReg status_copy{this};
		auto status_copy_reg =
			status_copy.alloc_specific(tpde::x64::AsmReg::CX);
		mov(status_copy_reg, entry_status.cur_reg_or_load(this),
			sizeof(zend_native_status));
		entry_status.reset(this);
		ValuePart status_argument{tpde::x64::PlatformConfig::GP_BANK, 4};
		status_argument.set_value(this, std::move(status_copy));
		return status_argument;
	}

	/*
	 * The end of a guarded fast path whose failures jump to slow: the cold
	 * block directly when it can be (generate_guarded_direct_exit()), else
	 * through a decision both paths set.
	 */
	template <typename FrameScratch, typename Successors>
	void finish_guarded(tpde::Label slow, tpde::Label done,
			ScratchReg &&decision, FrameScratch &frame_scratch,
			const Successors &successors) {
		if (guarded_exit_can_jump_directly(successors[1], successors[0])) {
			decision.reset();
			frame_scratch.reset();
			generate_guarded_direct_exit(slow, successors[1], successors[0]);
			return;
		}
		const AsmReg decision_reg = decision.cur_reg();
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		frame_scratch.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
	}
	/* Branches to the continuation of the FINALLY_CALL whose source
	 * position reg holds. */
	void emit_finally_return_dispatch(AsmReg reg) {
		const zend_tpde_plan *plan = adaptor->plan();
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
			ASM(CMP32ri, reg, call.source_position_id);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::jne, continued);
			generate_exception_branch(adaptor->block_ref(target));
			label_place(continued);
		}
	}
	/* Branches to the catch or finally handler whose source position,
	 * flagged with ZEND_NATIVE_FINALLY_EXCEPTION_FLAG, reg holds. */
	void emit_handler_dispatch(AsmReg reg) {
		const zend_tpde_plan *plan = adaptor->plan();
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
				adaptor->block_ref(handler.block_id);
			if (static_cast<uint32_t>(
					this->analyzer.block_idx(handler_block))
					>= this->block_labels.size()) {
				continue;
			}
			ASM(CMP32ri, reg,
				ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
					| handler.source_position_id);
			const auto continued = text_writer.label_create();
			generate_raw_jump(Jump::jne, continued);
			generate_exception_branch(handler_block);
			label_place(continued);
		}
	}
	/* A call TPDE does not see, of symbol. */
	void emit_symbol_call(tpde::SymRef symbol) {
		text_writer.ensure_space(16);
		ASM(CALL, text_writer.cur_ptr() + 5);
		reloc_text(symbol, tpde::elf::R_X86_64_PLT32,
			text_writer.offset() - 4, -4);
	}
	/* reg = &symbol, RIP-relative. */
	template <typename Reg>
	void emit_symbol_address(Reg reg, tpde::SymRef symbol) {
		text_writer.ensure_space(16);
		ASM(LEA64rm, reg, FE_MEM(FE_IP, 0, FE_NOREG, -1));
		reloc_text(symbol, tpde::elf::R_X86_64_PC32,
			text_writer.offset() - 4, -4);
	}
	/* reg = the pointer the symbol slot holds, RIP-relative. */
	template <typename Reg>
	void emit_symbol_load(Reg reg, tpde::SymRef slot) {
		text_writer.ensure_space(16);
		ASM(MOV64rm, reg, FE_MEM(FE_IP, 0, FE_NOREG, -1));
		reloc_text(slot, tpde::elf::R_X86_64_PC32,
			text_writer.offset() - 4, -4);
	}
	/* Whether the node reads nothing but the frame and the context. */
	static bool frame_only_operands(const Adaptor::InstNode &node) {
		for (IRValueRef operand : node.operands) {
			if (operand != IRValueRef{Adaptor::FRAME_VALUE}
					&& operand != IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT}) {
				return false;
			}
		}
		return true;
	}
	/* Ends the uses of the node's operands, which the code reads through
	 * the frame. */
	void consume_operands(const Adaptor::InstNode &node) {
		for (IRValueRef operand : node.operands) {
			auto consumed = val_ref(operand);
			(void) consumed;
		}
	}
	/* A constant call argument of size bytes. */
	template <typename Builder>
	void add_const_arg(Builder &builder, uint64_t value, uint32_t size) {
		builder.add_arg(ValuePart{value, size,
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	}
	/* Returns status from the function. */
	void emit_status_return(zend_native_status status) {
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(ValuePart{static_cast<uint32_t>(status), 4,
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
		return_builder.ret();
	}
	/* A pending exception: to the catching block, or out of the function. */
	void emit_exception_exit(zend_mir_block_id exception_block) {
		if (zend_mir_id_is_valid(exception_block)) {
			generate_exception_branch(adaptor->block_ref(exception_block));
		} else {
			emit_status_return(ZEND_NATIVE_EXCEPTION);
		}
	}
	/* A GuardedFast node's control block: successor 0 is its continuation,
	 * successor 1 its cold block (Adaptor::InstNode). */
	auto guarded_successors_of(const Adaptor::InstNode &node) const {
		ZEND_ASSERT(node.control_block != UINT32_MAX
			&& node.continuation_block != UINT32_MAX);
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		ZEND_ASSERT(successors.size() >= 2
			&& static_cast<uint32_t>(successors[0])
				== node.continuation_block
			&& static_cast<uint32_t>(successors[1]) == node.argument_index);
		return successors;
	}
	/*
	 * Observers are part of the system id: an image only runs in processes
	 * with the observer state it was compiled in. With observers, the
	 * observed path is taken unconditionally; without, nothing is emitted.
	 */
	template <typename Target>
	void emit_observer_exit(Target target) {
		if (ZEND_OBSERVER_ENABLED) {
			generate_raw_jump(Jump::jmp, target);
		}
	}

	/*
	 * A raw call of a runtime helper from inside an instruction that keeps
	 * the allocator state: every caller-saved register live_registers names
	 * (GP ids in the low word, XMM in the high word) is saved on the stack
	 * around it. set_arguments() loads the argument registers after the
	 * saves; check() runs on the result before the restores, which leave
	 * the flags alone. With call_rax, set_arguments() loads the target
	 * into RAX instead of the helper being called.
	 */
	template <typename SetArguments, typename Check>
	void emit_preserving_call(uint64_t live_registers,
			zend_native_runtime_helper_id helper, SetArguments &&set_arguments,
			Check &&check, bool call_rax = false) {
		/* A call TPDE does not see: the frame may not use the red zone. */
		this->stack.generated_call = true;
		static constexpr std::pair<uint32_t, FeRegGP> caller_saved_gp[] = {
			{0, FE_AX}, {1, FE_CX}, {2, FE_DX}, {6, FE_SI}, {7, FE_DI},
			{8, FE_R8}, {9, FE_R9}, {10, FE_R10}, {11, FE_R11}};
		static constexpr FeRegXMM caller_saved_xmm[] = {
			FE_XMM0, FE_XMM1, FE_XMM2, FE_XMM3, FE_XMM4, FE_XMM5, FE_XMM6,
			FE_XMM7, FE_XMM8, FE_XMM9, FE_XMM10, FE_XMM11, FE_XMM12, FE_XMM13,
			FE_XMM14, FE_XMM15};
		std::vector<FeRegGP> saved_gp;
		std::vector<FeRegXMM> saved_xmm;
		for (const auto &[id, reg] : caller_saved_gp) {
			if (((live_registers >> id) & 1) != 0) {
				saved_gp.push_back(reg);
			}
		}
		for (uint32_t i = 0; i < 16; ++i) {
			if (((live_registers >> (32 + i)) & 1) != 0) {
				saved_xmm.push_back(caller_saved_xmm[i]);
			}
		}
		const int32_t xmm_base = static_cast<int32_t>(
			(8 * saved_gp.size() + 15) & ~size_t{15});
		const int32_t save_area = xmm_base
			+ 16 * static_cast<int32_t>(saved_xmm.size()) + 16;
		ASM(LEA64rm, FE_SP, FE_MEM(FE_SP, 0, FE_NOREG, -save_area));
		for (size_t i = 0; i < saved_gp.size(); ++i) {
			ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG,
				static_cast<int32_t>(8 * i)), saved_gp[i]);
		}
		for (size_t i = 0; i < saved_xmm.size(); ++i) {
			ASM(SSE_MOVDQUmr, FE_MEM(FE_SP, 0, FE_NOREG,
				xmm_base + static_cast<int32_t>(16 * i)), saved_xmm[i]);
		}
		set_arguments();
		if (call_rax) {
			ASM(CALLr, FE_AX);
		} else {
			emit_symbol_call(runtime_symbol(helper));
		}
		check();
		for (size_t i = 0; i < saved_xmm.size(); ++i) {
			ASM(SSE_MOVDQUrm, saved_xmm[i], FE_MEM(FE_SP, 0, FE_NOREG,
				xmm_base + static_cast<int32_t>(16 * i)));
		}
		for (size_t i = 0; i < saved_gp.size(); ++i) {
			ASM(MOV64rm, saved_gp[i], FE_MEM(FE_SP, 0, FE_NOREG,
				static_cast<int32_t>(8 * i)));
		}
		ASM(LEA64rm, FE_SP, FE_MEM(FE_SP, 0, FE_NOREG, save_area));
	}

	/* Defines a result from a helper's 8-byte payload return, moved into
	 * an FP register for a double. */
	void set_payload_result(IRValueRef value, ValuePart &&payload) {
		auto [result_ref, result] = result_ref_single(value);
		if (val_parts(value).bank == tpde::x64::PlatformConfig::FP_BANK) {
			auto payload_reg = payload.cur_reg_or_load(this);
			ScratchReg converted{this};
			auto result_reg =
				converted.alloc(tpde::x64::PlatformConfig::FP_BANK);
			ASM(SSE_MOVQ_G2Xrr, result_reg, payload_reg);
			payload.reset(this);
			result.set_value(std::move(converted));
		} else {
			result.set_value(std::move(payload));
		}
	}

	/*
	 * The tail after a helper that returns a status: RETURNED continues
	 * inline, an exception branches to the exception block when there is
	 * one, and any other status returns from the function.
	 */
	void emit_status_tail(ValuePart &&status, zend_mir_block_id exception_block,
			bool local_return = false) {
		const auto status_reg = status.cur_reg_or_load(this);
		auto continued = text_writer.label_create();
		ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
		generate_raw_jump(Jump::je, continued);
		if (zend_mir_id_is_valid(exception_block)) {
			auto propagate = text_writer.label_create();
			ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
			generate_raw_jump(Jump::jne, propagate);
			generate_exception_branch(adaptor->block_ref(exception_block));
			label_place(propagate);
		}
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status), tpde::CCAssignment{});
		if (local_return) {
			return_builder.ret_local_path();
		} else {
			return_builder.ret();
		}
		label_place(continued);
	}

	/* The register the assigner gives the next 8-byte GP argument, invalid
	 * for a stack argument. A value built there needs no move when the call
	 * builder takes it. */
	static AsmReg next_gp_argument(
			const tpde::x64::CCAssignerSysV *assigner) {
		if (assigner == nullptr) {
			return AsmReg::make_invalid();
		}
		auto probe = *assigner;
		tpde::CCAssignment assignment{
			.bank = tpde::x64::PlatformConfig::GP_BANK, .size = 8};
		probe.assign_arg(assignment);
		return assignment.reg;
	}

	ValuePart copy_fixed_argument(AsmReg source,
			const tpde::x64::CCAssignerSysV *argument_of = nullptr) {
		ScratchReg copy{this};
		const AsmReg argument = next_gp_argument(argument_of);
		auto copy_reg = argument.valid()
			? copy.alloc_specific(argument) : copy.alloc_gp();
		ASM(MOV64rr, copy_reg, source);
		ValuePart value{
			tpde::x64::PlatformConfig::GP_BANK, sizeof(void *)};
		value.set_value(this, std::move(copy));
		return value;
	}

	/* The data slot holding an image symbol's address, created on first
	 * use; invalid when the image has no such symbol. */
	tpde::SymRef image_symbol_slot(
		zend_native_image_symbol_kind kind, uint32_t id) {
		/* Runtime helpers are image-wide symbols (prepare_image_symbols()
		 * registers them for function index 0), whichever component
		 * member refers to them. */
		const zend_native_image_symbol *symbol =
			zend_tpde_image_symbol_find(image_, kind, id,
				kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER
					? 0 : adaptor->current_function_index());
		if (symbol == nullptr) {
			return {};
		}
		const uint32_t index =
			static_cast<uint32_t>(symbol - image_->symbols);
		tpde::SymRef &reference = image_symbols_[index];
		if (!reference.valid()) {
			reference = assembler.sym_add_undef(symbol->name,
				tpde::Assembler::SymBinding::GLOBAL);
		}
		tpde::SymRef &slot = image_slots_[index];
		if (!slot.valid()) {
			const std::array<tpde::u8, sizeof(uintptr_t)> zero{};
			tpde::SecRef section = assembler.get_default_section(
				tpde::SectionKind::DataRelRO);
			uint32_t offset = 0;
			slot = assembler.sym_def_data(section, "", zero, alignof(uintptr_t),
				tpde::Assembler::SymBinding::LOCAL, &offset);
			assembler.reloc_abs(section, reference, offset, 0);
		}
		return slot;
	}
	ValuePart image_symbol_value(
		zend_native_image_symbol_kind kind, uint32_t id,
		const tpde::x64::CCAssignerSysV *argument_of = nullptr) {
		const tpde::SymRef slot = image_symbol_slot(kind, id);
		if (!slot.valid()) {
			return ValuePart{tpde::x64::PlatformConfig::GP_BANK, 8};
		}
		ValuePart target{tpde::x64::PlatformConfig::GP_BANK, 8};
		const AsmReg argument = next_gp_argument(argument_of);
		AsmReg target_reg;
		if (argument.valid()) {
			ScratchReg scratch{this};
			target_reg = scratch.alloc_specific(argument);
			target.set_value(this, std::move(scratch));
		} else {
			target_reg = target.alloc_reg(this);
		}
		emit_symbol_load(target_reg, slot);
		return target;
	}

	void generate_exception_branch(IRBlockRef target) {
		auto index = static_cast<uint32_t>(this->analyzer.block_idx(target));
		generate_raw_jump(Jump::jmp, this->block_labels[index]);
	}
	template <typename BranchJump>
	void generate_branch_to_block(
			BranchJump jump, IRBlockRef target,
			bool needs_split, bool last_inst) {
		const bool continuation = static_cast<uint32_t>(target)
			== current_continuation_block_;
		continuation_edge_emitted_ = continuation_edge_emitted_
			|| continuation;
		/* Inside a branch region, the region's own spill came first. */
		if (continuation && deopt_exit_fast_ && !in_branch_region_) {
			(void) spill_before_branch();
		}
		Base::generate_branch_to_block(
			jump, target, needs_split, last_inst);
	}
	void begin_branch_region() {
		in_branch_region_ = true;
		Base::begin_branch_region();
	}
	void end_branch_region() {
		in_branch_region_ = false;
		Base::end_branch_region();
	}
	void generate_uncond_branch(IRBlockRef target) {
		continuation_edge_emitted_ =
			continuation_edge_emitted_
			|| static_cast<uint32_t>(target)
				== current_continuation_block_;
		Base::generate_uncond_branch(target);
	}
	/*
	 * A guarded fast path can leave its node with plain jumps, the success
	 * path to the continuation and each failed check to the cold block,
	 * when neither block has PHIs (no edge moves) and no register holds an
	 * unspilled value (the branch spill emits no code the checks would
	 * skip). Otherwise it selects the edge through a decision register.
	 */
	/*
	 * General-purpose registers neither locked as scratch nor held by a
	 * fixed assignment. Inline forms that need many scratch registers take
	 * their helper instead when a large function leaves too few.
	 */
	uint32_t unlocked_gp_registers() const {
		uint32_t count = 0;
		for (uint32_t id = 0; id < 16; ++id) {
			if (((register_file.allocatable >> id) & 1) != 0
					&& register_file.lock_counts[id] == 0) {
				++count;
			}
		}
		return count;
	}
	bool guarded_exit_can_jump_directly(IRBlockRef cold, IRBlockRef hot) {
		if (branch_needs_split(cold) || branch_needs_split(hot)) {
			return false;
		}
		for (auto reg_id : ::tpde::util::BitSetIterator<>{
				register_file.used}) {
			const ::tpde::Reg reg{reg_id};
			const auto local_idx = register_file.reg_local_idx(reg);
			if (local_idx == INVALID_VAL_LOCAL_IDX) {
				continue;
			}
			::tpde::AssignmentPartRef part{
				val_assignment(local_idx), register_file.reg_part(reg)};
			if (!part.fixed_assignment() && part.modified()
					&& !part.variable_ref()) {
				return false;
			}
		}
		return true;
	}
	void generate_guarded_direct_exit(
			tpde::Label slow, IRBlockRef cold, IRBlockRef hot) {
		const auto spilled = spill_before_branch();
		begin_branch_region();
		/* The failed checks' jump to the cold block waits in the cold area,
		 * so the success path falls through to a following hot block. */
		const bool cold_stub = !text_writer.in_cold_area();
		generate_branch_to_block(Jump::jmp, hot, false, cold_stub);
		if (cold_stub) {
			cold_begin();
		}
		label_place(slow);
		generate_branch_to_block(Jump::jmp, cold, false, !cold_stub);
		if (cold_stub) {
			cold_end();
		}
		end_branch_region();
		release_spilled_regs(spilled);
	}
	/*
	 * An internal call whose verified MIR result is an exact scalar left that
	 * scalar in its result slot of this function's frame; read it inline
	 * instead of through a runtime helper. Returns false when the slot is
	 * not a CV/VAR/TMP of this frame, so the caller uses the helper.
	 */
	bool load_scalar_call_result(
			const Adaptor::InstNode &node,
			const zend_mir_source_operand_ref &operand) {
		if ((operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
				|| (operand.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& operand.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operand.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)) {
			return false;
		}
		const uint64_t physical_slot =
			operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				? uint64_t{operand.index}
				: uint64_t{adaptor->plan()->source_frame_variable_count}
					+ operand.index;
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + physical_slot) * sizeof(zval);
		if (offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		const zend_mir_scalar_type_mask type = adaptor->exact_type(node.result);
		if (type != ZEND_MIR_SCALAR_TYPE_NULL
				&& type != ZEND_MIR_SCALAR_TYPE_I1
				&& type != ZEND_MIR_SCALAR_TYPE_I64
				&& type != ZEND_MIR_SCALAR_TYPE_F64) {
			return false;
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		auto [result_ref, result] = result_ref_single(node.result);
		const int32_t payload = static_cast<int32_t>(offset);
		const int32_t type_byte = static_cast<int32_t>(
			offset + offsetof(zval, u1.type_info));
		if (val_parts(node.result).bank == tpde::x64::PlatformConfig::FP_BANK) {
			ScratchReg loaded{this};
			auto loaded_reg = loaded.alloc(tpde::x64::PlatformConfig::FP_BANK);
			ASM(SSE_MOVSDrm, loaded_reg,
				FE_MEM(frame_reg, 0, FE_NOREG, payload));
			result.set_value(std::move(loaded));
			return true;
		}
		ScratchReg loaded{this};
		auto loaded_reg = loaded.alloc_gp();
		if (type == ZEND_MIR_SCALAR_TYPE_I64) {
			ASM(MOV64rm, loaded_reg, FE_MEM(frame_reg, 0, FE_NOREG, payload));
		} else if (type == ZEND_MIR_SCALAR_TYPE_I1) {
			/* IS_FALSE and IS_TRUE are adjacent: the type decides. */
			ASM(MOVZXr32m8, loaded_reg,
				FE_MEM(frame_reg, 0, FE_NOREG, type_byte));
			ASM(SUB32ri, loaded_reg, IS_FALSE);
		} else {
			ASM(MOV32ri, loaded_reg, 0);
		}
		result.set_value(std::move(loaded));
		return true;
	}

	/*
	 * Stores a scalar literal (null, boolean, integer, double) into a zval
	 * slot with immediates. Returns false for any other literal, which the
	 * caller then copies at run time. Literal tables are immutable, so the
	 * value read at compile time is the value the copy would read.
	 */
	bool store_scalar_literal(AsmReg base, int32_t offset, const zval *literal) {
		if (literal == nullptr || Z_TYPE_INFO_P(literal) > IS_DOUBLE
				|| Z_TYPE_P(literal) == IS_UNDEF) {
			return false;
		}
		if (Z_TYPE_P(literal) == IS_LONG || Z_TYPE_P(literal) == IS_DOUBLE) {
			uint64_t bits;
			std::memcpy(&bits, &literal->value, sizeof(bits));
			if (static_cast<int64_t>(bits)
					== static_cast<int64_t>(static_cast<int32_t>(bits))) {
				ASM(MOV64mi, FE_MEM(base, 0, FE_NOREG, offset),
					static_cast<int32_t>(bits));
			} else {
				ScratchReg value{this};
				auto value_reg = value.alloc_gp();
				ASM(MOV64ri, value_reg, static_cast<int64_t>(bits));
				ASM(MOV64mr, FE_MEM(base, 0, FE_NOREG, offset), value_reg);
			}
		}
		ASM(MOV32mi,
			FE_MEM(base, 0, FE_NOREG,
				offset + static_cast<int32_t>(offsetof(zval, u1.type_info))),
			static_cast<int32_t>(Z_TYPE_INFO_P(literal)));
		return true;
	}

	/*
	 * Stores an exact scalar held in a register into the zval at base +
	 * offset: payload and type for a double or an integer, the type alone
	 * for a boolean (0 or 1 in the register) or null (no register).
	 * Returns false for any other type.
	 */
	bool store_exact_scalar(AsmReg base, int32_t offset,
			zend_mir_scalar_type_mask type, AsmReg value_reg) {
		const auto type_info = FE_MEM(base, 0, FE_NOREG,
			offset + static_cast<int32_t>(offsetof(zval, u1.type_info)));
		if (type == ZEND_MIR_SCALAR_TYPE_F64) {
			ASM(SSE_MOVSDmr, FE_MEM(base, 0, FE_NOREG, offset), value_reg);
			ASM(MOV32mi, type_info, IS_DOUBLE);
		} else if (type == ZEND_MIR_SCALAR_TYPE_I64) {
			ASM(MOV64mr, FE_MEM(base, 0, FE_NOREG, offset), value_reg);
			ASM(MOV32mi, type_info, IS_LONG);
		} else if (type == ZEND_MIR_SCALAR_TYPE_I1) {
			ScratchReg type_scratch{this};
			auto type_reg = type_scratch.alloc_gp();
			ASM(MOV32rr, type_reg, value_reg);
			ASM(ADD32ri, type_reg, IS_FALSE);
			ASM(MOV32mr, type_info, type_reg);
		} else if (type == ZEND_MIR_SCALAR_TYPE_NULL) {
			ASM(MOV32mi, type_info, IS_NULL);
		} else {
			return false;
		}
		return true;
	}

	/*
	 * Stores a boolean (0 or 1 in a register) into the zval at base +
	 * offset, payload included: boolean readers of the slot load the
	 * payload.
	 */
	void store_boolean_zval(AsmReg base, int32_t offset, AsmReg value_reg,
			AsmReg type_reg) {
		ASM(MOV64mr, FE_MEM(base, 0, FE_NOREG, offset), value_reg);
		ASM(MOV32rr, type_reg, value_reg);
		ASM(ADD32ri, type_reg, IS_FALSE);
		ASM(MOV32mr,
			FE_MEM(base, 0, FE_NOREG,
				offset + static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
	}

	void generate_guarded_decision_branch(
			ScratchReg &&decision, IRBlockRef nonzero_target,
			IRBlockRef zero_target) {
		const IRBlockRef next = analyzer.block_ref(next_block());
		const bool nonzero_needs_split =
			branch_needs_split(nonzero_target);
		const bool zero_needs_split = branch_needs_split(zero_target);
		const auto spilled = spill_before_branch();

		begin_branch_region();
		ASM(TEST32rr, decision.cur_reg(), decision.cur_reg());
		if (next == nonzero_target
				|| (next != zero_target && nonzero_needs_split)) {
			generate_branch_to_block(
				Jump::je, zero_target, zero_needs_split, false);
			generate_branch_to_block(
				Jump::jmp, nonzero_target, false, true);
		} else if (next == zero_target) {
			generate_branch_to_block(
				Jump::jne, nonzero_target,
				nonzero_needs_split, false);
			generate_branch_to_block(
				Jump::jmp, zero_target, false, true);
		} else {
			ZEND_ASSERT(!nonzero_needs_split);
			generate_branch_to_block(
				Jump::jne, nonzero_target, false, false);
			generate_branch_to_block(
				Jump::jmp, zero_target, false, true);
		}
		end_branch_region();
		release_spilled_regs(spilled);
	}
	template <typename BranchJump>
	void generate_cond_branch(
			BranchJump jump, IRBlockRef true_target,
			IRBlockRef false_target) {
		continuation_edge_emitted_ =
			continuation_edge_emitted_
			|| static_cast<uint32_t>(true_target)
				== current_continuation_block_
			|| static_cast<uint32_t>(false_target)
				== current_continuation_block_;
		Base::generate_cond_branch(jump, true_target, false_target);
	}

	/*
	 * Ask the adaptor per function view: a Zend entry whose only call is a
	 * direct typed-body call needs no runtime helper, yet is not a leaf, and a
	 * typed body can be a leaf even when its Zend entry uses helpers.
	 */
	bool cur_func_may_emit_calls() const {
		return adaptor->cur_func_may_emit_calls() || has_entry_variant();
	}
	tpde::SymRef cur_personality_func() const { return {}; }
	bool try_force_fixed_assignment(IRValueRef value) const {
		const zend_tpde_machine_value_kind kind =
			adaptor->machine_kind(value);
		const bool pointer_value =
			kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
		return value == IRValueRef{Adaptor::FRAME_VALUE}
			|| (!adaptor->typed_body()
				&& value == IRValueRef{
					Adaptor::EXECUTION_CONTEXT_ARGUMENT})
			|| (pointer_value
				&& adaptor->machine_value_is_register_authoritative(value))
			|| (adaptor->val_is_phi(value)
				&& kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& adaptor->machine_value_is_register_authoritative(value));
	}
	/*
	 * The frame goes to RBX: frame-relative operands, the most frequent
	 * memory operands, then need no SIB byte (R12 would). The other fixed
	 * values leave RBX to it.
	 */
	/*
	 * A section of rarely run code behind the function's hot code. A
	 * section opened inside one (a cold block, a slow path's own stub) stays
	 * where it is, with a jump over it, as its code is entered by jumps
	 * only and leaves by jumps.
	 */
	using ColdLabel = decltype(std::declval<tpde::x64::FunctionWriterX64 &>()
		.label_create());
	std::vector<std::optional<ColdLabel>> cold_sections_;
	size_t cold_sections_at_inst_ = 0;
	void cold_begin() {
		if (text_writer.in_cold_area()) {
			const ColdLabel skip = text_writer.label_create();
			generate_raw_jump(Jump::jmp, skip);
			cold_sections_.emplace_back(skip);
			return;
		}
		text_writer.begin_cold_area();
		cold_sections_.emplace_back(std::nullopt);
	}
	void cold_end() {
		ZEND_ASSERT(!cold_sections_.empty());
		const std::optional<ColdLabel> skip = cold_sections_.back();
		cold_sections_.pop_back();
		if (skip.has_value()) {
			label_place(*skip);
		} else {
			text_writer.end_cold_area();
		}
	}
	static constexpr int32_t frame_register_bias = 0x80;
	AsmReg select_fixed_assignment_reg(
			tpde::AssignmentPartRef part, IRValueRef value) {
		constexpr uint64_t bx = uint64_t{1} << tpde::x64::AsmReg::BX;
		if (value == IRValueRef{Adaptor::FRAME_VALUE}) {
			if (this->biased_off != 0) {
				return AsmReg{tpde::x64::AsmReg::BX};
			}
			if (part.bank() == tpde::x64::PlatformConfig::GP_BANK
					&& (this->register_file.allocatable & bx) != 0
					&& (this->register_file.used & bx) == 0
					&& (this->fixed_assignment_nonallocatable_mask & bx) == 0
					&& !this->stack.is_leaf_function
					&& (this->cur_cc_assigner()->get_ccinfo()
							.callee_saved_regs & bx) != 0) {
				/* RBX holds the frame plus 0x80: the frame's header and its
				 * first slots up to offset 0xff are disp8 operands. No other
				 * value may use RBX afterwards, even when the frame is dead:
				 * the encoder rebiases every operand naming it. */
				this->set_biased_reg(FE_BX, frame_register_bias);
				this->register_file.allocatable &= ~bx;
				return AsmReg{tpde::x64::AsmReg::BX};
			}
			return Base::select_fixed_assignment_reg(part, value);
		}
		if (this->stack.is_leaf_function) {
			return Base::select_fixed_assignment_reg(part, value);
		}
		const uint64_t saved = this->fixed_assignment_nonallocatable_mask;
		this->fixed_assignment_nonallocatable_mask |= bx;
		AsmReg reg = Base::select_fixed_assignment_reg(part, value);
		this->fixed_assignment_nonallocatable_mask = saved;
		if (!reg.valid()) {
			reg = Base::select_fixed_assignment_reg(part, value);
		}
		return reg;
	}
	ValueParts val_parts(IRValueRef value) const {
		const zend_tpde_machine_value_kind kind =
			adaptor->machine_kind(value);
		const zend_tpde_machine_representation_desc representation =
			zend_tpde_machine_representation(
				kind, adaptor->machine_value_is_register_authoritative(value));
		return {
			representation.parts[0].register_bank
					== ZEND_TPDE_MACHINE_REGISTER_FP
				? tpde::x64::PlatformConfig::FP_BANK
				: tpde::x64::PlatformConfig::GP_BANK,
			representation};
	}
	std::optional<ValRefSpecial> val_ref_special(IRValueRef value) {
		uint64_t bits;
		if (!adaptor->constant(value, &bits)) {
			return {};
		}
		return ValRefSpecial{
			.mode = 4,
			.bank = static_cast<uint8_t>(val_parts(value).bank.id()),
			.bits = bits};
	}
	ValuePart val_part_ref_special(ValRefSpecial &value, uint32_t) {
		return ValuePart{value.bits, 8, tpde::RegBank{value.bank}};
	}
	void define_func_idx(IRFuncRef function, uint32_t index) {
		(void) function;
		(void) index;
	}
	void start_func(uint32_t index) {
		/* A function that failed to compile left its guard patches. */
		current_direct_call_stack_guard_patches_.clear();
		generator_resume_labels_.clear();
		generator_gateway_state_.clear();
		deopt_resume_labels_.clear();
		deopt_disarmed_slot_ = 0;
		deopt_frame_slot_ = 0;
		deopt_context_slot_ = 0;
		lookup_reuse_slot_ = 0;
		lookup_reuse_reads_.clear();
		cold_sections_.clear();
		current_function_index_ = index;
		user_opcode_labels_.clear();
		user_opcode_dispatch_labels_.clear();
		fast_call_slots_.clear();
		fast_entry_slots_.clear();
		fast_do_entry_slots_.clear();
		user_opcode_result_reload_labels_.clear();
		catch_dispatch_label_.reset();
		typed_failure_label_.reset();
		/* A typed body returns doubles in XMM0 and XMM1. A value fixed there
		 * across blocks would block the result registers at a RETURN. */
		this->fixed_assignment_nonallocatable_mask |=
			(uint64_t{1} << tpde::x64::AsmReg::XMM0)
			| (uint64_t{1} << tpde::x64::AsmReg::XMM1);
		/* TPDE never fixes AX, DX and CX, so the status register of a body
		 * that may fail stays free as well. */
		Base::start_func(index);
		entry_variant_dispatch_pending_ = has_entry_variant();
		if (!adaptor->typed_body() && adaptor->plan() != nullptr
				&& adaptor->plan()->fast_call_eligible) {
			emit_fast_call_entry(index);
		}
	}
	/*
	 * The member's fast-call entry (zend_native_code_fast_call_entry()),
	 * written before the Zend entry so that both share their cache lines:
	 * frameless code with zend_native_call_fast_do()'s arguments (RDI the
	 * caller, RSI the context, RDX the descriptor, R8D the result offset or
	 * UINT32_MAX) that does the fast Do's work for a site sending exactly
	 * the parameters by value to this untyped function. It links and
	 * initializes the frame the Init pushed, calls the Zend entry and
	 * leaves through the shared zend_native_call_fast_leave(). Only
	 * caller-saved registers are used; the stack area holds the frame, the
	 * result offset and the discarded result.
	 */
	void emit_fast_call_entry(uint32_t function_index) {
		const zend_tpde_plan *plan = adaptor->plan();
		const uint32_t num_args = plan->source_num_args;
		const uint32_t last_var = plan->source_frame_variable_count;
		const tpde::SymRef leave_symbol =
			runtime_symbol(ZEND_NATIVE_HELPER_CALL_FAST_LEAVE);
		const tpde::SymRef release_symbol =
			runtime_symbol(ZEND_NATIVE_HELPER_CALL_FAST_RELEASE_CV);
		const tpde::SymRef release_this_symbol =
			runtime_symbol(ZEND_NATIVE_HELPER_CALL_FAST_RELEASE_THIS);
		const tpde::SymRef general_symbol =
			runtime_symbol(ZEND_NATIVE_HELPER_CALL_FAST_DO);
		const bool variadic = plan->fast_call_variadic;
		const tpde::SymRef variadic_symbol = variadic
			? runtime_symbol(ZEND_NATIVE_HELPER_CALL_RECEIVE_VARIADIC)
			: tpde::SymRef{};
		if (num_args > last_var || function_index >= this->func_syms.size()
				|| num_args > ZEND_NATIVE_CALL_FAST_RECEIVE_MAX
				|| !leave_symbol.valid() || !release_symbol.valid()
				|| !release_this_symbol.valid()
				|| !general_symbol.valid()
				|| (variadic && !variadic_symbol.valid())) {
			return;
		}
		/* No labels before the prologue (remove_prologue_bytes()): forward
		 * branches are rel32 jumps patched once their target is known. */
		auto branch = [&](bool conditional) {
			text_writer.ensure_space(16);
			if (conditional) {
				ASMF(JNZ, FE_JMPL, text_writer.cur_ptr());
			} else {
				ASMF(JMP, FE_JMPL, text_writer.cur_ptr());
			}
			return static_cast<uint32_t>(text_writer.offset());
		};
		auto branch_zero = [&]() {
			text_writer.ensure_space(16);
			ASMF(JZ, FE_JMPL, text_writer.cur_ptr());
			return static_cast<uint32_t>(text_writer.offset());
		};
		auto branch_below = [&]() {
			text_writer.ensure_space(16);
			ASMF(JC, FE_JMPL, text_writer.cur_ptr());
			return static_cast<uint32_t>(text_writer.offset());
		};
		auto branch_above = [&]() {
			text_writer.ensure_space(16);
			ASMF(JA, FE_JMPL, text_writer.cur_ptr());
			return static_cast<uint32_t>(text_writer.offset());
		};
		auto branch_not_above = [&]() {
			text_writer.ensure_space(16);
			ASMF(JBE, FE_JMPL, text_writer.cur_ptr());
			return static_cast<uint32_t>(text_writer.offset());
		};
		auto patch = [&](uint32_t branch_end, uint32_t target) {
			const int32_t displacement = static_cast<int32_t>(
				static_cast<int64_t>(target) - branch_end);
			std::memcpy(text_writer.begin_ptr() + branch_end - 4,
				&displacement, sizeof(displacement));
		};
		constexpr int32_t area = 56;
		auto member = [](size_t offset) {
			return static_cast<int32_t>(offset);
		};
		constexpr int32_t header_offset = -static_cast<int32_t>(
			sizeof(zend_native_user_call_site_header));
		const int32_t argument_count_offset = member(
			offsetof(zend_execute_data, This) + offsetof(zval, u2.num_args));
		const int32_t call_info_offset = member(
			offsetof(zend_execute_data, This)
				+ offsetof(zval, u1.type_info));
		const uint32_t min_arguments = plan->fast_call_min_arguments;
		const uint32_t start = text_writer.offset();
		/* Its own FDE: the CFA is RSP + 48 between the stack adjustments. */
		text_writer.eh_begin_fde();
		/*
		 * The frame the call site pushed (ADR 0025 section 3): an argument
		 * count this entry receives, no named or undefined arguments, and
		 * arguments of the types typed parameters take without a check.
		 * Any other frame continues, untouched, in
		 * zend_native_call_fast_do() with the same arguments.
		 */
		std::vector<uint32_t> to_general_do;
		ASM(MOV64rm, FE_AX, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, call))));
		ASM(MOV32rm, FE_R9, FE_MEM(FE_AX, 0, FE_NOREG,
			argument_count_offset));
		if (min_arguments != 0) {
			ASM(CMP32ri, FE_R9, static_cast<int32_t>(min_arguments));
			to_general_do.push_back(branch_below());
		}
		if (!variadic) {
			ASM(CMP32ri, FE_R9, static_cast<int32_t>(num_args));
			to_general_do.push_back(branch_above());
		}
		ASM(TEST32mi, FE_MEM(FE_AX, 0, FE_NOREG, call_info_offset),
			static_cast<int32_t>(ZEND_CALL_MAY_HAVE_UNDEF
				| ZEND_CALL_HAS_EXTRA_NAMED_PARAMS));
		to_general_do.push_back(branch(true));
		for (uint32_t parameter = 0; parameter < num_args; ++parameter) {
			const uint32_t mask = plan->fast_call_type_masks[parameter];
			if (mask == UINT32_MAX) {
				continue;
			}
			/* A missing argument takes its default, which fits. */
			uint32_t not_supplied = UINT32_MAX;
			if (parameter >= min_arguments) {
				ASM(CMP32ri, FE_R9, static_cast<int32_t>(parameter));
				not_supplied = branch_not_above();
			}
			ASM(MOVZXr32m8, FE_R10, FE_MEM(FE_AX, 0, FE_NOREG,
				member((ZEND_CALL_FRAME_SLOT + parameter) * sizeof(zval)
					+ offsetof(zval, u1.v.type))));
			ASM(MOV32ri, FE_R11, static_cast<int32_t>(mask));
			ASM(BT32rr, FE_R11, FE_R10);
			text_writer.ensure_space(16);
			ASMF(JNC, FE_JMPL, text_writer.cur_ptr());
			to_general_do.push_back(
				static_cast<uint32_t>(text_writer.offset()));
			if (not_supplied != UINT32_MAX) {
				patch(not_supplied, text_writer.offset());
			}
		}
		/* The frame fits. A dynamic site's frame (RCX, the target's Zend
		 * entry, is set) carries the target's run-time cache in its return
		 * value slot; the entry installs it below. */
		ASM(TEST64rr, FE_CX, FE_CX);
		{
			text_writer.ensure_space(16);
			ASMF(JZ, FE_JMPL, text_writer.cur_ptr());
			const uint32_t static_site = text_writer.offset();
			ASM(MOV64rm, FE_R10, FE_MEM(FE_AX, 0, FE_NOREG,
				member(offsetof(zend_execute_data, return_value))));
			ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG,
				member(offsetof(zend_execute_data, run_time_cache))), FE_R10);
			patch(static_site, text_writer.offset());
		}
		/* The exact entry: a site whose arguments the publication proved
		 * enters here (zend_native_code_exact_call_entry()). */
		const uint32_t exact = text_writer.offset();
		ASM(SUB64ri, FE_SP, area);
		const uint32_t cfi_location = text_writer.offset();
		text_writer.eh_advance(cfi_location - start);
		text_writer.eh_write_inst(
			tpde::dwarf::DW_CFA_def_cfa_offset, area + 8);
		ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 32), FE_SI);
		/* Unlink the frame from EX(call). */
		ASM(MOV64rm, FE_AX, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, call))));
		ASM(MOV64rm, FE_R9, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, prev_execute_data))));
		ASM(MOV64mr, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, call))), FE_R9);
		ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 0), FE_AX);
		/* The return value: the result slot, or the local zval. */
		ASM(MOV32rr, FE_R8, FE_R8);
		ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 8), FE_R8);
		ASM(LEA64rm, FE_R9, FE_MEM(FE_SP, 0, FE_NOREG, 16));
		ASM(LEA64rm, FE_R10, FE_MEM(FE_DI, 1, FE_R8, 0));
		ASM(CMP32ri, FE_R8, -1);
		ASM(CMOVNZ64rr, FE_R9, FE_R10);
		ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, return_value))), FE_R9);
		ASM(MOV32mi, FE_MEM(FE_R9, 0, FE_NOREG,
			member(offsetof(zval, u1.type_info))), IS_UNDEF);
		ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, prev_execute_data))), FE_DI);
		ASM(MOV64mi, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, call))), 0);
		/* The variadic parameter collects the extra arguments, which move
		 * behind the temporaries first; the call preserves nothing. */
		if (variadic) {
			ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 40), FE_DX);
			ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 48), FE_CX);
			ASM(MOV64rr, FE_DI, FE_AX);
			emit_symbol_call(variadic_symbol);
			ASM(MOV64rm, FE_AX, FE_MEM(FE_SP, 0, FE_NOREG, 0));
			ASM(MOV64rm, FE_DX, FE_MEM(FE_SP, 0, FE_NOREG, 40));
			ASM(MOV64rm, FE_CX, FE_MEM(FE_SP, 0, FE_NOREG, 48));
			ASM(MOV64rm, FE_SI, FE_MEM(FE_SP, 0, FE_NOREG, 32));
		}
		/* EX(opline) skips the RECV of every supplied parameter. */
		ASM(MOV64rm, FE_R9, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, func))));
		ASM(MOV64rm, FE_R9, FE_MEM(FE_R9, 0, FE_NOREG,
			member(offsetof(zend_op_array, opcodes))));
		if (num_args != 0) {
			static_assert(sizeof(zend_op) == 32);
			ASM(MOV32rm, FE_R10, FE_MEM(FE_AX, 0, FE_NOREG,
				argument_count_offset));
			if (variadic) {
				/* At most the declared parameters were received. */
				ASM(MOV32ri, FE_R11, static_cast<int32_t>(num_args));
				ASM(CMP32rr, FE_R10, FE_R11);
				ASM(CMOVA32rr, FE_R10, FE_R11);
			}
			ASM(SHL64ri, FE_R10, 5);
			ASM(ADD64rr, FE_R9, FE_R10);
		}
		ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, opline))), FE_R9);
		/* The other CVs start undefined (IS_UNDEF is zero): a zero zval
		 * per CV, a few stores straight, else four per loop iteration. A
		 * variadic parameter holds its array already. */
		const uint32_t first_local = num_args + (variadic ? 1 : 0);
		const uint32_t locals = last_var - first_local;
		const int32_t first_variable = static_cast<int32_t>(
			(ZEND_CALL_FRAME_SLOT + first_local) * sizeof(zval));
		if (locals == 1) {
			ASM(MOV32mi, FE_MEM(FE_AX, 0, FE_NOREG, first_variable
				+ static_cast<int32_t>(offsetof(zval, u1.type_info))),
				IS_UNDEF);
		} else if (locals != 0) {
			static_assert(IS_UNDEF == 0 && sizeof(zval) == 16);
			ASM(SSE_XORPDrr, FE_XMM0, FE_XMM0);
			uint32_t straight = locals;
			int32_t straight_base = first_variable;
			if (locals > 16) {
				/* No labels before the prologue (remove_prologue_bytes()):
				 * the backward branch is encoded directly. */
				ASM(LEA64rm, FE_R9, FE_MEM(FE_AX, 0, FE_NOREG,
					first_variable));
				ASM(MOV32ri, FE_R10, static_cast<int32_t>(locals / 4));
				const uint32_t loop = text_writer.offset();
				for (int32_t store = 0; store < 4; ++store) {
					ASM(SSE_MOVDQUmr, FE_MEM(FE_R9, 0, FE_NOREG,
						store * static_cast<int32_t>(sizeof(zval))), FE_XMM0);
				}
				ASM(ADD64ri, FE_R9, static_cast<int32_t>(4 * sizeof(zval)));
				ASM(SUB32ri, FE_R10, 1);
				text_writer.ensure_space(16);
				ASM(JNZ, text_writer.begin_ptr() + loop);
				straight = locals % 4;
				straight_base = first_variable + static_cast<int32_t>(
					(locals - straight) * sizeof(zval));
			}
			for (uint32_t variable = 0; variable < straight; ++variable) {
				ASM(SSE_MOVDQUmr, FE_MEM(FE_AX, 0, FE_NOREG, straight_base
					+ static_cast<int32_t>(variable * sizeof(zval))),
					FE_XMM0);
			}
		}
		/* A missing parameter takes its default literal, out of line. */
		uint32_t to_defaults = UINT32_MAX;
		uint32_t defaults_return = UINT32_MAX;
		if (min_arguments < num_args) {
			ASM(CMP32mi, FE_MEM(FE_AX, 0, FE_NOREG, argument_count_offset),
				static_cast<int32_t>(num_args));
			to_defaults = branch_below();
			defaults_return = text_writer.offset();
		}
		/* The site's run-time cache, or the target's one a dynamic site
		 * installed. */
		ASM(MOV64rm, FE_R9, FE_MEM(FE_DX, 0, FE_NOREG,
			header_offset + member(offsetof(
				zend_native_user_call_site_header, fast_run_time_cache))));
		ASM(TEST64rr, FE_CX, FE_CX);
		ASM(CMOVNZ64rm, FE_R9, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, run_time_cache))));
		ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG,
			member(offsetof(zend_execute_data, run_time_cache))), FE_R9);
		ASM(MOV64rm, FE_R9, FE_MEM(FE_SI, 0, FE_NOREG,
			member(offsetof(zend_native_execution_context,
				current_execute_data))));
		ASM(MOV64mr, FE_MEM(FE_R9, 0, FE_NOREG, 0), FE_AX);
		/* The Zend entry on the linked frame; RSI still holds the context. */
		ASM(MOV64rr, FE_DI, FE_AX);
		emit_symbol_call(this->func_syms[function_index]);
		/*
		 * The leave, as zend_native_call_fast_leave_inline() does it for a
		 * plain return: anything else leaves through
		 * zend_native_call_fast_leave(), which starts from this state.
		 */
		std::vector<uint32_t> to_general;
		ASM(MOV64rm, FE_DI, FE_MEM(FE_SP, 0, FE_NOREG, 0));
		ASM(TEST32rr, FE_AX, FE_AX);
		to_general.push_back(branch(true));
		ASM(MOV64rm, FE_R9, FE_MEM(FE_SP, 0, FE_NOREG, 32));
		ASM(MOV64rm, FE_R10, FE_MEM(FE_R9, 0, FE_NOREG,
			member(offsetof(zend_native_execution_context, exception))));
		ASM(CMP64mi, FE_MEM(FE_R10, 0, FE_NOREG, 0), 0);
		to_general.push_back(branch(true));
		ASM(MOV64rm, FE_R10, FE_MEM(FE_R9, 0, FE_NOREG,
			member(offsetof(zend_native_execution_context, vm_interrupt))));
		ASM(CMP8mi, FE_MEM(FE_R10, 0, FE_NOREG, 0), 0);
		to_general.push_back(branch(true));
		ASM(TEST32mi, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, This)
				+ offsetof(zval, u1.type_info))),
			static_cast<int32_t>(ZEND_CALL_HAS_SYMBOL_TABLE
				| ZEND_CALL_HAS_EXTRA_NAMED_PARAMS
				| ZEND_CALL_ALLOCATED | ZEND_CALL_FREE_EXTRA_ARGS));
		to_general.push_back(branch(true));
		/* An undefined result becomes null; a discarded counted result is
		 * released by the general leave. */
		ASM(MOV64rm, FE_R10, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, return_value))));
		ASM(CMP8mi, FE_MEM(FE_R10, 0, FE_NOREG,
			member(offsetof(zval, u1.v.type))), IS_UNDEF);
		{
			text_writer.ensure_space(16);
			ASMF(JNZ, FE_JMPL, text_writer.cur_ptr());
			const uint32_t defined = text_writer.offset();
			ASM(MOV32mi, FE_MEM(FE_R10, 0, FE_NOREG,
				member(offsetof(zval, u1.type_info))), IS_NULL);
			patch(defined, text_writer.offset());
		}
		ASM(CMP32mi, FE_MEM(FE_SP, 0, FE_NOREG, 8), -1);
		{
			text_writer.ensure_space(16);
			ASMF(JNZ, FE_JMPL, text_writer.cur_ptr());
			const uint32_t kept = text_writer.offset();
			ASM(TEST8mi, FE_MEM(FE_R10, 0, FE_NOREG,
				member(offsetof(zval, u1.v.type_flags))), IS_TYPE_REFCOUNTED);
			to_general.push_back(branch(true));
			patch(kept, text_writer.offset());
		}
		/* A variable destructor may inspect the backtrace: the dying frame
		 * is no longer current. */
		ASM(MOV64rm, FE_R10, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, prev_execute_data))));
		ASM(MOV64rm, FE_R11, FE_MEM(FE_R9, 0, FE_NOREG,
			member(offsetof(zend_native_execution_context,
				current_execute_data))));
		ASM(MOV64mr, FE_MEM(FE_R11, 0, FE_NOREG, 0), FE_R10);
		/* The CVs, as i_free_compiled_variables() releases them: a counted
		 * one out of line, after which the cursor is reloaded. Only CVs
		 * Zend's type inference lets hold a counted value are tested; a
		 * few of them each in turn, with the cursor set out of line. */
		std::vector<std::pair<uint32_t, uint32_t>> counted_branches;
		std::vector<int32_t> counted_slots;
		const uint64_t counted_cvs = plan->fast_call_counted_cvs
			& (last_var >= 64 ? ~UINT64_C(0)
				: (UINT64_C(1) << last_var) - 1);
		const bool counted_straight =
			std::popcount(counted_cvs) <= 8;
		if (counted_straight) {
			for (uint32_t variable = 0; variable < last_var;
					++variable) {
				if (((counted_cvs >> variable) & 1) == 0) {
					continue;
				}
				const int32_t slot = static_cast<int32_t>(
					(ZEND_CALL_FRAME_SLOT + variable) * sizeof(zval));
				ASM(TEST8mi, FE_MEM(FE_DI, 0, FE_NOREG, slot
					+ member(offsetof(zval, u1.v.type_flags))),
					IS_TYPE_REFCOUNTED);
				const uint32_t counted = branch(true);
				counted_branches.emplace_back(counted, text_writer.offset());
				counted_slots.push_back(slot);
			}
		} else if (last_var != 0) {
			ASM(LEA64rm, FE_R10, FE_MEM(FE_DI, 0, FE_NOREG,
				static_cast<int32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval))));
			ASM(LEA64rm, FE_R11, FE_MEM(FE_DI, 0, FE_NOREG,
				static_cast<int32_t>((ZEND_CALL_FRAME_SLOT + last_var)
					* sizeof(zval))));
			const uint32_t loop = text_writer.offset();
			ASM(TEST8mi, FE_MEM(FE_R10, 0, FE_NOREG,
				member(offsetof(zval, u1.v.type_flags))), IS_TYPE_REFCOUNTED);
			const uint32_t counted = branch(true);
			const uint32_t next = text_writer.offset();
			counted_branches.emplace_back(counted, next);
			ASM(ADD64ri, FE_R10, static_cast<int32_t>(sizeof(zval)));
			ASM(CMP64rr, FE_R10, FE_R11);
			text_writer.ensure_space(16);
			ASM(JNZ, text_writer.begin_ptr() + loop);
		}
		/* The frame lies on top of the VM stack. */
		ASM(MOV64rm, FE_DI, FE_MEM(FE_SP, 0, FE_NOREG, 0));
		ASM(MOV64rm, FE_R9, FE_MEM(FE_SP, 0, FE_NOREG, 32));
		ASM(MOV64rm, FE_R10, FE_MEM(FE_R9, 0, FE_NOREG,
			member(offsetof(zend_native_execution_context, vm_stack_top))));
		ASM(MOV64mr, FE_MEM(FE_R10, 0, FE_NOREG, 0), FE_DI);
		/* OBJ_RELEASE() of $this, else of a closure's object: GC_DELREF();
		 * a destroyed object or a possible GC root continues out of
		 * line. */
		ASM(MOV32rm, FE_AX, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, This)
				+ offsetof(zval, u1.type_info))));
		ASM(MOV64rm, FE_R9, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, This))));
		ASM(TEST32ri, FE_AX, static_cast<int32_t>(ZEND_CALL_RELEASE_THIS));
		const uint32_t receiver = branch(true);
		ASM(TEST32ri, FE_AX, static_cast<int32_t>(ZEND_CALL_CLOSURE));
		const uint32_t no_this = branch_zero();
		/* ZEND_CLOSURE_OBJECT(EX(func)). */
		ASM(MOV64rm, FE_R9, FE_MEM(FE_DI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, func))));
		ASM(SUB64ri, FE_R9, static_cast<int32_t>(sizeof(zend_object)));
		patch(receiver, text_writer.offset());
		ASM(SUB32mi, FE_MEM(FE_R9, 0, FE_NOREG, member(
			offsetof(zend_refcounted_h, refcount))), 1);
		const uint32_t this_released = branch_zero();
		ASM(TEST32mi, FE_MEM(FE_R9, 0, FE_NOREG, member(
			offsetof(zend_refcounted_h, u.type_info))),
			static_cast<int32_t>(
				GC_INFO_MASK | (GC_NOT_COLLECTABLE << GC_FLAGS_SHIFT)));
		const uint32_t this_candidate = branch_zero();
		patch(no_this, text_writer.offset());
		ASM(XOR32rr, FE_AX, FE_AX);
		const uint32_t epilogue = text_writer.offset();
		ASM(ADD64ri, FE_SP, area);
		text_writer.eh_advance(text_writer.offset() - cfi_location);
		text_writer.eh_write_inst(tpde::dwarf::DW_CFA_def_cfa_offset, 8);
		ASM(RET);
		/* Out of the return path, the CFA is RSP + area + 8 again. */
		const uint32_t out_of_line = text_writer.offset();
		text_writer.eh_advance(out_of_line - (epilogue + 4));
		text_writer.eh_write_inst(
			tpde::dwarf::DW_CFA_def_cfa_offset, area + 8);
		/* zend_native_call_fast_release_this(object, result, discarded),
		 * whose status the entry returns. The frame's memory is free: its
		 * result address is read before a destructor can reuse it. */
		patch(this_released, text_writer.offset());
		patch(this_candidate, text_writer.offset());
		ASM(MOV64rr, FE_DI, FE_R9);
		ASM(MOV64rm, FE_SI, FE_MEM(FE_SP, 0, FE_NOREG, 0));
		ASM(MOV64rm, FE_SI, FE_MEM(FE_SI, 0, FE_NOREG,
			member(offsetof(zend_execute_data, return_value))));
		ASM(XOR32rr, FE_DX, FE_DX);
		ASM(CMP32mi, FE_MEM(FE_SP, 0, FE_NOREG, 8), -1);
		ASM(SETZ8r, FE_DX);
		emit_symbol_call(release_this_symbol);
		text_writer.ensure_space(16);
		ASMF(JMP, FE_JMPL, text_writer.begin_ptr() + epilogue);
		for (size_t branch_index = 0; branch_index < counted_branches.size();
				++branch_index) {
			const auto &[counted, next] = counted_branches[branch_index];
			/* GC_DELREF(); a value still referenced that is no GC root
			 * candidate (not collectable, already buffered, no reference)
			 * needs nothing more, as gc_check_possible_root() decides. */
			patch(counted, text_writer.offset());
			if (counted_straight) {
				ASM(LEA64rm, FE_R10, FE_MEM(FE_DI, 0, FE_NOREG,
					counted_slots[branch_index]));
			}
			ASM(MOV64rm, FE_R9, FE_MEM(FE_R10, 0, FE_NOREG, 0));
			ASM(SUB32mi, FE_MEM(FE_R9, 0, FE_NOREG, member(
				offsetof(zend_refcounted_h, refcount))), 1);
			const uint32_t released = branch_zero();
			ASM(MOV32rm, FE_AX, FE_MEM(FE_R9, 0, FE_NOREG, member(
				offsetof(zend_refcounted_h, u.type_info))));
			ASM(TEST32ri, FE_AX, static_cast<int32_t>(
				GC_INFO_MASK | (GC_NOT_COLLECTABLE << GC_FLAGS_SHIFT)));
			const uint32_t candidate = branch_zero();
			ASM(AND32ri, FE_AX, GC_TYPE_MASK);
			ASM(CMP32ri, FE_AX, GC_REFERENCE);
			const uint32_t not_reference = branch(true);
			patch(not_reference, next);
			patch(released, text_writer.offset());
			patch(candidate, text_writer.offset());
			ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 40), FE_R10);
			ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 48), FE_R11);
			ASM(MOV64rr, FE_DI, FE_R10);
			emit_symbol_call(release_symbol);
			ASM(MOV64rm, FE_R10, FE_MEM(FE_SP, 0, FE_NOREG, 40));
			ASM(MOV64rm, FE_R11, FE_MEM(FE_SP, 0, FE_NOREG, 48));
			/* The straight tests address the frame. */
			ASM(MOV64rm, FE_DI, FE_MEM(FE_SP, 0, FE_NOREG, 0));
			const uint32_t back = branch(false);
			patch(back, next);
		}
		/* A missing parameter takes its default literal, as RECV_INIT
		 * does: from the last parameter down to the first one supplied. */
		if (to_defaults != UINT32_MAX) {
			/* From the last parameter down to the first one supplied, as
			 * RECV_INIT does; RAX still holds the frame. */
			patch(to_defaults, text_writer.offset());
			std::vector<uint32_t> defaults_done;
			ASM(MOV32rm, FE_R10, FE_MEM(FE_AX, 0, FE_NOREG,
				argument_count_offset));
			for (uint32_t parameter = num_args; parameter-- > min_arguments;) {
				const uint32_t literal_index =
					plan->fast_call_default_literals[parameter];
				const zval *literal = &plan->source_literals[literal_index];
				const int32_t slot = member(
					(ZEND_CALL_FRAME_SLOT + parameter) * sizeof(zval));
				ASM(CMP32ri, FE_R10, static_cast<int32_t>(parameter));
				defaults_done.push_back(branch_above());
				if (Z_TYPE_INFO_P(literal) <= IS_DOUBLE) {
					if (Z_TYPE_P(literal) == IS_LONG
							|| Z_TYPE_P(literal) == IS_DOUBLE) {
						uint64_t bits;
						std::memcpy(&bits, &literal->value, sizeof(bits));
						ASM(MOV64ri, FE_R11, static_cast<int64_t>(bits));
						ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG, slot), FE_R11);
					}
					ASM(MOV32mi, FE_MEM(FE_AX, 0, FE_NOREG,
						slot + member(offsetof(zval, u1.type_info))),
						static_cast<int32_t>(Z_TYPE_INFO_P(literal)));
				} else {
					/* An interned string or immutable array: copied from
					 * the literal table, which is the executing one. */
					ASM(MOV64rm, FE_R11, FE_MEM(FE_AX, 0, FE_NOREG,
						member(offsetof(zend_execute_data, func))));
					ASM(MOV64rm, FE_R11, FE_MEM(FE_R11, 0, FE_NOREG,
						member(offsetof(zend_op_array, literals))));
					ASM(MOV64rm, FE_R9, FE_MEM(FE_R11, 0, FE_NOREG,
						member(literal_index * sizeof(zval))));
					ASM(MOV64mr, FE_MEM(FE_AX, 0, FE_NOREG, slot), FE_R9);
					ASM(MOV32rm, FE_R9, FE_MEM(FE_R11, 0, FE_NOREG,
						member(literal_index * sizeof(zval)
							+ offsetof(zval, u1.type_info))));
					ASM(MOV32mr, FE_MEM(FE_AX, 0, FE_NOREG,
						slot + member(offsetof(zval, u1.type_info))), FE_R9);
				}
			}
			for (uint32_t done : defaults_done) {
				patch(done, text_writer.offset());
			}
			text_writer.ensure_space(16);
			ASMF(JMP, FE_JMPL, text_writer.begin_ptr() + defaults_return);
		}
		/* zend_native_call_fast_leave(callee, status, discarded). */
		const uint32_t general_leave = text_writer.offset();
		for (uint32_t general : to_general) {
			patch(general, general_leave);
		}
		ASM(MOV64rm, FE_DI, FE_MEM(FE_SP, 0, FE_NOREG, 0));
		ASM(MOV32rr, FE_SI, FE_AX);
		ASM(XOR32rr, FE_DX, FE_DX);
		ASM(CMP32mi, FE_MEM(FE_SP, 0, FE_NOREG, 8), -1);
		ASM(SETZ8r, FE_DX);
		emit_symbol_call(leave_symbol);
		ASM(ADD64ri, FE_SP, area);
		ASM(RET);
		/* Any other frame continues in zend_native_call_fast_do(), at the
		 * CFA of the entry before its stack adjustment. */
		const uint32_t general_do = text_writer.offset();
		text_writer.eh_advance(general_do - out_of_line);
		text_writer.eh_write_inst(tpde::dwarf::DW_CFA_def_cfa_offset, 8);
		for (uint32_t general : to_general_do) {
			patch(general, general_do);
		}
		text_writer.ensure_space(16);
		ASMF(JMP, FE_JMPL, text_writer.cur_ptr());
		reloc_text(general_symbol, tpde::elf::R_X86_64_PLT32,
			text_writer.offset() - 4, -4);
		text_writer.eh_end_fde();
		const tpde::SymRef symbol = assembler.sym_predef_func(
			"zend_native_fast_call_" + std::to_string(function_index),
			tpde::Assembler::SymBinding::GLOBAL);
		assembler.sym_def(symbol, text_writer.get_sec_ref(), start,
			text_writer.offset() - start);
		const tpde::SymRef exact_symbol = assembler.sym_predef_func(
			"zend_native_fast_call_exact_" + std::to_string(function_index),
			tpde::Assembler::SymBinding::GLOBAL);
		assembler.sym_def(exact_symbol, text_writer.get_sec_ref(), exact,
			text_writer.offset() - exact);
		text_writer.begin_func_after_prefix();
	}
	/*
	 * Entry specialization: before the first instruction of a Zend entry
	 * whose member has an integer variant, call the variant on the same frame
	 * when every checked argument slot holds an integer and return its
	 * status. The variant runs the whole body; the general entry continues
	 * only for other argument types.
	 */
	bool entry_variant_dispatch_pending_ = false;
	bool has_entry_variant() const {
		const zend_tpde_plan *plan = adaptor->plan();
		return !adaptor->typed_body() && plan != nullptr
			&& plan->entry_variant_member_plus_one != 0
			&& plan->entry_variant_long_mask != 0;
	}
	bool emit_entry_variant_dispatch() {
		const zend_tpde_plan *plan = adaptor->plan();
		if (plan->entry_variant_member_plus_one - 1
				>= this->func_syms.size()) {
			return false;
		}
		/* Before the first instruction only the prologue has run: RDI and
		 * RSI still hold the entry's frame and execution context. */
		auto general = text_writer.label_create();
		if (plan->deopt_resume_count != 0) {
			/* A deoptimization entry resumes this activation. */
			ASM(CMP8mi, FE_MEM(FE_SI, 0, FE_NOREG, static_cast<int32_t>(
				offsetof(zend_native_execution_context, deopt_resume))), 0);
			generate_raw_jump(Jump::jne, general);
		}
		for (uint32_t argument = 0; argument < 32; ++argument) {
			if (((plan->entry_variant_long_mask >> argument) & 1) == 0) {
				continue;
			}
			const int32_t type_offset = static_cast<int32_t>(
				(ZEND_CALL_FRAME_SLOT + argument) * sizeof(zval)
				+ offsetof(zval, u1.type_info));
			if (plan->entry_variant_numeric) {
				auto number = text_writer.label_create();
				ASM(CMP8mi, FE_MEM(FE_DI, 0, FE_NOREG, type_offset), IS_LONG);
				generate_raw_jump(Jump::je, number);
				ASM(CMP8mi, FE_MEM(FE_DI, 0, FE_NOREG, type_offset),
					IS_DOUBLE);
				generate_raw_jump(Jump::jne, general);
				label_place(number);
			} else {
				ASM(CMP8mi, FE_MEM(FE_DI, 0, FE_NOREG, type_offset), IS_LONG);
				generate_raw_jump(Jump::jne, general);
			}
		}
		/* A terminal cold path: the raw call leaves the allocator state of
		 * the general continuation untouched. */
		this->stack.generated_call = true;
		emit_symbol_call(this->func_syms[plan->entry_variant_member_plus_one - 1]);
		if (register_file.is_used(tpde::x64::AsmReg{tpde::x64::AsmReg::AX})) {
			return false;
		}
		ScratchReg status_scratch{this};
		status_scratch.alloc_specific(tpde::x64::AsmReg{tpde::x64::AsmReg::AX});
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
		status.set_value(this, std::move(status_scratch));
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status), tpde::CCAssignment{});
		return_builder.ret_local_path();
		label_place(general);
		return true;
	}
	void finish_func(uint32_t index) {
		if (typed_failure_label_.has_value()) {
			label_place(*typed_failure_label_);
			ASM(XOR32rr, FE_AX, FE_AX);
			ASM(XOR32rr, FE_DX, FE_DX);
			gen_func_epilog();
		}
		if (catch_dispatch_label_.has_value()) {
			label_place(*catch_dispatch_label_);
			emit_handler_dispatch(AsmReg{AsmReg::AX});
			gen_func_epilog();
		}
		const uint32_t frame_size = machine_frame_size();
		Base::finish_func(index);

		for (const auto &patch : current_direct_call_stack_guard_patches_) {
			const uint32_t offset = text_writer.label_offset(patch.label);
			const uint32_t target_frame_size =
				function_stack_frame_sizes_[patch.target_function];
			if (target_frame_size == UINT32_MAX) {
				pending_direct_call_stack_guard_patches_.push_back({
					offset, patch.target_function});
			} else {
				patch_direct_call_stack_guard(offset, target_frame_size);
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
			patch_direct_call_stack_guard(patch.offset, frame_size);
			pending_direct_call_stack_guard_patches_.erase(
				pending_direct_call_stack_guard_patches_.begin()
					+ static_cast<std::ptrdiff_t>(patch_index));
		}
	}
	void setup_var_ref_assignments() {
		for (uint32_t index = 0;
				index < adaptor->frame_slot_reference_count(); ++index) {
			init_variable_ref(adaptor->frame_slot_reference(index), index);
		}
	}
	void load_address_of_var_reference(
			AsmReg destination, ::tpde::AssignmentPartRef reference) {
		const IRValueRef value =
			adaptor->frame_slot_reference(reference.variable_ref_data());
		const zend_tpde_machine_reference *descriptor = nullptr;
		if (!adaptor->machine_reference(value, &descriptor)) {
			ZEND_UNREACHABLE();
		}
		AsmReg base;
		uint64_t offset;
		switch (descriptor->kind) {
			case ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT:
				base = canonical_frame_register();
				offset = static_cast<uint64_t>(descriptor->displacement);
				break;
			case ZEND_TPDE_MACHINE_REFERENCE_CONTEXT_FIELD:
			{
				auto context_ref = val_ref(
					IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
				auto context = context_ref.part(0);
				base = context.load_to_reg();
				offset = static_cast<uint64_t>(descriptor->displacement);
				if (offset <= INT32_MAX) {
					ASM(LEA64rm, destination,
						FE_MEM(base, 0, FE_NOREG,
							static_cast<int32_t>(offset)));
					return;
				}
				ASM(MOV64rr, destination, base);
				ScratchReg amount{this};
				auto amount_reg = amount.alloc_gp();
				materialize_constant(&offset,
					tpde::x64::PlatformConfig::GP_BANK, 8,
					amount_reg);
				ASM(ADD64rr, destination, amount_reg);
				return;
			}
			case ZEND_TPDE_MACHINE_REFERENCE_LITERAL:
				ASM(MOV64rm, destination,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, func))));
				ASM(MOV64rm, destination,
					FE_MEM(destination, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_op_array, literals))));
				offset = static_cast<uint64_t>(
						descriptor->stable_storage_or_layout_id)
					* descriptor->scale
					+ static_cast<uint64_t>(descriptor->displacement);
				if (offset <= INT32_MAX) {
					ASM(ADD64ri, destination,
						static_cast<int32_t>(offset));
					return;
				}
				{
					ScratchReg amount{this};
					auto amount_reg = amount.alloc_gp();
					materialize_constant(&offset,
						tpde::x64::PlatformConfig::GP_BANK, 8,
						amount_reg);
					ASM(ADD64rr, destination, amount_reg);
				}
				return;
			default:
				ZEND_UNREACHABLE();
		}
		if (offset <= INT32_MAX) {
			ASM(LEA64rm, destination,
				FE_MEM(base, 0, FE_NOREG, static_cast<int32_t>(offset)));
			return;
		}
		ASM(MOV64rr, destination, base);
		ScratchReg amount{this};
		auto amount_reg = amount.alloc_gp();
		materialize_constant(&offset,
			tpde::x64::PlatformConfig::GP_BANK, 8, amount_reg);
		ASM(ADD64rr, destination, amount_reg);
	}

	void emit_integer_dispatch(
		const zend_tpde_multi_branch_case *branch_cases,
		uint32_t branch_case_count,
		std::span<const tpde::Label> labels,
		tpde::x64::AsmReg value_reg,
		tpde::x64::AsmReg temp_reg,
		tpde::Label default_label);
	bool emit_machine_zval_type_info(
		zend_tpde_machine_value_kind kind,
		AsmReg payload_reg,
		AsmReg type_info_reg);
	bool emit_pointer_addref(
		zend_tpde_machine_value_kind kind,
		AsmReg payload_reg);
	bool emit_materializations(
		IRInstRef instruction, bool interrupt_slow_path = false);
	bool compile_boxed_cond_guard(IRInstRef instruction);
	bool compile_boxed_cond_cold(IRInstRef instruction);
	bool compile_boxed_cond_cold_branch(IRInstRef instruction);
	bool reload_generator_values(
		IRInstRef instruction, std::vector<ValueRef> &locked_values);
	bool compile_inst_impl(IRInstRef instruction, InstRange);
	bool compile_inst(IRInstRef instruction, InstRange);
	/* The Z_TYPE() bits a speculating tier-2 copy assumes for an
	 * instruction's result (zend_native_compiler_tier2_speculate_results()),
	 * or 0. */
	/*
	 * Whether a frameless call runs its handler directly from generated
	 * code: no observer is registered (part of the system id), every CV
	 * argument is defined and every temporary one holds no counted value,
	 * so the call needs neither a warning, an observer nor a release. The
	 * handler comes from the context's table, as images outlive the
	 * process (the OPcache file cache).
	 */
	bool frameless_inline_allowed(
			const zend_mir_executable_value_ref &operation,
			const zend_tpde_frameless_direct &direct) {
		const zend_tpde_plan *plan = adaptor->plan();
		const uint32_t position = operation.source_position_id;
		const uint32_t count =
			operation.source_opcode - ZEND_FRAMELESS_ICALL_0;
		const uint32_t handler = static_cast<uint32_t>(
			direct.descriptor & 0xffff);
		if (ZEND_OBSERVER_ENABLED || plan->source_opcodes == nullptr
				|| position >= plan->source_opcode_count
				|| (count == 3 && position + 1 >= plan->source_opcode_count)
				|| uint64_t{handler} * sizeof(void *) > INT32_MAX
				|| static_cast<uint32_t>(direct.slots) > INT32_MAX - sizeof(zval)
				|| uint64_t{position} * sizeof(zend_op) > INT32_MAX
				|| val_assignment(adaptor->val_local_idx(IRValueRef{
					Adaptor::EXECUTION_CONTEXT_ARGUMENT})) == nullptr) {
			return false;
		}
		for (uint32_t index = 0; index < count; ++index) {
			if (((direct.descriptor
					>> (ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT + index)) & 1)
					!= 0) {
				continue;
			}
			const uint32_t may_be = index == 0
				? plan->source_opcodes[position].op1_may_be
				: index == 1 ? plan->source_opcodes[position].op2_may_be
				: plan->source_opcodes[position + 1].op1_may_be;
			if (may_be == UINT32_MAX || (may_be & MAY_BE_UNDEF) != 0) {
				return false;
			}
			if (((direct.descriptor
					>> (ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT + index)) & 1)
					!= 0
					&& (may_be & (MAY_BE_STRING | MAY_BE_ARRAY
						| MAY_BE_OBJECT | MAY_BE_RESOURCE | MAY_BE_REF))
						!= 0) {
				return false;
			}
		}
		return true;
	}
};

uint32_t zval_type(const Adaptor &adaptor, IRValueRef value) {
	return zend_tpde_zval_type(adaptor.exact_type(value));
}

uint32_t zval_type(zend_mir_scalar_type_mask type) {
	return zend_tpde_zval_type(type);
}

void ZendCompilerX64::emit_integer_dispatch(
	const zend_tpde_multi_branch_case *branch_cases,
	uint32_t branch_case_count,
	std::span<const tpde::Label> labels,
	tpde::x64::AsmReg value_reg,
	tpde::x64::AsmReg temp_reg,
	tpde::Label default_label)
{
	std::vector<zend_tpde_integer_case> cases;
	int64_t low = 0;
	uint64_t range = 0;
	const zend_tpde_integer_dispatch_kind kind =
		zend_tpde_integer_dispatch(
			branch_cases, branch_case_count, &cases, &low, &range);
	auto emit_compare = [&](uint64_t expected, tpde::Label target) {
		materialize_constant(&expected,
			tpde::x64::PlatformConfig::GP_BANK, 8, temp_reg);
		ASM(CMP64rr, value_reg, temp_reg);
		generate_raw_jump(Jump::je, target);
	};
	if (kind == ZEND_TPDE_INTEGER_DISPATCH_LINEAR) {
		for (const zend_tpde_integer_case &entry : cases) {
			emit_compare(
				static_cast<uint64_t>(entry.value),
				labels[entry.label_index]);
		}
		generate_raw_jump(Jump::jmp, default_label);
		return;
	}

	const uint64_t low_bits = static_cast<uint64_t>(low);
	materialize_constant(&low_bits,
		tpde::x64::PlatformConfig::GP_BANK, 8, temp_reg);
	ASM(SUB64rr, value_reg, temp_reg);
	if (kind == ZEND_TPDE_INTEGER_DISPATCH_JUMP_TABLE) {
		const uint64_t high_index = range - 1;
		materialize_constant(&high_index,
			tpde::x64::PlatformConfig::GP_BANK, 8, temp_reg);
		ASM(CMP64rr, value_reg, temp_reg);
		generate_raw_jump(Jump::ja, default_label);
		auto &table = text_writer.create_jump_table(
			static_cast<uint32_t>(range), value_reg, temp_reg);
		std::ranges::fill(table.labels(), default_label);
		for (const zend_tpde_integer_case &entry : cases) {
			const uint64_t index =
				static_cast<uint64_t>(entry.value) - low_bits;
			table.labels()[index] = labels[entry.label_index];
		}
		return;
	}

	auto emit_balanced = [&](size_t begin, size_t end, auto &&self) -> void {
		const size_t count = end - begin;
		if (count <= 4) {
			for (size_t index = begin; index < end; ++index) {
				emit_compare(
					static_cast<uint64_t>(cases[index].value) - low_bits,
					labels[cases[index].label_index]);
			}
			generate_raw_jump(Jump::jmp, default_label);
			return;
		}
		const size_t middle = begin + count / 2;
		const uint64_t pivot =
			static_cast<uint64_t>(cases[middle].value) - low_bits;
		const tpde::Label greater = text_writer.label_create();
		emit_compare(pivot, labels[cases[middle].label_index]);
		generate_raw_jump(Jump::ja, greater);
		self(begin, middle, self);
		label_place(greater);
		self(middle + 1, end, self);
	};
	emit_balanced(0, cases.size(), emit_balanced);
}

bool ZendCompilerX64::emit_machine_zval_type_info(
		zend_tpde_machine_value_kind kind,
		AsmReg payload_reg,
		AsmReg type_info_reg) {
	if (kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
		const uint32_t type =
			zend_tpde_machine_value_zval_type(kind);
		const auto borrowed = text_writer.label_create();
		const auto ready = text_writer.label_create();
		ASM(MOV32rm, type_info_reg,
			FE_MEM(payload_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, u.type_info))));
		ASM(TEST32ri, type_info_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::jne, borrowed);
		ASM(MOV32ri, type_info_reg,
			type | (IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT));
		generate_raw_jump(Jump::jmp, ready);
		label_place(borrowed);
		ASM(MOV32ri, type_info_reg, type);
		label_place(ready);
		return true;
	}
	if (kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
		const auto immutable = text_writer.label_create();
		const auto ready = text_writer.label_create();

		ASM(MOV32rm, type_info_reg,
			FE_MEM(payload_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, u.type_info))));
		ASM(TEST32ri, type_info_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::jne, immutable);
		ASM(MOV32ri, type_info_reg, IS_ARRAY_EX);
		generate_raw_jump(Jump::jmp, ready);
		label_place(immutable);
		ASM(MOV32ri, type_info_reg, IS_ARRAY);
		label_place(ready);
		return true;
	}
	const uint32_t type_info =
		zend_tpde_machine_value_zval_type_info(kind);
	if (type_info == IS_UNDEF) {
		return false;
	}
	ASM(MOV32ri, type_info_reg, type_info);
	return true;
}

bool ZendCompilerX64::emit_pointer_addref(
		zend_tpde_machine_value_kind kind,
		AsmReg payload_reg) {
	if (kind != ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			&& kind != ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			&& kind != ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
			&& kind != ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
			&& kind != ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR) {
		return false;
	}
	if (kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
		const auto copied = text_writer.label_create();
		ScratchReg header{this};
		auto header_reg = header.alloc_gp();
		ASM(MOV32rm, header_reg,
			FE_MEM(payload_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, u.type_info))));
		ASM(TEST32ri, header_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::jne, copied);
		ASM(ADD32mi,
			FE_MEM(payload_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(copied);
		return true;
	}
	ASM(ADD32mi,
		FE_MEM(payload_reg, 0, FE_NOREG,
			static_cast<int32_t>(
				offsetof(zend_refcounted_h, refcount))),
		1);
	return true;
}

bool ZendCompilerX64::emit_materializations(
		IRInstRef instruction, bool interrupt_slow_path) {
	if (adaptor->typed_body()) {
		return true;
	}
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const auto materializations = adaptor->materializations(instruction);
	if (materializations.empty()) {
		return true;
	}
	const zend_mir_instruction_record record =
		adaptor->mir_instruction(instruction).record;
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
	/* Keep the canonical frame assignment locked while materializing every
	 * value. Loading a later operand may otherwise evict the frame and reuse
	 * its raw register before the next store. */
	const auto frame_local = adaptor->val_local_idx(
		IRValueRef{Adaptor::FRAME_VALUE});
	auto *frame_assignment = val_assignment(frame_local);
	ZEND_ASSERT(frame_assignment != nullptr);
	ValuePartRef frame_value{
		this, frame_local, frame_assignment, 0, false};
	const AsmReg frame_reg = frame_value.load_to_reg();
	for (uint32_t index = 0; index < materializations.size(); ++index) {
		const zend_tpde_materialization &materialization =
			materializations[index];
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + materialization.storage_id)
				* sizeof(zval);
		if (!zend_mir_id_is_valid(materialization.storage_id)
				|| offset > static_cast<uint64_t>(INT32_MAX)
					- sizeof(zval)) {
			return false;
		}
		const IRValueRef value =
			node.liveness_operands[
				node.materialization_operand_index + index];
		const zend_tpde_machine_value_kind machine_kind =
			adaptor->machine_kind(value);
		auto value_ref = val_ref(value);
		/* A value loaded from its own CV slot is still there: a write to
		 * the CV defines a new value. The frame stands in for a value the
		 * adaptor found in its slot. */
		if (value == IRValueRef{Adaptor::FRAME_VALUE}
				|| (materialization.storage_id
						< adaptor->plan()->source_frame_variable_count
					&& adaptor->slot_load_storage(value)
						== materialization.storage_id)) {
			continue;
		}
		auto payload = value_ref.part(0);
		AsmReg boolean_payload_reg = AsmReg::make_invalid();
		AsmReg payload_reg = AsmReg::make_invalid();
		if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_F64) {
			payload_reg = payload.load_to_reg();
			ASM(SSE_MOVSDmr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offset)),
				payload_reg);
		} else if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOOL
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
			payload_reg = payload.load_to_reg();
			if (machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
				boolean_payload_reg = payload_reg;
			}
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offset)),
				payload_reg);
		} else {
			if (!EncodeBase::encode_zend_native_store_u64(
					GenericValuePart{GenericValuePart::Expr{
						frame_reg, static_cast<int64_t>(offset)}},
					GenericValuePart{std::move(payload)})) {
				return false;
			}
		}
		const int32_t type_offset =
			static_cast<int32_t>(offset + offsetof(zval, u1.type_info));
		if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto type_info = value_ref.part(1);
			auto type_info_reg = type_info.load_to_reg();
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG, type_offset),
				type_info_reg);
		} else if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOOL) {
			ScratchReg type_info{this};
			auto type_info_reg = type_info.alloc_gp();
			ASM(MOV64rr, type_info_reg, boolean_payload_reg);
			ASM(ADD64ri, type_info_reg, IS_FALSE);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG, type_offset),
				type_info_reg);
		} else {
			ScratchReg type_info{this};
			auto type_info_reg = type_info.alloc_gp();
			if (!emit_machine_zval_type_info(
					machine_kind, payload_reg, type_info_reg)) {
				return false;
			}
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG, type_offset),
				type_info_reg);
		}
	}
	return true;
}

bool ZendCompilerX64::compile_boxed_cond_guard(IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const zend_tpde_instruction &mir =
		adaptor->mir_instruction(instruction);
	zend_tpde_value_condition layout;
	const bool register_string = node.operands.size() == 2
		&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE}
		&& adaptor->machine_kind(node.operands[1])
			== ZEND_TPDE_MACHINE_VALUE_STRING_PTR;
	const bool register_boxed = node.operands.size() == 2
		&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE}
		&& adaptor->machine_kind(node.operands[1])
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	if ((node.operands.size() != 1 && !register_string && !register_boxed)
			|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			|| !zend_tpde_value_condition_at(mir, &layout)
			|| layout.operand_offset > INT32_MAX
			|| node.argument_index == UINT32_MAX) {
		return false;
	}
	const auto successors = adaptor->block_succs(
		IRBlockRef{node.control_block});
	if (successors.size() < 3
			|| static_cast<uint32_t>(successors[2])
				!= node.argument_index) {
		return false;
	}
	auto [frame_ref, frame] = val_ref_single(node.operands[0]);
	auto frame_reg = frame.load_to_reg();
	ScratchReg type{this};
	ScratchReg value{this};
	ScratchReg decision{this};
	auto type_reg = type.alloc_gp();
	auto value_reg = value.alloc_gp();
	auto decision_reg = decision.alloc_gp();
	auto slow = text_writer.label_create();
	auto truthy = text_writer.label_create();
	auto falsey = text_writer.label_create();
	auto ready = text_writer.label_create();
	/* A CV that may hold a reference keeps its value inside the reference
	 * in its slot; storing the loaded value there would replace it. */
	const bool publish_operand =
		!(mir.value_operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& !mir.source_op1_reference_free);

	if (register_boxed && publish_operand) {
		/*
		 * The preceding fast node may leave a TMP register-authoritative.
		 * Materialize that value before the boxed branch observes the
		 * canonical Zend frame or delegates uncommon truthiness to the helper.
		 */
		auto boxed = val_ref(node.operands[1]);
		auto payload = boxed.part(0);
		auto type_info = boxed.part(1);
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)),
			payload.load_to_reg());
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset
					+ offsetof(zval, u1.type_info))),
			type_info.load_to_reg());
	}

	/*
	 * Branch straight to the successor blocks instead of deciding through a
	 * register and a switch whenever no successor needs PHI moves (a register
	 * string without a result never does). Every path jumps before the branch
	 * region is emitted, so spill live values here, ahead of the first jump;
	 * spilling at the region itself would place the stores on an unreachable
	 * fall-through.
	 */
	const bool direct_successor_branches =
		(register_string && !layout.has_result)
		|| (!branch_needs_split(successors[0])
			&& !branch_needs_split(successors[1])
			&& !branch_needs_split(successors[2]));
	decltype(spill_before_branch()) spilled{};
	if (direct_successor_branches) {
		spilled = spill_before_branch();
	}

	if (register_boxed && !publish_operand) {
		auto boxed = val_ref(node.operands[1]);
		(void) boxed;
	}
	if (register_string && layout.has_result && !publish_operand) {
		auto [string_ref, string] = val_ref_single(node.operands[1]);
		(void) string;
	} else if (register_string && layout.has_result) {
		auto [string_ref, string] = val_ref_single(node.operands[1]);
		auto string_reg = string.load_to_reg();
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)),
			string_reg);
		ScratchReg string_type{this};
		auto string_type_reg = string_type.alloc_gp();
		if (!emit_machine_zval_type_info(
				ZEND_TPDE_MACHINE_VALUE_STRING_PTR,
				string_reg, string_type_reg)) {
			return false;
		}
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset
					+ offsetof(zval, u1.type_info))),
			string_type_reg);
	}

	/*
	 * JMPZ_EX/JMPNZ_EX consume their source TMP before publishing the boolean
	 * result. The generic helper owns that lifetime transition; the scalar
	 * truthiness fast path only observes the slot and must not overwrite a
	 * refcounted source value directly. Register-authoritative values have been
	 * materialized above so the helper observes the canonical frame value.
	 */
	if (layout.has_result
			&& (node.materialization_count != 0 || node.cold_phi_inputs)) {
		/* The cold block alone materializes the condition into its slot
		 * or defines the PHI inputs of the branch. */
		generate_raw_jump(Jump::jmp, slow);
	} else if (layout.has_result) {
		/* A null, boolean or integer owns nothing: its truth decides inline
		 * and the result is published on both edges, as the VM's
		 * JMPZ_EX/JMPNZ_EX publish it. */
		ASM(MOVZXr32m8, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset
					+ offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_TRUE);
		generate_raw_jump(Jump::je, truthy);
		ASM(CMP32ri, type_reg, IS_NULL);
		generate_raw_jump(Jump::jb, slow);
		ASM(CMP32ri, type_reg, IS_FALSE);
		generate_raw_jump(Jump::jbe, falsey);
		ASM(CMP32ri, type_reg, IS_LONG);
		generate_raw_jump(Jump::jne, slow);
		ASM(CMP64mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)), 0);
		generate_raw_jump(Jump::jne, truthy);
		generate_raw_jump(Jump::jmp, falsey);
	} else if (register_string) {
		bool literal_truthy = false;
		if (adaptor->known_string_literal(
				node.operands[1], nullptr, &literal_truthy)) {
			auto [string_ref, string] = val_ref_single(node.operands[1]);
			string.reset();
			string_ref.reset();
			generate_raw_jump(
				Jump::jmp, literal_truthy ? truthy : falsey);
		} else {
			auto [string_ref, string] = val_ref_single(node.operands[1]);
			auto string_reg = string.load_to_reg();
			ASM(MOV64rm, type_reg,
				FE_MEM(string_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_string, len))));
			ASM(TEST64rr, type_reg, type_reg);
			generate_raw_jump(Jump::je, falsey);
			ASM(CMP64ri, type_reg, 1);
			generate_raw_jump(Jump::jne, truthy);
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(string_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_string, val))));
			ASM(CMP32ri, type_reg, '0');
			generate_raw_jump(Jump::je, falsey);
			generate_raw_jump(Jump::jmp, truthy);
		}
	} else if (!layout.has_result
			&& (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I64)) {
		if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			ASM(MOVZXr32m8, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset
						+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, value_reg, IS_TRUE);
		} else {
			ASM(MOV64rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(TEST64rr, value_reg, value_reg);
		}
		generate_raw_jump(node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
			? Jump::je : Jump::jne, truthy);
		generate_raw_jump(Jump::jmp, falsey);
	} else {
		ASM(MOVZXr32m8, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset
					+ offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_NULL);
		generate_raw_jump(Jump::je, falsey);
		ASM(CMP32ri, type_reg, IS_FALSE);
		generate_raw_jump(Jump::je, falsey);
		ASM(CMP32ri, type_reg, IS_TRUE);
		generate_raw_jump(Jump::je, truthy);
		ASM(CMP32ri, type_reg, IS_LONG);
		auto not_long = text_writer.label_create();
		generate_raw_jump(Jump::jne, not_long);
		ASM(MOV64rm, value_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)));
		ASM(TEST64rr, value_reg, value_reg);
		generate_raw_jump(Jump::jne, truthy);
		generate_raw_jump(Jump::jmp, falsey);

		label_place(not_long);
		ASM(CMP32ri, type_reg, IS_STRING);
		auto not_string = text_writer.label_create();
		generate_raw_jump(Jump::jne, not_string);
		ASM(MOV64rm, value_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)));
		ASM(MOV64rm, type_reg,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, len))));
		ASM(TEST64rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, falsey);
		ASM(CMP64ri, type_reg, 1);
		generate_raw_jump(Jump::jne, truthy);
		ASM(MOVZXr32m8, type_reg,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, val))));
		ASM(CMP32ri, type_reg, '0');
		generate_raw_jump(Jump::je, falsey);
		generate_raw_jump(Jump::jmp, truthy);

		label_place(not_string);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		auto not_array = text_writer.label_create();
		generate_raw_jump(Jump::jne, not_array);
		ASM(MOV64rm, value_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)));
		ASM(MOV32rm, type_reg,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumOfElements))));
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, truthy);
		generate_raw_jump(Jump::jmp, falsey);

		label_place(not_array);
		ASM(CMP32ri, type_reg, IS_RESOURCE);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, value_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)));
		ASM(MOV32rm, type_reg,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_resource, handle))));
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, truthy);
		generate_raw_jump(Jump::jmp, falsey);
	}

	if (direct_successor_branches) {
		/* The result tails still store through the frame register. */
		if (!layout.has_result) {
			frame.reset();
		}
		type.reset();
		value.reset();
		decision.reset();
		/* Without a result the tails only forward (jump threading resolves
		 * the jumps to them): out of the hot code. With a result they publish
		 * the boolean, as the VM's JMPZ_EX/JMPNZ_EX do, and stay hot. */
		const bool cold_tails = !layout.has_result
			&& !text_writer.in_cold_area();
		if (cold_tails) {
			cold_begin();
		}
		begin_branch_region();
		label_place(truthy);
		if (layout.has_result) {
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_TRUE);
		}
		generate_branch_to_block(
			Jump::jmp, successors[0], false, false);
		label_place(falsey);
		if (layout.has_result) {
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_FALSE);
		}
		generate_branch_to_block(
			Jump::jmp, successors[1], false, false);
		label_place(slow);
		generate_branch_to_block(
			Jump::jmp, successors[2], false, false);
		end_branch_region();
		if (cold_tails) {
			cold_end();
		}
		if (layout.has_result) {
			frame.reset();
		}
		release_spilled_regs(spilled);
		return true;
	}

	label_place(truthy);
	if (layout.has_result) {
		ASM(MOV64mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.result_offset)), 1);
		ASM(MOV32mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.result_offset + offsetof(zval, u1.type_info))),
			IS_TRUE);
	}
	ASM(MOV32ri, decision_reg, 1);
	generate_raw_jump(Jump::jmp, ready);
	label_place(falsey);
	if (layout.has_result) {
		ASM(MOV64mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.result_offset)), 0);
		ASM(MOV32mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.result_offset + offsetof(zval, u1.type_info))),
			IS_FALSE);
	}
	ASM(MOV32ri, decision_reg, 0);
	generate_raw_jump(Jump::jmp, ready);
	label_place(slow);
	ASM(MOV32ri, decision_reg, 2);
	label_place(ready);
	type.reset();
	value.reset();
	std::array<std::pair<uint64_t, IRBlockRef>, 2> cases{{
		{1, successors[0]},
		{2, successors[2]},
	}};
	generate_switch(std::move(decision), 32, successors[1], cases);
	return true;
}

bool ZendCompilerX64::compile_boxed_cond_cold(IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const zend_tpde_instruction &mir =
		adaptor->mir_instruction(instruction);
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.argument_index});
	if (node.operands.size() != 1 || !mir.has_value_operation
			|| successors.size() != 2) {
		return false;
	}
	/*
	 * JMPZ/JMPNZ of a CV and JMPZ_EX/JMPNZ_EX of null, a boolean or an
	 * integer decide here without the helper, publishing the boolean result
	 * of &&/||, as the helper's own fast path does; the decision joins the
	 * helper's through a stack slot. Every value is spilled first, so both
	 * paths reach the join with the same register state.
	 */
	zend_tpde_value_condition scalar_layout;
	const bool scalar_inline = zend_tpde_value_condition_at(mir, &scalar_layout)
		&& scalar_layout.operand_offset <= INT32_MAX - sizeof(zval)
		&& scalar_layout.result_offset <= INT32_MAX - sizeof(zval);
	int32_t scalar_slot = 0;
	auto scalar_join = text_writer.label_create();
	auto scalar_helper = text_writer.label_create();
	if (scalar_inline) {
		scalar_slot = inst_stack_slot(sizeof(uint32_t));
		release_spilled_regs(spill_before_branch(true));
		const auto frame_reg = canonical_frame_register();
		ScratchReg type{this};
		ScratchReg truth{this};
		const auto type_reg = type.alloc_gp();
		const auto truth_reg = truth.alloc_gp();
		const int32_t operand =
			static_cast<int32_t>(scalar_layout.operand_offset);
		auto is_long = text_writer.label_create();
		auto decided = text_writer.label_create();
		ASM(MOVZXr32m8, type_reg, FE_MEM(frame_reg, 0, FE_NOREG,
			operand + static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_LONG);
		generate_raw_jump(Jump::je, is_long);
		generate_raw_jump(Jump::ja, scalar_helper);
		ASM(CMP32ri, type_reg, IS_NULL);
		generate_raw_jump(Jump::jb, scalar_helper);
		ASM(XOR32rr, truth_reg, truth_reg);
		ASM(CMP32ri, type_reg, IS_TRUE);
		generate_raw_set(Jump::je, truth_reg);
		generate_raw_jump(Jump::jmp, decided);
		label_place(is_long);
		ASM(XOR32rr, truth_reg, truth_reg);
		ASM(CMP64mi, FE_MEM(frame_reg, 0, FE_NOREG, operand), 0);
		generate_raw_set(Jump::jne, truth_reg);
		label_place(decided);
		if (scalar_layout.has_result) {
			const int32_t result =
				static_cast<int32_t>(scalar_layout.result_offset);
			ASM(LEA32rm, type_reg,
				FE_MEM(truth_reg, 0, FE_NOREG, IS_FALSE));
			ASM(MOV32mr, FE_MEM(frame_reg, 0, FE_NOREG,
				result + static_cast<int32_t>(offsetof(zval, u1.type_info))),
				type_reg);
		}
		ASM(MOV32mr, FE_MEM(FE_BP, 0, FE_NOREG, scalar_slot), truth_reg);
		generate_raw_jump(Jump::jmp, scalar_join);
		label_place(scalar_helper);
	}
	tpde::x64::CCAssignerSysV assigner{false};
	CallBuilder builder{*this, assigner};
	builder.add_arg(CallArg{node.operands[0]});
	const zend_mir_executable_value_ref &operation = mir.value_operation;
	call_value_operation(builder, operation,
		mir.runtime_helper);
	ValuePart decision{tpde::x64::PlatformConfig::GP_BANK};
	builder.add_ret(decision, tpde::CCAssignment{});
	auto decision_scratch = std::move(decision).into_scratch(this);
	auto decision_reg = decision_scratch.cur_reg();
	emit_decision_exception_check(decision_reg, mir.exception_block_id,
		[&] { decision_scratch.reset(); });
	if (scalar_inline) {
		ASM(MOV32mr, FE_MEM(FE_BP, 0, FE_NOREG, scalar_slot), decision_reg);
		decision_scratch.reset();
		label_place(scalar_join);
		decision_scratch.alloc_gp();
		decision_reg = decision_scratch.cur_reg();
		ASM(MOV32rm, decision_reg, FE_MEM(FE_BP, 0, FE_NOREG, scalar_slot));
	}
	if (node.has_result) {
		auto result = result_ref(node.result);
		auto value = result.part(0);
		auto value_reg = value.alloc_reg();
		ASM(MOV32rr, value_reg, decision_reg);
		value.set_modified();
		return true;
	}
	ASM(TEST32rr, decision_reg, decision_reg);
	generate_cond_branch(Jump::jne, successors[0], successors[1]);
	return true;
}

bool ZendCompilerX64::compile_boxed_cond_cold_branch(
		IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.argument_index});
	if (node.operands.size() != 1 || successors.size() != 2) {
		return false;
	}
	auto [decision_ref, decision] = val_ref_single(node.operands[0]);
	auto decision_reg = decision.load_to_reg();
	ASM(TEST32rr, decision_reg, decision_reg);
	generate_cond_branch(Jump::jne, successors[0], successors[1]);
	return true;
}

bool ZendCompilerX64::reload_generator_values(
		IRInstRef instruction, std::vector<ValueRef> &locked_values) {
	auto context_use = val_ref(
		IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
	if (!context_use.has_assignment() || context_use.variable_ref()) {
		return false;
	}
	auto context_value = context_use.part(0);
	const auto context_reg = context_value.load_to_reg();
	ValuePart resumed_frame{tpde::x64::PlatformConfig::GP_BANK, 8};
	const auto frame_reg = resumed_frame.alloc_reg(this);
	ASM(MOV64rm, frame_reg,
		FE_MEM(context_reg, 0, FE_NOREG,
			static_cast<int32_t>(offsetof(
				zend_native_execution_context, current_execute_data))));
	ASM(MOV64rm, frame_reg, FE_MEM(frame_reg, 0, FE_NOREG, 0));
	for (const IRValueRef operand :
			adaptor->generator_resume_values(instruction)) {
		const zend_mir_storage_id storage =
			adaptor->canonical_storage(operand);
		const zend_tpde_machine_value_kind machine_kind =
			adaptor->machine_kind(operand);
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		if (!zend_mir_id_is_valid(storage)
				|| offset + offsetof(zval, u1.type_info) > INT32_MAX) {
			return false;
		}
		auto value = val_ref(operand);
		if (!value.has_assignment() || value.variable_ref()) {
			return false;
		}
		const ValueParts parts = val_parts(operand);
		for (uint32_t part = 0; part < parts.count(); ++part) {
			auto assignment = value.part_unowned(part).assignment();
			if (!assignment.register_valid()) {
				continue;
			}
			const auto stale_reg = assignment.get_reg();
			const bool owns_register = register_file.is_used(stale_reg)
				&& register_file.reg_local_idx(stale_reg)
					== adaptor->val_local_idx(operand)
				&& register_file.reg_part(stale_reg) == part;
			if (owns_register) {
				if (assignment.fixed_assignment()) {
					register_file.dec_lock_count_must_zero(stale_reg);
					--assignments.cur_fixed_assignment_count[
						assignment.bank().id()];
				} else if (register_file.is_fixed(stale_reg)) {
					return false;
				}
				register_file.unmark_used(stale_reg);
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
			if (machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL
					&& role == ZEND_TPDE_MACHINE_PART_VALUE) {
				ASM(MOV32rm, value_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offset + offsetof(zval, u1.type_info))));
				ASM(CMP32ri, value_reg, IS_TRUE);
				generate_raw_set(Jump::je, value_reg);
			} else if (machine_kind == ZEND_TPDE_MACHINE_VALUE_F64
					&& role == ZEND_TPDE_MACHINE_PART_VALUE) {
				ASM(SSE_MOVSDrm, value_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offset)));
			} else if (role == ZEND_TPDE_MACHINE_PART_VALUE
					|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				ASM(MOV64rm, value_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offset)));
			} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
				ASM(MOV32rm, value_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offset + offsetof(zval, u1.type_info))));
			} else {
				return false;
			}
			value_part.set_modified();
			/* A resume continuation may branch to an earlier-compiled block.
			 * Publish the authoritative frame reload before that block restores
			 * the value from TPDE's canonical spill slot. */
			spill(value_part.assignment());
		}
		locked_values.push_back(std::move(value));
	}
	resumed_frame.reset(this);
	return true;
}

bool ZendCompilerX64::compile_inst_impl(
	IRInstRef instruction, InstRange remaining_instructions) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	cold_sections_at_inst_ = cold_sections_.size();
	std::vector<ValueRef> generator_reload_locks;
	if (node.deopt_landing != UINT32_MAX
			&& !emit_deopt_landing(node.deopt_landing)) {
		return false;
	}
	if (!emit_materializations(instruction)
			|| !emit_deopt_stores(instruction)) {
		return false;
	}
	if (node.kind == Adaptor::InstKind::GuardedCold && node.deopt_exit) {
		return emit_deopt_exit(instruction);
	}
	if (node.kind != Adaptor::InstKind::GeneratorResume
			&& !adaptor->generator_resume_values(instruction).empty()
			&& !reload_generator_values(
				instruction, generator_reload_locks)) {
		return false;
	}
	if (node.kind == Adaptor::InstKind::BoxedCondGuard) {
		return compile_boxed_cond_guard(instruction);
	}
	if (node.kind == Adaptor::InstKind::BoxedCondCold) {
		return compile_boxed_cond_cold(instruction);
	}
	if (node.kind == Adaptor::InstKind::BoxedCondColdBranch) {
		return compile_boxed_cond_cold_branch(instruction);
	}
	if (node.kind == Adaptor::InstKind::ScalarSelect) {
		if (node.operands.size() != 3 || !node.has_result
				|| adaptor->exact_type(node.operands[0])
					!= ZEND_MIR_SCALAR_TYPE_I1
				|| adaptor->machine_kind(node.operands[0])
					!= ZEND_TPDE_MACHINE_VALUE_BOOL
				|| adaptor->exact_type(node.operands[1])
					!= adaptor->exact_type(node.result)
				|| adaptor->exact_type(node.operands[2])
					!= adaptor->exact_type(node.result)
				|| adaptor->machine_kind(node.operands[1])
					!= adaptor->machine_kind(node.result)
				|| adaptor->machine_kind(node.operands[2])
					!= adaptor->machine_kind(node.result)
				|| (adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_I64
					&& adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_BOOL)) {
			return false;
		}
		auto [condition_ref, condition] = val_ref_single(node.operands[0]);
		auto [true_ref, true_value] = val_ref_single(node.operands[1]);
		auto [false_ref, false_value] = val_ref_single(node.operands[2]);
		auto condition_reg = condition.load_to_reg();
		auto true_reg = true_value.load_to_reg();
		auto false_reg = false_value.load_to_reg();
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		/* result = condition ? true_value : false_value */
		ASM(MOV64rr, result_reg, false_reg);
		ASM(TEST64rr, condition_reg, condition_reg);
		generate_raw_cmov(Jump::jne, result_reg, true_reg, true);
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::UnboxPointer) {
		const zend_tpde_machine_value_kind result_kind =
			adaptor->machine_kind(node.result);
		if (node.operands.size() != 1 || !node.has_result
				|| adaptor->machine_kind(node.operands[0])
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| (result_kind != ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					&& result_kind != ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
					&& result_kind != ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
					&& result_kind != ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
					&& result_kind != ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
			return false;
		}
		auto source = val_ref(node.operands[0]);
		const ValueParts parts = val_parts(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		for (uint32_t part = 0; part < parts.count(); ++part) {
			if (parts.representation.parts[part].semantic_role
					!= ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				continue;
			}
			auto payload = source.part(part);
			auto payload_reg = payload.load_to_reg();
			ASM(MOV64rr, result.alloc_reg(), payload_reg);
			result.set_modified();
			return true;
		}
		return false;
	}
	if (node.kind == Adaptor::InstKind::TypedCallGuard) {
		/* Statepoint materializations precede the guarded values; they
		 * are liveness operands only. */
		const std::span<const IRValueRef> guard_operands =
			node.liveness_operands;
		ZEND_ASSERT(node.argument_index != UINT32_MAX
			&& node.continuation_block != UINT32_MAX);
		const IRBlockRef cold{node.argument_index};
		const IRBlockRef hot{node.continuation_block};
		/* Observers are part of the system id (emit_observer_exit()). */
		if (guard_operands.size() == node.materialization_count) {
			generate_uncond_branch(ZEND_OBSERVER_ENABLED ? cold : hot);
			return true;
		}
		const uint32_t guarded_operand_offset =
			1 + node.materialization_count;
		if (guard_operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				|| guarded_operand_offset > guard_operands.size()
				|| (guard_operands.size() - guarded_operand_offset) % 2 != 0) {
			return false;
		}

		/*
		 * A guarded boxed argument owns the copy produced by its fast read.
		 * The typed hot call releases that copy after use.  If a runtime type
		 * or observer guard selects the canonical cold call instead, transfer
		 * the same ownership into the source frame slot consumed by that path.
		 */
		auto [frame_ref, frame] = val_ref_single(guard_operands[0]);
		auto frame_reg = frame.load_to_reg();
		std::vector<ValueRef> guarded_values;
		std::vector<AsmReg> guarded_payload_regs;
		std::vector<AsmReg> guarded_type_regs;
		std::vector<uint32_t> guarded_expected_types;
		std::vector<int32_t> guarded_offsets;
		const uint32_t guarded_count =
			static_cast<uint32_t>(
				(guard_operands.size() - guarded_operand_offset) / 2);
		guarded_values.reserve(guarded_count);
		guarded_payload_regs.reserve(guarded_count);
		guarded_type_regs.reserve(guarded_count);
		guarded_expected_types.reserve(guarded_count);
		guarded_offsets.reserve(guarded_count);
		for (uint32_t operand = guarded_operand_offset;
				operand < guard_operands.size(); operand += 2) {
			if (adaptor->machine_kind(guard_operands[operand])
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				return false;
			}
			uint64_t expected_type;
			if (!adaptor->constant(
					guard_operands[operand + 1], &expected_type)
					|| expected_type > UINT32_MAX) {
				return false;
			}
			const zend_mir_storage_id storage_id =
				adaptor->canonical_storage(guard_operands[operand]);
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval);
			if (!zend_mir_id_is_valid(storage_id)
					|| offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			auto boxed = val_ref(guard_operands[operand]);
			const ValueParts parts = val_parts(guard_operands[operand]);
			/* A register-held boxed zval: payload, then type info
			 * (zend_tpde_machine_representation()). */
			if (parts.count() != 2) {
				return false;
			}
			constexpr int32_t payload_part = 0;
			constexpr int32_t type_part = 1;
			guarded_payload_regs.push_back(
				boxed.part(static_cast<uint32_t>(payload_part)).load_to_reg());
			guarded_type_regs.push_back(
				boxed.part(static_cast<uint32_t>(type_part)).load_to_reg());
			guarded_expected_types.push_back(
				static_cast<uint32_t>(expected_type));
			guarded_offsets.push_back(static_cast<int32_t>(offset));
			guarded_values.push_back(std::move(boxed));
		}
		ScratchReg masked_type{this};
		auto masked_type_reg = masked_type.alloc_gp();
		auto cold_transfer = text_writer.label_create();
		auto hot_branch = text_writer.label_create();
		const auto spilled = spill_before_branch();
		begin_branch_region();
		emit_observer_exit(cold_transfer);
		for (uint32_t index = 0; index < guarded_count; ++index) {
			ASM(CMP8ri, guarded_type_regs[index], static_cast<int32_t>(guarded_expected_types[index]));
			generate_raw_jump(Jump::jne, cold_transfer);
		}
		generate_raw_jump(Jump::jmp, hot_branch);

		label_place(cold_transfer);
		for (uint32_t index = 0; index < guarded_count; ++index) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG, guarded_offsets[index]),
				guarded_payload_regs[index]);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					guarded_offsets[index]
						+ static_cast<int32_t>(offsetof(zval, u1.type_info))),
				guarded_type_regs[index]);
		}
		generate_branch_to_block(
			Jump::jmp, cold, branch_needs_split(cold), false);
		label_place(hot_branch);
		generate_branch_to_block(Jump::jmp, hot, false, true);
		end_branch_region();
		release_spilled_regs(spilled);
		return true;
	}
	if (node.kind == Adaptor::InstKind::StringLengthValue) {
		if (node.operands.size() != 1 || !node.has_result
				|| adaptor->machine_kind(node.operands[0])
					!= ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| adaptor->exact_type(node.result)
					!= ZEND_MIR_SCALAR_TYPE_I64) {
			return false;
		}
		auto [string_ref, string] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		ASM(MOV64rm, result.alloc_reg(),
			FE_MEM(string.load_to_reg(), 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, len))));
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::BoxScalar) {
		if (node.operands.size() != 1 || !node.has_result
				|| adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			return false;
		}
		const zend_mir_scalar_type_mask input_type =
			adaptor->exact_type(node.operands[0]);
		if (!((input_type == ZEND_MIR_SCALAR_TYPE_I64
						&& adaptor->machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (input_type == ZEND_MIR_SCALAR_TYPE_I1
						&& adaptor->machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_BOOL)
					|| (input_type == ZEND_MIR_SCALAR_TYPE_F64
						&& adaptor->machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_F64))) {
			return false;
		}
		auto [source_ref, source] =
			val_ref_single(node.operands[0]);
		auto source_reg = source.load_to_reg();
		auto result = result_ref(node.result);
		const ValueParts parts = val_parts(node.result);
		for (uint32_t part = 0; part < parts.count(); ++part) {
			auto value = result.part(part);
			auto value_reg = value.alloc_reg();
			switch (parts.representation.parts[part].semantic_role) {
			case ZEND_TPDE_MACHINE_PART_PAYLOAD:
					if (input_type == ZEND_MIR_SCALAR_TYPE_F64) {
						ASM(SSE_MOVQ_X2Grr, value_reg, source_reg);
					} else {
						ASM(MOV64rr, value_reg, source_reg);
					}
					break;
				case ZEND_TPDE_MACHINE_PART_TYPE_INFO:
					if (input_type == ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(MOV32rr, value_reg, source_reg);
						ASM(ADD32ri, value_reg, IS_FALSE);
					} else if (input_type == ZEND_MIR_SCALAR_TYPE_I64) {
						ASM(MOV32ri, value_reg, IS_LONG);
					} else {
						ASM(MOV32ri, value_reg, IS_DOUBLE);
					}
					break;
				default:
					return false;
			}
			value.set_modified();
		}
		return true;
	}
	if (node.kind == Adaptor::InstKind::UnboxScalar) {
		if (node.operands.size() != 1 || !node.has_result
				|| adaptor->machine_kind(node.operands[0])
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| (adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_I64
					&& adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_BOOL)) {
			return false;
		}
		auto source = val_ref(node.operands[0]);
		const ValueParts parts = val_parts(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		for (uint32_t part = 0; part < parts.count(); ++part) {
			if (parts.representation.parts[part].semantic_role
					!= ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				continue;
			}
			auto payload = source.part(part);
			ASM(MOV64rr, result.alloc_reg(), payload.load_to_reg());
			result.set_modified();
			return true;
		}
		return false;
	}
	if (node.kind == Adaptor::InstKind::UnboxReferenceScalar) {
		if (node.operands.size() != 1 || !node.has_result
				|| adaptor->machine_kind(node.operands[0])
					!= ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
				|| adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_I64) {
			return false;
		}
		auto [source_ref, source] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		ASM(MOV64rm, result.alloc_reg(),
			FE_MEM(source.load_to_reg(), 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_reference, val.value))));
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::LoadFrame) {
		auto [source_ref, source] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto source_reg = source.load_to_reg();
		auto result_reg = result.alloc_reg();
		ASM(MOV64rr, result_reg, source_reg);
		/* Tier 2 (ADR 0025 section 4): count the call down in the entry
		 * cell; the last one queues the function for recompilation. */
		if (adaptor->plan()->call_count_cell != nullptr
				&& !adaptor->typed_body()
				&& runtime_symbol(ZEND_NATIVE_HELPER_TIER2_NOTE).valid()
				&& runtime_symbol(ZEND_NATIVE_HELPER_TIER2_RECORD).valid()) {
			ValuePart cell = image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				ZEND_NATIVE_TIER2_COUNT_SYMBOL_ID);
			if (cell.has_reg()) {
				auto cell_scratch = std::move(cell).into_scratch(this);
				const AsmReg cell_reg = cell_scratch.cur_reg();
				auto counted = text_writer.label_create();
				auto queue = text_writer.label_create();
				auto record = text_writer.label_create();
				ASM(SUB32mi, FE_MEM(cell_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_entry_cell, tier2_countdown))), 1);
				generate_raw_jump(Jump::je, queue);
				/* The last calls before the threshold record their
				 * argument types for the recompilation. */
				ASM(CMP32mi, FE_MEM(cell_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_entry_cell, tier2_countdown))),
					ZEND_NATIVE_TIER2_RECORD_CALLS);
				generate_raw_jump(Jump::jbe, record);
				const bool cold_queue = !text_writer.in_cold_area();
				if (cold_queue) {
					cold_begin();
				} else {
					generate_raw_jump(Jump::jmp, counted);
				}
				/* The entry's stack may not be call-aligned yet: save the
				 * caller-saved registers, align, call, restore. */
				auto preserving_call = [&](zend_native_runtime_helper_id helper,
						bool with_frame) {
					this->stack.generated_call = true;
					static constexpr FeRegGP saved[] = {
						FE_AX, FE_CX, FE_DX, FE_SI, FE_DI,
						FE_R8, FE_R9, FE_R10, FE_R11};
					ASM(LEA64rm, FE_SP, FE_MEM(FE_SP, 0, FE_NOREG, -80));
					for (int i = 0; i < 9; ++i) {
						ASM(MOV64mr, FE_MEM(FE_SP, 0, FE_NOREG, 8 * i),
							saved[i]);
					}
					if (with_frame) {
						ASM(PUSHr, source_reg);
						ASM(PUSHr, cell_reg);
						ASM(POPr, FE_DI);
						ASM(POPr, FE_SI);
					} else {
						ASM(MOV64rr, FE_DI, cell_reg);
					}
					ASM(MOV64rr, FE_AX, FE_SP);
					ASM(AND64ri, FE_SP, -16);
					ASM(PUSHr, FE_AX);
					ASM(PUSHr, FE_AX);
					emit_symbol_call(runtime_symbol(helper));
					ASM(MOV64rm, FE_SP, FE_MEM(FE_SP, 0, FE_NOREG, 0));
					for (int i = 0; i < 9; ++i) {
						ASM(MOV64rm, saved[i],
							FE_MEM(FE_SP, 0, FE_NOREG, 8 * i));
					}
					ASM(LEA64rm, FE_SP, FE_MEM(FE_SP, 0, FE_NOREG, 80));
					generate_raw_jump(Jump::jmp, counted);
				};
				label_place(queue);
				preserving_call(ZEND_NATIVE_HELPER_TIER2_NOTE, false);
				label_place(record);
				preserving_call(ZEND_NATIVE_HELPER_TIER2_RECORD, true);
				if (cold_queue) {
					cold_end();
				}
				label_place(counted);
			}
		}
		/* The context operand is one use, whatever reads it below. */
		std::optional<std::pair<ValueRef, ValuePartRef>> context_use;
		AsmReg context_reg = AsmReg::make_invalid();
		if (node.operands.size() == 2) {
			context_use.emplace(val_ref_single(node.operands[1]));
			/* Loaded before the first branch: past one the value state
			 * may not change. */
			context_reg = context_use->second.load_to_reg();
		}
		if (adaptor->plan()->tier2_fallback_entry != nullptr
				&& context_use.has_value() && !adaptor->typed_body()) {
			if (!emit_tier2_arg_guards(result_reg, context_reg)) {
				return false;
			}
		}
		std::optional<tpde::Label> deopt_entry;
		if (context_use.has_value()
				&& adaptor->plan()->deopt_resume_count != 0) {
			/* A deoptimization entry continues a frame whose temporaries
			 * are live. */
			deopt_entry = text_writer.label_create();
			ASM(CMP8mi, FE_MEM(context_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_native_execution_context, deopt_resume))),
				0);
			generate_raw_jump(Jump::jne, *deopt_entry);
		}
		if (adaptor->plan()->entry_undef_temporary_count != 0) {
			auto initialized = text_writer.label_create();
			/* A resumed generator frame keeps its temporaries; only a
			 * generator's entry can see one. */
			if (adaptor->plan()->source_generator) {
				ScratchReg call_info{this};
				auto call_info_reg = call_info.alloc_gp();
				ASM(MOV32rm, call_info_reg,
					FE_MEM(result_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, This)
								+ offsetof(zval, u1.type_info))));
				ASM(TEST32ri, call_info_reg, ZEND_CALL_GENERATOR);
				generate_raw_jump(Jump::jne, initialized);
			}
			for (uint32_t required = 0;
					required
						< adaptor->plan()->entry_undef_temporary_count;
					++required) {
				const uint32_t index =
					adaptor->plan()->entry_undef_temporary_indices[
						required];
				const int32_t offset = static_cast<int32_t>(
					(uint64_t{ZEND_CALL_FRAME_SLOT}
						+ adaptor->plan()->source_frame_variable_count
						+ index)
						* sizeof(zval)
					+ sizeof(uint64_t));
				ASM(MOV64mi,
					FE_MEM(result_reg, 0, FE_NOREG, offset), 0);
			}
			label_place(initialized);
		}
		if (deopt_entry) {
			label_place(*deopt_entry);
		}
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeLanding) {
		const zend_tpde_plan *plan = adaptor->plan();
		if (plan->source_opcodes == nullptr
				|| node.argument_index >= plan->source_opcode_count) {
			return false;
		}
		while (user_opcode_labels_.size() < plan->source_opcode_count) {
			user_opcode_labels_.push_back(text_writer.label_create());
			user_opcode_dispatch_labels_.push_back(
				text_writer.label_create());
			user_opcode_result_reload_labels_.push_back(
				text_writer.label_create());
		}
		label_place(user_opcode_labels_[node.argument_index]);
		return true;
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeDispatch) {
		if (node.argument_index >= user_opcode_dispatch_labels_.size()) {
			return false;
		}
		label_place(user_opcode_dispatch_labels_[node.argument_index]);
		return true;
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeGateway) {
		const zend_tpde_plan *plan = adaptor->plan();
		const auto dispatch_sources =
			adaptor->user_opcode_dispatch_to_sources();
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
				|| user_opcode_labels_.size()
					< plan->source_opcode_count) {
			return false;
		}
		const uint32_t source_position = node.argument_index;
		const auto &next_landings =
			adaptor->user_opcode_next_landings();
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{node.operands[0]});
		builder.add_arg(CallArg{node.operands[1]});
		add_const_arg(builder, source_position, 4);
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_OPCODE_INVOKE));
		ValuePart action{tpde::x64::PlatformConfig::GP_BANK, 8};
		ValuePart selected_position{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		builder.add_ret(action, tpde::CCAssignment{});
		builder.add_ret(selected_position, tpde::CCAssignment{});
		auto action_reg = action.cur_reg_or_load(this);
		ScratchReg position{this};
		auto position_reg =
			position.alloc_specific(tpde::x64::AsmReg::R11);
		mov(position_reg, selected_position.cur_reg_or_load(this), 4);
		selected_position.reset(this);
		ScratchReg selected_opcode{this};
		auto selected_opcode_reg =
			selected_opcode.alloc_specific(tpde::x64::AsmReg::R10);
		mov(selected_opcode_reg, action_reg, 4);
		ASM(AND32ri, selected_opcode_reg, UINT32_C(0xff));
		auto return_action = text_writer.label_create();
		auto returned = text_writer.label_create();
		auto exception = text_writer.label_create();
		auto continued = text_writer.label_create();
		auto dispatch = text_writer.label_create();
		auto dispatch_to = text_writer.label_create();
		ASM(CMP32ri, action_reg, UINT32_MAX);
		generate_raw_jump(Jump::je, exception);
		ASM(CMP32ri, action_reg, ZEND_USER_OPCODE_CONTINUE);
		generate_raw_jump(Jump::je, continued);
		ASM(CMP32ri, action_reg, ZEND_USER_OPCODE_RETURN);
		generate_raw_jump(Jump::je, return_action);
		ASM(CMP32ri, action_reg, ZEND_USER_OPCODE_LEAVE);
		generate_raw_jump(Jump::je, returned);
		ASM(CMP32ri, action_reg, ZEND_USER_OPCODE_DISPATCH);
		generate_raw_jump(Jump::je, dispatch);
		generate_raw_jump(Jump::jmp, dispatch_to);
		action.reset(this);
		label_place(continued);
		ASM(ADD32ri, position_reg, 1);
		for (uint32_t source = 0;
				source < user_opcode_labels_.size(); ++source) {
			if (next_landings[source] != source) {
				continue;
			}
			ASM(CMP32ri, position_reg, source);
			generate_raw_jump(
				Jump::je, user_opcode_labels_[source]);
		}
		generate_raw_jump(Jump::jmp, exception);
		label_place(dispatch);
		for (uint32_t source = 0;
				source < user_opcode_dispatch_labels_.size(); ++source) {
			if (next_landings[source] != source) {
				continue;
			}
			ASM(CMP32ri, position_reg, source);
			generate_raw_jump(
				Jump::je, user_opcode_dispatch_labels_[source]);
		}
		generate_raw_jump(Jump::jmp, exception);
		label_place(dispatch_to);
		for (uint32_t source = 0;
				source < user_opcode_dispatch_labels_.size(); ++source) {
			if (next_landings[source] != source) {
				continue;
			}
			auto next_candidate = text_writer.label_create();
			ASM(CMP32ri, position_reg, source);
			generate_raw_jump(Jump::jne, next_candidate);
			ASM(CMP32ri, selected_opcode_reg,
				plan->source_opcodes[source].opcode);
			generate_raw_jump(
				Jump::je, user_opcode_dispatch_labels_[source]);
			label_place(next_candidate);
		}
		struct DispatchToCase {
			tpde::Label label;
			const zend_tpde_instruction *instruction;
			const zend_mir_executable_value_ref *operation;
			uint32_t source;
			uint32_t target_opcode;
			zend_tpde_user_opcode_target_kind kind;
			zend_native_runtime_helper_id helper;
			uint32_t frame_operand;
			uint32_t slow_frame_operand;
		};
		std::vector<DispatchToCase> dispatch_cases;
		dispatch_cases.reserve(dispatch_case_count);
		uint32_t frame_operand = 4;
		for (size_t source_index = 0;
				source_index < dispatch_sources.size(); ++source_index) {
			const uint32_t source = dispatch_sources[source_index];
			auto next_candidate = text_writer.label_create();
			ASM(CMP32ri, position_reg, source);
			generate_raw_jump(Jump::jne, next_candidate);
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
				auto target = text_writer.label_create();
				ASM(CMP32ri, selected_opcode_reg, target_case.opcode);
				generate_raw_jump(Jump::je, target);
				dispatch_cases.push_back({
					target,
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
			label_place(next_candidate);
		}
		generate_raw_jump(Jump::jmp, exception);
		position.reset();
		selected_opcode.reset();
		for (const DispatchToCase &dispatch_case : dispatch_cases) {
			label_place(dispatch_case.label);
			const zend_mir_executable_value_ref &operation =
				*dispatch_case.operation;
			auto jump_to_source = [&](uint32_t source) {
				const uint32_t landing =
					source < next_landings.size()
						? next_landings[source] : UINT32_MAX;
				if (landing == UINT32_MAX
						|| landing >= user_opcode_labels_.size()) {
					generate_raw_jump(Jump::jmp, exception);
				} else {
					generate_raw_jump(
						Jump::jmp, user_opcode_labels_[landing]);
				}
			};
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_NOP) {
				auto [frame_ref, frame] =
					val_ref_single(
						node.operands[dispatch_case.frame_operand]);
				frame.reset();
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_JUMP_OP1) {
				auto [frame_ref, frame] =
					val_ref_single(
						node.operands[dispatch_case.frame_operand]);
				frame.reset();
				jump_to_source(
					plan->user_opcode_source_op1_targets[
						dispatch_case.source]);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_CALL) {
				const uint32_t target =
					plan->user_opcode_source_op1_targets[
						dispatch_case.source];
				if (operation.result_storage_id == ZEND_MIR_ID_INVALID
						|| target == UINT32_MAX
						|| operation.result_storage_id
							> (UINT32_MAX / sizeof(zval))
								- ZEND_CALL_FRAME_SLOT) {
					auto [frame_ref, frame] = val_ref_single(
						node.operands[dispatch_case.frame_operand]);
					frame.reset();
					generate_raw_jump(Jump::jmp, exception);
					continue;
				}
				const int32_t result_offset =
					static_cast<int32_t>(
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.result_storage_id)
						* sizeof(zval));
				auto [frame_ref, frame] = val_ref_single(
					node.operands[dispatch_case.frame_operand]);
				auto frame_scratch = frame_register(std::move(frame));
				ASM(MOV64mi,
					FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
						result_offset),
					0);
				ASM(MOV32mi,
					FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
						result_offset
							+ static_cast<int32_t>(
								offsetof(zval, u2.opline_num))),
					dispatch_case.source);
				frame_scratch.reset();
				jump_to_source(target);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_RETURN) {
				if (operation.op1_storage_id == ZEND_MIR_ID_INVALID
						|| operation.op1.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_TMP
						|| operation.op1_storage_id
							> (INT32_MAX / sizeof(zval))
								- ZEND_CALL_FRAME_SLOT) {
					auto [fast_ref, fast_frame] = val_ref_single(
						node.operands[dispatch_case.frame_operand]);
					auto [slow_ref, slow_frame] = val_ref_single(
						node.operands[
							dispatch_case.slow_frame_operand]);
					fast_frame.reset();
					slow_frame.reset();
					generate_raw_jump(Jump::jmp, exception);
					continue;
				}
				const int32_t operand_offset =
					static_cast<int32_t>(
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.op1_storage_id)
						* sizeof(zval));
				auto slow_exception = text_writer.label_create();
				auto [frame_ref, frame] = val_ref_single(
					node.operands[dispatch_case.frame_operand]);
				auto frame_scratch = frame_register(std::move(frame));
				ScratchReg continuation{this};
				auto continuation_reg = continuation.alloc_gp();
				ASM(MOV32rm, continuation_reg,
					FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
						operand_offset
							+ static_cast<int32_t>(
								offsetof(zval, u2.opline_num))));
				frame_scratch.reset();
				ASM(CMP32ri, continuation_reg, UINT32_MAX);
				generate_raw_jump(Jump::je, slow_exception);
				for (uint32_t source = 0;
						source + 1 < next_landings.size(); ++source) {
					const uint32_t landing = next_landings[source + 1];
					if (landing == UINT32_MAX
							|| landing >= user_opcode_labels_.size()) {
						continue;
					}
					ASM(CMP32ri, continuation_reg, source);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::jne, continued);
					generate_raw_jump(
						Jump::jmp, user_opcode_labels_[landing]);
					label_place(continued);
				}
				continuation.reset();
				generate_raw_jump(Jump::jmp, exception);
				label_place(slow_exception);
				tpde::x64::CCAssignerSysV finally_assigner{false};
				CallBuilder finally_call{*this, finally_assigner};
				finally_call.add_arg(CallArg{
					node.operands[dispatch_case.slow_frame_operand]});
				add_const_arg(finally_call,
					encode_source_operand(operation.op1), 8);
				add_const_arg(finally_call, operation.op2_unused_payload, 4);
				add_const_arg(finally_call, dispatch_case.source, 4);
				finally_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart selected{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				finally_call.add_ret(selected, tpde::CCAssignment{});
				auto selected_reg = selected.cur_reg_or_load(this);
				for (uint32_t i = 0; i < plan->instruction_count; ++i) {
					const zend_mir_instruction_record handler =
						zend_tpde_instruction_record_at(
							plan, &plan->instructions[i]);
					if ((handler.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
							&& handler.opcode
								!= ZEND_MIR_OPCODE_FINALLY_ENTER)
							|| !zend_mir_id_is_valid(
								handler.source_position_id)) {
						continue;
					}
					ASM(CMP32ri, selected_reg,
						ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
							| handler.source_position_id);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::jne, continued);
					jump_to_source(handler.source_position_id);
					label_place(continued);
				}
				selected.reset(this);
				generate_raw_jump(Jump::jmp, exception);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_CATCH) {
				tpde::x64::CCAssignerSysV catch_assigner{false};
				CallBuilder catch_call{*this, catch_assigner};
				catch_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				add_const_arg(catch_call,
					encode_source_operand(operation.op1), 8);
				add_const_arg(catch_call,
					encode_source_operand(operation.result), 8);
				add_const_arg(catch_call, operation.extended_value, 4);
				add_const_arg(catch_call, dispatch_case.source, 4);
				catch_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart result{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				catch_call.add_ret(result, tpde::CCAssignment{});
				auto result_reg = result.cur_reg_or_load(this);
				auto catch_branch = text_writer.label_create();
				auto catch_matched = text_writer.label_create();
				ASM(CMP32ri, result_reg, ZEND_NATIVE_CATCH_EXCEPTION);
				generate_raw_jump(Jump::je, exception);
				ASM(CMP32ri, result_reg, ZEND_NATIVE_CATCH_BRANCH);
				generate_raw_jump(Jump::je, catch_branch);
				ASM(CMP32ri, result_reg, ZEND_NATIVE_CATCH_MATCHED);
				generate_raw_jump(Jump::je, catch_matched);
				result.reset(this);
				generate_raw_jump(Jump::jmp, exception);
				label_place(catch_branch);
				jump_to_source(
					plan->user_opcode_source_op2_targets[
						dispatch_case.source]);
				label_place(catch_matched);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_RECEIVE) {
				tpde::x64::CCAssignerSysV receive_assigner{false};
				CallBuilder receive_call{*this, receive_assigner};
				receive_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				add_const_arg(receive_call, dispatch_case.target_opcode, 4);
				add_const_arg(receive_call, operation.op1_unused_payload, 4);
				add_const_arg(receive_call,
					encode_source_operand( operation.op2, operation.op2_unused_payload), 8);
				add_const_arg(receive_call, operation.op2_unused_payload, 4);
				add_const_arg(receive_call,
					encode_source_operand(operation.result), 8);
				add_const_arg(receive_call, dispatch_case.source, 4);
				receive_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				receive_call.add_ret(status, tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
				auto received = text_writer.label_create();
				generate_raw_jump(Jump::je, received);
				status.reset(this);
				generate_raw_jump(Jump::jmp, exception);
				label_place(received);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_CALL_FRAGMENT) {
				tpde::x64::CCAssignerSysV fragment_assigner{false};
				CallBuilder fragment_call{*this, fragment_assigner};
				fragment_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				add_const_arg(fragment_call, dispatch_case.target_opcode, 4);
				add_const_arg(fragment_call,
					encode_source_operand( operation.op1, operation.op1_unused_payload), 8);
				add_const_arg(fragment_call, operation.op1_unused_payload, 4);
				add_const_arg(fragment_call,
					encode_source_operand( operation.op2, operation.op2_unused_payload), 8);
				add_const_arg(fragment_call, operation.op2_unused_payload, 4);
				add_const_arg(fragment_call,
					encode_source_operand( operation.result, operation.result_unused_payload), 8);
				add_const_arg(fragment_call,
					operation.result_unused_payload, 4);
				add_const_arg(fragment_call, operation.extended_value, 4);
				add_const_arg(fragment_call, dispatch_case.source, 4);
				fragment_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				fragment_call.add_ret(status, tpde::CCAssignment{});
				emit_status_tail(std::move(status),
					dispatch_case.instruction != nullptr
						? dispatch_case.instruction->exception_block_id
						: ZEND_MIR_ID_INVALID);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_RETURN) {
				tpde::x64::CCAssignerSysV return_assigner{false};
				CallBuilder return_call{*this, return_assigner};
				return_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				add_const_arg(return_call, dispatch_case.source, 4);
				add_const_arg(return_call,
					encode_source_operand(operation.op1), 8);
				add_const_arg(return_call, dispatch_case.target_opcode, 4);
				add_const_arg(return_call, operation.extended_value, 4);
				return_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				return_call.add_ret(status, tpde::CCAssignment{});
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(
					std::move(status), tpde::CCAssignment{});
				return_builder.ret();
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_THROW) {
				tpde::x64::CCAssignerSysV throw_assigner{false};
				CallBuilder throw_call{*this, throw_assigner};
				throw_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				add_const_arg(throw_call,
					encode_source_operand(operation.op1), 8);
				add_const_arg(throw_call, dispatch_case.target_opcode, 4);
				add_const_arg(throw_call, dispatch_case.source, 4);
				throw_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				throw_call.add_ret(status, tpde::CCAssignment{});
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(
					std::move(status), tpde::CCAssignment{});
				return_builder.ret();
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_MULTI_BRANCH) {
				zend_tpde_user_multi_branch layout;
				if (!zend_tpde_user_multi_branch_at(
						plan, operation, dispatch_case.target_opcode,
						&layout)
						|| layout.operand_offset > INT32_MAX) {
					auto [frame_ref, frame] = val_ref_single(
						node.operands[dispatch_case.frame_operand]);
					frame.reset();
					generate_raw_jump(Jump::jmp, exception);
					continue;
				}
				std::vector<tpde::Label> case_labels;
				case_labels.reserve(layout.case_count);
				for (uint32_t index = 0;
						index < layout.case_count;
						++index) {
					case_labels.push_back(text_writer.label_create());
				}
				auto default_label = text_writer.label_create();
				auto fallback_label =
					layout.target_opcode == ZEND_MATCH
						? default_label : text_writer.label_create();
				auto long_label = text_writer.label_create();
				auto string_label = text_writer.label_create();
				auto [frame_ref, frame] = val_ref_single(
					node.operands[dispatch_case.frame_operand]);
				auto frame_scratch = frame_register(std::move(frame));
				ScratchReg slot{this};
				ScratchReg type{this};
				ScratchReg value{this};
				ScratchReg probe{this};
				ScratchReg constant{this};
				auto slot_reg = slot.alloc_gp();
				auto type_reg = type.alloc_gp();
				auto value_reg = value.alloc_gp();
				auto probe_reg = probe.alloc_gp();
				auto constant_reg = constant.alloc_gp();
				ASM(MOV64rr, slot_reg, frame_scratch.cur_reg());
				ASM(ADD64ri, slot_reg,
					static_cast<int32_t>(layout.operand_offset));
				ASM(MOVZXr32m8, type_reg,
					FE_MEM(slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
				auto spilled = spill_before_branch();
				begin_branch_region();
				auto dereferenced = text_writer.label_create();
				ASM(CMP32ri, type_reg, IS_REFERENCE);
				generate_raw_jump(Jump::jne, dereferenced);
				ASM(MOV64rm, slot_reg,
					FE_MEM(slot_reg, 0, FE_NOREG, 0));
				ASM(ADD64ri, slot_reg,
					static_cast<int32_t>(offsetof(zend_reference, val)));
				ASM(MOVZXr32m8, type_reg,
					FE_MEM(slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
				label_place(dereferenced);
				if (layout.target_opcode != ZEND_SWITCH_STRING) {
					ASM(CMP32ri, type_reg, IS_LONG);
					generate_raw_jump(Jump::je, long_label);
				}
				if (layout.target_opcode != ZEND_SWITCH_LONG) {
					ASM(CMP32ri, type_reg, IS_STRING);
					generate_raw_jump(Jump::je, string_label);
				}
				generate_raw_jump(Jump::jmp, fallback_label);

				label_place(long_label);
				ASM(MOV64rm, value_reg,
					FE_MEM(slot_reg, 0, FE_NOREG, 0));
				emit_integer_dispatch(
					layout.cases, layout.case_count, case_labels,
					value_reg, constant_reg, default_label);

				label_place(string_label);
				ASM(MOV64rm, value_reg,
					FE_MEM(slot_reg, 0, FE_NOREG, 0));
				for (uint32_t case_index = 0;
						case_index < layout.case_count; ++case_index) {
					const zend_tpde_multi_branch_case &branch_case =
						layout.cases[case_index];
					if (branch_case.string_key != nullptr) {
						auto next_case = text_writer.label_create();
						const uint64_t length = branch_case.string_length;
						ASM(MOV64rm, probe_reg,
							FE_MEM(value_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_string, len))));
						materialize_constant(
							&length,
							tpde::x64::PlatformConfig::GP_BANK,
							8, constant_reg);
						ASM(CMP64rr, probe_reg, constant_reg);
						generate_raw_jump(Jump::jne, next_case);
						size_t offset = 0;
						while (offset < branch_case.string_length) {
							const uint32_t width =
								branch_case.string_length - offset >= 8 ? 8
								: branch_case.string_length - offset >= 4 ? 4
								: branch_case.string_length - offset >= 2 ? 2 : 1;
							const size_t byte_offset =
								offsetof(zend_string, val) + offset;
							if (byte_offset > INT32_MAX) {
								return false;
							}
							uint64_t expected = 0;
							memcpy(&expected,
								branch_case.string_key + offset, width);
							switch (width) {
								case 8:
									ASM(MOV64rm, probe_reg,
										FE_MEM(value_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												byte_offset)));
									break;
								case 4:
									ASM(MOV32rm, probe_reg,
										FE_MEM(value_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												byte_offset)));
									break;
								case 2:
									ASM(MOVZXr32m16, probe_reg,
										FE_MEM(value_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												byte_offset)));
									break;
								default:
									ASM(MOVZXr32m8, probe_reg,
										FE_MEM(value_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												byte_offset)));
									break;
							}
							materialize_constant(
								&expected,
								tpde::x64::PlatformConfig::GP_BANK,
								width, constant_reg);
							ASM(CMP64rr, probe_reg, constant_reg);
							generate_raw_jump(Jump::jne, next_case);
							offset += width;
						}
						generate_raw_jump(
							Jump::jmp, case_labels[case_index]);
						label_place(next_case);
					}
				}
				generate_raw_jump(Jump::jmp, default_label);

				for (uint32_t case_index = 0;
						case_index < layout.case_count; ++case_index) {
					label_place(case_labels[case_index]);
					jump_to_source(layout.cases[case_index].target);
				}
				label_place(default_label);
				jump_to_source(layout.default_target);
				if (layout.target_opcode != ZEND_MATCH) {
					label_place(fallback_label);
					jump_to_source(layout.fallback_target);
				}
				end_branch_region();
				release_spilled_regs(spilled);
				continue;
			}
			if (dispatch_case.kind
						== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2
					|| dispatch_case.kind
						== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_OP2
					|| dispatch_case.kind
						== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_EXTENDED) {
				tpde::x64::CCAssignerSysV branch_assigner{false};
				CallBuilder branch_call{*this, branch_assigner};
				branch_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				add_const_arg(branch_call,
					encode_source_operand(operation.op1), 8);
				add_const_arg(branch_call,
					encode_source_operand(operation.op2), 8);
				add_const_arg(branch_call,
					encode_source_operand(operation.result), 8);
				add_const_arg(branch_call, operation.extended_value, 4);
				add_const_arg(branch_call, dispatch_case.target_opcode, 4);
				add_const_arg(branch_call, dispatch_case.source, 4);
				branch_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart result{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				branch_call.add_ret(result, tpde::CCAssignment{});
				auto result_reg = result.cur_reg_or_load(this);
				auto branch_target = text_writer.label_create();
				auto branch_following = text_writer.label_create();
				ASM(CMP32ri, result_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
				generate_raw_jump(Jump::je, exception);
				if (dispatch_case.kind
						== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2) {
					ASM(CMP32ri, result_reg, ZEND_NATIVE_ITERATOR_NEXT);
				} else {
					ASM(CMP32ri, result_reg, ZEND_NATIVE_ITERATOR_END);
				}
				generate_raw_jump(Jump::je, branch_target);
				ASM(CMP32ri, result_reg,
					dispatch_case.kind
							== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2
						? ZEND_NATIVE_ITERATOR_END
						: ZEND_NATIVE_ITERATOR_NEXT);
				generate_raw_jump(Jump::je, branch_following);
				result.reset(this);
				generate_raw_jump(Jump::jmp, exception);
				label_place(branch_target);
				jump_to_source(dispatch_case.kind
							== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_EXTENDED
						? plan->user_opcode_source_extended_targets[
							dispatch_case.source]
						: plan->user_opcode_source_op2_targets[
							dispatch_case.source]);
				label_place(branch_following);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			const bool explicit_object_operands =
				zend_tpde_helper_has_unused_operand_payloads(
					dispatch_case.helper);
			const bool explicit_auxiliary =
				zend_tpde_helper_has_explicit_auxiliary(
					dispatch_case.helper);
			auto encode_operand = [&](const zend_mir_source_operand_ref &operand,
					uint32_t unused_payload) {
				return explicit_object_operands
					? encode_source_operand(
						operand, unused_payload)
					: encode_source_operand(operand);
			};
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder operation_call{*this, assigner};
			operation_call.add_arg(
				CallArg{node.operands[dispatch_case.frame_operand]});
			add_const_arg(operation_call,
				encode_operand( operation.op1, operation.op1_unused_payload), 8);
			add_const_arg(operation_call,
				encode_operand( operation.op2, operation.op2_unused_payload), 8);
			add_const_arg(operation_call,
				encode_operand( operation.result, operation.result_unused_payload), 8);
			if (explicit_auxiliary) {
				add_const_arg(operation_call,
					encode_operand(operation.auxiliary, operation.auxiliary_unused_payload), 8);
			}
			add_const_arg(operation_call, operation.extended_value, 4);
			add_const_arg(operation_call, dispatch_case.target_opcode, 4);
			add_const_arg(operation_call, dispatch_case.source, 4);
			operation_call.call(runtime_symbol(dispatch_case.helper));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			operation_call.add_ret(status, tpde::CCAssignment{});
			emit_status_tail(std::move(status),
				dispatch_case.instruction != nullptr
					? dispatch_case.instruction->exception_block_id
					: ZEND_MIR_ID_INVALID);
			if (adaptor->user_opcode_result_reload_source(
						dispatch_case.source)
					&& dispatch_case.source
						< user_opcode_result_reload_labels_.size()) {
				generate_raw_jump(Jump::jmp,
					user_opcode_result_reload_labels_[
						dispatch_case.source]);
				continue;
			}
			const uint32_t following =
				dispatch_case.source + 1 < next_landings.size()
				? next_landings[dispatch_case.source + 1] : UINT32_MAX;
			if (following == UINT32_MAX
					|| following >= user_opcode_labels_.size()) {
				generate_raw_jump(Jump::jmp, exception);
			} else {
				generate_raw_jump(
					Jump::jmp, user_opcode_labels_[following]);
			}
		}
		label_place(return_action);
		{
			auto [frame_ref, frame] = val_ref_single(node.operands[2]);
			auto frame_reg = frame.load_to_reg();
			ScratchReg call_info{this};
			auto call_info_reg = call_info.alloc_gp();
			ASM(MOV32rm, call_info_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, call_info_reg, ZEND_CALL_GENERATOR);
			generate_raw_jump(Jump::je, returned);
			call_info.reset();
		}
		{
			tpde::x64::CCAssignerSysV return_assigner{false};
			CallBuilder return_call{*this, return_assigner};
			return_call.add_arg(CallArg{node.operands[3]});
			return_call.call(runtime_symbol(
				ZEND_NATIVE_HELPER_GENERATOR_USER_OPCODE_RETURN));
			ValuePart status{
				tpde::x64::PlatformConfig::GP_BANK, 4};
			return_call.add_ret(status, tpde::CCAssignment{});
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status),
				tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(returned);
		emit_status_return(ZEND_NATIVE_RETURNED);
		label_place(exception);
		emit_status_return(ZEND_NATIVE_EXCEPTION);
		return true;
	}
	if (node.kind == Adaptor::InstKind::UserCallInit
			|| node.kind == Adaptor::InstKind::UserCallSend
			|| node.kind == Adaptor::InstKind::UserCallCheck
			|| node.kind == Adaptor::InstKind::UserCallExpand
			|| node.kind == Adaptor::InstKind::UserCallDo) {
		const zend_tpde_source_call_phase_entry *phase =
			adaptor->source_call_phase(node);
		if (phase == nullptr) {
			return false;
		}
		const bool send_phase =
			node.kind == Adaptor::InstKind::UserCallSend;
		auto frame_liveness = val_ref(node.operands[0]);
		auto context_liveness = val_ref(node.operands[1]);
		/*
		 * Source-call phases contain target-local fast/slow branches which are
		 * invisible to TPDE's IR CFG.  Publish every live non-fixed assignment
		 * before either path can spill or clobber it so the join may reload the
		 * same canonical value regardless of the runtime path taken.
		 */
		(void) spill_before_branch(true);
		auto context_register = [&]() {
			auto context = context_liveness.part_unowned(0);
			auto reg = context.load_to_reg();
			context.reset();
			return reg;
		};
		auto context_argument = [&]() {
			auto context = context_liveness.part_unowned(0);
			auto argument = copy_fixed_argument(context.load_to_reg());
			context.reset();
			return argument;
		};
		if (node.kind == Adaptor::InstKind::UserCallInit) {
			const zend_tpde_instruction &call =
				adaptor->plan()->instructions[node.mir_instruction_index];
			if (call.user_call == nullptr) {
				return false;
			}
			zend_tpde_user_call_setup setup;
			if (!zend_tpde_user_call_setup_layout(adaptor->plan(),
					call.user_call, INT32_MAX, &setup)) {
				return false;
			}
			const uint64_t argument_count = setup.argument_count;
			const uint64_t setup_size = setup.setup_size;
			const bool uses_discarded_return = setup.uses_discarded_return;
			const uint64_t result_offset = setup.result_offset;

			/*
			 * Native call-site fast path (ADR 0025 section 3): while the site
			 * header publishes a target for the current call-cache epoch, push
			 * its frame as the VM does (header, receiver, EX(call) link) and
			 * record the mode in the call's slot; Send and Do follow it. A miss
			 * records the universal mode and runs the universal setup below.
			 */
			const bool fast_site = source_call_fast_eligible(call);
			TargetBranchState fast_spilled;
			auto fast_join = text_writer.label_create();
			if (fast_site) {
				const int32_t fast_slot =
					fast_call_slot(node.mir_instruction_index);
				fast_spilled = spill_target_branch_state();
				auto fast_miss = text_writer.label_create();
				const zend_native_user_call_descriptor *descriptor =
					call.user_call;
				if (descriptor->init_opcode == ZEND_NEW) {
					/* The published constructor: the helper creates the
					 * object in the result slot and pushes the frame. */
					const uint64_t object_offset =
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ adaptor->plan()->source_frame_variable_count
							+ descriptor->init_result.index) * sizeof(zval);
					if (object_offset > INT32_MAX - sizeof(zval)) {
						return false;
					}
					/* The helper checks the epoch and re-arms a stale
					 * site. */
					emit_observer_exit(fast_miss);
					{
						tpde::x64::CCAssignerSysV assigner{false};
						CallBuilder builder{*this, assigner};
						builder.add_arg(copy_fixed_argument(
							canonical_frame_register(), &assigner), tpde::CCAssignment{});
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
							call.id, &assigner), tpde::CCAssignment{});
						add_const_arg(builder, object_offset, 4);
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_CALL_FAST_NEW));
						ValuePart created{
							tpde::x64::PlatformConfig::GP_BANK, 1};
						builder.add_ret(created, tpde::CCAssignment{});
						ASM(TEST8rr, created.cur_reg_or_load(this),
							created.cur_reg_or_load(this));
						created.reset(this);
					}
					generate_raw_jump(Jump::je, fast_miss);
					ASM(MOV32mi, FE_MEM(FE_BP, 0, FE_NOREG, fast_slot), 1);
					/* Falls through to fast_join: the re-arm stub and the miss
					 * path between them are emitted into the cold area. */
				} else if (source_call_fast_dynamic(call)) {
					/* A recorded native target of the callable: the helper
					 * pushes and links its frame and returns its entry and
					 * the entry the Do calls. */
					const int32_t entry_slot =
						fast_entry_slot(node.mir_instruction_index);
					const int32_t do_entry_slot =
						fast_do_entry_slot(node.mir_instruction_index);
					{
						tpde::x64::CCAssignerSysV assigner{false};
						CallBuilder builder{*this, assigner};
						builder.add_arg(copy_fixed_argument(
							canonical_frame_register(), &assigner), tpde::CCAssignment{});
						auto descriptor_value = image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
							call.id);
						auto descriptor_scratch =
							std::move(descriptor_value).into_scratch(this);
						ValuePart descriptor_part{
							tpde::x64::PlatformConfig::GP_BANK, 8};
						descriptor_part.set_value(
							this, std::move(descriptor_scratch));
						builder.add_arg(std::move(descriptor_part),
							tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_CALL_FAST_DYNAMIC_INIT));
						ValuePart entry{tpde::x64::PlatformConfig::GP_BANK, 8};
						ValuePart do_entry{
							tpde::x64::PlatformConfig::GP_BANK, 8};
						builder.add_ret(entry, tpde::CCAssignment{});
						builder.add_ret(do_entry, tpde::CCAssignment{});
						ASM(MOV64mr, FE_MEM(FE_BP, 0, FE_NOREG, do_entry_slot),
							do_entry.cur_reg_or_load(this));
						do_entry.reset(this);
						auto entry_reg = entry.cur_reg_or_load(this);
						ASM(MOV64mr, FE_MEM(FE_BP, 0, FE_NOREG, entry_slot),
							entry_reg);
						ASM(TEST64rr, entry_reg, entry_reg);
						entry.reset(this);
					}
					generate_raw_jump(Jump::je, fast_miss);
					ASM(MOV32mi, FE_MEM(FE_BP, 0, FE_NOREG, fast_slot), 1);
					/* Falls through to fast_join: the re-arm stub and the miss
					 * path between them are emitted into the cold area. */
				} else {
				const bool method =
					descriptor->init_opcode == ZEND_INIT_METHOD_CALL;
				const bool static_call =
					descriptor->init_opcode == ZEND_INIT_STATIC_METHOD_CALL;
				const bool late_static = static_call
					&& descriptor->init_op1.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					&& (descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK)
						== ZEND_FETCH_CLASS_STATIC;
				const bool this_receiver = method
					&& descriptor->init_op1.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED;
				const bool temporary_receiver = method && !this_receiver
					&& descriptor->init_op1.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV;
				const uint64_t receiver_offset = method && !this_receiver
					? (uint64_t{ZEND_CALL_FRAME_SLOT}
						+ descriptor->init_op1.index
						+ (temporary_receiver ? uint64_t{
							adaptor->plan()->source_frame_variable_count} : 0))
						* sizeof(zval)
					: 0;
				if (receiver_offset > INT32_MAX - sizeof(zval)) {
					return false;
				}
				constexpr int32_t header_offset = -static_cast<int32_t>(
					sizeof(zend_native_user_call_site_header));
				auto header_field = [&](AsmReg descriptor_reg, size_t field) {
					return FE_MEM(descriptor_reg, 0, FE_NOREG,
						header_offset + static_cast<int32_t>(field));
				};
				/* A stale epoch first tries the re-arm stub below. */
				auto fast_retry = text_writer.label_create();
				auto fast_rearm = text_writer.label_create();
				AsmReg rearm_descriptor_reg{};
				uint64_t rearm_live_registers = ~uint64_t{0};
				{
					auto descriptor_value = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id);
					auto descriptor_scratch =
						std::move(descriptor_value).into_scratch(this);
					auto descriptor_reg = descriptor_scratch.cur_reg();
					rearm_descriptor_reg = descriptor_reg;
					ScratchReg value{this};
					ScratchReg callee{this};
					ScratchReg object{this};
					auto value_reg = value.alloc_gp();
					auto callee_reg = callee.alloc_gp();
					auto object_reg = object.alloc_gp();
					label_place(fast_retry);
					ASM(MOV64rm, value_reg,
						FE_MEM(context_register(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								call_cache_epoch))));
					ASM(MOV64rm, value_reg,
						FE_MEM(value_reg, 0, FE_NOREG, 0));
					ASM(CMP64rm, value_reg, header_field(descriptor_reg,
						offsetof(zend_native_user_call_site_header,
							fast_epoch)));
					/* The registers the stub must preserve: those in use
					 * here, where the retry and the miss resume, but for the
					 * scratch registers above, which the retry reloads
					 * (all but the descriptor). */
					rearm_live_registers = register_file.used;
					rearm_live_registers &= ~(uint64_t{1} << value_reg.id());
					rearm_live_registers &= ~(uint64_t{1} << callee_reg.id());
					rearm_live_registers &= ~(uint64_t{1} << object_reg.id());
					generate_raw_jump(Jump::jne, fast_rearm);
					emit_observer_exit(fast_miss);
					if (method) {
						if (this_receiver) {
							ASM(CMP8mi,
								FE_MEM(canonical_frame_register(), 0,
									FE_NOREG, static_cast<int32_t>(
										offsetof(zend_execute_data, This)
										+ offsetof(zval, u1.type_info))),
								IS_OBJECT);
							generate_raw_jump(Jump::jne, fast_miss);
							ASM(MOV64rm, object_reg,
								FE_MEM(canonical_frame_register(), 0,
									FE_NOREG, static_cast<int32_t>(
										offsetof(zend_execute_data, This))));
						} else {
							auto plain = text_writer.label_create();
							ASM(LEA64rm, value_reg,
								FE_MEM(canonical_frame_register(), 0,
									FE_NOREG,
									static_cast<int32_t>(receiver_offset)));
							if (!temporary_receiver) {
								/* A global or static CV is a reference. */
								ASM(CMP8mi,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(
											offsetof(zval, u1.type_info))),
									IS_REFERENCE);
								generate_raw_jump(Jump::jne, plain);
								ASM(MOV64rm, value_reg,
									FE_MEM(value_reg, 0, FE_NOREG, 0));
								ASM(ADD64ri, value_reg, static_cast<int32_t>(
									offsetof(zend_reference, val)));
							}
							label_place(plain);
							ASM(CMP8mi,
								FE_MEM(value_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zval, u1.type_info))),
								IS_OBJECT);
							generate_raw_jump(Jump::jne, fast_miss);
							ASM(MOV64rm, object_reg,
								FE_MEM(value_reg, 0, FE_NOREG, 0));
						}
						ASM(MOV64rm, value_reg,
							FE_MEM(object_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_object, ce))));
						ASM(CMP64rm, value_reg, header_field(descriptor_reg,
							offsetof(zend_native_user_call_site_header,
								fast_key)));
						if (this_receiver) {
							/* Another class of $this still calls a
							 * receiver-independent target. */
							auto key_matched = text_writer.label_create();
							generate_raw_jump(Jump::je, key_matched);
							ASM(TEST32mi, header_field(descriptor_reg,
								offsetof(zend_native_user_call_site_header,
									fast_flags)),
								ZEND_NATIVE_CALL_FAST_ANY_THIS);
							generate_raw_jump(Jump::je, fast_miss);
							label_place(key_matched);
						} else {
							generate_raw_jump(Jump::jne, fast_miss);
						}
					}
					if (static_call && late_static) {
						/* static::: the caller's called scope, which is the
						 * callee's, must be the class the site was
						 * published for. */
						const int32_t this_offset = static_cast<int32_t>(
							offsetof(zend_execute_data, This));
						auto have_scope = text_writer.label_create();
						ASM(MOV64rm, object_reg, FE_MEM(canonical_frame_register(),
							0, FE_NOREG, this_offset));
						ASM(CMP8mi, FE_MEM(canonical_frame_register(), 0,
							FE_NOREG, static_cast<int32_t>(this_offset
								+ offsetof(zval, u1.type_info))), IS_OBJECT);
						generate_raw_jump(Jump::jne, have_scope);
						ASM(MOV64rm, object_reg, FE_MEM(object_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_object, ce))));
						label_place(have_scope);
						ASM(CMP64rm, object_reg, header_field(descriptor_reg,
							offsetof(zend_native_user_call_site_header,
								fast_key)));
						generate_raw_jump(Jump::jne, fast_miss);
					} else if (static_call) {
						/* The callee's This: the caller's object for an
						 * instance method, the caller's called scope for a
						 * forwarding static call, else the named class. */
						const int32_t this_offset = static_cast<int32_t>(
							offsetof(zend_execute_data, This));
						const int32_t this_type = static_cast<int32_t>(
							offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info));
						auto not_this = text_writer.label_create();
						auto fixed = text_writer.label_create();
						auto have_scope = text_writer.label_create();
						ASM(TEST32mi, header_field(descriptor_reg,
							offsetof(zend_native_user_call_site_header,
								fast_flags)),
							ZEND_NATIVE_CALL_FAST_STATIC_THIS);
						generate_raw_jump(Jump::je, not_this);
						ASM(CMP8mi, FE_MEM(canonical_frame_register(), 0,
							FE_NOREG, this_type), IS_OBJECT);
						generate_raw_jump(Jump::jne, fast_miss);
						ASM(MOV64rm, object_reg, FE_MEM(canonical_frame_register(),
							0, FE_NOREG, this_offset));
						generate_raw_jump(Jump::jmp, have_scope);
						label_place(not_this);
						ASM(TEST32mi, header_field(descriptor_reg,
							offsetof(zend_native_user_call_site_header,
								fast_flags)),
							ZEND_NATIVE_CALL_FAST_STATIC_FORWARD);
						generate_raw_jump(Jump::je, fixed);
						ASM(MOV64rm, object_reg, FE_MEM(canonical_frame_register(),
							0, FE_NOREG, this_offset));
						ASM(CMP8mi, FE_MEM(canonical_frame_register(), 0,
							FE_NOREG, this_type), IS_OBJECT);
						generate_raw_jump(Jump::jne, have_scope);
						ASM(MOV64rm, object_reg, FE_MEM(object_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_object, ce))));
						generate_raw_jump(Jump::jmp, have_scope);
						label_place(fixed);
						ASM(MOV64rm, object_reg, header_field(descriptor_reg,
							offsetof(zend_native_user_call_site_header,
								fast_key)));
						label_place(have_scope);
					}
					/* VM stack space for the callee frame. */
					ASM(MOV64rm, callee_reg,
						FE_MEM(context_register(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_top))));
					ASM(MOV64rm, callee_reg,
						FE_MEM(callee_reg, 0, FE_NOREG, 0));
					ASM(MOV64rm, value_reg,
						FE_MEM(context_register(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_end))));
					ASM(MOV64rm, value_reg,
						FE_MEM(value_reg, 0, FE_NOREG, 0));
					ASM(SUB64rr, value_reg, callee_reg);
					{
						ScratchReg size{this};
						auto size_reg = size.alloc_gp();
						ASM(MOV32rm, size_reg, header_field(descriptor_reg,
							offsetof(zend_native_user_call_site_header,
								fast_frame_size)));
						ASM(CMP64rr, value_reg, size_reg);
						generate_raw_jump(Jump::jb, fast_miss);
						ASM(ADD64rr, size_reg, callee_reg);
						ASM(MOV64rm, value_reg,
							FE_MEM(context_register(), 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_execution_context,
									vm_stack_top))));
						ASM(MOV64mr, FE_MEM(value_reg, 0, FE_NOREG, 0),
							size_reg);
					}
					/* zend_vm_init_call_frame(): function, receiver, call
					 * info and argument count. */
					ASM(MOV64rm, value_reg, header_field(descriptor_reg,
						offsetof(zend_native_user_call_site_header,
							fast_function)));
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG, static_cast<int32_t>(
							offsetof(zend_execute_data, func))),
						value_reg);
					if (method || static_call) {
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))),
							object_reg);
						if (temporary_receiver) {
							/* ZEND_CALL_RELEASE_THIS takes the temporary's
							 * reference, as ZEND_INIT_METHOD_CALL moves it. */
							ASM(MOV32mi,
								FE_MEM(canonical_frame_register(), 0, FE_NOREG,
									static_cast<int32_t>(receiver_offset
										+ offsetof(zval, u1.type_info))),
								IS_UNDEF);
						} else if (method && !this_receiver) {
							/* ZEND_CALL_RELEASE_THIS owns one reference. */
							ASM(ADD32mi,
								FE_MEM(object_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))), 1);
						}
					} else {
						ASM(MOV64mi,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))), 0);
					}
					ASM(MOV32rm, value_reg, header_field(descriptor_reg,
						offsetof(zend_native_user_call_site_header,
							fast_call_info)));
					ASM(MOV32mr,
						FE_MEM(callee_reg, 0, FE_NOREG, static_cast<int32_t>(
							offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))),
						value_reg);
					ASM(MOV32mi,
						FE_MEM(callee_reg, 0, FE_NOREG, static_cast<int32_t>(
							offsetof(zend_execute_data, This)
							+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(descriptor->argument_count));
					/* Link the pending call: call->prev_execute_data =
					 * EX(call); EX(call) = call. */
					ASM(MOV64rm, value_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG, static_cast<int32_t>(
							offsetof(zend_execute_data, prev_execute_data))),
						value_reg);
					ASM(MOV64mr,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))),
						callee_reg);
					ASM(MOV32mi, FE_MEM(FE_BP, 0, FE_NOREG, fast_slot), 1);
					/* Falls through to fast_join: the re-arm stub and the miss
					 * path between them are emitted into the cold area. */
				}
				/*
				 * The re-arm stub: zend_native_call_fast_rearm() called with
				 * every caller-saved register preserved by hand, so the
				 * allocator state of the check above holds again at the
				 * retry, and of the universal protocol at the miss.
				 */
				/* The stub runs once per site and request: out of the hot
				 * code. */
				cold_begin();
				label_place(fast_rearm);
				emit_preserving_call(rearm_live_registers,
					ZEND_NATIVE_HELPER_CALL_FAST_REARM, [&] {
						ASM(MOV64rr, FE_SI, rearm_descriptor_reg);
						ASM(MOV64rr, FE_DI, canonical_frame_register());
					}, [&] { ASM(TEST8rr, FE_AX, FE_AX); });
				generate_raw_jump(Jump::jne, fast_retry);
				generate_raw_jump(Jump::jmp, fast_miss);
				cold_end();
				}
				/* A published site takes the universal protocol once per
				 * request at most: out of the hot code. */
				cold_begin();
				label_place(fast_miss);
				ASM(MOV32mi, FE_MEM(FE_BP, 0, FE_NOREG, fast_slot), 0);
			}

			/*
			 * The universal protocol: zend_native_call_universal_init()
			 * reserves the setup frame and activation, resolves the target
			 * and pushes the callee frame out of line, keeping call sites
			 * small now that resolved targets take the fast path.
			 */
			ValuePart resolution_status{
				tpde::x64::PlatformConfig::GP_BANK, 4};
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register(), &assigner), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id, &assigner),
					tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					call.call_site->target_id, &assigner), tpde::CCAssignment{});
				add_const_arg(builder, setup_size, 4);
				add_const_arg(builder, argument_count, 4);
				builder.add_arg(ValuePart{uses_discarded_return
						? uint64_t{UINT32_MAX} : result_offset, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_UNIVERSAL_INIT));
				builder.add_ret(resolution_status, tpde::CCAssignment{});
			}
			auto resolution_spilled = spill_target_branch_state();
			auto resolved = text_writer.label_create();
			ASM(CMP32ri, resolution_status.cur_reg_or_load(this),
				ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS);
			generate_raw_jump(Jump::je, resolved);
			resolution_status.reset(this);
			emit_exception_exit(call.exception_block_id);
			label_place(resolved);
			resolution_status.reset(this);
			reconcile_target_branch_state(resolution_spilled);
			if (fast_site) {
				generate_raw_jump(Jump::jmp, fast_join);
				cold_end();
				label_place(fast_join);
				reconcile_target_branch_state(fast_spilled);
			}
			return true;
		}
		const zend_tpde_instruction &call =
			adaptor->plan()->instructions[node.mir_instruction_index];
		if (call.user_call == nullptr
				|| call.user_call->argument_count != call.call_argument_count) {
			return false;
		}
		auto load_active_activation = [&](auto destination_reg) {
			ASM(MOV64rm, destination_reg,
				FE_MEM(context_register(), 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_execution_context, active_direct_call))));
			ASM(MOV64rm, destination_reg,
				FE_MEM(destination_reg, 0, FE_NOREG, 0));
		};
		/*
		 * The fast path's failure edge: the pending fast frame stays linked in
		 * EX(call), so Zend's unfinished-call cleanup owns it, as in the VM.
		 */
		auto emit_fast_failure = [&]() {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(copy_fixed_argument(
				canonical_frame_register(), &assigner), tpde::CCAssignment{});
			add_const_arg(builder, node.source_position, 4);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			auto prepared = text_writer.label_create();
			ASM(CMP32ri, status_reg, SUCCESS);
			generate_raw_jump(Jump::je, prepared);
			status.reset(this);
			emit_status_return(ZEND_NATIVE_BAILOUT);
			label_place(prepared);
			status.reset(this);
			emit_exception_exit(call.exception_block_id);
		};
		/* A failure of the call's own phase also releases its activation. */
		auto emit_phase_failure = [&]() {
			{
				ScratchReg activation{this};
				auto activation_reg = activation.alloc_gp();
				load_active_activation(activation_reg);
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				ValuePart activation_value{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				activation_value.set_value(this, std::move(activation));
				builder.add_arg(
					std::move(activation_value), tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE));
			}
			emit_fast_failure();
		};
		const bool fast_site = source_call_fast_eligible(call);
		/* A direct scalar send has one TPDE use; the fast and universal
		 * paths share its ValueRef. */
		std::optional<ValueRef> shared_direct;
		/* A fast site's Send branches here for a pending exception. */
		auto send_exception = text_writer.label_create();
		auto direct_value_ref = [&]() -> ValueRef & {
			if (!shared_direct) {
				shared_direct.emplace(val_ref(node.operands[2]));
			}
			return *shared_direct;
		};
		auto compile_universal_send = [&]() -> bool {
			if (node.argument_index >= call.user_call->argument_count) {
				return false;
			}
			if (fast_site && (phase->operand_flags
					& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) == 0) {
				/* A fast site's universal Send is cold: one out-of-line
				 * call keeps the site small. */
				ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(copy_fixed_argument(
						canonical_frame_register(), &assigner), tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					add_const_arg(builder, node.argument_index, 4);
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_CALL_UNIVERSAL_SEND));
					builder.add_ret(status, tpde::CCAssignment{});
				}
				auto sent_universal = text_writer.label_create();
				ASM(CMP32ri, status.cur_reg_or_load(this), SUCCESS);
				generate_raw_jump(Jump::je, sent_universal);
				status.reset(this);
				/* The helper released the activation. */
				generate_raw_jump(Jump::jmp, send_exception);
				label_place(sent_universal);
				status.reset(this);
				return true;
			}
			auto deferred = text_writer.label_create();
			auto completed = text_writer.label_create();
			ScratchReg activation{this};
			ScratchReg placement{this};
			auto activation_reg = activation.alloc_gp();
			auto placement_reg = placement.alloc_gp();
			load_active_activation(activation_reg);
			ASM(MOV64rm, placement_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							placements))));
			activation.reset();
			ASM(ADD64ri, placement_reg,
				static_cast<int32_t>(node.argument_index
					* sizeof(zend_native_user_call_placement)));
			ASM(TEST32mi,
				FE_MEM(placement_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_user_call_placement, flags))),
				ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION);
			generate_raw_jump(Jump::jne, deferred);
			ValuePart send_status{
				tpde::x64::PlatformConfig::GP_BANK, 4};
			if ((phase->operand_flags
					& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) != 0) {
				if (node.operands.size() != 3) {
					return false;
				}
				ScratchReg target{this};
				auto target_reg = target.alloc_gp();
				ASM(MOV32rm, target_reg,
					FE_MEM(placement_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
								zend_native_user_call_placement,
								target_index))));
				placement.reset();
				/* Store the scalar into ZEND_CALL_ARG(call, target + 1) directly, as the
				 * setter helpers do. */
				const zend_mir_scalar_type_mask direct_type =
					adaptor->exact_type(node.operands[2]);
				const int32_t argument_base = static_cast<int32_t>(
					ZEND_CALL_FRAME_SLOT * sizeof(zval));
				const int32_t argument_type = argument_base
					+ static_cast<int32_t>(offsetof(zval, u1.type_info));
				{
					ScratchReg callee{this};
					auto callee_reg = callee.alloc_gp();
					ASM(MOV64rm, callee_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_execute_data, call))));
					ASM(SHL64ri, target_reg, 4);
					ASM(ADD64rr, target_reg, callee_reg);
				}
				auto &direct_value = direct_value_ref();
				auto direct_part = direct_value.part(0);
				if (!store_exact_scalar(target_reg, argument_base, direct_type,
						direct_type == ZEND_MIR_SCALAR_TYPE_NULL
							? AsmReg::make_invalid()
							: direct_part.load_to_reg())) {
					return false;
				}
				generate_raw_jump(Jump::jmp, completed);
			} else {
				/*
				 * A positional by-value SEND of a CV, temporary or literal moves the
				 * value into ZEND_CALL_ARG(call, n) directly unless the resolved target
				 * takes it by reference, packs it into a variadic or places it by name.
				 * Every other case, an undefined CV or a value the VM duplicates takes
				 * the source setter.
				 */
				const zend_native_direct_internal_call_argument &send_argument =
					call.user_call->arguments[node.argument_index];
				const zend_mir_source_operand_ref &send_source =
					send_argument.source_operand;
				const uint32_t send_number = send_argument.auxiliary_payload != 0
					? send_argument.auxiliary_payload : send_argument.ordinal + 1;
				const bool send_slot = send_source.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| send_source.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
				const bool send_cv = send_slot
					&& send_source.slot_kind == ZEND_MIR_SOURCE_SLOT_CV;
				const bool send_tmp = send_slot
					&& send_source.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
				const bool send_literal =
					send_source.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
				const uint64_t send_source_offset = send_literal
					? static_cast<uint64_t>(send_source.index) * sizeof(zval)
					: (uint64_t{ZEND_CALL_FRAME_SLOT} + send_source.index
						+ (send_tmp ? uint64_t{
							adaptor->plan()->source_frame_variable_count} : 0))
						* sizeof(zval);
				const uint64_t send_target_offset =
					(uint64_t{ZEND_CALL_FRAME_SLOT} + send_number - 1) * sizeof(zval);
				const bool send_inline =
					send_argument.mode == ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
					&& send_argument.auxiliary_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					&& (send_argument.source_opcode == ZEND_SEND_VAL
						|| send_argument.source_opcode == ZEND_SEND_VAL_EX
						|| send_argument.source_opcode == ZEND_SEND_VAR
						|| send_argument.source_opcode == ZEND_SEND_VAR_EX)
					&& (send_cv || send_tmp || send_literal)
					&& send_number != 0
					&& send_source_offset <= INT32_MAX - sizeof(zval)
					&& send_target_offset <= INT32_MAX - sizeof(zval);
				auto send_slow = text_writer.label_create();
				if (send_inline) {
					ScratchReg callee{this};
					ScratchReg source{this};
					ScratchReg payload{this};
					ScratchReg type{this};
					auto callee_reg = callee.alloc_gp();
					auto source_reg = source.alloc_gp();
					auto payload_reg = payload.alloc_gp();
					auto type_reg = type.alloc_gp();
					ASM(TEST32mi,
						FE_MEM(placement_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_user_call_placement, flags))),
						ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_SHOULD_REF
							| ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_MUST_REF
							| ZEND_NATIVE_USER_CALL_PLACEMENT_NAMED
							| ZEND_NATIVE_USER_CALL_PLACEMENT_VARIADIC
							| ZEND_NATIVE_USER_CALL_PLACEMENT_EXTRA_NAMED);
					placement.reset();
					generate_raw_jump(Jump::jne, send_slow);
					ASM(MOV64rm, callee_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_execute_data, call))));
					ASM(CMP32mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_execute_data, This)
								+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(send_number));
					generate_raw_jump(Jump::jb, send_slow);
					if (send_literal) {
						ASM(MOV64rm, source_reg,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(offsetof(zend_execute_data, func))));
						ASM(MOV64rm, source_reg,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(zend_op_array, literals))));
						ASM(ADD64ri, source_reg, static_cast<int32_t>(send_source_offset));
					} else {
						ASM(LEA64rm, source_reg,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(send_source_offset)));
					}
					ASM(MOV32rm, type_reg,
						FE_MEM(source_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zval, u1.type_info))));
					ASM(MOVZXr32r8, payload_reg, type_reg);
					if (send_cv) {
						auto plain = text_writer.label_create();
						ASM(CMP32ri, payload_reg, IS_UNDEF);
						generate_raw_jump(Jump::je, send_slow);
						ASM(CMP32ri, payload_reg, IS_REFERENCE);
						generate_raw_jump(Jump::jne, plain);
						ASM(MOV64rm, source_reg, FE_MEM(source_reg, 0, FE_NOREG, 0));
						ASM(ADD64ri, source_reg,
							static_cast<int32_t>(offsetof(zend_reference, val)));
						ASM(MOV32rm, type_reg,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(zval, u1.type_info))));
						label_place(plain);
					} else if (send_tmp) {
						ASM(CMP32ri, payload_reg, IS_REFERENCE);
						generate_raw_jump(Jump::je, send_slow);
						ASM(CMP32ri, payload_reg, IS_INDIRECT);
						generate_raw_jump(Jump::je, send_slow);
					}
					ASM(MOV64rm, payload_reg, FE_MEM(source_reg, 0, FE_NOREG, 0));
					if (!send_tmp) {
						/* ZVAL_COPY_OR_DUP: a counted value gains a reference unless
						 * it is persistent, which the setter duplicates. */
						auto counted_done = text_writer.label_create();
						ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, counted_done);
						if (send_literal) {
							generate_raw_jump(Jump::jmp, send_slow);
						} else {
							ASM(TEST32mi,
								FE_MEM(payload_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, u.type_info))),
								GC_PERSISTENT);
							generate_raw_jump(Jump::jne, send_slow);
							ASM(ADD32mi,
								FE_MEM(payload_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
						}
						label_place(counted_done);
					}
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(send_target_offset)),
						payload_reg);
					ASM(MOV32mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(send_target_offset
								+ offsetof(zval, u1.type_info))),
						type_reg);
					if (send_tmp) {
						ASM(MOV32mi,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(zval, u1.type_info))),
							IS_UNDEF);
					}
					generate_raw_jump(Jump::jmp, completed);
				} else {
					placement.reset();
				}
				label_place(send_slow);
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register(), &assigner), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id, &assigner),
					tpde::CCAssignment{});
				add_const_arg(builder, node.argument_index, 4);
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT));
				builder.add_ret(send_status, tpde::CCAssignment{});
				auto status_reg = send_status.cur_reg_or_load(this);
				ASM(CMP32ri, status_reg, SUCCESS);
				generate_raw_jump(Jump::je, completed);
				send_status.reset(this);
				emit_phase_failure();
			}
			label_place(deferred);
			{
				ScratchReg runtime_activation{this};
				auto runtime_activation_reg =
					runtime_activation.alloc_gp();
				load_active_activation(runtime_activation_reg);
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				ValuePart activation_value{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				activation_value.set_value(
					this, std::move(runtime_activation));
				builder.add_arg(
					std::move(activation_value), tpde::CCAssignment{});
				add_const_arg(builder, node.argument_index, 4);
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_USER_CALL_SEND_RESOLVED_ARGUMENT));
				ValuePart runtime_status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				builder.add_ret(runtime_status, tpde::CCAssignment{});
				auto runtime_status_reg =
					runtime_status.cur_reg_or_load(this);
				ASM(CMP32ri, runtime_status_reg, SUCCESS);
				generate_raw_jump(Jump::je, completed);
				runtime_status.reset(this);
				emit_phase_failure();
			}
			label_place(completed);
			return true;
		};
		if (node.kind == Adaptor::InstKind::UserCallSend && fast_site
				&& source_call_fast_dynamic(call)) {
			if (node.argument_index >= call.user_call->argument_count) {
				return false;
			}
			/* A dynamic fast frame is EX(call): send to it as the VM does. */
			auto fast_spilled = spill_target_branch_state();
			auto universal = text_writer.label_create();
			auto sent = text_writer.label_create();
			ASM(CMP32mi, FE_MEM(FE_BP, 0, FE_NOREG,
				fast_call_slot(node.mir_instruction_index)), 0);
			generate_raw_jump(Jump::je, universal);
			const zend_native_direct_internal_call_argument &send_argument =
				call.user_call->arguments[node.argument_index];
			const bool context_held = val_assignment(adaptor->val_local_idx(
					IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}))
				!= nullptr;
			const uint64_t array_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ send_argument.source_operand.index) * sizeof(zval);
			if (send_argument.source_opcode == ZEND_SEND_ARRAY
					&& send_argument.auxiliary_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					&& (send_argument.source_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| send_argument.source_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& send_argument.source_operand.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV
					&& context_held
					&& array_offset <= INT32_MAX - sizeof(zval)) {
				/*
				 * call_user_func_array($f, $cv) with a packed array without
				 * holes and a target that takes the first twelve arguments
				 * by value (no trampoline): extend the frame on top of the
				 * VM stack and copy the elements as
				 * zend_native_call_fast_send() does. Anything else takes
				 * that helper. The caller-saved registers are free first,
				 * as after its call.
				 */
				const uint64_t callee_saved =
					cur_cc_assigner()->get_ccinfo().callee_saved_regs;
				for (auto reg : tpde::util::BitSetIterator<>{
						register_file.used & ~callee_saved}) {
					if (!register_file.is_fixed(AsmReg{reg})) {
						evict_reg(AsmReg{reg});
					}
				}
				const AsmReg frame_reg = canonical_frame_register();
				const AsmReg context_reg = canonical_value_register(
					IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
				auto helper = text_writer.label_create();
				ScratchReg callee{this};
				ScratchReg table{this};
				ScratchReg count{this};
				ScratchReg work{this};
				ScratchReg element{this};
				ScratchReg target{this};
				const AsmReg callee_reg = callee.alloc_gp();
				const AsmReg table_reg = table.alloc_gp();
				const AsmReg count_reg = count.alloc_gp();
				const AsmReg work_reg = work.alloc_gp();
				const AsmReg element_reg = element.alloc_gp();
				const AsmReg target_reg = target.alloc_gp();
				ASM(MOV64rm, callee_reg, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, call))));
				/* The target: no by-reference parameter among the first
				 * twelve, no trampoline. */
				ASM(MOV64rm, work_reg, FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
				ASM(TEST32mi, FE_MEM(work_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_function, quick_arg_flags))),
					static_cast<int32_t>(0xffffff00u));
				generate_raw_jump(Jump::jne, helper);
				ASM(TEST32mi, FE_MEM(work_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_function, common.fn_flags))),
					static_cast<int32_t>(ZEND_ACC_CALL_VIA_TRAMPOLINE));
				generate_raw_jump(Jump::jne, helper);
				/* The array, through a reference. */
				ASM(LEA64rm, table_reg, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(array_offset)));
				{
					auto direct = text_writer.label_create();
					ASM(CMP8mi, FE_MEM(table_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.v.type))),
						IS_REFERENCE);
					generate_raw_jump(Jump::jne, direct);
					ASM(MOV64rm, table_reg, FE_MEM(table_reg, 0, FE_NOREG, 0));
					ASM(ADD64ri, table_reg, static_cast<int32_t>(
						offsetof(zend_reference, val)));
					label_place(direct);
				}
				ASM(CMP8mi, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.v.type))),
					IS_ARRAY);
				generate_raw_jump(Jump::jne, helper);
				ASM(MOV64rm, table_reg, FE_MEM(table_reg, 0, FE_NOREG, 0));
				ASM(TEST32mi, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(HashTable, u.flags))),
					HASH_FLAG_PACKED);
				generate_raw_jump(Jump::je, helper);
				ASM(MOV32rm, count_reg, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(HashTable, nNumUsed))));
				ASM(CMP32rm, count_reg, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(HashTable, nNumOfElements))));
				generate_raw_jump(Jump::jne, helper);
				/* At most twelve arguments in all, as the flags cover. */
				ASM(MOV32rm, work_reg, FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u2.num_args))));
				ASM(ADD32rr, work_reg, count_reg);
				ASM(CMP32ri, work_reg, MAX_ARG_FLAG_NUM);
				generate_raw_jump(Jump::ja, helper);
				/* zend_vm_stack_extend_call_frame(): room on the stack. */
				ASM(MOV64rm, target_reg, FE_MEM(context_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_execution_context, vm_stack_end))));
				ASM(MOV64rm, target_reg, FE_MEM(target_reg, 0, FE_NOREG, 0));
				ASM(MOV64rm, element_reg, FE_MEM(context_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_execution_context, vm_stack_top))));
				ASM(SUB64rm, target_reg, FE_MEM(element_reg, 0, FE_NOREG, 0));
				ASM(SHR64ri, target_reg, 4);
				ASM(CMP64rr, target_reg, count_reg);
				generate_raw_jump(Jump::jbe, helper);
				/* The arguments start after those already sent. */
				ASM(MOV32rm, target_reg, FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u2.num_args))));
				ASM(MOV32mr, FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u2.num_args))), work_reg);
				ASM(SHL64ri, target_reg, 4);
				ASM(LEA64rm, target_reg, FE_MEM(callee_reg, 1, target_reg,
					static_cast<int32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval))));
				/* EG(vm_stack_top) += count. */
				ASM(MOV64rr, work_reg, count_reg);
				ASM(SHL64ri, work_reg, 4);
				ASM(ADD64mr, FE_MEM(element_reg, 0, FE_NOREG, 0), work_reg);
				ASM(MOV64rm, table_reg, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(HashTable, arPacked))));
				ASM(TEST32rr, count_reg, count_reg);
				generate_raw_jump(Jump::je, sent);
				{
					/* ZVAL_COPY_DEREF() of each element. */
					auto loop = text_writer.label_create();
					auto copy = text_writer.label_create();
					auto counted = text_writer.label_create();
					label_place(loop);
					ASM(MOV32rm, work_reg, FE_MEM(table_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))));
					ASM(MOV64rm, element_reg, FE_MEM(table_reg, 0, FE_NOREG, 0));
					ASM(TEST32ri, work_reg, 0xff00);
					generate_raw_jump(Jump::je, copy);
					ASM(CMP8ri, work_reg, IS_REFERENCE);
					generate_raw_jump(Jump::jne, counted);
					ASM(MOV32rm, work_reg, FE_MEM(element_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_reference, val)
							+ offsetof(zval, u1.type_info))));
					ASM(MOV64rm, element_reg, FE_MEM(element_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_reference, val))));
					ASM(TEST32ri, work_reg, 0xff00);
					generate_raw_jump(Jump::je, copy);
					label_place(counted);
					ASM(ADD32mi, FE_MEM(element_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_refcounted_h, refcount))), 1);
					label_place(copy);
					ASM(MOV64mr, FE_MEM(target_reg, 0, FE_NOREG, 0),
						element_reg);
					ASM(MOV32mr, FE_MEM(target_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
						work_reg);
					ASM(ADD64ri, table_reg, static_cast<int32_t>(sizeof(zval)));
					ASM(ADD64ri, target_reg, static_cast<int32_t>(sizeof(zval)));
					ASM(SUB32ri, count_reg, 1);
					generate_raw_jump(Jump::jne, loop);
				}
				generate_raw_jump(Jump::jmp, sent);
				label_place(helper);
			}
			/* A positional by-value send of a CV, a temporary or an
			 * uncounted literal into a slot the frame has, for a target
			 * that takes the parameter by value: the fast path of
			 * zend_native_call_set_explicit_argument(). */
			const uint32_t send_number =
				send_argument.auxiliary_payload != 0
					? send_argument.auxiliary_payload
					: send_argument.ordinal + 1;
			const zend_mir_source_operand_ref &send_source =
				send_argument.source_operand;
			const bool send_slot = (send_source.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT
					|| send_source.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
				&& (send_source.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
					|| send_source.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP);
			const zval *send_literal = send_source.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL
					&& adaptor->plan()->source_literals != nullptr
				? &adaptor->plan()->source_literals[send_source.index]
				: nullptr;
			const uint64_t send_offset = send_slot
				? (uint64_t{ZEND_CALL_FRAME_SLOT} + send_source.index
					+ (send_source.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
						? adaptor->plan()->source_frame_variable_count : 0))
					* sizeof(zval)
				: 0;
			if (send_argument.mode == ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
					&& send_argument.auxiliary_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					/* SEND_USER sends by value too; a by-reference
					 * parameter (a warning) takes the helper. */
					&& (send_argument.source_opcode == ZEND_SEND_VAL
						|| send_argument.source_opcode == ZEND_SEND_VAL_EX
						|| send_argument.source_opcode == ZEND_SEND_VAR
						|| send_argument.source_opcode == ZEND_SEND_VAR_EX
						|| send_argument.source_opcode == ZEND_SEND_USER)
					&& send_number >= 1 && send_number <= MAX_ARG_FLAG_NUM
					/* Scalar literals only: a string's address is the
					 * process's (images outlive it). */
					&& (send_slot || (send_literal != nullptr
						&& Z_TYPE_P(send_literal) <= IS_DOUBLE))
					&& send_offset <= INT32_MAX - sizeof(zval)) {
				const uint64_t callee_saved =
					cur_cc_assigner()->get_ccinfo().callee_saved_regs;
				for (auto reg : tpde::util::BitSetIterator<>{
						register_file.used & ~callee_saved}) {
					if (!register_file.is_fixed(AsmReg{reg})) {
						evict_reg(AsmReg{reg});
					}
				}
				const AsmReg frame_reg = canonical_frame_register();
				auto helper = text_writer.label_create();
				ScratchReg callee{this};
				ScratchReg work{this};
				ScratchReg value{this};
				const AsmReg callee_reg = callee.alloc_gp();
				const AsmReg work_reg = work.alloc_gp();
				const AsmReg value_reg = value.alloc_gp();
				const int32_t target_offset = static_cast<int32_t>(
					(ZEND_CALL_FRAME_SLOT + send_number - 1) * sizeof(zval));
				ASM(MOV64rm, callee_reg, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, call))));
				ASM(CMP32mi, FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u2.num_args))),
					static_cast<int32_t>(send_number));
				generate_raw_jump(Jump::jb, helper);
				ASM(MOV64rm, work_reg, FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
				ASM(TEST32mi, FE_MEM(work_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_function, common.fn_flags))),
					static_cast<int32_t>(ZEND_ACC_CALL_VIA_TRAMPOLINE));
				generate_raw_jump(Jump::jne, helper);
				/* QUICK_ARG_SHOULD_BE_SENT_BY_REF() (little endian). */
				ASM(TEST32mi, FE_MEM(work_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_function, quick_arg_flags))),
					static_cast<int32_t>(
						uint32_t{ZEND_SEND_BY_REF | ZEND_SEND_PREFER_REF}
							<< ((send_number + 3) * 2)));
				generate_raw_jump(Jump::jne, helper);
				if (send_literal != nullptr) {
					uint64_t bits;
					std::memcpy(&bits, &send_literal->value, sizeof(bits));
					ASM(MOV64ri, value_reg, static_cast<int64_t>(bits));
					ASM(MOV64mr, FE_MEM(callee_reg, 0, FE_NOREG,
						target_offset), value_reg);
					ASM(MOV32mi, FE_MEM(callee_reg, 0, FE_NOREG,
						target_offset + static_cast<int32_t>(
							offsetof(zval, u1.type_info))),
						static_cast<int32_t>(Z_TYPE_INFO_P(send_literal)));
				} else {
					const int32_t source_offset =
						static_cast<int32_t>(send_offset);
					ASM(MOV32rm, work_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						source_offset + static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
					ASM(MOV64rm, value_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						source_offset));
					if (send_source.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
						/* A defined CV, through a reference, gains a
						 * reference. */
						auto copy = text_writer.label_create();
						auto counted = text_writer.label_create();
						ASM(TEST8rr, work_reg, work_reg);
						generate_raw_jump(Jump::je, helper);
						ASM(TEST32ri, work_reg, 0xff00);
						generate_raw_jump(Jump::je, copy);
						ASM(CMP8ri, work_reg, IS_REFERENCE);
						generate_raw_jump(Jump::jne, counted);
						ASM(MOV32rm, work_reg, FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_reference, val)
								+ offsetof(zval, u1.type_info))));
						ASM(MOV64rm, value_reg, FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_reference, val))));
						ASM(TEST32ri, work_reg, 0xff00);
						generate_raw_jump(Jump::je, copy);
						label_place(counted);
						ASM(ADD32mi, FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_refcounted_h, refcount))), 1);
						label_place(copy);
					} else {
						/* A temporary moves, unless it is indirect or a
						 * reference. */
						ASM(CMP8ri, work_reg, IS_INDIRECT);
						generate_raw_jump(Jump::je, helper);
						ASM(CMP8ri, work_reg, IS_REFERENCE);
						generate_raw_jump(Jump::je, helper);
						ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG,
							source_offset + static_cast<int32_t>(
								offsetof(zval, u1.type_info))), IS_UNDEF);
					}
					ASM(MOV64mr, FE_MEM(callee_reg, 0, FE_NOREG,
						target_offset), value_reg);
					ASM(MOV32mr, FE_MEM(callee_reg, 0, FE_NOREG,
						target_offset + static_cast<int32_t>(
							offsetof(zval, u1.type_info))), work_reg);
				}
				generate_raw_jump(Jump::jmp, sent);
				label_place(helper);
			}
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register(), &assigner), tpde::CCAssignment{});
				auto descriptor_value = image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id);
				auto descriptor_scratch =
					std::move(descriptor_value).into_scratch(this);
				ValuePart descriptor_part{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				descriptor_part.set_value(this, std::move(descriptor_scratch));
				builder.add_arg(
					std::move(descriptor_part), tpde::CCAssignment{});
				add_const_arg(builder, node.argument_index, 4);
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_FAST_SEND));
				ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
				builder.add_ret(status, tpde::CCAssignment{});
				ASM(CMP32ri, status.cur_reg_or_load(this),
					ZEND_NATIVE_RETURNED);
				status.reset(this);
			}
			generate_raw_jump(Jump::je, sent);
			generate_raw_jump(Jump::jmp, send_exception);
			/* The universal Send and the exception path run at most once
			 * per site and request: out of the hot code. */
			cold_begin();
			label_place(universal);
			reconcile_target_branch_state(fast_spilled);
			if (!compile_universal_send()) {
				cold_end();
				return false;
			}
			generate_raw_jump(Jump::jmp, sent);
			label_place(send_exception);
			emit_fast_failure();
			cold_end();
			label_place(sent);
			return true;
		}
		if (node.kind == Adaptor::InstKind::UserCallSend && fast_site) {
			if (node.argument_index >= call.user_call->argument_count) {
				return false;
			}
			/* A fast frame takes the argument in ZEND_CALL_ARG(call, n + 1),
			 * as SEND_VAL/SEND_VAR do; the universal send runs otherwise. */
			const int32_t fast_slot =
				fast_call_slot(node.mir_instruction_index);
			const zend_native_direct_internal_call_argument &argument =
				call.user_call->arguments[node.argument_index];
			const zend_mir_source_operand_ref &source =
				argument.source_operand;
			const bool literal =
				source.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
			const bool cv = !literal
				&& source.slot_kind == ZEND_MIR_SOURCE_SLOT_CV;
			/* A VAR a function result filled may hold a reference, which
			 * ZEND_SEND_VAR unwraps as NO_REF_EX does. */
			const bool var_reference = !literal
				&& source.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
				&& (argument.source_opcode == ZEND_SEND_VAR_NO_REF_EX
					|| argument.source_opcode == ZEND_SEND_VAR
					|| argument.source_opcode == ZEND_SEND_VAR_EX);
			const uint64_t source_offset = literal
				? uint64_t{source.index} * sizeof(zval)
				: (uint64_t{ZEND_CALL_FRAME_SLOT} + source.index
					+ (cv ? 0 : uint64_t{
						adaptor->plan()->source_frame_variable_count}))
					* sizeof(zval);
			const uint64_t target_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + node.argument_index)
					* sizeof(zval);
			if (source_offset > INT32_MAX - sizeof(zval)
					|| target_offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			const zend_tpde_source_call_phase_entry *send_phase =
				zend_tpde_source_call_phase_at(
					adaptor->plan(), node.source_position);
			const bool direct = send_phase != nullptr
				&& (send_phase->operand_flags
					& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) != 0;
			if (direct && node.operands.size() != 3) {
				return false;
			}
			if (direct) {
				/* Both paths read the scalar; it enters the branch in a
				 * register so the branch state covers it. */
				auto part = direct_value_ref().part_unowned(0);
				(void) part.load_to_reg();
			}
			auto fast_spilled = spill_target_branch_state();
			auto universal = text_writer.label_create();
			auto sent = text_writer.label_create();
			ASM(CMP32mi, FE_MEM(FE_BP, 0, FE_NOREG, fast_slot), 0);
			generate_raw_jump(Jump::je, universal);
			if (direct) {
				/* A scalar held in a register goes straight into the
				 * argument zval. */
				const zend_mir_scalar_type_mask direct_type =
					adaptor->exact_type(node.operands[2]);
				ScratchReg callee{this};
				auto callee_reg = callee.alloc_gp();
				ASM(MOV64rm, callee_reg,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, call))));
				auto part = direct_value_ref().part_unowned(0);
				if (!store_exact_scalar(callee_reg,
						static_cast<int32_t>(target_offset), direct_type,
						direct_type == ZEND_MIR_SCALAR_TYPE_NULL
							? AsmReg::make_invalid() : part.load_to_reg())) {
					return false;
				}
				part.reset();
				generate_raw_jump(Jump::jmp, sent);
			} else if (cv) {
				/* SEND_VAR of an undefined CV warns and sends null. No scratch
				 * register is live across the warning call. */
				auto defined = text_writer.label_create();
				auto undefined = text_writer.label_create();
				{
					ScratchReg type{this};
					auto type_reg = type.alloc_gp();
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(source_offset
								+ offsetof(zval, u1.type_info))));
					ASM(CMP32ri, type_reg, IS_UNDEF);
					generate_raw_jump(Jump::je, undefined);
				}
				/* The warning path: out of the hot code. */
				cold_begin();
				label_place(undefined);
				/* A by-reference parameter (the site's fast_ref_mask) takes
				 * the undefined CV without a warning; the fast Do makes it
				 * a null reference. */
				auto undefined_null = text_writer.label_create();
				if (argument.source_opcode == ZEND_SEND_VAR_EX
						&& node.argument_index < 32) {
					auto descriptor_value = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id);
					auto descriptor_scratch =
						std::move(descriptor_value).into_scratch(this);
					ASM(TEST32mi,
						FE_MEM(descriptor_scratch.cur_reg(), 0, FE_NOREG,
							-static_cast<int32_t>(sizeof(
								zend_native_user_call_site_header))
							+ static_cast<int32_t>(offsetof(
								zend_native_user_call_site_header,
								fast_ref_mask))),
						static_cast<int32_t>(
							UINT32_C(1) << node.argument_index));
				}
				if (argument.source_opcode == ZEND_SEND_VAR_EX
						&& node.argument_index < 32) {
					generate_raw_jump(Jump::jne, undefined_null);
				}
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(copy_fixed_argument(
						canonical_frame_register(), &assigner), tpde::CCAssignment{});
					add_const_arg(builder, source.index, 4);
					add_const_arg(builder, argument.source_position, 4);
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_CALL_FAST_UNDEFINED_ARGUMENT));
				}
				{
					ScratchReg exception{this};
					auto exception_reg = exception.alloc_gp();
					ASM(MOV64rm, exception_reg,
						FE_MEM(context_register(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context, exception))));
					ASM(MOV64rm, exception_reg,
						FE_MEM(exception_reg, 0, FE_NOREG, 0));
					ASM(TEST64rr, exception_reg, exception_reg);
					generate_raw_jump(Jump::jne, send_exception);
				}
				label_place(undefined_null);
				{
					ScratchReg callee{this};
					auto callee_reg = callee.alloc_gp();
					ASM(MOV64rm, callee_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(MOV32mi,
						FE_MEM(callee_reg, 0, FE_NOREG, static_cast<int32_t>(
							target_offset + offsetof(zval, u1.type_info))),
						IS_NULL);
				}
				generate_raw_jump(Jump::jmp, sent);
				cold_end();
				label_place(defined);
			} else if (var_reference) {
				/* ZEND_SEND_VAR of a reference: the helper moves the
				 * referenced value and releases the reference. No scratch
				 * register is live across the call. */
				auto plain = text_writer.label_create();
				auto reference = text_writer.label_create();
				{
					ScratchReg type{this};
					auto type_reg = type.alloc_gp();
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(source_offset
								+ offsetof(zval, u1.type_info))));
					ASM(CMP32ri, type_reg, IS_REFERENCE);
					generate_raw_jump(Jump::je, reference);
				}
				cold_begin();
				label_place(reference);
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ScratchReg target{this};
					ScratchReg variable{this};
					auto target_reg = target.alloc_gp();
					auto variable_reg = variable.alloc_gp();
					ASM(MOV64rm, target_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(ADD64ri, target_reg,
						static_cast<int32_t>(target_offset));
					ASM(LEA64rm, variable_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(source_offset)));
					ValuePart target_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					target_value.set_value(this, std::move(target));
					builder.add_arg(
						std::move(target_value), tpde::CCAssignment{});
					ValuePart variable_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					variable_value.set_value(this, std::move(variable));
					builder.add_arg(
						std::move(variable_value), tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_CALL_FAST_SEND_VAR_REFERENCE));
				}
				generate_raw_jump(Jump::jmp, sent);
				cold_end();
				label_place(plain);
			}
			if (!direct) {
				ScratchReg address{this};
				ScratchReg payload{this};
				ScratchReg type{this};
				auto address_reg = address.alloc_gp();
				auto payload_reg = payload.alloc_gp();
				auto type_reg = type.alloc_gp();
				if (literal) {
					ASM(MOV64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_op_array, literals))));
					ASM(ADD64ri, address_reg,
						static_cast<int32_t>(source_offset));
				} else {
					ASM(LEA64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(source_offset)));
				}
				if (cv) {
					auto plain = text_writer.label_create();
					ASM(CMP8mi,
						FE_MEM(address_reg, 0, FE_NOREG, static_cast<int32_t>(
							offsetof(zval, u1.type_info))), IS_REFERENCE);
					generate_raw_jump(Jump::jne, plain);
					ASM(MOV64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG, 0));
					ASM(ADD64ri, address_reg, static_cast<int32_t>(
						offsetof(zend_reference, val)));
					label_place(plain);
				}
				ASM(MOV64rm, payload_reg, FE_MEM(address_reg, 0, FE_NOREG, 0));
				ASM(MOV32rm, type_reg,
					FE_MEM(address_reg, 0, FE_NOREG, static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
				if (cv || literal) {
					/* A CV or literal keeps its value; the argument adds a
					 * reference. A temporary moves into the argument. */
					auto uncounted = text_writer.label_create();
					ASM(TEST32ri, type_reg,
						IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
					generate_raw_jump(Jump::je, uncounted);
					ASM(ADD32mi,
						FE_MEM(payload_reg, 0, FE_NOREG, static_cast<int32_t>(
							offsetof(zend_refcounted_h, refcount))), 1);
					label_place(uncounted);
				}
				ASM(MOV64rm, address_reg,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, call))));
				ASM(MOV64mr,
					FE_MEM(address_reg, 0, FE_NOREG,
						static_cast<int32_t>(target_offset)), payload_reg);
				ASM(MOV32mr,
					FE_MEM(address_reg, 0, FE_NOREG, static_cast<int32_t>(
						target_offset + offsetof(zval, u1.type_info))),
					type_reg);
				generate_raw_jump(Jump::jmp, sent);
			}
			/* The universal Send and the exception path: out of the hot
			 * code. */
			cold_begin();
			label_place(universal);
			reconcile_target_branch_state(fast_spilled);
			if (!compile_universal_send()) {
				cold_end();
				return false;
			}
			generate_raw_jump(Jump::jmp, sent);
			cold_end();
			label_place(sent);
			reconcile_target_branch_state(fast_spilled);
			cold_begin();
			label_place(send_exception);
			emit_fast_failure();
			cold_end();
			return true;
		}
		if (node.kind == Adaptor::InstKind::UserCallSend) {
			return compile_universal_send();
		}
		auto compile_universal_check = [&]() -> bool {
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), tpde::CCAssignment{});
			add_const_arg(builder, node.argument_index, 4);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CHECK_FUNC_ARG_RESOLVED));
			ValuePart checked{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(checked, tpde::CCAssignment{});
			auto checked_reg = checked.cur_reg_or_load(this);
			auto valid = text_writer.label_create();
			ASM(CMP32ri, checked_reg, ZEND_NATIVE_CHECK_FUNC_ARG_EXCEPTION);
			generate_raw_jump(Jump::jne, valid);
			checked.reset(this);
			emit_phase_failure();
			label_place(valid);
			ScratchReg active{this};
			ScratchReg callee{this};
			ScratchReg call_info{this};
			auto active_reg = active.alloc_gp();
			auto callee_reg = callee.alloc_gp();
			auto call_info_reg = call_info.alloc_gp();
			load_active_activation(active_reg);
			ASM(MOV64rm, callee_reg,
				FE_MEM(active_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, callee))));
			ASM(MOV32rm, call_info_reg,
				FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info))));
			auto by_value = text_writer.label_create();
			auto updated = text_writer.label_create();
			ASM(CMP32ri, checked_reg, ZEND_NATIVE_CHECK_FUNC_ARG_BY_VALUE);
			generate_raw_jump(Jump::je, by_value);
			ASM(OR32ri, call_info_reg, ZEND_CALL_SEND_ARG_BY_REF);
			generate_raw_jump(Jump::jmp, updated);
			label_place(by_value);
			ASM(AND32ri, call_info_reg,
				static_cast<int32_t>(~ZEND_CALL_SEND_ARG_BY_REF));
			label_place(updated);
			ASM(MOV32mr,
				FE_MEM(callee_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info))), call_info_reg);
			return true;
		};
		if (node.kind == Adaptor::InstKind::UserCallCheck) {
			if (node.source_position >= adaptor->plan()->source_opcode_count) {
				return false;
			}
			const uint8_t source_opcode =
				adaptor->plan()->source_opcodes[node.source_position].opcode;
			if (source_opcode == ZEND_CHECK_UNDEF_ARGS) {
				return node.argument_index == UINT32_MAX;
			}
			if (source_opcode != ZEND_CHECK_FUNC_ARG
					|| node.argument_index >= call.user_call->argument_count) {
				return false;
			}
			/* A fast frame's target takes the argument by value and its
			 * call info has no by-reference send: nothing to check. */
			std::optional<TargetBranchState> check_spilled;
			auto checked_done = text_writer.label_create();
			if (fast_site) {
				check_spilled.emplace(spill_target_branch_state());
				ASM(CMP32mi, FE_MEM(FE_BP, 0, FE_NOREG,
					fast_call_slot(node.mir_instruction_index)), 0);
				generate_raw_jump(Jump::jne, checked_done);
				reconcile_target_branch_state(*check_spilled);
			}
			if (!compile_universal_check()) {
				return false;
			}
			label_place(checked_done);
			return true;
		}
		if (node.kind == Adaptor::InstKind::UserCallExpand) {
			/* A fast frame received every argument by position; the
			 * universal protocol expands out of line. */
			auto fast_expanded = text_writer.label_create();
			auto universal_expand = text_writer.label_create();
			if (fast_site) {
				ASM(CMP32mi, FE_MEM(FE_BP, 0, FE_NOREG,
					fast_call_slot(node.mir_instruction_index)), 0);
				generate_raw_jump(Jump::je, universal_expand);
				cold_begin();
				label_place(universal_expand);
			}
			ValuePart expanded{tpde::x64::PlatformConfig::GP_BANK, 4};
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_UNIVERSAL_EXPAND));
				builder.add_ret(expanded, tpde::CCAssignment{});
			}
			ASM(CMP32ri, expanded.cur_reg_or_load(this), SUCCESS);
			generate_raw_jump(Jump::je, fast_expanded);
			expanded.reset(this);
			emit_fast_failure();
			if (fast_site) {
				cold_end();
			}
			label_place(fast_expanded);
			expanded.reset(this);
			return true;
		}
		if (node.kind != Adaptor::InstKind::UserCallDo) {
			return false;
		}

		{
			ScratchReg opline{this};
			auto opline_reg = opline.alloc_gp();
			ASM(MOV64rm, opline_reg,
				FE_MEM(canonical_frame_register(), 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, opline_reg,
				FE_MEM(opline_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_op_array, opcodes))));
			const uint64_t opline_offset =
				static_cast<uint64_t>(node.source_position) * sizeof(zend_op);
			if (opline_offset > INT32_MAX) {
				return false;
			}
			ASM(ADD64ri, opline_reg, static_cast<int32_t>(opline_offset));
			ASM(MOV64mr,
				FE_MEM(canonical_frame_register(), 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, opline))),
				opline_reg);
		}

		auto do_succeeded = text_writer.label_create();
		auto do_exception = text_writer.label_create();
		if (fast_site) {
			/*
			 * The fast frame (see the Init phase): unlink it from EX(call),
			 * initialize it for its untyped target taking exactly these
			 * arguments (i_init_func_execute_data()), enter the cached native
			 * entry and leave through zend_native_call_fast_leave().
			 */
			const zend_native_user_call_descriptor *descriptor =
				call.user_call;
			const bool dynamic = source_call_fast_dynamic(call);
			const int32_t fast_slot =
				fast_call_slot(node.mir_instruction_index);
			const zend_mir_source_operand_ref &result_operand =
				descriptor->do_result;
			const bool result_used = result_operand.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED;
			const uint64_t result_offset = result_used
				? (uint64_t{ZEND_CALL_FRAME_SLOT}
					+ adaptor->plan()->source_frame_variable_count
					+ result_operand.index) * sizeof(zval)
				: 0;
			if (result_offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			constexpr int32_t header_offset = -static_cast<int32_t>(
				sizeof(zend_native_user_call_site_header));
			auto fast_spilled = spill_target_branch_state();
			auto universal_do = text_writer.label_create();
			ASM(CMP32mi, FE_MEM(FE_BP, 0, FE_NOREG, fast_slot), 0);
			generate_raw_jump(Jump::je, universal_do);
			/*
			 * zend_native_call_fast_do() unlinks, initializes, enters and
			 * leaves the frame out of line; the site keeps one call.
			 */
			AsmReg status_reg{};
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				/* The entry is the fourth SysV argument: reserve RCX before
				 * the earlier arguments are placed. */
				ScratchReg entry{this};
				auto entry_reg = entry.alloc_specific(tpde::x64::AsmReg::CX);
				if (dynamic) {
					ASM(MOV64rm, entry_reg, FE_MEM(FE_BP, 0, FE_NOREG,
						fast_entry_slot(node.mir_instruction_index)));
				} else {
					ASM(XOR32rr, entry_reg, entry_reg);
				}
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register(), &assigner), tpde::CCAssignment{});
				builder.add_arg(context_argument(), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id, &assigner),
					tpde::CCAssignment{});
				ValuePart entry_value{tpde::x64::PlatformConfig::GP_BANK, 8};
				entry_value.set_value(this, std::move(entry));
				builder.add_arg(std::move(entry_value), tpde::CCAssignment{});
				builder.add_arg(ValuePart{result_used
						? result_offset : uint64_t{UINT32_MAX}, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				if (dynamic) {
					/* The Init's choice: the target's native call entry or
					 * zend_native_call_fast_do(). */
					ScratchReg target{this};
					const auto target_reg = target.alloc_specific(
						tpde::x64::AsmReg{tpde::x64::AsmReg::R11});
					ASM(MOV64rm, target_reg, FE_MEM(FE_BP, 0, FE_NOREG,
						fast_do_entry_slot(node.mir_instruction_index)));
					ValuePart target_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					target_value.set_value(this, std::move(target));
					builder.call(std::move(target_value));
				} else {
					/* The published site's fast Do: the target's fast-call
					 * entry or zend_native_call_fast_do(), from the header
					 * before the descriptor in RDX. */
					ScratchReg target{this};
					const auto target_reg = target.alloc_specific(
						tpde::x64::AsmReg{tpde::x64::AsmReg::R11});
					ASM(MOV64rm, target_reg, FE_MEM(FE_DX, 0, FE_NOREG,
						header_offset + static_cast<int32_t>(offsetof(
							zend_native_user_call_site_header,
							fast_do_entry))));
					ValuePart target_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					target_value.set_value(this, std::move(target));
					builder.call(std::move(target_value));
				}
				ValuePart left{tpde::x64::PlatformConfig::GP_BANK, 4};
				builder.add_ret(left, tpde::CCAssignment{});
				const auto left_reg = left.cur_reg_or_load(this);
				status_reg = left_reg;
				ASM(CMP32ri, left_reg, ZEND_NATIVE_RETURNED);
				left.reset(this);
			}
			{
				/* A returned call continues inline; failures, the scalar
				 * contract violation and the universal Do run out of the
				 * hot code. */
				auto failed = text_writer.label_create();
				generate_raw_jump(Jump::jne, failed);
				if ((descriptor->flags
						& ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT) != 0) {
					/* The consumer takes an exact scalar; another result type
					 * violates the contract, as the universal call reports. */
					auto violated = text_writer.label_create();
					{
						ScratchReg type{this};
						auto type_reg = type.alloc_gp();
						ASM(MOVZXr32m8, type_reg,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(result_offset
									+ offsetof(zval, u1.type_info))));
						if (descriptor->result_type
								== ZEND_MIR_SCALAR_TYPE_I1) {
							ASM(SUB32ri, type_reg, IS_FALSE);
							ASM(CMP32ri, type_reg, 1);
							generate_raw_jump(Jump::ja, violated);
						} else {
							ASM(CMP32ri, type_reg,
								descriptor->result_type
										== ZEND_MIR_SCALAR_TYPE_I64
									? IS_LONG : IS_DOUBLE);
							generate_raw_jump(Jump::jne, violated);
						}
					}
					cold_begin();
					label_place(violated);
					{
						tpde::x64::CCAssignerSysV violation_assigner{false};
						CallBuilder violation_builder{
							*this, violation_assigner};
						violation_builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_CALL_FAST_SCALAR_VIOLATION));
					}
					generate_raw_jump(Jump::jmp, do_exception);
				} else {
					cold_begin();
				}
				/* The failure edge leaves the status compare directly: the
				 * status is still in its return register. */
				label_place(failed);
				ASM(CMP32ri, status_reg, ZEND_NATIVE_BAILOUT);
				generate_raw_jump(Jump::jne, do_exception);
				emit_status_return(ZEND_NATIVE_BAILOUT);
			}
			/* The universal Do and the exception path: out of the hot
			 * code, after the failure paths. */
			label_place(universal_do);
			reconcile_target_branch_state(fast_spilled);
		}
		/*
		 * The universal protocol's Do runs out of line in
		 * zend_native_call_universal_do(): trampoline normalization and the
		 * internal or user invocation, releasing the activation.
		 */
		{
			ValuePart done{tpde::x64::PlatformConfig::GP_BANK, 4};
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register(), &assigner), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id, &assigner),
					tpde::CCAssignment{});
				builder.add_arg(context_argument(), tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_UNIVERSAL_DO));
				builder.add_ret(done, tpde::CCAssignment{});
			}
			ASM(CMP32ri, done.cur_reg_or_load(this), ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::je, do_succeeded);
			done.reset(this);
		}
		/* The pending exception of either protocol, once per site. */
		label_place(do_exception);
		emit_fast_failure();
		if (cold_sections_.size() > cold_sections_at_inst_) {
			cold_end();
		}

		label_place(do_succeeded);
		if (node.has_result) {
			const zend_mir_source_operand_ref &result_operand =
				call.user_call->do_result;
			if ((result_operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& result_operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
					|| (result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
						&& result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
						&& result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)) {
				return false;
			}
			const uint64_t storage = result_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_CV
				? result_operand.index
				: static_cast<uint64_t>(
					adaptor->plan()->source_frame_variable_count)
					+ result_operand.index;
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				if (val_parts(node.result).count() != 2) {
					return false;
				}
				auto payload = result.part(0);
				auto type_info = result.part(1);
				ASM(MOV64rm, payload.alloc_reg(),
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(offset)));
				ASM(MOV32rm, type_info.alloc_reg(),
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offset + offsetof(zval, u1.type_info))));
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] = result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				if (adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_F64) {
					ScratchReg payload{this};
					auto payload_reg = payload.alloc_gp();
					ASM(MOV64rm, payload_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offset)));
					ASM(SSE_MOVQ_G2Xrr, result_reg, payload_reg);
				} else if (adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_BOOL) {
					ASM(MOV32rm, result_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offset
								+ offsetof(zval, u1.type_info))));
					ASM(SUB32ri, result_reg, IS_FALSE);
				} else {
					ASM(MOV64rm, result_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offset)));
				}
				result.set_modified();
			}
		}
		return true;
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeCallFragment) {
		if (node.operands.size() != 1
				|| node.mir_instruction_index >=
					adaptor->plan()->instruction_count) {
			return false;
		}
		const zend_tpde_instruction &call =
			adaptor->plan()->instructions[node.mir_instruction_index];
		if (!call.user_opcode_call_fragments || call.user_call == nullptr
				|| (call.entry_cell == nullptr)
					== (call.internal_call_cell == nullptr)) {
			return false;
		}
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{node.operands[0]});
		if (call.entry_cell != nullptr) {
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site->target_id, &assigner), tpde::CCAssignment{});
			add_const_arg(builder, UINT64_C(0), 8);
		} else {
			add_const_arg(builder, UINT64_C(0), 8);
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site->target_id, &assigner), tpde::CCAssignment{});
		}
		builder.add_arg(image_symbol_value(
			ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
			call.id, &assigner), tpde::CCAssignment{});
		add_const_arg(builder, node.argument_index, 4);
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_FRAGMENT));
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
		ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
		builder.add_ret(status, tpde::CCAssignment{});
		builder.add_ret(payload, tpde::CCAssignment{});
		emit_status_tail(std::move(status),
			call.exception_block_id);
		if (node.has_result) {
			set_payload_result(node.result, std::move(payload));
		} else {
			payload.reset(this);
		}
		return true;
	}
	if (node.kind == Adaptor::InstKind::GeneratorGateway
			&& node.deopt_resume) {
		return emit_deopt_gateway(node);
	}
	if (node.kind == Adaptor::InstKind::GeneratorGateway) {
		if (node.operands.size() != 2
				|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				|| node.operands[1]
					!= IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}
				|| adaptor->generator_resume_targets().empty()) {
			return false;
		}
		while (generator_resume_labels_.size()
				< adaptor->generator_resume_targets().size()) {
			generator_resume_labels_.push_back(text_writer.label_create());
		}
		auto normal = text_writer.label_create();
		auto invalid = text_writer.label_create();
		{
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto [context_ref, context] = val_ref_single(node.operands[1]);
			auto frame_reg = frame.load_to_reg();
			auto context_reg = context.load_to_reg();
			/* The gateway uses target-local jumps into ordinary CFG blocks. The
			 * prologue supplies new ABI arguments on every invocation, so refresh
			 * both fixed-argument spill slots before any resume jump. */
			frame.set_modified();
			spill(frame.assignment());
			context.set_modified();
			spill(context.assignment());
			generator_gateway_state_ = spill_target_branch_state();
			generator_gateway_state_.push_back(TargetBranchAssignment{
				adaptor->val_local_idx(IRValueRef{Adaptor::FRAME_VALUE}), 0});
			generator_gateway_state_.push_back(TargetBranchAssignment{
				adaptor->val_local_idx(
					IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}), 0});
			ScratchReg call_info{this};
			auto call_info_reg = call_info.alloc_gp();
			ASM(MOV32rm, call_info_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, call_info_reg, ZEND_CALL_GENERATOR);
			generate_raw_jump(Jump::je, normal);
			call_info.reset();
			ScratchReg opline{this};
			ScratchReg function{this};
			ScratchReg target{this};
			auto opline_reg = opline.alloc_gp();
			auto function_reg = function.alloc_gp();
			auto target_reg = target.alloc_gp();
			ScratchReg exception{this};
			auto exception_reg = exception.alloc_gp();
			ASM(MOV64rm, opline_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, opline))));
			ASM(MOV64rm, function_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, exception_reg,
				FE_MEM(context_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_native_execution_context, exception))));
			ASM(MOV64rm, exception_reg,
				FE_MEM(exception_reg, 0, FE_NOREG, 0));
			for (uint32_t index = 0;
					index < adaptor->generator_resume_targets().size(); ++index) {
				const uint64_t byte_offset =
					uint64_t{adaptor->generator_resume_targets()[index]}
						* sizeof(zend_op);
				if (byte_offset > INT32_MAX) {
					return false;
				}
				ASM(MOV64rm, target_reg,
					FE_MEM(function_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_function, op_array.opcodes))));
				if (byte_offset != 0) {
					ASM(ADD64ri, target_reg,
						static_cast<int32_t>(byte_offset));
				}
				ASM(CMP64rr, opline_reg, target_reg);
				generate_raw_jump(
					Jump::je, generator_resume_labels_[index]);
			}
			ASM(TEST64rr, exception_reg, exception_reg);
			generate_raw_jump(Jump::je, invalid);
			ASM(MOV64rm, opline_reg,
				FE_MEM(context_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_execution_context,
						opline_before_exception))));
			ASM(MOV64rm, opline_reg,
				FE_MEM(opline_reg, 0, FE_NOREG, 0));
			for (uint32_t index = 0;
					index < adaptor->generator_resume_targets().size(); ++index) {
				const uint64_t byte_offset =
					uint64_t{adaptor->generator_resume_targets()[index] - 1}
						* sizeof(zend_op);
				if (byte_offset > INT32_MAX) {
					return false;
				}
				ASM(MOV64rm, target_reg,
					FE_MEM(function_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_function, op_array.opcodes))));
				if (byte_offset != 0) {
					ASM(ADD64ri, target_reg,
						static_cast<int32_t>(byte_offset));
				}
				auto next = text_writer.label_create();
				ASM(CMP64rr, opline_reg, target_reg);
				generate_raw_jump(Jump::jne, next);
				const zend_mir_block_id exception_block =
					adaptor->generator_resume_exception_blocks()[index];
				if (zend_mir_id_is_valid(exception_block)) {
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, opline))),
						opline_reg);
					generate_exception_branch(
						adaptor->block_ref(exception_block));
				} else {
					generate_raw_jump(Jump::jmp, invalid);
				}
				label_place(next);
			}
			generate_raw_jump(Jump::jmp, invalid);
		}
		label_place(invalid);
		emit_status_return(ZEND_NATIVE_EXCEPTION);
		label_place(normal);
		return true;
	}
	if (node.kind == Adaptor::InstKind::GeneratorResume) {
		if (node.argument_index >= generator_resume_labels_.size()
				|| node.operands.empty()
				|| node.operands[0]
					!= IRValueRef{Adaptor::FRAME_VALUE}) {
			return false;
		}
		label_place(generator_resume_labels_[node.argument_index]);
		auto [resume_frame_ref, resume_frame] =
			val_ref_single(node.operands[0]);
		resume_frame.reset();
		resume_frame_ref.reset();
		reconcile_target_branch_state(generator_gateway_state_);
		/* Keep the canonical frame assignment locked while allocating and
		 * reloading every resume-live value. Otherwise register pressure may
		 * evict the frame between two loads while the raw register handle is
		 * still used as their base. */
		const auto frame_local = adaptor->val_local_idx(
			IRValueRef{Adaptor::FRAME_VALUE});
		auto *frame_assignment = val_assignment(frame_local);
		ZEND_ASSERT(frame_assignment != nullptr);
		ValuePartRef frame_value{
			this, frame_local, frame_assignment, 0, false};
		auto frame_reg = frame_value.load_to_reg();
		for (const IRValueRef operand :
				adaptor->generator_resume_values(instruction)) {
			const zend_mir_storage_id storage =
				adaptor->canonical_storage(operand);
			const zend_tpde_machine_value_kind machine_kind =
				adaptor->machine_kind(operand);
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (!zend_mir_id_is_valid(storage)
					|| offset + offsetof(zval, u1.type_info)
						> INT32_MAX) {
				return false;
			}
			auto value = result_ref(operand);
			if (!value.has_assignment()
					|| value.variable_ref()) {
				return false;
			}
			/*
			 * Resume redefines the machine value from its canonical Zend
			 * slot. Invalidate TPDE's spill copy before allocating so no
			 * stale reload is emitted ahead of the authoritative frame load.
			 */
			const ValueParts parts = val_parts(operand);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto assignment = value.part_unowned(part).assignment();
				if (!assignment.register_valid()) {
					continue;
				}
				const auto stale_reg = assignment.get_reg();
				const bool owns_register = register_file.is_used(stale_reg)
						&& register_file.reg_local_idx(stale_reg)
							== adaptor->val_local_idx(operand)
						&& register_file.reg_part(stale_reg) == part;
				if (owns_register) {
					if (assignment.fixed_assignment()) {
						register_file.dec_lock_count_must_zero(stale_reg);
						--assignments.cur_fixed_assignment_count[
							assignment.bank().id()];
					} else if (register_file.is_fixed(stale_reg)) {
						return false;
					}
					register_file.unmark_used(stale_reg);
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
				if (machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL
						&& role == ZEND_TPDE_MACHINE_PART_VALUE) {
					ASM(MOV32rm, value_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offset + offsetof(zval, u1.type_info))));
					ASM(CMP32ri, value_reg, IS_TRUE);
					generate_raw_set(Jump::je, value_reg);
				} else if (machine_kind == ZEND_TPDE_MACHINE_VALUE_F64
						&& role == ZEND_TPDE_MACHINE_PART_VALUE) {
					ASM(SSE_MOVSDrm, value_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(offset)));
				} else if (role == ZEND_TPDE_MACHINE_PART_VALUE
						|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					ASM(MOV64rm, value_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(offset)));
				} else if (role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					ASM(MOV32rm, value_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offset + offsetof(zval, u1.type_info))));
				} else {
					return false;
				}
				value_part.set_modified();
			}
		}
		return true;
	}
	/* A reload whose value nothing uses emits no load; its operands are
	 * still consumed. */
	if (node.kind == Adaptor::InstKind::ZvalPayloadLoad
			&& node.has_result
			&& this->analyzer.liveness_info(
				adaptor->val_local_idx(node.result)).ref_count <= 1) {
		for (IRValueRef operand : node.liveness_operands) {
			auto unused_use = val_ref(operand);
			(void) unused_use;
		}
		return true;
	}
	if (node.kind == Adaptor::InstKind::ZvalGuardArguments) {
		if (node.operands.size() != 1
				|| node.operands[0]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				|| adaptor->argument_guards().empty()) {
			return false;
		}
		auto [frame_ref, frame] = val_ref_single(node.operands[0]);
		auto frame_reg = frame.load_to_reg();
		auto mismatch = text_writer.label_create();
		for (const Adaptor::ArgumentGuard &guard :
				adaptor->argument_guards()) {
			const uint32_t expected_type =
				zend_mir_scalar_type_is_exact(guard.exact_type)
					? zval_type(guard.exact_type)
					: zend_tpde_machine_value_zval_type(
						guard.machine_kind);
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + guard.storage_id)
				* sizeof(zval);
			if (!zend_mir_id_is_valid(guard.storage_id)
					|| expected_type == IS_UNDEF
					|| offset + offsetof(zval, u1.type_info)
						> INT32_MAX) {
				return false;
			}
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offset + offsetof(zval, u1.type_info))));
			if (guard.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				auto matched_boolean = text_writer.label_create();
				ASM(CMP32ri, type_reg, IS_FALSE);
				generate_raw_jump(Jump::je, matched_boolean);
				ASM(CMP32ri, type_reg, IS_TRUE);
				generate_raw_jump(Jump::jne, mismatch);
				type.reset();
				label_place(matched_boolean);
			} else {
				ASM(CMP32ri, type_reg,
					static_cast<int32_t>(expected_type));
				generate_raw_jump(Jump::jne, mismatch);
				type.reset();
			}
		}
		auto matched = text_writer.label_create();
		generate_raw_jump(Jump::jmp, matched);
		frame.reset();
		frame_ref.reset();
		label_place(mismatch);
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETRY, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				::tpde::CCAssignment{});
			return_builder.ret_local_path();
		}
		label_place(matched);
		return true;
	}
	if (node.kind == Adaptor::InstKind::ZvalReferenceResolve) {
		if (node.operands.size() != 1) {
			return false;
		}
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
		const bool frame_slot = adaptor->frame_slot_reference(
			node.operands[0], &storage_id);
		const uint64_t frame_offset = frame_slot
			? (uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval)
			: 0;
		if (frame_offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		if (frame_slot) {
			ASM(LEA64rm, result_reg,
				FE_MEM(canonical_frame_register(), 0, FE_NOREG,
					static_cast<int32_t>(frame_offset)));
		} else {
			auto [address_ref, address] =
				val_ref_single(node.operands[0]);
			ASM(MOV64rr, result_reg, address.load_to_reg());
		}
		ScratchReg type{this};
		ScratchReg referenced{this};
		auto type_reg = type.alloc_gp();
		auto referenced_reg = referenced.alloc_gp();
		ASM(MOVZXr32m8, type_reg,
			FE_MEM(result_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zval, u1.type_info))));
		ASM(MOV64rm, referenced_reg,
			FE_MEM(result_reg, 0, FE_NOREG, 0));
		ASM(ADD64ri, referenced_reg,
			static_cast<int32_t>(offsetof(zend_reference, val)));
		ASM(CMP32ri, type_reg, IS_REFERENCE);
		generate_raw_cmov(
			Jump::je, result_reg, referenced_reg, true);
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::ZvalBoxedStore) {
		if (node.operands.size() != 2
				|| node.operands[1] != IRValueRef{Adaptor::FRAME_VALUE}
				|| adaptor->machine_kind(node.operands[0])
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| !zend_mir_id_is_valid(node.storage_id)) {
			return false;
		}
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + node.storage_id) * sizeof(zval);
		if (offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		auto boxed = val_ref(node.operands[0]);
		auto payload = boxed.part(0);
		auto type_info = boxed.part(1);
		auto [frame_ref, frame] = val_ref_single(node.operands[1]);
		auto frame_reg = frame.load_to_reg();
		auto store_part = [&](ValuePartRef &part, uint32_t part_offset,
				uint32_t size) {
			AsmReg reg;
			ScratchReg stack_reload{this};
			if (part.has_assignment() && part.assignment().stack_valid()) {
				auto assignment = part.assignment();
				reg = stack_reload.alloc_gp();
				load_from_stack(reg, assignment.frame_off(),
					assignment.part_size());
			} else {
				reg = part.load_to_reg();
			}
			if (size == 8) {
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(part_offset)),
					reg);
			} else {
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(part_offset)),
					reg);
			}
		};
		store_part(payload, static_cast<uint32_t>(offset), 8);
		store_part(type_info,
			static_cast<uint32_t>(offset + offsetof(zval, u1.type_info)), 4);
		return true;
	}
	if (node.kind == Adaptor::InstKind::ZvalPayloadLoad) {
		if (node.operands.size() != 1
				|| ((!zend_mir_scalar_type_is_exact(node.exact_type)
						|| node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL)
					&& zend_tpde_machine_value_zval_type(
						adaptor->machine_kind(node.result))
						== IS_UNDEF
					&& adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
			return false;
		}
		const zend_tpde_machine_value_kind result_kind =
			adaptor->machine_kind(node.result);
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
		const bool frame_slot = adaptor->frame_slot_reference(
			node.operands[0], &storage_id);
		const uint64_t frame_offset = frame_slot
			? (uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval)
			: 0;
		if (frame_offset > static_cast<uint64_t>(INT64_MAX)
				- sizeof(zval)) {
			return false;
		}
		if (result_kind != ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& (node.exact_type == ZEND_MIR_SCALAR_TYPE_I64
				|| result_kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| result_kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
				|| result_kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
				|| result_kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
				|| result_kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
			auto result = result_ref(node.result);
			if (frame_slot) {
				return EncodeBase::encode_zend_native_load_u64(
					GenericValuePart{GenericValuePart::Expr{
						canonical_frame_register(),
						static_cast<int64_t>(frame_offset)}},
					result.part(0));
			}
			auto address = val_ref(node.operands[0]);
			return EncodeBase::encode_zend_native_load_u64(
				address.part(0), result.part(0));
		}
		if (frame_slot && frame_offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		if (frame_slot
				&& result_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto result_value = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto value = result_value.part(part);
				auto value_reg = value.alloc_reg();
				const zend_tpde_machine_part_role role =
					parts.representation.parts[part].semantic_role;
				if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					/* A boxed payload is a general-purpose part, a double
					 * included. */
					ASM(MOV64rm, value_reg,
						FE_MEM(canonical_frame_register(), 0,
							FE_NOREG, static_cast<int32_t>(frame_offset)));
				} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					if (node.exact_type
							== ZEND_MIR_SCALAR_TYPE_I64
						|| node.exact_type
							== ZEND_MIR_SCALAR_TYPE_F64) {
						const uint64_t type_info =
							zval_type(node.exact_type);
						materialize_constant(
							&type_info,
							tpde::x64::PlatformConfig::GP_BANK,
							4, value_reg);
					} else {
						ASM(MOV32rm, value_reg,
							FE_MEM(canonical_frame_register(), 0,
								FE_NOREG,
								static_cast<int32_t>(frame_offset
									+ offsetof(zval, u1.type_info))));
					}
				} else {
					return false;
				}
				value.set_modified();
			}
			return true;
		}
		if (frame_offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		auto emit = [&](AsmReg address, int32_t offset) {
			if (result_kind
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result_value = result_ref(node.result);
				const ValueParts parts = val_parts(node.result);
				for (uint32_t part = 0; part < parts.count(); ++part) {
					auto value = result_value.part(part);
					auto value_reg = value.alloc_reg();
					const zend_tpde_machine_part_role role =
						parts.representation.parts[part].semantic_role;
					if (role != ZEND_TPDE_MACHINE_PART_PAYLOAD
							&& role != ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
						return false;
					}
					if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
						ASM(MOV64rm, value_reg,
							FE_MEM(address, 0, FE_NOREG, offset));
					} else {
						ASM(MOV32rm, value_reg,
							FE_MEM(address, 0, FE_NOREG,
								offset + static_cast<int32_t>(
									offsetof(zval, u1.type_info))));
					}
					value.set_modified();
				}
				return true;
			}
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			switch (node.exact_type) {
				case ZEND_MIR_SCALAR_TYPE_I1:
					ASM(MOV32rm, result_reg,
						FE_MEM(address, 0, FE_NOREG,
							offset + static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
					ASM(CMP32ri, result_reg, IS_TRUE);
					generate_raw_set(Jump::je, result_reg);
					break;
				case ZEND_MIR_SCALAR_TYPE_I64:
					ASM(MOV64rm, result_reg,
						FE_MEM(address, 0, FE_NOREG, offset));
					break;
				case ZEND_MIR_SCALAR_TYPE_F64:
					ASM(SSE_MOVSDrm, result_reg,
						FE_MEM(address, 0, FE_NOREG, offset));
					break;
				default:
					switch (result_kind) {
						case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
						case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
						case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
						case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
						case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
							ASM(MOV64rm, result_reg,
								FE_MEM(address, 0, FE_NOREG, offset));
							break;
						default:
							return false;
					}
					break;
			}
			result.set_modified();
			return true;
		};
		if (frame_slot) {
			return emit(canonical_frame_register(),
				static_cast<int32_t>(frame_offset));
		}
		auto [address_ref, address] = val_ref_single(node.operands[0]);
		return emit(address.load_to_reg(), 0);
	}
	const zend_tpde_instruction &mir = adaptor->mir_instruction(instruction);
	const zend_mir_instruction_record record =
		adaptor->instruction_record(instruction);
	if (!zend_mir_id_is_valid(record.id)) {
		return false;
	}
	if (mir.debug_probe) {
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		add_const_arg(builder,
			static_cast<uint32_t>(record.source_position_id), 4);
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_SOURCE_PROBE));
	}
	if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
		if (mir.zval_store_lazy_scalar) {
			if (node.kind != Adaptor::InstKind::MIR
					|| node.operands.size() != 1
					|| node.operands[0]
						!= IRValueRef{Adaptor::FRAME_VALUE}) {
				return false;
			}
			auto frame_value = val_ref(node.operands[0]);
			(void) frame_value;
			return true;
		}
		if (mir.zval_store_plain) {
			const zend_mir_storage_id storage = mir.zval_store_storage_id;
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (node.operands.size() != 2
					|| node.operands[1] != IRValueRef{Adaptor::FRAME_VALUE}
					|| !zend_mir_id_is_valid(storage)
					|| offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			const IRValueRef input = node.operands[0];
			const zend_mir_scalar_type_mask exact_type =
				adaptor->exact_type(input);
			auto frame_value = val_ref(node.operands[1]);
			(void) frame_value;
			const int32_t payload_offset = static_cast<int32_t>(offset);
			const int32_t type_offset = static_cast<int32_t>(
				offset + offsetof(zval, u1.type_info));
			if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				ASM(MOV32mi, FE_MEM(canonical_frame_register(), 0, FE_NOREG,
					type_offset), IS_NULL);
				return true;
			}
			if (!zend_mir_scalar_type_is_exact(exact_type)) {
				return false;
			}
			auto [value_ref, value] = val_ref_single(input);
			auto value_reg = value.load_to_reg();
			if (val_parts(input).bank == tpde::x64::PlatformConfig::FP_BANK) {
				ASM(SSE_MOVSDmr, FE_MEM(canonical_frame_register(), 0,
					FE_NOREG, payload_offset), value_reg);
			} else {
				ASM(MOV64mr, FE_MEM(canonical_frame_register(), 0,
					FE_NOREG, payload_offset), value_reg);
			}
			if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				ScratchReg kind{this};
				auto kind_reg = kind.alloc_gp();
				ASM(MOV32rr, kind_reg, value_reg);
				ASM(ADD32ri, kind_reg, IS_FALSE);
				ASM(MOV32mr, FE_MEM(canonical_frame_register(), 0,
					FE_NOREG, type_offset), kind_reg);
			} else {
				ASM(MOV32mi, FE_MEM(canonical_frame_register(), 0,
					FE_NOREG, type_offset),
					static_cast<int32_t>(zval_type(exact_type)));
			}
			return true;
		}
		if (node.operands.size() != 2
				|| node.operands[1] != IRValueRef{Adaptor::FRAME_VALUE}
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const IRValueRef input = node.operands[0];
		const zend_mir_storage_id storage = mir.zval_store_storage_id;
		const zend_mir_scalar_type_mask exact_type =
			adaptor->exact_type(input);
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		if (!zend_mir_id_is_valid(storage)
				|| !zend_mir_scalar_type_is_exact(exact_type)
				|| mir.runtime_helper
					!= ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW
				|| offset > INT32_MAX
				|| offset + offsetof(zval, u1.type_info) > INT32_MAX) {
			return false;
		}
		if (node.kind == Adaptor::InstKind::GuardedCold) {
			auto input_value = val_ref(node.operands[0]);
			auto frame_value = val_ref(node.operands[1]);
			ScratchReg slot_argument{this};
			auto slot_argument_reg = slot_argument.alloc_gp();
			ASM(MOV64rr, slot_argument_reg, canonical_frame_register());
			ASM(ADD64ri, slot_argument_reg, static_cast<int32_t>(offset));
			ValuePart slot_pointer{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			slot_pointer.set_value(this, std::move(slot_argument));
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(
				std::move(slot_pointer), tpde::CCAssignment{});
			builder.call(runtime_symbol(mir.runtime_helper));
			ScratchReg store_slot{this};
			ScratchReg store_type{this};
			auto store_slot_reg = store_slot.alloc_gp();
			auto store_type_reg = store_type.alloc_gp();
			ASM(MOV64rr, store_slot_reg, canonical_frame_register());
			ASM(ADD64ri, store_slot_reg, static_cast<int32_t>(offset));
			ASM(MOV32rm, store_type_reg,
				FE_MEM(store_slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			auto store_slot_resolved = text_writer.label_create();
			ASM(CMP32ri, store_type_reg, IS_REFERENCE_EX);
			generate_raw_jump(Jump::jne, store_slot_resolved);
			ASM(MOV64rm, store_slot_reg,
				FE_MEM(store_slot_reg, 0, FE_NOREG, 0));
			ASM(ADD64ri, store_slot_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
			label_place(store_slot_resolved);
			if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				ASM(MOV64mi,
					FE_MEM(store_slot_reg, 0, FE_NOREG, 0), 0);
				ASM(MOV32mi,
					FE_MEM(store_slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
					IS_NULL);
			} else {
				auto input = input_value.part(0);
				auto input_reg = input.load_to_reg();
				if (val_parts(node.operands[0]).bank
						== tpde::x64::PlatformConfig::FP_BANK) {
					ASM(SSE_MOVSDmr,
						FE_MEM(store_slot_reg, 0, FE_NOREG, 0),
						input_reg);
				} else {
					ASM(MOV64mr,
						FE_MEM(store_slot_reg, 0, FE_NOREG, 0),
						input_reg);
				}
				if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
					ASM(MOV64rr, store_type_reg, input_reg);
					ASM(ADD64ri, store_type_reg, IS_FALSE);
					ASM(MOV32mr,
						FE_MEM(store_slot_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						store_type_reg);
				} else {
					ASM(MOV32mi,
						FE_MEM(store_slot_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						static_cast<int32_t>(zval_type(exact_type)));
				}
			}
			generate_uncond_branch(
				IRBlockRef{node.continuation_block});
			return true;
		}
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			return false;
		}
		if (node.control_block == UINT32_MAX) {
			return false;
		}
		const auto successors = guarded_successors_of(node);
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		auto slow_release = text_writer.label_create();
		auto store_value = text_writer.label_create();
		auto slot_resolved = text_writer.label_create();
		auto finished = text_writer.label_create();
		ScratchReg old_type{this};
		ScratchReg slot{this};
		ScratchReg counted{this};
		ScratchReg decision{this};
		auto slot_reg = slot.alloc_gp();
		auto old_type_reg = old_type.alloc_gp();
		auto counted_reg = counted.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		/* The old value is released first (no destructor runs here: a
		 * last reference or a collectable one takes the cold path), then
		 * the scalar is stored into the resolved slot. */
		ASM(LEA64rm, slot_reg,
			FE_MEM(frame_reg, 0, FE_NOREG, static_cast<int32_t>(offset)));
		ASM(MOV32rm, old_type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, old_type_reg, IS_REFERENCE_EX);
		generate_raw_jump(Jump::jne, slot_resolved);
		ASM(MOV64rm, slot_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(offsetof(zend_reference, val)));
		ASM(MOV32rm, old_type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		label_place(slot_resolved);
		ASM(TEST32ri, old_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::je, store_value);
		ASM(TEST32ri, old_type_reg,
			IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::jne, slow_release);
		ASM(MOV64rm, counted_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));
		ASM(CMP32mi,
			FE_MEM(counted_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_refcounted_h, refcount))),
			1);
		generate_raw_jump(Jump::jbe, slow_release);
		ASM(SUB32mi,
			FE_MEM(counted_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_refcounted_h, refcount))),
			1);

		label_place(store_value);
		if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
			ASM(MOV64mi, FE_MEM(slot_reg, 0, FE_NOREG, 0), 0);
			ASM(MOV32mi,
				FE_MEM(slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
				IS_NULL);
		} else {
			auto [value_ref, value] = val_ref_single(input);
			auto value_reg = value.load_to_reg();
			if (val_parts(input).bank
					== tpde::x64::PlatformConfig::FP_BANK) {
				ASM(SSE_MOVSDmr, FE_MEM(slot_reg, 0, FE_NOREG, 0),
					value_reg);
			} else {
				ASM(MOV64mr, FE_MEM(slot_reg, 0, FE_NOREG, 0), value_reg);
			}
			if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				ASM(MOV64rr, old_type_reg, value_reg);
				ASM(ADD64ri, old_type_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
					old_type_reg);
			} else {
				ASM(MOV32mi,
					FE_MEM(slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
					static_cast<int32_t>(zval_type(exact_type)));
			}
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, finished);

		label_place(slow_release);
		ASM(MOV32ri, decision_reg, 1);
		label_place(finished);
		old_type.reset();
		slot.reset();
		counted.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	}
	if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR) {
		zend_mir_scalar_type_mask exact_type = mir.source_effect_exact_type;
		if (!zend_mir_scalar_type_is_exact(exact_type)
				|| node.operands.empty()) {
			return false;
		}
		tpde::x64::CCAssignerSysV assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
		if (exact_type == ZEND_MIR_SCALAR_TYPE_F64) {
			builder.add_arg(CallArg{node.operands[0]});
			builder.call(runtime_symbol(mir.runtime_helper));
		} else {
			if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				builder.add_arg(ValuePart{uint64_t{0}, 8,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			} else {
				builder.add_arg(CallArg{node.operands[0]});
			}
			add_const_arg(builder, static_cast<uint32_t>(exact_type), 4);
			builder.call(runtime_symbol(mir.runtime_helper));
		}
		return true;
	}
	auto unary = [&]() {
		return val_ref_single(node.operands[0]);
	};
	auto binary = [&]() {
		return std::pair{val_ref_single(node.operands[0]),
			val_ref_single(node.operands[1])};
	};
	auto copy_result = [&]() {
		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto source_value = val_ref(node.operands[0]);
			auto result_value = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto source = source_value.part(part);
				auto result = result_value.part(part);
				auto source_reg = source.load_to_reg();
				auto result_reg = result.alloc_try_reuse(source);
				if (source_reg != result_reg) {
					mov(result_reg, source_reg, parts.size_bytes(part));
				}
				result.set_modified();
			}
			return true;
		}
		auto [source_ref, source] = unary();
		auto [result_ref, result] = result_ref_single(node.result);
		auto source_reg = source.load_to_reg();
		auto result_reg = result.alloc_try_reuse(source);
		if (source_reg != result_reg) {
			mov(result_reg, source_reg, 8);
		}
		result.set_modified();
		return true;
	};
	auto encode_binary = [&](auto encode) {
		auto left = val_ref(node.operands[0]);
		auto right = val_ref(node.operands[1]);
		auto result = result_ref(node.result);
		return encode(left.part(0), right.part(0), result.part(0));
	};
	auto can_fuse_compare_branch = [&]() {
		if (remaining_instructions.from == remaining_instructions.to) {
			return false;
		}
		const IRInstRef next = *remaining_instructions.from;
		const Adaptor::InstNode &consumer = adaptor->node(next);
		return consumer.kind == Adaptor::InstKind::MIR
			&& consumer.operands.size() == 1
			&& consumer.operands[0] == node.result
			&& adaptor->instruction_record(next).opcode
				== ZEND_MIR_OPCODE_COND_BRANCH
			&& this->analyzer.liveness_info(
				adaptor->val_local_idx(node.result)).ref_count == 2;
	};
	auto fuse_compare_branch = [&](Jump condition) {
		if (!can_fuse_compare_branch()) {
			return false;
		}
		const IRInstRef next = *remaining_instructions.from;
		const Adaptor::InstNode &consumer = adaptor->node(next);
		const auto &successors = adaptor->block_succs(
			IRBlockRef{consumer.control_block});
		if (successors.size() != 2) {
			return false;
		}
		generate_cond_branch(condition, successors[0], successors[1]);
		adaptor->mark_fused(next);
		return true;
	};
	auto integer_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		uint64_t immediate_bits;
		auto left_reg = left.load_to_reg();
		if (adaptor->constant(node.operands[1], &immediate_bits)
				&& static_cast<int64_t>(immediate_bits) >= INT32_MIN
				&& static_cast<int64_t>(immediate_bits) <= INT32_MAX) {
			ASM(CMP64ri, left_reg,
				static_cast<int32_t>(
					static_cast<int64_t>(immediate_bits)));
		} else {
			ASM(CMP64rr, left_reg, right.load_to_reg());
		}
		if (fuse_compare_branch(condition)) {
			return true;
		}
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		generate_raw_set(condition, result_reg);
		result.set_modified();
		return true;
	};
	/*
	 * PHP compares doubles with IEEE semantics: an unordered (NaN) operand
	 * makes <, <= and == false. UCOMISD sets ZF, PF and CF when unordered,
	 * so order the operands to test "above" and check parity for equality.
	 */
	auto floating_compare = [&](zend_mir_opcode opcode) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		if (opcode == ZEND_MIR_OPCODE_F64_EQ) {
			ASM(SSE_UCOMISDrr, left.load_to_reg(), right.load_to_reg());
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ScratchReg ordered{this};
			auto ordered_reg = ordered.alloc_gp();
			generate_raw_set(Jump::je, result_reg);
			generate_raw_set(Jump::jnp, ordered_reg);
			ASM(AND32rr, result_reg, ordered_reg);
			result.set_modified();
			return true;
		}
		const Jump condition = opcode == ZEND_MIR_OPCODE_F64_LT
			? Jump::ja : Jump::jae;
		auto left_reg = left.load_to_reg();
		auto right_reg = right.load_to_reg();
		ASM(SSE_UCOMISDrr, right_reg, left_reg);
		if (fuse_compare_branch(condition)) {
			return true;
		}
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		generate_raw_set(condition, result_reg);
		result.set_modified();
		return true;
	};
	auto materialize_boxed_boundary_operand = [&](uint32_t operand_index,
			zend_mir_storage_id storage) {
		if (operand_index == UINT32_MAX) {
			return true;
		}
		if (!zend_mir_id_is_valid(storage)
				|| operand_index >= node.liveness_operands.size()) {
			return false;
		}
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		if (offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		const IRValueRef operand = node.liveness_operands[operand_index];
		if (adaptor->machine_kind(operand)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			return false;
		}
		auto boxed = val_ref(operand);
		auto payload = boxed.part(0);
		auto type_info = boxed.part(1);
		auto store_part = [&](ValuePartRef &part, uint32_t part_offset,
				uint32_t size) {
			AsmReg reg;
			ScratchReg stack_reload{this};
			if (node.kind == Adaptor::InstKind::GuardedCold
					&& part.has_assignment()
					&& part.assignment().stack_valid()) {
				auto assignment = part.assignment();
				reg = stack_reload.alloc_gp();
				load_from_stack(reg, assignment.frame_off(),
					assignment.part_size());
			} else {
				reg = part.load_to_reg();
			}
			if (size == 8) {
				ASM(MOV64mr,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(part_offset)),
					reg);
			} else {
				ASM(MOV32mr,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(part_offset)),
					reg);
			}
		};
		store_part(payload, static_cast<uint32_t>(offset), 8);
		store_part(type_info,
			static_cast<uint32_t>(offset + offsetof(zval, u1.type_info)), 4);
		return true;
	};
	auto execute_value_operation_with = [&](ValuePart *frame_argument,
			zend_native_runtime_helper_id helper, uint32_t source_opcode) {
		/*
		 * A guarded fast node must never execute the generic helper itself.
		 * The adaptor exposes that call as a distinct allocator-visible cold
		 * block. Executing it here would consume temporary operands once and
		 * then execute the same operation again in GuardedCold.
		 */
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			if (frame_argument != nullptr
					|| node.argument_index == UINT32_MAX) {
				return false;
			}
			consume_operands(node);
			if (node.has_result) {
				auto unreachable_definition = result_ref(node.result);
				unreachable_definition.reset();
			}
			if (node.continuation_block != UINT32_MAX
					&& node.control_block != UINT32_MAX) {
				for (IRValueRef phi : adaptor->block_phis(
						IRBlockRef{node.continuation_block})) {
					const IRValueRef incoming = adaptor->val_as_phi(phi)
						.incoming_val_for_block(IRBlockRef{node.control_block});
					auto unreachable_phi_definition = result_ref(phi);
					auto unreachable_phi_input = val_ref(incoming);
					unreachable_phi_definition.reset();
					unreachable_phi_input.reset();
				}
			}
			generate_branch_to_block(Jump::jmp,
				IRBlockRef{node.argument_index}, false, true);
			continuation_edge_emitted_ = true;
			return true;
		}
		const bool explicit_object_operands =
			zend_tpde_helper_has_unused_operand_payloads(helper);
		const bool explicit_auxiliary =
			zend_tpde_helper_has_explicit_auxiliary(helper);
		if (node.operands.empty()
				|| node.operands.back()
					!= IRValueRef{Adaptor::FRAME_VALUE}
				|| !zend_tpde_helper_has_explicit_operands(helper)
				|| !mir.has_value_operation) {
			return false;
		}
		const zend_mir_executable_value_ref &operation =
			mir.value_operation;
		if (!materialize_boxed_boundary_operand(
				node.boxed_op1_boundary_operand_index,
				operation.op1_storage_id)
				|| !materialize_boxed_boundary_operand(
					node.boxed_op2_boundary_operand_index,
					operation.op2_storage_id)) {
			return false;
		}
		const bool const_include_once =
			helper == ZEND_NATIVE_HELPER_DYNAMIC_INCLUDE_OR_EVAL
			&& operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& operation.result.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			&& (operation.extended_value == ZEND_INCLUDE_ONCE
				|| operation.extended_value == ZEND_REQUIRE_ONCE);
		/* CHECK_FUNC_ARG of a positional argument among the first
		 * MAX_ARG_FLAG_NUM: the VM's quick test of the pending call's
		 * function, which cannot fail. */
		if (helper == ZEND_NATIVE_HELPER_VALUE_CHECK_FUNC_ARG
				&& frame_argument == nullptr && !node.has_result
				&& operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& operation.op2_unused_payload >= 1
				&& operation.op2_unused_payload <= MAX_ARG_FLAG_NUM) {
			static_assert(ZEND_CALL_SEND_ARG_BY_REF == UINT32_C(1) << 31);
			auto frame_use = val_ref(node.operands.back());
			(void) frame_use;
			const uint32_t mask = uint32_t{ZEND_SEND_BY_REF
				| ZEND_SEND_PREFER_REF}
				<< ((operation.op2_unused_payload + 3) * 2);
			const int32_t call_info = static_cast<int32_t>(
				offsetof(zend_execute_data, This)
					+ offsetof(zval, u1.type_info));
			ScratchReg call{this};
			ScratchReg by_reference{this};
			ScratchReg info{this};
			auto call_reg = call.alloc_gp();
			auto by_reference_reg = by_reference.alloc_gp();
			auto info_reg = info.alloc_gp();
			ASM(MOV64rm, call_reg, FE_MEM(canonical_frame_register(), 0,
				FE_NOREG, static_cast<int32_t>(
					offsetof(zend_execute_data, call))));
			ASM(MOV64rm, by_reference_reg, FE_MEM(call_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(TEST32mi, FE_MEM(by_reference_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_function, quick_arg_flags))),
				static_cast<int32_t>(mask));
			generate_raw_set(Jump::jne, by_reference_reg);
			ASM(SHL32ri, by_reference_reg, 31);
			ASM(MOV32rm, info_reg, FE_MEM(call_reg, 0, FE_NOREG, call_info));
			ASM(AND32ri, info_reg, INT32_MAX);
			ASM(OR32rr, info_reg, by_reference_reg);
			ASM(MOV32mr, FE_MEM(call_reg, 0, FE_NOREG, call_info), info_reg);
			return true;
		}
		if (!emit_deopt_stress_transfer(node.mir_instruction_index)) {
			return false;
		}
		/* CHECK_UNDEF_ARGS: only a pending frame flagged as possibly
		 * holding undefined arguments needs the helper. */
		if (helper == ZEND_NATIVE_HELPER_VALUE_CHECK_UNDEF_ARGS
				&& frame_argument == nullptr && !node.has_result) {
			{
				auto frame_use = val_ref(node.operands.back());
				(void) frame_use;
			}
			const AsmReg frame_reg = canonical_frame_register();
			ScratchReg status_scratch{this};
			ScratchReg pending{this};
			const AsmReg status_reg = status_scratch.alloc_gp();
			const AsmReg pending_reg = pending.alloc_gp();
			auto undefined = text_writer.label_create();
			auto checked = text_writer.label_create();
			ASM(MOV64rm, pending_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, call))));
			ASM(TEST32mi, FE_MEM(pending_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, This)
					+ offsetof(zval, u1.type_info))),
				static_cast<int32_t>(ZEND_CALL_MAY_HAVE_UNDEF));
			generate_raw_jump(Jump::jne, undefined);
			ASM(XOR32rr, status_reg, status_reg);
			cold_begin();
			label_place(undefined);
			emit_preserving_call(register_file.used
					& ~(uint64_t{1} << status_reg.id()),
				ZEND_NATIVE_HELPER_CALL_CHECK_UNDEF_FRAME,
				[&] { ASM(LEA64rm, FE_DI, FE_MEM(frame_reg, 0, FE_NOREG, 0)); },
				[&] { ASM(MOV32rr, status_reg, FE_AX); });
			generate_raw_jump(Jump::jmp, checked);
			cold_end();
			label_place(checked);
			pending.reset();
			/* As after a call: the status tail's return path then spills
			 * nothing the continuation would miss. */
			const uint64_t callee_saved =
				cur_cc_assigner()->get_ccinfo().callee_saved_regs;
			for (auto reg : tpde::util::BitSetIterator<>{
					register_file.used & ~callee_saved}) {
				if (!register_file.is_fixed(AsmReg{reg})) {
					evict_reg(AsmReg{reg});
				}
			}
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			status.set_value(this, std::move(status_scratch));
			emit_status_tail(std::move(status), mir.exception_block_id, true);
			return true;
		}
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		zend_tpde_frameless_direct frameless_direct{};
		const bool frameless_inline =
			helper == ZEND_NATIVE_HELPER_CALL_FRAMELESS_INTERNAL
			&& frame_argument == nullptr
			&& zend_tpde_frameless_direct_at(mir, &frameless_direct)
			&& frameless_inline_allowed(operation, frameless_direct);
		std::optional<ScratchReg> frameless_status;
		zend_tpde_concat_assign_direct concat_assign_direct{};
		zend_tpde_concat_direct concat_direct{};
		zend_tpde_dim_direct dim_direct{};
		if (zend_tpde_helper_requires_undef_result(helper, operation)) {
			const uint64_t result_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ operation.result_storage_id) * sizeof(zval)
				+ offsetof(zval, u1.type_info);
			if (result_offset > INT32_MAX - sizeof(uint32_t)) {
				return false;
			}
			ASM(MOV32mi,
				FE_MEM(canonical_frame_register(), 0, FE_NOREG,
					static_cast<int32_t>(result_offset)),
				IS_UNDEF);
		}
		if (frameless_inline) {
			/* The handler takes no frame. */
			auto frame_use = val_ref(node.operands.back());
			(void) frame_use;
		} else if (frame_argument != nullptr) {
			builder.add_arg(
				std::move(*frame_argument), tpde::CCAssignment{});
		} else {
			builder.add_arg(CallArg{node.operands.back()});
		}
		auto encode_operand = [&](const zend_mir_source_operand_ref &operand,
				uint32_t unused_payload) {
			return explicit_object_operands
				? encode_source_operand(operand, unused_payload)
				: encode_source_operand(operand);
		};
		/* ASSIGN_DIM takes the addresses of its container slot, key and
		 * value: the helper decodes nothing. */
		const bool assign_dim_address =
			helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
			&& zend_tpde_dim_direct_at(mir, &dim_direct);
		/* So does a two- or three-argument frameless call. */
		const bool frameless_address =
			helper == ZEND_NATIVE_HELPER_CALL_FRAMELESS_INTERNAL
			&& (source_opcode == ZEND_FRAMELESS_ICALL_2
				|| source_opcode == ZEND_FRAMELESS_ICALL_3)
			&& zend_tpde_frameless_direct_at(mir, &frameless_direct);
		/* INIT_ARRAY and ADD_ARRAY_ELEMENT of literal, CV and temporary
		 * operands take addresses too. */
		struct ValueAddress {
			uint32_t kind;
			uint64_t value;
		};
		auto value_address = [&](const zend_mir_source_operand_ref &operand,
				zend_mir_storage_id storage, ValueAddress *out) {
			if (operand.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				*out = {ZEND_NATIVE_DIM_DIRECT_UNUSED, 0};
				return true;
			}
			if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
				*out = {ZEND_NATIVE_DIM_DIRECT_CONST, operand.index};
				return uint64_t{operand.index} * sizeof(zval) <= INT32_MAX;
			}
			if ((operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
					|| !zend_mir_id_is_valid(storage)
					|| (operand.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
						&& operand.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP)) {
				return false;
			}
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			*out = {operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
					? uint32_t{ZEND_NATIVE_DIM_DIRECT_CV}
					: uint32_t{ZEND_NATIVE_DIM_DIRECT_TMP},
				offset};
			return offset <= INT32_MAX - sizeof(zval);
		};
		ValueAddress address_op1{}, address_op2{}, address_result{};
		const zend_native_runtime_helper_id address_helper =
			helper == ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY
				? ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY_ADDRESS
			: helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT
				? ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT_ADDRESS
			: ZEND_NATIVE_HELPER_COUNT;
		const bool array_address =
			address_helper != ZEND_NATIVE_HELPER_COUNT
			&& value_address(operation.op1, operation.op1_storage_id,
				&address_op1)
			&& value_address(operation.op2, operation.op2_storage_id,
				&address_op2)
			&& value_address(operation.result, operation.result_storage_id,
				&address_result)
			&& address_result.kind != ZEND_NATIVE_DIM_DIRECT_UNUSED
			&& address_result.kind != ZEND_NATIVE_DIM_DIRECT_CONST;
		/* The generic form takes the encoded operands; the direct and
		 * address forms below take offsets or addresses instead. */
		auto add_encoded_op1 = [&] {
			add_const_arg(builder,
				encode_operand( operation.op1, operation.op1_unused_payload), 8);
		};
		if (frameless_inline) {
			/*
			 * zend_native_call_frameless_*(): the opline for warnings, a
			 * null result, then the handler from the context's table on the
			 * result and each argument's (dereferenced) address.
			 */
			const uint64_t descriptor = frameless_direct.descriptor;
			const uint32_t count =
				operation.source_opcode - ZEND_FRAMELESS_ICALL_0;
			const uint32_t handler = static_cast<uint32_t>(descriptor & 0xffff);
			const AsmReg frame_reg = canonical_frame_register();
			const AsmReg context_reg = canonical_value_register(
				IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
			const uint32_t result_offset =
				static_cast<uint32_t>(frameless_direct.slots);
			{
				ScratchReg opline{this};
				const AsmReg opline_reg = opline.alloc_gp();
				const uint64_t opline_offset =
					uint64_t{operation.source_position_id} * sizeof(zend_op);
				ASM(MOV64rm, opline_reg, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
				ASM(MOV64rm, opline_reg, FE_MEM(opline_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_op_array, opcodes))));
				if (opline_offset != 0) {
					ASM(ADD64ri, opline_reg,
						static_cast<int32_t>(opline_offset));
				}
				ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, opline))), opline_reg);
				ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))), IS_NULL);
			}
			auto address_arg = [&](uint32_t offset, bool literal,
					bool may_be_reference) {
				ScratchReg address{this};
				const AsmReg address_reg = address.alloc_gp();
				if (literal) {
					ASM(MOV64rm, address_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, func))));
					ASM(MOV64rm, address_reg, FE_MEM(address_reg, 0,
						FE_NOREG, static_cast<int32_t>(
							offsetof(zend_op_array, literals))));
					ASM(LEA64rm, address_reg, FE_MEM(address_reg, 0,
						FE_NOREG, static_cast<int32_t>(
							offset * sizeof(zval))));
				} else {
					ASM(LEA64rm, address_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offset)));
					if (may_be_reference) {
						auto direct = text_writer.label_create();
						ASM(CMP8mi, FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.v.type))), IS_REFERENCE);
						generate_raw_jump(Jump::jne, direct);
						ASM(MOV64rm, address_reg,
							FE_MEM(address_reg, 0, FE_NOREG, 0));
						ASM(ADD64ri, address_reg, static_cast<int32_t>(
							offsetof(zend_reference, val)));
						label_place(direct);
					}
				}
				ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
				value.set_value(this, std::move(address));
				builder.add_arg(std::move(value), tpde::CCAssignment{});
			};
			address_arg(result_offset, false, false);
			const uint32_t offsets[3] = {
				static_cast<uint32_t>(frameless_direct.slots >> 32),
				static_cast<uint32_t>(frameless_direct.more_slots),
				static_cast<uint32_t>(frameless_direct.more_slots >> 32)};
			for (uint32_t index = 0; index < count; ++index) {
				const bool literal = ((descriptor
					>> (ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT + index)) & 1)
					!= 0;
				const zend_tpde_source_opcode &source =
					adaptor->plan()->source_opcodes[
						operation.source_position_id + (index == 2 ? 1 : 0)];
				const uint32_t may_be = index == 1
					? source.op2_may_be : source.op1_may_be;
				if (!literal && offsets[index] > INT32_MAX - sizeof(zval)) {
					return false;
				}
				address_arg(offsets[index], literal,
					!literal && (may_be & MAY_BE_REF) != 0);
			}
			/* After the arguments, which hold the argument registers. */
			ScratchReg target{this};
			const AsmReg target_reg = target.alloc_gp();
			ASM(MOV64rm, target_reg, FE_MEM(context_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(
					zend_native_execution_context, frameless_handlers))));
			ASM(MOV64rm, target_reg, FE_MEM(target_reg, 0, FE_NOREG,
				static_cast<int32_t>(handler * sizeof(void *))));
			ValuePart target_part{tpde::x64::PlatformConfig::GP_BANK, 8};
			target_part.set_value(this, std::move(target));
			builder.call(std::move(target_part));
			/* The handler returns nothing: an exception is the status.
			 * The context stays in its callee-saved register. */
			frameless_status.emplace(this);
			const AsmReg status_reg = frameless_status->alloc_gp();
			ASM(MOV64rm, status_reg, FE_MEM(context_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(
					zend_native_execution_context, exception))));
			ASM(CMP64mi, FE_MEM(status_reg, 0, FE_NOREG, 0), 0);
			generate_raw_set(Jump::jne, status_reg);
			static_assert(ZEND_NATIVE_RETURNED == 0
				&& ZEND_NATIVE_EXCEPTION == 1);
		} else if (array_address) {
			for (const ValueAddress *operand :
					{&address_op1, &address_op2, &address_result}) {
				ScratchReg address{this};
				const AsmReg address_reg = address.alloc_gp();
				if (operand->kind == ZEND_NATIVE_DIM_DIRECT_UNUSED) {
					ASM(XOR32rr, address_reg, address_reg);
				} else if (operand->kind == ZEND_NATIVE_DIM_DIRECT_CONST) {
					ASM(MOV64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_op_array, literals))));
					ASM(LEA64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								operand->value * sizeof(zval))));
				} else {
					ASM(LEA64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(operand->value)));
				}
				ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
				value.set_value(this, std::move(address));
				builder.add_arg(std::move(value), tpde::CCAssignment{});
			}
			add_const_arg(builder, operation.extended_value, 4);
			builder.add_arg(ValuePart{
				uint64_t{address_op1.kind} | (uint64_t{address_op2.kind} << 2)
					| (uint64_t{address_result.kind} << 4)
					| (uint64_t{source_opcode & 0xff} << 8)
					| (uint64_t{operation.source_position_id} << 32),
				8, tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(address_helper));
		} else if (frameless_address) {
			/* The result slot, then each argument from its literal or
			 * frame slot (the descriptor's CONST bits). */
			auto slot_arg = [&](bool literal, uint32_t offset) {
				ScratchReg address{this};
				const AsmReg address_reg = address.alloc_gp();
				if (literal) {
					ASM(MOV64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_op_array, literals))));
					ASM(LEA64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(offset * sizeof(zval))));
				} else {
					ASM(LEA64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offset)));
				}
				ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
				value.set_value(this, std::move(address));
				builder.add_arg(std::move(value), tpde::CCAssignment{});
			};
			const uint64_t descriptor = frameless_direct.descriptor;
			auto literal_argument = [&](uint32_t index) {
				return ((descriptor
					>> (ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT + index)) & 1)
					!= 0;
			};
			slot_arg(false, static_cast<uint32_t>(frameless_direct.slots));
			slot_arg(literal_argument(0),
				static_cast<uint32_t>(frameless_direct.slots >> 32));
			slot_arg(literal_argument(1),
				static_cast<uint32_t>(frameless_direct.more_slots));
			if (source_opcode == ZEND_FRAMELESS_ICALL_3) {
				slot_arg(literal_argument(2),
					static_cast<uint32_t>(frameless_direct.more_slots >> 32));
			}
			add_const_arg(builder, descriptor, 8);
			builder.call(runtime_symbol(
				source_opcode == ZEND_FRAMELESS_ICALL_3
					? ZEND_NATIVE_HELPER_CALL_FRAMELESS_3_ADDRESS
					: ZEND_NATIVE_HELPER_CALL_FRAMELESS_2_ADDRESS));
		} else if (assign_dim_address) {
			auto address_arg = [&](uint32_t kind, uint32_t offset) {
				ScratchReg address{this};
				const AsmReg address_reg = address.alloc_gp();
				if (kind == ZEND_NATIVE_DIM_DIRECT_CONST) {
					ASM(MOV64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_op_array, literals))));
					ASM(LEA64rm, address_reg,
						FE_MEM(address_reg, 0, FE_NOREG,
							static_cast<int32_t>(offset * sizeof(zval))));
				} else if (kind == ZEND_NATIVE_DIM_DIRECT_UNUSED) {
					ASM(XOR32rr, address_reg, address_reg);
				} else {
					ASM(LEA64rm, address_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(offset)));
				}
				ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
				value.set_value(this, std::move(address));
				builder.add_arg(std::move(value), tpde::CCAssignment{});
			};
			address_arg(ZEND_NATIVE_DIM_DIRECT_CV,
				static_cast<uint32_t>(dim_direct.slots));
			address_arg(static_cast<uint32_t>(dim_direct.descriptor & 3),
				static_cast<uint32_t>(dim_direct.slots >> 32));
			address_arg(static_cast<uint32_t>((dim_direct.descriptor >> 2) & 3),
				static_cast<uint32_t>(dim_direct.more_slots));
			add_const_arg(builder, dim_direct.descriptor, 8);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_ADDRESS));
		} else if (const_include_once) {
			add_encoded_op1();
			add_const_arg(builder, operation.extended_value, 4);
			add_const_arg(builder, operation.source_position_id, 4);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CONST_INCLUDE_ONCE));
		} else if (helper == ZEND_NATIVE_HELPER_CALL_FRAMELESS_INTERNAL
				&& zend_tpde_frameless_direct_at(mir, &frameless_direct)) {
			/* Only ICALL_0 and ICALL_1 get here; ICALL_2 and ICALL_3 take
			 * the address form above. */
			add_const_arg(builder, frameless_direct.descriptor, 8);
			add_const_arg(builder, frameless_direct.slots, 8);
			add_const_arg(builder, frameless_direct.more_slots, 8);
			builder.call(runtime_symbol(
				source_opcode == ZEND_FRAMELESS_ICALL_1
					? ZEND_NATIVE_HELPER_CALL_FRAMELESS_1
					: ZEND_NATIVE_HELPER_CALL_FRAMELESS_DIRECT));
		} else if ((helper == ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R
					|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM)
				&& zend_tpde_dim_direct_at(mir, &dim_direct)) {
			add_const_arg(builder, dim_direct.descriptor, 8);
			add_const_arg(builder, dim_direct.slots, 8);
			add_const_arg(builder, dim_direct.more_slots, 8);
			builder.call(runtime_symbol(
				helper == ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R
					? ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R_DIRECT
					: ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM_DIRECT));
		} else if (helper == ZEND_NATIVE_HELPER_VALUE_BINARY_OP
				&& zend_tpde_identical_direct_at(mir, &concat_direct)) {
			add_const_arg(builder, concat_direct.descriptor, 8);
			add_const_arg(builder, concat_direct.slots, 8);
			add_const_arg(builder, concat_direct.result_offset, 8);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_VALUE_IDENTICAL_DIRECT));
		} else if ((helper == ZEND_NATIVE_HELPER_VALUE_CONCAT
					|| helper == ZEND_NATIVE_HELPER_VALUE_FAST_CONCAT)
				&& zend_tpde_concat_direct_at(mir, &concat_direct)) {
			add_const_arg(builder, concat_direct.descriptor, 8);
			add_const_arg(builder, concat_direct.slots, 8);
			add_const_arg(builder, concat_direct.result_offset, 8);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_VALUE_CONCAT_DIRECT));
		} else if (helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_OP
				&& zend_tpde_concat_assign_direct_at(
					mir, &concat_assign_direct)) {
			add_const_arg(builder, concat_assign_direct.descriptor, 8);
			add_const_arg(builder, concat_assign_direct.slots, 8);
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_VALUE_CONCAT_ASSIGN_DIRECT));
		} else if (helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL) {
			add_encoded_op1();
			add_const_arg(builder, source_opcode, 4);
			add_const_arg(builder, operation.source_position_id, 4);
			builder.call(runtime_symbol(helper));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			if (zend_mir_id_is_valid(mir.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(mir.exception_block_id));
				status.reset(this);
				return true;
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), tpde::CCAssignment{});
			return_builder.ret();
			return true;
		} else {
			add_encoded_op1();
			add_const_arg(builder,
				encode_operand( operation.op2, operation.op2_unused_payload), 8);
			add_const_arg(builder,
				encode_operand( operation.result, operation.result_unused_payload), 8);
			if (explicit_auxiliary) {
				add_const_arg(builder,
					encode_operand(operation.auxiliary, operation.auxiliary_unused_payload), 8);
			}
			add_const_arg(builder, operation.extended_value, 4);
			add_const_arg(builder, source_opcode, 4);
			add_const_arg(builder, operation.source_position_id, 4);
			builder.call(runtime_symbol(helper));
		}
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
		if (frameless_inline) {
			status.set_value(this, std::move(*frameless_status));
		} else {
			builder.add_ret(status, tpde::CCAssignment{});
		}
		emit_status_tail(std::move(status),
			mir.exception_block_id, true);
		/*
		 * Some canonical value operations, such as COUNT, execute only
		 * through this helper path. When the adaptor selected a boxed machine
		 * result for an optimized direct-call argument, snapshot the complete
		 * result before OPcache reuses the temporary frame slot.
		 */
		if (node.kind != Adaptor::InstKind::GuardedCold && node.has_result
				&& adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			const zend_mir_storage_id storage = node.mutation_result
				? operation.op1_storage_id : operation.result_storage_id;
			const uint64_t frame_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (!zend_mir_id_is_valid(storage)
					|| frame_offset > INT32_MAX - sizeof(zval)) {
				return false;
			}
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto value = result.part(part);
				auto value_reg = value.alloc_reg();
				const zend_tpde_machine_part_role role =
					parts.representation.parts[part].semantic_role;
				if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					ASM(MOV64rm, value_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(frame_offset)));
				} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					ASM(MOV32rm, value_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(frame_offset
								+ offsetof(zval, u1.type_info))));
				} else {
					return false;
				}
				value.set_modified();
			}
		}
		return true;
	};
	/*
	 * ROPE_INIT/ROPE_ADD of a literal piece, which is a string: the piece,
	 * copied unless interned, goes into the rope's slot as the VM stores
	 * it. The literal comes from the executing op array.
	 */
	auto rope_literal_inline = [&]() -> bool {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		const zend_tpde_plan *plan = adaptor->plan();
		if (!mir.has_value_operation || node.has_result
				|| operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				|| plan->source_literals == nullptr
				|| Z_TYPE(plan->source_literals[operation.op2.index])
					!= IS_STRING
				|| (operation.source_opcode != ZEND_ROPE_INIT
					&& operation.source_opcode != ZEND_ROPE_ADD)) {
			return false;
		}
		const bool initialize = operation.source_opcode == ZEND_ROPE_INIT;
		const zend_mir_storage_id rope_storage = initialize
			? operation.result_storage_id : operation.op1_storage_id;
		if (!zend_mir_id_is_valid(rope_storage)) {
			return false;
		}
		if (!frame_only_operands(node)) {
			return false;
		}
		const uint64_t piece_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + rope_storage) * sizeof(zval)
			+ (initialize ? 0 : uint64_t{operation.extended_value})
				* sizeof(zend_string *);
		const uint64_t literal_offset =
			uint64_t{operation.op2.index} * sizeof(zval);
		if (piece_offset > INT32_MAX - sizeof(void *)
				|| literal_offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		consume_operands(node);
		const AsmReg frame_reg = canonical_frame_register();
		ScratchReg piece{this};
		const AsmReg piece_reg = piece.alloc_gp();
		auto interned = text_writer.label_create();
		ASM(MOV64rm, piece_reg, FE_MEM(frame_reg, 0, FE_NOREG,
			static_cast<int32_t>(offsetof(zend_execute_data, func))));
		ASM(MOV64rm, piece_reg, FE_MEM(piece_reg, 0, FE_NOREG,
			static_cast<int32_t>(offsetof(zend_op_array, literals))));
		ASM(MOV64rm, piece_reg, FE_MEM(piece_reg, 0, FE_NOREG,
			static_cast<int32_t>(literal_offset)));
		ASM(TEST32mi, FE_MEM(piece_reg, 0, FE_NOREG,
			static_cast<int32_t>(offsetof(zend_refcounted_h, u.type_info))),
			static_cast<int32_t>(IS_STR_INTERNED << GC_FLAGS_SHIFT));
		generate_raw_jump(Jump::jne, interned);
		ASM(ADD32mi, FE_MEM(piece_reg, 0, FE_NOREG,
			static_cast<int32_t>(offsetof(zend_refcounted_h, refcount))), 1);
		label_place(interned);
		ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG,
			static_cast<int32_t>(piece_offset)), piece_reg);
		return true;
	};
	auto execute_value_operation = [&]() {
		return execute_value_operation_with(
			nullptr, mir.runtime_helper,
			mir.has_value_operation
				? mir.value_operation.source_opcode : UINT32_MAX);
	};
	/*
	 * Publish a machine value to its canonical frame slot for a helper that
	 * reads the slot, consuming the value. A cold block reloads parts whose
	 * stack copy is valid instead of trusting the register.
	 */
	auto materialize_cold_operand = [&](
			IRValueRef operand, zend_mir_storage_id storage) {
		if (!zend_mir_id_is_valid(storage)) {
			auto consumed = val_ref(operand);
			(void) consumed;
			return true;
		}
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		if (offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		const zend_tpde_machine_value_kind kind =
			adaptor->machine_kind(operand);
		auto value = val_ref(operand);
		const ValueParts parts = val_parts(operand);
		std::vector<ValuePartRef> locked_parts;
		locked_parts.reserve(parts.count());
		AsmReg payload_reg{};
		bool have_payload = false;
		for (uint32_t part = 0; part < parts.count(); ++part) {
			locked_parts.emplace_back(value.part(part));
			auto &part_value = locked_parts.back();
			AsmReg part_reg;
			ScratchReg stack_reload{this};
			if (node.kind == Adaptor::InstKind::GuardedCold
					&& part_value.has_assignment()
					&& part_value.assignment().stack_valid()) {
				auto assignment = part_value.assignment();
				part_reg = stack_reload.alloc_gp();
				load_from_stack(part_reg, assignment.frame_off(),
					assignment.part_size());
			} else {
				part_reg = part_value.load_to_reg();
			}
			const zend_tpde_machine_part_role role =
				parts.representation.parts[part].semantic_role;
			if (role == ZEND_TPDE_MACHINE_PART_VALUE
					|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				payload_reg = part_reg;
				have_payload = true;
				if (kind == ZEND_TPDE_MACHINE_VALUE_F64) {
					ASM(SSE_MOVSDmr,
						FE_MEM(canonical_frame_register(), 0,
							FE_NOREG, static_cast<int32_t>(offset)),
						part_reg);
				} else {
					ASM(MOV64mr,
						FE_MEM(canonical_frame_register(), 0,
							FE_NOREG, static_cast<int32_t>(offset)),
						part_reg);
				}
			} else if (role
					== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
				ASM(MOV32mr,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offset + offsetof(zval, u1.type_info))),
					part_reg);
			} else {
				return false;
			}
		}
		if (!have_payload) {
			return false;
		}
		if (kind != ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
				ScratchReg type_info{this};
				auto type_info_reg = type_info.alloc_gp();
				ASM(MOV64rr, type_info_reg, payload_reg);
				ASM(ADD64ri, type_info_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offset + offsetof(zval, u1.type_info))),
					type_info_reg);
			} else {
				ScratchReg type_info{this};
				auto type_info_reg = type_info.alloc_gp();
				if (!emit_machine_zval_type_info(
						kind, payload_reg, type_info_reg)) {
					return false;
				}
				ASM(MOV32mr,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(
							offset + offsetof(zval, u1.type_info))),
					type_info_reg);
			}
		}
		return true;
	};
	if (node.kind == Adaptor::InstKind::GuardedCold
			&& record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
		/* Boundary operands are published by execute_value_operation. */
		auto materialize_cold_operand_unless_boundary = [&](
				IRValueRef operand, zend_mir_storage_id storage,
				uint32_t boundary_operand_index) {
			return boundary_operand_index != UINT32_MAX
				|| materialize_cold_operand(operand, storage);
		};
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
				&& node.operands.size() >= 2
				&& (!materialize_cold_operand_unless_boundary(node.operands[0],
						mir.value_operation.op1_storage_id,
						node.boxed_op1_boundary_operand_index)
					|| !materialize_cold_operand_unless_boundary(node.operands[1],
						mir.value_operation.op2_storage_id,
						node.boxed_op2_boundary_operand_index))) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& node.assign_op_right_operand_index
					< node.operands.size()
				&& node.operands[node.assign_op_right_operand_index]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				&& !materialize_cold_operand_unless_boundary(
					node.operands[node.assign_op_right_operand_index],
					mir.value_operation.op2_storage_id,
					node.boxed_op2_boundary_operand_index)) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& node.assign_op_left_operand_index
					< node.operands.size()
				&& node.operands[node.assign_op_left_operand_index]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				&& !materialize_cold_operand_unless_boundary(
					node.operands[node.assign_op_left_operand_index],
					mir.value_operation.op1_storage_id,
					node.boxed_op1_boundary_operand_index)) {
			return false;
		}
		if (node.continuation_block == UINT32_MAX
				|| !execute_value_operation()) {
			return false;
		}
		if (record.source_position_id
					< user_opcode_result_reload_labels_.size()
				&& adaptor->user_opcode_result_reload_source(
					record.source_position_id)) {
			label_place(user_opcode_result_reload_labels_[
				record.source_position_id]);
		}
		if (node.has_result) {
			const zend_mir_storage_id storage =
				node.mutation_result
					? mir.value_operation.op1_storage_id
					: mir.value_operation.result_storage_id;
			const uint64_t frame_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage)
					* sizeof(zval);
			if (!zend_mir_id_is_valid(storage)
					|| frame_offset > INT32_MAX - sizeof(zval)
					|| (node.mutation_result
						? !((adaptor->representation(node.result)
									== ZEND_MIR_REPRESENTATION_I64
								&& adaptor->exact_type(node.result)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& adaptor->machine_kind(node.result)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| (adaptor->representation(node.result)
									== ZEND_MIR_REPRESENTATION_ZVAL
								&& adaptor->machine_kind(node.result)
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
						: !((adaptor->representation(node.result)
										== ZEND_MIR_REPRESENTATION_I64
									&& adaptor->exact_type(node.result)
										== ZEND_MIR_SCALAR_TYPE_I64
									&& adaptor->machine_kind(node.result)
										== ZEND_TPDE_MACHINE_VALUE_I64)
								|| (adaptor->representation(node.result)
										== ZEND_MIR_REPRESENTATION_I1
									&& adaptor->exact_type(node.result)
										== ZEND_MIR_SCALAR_TYPE_I1
									&& adaptor->machine_kind(node.result)
										== ZEND_TPDE_MACHINE_VALUE_BOOL)
								|| adaptor->machine_kind(node.result)
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))) {
				return false;
			}
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64
					|| adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_BOOL) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64rm, result_reg,
					FE_MEM(canonical_frame_register(), 0,
						FE_NOREG,
						static_cast<int32_t>(frame_offset)));
				result.set_modified();
				generate_branch_to_block(Jump::jmp,
					IRBlockRef{node.continuation_block}, false, true);
				return true;
			}
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto value = result.part(part);
				auto value_reg = value.alloc_reg();
				const zend_tpde_machine_part_role role =
					parts.representation.parts[part].semantic_role;
				if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					ASM(MOV64rm, value_reg,
						FE_MEM(canonical_frame_register(), 0,
							FE_NOREG,
							static_cast<int32_t>(frame_offset)));
				} else if (role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					ASM(MOV32rm, value_reg,
						FE_MEM(canonical_frame_register(), 0,
							FE_NOREG,
							static_cast<int32_t>(
								frame_offset
									+ offsetof(zval, u1.type_info))));
				} else {
					return false;
				}
				value.set_modified();
			}
		}
		generate_branch_to_block(Jump::jmp,
			IRBlockRef{node.continuation_block}, false, true);
		return true;
	}
	auto branch_to_guarded_cold = [&]() {
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.argument_index == UINT32_MAX) {
			return false;
		}
		for (IRValueRef operand : node.operands) {
			if (adaptor->machine_reference(operand, nullptr)) {
				continue;
			}
			auto consumed = val_ref(operand);
			(void) consumed;
		}
		if (node.has_result && mir.deopt_exit_resume_plus_one != 0
				&& !adaptor->typed_body()) {
			/*
			 * A deoptimization exit's continuation copies the fast result
			 * (no PHI). The fast path always exits here; define the
			 * unreachable result so the continuation compiles.
			 */
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto value = result.part(part);
				const AsmReg value_reg = value.alloc_reg();
				if (value.bank() == tpde::x64::PlatformConfig::FP_BANK) {
					ASM(SSE_XORPDrr, value_reg, value_reg);
				} else {
					ASM(XOR32rr, value_reg, value_reg);
				}
				value.set_modified();
			}
		} else if (node.has_result) {
			/*
			 * Selection rejected the nominal fast implementation, so this
			 * block has no executable edge to the continuation PHI. Retire
			 * the unreachable fast definition in TPDE's compile-time
			 * liveness without emitting a value.
			 */
			auto unreachable_definition = result_ref(node.result);
			unreachable_definition.reset();
		}
		if (node.continuation_block != UINT32_MAX
				&& node.control_block != UINT32_MAX) {
			for (IRValueRef phi : adaptor->block_phis(
					IRBlockRef{node.continuation_block})) {
				auto unreachable_phi_definition = result_ref(phi);
				auto unreachable_phi_input = val_ref(
					adaptor->val_as_phi(phi).incoming_val_for_block(
						IRBlockRef{node.control_block}));
				unreachable_phi_definition.reset();
				unreachable_phi_input.reset();
			}
		}
		generate_branch_to_block(Jump::jmp,
			IRBlockRef{node.argument_index}, false, true);
		continuation_edge_emitted_ = true;
		return true;
	};
	/* Very large source components can contain hundreds of thousands of
	 * guarded value operations.  Keep their native control flow and canonical
	 * runtime semantics, but avoid repeating the sizeable speculative fast
	 * sequence at every site.  The existing cold block remains ordinary TPDE
	 * code and calls the operation-specific native runtime helper. */
	if (adaptor->compact_guarded_value_operation(instruction)) {
		return branch_to_guarded_cold();
	}
	auto operation_machine_reference =
		[&](zend_tpde_machine_reference_kind expected)
			-> const zend_tpde_machine_reference * {
			const zend_tpde_machine_reference *reference = nullptr;
			return adaptor->operation_machine_reference(
						node.mir_instruction_index, &reference)
					&& reference->kind == expected
				? reference : nullptr;
		};
	auto copy_slot = [&](
			const zend_mir_source_operand_ref &source_operand,
			zend_mir_storage_id source_storage,
			zend_mir_storage_id target_storage,
			zend_mir_storage_id result_storage,
			bool move_source, bool fresh_target = false) {
		/* A literal source is copied from the literal table, as
		 * ZEND_ASSIGN copies a CONST operand. A fresh target, the result
		 * temporary of QM_ASSIGN, holds nothing to release, as the VM's
		 * QM_ASSIGN overwrites it. */
		const bool literal_source =
			source_operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
		const uint64_t literal_offset =
			uint64_t{source_operand.index} * sizeof(zval);
		if (literal_source) {
			move_source = false;
		}
		if ((literal_source
				? literal_offset > INT32_MAX - sizeof(zval)
				: source_storage == ZEND_MIR_ID_INVALID)
				|| target_storage == ZEND_MIR_ID_INVALID
				|| source_storage == target_storage
				|| (result_storage != ZEND_MIR_ID_INVALID
					&& (result_storage == source_storage
						|| result_storage == target_storage))) {
			return branch_to_guarded_cold();
		}
		const uint64_t source_offset = literal_source ? 0
			: (uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		const uint64_t target_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + target_storage) * sizeof(zval);
		const uint64_t result_offset = result_storage == ZEND_MIR_ID_INVALID
			? 0 : (uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
		if (source_offset > INT32_MAX - sizeof(zval)
				|| target_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return branch_to_guarded_cold();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors = guarded_successors_of(node);

		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg source_type{this};
		ScratchReg target_type{this};
		ScratchReg source_payload{this};
		ScratchReg low_word{this};
		ScratchReg probe{this};
		ScratchReg decision{this};
		ScratchReg target_address{this};
		auto source_type_reg = source_type.alloc_gp();
		auto target_type_reg = target_type.alloc_gp();
		auto source_payload_reg = source_payload.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto probe_reg = probe.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		auto target_reg = target_address.alloc_gp();

		const bool register_source = !literal_source
			&& !node.operands.empty()
			&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			&& adaptor->machine_value_is_register_authoritative(
				node.operands[0]);

		if (register_source) {
			const zend_tpde_machine_value_kind source_kind =
				adaptor->machine_kind(node.operands[0]);
			if (source_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto source = val_ref(node.operands[0]);
				const ValueParts parts = val_parts(node.operands[0]);
				if (parts.count() != 2) {
					return false;
				}
				for (uint32_t part = 0; part < parts.count(); ++part) {
					auto value = source.part(part);
					auto value_reg = value.load_to_reg();
					switch (parts.representation.parts[part].semantic_role) {
						case ZEND_TPDE_MACHINE_PART_PAYLOAD:
							ASM(MOV64rr, source_payload_reg,
								value_reg);
							break;
						case ZEND_TPDE_MACHINE_PART_TYPE_INFO:
							ASM(MOV32rr, source_type_reg,
								value_reg);
							break;
						default:
							return false;
					}
				}
			} else {
				auto [source_ref, source] =
					val_ref_single(node.operands[0]);
				if (source_kind == ZEND_TPDE_MACHINE_VALUE_F64) {
					ASM(SSE_MOVQ_X2Grr, source_payload_reg,
						source.load_to_reg());
				} else {
					ASM(MOV64rr, source_payload_reg,
						source.load_to_reg());
				}
				if (source_kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
					ASM(MOV64rr, source_type_reg,
						source_payload_reg);
					ASM(ADD32ri, source_type_reg, IS_FALSE);
				} else {
					if (!emit_machine_zval_type_info(
							source_kind, source_payload_reg,
							source_type_reg)) {
						return false;
					}
				}
			}
		} else if (literal_source) {
			ASM(MOV64rm, probe_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, probe_reg,
				FE_MEM(probe_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_op_array, literals))));
			ASM(MOV64rm, source_payload_reg,
				FE_MEM(probe_reg, 0, FE_NOREG,
					static_cast<int32_t>(literal_offset)));
			ASM(MOV32rm, source_type_reg,
				FE_MEM(probe_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						literal_offset + offsetof(zval, u1.type_info))));
		} else {
			ASM(MOV32rm, source_type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						source_offset + offsetof(zval, u1.type_info))));
			ASM(MOV64rm, source_payload_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(source_offset)));
		}
		if (!literal_source
				&& source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(CMP32ri, source_type_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
		}
		if (!literal_source && !register_source && !move_source
				&& source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			/* A CV bound by global, static or & is read through its
			 * reference and copied, as ZEND_ASSIGN dereferences it. */
			auto plain_source = text_writer.label_create();
			ASM(CMP8ri, source_type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::jne, plain_source);
			ASM(MOV32rm, source_type_reg,
				FE_MEM(source_payload_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_reference, val)
						+ offsetof(zval, u1.type_info))));
			ASM(MOV64rm, source_payload_reg,
				FE_MEM(source_payload_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_reference, val))));
			ASM(CMP8ri, source_type_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
			/* The target may be the same reference, whose release must
			 * not free the value copied: the helper handles that. */
			if (!fresh_target) {
				ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(target_offset
						+ offsetof(zval, u1.v.type))), IS_REFERENCE);
				generate_raw_jump(Jump::je, slow);
			}
			label_place(plain_source);
		} else if (!literal_source
				&& (source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
					|| source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)) {
			ASM(CMP8ri, source_type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
		}
		/*
		 * Immutable refcounted constants need the VM's duplication rules
		 * before they become a writable CV.  They also must never receive a
		 * native refcount write.  Keep that ownership transition on the
		 * semantic cold path.
		 */
		ASM(TEST32ri, source_type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto source_mutable = text_writer.label_create();
		generate_raw_jump(Jump::je, source_mutable);
		ASM(MOV32rm, probe_reg,
			FE_MEM(source_payload_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, u.type_info))));
		ASM(TEST32ri, probe_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::jne, slow);
		label_place(source_mutable);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV32rm, probe_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						result_offset + offsetof(zval, u1.type_info))));
			ASM(CMP32ri, probe_reg, IS_DOUBLE);
			generate_raw_jump(Jump::ja, slow);
		}
		ASM(LEA64rm, target_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(target_offset)));
		auto target_released = text_writer.label_create();
		if (!fresh_target) {
			/* A CV bound by global, static or & is written through its
			 * reference, like zend_assign_to_variable(); a reference with
			 * typed property sources needs the helper's coercion. */
			ASM(MOV32rm, target_type_reg,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP8ri, target_type_reg, IS_REFERENCE);
			auto target_plain = text_writer.label_create();
			auto target_reference = text_writer.label_create();
			generate_raw_jump(Jump::je, target_reference);
			/* The reference is followed out of the hot code. */
			const bool cold_reference = !text_writer.in_cold_area();
			if (cold_reference) {
				cold_begin();
			} else {
				generate_raw_jump(Jump::jmp, target_plain);
			}
			label_place(target_reference);
			ASM(MOV64rm, target_reg, FE_MEM(target_reg, 0, FE_NOREG, 0));
			ASM(CMP64mi,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_reference, sources.ptr))),
				0);
			generate_raw_jump(Jump::jne, slow);
			ASM(ADD64ri, target_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
			ASM(MOV32rm, target_type_reg,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			if (cold_reference) {
				generate_raw_jump(Jump::jmp, target_plain);
				cold_end();
			}
			label_place(target_plain);
			ASM(TEST32ri, target_type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::je, target_released);
			/*
			 * The old value keeps another owner, so only its refcount
			 * drops. GC_DTOR_NO_REF() must still purple a shared
			 * collectable value that may leak; one already buffered or not
			 * collectable needs nothing more. The helper does the rest.
			 * This is the last check: the release follows directly.
			 * A string's last owner frees it out of line, which runs no
			 * code and raises nothing, before the store.
			 */
			auto release = text_writer.label_create();
			ASM(MOV64rm, low_word_reg,
				FE_MEM(target_reg, 0, FE_NOREG, 0));
			ASM(MOV32rm, probe_reg,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))));
			ASM(CMP32ri, probe_reg, 1);
			{
				auto last_owner = text_writer.label_create();
				generate_raw_jump(Jump::jle, last_owner);
				const bool cold_free = !text_writer.in_cold_area();
				auto freed_join = text_writer.label_create();
				if (cold_free) {
					cold_begin();
				} else {
					generate_raw_jump(Jump::jmp, freed_join);
				}
				label_place(last_owner);
				ASM(CMP8ri, target_type_reg, IS_STRING);
				generate_raw_jump(Jump::jne, slow);
				emit_preserving_call(static_cast<uint64_t>(
						register_file.used),
					ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW, [&] {
						ASM(MOV64rr, FE_DI, target_reg);
					}, [] {});
				generate_raw_jump(Jump::jmp, target_released);
				if (cold_free) {
					cold_end();
				}
				label_place(freed_join);
			}
			ASM(TEST32ri, target_type_reg,
				IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::je, release);
			ASM(TEST32mi,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, u.type_info))),
				static_cast<int32_t>(
					GC_INFO_MASK | (GC_NOT_COLLECTABLE << GC_FLAGS_SHIFT)));
			generate_raw_jump(Jump::je, slow);
			label_place(release);
			ASM(SUB32mi,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				1);
		}
		label_place(target_released);
		ASM(MOV64rr, low_word_reg, source_payload_reg);
		const uint32_t source_refcount_increments =
			(!move_source ? 1 : 0)
			+ (result_storage != ZEND_MIR_ID_INVALID ? 1 : 0);
		if (source_refcount_increments != 0) {
			ASM(TEST32ri, source_type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			auto value_owned = text_writer.label_create();
			generate_raw_jump(Jump::je, value_owned);
			ASM(ADD32mi,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				source_refcount_increments);
			label_place(value_owned);
		}
		ASM(MOV64mr,
			FE_MEM(target_reg, 0, FE_NOREG, 0),
			low_word_reg);
		ASM(MOV32mr,
			FE_MEM(target_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			source_type_reg);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset)),
				low_word_reg);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						result_offset + offsetof(zval, u1.type_info))),
				source_type_reg);
		}
		if (move_source) {
			ASM(MOV32ri, source_type_reg, IS_UNDEF);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
					source_offset + offsetof(zval, u1.type_info))),
				source_type_reg);
		}
		if (!register_source) {
			/* Nothing to publish on the cold edge: leave directly. */
			source_type.reset();
			target_type.reset();
			source_payload.reset();
			low_word.reset();
			probe.reset();
			target_address.reset();
			if (guarded_exit_can_jump_directly(successors[1], successors[0])) {
				decision.reset();
				frame_scratch.reset();
				generate_guarded_direct_exit(
					slow, successors[1], successors[0]);
				return true;
			}
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		if (register_source) {
			/*
			 * The helper consumes canonical Zend slots.  Keep the source
			 * register-authoritative on the hot edge and materialize it only
			 * when the generated guard actually selects the cold edge.
			 */
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(source_offset)),
				source_payload_reg);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						source_offset + offsetof(zval, u1.type_info))),
				source_type_reg);
		}
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		source_type.reset();
		target_type.reset();
		source_payload.reset();
		low_word.reset();
		probe.reset();
		target_address.reset();
		frame_scratch.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	auto copy_temporary_slot = [&]() {
		const zend_mir_storage_id source_storage =
			mir.value_operation.op1_storage_id;
		const zend_mir_storage_id result_storage =
			mir.value_operation.result_storage_id;
		if (source_storage == ZEND_MIR_ID_INVALID
				|| result_storage == ZEND_MIR_ID_INVALID
				|| source_storage == result_storage) {
			return branch_to_guarded_cold();
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
		if (source_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return branch_to_guarded_cold();
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg probe{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto probe_reg = probe.alloc_gp();

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					source_offset + offsetof(zval, u1.type_info))));
		ASM(MOV64rm, value_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(source_offset)));
		ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto copied = text_writer.label_create();
		generate_raw_jump(Jump::je, copied);
		ASM(ADD32mi,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(copied);
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset)),
			value_reg);
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					result_offset + offsetof(zval, u1.type_info))),
			type_reg);
		return true;
	};
	auto free_temporary_slot = [&]() {
		const zend_mir_storage_id source_storage =
			mir.value_operation.op1_storage_id;
		if (source_storage == ZEND_MIR_ID_INVALID) {
			return branch_to_guarded_cold();
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		if (source_offset > INT32_MAX - sizeof(zval)) {
			return branch_to_guarded_cold();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors = guarded_successors_of(node);
		auto slow = text_writer.label_create();
		auto released = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg probe{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto probe_reg = probe.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					source_offset + offsetof(zval, u1.type_info))));
		ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::je, released);
		ASM(MOV64rm, value_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(source_offset)));
		ASM(MOV32rm, probe_reg,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))));
		ASM(CMP32ri, probe_reg, 1);
		generate_raw_jump(Jump::jle, slow);
		ASM(SUB32mi,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(released);
		ASM(MOV32ri, type_reg, IS_UNDEF);
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					source_offset + offsetof(zval, u1.type_info))),
			type_reg);
		ASM(MOV32ri, decision_reg, 0);
		auto done = text_writer.label_create();
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		type.reset();
		value.reset();
		probe.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	/*
	 * FETCH_DIM_R, FETCH_DIM_IS (??), isset() and empty() of an array
	 * element whose container, key and result live in frame slots or
	 * literals, composed of EncodeGen snippets: zend_native_array_find_literal() or _find_key() probe the
	 * element without changing anything, a temporary container is consumed
	 * only when another owner keeps it alive, and every undecided probe takes
	 * the guarded cold block, whose helper repeats the whole operation.
	 * Returns 1 when emitted, 0 when the form does not apply and -1 on an
	 * encoding failure.
	 */
	/* Coalesce is FETCH_DIM_IS: a read whose missing key or scalar
	 * container yields null without a diagnostic. Write is FETCH_DIM_W, RW
	 * and UNSET, which return the INDIRECT of an existing element of an
	 * unshared array. */
	enum class ElementAccess : uint8_t {
		Read, Coalesce, Write, Isset, Empty, Assign
	};
	/* The read of an isset's element (lookup_reuse_consumer()). */
	IRInstRef reuse_read{UINT32_MAX};
	auto array_element = [&](ElementAccess access) -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return 0;
		}
		/* A read's integer key may be a register value that its frame
		 * slot does not canonically hold: the first operand, an exact
		 * integer, used from its register. */
		const bool register_key = (access == ElementAccess::Read
				|| access == ElementAccess::Coalesce
				|| access == ElementAccess::Write)
			&& operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& !node.operands.empty()
			&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			&& !zend_mir_id_is_valid(
				adaptor->canonical_storage(node.operands[0]))
			&& adaptor->machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_I64
			&& adaptor->exact_type(node.operands[0])
				== ZEND_MIR_SCALAR_TYPE_I64
			&& adaptor->representation(node.operands[0])
				== ZEND_MIR_REPRESENTATION_I64;
		/* A container or key held as a machine value is published to its
		 * frame slot first, as the helper of the cold block reads it. */
		std::vector<std::pair<IRValueRef, zend_mir_storage_id>> published;
		for (IRValueRef operand : node.operands) {
			if (operand == IRValueRef{Adaptor::FRAME_VALUE}
					|| operand == IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT}) {
				continue;
			}
			/* An element reference only describes the packed form; the
			 * constant of a literal integer key is read from the literal
			 * table. */
			uint64_t constant_bits = 0;
			if ((register_key && operand == node.operands[0])
					|| adaptor->machine_reference(operand, nullptr)
					|| (operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& adaptor->constant(operand, &constant_bits)
						&& adaptor->plan()->source_literals != nullptr
						&& operation.op2.index
							< adaptor->plan()->source_literal_count
						&& Z_TYPE(adaptor->plan()->source_literals[
							operation.op2.index]) == IS_LONG
						&& static_cast<zend_long>(constant_bits)
							== Z_LVAL(adaptor->plan()->source_literals[
								operation.op2.index]))) {
				continue;
			}
			const zend_mir_storage_id storage =
				adaptor->canonical_storage(operand);
			const zend_tpde_machine_value_kind kind =
				adaptor->machine_kind(operand);
			if (!zend_mir_id_is_valid(storage)
					|| (storage != operation.op1_storage_id
						&& storage != operation.op2_storage_id
						&& !(access == ElementAccess::Assign
							&& storage == operation.auxiliary_storage_id))
					|| (kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						? val_parts(operand).count() != 2
						: kind != ZEND_TPDE_MACHINE_VALUE_BOOL
							&& kind != ZEND_TPDE_MACHINE_VALUE_STRING_PTR
							&& kind != ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
							&& zend_tpde_machine_value_zval_type_info(kind)
								== IS_UNDEF)) {
				return 0;
			}
			for (const auto &[other, other_storage] : published) {
				if (other_storage == storage) {
					return 0;
				}
			}
			published.emplace_back(operand, storage);
		}
		const auto successors = guarded_successors_of(node);
		auto slot_kind = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA
				? operand.slot_kind : ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
		};
		const bool reads = access == ElementAccess::Read
			|| access == ElementAccess::Coalesce;
		const bool tests = access == ElementAccess::Isset
			|| access == ElementAccess::Empty;
		/* An assignment replaces an existing element in place: the write
		 * fetch's lookup, then the value. */
		const bool assigns = access == ElementAccess::Assign;
		const bool writes = access == ElementAccess::Write || assigns;
		const bool container_literal =
			operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
		const bool container_temporary =
			slot_kind(operation.op1) == ZEND_MIR_SOURCE_SLOT_TMP;
		/* The VAR of a write fetch holds a previous fetch's INDIRECT. */
		const bool container_var = writes
			&& slot_kind(operation.op1) == ZEND_MIR_SOURCE_SLOT_VAR;
		const bool key_literal =
			operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
		if ((!container_literal && !container_temporary && !container_var
					&& slot_kind(operation.op1) != ZEND_MIR_SOURCE_SLOT_CV)
				|| (writes && (container_literal || container_temporary
					|| node.has_result))
				|| (!key_literal
					&& slot_kind(operation.op2) != ZEND_MIR_SOURCE_SLOT_CV
					&& slot_kind(operation.op2) != ZEND_MIR_SOURCE_SLOT_TMP)
				|| (!assigns
					&& slot_kind(operation.result) != ZEND_MIR_SOURCE_SLOT_TMP
					&& slot_kind(operation.result)
						!= ZEND_MIR_SOURCE_SLOT_VAR)
				|| (assigns
					&& (operation.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| (operation.auxiliary.kind
								!= ZEND_MIR_SOURCE_OPERAND_LITERAL
							&& slot_kind(operation.auxiliary)
								!= ZEND_MIR_SOURCE_SLOT_CV
							&& slot_kind(operation.auxiliary)
								!= ZEND_MIR_SOURCE_SLOT_TMP)
						|| (operation.auxiliary.kind
								!= ZEND_MIR_SOURCE_OPERAND_LITERAL
							&& (!zend_mir_id_is_valid(
									operation.auxiliary_storage_id)
								|| operation.auxiliary_storage_id
									== operation.op1_storage_id))))
				|| (!container_literal
					&& !zend_mir_id_is_valid(operation.op1_storage_id))
				|| (!key_literal && !register_key
					&& !zend_mir_id_is_valid(operation.op2_storage_id))
				|| (!assigns
					&& (!zend_mir_id_is_valid(operation.result_storage_id)
						|| operation.result_storage_id
							== operation.op1_storage_id
						|| operation.result_storage_id
							== operation.op2_storage_id))
				|| (node.has_result && reads
					&& !((adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& val_parts(node.result).count() == 2)
						|| (adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_I64
							&& adaptor->exact_type(node.result)
								== ZEND_MIR_SCALAR_TYPE_I64
							&& val_parts(node.result).count() == 1)))
				|| (node.has_result && tests
					&& val_parts(node.result).count() != 1)
				|| (node.has_result && access == ElementAccess::Coalesce
					&& adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
			return 0;
		}
		auto frame_offset = [](zend_mir_storage_id storage) {
			return (uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		};
		const uint64_t container_offset = container_literal
			? uint64_t{operation.op1.index} * sizeof(zval)
			: frame_offset(operation.op1_storage_id);
		const uint64_t key_offset = key_literal
			? uint64_t{operation.op2.index} * sizeof(zval)
			: !zend_mir_id_is_valid(operation.op2_storage_id) ? 0
			: frame_offset(operation.op2_storage_id);
		const uint64_t result_offset = assigns
			? 0 : frame_offset(operation.result_storage_id);
		const bool value_literal = assigns
			&& operation.auxiliary.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
		/* An assignment's literal key is an integer or a string with its
		 * hash, which the compiler made non-numeric. */
		if (assigns && key_literal
				&& (adaptor->plan()->source_literals == nullptr
					|| operation.op2.index
						>= adaptor->plan()->source_literal_count
					|| (Z_TYPE(adaptor->plan()->source_literals[
							operation.op2.index]) != IS_LONG
						&& (Z_TYPE(adaptor->plan()->source_literals[
								operation.op2.index]) != IS_STRING
							|| ZSTR_H(Z_STR(adaptor->plan()->source_literals[
								operation.op2.index])) == 0)))) {
			return 0;
		}
		const uint64_t value_offset = !assigns ? 0
			: value_literal
				? uint64_t{operation.auxiliary.index} * sizeof(zval)
				: frame_offset(operation.auxiliary_storage_id);
		/* A CV container Zend's type inference proves to be an array,
		 * neither undefined nor a reference: the known_* lookups take its
		 * table without testing the zval. Some of them need one scratch
		 * register more than the general lookups. */
		const bool known_array = [&] {
			const zend_tpde_plan *plan = adaptor->plan();
			if (container_literal || container_temporary || container_var
					|| unlocked_gp_registers() < 10
					|| plan->source_opcodes == nullptr
					|| operation.source_position_id
						>= plan->source_opcode_count) {
				return false;
			}
			const zend_tpde_source_opcode &source =
				plan->source_opcodes[operation.source_position_id];
			return source.op1_type == IS_CV
				&& source.op1_var == container_offset
				&& (source.op1_may_be & (MAY_BE_ANY | MAY_BE_UNDEF
						| MAY_BE_REF | MAY_BE_INDIRECT)) == MAY_BE_ARRAY;
		}();
		/* The lookup snippets use up to seven scratch registers; with the
		 * literal register held across them, one stays spare. */
		if (container_offset > INT32_MAX - sizeof(zval)
				|| key_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)
				|| value_offset > INT32_MAX - sizeof(zval)
				|| unlocked_gp_registers() < 9) {
			return 0;
		}
		for (const auto &[operand, storage] : published) {
			if (!materialize_cold_operand(operand, storage)) {
				return -1;
			}
		}

		auto slow = text_writer.label_create();
		auto absent = text_writer.label_create();
		auto answered = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg decision{this};
		ScratchReg answer{this};
		/* A literal key the compiler can read is passed as what it is: an
		 * integer index, or a string with its hash. A runtime key Zend's
		 * type inference proves may be an integer and never a string is
		 * passed as an index read from its slot; unless it is proven an
		 * integer (neither undefined nor a reference), any other type
		 * takes the helper. */
		enum class LiteralKey { None, Index, String, Register };
		LiteralKey literal_key = LiteralKey::None;
		uint64_t literal_key_value = 0;
		bool register_key_checked = false;
		{
			const zend_tpde_plan *plan = adaptor->plan();
			if (register_key) {
				literal_key = LiteralKey::Register;
			} else if (!key_literal && unlocked_gp_registers() >= 10
					&& plan->source_opcodes != nullptr
					&& operation.source_position_id
						< plan->source_opcode_count) {
				const zend_tpde_source_opcode &source =
					plan->source_opcodes[operation.source_position_id];
				const uint32_t may_be = source.op2_may_be
					& (MAY_BE_ANY | MAY_BE_UNDEF | MAY_BE_REF
						| MAY_BE_INDIRECT);
				if (source.op2_type != IS_CONST
						&& source.op2_var == key_offset
						&& source.op2_may_be != UINT32_MAX
						&& (may_be & MAY_BE_LONG) != 0
						&& (may_be & MAY_BE_STRING) == 0) {
					literal_key = LiteralKey::Register;
					register_key_checked = may_be != MAY_BE_LONG;
				}
			}
			if (key_literal && plan->source_literals != nullptr
					&& operation.op2.index < plan->source_literal_count) {
				const zval *literal =
					&plan->source_literals[operation.op2.index];
				if (Z_TYPE_P(literal) == IS_LONG) {
					literal_key = LiteralKey::Index;
					literal_key_value =
						static_cast<uint64_t>(Z_LVAL_P(literal));
				} else if (Z_TYPE_P(literal) == IS_STRING
						&& ZSTR_H(Z_STR_P(literal)) != 0) {
					literal_key = LiteralKey::String;
					literal_key_value = ZSTR_H(Z_STR_P(literal));
				}
			}
		}
		ScratchReg literals{this};
		AsmReg literals_reg = frame_reg;
		if (container_literal || value_literal
				|| (key_literal && literal_key != LiteralKey::Index)) {
			literals_reg = literals.alloc_gp();
			ASM(MOV64rm, literals_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, literals_reg,
				FE_MEM(literals_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_op_array, literals))));
		}
		auto address = [](AsmReg base, uint64_t offset) {
			return GenericValuePart{GenericValuePart::Expr{
				base, static_cast<int64_t>(offset)}};
		};
		/* The operation consumes a temporary key: one that needs a release
		 * (a counted string) takes the helper. */
		if (slot_kind(operation.op2) == ZEND_MIR_SOURCE_SLOT_TMP
				&& !register_key) {
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(key_offset
					+ offsetof(zval, u1.type_info))),
				Z_TYPE_FLAGS_MASK);
			generate_raw_jump(Jump::jne, slow);
		}
		/* An assignment may insert into the array before it stores the
		 * value, so the value is checked first: defined and not a
		 * reference. An array value is copied after the insertion as
		 * ZEND_ASSIGN_DIM copies it: only the container's own zval holds
		 * the unshared array, which is another storage (see above) or the
		 * target of a VAR's INDIRECT, where the VM stores it the same. */
		if (assigns && !value_literal) {
			ScratchReg value_type{this};
			const AsmReg type_reg = value_type.alloc_gp();
			ASM(MOVZXr32m8, type_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(value_offset
					+ offsetof(zval, u1.v.type))));
			ASM(CMP32ri, type_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
			ASM(CMP32ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
		}
		const AsmReg container_base =
			container_literal ? literals_reg : frame_reg;
		ValuePart element{tpde::x64::PlatformConfig::GP_BANK, 8};
		auto container_address = address(container_base, container_offset);
		auto key_address = key_literal
			? address(literals_reg, key_offset)
			: address(frame_reg, key_offset);
		/*
		 * A runtime key: a string looks up inline, any other key (an
		 * integer, a reference) out of line, both into one register. Only
		 * with a register to spare, so that neither lookup evicts.
		 */
		const bool split_string_key = !key_literal
			&& unlocked_gp_registers() >= 10;
		auto string_key_split = [&](auto &&string_lookup,
				auto &&general_lookup, ValuePart &out) -> bool {
			ScratchReg common{this};
			const AsmReg common_reg = common.alloc_gp();
			auto other = text_writer.label_create();
			auto joined = text_writer.label_create();
			ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(key_offset
					+ offsetof(zval, u1.v.type))), IS_STRING);
			generate_raw_jump(Jump::jne, other);
			{
				ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
				if (!string_lookup(value)) {
					return false;
				}
				mov(common_reg, value.cur_reg_or_load(this), 8);
				value.reset(this);
			}
			const bool cold_other = !text_writer.in_cold_area();
			if (cold_other) {
				cold_begin();
			} else {
				generate_raw_jump(Jump::jmp, joined);
			}
			label_place(other);
			{
				ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
				if (!general_lookup(value)) {
					return false;
				}
				mov(common_reg, value.cur_reg_or_load(this), 8);
				value.reset(this);
			}
			if (cold_other) {
				generate_raw_jump(Jump::jmp, joined);
				cold_end();
			}
			label_place(joined);
			out.set_value(this, std::move(common));
			return true;
		};
		/* isset() of an array element decides in one snippet: set, not
		 * set, or the helper's. */
		/* An isset whose element a read reuses finds it in the general
		 * form, which also leaves its address; a read that reuses one skips
		 * its own lookup. */
		const bool reuse_store = access == ElementAccess::Isset
			&& reuse_read != IRInstRef{UINT32_MAX} && !container_temporary
			&& !register_key && literal_key != LiteralKey::Register;
		const bool reuse_load = (access == ElementAccess::Read
				|| (access == ElementAccess::Write && !container_var))
			&& lookup_reuse_armed(instruction) && !register_key
			&& literal_key != LiteralKey::Register
			&& unlocked_gp_registers() >= 10;
		if (access == ElementAccess::Isset && !container_temporary
				&& !reuse_store) {
			ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
			bool tested;
			if (literal_key == LiteralKey::Index) {
				tested = (known_array
				? EncodeBase::encode_zend_native_known_isset_idx(
					std::move(container_address),
					GenericValuePart{ValuePartRef{this, literal_key_value, 8,
						tpde::x64::PlatformConfig::GP_BANK}}, value)
				: EncodeBase::encode_zend_native_array_isset_idx(
					std::move(container_address),
					GenericValuePart{ValuePartRef{this, literal_key_value, 8,
						tpde::x64::PlatformConfig::GP_BANK}}, value));
			} else if (literal_key == LiteralKey::String) {
				ScratchReg name{this};
				ASM(MOV64rm, name.alloc_gp(),
					FE_MEM(literals_reg, 0, FE_NOREG,
						static_cast<int32_t>(key_offset)));
				tested = (known_array
				? EncodeBase::encode_zend_native_known_isset_str(
					std::move(container_address),
					GenericValuePart{std::move(name)},
					GenericValuePart{ValuePartRef{this, literal_key_value, 8,
						tpde::x64::PlatformConfig::GP_BANK}}, value)
				: EncodeBase::encode_zend_native_array_isset_str(
					std::move(container_address),
					GenericValuePart{std::move(name)},
					GenericValuePart{ValuePartRef{this, literal_key_value, 8,
						tpde::x64::PlatformConfig::GP_BANK}}, value));
			} else if (key_literal) {
				tested = (known_array
				? EncodeBase::encode_zend_native_known_isset_literal(
					std::move(container_address), std::move(key_address),
					value)
				: EncodeBase::encode_zend_native_array_isset_literal(
					std::move(container_address), std::move(key_address),
					value));
			} else if (split_string_key) {
				tested = string_key_split(
					[&](ValuePart &out) {
						return (known_array
				? EncodeBase::encode_zend_native_known_isset_string_key(
							address(container_base, container_offset),
							address(frame_reg, key_offset), out)
				: EncodeBase::encode_zend_native_array_isset_string_key(
							address(container_base, container_offset),
							address(frame_reg, key_offset), out));
					},
					[&](ValuePart &out) {
						return (known_array
				? EncodeBase::encode_zend_native_known_isset_key(
							address(container_base, container_offset),
							address(frame_reg, key_offset), out)
				: EncodeBase::encode_zend_native_array_isset_key(
							address(container_base, container_offset),
							address(frame_reg, key_offset), out));
					}, value);
			} else {
				tested = (known_array
				? EncodeBase::encode_zend_native_known_isset_key(
					std::move(container_address), std::move(key_address),
					value)
				: EncodeBase::encode_zend_native_array_isset_key(
					std::move(container_address), std::move(key_address),
					value));
			}
			if (!tested) {
				return -1;
			}
			auto decision_reg = decision.alloc_gp();
			auto answer_reg = answer.alloc_gp();
			mov(answer_reg, value.cur_reg_or_load(this), 8);
			value.reset(this);
			literals.reset();
			ASM(CMP64ri, answer_reg, ZEND_NATIVE_ISSET_UNKNOWN);
			generate_raw_jump(Jump::je, slow);
			if (node.has_result) {
				auto [result_ref, result] = result_ref_single(node.result);
				mov(result.alloc_reg(), answer_reg, 8);
				result.set_modified();
			} else {
				/* IS_FALSE + answer is IS_FALSE or IS_TRUE. */
				ASM(ADD32ri, answer_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(result_offset
							+ offsetof(zval, u1.type_info))),
					answer_reg);
			}
			answer.reset();
			finish_guarded(slow, done, std::move(decision), frame_scratch,
				successors);
			return 1;
		}
		/*
		 * An assignment under a string key, or a key of unknown type, looks
		 * its element up, or inserts it, in one call of
		 * zend_native_array_assign_lookup(): a lookup inline would repeat
		 * zend_hash_lookup() at every site for little gain. NULL (UNKNOWN)
		 * takes the helper.
		 */
		auto reuse_cached = text_writer.label_create();
		auto reuse_joined = text_writer.label_create();
		if (reuse_store) {
			ASM(MOV64mi, FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()), 0);
		} else if (reuse_load) {
			/* A write fetch changes the found element in place only in an
			 * array without another owner (zend_native_probe_array_w()); the
			 * isset found it, so the CV holds an array, maybe through a
			 * reference. */
			auto shared = text_writer.label_create();
			if (writes) {
				ScratchReg table{this};
				const AsmReg table_reg = table.alloc_gp();
				auto direct = text_writer.label_create();
				ASM(MOV64rm, table_reg, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(container_offset)));
				ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(container_offset
						+ offsetof(zval, u1.v.type))), IS_REFERENCE);
				generate_raw_jump(Jump::jne, direct);
				ASM(MOV64rm, table_reg, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_reference, val))));
				label_place(direct);
				ASM(CMP32mi, FE_MEM(table_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_refcounted_h,
						refcount))), 1);
				generate_raw_jump(Jump::jne, shared);
			}
			ASM(CMP64mi, FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()), 0);
			generate_raw_jump(Jump::jne, reuse_cached);
			label_place(shared);
		}
		/* A keyed assignment looks up or inserts its key in C. */
		const bool lookup_call = assigns
			&& literal_key != LiteralKey::Index
			&& literal_key != LiteralKey::Register;
		bool found;
		if (lookup_call) {
			if (container_var) {
				ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(container_offset
						+ offsetof(zval, u1.v.type))), IS_INDIRECT);
				generate_raw_jump(Jump::jne, slow);
			}
			ScratchReg looked{this};
			const AsmReg looked_reg = looked.alloc_gp();
			const uint64_t live_registers = static_cast<uint64_t>(
				register_file.used) & ~(uint64_t{1} << looked_reg.id());
			emit_preserving_call(live_registers,
				ZEND_NATIVE_HELPER_ARRAY_ASSIGN_LOOKUP, [&] {
					/* The key's address first, through a caller-saved
					 * register neither base uses, as the frame or the
					 * literals may sit in either argument register. */
					const AsmReg key_base =
						key_literal ? literals_reg : frame_reg;
					FeRegGP key_temp = FE_AX;
					for (const auto &[id, reg] : {std::pair{AsmReg::AX, FE_AX},
							std::pair{AsmReg::CX, FE_CX},
							std::pair{AsmReg::DX, FE_DX}}) {
						if (frame_reg.id() != id && key_base.id() != id) {
							key_temp = reg;
							break;
						}
					}
					ASM(LEA64rm, key_temp, FE_MEM(key_base, 0, FE_NOREG,
						static_cast<int32_t>(key_offset)));
					/* The container's zval: the CV, or the target of the
					 * VAR's INDIRECT. */
					if (container_var) {
						ASM(MOV64rm, FE_DI, FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(container_offset)));
					} else {
						ASM(LEA64rm, FE_DI, FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(container_offset)));
					}
					ASM(MOV64rr, FE_SI, key_temp);
				}, [&] {
					ASM(MOV64rr, looked_reg, FE_AX);
				});
			element.set_value(this, std::move(looked));
			found = true;
		} else if (literal_key != LiteralKey::None) {
			ScratchReg key_index{this};
			if (register_key_checked) {
				ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(key_offset
						+ offsetof(zval, u1.v.type))), IS_LONG);
				generate_raw_jump(Jump::jne, slow);
			}
			if (register_key) {
				auto [key_ref, key_part] = val_ref_single(node.operands[0]);
				ASM(MOV64rr, key_index.alloc_gp(), key_part.load_to_reg());
			} else if (literal_key == LiteralKey::Register) {
				ASM(MOV64rm, key_index.alloc_gp(), FE_MEM(frame_reg, 0,
					FE_NOREG, static_cast<int32_t>(key_offset)));
			}
			auto constant = [&]() {
				return literal_key == LiteralKey::Register
					? GenericValuePart{std::move(key_index)}
					: GenericValuePart{ValuePartRef{this, literal_key_value,
						8, tpde::x64::PlatformConfig::GP_BANK}};
			};
			const bool index = literal_key == LiteralKey::Index
				|| literal_key == LiteralKey::Register;
			ScratchReg name{this};
			if (!index) {
				ASM(MOV64rm, name.alloc_gp(),
					FE_MEM(literals_reg, 0, FE_NOREG,
						static_cast<int32_t>(key_offset)));
			}
			auto name_part = [&]() {
				return GenericValuePart{std::move(name)};
			};
			if (assigns && !index) {
				return -1;
			} else if (assigns) {
				found = container_var
					? EncodeBase::encode_zend_native_indirect_assign_idx(
						std::move(container_address), constant(), element)
					: EncodeBase::encode_zend_native_array_assign_idx(
						std::move(container_address), constant(), element);
			} else if (container_var) {
				found = index
					? EncodeBase::encode_zend_native_indirect_find_idx_w(
						std::move(container_address), constant(), element)
					: EncodeBase::encode_zend_native_indirect_find_str_w(
						std::move(container_address), name_part(),
						constant(), element);
			} else if (writes) {
				found = index
					? (known_array
				? EncodeBase::encode_zend_native_known_find_idx_w(
						std::move(container_address), constant(), element)
				: EncodeBase::encode_zend_native_array_find_idx_w(
						std::move(container_address), constant(), element))
					: (known_array
				? EncodeBase::encode_zend_native_known_find_str_w(
						std::move(container_address), name_part(),
						constant(), element)
				: EncodeBase::encode_zend_native_array_find_str_w(
						std::move(container_address), name_part(),
						constant(), element));
			} else if (tests || access == ElementAccess::Coalesce) {
				found = index
					? (known_array
				? EncodeBase::encode_zend_native_known_find_idx(
						std::move(container_address), constant(), element)
				: EncodeBase::encode_zend_native_array_test_idx(
						std::move(container_address), constant(), element))
					: (known_array
				? EncodeBase::encode_zend_native_known_find_str(
						std::move(container_address), name_part(),
						constant(), element)
				: EncodeBase::encode_zend_native_array_test_str(
						std::move(container_address), name_part(),
						constant(), element));
			} else {
				found = index
					? (known_array
				? EncodeBase::encode_zend_native_known_find_idx(
						std::move(container_address), constant(), element)
				: EncodeBase::encode_zend_native_array_find_idx(
						std::move(container_address), constant(), element))
					: (known_array
				? EncodeBase::encode_zend_native_known_find_str(
						std::move(container_address), name_part(),
						constant(), element)
				: EncodeBase::encode_zend_native_array_find_str(
						std::move(container_address), name_part(),
						constant(), element));
			}
		} else
		{
			/* Keyed assignments take lookup_call. */
			ZEND_ASSERT(!assigns);
			if (container_var) {
				found = key_literal
					? EncodeBase::encode_zend_native_indirect_find_literal_w(
						std::move(container_address), std::move(key_address),
						element)
					: EncodeBase::encode_zend_native_indirect_find_key_w(
						std::move(container_address), std::move(key_address),
						element);
			} else if (writes) {
				found = key_literal
					? (known_array
				? EncodeBase::encode_zend_native_known_find_literal_w(
						std::move(container_address), std::move(key_address),
						element)
				: EncodeBase::encode_zend_native_array_find_literal_w(
						std::move(container_address), std::move(key_address),
						element))
					: (known_array
				? EncodeBase::encode_zend_native_known_find_key_w(
						std::move(container_address), std::move(key_address),
						element)
				: EncodeBase::encode_zend_native_array_find_key_w(
						std::move(container_address), std::move(key_address),
						element));
			} else if (split_string_key) {
				const bool testing = tests || access == ElementAccess::Coalesce;
				found = string_key_split(
					[&](ValuePart &out) {
						return testing
							? (known_array
				? EncodeBase::encode_zend_native_known_find_string_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out)
				: EncodeBase::encode_zend_native_array_test_string_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out))
							: (known_array
				? EncodeBase::encode_zend_native_known_find_string_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out)
				: EncodeBase::encode_zend_native_array_find_string_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out));
					},
					[&](ValuePart &out) {
						return testing
							? (known_array
				? EncodeBase::encode_zend_native_known_find_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out)
				: EncodeBase::encode_zend_native_array_test_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out))
							: (known_array
				? EncodeBase::encode_zend_native_known_find_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out)
				: EncodeBase::encode_zend_native_array_find_key(
								address(container_base, container_offset),
								address(frame_reg, key_offset), out));
					}, element);
			} else if (tests || access == ElementAccess::Coalesce) {
				/* An undefined or null container has no element to test. */
				found = key_literal
					? (known_array
				? EncodeBase::encode_zend_native_known_find_literal(
						std::move(container_address), std::move(key_address),
						element)
				: EncodeBase::encode_zend_native_array_test_literal(
						std::move(container_address), std::move(key_address),
						element))
					: (known_array
				? EncodeBase::encode_zend_native_known_find_key(
						std::move(container_address), std::move(key_address),
						element)
				: EncodeBase::encode_zend_native_array_test_key(
						std::move(container_address), std::move(key_address),
						element));
			} else {
				found = key_literal
					? (known_array
				? EncodeBase::encode_zend_native_known_find_literal(
						std::move(container_address), std::move(key_address),
						element)
				: EncodeBase::encode_zend_native_array_find_literal(
						std::move(container_address), std::move(key_address),
						element))
					: (known_array
				? EncodeBase::encode_zend_native_known_find_key(
						std::move(container_address), std::move(key_address),
						element)
				: EncodeBase::encode_zend_native_array_find_key(
						std::move(container_address), std::move(key_address),
						element));
			}
		}
		if (!found) {
			return -1;
		}
		const AsmReg element_reg = element.cur_reg_or_load(this);
		if (reuse_load) {
			generate_raw_jump(Jump::jmp, reuse_joined);
			label_place(reuse_cached);
			ASM(MOV64rm, element_reg,
				FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()));
			label_place(reuse_joined);
		}
		/* Held from here on; the lookup needed the registers before. */
		auto decision_reg = decision.alloc_gp();
		auto answer_reg = answer.alloc_gp();
		auto unknown = text_writer.label_create();
		/* A read of a CV that is not an array may be a string offset: the
		 * one-character string, looked up out of the hot code. */
		const bool string_reads = access == ElementAccess::Read
			&& !known_array && !container_literal && !container_temporary
			&& !register_key
			&& (literal_key == LiteralKey::Index || !key_literal);
		auto string_offset = text_writer.label_create();
		auto element_found = text_writer.label_create();
		ASM(CMP64ri, element_reg,
			static_cast<int32_t>(ZEND_NATIVE_ELEMENT_ABSENT));
		generate_raw_jump(Jump::jb,
			access == ElementAccess::Coalesce ? unknown
				: string_reads ? string_offset : slow);
		/* A read of a missing key warns and a write fetch inserts it; the
		 * helper does that. An assignment under an integer key into a CV's
		 * unshared array inserts the null element out of line instead and
		 * stores into it as into an existing one. */
		const bool insert_index = assigns && !container_var
			&& !container_temporary && !container_literal
			&& (literal_key == LiteralKey::Index
				|| literal_key == LiteralKey::Register)
			/* A register key of a string-key assignment has no helper. */
			&& runtime_symbol(ZEND_NATIVE_HELPER_ARRAY_INSERT_INDEX).valid();
		auto insert = text_writer.label_create();
		auto inserted = text_writer.label_create();
		generate_raw_jump(Jump::je, insert_index ? insert
			: access == ElementAccess::Read || writes ? slow : absent);
		if (reuse_store) {
			ASM(MOV64mr, FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()),
				element_reg);
			lookup_reuse_reads_.push_back(static_cast<uint32_t>(reuse_read));
		}
		if (insert_index) {
			const bool cold_insert = !text_writer.in_cold_area();
			if (cold_insert) {
				cold_begin();
			} else {
				generate_raw_jump(Jump::jmp, inserted);
			}
			label_place(insert);
			const uint64_t live_registers = static_cast<uint64_t>(
				register_file.used) & ~(uint64_t{1} << element_reg.id());
			emit_preserving_call(live_registers,
				ZEND_NATIVE_HELPER_ARRAY_INSERT_INDEX, [&] {
					const auto key_to_si = [&] {
						if (literal_key == LiteralKey::Index) {
							ASM(MOV64ri, FE_SI,
								static_cast<int64_t>(literal_key_value));
						} else {
							ASM(MOV64rm, FE_SI, FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(key_offset)));
						}
					};
					/* The frame may sit in either argument register. */
					if (frame_reg.id() == AsmReg{AsmReg::DI}.id()) {
						key_to_si();
						ASM(LEA64rm, FE_DI, FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(container_offset)));
					} else {
						ASM(LEA64rm, FE_DI, FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(container_offset)));
						key_to_si();
					}
				}, [&] {
					ASM(MOV64rr, element_reg, FE_AX);
				});
			generate_raw_jump(Jump::jmp, inserted);
			if (cold_insert) {
				cold_end();
			}
			label_place(inserted);
		}
		if (string_reads) {
			const bool cold_string = !text_writer.in_cold_area();
			if (cold_string) {
				cold_begin();
			} else {
				generate_raw_jump(Jump::jmp, element_found);
			}
			label_place(string_offset);
			ValuePart chars = image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
				ZEND_NATIVE_HELPER_CHAR_STRINGS);
			if (!chars.has_reg()) {
				generate_raw_jump(Jump::jmp, slow);
			} else {
				auto chars_scratch = std::move(chars).into_scratch(this);
				ValuePart offset_element{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				const bool encoded = literal_key == LiteralKey::Index
					? EncodeBase::encode_zend_native_string_offset_idx(
						address(container_base, container_offset),
						GenericValuePart{ValuePartRef{this, literal_key_value,
							8, tpde::x64::PlatformConfig::GP_BANK}},
						GenericValuePart{std::move(chars_scratch)}, offset_element)
					: EncodeBase::encode_zend_native_string_offset_key(
						address(container_base, container_offset),
						address(frame_reg, key_offset),
						GenericValuePart{std::move(chars_scratch)}, offset_element);
				if (!encoded) {
					return -1;
				}
				mov(element_reg, offset_element.cur_reg_or_load(this), 8);
				offset_element.reset(this);
				ASM(TEST64rr, element_reg, element_reg);
				generate_raw_jump(Jump::je, slow);
				generate_raw_jump(Jump::jmp, element_found);
			}
			if (cold_string) {
				cold_end();
			}
			label_place(element_found);
		}
		if (container_temporary) {
			ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_container_shared(
					address(frame_reg, container_offset), shared)) {
				return -1;
			}
			const AsmReg shared_reg = shared.cur_reg_or_load(this);
			ASM(TEST64rr, shared_reg, shared_reg);
			shared.reset(this);
			generate_raw_jump(Jump::je, slow);
		}
		if (access == ElementAccess::Read && node.has_result
				&& adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64) {
			/* An integer result needs an integer element. */
			ValuePart is_long{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_zval_is_long(
					address(element_reg, 0), is_long)) {
				return -1;
			}
			const AsmReg is_long_reg = is_long.cur_reg_or_load(this);
			ASM(TEST64rr, is_long_reg, is_long_reg);
			is_long.reset(this);
			generate_raw_jump(Jump::je, slow);
			auto [result_ref, result] = result_ref_single(node.result);
			if (!EncodeBase::encode_zend_native_load_u64(
					address(element_reg, 0), result)) {
				return -1;
			}
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset)),
				result.load_to_reg());
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))),
				IS_LONG);
		} else if (reads) {
			ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
			ValuePart type_info{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_zval_copy_deref(
					address(element_reg, 0), payload, type_info)) {
				return -1;
			}
			const AsmReg payload_reg = payload.cur_reg_or_load(this);
			const AsmReg type_info_reg = type_info.cur_reg_or_load(this);
			/* The owned copy lives in the result temporary; a boxed register
			 * result mirrors it, as the cold path reloads it. */
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset)),
				payload_reg);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))),
				type_info_reg);
			if (node.has_result && access == ElementAccess::Read) {
				auto result = result_ref(node.result);
				auto result_payload = result.part(0);
				auto result_type = result.part(1);
				mov(result_payload.alloc_reg(), payload_reg, 8);
				mov(result_type.alloc_reg(), type_info_reg, 4);
				result_payload.set_modified();
				result_type.set_modified();
			}
			payload.reset(this);
			type_info.reset(this);
			if (access == ElementAccess::Coalesce) {
				generate_raw_jump(Jump::jmp, answered);
				/* Not an array: a scalar container reads as null. */
				label_place(unknown);
				ValuePart scalar{tpde::x64::PlatformConfig::GP_BANK, 8};
				if (!EncodeBase::encode_zend_native_zval_is_scalar(
						address(container_base, container_offset), scalar)) {
					return -1;
				}
				const AsmReg scalar_reg = scalar.cur_reg_or_load(this);
				ASM(TEST64rr, scalar_reg, scalar_reg);
				scalar.reset(this);
				generate_raw_jump(Jump::je, slow);
				label_place(absent);
				if (container_temporary) {
					ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
					if (!EncodeBase::encode_zend_native_container_shared(
							address(frame_reg, container_offset), shared)) {
						return -1;
					}
					const AsmReg shared_reg = shared.cur_reg_or_load(this);
					ASM(TEST64rr, shared_reg, shared_reg);
					shared.reset(this);
					generate_raw_jump(Jump::je, slow);
				}
				ASM(MOV32mi,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(result_offset
							+ offsetof(zval, u1.type_info))),
					IS_NULL);
				label_place(answered);
			}
		} else if (assigns) {
			/* The element's old value needs no release (an appended
			 * element reads as null), or is a string, whose release runs
			 * no code: dropped before the store, freed out of line by its
			 * last owner. Anything else takes the helper. The value,
			 * checked above, is copied, a temporary moved. A literal is
			 * counted without opcache. */
			const int32_t type_offset =
				static_cast<int32_t>(offsetof(zval, u1.type_info));
			const AsmReg value_base = value_literal ? literals_reg : frame_reg;
			ScratchReg value_type{this};
			ScratchReg value_payload{this};
			const AsmReg type_reg = value_type.alloc_gp();
			const AsmReg payload_reg = value_payload.alloc_gp();
			{
				auto released = text_writer.label_create();
				auto last_owner = text_writer.label_create();
				ASM(MOV32rm, type_reg,
					FE_MEM(element_reg, 0, FE_NOREG, type_offset));
				ASM(TEST32ri, type_reg, Z_TYPE_FLAGS_MASK);
				generate_raw_jump(Jump::je, released);
				ASM(CMP8ri, type_reg, IS_STRING);
				generate_raw_jump(Jump::jne, slow);
				ASM(MOV64rm, payload_reg, FE_MEM(element_reg, 0, FE_NOREG, 0));
				ASM(CMP32mi, FE_MEM(payload_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_refcounted_h, refcount))),
					1);
				generate_raw_jump(Jump::jle, last_owner);
				ASM(SUB32mi, FE_MEM(payload_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_refcounted_h, refcount))),
					1);
				const bool cold_free = !text_writer.in_cold_area();
				if (cold_free) {
					cold_begin();
				} else {
					generate_raw_jump(Jump::jmp, released);
				}
				label_place(last_owner);
				emit_preserving_call(static_cast<uint64_t>(register_file.used),
					ZEND_NATIVE_HELPER_ZVAL_RELEASE_SLOW, [&] {
						ASM(MOV64rr, FE_DI, element_reg);
					}, [] {});
				generate_raw_jump(Jump::jmp, released);
				if (cold_free) {
					cold_end();
				}
				label_place(released);
			}
			ASM(MOV32rm, type_reg, FE_MEM(value_base, 0, FE_NOREG,
				static_cast<int32_t>(value_offset) + type_offset));
			ASM(MOV64rm, payload_reg, FE_MEM(value_base, 0, FE_NOREG,
				static_cast<int32_t>(value_offset)));
			const zend_tpde_plan *value_plan = adaptor->plan();
			const bool counted_literal = value_literal
				&& (value_plan->source_literals == nullptr
					|| operation.auxiliary.index
						>= value_plan->source_literal_count
					|| Z_REFCOUNTED(value_plan->source_literals[
						operation.auxiliary.index]));
			if (counted_literal || (!value_literal
					&& slot_kind(operation.auxiliary)
						== ZEND_MIR_SOURCE_SLOT_CV)) {
				auto uncounted = text_writer.label_create();
				ASM(TEST32ri, type_reg, Z_TYPE_FLAGS_MASK);
				generate_raw_jump(Jump::je, uncounted);
				ASM(ADD32mi, FE_MEM(payload_reg, 0, FE_NOREG, 0), 1);
				label_place(uncounted);
			}
			ASM(MOV64mr, FE_MEM(element_reg, 0, FE_NOREG, 0), payload_reg);
			ASM(MOV32mr, FE_MEM(element_reg, 0, FE_NOREG, type_offset),
				type_reg);
		} else if (writes) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset)),
				element_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))),
				IS_INDIRECT);
		} else {
			ValuePart value{tpde::x64::PlatformConfig::GP_BANK, 8};
			const bool tested = access == ElementAccess::Isset
				? EncodeBase::encode_zend_native_zval_isset(
					address(element_reg, 0), value)
				: EncodeBase::encode_zend_native_zval_empty(
					address(element_reg, 0), value);
			if (!tested) {
				return -1;
			}
			mov(answer_reg, value.cur_reg_or_load(this), 8);
			value.reset(this);
			if (access == ElementAccess::Empty) {
				ASM(CMP64ri, answer_reg, ZEND_NATIVE_EMPTY_UNKNOWN);
				generate_raw_jump(Jump::je, slow);
			}
			generate_raw_jump(Jump::jmp, answered);
			label_place(absent);
			ASM(MOV64ri, answer_reg,
				access == ElementAccess::Empty ? 1 : 0);
			if (container_temporary) {
				ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
				if (!EncodeBase::encode_zend_native_container_shared(
						address(frame_reg, container_offset), shared)) {
					return -1;
				}
				const AsmReg shared_reg = shared.cur_reg_or_load(this);
				ASM(TEST64rr, shared_reg, shared_reg);
				shared.reset(this);
				generate_raw_jump(Jump::je, slow);
			}
			label_place(answered);
		}
		element.reset(this);
		literals.reset();
		if (container_temporary) {
			if (!EncodeBase::encode_zend_native_release_shared(
					address(frame_reg, container_offset))) {
				return -1;
			}
		}
		if (access == ElementAccess::Coalesce && node.has_result) {
			/* Both answers are in the result temporary. */
			auto result = result_ref(node.result);
			auto result_payload = result.part(0);
			auto result_type = result.part(1);
			ASM(MOV64rm, result_payload.alloc_reg(),
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset)));
			ASM(MOV32rm, result_type.alloc_reg(),
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))));
			result_payload.set_modified();
			result_type.set_modified();
		} else if (tests && node.has_result) {
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			mov(result_reg, answer_reg, 8);
			result.set_modified();
		} else if (tests) {
			/* IS_FALSE + answer is IS_FALSE or IS_TRUE. */
			ASM(ADD32ri, answer_reg, IS_FALSE);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))),
				answer_reg);
		}
		answer.reset();
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	/*
	 * FETCH_DIM_R whose key, and possibly container, are SSA values and whose
	 * result is an integer or a boxed value, composed of EncodeGen snippets:
	 * the container's array (zend_native_zval_table, _boxed_table or an
	 * array pointer), the element under an integer, string or boxed key
	 * (zend_native_table_find_*), and the result. Every undecided probe
	 * takes the guarded cold block. Returns 1 when emitted, 0 when the form
	 * does not apply and -1 on an encoding failure.
	 */
	auto array_element_register = [&]() -> int {
		zend_tpde_array_read layout;
		const zend_tpde_machine_reference *element_reference =
			operation_machine_reference(
				ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT);
		if (node.kind != Adaptor::InstKind::GuardedFast || !node.has_result
				|| node.operands.empty()
				|| !zend_tpde_array_read_at(mir, &layout, true)
				|| element_reference == nullptr
				|| (!layout.container_literal
					&& !zend_mir_id_is_valid(
						element_reference->base_value_id))
				|| !zend_mir_id_is_valid(element_reference->index_value_id)
				|| element_reference->scale != sizeof(zval)
				|| element_reference->access_width != sizeof(zval)
				|| layout.container_offset > INT32_MAX - 8
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return 0;
		}
		const auto successors = guarded_successors_of(node);
		const IRValueRef key_value = node.operands[0];
		const zend_tpde_machine_value_kind key_kind =
			adaptor->machine_kind(key_value);
		const bool key_long = key_kind == ZEND_TPDE_MACHINE_VALUE_I64
			&& adaptor->exact_type(key_value) == ZEND_MIR_SCALAR_TYPE_I64
			&& adaptor->representation(key_value)
				== ZEND_MIR_REPRESENTATION_I64;
		const bool key_string =
			key_kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR;
		const bool key_boxed = key_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& adaptor->representation(key_value)
				== ZEND_MIR_REPRESENTATION_ZVAL
			&& val_parts(key_value).count() == 2;
		const bool result_long =
			adaptor->machine_kind(node.result) == ZEND_TPDE_MACHINE_VALUE_I64
			&& adaptor->exact_type(node.result) == ZEND_MIR_SCALAR_TYPE_I64
			&& adaptor->representation(node.result)
				== ZEND_MIR_REPRESENTATION_I64;
		const bool result_boxed = adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& adaptor->representation(node.result)
				== ZEND_MIR_REPRESENTATION_ZVAL
			&& val_parts(node.result).count() == 2;
		/* A temporary key is consumed by the read: only an integer, which
		 * needs no release, stays here. */
		const bool temporary_key =
			mir.value_operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
			|| mir.value_operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR;
		if ((!key_long && !key_string && !key_boxed)
				|| (!result_long && !result_boxed)
				|| (temporary_key && !key_long)) {
			return 0;
		}
		const bool register_receiver = node.operands.size() > 1
			&& node.machine_reference_operand_index != 1
			&& (adaptor->machine_kind(node.operands[1])
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
				|| (adaptor->machine_kind(node.operands[1])
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& adaptor->representation(node.operands[1])
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& val_parts(node.operands[1]).count() == 2));
		if (!register_receiver && layout.container_literal
				&& node.machine_reference_operand_index
					>= node.operands.size()) {
			return 0;
		}
		if (!register_receiver && !layout.container_literal
				&& mir.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
			return 0;
		}
		/* The lookup snippets use up to six scratch registers, with the
		 * array and the key parts held across them. */
		if (unlocked_gp_registers() < (key_boxed ? 10u : 9u)) {
			return 0;
		}
		auto part_index = [&](IRValueRef value,
				zend_tpde_machine_part_role role) -> uint32_t {
			const ValueParts parts = val_parts(value);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				if (parts.representation.parts[part].semantic_role == role) {
					return part;
				}
			}
			return UINT32_MAX;
		};

		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg decision{this};
		/* The container's array, or null. */
		ValuePart table{tpde::x64::PlatformConfig::GP_BANK, 8};
		if (register_receiver) {
			auto receiver = val_ref(node.operands[1]);
			if (adaptor->machine_kind(node.operands[1])
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
				mov(table.alloc_reg(this), receiver.part(0).load_to_reg(), 8);
			} else {
				const uint32_t payload = part_index(node.operands[1],
					ZEND_TPDE_MACHINE_PART_PAYLOAD);
				const uint32_t type_info = part_index(node.operands[1],
					ZEND_TPDE_MACHINE_PART_TYPE_INFO);
				if (payload == UINT32_MAX || type_info == UINT32_MAX
						|| !EncodeBase::encode_zend_native_boxed_table(
							receiver.part(payload), receiver.part(type_info),
							table)) {
					return -1;
				}
			}
		} else if (layout.container_literal) {
			auto [literal_ref, literal] = val_ref_single(
				node.operands[node.machine_reference_operand_index]);
			if (!EncodeBase::encode_zend_native_zval_table(
					GenericValuePart{GenericValuePart::Expr{
						literal.load_to_reg(), 0}},
					table)) {
				return -1;
			}
		} else if (!EncodeBase::encode_zend_native_zval_table(
				GenericValuePart{GenericValuePart::Expr{frame_reg,
					static_cast<int64_t>(layout.container_offset)}},
				table)) {
			return -1;
		}
		const AsmReg table_reg = table.cur_reg_or_load(this);
		ASM(TEST64rr, table_reg, table_reg);
		generate_raw_jump(Jump::je, slow);
		ValuePart element{tpde::x64::PlatformConfig::GP_BANK, 8};
		{
			auto key = val_ref(key_value);
			GenericValuePart table_value{
				GenericValuePart::Expr{table_reg, 0}};
			bool found;
			if (key_long) {
				found = EncodeBase::encode_zend_native_table_find_long(
					std::move(table_value), key.part(0), element);
			} else if (key_string) {
				found = EncodeBase::encode_zend_native_table_find_string(
					std::move(table_value), key.part(0), element);
			} else {
				const uint32_t payload = part_index(key_value,
					ZEND_TPDE_MACHINE_PART_PAYLOAD);
				const uint32_t type_info = part_index(key_value,
					ZEND_TPDE_MACHINE_PART_TYPE_INFO);
				found = payload != UINT32_MAX && type_info != UINT32_MAX
					&& EncodeBase::encode_zend_native_table_find_boxed(
						std::move(table_value), key.part(payload),
						key.part(type_info), element);
			}
			if (!found) {
				return -1;
			}
		}
		table.reset(this);
		const AsmReg element_reg = element.cur_reg_or_load(this);
		auto decision_reg = decision.alloc_gp();
		/* A missing key warns and an undecided probe needs the helper. */
		ASM(CMP64ri, element_reg,
			static_cast<int32_t>(ZEND_NATIVE_ELEMENT_ABSENT));
		generate_raw_jump(Jump::jbe, slow);
		const zend_mir_executable_value_ref &read = mir.value_operation;
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + read.result_storage_id)
				* sizeof(zval);
		const bool publish = zend_mir_id_is_valid(read.result_storage_id)
			&& (read.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| read.result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
			&& result_offset <= INT32_MAX - sizeof(zval);
		auto result = result_ref(node.result);
		if (result_long) {
			ValuePart is_long{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_zval_is_long(
					GenericValuePart{GenericValuePart::Expr{element_reg, 0}},
					is_long)) {
				return -1;
			}
			const AsmReg is_long_reg = is_long.cur_reg_or_load(this);
			ASM(TEST64rr, is_long_reg, is_long_reg);
			is_long.reset(this);
			generate_raw_jump(Jump::je, slow);
			auto value = result.part(0);
			if (!EncodeBase::encode_zend_native_load_u64(
					GenericValuePart{GenericValuePart::Expr{element_reg, 0}},
					value)) {
				return -1;
			}
			if (publish) {
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(result_offset)),
					value.load_to_reg());
				ASM(MOV32mi,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(result_offset
							+ offsetof(zval, u1.type_info))),
					IS_LONG);
			}
		} else {
			/* An owned copy; frame-slot consumers read it there. */
			const uint32_t payload =
				part_index(node.result, ZEND_TPDE_MACHINE_PART_PAYLOAD);
			const uint32_t type_info =
				part_index(node.result, ZEND_TPDE_MACHINE_PART_TYPE_INFO);
			if (payload == UINT32_MAX || type_info == UINT32_MAX) {
				return -1;
			}
			auto result_payload = result.part(payload);
			auto result_type = result.part(type_info);
			if (!EncodeBase::encode_zend_native_zval_copy_deref(
					GenericValuePart{GenericValuePart::Expr{element_reg, 0}},
					result_payload, result_type)) {
				return -1;
			}
			if (publish) {
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(result_offset)),
					result_payload.load_to_reg());
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(result_offset
							+ offsetof(zval, u1.type_info))),
					result_type.load_to_reg());
			}
		}
		element.reset(this);
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	auto read_array = [&]() {
		if (const int element = array_element(ElementAccess::Read);
				element != 0) {
			return element > 0;
		}
		if (const int element = array_element_register(); element != 0) {
			return element > 0;
		}
		return branch_to_guarded_cold();
	};
	/*
	 * A comparison or TYPE_CHECK fused into the following branch (see
	 * freeze_fused_compare_branches): the branch evaluates it. Its operands,
	 * including boundary transports, are dead; statepoint materializations
	 * were consumed already.
	 */
	auto fused_into_branch_placeholder = [&]() -> bool {
		/* A register-held operand, whether a node operand or a
		 * liveness-only register result such as a property read, is
		 * published to its slot, which the branch reads. */
		const zend_mir_executable_value_ref &compare_operation =
			mir.value_operation;
		for (size_t index = 0;
				index < node.liveness_operands.size(); ++index) {
			if (materialized_operand(instruction, index)) {
				continue;
			}
			const IRValueRef operand = node.liveness_operands[index];
			const zend_mir_storage_id storage =
				operand == IRValueRef{Adaptor::FRAME_VALUE}
					|| operand == IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT}
				? ZEND_MIR_ID_INVALID
				: adaptor->canonical_storage(operand);
			if (zend_mir_id_is_valid(storage)
					&& (storage == compare_operation.op1_storage_id
						|| storage
							== compare_operation.op2_storage_id)) {
				if (!materialize_cold_operand(operand, storage)) {
					return false;
				}
				continue;
			}
			auto consumed = val_ref(operand);
			(void) consumed;
		}
		if (node.has_result) {
			auto result = result_ref(node.result);
			for (uint32_t part = 0;
					part < val_parts(node.result).count(); ++part) {
				auto value = result.part(part);
				ASM(MOV32ri, value.alloc_reg(), IS_FALSE);
				value.set_modified();
			}
		}
		return true;
	};
	/*
	 * TYPE_CHECK of a CV or temporary through zend_native_zval_type_check():
	 * the bool lands in the result temporary and a register result; a
	 * temporary is consumed only when another owner keeps it alive. An
	 * undefined variable, which warns, and a resource check, which asks the
	 * resource list, take the guarded cold block. Returns 1 when emitted, 0
	 * when the form does not apply and -1 on an encoding failure.
	 */
	auto type_check_inline = [&]() -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		auto frame_slot = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
		};
		const bool temporary = frame_slot(operation.op1)
			&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| operation.source_opcode != ZEND_TYPE_CHECK
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| operation.extended_value == MAY_BE_RESOURCE
				|| !frame_slot(operation.op1)
				|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& !temporary)
				|| operation.result_storage_id == operation.op1_storage_id
				|| !zend_mir_id_is_valid(operation.op1_storage_id)
				|| !frame_slot(operation.result)
				|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV)
				|| !zend_mir_id_is_valid(operation.result_storage_id)
				|| (node.has_result && val_parts(node.result).count() > 2)
				|| unlocked_gp_registers() < 6) {
			return 0;
		}
		if (!frame_only_operands(node)) {
			return 0;
		}
		const auto successors = guarded_successors_of(node);
		const uint64_t value_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
				* sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (value_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return 0;
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg mask{this};
		ASM(MOV32ri, mask.alloc_gp(),
			static_cast<int32_t>(operation.extended_value));
		ValuePart matched{tpde::x64::PlatformConfig::GP_BANK, 8};
		if (!EncodeBase::encode_zend_native_zval_type_check(
				GenericValuePart{GenericValuePart::Expr{frame_reg,
					static_cast<int64_t>(value_offset)}},
				GenericValuePart{std::move(mask)}, matched)) {
			return -1;
		}
		const AsmReg matched_reg = matched.cur_reg_or_load(this);
		ScratchReg decision{this};
		ScratchReg type{this};
		auto decision_reg = decision.alloc_gp();
		auto type_reg = type.alloc_gp();
		ASM(CMP64ri, matched_reg, ZEND_NATIVE_TYPE_CHECK_UNDEFINED);
		generate_raw_jump(Jump::je, slow);
		/* A CV result is overwritten: its old value must need no
		 * release (a counted one takes the helper). */
		if (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
				Z_TYPE_FLAGS_MASK);
			generate_raw_jump(Jump::jne, slow);
		}
		if (temporary) {
			auto value_address = GenericValuePart{GenericValuePart::Expr{
				frame_reg, static_cast<int64_t>(value_offset)}};
			ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_container_shared(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(value_offset)}}, shared)) {
				return -1;
			}
			const AsmReg shared_reg = shared.cur_reg_or_load(this);
			ASM(TEST64rr, shared_reg, shared_reg);
			shared.reset(this);
			generate_raw_jump(Jump::je, slow);
			if (!EncodeBase::encode_zend_native_release_shared(
					std::move(value_address))) {
				return -1;
			}
		}
		/* IS_FALSE + matched is IS_FALSE or IS_TRUE. */
		ASM(LEA32rm, type_reg,
			FE_MEM(matched_reg, 0, FE_NOREG, IS_FALSE));
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
			type_reg);
		if (node.has_result && val_parts(node.result).count() == 1) {
			auto [result_ref, result] = result_ref_single(node.result);
			mov(result.alloc_reg(), matched_reg, 8);
			result.set_modified();
		} else if (node.has_result) {
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto result_part = result.part(part);
				if (parts.representation.parts[part].semantic_role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					mov(result_part.alloc_reg(), type_reg, 4);
				} else {
					ASM(XOR32rr, result_part.alloc_reg(),
						result_part.cur_reg());
				}
				result_part.set_modified();
			}
		}
		matched.reset(this);
		type.reset();
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	/*
	 * count() of an array CV or temporary through
	 * zend_native_zval_array_count(): the integer lands in the result
	 * temporary and a register result; a temporary is consumed only when
	 * another owner keeps it alive. Every other value takes the guarded cold
	 * block. Returns 1 when emitted, 0 when the form does not apply and -1
	 * on an encoding failure.
	 */
	/*
	 * QM_ASSIGN of a literal: zend_native_zval_copy() copies it, with its
	 * reference, into the fresh result temporary and a register result.
	 * Returns 1 when emitted and 0 when the form does not apply, -1 on an
	 * encoding failure.
	 */
	/*
	 * === and !== of a CV or temporary and a literal through
	 * zend_native_zval_identical(): the bool lands in the result temporary
	 * and a register result; a temporary is consumed only when another
	 * owner keeps it alive. An undefined variable, doubles, arrays and
	 * objects take the guarded cold block. Returns 1 when emitted, 0 when the
	 * form does not apply and -1 on an encoding failure.
	 */
	auto identical_literal = [&]() -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		auto frame_slot = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
		};
		const bool negated = operation.source_opcode == ZEND_IS_NOT_IDENTICAL;
		const bool temporary = frame_slot(operation.op1)
			&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
		/* The second operand: a literal, or a CV or temporary compared
		 * through zend_native_zval_identical_any(); a temporary is
		 * consumed as the first one. */
		const bool other_temporary = frame_slot(operation.op2)
			&& operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
		const bool variable_other = frame_slot(operation.op2)
			&& (operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| other_temporary)
			&& zend_mir_id_is_valid(operation.op2_storage_id)
			&& operation.op2_storage_id != operation.result_storage_id
			&& operation.op2_storage_id != operation.op1_storage_id;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| mir.fused_into_branch
				|| (operation.source_opcode != ZEND_IS_IDENTICAL && !negated)
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| (operation.op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
					&& !variable_other)
				|| !frame_slot(operation.op1)
				|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& !temporary)
				|| !zend_mir_id_is_valid(operation.op1_storage_id)
				|| !frame_slot(operation.result)
				|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV)
				|| !zend_mir_id_is_valid(operation.result_storage_id)
				|| operation.result_storage_id == operation.op1_storage_id
				|| (variable_other && operation.result_storage_id
					== operation.op2_storage_id)
				|| (node.has_result && val_parts(node.result).count() > 2)
				/* The snippet's seven scratch registers, the frame and the
				 * literals. */
				|| unlocked_gp_registers() < 9) {
			return 0;
		}
		/* A temporary may be held boxed in registers: it is stored into its
		 * slot first, and a register copy of the literal is not needed. */
		const bool register_temporary = temporary && !variable_other
			&& node.operands.size() == 3
			&& node.materialization_count == 0
			&& node.operands[2] == IRValueRef{Adaptor::FRAME_VALUE}
			&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			&& adaptor->machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& val_parts(node.operands[0]).count() == 2;
		if (!register_temporary) {
			if (!frame_only_operands(node)) {
				return 0;
			}
		}
		const auto successors = guarded_successors_of(node);
		const uint64_t value_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
				* sizeof(zval);
		const uint64_t literal_offset = variable_other
			? (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op2_storage_id)
				* sizeof(zval)
			: uint64_t{operation.op2.index} * sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (value_offset > INT32_MAX - sizeof(zval)
				|| literal_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return 0;
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		if (register_temporary) {
			auto boxed = val_ref(node.operands[0]);
			auto payload = boxed.part(0);
			auto type_info = boxed.part(1);
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(value_offset)),
				payload.load_to_reg());
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(value_offset
						+ offsetof(zval, u1.type_info))),
				type_info.load_to_reg());
			payload.reset();
			type_info.reset();
			if (node.operands[1] != IRValueRef{Adaptor::FRAME_VALUE}) {
				auto literal_copy = val_ref(node.operands[1]);
				(void) literal_copy;
			}
		}
		ScratchReg literals{this};
		ValuePart matched{tpde::x64::PlatformConfig::GP_BANK, 8};
		if (variable_other) {
			if (!EncodeBase::encode_zend_native_zval_identical_any(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(value_offset)}},
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(literal_offset)}},
					matched)) {
				return -1;
			}
		} else {
			auto literals_reg = literals.alloc_gp();
			ASM(MOV64rm, literals_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, literals_reg,
				FE_MEM(literals_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_op_array, literals))));
			if (!EncodeBase::encode_zend_native_zval_identical(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(value_offset)}},
					GenericValuePart{GenericValuePart::Expr{literals_reg,
						static_cast<int64_t>(literal_offset)}},
					matched)) {
				return -1;
			}
		}
		literals.reset();
		const AsmReg matched_reg = matched.cur_reg_or_load(this);
		ScratchReg decision{this};
		ScratchReg type{this};
		auto decision_reg = decision.alloc_gp();
		auto type_reg = type.alloc_gp();
		ASM(CMP64ri, matched_reg, ZEND_NATIVE_IDENTICAL_UNKNOWN);
		generate_raw_jump(Jump::je, slow);
		/* A CV result is overwritten: its old value must need no
		 * release (a counted one takes the helper). */
		if (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
				Z_TYPE_FLAGS_MASK);
			generate_raw_jump(Jump::jne, slow);
		}
		/* A temporary second operand must need no release: a counted
		 * one takes the helper. */
		if (variable_other && other_temporary) {
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(literal_offset
					+ offsetof(zval, u1.type_info))),
				Z_TYPE_FLAGS_MASK);
			generate_raw_jump(Jump::jne, slow);
		}
		if (temporary) {
			ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_container_shared(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(value_offset)}}, shared)) {
				return -1;
			}
			const AsmReg shared_reg = shared.cur_reg_or_load(this);
			ASM(TEST64rr, shared_reg, shared_reg);
			shared.reset(this);
			generate_raw_jump(Jump::je, slow);
			if (!EncodeBase::encode_zend_native_release_shared(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(value_offset)}})) {
				return -1;
			}
		}
		if (negated) {
			ASM(XOR32ri, matched_reg, 1);
		}
		/* IS_FALSE + matched is IS_FALSE or IS_TRUE. */
		ASM(LEA32rm, type_reg,
			FE_MEM(matched_reg, 0, FE_NOREG, IS_FALSE));
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
			type_reg);
		if (node.has_result && val_parts(node.result).count() == 1) {
			auto [result_ref, result] = result_ref_single(node.result);
			mov(result.alloc_reg(), matched_reg, 8);
			result.set_modified();
		} else if (node.has_result) {
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto result_part = result.part(part);
				if (parts.representation.parts[part].semantic_role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					mov(result_part.alloc_reg(), type_reg, 4);
				} else {
					ASM(XOR32rr, result_part.alloc_reg(),
						result_part.cur_reg());
				}
				result_part.set_modified();
			}
		}
		matched.reset(this);
		type.reset();
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	/*
	 * array_key_exists() of a literal or CV key in an array CV or temporary
	 * through the element probes: a found element is true and a certainly
	 * absent key false, both in the result temporary and a register result;
	 * a temporary array is consumed only when another owner keeps it alive.
	 * Undecided probes take the guarded cold block. Returns 1 when emitted, 0
	 * when the form does not apply and -1 on an encoding failure.
	 */
	auto key_exists_inline = [&]() -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		auto frame_slot = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
		};
		const bool key_literal =
			operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
		const bool temporary = frame_slot(operation.op2)
			&& operation.op2.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| operation.source_opcode != ZEND_ARRAY_KEY_EXISTS
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| (!key_literal
					&& (!frame_slot(operation.op1)
						|| operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
						|| !zend_mir_id_is_valid(operation.op1_storage_id)))
				|| !frame_slot(operation.op2)
				|| (operation.op2.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& !temporary)
				|| !zend_mir_id_is_valid(operation.op2_storage_id)
				|| !frame_slot(operation.result)
				|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV)
				|| !zend_mir_id_is_valid(operation.result_storage_id)
				|| operation.result_storage_id == operation.op2_storage_id
				|| (!key_literal
					&& operation.result_storage_id
						== operation.op1_storage_id)
				|| (node.has_result && val_parts(node.result).count() > 2)
				/* The probe's seven scratch registers, its result and the
				 * literals, with one to spare. */
				|| unlocked_gp_registers() < 10) {
			return 0;
		}
		if (!frame_only_operands(node)) {
			return 0;
		}
		const auto successors = guarded_successors_of(node);
		auto frame_offset = [](zend_mir_storage_id storage) {
			return (uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		};
		const uint64_t key_offset = key_literal
			? uint64_t{operation.op1.index} * sizeof(zval)
			: frame_offset(operation.op1_storage_id);
		const uint64_t array_offset = frame_offset(operation.op2_storage_id);
		const uint64_t result_offset =
			frame_offset(operation.result_storage_id);
		if (key_offset > INT32_MAX - sizeof(zval)
				|| array_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return 0;
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		auto address = [](AsmReg base, uint64_t offset) {
			return GenericValuePart{GenericValuePart::Expr{
				base, static_cast<int64_t>(offset)}};
		};
		/* A read of the found element reuses it (lookup_reuse_consumer()). */
		const IRInstRef reuse = temporary
			? IRInstRef{UINT32_MAX} : lookup_reuse_consumer(instruction);
		if (reuse != IRInstRef{UINT32_MAX}) {
			ASM(MOV64mi, FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()), 0);
		}
		ValuePart element{tpde::x64::PlatformConfig::GP_BANK, 8};
		{
			ScratchReg literals{this};
			AsmReg key_base = frame_reg;
			if (key_literal) {
				key_base = literals.alloc_gp();
				ASM(MOV64rm, key_base,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, func))));
				ASM(MOV64rm, key_base,
					FE_MEM(key_base, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_op_array, literals))));
			}
			/* A literal argument keeps a numeric string, which only a
			 * dimension literal has as an integer: probe it as a runtime
			 * key. */
			const bool probed = EncodeBase::encode_zend_native_array_find_key(
				address(frame_reg, array_offset),
				address(key_base, key_offset), element);
			if (!probed) {
				return -1;
			}
		}
		const AsmReg element_reg = element.cur_reg_or_load(this);
		ScratchReg decision{this};
		ScratchReg answer{this};
		auto decision_reg = decision.alloc_gp();
		auto answer_reg = answer.alloc_gp();
		ASM(CMP64ri, element_reg,
			static_cast<int32_t>(ZEND_NATIVE_ELEMENT_ABSENT));
		generate_raw_jump(Jump::jb, slow);
		if (reuse != IRInstRef{UINT32_MAX}) {
			/* ABSENT only reaches the false edge, where nothing reads it. */
			ASM(MOV64mr, FE_MEM(FE_BP, 0, FE_NOREG, lookup_reuse_slot()),
				element_reg);
			lookup_reuse_reads_.push_back(static_cast<uint32_t>(reuse));
		}
		/* ABSENT is 1 and an element pointer more: exists = element > 1. */
		generate_raw_set(Jump::ja, answer_reg);
		element.reset(this);
		/* A CV result is overwritten: its old value must need no
		 * release (a counted one takes the helper). */
		if (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
				Z_TYPE_FLAGS_MASK);
			generate_raw_jump(Jump::jne, slow);
		}
		if (temporary) {
			ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_container_shared(
					address(frame_reg, array_offset), shared)) {
				return -1;
			}
			const AsmReg shared_reg = shared.cur_reg_or_load(this);
			ASM(TEST64rr, shared_reg, shared_reg);
			shared.reset(this);
			generate_raw_jump(Jump::je, slow);
			if (!EncodeBase::encode_zend_native_release_shared(
					address(frame_reg, array_offset))) {
				return -1;
			}
		}
		{
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			ASM(LEA32rm, type_reg,
				FE_MEM(answer_reg, 0, FE_NOREG, IS_FALSE));
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset
						+ offsetof(zval, u1.type_info))),
				type_reg);
			if (node.has_result && val_parts(node.result).count() == 1) {
				auto [result_ref, result] = result_ref_single(node.result);
				mov(result.alloc_reg(), answer_reg, 8);
				result.set_modified();
			} else if (node.has_result) {
				auto result = result_ref(node.result);
				const ValueParts parts = val_parts(node.result);
				for (uint32_t part = 0; part < parts.count(); ++part) {
					auto result_part = result.part(part);
					if (parts.representation.parts[part].semantic_role
							== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
						mov(result_part.alloc_reg(), type_reg, 4);
					} else {
						ASM(XOR32rr, result_part.alloc_reg(),
							result_part.cur_reg());
					}
					result_part.set_modified();
				}
			}
		}
		answer.reset();
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	/*
	 * FE_FREE of an array holder that another owner keeps alive drops one
	 * reference (zend_native_iterator_shared, _release_shared), as the VM's
	 * inlined zval_ptr_dtor_nogc does. Object iterators and a last reference
	 * take the guarded cold block. Returns 1 when emitted, 0 when the form
	 * does not apply and -1 on an encoding failure.
	 */
	auto iterator_free_inline = [&]() -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| operation.source_opcode != ZEND_FE_FREE
				|| node.has_result
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| (operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
				|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_VAR)
				|| !zend_mir_id_is_valid(operation.op1_storage_id)
				|| unlocked_gp_registers() < 6) {
			return 0;
		}
		if (!frame_only_operands(node)) {
			return 0;
		}
		const auto successors = guarded_successors_of(node);
		const uint64_t holder_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
				* sizeof(zval);
		if (holder_offset > INT32_MAX - sizeof(zval)) {
			return 0;
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		auto holder = [&]() {
			return GenericValuePart{GenericValuePart::Expr{frame_reg,
				static_cast<int64_t>(holder_offset)}};
		};
		ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
		if (!EncodeBase::encode_zend_native_iterator_shared(
				holder(), shared)) {
			return -1;
		}
		const AsmReg shared_reg = shared.cur_reg_or_load(this);
		ScratchReg decision{this};
		auto decision_reg = decision.alloc_gp();
		ASM(TEST64rr, shared_reg, shared_reg);
		shared.reset(this);
		generate_raw_jump(Jump::je, slow);
		if (!EncodeBase::encode_zend_native_release_shared(holder())) {
			return -1;
		}
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	auto copy_literal = [&]() -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| operation.source_opcode != ZEND_QM_ASSIGN
				|| operation.op1.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
					&& operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
				|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR)
				|| !zend_mir_id_is_valid(operation.result_storage_id)
				|| (node.has_result
					&& (adaptor->machine_kind(node.result)
							!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| val_parts(node.result).count() != 2))
				|| unlocked_gp_registers() < 6) {
			return 0;
		}
		if (!frame_only_operands(node)) {
			return 0;
		}
		const auto successors = guarded_successors_of(node);
		const uint64_t literal_offset =
			uint64_t{operation.op1.index} * sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (literal_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return 0;
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg literals{this};
		auto literals_reg = literals.alloc_gp();
		ASM(MOV64rm, literals_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, func))));
		ASM(MOV64rm, literals_reg,
			FE_MEM(literals_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_op_array, literals))));
		ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
		ValuePart type_info{tpde::x64::PlatformConfig::GP_BANK, 8};
		if (!EncodeBase::encode_zend_native_zval_copy(
				GenericValuePart{GenericValuePart::Expr{literals_reg,
					static_cast<int64_t>(literal_offset)}},
				payload, type_info)) {
			return -1;
		}
		literals.reset();
		const AsmReg payload_reg = payload.cur_reg_or_load(this);
		const AsmReg type_info_reg = type_info.cur_reg_or_load(this);
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset)),
			payload_reg);
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
			type_info_reg);
		if (node.has_result) {
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto result_part = result.part(part);
				mov(result_part.alloc_reg(),
					parts.representation.parts[part].semantic_role
							== ZEND_TPDE_MACHINE_PART_TYPE_INFO
						? type_info_reg : payload_reg, 8);
				result_part.set_modified();
			}
		}
		payload.reset(this);
		type_info.reset(this);
		frame_scratch.reset();
		/* The copy cannot fail: the fast block continues unconditionally. */
		generate_uncond_branch(successors[0]);
		return 1;
	};
	auto count_inline = [&]() -> int {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		auto frame_slot = [](const zend_mir_source_operand_ref &operand) {
			return operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
		};
		const bool temporary = frame_slot(operation.op1)
			&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| !mir.has_value_operation
				|| operation.source_opcode != ZEND_COUNT
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| !frame_slot(operation.op1)
				|| (operation.op1.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
					&& !temporary)
				|| !zend_mir_id_is_valid(operation.op1_storage_id)
				|| !frame_slot(operation.result)
				|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_VAR
					&& operation.result.slot_kind
						!= ZEND_MIR_SOURCE_SLOT_CV)
				|| !zend_mir_id_is_valid(operation.result_storage_id)
				|| operation.result_storage_id == operation.op1_storage_id
				|| (node.has_result && val_parts(node.result).count() > 2)
				|| unlocked_gp_registers() < 6) {
			return 0;
		}
		if (!frame_only_operands(node)) {
			return 0;
		}
		const auto successors = guarded_successors_of(node);
		const uint64_t value_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
				* sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (value_offset > INT32_MAX - sizeof(zval)
				|| result_offset > INT32_MAX - sizeof(zval)) {
			return 0;
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		auto value_address = [&]() {
			return GenericValuePart{GenericValuePart::Expr{frame_reg,
				static_cast<int64_t>(value_offset)}};
		};
		ValuePart count{tpde::x64::PlatformConfig::GP_BANK, 8};
		if (!EncodeBase::encode_zend_native_zval_array_count(
				value_address(), count)) {
			return -1;
		}
		const AsmReg count_reg = count.cur_reg_or_load(this);
		ScratchReg decision{this};
		auto decision_reg = decision.alloc_gp();
		ASM(CMP64ri, count_reg, -1);
		generate_raw_jump(Jump::je, slow);
		/* A CV result is overwritten: its old value must need no
		 * release (a counted one takes the helper). */
		if (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
				Z_TYPE_FLAGS_MASK);
			generate_raw_jump(Jump::jne, slow);
		}
		if (temporary) {
			ValuePart shared{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_container_shared(
					value_address(), shared)) {
				return -1;
			}
			const AsmReg shared_reg = shared.cur_reg_or_load(this);
			ASM(TEST64rr, shared_reg, shared_reg);
			shared.reset(this);
			generate_raw_jump(Jump::je, slow);
			if (!EncodeBase::encode_zend_native_release_shared(
					value_address())) {
				return -1;
			}
		}
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset)),
			count_reg);
		ASM(MOV32mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(result_offset
					+ offsetof(zval, u1.type_info))),
			IS_LONG);
		if (node.has_result && val_parts(node.result).count() == 1) {
			auto [result_ref, result] = result_ref_single(node.result);
			mov(result.alloc_reg(), count_reg, 8);
			result.set_modified();
		} else if (node.has_result) {
			auto result = result_ref(node.result);
			const ValueParts parts = val_parts(node.result);
			for (uint32_t part = 0; part < parts.count(); ++part) {
				auto result_part = result.part(part);
				if (parts.representation.parts[part].semantic_role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					ASM(MOV32ri, result_part.alloc_reg(), IS_LONG);
				} else {
					mov(result_part.alloc_reg(), count_reg, 8);
				}
				result_part.set_modified();
			}
		}
		count.reset(this);
		finish_guarded(slow, done, std::move(decision), frame_scratch,
			successors);
		return 1;
	};
	auto write_array = [&]() {
		if (const int element = array_element(ElementAccess::Write);
				element != 0) {
			return element > 0;
		}
		return branch_to_guarded_cold();
	};
	auto coalesce_array = [&]() {
		if (const int element = array_element(ElementAccess::Coalesce);
				element != 0) {
			return element > 0;
		}
		return branch_to_guarded_cold();
	};
	auto isset_array = [&]() {
		reuse_read = lookup_reuse_consumer(instruction);
		const int element = array_element(
			(mir.value_operation.extended_value & ZEND_ISEMPTY) != 0
				? ElementAccess::Empty : ElementAccess::Isset);
		if (element != 0) {
			return element > 0;
		}
		return branch_to_guarded_cold();
	};
	auto append_packed_array = [&]() {
		zend_tpde_packed_array_append layout;
		const zend_tpde_machine_reference *element_reference =
			operation_machine_reference(
				ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT);

		const bool append_layout =
			zend_tpde_packed_array_append_at(mir, &layout);
		if (!append_layout
				|| (!layout.indirect_container
					&& (element_reference == nullptr
						|| !zend_mir_id_is_valid(
							element_reference->base_value_id)
						|| zend_mir_id_is_valid(
							element_reference->index_value_id)
						|| element_reference->scale != sizeof(zval)
						|| element_reference->access_width
							!= sizeof(zval)))
				|| layout.container_offset > INT32_MAX - 8
				|| layout.value_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
			/* A keyed assignment replaces an existing element inline; an
			 * append without its inline layout calls the helper (see
			 * freeze_machine_control_flow()). */
			if (node.kind == Adaptor::InstKind::GuardedFast
					&& mir.value_operation.op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				const int element = array_element(ElementAccess::Assign);
				if (element != 0) {
					return element > 0;
				}
				return branch_to_guarded_cold();
			}
			return execute_value_operation();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors = guarded_successors_of(node);
		const bool scalar_value =
			node.packed_append_value_operand_index < node.operands.size()
			&& adaptor->representation(node.operands[
				node.packed_append_value_operand_index])
				== ZEND_MIR_REPRESENTATION_I64
			&& adaptor->exact_type(node.operands[
				node.packed_append_value_operand_index])
				== ZEND_MIR_SCALAR_TYPE_I64
			&& adaptor->machine_kind(node.operands[
				node.packed_append_value_operand_index])
				== ZEND_TPDE_MACHINE_VALUE_I64;
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		/* Nine scratch registers; see unlocked_gp_registers(). */
		if (unlocked_gp_registers() < 10) {
			return branch_to_guarded_cold();
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg array{this};
		ScratchReg count{this};
		ScratchReg limit{this};
		ScratchReg element{this};
		ScratchReg low_word{this};
		ScratchReg high_word{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto count_reg = count.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		if (layout.indirect_container) {
			/* The VAR addresses the array's zval, as ZEND_FETCH_DIM_W left
			 * it; the helper handles any other VAR. */
			ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.container_offset
					+ offsetof(zval, u1.v.type))), IS_INDIRECT);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, element_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.container_offset)));
			load_array_container(element_reg, 0, type_reg, array_reg, slow);
		} else {
			load_array_container(frame_reg,
				static_cast<int32_t>(layout.container_offset),
				type_reg, array_reg, slow);
		}
		ASM(CMP32mi,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		generate_raw_jump(Jump::jne, slow);
		ASM(TEST32mi,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_refcounted_h, u))),
			IS_ARRAY_IMMUTABLE);
		generate_raw_jump(Jump::jne, slow);
		ASM(TEST32mi,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, u))),
			HASH_FLAG_PACKED);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV32rm, count_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumUsed))));
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nTableSize))));
		ASM(CMP32rr, count_reg, limit_reg);
		generate_raw_jump(Jump::jae, slow);
		ASM(MOV64rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(HashTable, nNextFreeElement))));
		ASM(CMP64rr, count_reg, limit_reg);
		generate_raw_jump(Jump::jne, slow);

		if (!scalar_value) {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP8ri, type_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
			ASM(CMP8ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
			ASM(CMP8ri, type_reg, IS_INDIRECT);
			generate_raw_jump(Jump::je, slow);
		}
		if (layout.has_result) {
			ASM(MOV32rm, limit_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, limit_reg, IS_UNDEF);
			generate_raw_jump(Jump::jne, slow);
		}

		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arPacked))));
		ASM(SHL64ri, count_reg, 4);
		ASM(ADD64rr, element_reg, count_reg);
		ASM(SHR64ri, count_reg, 4);
		if (scalar_value) {
			auto [value_ref, value] = val_ref_single(node.operands[
				node.packed_append_value_operand_index]);
			auto value_reg = value.load_to_reg();
			ASM(MOV64rr, low_word_reg, value_reg);
			ASM(MOV64ri, high_word_reg, IS_LONG);
		} else {
			ASM(MOV64rm, low_word_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.value_offset)));
			ASM(MOV64rm, high_word_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.value_offset + 8)));
		}
		ASM(MOV64mr,
			FE_MEM(element_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV64mr,
			FE_MEM(element_reg, 0, FE_NOREG, 8), high_word_reg);
		if (layout.move_value) {
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))),
				IS_UNDEF);
		} else if (!scalar_value) {
			auto copied = text_writer.label_create();
			ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::je, copied);
			ASM(ADD32mi,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				1);
			label_place(copied);
		}
		ASM(ADD32ri, count_reg, 1);
		ASM(MOV32mr,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumUsed))),
			count_reg);
		ASM(ADD32mi,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumOfElements))),
			1);
		ASM(MOV64mr,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNextFreeElement))),
			count_reg);
		if (layout.indirect_container) {
			/* The consumed VAR, as the helper leaves it. */
			ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.container_offset
					+ offsetof(zval, u1.type_info))), IS_UNDEF);
		}
		if (layout.has_result) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				low_word_reg);
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset + 8)),
				high_word_reg);
			if (!scalar_value) {
				auto result_copied = text_writer.label_create();
				ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
				generate_raw_jump(Jump::je, result_copied);
				ASM(ADD32mi,
					FE_MEM(low_word_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_refcounted_h, refcount))),
					1);
				label_place(result_copied);
			}
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto direct_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (guarded_exit_can_jump_directly(
					direct_successors[1], direct_successors[0])) {
				generate_guarded_direct_exit(
					slow, direct_successors[1], direct_successors[0]);
				return true;
			}
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		type.reset();
		array.reset();
		count.reset();
		limit.reset();
		element.reset();
		low_word.reset();
		high_word.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	auto string_length = [&]() {
		zend_tpde_string_length layout;

		if (!zend_tpde_string_length_at(mir, &layout, true)) {
			zend_tpde_bool_unary boolean;
			const bool boolean_layout =
				zend_tpde_bool_unary_at(mir, &boolean);
			const bool register_boolean = boolean_layout
				&& !node.synthetic
				&& node.kind == Adaptor::InstKind::MIR
				&& node.has_result
				&& node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				&& node.operands.size() == 2
				&& adaptor->exact_type(node.operands[0])
					== ZEND_MIR_SCALAR_TYPE_I1
				&& adaptor->machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_BOOL
				&& node.operands[1] == IRValueRef{Adaptor::FRAME_VALUE};
			if (register_boolean) {
				auto [operand_ref, operand] =
					val_ref_single(node.operands[0]);
				auto [result_ref, result] = result_ref_single(node.result);
				auto operand_reg = operand.load_to_reg();
				auto result_reg = result.alloc_reg();
				ASM(MOV32rr, result_reg, operand_reg);
				if (boolean.negate) {
					ASM(XOR32ri, result_reg, 1);
				}
				result.set_modified();
				return true;
			}
			const bool frame_only = node.operands.size() == 1
				&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE};
			if (!node.synthetic
					&& node.kind == Adaptor::InstKind::MIR
					&& node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
					&& frame_only
					&& boolean_layout) {
				auto [frame_ref, frame] =
					val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
				auto frame_scratch = frame_register(std::move(frame));
				auto frame_reg = frame_scratch.cur_reg();
				ScratchReg result{this};
				auto result_reg = result.alloc_gp();
				ASM(MOV32rm, result_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(boolean.operand_offset)));
				if (boolean.negate) {
					ASM(XOR32ri, result_reg, 1);
				}
				ScratchReg type{this};
				store_boolean_zval(frame_reg,
					static_cast<int32_t>(boolean.result_offset), result_reg,
					type.alloc_gp());
				return true;
			}
			/*
			 * ! and (bool) of an untyped null, boolean, integer, string or
			 * array decide inline; an undefined variable (which warns), a
			 * reference, a counted temporary string or array (which the
			 * operation releases) and any other type take the guarded cold
			 * block.
			 */
			if (boolean_layout && frame_only
					&& node.kind == Adaptor::InstKind::GuardedFast
					&& node.control_block != UINT32_MAX
					&& node.continuation_block != UINT32_MAX
					&& boolean.operand_offset <= INT32_MAX - sizeof(zval)
					&& boolean.result_offset <= INT32_MAX - sizeof(zval)
					&& (!node.has_result
						|| val_parts(node.result).count() <= 2)) {
				const auto successors =
					adaptor->block_succs(IRBlockRef{node.control_block});
				if (successors.size() >= 2
						&& static_cast<uint32_t>(successors[0])
							== node.continuation_block
						&& static_cast<uint32_t>(successors[1])
							== node.argument_index) {
					auto slow = text_writer.label_create();
					auto done = text_writer.label_create();
					auto integer = text_writer.label_create();
					auto decided = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = frame_register(std::move(frame));
					auto frame_reg = frame_scratch.cur_reg();
					ScratchReg decision{this};
					ScratchReg type{this};
					ScratchReg truth{this};
					auto decision_reg = decision.alloc_gp();
					auto type_reg = type.alloc_gp();
					auto truth_reg = truth.alloc_gp();
					const int32_t operand =
						static_cast<int32_t>(boolean.operand_offset);
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							operand + static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
					auto string = text_writer.label_create();
					auto array = text_writer.label_create();
					ASM(XOR32rr, truth_reg, truth_reg);
					ASM(CMP32ri, type_reg, IS_LONG);
					generate_raw_jump(Jump::je, integer);
					ASM(CMP32ri, type_reg, IS_STRING);
					generate_raw_jump(Jump::je, string);
					ASM(CMP32ri, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::je, array);
					ASM(CMP32ri, type_reg, IS_TRUE);
					generate_raw_jump(Jump::ja, slow);
					ASM(TEST32rr, type_reg, type_reg);
					generate_raw_jump(Jump::je, slow);
					ASM(CMP32ri, type_reg, IS_TRUE);
					generate_raw_set(Jump::je, truth_reg);
					generate_raw_jump(Jump::jmp, decided);
					const bool releases_operand =
						mir.value_operation.op1.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_CV;
					label_place(string);
					if (releases_operand) {
						ASM(TEST8mi, FE_MEM(frame_reg, 0, FE_NOREG,
							operand + static_cast<int32_t>(
								offsetof(zval, u1.v.type_flags))),
							IS_TYPE_REFCOUNTED);
						generate_raw_jump(Jump::jne, slow);
					}
					/* A string is true unless empty or "0". */
					ASM(MOV64rm, type_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						operand));
					ASM(CMP64mi, FE_MEM(type_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_string, len))), 1);
					generate_raw_set(Jump::ja, truth_reg);
					generate_raw_jump(Jump::jne, decided);
					ASM(CMP8mi, FE_MEM(type_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_string, val))), '0');
					generate_raw_set(Jump::jne, truth_reg);
					generate_raw_jump(Jump::jmp, decided);
					label_place(array);
					if (releases_operand) {
						ASM(TEST8mi, FE_MEM(frame_reg, 0, FE_NOREG,
							operand + static_cast<int32_t>(
								offsetof(zval, u1.v.type_flags))),
							IS_TYPE_REFCOUNTED);
						generate_raw_jump(Jump::jne, slow);
					}
					ASM(MOV64rm, type_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						operand));
					ASM(CMP32mi, FE_MEM(type_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(HashTable,
							nNumOfElements))), 0);
					generate_raw_set(Jump::jne, truth_reg);
					generate_raw_jump(Jump::jmp, decided);
					label_place(integer);
					ASM(CMP64mi, FE_MEM(frame_reg, 0, FE_NOREG, operand), 0);
					generate_raw_set(Jump::jne, truth_reg);
					label_place(decided);
					if (boolean.negate) {
						ASM(XOR32ri, truth_reg, 1);
					}
					/* IS_FALSE + truth is IS_FALSE or IS_TRUE. */
					ASM(LEA32rm, type_reg,
						FE_MEM(truth_reg, 0, FE_NOREG, IS_FALSE));
					ASM(MOV32mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(boolean.result_offset
								+ offsetof(zval, u1.type_info))),
						type_reg);
					if (node.has_result
							&& val_parts(node.result).count() == 1) {
						auto [result_ref, result] =
							result_ref_single(node.result);
						mov(result.alloc_reg(), truth_reg, 8);
						result.set_modified();
					} else if (node.has_result) {
						auto result = result_ref(node.result);
						const ValueParts parts = val_parts(node.result);
						for (uint32_t part = 0; part < parts.count(); ++part) {
							auto result_part = result.part(part);
							if (parts.representation.parts[part].semantic_role
									== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
								mov(result_part.alloc_reg(), type_reg, 4);
							} else {
								ASM(XOR32rr, result_part.alloc_reg(),
									result_part.cur_reg());
							}
							result_part.set_modified();
						}
					}
					truth.reset();
					type.reset();
					finish_guarded(slow, done, std::move(decision), frame_scratch,
						successors);
					return true;
				}
			}
			return execute_value_operation();
		}
		const bool frame_only = node.operands.size() == 1
			&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE};
		const bool register_string = node.operands.size() == 2
			&& adaptor->machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			&& node.operands[1] == IRValueRef{Adaptor::FRAME_VALUE};
		const bool normal_register_string = register_string
			&& !node.synthetic
			&& node.kind == Adaptor::InstKind::MIR
			&& node.has_result
			&& node.exact_type == ZEND_MIR_SCALAR_TYPE_I64;
		const bool normal_frame_string = frame_only
			&& !node.synthetic
			&& node.kind == Adaptor::InstKind::MIR
			&& node.has_result
			&& node.exact_type == ZEND_MIR_SCALAR_TYPE_I64;
		if (layout.operand_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8
				|| (!frame_only && !register_string)) {
			return false;
		}
		if (normal_register_string) {
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_scratch = frame_register(std::move(frame));
			auto frame_reg = frame_scratch.cur_reg();
			auto string = val_ref(node.operands[0]);
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			uint64_t known_length = 0;
			if (adaptor->known_string_literal(
					node.operands[0], &known_length, nullptr)) {
				ASM(MOV64ri, result_reg, known_length);
			} else {
				auto string_reg = string.part(0).load_to_reg();
				ASM(MOV64rm, result_reg,
					FE_MEM(string_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_string, len))));
			}
			/*
			 * Source call arguments may remain canonical zvals even when the
			 * producer is register-authoritative.  Publish strlen's complete
			 * result before such a call can observe the temporary; later machine
			 * consumers still use result_reg directly.
			 */
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				result_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_LONG);
			result.set_modified();
			return true;
		}
		if (normal_frame_string) {
			auto [frame_ref, frame] =
				val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
			auto frame_scratch = frame_register(std::move(frame));
			auto frame_reg = frame_scratch.cur_reg();
			ScratchReg string{this};
			auto string_reg = string.alloc_gp();
			ASM(MOV64rm, string_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(MOV64rm, result_reg,
				FE_MEM(string_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_string, len))));
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				result_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_LONG);
			result.set_modified();
			return true;
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors = guarded_successors_of(node);
		if (register_string) {
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_scratch = frame_register(std::move(frame));
			auto frame_reg = frame_scratch.cur_reg();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			uint64_t known_length = 0;
			if (adaptor->known_string_literal(
					node.operands[0], &known_length, nullptr)) {
				ASM(MOV64ri, result_reg, known_length);
			} else {
				auto [string_ref, string] =
					val_ref_single(node.operands[0]);
				auto string_reg = string.load_to_reg();
				ASM(MOV64rm, result_reg,
					FE_MEM(string_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_string, len))));
			}
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				result_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_LONG);
			result.set_modified();
			generate_uncond_branch(successors[0]);
			return true;
		}
		auto slow = text_writer.label_create();
		auto ready = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg string{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto string_reg = string.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		ASM(MOVZXr32m8, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.operand_offset + offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, string_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)));

		/* A frame-only consumer such as RETURN reads the published slot. */
		auto store_length = [&](AsmReg length_reg) {
			ASM(MOV64rm, length_reg,
				FE_MEM(string_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_string, len))));
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				length_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_LONG);
		};
		if (node.has_result) {
			auto [result_ref, result] = result_ref_single(node.result);
			store_length(result.alloc_reg());
			result.set_modified();
		} else {
			ScratchReg length{this};
			store_length(length.alloc_gp());
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto direct_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (guarded_exit_can_jump_directly(
					direct_successors[1], direct_successors[0])) {
				generate_guarded_direct_exit(
					slow, direct_successors[1], direct_successors[0]);
				return true;
			}
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, ready);

		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(ready);
		type.reset();
		string.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	auto long_binary = [&]() {
		if (const int identical = identical_literal(); identical != 0) {
			return identical > 0;
		}
		zend_tpde_long_binary layout{};
		const bool framed_layout =
			zend_tpde_long_binary_at(mir, &layout);
		auto register_long_operand = [&](IRValueRef operand) {
			return adaptor->exact_type(operand)
						== ZEND_MIR_SCALAR_TYPE_I64
				|| (adaptor->exact_type(operand)
						== ZEND_MIR_SCALAR_TYPE_NONE
					&& adaptor->machine_kind(operand)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL);
		};
		const bool register_layout =
			node.has_result
				&& node.operands.size() == 3
				&& register_long_operand(node.operands[0])
				&& register_long_operand(node.operands[1]);
		bool register_result_layout = false;
		if (!framed_layout && register_layout
				&& zend_mir_id_is_valid(
					mir.value_operation.result_storage_id)
				&& (mir.value_operation.result.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT
					|| mir.value_operation.result.kind
						== ZEND_MIR_SOURCE_OPERAND_SSA)
				&& (mir.value_operation.result.slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP
					|| mir.value_operation.result.slot_kind
						== ZEND_MIR_SOURCE_SLOT_VAR
					|| mir.value_operation.result.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV)) {
			const uint64_t result_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ mir.value_operation.result_storage_id)
					* sizeof(zval);
			if (result_offset <= UINT32_MAX - 8) {
				layout.result_offset =
					static_cast<uint32_t>(result_offset);
				register_result_layout = true;
			}
		}
		/*
		 * Operands that live in CV slots or literals, as in most untyped
		 * loops, never reach the register form. Evaluate integer and floating
		 * point operands inline like the VM's fast paths: long/long with an
		 * overflow check, any long/double mix in double precision. Other
		 * types, integer overflow, integer division and a zero divisor use
		 * the helper.
		 */
		auto numeric_framed_binary = [&]() -> int {
			const zend_mir_executable_value_ref &operation =
				mir.value_operation;
			const uint32_t opcode = operation.source_opcode;
			const bool arithmetic = opcode == ZEND_ADD
				|| opcode == ZEND_SUB || opcode == ZEND_MUL
				|| opcode == ZEND_DIV;
			const bool comparison = opcode == ZEND_IS_SMALLER
				|| opcode == ZEND_IS_SMALLER_OR_EQUAL
				|| opcode == ZEND_IS_EQUAL || opcode == ZEND_IS_NOT_EQUAL;
			/* Integer-only operations: two longs inline, anything else,
			 * a zero or -1 divisor and a shift count outside 0..63 use the
			 * helper. */
			const bool integer_op = opcode == ZEND_MOD
				|| opcode == ZEND_SL || opcode == ZEND_SR;
			/* A proven operation of two numbers cannot fail: integer
			 * overflow continues in double. A typed body emits it without a
			 * guarded diamond; in a Zend entry its cold edge is dead. */
			const bool proven =
				zend_tpde_numeric_binary_proven(adaptor->plan(), mir);
			const bool guarded = node.kind == Adaptor::InstKind::GuardedFast;
			if ((!arithmetic && !comparison && !integer_op)
					|| (!guarded && !(proven && adaptor->typed_body()))
					|| !mir.has_value_operation
					|| operation.opcode != ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
				return -1;
			}
			/*
			 * Register operands, such as the boxed result of a preceding
			 * numeric node, take this form only where the long-only register
			 * form cannot: MUL and DIV, or any result that can hold a double
			 * or a boolean.
			 */
			/* The plan keeps the result of two longs an exact long: an
			 * overflow fails the typed body, whose caller repeats the call. */
			if (adaptor->typed_body() && !guarded && arithmetic
					&& opcode != ZEND_DIV && node.has_result
					&& node.operands.size() == 2
					&& adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_I64) {
				ScratchReg left_value{this};
				ScratchReg right_value{this};
				auto left_reg = left_value.alloc_gp();
				auto right_reg = right_value.alloc_gp();
				auto load_long = [&](IRValueRef operand, AsmReg target) {
					if (adaptor->machine_kind(operand)
							== ZEND_TPDE_MACHINE_VALUE_I64) {
						auto [value_ref, value] = val_ref_single(operand);
						ASM(MOV64rr, target, value.load_to_reg());
						return true;
					}
					if (adaptor->machine_kind(operand)
							!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
						return false;
					}
					auto value = val_ref(operand);
					auto payload = value.part(0);
					ASM(MOV64rr, target, payload.load_to_reg());
					return true;
				};
				if (!load_long(node.operands[0], left_reg)
						|| !load_long(node.operands[1], right_reg)) {
					return 0;
				}
				switch (opcode) {
					case ZEND_ADD:
						ASM(ADD64rr, left_reg, right_reg);
						break;
					case ZEND_SUB:
						ASM(SUB64rr, left_reg, right_reg);
						break;
					default:
						ASM(IMUL64rr, left_reg, right_reg);
						break;
				}
				generate_raw_jump(Jump::jo, typed_failure_label());
				auto [result_ref_value, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64rr, result_reg, left_reg);
				result.set_modified();
				return 1;
			}
			/* A typed body passes both operands without a frame operand. */
			const bool register_operands = register_layout
				|| (adaptor->typed_body() && !guarded
					&& node.has_result && node.operands.size() == 2
					&& register_long_operand(node.operands[0])
					&& register_long_operand(node.operands[1]));
			if (register_operands) {
				const zend_tpde_machine_value_kind kind = node.has_result
					? adaptor->machine_kind(node.result)
					: ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
				if (!(opcode == ZEND_MUL || opcode == ZEND_DIV
						|| kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						|| (comparison
							&& kind == ZEND_TPDE_MACHINE_VALUE_BOOL))) {
					return -1;
				}
				for (uint32_t index = 0; index < 2; ++index) {
					const IRValueRef operand = node.operands[index];
					if (!(adaptor->machine_kind(operand)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& val_parts(operand).count() == 2)
							&& !(adaptor->machine_kind(operand)
									== ZEND_TPDE_MACHINE_VALUE_I64
								&& adaptor->exact_type(operand)
									== ZEND_MIR_SCALAR_TYPE_I64)) {
						return -1;
					}
				}
			} else {
				if (!frame_only_operands(node)) {
					return -1;
				}
			}
			struct FramedOperand {
				bool literal;
				int32_t offset;
				bool reg;
				uint32_t index;
			};
			auto framed_operand = [&](const zend_mir_source_operand_ref &operand,
					zend_mir_storage_id storage, FramedOperand *out) {
				uint64_t offset;
				out->reg = false;
				if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
					out->literal = true;
					offset = uint64_t{operand.index} * sizeof(zval);
				} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
							|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
						&& (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
							|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
							|| operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
						&& zend_mir_id_is_valid(storage)) {
					/* With frame-only operands a temporary is published in its
					 * slot; consuming a number needs no release. */
					out->literal = false;
					offset = (uint64_t{ZEND_CALL_FRAME_SLOT} + storage)
						* sizeof(zval);
				} else {
					return false;
				}
				if (offset > INT32_MAX - sizeof(zval)) {
					return false;
				}
				out->offset = static_cast<int32_t>(offset);
				return true;
			};
			FramedOperand left{false, 0, true, 0}, right{false, 0, true, 1};
			if ((!register_operands
						&& (!framed_operand(
								operation.op1, operation.op1_storage_id, &left)
							|| !framed_operand(
								operation.op2, operation.op2_storage_id,
								&right)))
					|| (operation.result.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operation.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| (operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
						&& operation.result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_VAR
						&& operation.result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_CV)
					|| (!register_operands
						&& !zend_mir_id_is_valid(
							operation.result_storage_id))) {
				return -1;
			}
			/* A typed body has no frame; its result lives in registers. */
			const bool result_slot =
				zend_mir_id_is_valid(operation.result_storage_id)
				&& !adaptor->typed_body();
			/* A CV result only replaces a non-refcounted value: OPcache
			 * contracts ASSIGN into it only then, and an aliased input was
			 * just checked to be numeric. A temporary never aliases input. */
			const uint64_t result_offset64 = result_slot
				? (uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
					* sizeof(zval)
				: 0;
			if (result_offset64 > INT32_MAX - sizeof(zval)
					|| (!register_operands
						&& operation.result.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
						&& ((!left.literal && operation.op1_storage_id
								== operation.result_storage_id)
							|| (!right.literal && operation.op2_storage_id
								== operation.result_storage_id)))) {
				return -1;
			}
			const int32_t result_offset =
				static_cast<int32_t>(result_offset64);
			const zend_tpde_machine_value_kind result_kind = node.has_result
				? adaptor->machine_kind(node.result)
				: ZEND_TPDE_MACHINE_VALUE_I64;
			if (node.has_result
					&& result_kind != ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& !(comparison
						&& result_kind == ZEND_TPDE_MACHINE_VALUE_BOOL)) {
				return -1;
			}
			const auto guarded_successors = guarded
				? adaptor->block_succs(IRBlockRef{node.control_block})
				: decltype(adaptor->block_succs(
					IRBlockRef{node.control_block})){};
			if (guarded
					&& (node.control_block == UINT32_MAX
						|| node.continuation_block == UINT32_MAX
						|| guarded_successors.size() < 2
						|| static_cast<uint32_t>(guarded_successors[0])
							!= node.continuation_block
						|| static_cast<uint32_t>(guarded_successors[1])
							!= node.argument_index)) {
				return 0;
			}

			/* Zend's type inference fixes most operand types in numeric
			 * code; test only the unknown ones. */
			uint8_t left_known = IS_UNDEF;
			uint8_t right_known = IS_UNDEF;
			if (!register_operands
					&& operation.source_position_id
						< adaptor->plan()->source_opcode_count) {
				const zend_tpde_source_opcode &source =
					adaptor->plan()->source_opcodes[
						operation.source_position_id];
				if (source.opcode == opcode) {
					left_known = source.op1_known_type;
					right_known = source.op2_known_type;
				}
			}
			if (proven) {
				if (left_known == IS_UNDEF) {
					left_known = ZEND_TPDE_KNOWN_NUMBER;
				}
				if (right_known == IS_UNDEF) {
					right_known = ZEND_TPDE_KNOWN_NUMBER;
				}
			}
			const bool known_long =
				left_known == IS_LONG && right_known == IS_LONG;
			const bool known_double = !register_operands
				&& ((left_known == IS_DOUBLE
						&& (right_known == IS_LONG
							|| right_known == IS_DOUBLE))
					|| (right_known == IS_DOUBLE
						&& left_known == IS_LONG));
			/*
			 * Double arithmetic other than division and every comparison of
			 * known numbers cannot fail: compute in SSE registers straight
			 * from the slots and store a double or boolean.
			 */
			if (known_double && opcode != ZEND_DIV && !integer_op
					&& (arithmetic
						|| !node.has_result
						|| result_kind == ZEND_TPDE_MACHINE_VALUE_BOOL
						|| result_kind
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
				auto [frame_ref, frame] =
					val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
				auto frame_scratch = frame_register(std::move(frame));
				auto frame_reg = frame_scratch.cur_reg();
				ScratchReg literals{this};
				ScratchReg value{this};
				ScratchReg left_double{this};
				ScratchReg right_double{this};
				auto value_reg = value.alloc_gp();
				auto left_fp = left_double.alloc(
					tpde::x64::PlatformConfig::FP_BANK);
				auto right_fp = right_double.alloc(
					tpde::x64::PlatformConfig::FP_BANK);
				AsmReg literals_reg = frame_reg;
				if (left.literal || right.literal) {
					literals_reg = literals.alloc_gp();
					ASM(MOV64rm, literals_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_execute_data, func))));
					ASM(MOV64rm, literals_reg,
						FE_MEM(literals_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_op_array, literals))));
				}
				auto load_double = [&](const FramedOperand &operand,
						uint8_t known, AsmReg fp_reg) {
					const AsmReg base =
						operand.literal ? literals_reg : frame_reg;
					if (known == IS_DOUBLE) {
						ASM(SSE_MOVSDrm, fp_reg,
							FE_MEM(base, 0, FE_NOREG, operand.offset));
					} else {
						ASM(MOV64rm, value_reg,
							FE_MEM(base, 0, FE_NOREG, operand.offset));
						ASM(SSE_CVTSI2SD64rr, fp_reg, value_reg);
					}
				};
				load_double(left, left_known, left_fp);
				load_double(right, right_known, right_fp);
				literals.reset();
				if (arithmetic) {
					switch (opcode) {
						case ZEND_ADD:
							ASM(SSE_ADDSDrr, left_fp, right_fp);
							break;
						case ZEND_SUB:
							ASM(SSE_SUBSDrr, left_fp, right_fp);
							break;
						default:
							ASM(SSE_MULSDrr, left_fp, right_fp);
							break;
					}
					if (result_slot) {
						ASM(SSE_MOVSDmr,
							FE_MEM(frame_reg, 0, FE_NOREG, result_offset),
							left_fp);
						ASM(MOV32mi,
							FE_MEM(frame_reg, 0, FE_NOREG, result_offset
								+ static_cast<int32_t>(
									offsetof(zval, u1.type_info))),
							IS_DOUBLE);
					}
					if (node.has_result) {
						auto fast_result = result_ref(node.result);
						auto payload = fast_result.part(0);
						auto type_info = fast_result.part(1);
						auto payload_reg = payload.alloc_reg();
						auto type_info_reg = type_info.alloc_reg();
						ASM(SSE_MOVQ_X2Grr, payload_reg, left_fp);
						ASM(MOV32ri, type_info_reg, IS_DOUBLE);
						payload.set_modified();
						type_info.set_modified();
					}
				} else {
					ScratchReg parity{this};
					auto parity_reg = parity.alloc_gp();
					switch (opcode) {
						case ZEND_IS_SMALLER:
							ASM(SSE_UCOMISDrr, right_fp, left_fp);
							generate_raw_set(Jump::ja, value_reg);
							break;
						case ZEND_IS_SMALLER_OR_EQUAL:
							ASM(SSE_UCOMISDrr, right_fp, left_fp);
							generate_raw_set(Jump::jae, value_reg);
							break;
						case ZEND_IS_EQUAL:
							ASM(SSE_UCOMISDrr, left_fp, right_fp);
							generate_raw_set(Jump::je, value_reg);
							generate_raw_set(Jump::jnp, parity_reg);
							ASM(AND32rr, value_reg, parity_reg);
							break;
						default:
							ASM(SSE_UCOMISDrr, left_fp, right_fp);
							generate_raw_set(Jump::jne, value_reg);
							generate_raw_set(Jump::jp, parity_reg);
							ASM(OR32rr, value_reg, parity_reg);
							break;
					}
					if (result_slot) {
						ASM(MOV32rr, parity_reg, value_reg);
						ASM(ADD32ri, parity_reg, IS_FALSE);
						ASM(MOV32mr,
							FE_MEM(frame_reg, 0, FE_NOREG, result_offset
								+ static_cast<int32_t>(
									offsetof(zval, u1.type_info))),
							parity_reg);
					}
					if (node.has_result) {
						if (result_kind
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
							auto fast_result = result_ref(node.result);
							auto payload = fast_result.part(0);
							auto type_info = fast_result.part(1);
							auto payload_reg = payload.alloc_reg();
							auto type_info_reg = type_info.alloc_reg();
							ASM(MOV64rr, payload_reg, value_reg);
							ASM(MOV32rr, type_info_reg, value_reg);
							ASM(ADD32ri, type_info_reg, IS_FALSE);
							payload.set_modified();
							type_info.set_modified();
						} else {
							auto [bool_ref, bool_result] =
								result_ref_single(node.result);
							auto bool_reg = bool_result.alloc_reg();
							ASM(MOV64rr, bool_reg, value_reg);
							bool_result.set_modified();
						}
					}
				}
				left_double.reset();
				right_double.reset();
				if (!guarded) {
					return 1;
				}
				/* Both operands are known doubles: the cold edge is never
				 * taken. */
				value.reset();
				generate_uncond_branch(guarded_successors[0]);
				return 1;
			}

			auto slow = text_writer.label_create();
			auto done = text_writer.label_create();
			auto mixed = text_writer.label_create();
			auto store = text_writer.label_create();
			/* IDIV takes rdx:rax and a variable shift its count in cl;
			 * reserve them before the operands are placed. */
			ScratchReg fixed_ax{this};
			ScratchReg fixed_dx{this};
			ScratchReg fixed_cx{this};
			const AsmReg ax_reg{AsmReg::AX};
			const AsmReg dx_reg{AsmReg::DX};
			const AsmReg cx_reg{AsmReg::CX};
			if (opcode == ZEND_DIV || opcode == ZEND_MOD) {
				fixed_ax.alloc_specific(ax_reg);
				fixed_dx.alloc_specific(dx_reg);
			} else if (opcode == ZEND_SL || opcode == ZEND_SR) {
				fixed_cx.alloc_specific(cx_reg);
			}
			ScratchReg frame_scratch{this};
			AsmReg frame_reg = AsmReg::make_invalid();
			if (!register_operands || result_slot) {
				auto [frame_ref, frame] =
					val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
				frame_scratch = std::move(frame).into_scratch();
				frame_reg = frame_scratch.cur_reg();
			}
			ScratchReg literals{this};
			ScratchReg left_type{this};
			ScratchReg right_type{this};
			ScratchReg left_value{this};
			ScratchReg right_value{this};
			ScratchReg left_double{this};
			ScratchReg right_double{this};
			auto left_type_reg = left_type.alloc_gp();
			auto right_type_reg = right_type.alloc_gp();
			auto left_reg = left_value.alloc_gp();
			auto right_reg = right_value.alloc_gp();
			/* Register pressure: the right type becomes the result type once
			 * the operands are classified, the left type the decision. */
			const AsmReg result_type_reg = right_type_reg;
			const AsmReg decision_reg = left_type_reg;
			auto left_fp = left_double.alloc(
				tpde::x64::PlatformConfig::FP_BANK);
			auto right_fp = right_double.alloc(
				tpde::x64::PlatformConfig::FP_BANK);
			AsmReg literals_reg = frame_reg;
			if (left.literal || right.literal) {
				literals_reg = literals.alloc_gp();
				ASM(MOV64rm, literals_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, func))));
				ASM(MOV64rm, literals_reg,
					FE_MEM(literals_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_op_array, literals))));
			}
			auto load = [&](const FramedOperand &operand, AsmReg type_reg,
					AsmReg value_reg, uint8_t known,
					zend_mir_storage_id storage) {
				if (operand.reg) {
					const IRValueRef source = node.operands[operand.index];
					if (adaptor->machine_kind(source)
							== ZEND_TPDE_MACHINE_VALUE_I64) {
						auto [value_ref, value] = val_ref_single(source);
						ASM(MOV64rr, value_reg, value.load_to_reg());
						ASM(MOV32ri, type_reg, IS_LONG);
					} else {
						auto value = val_ref(source);
						auto payload = value.part(0);
						auto type_info = value.part(1);
						ASM(MOV64rr, value_reg, payload.load_to_reg());
						ASM(MOVZXr32r8, type_reg, type_info.load_to_reg());
					}
					return;
				}
				const AsmReg base = operand.literal ? literals_reg : frame_reg;
				ASM(MOVZXr32m8, type_reg,
					FE_MEM(base, 0, FE_NOREG, operand.offset
						+ static_cast<int32_t>(offsetof(zval, u1.type_info))));
				ASM(MOV64rm, value_reg,
					FE_MEM(base, 0, FE_NOREG, operand.offset));
				/* A CV bound by global, static or & holds a reference; read
				 * through it like the VM. A CV result written back into the
				 * same slot keeps the helper, which updates the referent. */
				if (!operand.literal
						&& (known == IS_UNDEF
							|| known == ZEND_TPDE_KNOWN_BOOL)
						&& !(operation.result.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV
							&& storage == operation.result_storage_id)) {
					auto plain = text_writer.label_create();
					ASM(CMP32ri, type_reg, IS_REFERENCE);
					generate_raw_jump(Jump::jne, plain);
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_reference, val)
								+ offsetof(zval, u1.type_info))));
					ASM(MOV64rm, value_reg,
						FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_reference, val))));
					label_place(plain);
				}
			};
			load(left, left_type_reg, left_reg, left_known,
				operation.op1_storage_id);
			load(right, right_type_reg, right_reg, right_known,
				operation.op2_storage_id);

			/* long op long; a known double goes straight to the double path */
			if (!known_double) {
				if (!known_long) {
					ASM(CMP32ri, left_type_reg, IS_LONG);
					generate_raw_jump(Jump::jne, mixed);
					ASM(CMP32ri, right_type_reg, IS_LONG);
					generate_raw_jump(Jump::jne, mixed);
				}
				/* Proven arithmetic keeps the left operand in the dead right type
				 * register and recomputes an overflowing result in double. */
				auto overflow = text_writer.label_create();
				const bool overflow_edge = proven && arithmetic;
				if (overflow_edge) {
					ASM(MOV64rr, result_type_reg, left_reg);
				}
				switch (opcode) {
					case ZEND_ADD:
						ASM(ADD64rr, left_reg, right_reg);
						generate_raw_jump(Jump::jo,
							overflow_edge ? overflow : slow);
						ASM(MOV32ri, result_type_reg, IS_LONG);
						break;
					case ZEND_SUB:
						ASM(SUB64rr, left_reg, right_reg);
						generate_raw_jump(Jump::jo,
							overflow_edge ? overflow : slow);
						ASM(MOV32ri, result_type_reg, IS_LONG);
						break;
					case ZEND_MUL:
						ASM(IMUL64rr, left_reg, right_reg);
						generate_raw_jump(Jump::jo,
							overflow_edge ? overflow : slow);
						ASM(MOV32ri, result_type_reg, IS_LONG);
						break;
					case ZEND_DIV:
						/* An exact quotient stays a long; otherwise divide both
						 * operands in double precision below. */
						ASM(TEST64rr, right_reg, right_reg);
						generate_raw_jump(Jump::je, slow);
						ASM(CMP64ri, right_reg, -1);
						generate_raw_jump(Jump::je, slow);
						ASM(MOV64rr, ax_reg, left_reg);
						ASM(CQO);
						ASM(IDIV64r, right_reg);
						ASM(TEST64rr, dx_reg, dx_reg);
						generate_raw_jump(Jump::jne, mixed);
						ASM(MOV64rr, left_reg, ax_reg);
						ASM(MOV32ri, result_type_reg, IS_LONG);
						break;
					case ZEND_MOD:
						ASM(TEST64rr, right_reg, right_reg);
						generate_raw_jump(Jump::je, slow);
						ASM(CMP64ri, right_reg, -1);
						generate_raw_jump(Jump::je, slow);
						ASM(MOV64rr, ax_reg, left_reg);
						ASM(CQO);
						ASM(IDIV64r, right_reg);
						ASM(MOV64rr, left_reg, dx_reg);
						ASM(MOV32ri, result_type_reg, IS_LONG);
						break;
					case ZEND_SL:
					case ZEND_SR:
						/* Unsigned: a negative count is out of range too. */
						ASM(CMP64ri, right_reg, 63);
						generate_raw_jump(Jump::ja, slow);
						ASM(MOV64rr, cx_reg, right_reg);
						if (opcode == ZEND_SL) {
							ASM(SHL64rr, left_reg, cx_reg);
						} else {
							ASM(SAR64rr, left_reg, cx_reg);
						}
						ASM(MOV32ri, result_type_reg, IS_LONG);
						break;
					default: {
						const Jump condition =
							opcode == ZEND_IS_SMALLER ? Jump::jl
							: opcode == ZEND_IS_SMALLER_OR_EQUAL ? Jump::jle
							: opcode == ZEND_IS_EQUAL ? Jump::je
							: Jump::jne;
						ASM(CMP64rr, left_reg, right_reg);
						generate_raw_set(condition, left_reg);
						ASM(MOV32rr, result_type_reg, left_reg);
						ASM(ADD32ri, result_type_reg, IS_FALSE);
						break;
					}
				}
				generate_raw_jump(Jump::jmp, store);
				if (overflow_edge) {
					label_place(overflow);
					ASM(SSE_CVTSI2SD64rr, left_fp, result_type_reg);
					ASM(SSE_CVTSI2SD64rr, right_fp, right_reg);
					switch (opcode) {
						case ZEND_ADD:
							ASM(SSE_ADDSDrr, left_fp, right_fp);
							break;
						case ZEND_SUB:
							ASM(SSE_SUBSDrr, left_fp, right_fp);
							break;
						default:
							ASM(SSE_MULSDrr, left_fp, right_fp);
							break;
					}
					ASM(SSE_MOVQ_X2Grr, left_reg, left_fp);
					ASM(MOV32ri, result_type_reg, IS_DOUBLE);
					generate_raw_jump(Jump::jmp, store);
				}
			}

			/* long or double, at least one double */
			label_place(mixed);
			if (integer_op) {
				generate_raw_jump(Jump::jmp, slow);
			}
			auto to_double = [&](AsmReg type_reg, AsmReg value_reg,
					AsmReg fp_reg, uint8_t known) {
				if (known == IS_DOUBLE) {
					ASM(SSE_MOVQ_G2Xrr, fp_reg, value_reg);
					return;
				}
				if (known == IS_LONG) {
					ASM(SSE_CVTSI2SD64rr, fp_reg, value_reg);
					return;
				}
				auto is_long = text_writer.label_create();
				auto converted = text_writer.label_create();
				ASM(CMP32ri, type_reg, IS_LONG);
				generate_raw_jump(Jump::je, is_long);
				if (known != ZEND_TPDE_KNOWN_NUMBER) {
					ASM(CMP32ri, type_reg, IS_DOUBLE);
					generate_raw_jump(Jump::jne, slow);
				}
				ASM(SSE_MOVQ_G2Xrr, fp_reg, value_reg);
				generate_raw_jump(Jump::jmp, converted);
				label_place(is_long);
				ASM(SSE_CVTSI2SD64rr, fp_reg, value_reg);
				label_place(converted);
			};
			to_double(left_type_reg, left_reg, left_fp, left_known);
			to_double(right_type_reg, right_reg, right_fp, right_known);
			switch (opcode) {
				case ZEND_ADD:
					ASM(SSE_ADDSDrr, left_fp, right_fp);
					break;
				case ZEND_SUB:
					ASM(SSE_SUBSDrr, left_fp, right_fp);
					break;
				case ZEND_MUL:
					ASM(SSE_MULSDrr, left_fp, right_fp);
					break;
				case ZEND_DIV: {
					/* A zero or NaN divisor sets ZF; the helper throws. */
					ScratchReg zero{this};
					auto zero_fp = zero.alloc(
						tpde::x64::PlatformConfig::FP_BANK);
					ASM(SSE_XORPDrr, zero_fp, zero_fp);
					ASM(SSE_UCOMISDrr, right_fp, zero_fp);
					generate_raw_jump(Jump::je, slow);
					ASM(SSE_DIVSDrr, left_fp, right_fp);
					break;
				}
				default:
					break;
			}
			if (arithmetic) {
				ASM(SSE_MOVQ_X2Grr, left_reg, left_fp);
				ASM(MOV32ri, result_type_reg, IS_DOUBLE);
			} else {
				switch (opcode) {
					case ZEND_IS_SMALLER:
						ASM(SSE_UCOMISDrr, right_fp, left_fp);
						generate_raw_set(Jump::ja, left_reg);
						break;
					case ZEND_IS_SMALLER_OR_EQUAL:
						ASM(SSE_UCOMISDrr, right_fp, left_fp);
						generate_raw_set(Jump::jae, left_reg);
						break;
					case ZEND_IS_EQUAL:
						ASM(SSE_UCOMISDrr, left_fp, right_fp);
						generate_raw_set(Jump::je, left_reg);
						generate_raw_set(Jump::jnp, right_reg);
						ASM(AND32rr, left_reg, right_reg);
						break;
					default:
						ASM(SSE_UCOMISDrr, left_fp, right_fp);
						generate_raw_set(Jump::jne, left_reg);
						generate_raw_set(Jump::jp, right_reg);
						ASM(OR32rr, left_reg, right_reg);
						break;
				}
				ASM(MOV32rr, result_type_reg, left_reg);
				ASM(ADD32ri, result_type_reg, IS_FALSE);
			}

			label_place(store);
			literals.reset();
			left_double.reset();
			right_double.reset();
			if (result_slot) {
				ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG, result_offset),
					left_reg);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG, result_offset
						+ static_cast<int32_t>(offsetof(zval, u1.type_info))),
					result_type_reg);
			}
			if (node.has_result) {
				if (result_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto fast_result = result_ref(node.result);
					auto payload = fast_result.part(0);
					auto type_info = fast_result.part(1);
					auto payload_reg = payload.alloc_reg();
					auto type_info_reg = type_info.alloc_reg();
					ASM(MOV64rr, payload_reg, left_reg);
					ASM(MOV32rr, type_info_reg, result_type_reg);
					payload.set_modified();
					type_info.set_modified();
				} else {
					auto [bool_ref, bool_result] =
						result_ref_single(node.result);
					auto bool_reg = bool_result.alloc_reg();
					ASM(MOV64rr, bool_reg, left_reg);
					bool_result.set_modified();
				}
			}
			if (!guarded) {
				right_value.reset();
				return 1;
			}
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				const auto direct_successors =
					adaptor->block_succs(IRBlockRef{node.control_block});
				if (guarded_exit_can_jump_directly(
						direct_successors[1], direct_successors[0])) {
					generate_guarded_direct_exit(
						slow, direct_successors[1], direct_successors[0]);
					return 1;
				}
			}
			ASM(MOV32ri, decision_reg, 0);
			generate_raw_jump(Jump::jmp, done);
			label_place(slow);
			ASM(MOV32ri, decision_reg, 1);
			label_place(done);
			right_value.reset();
			generate_guarded_decision_branch(
				std::move(left_type), guarded_successors[1],
				guarded_successors[0]);
			return 1;
		};
		{
			const int framed = numeric_framed_binary();
			if (framed >= 0) {
				return framed != 0;
			}
		}
		if (!register_layout
				|| (!framed_layout && !register_result_layout)
				|| (framed_layout
					&& (layout.left.offset > INT32_MAX - 8
						|| layout.right.offset > INT32_MAX - 8
						|| layout.result_offset > INT32_MAX - 8))
				|| !node.has_result
				|| node.operands.size() != 3
				|| node.operands[2]
					!= IRValueRef{Adaptor::FRAME_VALUE}) {
			return branch_to_guarded_cold();
		}
		layout.source_opcode = mir.value_operation.source_opcode;
		const bool supported_source_opcode =
			layout.source_opcode == ZEND_ADD
			|| layout.source_opcode == ZEND_SUB
			|| layout.source_opcode == ZEND_BW_OR
			|| layout.source_opcode == ZEND_BW_AND
			|| layout.source_opcode == ZEND_BW_XOR
			|| layout.source_opcode == ZEND_SPACESHIP
			|| layout.source_opcode == ZEND_IS_IDENTICAL
			|| layout.source_opcode == ZEND_IS_NOT_IDENTICAL
			|| layout.source_opcode == ZEND_IS_EQUAL
			|| layout.source_opcode == ZEND_IS_NOT_EQUAL
			|| layout.source_opcode == ZEND_IS_SMALLER
			|| layout.source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
		if (!supported_source_opcode) {
			return branch_to_guarded_cold();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			return false;
		}
		const auto guarded_successors = guarded_successors_of(node);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg result_value{this};
		ScratchReg decision{this};
		auto result_reg = result_value.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		auto load_register_long =
			[&](IRValueRef operand, AsmReg target) {
				if (adaptor->machine_kind(operand)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto value = val_ref(operand);
					const ValueParts parts = val_parts(operand);
					if (parts.count() != 2) {
						return false;
					}
					auto payload = value.part(0);
					auto type_info = value.part(1);
					auto payload_reg = payload.load_to_reg();
					auto type_info_reg = type_info.load_to_reg();
					ASM(CMP32ri, type_info_reg, IS_LONG);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rr, target, payload_reg);
					return true;
				}
				auto [value_ref, value] = val_ref_single(operand);
				ASM(MOV64rr, target, value.load_to_reg());
				return true;
			};
		if (!load_register_long(node.operands[0], result_reg)) {
			return false;
		}
		const bool boolean_result =
			layout.source_opcode == ZEND_IS_IDENTICAL
			|| layout.source_opcode == ZEND_IS_NOT_IDENTICAL
			|| layout.source_opcode == ZEND_IS_EQUAL
			|| layout.source_opcode == ZEND_IS_NOT_EQUAL
			|| layout.source_opcode == ZEND_IS_SMALLER
			|| layout.source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
		{
			ScratchReg right_value{this};
			auto right_reg = right_value.alloc_gp();
			if (!load_register_long(node.operands[1], right_reg)) {
				return false;
			}
				switch (layout.source_opcode) {
				case ZEND_ADD:
					ASM(ADD64rr, result_reg, right_reg);
					generate_raw_jump(Jump::jo, slow);
					break;
				case ZEND_SUB:
					ASM(SUB64rr, result_reg, right_reg);
					generate_raw_jump(Jump::jo, slow);
					break;
				case ZEND_BW_OR:
					ASM(OR64rr, result_reg, right_reg);
					break;
				case ZEND_BW_AND:
					ASM(AND64rr, result_reg, right_reg);
					break;
				case ZEND_BW_XOR:
					ASM(XOR64rr, result_reg, right_reg);
					break;
				case ZEND_SPACESHIP: {
					ScratchReg less{this};
					auto less_reg = less.alloc_gp();
					ASM(CMP64rr, result_reg, right_reg);
					generate_raw_set(Jump::jl, less_reg);
					generate_raw_set(Jump::jg, result_reg);
					ASM(SUB64rr, result_reg, less_reg);
					break;
				}
				case ZEND_IS_IDENTICAL:
				case ZEND_IS_EQUAL:
					ASM(CMP64rr, result_reg, right_reg);
					generate_raw_set(Jump::je, result_reg);
					break;
				case ZEND_IS_NOT_IDENTICAL:
				case ZEND_IS_NOT_EQUAL:
					ASM(CMP64rr, result_reg, right_reg);
					generate_raw_set(Jump::jne, result_reg);
					break;
				case ZEND_IS_SMALLER:
					ASM(CMP64rr, result_reg, right_reg);
					generate_raw_set(Jump::jl, result_reg);
					break;
				case ZEND_IS_SMALLER_OR_EQUAL:
					ASM(CMP64rr, result_reg, right_reg);
					generate_raw_set(Jump::jle, result_reg);
					break;
				default:
					return false;
			}
		}
		if (framed_layout || register_result_layout) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				result_reg);
			if (boolean_result) {
				ScratchReg result_type{this};
				auto result_type_reg = result_type.alloc_gp();
				ASM(MOV64rr, result_type_reg, result_reg);
				ASM(ADD64ri, result_type_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							layout.result_offset
								+ offsetof(zval, u1.type_info))),
					result_type_reg);
			} else {
				ASM(MOV32mi,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							layout.result_offset
								+ offsetof(zval, u1.type_info))),
					IS_LONG);
			}
		}
		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto fast_result = result_ref(node.result);
			auto payload = fast_result.part(0);
			auto type_info = fast_result.part(1);
			auto payload_reg = payload.alloc_reg();
			auto type_info_reg = type_info.alloc_reg();
			ASM(MOV64rr, payload_reg, result_reg);
			if (boolean_result) {
				ASM(MOV32rr, type_info_reg, result_reg);
				ASM(ADD32ri, type_info_reg, IS_FALSE);
			} else {
				ASM(MOV32ri, type_info_reg, IS_LONG);
			}
			payload.set_modified();
			type_info.set_modified();
		} else {
			auto [fast_result_ref, fast_result] =
				result_ref_single(node.result);
			auto fast_result_reg = fast_result.alloc_reg();
			ASM(MOV64rr, fast_result_reg, result_reg);
			fast_result.set_modified();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto direct_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (guarded_exit_can_jump_directly(
					direct_successors[1], direct_successors[0])) {
				generate_guarded_direct_exit(
					slow, direct_successors[1], direct_successors[0]);
				return true;
			}
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		result_value.reset();
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	auto long_assign_op = [&]() {
		zend_tpde_long_assign_op layout;

		if (!zend_tpde_long_assign_op_at(mir, &layout)
				|| layout.left_offset > INT32_MAX - 8
				|| layout.right.offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
			return branch_to_guarded_cold();
		}
		if (layout.has_result != node.has_result
				&& !(node.mutation_result
					&& !layout.has_result && node.has_result)) {
			return branch_to_guarded_cold();
		}
		/* ASSIGN_OP is always guarded (see freeze_machine_control_flow()),
		 * also in typed bodies. */
		ZEND_ASSERT(node.kind == Adaptor::InstKind::GuardedFast);
		const auto guarded_successors = guarded_successors_of(node);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg left{this};
		ScratchReg right{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto left_reg = left.alloc_gp();
		auto right_reg = right.alloc_gp();
		auto decision_reg = decision.alloc_gp();

			if (node.assign_op_left_operand_index < node.operands.size()
					&& adaptor->representation(
						node.operands[node.assign_op_left_operand_index])
						== ZEND_MIR_REPRESENTATION_I64
					&& adaptor->exact_type(
						node.operands[node.assign_op_left_operand_index])
						== ZEND_MIR_SCALAR_TYPE_I64
					&& adaptor->machine_kind(
						node.operands[node.assign_op_left_operand_index])
						== ZEND_TPDE_MACHINE_VALUE_I64) {
				auto [left_ref, left_value] =
					val_ref_single(
						node.operands[node.assign_op_left_operand_index]);
				ASM(MOV64rr, left_reg, left_value.load_to_reg());
			} else if (node.assign_op_left_operand_index < node.operands.size()
					&& adaptor->machine_kind(
						node.operands[node.assign_op_left_operand_index])
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto left_value = val_ref(
					node.operands[node.assign_op_left_operand_index]);
			auto payload = left_value.part(0);
			auto type_info = left_value.part(1);
			ASM(MOV64rr, left_reg, payload.load_to_reg());
			ASM(CMP8ri, type_info.load_to_reg(), IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		} else {
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.left_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, left_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.left_offset)));
		}

			if (node.assign_op_right_operand_index < node.operands.size()
					&& adaptor->representation(
						node.operands[node.assign_op_right_operand_index])
						== ZEND_MIR_REPRESENTATION_I64
					&& adaptor->exact_type(
						node.operands[node.assign_op_right_operand_index])
						== ZEND_MIR_SCALAR_TYPE_I64
					&& adaptor->machine_kind(
						node.operands[node.assign_op_right_operand_index])
						== ZEND_TPDE_MACHINE_VALUE_I64) {
				auto [right_ref, right_value] =
					val_ref_single(
						node.operands[node.assign_op_right_operand_index]);
				ASM(MOV64rr, right_reg, right_value.load_to_reg());
			} else if (node.assign_op_right_operand_index < node.operands.size()
					&& adaptor->machine_kind(
						node.operands[node.assign_op_right_operand_index])
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto right_value = val_ref(
					node.operands[node.assign_op_right_operand_index]);
			auto payload = right_value.part(0);
			auto type_info = right_value.part(1);
			auto payload_reg = payload.load_to_reg();
			auto type_info_reg = type_info.load_to_reg();
			ASM(MOV64rr, right_reg, payload_reg);
			ASM(CMP8ri, type_info_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		} else if (layout.right.literal) {
			if (node.machine_reference_operand_index
						>= node.operands.size()) {
				return branch_to_guarded_cold();
			}
			auto [literal_ref, literal] = val_ref_single(
				node.operands[node.machine_reference_operand_index]);
			auto literal_reg = literal.load_to_reg();
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(literal_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, right_reg,
				FE_MEM(literal_reg, 0, FE_NOREG, 0));
		} else {
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.right.offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, right_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.right.offset)));
		}

		if (layout.has_result) {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
		}

		switch (layout.source_opcode) {
			case ZEND_ADD:
				ASM(ADD64rr, left_reg, right_reg);
				generate_raw_jump(Jump::jo, slow);
				break;
			case ZEND_SUB:
				ASM(SUB64rr, left_reg, right_reg);
				generate_raw_jump(Jump::jo, slow);
				break;
			case ZEND_BW_OR:
				ASM(OR64rr, left_reg, right_reg);
				break;
			case ZEND_BW_AND:
				ASM(AND64rr, left_reg, right_reg);
				break;
			case ZEND_BW_XOR:
				ASM(XOR64rr, left_reg, right_reg);
				break;
			default:
				return false;
		}
		if (node.mutation_result) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64rr, result_reg, left_reg);
				result.set_modified();
			} else if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				const ValueParts parts = val_parts(node.result);
				for (uint32_t part = 0; part < parts.count(); ++part) {
					auto value = result.part(part);
					auto value_reg = value.alloc_reg();
					const zend_tpde_machine_part_role role =
						parts.representation.parts[part].semantic_role;
					if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
						ASM(MOV64rr, value_reg, left_reg);
					} else if (role
							== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
						ASM(MOV32ri, value_reg, IS_LONG);
					} else {
						return false;
					}
					value.set_modified();
				}
			} else {
				return false;
			}
		}
		if (!(node.mutation_result && mir.mutation_lazy_scalar)) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.left_offset)),
				left_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.left_offset + offsetof(zval, u1.type_info))),
				IS_LONG);
		}
		if (layout.has_result) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(MOV64rr, result_reg, left_reg);
			result.set_modified();
		}
		if (layout.consume_right) {
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.right.offset
						+ offsetof(zval, u1.type_info))),
				IS_UNDEF);
		}
		if (guarded_exit_can_jump_directly(
				guarded_successors[1], guarded_successors[0])) {
			generate_guarded_direct_exit(
				slow, guarded_successors[1], guarded_successors[0]);
			return true;
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		left.reset();
		right.reset();
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		generate_guarded_decision_branch(
			std::move(decision), guarded_successors[1],
			guarded_successors[0]);
		return true;
	};
	auto long_incdec = [&]() {
		zend_tpde_long_incdec layout;

		if (!zend_tpde_long_incdec_at(mir, &layout, true)
				|| layout.operand_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8
				|| (layout.indirect
					&& (node.mutation_result || mir.mutation_lazy_scalar
						|| (!node.operands.empty()
							&& node.operands[0]
								!= IRValueRef{Adaptor::FRAME_VALUE})))) {
			return branch_to_guarded_cold();
		}
		/* A result without a machine value is published to its slot. */
		if (layout.has_result != node.has_result
				&& !(node.mutation_result
					&& !layout.has_result && node.has_result)
				&& !(layout.has_result && !node.has_result)) {
			return branch_to_guarded_cold();
		}
		/* INCDEC is always guarded (see freeze_machine_control_flow()),
		 * also in typed bodies. */
		ZEND_ASSERT(node.kind == Adaptor::InstKind::GuardedFast);
		const bool machine_result = node.has_result;
		const auto guarded_successors = guarded_successors_of(node);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg limit{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		/* Address of the updated integer when it is not the CV slot itself. */
		ScratchReg target{this};
		AsmReg target_reg{};

		if (!node.operands.empty()
				&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				&& adaptor->representation(node.operands[0])
					== ZEND_MIR_REPRESENTATION_I64
				&& adaptor->exact_type(node.operands[0])
					== ZEND_MIR_SCALAR_TYPE_I64
				&& adaptor->machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_I64) {
			auto [operand_ref, operand] =
				val_ref_single(node.operands[0]);
			ASM(MOV64rr, value_reg, operand.load_to_reg());
		} else if (!node.operands.empty()
				&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				&& adaptor->machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto operand = val_ref(node.operands[0]);
			auto payload = operand.part(0);
			auto type_info = operand.part(1);
			ASM(MOV64rr, value_reg, payload.load_to_reg());
			ASM(CMP8ri, type_info.load_to_reg(), IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		} else if (layout.indirect) {
			/*
			 * ++/-- through a write fetch (++$a[$k]): the VAR holds the
			 * INDIRECT to the element. An integer or an untyped reference's
			 * integer is updated in place; the helper does the rest.
			 */
			auto loaded = text_writer.label_create();
			target_reg = target.alloc_gp();
			ASM(CMP8mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.operand_offset
							+ offsetof(zval, u1.type_info))),
				IS_INDIRECT);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, target_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::je, loaded);
			ASM(CMP32ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, target_reg, FE_MEM(target_reg, 0, FE_NOREG, 0));
			ASM(CMP64mi,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_reference, sources.ptr))),
				0);
			generate_raw_jump(Jump::jne, slow);
			ASM(ADD64ri, target_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
			ASM(CMP8mi,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
				IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			label_place(loaded);
			ASM(MOV64rm, value_reg, FE_MEM(target_reg, 0, FE_NOREG, 0));
		} else if (!(node.mutation_result && mir.mutation_lazy_scalar)) {
			/*
			 * The CV may hold a reference, as for an int &$value parameter.
			 * Update an untyped reference's integer in place; typed
			 * references need the helper's type checks.
			 */
			auto direct = text_writer.label_create();
			auto loaded = text_writer.label_create();
			target_reg = target.alloc_gp();
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.operand_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::je, direct);
			ASM(CMP32ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, target_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(CMP64mi,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_reference, sources.ptr))),
				0);
			generate_raw_jump(Jump::jne, slow);
			ASM(ADD64ri, target_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(target_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			generate_raw_jump(Jump::jmp, loaded);
			label_place(direct);
			ASM(LEA64rm, target_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			label_place(loaded);
			ASM(MOV64rm, value_reg, FE_MEM(target_reg, 0, FE_NOREG, 0));
		} else {
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.operand_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
		}
		ASM(MOV64ri, limit_reg,
			layout.increment ? ZEND_LONG_MAX : ZEND_LONG_MIN);
		ASM(CMP64rr, value_reg, limit_reg);
		generate_raw_jump(Jump::je, slow);

		if (layout.has_result) {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
			if (layout.post) {
				if (machine_result) {
					auto [result_ref, result] =
						result_ref_single(node.result);
					auto result_reg = result.alloc_reg();
					ASM(MOV64rr, result_reg, value_reg);
					result.set_modified();
				} else {
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.result_offset)),
						value_reg);
				}
			}
		}
		ASM(ADD64ri, value_reg, layout.increment ? 1 : -1);
		if (node.mutation_result) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64rr, result_reg, value_reg);
				result.set_modified();
			} else if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				const ValueParts parts = val_parts(node.result);
				for (uint32_t part = 0; part < parts.count(); ++part) {
					auto result_part = result.part(part);
					auto result_reg = result_part.alloc_reg();
					const zend_tpde_machine_part_role role =
						parts.representation.parts[part].semantic_role;
					if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
						ASM(MOV64rr, result_reg, value_reg);
					} else if (role
							== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
						ASM(MOV32ri, result_reg, IS_LONG);
					} else {
						return false;
					}
					result_part.set_modified();
				}
			} else {
				return false;
			}
		}
		if (target.has_reg()) {
			ASM(MOV64mr, FE_MEM(target_reg, 0, FE_NOREG, 0), value_reg);
		} else if (!(node.mutation_result && mir.mutation_lazy_scalar)) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)),
				value_reg);
		}
		if (layout.has_result) {
			if (!layout.post) {
				if (machine_result) {
					auto [result_ref, result] =
						result_ref_single(node.result);
					auto result_reg = result.alloc_reg();
					ASM(MOV64rr, result_reg, value_reg);
					result.set_modified();
				} else {
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.result_offset)),
						value_reg);
				}
			}
			if (!machine_result) {
				ASM(MOV32ri, type_reg, IS_LONG);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							layout.result_offset
								+ offsetof(zval, u1.type_info))),
					type_reg);
			}
		}
		type.reset();
		value.reset();
		limit.reset();
		target.reset();
		frame_scratch.reset();
		if (guarded_exit_can_jump_directly(
				guarded_successors[1], guarded_successors[0])) {
			decision.reset();
			generate_guarded_direct_exit(
				slow, guarded_successors[1], guarded_successors[0]);
			return true;
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		value.reset();
		limit.reset();
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		generate_guarded_decision_branch(
			std::move(decision), guarded_successors[1],
			guarded_successors[0]);
		return true;
	};
	auto slot_isset_empty = [&]() {
		zend_tpde_slot_isset_empty layout;

		if (!zend_tpde_slot_isset_empty_at(mir, &layout)
				|| layout.operand_offset
					> static_cast<uint32_t>(INT32_MAX
						- offsetof(zval, u1.type_info))
				|| layout.result_offset
					> static_cast<uint32_t>(INT32_MAX
						- offsetof(zval, u1.type_info))) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result
				&& node.kind != Adaptor::InstKind::GuardedFast) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors = guarded_successors_of(node);
		}
		if (!node.has_result
				&& node.kind == Adaptor::InstKind::GuardedFast
				&& (node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
					|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
					|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I64)) {
			auto [frame_ref, frame] =
				val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
			auto frame_scratch = frame_register(std::move(frame));
			auto frame_reg = frame_scratch.cur_reg();
			ScratchReg result{this};
			ScratchReg type{this};
			auto result_reg = result.alloc_gp();
			auto type_reg = type.alloc_gp();
			if (!layout.is_empty) {
				ASM(MOV32ri, result_reg,
					node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL ? 0 : 1);
			} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				ASM(MOV32ri, result_reg, 1);
			} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				ASM(MOV32rm, type_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(layout.operand_offset)));
				ASM(TEST32rr, type_reg, type_reg);
				generate_raw_set(Jump::je, result_reg);
			} else {
				ASM(MOV64rm, type_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(layout.operand_offset)));
				ASM(TEST64rr, type_reg, type_reg);
				generate_raw_set(Jump::je, result_reg);
			}
			store_boolean_zval(frame_reg,
				static_cast<int32_t>(layout.result_offset), result_reg,
				type_reg);
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_branch_to_block(
				Jump::jmp, successors[0], false, false);
			return true;
		}
		if (node.has_result) {
			const bool invert_result =
				(mir.machine_control_flow_flags
					& ZEND_TPDE_MACHINE_CONTROL_FLOW_INVERT_RESULT) != 0;
			const bool has_scalar_operand = !node.operands.empty()
				&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE};
			const IRValueRef operand = has_scalar_operand
				? node.operands[0] : IRValueRef{Adaptor::FRAME_VALUE};
			const zend_mir_scalar_type_mask type = has_scalar_operand
				? adaptor->exact_type(operand) : node.exact_type;
			if (type == ZEND_MIR_SCALAR_TYPE_NULL
					|| type == ZEND_MIR_SCALAR_TYPE_I1
					|| type == ZEND_MIR_SCALAR_TYPE_I64) {
				auto [result_ref, result] = result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				if (!layout.is_empty) {
					ASM(MOV32ri, result_reg,
						type == ZEND_MIR_SCALAR_TYPE_NULL ? 0 : 1);
				} else if (type == ZEND_MIR_SCALAR_TYPE_NULL) {
					ASM(MOV32ri, result_reg, 1);
				} else if (has_scalar_operand) {
					auto [operand_ref, value] = val_ref_single(operand);
					auto value_reg = value.load_to_reg();
					if (type == ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(TEST32rr, value_reg, value_reg);
					} else {
						ASM(TEST64rr, value_reg, value_reg);
					}
					generate_raw_set(
						invert_result ? Jump::jne : Jump::je, result_reg);
				} else {
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = frame_register(std::move(frame));
					ScratchReg value{this};
					auto value_reg = value.alloc_gp();
					if (type == ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(MOV32rm, value_reg,
							FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
								static_cast<int32_t>(layout.operand_offset)));
						ASM(TEST32rr, value_reg, value_reg);
					} else {
						ASM(MOV64rm, value_reg,
							FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
								static_cast<int32_t>(layout.operand_offset)));
						ASM(TEST64rr, value_reg, value_reg);
					}
					generate_raw_set(
						invert_result ? Jump::jne : Jump::je, result_reg);
				}
				result.set_modified();
				if (node.kind == Adaptor::InstKind::GuardedFast) {
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = frame_register(std::move(frame));
					ScratchReg result_type{this};
					store_boolean_zval(frame_scratch.cur_reg(),
						static_cast<int32_t>(layout.result_offset), result_reg,
						result_type.alloc_gp());
					const auto successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					generate_branch_to_block(
						Jump::jmp, successors[0], false, false);
				}
				return true;
			}
		}
		auto slow = text_writer.label_create();
		auto truthy = text_writer.label_create();
		auto falsey = text_writer.label_create();
		auto store = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		if (node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
			generate_raw_jump(Jump::jmp,
				layout.is_empty ? truthy : falsey);
		} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I64) {
			if (!layout.is_empty) {
				generate_raw_jump(Jump::jmp, truthy);
			} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				ASM(MOV32rm, value_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(layout.operand_offset)));
				ASM(TEST32rr, value_reg, value_reg);
				generate_raw_jump(Jump::jne, truthy);
				generate_raw_jump(Jump::jmp, falsey);
			} else {
				ASM(MOV64rm, value_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(layout.operand_offset)));
				ASM(TEST64rr, value_reg, value_reg);
				generate_raw_jump(Jump::jne, truthy);
				generate_raw_jump(Jump::jmp, falsey);
			}
		} else {
		ASM(MOVZXr32m8, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.operand_offset
						+ offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_NULL);
		generate_raw_jump(Jump::jle, falsey);
		ASM(CMP32ri, type_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);
		if (!layout.is_empty) {
			generate_raw_jump(Jump::jmp, truthy);
		} else {
			ASM(CMP32ri, type_reg, IS_FALSE);
			generate_raw_jump(Jump::je, falsey);
			ASM(CMP32ri, type_reg, IS_TRUE);
			generate_raw_jump(Jump::je, truthy);
			ASM(CMP32ri, type_reg, IS_LONG);
			auto not_long = text_writer.label_create();
			generate_raw_jump(Jump::jne, not_long);
			ASM(MOV64rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(TEST64rr, value_reg, value_reg);
			generate_raw_jump(Jump::jne, truthy);
			generate_raw_jump(Jump::jmp, falsey);

			label_place(not_long);
			ASM(CMP32ri, type_reg, IS_STRING);
			auto not_string = text_writer.label_create();
			generate_raw_jump(Jump::jne, not_string);
			ASM(MOV64rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(MOV64rm, type_reg,
				FE_MEM(value_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_string, len))));
			ASM(TEST64rr, type_reg, type_reg);
			generate_raw_jump(Jump::je, falsey);
			ASM(CMP64ri, type_reg, 1);
			generate_raw_jump(Jump::jne, truthy);
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(value_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_string, val))));
			ASM(CMP32ri, type_reg, '0');
			generate_raw_jump(Jump::je, falsey);
			generate_raw_jump(Jump::jmp, truthy);

			label_place(not_string);
			ASM(CMP32ri, type_reg, IS_ARRAY);
			auto not_array = text_writer.label_create();
			generate_raw_jump(Jump::jne, not_array);
			ASM(MOV64rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(MOV32rm, type_reg,
				FE_MEM(value_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(HashTable, nNumOfElements))));
			ASM(TEST32rr, type_reg, type_reg);
			generate_raw_jump(Jump::jne, truthy);
			generate_raw_jump(Jump::jmp, falsey);

			label_place(not_array);
			ASM(CMP32ri, type_reg, IS_RESOURCE);
			auto not_resource = text_writer.label_create();
			generate_raw_jump(Jump::jne, not_resource);
			ASM(MOV64rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)));
			ASM(MOV32rm, type_reg,
				FE_MEM(value_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_resource, handle))));
			ASM(TEST32rr, type_reg, type_reg);
			generate_raw_jump(Jump::jne, truthy);
			generate_raw_jump(Jump::jmp, falsey);
			label_place(not_resource);
			generate_raw_jump(Jump::jmp, slow);
		}
		}

		label_place(truthy);
		ASM(MOV32ri, type_reg,
			node.kind == Adaptor::InstKind::GuardedFast
				? (layout.is_empty ? 0 : 1)
				: (layout.is_empty ? IS_FALSE : IS_TRUE));
		generate_raw_jump(Jump::jmp, store);
		label_place(falsey);
		ASM(MOV32ri, type_reg,
			node.kind == Adaptor::InstKind::GuardedFast
				? (layout.is_empty ? 1 : 0)
				: (layout.is_empty ? IS_TRUE : IS_FALSE));
		label_place(store);
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, value_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			if (node.has_result) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV32rr, result_reg, type_reg);
				result.set_modified();
			} else {
				store_boolean_zval(frame_reg,
					static_cast<int32_t>(layout.result_offset), type_reg,
					value_reg);
			}
			ASM(MOV32ri, decision_reg, 0);
		} else {
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))),
				type_reg);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		value.reset();
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
		return true;
	};
	auto object_property_read = [&]() {
		zend_tpde_object_property_read layout;
		const zend_tpde_machine_reference *property_reference =
			operation_machine_reference(
				ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT);

		/* A result without a machine value lives only in its temporary:
		 * the copy goes there, as the helper's cached read puts it. */
		const bool frame_result = !node.has_result
			&& (mir.value_operation.result.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
				|| mir.value_operation.result.slot_kind
					== ZEND_MIR_SOURCE_SLOT_VAR);
		const bool isset_read = record.opcode
			== ZEND_MIR_OPCODE_OBJECT_FETCH_IS;
		const bool func_arg_read = record.opcode
			== ZEND_MIR_OPCODE_OBJECT_FETCH_FUNC_ARG;
		if (!(isset_read
					? frame_result
						&& zend_tpde_object_property_isset_read_at(
							mir, &layout)
					: func_arg_read
						? frame_result
							&& zend_tpde_object_property_func_arg_read_at(
								mir, &layout)
						: zend_tpde_object_property_read_at(mir, &layout))
				|| (!frame_result && !node.has_result)
				|| (!frame_result
					&& (property_reference == nullptr
						|| property_reference->stable_storage_or_layout_id
							!= layout.cache_offset
						|| property_reference->access_width
							!= sizeof(zval)))
				|| layout.receiver_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX
				|| layout.cache_offset > INT32_MAX - 3 * sizeof(void *)
				|| node.kind != Adaptor::InstKind::GuardedFast) {
			return branch_to_guarded_cold();
		}
		const auto guarded_successors = guarded_successors_of(node);
		const zend_tpde_machine_value_kind result_kind = frame_result
			? ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			: adaptor->machine_kind(node.result);
		const zend_mir_scalar_type_mask exact = frame_result
			? ZEND_MIR_SCALAR_TYPE_NONE : adaptor->exact_type(node.result);
		const bool boxed = result_kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
		const bool pointer = result_kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| result_kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| result_kind == ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
			|| result_kind == ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
			|| result_kind == ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR;
		if (!boxed && exact != ZEND_MIR_SCALAR_TYPE_I1
				&& exact != ZEND_MIR_SCALAR_TYPE_I64
				&& exact != ZEND_MIR_SCALAR_TYPE_F64 && !pointer) {
			return false;
		}
		/*
		 * The declared property the VM run-time cache names is probed by
		 * zend_native_property_slot(); a miss leaves everything unchanged and
		 * takes the guarded cold block, whose helper repeats the fetch.
		 */
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg decision{this};
		auto decision_reg = decision.alloc_gp();
		{
			ScratchReg cache{this};
			auto cache_reg = cache.alloc_gp();
			if (func_arg_read) {
				/* An argument sent by reference fetches for writing. */
				ASM(MOV64rm, cache_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, call))));
				ASM(TEST32mi,
					FE_MEM(cache_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))),
					static_cast<int32_t>(ZEND_CALL_SEND_ARG_BY_REF));
				generate_raw_jump(Jump::jne, slow);
			}
			ASM(MOV64rm, cache_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, run_time_cache))));
			ASM(TEST64rr, cache_reg, cache_reg);
			generate_raw_jump(Jump::je, slow);
			/* A reference property reads as its value (ZVAL_COPY_DEREF). */
			ValuePart slot{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_property_read_slot(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(layout.receiver_offset)}},
					GenericValuePart{GenericValuePart::Expr{
						std::move(cache),
						static_cast<int64_t>(layout.cache_offset)}},
					slot)) {
				return false;
			}
			const AsmReg slot_reg = slot.cur_reg_or_load(this);
			ASM(TEST64rr, slot_reg, slot_reg);
			generate_raw_jump(Jump::je, slow);
			GenericValuePart property{
				GenericValuePart::Expr{slot_reg, 0}};
			bool encoded;
			if (frame_result) {
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
				ValuePart type_info{tpde::x64::PlatformConfig::GP_BANK, 4};
				encoded = EncodeBase::encode_zend_native_zval_copy(
					std::move(property), payload, type_info);
				if (encoded) {
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.result_offset)),
						payload.cur_reg_or_load(this));
					ASM(MOV32mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.result_offset
								+ offsetof(zval, u1.type_info))),
						type_info.cur_reg_or_load(this));
				}
				payload.reset(this);
				type_info.reset(this);
				slot.reset(this);
				if (!encoded) {
					return false;
				}
			} else {
				auto result = result_ref(node.result);
				if (boxed) {
					/* An owned copy, published to the result temporary as the
					 * helper does, since consumers such as RETURN read it there. */
					auto payload = result.part(0);
					auto type_info = result.part(1);
					encoded = EncodeBase::encode_zend_native_zval_copy(
						std::move(property), payload, type_info);
					if (encoded) {
						ASM(MOV64mr,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(layout.result_offset)),
							payload.load_to_reg());
						ASM(MOV32mr,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(layout.result_offset
									+ offsetof(zval, u1.type_info))),
							type_info.load_to_reg());
					}
				} else if (exact == ZEND_MIR_SCALAR_TYPE_I1) {
					encoded = EncodeBase::encode_zend_native_zval_is_true_type(
						std::move(property), result.part(0));
				} else if (exact == ZEND_MIR_SCALAR_TYPE_F64) {
					encoded = EncodeBase::encode_zend_native_load_f64(
						std::move(property), result.part(0));
				} else {
					encoded = EncodeBase::encode_zend_native_load_u64(
						std::move(property), result.part(0));
				}
				slot.reset(this);
				if (!encoded) {
					return false;
				}
			}
		}
		if (guarded_exit_can_jump_directly(
				guarded_successors[1], guarded_successors[0])) {
			decision.reset();
			frame_scratch.reset();
			generate_guarded_direct_exit(
				slow, guarded_successors[1], guarded_successors[0]);
			return true;
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		frame_scratch.reset();
		generate_guarded_decision_branch(std::move(decision),
			guarded_successors[1], guarded_successors[0]);
		return true;
	};
	/*
	 * FETCH_OBJ_W of a cached untyped declared property: the VAR result
	 * addresses the slot through IS_INDIRECT, as the helper's cached fetch
	 * does; anything else takes the guarded cold block.
	 */
	auto object_property_address = [&]() {
		zend_tpde_object_property_read layout;

		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.has_result
				|| !zend_tpde_object_property_write_fetch_at(mir, &layout)
				|| layout.receiver_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX
				|| layout.cache_offset > INT32_MAX - 3 * sizeof(void *)) {
			return branch_to_guarded_cold();
		}
		const auto guarded_successors = guarded_successors_of(node);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg decision{this};
		auto decision_reg = decision.alloc_gp();
		{
			ScratchReg cache{this};
			auto cache_reg = cache.alloc_gp();
			ASM(MOV64rm, cache_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, run_time_cache))));
			ASM(TEST64rr, cache_reg, cache_reg);
			generate_raw_jump(Jump::je, slow);
			/* A typed property's info in the cache entry needs the
			 * helper's checks. */
			ASM(CMP64mi,
				FE_MEM(cache_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.cache_offset + 2 * sizeof(void *))),
				0);
			generate_raw_jump(Jump::jne, slow);
			ValuePart slot{tpde::x64::PlatformConfig::GP_BANK, 8};
			if (!EncodeBase::encode_zend_native_property_slot(
					GenericValuePart{GenericValuePart::Expr{frame_reg,
						static_cast<int64_t>(layout.receiver_offset)}},
					GenericValuePart{GenericValuePart::Expr{
						std::move(cache),
						static_cast<int64_t>(layout.cache_offset)}},
					slot)) {
				return false;
			}
			const AsmReg slot_reg = slot.cur_reg_or_load(this);
			ASM(TEST64rr, slot_reg, slot_reg);
			generate_raw_jump(Jump::je, slow);
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				slot_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset
						+ offsetof(zval, u1.type_info))),
				IS_INDIRECT);
			slot.reset(this);
		}
		if (guarded_exit_can_jump_directly(
				guarded_successors[1], guarded_successors[0])) {
			decision.reset();
			frame_scratch.reset();
			generate_guarded_direct_exit(
				slow, guarded_successors[1], guarded_successors[0]);
			return true;
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		frame_scratch.reset();
		generate_guarded_decision_branch(std::move(decision),
			guarded_successors[1], guarded_successors[0]);
		return true;
	};
	auto object_property_write = [&]() {
		zend_tpde_object_property_write layout;
		const zend_tpde_machine_reference *property_reference =
			operation_machine_reference(
				ZEND_TPDE_MACHINE_REFERENCE_PROPERTY_SLOT);

		if (!zend_tpde_object_property_write_at(mir, &layout)
				|| property_reference == nullptr
				|| property_reference->stable_storage_or_layout_id
					!= layout.cache_offset
				|| property_reference->access_width != sizeof(zval)
				|| layout.receiver_offset > INT32_MAX
				|| layout.value_offset > INT32_MAX
				|| layout.cache_offset > INT32_MAX - 3 * sizeof(void *)) {
			return execute_value_operation();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const bool scalar_value =
			node.property_write_value_operand_index != UINT32_MAX
			&& node.property_write_value_operand_index < node.operands.size()
			&& adaptor->machine_kind(node.operands[
				node.property_write_value_operand_index])
				== ZEND_TPDE_MACHINE_VALUE_I64
			&& adaptor->exact_type(node.operands[
				node.property_write_value_operand_index])
				== ZEND_MIR_SCALAR_TYPE_I64;
		const auto successors = guarded_successors_of(node);
		auto slow = text_writer.label_create();
		auto property_metadata_valid = text_writer.label_create();
		auto old_released = text_writer.label_create();
		auto value_owned = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg object{this};
		ScratchReg cache{this};
		ScratchReg offset{this};
		ScratchReg property{this};
		ScratchReg type{this};
		ScratchReg low_word{this};
		ScratchReg decision{this};
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		/* A literal value lives in the literal table: its base goes into
		 * base_reg, otherwise the value is a frame slot. */
		auto load_literal_base = [&](AsmReg base_reg) {
			ASM(MOV64rm, base_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, base_reg, FE_MEM(base_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_op_array, literals))));
		};

		ASM(MOVZXr32m8, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.receiver_offset
						+ offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_OBJECT);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, object_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.receiver_offset)));
		ASM(MOV32rm, offset_reg,
			FE_MEM(object_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_object, extra_flags))));
		ASM(TEST32ri, offset_reg,
			IS_OBJ_LAZY_UNINITIALIZED | IS_OBJ_LAZY_PROXY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, cache_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_execute_data, run_time_cache))));
		ASM(TEST64rr, cache_reg, cache_reg);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rm, type_reg,
			FE_MEM(object_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_object, ce))));
		ASM(MOV64rm, offset_reg,
			FE_MEM(type_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_class_entry, create_object))));
		ASM(TEST64rr, offset_reg, offset_reg);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, property_reg,
			FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.cache_offset)));
		ASM(CMP64rr, type_reg, property_reg);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, offset_reg,
			FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.cache_offset + sizeof(void *))));
		ASM(CMP64ri, offset_reg, ZEND_FIRST_PROPERTY_OFFSET);
		generate_raw_jump(Jump::jl, slow);
		ASM(MOV64rm, type_reg,
			FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.cache_offset + 2 * sizeof(void *))));
		ASM(TEST64rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, property_metadata_valid);
		ASM(MOV32rm, cache_reg,
			FE_MEM(type_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_property_info, flags))));
		ASM(TEST32ri, cache_reg, ZEND_ACC_READONLY);
		generate_raw_jump(Jump::jne, slow);
		ASM(TEST32ri, cache_reg, ZEND_ACC_PPP_SET_MASK);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, cache_reg,
			FE_MEM(type_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_property_info, hooks))));
		ASM(TEST64rr, cache_reg, cache_reg);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV32rm, cache_reg,
			FE_MEM(type_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_property_info, type)
						+ offsetof(zend_type, type_mask))));
		ASM(TEST32ri, cache_reg, 1u << IS_LONG);
		generate_raw_jump(Jump::je, slow);
		if (!scalar_value) {
			AsmReg value_base = frame_reg;
			if (layout.literal_value) {
				load_literal_base(cache_reg);
				value_base = cache_reg;
			}
			ASM(MOVZXr32m8, cache_reg,
				FE_MEM(value_base, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP32ri, cache_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		}
		label_place(property_metadata_valid);
		ASM(MOV64rr, property_reg, object_reg);
		ASM(ADD64rr, property_reg, offset_reg);
		ASM(MOV32rm, type_reg,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP8ri, type_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);
		ASM(CMP8ri, type_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);

		if (scalar_value) {
			ASM(MOV32ri, type_reg, IS_LONG);
		} else {
			AsmReg value_base = frame_reg;
			if (layout.literal_value) {
				load_literal_base(low_word_reg);
				value_base = low_word_reg;
			}
			ASM(MOV32rm, type_reg,
				FE_MEM(value_base, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))));
			ASM(CMP8ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
		}

		ASM(TEST8mi,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.v.type_flags))),
			IS_TYPE_REFCOUNTED);
		generate_raw_jump(Jump::je, old_released);
		ASM(MOV64rm, cache_reg,
			FE_MEM(property_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, offset_reg,
			FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))));
		ASM(CMP32ri, offset_reg, 1);
		generate_raw_jump(Jump::jle, slow);
		ASM(SUB32mi,
			FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(old_released);

		if (scalar_value) {
			auto [value_ref, value] = val_ref_single(node.operands[
				node.property_write_value_operand_index]);
			auto value_reg = value.load_to_reg();
			ASM(MOV64rr, low_word_reg, value_reg);
		} else {
			/* For a literal, low_word still holds the literal base. */
			ASM(MOV64rm, low_word_reg,
				FE_MEM(layout.literal_value ? low_word_reg : frame_reg, 0,
					FE_NOREG, static_cast<int32_t>(layout.value_offset)));
		}
		if (!scalar_value && !layout.move_value) {
			ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::je, value_owned);
			ASM(ADD32mi,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				1);
			label_place(value_owned);
		}
		ASM(MOV64mr,
			FE_MEM(property_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV32mr,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
		if (layout.move_value) {
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))),
				IS_UNDEF);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto direct_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (guarded_exit_can_jump_directly(
					direct_successors[1], direct_successors[0])) {
				generate_guarded_direct_exit(
					slow, direct_successors[1], direct_successors[0]);
				return true;
			}
		}
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		low_word.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	/*
	 * FETCH_CONSTANT reads the constant the runtime cache slot names, like
	 * the VM: a cached constant with a non-refcounted value is copied to the
	 * result slot inline. An empty or special slot, a counted value and a
	 * result held in registers use the helper, which also fills the slot.
	 */
	auto fetch_constant_inline = [&]() {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		/* FETCH_CONSTANT is never guarded (see
		 * freeze_machine_control_flow()): the helper runs inline when the
		 * cached constant is missing or counted. */
		bool frame_operands = node.kind == Adaptor::InstKind::MIR
			&& !node.has_result
			&& mir.has_value_operation
			&& operation.source_opcode == ZEND_FETCH_CONSTANT
			&& (operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
			&& zend_mir_id_is_valid(operation.result_storage_id)
			&& operation.extended_value <= INT32_MAX - sizeof(void *);
		for (IRValueRef operand : node.operands) {
			frame_operands = frame_operands
				&& (operand == IRValueRef{Adaptor::FRAME_VALUE}
					|| operand == IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT});
		}
		const uint64_t result_offset64 =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (!frame_operands || result_offset64 > INT32_MAX - sizeof(zval)) {
			return execute_value_operation();
		}
		const int32_t result_offset = static_cast<int32_t>(result_offset64);
		const auto spilled = spill_before_branch(true);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		{
			const AsmReg frame_reg = canonical_frame_register();
			ScratchReg constant{this};
			ScratchReg type{this};
			auto constant_reg = constant.alloc_gp();
			auto type_reg = type.alloc_gp();
			ASM(MOV64rm, constant_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, run_time_cache))));
			ASM(TEST64rr, constant_reg, constant_reg);
			generate_raw_jump(Jump::je, slow);
			ASM(MOV64rm, constant_reg,
				FE_MEM(constant_reg, 0, FE_NOREG,
					static_cast<int32_t>(operation.extended_value)));
			ASM(TEST64rr, constant_reg, constant_reg);
			generate_raw_jump(Jump::je, slow);
			ASM(TEST32ri, constant_reg, CACHE_SPECIAL);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV32rm, type_reg,
				FE_MEM(constant_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_constant, value)
						+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, type_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG, result_offset
					+ static_cast<int32_t>(offsetof(zval, u1.type_info))),
				type_reg);
			ASM(MOV64rm, constant_reg,
				FE_MEM(constant_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_constant, value))));
			ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG, result_offset),
				constant_reg);
			generate_raw_jump(Jump::jmp, done);
		}
		label_place(slow);
		if (!execute_value_operation()) {
			return false;
		}
		label_place(done);
		release_spilled_regs(spilled);
		return true;
	};
	/*
	 * BIND_GLOBAL of a CV as the VM binds it: the run-time cache keeps the
	 * global's bucket offset in EG(symbol_table); while that bucket still
	 * holds the name and a reference, the CV takes another reference to
	 * it. Anything else, including a CV that holds a counted value, runs
	 * the helper, which also refreshes the cached offset. A non-ZTS build
	 * reads EG(symbol_table)'s address from the execution context.
	 */
	auto bind_global_inline = [&]() {
#ifdef ZTS
		return execute_value_operation();
#else
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		bool frame_operands = node.kind == Adaptor::InstKind::MIR
			&& !node.has_result && mir.has_value_operation
			&& operation.source_opcode == ZEND_BIND_GLOBAL
			&& (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& zend_mir_id_is_valid(operation.op1_storage_id)
			&& operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& operation.extended_value <= INT32_MAX - sizeof(void *)
			&& uint64_t{operation.op2.index} * sizeof(zval) <= INT32_MAX;
		for (IRValueRef operand : node.operands) {
			frame_operands = frame_operands
				&& (operand == IRValueRef{Adaptor::FRAME_VALUE}
					|| operand == IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT});
		}
		const uint64_t local_offset64 =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
				* sizeof(zval);
		/* Images outlive the process (the OPcache file cache): the symbol
		 * table comes from the context. */
		if (!frame_operands || local_offset64 > INT32_MAX - sizeof(zval)
				|| val_assignment(adaptor->val_local_idx(IRValueRef{
					Adaptor::EXECUTION_CONTEXT_ARGUMENT})) == nullptr) {
			return execute_value_operation();
		}
		const int32_t local_offset = static_cast<int32_t>(local_offset64);
		const AsmReg symbol_context_reg = canonical_value_register(
			IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
		const auto spilled = spill_before_branch(true);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		{
			const AsmReg frame_reg = canonical_frame_register();
			ScratchReg offset{this};
			ScratchReg table{this};
			ScratchReg bucket{this};
			auto offset_reg = offset.alloc_gp();
			auto table_reg = table.alloc_gp();
			auto bucket_reg = bucket.alloc_gp();
			ASM(MOV64rm, offset_reg,
				FE_MEM(frame_reg, 0, FE_NOREG, static_cast<int32_t>(
					offsetof(zend_execute_data, run_time_cache))));
			ASM(TEST64rr, offset_reg, offset_reg);
			generate_raw_jump(Jump::je, slow);
			/* The cache holds offset + 1; an empty slot wraps below. */
			ASM(MOV64rm, offset_reg, FE_MEM(offset_reg, 0, FE_NOREG,
				static_cast<int32_t>(operation.extended_value)));
			ASM(SUB64ri, offset_reg, 1);
			ASM(MOV64rm, table_reg, FE_MEM(symbol_context_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(
					zend_native_execution_context, symbol_table))));
			ASM(MOV32rm, bucket_reg, FE_MEM(table_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumUsed))));
			ASM(SHL64ri, bucket_reg, 5);
			static_assert(sizeof(Bucket) == 32);
			ASM(CMP64rr, offset_reg, bucket_reg);
			generate_raw_jump(Jump::jae, slow);
			ASM(MOV64rm, bucket_reg, FE_MEM(table_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arData))));
			ASM(ADD64rr, bucket_reg, offset_reg);
			/* The literal name must be the bucket's key string. */
			ASM(MOV64rm, table_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, func))));
			ASM(MOV64rm, table_reg, FE_MEM(table_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_op_array, literals))));
			ASM(MOV64rm, table_reg, FE_MEM(table_reg, 0, FE_NOREG,
				static_cast<int32_t>(operation.op2.index * sizeof(zval))));
			ASM(CMP64rm, table_reg, FE_MEM(bucket_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, key))));
			generate_raw_jump(Jump::jne, slow);
			{
				auto direct = text_writer.label_create();
				ASM(CMP8mi, FE_MEM(bucket_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
					IS_INDIRECT);
				generate_raw_jump(Jump::jne, direct);
				ASM(MOV64rm, bucket_reg, FE_MEM(bucket_reg, 0, FE_NOREG, 0));
				label_place(direct);
			}
			ASM(CMP8mi, FE_MEM(bucket_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
				IS_REFERENCE);
			generate_raw_jump(Jump::jne, slow);
			ASM(TEST32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				local_offset + static_cast<int32_t>(
					offsetof(zval, u1.type_info))),
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, bucket_reg, FE_MEM(bucket_reg, 0, FE_NOREG, 0));
			ASM(ADD32mi, FE_MEM(bucket_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_refcounted_h, refcount))),
				1);
			ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG, local_offset),
				bucket_reg);
			ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG,
				local_offset + static_cast<int32_t>(
					offsetof(zval, u1.type_info))),
				IS_REFERENCE_EX);
			generate_raw_jump(Jump::jmp, done);
		}
		label_place(slow);
		if (!execute_value_operation()) {
			return false;
		}
		label_place(done);
		release_spilled_regs(spilled);
		return true;
#endif
	};
	/*
	 * FETCH_CLASS_CONSTANT of a literal name as the VM reads it: the
	 * run-time cache keeps the class and the constant's value; for a literal
	 * class the value alone decides, for self::, parent:: and static:: the
	 * class, resolved from the caller, must match. An uncounted value is
	 * copied into the result; anything else runs the helper, which also
	 * fills the cache.
	 */
	auto class_constant_inline = [&]() {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		const uint32_t fetch =
			operation.op1_unused_payload & ZEND_FETCH_CLASS_MASK;
		const bool literal_class =
			operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
		bool frame_operands = node.kind == Adaptor::InstKind::MIR
			&& !node.has_result && mir.has_value_operation
			&& operation.source_opcode == ZEND_FETCH_CLASS_CONSTANT
			&& operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& (literal_class
				|| (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
					&& (fetch == ZEND_FETCH_CLASS_SELF
						|| fetch == ZEND_FETCH_CLASS_PARENT
						|| fetch == ZEND_FETCH_CLASS_STATIC)))
			&& (operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
			&& zend_mir_id_is_valid(operation.result_storage_id)
			&& operation.extended_value <= INT32_MAX - 2 * sizeof(void *);
		for (IRValueRef operand : node.operands) {
			frame_operands = frame_operands
				&& (operand == IRValueRef{Adaptor::FRAME_VALUE}
					|| operand == IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT});
		}
		const uint64_t result_offset64 =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (!frame_operands || result_offset64 > INT32_MAX - sizeof(zval)) {
			return execute_value_operation();
		}
		const int32_t result_offset = static_cast<int32_t>(result_offset64);
		const int32_t class_slot =
			static_cast<int32_t>(operation.extended_value);
		const int32_t value_slot = class_slot
			+ static_cast<int32_t>(sizeof(void *));
		const auto spilled = spill_before_branch(true);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		{
			const AsmReg frame_reg = canonical_frame_register();
			ScratchReg cache{this};
			ScratchReg scope{this};
			ScratchReg value{this};
			auto cache_reg = cache.alloc_gp();
			auto scope_reg = scope.alloc_gp();
			auto value_reg = value.alloc_gp();
			ASM(MOV64rm, cache_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_execute_data, run_time_cache))));
			ASM(TEST64rr, cache_reg, cache_reg);
			generate_raw_jump(Jump::je, slow);
			if (!literal_class) {
				if (fetch == ZEND_FETCH_CLASS_STATIC) {
					/* The called scope: $this's class or the class This
					 * holds. */
					auto have_scope = text_writer.label_create();
					ASM(MOV64rm, scope_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, This))));
					ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))), IS_OBJECT);
					generate_raw_jump(Jump::jne, have_scope);
					ASM(MOV64rm, scope_reg, FE_MEM(scope_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_object, ce))));
					label_place(have_scope);
				} else {
					ASM(MOV64rm, scope_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, func))));
					ASM(MOV64rm, scope_reg, FE_MEM(scope_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_op_array, scope))));
					if (fetch == ZEND_FETCH_CLASS_PARENT) {
						ASM(TEST64rr, scope_reg, scope_reg);
						generate_raw_jump(Jump::je, slow);
						ASM(MOV64rm, scope_reg, FE_MEM(scope_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_class_entry, parent))));
					}
				}
				ASM(TEST64rr, scope_reg, scope_reg);
				generate_raw_jump(Jump::je, slow);
				ASM(CMP64rm, scope_reg,
					FE_MEM(cache_reg, 0, FE_NOREG, class_slot));
				generate_raw_jump(Jump::jne, slow);
			}
			ASM(MOV64rm, cache_reg, FE_MEM(cache_reg, 0, FE_NOREG, value_slot));
			ASM(TEST64rr, cache_reg, cache_reg);
			generate_raw_jump(Jump::je, slow);
			ASM(MOV32rm, value_reg, FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(TEST32ri, value_reg, IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV32mr, FE_MEM(frame_reg, 0, FE_NOREG, result_offset
				+ static_cast<int32_t>(offsetof(zval, u1.type_info))),
				value_reg);
			ASM(MOV64rm, value_reg, FE_MEM(cache_reg, 0, FE_NOREG, 0));
			ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG, result_offset),
				value_reg);
			generate_raw_jump(Jump::jmp, done);
		}
		label_place(slow);
		if (!execute_value_operation()) {
			return false;
		}
		label_place(done);
		release_spilled_regs(spilled);
		return true;
	};
	/*
	 * INSTANCEOF of a CV against a literal class as the VM tests it: the
	 * run-time cache keeps the class; a value that is no object is not an
	 * instance, and an object is one when its class, a parent or an
	 * interface is that class; a reference is looked through. An undefined
	 * variable and an unfilled cache run the helper.
	 */
	auto instanceof_inline = [&]() {
		const zend_mir_executable_value_ref &operation = mir.value_operation;
		bool frame_operands = node.kind == Adaptor::InstKind::MIR
			&& !node.has_result && mir.has_value_operation
			&& operation.source_opcode == ZEND_INSTANCEOF
			&& (operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& zend_mir_id_is_valid(operation.op1_storage_id)
			&& operation.op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& (operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operation.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& (operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
				|| operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV)
			&& zend_mir_id_is_valid(operation.result_storage_id)
			&& operation.result_storage_id != operation.op1_storage_id
			&& operation.extended_value <= INT32_MAX - sizeof(void *);
		/* A CV result releases its previous value: a counted one takes
		 * the helper. */
		const bool result_cv =
			operation.result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV;
		for (IRValueRef operand : node.operands) {
			frame_operands = frame_operands
				&& (operand == IRValueRef{Adaptor::FRAME_VALUE}
					|| operand == IRValueRef{
						Adaptor::EXECUTION_CONTEXT_ARGUMENT});
		}
		const uint64_t value_offset64 =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.op1_storage_id)
				* sizeof(zval);
		const uint64_t result_offset64 =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + operation.result_storage_id)
				* sizeof(zval);
		if (!frame_operands || value_offset64 > INT32_MAX - sizeof(zval)
				|| result_offset64 > INT32_MAX - sizeof(zval)) {
			return execute_value_operation();
		}
		const int32_t value_offset = static_cast<int32_t>(value_offset64);
		const int32_t result_offset = static_cast<int32_t>(result_offset64);
		const int32_t result_type = result_offset
			+ static_cast<int32_t>(offsetof(zval, u1.type_info));
		const auto spilled = spill_before_branch(true);
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		{
			const AsmReg frame_reg = canonical_frame_register();
			ScratchReg type{this};
			ScratchReg klass{this};
			auto type_reg = type.alloc_gp();
			auto class_reg = klass.alloc_gp();
			if (result_cv) {
				ASM(TEST8mi, FE_MEM(frame_reg, 0, FE_NOREG,
					result_offset + static_cast<int32_t>(
						offsetof(zval, u1.v.type_flags))),
					IS_TYPE_REFCOUNTED);
				generate_raw_jump(Jump::jne, slow);
			}
			/* The value's zval, behind a reference (a global) too. */
			ScratchReg value{this};
			auto value_reg = value.alloc_gp();
			ASM(LEA64rm, value_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				value_offset));
			ASM(MOVZXr32m8, type_reg, FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
			auto dereferenced = text_writer.label_create();
			ASM(CMP32ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::jne, dereferenced);
			ASM(MOV64rm, value_reg, FE_MEM(value_reg, 0, FE_NOREG, 0));
			ASM(LEA64rm, value_reg, FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_reference, val))));
			ASM(MOVZXr32m8, type_reg, FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
			label_place(dereferenced);
			ASM(CMP32ri, type_reg, IS_OBJECT);
			auto object = text_writer.label_create();
			generate_raw_jump(Jump::je, object);
			/* An undefined CV warns in the helper. */
			ASM(CMP32ri, type_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
			ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG, result_type),
				IS_FALSE);
			generate_raw_jump(Jump::jmp, done);
			label_place(object);
			ASM(MOV64rm, class_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_execute_data, run_time_cache))));
			ASM(TEST64rr, class_reg, class_reg);
			generate_raw_jump(Jump::je, slow);
			ASM(MOV64rm, class_reg, FE_MEM(class_reg, 0, FE_NOREG,
				static_cast<int32_t>(operation.extended_value)));
			ASM(TEST64rr, class_reg, class_reg);
			generate_raw_jump(Jump::je, slow);
			ASM(MOV64rm, type_reg, FE_MEM(value_reg, 0, FE_NOREG, 0));
			ASM(MOV64rm, type_reg, FE_MEM(type_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_object, ce))));
			auto instance = text_writer.label_create();
			auto not_instance = text_writer.label_create();
			ASM(CMP64rr, class_reg, type_reg);
			generate_raw_jump(Jump::je, instance);
			/* instanceof_function_slow(): the interfaces of a linked class,
			 * else its parent chain. */
			{
				ScratchReg index{this};
				auto index_reg = index.alloc_gp();
				auto parent_loop = text_writer.label_create();
				auto interface_loop = text_writer.label_create();
				ASM(TEST32mi, FE_MEM(class_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_class_entry,
						ce_flags))), ZEND_ACC_INTERFACE);
				auto parents = text_writer.label_create();
				generate_raw_jump(Jump::je, parents);
				ASM(MOV32rm, index_reg, FE_MEM(type_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_class_entry,
						num_interfaces))));
				ASM(MOV64rm, type_reg, FE_MEM(type_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_class_entry,
						interfaces))));
				label_place(interface_loop);
				ASM(SUB32ri, index_reg, 1);
				generate_raw_jump(Jump::jb, not_instance);
				ASM(CMP64rm, class_reg, FE_MEM(type_reg, 8, index_reg, 0));
				generate_raw_jump(Jump::je, instance);
				generate_raw_jump(Jump::jmp, interface_loop);
				label_place(parents);
				label_place(parent_loop);
				ASM(MOV64rm, type_reg, FE_MEM(type_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zend_class_entry,
						parent))));
				ASM(CMP64rr, class_reg, type_reg);
				generate_raw_jump(Jump::je, instance);
				ASM(TEST64rr, type_reg, type_reg);
				generate_raw_jump(Jump::jne, parent_loop);
			}
			label_place(not_instance);
			ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG, result_type),
				IS_FALSE);
			generate_raw_jump(Jump::jmp, done);
			label_place(instance);
			ASM(MOV32mi, FE_MEM(frame_reg, 0, FE_NOREG, result_type),
				IS_TRUE);
			generate_raw_jump(Jump::jmp, done);
		}
		label_place(slow);
		if (!execute_value_operation()) {
			return false;
		}
		label_place(done);
		release_spilled_regs(spilled);
		return true;
	};
	auto dynamic_fetch_read = [&]() {
		zend_tpde_dynamic_fetch_read layout;

		if (!zend_tpde_dynamic_fetch_read_at(mir, &layout)) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result) {
			return branch_to_guarded_cold();
		}
		/* DYNAMIC_FETCH_R is always guarded (see
		 * freeze_machine_control_flow()); a name without a direct CV takes
		 * the cold path. */
		ZEND_ASSERT(node.kind == Adaptor::InstKind::GuardedFast);
		const auto guarded_successors = guarded_successors_of(node);
		if (layout.cv_index == UINT32_MAX) {
			return branch_to_guarded_cold();
		}
		if (layout.direct_long) {
			const uint64_t value_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + layout.cv_index)
					* sizeof(zval);
			if (value_offset > INT32_MAX - sizeof(zval)
					|| adaptor->representation(node.result)
						!= ZEND_MIR_REPRESENTATION_I64
					|| adaptor->exact_type(node.result)
						!= ZEND_MIR_SCALAR_TYPE_I64
					|| adaptor->machine_kind(node.result)
						!= ZEND_TPDE_MACHINE_VALUE_I64) {
				return false;
			}
			if (node.operands.size() == 2) {
				auto [name_ref, name_value] =
					val_ref_single(node.operands[0]);
				(void) name_ref;
				(void) name_value;
			}
			auto [frame_ref, frame] =
				val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
			auto frame_reg = frame.load_to_reg();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(MOV64rm, result_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(value_offset)));
			result.set_modified();
			generate_branch_to_block(Jump::jmp,
				IRBlockRef{node.continuation_block}, false, true);
			return true;
		}
		const uint64_t cv_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + layout.cv_index) * sizeof(zval);
		if (cv_offset > INT32_MAX - sizeof(zval)) {
			return false;
		}
		auto slow = text_writer.label_create();
		auto not_indirect = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = frame_register(std::move(frame));
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg slot{this};
		ScratchReg type{this};
		ScratchReg decision{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		ASM(LEA64rm, slot_reg,
			FE_MEM(frame_reg, 0, FE_NOREG, static_cast<int32_t>(cv_offset)));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP8ri, type_reg, IS_INDIRECT);
		generate_raw_jump(Jump::jne, not_indirect);
		ASM(MOV64rm, slot_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		label_place(not_indirect);
		ASM(CMP8ri, type_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);

		ASM(MOV32ri, decision_reg, 0);
		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto result = result_ref(node.result);
			auto payload = result.part(0);
			auto type_info = result.part(1);
			auto payload_reg = payload.alloc_reg();
			auto type_info_reg = type_info.alloc_reg();
			ASM(MOV64rm, payload_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(MOV32rr, type_info_reg, type_reg);
			payload.set_modified();
			type_info.set_modified();
			ASM(TEST32ri, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::je, done);
			ASM(ADD32mi,
				FE_MEM(payload_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				1);
		} else {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			switch (adaptor->exact_type(node.result)) {
				case ZEND_MIR_SCALAR_TYPE_I1:
					ASM(CMP32ri, type_reg, IS_TRUE);
					generate_raw_set(Jump::je, result_reg);
					break;
				case ZEND_MIR_SCALAR_TYPE_I64:
					ASM(MOV64rm, result_reg,
						FE_MEM(slot_reg, 0, FE_NOREG, 0));
					break;
				case ZEND_MIR_SCALAR_TYPE_F64:
					ASM(SSE_MOVSDrm, result_reg,
						FE_MEM(slot_reg, 0, FE_NOREG, 0));
					break;
				default:
					switch (adaptor->machine_kind(node.result)) {
						case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
						case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
						case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
						case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
						case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
							ASM(MOV64rm, result_reg,
								FE_MEM(slot_reg, 0, FE_NOREG, 0));
							break;
						default:
							return false;
					}
			}
			result.set_modified();
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		ASM(MOV32ri, decision_reg, 1);
		label_place(done);
		generate_guarded_decision_branch(
			std::move(decision), guarded_successors[1],
			guarded_successors[0]);
		return true;
	};

	switch (node.kind) {
		case Adaptor::InstKind::ZvalCopy:
		case Adaptor::InstKind::ZvalMove:
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_COPY_TMP) {
				return copy_temporary_slot();
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN) {
				return copy_slot(
					mir.value_operation.op2,
					mir.value_operation.op2_storage_id,
					mir.value_operation.op1_storage_id,
					mir.value_operation.result_storage_id,
					node.kind == Adaptor::InstKind::ZvalMove);
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_QM_ASSIGN) {
				return copy_slot(
					mir.value_operation.op1,
					mir.value_operation.op1_storage_id,
					mir.value_operation.result_storage_id,
					ZEND_MIR_ID_INVALID,
					node.kind == Adaptor::InstKind::ZvalMove, true);
			}
			return false;
		case Adaptor::InstKind::ZvalReleaseFast:
			return record.opcode == ZEND_MIR_OPCODE_VALUE_FREE
				? free_temporary_slot() : false;
		case Adaptor::InstKind::SlowPathCall:
			/*
			 * The helper reads its operands from their frame slots. A value
			 * the preceding fast node left register-authoritative (for example
			 * the right-hand side of an assignment to a dynamic target) must be
			 * published there first.
			 */
			for (IRValueRef operand : node.operands) {
				if (operand == IRValueRef{Adaptor::FRAME_VALUE}
						|| operand == IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT}) {
					continue;
				}
				if (!materialize_cold_operand(
						operand, adaptor->canonical_storage(operand))) {
					return false;
				}
			}
			if ((record.opcode == ZEND_MIR_OPCODE_VALUE_ROPE_INIT
						|| record.opcode == ZEND_MIR_OPCODE_VALUE_ROPE_ADD)
					&& rope_literal_inline()) {
				return true;
			}
			if (mir.has_value_operation
					&& mir.value_operation.source_opcode == ZEND_ASSIGN_DIM_OP
					&& (mir.value_operation.extended_value == ZEND_ADD
						|| mir.value_operation.extended_value == ZEND_SUB
						|| mir.value_operation.extended_value == ZEND_MUL)
					&& !zend_mir_id_is_valid(
						mir.value_operation.result_storage_id)) {
				/*
				 * $a[$k] op= $v on an existing number of an owned packed array
				 * updates the element inline like the VM's numeric fast paths.
				 * Every other container, key, element or value keeps the
				 * helper.
				 */
				const zend_mir_executable_value_ref &operation =
					mir.value_operation;
				auto slot_offset = [&](const zend_mir_source_operand_ref &operand,
						zend_mir_storage_id storage, bool allow_literal,
						bool *literal, int32_t *out) {
					uint64_t offset;
					if (operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
						if (!allow_literal) {
							return false;
						}
						*literal = true;
						offset = uint64_t{operand.index} * sizeof(zval);
					} else if ((operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
								|| operand.kind == ZEND_MIR_SOURCE_OPERAND_SSA)
							&& (operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
								|| operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_TMP)
							&& zend_mir_id_is_valid(storage)) {
						*literal = false;
						offset = (uint64_t{ZEND_CALL_FRAME_SLOT} + storage)
							* sizeof(zval);
					} else {
						return false;
					}
					if (offset > INT32_MAX - sizeof(zval)) {
						return false;
					}
					*out = static_cast<int32_t>(offset);
					return true;
				};
				bool container_literal = false, key_literal = false,
					value_literal = false;
				int32_t container_offset = 0, key_offset = 0, value_offset = 0;
				if (operation.op1.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
						&& slot_offset(operation.op1, operation.op1_storage_id,
							false, &container_literal, &container_offset)
						&& slot_offset(operation.op2, operation.op2_storage_id,
							true, &key_literal, &key_offset)
						&& slot_offset(operation.auxiliary,
							operation.auxiliary_storage_id, true,
							&value_literal, &value_offset)) {
					const uint32_t opcode = operation.extended_value;
					const int32_t type_info =
						static_cast<int32_t>(offsetof(zval, u1.type_info));
					const auto spilled = spill_before_branch(true);
					auto slow = text_writer.label_create();
					auto done = text_writer.label_create();
					{
						const AsmReg frame_reg = canonical_frame_register();
						ScratchReg literals{this};
						ScratchReg element{this};
						ScratchReg left{this};
						ScratchReg right{this};
						ScratchReg left_type{this};
						ScratchReg right_type{this};
						ScratchReg left_double{this};
						ScratchReg right_double{this};
						auto element_reg = element.alloc_gp();
						auto left_reg = left.alloc_gp();
						auto right_reg = right.alloc_gp();
						auto left_type_reg = left_type.alloc_gp();
						auto right_type_reg = right_type.alloc_gp();
						auto left_fp = left_double.alloc(
							tpde::x64::PlatformConfig::FP_BANK);
						auto right_fp = right_double.alloc(
							tpde::x64::PlatformConfig::FP_BANK);
						ScratchReg other_key{this};
						ScratchReg key_length{this};
						ScratchReg key_byte{this};
						auto other_key_reg = other_key.alloc_gp();
						auto key_length_reg = key_length.alloc_gp();
						auto key_byte_reg = key_byte.alloc_gp();
						AsmReg literals_reg = frame_reg;
						if (key_literal || value_literal) {
							literals_reg = literals.alloc_gp();
							ASM(MOV64rm, literals_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_execute_data, func))));
							ASM(MOV64rm, literals_reg,
								FE_MEM(literals_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_op_array, literals))));
						}
						/* Owned packed array, possibly behind a reference. */
						auto container_ready = text_writer.label_create();
						ASM(LEA64rm, element_reg,
							FE_MEM(frame_reg, 0, FE_NOREG, container_offset));
						ASM(CMP8mi, FE_MEM(element_reg, 0, FE_NOREG, type_info),
							IS_REFERENCE);
						generate_raw_jump(Jump::jne, container_ready);
						ASM(MOV64rm, element_reg,
							FE_MEM(element_reg, 0, FE_NOREG, 0));
						ASM(ADD64ri, element_reg, static_cast<int32_t>(
							offsetof(zend_reference, val)));
						label_place(container_ready);
						ASM(CMP8mi, FE_MEM(element_reg, 0, FE_NOREG, type_info),
							IS_ARRAY);
						generate_raw_jump(Jump::jne, slow);
						ASM(MOV64rm, element_reg,
							FE_MEM(element_reg, 0, FE_NOREG, 0));
						ASM(CMP32mi,
							FE_MEM(element_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))),
							1);
						generate_raw_jump(Jump::jne, slow);
						const AsmReg key_base =
							key_literal ? literals_reg : frame_reg;
						auto hash_key = text_writer.label_create();
						auto have_element = text_writer.label_create();
						ASM(TEST32mi,
							FE_MEM(element_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(HashTable, u))),
							HASH_FLAG_PACKED);
						generate_raw_jump(Jump::je, hash_key);
						/* Existing element by integer key. */
						ASM(CMP8mi, FE_MEM(key_base, 0, FE_NOREG,
							key_offset + type_info), IS_LONG);
						generate_raw_jump(Jump::jne, slow);
						ASM(MOV64rm, left_reg,
							FE_MEM(key_base, 0, FE_NOREG, key_offset));
						ASM(MOV32rm, left_type_reg,
							FE_MEM(element_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									HashTable, nNumUsed))));
						ASM(CMP64rr, left_reg, left_type_reg);
						generate_raw_jump(Jump::jae, slow);
						ASM(SHL64ri, left_reg, 4);
						ASM(ADD64rm, left_reg,
							FE_MEM(element_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									HashTable, arPacked))));
						ASM(MOV64rr, element_reg, left_reg);
						generate_raw_jump(Jump::jmp, have_element);
						/*
						 * Existing element by string key in a hash: probe with the
						 * key's cached hash; the same string or equal bytes match.
						 */
						label_place(hash_key);
						{
							auto probe = text_writer.label_create();
							auto next = text_writer.label_create();
							auto compare = text_writer.label_create();
							auto found = text_writer.label_create();
							ASM(CMP8mi, FE_MEM(key_base, 0, FE_NOREG,
								key_offset + type_info), IS_STRING);
							generate_raw_jump(Jump::jne, slow);
							/* right: key, left_type: hash, right_type: index,
							 * left: bucket, element: HashTable. */
							ASM(MOV64rm, right_reg,
								FE_MEM(key_base, 0, FE_NOREG, key_offset));
							ASM(MOV64rm, left_type_reg,
								FE_MEM(right_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_string, h))));
							ASM(TEST64rr, left_type_reg, left_type_reg);
							generate_raw_jump(Jump::je, slow);
							ASM(MOV32rm, right_type_reg,
								FE_MEM(element_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										HashTable, nTableMask))));
							ASM(OR32rr, right_type_reg, left_type_reg);
							ASM(MOVSXr64r32, right_type_reg, right_type_reg);
							ASM(MOV64rm, element_reg,
								FE_MEM(element_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										HashTable, arData))));
							ASM(MOV32rm, right_type_reg,
								FE_MEM(element_reg, 4, right_type_reg, 0));
							label_place(probe);
							ASM(CMP32ri, right_type_reg, HT_INVALID_IDX);
							generate_raw_jump(Jump::je, slow);
							ASM(MOV64rr, left_reg, right_type_reg);
							ASM(SHL64ri, left_reg, 5);
							ASM(ADD64rr, left_reg, element_reg);
							ASM(CMP64rm, left_type_reg,
								FE_MEM(left_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(Bucket, h))));
							generate_raw_jump(Jump::jne, next);
							ASM(CMP64rm, right_reg,
								FE_MEM(left_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(Bucket, key))));
							generate_raw_jump(Jump::je, found);
							generate_raw_jump(Jump::jmp, compare);
							label_place(next);
							ASM(MOV32rm, right_type_reg,
								FE_MEM(left_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(Bucket, val)
										+ offsetof(zval, u2.next))));
							generate_raw_jump(Jump::jmp, probe);
							/* Equal hash, another string: equal bytes match. */
							label_place(compare);
							{
								auto bytes = text_writer.label_create();
								ASM(MOV64rm, other_key_reg,
									FE_MEM(left_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											Bucket, key))));
								ASM(TEST64rr, other_key_reg, other_key_reg);
								generate_raw_jump(Jump::je, next);
								ASM(MOV64rm, key_length_reg,
									FE_MEM(other_key_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_string, len))));
								ASM(CMP64rm, key_length_reg,
									FE_MEM(right_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_string, len))));
								generate_raw_jump(Jump::jne, next);
								label_place(bytes);
								ASM(TEST64rr, key_length_reg, key_length_reg);
								generate_raw_jump(Jump::je, found);
								ASM(SUB64ri, key_length_reg, 1);
								ASM(MOVZXr32m8, key_byte_reg,
									FE_MEM(other_key_reg, 1, key_length_reg,
										static_cast<int32_t>(offsetof(
											zend_string, val))));
								ASM(MOVZXr32m8, right_type_reg,
									FE_MEM(right_reg, 1, key_length_reg,
										static_cast<int32_t>(offsetof(
											zend_string, val))));
								ASM(CMP32rr, key_byte_reg, right_type_reg);
								generate_raw_jump(Jump::jne, next);
								generate_raw_jump(Jump::jmp, bytes);
							}
							label_place(found);
							ASM(MOV64rr, element_reg, left_reg);
						}
						label_place(have_element);
						/* Operands: the element and the value. */
						const AsmReg value_base =
							value_literal ? literals_reg : frame_reg;
						ASM(MOVZXr32m8, left_type_reg,
							FE_MEM(element_reg, 0, FE_NOREG, type_info));
						ASM(MOV64rm, left_reg, FE_MEM(element_reg, 0, FE_NOREG, 0));
						ASM(MOVZXr32m8, right_type_reg,
							FE_MEM(value_base, 0, FE_NOREG,
								value_offset + type_info));
						ASM(MOV64rm, right_reg,
							FE_MEM(value_base, 0, FE_NOREG, value_offset));
						auto mixed = text_writer.label_create();
						auto store = text_writer.label_create();
						ASM(CMP32ri, left_type_reg, IS_LONG);
						generate_raw_jump(Jump::jne, mixed);
						ASM(CMP32ri, right_type_reg, IS_LONG);
						generate_raw_jump(Jump::jne, mixed);
						if (opcode == ZEND_ADD) {
							ASM(ADD64rr, left_reg, right_reg);
						} else if (opcode == ZEND_SUB) {
							ASM(SUB64rr, left_reg, right_reg);
						} else {
							ASM(IMUL64rr, left_reg, right_reg);
						}
						generate_raw_jump(Jump::jo, slow);
						ASM(MOV32ri, right_type_reg, IS_LONG);
						generate_raw_jump(Jump::jmp, store);
						label_place(mixed);
						auto to_double = [&](AsmReg type_reg, AsmReg value_reg,
								AsmReg fp_reg) {
							auto is_long = text_writer.label_create();
							auto converted = text_writer.label_create();
							ASM(CMP32ri, type_reg, IS_LONG);
							generate_raw_jump(Jump::je, is_long);
							ASM(CMP32ri, type_reg, IS_DOUBLE);
							generate_raw_jump(Jump::jne, slow);
							ASM(SSE_MOVQ_G2Xrr, fp_reg, value_reg);
							generate_raw_jump(Jump::jmp, converted);
							label_place(is_long);
							ASM(SSE_CVTSI2SD64rr, fp_reg, value_reg);
							label_place(converted);
						};
						to_double(left_type_reg, left_reg, left_fp);
						to_double(right_type_reg, right_reg, right_fp);
						if (opcode == ZEND_ADD) {
							ASM(SSE_ADDSDrr, left_fp, right_fp);
						} else if (opcode == ZEND_SUB) {
							ASM(SSE_SUBSDrr, left_fp, right_fp);
						} else {
							ASM(SSE_MULSDrr, left_fp, right_fp);
						}
						ASM(SSE_MOVQ_X2Grr, left_reg, left_fp);
						ASM(MOV32ri, right_type_reg, IS_DOUBLE);
						label_place(store);
						ASM(MOV64mr, FE_MEM(element_reg, 0, FE_NOREG, 0),
							left_reg);
						ASM(MOV32mr, FE_MEM(element_reg, 0, FE_NOREG, type_info),
							right_type_reg);
						generate_raw_jump(Jump::jmp, done);
					}
					label_place(slow);
					if (!execute_value_operation()) {
						return false;
					}
					label_place(done);
					release_spilled_regs(spilled);
					return true;
				}
			}
			if (mir.runtime_helper == ZEND_NATIVE_HELPER_VALUE_FE_FREE
					&& mir.has_value_operation
					&& zend_mir_id_is_valid(
						mir.value_operation.op1_storage_id)
					&& (mir.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_TMP
						|| mir.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_VAR)) {
				/*
				 * A foreach over an immutable array leaves a holder that
				 * needs no release; only clear it as the helper would.
				 * Iterators and counted arrays keep the runtime primitive.
				 */
				const uint64_t holder_offset =
					(uint64_t{ZEND_CALL_FRAME_SLOT}
						+ mir.value_operation.op1_storage_id)
					* sizeof(zval);
				if (holder_offset > INT32_MAX - sizeof(zval)) {
					return execute_value_operation();
				}
				const int32_t type_offset = static_cast<int32_t>(
					holder_offset + offsetof(zval, u1.type_info));
				const auto spilled = spill_before_branch(true);
				auto done = text_writer.label_create();
				auto slow = text_writer.label_create();
				ASM(CMP32mi,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						type_offset),
					IS_ARRAY);
				generate_raw_jump(Jump::jne, slow);
				ASM(MOV32mi,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						type_offset),
					IS_UNDEF);
				generate_raw_jump(Jump::jmp, done);
				label_place(slow);
				if (!execute_value_operation()) {
					return false;
				}
				label_place(done);
				release_spilled_regs(spilled);
				return true;
			}
			return execute_value_operation();
		default:
			break;
	}

	if ((record.opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
				&& record.opcode
					<= ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED)
			|| (record.opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				&& record.opcode
					<= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL)
			|| record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			|| record.opcode == ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
			|| record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME) {
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
				&& mir.fused_into_branch && !adaptor->typed_body()) {
			return fused_into_branch_placeholder();
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
				&& node.kind == Adaptor::InstKind::GuardedFast) {
			if (const int checked = type_check_inline(); checked != 0) {
				return checked > 0;
			}
			return branch_to_guarded_cold();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R
				|| ((record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_IS
						|| record.opcode
							== ZEND_MIR_OPCODE_OBJECT_FETCH_FUNC_ARG)
					&& node.kind == Adaptor::InstKind::GuardedFast)) {
			return object_property_read();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_W
				&& node.kind == Adaptor::InstKind::GuardedFast) {
			return object_property_address();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN) {
			return object_property_write();
		}
		if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R) {
			return dynamic_fetch_read();
		}
		if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_CONSTANT) {
			return fetch_constant_inline();
		}
		if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_BIND_GLOBAL) {
			return bind_global_inline();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_CONSTANT) {
			return class_constant_inline();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_INSTANCEOF) {
			return instanceof_inline();
		}
		return execute_value_operation();
	}
	switch (record.opcode) {
		case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_SEPARATE:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
			return copy_temporary_slot();
		case ZEND_MIR_OPCODE_VALUE_FREE:
			return free_temporary_slot();
		case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
			if (mir.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
				return execute_value_operation();
			}
			return copy_slot(
				mir.value_operation.op2,
				mir.value_operation.op2_storage_id,
				mir.value_operation.op1_storage_id,
				mir.value_operation.result_storage_id,
				mir.value_operation.op2.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP);
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			if (mir.value_operation.op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
				if (const int copied = copy_literal(); copied != 0) {
					return copied > 0;
				}
			}
			return copy_slot(
				mir.value_operation.op1,
				mir.value_operation.op1_storage_id,
				mir.value_operation.result_storage_id,
				ZEND_MIR_ID_INVALID,
				mir.value_operation.op1.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
					|| mir.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_VAR,
				true);
		case ZEND_MIR_OPCODE_VALUE_CONCAT:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
		case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
			if (rope_literal_inline()) {
				return true;
			}
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ROPE_END:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
			return read_array();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
			return write_array();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
			return write_array();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
			return coalesce_array();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
			return write_array();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
			return append_packed_array();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_UNSET_DIM:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
			return isset_array();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
			return long_assign_op();
		case ZEND_MIR_OPCODE_VALUE_FE_FREE:
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				if (const int freed = iterator_free_inline(); freed != 0) {
					return freed > 0;
				}
				return branch_to_guarded_cold();
			}
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
			if (mir.fused_into_branch && !adaptor->typed_body()) {
				return fused_into_branch_placeholder();
			}
			return long_binary();
		case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
			return string_length();
		case ZEND_MIR_OPCODE_VALUE_CAST:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			return slot_isset_empty();
		case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_INCDEC:
			return long_incdec();
		case ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE:
			/*
			 * The frozen source operand is an allocator-visible SSA use even
			 * when Zend's canonical-frame helper performs the semantic check.
			 * Consume it here before either eliding the proven typed-body check
			 * or emitting the ordinary helper call.
			 */
		{
			/*
			 * Like the VM, accept a register-held boxed result whose type is
			 * in the declared return mask without calling the helper, which
			 * handles coercion, references and errors.
			 */
			const bool needs_helper = !adaptor->typed_body()
				&& mir.runtime_helper != ZEND_NATIVE_HELPER_COUNT;
			const bool inline_check = needs_helper
				&& mir.has_value_operation
				&& mir.value_operation.op1.kind
					!= ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& mir.value_operation.result.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED;
			ScratchReg verified_type{this};
			bool have_type = false;
			for (IRValueRef operand : node.operands) {
				if (operand != IRValueRef{Adaptor::FRAME_VALUE}
						&& operand != IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT}) {
					auto verified = val_ref(operand);
					if (inline_check && !have_type
							&& adaptor->machine_kind(operand)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& val_parts(operand).count() == 2) {
						auto type_info = verified.part(1);
						auto type_reg = verified_type.alloc_gp();
						ASM(MOVZXr32r8, type_reg, type_info.load_to_reg());
						have_type = true;
					}
				}
			}
			if (!needs_helper) {
				/* OPcache folds a constant return into VERIFY_RETURN_TYPE
				 * with a result the RETURN reads: publish the literal. */
				const zend_mir_executable_value_ref &verify =
					mir.value_operation;
				if (!adaptor->typed_body() && mir.has_value_operation
						&& verify.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& verify.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						&& zend_mir_id_is_valid(verify.result_storage_id)) {
					const uint64_t literal_offset =
						uint64_t{verify.op1.index} * sizeof(zval);
					const uint64_t result_offset =
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ verify.result_storage_id) * sizeof(zval);
					if (literal_offset > INT32_MAX - sizeof(zval)
							|| result_offset > INT32_MAX - sizeof(zval)) {
						return false;
					}
					const AsmReg frame_reg = canonical_frame_register();
					ScratchReg literals{this};
					ScratchReg value{this};
					auto literals_reg = literals.alloc_gp();
					auto value_reg = value.alloc_gp();
					ASM(MOV64rm, literals_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_execute_data, func))));
					ASM(MOV64rm, literals_reg,
						FE_MEM(literals_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_op_array, literals))));
					ASM(MOV64rm, value_reg,
						FE_MEM(literals_reg, 0, FE_NOREG,
							static_cast<int32_t>(literal_offset)));
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(result_offset)),
						value_reg);
					ASM(MOV32rm, value_reg,
						FE_MEM(literals_reg, 0, FE_NOREG,
							static_cast<int32_t>(literal_offset
								+ offsetof(zval, u1.type_info))));
					ASM(MOV32mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(result_offset
								+ offsetof(zval, u1.type_info))),
						value_reg);
				}
				return true;
			}
			if (!have_type) {
				return execute_value_operation();
			}
			const auto spilled = spill_before_branch(true);
			auto done = text_writer.label_create();
			{
				ScratchReg mask{this};
				auto mask_reg = mask.alloc_gp();
				ASM(MOV64rm, mask_reg,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, func))));
				ASM(MOV64rm, mask_reg,
					FE_MEM(mask_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_function, common.arg_info))));
				ASM(MOV32rm, mask_reg,
					FE_MEM(mask_reg, 0, FE_NOREG,
						-static_cast<int32_t>(sizeof(zend_arg_info))
							+ static_cast<int32_t>(
								offsetof(zend_arg_info, type)
									+ offsetof(zend_type, type_mask))));
				ASM(BT32rr, mask_reg, verified_type.cur_reg());
				generate_raw_jump(Jump::jb, done);
			}
			verified_type.reset();
			if (!execute_value_operation()) {
				return false;
			}
			label_place(done);
			release_spilled_regs(spilled);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_ECHO:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_FUNC_NUM_ARGS: {
			if (node.operands.size() != 1
					|| record.representation != ZEND_MIR_REPRESENTATION_I64
					|| !mir.has_value_operation
					|| mir.value_operation.result_storage_id
						== ZEND_MIR_ID_INVALID) {
				return false;
			}
			const uint64_t result_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ mir.value_operation.result_storage_id) * sizeof(zval);
			if (result_offset > INT32_MAX - offsetof(zval, u1.type_info)) {
				return false;
			}
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_reg = frame.load_to_reg();
			ScratchReg result_value{this};
			auto result_reg = result_value.alloc_gp();
			ASM(MOV32rm, result_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, This)
							+ offsetof(zval, u2.num_args))));
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(result_offset)),
				result_reg);
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						result_offset + offsetof(zval, u1.type_info))),
				IS_LONG);
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				ASM(MOV64rr, payload_reg, result_reg);
				ASM(MOV32ri, type_info_reg, IS_LONG);
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto scalar_reg = result.alloc_reg();
				ASM(MOV64rr, scalar_reg, result_reg);
				result.set_modified();
			}
			return true;
		}
		case ZEND_MIR_OPCODE_FUNC_GET_ARGS:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_GENERATOR_CREATE:
		case ZEND_MIR_OPCODE_GENERATOR_YIELD:
		case ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM:
		case ZEND_MIR_OPCODE_GENERATOR_RETURN:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_COUNT:
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				if (const int counted = count_inline(); counted != 0) {
					return counted > 0;
				}
				return branch_to_guarded_cold();
			}
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ARRAY_KEY_EXISTS:
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				if (const int exists = key_exists_inline(); exists != 0) {
					return exists > 0;
				}
				return branch_to_guarded_cold();
			}
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_GET_TYPE:
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
		case ZEND_MIR_OPCODE_VALUE_CASE:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_COPY:
		case ZEND_MIR_OPCODE_CANONICALIZE:
		case ZEND_MIR_OPCODE_I1_TO_I64:
			return copy_result();
		case ZEND_MIR_OPCODE_STATEPOINT:
			if ((record.effects & ZEND_MIR_EFFECT_MASK(
					ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				if (node.operands.size() != 2
						|| mir.source_opline_index == UINT32_MAX) {
					return false;
				}
				/*
				 * The interrupt poll contains a target-local fast/slow branch which
				 * is invisible to TPDE's IR CFG. The slow-path call invalidates
				 * caller-saved assignments in the shared compile-time state. Spill
				 * live values before the branch so the fast path reaches the join
				 * with the same canonical copies available for later reloads.
				 */
				const auto spilled = spill_before_branch(true);
				auto done = text_writer.label_create();
				auto slow = text_writer.label_create();
				auto [context_ref, context] =
					val_ref_single(node.operands[1]);
				auto context_scratch =
					std::move(context).into_scratch();
				ScratchReg pending{this};
				auto pending_reg = pending.alloc_gp();
				/* The context always points at EG(vm_interrupt); a pending
				 * interrupt is handled out of the hot code. */
				ASM(MOV64rm, pending_reg,
					FE_MEM(context_scratch.cur_reg(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_interrupt))));
				ASM(CMP8mi, FE_MEM(pending_reg, 0, FE_NOREG, 0), 0);
				generate_raw_jump(Jump::jne, slow);
				const bool cold_poll = !text_writer.in_cold_area();
				if (cold_poll) {
					cold_begin();
				} else {
					generate_raw_jump(Jump::jmp, done);
				}
				label_place(slow);
				context_scratch.reset();
				pending.reset();
				if (!emit_materializations(instruction, true)) {
					return false;
				}
				{
					tpde::x64::CCAssignerSysV assigner;
					CallBuilder builder{*this, assigner};
					builder.add_arg(CallArg{node.operands[0]});
					add_const_arg(builder, mir.source_opline_index, 4);
					builder.call(
						runtime_symbol(ZEND_NATIVE_HELPER_INTERRUPT_POLL));
				}
				generate_raw_jump(Jump::jmp, done);
				if (cold_poll) {
					cold_end();
				}
				label_place(done);
				release_spilled_regs(spilled);
				return true;
			}
			[[fallthrough]];
		case ZEND_MIR_OPCODE_SCALAR_DROP:
			consume_operands(node);
			return true;
		case ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_add_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_sub_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_mul_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_BIT_OR:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_or_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_BIT_AND:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_and_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_BIT_XOR:
		case ZEND_MIR_OPCODE_I1_XOR:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_xor_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_BIT_NOT: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto source_reg = source.load_to_reg();
			auto result_reg = result.alloc_try_reuse(source);
			if (result_reg != source_reg) mov(result_reg, source_reg, 8);
			ASM(NOT64r, result_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_I1_NOT:
		case ZEND_MIR_OPCODE_I64_TO_I1: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto source_reg = source.load_to_reg();
			ASM(TEST64rr, source_reg, source_reg);
			auto result_reg = result.alloc_reg();
			generate_raw_set(record.opcode == ZEND_MIR_OPCODE_I1_NOT
				? Jump::je : Jump::jne, result_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_I64_EQ:
		case ZEND_MIR_OPCODE_I1_EQ:
			if (can_fuse_compare_branch()) {
				return integer_compare(Jump::je);
			}
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_eq_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_LT:
			if (can_fuse_compare_branch()) {
				return integer_compare(Jump::jl);
			}
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_lt_i64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_LE:
			if (can_fuse_compare_branch()) {
				return integer_compare(Jump::jle);
			}
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_le_i64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_CMP: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			auto [result_ref, result] = result_ref_single(node.result);
			ASM(CMP64rr, left.load_to_reg(), right.load_to_reg());
			ScratchReg less{this};
			ScratchReg greater{this};
			auto less_reg = less.alloc_gp();
			auto greater_reg = greater.alloc_gp();
			generate_raw_set(Jump::jl, less_reg);
			generate_raw_set(Jump::jg, greater_reg);
			ASM(SUB64rr, greater_reg, less_reg);
			result.set_value(std::move(greater));
			return true;
		}
		case ZEND_MIR_OPCODE_I64_MOD_NONZERO: {
			ScratchReg rax{this};
			ScratchReg rdx{this};
			ScratchReg divisor{this};
			auto ax = rax.alloc_specific(tpde::x64::AsmReg::AX);
			auto dx = rdx.alloc_specific(tpde::x64::AsmReg::DX);
			auto cx = divisor.alloc_specific(tpde::x64::AsmReg::CX);
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			mov(ax, left.load_to_reg(), 8);
			mov(cx, right.load_to_reg(), 8);
			ASM(CQO);
			ASM(IDIV64r, cx);
			auto [result_ref, result] = result_ref_single(node.result);
			result.set_value(std::move(rdx));
			return true;
		}
		case ZEND_MIR_OPCODE_I64_SHL_CHECKED:
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED: {
			ScratchReg count{this};
			auto cx = count.alloc_specific(tpde::x64::AsmReg::CX);
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			mov(cx, right.load_to_reg(), 8);
			auto [result_ref, result] = result_ref_single(node.result);
			auto left_reg = left.load_to_reg();
			auto result_reg = result.alloc_try_reuse(left);
			if (result_reg != left_reg) mov(result_reg, left_reg, 8);
			if (record.opcode == ZEND_MIR_OPCODE_I64_SHL_CHECKED) {
				ASM(SHL64rr, result_reg, cx);
			} else {
				ASM(SAR64rr, result_reg, cx);
			}
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_F64_ADD:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_add_f64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_F64_SUB:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_sub_f64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_F64_MUL:
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_mul_f64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_F64_EQ:
		case ZEND_MIR_OPCODE_F64_LT:
		case ZEND_MIR_OPCODE_F64_LE:
			return floating_compare(record.opcode);
		case ZEND_MIR_OPCODE_F64_CMP: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			auto [result_ref, result] = result_ref_single(node.result);
			ASM(SSE_UCOMISDrr, left.load_to_reg(), right.load_to_reg());
			ScratchReg less{this};
			ScratchReg greater{this};
			auto less_reg = less.alloc_gp();
			auto greater_reg = greater.alloc_gp();
			generate_raw_set(Jump::jb, less_reg);
			generate_raw_set(Jump::ja, greater_reg);
			ASM(SUB64rr, greater_reg, less_reg);
			result.set_value(std::move(greater));
			return true;
		}
		case ZEND_MIR_OPCODE_I64_TO_F64:
		case ZEND_MIR_OPCODE_I1_TO_F64: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(SSE_CVTSI2SD64rr, result_reg, source.load_to_reg());
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_F64_TO_I64_CHECKED: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(SSE_CVTTSD2SI64rr, result_reg, source.load_to_reg());
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_F64_TO_I1: {
			auto [source_ref, source] = unary();
			ScratchReg bits{this};
			auto bits_reg = bits.alloc_gp();
			ASM(SSE_MOVQ_X2Grr, bits_reg, source.load_to_reg());
			ASM(SHL64ri, bits_reg, 1);
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			generate_raw_set(Jump::jne, result_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_BRANCH:
			generate_uncond_branch(adaptor->block_succs(
				IRBlockRef{node.control_block})[0]);
			return true;
		case ZEND_MIR_OPCODE_COND_BRANCH: {
			auto [condition_ref, condition] = unary();
			auto condition_reg = condition.load_to_reg();
			ASM(TEST64rr, condition_reg, condition_reg);
			const auto &successors = adaptor->block_succs(
				IRBlockRef{node.control_block});
			generate_cond_branch(Jump::jne,
				successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH: {
			zend_tpde_multi_branch layout;
			if (!zend_tpde_multi_branch_at(
						adaptor->plan(), mir, record, &layout)
					|| node.operands.size() != 1
					|| layout.operand_offset > INT32_MAX) {
				return false;
			}
			const zend_tpde_plan *plan = adaptor->plan();
			std::vector<IRBlockRef> targets;
			std::vector<tpde::Label> case_labels;
			targets.reserve(layout.successor_count);
			case_labels.reserve(layout.case_count);
			for (uint32_t i = 0; i < layout.successor_count; ++i) {
				zend_mir_block_id target_id;
				if (!zend_tpde_block_successor_at(
						plan, record.block_id, i, &target_id)) {
					return false;
				}
				IRBlockRef target = adaptor->block_ref(target_id);
				if (target == Adaptor::INVALID_BLOCK_REF) {
					return false;
				}
				targets.push_back(target);
			}
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_scratch = frame_register(std::move(frame));
			if (layout.constant_successor != UINT32_MAX) {
				generate_uncond_branch(targets[layout.constant_successor]);
				return true;
			}
			for (uint32_t i = 0; i < layout.case_count; ++i) {
				case_labels.push_back(text_writer.label_create());
			}
			auto default_label = text_writer.label_create();
			auto fallback_label = layout.source_opcode == ZEND_MATCH
				? default_label : text_writer.label_create();
			auto long_label = text_writer.label_create();
			auto string_label = text_writer.label_create();
			/* MATCH warns about an undefined CV; type inference often
			 * proves it defined. */
			const uint32_t match_position =
				mir.value_operation.source_position_id;
			const bool check_undefined_cv =
				layout.source_opcode == ZEND_MATCH
				&& (mir.value_operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT
					|| mir.value_operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SSA)
				&& mir.value_operation.op1.slot_kind
					== ZEND_MIR_SOURCE_SLOT_CV
				&& (plan->source_opcodes == nullptr
					|| match_position >= plan->source_opcode_count
					|| plan->source_opcodes[match_position].op1_may_be
						== UINT32_MAX
					|| (plan->source_opcodes[match_position].op1_may_be
						& MAY_BE_UNDEF) != 0);
			if (check_undefined_cv) {
				ValuePart frame_argument{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				frame_argument.set_value(this, take_frame(frame_scratch));
				if (!execute_value_operation_with(
						&frame_argument,
						ZEND_NATIVE_HELPER_VALUE_CHECK_VAR,
						ZEND_CHECK_VAR)) {
					return false;
				}
			}
			ScratchReg slot{this};
			ScratchReg type{this};
			ScratchReg value{this};
			ScratchReg probe{this};
			ScratchReg constant{this};
			auto slot_reg = slot.alloc_gp();
			auto type_reg = type.alloc_gp();
			auto value_reg = value.alloc_gp();
			auto probe_reg = probe.alloc_gp();
			auto constant_reg = constant.alloc_gp();
			ASM(MOV64rr, slot_reg,
				check_undefined_cv
					? canonical_frame_register() : frame_scratch.cur_reg());
			ASM(ADD64ri, slot_reg,
				static_cast<int32_t>(layout.operand_offset));
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			auto spilled = spill_before_branch();
			begin_branch_region();
			auto dereferenced = text_writer.label_create();
			ASM(CMP32ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::jne, dereferenced);
			ASM(MOV64rm, slot_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(ADD64ri, slot_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
			ASM(MOVZXr32m8, type_reg,
				FE_MEM(slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			label_place(dereferenced);
			if (layout.source_opcode != ZEND_SWITCH_STRING) {
				ASM(CMP32ri, type_reg, IS_LONG);
				generate_raw_jump(Jump::je, long_label);
			}
			if (layout.source_opcode != ZEND_SWITCH_LONG) {
				ASM(CMP32ri, type_reg, IS_STRING);
				generate_raw_jump(Jump::je, string_label);
			}
			generate_raw_jump(Jump::jmp, fallback_label);

			label_place(long_label);
			ASM(MOV64rm, value_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			emit_integer_dispatch(
				layout.cases, layout.case_count, case_labels,
				value_reg, constant_reg, default_label);

			label_place(string_label);
			ASM(MOV64rm, value_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			for (uint32_t case_index = 0;
					case_index < layout.case_count; ++case_index) {
				const zend_tpde_multi_branch_case &branch_case =
					layout.cases[case_index];
				if (branch_case.string_key != nullptr) {
					auto next_case = text_writer.label_create();
					const uint64_t length = branch_case.string_length;
					ASM(MOV64rm, probe_reg,
						FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_string, len))));
					materialize_constant(
						&length, tpde::x64::PlatformConfig::GP_BANK,
						8, constant_reg);
					ASM(CMP64rr, probe_reg, constant_reg);
					generate_raw_jump(Jump::jne, next_case);
					size_t offset = 0;
					while (offset < branch_case.string_length) {
						const uint32_t width =
							branch_case.string_length - offset >= 8 ? 8
							: branch_case.string_length - offset >= 4 ? 4
							: branch_case.string_length - offset >= 2 ? 2 : 1;
						const size_t byte_offset =
							offsetof(zend_string, val) + offset;
						if (byte_offset > INT32_MAX) {
							return false;
						}
						uint64_t expected = 0;
						memcpy(&expected, branch_case.string_key + offset,
							width);
						switch (width) {
							case 8:
								ASM(MOV64rm, probe_reg,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(byte_offset)));
								break;
							case 4:
								ASM(MOV32rm, probe_reg,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(byte_offset)));
								break;
							case 2:
								ASM(MOVZXr32m16, probe_reg,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(byte_offset)));
								break;
							default:
								ASM(MOVZXr32m8, probe_reg,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(byte_offset)));
								break;
						}
						materialize_constant(
							&expected, tpde::x64::PlatformConfig::GP_BANK,
							width, constant_reg);
						ASM(CMP64rr, probe_reg, constant_reg);
						generate_raw_jump(Jump::jne, next_case);
						offset += width;
					}
					generate_raw_jump(
						Jump::jmp, case_labels[case_index]);
					label_place(next_case);
				}
			}
			generate_raw_jump(Jump::jmp, default_label);

			for (uint32_t i = 0; i < case_labels.size(); ++i) {
				label_place(case_labels[i]);
				generate_branch_to_block(
					Jump::jmp, targets[i], false, false);
			}
			label_place(default_label);
			generate_branch_to_block(Jump::jmp,
				targets[layout.successor_count
					- (layout.source_opcode == ZEND_MATCH ? 1 : 2)],
				false, false);
			if (layout.source_opcode != ZEND_MATCH) {
				label_place(fallback_label);
				generate_branch_to_block(
					Jump::jmp, targets.back(), false, false);
			}
			end_branch_region();
			release_spilled_regs(spilled);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
		case ZEND_MIR_OPCODE_ITERATOR_BRANCH:
		case ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH:
		case ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH: {
			const bool register_machine_condition =
				record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& node.operands.size() == 1
				&& node.operands[0]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				&& (adaptor->machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_BOOL
					|| adaptor->machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_I64);
			const bool register_boxed_condition =
				record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& node.operands.size() == 2
				&& node.operands[0]
					== IRValueRef{Adaptor::FRAME_VALUE}
				&& adaptor->machine_kind(node.operands[1])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
			/* A fused comparison: the branch evaluates it (see
			 * freeze_fused_compare_branches). */
			const bool fused = record.opcode
					== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& mir.fused_compare_plus_one != 0
				&& !adaptor->typed_body()
				&& mir.fused_compare_plus_one
					<= adaptor->plan()->instruction_count;
			const zend_tpde_instruction *fused_compare = fused
				? &adaptor->plan()->instructions[
					mir.fused_compare_plus_one - 1]
				: nullptr;
			const bool register_condition =
				register_boxed_condition && !fused;
			/* The helper reads the condition from its slot. A CV that may
			 * hold a reference already has its value there, inside the
			 * reference, which a store of the loaded value would replace. */
			const bool publish_boxed_condition = register_boxed_condition
				&& !(mir.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV
					&& !mir.source_op1_reference_free);
			if ((node.operands.size() != 1 && !register_boxed_condition)
					|| !mir.has_value_operation) {
				return false;
			}
			if (record.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				zend_tpde_array_iterator_reset reset_layout;

				if (zend_tpde_array_iterator_reset_at(
							mir, &reset_layout, true)
						&& reset_layout.source_literal_index
							<= INT32_MAX / sizeof(zval)
						&& reset_layout.source_offset <= INT32_MAX
						&& reset_layout.holder_offset <= INT32_MAX - 16) {
					const int32_t decision_slot =
						inst_stack_slot(sizeof(uint32_t));
					if (decision_slot >= 0) {
						return false;
					}
					auto slow = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = frame_register(std::move(frame));
					auto spilled = spill_before_branch();
					release_spilled_regs(spilled);
					auto frame_reg = frame_scratch.cur_reg();
					ScratchReg array{this};
					ScratchReg type_info{this};
					ScratchReg high_word{this};
					ScratchReg literal{this};
					auto array_reg = array.alloc_gp();
					auto type_info_reg = type_info.alloc_gp();
					auto high_word_reg = high_word.alloc_gp();
					/* OPcache propagates a literal array into FE_RESET_R.
					 * Address it through the executing op array. */
					AsmReg source_reg = frame_reg;
					int32_t source_offset =
						static_cast<int32_t>(reset_layout.source_offset);
					if (reset_layout.source_literal) {
						source_reg = literal.alloc_gp();
						ASM(MOV64rm, source_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, func))));
						ASM(MOV64rm, source_reg,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_op_array, literals))));
						source_offset = static_cast<int32_t>(
							reset_layout.source_literal_index * sizeof(zval));
					}
					auto copy = text_writer.label_create();

					/* An array zval without the refcounted flag, such as an
					 * immutable literal, is copied without a reference. */
					ASM(MOV32rm, type_info_reg,
						FE_MEM(source_reg, 0, FE_NOREG,
							source_offset + static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
					ASM(MOV64rm, array_reg,
						FE_MEM(source_reg, 0, FE_NOREG, source_offset));
					ASM(CMP32ri, type_info_reg, IS_ARRAY);
					generate_raw_jump(Jump::je, copy);
					ASM(AND32ri, type_info_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_info_reg, IS_ARRAY);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV32rm, type_info_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, u.type_info))));
					ASM(TEST32ri, type_info_reg, GC_IMMUTABLE);
					generate_raw_jump(Jump::jne, slow);
					if (!reset_layout.source_temporary) {
						/* A temporary's reference moves into the holder. */
						ASM(ADD32mi,
							FE_MEM(array_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))),
							1);
					}
					label_place(copy);
					ASM(MOV64rm, high_word_reg,
						FE_MEM(source_reg, 0, FE_NOREG, source_offset + 8));
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(reset_layout.holder_offset)),
						array_reg);
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								reset_layout.holder_offset + 8)),
						high_word_reg);
					ASM(MOV32mi,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(reset_layout.holder_offset
								+ offsetof(zval, u2))),
						0);
					ASM(MOV32mi,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot),
						ZEND_NATIVE_ITERATOR_NEXT);
					generate_raw_jump(Jump::jmp, branch);
					array.reset();
					type_info.reset();
					high_word.reset();
					literal.reset();
					label_place(slow);

					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, take_frame(frame_scratch));
					builder.add_arg(std::move(frame_argument),
						tpde::CCAssignment{});
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					call_value_operation(builder, operation,
						mir.runtime_helper);
					ValuePart decision{
						tpde::x64::PlatformConfig::GP_BANK, 4};
					builder.add_ret(decision, tpde::CCAssignment{});
					auto decision_reg = decision.cur_reg_or_load(this);
					emit_decision_exception_check(decision_reg, mir.exception_block_id,
						[&] { decision.reset(this); });
					ASM(MOV32mr,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot),
						decision_reg);
					decision.reset(this);
					label_place(branch);
					const auto &successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					ScratchReg branch_decision{this};
					auto branch_decision_reg = branch_decision.alloc_gp();
					ASM(MOV32rm, branch_decision_reg,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot));
					ASM(TEST32rr, branch_decision_reg, branch_decision_reg);
					generate_cond_branch(
						Jump::jne, successors[0], successors[1]);
					return true;
				}

				zend_tpde_packed_iterator_fetch layout;

				if (zend_tpde_packed_iterator_fetch_at(mir, &layout, true)
						&& layout.holder_offset <= INT32_MAX
						&& layout.destination_offset <= INT32_MAX
						&& layout.key_offset <= INT32_MAX
						&& (!layout.has_key || !node.has_result)) {
					if (node.has_result
							&& !((adaptor->machine_kind(node.result)
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
								&& val_parts(node.result).count() == 2)
								|| (layout.destination_scalar_only
									&& adaptor->machine_kind(node.result)
										== ZEND_TPDE_MACHINE_VALUE_I64
									&& val_parts(node.result).count() == 1))) {
						return false;
					}
					const int32_t decision_slot =
						inst_stack_slot(sizeof(uint32_t));
					if (decision_slot >= 0) {
						return false;
					}
					auto slow = text_writer.label_create();
					auto end = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = frame_register(std::move(frame));
					auto frame_reg = frame_scratch.cur_reg();
					/* Without a result or edge moves, each path leaves for
					 * its successor directly instead of merging a decision:
					 * the next element falls through to the loop body, the
					 * end jumps to the exit and the helper path, out of the
					 * hot code, branches on its returned decision. All values
					 * are spilled first so that every exit has one state. */
					const auto &fetch_successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					const bool direct_exits = !node.has_result
						&& fetch_successors.size() == 2
						&& fetch_successors[0] != fetch_successors[1]
						&& !branch_needs_split(fetch_successors[0])
						&& !branch_needs_split(fetch_successors[1]);
					auto exit_to = [&](Jump jump, uint32_t successor,
							bool last_inst) {
						begin_branch_region();
						generate_branch_to_block(jump,
							fetch_successors[successor], false, last_inst);
						end_branch_region();
					};
					if (direct_exits) {
						release_spilled_regs(spill_before_branch(true));
					}
					ScratchReg type{this};
					ScratchReg array{this};
					ScratchReg position{this};
					ScratchReg limit{this};
					ScratchReg element{this};
					ScratchReg value{this};
					auto type_reg = type.alloc_gp();
					auto array_reg = array.alloc_gp();
					auto position_reg = position.alloc_gp();
					auto limit_reg = limit.alloc_gp();
					auto element_reg = element.alloc_gp();
					auto value_reg = value.alloc_gp();
					ScratchReg key_payload{this};
					ScratchReg key_type{this};
					auto key_payload_reg = key_payload.alloc_gp();
					auto key_type_reg = key_type.alloc_gp();
					auto element_ready = text_writer.label_create();

					ASM(MOVZXr32m8, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset
								+ offsetof(zval, u1.type_info))));
					ASM(CMP32ri, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rm, array_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset)));
					ASM(MOV32rm, type_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(HashTable, u))));
					ASM(TEST32ri, type_reg, HASH_FLAG_PACKED);
					auto packed_iteration = text_writer.label_create();
					generate_raw_jump(Jump::jne, packed_iteration);
					/* A hash: the next used bucket from the position on. */
					{
						auto scan = text_writer.label_create();
						auto used = text_writer.label_create();
						auto string_key = text_writer.label_create();
						ASM(MOV32rm, limit_reg,
							FE_MEM(array_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(HashTable, nNumUsed))));
						ASM(MOV32rm, position_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(layout.holder_offset
									+ offsetof(zval, u2.fe_pos))));
						label_place(scan);
						ASM(CMP32rr, position_reg, limit_reg);
						if (direct_exits) {
							exit_to(Jump::jae, 1, false);
						} else {
							generate_raw_jump(Jump::jae, end);
						}
						ASM(MOV64rr, element_reg, position_reg);
						ASM(SHL64ri, element_reg, 5);
						ASM(ADD64rm, element_reg,
							FE_MEM(array_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(HashTable, arData))));
						ASM(CMP8mi,
							FE_MEM(element_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zval, u1.type_info))),
							IS_UNDEF);
						generate_raw_jump(Jump::jne, used);
						ASM(ADD32ri, position_reg, 1);
						generate_raw_jump(Jump::jmp, scan);
						label_place(used);
						if (layout.has_key) {
							ASM(MOV64rm, key_payload_reg,
								FE_MEM(element_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(Bucket, key))));
							ASM(TEST64rr, key_payload_reg, key_payload_reg);
							generate_raw_jump(Jump::jne, string_key);
							ASM(MOV64rm, key_payload_reg,
								FE_MEM(element_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(Bucket, h))));
							ASM(MOV32ri, key_type_reg, IS_LONG);
							generate_raw_jump(Jump::jmp, element_ready);
							/* A string key: interned or counted. */
							label_place(string_key);
							ASM(MOV32ri, key_type_reg, IS_STRING);
							ASM(TEST32mi,
								FE_MEM(key_payload_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, u))),
								IS_STR_INTERNED << GC_FLAGS_SHIFT);
							generate_raw_jump(Jump::jne, element_ready);
							ASM(MOV32ri, key_type_reg, IS_STRING_EX);
							generate_raw_jump(Jump::jmp, element_ready);
						} else {
							generate_raw_jump(Jump::jmp, element_ready);
						}
					}
					label_place(packed_iteration);
					ASM(MOV32rm, limit_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(HashTable, nNumUsed))));
					ASM(MOV32rm, type_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(HashTable, nNumOfElements))));
					ASM(CMP32rr, limit_reg, type_reg);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV32rm, position_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset
								+ offsetof(zval, u2.fe_pos))));
					ASM(CMP32rr, position_reg, limit_reg);
					if (direct_exits) {
						exit_to(Jump::jae, 1, false);
					} else {
						generate_raw_jump(Jump::jae, end);
					}

					ASM(MOV64rm, element_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(HashTable, arPacked))));
					ASM(SHL64ri, position_reg, 4);
					ASM(ADD64rr, element_reg, position_reg);
					ASM(SHR64ri, position_reg, 4);
					if (layout.has_key) {
						ASM(MOV64rr, key_payload_reg, position_reg);
						ASM(MOV32ri, key_type_reg, IS_LONG);
					}
					label_place(element_ready);
					ASM(MOV32rm, type_reg,
						FE_MEM(element_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
					ASM(MOV64rm, value_reg,
						FE_MEM(element_reg, 0, FE_NOREG, 0));
					auto value_owned = text_writer.label_create();
					if (layout.destination_scalar_only) {
						ASM(CMP8ri, type_reg, IS_LONG);
						generate_raw_jump(Jump::jne, slow);
					} else {
						/* FE_FETCH_R copies like ZVAL_COPY_OR_DUP; a
						 * reference, an indirect slot or a persistent
						 * counted value keeps the helper. */
						/* IS_REFERENCE, IS_INDIRECT and every type above. */
						ASM(CMP8ri, type_reg, IS_REFERENCE);
						generate_raw_jump(Jump::jae, slow);
						ASM(TEST32ri, type_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, value_owned);
						ASM(TEST32mi,
							FE_MEM(value_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, u.type_info))),
							GC_PERSISTENT);
						generate_raw_jump(Jump::jne, slow);
					}
					label_place(value_owned);
					if (!layout.destination_scalar_only) {
						/* The loop variable usually holds the previous element,
						 * which the array still owns: overwrite it inline unless
						 * it is a reference, the last owner or a new GC root
						 * (zend_native_cv_overwritable()). An uncounted value
						 * falls through; a counted one is checked and released
						 * out of line, after every other guard. */
						const int32_t destination =
							static_cast<int32_t>(layout.destination_offset);
						auto overwritable = text_writer.label_create();
						auto counted = text_writer.label_create();
						ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
							destination + static_cast<int32_t>(
								offsetof(zval, u1.v.type_flags))), 0);
						generate_raw_jump(Jump::jne, counted);
						const bool cold_counted = !text_writer.in_cold_area();
						if (cold_counted) {
							cold_begin();
						} else {
							generate_raw_jump(Jump::jmp, overwritable);
						}
						label_place(counted);
						ASM(CMP8mi, FE_MEM(frame_reg, 0, FE_NOREG,
							destination + static_cast<int32_t>(
								offsetof(zval, u1.v.type))), IS_REFERENCE);
						generate_raw_jump(Jump::je, slow);
						ASM(MOV64rm, limit_reg,
							FE_MEM(frame_reg, 0, FE_NOREG, destination));
						ASM(CMP32mi, FE_MEM(limit_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, refcount))), 1);
						generate_raw_jump(Jump::je, slow);
						ASM(TEST32mi, FE_MEM(limit_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, u.type_info))),
							static_cast<int32_t>(GC_INFO_MASK
								| (GC_NOT_COLLECTABLE << GC_FLAGS_SHIFT)));
						generate_raw_jump(Jump::je, slow);
						/* Another owner keeps the old value. */
						ASM(SUB32mi, FE_MEM(limit_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, refcount))), 1);
						generate_raw_jump(Jump::jmp, overwritable);
						if (cold_counted) {
							cold_end();
						}
						label_place(overwritable);
					}

					/* All guards precede the first observable mutation. */
					ASM(ADD32ri, position_reg, 1);
					ASM(MOV32mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset
								+ offsetof(zval, u2.fe_pos))),
						position_reg);
					if (!layout.destination_scalar_only) {
						auto value_counted = text_writer.label_create();
						ASM(TEST32ri, type_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, value_counted);
						ASM(ADD32mi,
							FE_MEM(value_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))),
							1);
						label_place(value_counted);
					}
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.destination_offset)),
						value_reg);
					ASM(MOV32mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.destination_offset
								+ offsetof(zval, u1.type_info))),
						type_reg);
					if (layout.has_key) {
						auto key_stored = text_writer.label_create();
						ASM(MOV64mr,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(layout.key_offset)),
							key_payload_reg);
						ASM(MOV32mr,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(layout.key_offset
									+ offsetof(zval, u1.type_info))),
							key_type_reg);
						ASM(CMP32ri, key_type_reg, IS_STRING_EX);
						generate_raw_jump(Jump::jne, key_stored);
						ASM(ADD32mi,
							FE_MEM(key_payload_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))),
							1);
						label_place(key_stored);
					}
					if (direct_exits) {
						exit_to(Jump::jmp, 0, true);
						cold_begin();
					} else {
						ASM(MOV32mi,
							FE_MEM(FE_BP, 0, FE_NOREG, decision_slot), 1);
						generate_raw_jump(Jump::jmp, branch);

						label_place(end);
						ASM(MOV32mi,
							FE_MEM(FE_BP, 0, FE_NOREG, decision_slot), 0);
						generate_raw_jump(Jump::jmp, branch);
					}

					label_place(slow);
					type.reset();
					array.reset();
					position.reset();
					limit.reset();
					element.reset();
					value.reset();
					key_payload.reset();
					key_type.reset();
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, take_frame(frame_scratch));
					builder.add_arg(std::move(frame_argument),
						tpde::CCAssignment{});
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					call_value_operation(builder, operation,
						mir.runtime_helper);
					ValuePart decision{
						tpde::x64::PlatformConfig::GP_BANK};
					builder.add_ret(decision, tpde::CCAssignment{});
					auto decision_reg = decision.cur_reg_or_load(this);
					emit_decision_exception_check(decision_reg, mir.exception_block_id,
						[&] { decision.reset(this); });
					if (direct_exits) {
						ASM(TEST32rr, decision_reg, decision_reg);
						decision.reset(this);
						exit_to(Jump::jne, 0, false);
						exit_to(Jump::jmp, 1, false);
						cold_end();
						return true;
					}
					ASM(MOV32mr,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot),
						decision_reg);
					decision.reset(this);

					label_place(branch);
					if (node.has_result) {
						if (adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_I64) {
							auto [result_ref, result] =
								result_ref_single(node.result);
							auto result_reg = result.alloc_reg();
							ASM(MOV64rm, result_reg,
								FE_MEM(canonical_frame_register(), 0,
									FE_NOREG, static_cast<int32_t>(
										layout.destination_offset)));
							result.set_modified();
						} else {
							auto result = result_ref(node.result);
							const ValueParts parts = val_parts(node.result);
							for (uint32_t part = 0; part < parts.count(); ++part) {
								auto value_part = result.part(part);
								auto value_reg = value_part.alloc_reg();
								const zend_tpde_machine_part_role role =
									parts.representation.parts[part].semantic_role;
								if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
									ASM(MOV64rm, value_reg,
										FE_MEM(canonical_frame_register(), 0,
											FE_NOREG, static_cast<int32_t>(
												layout.destination_offset)));
								} else if (role
										== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
									ASM(MOV32rm, value_reg,
										FE_MEM(canonical_frame_register(), 0,
											FE_NOREG, static_cast<int32_t>(
												layout.destination_offset
												+ offsetof(zval, u1.type_info))));
								} else {
									return false;
								}
								value_part.set_modified();
							}
						}
					}
					const auto &successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					ScratchReg branch_decision{this};
					auto branch_decision_reg = branch_decision.alloc_gp();
					ASM(MOV32rm, branch_decision_reg,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot));
					ASM(TEST32rr,
						branch_decision_reg, branch_decision_reg);
					generate_cond_branch(
						Jump::jne, successors[0], successors[1]);
					return true;
				}
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
				if (register_machine_condition) {
					auto [condition_ref, condition] =
						val_ref_single(node.operands[0]);
					auto condition_reg = condition.load_to_reg();
					const auto &successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					ASM(TEST64rr, condition_reg, condition_reg);
					generate_cond_branch(
						Jump::jne, successors[0], successors[1]);
					return true;
				}
				zend_tpde_value_condition layout;
				bool have_condition_layout =
					zend_tpde_value_condition_at(mir, &layout)
					&& layout.operand_offset <= INT32_MAX;
				/* A JMPZ/JMPNZ on a temporary in its frame slot tests it
				 * inline; a counted value keeps the helper, which releases
				 * the temporary. */
				const bool frame_temporary_condition =
					!register_boxed_condition
					&& node.operands.size() == 1
					&& mir.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP
					&& (mir.value_operation.source_opcode == ZEND_JMPZ
						|| mir.value_operation.source_opcode == ZEND_JMPNZ);
				/* ?? and &&/|| of a temporary or CV in its slot, or of a
				 * temporary held in registers, whose result lives in its
				 * slot only. */
				const bool result_condition = !node.has_result
					&& (mir.value_operation.source_opcode == ZEND_COALESCE
						|| mir.value_operation.source_opcode == ZEND_JMPZ_EX
						|| mir.value_operation.source_opcode == ZEND_JMPNZ_EX)
					&& (mir.value_operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| mir.value_operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& (register_boxed_condition
						? mir.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_TMP
						: node.operands.size() == 1
							&& (mir.value_operation.op1.slot_kind
									== ZEND_MIR_SOURCE_SLOT_TMP
								|| mir.value_operation.op1.slot_kind
									== ZEND_MIR_SOURCE_SLOT_CV))
					&& (mir.value_operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_SLOT
						|| mir.value_operation.result.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& mir.value_operation.result.slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP;
				if (!have_condition_layout
						&& (register_boxed_condition || fused
							|| frame_temporary_condition
							|| result_condition)) {
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					const bool has_result =
						operation.source_opcode == ZEND_JMPZ_EX
						|| operation.source_opcode == ZEND_JMPNZ_EX
						|| operation.source_opcode == ZEND_COALESCE;
					const bool supported_opcode =
						operation.source_opcode == ZEND_JMPZ
						|| operation.source_opcode == ZEND_JMPNZ
						|| ((operation.source_opcode == ZEND_JMPZ_EX
								|| operation.source_opcode == ZEND_JMPNZ_EX
								|| operation.source_opcode == ZEND_COALESCE)
							&& !fused && result_condition);
					const uint64_t operand_offset =
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.op1_storage_id) * sizeof(zval);
					const uint64_t result_offset = has_result
						? (uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.result_storage_id) * sizeof(zval)
						: 0;
					if (supported_opcode
							&& zend_mir_id_is_valid(
								operation.op1_storage_id)
							&& (!has_result || zend_mir_id_is_valid(
								operation.result_storage_id))
							&& operand_offset
								<= INT32_MAX - sizeof(zval)
							&& result_offset
								<= INT32_MAX - sizeof(zval)) {
						layout.operand_offset =
							static_cast<uint32_t>(operand_offset);
						layout.result_offset =
							static_cast<uint32_t>(result_offset);
						layout.source_opcode = operation.source_opcode;
						layout.has_result = has_result;
						have_condition_layout = true;
					}
				}

				if (!have_condition_layout && register_boxed_condition) {
					/*
					 * JMP_NULL, COALESCE, JMP_SET, and ASSERT_CHECK retain
					 * opcode-specific branch and result semantics in the native
					 * helper. Publish the register-authoritative operand to its
					 * canonical frame slot before that helper observes it.
					 */
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					const uint64_t operand_offset =
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.op1_storage_id) * sizeof(zval);
					if (!zend_mir_id_is_valid(operation.op1_storage_id)
							|| operand_offset > INT32_MAX - sizeof(zval)) {
						return false;
					}
					auto boxed = val_ref(node.operands[1]);
					const ValueParts parts = val_parts(node.operands[1]);
					if (parts.count() != 2) {
						return false;
					}
					if (publish_boxed_condition) {
						auto payload = boxed.part(0);
						auto type_info = boxed.part(1);
						ASM(MOV64mr,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(operand_offset)),
							payload.load_to_reg());
						ASM(MOV32mr,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(operand_offset
									+ offsetof(zval, u1.type_info))),
							type_info.load_to_reg());
					}
				}

				if (fused && !have_condition_layout) {
					return false;
				}
				if (have_condition_layout) {
					/*
					 * The truthiness fast path and the helper slow path are
					 * target-local branches invisible to TPDE's CFG, and only the
					 * slow path calls. Publish every live assignment first so the
					 * join reloads the same value on either path; on x86-64 few
					 * values survive a call in callee-saved registers.
					 */
					(void) spill_before_branch(true);
					const int32_t decision_slot =
						inst_stack_slot(sizeof(uint32_t));
					if (decision_slot >= 0) {
						return false;
					}
					auto slow = text_writer.label_create();
					auto truthy = text_writer.label_create();
					auto falsey = text_writer.label_create();
					auto fast_ready = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = frame_register(std::move(frame));
					auto frame_reg = frame_scratch.cur_reg();
					ScratchReg type{this};
					ScratchReg value{this};
					auto type_reg = type.alloc_gp();
					auto value_reg = value.alloc_gp();
					if (fused) {
						/* The comparison defined only a placeholder, and
						 * the branch reads no boundary operand. */
						if (register_boxed_condition) {
							auto placeholder = val_ref(node.operands[1]);
							(void) placeholder;
						}
						for (size_t index = node.operands.size();
								index < node.liveness_operands.size();
								++index) {
							if (materialized_operand(instruction, index)) {
								continue;
							}
							auto boundary =
								val_ref(node.liveness_operands[index]);
							(void) boundary;
						}
					}
					std::optional<ValueRef> boxed_condition;
					std::optional<ValuePartRef> boxed_payload;
					std::optional<ValuePartRef> boxed_type_info;
					AsmReg boxed_payload_reg{};
					AsmReg boxed_type_info_reg{};
					if (register_condition) {
						boxed_condition.emplace(
							val_ref(node.operands[1]));
						const ValueParts parts =
							val_parts(node.operands[1]);
						if (parts.count() != 2) {
							return false;
						}
						boxed_payload.emplace(
							boxed_condition->part(0));
						boxed_type_info.emplace(
							boxed_condition->part(1));
						boxed_payload_reg =
							boxed_payload->load_to_reg();
						boxed_type_info_reg =
							boxed_type_info->load_to_reg();
					}
					auto load_condition_payload = [&]() {
						if (register_condition) {
							mov(value_reg, boxed_payload_reg, 8);
						} else {
							ASM(MOV64rm, value_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										layout.operand_offset)));
						}
					};

					/*
					 * Result-producing short-circuit branches must consume their
					 * source TMP.  Leave those to the native helper below instead
					 * of overwriting the source slot in the truthiness fast path.
					 */
					if (layout.has_result) {
						/* ?? moves a defined non-null temporary into the
						 * result or copies a CV; &&/|| publish the truth
						 * of a null, boolean or integer, which owns
						 * nothing. Anything else takes the helper. */
						const bool coalesce =
							layout.source_opcode == ZEND_COALESCE;
						const bool from_cv =
							mir.value_operation.op1.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV;
						const int32_t operand_offset =
							static_cast<int32_t>(layout.operand_offset);
						const int32_t result_offset =
							static_cast<int32_t>(layout.result_offset);
						auto store_result_type = [&](uint32_t type_info) {
							ASM(MOV32mi,
								FE_MEM(frame_reg, 0, FE_NOREG,
									result_offset
										+ static_cast<int32_t>(offsetof(
											zval, u1.type_info))),
								type_info);
						};
						if (register_condition) {
							mov(type_reg, boxed_type_info_reg, 4);
						} else {
							ASM(MOV32rm, type_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									operand_offset + static_cast<int32_t>(
										offsetof(zval, u1.type_info))));
						}
						ASM(MOVZXr32r8, value_reg, type_reg);
						if (coalesce) {
							ASM(CMP32ri, value_reg, IS_NULL);
							/* Undefined and null fall through to the
							 * default, as isset() decides. */
							generate_raw_jump(Jump::jbe, falsey);
							ASM(CMP32ri, value_reg, IS_REFERENCE);
							generate_raw_jump(Jump::je, slow);
							ASM(CMP32ri, value_reg, IS_INDIRECT);
							generate_raw_jump(Jump::je, slow);
							if (from_cv) {
								auto counted_done =
									text_writer.label_create();
								ASM(MOV64rm, value_reg,
									FE_MEM(frame_reg, 0, FE_NOREG,
										operand_offset));
								ASM(TEST32ri, type_reg,
									IS_TYPE_REFCOUNTED
										<< Z_TYPE_FLAGS_SHIFT);
								generate_raw_jump(Jump::je, counted_done);
								ASM(TEST32mi,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_refcounted_h,
											u.type_info))),
									GC_PERSISTENT);
								generate_raw_jump(Jump::jne, slow);
								ASM(ADD32mi,
									FE_MEM(value_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_refcounted_h, refcount))),
									1);
								label_place(counted_done);
							} else if (register_condition) {
								mov(value_reg, boxed_payload_reg, 8);
							} else {
								ASM(MOV64rm, value_reg,
									FE_MEM(frame_reg, 0, FE_NOREG,
										operand_offset));
							}
							ASM(MOV64mr,
								FE_MEM(frame_reg, 0, FE_NOREG,
									result_offset),
								value_reg);
							ASM(MOV32mr,
								FE_MEM(frame_reg, 0, FE_NOREG,
									result_offset + static_cast<int32_t>(
										offsetof(zval, u1.type_info))),
								type_reg);
							if (!from_cv
									&& layout.operand_offset
										!= layout.result_offset) {
								ASM(MOV32mi,
									FE_MEM(frame_reg, 0, FE_NOREG,
										operand_offset
											+ static_cast<int32_t>(offsetof(
												zval, u1.type_info))),
									IS_UNDEF);
							}
							generate_raw_jump(Jump::jmp, truthy);
						} else {
							auto result_true = text_writer.label_create();
							auto result_false = text_writer.label_create();
							ASM(CMP32ri, value_reg, IS_TRUE);
							generate_raw_jump(Jump::je, result_true);
							ASM(CMP32ri, value_reg, IS_NULL);
							generate_raw_jump(Jump::jb, slow);
							ASM(CMP32ri, value_reg, IS_FALSE);
							generate_raw_jump(Jump::jbe, result_false);
							ASM(CMP32ri, value_reg, IS_LONG);
							generate_raw_jump(Jump::jne, slow);
							load_condition_payload();
							ASM(TEST64rr, value_reg, value_reg);
							generate_raw_jump(Jump::je, result_false);
							label_place(result_true);
							store_result_type(IS_TRUE);
							generate_raw_jump(Jump::jmp, truthy);
							label_place(result_false);
							store_result_type(IS_FALSE);
							generate_raw_jump(Jump::jmp, falsey);
						}
					}
					if (fused && !emit_fused_compare(
							fused_compare, frame_reg, type_reg, value_reg,
							truthy, falsey, slow)) {
						return false;
					}

					/* The truthiness of the condition operand, unless a
					 * result-producing or fused form above decided every
					 * outcome. */
					if (!layout.has_result && !fused) {
						if (register_condition) {
							mov(type_reg, boxed_type_info_reg, 4);
						} else {
							ASM(MOV32rm, type_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										layout.operand_offset
											+ offsetof(zval, u1.type_info))));
						}
						/* Inference may know the condition is a boolean, such as a
						 * comparison result, which owns nothing. */
						const uint32_t condition_position =
							mir.value_operation.source_position_id;
						const bool known_bool = !layout.has_result
							&& adaptor->plan()->source_opcodes != nullptr
							&& condition_position
								< adaptor->plan()->source_opcode_count
							&& adaptor->plan()->source_opcodes[
									condition_position].op1_known_type
								== ZEND_TPDE_KNOWN_BOOL;
						if (known_bool) {
							ASM(CMP8ri, type_reg, IS_TRUE);
							generate_raw_jump(Jump::je, truthy);
							generate_raw_jump(Jump::jmp, falsey);
						}
						if (!known_bool) {
							/* A counted temporary, in its slot or held in registers,
							 * owns a reference that only the helper releases. */
							if (frame_temporary_condition
									|| (register_condition
										&& mir.value_operation.op1.slot_kind
											== ZEND_MIR_SOURCE_SLOT_TMP)) {
								ASM(TEST32ri, type_reg,
									IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
								generate_raw_jump(Jump::jne, slow);
							}
							ASM(AND32ri, type_reg, Z_TYPE_MASK);
							ASM(CMP32ri, type_reg, IS_NULL);
							generate_raw_jump(Jump::je, falsey);
							ASM(CMP32ri, type_reg, IS_FALSE);
							generate_raw_jump(Jump::je, falsey);
							ASM(CMP32ri, type_reg, IS_TRUE);
							generate_raw_jump(Jump::je, truthy);
							ASM(CMP32ri, type_reg, IS_LONG);
							auto not_long = text_writer.label_create();
							generate_raw_jump(Jump::jne, not_long);
							load_condition_payload();
							ASM(TEST64rr, value_reg, value_reg);
							generate_raw_jump(Jump::jne, truthy);
							generate_raw_jump(Jump::jmp, falsey);

							label_place(not_long);
							ASM(CMP32ri, type_reg, IS_STRING);
							auto not_string = text_writer.label_create();
							generate_raw_jump(Jump::jne, not_string);
							load_condition_payload();
							ASM(MOV64rm, type_reg,
								FE_MEM(value_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_string, len))));
							ASM(TEST64rr, type_reg, type_reg);
							generate_raw_jump(Jump::je, falsey);
							ASM(CMP64ri, type_reg, 1);
							generate_raw_jump(Jump::jne, truthy);
							ASM(MOVZXr32m8, type_reg,
								FE_MEM(value_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_string, val))));
							ASM(CMP32ri, type_reg, '0');
							generate_raw_jump(Jump::je, falsey);
							generate_raw_jump(Jump::jmp, truthy);

							label_place(not_string);
							ASM(CMP32ri, type_reg, IS_ARRAY);
							auto not_array = text_writer.label_create();
							generate_raw_jump(Jump::jne, not_array);
							load_condition_payload();
							ASM(MOV32rm, type_reg,
								FE_MEM(value_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(HashTable, nNumOfElements))));
							ASM(TEST32rr, type_reg, type_reg);
							generate_raw_jump(Jump::jne, truthy);
							generate_raw_jump(Jump::jmp, falsey);
							label_place(not_array);
							ASM(CMP32ri, type_reg, IS_RESOURCE);
							generate_raw_jump(Jump::jne, slow);
							load_condition_payload();
							ASM(MOV32rm, type_reg,
								FE_MEM(value_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_resource, handle))));
							ASM(TEST32rr, type_reg, type_reg);
							generate_raw_jump(Jump::jne, truthy);
							generate_raw_jump(Jump::jmp, falsey);
						}
					}

					const auto &successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					/*
					 * Without PHIs at either successor no edge moves are
					 * needed, so each outcome jumps to its block directly;
					 * otherwise both paths join at one TPDE branch.
					 */
					const bool direct_tails = successors.size() >= 2
						&& !branch_needs_split(successors[0])
						&& !branch_needs_split(successors[1]);
					if (!direct_tails) {
						label_place(truthy);
						ASM(MOV32mi,
							FE_MEM(FE_BP, 0, FE_NOREG,
								decision_slot),
							1);
						generate_raw_jump(Jump::jmp, fast_ready);
						label_place(falsey);
						ASM(MOV32mi,
							FE_MEM(FE_BP, 0, FE_NOREG,
								decision_slot),
							0);
						label_place(fast_ready);
					}
					type.reset();
					value.reset();
					if (!direct_tails) {
						generate_raw_jump(Jump::jmp, branch);
					}
					/* The helper decides the uncommon conditions: out of the
					 * hot code. */
					cold_begin();
					label_place(slow);
					if (fused && !emit_fused_compare_helper(
							fused_compare, mir, frame_scratch)) {
						return false;
					}
					if (register_condition) {
						if (publish_boxed_condition) {
							ASM(MOV64mr,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										layout.operand_offset)),
								boxed_payload_reg);
							ASM(MOV32mr,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(layout.operand_offset
										+ offsetof(zval, u1.type_info))),
								boxed_type_info_reg);
						}
						boxed_type_info.reset();
						boxed_payload.reset();
						boxed_condition.reset();
					}

					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, take_frame(frame_scratch));
					builder.add_arg(
						std::move(frame_argument), tpde::CCAssignment{});
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					call_value_operation(builder, operation,
						mir.runtime_helper);
					ValuePart decision{
						tpde::x64::PlatformConfig::GP_BANK};
					builder.add_ret(decision, tpde::CCAssignment{});
					auto decision_reg =
						decision.cur_reg_or_load(this);
					emit_decision_exception_check(decision_reg, mir.exception_block_id,
						[&] { decision.reset(this); });
					if (direct_tails) {
						ASM(TEST32rr, decision_reg, decision_reg);
						decision.reset(this);
						generate_raw_jump(Jump::jne, truthy);
						generate_raw_jump(Jump::jmp, falsey);
						/* The truthy tail only forwards (jump threading
						 * resolves the jumps to it to the block): it stays in
						 * the cold area, so the inline code's last jump to the
						 * falsey tail is one to the next instruction. */
						const auto spilled = spill_before_branch();
						begin_branch_region();
						label_place(truthy);
						generate_branch_to_block(
							Jump::jmp, successors[0], false, false);
						cold_end();
						label_place(falsey);
						generate_branch_to_block(
							Jump::jmp, successors[1], false, true);
						end_branch_region();
						release_spilled_regs(spilled);
						return true;
					}
					ASM(MOV32mr,
						FE_MEM(FE_BP, 0, FE_NOREG,
							decision_slot),
						decision_reg);
					decision.reset(this);
					generate_raw_jump(Jump::jmp, branch);
					cold_end();
					label_place(branch);
					ScratchReg branch_decision{this};
					auto branch_decision_reg =
						branch_decision.alloc_gp();
					ASM(MOV32rm, branch_decision_reg,
						FE_MEM(FE_BP, 0, FE_NOREG,
							decision_slot));
					ASM(TEST32rr,
						branch_decision_reg, branch_decision_reg);
					generate_cond_branch(
						Jump::jne, successors[0], successors[1]);
					return true;
				}
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			const zend_mir_executable_value_ref &operation =
				mir.value_operation;
			call_value_operation(builder, operation,
				mir.runtime_helper);
			ValuePart decision{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(decision, tpde::CCAssignment{});
			auto decision_reg = decision.cur_reg_or_load(this);
			const int32_t decision_slot = node.has_result
				? inst_stack_slot(sizeof(uint32_t)) : -1;
			if (node.has_result && decision_slot >= 0) {
				return false;
			}
			if (node.has_result) {
				ASM(MOV32mr,
					FE_MEM(FE_BP, 0, FE_NOREG, decision_slot),
					decision_reg);
			}
			emit_decision_exception_check(decision_reg, mir.exception_block_id,
				[&] { decision.reset(this); });
			const auto &successors = adaptor->block_succs(
				IRBlockRef{node.control_block});
			if (node.has_result) {
				const zend_mir_storage_id storage =
					operation.result_storage_id;
				const uint64_t frame_offset =
					(uint64_t{ZEND_CALL_FRAME_SLOT} + storage)
						* sizeof(zval);
				const bool scalar_result =
					(adaptor->representation(node.result)
							== ZEND_MIR_REPRESENTATION_I64
						&& adaptor->exact_type(node.result)
							== ZEND_MIR_SCALAR_TYPE_I64
						&& adaptor->machine_kind(node.result)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (adaptor->representation(node.result)
							== ZEND_MIR_REPRESENTATION_I1
						&& adaptor->exact_type(node.result)
							== ZEND_MIR_SCALAR_TYPE_I1
						&& adaptor->machine_kind(node.result)
							== ZEND_TPDE_MACHINE_VALUE_BOOL);
				const zend_tpde_machine_value_kind result_kind =
					adaptor->machine_kind(node.result);
				const bool pointer_result =
					adaptor->representation(node.result)
							== ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
					&& (result_kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR
						|| result_kind
							== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
						|| result_kind
							== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
						|| result_kind
							== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
						|| result_kind
							== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR);
				if (!zend_mir_id_is_valid(storage)
						|| frame_offset > UINT32_MAX - sizeof(zval)
						|| (!scalar_result && !pointer_result)) {
					return false;
				}
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64rm, result_reg,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(frame_offset)));
				result.set_modified();
				ScratchReg branch_decision{this};
				auto branch_decision_reg =
					branch_decision.alloc_gp();
				ASM(MOV32rm, branch_decision_reg,
					FE_MEM(FE_BP, 0, FE_NOREG, decision_slot));
				ASM(TEST32rr,
					branch_decision_reg, branch_decision_reg);
				generate_cond_branch(
					Jump::jne, successors[0], successors[1]);
			} else {
				ASM(TEST32rr, decision_reg, decision_reg);
				generate_cond_branch(
					Jump::jne, successors[0], successors[1]);
			}
			return true;
		}
		case ZEND_MIR_OPCODE_CALL_DIRECT_USER: {
			const zend_tpde_instruction &call =
				adaptor->mir_instruction(instruction);
			if (call.direct_call != nullptr) {
				const bool local_component_call =
					call.component_target_index != UINT32_MAX;
				if (local_component_call
						&& call.component_target_index
							>= this->func_syms.size()) {
					return false;
				}
				const bool generated_fast_path =
					(call.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
				const bool leaf_scalar_frame =
					generated_fast_path
					&& (call.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME) != 0;
				const bool private_inline_body = leaf_scalar_frame;
				const uint32_t typed_body_function =
					local_component_call
						&& adaptor->typed_component_call(instruction)
						? call.component_body_function_index
						: UINT32_MAX;
				const bool result_unused =
					call.direct_call->result_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED;
				const bool generation_leased =
					local_component_call
					|| (call.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_GENERATION_LEASED) != 0;
				const uint32_t argument_count = call.call_argument_count;
				if (adaptor->typed_body()
						&& typed_body_function != UINT32_MAX) {
					if (typed_body_function >= this->func_syms.size()
							|| node.operands.size() != argument_count) {
						return false;
					}
					/* A failed call fails this body too. */
					if (!emit_typed_body_call(node, typed_body_function,
							call.component_target_index, argument_count,
							nullptr)) {
						return false;
					}
					adaptor->mark_typed_body_call(
						call.direct_call->frame_size);
					return true;
				}
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
				auto compiled_variable_used =
					[&](uint32_t variable_index) {
						return variable_index < owned_argument_variable_count
							|| !local_component_call
							|| adaptor->component_compiled_variable_used(
								call.component_target_index,
								variable_index);
					};
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
				const uint32_t frame_operand = split_cold
					? 0
					: typed_body_function != UINT32_MAX
						? argument_count
						: generated_fast_path ? argument_count : 0;
				const uint32_t frame_use_count =
						split_cold ? 1
							: typed_body_function != UINT32_MAX ? 2
							: generated_fast_path
								? (private_inline_body ? 3 : 6 + node.has_result)
									+ (!private_inline_body && variadic_frame ? 2 : 0)
								: 2;
				const uint32_t context_operand = frame_operand
					+ frame_use_count;
				const uint32_t slow_enter_frame_use =
					generated_fast_path ? (private_inline_body ? 1 : 4) : 0;
				const uint32_t slow_enter_context_use =
					generated_fast_path ? (private_inline_body ? 2 : 4) : 0;
				const uint32_t slow_entry_context_use =
					generated_fast_path ? (private_inline_body ? 3 : 5) : 1;
				const uint32_t slow_leave_frame_use =
					generated_fast_path ? (private_inline_body ? 2 : 5) : 1;
				const uint32_t slow_leave_context_use =
					generated_fast_path ? (private_inline_body ? 4 : 6) : 2;
				const bool typed_body_call =
					typed_body_function != UINT32_MAX
					&& (node.kind == Adaptor::InstKind::GuardedFast
						|| node.kind == Adaptor::InstKind::MIR)
					&& !node.inlined_user_body
					&& adaptor->typed_body_return_type(
						call.component_target_index).valid
					&& adaptor->typed_body_arguments_match(
						call.component_target_index, node.operands);
				if (generated_fast_path
						&& node.kind == Adaptor::InstKind::GuardedFast
						&& !typed_body_call
						&& !node.inlined_user_body) {
					if (node.operands.size() < context_operand) {
						return false;
					}
					const uint32_t context_use_count =
						static_cast<uint32_t>(node.operands.size())
						- context_operand;
					auto discard_operand = [&](uint32_t index) {
						auto discarded = val_ref(node.operands[index]);
						(void) discarded;
					};
					if (private_inline_body) {
						discard_operand(frame_operand + 1);
						discard_operand(frame_operand + 2);
						for (uint32_t use = 2;
								use < context_use_count; ++use) {
							discard_operand(context_operand + use);
						}
					} else {
						discard_operand(frame_operand + 4);
						discard_operand(frame_operand + 5);
						for (uint32_t use = 4;
								use < context_use_count; ++use) {
							discard_operand(context_operand + use);
						}
					}
				}
				auto slow_path = text_writer.label_create();
				auto successful = text_writer.label_create();
				int32_t leaf_private_frame_slot = 0;
				int32_t leaf_caller_frame_slot = 0;
				auto load_generated_result = [&](AsmReg result_frame_reg) {
					if (node.has_result) {
						ScratchReg result_slot{this};
						auto result_slot_reg = result_slot.alloc_gp();
						ASM(MOV64rr, result_slot_reg, result_frame_reg);
						if (call.direct_call->result_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV) {
							ASM(ADD64ri, result_slot_reg,
								static_cast<int32_t>(
									(ZEND_CALL_FRAME_SLOT
										+ call.direct_call->result_operand.index)
									* sizeof(zval)));
						} else {
							/* The caller frame belongs to this function: its slot
							 * layout is a compile-time constant. */
							ASM(ADD64ri, result_slot_reg, static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ adaptor->plan()->source_frame_variable_count
									+ call.direct_call->result_operand.index)
								* sizeof(zval)));
						}
						if (adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
							auto result = result_ref(node.result);
							const ValueParts parts = val_parts(node.result);
							for (uint32_t part = 0;
									part < parts.count(); ++part) {
								auto value = result.part(part);
								auto value_reg = value.alloc_reg();
								const zend_tpde_machine_part_role role =
									parts.representation.parts[part]
										.semantic_role;
								if (role
										== ZEND_TPDE_MACHINE_PART_PAYLOAD) {
									ASM(MOV64rm, value_reg,
										FE_MEM(result_slot_reg, 0,
											FE_NOREG, 0));
								} else if (role
										== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
									ASM(MOV32rm, value_reg,
										FE_MEM(result_slot_reg, 0,
											FE_NOREG,
											static_cast<int32_t>(offsetof(
												zval, u1.type_info))));
								} else {
									return;
								}
								value.set_modified();
							}
						} else {
							auto [result_ref, result] =
								result_ref_single(node.result);
							auto result_reg = result.alloc_reg();
							if (val_parts(node.result).bank
									== tpde::x64::PlatformConfig::FP_BANK) {
								ASM(SSE_MOVSDrm, result_reg,
									FE_MEM(result_slot_reg, 0, FE_NOREG, 0));
							} else if (adaptor->exact_type(node.result)
									== ZEND_MIR_SCALAR_TYPE_I1) {
								/*
								 * A bool zval carries its value in the type;
								 * the payload is undefined (ZVAL_TRUE does not
								 * write it).
								 */
								ASM(MOVZXr32m8, result_reg,
									FE_MEM(result_slot_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zval, u1.type_info))));
								ASM(CMP32ri, result_reg, IS_TRUE);
								generate_raw_set(Jump::je, result_reg);
							} else {
								ASM(MOV64rm, result_reg,
									FE_MEM(result_slot_reg, 0, FE_NOREG, 0));
							}
							result.set_modified();
						}
					}
				};
				auto finish_generated_result = [&]() {
					if (node.has_result) {
						ScratchReg result_frame{this};
						auto result_frame_reg = result_frame.alloc_gp();
						if (private_inline_body) {
							ASM(MOV64rm, result_frame_reg,
								FE_MEM(FE_BP, 0, FE_NOREG,
									leaf_caller_frame_slot));
						} else {
							auto [result_frame_ref,
								result_frame_value] =
									val_ref_single(node.operands[
										frame_operand + 6
											+ (variadic_frame ? 2 : 0)]);
							ASM(MOV64rr, result_frame_reg,
								result_frame_value.load_to_reg());
						}
						load_generated_result(result_frame_reg);
					}
					if (private_inline_body) {
						free_stack_slot(
							static_cast<uint32_t>(leaf_caller_frame_slot),
							sizeof(void *));
						free_stack_slot(
							static_cast<uint32_t>(leaf_private_frame_slot),
							call.direct_call->frame_size);
					}
				};
				auto call_slow_target = [&]() {
					if (node.kind != Adaptor::InstKind::GuardedFast
							|| node.argument_index == UINT32_MAX) {
						return slow_path;
					}
					return this->block_labels[static_cast<uint32_t>(
						this->analyzer.block_idx(
							IRBlockRef{node.argument_index}))];
				};
				if (node.kind == Adaptor::InstKind::GuardedFast
						&& !typed_body_call) {
					auto spilled = spill_before_branch();
					release_spilled_regs(spilled);
				}
				if (node.kind == Adaptor::InstKind::GuardedFast
						&& node.inlined_user_body) {
					if (node.continuation_block == UINT32_MAX
							|| node.inlined_operand_index
								>= node.operands.size()) {
						return false;
					}
					const auto checked_steps =
						adaptor->inlined_checked_steps(node);
					const uint32_t inline_operand_count =
						checked_steps.empty()
							? 1 : node.inlined_checked_operand_count;
					if (node.operands.size() < inline_operand_count
							|| node.inlined_operand_index
							> node.operands.size()
								- inline_operand_count) {
						return false;
					}
					const bool has_inline_context =
						node.inlined_operand_index > context_operand;
					for (uint32_t operand = 0;
							operand < node.operands.size(); ++operand) {
						if ((operand >= node.inlined_operand_index
								&& operand < node.inlined_operand_index
									+ inline_operand_count)
								|| (has_inline_context
									&& operand == context_operand)) {
							continue;
						}
						auto discarded = val_ref(node.operands[operand]);
						(void) discarded;
					}
					/*
					 * A split guard already routed observer-visible execution
					 * into the materialized cold block.  Older machine CFGs
					 * keep that guard in this block and therefore retain the
					 * context operand; handle both layouts without creating a
					 * private Zend frame on the register-only edge.
					 */
					if (has_inline_context) {
						auto [context_ref, context] =
							val_ref_single(
								node.operands[context_operand]);
						(void) context;
						emit_observer_exit(call_slow_target());
					}
					if (!checked_steps.empty()) {
						if (checked_steps.size() > 1) {
							/*
							 * A chain of checked ADD/SUB steps over one
							 * accumulator. Any overflow re-executes the call on
							 * its slow path; the inlined body is effect-closed.
							 */
							if (inline_operand_count != checked_steps.size() + 1
									|| node.inlined_operand_index + 1
										>= node.operands.size()
									|| !node.has_result) {
								return false;
							}
							auto [left_ref, left] = val_ref_single(
								node.operands[node.inlined_operand_index]);
							auto [right_ref, right] = val_ref_single(
								node.operands[
									node.inlined_operand_index + 1]);
							ScratchReg computed{this};
							auto computed_reg = computed.alloc_gp();
							/* computed = lhs op rhs; either may be computed. */
							auto emit_checked = [&](uint32_t source_opcode,
									AsmReg lhs_reg, AsmReg rhs_reg) {
								if (source_opcode != ZEND_ADD
										&& source_opcode != ZEND_SUB) {
									return false;
								}
								if (rhs_reg == computed_reg
										&& lhs_reg != computed_reg) {
									if (source_opcode == ZEND_ADD) {
										ASM(ADD64rr, computed_reg, lhs_reg);
									} else {
										ScratchReg difference{this};
										auto difference_reg =
											difference.alloc_gp();
										ASM(MOV64rr, difference_reg, lhs_reg);
										ASM(SUB64rr, difference_reg,
											computed_reg);
										/* MOV leaves the SUB flags intact. */
										ASM(MOV64rr, computed_reg,
											difference_reg);
									}
								} else {
									if (lhs_reg != computed_reg) {
										ASM(MOV64rr, computed_reg, lhs_reg);
									}
									if (source_opcode == ZEND_ADD) {
										ASM(ADD64rr, computed_reg, rhs_reg);
									} else {
										ASM(SUB64rr, computed_reg, rhs_reg);
									}
								}
								generate_raw_jump(Jump::jo, call_slow_target());
								return true;
							};
							if (!emit_checked(checked_steps[0].source_opcode,
									left.load_to_reg(), right.load_to_reg())) {
								return false;
							}
							for (uint32_t step = 1;
									step < checked_steps.size(); ++step) {
								auto [other_ref, other] = val_ref_single(
									node.operands[node.inlined_operand_index
										+ step + 1]);
								const auto other_reg = other.load_to_reg();
								if (!emit_checked(
										checked_steps[step].source_opcode,
										checked_steps[step].accumulator_is_left
											? computed_reg : other_reg,
										checked_steps[step].accumulator_is_left
											? other_reg : computed_reg)) {
									return false;
								}
							}
							auto [result_ref, result] =
								result_ref_single(node.result);
							result.set_value(std::move(computed));
							generate_uncond_branch(
								IRBlockRef{node.continuation_block});
							return true;
						}
						if (node.inlined_operand_index + 1
								>= node.operands.size()) {
							return false;
						}
						auto [left_ref, left] = val_ref_single(
							node.operands[node.inlined_operand_index]);
						auto [right_ref, right] = val_ref_single(
							node.operands[
								node.inlined_operand_index + 1]);
						auto left_reg = left.load_to_reg();
						auto right_reg = right.load_to_reg();
						if (node.has_result) {
							ScratchReg computed{this};
							auto computed_reg = computed.alloc_gp();
							ASM(MOV64rr, computed_reg, left_reg);
							switch (checked_steps[0].source_opcode) {
								case ZEND_ADD:
									ASM(ADD64rr,
										computed_reg, right_reg);
									break;
								case ZEND_SUB:
									ASM(SUB64rr,
										computed_reg, right_reg);
									break;
								default:
									return false;
							}
							auto [result_ref, result] =
								result_ref_single(node.result);
							result.set_value(std::move(computed));
							if (node.argument_index == UINT32_MAX) {
								return false;
							}
							generate_cond_branch(
								Jump::jo,
								IRBlockRef{node.argument_index},
								IRBlockRef{node.continuation_block});
							return true;
						}
					} else {
						auto [inline_ref, inline_value] =
							val_ref_single(node.operands[
								node.inlined_operand_index]);
						if (node.has_result) {
							auto [result_ref, result] =
								result_ref_single(node.result);
							auto source_reg =
								inline_value.load_to_reg();
							auto result_reg =
								result.alloc_try_reuse(inline_value);
							if (source_reg != result_reg) {
								ASM(MOV64rr,
									result_reg, source_reg);
							}
							result.set_modified();
						}
					}
					generate_uncond_branch(
						IRBlockRef{node.continuation_block});
					return true;
				}
				if (typed_body_call) {
					const zend_tpde_plan *body_plan =
						adaptor->component_plan(
							call.component_target_index);
					if (body_plan == nullptr
							|| body_plan->argument_count != argument_count
							|| typed_body_function
								>= this->func_syms.size()
							|| node.continuation_block == UINT32_MAX
							|| node.operands.size() < argument_count) {
						return false;
					}
					/* A failed call repeats through the canonical cold call. */
					ScratchReg failure_decision{this};
					if (!emit_typed_body_call(node, typed_body_function,
							call.component_target_index, argument_count,
							&failure_decision)) {
						return false;
					}
					/*
					 * Operands after the arguments are owned boxed copies made by
					 * the caller's fast reads; the typed body only borrowed them.
					 * Drop the caller's reference now that the call returned.
					 */
					for (uint32_t operand = argument_count;
							operand < node.operands.size(); ++operand) {
						if (adaptor->machine_kind(node.operands[operand])
								!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
								|| adaptor->ownership(node.operands[operand])
									!= ZEND_MIR_OWNERSHIP_STATE_OWNED) {
							return false;
						}
						auto boxed = val_ref(node.operands[operand]);
						const ValueParts parts = val_parts(node.operands[operand]);
						/* A register-held boxed zval: payload, then type info
						 * (zend_tpde_machine_representation()). */
						if (parts.count() != 2) {
							return false;
						}
						constexpr int32_t payload_part = 0;
						constexpr int32_t type_part = 1;
						auto payload = boxed.part(
							static_cast<uint32_t>(payload_part));
						auto type_info = boxed.part(
							static_cast<uint32_t>(type_part));
						auto payload_reg = payload.load_to_reg();
						auto type_info_reg = type_info.load_to_reg();
						auto released = text_writer.label_create();
						ASM(TEST32ri, type_info_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, released);
						ASM(SUB32mi,
							FE_MEM(payload_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))),
							1);
						label_place(released);
					}
					adaptor->mark_typed_body_call(
						call.direct_call->frame_size);
					if (failure_decision.has_reg()) {
						if (node.argument_index == UINT32_MAX) {
							return false;
						}
						generate_guarded_decision_branch(
							std::move(failure_decision),
							IRBlockRef{node.continuation_block},
							IRBlockRef{node.argument_index});
						return true;
					}
					generate_uncond_branch(
						IRBlockRef{node.continuation_block});
					return true;
				}
				if (generated_fast_path
						&& node.kind != Adaptor::InstKind::GuardedCold) {
					/*
					 * Materialize the callee directly in the first native-entry
					 * argument register. This gives the large-frame path one
					 * stable callee register without reserving both ABI argument
					 * registers throughout frame construction. The indirect
					 * entry target is materialized in R11 immediately before the
					 * call, so argument placement cannot overwrite it.
					 */
					ScratchReg fast_callee_argument_register{this};
					fast_callee_argument_register.alloc_specific(
						tpde::x64::AsmReg::DI);
					if (private_inline_body) {
						auto [frame_ref, frame] =
							val_ref_single(node.operands[frame_operand]);
						auto frame_scratch =
							std::move(frame).into_scratch();
						auto frame_reg = frame_scratch.cur_reg();
						auto [context_ref, context] =
							val_ref_single(node.operands[context_operand]);
						auto context_scratch =
							std::move(context).into_scratch();
						auto context_reg = context_scratch.cur_reg();
						auto cell_value = image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
							call.call_site->target_id);
						auto cell_scratch =
							std::move(cell_value).into_scratch(this);
						auto cell_reg = cell_scratch.cur_reg();
						leaf_private_frame_slot = allocate_stack_slot(
							call.direct_call->frame_size);
						leaf_caller_frame_slot =
							allocate_stack_slot(sizeof(void *));
						if (leaf_private_frame_slot >= 0
								|| leaf_caller_frame_slot >= 0) {
							return false;
						}
						ASM(MOV64mr,
							FE_MEM(FE_BP, 0, FE_NOREG,
								leaf_caller_frame_slot),
							frame_reg);
						ScratchReg first{this};
						ScratchReg second{this};
						auto first_reg = first.alloc_gp();
						auto second_reg = second.alloc_gp();

						/*
						 * A leaf binding names an exact, already-published
						 * immutable callee. The compiler declines this
						 * representation when a frame probe is installed, so
						 * those compile-time invariants do not need to be
						 * reloaded at every loop iteration.
						 */
#ifdef ZEND_CHECK_STACK_LIMIT
						ASM(MOV64rm, first_reg,
							FE_MEM(context_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_execution_context,
									stack_limit))));
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG, 0));
						ASM(CMP64rr, FE_SP, first_reg);
						generate_raw_jump(Jump::jbe, call_slow_target());
#endif
						emit_observer_exit(call_slow_target());
						auto callee_reg =
							fast_callee_argument_register.cur_reg();
						ASM(LEA64rm, callee_reg,
							FE_MEM(FE_BP, 0, FE_NOREG,
								leaf_private_frame_slot));
						ASM(MOV64rm, second_reg,
							FE_MEM(cell_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_entry_cell, function))));
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, func))),
							second_reg);
						ASM(MOV64mi,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, call))),
							1);

						for (uint32_t index = 0;
								index < argument_count; ++index) {
							zend_mir_call_argument_ref source_argument;
							if (!zend_tpde_call_argument_at(
									adaptor->plan(),
									call.call_argument_offset + index,
									&source_argument)) {
								return false;
							}
							auto argument_value_ref =
								val_ref(node.operands[index]);
							auto argument = argument_value_ref.part(0);
							const int32_t offset =
								static_cast<int32_t>(
									(ZEND_CALL_FRAME_SLOT + index)
										* sizeof(zval));
							const zend_native_direct_call_argument
								&descriptor_argument =
									call.direct_call->arguments[index];
							if (source_argument.send_opline_index
									>= adaptor->plan()
										->source_opcode_count) {
								return false;
							}
							const uint8_t source_argument_type =
								adaptor->plan()->source_opcodes[
									source_argument.send_opline_index]
										.op1_type;
							const bool copy_argument =
								source_argument_type == IS_CV
								|| source_argument_type == IS_CONST;
							if (node.operands[index]
									== IRValueRef{Adaptor::FRAME_VALUE}
									&& source_argument.source_operand.kind
										!= ZEND_MIR_SOURCE_OPERAND_LITERAL
									&& zend_mir_scalar_type_is_exact(
										descriptor_argument.exact_type)) {
								if (descriptor_argument.source_frame_offset
										> INT32_MAX) {
									return false;
								}
								ScratchReg payload{this};
								auto payload_reg = payload.alloc_gp();
								if (descriptor_argument.exact_type
										== ZEND_MIR_SCALAR_TYPE_I1) {
									ASM(MOV32rm, payload_reg,
										FE_MEM(frame_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												descriptor_argument
													.source_frame_offset
												+ offsetof(zval, u1.type_info))));
									ASM(CMP32ri, payload_reg, IS_TRUE);
									generate_raw_set(Jump::je, payload_reg);
								} else {
									ASM(MOV64rm, payload_reg,
										FE_MEM(frame_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												descriptor_argument
													.source_frame_offset)));
								}
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset),
									payload_reg);
								ASM(MOV64mi,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + 8),
									0);
								if (descriptor_argument.exact_type
										== ZEND_MIR_SCALAR_TYPE_I1) {
									ScratchReg kind{this};
									auto kind_reg = kind.alloc_gp();
									ASM(MOV64rr, kind_reg, payload_reg);
									ASM(ADD64ri, kind_reg, IS_FALSE);
									ASM(MOV32mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										kind_reg);
								} else {
									ASM(MOV32mi,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										static_cast<int32_t>(zval_type(
											descriptor_argument.exact_type)));
								}
							} else {
								if (source_argument.source_operand.kind
											== ZEND_MIR_SOURCE_OPERAND_LITERAL
										|| node.operands[index]
											== IRValueRef{Adaptor::FRAME_VALUE}) {
								if (source_argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
									ScratchReg literal{this};
									auto literal_reg = literal.alloc_gp();
									ASM(MOV64ri, literal_reg,
										call.direct_call->arguments[index]
											.scalar_bits);
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset),
										literal_reg);
									ASM(MOV64mi,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										zval_type(call.direct_call
											->arguments[index].exact_type)
											+ (call.direct_call
													->arguments[index].exact_type
												== ZEND_MIR_SCALAR_TYPE_I1
												? static_cast<uint32_t>(
													call.direct_call
														->arguments[index]
														.scalar_bits)
												: 0));
								} else {
									auto source_frame_reg =
										argument.load_to_reg();
									if (call.direct_call->arguments[index]
											.source_frame_offset
											> INT32_MAX) {
										return false;
									}
									const int32_t source_offset =
										static_cast<int32_t>(
											call.direct_call->arguments[index]
												.source_frame_offset);
									ScratchReg low_word{this};
									ScratchReg high_word{this};
									auto low_word_reg =
										low_word.alloc_gp();
									auto high_word_reg =
										high_word.alloc_gp();
									ASM(MOV64rm, low_word_reg,
										FE_MEM(source_frame_reg, 0,
											FE_NOREG, source_offset));
									ASM(MOV64rm, high_word_reg,
										FE_MEM(source_frame_reg, 0,
											FE_NOREG,
											source_offset + 8));
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset),
										low_word_reg);
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										high_word_reg);
								}
							} else if (adaptor->machine_kind(
									node.operands[index])
										== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
									&& adaptor
										->machine_value_is_register_authoritative(
											node.operands[index])) {
								auto low_word =
									std::move(argument).into_scratch();
								auto high_part =
									argument_value_ref.part(1);
								auto high_word =
									std::move(high_part).into_scratch();
								if (descriptor_argument.exact_type
										== ZEND_MIR_SCALAR_TYPE_I1) {
									ASM(CMP32ri, high_word.cur_reg(), IS_TRUE);
									generate_raw_set(
										Jump::je, low_word.cur_reg());
								}
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset),
									low_word.cur_reg());
								ASM(MOV32mr,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + 8),
									high_word.cur_reg());
								ASM(MOV32mi,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + static_cast<int32_t>(
											offsetof(zval, u2))),
									0);
								if (copy_argument) {
									ASM(TEST32ri, high_word.cur_reg(), IS_TYPE_REFCOUNTED
											<< Z_TYPE_FLAGS_SHIFT);
									auto copied = text_writer.label_create();
									generate_raw_jump(Jump::je, copied);
									ASM(ADD32mi,
										FE_MEM(low_word.cur_reg(), 0,
											FE_NOREG,
											static_cast<int32_t>(offsetof(
												zend_refcounted_h, refcount))),
										1);
									label_place(copied);
								}
							} else {
								auto argument_reg =
									argument.load_to_reg();
								if (val_parts(node.operands[index]).bank
										== tpde::x64::PlatformConfig::
											FP_BANK) {
									ASM(SSE_MOVSDmr,
										FE_MEM(callee_reg, 0,
											FE_NOREG, offset),
										argument_reg);
								} else {
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0,
											FE_NOREG, offset),
										argument_reg);
								}
								ASM(MOV64mi,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + 8),
									0);
								const uint32_t type =
									zval_type(*adaptor,
										node.operands[index]);
								if (type == IS_FALSE) {
									ScratchReg kind{this};
									auto kind_reg = kind.alloc_gp();
									ASM(MOV64rr, kind_reg,
										argument_reg);
									ASM(ADD64ri, kind_reg,
										IS_FALSE);
									ASM(MOV32mr,
										FE_MEM(callee_reg, 0,
											FE_NOREG, offset + 8),
										kind_reg);
								} else {
									ASM(MOV32mi,
										FE_MEM(callee_reg, 0,
											FE_NOREG, offset + 8),
										static_cast<int32_t>(type));
								}
							}
								}
							}
						/*
						 * The result slot may alias any source argument
						 * (`$value = leaf($value)`).  Snapshot every argument
						 * into the private frame before publishing and
						 * invalidating that slot.
						 */
						if (result_unused) {
							ASM(MOV64mi,
								FE_MEM(callee_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_execute_data,
										return_value))),
								0);
						} else {
							ASM(MOV64rr, second_reg, frame_reg);
							if (call.direct_call->result_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_CV) {
								ASM(ADD64ri, second_reg,
									static_cast<int32_t>(
										(ZEND_CALL_FRAME_SLOT
											+ call.direct_call
												->result_operand.index)
										* sizeof(zval)));
							} else {
								/* The caller frame belongs to this function: its slot
								 * layout is a compile-time constant. */
								ASM(ADD64ri, second_reg, static_cast<int32_t>(
									(ZEND_CALL_FRAME_SLOT
										+ adaptor->plan()->source_frame_variable_count
										+ call.direct_call->result_operand.index)
									* sizeof(zval)));
							}
							ASM(MOV64mr,
								FE_MEM(callee_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_execute_data,
										return_value))),
								second_reg);
							ASM(MOV32mi,
								FE_MEM(second_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zval, u1.type_info))),
								IS_UNDEF);
						}
						first.reset();
						second.reset();
						ValuePart callee_value{
							tpde::x64::PlatformConfig::GP_BANK, 8};
						callee_value.set_value(
							this, std::move(fast_callee_argument_register));
						/*
						 * R11 carries the entry target. Release the dead frame
						 * scratches first and move the cell out of R11 if the
						 * allocator placed it there: claiming a fixed register
						 * is invalid.
						 */
						frame_scratch.reset();
						context_scratch.reset();
						if (cell_reg == tpde::x64::AsmReg{tpde::x64::AsmReg::R11}) {
							ScratchReg moved_cell{this};
							auto moved_cell_reg = moved_cell.alloc_gp();
							mov(moved_cell_reg, cell_reg, 8);
							cell_scratch = std::move(moved_cell);
							cell_reg = moved_cell_reg;
						}
						ScratchReg entry_argument{this};
						auto entry_argument_reg =
							entry_argument.alloc_specific(
								tpde::x64::AsmReg::R11);
						ASM(MOV64rm, entry_argument_reg,
							FE_MEM(cell_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_entry_cell, code))));
						ASM(MOV64rm, entry_argument_reg,
							FE_MEM(entry_argument_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_code, entry))));
						ValuePart entry_value{
							tpde::x64::PlatformConfig::GP_BANK, 8};
						entry_value.set_value(
							this, std::move(entry_argument));
						cell_scratch.reset();
						tpde::x64::CCAssignerSysV fast_assigner{false};
						CallBuilder fast_builder{*this, fast_assigner};
						fast_builder.add_arg(std::move(callee_value),
							tpde::CCAssignment{});
						fast_builder.add_arg(CallArg{
							node.operands[context_operand + 1]});
						fast_builder.call(std::move(entry_value));
						ValuePart fast_status{
							tpde::x64::PlatformConfig::GP_BANK, 4};
						fast_builder.add_ret(
							fast_status, tpde::CCAssignment{});
						auto fast_status_reg =
							fast_status.cur_reg_or_load(this);
						ASM(CMP32ri, fast_status_reg,
							ZEND_NATIVE_RETURNED);
						auto leaf_returned =
							text_writer.label_create();
						generate_raw_jump(
							Jump::je, leaf_returned);
						ASM(CMP32ri, fast_status_reg,
							ZEND_NATIVE_RETRY);
						generate_raw_jump(
							Jump::je, call_slow_target());
						if (zend_mir_id_is_valid(
								call.exception_block_id)) {
							auto propagate =
								text_writer.label_create();
							ASM(CMP32ri, fast_status_reg,
								ZEND_NATIVE_EXCEPTION);
							generate_raw_jump(
								Jump::jne, propagate);
							generate_exception_branch(
								adaptor->block_ref(
									call.exception_block_id));
							label_place(propagate);
						}
						{
							RetBuilder return_builder{
								*this, *cur_cc_assigner()};
							return_builder.add(
								std::move(fast_status),
								tpde::CCAssignment{});
							return_builder.ret();
						}
						label_place(leaf_returned);
						fast_status.reset(this);
						generate_raw_jump(
							Jump::jmp, successful);
					} else {
					const uint64_t activation_size =
						(sizeof(zend_native_direct_activation) + sizeof(zval) - 1)
							/ sizeof(zval) * sizeof(zval);
					const uint64_t reservation_size =
						static_cast<uint64_t>(call.direct_call->frame_size)
							+ activation_size;
					if (reservation_size > INT32_MAX) {
						return false;
					}
					auto [frame_ref, frame] =
						val_ref_single(node.operands[frame_operand]);
					auto frame_scratch = frame_register(std::move(frame));
					auto frame_reg = frame_scratch.cur_reg();
					auto [context_ref, context] =
						val_ref_single(node.operands[context_operand]);
					auto context_scratch = std::move(context).into_scratch();
					auto context_reg = context_scratch.cur_reg();
					auto cell_value = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site->target_id);
					auto cell_scratch =
						std::move(cell_value).into_scratch(this);
					auto cell_reg = cell_scratch.cur_reg();
					auto descriptor_value = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id);
					auto descriptor_scratch =
						std::move(descriptor_value).into_scratch(this);
					auto descriptor_reg = descriptor_scratch.cur_reg();
					ScratchReg first{this};
					ScratchReg second{this};
					ScratchReg published_code{this};
					auto first_reg = first.alloc_gp();
					auto second_reg = second.alloc_gp();
					AsmReg published_code_reg = local_component_call
						? AsmReg::make_invalid() : published_code.alloc_gp();
					std::optional<ScratchReg> run_time_cache;
					auto load_callee_function =
						[this, local_component_call, descriptor_reg, cell_reg](
							AsmReg destination) {
							ASM(MOV64rm, destination,
								FE_MEM(
									local_component_call
										? descriptor_reg : cell_reg,
									0, FE_NOREG,
									local_component_call
										? static_cast<int32_t>(offsetof(
											zend_native_direct_call_descriptor,
											expected_function))
										: static_cast<int32_t>(offsetof(
											zend_native_entry_cell, function))));
						};

					if (!local_component_call) {
						ASM(MOV64rm, published_code_reg,
							FE_MEM(cell_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_native_entry_cell, code))));
						ASM(TEST64rr, published_code_reg, published_code_reg);
						generate_raw_jump(Jump::je, call_slow_target());
						load_callee_function(first_reg);
						ASM(MOV64rm, second_reg,
							FE_MEM(descriptor_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_direct_call_descriptor,
									expected_function))));
						ASM(CMP64rr, first_reg, second_reg);
						generate_raw_jump(Jump::jne, call_slow_target());
						ASM(CMP8mi,
							FE_MEM(published_code_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_native_code, executable))),
							1);
						generate_raw_jump(Jump::jne, call_slow_target());
						ASM(MOV64rm, first_reg,
							FE_MEM(cell_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_entry_cell, frame_probe))));
						ASM(TEST64rr, first_reg, first_reg);
						generate_raw_jump(Jump::jne, call_slow_target());
					}
					/*
					 * The validated code pointer is next needed when the
					 * activation is linked. x86-64 has too few registers to keep
					 * it live across frame construction; spill the exact value
					 * rather than reloading the cell, which may be republished.
					 * A component-local call has no published code.
					 */
					int32_t published_code_slot = 0;
					if (!local_component_call) {
						published_code_slot =
							inst_stack_slot(sizeof(void *));
						if (published_code_slot >= 0) {
							return false;
						}
						ASM(MOV64mr,
							FE_MEM(FE_BP, 0, FE_NOREG, published_code_slot),
							published_code_reg);
					}
					published_code.reset();
					emit_observer_exit(call_slow_target());
					ASM(MOV64rm, first_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(TEST64rr, first_reg, first_reg);
					generate_raw_jump(Jump::jne, call_slow_target());
					if (call.direct_call->expected_function
							->op_array.cache_size != 0) {
						run_time_cache.emplace(this);
						auto cache_reg = run_time_cache->alloc_gp();
						load_callee_function(first_reg);
						ASM(MOV64rm, cache_reg,
							FE_MEM(first_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_op_array, run_time_cache__ptr))));
						ASM(TEST32ri, cache_reg, 1);
						auto cache_resolved = text_writer.label_create();
						generate_raw_jump(Jump::je, cache_resolved);
						ASM(MOV64rm, first_reg,
							FE_MEM(context_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_execution_context,
									map_ptr_base_address))));
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG, 0));
						ASM(ADD64rr, cache_reg, first_reg);
						ASM(MOV64rm, cache_reg,
							FE_MEM(cache_reg, 0, FE_NOREG, 0));
						label_place(cache_resolved);
						ASM(TEST64rr, cache_reg, cache_reg);
						generate_raw_jump(Jump::je, call_slow_target());
					}
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
						ASM(MOVZXr32m8, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This)
										+ offsetof(zval, u1.type_info))));
						ASM(CMP32ri, first_reg, IS_OBJECT);
						generate_raw_jump(Jump::jne, call_slow_target());
					} else if (call.direct_call->receiver_kind
								== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE
							&& (call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
								!= 0) {
						ASM(MOV64rm, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))));
						ASM(MOVZXr32m8, second_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This)
										+ offsetof(zval, u1.type_info))));
						ASM(CMP32ri, second_reg, IS_OBJECT);
						auto called_scope_ready = text_writer.label_create();
						generate_raw_jump(Jump::jne, called_scope_ready);
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_object, ce))));
						label_place(called_scope_ready);
						ASM(TEST64rr, first_reg, first_reg);
						generate_raw_jump(Jump::je, call_slow_target());
						load_callee_function(second_reg);
						ASM(MOV64rm, second_reg,
							FE_MEM(second_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_op_array, scope))));
						auto called_scope_compatible =
							text_writer.label_create();
						auto check_called_scope = text_writer.label_create();
						label_place(check_called_scope);
						ASM(CMP64rr, first_reg, second_reg);
						generate_raw_jump(
							Jump::je, called_scope_compatible);
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_class_entry, parent))));
						ASM(TEST64rr, first_reg, first_reg);
						generate_raw_jump(
							Jump::jne, check_called_scope);
						generate_raw_jump(Jump::jmp, call_slow_target());
						label_place(called_scope_compatible);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						const int32_t receiver_offset =
							static_cast<int32_t>(
								call.direct_call->receiver_source_frame_offset);
						ASM(MOVZXr32m8, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								receiver_offset + static_cast<int32_t>(
									offsetof(zval, u1.type_info))));
						ASM(CMP32ri, first_reg, IS_OBJECT);
						generate_raw_jump(Jump::jne, call_slow_target());
						ASM(MOV64rm, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG, receiver_offset));
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_object, ce))));
						load_callee_function(second_reg);
						ASM(MOV64rm, second_reg,
							FE_MEM(second_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_op_array, scope))));
						auto receiver_compatible = text_writer.label_create();
						auto check_receiver_class = text_writer.label_create();
						label_place(check_receiver_class);
						ASM(CMP64rr, first_reg, second_reg);
						generate_raw_jump(
							Jump::je, receiver_compatible);
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_class_entry, parent))));
						ASM(TEST64rr, first_reg, first_reg);
						generate_raw_jump(
							Jump::jne, check_receiver_class);
						generate_raw_jump(Jump::jmp, call_slow_target());
						label_place(receiver_compatible);
					}

					/*
					 * A boxed by-value CV can be copied inline while it is a
					 * defined, non-reference zval. A by-reference CV can be
					 * copied inline once it already contains a reference; the
					 * canonical slow path owns first-time reference creation.
					 * Guard every boxed source before publishing or reserving a
					 * frame.
					 */
					for (uint32_t index = 0; index < argument_count; ++index) {
						const zend_native_direct_call_argument &argument =
							call.direct_call->arguments[index];
						const zend_mir_scalar_type_mask argument_guard_type =
							call.direct_call_argument_guard_types != nullptr
								? call.direct_call_argument_guard_types[index]
								: ZEND_MIR_SCALAR_TYPE_NONE;
						if (argument.mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& zend_mir_scalar_type_is_exact(
									argument_guard_type)) {
							/*
							 * A typed callee receives this boxed argument without
							 * coercion. Check its runtime type; any other type
							 * takes the canonical call, which coerces or throws.
							 */
							if (argument.source_frame_offset == UINT32_MAX
									|| argument.source_frame_offset
										> INT32_MAX - sizeof(zval)) {
								return false;
							}
							const bool register_boxed_argument =
								node.operands[index]
									!= IRValueRef{Adaptor::FRAME_VALUE}
								&& adaptor->machine_kind(node.operands[index])
									== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
								&& adaptor->machine_value_is_register_authoritative(
									node.operands[index])
								&& val_assignment(adaptor->val_local_idx(
									node.operands[index])) != nullptr;
							if (register_boxed_argument) {
								const auto boxed_local = adaptor->val_local_idx(
									node.operands[index]);
								auto *boxed_assignment = val_assignment(boxed_local);
								const ValueParts parts = val_parts(node.operands[index]);
								int32_t type_part = -1;
								for (uint32_t part = 0; part < parts.count(); ++part) {
									if (parts.representation.parts[part].semantic_role
											== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
										type_part = static_cast<int32_t>(part);
										break;
									}
								}
								if (type_part < 0) {
									return false;
								}
								ValuePartRef type_info{this, boxed_local,
									boxed_assignment,
									static_cast<uint32_t>(type_part), false};
								mov(first_reg, type_info.load_to_reg(), 4);
							} else {
								ASM(MOV32rm, first_reg,
									FE_MEM(frame_reg, 0, FE_NOREG,
										static_cast<int32_t>(
											argument.source_frame_offset
												+ offsetof(zval, u1.type_info))));
							}
							ASM(AND32ri, first_reg, Z_TYPE_MASK);
							if (argument_guard_type == ZEND_MIR_SCALAR_TYPE_I1) {
								auto value_type_valid = text_writer.label_create();
								ASM(CMP32ri, first_reg, IS_FALSE);
								generate_raw_jump(Jump::je, value_type_valid);
								ASM(CMP32ri, first_reg, IS_TRUE);
								generate_raw_jump(Jump::jne, call_slow_target());
								label_place(value_type_valid);
							} else {
								ASM(CMP32ri, first_reg,
									static_cast<int32_t>(
										zval_type(argument_guard_type)));
								generate_raw_jump(Jump::jne, call_slow_target());
							}
							continue;
						}
						/* A moved temporary or call result is defined and
						 * never a reference. */
						if (zend_mir_scalar_type_is_exact(
								argument.exact_type)
								|| argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL
								|| (argument.mode
										== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
									&& (argument.source_operand.slot_kind
											== ZEND_MIR_SOURCE_SLOT_TMP
										|| argument.source_operand.slot_kind
											== ZEND_MIR_SOURCE_SLOT_VAR))) {
							continue;
						}
						const int32_t source_offset =
							static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ argument.source_operand.index)
								* sizeof(zval)
								+ offsetof(zval, u1.type_info));
						ASM(MOVZXr32m8, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG, source_offset));
						ASM(CMP32ri, first_reg, IS_REFERENCE);
						generate_raw_jump(
							argument.mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
								? Jump::jne : Jump::je,
							call_slow_target());
						if (argument.mode
								== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
							ASM(CMP32ri, first_reg, IS_UNDEF);
							generate_raw_jump(Jump::je, call_slow_target());
						} else if (zend_mir_scalar_type_is_exact(
								argument_guard_type)) {
							/*
							 * By reference, the typed callee sees the referenced
							 * value. A mismatch must take the canonical call so
							 * its RECV coerces through the reference.
							 */
							if (argument.source_frame_offset == UINT32_MAX
									|| argument.source_frame_offset > INT32_MAX) {
								return false;
							}
							ASM(MOV64rm, second_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										argument.source_frame_offset)));
							ASM(MOVZXr32m8, second_reg,
								FE_MEM(second_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_reference, val)
											+ offsetof(zval, u1.type_info))));
							if (argument_guard_type == ZEND_MIR_SCALAR_TYPE_I1) {
								auto reference_type_valid = text_writer.label_create();
								ASM(CMP32ri, second_reg, IS_FALSE);
								generate_raw_jump(Jump::je, reference_type_valid);
								ASM(CMP32ri, second_reg, IS_TRUE);
								generate_raw_jump(Jump::jne, call_slow_target());
								label_place(reference_type_valid);
							} else {
								ASM(CMP32ri, second_reg,
									static_cast<int32_t>(
										zval_type(argument_guard_type)));
								generate_raw_jump(Jump::jne, call_slow_target());
							}
						}
					}

					/*
					 * Keep recursive Native calls on Zend's C-stack safety
					 * contract.  The slow path owns the canonical overflow
					 * error and bailout; successful calls stay helper-free.
					 */
#ifdef ZEND_CHECK_STACK_LIMIT
					ASM(MOV64rm, first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								stack_limit))));
					ASM(MOV64rm, first_reg,
						FE_MEM(first_reg, 0, FE_NOREG, 0));
					if (local_component_call) {
						emit_direct_call_stack_guard_position(
							second_reg, call.component_target_index);
						ASM(CMP64rr, second_reg, first_reg);
					} else {
						ASM(CMP64rr, FE_SP, first_reg);
					}
					generate_raw_jump(Jump::jbe, call_slow_target());
#endif

					/* Reserve the current VM-stack page without a C transition. */
					ASM(MOV64rm, first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_top))));
					ASM(MOV64rm, first_reg,
						FE_MEM(first_reg, 0, FE_NOREG, 0));
					ASM(MOV64rm, second_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_end))));
					ASM(MOV64rm, second_reg,
						FE_MEM(second_reg, 0, FE_NOREG, 0));
					ASM(SUB64rr, second_reg, first_reg);
					ASM(CMP64ri, second_reg,
						static_cast<int32_t>(reservation_size));
					generate_raw_jump(Jump::jb, call_slow_target());

					/* The activation leads the callee frame, so the frame ends
					 * at the stack top and a tier-2 host entry can grow it. */
					auto callee_reg = fast_callee_argument_register.cur_reg();
					ASM(LEA64rm, callee_reg,
						FE_MEM(first_reg, 0, FE_NOREG,
							static_cast<int32_t>(activation_size)));
					ASM(LEA64rm, second_reg,
						FE_MEM(first_reg, 0, FE_NOREG,
							static_cast<int32_t>(reservation_size)));
					{
						ScratchReg address{this};
						auto address_reg = address.alloc_gp();
						ASM(MOV64rm, address_reg,
							FE_MEM(context_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_execution_context,
									vm_stack_top))));
						ASM(MOV64mr,
							FE_MEM(address_reg, 0, FE_NOREG, 0), second_reg);
					}
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))),
						callee_reg);

					/* Initialize the exact Zend frame layout. */
					{
						ScratchReg function{this};
						auto function_reg = function.alloc_gp();
						load_callee_function(function_reg);
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, func))),
							function_reg);
					}
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))),
						0);
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, prev_execute_data))),
						frame_reg);
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, symbol_table))),
						0);
					if (run_time_cache.has_value()) {
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, run_time_cache))),
							run_time_cache->cur_reg());
						run_time_cache->reset();
					} else {
						ASM(MOV64mi,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, run_time_cache))),
							0);
					}
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, extra_named_params))),
						0);
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
						ASM(MOV64rm, second_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))));
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))),
							second_reg);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE) {
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
								!= 0) {
							ASM(MOV64rm, second_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_execute_data, This))));
							ASM(MOVZXr32m8, first_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_execute_data, This)
											+ offsetof(zval, u1.type_info))));
							ASM(CMP32ri, first_reg, IS_OBJECT);
							auto called_scope_ready =
								text_writer.label_create();
							generate_raw_jump(
								Jump::jne, called_scope_ready);
							ASM(MOV64rm, second_reg,
								FE_MEM(second_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_object, ce))));
							label_place(called_scope_ready);
						} else {
							ASM(MOV64rm, second_reg,
								FE_MEM(descriptor_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_native_direct_call_descriptor,
										called_scope))));
						}
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))),
							second_reg);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						ASM(MOV64rm, second_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(call.direct_call
									->receiver_source_frame_offset)));
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) == 0) {
							ASM(ADD32mi,
								FE_MEM(second_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))), 1);
						}
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))),
							second_reg);
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0) {
							ASM(MOV32mi,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(call.direct_call
										->receiver_source_frame_offset
										+ offsetof(zval, u1.type_info))),
								IS_UNDEF);
						}
					} else {
						ASM(MOV64mi,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This))),
							0);
					}
					ASM(MOV32mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, This)
								+ offsetof(zval, u1.type_info))),
						ZEND_CALL_NESTED_FUNCTION
							| ((call.direct_call->receiver_kind
										== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS
									|| call.direct_call->receiver_kind
										== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT)
								? ZEND_CALL_HAS_THIS : 0)
							| (call.direct_call->receiver_kind
									== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
								? ZEND_CALL_RELEASE_THIS : 0)
							| (release_extra_arguments
								? ZEND_CALL_FREE_EXTRA_ARGS : 0));
					ASM(MOV32mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, This)
								+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(
							call.direct_call->frame_argument_count));

					/* Publish the caller source position used by stack traces. */
					ASM(MOV64rm, second_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, second_reg,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_op_array, opcodes))));
					if (call.direct_call->source_position != 0) {
						ASM(ADD64ri, second_reg,
							static_cast<int32_t>(
								call.direct_call->source_position
								* sizeof(zend_op)));
					}
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, opline))),
						second_reg);
					ASM(MOV64rm, second_reg,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, second_reg,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_op_array, opcodes))));
					if (fixed_argument_count != 0) {
						ASM(ADD64ri, second_reg,
							static_cast<int32_t>(
								fixed_argument_count * sizeof(zend_op)));
					}
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, opline))),
						second_reg);

					/* Resolve the caller's canonical result zval. */
					if (result_unused) {
						ASM(MOV64rr, second_reg, callee_reg);
						ASM(SUB64ri, second_reg, static_cast<int32_t>(
							activation_size - offsetof(
								zend_native_direct_activation, discarded_return)));
					} else {
						ASM(MOV64rr, second_reg, frame_reg);
						if (call.direct_call->result_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV) {
							ASM(ADD64ri, second_reg, static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ call.direct_call->result_operand.index)
								* sizeof(zval)));
						} else {
							/* The caller frame belongs to this function: its slot
							 * layout is a compile-time constant. */
							ASM(ADD64ri, second_reg, static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ adaptor->plan()->source_frame_variable_count
									+ call.direct_call->result_operand.index)
								* sizeof(zval)));
						}
					}
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, return_value))),
						second_reg);
					/*
					 * The result slot is cleared after the arguments are copied:
					 * the optimizer may assign a temporary argument and the DO
					 * result the same slot.
					 *
					 * Argument copying may need four temporary registers for a
					 * boxed zval. The two preflight temporaries are dead here;
					 * release them and reacquire dedicated metadata temporaries
					 * after all arguments and CVs have been initialized.
					 */
					first.reset();
					second.reset();

					for (uint32_t index = 0; index < argument_count; ++index) {
						zend_mir_call_argument_ref source_argument;
						if (!zend_tpde_call_argument_at(adaptor->plan(),
								call.call_argument_offset + index,
								&source_argument)) {
							return false;
						}
						auto argument_value_ref =
							val_ref(node.operands[index]);
						auto argument = argument_value_ref.part(0);
						const zend_native_direct_call_argument &descriptor_argument =
							call.direct_call->arguments[index];
						if (source_argument.send_opline_index
								>= adaptor->plan()->source_opcode_count) {
							return false;
						}
						const uint8_t source_argument_type =
							adaptor->plan()->source_opcodes[
								source_argument.send_opline_index].op1_type;
						const bool copy_argument =
							source_argument_type == IS_CV
							|| source_argument_type == IS_CONST;
							const uint32_t frame_slot =
							descriptor_argument.ordinal < fixed_argument_count
								? descriptor_argument.ordinal
								: first_extra_argument_slot
									+ descriptor_argument.ordinal
										- fixed_argument_count;
						const int32_t offset = static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT + frame_slot)
								* sizeof(zval));
						const zend_tpde_machine_value_kind argument_kind =
							adaptor->machine_kind(node.operands[index]);
						const bool register_pointer_argument =
							adaptor->machine_value_is_register_authoritative(
								node.operands[index])
							&& (argument_kind
									== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
								|| argument_kind
									== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
								|| argument_kind
									== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
								|| argument_kind
									== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
								|| argument_kind
									== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR);
							if (zend_mir_scalar_type_is_exact(
									descriptor_argument.exact_type)) {
								if (descriptor_argument.source_operand.kind
										== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
									ScratchReg literal{this};
									auto literal_reg = literal.alloc_gp();
									ASM(MOV64ri, literal_reg,
										descriptor_argument.scalar_bits);
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset),
										literal_reg);
									ASM(MOV64mi,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										zval_type(
											descriptor_argument.exact_type)
											+ (descriptor_argument.exact_type
												== ZEND_MIR_SCALAR_TYPE_I1
												? static_cast<uint32_t>(
													descriptor_argument
														.scalar_bits)
												: 0));
								} else if (node.operands[index]
										!= IRValueRef{Adaptor::FRAME_VALUE}) {
									auto argument_reg = argument.load_to_reg();
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1
										&& argument_kind
											== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
										auto type_part = argument_value_ref.part(1);
										auto type_info =
											std::move(type_part).into_scratch();
										ASM(CMP32ri, type_info.cur_reg(), IS_TRUE);
										generate_raw_set(Jump::je, argument_reg);
									}
									if (val_parts(node.operands[index]).bank
											== tpde::x64::PlatformConfig::FP_BANK) {
										ASM(SSE_MOVSDmr,
											FE_MEM(callee_reg, 0, FE_NOREG, offset),
											argument_reg);
									} else {
										ASM(MOV64mr,
											FE_MEM(callee_reg, 0, FE_NOREG, offset),
											argument_reg);
									}
									ASM(MOV64mi,
										FE_MEM(callee_reg, 0, FE_NOREG, offset + 8),
										0);
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										ScratchReg kind{this};
										auto kind_reg = kind.alloc_gp();
										ASM(MOV64rr, kind_reg, argument_reg);
										ASM(ADD64ri, kind_reg, IS_FALSE);
										ASM(MOV32mr,
											FE_MEM(callee_reg, 0, FE_NOREG,
												offset + 8),
											kind_reg);
									} else {
										ASM(MOV32mi,
											FE_MEM(callee_reg, 0, FE_NOREG,
												offset + 8),
											static_cast<int32_t>(zval_type(
												descriptor_argument.exact_type)));
									}
								} else {
									if (descriptor_argument.source_frame_offset
											> INT32_MAX) {
										return false;
									}
									ScratchReg payload{this};
									auto payload_reg = payload.alloc_gp();
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										ASM(MOV32rm, payload_reg,
											FE_MEM(frame_reg, 0, FE_NOREG,
												static_cast<int32_t>(
													descriptor_argument
														.source_frame_offset
													+ offsetof(zval, u1.type_info))));
										ASM(CMP32ri, payload_reg, IS_TRUE);
										generate_raw_set(Jump::je, payload_reg);
									} else {
										ASM(MOV64rm, payload_reg,
											FE_MEM(frame_reg, 0, FE_NOREG,
												static_cast<int32_t>(
													descriptor_argument
														.source_frame_offset)));
									}
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset),
										payload_reg);
									ASM(MOV64mi,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8), 0);
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										ScratchReg kind{this};
										auto kind_reg = kind.alloc_gp();
										ASM(MOV64rr, kind_reg, payload_reg);
										ASM(ADD64ri, kind_reg, IS_FALSE);
									ASM(MOV32mr,
										FE_MEM(callee_reg, 0,
											FE_NOREG, offset + 8),
										kind_reg);
								} else {
										ASM(MOV32mi,
											FE_MEM(callee_reg, 0,
												FE_NOREG, offset + 8),
											static_cast<int32_t>(zval_type(
												descriptor_argument
													.exact_type)));
									}
								}
							} else if (descriptor_argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL
								&& node.operands[frame_operand]
									== IRValueRef{Adaptor::FRAME_VALUE}
								&& adaptor->plan()->source_literals != nullptr
								&& descriptor_argument.source_operand.index
									< adaptor->plan()->source_literal_count
								&& store_scalar_literal(callee_reg, offset,
									&adaptor->plan()->source_literals[
										descriptor_argument.source_operand.index])) {
								/* Stored with immediates. */
							} else if (descriptor_argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
								ScratchReg source_address{this};
								ScratchReg low_word{this};
								ScratchReg high_word{this};
								auto source_address_reg = source_address.alloc_gp();
								auto low_word_reg = low_word.alloc_gp();
								auto high_word_reg = high_word.alloc_gp();
								ASM(MOV64rm, source_address_reg,
									FE_MEM(frame_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_execute_data, func))));
								ASM(MOV64rm, source_address_reg,
									FE_MEM(source_address_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_op_array, literals))));
								const uint64_t source_literal_offset =
									static_cast<uint64_t>(
										descriptor_argument.source_operand.index)
										* sizeof(zval);
								if (source_literal_offset > INT32_MAX) {
									return false;
								}
								if (source_literal_offset != 0) {
									ASM(ADD64ri, source_address_reg,
										static_cast<int32_t>(source_literal_offset));
								}
								ASM(MOV64rm, low_word_reg,
									FE_MEM(source_address_reg, 0, FE_NOREG, 0));
								ASM(MOV64rm, high_word_reg,
									FE_MEM(source_address_reg, 0, FE_NOREG, 8));
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset),
									low_word_reg);
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset + 8),
									high_word_reg);
								ASM(TEST32ri, high_word_reg,
									IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
								auto copied = text_writer.label_create();
								generate_raw_jump(Jump::je, copied);
								ASM(ADD32mi,
									FE_MEM(low_word_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_refcounted_h, refcount))), 1);
								label_place(copied);
							} else if (register_pointer_argument) {
								auto pointer =
									std::move(argument).into_scratch();
								ScratchReg type_info{this};
								auto type_info_reg = type_info.alloc_gp();
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset),
									pointer.cur_reg());
								if (!emit_machine_zval_type_info(
										argument_kind, pointer.cur_reg(),
										type_info_reg)) {
									return false;
								}
								ASM(MOV32mr,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + 8),
									type_info_reg);
								ASM(MOV32mi,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + static_cast<int32_t>(
											offsetof(zval, u2))),
									0);
								if (copy_argument) {
									if (!emit_pointer_addref(
											argument_kind, pointer.cur_reg())) {
										return false;
									}
								} else {
									if (descriptor_argument.source_frame_offset
											> INT32_MAX) {
										return false;
									}
									ASM(MOV32mi,
										FE_MEM(frame_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												descriptor_argument
													.source_frame_offset
												+ offsetof(zval, u1.type_info))),
										IS_UNDEF);
								}
							} else if (adaptor->machine_kind(
									node.operands[index])
										== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
									&& adaptor
										->machine_value_is_register_authoritative(
											node.operands[index])) {
								auto low_word =
									std::move(argument).into_scratch();
								auto high_part =
									argument_value_ref.part(1);
								auto high_word =
									std::move(high_part).into_scratch();
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset),
									low_word.cur_reg());
								ASM(MOV32mr,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + 8),
									high_word.cur_reg());
								ASM(MOV32mi,
									FE_MEM(callee_reg, 0, FE_NOREG,
										offset + static_cast<int32_t>(
											offsetof(zval, u2))),
									0);
								if (copy_argument) {
									ASM(TEST32ri, high_word.cur_reg(), IS_TYPE_REFCOUNTED
											<< Z_TYPE_FLAGS_SHIFT);
									auto copied = text_writer.label_create();
									generate_raw_jump(Jump::je, copied);
									ASM(ADD32mi,
										FE_MEM(low_word.cur_reg(), 0,
											FE_NOREG,
											static_cast<int32_t>(offsetof(
												zend_refcounted_h, refcount))),
										1);
									label_place(copied);
								} else {
									if (descriptor_argument.source_frame_offset
											> INT32_MAX) {
										return false;
									}
									ASM(MOV32mi,
										FE_MEM(frame_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												descriptor_argument
													.source_frame_offset
												+ offsetof(zval, u1.type_info))),
										IS_UNDEF);
								}
							} else if (node.operands[index]
										!= IRValueRef{Adaptor::FRAME_VALUE}
									&& (argument_kind
											== ZEND_TPDE_MACHINE_VALUE_I64
										|| argument_kind
											== ZEND_TPDE_MACHINE_VALUE_F64
										|| argument_kind
											== ZEND_TPDE_MACHINE_VALUE_BOOL)) {
								/* A scalar in a register although the call
								 * descriptor records no exact type: store its
								 * payload and type instead of copying a slot. */
								auto payload_reg = argument.load_to_reg();
								ScratchReg bits{this};
								auto bits_reg = bits.alloc_gp();
								if (argument_kind
										== ZEND_TPDE_MACHINE_VALUE_F64) {
									ASM(SSE_MOVQ_X2Grr, bits_reg, payload_reg);
								} else {
									ASM(MOV64rr, bits_reg, payload_reg);
								}
								ASM(MOV64mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset),
									bits_reg);
								if (argument_kind
										== ZEND_TPDE_MACHINE_VALUE_BOOL) {
									ASM(ADD64ri, bits_reg, IS_FALSE);
									ASM(MOV64mr,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										bits_reg);
								} else {
									ASM(MOV64mi,
										FE_MEM(callee_reg, 0, FE_NOREG,
											offset + 8),
										argument_kind
												== ZEND_TPDE_MACHINE_VALUE_F64
											? IS_DOUBLE : IS_LONG);
								}
								if (!copy_argument) {
									if (descriptor_argument.source_frame_offset
											> INT32_MAX) {
										return false;
									}
									ASM(MOV32mi,
										FE_MEM(frame_reg, 0, FE_NOREG,
											static_cast<int32_t>(
												descriptor_argument
													.source_frame_offset
												+ offsetof(zval, u1.type_info))),
										IS_UNDEF);
								}
							} else {
								auto source_frame_reg = argument.load_to_reg();
							if (descriptor_argument.source_frame_offset
									> INT32_MAX) {
								return false;
							}
							const int32_t source_offset =
								static_cast<int32_t>(
									descriptor_argument.source_frame_offset);
							/* Two scratch registers: the source slot is addressed through
							 * its frame and the type info is the low half of the high word. */
							ScratchReg low_word{this};
							ScratchReg high_word{this};
							auto low_word_reg = low_word.alloc_gp();
							auto high_word_reg = high_word.alloc_gp();
							const AsmReg source_address_reg = source_frame_reg;
							ASM(MOV64rm, low_word_reg,
								FE_MEM(source_address_reg, 0, FE_NOREG, source_offset));
							ASM(MOV64rm, high_word_reg,
								FE_MEM(source_address_reg, 0, FE_NOREG, source_offset + 8));
							ASM(MOV64mr,
								FE_MEM(callee_reg, 0, FE_NOREG, offset),
								low_word_reg);
							ASM(MOV64mr,
								FE_MEM(callee_reg, 0, FE_NOREG, offset + 8),
								high_word_reg);
							if (copy_argument) {
								ASM(TEST32ri, high_word_reg,
									IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
								auto copied = text_writer.label_create();
								generate_raw_jump(Jump::je, copied);
								ASM(ADD32mi,
									FE_MEM(low_word_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_refcounted_h, refcount))),
									1);
								label_place(copied);
							} else {
								ASM(MOV32mi,
									FE_MEM(source_address_reg, 0, FE_NOREG,
										source_offset + static_cast<int32_t>(offsetof(zval, u1.type_info))),
									IS_UNDEF);
							}
						}
					}
					{
						ScratchReg result_slot{this};
						auto result_slot_reg = result_slot.alloc_gp();
						ASM(MOV64rm, result_slot_reg,
							FE_MEM(callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, return_value))));
						ASM(MOV32mi,
							FE_MEM(result_slot_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zval, u1.type_info))),
							IS_UNDEF);
					}
					for (uint32_t index = 0;
							index < callee_argument_count; ++index) {
						const uint32_t literal_index =
							zend_native_direct_call_default_literals_const(
								call.direct_call)[index];
						if (literal_index == UINT32_MAX) {
							continue;
						}
						const int32_t offset = static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
						const zend_function *expected =
							call.direct_call->expected_function;
						if (expected != nullptr
								&& expected->type == ZEND_USER_FUNCTION
								&& literal_index
									< static_cast<uint32_t>(
										expected->op_array.last_literal)
								&& store_scalar_literal(callee_reg, offset,
									&expected->op_array.literals[
										literal_index])) {
							continue;
						}
						ScratchReg source_address{this};
						ScratchReg low_word{this};
						ScratchReg high_word{this};
						auto source_address_reg = source_address.alloc_gp();
						auto low_word_reg = low_word.alloc_gp();
						auto high_word_reg = high_word.alloc_gp();

						load_callee_function(source_address_reg);
						ASM(MOV64rm, source_address_reg,
							FE_MEM(source_address_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_op_array, literals))));
						if (literal_index != 0) {
							ASM(ADD64ri, source_address_reg,
								static_cast<int32_t>(
									literal_index * sizeof(zval)));
						}
						ASM(MOV64rm, low_word_reg,
							FE_MEM(source_address_reg, 0, FE_NOREG, 0));
						ASM(MOV64rm, high_word_reg,
							FE_MEM(source_address_reg, 0, FE_NOREG, 8));
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG, offset),
							low_word_reg);
						ASM(MOV64mr,
							FE_MEM(callee_reg, 0, FE_NOREG, offset + 8),
							high_word_reg);
						ASM(TEST32ri, high_word_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						auto copied = text_writer.label_create();
						generate_raw_jump(Jump::je, copied);
						ASM(ADD32mi,
							FE_MEM(low_word_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))),
							1);
						label_place(copied);
					}
					/* ZVAL_UNDEF writes only the type, as the VM's frame
					 * setup does. */
					for (uint32_t index = fixed_argument_count;
							index < compiled_variable_count; ++index) {
						const int32_t offset = static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval)
							+ offsetof(zval, u1.type_info));
						ASM(MOV32mi,
							FE_MEM(callee_reg, 0, FE_NOREG, offset), IS_UNDEF);
					}

					/* Complete and link the stable trailing activation. */
					ScratchReg metadata_first{this};
					ScratchReg metadata_second{this};
					auto metadata_first_reg = metadata_first.alloc_gp();
					auto metadata_second_reg = metadata_second.alloc_gp();
					/* The activation register is biased so that every field
					 * is reachable with an 8-bit displacement. */
					constexpr int32_t activation_bias = 0x80;
					static_assert(sizeof(zend_native_direct_activation)
						<= 2 * activation_bias);
					const auto activation_field = [&](size_t field) {
						return FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(field) - activation_bias);
					};
					ASM(LEA64rm, metadata_second_reg,
						FE_MEM(callee_reg, 0, FE_NOREG,
							activation_bias
								- static_cast<int32_t>(activation_size)));
					ASM(MOV64mr,
						activation_field(offsetof(
							zend_native_direct_activation, caller)),
						frame_reg);
					ASM(MOV64mr,
						activation_field(offsetof(
							zend_native_direct_activation, callee)),
						callee_reg);
					if (local_component_call) {
						ASM(MOV64mi,
							activation_field(offsetof(
								zend_native_direct_activation, cell)),
							0);
					} else {
						ASM(MOV64mr,
							activation_field(offsetof(
								zend_native_direct_activation, cell)),
							cell_reg);
					}
					if (local_component_call) {
						ASM(MOV64mi,
							activation_field(offsetof(
								zend_native_direct_activation, code)),
							0);
					} else {
						published_code_reg = published_code.alloc_gp();
						ASM(MOV64rm, published_code_reg,
							FE_MEM(FE_BP, 0, FE_NOREG, published_code_slot));
						ASM(MOV64mr,
							activation_field(offsetof(
								zend_native_direct_activation, code)),
							published_code_reg);
					}
					ASM(MOV64mr,
						activation_field(offsetof(
							zend_native_direct_activation, descriptor)),
						descriptor_reg);
					/* status and the first four flags share one quadword,
					 * the remaining flags (fiber_published is rewritten on
					 * every suspend) the next one. */
					static_assert(IS_UNDEF == 0);
					static_assert(offsetof(zend_native_direct_activation,
							status) % 8 == 0);
					static_assert(offsetof(zend_native_direct_activation,
							uses_discarded_return)
						== offsetof(zend_native_direct_activation, status) + 4);
					static_assert(offsetof(zend_native_direct_activation,
							raw_arguments_owned)
						== offsetof(zend_native_direct_activation, status) + 5);
					static_assert(offsetof(zend_native_direct_activation,
							frame_initialized)
						== offsetof(zend_native_direct_activation, status) + 6);
					static_assert(offsetof(zend_native_direct_activation,
							frame_requires_finish)
						== offsetof(zend_native_direct_activation, status) + 7);
					static_assert(offsetof(zend_native_direct_activation,
							cell_active)
						== offsetof(zend_native_direct_activation, status) + 8);
					static_assert(offsetof(zend_native_direct_activation,
							fiber_published)
						< offsetof(zend_native_direct_activation, status) + 16);
					static_assert(sizeof(zend_native_direct_activation)
						>= offsetof(zend_native_direct_activation, status) + 16);
					ASM(MOV64ri, metadata_first_reg,
						static_cast<int64_t>(
							(uint64_t{result_unused ? 1u : 0u} << 32)
							| (uint64_t{1} << 48) | (uint64_t{1} << 56)));
					ASM(MOV64mr,
						activation_field(offsetof(
							zend_native_direct_activation, status)),
						metadata_first_reg);
					/* Clears every flag after cell_active, heap_allocated too. */
					static_assert(offsetof(zend_native_direct_activation,
							heap_allocated)
						< offsetof(zend_native_direct_activation, status) + 16);
					ASM(MOV64mi,
						activation_field(offsetof(
							zend_native_direct_activation, status) + 8),
						generation_leased ? 0 : 1);
					ASM(MOV64mi,
						activation_field(offsetof(
							zend_native_direct_activation, discarded_return)),
						0);
					ASM(MOV64mi,
						activation_field(offsetof(
							zend_native_direct_activation, discarded_return)
							+ 8),
						0);
					ASM(MOV64mi,
						activation_field(offsetof(
							zend_native_direct_activation, pending_call)),
						0);
					ASM(MOV64rm, metadata_first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								active_direct_call))));
					ASM(MOV64rm, descriptor_reg,
						FE_MEM(metadata_first_reg, 0, FE_NOREG, 0));
					ASM(MOV64mr,
						activation_field(offsetof(
							zend_native_direct_activation, previous)),
						descriptor_reg);
					ASM(LEA64rm, descriptor_reg,
						activation_field(0));
					ASM(MOV64mr,
						FE_MEM(metadata_first_reg, 0, FE_NOREG, 0),
						descriptor_reg);
					ASM(MOV64rm, metadata_first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								current_execute_data))));
					ASM(MOV64mr,
						FE_MEM(metadata_first_reg, 0, FE_NOREG, 0),
						callee_reg);
					if (!generation_leased) {
						ASM(ADD32mi,
							FE_MEM(cell_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_entry_cell, active_calls))),
							1);
					}

					/* Bind component-local edges directly to TPDE's local
					 * function symbol. */
					metadata_first.reset();
					metadata_second.reset();
					ValuePart callee_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					if (variadic_frame) {
						fast_callee_argument_register.reset();
						published_code.reset();
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						ValuePart receive_status{
							tpde::x64::PlatformConfig::GP_BANK, 4};
						{
							tpde::x64::CCAssignerSysV receive_assigner{false};
							CallBuilder receive_builder{*this, receive_assigner};
							receive_builder.add_arg(
								CallArg{node.operands[frame_operand + 6]});
							add_const_arg(receive_builder,
								ZEND_RECV_VARIADIC, 4);
							add_const_arg(receive_builder,
								fixed_argument_count + 1, 4);
							add_const_arg(receive_builder, UINT64_C(0), 8);
							add_const_arg(receive_builder, UINT64_C(0), 4);
							receive_builder.add_arg(ValuePart{
								uint64_t{IS_CV} | ((uint64_t{ZEND_CALL_FRAME_SLOT}
										+ fixed_argument_count) * sizeof(zval) << 8),
								8, tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
							add_const_arg(receive_builder,
								fixed_argument_count, 4);
							receive_builder.call(runtime_symbol(
								ZEND_NATIVE_HELPER_RECEIVE_EXPLICIT_PENDING));
							receive_builder.add_ret(
								receive_status, tpde::CCAssignment{});
						}
						/* Exact descriptor guards make receive failure unreachable. */
						receive_status.reset(this);
						auto [receive_frame_ref, receive_frame] =
							val_ref_single(node.operands[frame_operand + 7]);
						auto receive_frame_reg = receive_frame.load_to_reg();
						ScratchReg received_callee{this};
						auto received_callee_reg = received_callee.alloc_gp();
						ASM(MOV64rm, received_callee_reg,
							FE_MEM(receive_frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, call))));
						receive_frame.reset();
						if (received_callee_reg == tpde::x64::AsmReg{tpde::x64::AsmReg::R11}) {
							ScratchReg moved_callee{this};
							auto moved_callee_reg = moved_callee.alloc_gp();
							mov(moved_callee_reg, received_callee_reg, 8);
							received_callee = std::move(moved_callee);
						}
						/* The activation's code: the validated value, not
						 * the cell, which may have been republished. */
						if (!local_component_call) {
							published_code_reg = published_code.alloc_gp();
							ASM(MOV64rm, published_code_reg,
								FE_MEM(FE_BP, 0, FE_NOREG,
									published_code_slot));
						}
						callee_value.set_value(
							this, std::move(received_callee));
					} else {
						callee_value.set_value(this,
							std::move(fast_callee_argument_register));
					}
					ValuePart fast_status{
						tpde::x64::PlatformConfig::GP_BANK, 4};
					if (local_component_call) {
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						published_code.reset();
						tpde::x64::CCAssignerSysV fast_assigner{false};
						CallBuilder fast_builder{*this, fast_assigner};
						fast_builder.add_arg(
							std::move(callee_value), tpde::CCAssignment{});
						fast_builder.add_arg(
							CallArg{node.operands[context_operand + 1]});
						fast_builder.call(
							this->func_syms[call.component_target_index]);
						fast_builder.add_ret(
							fast_status, tpde::CCAssignment{});
					} else {
						/* R11 carries the entry target; see the direct path above. */
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						if (published_code_reg == tpde::x64::AsmReg{tpde::x64::AsmReg::R11}) {
							ScratchReg moved_code{this};
							auto moved_code_reg = moved_code.alloc_gp();
							mov(moved_code_reg, published_code_reg, 8);
							published_code = std::move(moved_code);
							published_code_reg = moved_code_reg;
						}
						ScratchReg entry_argument{this};
						auto entry_argument_reg =
							entry_argument.alloc_specific(
								tpde::x64::AsmReg::R11);
						ASM(MOV64rm, entry_argument_reg,
							FE_MEM(published_code_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_native_code, entry))));
						ValuePart entry_value{
							tpde::x64::PlatformConfig::GP_BANK, 8};
						entry_value.set_value(
							this, std::move(entry_argument));
						published_code.reset();
						tpde::x64::CCAssignerSysV fast_assigner{false};
						CallBuilder fast_builder{*this, fast_assigner};
						fast_builder.add_arg(
							std::move(callee_value), tpde::CCAssignment{});
						fast_builder.add_arg(
							CallArg{node.operands[context_operand + 1]});
						fast_builder.call(std::move(entry_value));
						fast_builder.add_ret(
							fast_status, tpde::CCAssignment{});
					}

					/* Reacquire frame/context after the native ABI call. */
					auto [post_frame_ref, post_frame] =
						val_ref_single(node.operands[frame_operand + 1]);
					auto post_frame_scratch =
						std::move(post_frame).into_scratch();
					auto post_frame_reg = post_frame_scratch.cur_reg();
					auto [post_context_ref, post_context] =
						val_ref_single(node.operands[context_operand + 2]);
					auto post_context_scratch =
						std::move(post_context).into_scratch();
					auto post_context_reg = post_context_scratch.cur_reg();
					ScratchReg post_callee{this};
					ScratchReg activation{this};
					ScratchReg probe{this};
					auto post_callee_reg = post_callee.alloc_gp();
					auto activation_reg = activation.alloc_gp();
					auto probe_reg = probe.alloc_gp();
					ASM(MOV64rm, post_callee_reg,
						FE_MEM(post_frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								active_direct_call))));
					ASM(MOV64rm, activation_reg,
						FE_MEM(probe_reg, 0, FE_NOREG, 0));
					{
						const auto status_reg =
							fast_status.cur_reg_or_load(this);
						ASM(MOV32mr,
							FE_MEM(activation_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_direct_activation, status))),
							status_reg);
						static_assert(ZEND_NATIVE_RETURNED == 0);
						ASM(TEST32rr, status_reg, status_reg);
					}
					fast_status.reset(this);
					auto complete_fast = text_writer.label_create();
					generate_raw_jump(Jump::jne, complete_fast);
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context, exception))));
					ASM(MOV64rm, probe_reg,
						FE_MEM(probe_reg, 0, FE_NOREG, 0));
					ASM(TEST64rr, probe_reg, probe_reg);
					generate_raw_jump(Jump::jne, complete_fast);
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_interrupt))));
					ASM(CMP8mi, FE_MEM(probe_reg, 0, FE_NOREG, 0), 0);
					generate_raw_jump(Jump::jne, complete_fast);
					/*
					 * Dynamic local-symbol operations may attach a HashTable to
					 * an otherwise inlineable direct callee. Its destruction
					 * belongs to the canonical frame finisher, not the
					 * helper-free scalar/CV release loop below.
					 */
					ASM(MOV32rm, probe_reg,
						FE_MEM(post_callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, This)
									+ offsetof(zval, u1.type_info))));
					ASM(TEST32ri, probe_reg, ZEND_CALL_ALLOCATED);
					generate_raw_jump(Jump::jne, complete_fast);
					ASM(TEST32ri, probe_reg, ZEND_CALL_HAS_SYMBOL_TABLE);
					generate_raw_jump(Jump::jne, complete_fast);
					const bool boxed_result_written =
						!result_unused
						&& ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT)
								== 0
							|| call.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_NONE);
					if (!boxed_result_written) {
						ASM(MOV64rm, probe_reg,
							FE_MEM(post_callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, return_value))));
					}
					if (result_unused) {
						ASM(CMP32mi, FE_MEM(probe_reg, 0, FE_NOREG, 8),
							IS_DOUBLE);
						generate_raw_jump(Jump::ja, complete_fast);
					} else if (boxed_result_written) {
						/* The callee already wrote the complete boxed zval. */
					} else if (call.direct_call->result_type
							== ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(MOV32rm, probe_reg,
							FE_MEM(probe_reg, 0, FE_NOREG, 8));
						ASM(CMP32ri, probe_reg, IS_FALSE);
						generate_raw_jump(Jump::jb, complete_fast);
						ASM(CMP32ri, probe_reg, IS_TRUE);
						generate_raw_jump(Jump::ja, complete_fast);
						} else {
							ASM(CMP32mi, FE_MEM(probe_reg, 0, FE_NOREG, 8),
								static_cast<int32_t>(zval_type(
									call.direct_call->result_type)));
							generate_raw_jump(Jump::jne, complete_fast);
						}
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0) {
							generate_raw_jump(Jump::jmp, complete_fast);
						}
					}
						/*
					 * Mirror Zend's sequential frame cleanup. A decremented slot
					 * is made UNDEF before advancing, so the canonical rare path
					 * can resume safely if a later alias owns the final reference.
					 */
					{
						ScratchReg counted{this};
						auto counted_reg = counted.alloc_gp();
						for (uint32_t index = 0;
								index < compiled_variable_count; ++index) {
							if (!compiled_variable_used(index)) {
								continue;
							}
							const int32_t offset = static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
							ASM(TEST8mi,
								FE_MEM(post_callee_reg, 0, FE_NOREG,
									offset + static_cast<int32_t>(
										offsetof(zval, u1.v.type_flags))),
								IS_TYPE_REFCOUNTED);
							auto released = text_writer.label_create();
							generate_raw_jump(Jump::je, released);
							ASM(MOV64rm, counted_reg,
								FE_MEM(post_callee_reg, 0, FE_NOREG, offset));
							ASM(CMP32mi,
								FE_MEM(counted_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							generate_raw_jump(Jump::je, complete_fast);
							ASM(SUB32mi,
								FE_MEM(counted_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							ASM(MOV32mi,
								FE_MEM(post_callee_reg, 0, FE_NOREG,
									offset + static_cast<int32_t>(
										offsetof(zval, u1.type_info))),
								IS_UNDEF);
							label_place(released);
						}
						for (uint32_t index = 0;
								index < argument_count; ++index) {
							const uint32_t ordinal =
								call.direct_call->arguments[index].ordinal;
							if (ordinal < fixed_argument_count) {
								continue;
							}
							const uint32_t frame_slot =
								first_extra_argument_slot
									+ ordinal - fixed_argument_count;
							const int32_t offset = static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT + frame_slot)
									* sizeof(zval));
							ASM(TEST8mi,
								FE_MEM(post_callee_reg, 0, FE_NOREG,
									offset + static_cast<int32_t>(
										offsetof(zval, u1.v.type_flags))),
								IS_TYPE_REFCOUNTED);
							auto released = text_writer.label_create();
							generate_raw_jump(Jump::je, released);
							ASM(MOV64rm, counted_reg,
								FE_MEM(post_callee_reg, 0, FE_NOREG, offset));
							ASM(CMP32mi,
								FE_MEM(counted_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							generate_raw_jump(Jump::je, complete_fast);
							ASM(SUB32mi,
								FE_MEM(counted_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							ASM(MOV32mi,
								FE_MEM(post_callee_reg, 0, FE_NOREG,
									offset + static_cast<int32_t>(
										offsetof(zval, u1.type_info))),
								IS_UNDEF);
							label_place(released);
						}
					}
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						ASM(MOV64rm, probe_reg,
							FE_MEM(post_callee_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_execute_data, This))));
						ASM(SUB32mi,
							FE_MEM(probe_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))), 1);
					}

					/* Helper-free successful completion. */
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								current_execute_data))));
					ASM(MOV64mr,
						FE_MEM(probe_reg, 0, FE_NOREG, 0), post_frame_reg);
					ASM(MOV64rm, probe_reg,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, pending_call))));
					ASM(MOV64mr,
						FE_MEM(post_frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_execute_data, call))),
						probe_reg);
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								active_direct_call))));
					ASM(MOV64rm, activation_reg,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, previous))));
					ASM(MOV64mr,
						FE_MEM(probe_reg, 0, FE_NOREG, 0), activation_reg);
					if (!generation_leased) {
						auto fast_cell = image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
							call.call_site->target_id);
						auto fast_cell_scratch =
							std::move(fast_cell).into_scratch(this);
						ASM(SUB32mi,
							FE_MEM(fast_cell_scratch.cur_reg(), 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_entry_cell, active_calls))),
							1);
						fast_cell_scratch.reset();
					}
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_top))));
					ASM(SUB64ri, post_callee_reg,
						static_cast<int32_t>(activation_size));
					ASM(MOV64mr,
						FE_MEM(probe_reg, 0, FE_NOREG, 0), post_callee_reg);
					generate_raw_jump(Jump::jmp, successful);

					/* Rare completion retains full exception/interrupt cleanup. */
					label_place(complete_fast);
					post_frame_scratch.reset();
					post_context_scratch.reset();
					post_callee.reset();
					probe.reset();
					ASM(MOV32rm, activation_reg,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, status))));
					/*
					 * The status is the fourth SysV argument. Materialize it
					 * directly in RCX: left in whichever register the
					 * activation happened to get (often RDI), it stays fixed
					 * while the earlier arguments are placed and their
					 * eviction of that register would be invalid.
					 */
					ValuePart finish_status_argument{
						tpde::x64::PlatformConfig::GP_BANK, 4};
					if (activation_reg
							== tpde::x64::AsmReg{tpde::x64::AsmReg::CX}) {
						finish_status_argument.set_value(
							this, std::move(activation));
					} else {
						ScratchReg finish_status_register{this};
						auto finish_status_reg =
							finish_status_register.alloc_specific(
								tpde::x64::AsmReg::CX);
						mov(finish_status_reg, activation_reg, 4);
						activation.reset();
						finish_status_argument.set_value(
							this, std::move(finish_status_register));
					}
					{
						auto [finish_frame_ref, finish_frame] =
							val_ref_single(node.operands[frame_operand + 2]);
						(void) finish_frame_ref;
						finish_frame.reset();
					}
					tpde::x64::CCAssignerSysV finish_assigner{false};
					CallBuilder finish_builder{*this, finish_assigner};
					finish_builder.add_arg(
						CallArg{node.operands[frame_operand + 3]});
					finish_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id, &finish_assigner), tpde::CCAssignment{});
					finish_builder.add_arg(
						CallArg{node.operands[context_operand + 3]});
					finish_builder.add_arg(
						std::move(finish_status_argument),
						tpde::CCAssignment{});
					finish_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE));
					ValuePart finish_status{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					ValuePart finish_payload{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					finish_builder.add_ret(
						finish_status, tpde::CCAssignment{});
					finish_builder.add_ret(
						finish_payload, tpde::CCAssignment{});
					finish_payload.reset(this);
					emit_status_tail(std::move(finish_status),
						call.exception_block_id);
					finish_status.reset(this);
					generate_raw_jump(Jump::jmp, successful);
					}
					if (node.kind == Adaptor::InstKind::GuardedFast) {
						if (node.continuation_block == UINT32_MAX) {
							return false;
						}
						label_place(successful);
						finish_generated_result();
						generate_uncond_branch(
							IRBlockRef{node.continuation_block});
						return true;
					}
					label_place(slow_path);
				}
				if (split_cold && node.operands.size() < 4) {
					return false;
				}
				ValuePart callee{tpde::x64::PlatformConfig::GP_BANK, 8};
				ValuePart entry{tpde::x64::PlatformConfig::GP_BANK, 8};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(copy_fixed_argument(
						canonical_frame_register(), &assigner),
						tpde::CCAssignment{});
					if (!split_cold) {
						auto frame_liveness =
							val_ref(node.operands[
								frame_operand + slow_enter_frame_use]);
						(void) frame_liveness;
					}
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site->target_id, &assigner), tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					if (split_cold) {
						builder.add_arg(CallArg{
							node.operands[context_operand]});
					} else {
						builder.add_arg(copy_fixed_argument(
							canonical_value_register(IRValueRef{
								Adaptor::EXECUTION_CONTEXT_ARGUMENT}), &assigner),
							tpde::CCAssignment{});
						auto context_liveness =
							val_ref(node.operands[
								context_operand + slow_enter_context_use]);
						(void) context_liveness;
					}
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DIRECT_USER_CALL_ENTER));
					builder.add_ret(callee, tpde::CCAssignment{});
					builder.add_ret(entry, tpde::CCAssignment{});
				}
				ValuePart entry_status_argument = call_resolved_entry(
					std::move(callee), std::move(entry),
					[&](CallBuilder &builder,
							tpde::x64::CCAssignerSysV &assigner) {
						if (split_cold) {
							builder.add_arg(CallArg{
								node.operands[context_operand + 1]});
							return;
						}
						builder.add_arg(copy_fixed_argument(
							canonical_value_register(IRValueRef{
								Adaptor::EXECUTION_CONTEXT_ARGUMENT}),
							&assigner), tpde::CCAssignment{});
						auto context_liveness =
							val_ref(node.operands[
								context_operand + slow_entry_context_use]);
						(void) context_liveness;
					});
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register(), &assigner),
					tpde::CCAssignment{});
				{
					auto frame_liveness =
						val_ref(node.operands[split_cold
							? frame_operand
							: frame_operand + slow_leave_frame_use]);
					(void) frame_liveness;
				}
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
					call.id, &assigner), tpde::CCAssignment{});
				if (split_cold) {
					builder.add_arg(CallArg{
						node.operands[context_operand + 2]});
				} else {
					builder.add_arg(copy_fixed_argument(
						canonical_value_register(IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT}), &assigner),
						tpde::CCAssignment{});
					auto context_liveness =
						val_ref(node.operands[
							context_operand + slow_leave_context_use]);
					(void) context_liveness;
				}
				builder.add_arg(
					std::move(entry_status_argument), tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE));
				ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
				builder.add_ret(status, tpde::CCAssignment{});
				builder.add_ret(payload, tpde::CCAssignment{});
				emit_status_tail(std::move(status),
					call.exception_block_id);
				if (node.kind == Adaptor::InstKind::GuardedCold) {
					if (node.continuation_block == UINT32_MAX) {
						return false;
					}
					payload.reset(this);
					load_generated_result(canonical_frame_register());
					generate_uncond_branch(
						IRBlockRef{node.continuation_block});
					return true;
				}
				if (generated_fast_path) {
					payload.reset(this);
					label_place(successful);
					finish_generated_result();
				} else if (node.has_result
						&& adaptor->machine_kind(node.result)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					payload.reset(this);
					auto [result_frame_ref, result_frame] =
						val_ref_single(node.operands[frame_operand + 2]);
					auto result_frame_scratch =
						std::move(result_frame).into_scratch();
					auto result_frame_reg =
						result_frame_scratch.cur_reg();
					ScratchReg result_slot{this};
					auto result_slot_reg = result_slot.alloc_gp();
					ASM(MOV64rr, result_slot_reg, result_frame_reg);
					if (call.direct_call->result_operand.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV) {
						ASM(ADD64ri, result_slot_reg,
							static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ call.direct_call
										->result_operand.index)
								* sizeof(zval)));
					} else {
						/* The caller frame belongs to this function: its slot
						 * layout is a compile-time constant. */
						ASM(ADD64ri, result_slot_reg, static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT
								+ adaptor->plan()->source_frame_variable_count
								+ call.direct_call->result_operand.index)
							* sizeof(zval)));
					}
					auto result = result_ref(node.result);
					const ValueParts parts = val_parts(node.result);
					for (uint32_t part = 0;
							part < parts.count(); ++part) {
						auto value = result.part(part);
						auto value_reg = value.alloc_reg();
						const zend_tpde_machine_part_role role =
							parts.representation.parts[part].semantic_role;
						if (role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
							ASM(MOV64rm, value_reg,
								FE_MEM(result_slot_reg, 0, FE_NOREG, 0));
						} else if (role
								== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
							ASM(MOV32rm, value_reg,
								FE_MEM(result_slot_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zval, u1.type_info))));
						} else {
							return false;
						}
						value.set_modified();
					}
				} else if (node.has_result) {
					set_payload_result(node.result, std::move(payload));
				} else {
					payload.reset(this);
				}
				return true;
			}
			if (call.user_call != nullptr
					&& call.user_call->do_opcode != ZEND_CALLABLE_CONVERT
					&& call.user_call->do_opcode
						!= ZEND_CALLABLE_CONVERT_PARTIAL) {
				const uint32_t frame_operand = call.operand_count;
				const uint32_t context_operand = frame_operand + 2;
				ValuePart callee{tpde::x64::PlatformConfig::GP_BANK, 8};
				ValuePart entry{tpde::x64::PlatformConfig::GP_BANK, 8};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder enter_builder{*this, assigner};
					enter_builder.add_arg(
						CallArg{node.operands[frame_operand]});
					enter_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site->target_id, &assigner), tpde::CCAssignment{});
					enter_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					enter_builder.add_arg(CallArg{
						node.operands[context_operand]});
					enter_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_RESOLVE));
					enter_builder.add_ret(callee, tpde::CCAssignment{});
					enter_builder.add_ret(entry, tpde::CCAssignment{});
				}
				ValuePart entry_status_argument = call_resolved_entry(
					std::move(callee), std::move(entry),
					[&](CallBuilder &builder, tpde::x64::CCAssignerSysV &) {
						builder.add_arg(CallArg{
							node.operands[context_operand + 1]});
					});
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder leave_builder{*this, assigner};
				leave_builder.add_arg(
					CallArg{node.operands[frame_operand + 1]});
				leave_builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
					call.id, &assigner), tpde::CCAssignment{});
				leave_builder.add_arg(CallArg{
					node.operands[context_operand + 2]});
				leave_builder.add_arg(
					std::move(entry_status_argument), tpde::CCAssignment{});
				leave_builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_USER_CALL_RELEASE_RESOLUTION));
				ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
				leave_builder.add_ret(status, tpde::CCAssignment{});
				leave_builder.add_ret(payload, tpde::CCAssignment{});
				emit_status_tail(std::move(status),
					call.exception_block_id);
				if (node.has_result) {
					set_payload_result(node.result, std::move(payload));
				} else {
					payload.reset(this);
				}
				return true;
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(copy_fixed_argument(canonical_frame_register(), &assigner),
				tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site->target_id, &assigner), tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
				call.id, &assigner), tpde::CCAssignment{});
			for (IRValueRef operand : node.operands) {
				auto liveness = val_ref(operand);
				(void) liveness;
			}
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CALL_CONVERT_EXPLICIT));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			emit_status_tail(std::move(status),
				call.exception_block_id);
			if (node.has_result
					&& !load_scalar_call_result(
						node, call.call_site->result_operand)) {
				tpde::x64::CCAssignerSysV result_assigner{false};
				CallBuilder result_builder{*this, result_assigner};
				result_builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
					add_const_arg(result_builder,
						encode_source_operand(call.call_site->result_operand), 8);
				add_const_arg(result_builder,
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4);
				result_builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
				result_builder.add_ret(payload, tpde::CCAssignment{});
				set_payload_result(node.result, std::move(payload));
			}
			return true;
		}
		case ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL: {
			const zend_tpde_instruction &call =
				adaptor->mir_instruction(instruction);
			/*
			 * A positional by-value SEND of a CV, temporary or literal in its
			 * frame slot moves into ZEND_CALL_ARG(call, n) directly while the
			 * bound function takes that parameter purely by value. An
			 * undefined CV, a reference or indirect temporary and a value the
			 * VM duplicates keep the source setter, which the fast path joins.
			 */
			auto emit_internal_argument_transfer = [&](
					const zend_native_direct_internal_call_argument &send,
					uint32_t argument_index,
					IRValueRef frame_operand) -> bool {
				const zend_mir_source_operand_ref &source = send.source_operand;
				const uint32_t number = send.auxiliary_payload != 0
					? send.auxiliary_payload : send.ordinal + 1;
				const bool slot = source.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
					|| source.kind == ZEND_MIR_SOURCE_OPERAND_SSA;
				const bool from_cv = slot
					&& source.slot_kind == ZEND_MIR_SOURCE_SLOT_CV;
				const bool from_tmp = slot
					&& source.slot_kind == ZEND_MIR_SOURCE_SLOT_TMP;
				const bool from_literal =
					source.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL;
				const uint64_t source_offset = from_literal
					? static_cast<uint64_t>(source.index) * sizeof(zval)
					: (uint64_t{ZEND_CALL_FRAME_SLOT} + source.index
						+ (from_tmp ? uint64_t{
							adaptor->plan()->source_frame_variable_count} : 0))
						* sizeof(zval);
				const uint64_t target_offset =
					(uint64_t{ZEND_CALL_FRAME_SLOT} + number - 1) * sizeof(zval);
				if (send.mode != ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
						|| send.auxiliary_operand.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| (send.source_opcode != ZEND_SEND_VAL
							&& send.source_opcode != ZEND_SEND_VAL_EX
							&& send.source_opcode != ZEND_SEND_VAR
							&& send.source_opcode != ZEND_SEND_VAR_EX)
						|| !(from_cv || from_tmp || from_literal)
						|| number == 0 || number > 32
						|| (from_tmp && source.index
							>= adaptor->plan()->source_temporary_count)
						|| source_offset > INT32_MAX - sizeof(zval)
						|| target_offset > INT32_MAX - sizeof(zval)) {
					return false;
				}
				auto spilled = spill_target_branch_state();
				auto slow = text_writer.label_create();
				auto done = text_writer.label_create();
				{
					ScratchReg callee{this};
					ScratchReg source_address{this};
					ScratchReg payload{this};
					ScratchReg type{this};
					auto callee_reg = callee.alloc_gp();
					auto source_reg = source_address.alloc_gp();
					auto payload_reg = payload.alloc_gp();
					auto type_reg = type.alloc_gp();
					auto cell = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id);
					auto cell_scratch = std::move(cell).into_scratch(this);
					ASM(TEST32mi,
						FE_MEM(cell_scratch.cur_reg(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_internal_call_cell,
								by_value_arguments))),
						static_cast<int32_t>(UINT32_C(1) << (number - 1)));
					cell_scratch.reset();
					generate_raw_jump(Jump::je, slow);
					ASM(MOV64rm, callee_reg,
						FE_MEM(canonical_frame_register(), 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(CMP32mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_execute_data, This)
								+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(number));
					generate_raw_jump(Jump::jb, slow);
					if (from_literal) {
						ASM(MOV64rm, source_reg,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, func))));
						ASM(MOV64rm, source_reg,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_op_array, literals))));
						ASM(ADD64ri, source_reg,
							static_cast<int32_t>(source_offset));
					} else {
						ASM(LEA64rm, source_reg,
							FE_MEM(canonical_frame_register(), 0, FE_NOREG,
								static_cast<int32_t>(source_offset)));
					}
					ASM(MOV32rm, type_reg,
						FE_MEM(source_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zval, u1.type_info))));
					ASM(MOVZXr32r8, payload_reg, type_reg);
					if (from_cv) {
						auto plain = text_writer.label_create();
						ASM(CMP32ri, payload_reg, IS_UNDEF);
						generate_raw_jump(Jump::je, slow);
						ASM(CMP32ri, payload_reg, IS_REFERENCE);
						generate_raw_jump(Jump::jne, plain);
						ASM(MOV64rm, source_reg,
							FE_MEM(source_reg, 0, FE_NOREG, 0));
						ASM(ADD64ri, source_reg,
							static_cast<int32_t>(offsetof(zend_reference, val)));
						ASM(MOV32rm, type_reg,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zval, u1.type_info))));
						label_place(plain);
					} else if (from_tmp) {
						ASM(CMP32ri, payload_reg, IS_REFERENCE);
						generate_raw_jump(Jump::je, slow);
						ASM(CMP32ri, payload_reg, IS_INDIRECT);
						generate_raw_jump(Jump::je, slow);
					}
					ASM(MOV64rm, payload_reg, FE_MEM(source_reg, 0, FE_NOREG, 0));
					if (!from_tmp) {
						/* ZVAL_COPY_OR_DUP: a counted value gains a reference
						 * unless it is persistent, which the setter duplicates. */
						auto counted_done = text_writer.label_create();
						ASM(TEST32ri, type_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, counted_done);
						if (from_literal) {
							generate_raw_jump(Jump::jmp, slow);
						} else {
							ASM(TEST32mi,
								FE_MEM(payload_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, u.type_info))),
								GC_PERSISTENT);
							generate_raw_jump(Jump::jne, slow);
							ASM(ADD32mi,
								FE_MEM(payload_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
						}
						label_place(counted_done);
					}
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(target_offset)),
						payload_reg);
					ASM(MOV32mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(target_offset
								+ offsetof(zval, u1.type_info))),
						type_reg);
					if (from_tmp) {
						ASM(MOV32mi,
							FE_MEM(source_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zval, u1.type_info))),
							IS_UNDEF);
					}
					generate_raw_jump(Jump::jmp, done);
				}
				label_place(slow);
				{
					auto source_liveness =
						val_ref(IRValueRef{Adaptor::FRAME_VALUE});
					(void) source_liveness;
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(CallArg{frame_operand});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					add_const_arg(builder, argument_index, 4);
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
				}
				label_place(done);
				reconcile_target_branch_state(spilled);
				return true;
			};
			if (node.direct_internal_argument_transport) {
				const uint32_t argument_count =
					call.call_argument_count;
				const uint32_t frame_base = argument_count;
				/*
				 * A plain site, decided once here: a bound function without
				 * receiver or scope that is neither deprecated, nodiscard nor
				 * a trampoline, a source Do and an unused or temporary
				 * result. Its Init and Do skip the checks of the general
				 * helpers.
				 */
				const zend_native_direct_internal_call_descriptor
					*internal_descriptor = call.direct_internal_call;
				const zend_function *bound_function =
					call.internal_call_cell != nullptr
						? call.internal_call_cell->function : nullptr;
				const bool plain_internal = bound_function != nullptr
					&& call.internal_call_cell->receiver_kind
						== ZEND_NATIVE_INTERNAL_RECEIVER_NONE
					&& internal_descriptor->receiver_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED
					&& bound_function->type == ZEND_INTERNAL_FUNCTION
					&& bound_function->common.scope == nullptr
					&& (bound_function->common.fn_flags
						& (ZEND_ACC_DEPRECATED | ZEND_ACC_NODISCARD
							| ZEND_ACC_CALL_VIA_TRAMPOLINE)) == 0
					&& internal_descriptor->initial_argument_count
						<= internal_descriptor->argument_count
					&& internal_descriptor->do_opcode
						!= ZEND_CALLABLE_CONVERT
					&& internal_descriptor->do_opcode
						!= ZEND_CALLABLE_CONVERT_PARTIAL
					&& (internal_descriptor->result_operand.kind
							== ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| ((internal_descriptor->result_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_SLOT
								|| internal_descriptor->result_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_SSA)
							&& internal_descriptor->result_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_TMP));
				const uint64_t push_frame_size = plain_internal
					? (uint64_t{ZEND_CALL_FRAME_SLOT}
						+ internal_descriptor->initial_argument_count
						+ bound_function->common.T) * sizeof(zval)
					: 0;
				const uint64_t push_opline_offset = plain_internal
					? uint64_t{internal_descriptor->init_source_position}
						* sizeof(zend_op)
					: 0;
				/* The context, which the inline push reads, must be held
				 * in a register here. */
				const bool context_held = val_assignment(adaptor->val_local_idx(
						IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}))
					!= nullptr;
				if (plain_internal && context_held
						&& image_symbol_slot(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id).valid()
						&& push_frame_size <= INT32_MAX
						&& push_opline_offset <= INT32_MAX
						&& internal_descriptor->initial_argument_count <= 8) {
					/*
					 * ZEND_INIT_FCALL inline: push the frame of the bound
					 * function, as zend_native_internal_call_push() does. A
					 * full VM stack page takes that helper out of line, with
					 * every caller-saved register in use preserved.
					 */
					{
						auto frame_use = val_ref(node.operands[frame_base]);
						(void) frame_use;
					}
					auto cell_value = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id);
					auto cell_scratch = std::move(cell_value).into_scratch(this);
					const AsmReg cell_reg = cell_scratch.cur_reg();
					const tpde::SymRef descriptor_slot = image_symbol_slot(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id);
					ScratchReg top_pointer{this};
					ScratchReg top{this};
					ScratchReg work{this};
					const AsmReg top_pointer_reg = top_pointer.alloc_gp();
					const AsmReg top_reg = top.alloc_gp();
					const AsmReg work_reg = work.alloc_gp();
					const AsmReg frame_reg = canonical_frame_register();
					const AsmReg push_context_reg = canonical_value_register(
						IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
					auto overflow = text_writer.label_create();
					auto pushed = text_writer.label_create();
					const uint64_t live_registers = register_file.used;
					ASM(MOV64rm, top_pointer_reg,
						FE_MEM(push_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context, vm_stack_top))));
					ASM(MOV64rm, top_reg, FE_MEM(top_pointer_reg, 0, FE_NOREG, 0));
					ASM(MOV64rm, work_reg,
						FE_MEM(push_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context, vm_stack_end))));
					ASM(MOV64rm, work_reg, FE_MEM(work_reg, 0, FE_NOREG, 0));
					ASM(SUB64rr, work_reg, top_reg);
					ASM(CMP64ri, work_reg, static_cast<int32_t>(push_frame_size));
					generate_raw_jump(Jump::jb, overflow);
					ASM(LEA64rm, work_reg, FE_MEM(top_reg, 0, FE_NOREG,
						static_cast<int32_t>(push_frame_size)));
					ASM(MOV64mr, FE_MEM(top_pointer_reg, 0, FE_NOREG, 0), work_reg);
					ASM(MOV64rm, work_reg, FE_MEM(cell_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_internal_call_cell, function))));
					ASM(MOV64mr, FE_MEM(top_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, func))),
						work_reg);
					ASM(MOV64mi, FE_MEM(top_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This))),
						0);
					ASM(MOV32mi, FE_MEM(top_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))),
						ZEND_CALL_NESTED_FUNCTION);
					ASM(MOV32mi, FE_MEM(top_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This)
							+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(
							internal_descriptor->initial_argument_count));
					for (uint32_t index = 0;
							index < internal_descriptor->initial_argument_count;
							++index) {
						ASM(MOV32mi, FE_MEM(top_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval)
								+ offsetof(zval, u1.type_info))),
							IS_UNDEF);
					}
					ASM(MOV64rm, work_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, call))));
					ASM(MOV64mr, FE_MEM(top_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, prev_execute_data))),
						work_reg);
					ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, call))),
						top_reg);
					ASM(MOV64rm, work_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, func))));
					ASM(MOV64rm, work_reg, FE_MEM(work_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_op_array, opcodes))));
					if (push_opline_offset != 0) {
						ASM(ADD64ri, work_reg,
							static_cast<int32_t>(push_opline_offset));
					}
					ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, opline))),
						work_reg);
					label_place(pushed);
					cold_begin();
					label_place(overflow);
					emit_preserving_call(live_registers,
						ZEND_NATIVE_HELPER_INTERNAL_CALL_PUSH, [&] {
							/* The cell and the descriptor, loaded from its
							 * slot, go to the second and third arguments. */
							ASM(MOV64rr, FE_SI, cell_reg);
							emit_symbol_load(FE_DX, descriptor_slot);
							ASM(MOV64rr, FE_DI, frame_reg);
						}, [] {});
					generate_raw_jump(Jump::jmp, pushed);
					cold_end();
					work.reset();
					top.reset();
					top_pointer.reset();
					cell_scratch.reset();
				} else {
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(
						CallArg{node.operands[frame_base]});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id, &assigner), tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					builder.call(runtime_symbol(plain_internal
						? ZEND_NATIVE_HELPER_INTERNAL_CALL_PUSH
						: ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN));
				}

				for (uint32_t index = 0;
						index < argument_count; ++index) {
					const IRValueRef operand = node.operands[index];
					const zend_native_direct_internal_call_argument &argument =
						call.direct_internal_call->arguments[index];
					/* A register-held boxed SEND_VAL value goes straight to the
					 * argument helper instead of through its frame slot. */
					const bool direct_boxed_argument =
						operand != IRValueRef{Adaptor::FRAME_VALUE}
						&& adaptor->machine_kind(operand)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& adaptor->machine_value_is_register_authoritative(
							operand)
						&& (argument.source_opcode == ZEND_SEND_VAL
							|| argument.source_opcode == ZEND_SEND_VAL_EX);
					const IRValueRef frame_operand =
						node.operands[frame_base + 1 + index];
					if (operand == IRValueRef{Adaptor::FRAME_VALUE}
							&& emit_internal_argument_transfer(
								argument, index, frame_operand)) {
						continue;
					}
					/*
					 * A register-held by-value argument of the bound function
					 * is stored into ZEND_CALL_ARG(EX(call), n) directly, as
					 * SEND_VAL stores it: a temporary's bits, or an exact
					 * scalar with its type. By-reference, named and extra
					 * arguments keep the argument helpers.
					 */
					{
						const zend_function *internal_function =
							call.internal_call_cell != nullptr
								? call.internal_call_cell->function : nullptr;
						const uint32_t argument_number =
							argument.auxiliary_payload != 0
								? argument.auxiliary_payload
								: argument.ordinal + 1;
						const zend_mir_scalar_type_mask exact =
							operand != IRValueRef{Adaptor::FRAME_VALUE}
								? adaptor->exact_type(operand)
								: ZEND_MIR_SCALAR_TYPE_NONE;
						const bool scalar = operand
								!= IRValueRef{Adaptor::FRAME_VALUE}
							&& adaptor->machine_kind(operand)
								!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& (exact == ZEND_MIR_SCALAR_TYPE_I64
								|| exact == ZEND_MIR_SCALAR_TYPE_I1
								|| exact == ZEND_MIR_SCALAR_TYPE_NULL
								|| exact == ZEND_MIR_SCALAR_TYPE_F64);
						const uint64_t argument_offset =
							(uint64_t{ZEND_CALL_FRAME_SLOT} + argument_number - 1)
								* sizeof(zval);
						if (internal_function != nullptr
								&& (direct_boxed_argument || scalar)
								&& argument.mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
								&& argument.auxiliary_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_UNUSED
								&& argument_number != 0
								&& argument_number <= call.direct_internal_call
									->initial_argument_count
								&& !ARG_SHOULD_BE_SENT_BY_REF(
									internal_function, argument_number)
								&& argument_offset
									<= INT32_MAX - sizeof(zval)) {
							const int32_t payload_offset =
								static_cast<int32_t>(argument_offset);
							const int32_t type_offset = payload_offset
								+ static_cast<int32_t>(
									offsetof(zval, u1.type_info));
							auto [frame_ref, frame] =
								val_ref_single(frame_operand);
							auto frame_reg = frame.load_to_reg();
							ScratchReg callee{this};
							auto callee_reg = callee.alloc_gp();
							ASM(MOV64rm, callee_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_execute_data, call))));
							if (direct_boxed_argument) {
								auto boxed = val_ref(operand);
								/* The boxed transport's second frame use, as
								 * the helper form consumes it. */
								auto frame_materialization_liveness =
									val_ref(frame_operand);
								(void) frame_materialization_liveness;
								auto payload = boxed.part(0);
								auto type_info = boxed.part(1);
								ASM(MOV64mr, FE_MEM(callee_reg, 0, FE_NOREG,
									payload_offset), payload.load_to_reg());
								ASM(MOV32mr, FE_MEM(callee_reg, 0, FE_NOREG,
									type_offset), type_info.load_to_reg());
							} else if (exact == ZEND_MIR_SCALAR_TYPE_NULL) {
								auto [value_ref, value] =
									val_ref_single(operand);
								(void) value;
								ASM(MOV32mi, FE_MEM(callee_reg, 0, FE_NOREG,
									type_offset), IS_NULL);
							} else {
								auto [value_ref, value] =
									val_ref_single(operand);
								if (!store_exact_scalar(callee_reg,
										payload_offset, exact,
										value.load_to_reg())) {
									return false;
								}
							}
							continue;
						}
					}
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					if (operand != IRValueRef{Adaptor::FRAME_VALUE}
							&& adaptor->machine_kind(operand)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
							&& !direct_boxed_argument) {
						const zend_mir_source_operand_ref &source =
							call.direct_internal_call->arguments[index]
								.source_operand;
						if ((source.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
								&& source.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
								|| (source.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
									&& source.slot_kind
										!= ZEND_MIR_SOURCE_SLOT_VAR)
								|| source.index >= adaptor->plan()
									->source_temporary_count) {
							return false;
						}
						const uint64_t storage = static_cast<uint64_t>(
							adaptor->plan()->source_frame_variable_count)
							+ source.index;
						const uint64_t offset =
							(uint64_t{ZEND_CALL_FRAME_SLOT} + storage)
							* sizeof(zval);
						if (offset > static_cast<uint64_t>(INT32_MAX)
								- offsetof(zval, u1.type_info)) {
							return false;
						}
						auto boxed = val_ref(operand);
						auto payload = boxed.part(0);
						auto type_info = boxed.part(1);
						auto [frame_ref, frame] = val_ref_single(frame_operand);
						auto frame_reg = frame.load_to_reg();
						auto store_boxed_part = [&](ValuePartRef &part,
								int32_t part_offset, uint32_t size) {
							AsmReg reg;
							ScratchReg stack_reload{this};
							if (part.has_assignment()
									&& part.assignment().stack_valid()) {
								auto assignment = part.assignment();
								reg = stack_reload.alloc_gp();
								load_from_stack(reg, assignment.frame_off(),
									assignment.part_size());
							} else {
								reg = part.load_to_reg();
							}
							if (size == 8) {
								ASM(MOV64mr,
									FE_MEM(frame_reg, 0, FE_NOREG, part_offset),
									reg);
							} else {
								ASM(MOV32mr,
									FE_MEM(frame_reg, 0, FE_NOREG, part_offset),
									reg);
							}
						};
						store_boxed_part(payload,
							static_cast<int32_t>(offset), 8);
						store_boxed_part(type_info,
							static_cast<int32_t>(offset
								+ offsetof(zval, u1.type_info)), 4);
					}
					if (direct_boxed_argument) {
						auto boxed = val_ref(operand);
						{
							auto frame_materialization_liveness =
								val_ref(frame_operand);
							(void) frame_materialization_liveness;
						}
						builder.add_arg(CallArg{frame_operand});
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id, &assigner), tpde::CCAssignment{});
						add_const_arg(builder, index, 4);
						for (uint32_t part = 0; part < 2; ++part) {
							builder.add_arg(boxed.part(part),
								tpde::CCAssignment{});
						}
						boxed.reset();
						/* The type info is the fifth SysV argument. */
						ASM(OR32ri, FE_R8,
							ZEND_NATIVE_DIRECT_INTERNAL_ARGUMENT_BOXED_TYPE_INFO);
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT));
						continue;
					}
					builder.add_arg(CallArg{frame_operand});
					if (operand == IRValueRef{Adaptor::FRAME_VALUE}) {
						auto source_liveness = val_ref(operand);
						(void) source_liveness;
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id, &assigner), tpde::CCAssignment{});
						add_const_arg(builder, index, 4);
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
						continue;
					}
					if (adaptor->machine_kind(operand)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id, &assigner), tpde::CCAssignment{});
						add_const_arg(builder, index, 4);
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
						continue;
					}
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					add_const_arg(builder, index, 4);
					builder.add_arg(CallArg{operand});
					if (adaptor->exact_type(operand)
							== ZEND_MIR_SCALAR_TYPE_F64) {
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT));
					} else {
						add_const_arg(builder,
							static_cast<uint32_t>( adaptor->exact_type(operand)), 4);
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT));
					}
				}
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				const uint64_t do_opline_offset = plain_internal
					? uint64_t{internal_descriptor->do_source_position}
						* sizeof(zend_op)
					: 0;
				const bool unused_result =
					internal_descriptor->result_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED;
				const uint64_t do_result_offset =
					plain_internal && !unused_result
					? (uint64_t{ZEND_CALL_FRAME_SLOT}
						+ adaptor->plan()->source_frame_variable_count
						+ internal_descriptor->result_operand.index)
						* sizeof(zval)
					: 0;
				const tpde::SymRef do_descriptor_slot = image_symbol_slot(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					call.id);
				/*
				 * The Do of a plain site inline, as
				 * zend_native_internal_call_do_plain() does with no observer
				 * and no zend_execute_internal: the handler runs on the
				 * pushed frame; a quiet return with uncounted arguments
				 * pops it here, anything else finishes in
				 * zend_native_internal_call_do_plain_finish().
				 */
				const bool do_context_held = val_assignment(
						adaptor->val_local_idx(IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT})) != nullptr;
				/* Images outlive the process (the OPcache file cache): the
				 * code reaches globals and the handler through the context
				 * and the call cell only. Observers and zend_execute_internal
				 * are part of the system id. */
				const bool inline_do = plain_internal && do_context_held
					&& !ZEND_OBSERVER_ENABLED
					&& zend_execute_internal == nullptr
					&& do_descriptor_slot.valid()
					&& image_symbol_slot(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id).valid()
					&& do_opline_offset <= INT32_MAX
					&& do_result_offset <= INT32_MAX - sizeof(zval)
					&& internal_descriptor->argument_count <= 8
					&& runtime_symbol(
						ZEND_NATIVE_HELPER_INTERNAL_CALL_DO_PLAIN_FINISH)
						.valid();
				if (inline_do) {
					{
						auto frame_use = val_ref(
							node.operands[frame_base + 1 + argument_count]);
						(void) frame_use;
					}
					const AsmReg frame_reg = canonical_frame_register();
					const int32_t temporary_slot = unused_result
						? inst_stack_slot(sizeof(zval)) : 0;
					const tpde::SymRef do_cell_slot = image_symbol_slot(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id);
					ScratchReg status_scratch{this};
					ScratchReg callee{this};
					ScratchReg work{this};
					ScratchReg return_value{this};
					const AsmReg status_reg = status_scratch.alloc_gp();
					const AsmReg callee_reg = callee.alloc_gp();
					const AsmReg work_reg = work.alloc_gp();
					const AsmReg return_value_reg = return_value.alloc_gp();
					auto general = text_writer.label_create();
					auto finish = text_writer.label_create();
					auto finished = text_writer.label_create();
					const AsmReg context_reg = canonical_value_register(
						IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
					auto context_global = [&](AsmReg target, size_t field) {
						ASM(MOV64rm, target, FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(field)));
					};
					/* A pending exception (a send may leave one) takes the
					 * helper, which finishes the call as the VM does. */
					context_global(work_reg, offsetof(
						zend_native_execution_context, exception));
					ASM(CMP64mi, FE_MEM(work_reg, 0, FE_NOREG, 0), 0);
					generate_raw_jump(Jump::jne, general);
					ASM(MOV64rm, callee_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, call))));
					ASM(MOV64rm, work_reg, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, func))));
					ASM(MOV64rm, work_reg, FE_MEM(work_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_op_array, opcodes))));
					if (do_opline_offset != 0) {
						ASM(ADD64ri, work_reg,
							static_cast<int32_t>(do_opline_offset));
					}
					ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, opline))),
						work_reg);
					ASM(MOV64rm, work_reg, FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, prev_execute_data))));
					ASM(MOV64mr, FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, call))),
						work_reg);
					ASM(LEA64rm, work_reg, FE_MEM(frame_reg, 0, FE_NOREG, 0));
					ASM(MOV64mr, FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, prev_execute_data))),
						work_reg);
					if (unused_result) {
						ASM(LEA64rm, return_value_reg, FE_MEM(FE_BP, 0,
							FE_NOREG, temporary_slot));
					} else {
						ASM(LEA64rm, return_value_reg, FE_MEM(frame_reg, 0,
							FE_NOREG, static_cast<int32_t>(do_result_offset)));
					}
					ASM(MOV32mi, FE_MEM(return_value_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))), IS_NULL);
					context_global(work_reg, offsetof(
						zend_native_execution_context, current_execute_data));
					ASM(MOV64mr, FE_MEM(work_reg, 0, FE_NOREG, 0),
						callee_reg);
					/* The handler, with every live caller-saved register
					 * kept: the value state stays as it is. */
					emit_preserving_call(register_file.used,
						ZEND_NATIVE_HELPER_COUNT,
						[&] {
							/* Either source may sit in the other's
							 * argument register. */
							const AsmReg di{AsmReg::DI};
							const AsmReg si{AsmReg::SI};
							if (return_value_reg != di) {
								ASM(MOV64rr, FE_DI, callee_reg);
								ASM(MOV64rr, FE_SI, return_value_reg);
							} else if (callee_reg != si) {
								ASM(MOV64rr, FE_SI, return_value_reg);
								ASM(MOV64rr, FE_DI, callee_reg);
							} else {
								ASM(XCHG64rr, FE_DI, FE_SI);
							}
							/* The bound function's handler, through the
							 * cell; the sources are read already. */
							emit_symbol_load(FE_AX, do_cell_slot);
							ASM(MOV64rm, FE_AX, FE_MEM(FE_AX, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_internal_call_cell,
									function))));
							ASM(MOV64rm, FE_AX, FE_MEM(FE_AX, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_internal_function, handler))));
						},
						[] {}, true);
					context_global(work_reg, offsetof(
						zend_native_execution_context, vm_interrupt));
					ASM(CMP8mi, FE_MEM(work_reg, 0, FE_NOREG, 0), 0);
					generate_raw_jump(Jump::jne, finish);
					context_global(work_reg, offsetof(
						zend_native_execution_context, exception));
					ASM(CMP64mi, FE_MEM(work_reg, 0, FE_NOREG, 0), 0);
					generate_raw_jump(Jump::jne, finish);
					ASM(CMP32mi, FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This)
							+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(
							internal_descriptor->argument_count));
					generate_raw_jump(Jump::jne, finish);
					/* zend_vm_stack_free_args(): a counted argument this
					 * release would destroy takes the finish; the others
					 * only lose the frame's reference (no GC root check,
					 * as zval_ptr_dtor_nogc()). */
					auto argument_offset = [&](uint32_t index, size_t field) {
						return static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval)
							+ field);
					};
					for (uint32_t index = 0;
							index < internal_descriptor->argument_count;
							++index) {
						auto uncounted = text_writer.label_create();
						ASM(TEST8mi, FE_MEM(callee_reg, 0, FE_NOREG,
							argument_offset(index,
								offsetof(zval, u1.v.type_flags))),
							IS_TYPE_REFCOUNTED);
						generate_raw_jump(Jump::je, uncounted);
						ASM(MOV64rm, work_reg, FE_MEM(callee_reg, 0, FE_NOREG,
							argument_offset(index, 0)));
						ASM(CMP32mi, FE_MEM(work_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_refcounted_h, refcount))), 1);
						generate_raw_jump(Jump::je, finish);
						label_place(uncounted);
					}
					ASM(TEST32mi, FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))),
						ZEND_CALL_ALLOCATED);
					generate_raw_jump(Jump::jne, finish);
					if (unused_result) {
						ASM(TEST8mi, FE_MEM(FE_BP, 0, FE_NOREG,
							temporary_slot + static_cast<int32_t>(
								offsetof(zval, u1.v.type_flags))),
							IS_TYPE_REFCOUNTED);
						generate_raw_jump(Jump::jne, finish);
					}
					for (uint32_t index = 0;
							index < internal_descriptor->argument_count;
							++index) {
						auto uncounted = text_writer.label_create();
						ASM(TEST8mi, FE_MEM(callee_reg, 0, FE_NOREG,
							argument_offset(index,
								offsetof(zval, u1.v.type_flags))),
							IS_TYPE_REFCOUNTED);
						generate_raw_jump(Jump::je, uncounted);
						ASM(MOV64rm, work_reg, FE_MEM(callee_reg, 0, FE_NOREG,
							argument_offset(index, 0)));
						ASM(SUB32mi, FE_MEM(work_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_refcounted_h, refcount))), 1);
						label_place(uncounted);
					}
					/* EG(current_execute_data) and the VM stack top return
					 * to the caller. */
					context_global(work_reg, offsetof(
						zend_native_execution_context, current_execute_data));
					ASM(LEA64rm, status_reg,
						FE_MEM(frame_reg, 0, FE_NOREG, 0));
					ASM(MOV64mr, FE_MEM(work_reg, 0, FE_NOREG, 0),
						status_reg);
					context_global(work_reg, offsetof(
						zend_native_execution_context, vm_stack_top));
					ASM(MOV64mr, FE_MEM(work_reg, 0, FE_NOREG, 0),
						callee_reg);
					ASM(XOR32rr, status_reg, status_reg);
					cold_begin();
					const uint64_t live = register_file.used
						& ~(uint64_t{1} << status_reg.id());
					label_place(finish);
					emit_preserving_call(live,
						ZEND_NATIVE_HELPER_INTERNAL_CALL_DO_PLAIN_FINISH,
						[&] {
							ASM(PUSHr, callee_reg);
							ASM(PUSHr, return_value_reg);
							ASM(POPr, FE_DX);
							ASM(POPr, FE_SI);
							emit_symbol_load(FE_CX, do_descriptor_slot);
							ASM(LEA64rm, FE_DI,
								FE_MEM(frame_reg, 0, FE_NOREG, 0));
						},
						[&] { ASM(MOV32rr, status_reg, FE_AX); });
					generate_raw_jump(Jump::jmp, finished);
					label_place(general);
					emit_preserving_call(live,
						ZEND_NATIVE_HELPER_INTERNAL_CALL_DO_PLAIN,
						[&] {
							text_writer.ensure_space(32);
							ASM(MOV64rm, FE_SI,
								FE_MEM(FE_IP, 0, FE_NOREG, -1));
							reloc_text(do_cell_slot,
								tpde::elf::R_X86_64_PC32,
								text_writer.offset() - 4, -4);
							ASM(MOV64rm, FE_DX,
								FE_MEM(FE_IP, 0, FE_NOREG, -1));
							reloc_text(do_descriptor_slot,
								tpde::elf::R_X86_64_PC32,
								text_writer.offset() - 4, -4);
							ASM(LEA64rm, FE_DI,
								FE_MEM(frame_reg, 0, FE_NOREG, 0));
						},
						[&] { ASM(MOV32rr, status_reg, FE_AX); });
					generate_raw_jump(Jump::jmp, finished);
					cold_end();
					label_place(finished);
					callee.reset();
					work.reset();
					return_value.reset();
					/* Both paths kept every value in its register; leave
					 * the caller-saved ones free, as after a call, so that
					 * the status tail's return path spills nothing the
					 * continuation would then miss. */
					{
						const uint64_t callee_saved = cur_cc_assigner()
							->get_ccinfo().callee_saved_regs;
						const uint64_t evict = register_file.used
							& ~callee_saved;
						for (auto reg : tpde::util::BitSetIterator<>{evict}) {
							if (!register_file.is_fixed(AsmReg{reg})) {
								evict_reg(AsmReg{reg});
							}
						}
					}
					status.set_value(this, std::move(status_scratch));
				} else {
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(CallArg{
						node.operands[frame_base + 1 + argument_count]});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id, &assigner),
						tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id, &assigner), tpde::CCAssignment{});
					builder.call(runtime_symbol(plain_internal
						? ZEND_NATIVE_HELPER_INTERNAL_CALL_DO_PLAIN
						: ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE));
					builder.add_ret(status, tpde::CCAssignment{});
				}
				emit_status_tail(std::move(status),
					call.exception_block_id);
				if (node.has_result
						&& !(node.operands[frame_base + 2 + argument_count]
								== IRValueRef{Adaptor::FRAME_VALUE}
							&& load_scalar_call_result(
								node, call.call_site->result_operand))) {
					tpde::x64::CCAssignerSysV result_assigner{false};
					CallBuilder result_builder{*this, result_assigner};
					result_builder.add_arg(CallArg{
						node.operands[frame_base + 2 + argument_count]});
					add_const_arg(result_builder,
						encode_source_operand( call.call_site->result_operand), 8);
					add_const_arg(result_builder,
						static_cast<uint32_t>( adaptor->exact_type(node.result)), 4);
					result_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
					ValuePart payload{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					result_builder.add_ret(
						payload, tpde::CCAssignment{});
					set_payload_result(node.result, std::move(payload));
				}
				return true;
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site->target_id, &assigner), tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
				call.id, &assigner), tpde::CCAssignment{});
			builder.call(runtime_symbol(call.internal_call_cell != nullptr
					&& zend_native_internal_call_descriptor_plain(
						call.internal_call_cell, call.direct_internal_call)
				? ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_PLAIN
				: ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
			ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
			builder.add_ret(status, tpde::CCAssignment{});
			builder.add_ret(payload, tpde::CCAssignment{});
			emit_status_tail(std::move(status),
				call.exception_block_id);
			if (node.has_result) {
				set_payload_result(node.result, std::move(payload));
			} else {
				payload.reset(this);
			}
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_ENTER: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			add_const_arg(builder, record.source_position_id, 4);
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_ENTER));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			emit_status_tail(std::move(status),
				ZEND_MIR_ID_INVALID);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_CALL: {
			const zend_tpde_plan *plan = adaptor->plan();
			if (plan->source_opcodes == nullptr
					|| record.source_position_id
						>= plan->source_opcode_count) {
				return false;
			}
			const zend_tpde_source_opcode &opline =
				plan->source_opcodes[record.source_position_id];
			if (opline.opcode != ZEND_FAST_CALL
					|| opline.result_type != IS_TMP_VAR
					|| opline.result_var > INT32_MAX
					|| opline.result_var
						> INT32_MAX
							- static_cast<int32_t>(
								offsetof(zval, u2.opline_num))) {
				return false;
			}
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_scratch = frame_register(std::move(frame));
			ASM(MOV64mi,
				FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
					static_cast<int32_t>(opline.result_var)),
				0);
			ASM(MOV32mi,
				FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
					static_cast<int32_t>(opline.result_var)
						+ static_cast<int32_t>(
							offsetof(zval, u2.opline_num))),
				record.source_position_id);
			frame_scratch.reset();
			const auto &successors = adaptor->block_succs(
				IRBlockRef{node.control_block});
			if (successors.size() < 2) {
				return false;
			}
			generate_uncond_branch(successors[0]);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_RETURN: {
			const zend_tpde_plan *plan = adaptor->plan();
			if (plan->source_opcodes == nullptr
					|| record.source_position_id
						>= plan->source_opcode_count) {
				return false;
			}
			const zend_tpde_source_opcode &opline =
				plan->source_opcodes[record.source_position_id];
			if (opline.opcode != ZEND_FAST_RET
					|| opline.op1_type != IS_TMP_VAR
					|| opline.op1_var > INT32_MAX
					|| opline.op1_var
						> INT32_MAX
							- static_cast<int32_t>(
								offsetof(zval, u2.opline_num))) {
				return false;
			}
			auto slow_exception = text_writer.label_create();
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_scratch = frame_register(std::move(frame));
			ScratchReg direct_continuation{this};
			auto direct_continuation_reg =
				direct_continuation.alloc_gp();
			ASM(MOV32rm, direct_continuation_reg,
				FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
					static_cast<int32_t>(opline.op1_var)
						+ static_cast<int32_t>(
							offsetof(zval, u2.opline_num))));
			frame_scratch.reset();
			ASM(CMP32ri, direct_continuation_reg, UINT32_MAX);
			generate_raw_jump(Jump::je, slow_exception);
			if (plan->user_opcode_callbacks) {
				const auto &next_landings =
					adaptor->user_opcode_next_landings();
				for (uint32_t source = 0;
						source + 1 < next_landings.size(); ++source) {
					const uint32_t landing = next_landings[source + 1];
					if (landing == UINT32_MAX
							|| landing >= user_opcode_labels_.size()) {
						continue;
					}
					ASM(CMP32ri, direct_continuation_reg, source);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::jne, continued);
					generate_raw_jump(
						Jump::jmp, user_opcode_labels_[landing]);
					label_place(continued);
				}
			} else {
				emit_finally_return_dispatch(direct_continuation_reg);
			}
			direct_continuation.reset();
			emit_status_return(ZEND_NATIVE_EXCEPTION);
			label_place(slow_exception);
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[1]});
			add_const_arg(builder, record.source_position_id, 4);
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_RETURN));
			ValuePart continuation{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(continuation, tpde::CCAssignment{});
			auto continuation_reg = continuation.cur_reg_or_load(this);
			auto generator_returned = text_writer.label_create();
			ASM(CMP32ri, continuation_reg,
				ZEND_NATIVE_FINALLY_GENERATOR_RETURNED);
			generate_raw_jump(Jump::je, generator_returned);
			emit_finally_return_dispatch(continuation_reg);
			emit_handler_dispatch(continuation_reg);
			continuation.reset(this);
			emit_status_return(ZEND_NATIVE_EXCEPTION);
			label_place(generator_returned);
			RetBuilder generator_return_builder{
				*this, *cur_cc_assigner()};
			generator_return_builder.add(ValuePart{
				ZEND_NATIVE_GENERATOR_RETURNED, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			generator_return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_CATCH_ENTER: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			add_const_arg(builder, record.source_position_id, 4);
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CATCH_ENTER));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			if (zend_mir_id_is_valid(mir.exception_block_id)) {
				ASM(CMP32ri, status_reg, ZEND_NATIVE_CATCH_EXCEPTION);
				auto no_exception = text_writer.label_create();
				generate_raw_jump(Jump::jne, no_exception);
				generate_exception_branch(
					adaptor->block_ref(mir.exception_block_id));
				label_place(no_exception);
			}
			ASM(CMP32ri, status_reg, ZEND_NATIVE_CATCH_MATCHED);
			const auto &successors = adaptor->block_succs(
				IRBlockRef{node.control_block});
			uint32_t successor_count =
				static_cast<uint32_t>(successors.size());
			if (zend_mir_id_is_valid(mir.exception_block_id)
					&& successor_count != 0
					&& successors[successor_count - 1]
						== adaptor->block_ref(mir.exception_block_id)) {
				/* The frozen exceptional edge follows the source successors. */
				--successor_count;
			}
			if (successor_count == 2) {
				generate_cond_branch(Jump::je, successors[0], successors[1]);
				status.reset(this);
				return true;
			}
			if (successor_count != 1) {
				status.reset(this);
				return false;
			}
			auto propagate = text_writer.label_create();
			generate_raw_jump(Jump::jne, propagate);
			generate_exception_branch(successors[0]);
			label_place(propagate);
			status.reset(this);
			if (!catch_dispatch_label_.has_value()) {
				catch_dispatch_label_ = text_writer.label_create();
			}
			generate_raw_jump(Jump::jmp, *catch_dispatch_label_);
			return true;
		}
		case ZEND_MIR_OPCODE_RETURN: {
			if (adaptor->typed_body()) {
				if (node.operands.size() != 1) {
					return false;
				}
				ScratchReg status{this};
				reserve_typed_return_status(status);
				RetBuilder return_builder{
					*this, *cur_cc_assigner()};
				if (!add_boxed_scalar_return(
						return_builder, node.operands[0])) {
					return_builder.add(node.operands[0]);
				}
				add_typed_return_status(return_builder, std::move(status));
				return_builder.ret();
				return true;
			}
			{
			auto [value_ref, value] = val_ref_single(node.operands[0]);
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_reg = frame.load_to_reg();
			const uint64_t source_offset =
				uint64_t{record.source_position_id} * sizeof(zend_op);
			if (source_offset > INT32_MAX) {
				return false;
			}
			ScratchReg source_position{this};
			auto source_position_reg = source_position.alloc_gp();
			ASM(MOV64rm, source_position_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, func))));
			ASM(MOV64rm, source_position_reg,
				FE_MEM(source_position_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_function, op_array.opcodes))));
			if (source_offset != 0) {
				ASM(ADD64ri, source_position_reg,
					static_cast<int32_t>(source_offset));
			}
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, opline))),
				source_position_reg);
			ScratchReg pointer{this};
			auto pointer_reg = pointer.alloc_gp();
			ASM(MOV64rm, pointer_reg, FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, return_value))));
			auto no_result = text_writer.label_create();
			ASM(TEST64rr, pointer_reg, pointer_reg);
			generate_raw_jump(Jump::je, no_result);
			auto value_reg = value.load_to_reg();
			if (val_parts(node.operands[0]).bank == tpde::x64::PlatformConfig::FP_BANK) {
				ASM(SSE_MOVSDmr, FE_MEM(pointer_reg, 0, FE_NOREG, 0), value_reg);
			} else {
				ASM(MOV64mr, FE_MEM(pointer_reg, 0, FE_NOREG, 0), value_reg);
			}
			uint32_t type = zval_type(*adaptor, node.operands[0]);
			if (type == IS_FALSE) {
				ScratchReg kind{this};
				auto kind_reg = kind.alloc_gp();
				mov(kind_reg, value_reg, 8);
				ASM(ADD64ri, kind_reg, IS_FALSE);
				ASM(MOV32mr, FE_MEM(pointer_reg, 0, FE_NOREG, 8), kind_reg);
			} else {
				ASM(MOV32mi, FE_MEM(pointer_reg, 0, FE_NOREG, 8),
					static_cast<int32_t>(type));
			}
			label_place(no_result);
			}
			emit_status_return(ZEND_NATIVE_RETURNED);
			return true;
		}
		case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL: {
			if (adaptor->typed_body()) {
				if (node.operands.size() != 1) {
					return false;
				}
				const auto kind =
					adaptor->machine_kind(node.operands[0]);
				const zend_mir_ownership_state ownership =
					adaptor->ownership(node.operands[0]);
				const zend_mir_refcount_state refcount_state =
					adaptor->refcount_state(node.operands[0]);
				const bool return_addref =
					ownership == ZEND_MIR_OWNERSHIP_STATE_BORROWED
					&& refcount_state != ZEND_MIR_REFCOUNT_IMMORTAL;
				if (ownership != ZEND_MIR_OWNERSHIP_STATE_BORROWED
						&& ownership != ZEND_MIR_OWNERSHIP_STATE_OWNED
						&& ownership
							!= ZEND_MIR_OWNERSHIP_STATE_SHARED_OWNED) {
					return false;
				}
				/* Read a returned number before reserving the status
				 * register, so that no live part of it is evicted. */
				if (adaptor->machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& (adaptor->plan()->typed_body_return_abi.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_I64
							|| adaptor->plan()->typed_body_return_abi
									.machine_kind
								== ZEND_TPDE_MACHINE_VALUE_F64)) {
					return emit_number_return(node.operands[0]);
				}
				ScratchReg status{this};
				reserve_typed_return_status(status);
				RetBuilder return_builder{
					*this, *cur_cc_assigner()};
				if (return_addref
						&& kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto returned = val_ref(node.operands[0]);
					auto payload = returned.part(0);
					auto type_info = returned.part(1);
					auto payload_reg = payload.load_to_reg();
					auto type_info_reg = type_info.load_to_reg();
					auto copied = text_writer.label_create();
					ASM(TEST32ri, type_info_reg,
						IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
					generate_raw_jump(Jump::je, copied);
					ASM(ADD32mi,
						FE_MEM(payload_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, refcount))),
						1);
					label_place(copied);
					return_builder.add(
						std::move(payload), tpde::CCAssignment{});
					return_builder.add(
						std::move(type_info), tpde::CCAssignment{});
				} else if (return_addref
						&& (kind
								== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
							|| kind
								== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
							|| kind
								== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
							|| kind
								== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
							|| kind
								== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
					auto [returned_ref, returned] =
						val_ref_single(node.operands[0]);
					auto payload_reg = returned.load_to_reg();
					if (!emit_pointer_addref(kind, payload_reg)) {
						return false;
					}
					return_builder.add(
						std::move(returned), tpde::CCAssignment{});
				} else if (!add_boxed_scalar_return(
						return_builder, node.operands[0])) {
				return_builder.add(node.operands[0]);
				}
				add_typed_return_status(return_builder, std::move(status));
				return_builder.ret();
				return true;
			}
			if (mir.value_operation.source_opcode == ZEND_RETURN
					&& node.operands.size() >= 2
					&& node.operands[0]
						!= IRValueRef{Adaptor::FRAME_VALUE}) {
				const IRValueRef returned_ref = node.operands[0];
				const zend_tpde_machine_value_kind kind =
					adaptor->machine_kind(returned_ref);
				const zend_mir_ownership_state ownership =
					adaptor->ownership(returned_ref);
				const zend_mir_refcount_state refcount_state =
					adaptor->refcount_state(returned_ref);
				const bool copy_source =
					mir.value_operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| mir.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV;
				const bool return_addref =
					(ownership == ZEND_MIR_OWNERSHIP_STATE_BORROWED
						|| copy_source)
					&& refcount_state != ZEND_MIR_REFCOUNT_IMMORTAL;
				const uint64_t return_source_offset =
					(uint64_t{ZEND_CALL_FRAME_SLOT}
						+ mir.value_operation.op1_storage_id) * sizeof(zval);
				if (ownership != ZEND_MIR_OWNERSHIP_STATE_BORROWED
						&& ownership != ZEND_MIR_OWNERSHIP_STATE_OWNED
						&& ownership
							!= ZEND_MIR_OWNERSHIP_STATE_SHARED_OWNED) {
					return false;
				}
				if (!copy_source
						&& return_source_offset > INT32_MAX
							- static_cast<int32_t>(
								offsetof(zval, u1.type_info))) {
					return false;
				}
				{
				auto [frame_ref, frame] =
					val_ref_single(node.operands[1]);
				auto frame_reg = frame.load_to_reg();
				const uint64_t source_offset =
					uint64_t{record.source_position_id}
						* sizeof(zend_op);
				if (source_offset > INT32_MAX) {
					return false;
				}
				ScratchReg source_position{this};
				auto source_position_reg = source_position.alloc_gp();
				ASM(MOV64rm, source_position_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, func))));
				ASM(MOV64rm, source_position_reg,
					FE_MEM(source_position_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(
								zend_function,
								op_array.opcodes))));
				if (source_offset != 0) {
					ASM(ADD64ri, source_position_reg,
						static_cast<int32_t>(source_offset));
				}
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zend_execute_data, opline))),
					source_position_reg);
				ScratchReg pointer{this};
				auto pointer_reg = pointer.alloc_gp();
				ASM(MOV64rm, pointer_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(
								zend_execute_data,
								return_value))));
				auto no_result = text_writer.label_create();
				ASM(TEST64rr, pointer_reg, pointer_reg);
				generate_raw_jump(Jump::je, no_result);
				if (kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto returned = val_ref(returned_ref);
					const ValueParts parts = val_parts(returned_ref);
					std::vector<ValuePartRef> locked_parts;
					locked_parts.reserve(parts.count());
					tpde::x64::AsmReg payload_reg{};
					tpde::x64::AsmReg type_info_reg{};
					bool have_payload = false;
					bool have_type_info = false;
					for (uint32_t part = 0;
							part < parts.count(); ++part) {
						locked_parts.emplace_back(
							returned.part(part));
						auto &value = locked_parts.back();
						auto value_reg = value.load_to_reg();
						const zend_tpde_machine_part_role role =
							parts.representation.parts[part]
								.semantic_role;
						if (role
								== ZEND_TPDE_MACHINE_PART_PAYLOAD) {
							payload_reg = value_reg;
							have_payload = true;
						} else if (role
								== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
							type_info_reg = value_reg;
							have_type_info = true;
						} else {
							return false;
						}
					}
					if (!have_payload || !have_type_info) {
						return false;
					}
					if (return_addref) {
						auto copied = text_writer.label_create();
						ASM(TEST32ri, type_info_reg,
							IS_TYPE_REFCOUNTED
								<< Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, copied);
						ASM(ADD32mi,
							FE_MEM(payload_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h,
									refcount))),
							1);
						label_place(copied);
					}
					ASM(MOV64mr,
						FE_MEM(pointer_reg, 0, FE_NOREG, 0),
						payload_reg);
					ASM(MOV32mr,
						FE_MEM(pointer_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						type_info_reg);
				} else {
					uint64_t constant_bits = 0;
					ScratchReg constant_value{this};
					tpde::x64::AsmReg value_reg;
					if (adaptor->constant(
							returned_ref, &constant_bits)) {
						value_reg = constant_value.alloc(
							val_parts(returned_ref).bank);
						materialize_constant(&constant_bits,
							val_parts(returned_ref).bank, 8,
							value_reg);
					} else {
						auto [returned_value_ref, returned] =
							val_ref_single(returned_ref);
						value_reg = returned.load_to_reg();
					}
					if (return_addref
							&& (kind
									== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
								|| kind
									== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
								|| kind
									== ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR
								|| kind
									== ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR
								|| kind
									== ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR)) {
						if (!emit_pointer_addref(kind, value_reg)) {
							return false;
						}
					}
					if (val_parts(returned_ref).bank
							== tpde::x64::PlatformConfig::FP_BANK) {
						ASM(SSE_MOVSDmr,
							FE_MEM(pointer_reg, 0, FE_NOREG, 0),
							value_reg);
					} else {
						ASM(MOV64mr,
							FE_MEM(pointer_reg, 0, FE_NOREG, 0),
							value_reg);
					}
					ScratchReg type_info{this};
					auto type_info_reg = type_info.alloc_gp();
					if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
						mov(type_info_reg, value_reg, 8);
						ASM(ADD64ri, type_info_reg, IS_FALSE);
					} else if (zend_tpde_machine_value_zval_type(kind)
							!= IS_UNDEF) {
						if (!emit_machine_zval_type_info(
								kind, value_reg, type_info_reg)) {
							return false;
						}
					} else {
						const uint32_t type_info_value =
							zval_type(*adaptor, returned_ref);
						if (type_info_value == IS_UNDEF) {
							return false;
						}
						ASM(MOV32ri, type_info_reg,
							type_info_value);
					}
					ASM(MOV32mr,
						FE_MEM(pointer_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						type_info_reg);
					}
					if (!copy_source) {
						ASM(MOV32mi,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									return_source_offset
									+ offsetof(zval, u1.type_info))),
							IS_UNDEF);
					}
					label_place(no_result);
				}
					RetBuilder return_builder{
						*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_RETURNED, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
				return true;
			}
			if (mir.direct_scalar_return) {
				{
					auto [frame_ref, frame] =
						val_ref_single(node.operands[0]);
					auto frame_reg = frame.load_to_reg();
					const uint64_t source_offset =
						uint64_t{record.source_position_id}
							* sizeof(zend_op);
					if (source_offset > INT32_MAX) {
						return false;
					}
					ScratchReg source_position{this};
					auto source_position_reg =
						source_position.alloc_gp();
					ASM(MOV64rm, source_position_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, func))));
					ASM(MOV64rm, source_position_reg,
						FE_MEM(source_position_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_function,
									op_array.opcodes))));
					if (source_offset != 0) {
						ASM(ADD64ri, source_position_reg,
							static_cast<int32_t>(source_offset));
					}
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, opline))),
						source_position_reg);
					ScratchReg return_pointer{this};
					auto return_reg = return_pointer.alloc_gp();
					ASM(MOV64rm, return_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, return_value))));
					auto clear_source = text_writer.label_create();
					ASM(TEST64rr, return_reg, return_reg);
					generate_raw_jump(Jump::je, clear_source);
					ScratchReg payload{this};
					auto payload_reg = payload.alloc_gp();
					ScratchReg kind{this};
					auto kind_reg = kind.alloc_gp();
					ASM(MOV64rm, payload_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								mir.direct_scalar_return_offset)));
					ASM(MOV32rm, kind_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								mir.direct_scalar_return_offset
								+ offsetof(zval, u1.type_info))));
					ASM(MOV64mr,
						FE_MEM(return_reg, 0, FE_NOREG, 0), payload_reg);
					ASM(MOV32mr,
						FE_MEM(return_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						kind_reg);
					label_place(clear_source);
					if (mir.value_operation.op1.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_CV) {
						ASM(MOV32mi,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									mir.direct_scalar_return_offset
									+ offsetof(zval, u1.type_info))),
							IS_UNDEF);
					}
				}
				emit_status_return(ZEND_NATIVE_RETURNED);
				return true;
			}
			if (node.operands.empty()
					|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}) {
				return false;
			}
			/*
			 * Returning a temporary moves it into the caller's return zval;
			 * returning a CV copies it with a reference the frame's CV
			 * release balances. Do that inline; a reference, an undefined
			 * slot or a discarded result (no return zval) keeps the helper.
			 */
			const uint64_t temporary_return_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ mir.value_operation.op1_storage_id) * sizeof(zval);
			const bool cv_return = mir.value_operation.op1.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV;
			if (mir.value_operation.source_opcode == ZEND_RETURN
					&& (mir.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_TMP
						|| mir.value_operation.op1.slot_kind
							== ZEND_MIR_SOURCE_SLOT_VAR
						|| cv_return)
					&& zend_mir_id_is_valid(
						mir.value_operation.op1_storage_id)
					&& temporary_return_offset
						<= INT32_MAX - sizeof(zval)) {
				const int32_t payload_offset =
					static_cast<int32_t>(temporary_return_offset);
				const int32_t type_offset = payload_offset
					+ static_cast<int32_t>(offsetof(zval, u1.type_info));
				auto helper = text_writer.label_create();
				{
					const AsmReg frame_reg = canonical_frame_register();
					ScratchReg return_value{this};
					ScratchReg payload{this};
					ScratchReg type{this};
					auto return_value_reg = return_value.alloc_gp();
					auto payload_reg = payload.alloc_gp();
					auto type_reg = type.alloc_gp();
					ASM(MOV64rm, return_value_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_execute_data, return_value))));
					ASM(TEST64rr, return_value_reg, return_value_reg);
					generate_raw_jump(Jump::je, helper);
					ASM(MOV32rm, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG, type_offset));
					ASM(CMP8ri, type_reg, IS_REFERENCE);
					generate_raw_jump(Jump::je, helper);
					ASM(CMP8ri, type_reg, IS_UNDEF);
					generate_raw_jump(Jump::je, helper);
					ASM(MOV64rm, payload_reg,
						FE_MEM(frame_reg, 0, FE_NOREG, payload_offset));
					ASM(MOV64mr,
						FE_MEM(return_value_reg, 0, FE_NOREG, 0),
						payload_reg);
					ASM(MOV32mr,
						FE_MEM(return_value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						type_reg);
					if (cv_return) {
						auto uncounted = text_writer.label_create();
						ASM(TEST32ri, type_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::je, uncounted);
						ASM(ADD32mi,
							FE_MEM(payload_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_refcounted_h, refcount))), 1);
						label_place(uncounted);
					} else {
						ASM(MOV32mi,
							FE_MEM(frame_reg, 0, FE_NOREG, type_offset),
							IS_UNDEF);
					}
				}
				emit_status_return(ZEND_NATIVE_RETURNED);
				label_place(helper);
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(
				copy_fixed_argument(canonical_frame_register(), &assigner),
				tpde::CCAssignment{});
			{
				auto frame_liveness = val_ref(node.operands[0]);
				(void) frame_liveness;
			}
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				encode_source_operand(mir.value_operation.op1), 8,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{mir.value_operation.source_opcode, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{mir.value_operation.extended_value, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, ::tpde::CCAssignment{});
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		default:
			return false;
	}
}

bool ZendCompilerX64::compile_inst(
	IRInstRef instruction, InstRange remaining_instructions) {
	if (entry_variant_dispatch_pending_) {
		entry_variant_dispatch_pending_ = false;
		if (!emit_entry_variant_dispatch()) {
			return false;
		}
	}
	const Adaptor::InstNode &node = adaptor->node(instruction);
	current_continuation_block_ = node.continuation_block;
	continuation_edge_emitted_ = false;
	deopt_exit_fast_ = node.kind == Adaptor::InstKind::GuardedFast
		&& !adaptor->typed_body()
		&& adaptor->mir_instruction(instruction).deopt_exit_resume_plus_one
			!= 0;
	const bool compiled =
		compile_inst_impl(instruction, remaining_instructions);
	for (const auto &[slot, size] : inst_stack_slots_) {
		free_stack_slot(static_cast<uint32_t>(slot), size);
	}
	inst_stack_slots_.clear();
	if (!compiled || node.kind != Adaptor::InstKind::GuardedFast
			|| continuation_edge_emitted_) {
		return compiled;
	}
	if (node.continuation_block == UINT32_MAX) {
		return false;
	}
	generate_uncond_branch(IRBlockRef{node.continuation_block});
	return true;
}

struct X64ImageState {
	Adaptor adaptor;
	ZendCompilerX64 compiler;

	explicit X64ImageState(
		std::span<const zend_tpde_plan *const> plans,
		zend_native_image *image)
		: adaptor{plans}, compiler{&adaptor, image} {}
};

void destroy_x64_state(void *state) {
	delete static_cast<X64ImageState *>(state);
}

} // namespace

zend_result zend_tpde_emit_linux_x64(
	const zend_tpde_plan *const *plans,
	uint32_t plan_count,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	auto state = std::make_unique<X64ImageState>(
		std::span<const zend_tpde_plan *const>{plans, plan_count}, image);
	if (!state->adaptor.valid()) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"TPDE rejected the malformed ZNMIR x86-64 adaptor graph");
		return FAILURE;
	}
	if (!state->compiler.compile()) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"TPDE failed to compile the ZNMIR x86-64 adaptor graph");
		return FAILURE;
	}
	std::vector<tpde::u8> object =
		state->compiler.assembler.build_object_file();
	/* The publisher rejects writable executable sections. */
	if (object.empty()
			|| !zend_tpde_image_append(
				image, object.data(), object.size())) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to retain relocatable TPDE x86-64 image");
		return FAILURE;
	}
	image->metrics.direct_leaf_scalar_sites =
		state->adaptor.inlined_user_body_count();
	image->metrics.direct_typed_body_sites =
		state->adaptor.typed_body_call_site_count();
	image->metrics.direct_call_frame_bytes -= std::min(
		image->metrics.direct_call_frame_bytes,
		state->adaptor.typed_body_frame_bytes_elided());
	image->target_state = state.release();
	image->destroy_target_state = destroy_x64_state;
	return SUCCESS;
}
