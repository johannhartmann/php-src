// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"

#include <tpde/x64/CompilerX64.hpp>
#include <tpde/ElfMapper.hpp>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>

namespace {

using Adaptor = zend::native::tpde::ZendIRAdaptor;
using IRValueRef = zend::native::tpde::IRValueRef;
using IRInstRef = zend::native::tpde::IRInstRef;
using IRBlockRef = zend::native::tpde::IRBlockRef;
using IRFuncRef = zend::native::tpde::IRFuncRef;

class ZendCompilerX64 final
	: public tpde::x64::CompilerX64<Adaptor, ZendCompilerX64> {
	using Base = tpde::x64::CompilerX64<Adaptor, ZendCompilerX64>;

public:
	struct ValRefSpecial {
		uint8_t mode = 4;
		uint8_t bank = 0;
		uint8_t padding[6]{};
		uint64_t bits = 0;
	};

	struct ValueParts {
		tpde::RegBank bank;
		uint32_t count() const { return 1; }
		uint32_t size_bytes(uint32_t) const { return 8; }
		tpde::RegBank reg_bank(uint32_t) const { return bank; }
	};

	explicit ZendCompilerX64(Adaptor *adaptor) : Base{adaptor} {}

	void generate_exception_branch(IRBlockRef target) {
		auto index = static_cast<uint32_t>(this->analyzer.block_idx(target));
		generate_raw_jump(Jump::jmp, this->block_labels[index]);
	}

	bool cur_func_may_emit_calls() const { return adaptor->plan()->may_emit_calls; }
	tpde::SymRef cur_personality_func() const { return {}; }
	ValueParts val_parts(IRValueRef value) const {
		return {adaptor->representation(value) == ZEND_MIR_REPRESENTATION_DOUBLE
			? tpde::x64::PlatformConfig::FP_BANK
			: tpde::x64::PlatformConfig::GP_BANK};
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

	bool compile_inst(IRInstRef instruction, InstRange);
};

uint32_t zval_type(const Adaptor &adaptor, IRValueRef value) {
	switch (adaptor.exact_type(value)) {
		case ZEND_MIR_SCALAR_TYPE_NULL: return IS_NULL;
		case ZEND_MIR_SCALAR_TYPE_I1: return IS_FALSE;
		case ZEND_MIR_SCALAR_TYPE_I64: return IS_LONG;
		case ZEND_MIR_SCALAR_TYPE_F64: return IS_DOUBLE;
		default: return IS_UNDEF;
	}
}

bool ZendCompilerX64::compile_inst(IRInstRef instruction, InstRange) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	if (node.kind == Adaptor::InstKind::LoadFrame) {
		auto [source_ref, source] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto source_reg = source.load_to_reg();
		auto result_reg = result.alloc_reg();
		ASM(MOV64rr, result_reg, source_reg);
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::LoadArgument) {
		auto [base_ref, base] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto base_reg = base.load_to_reg();
		auto result_reg = result.alloc_reg();
		int32_t offset = static_cast<int32_t>(
			(ZEND_CALL_FRAME_SLOT + node.argument_index) * sizeof(zval));
		switch (adaptor->exact_type(node.result)) {
			case ZEND_MIR_SCALAR_TYPE_NULL:
				ASM(MOV32ri, result_reg, 0);
				break;
			case ZEND_MIR_SCALAR_TYPE_I1:
				ASM(MOV32rm, result_reg, FE_MEM(base_reg, 0, FE_NOREG,
					offset + static_cast<int32_t>(offsetof(zval, u1.type_info))));
				ASM(CMP32ri, result_reg, IS_TRUE);
				generate_raw_set(Jump::je, result_reg);
				break;
			case ZEND_MIR_SCALAR_TYPE_I64:
				ASM(MOV64rm, result_reg,
					FE_MEM(base_reg, 0, FE_NOREG, offset));
				break;
			case ZEND_MIR_SCALAR_TYPE_F64:
				ASM(SSE_MOVSDrm, result_reg,
					FE_MEM(base_reg, 0, FE_NOREG, offset));
				break;
			default:
				return false;
		}
		result.set_modified();
		return true;
	}
	const zend_tpde_instruction &mir = adaptor->mir_instruction(instruction);
	if (mir.source_effect == ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE) {
		if (mir.source_effect_exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| node.operands.empty()) {
			return false;
		}
		auto [frame_ref, frame] = val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		ScratchReg slot{this};
		auto slot_reg = slot.alloc_gp();
		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval)));
		ValuePart slot_pointer{tpde::x64::PlatformConfig::GP_BANK, 8};
		slot_pointer.set_value(this, std::move(slot));

		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		builder.add_arg(std::move(frame), tpde::CCAssignment{});
		builder.add_arg(std::move(slot_pointer), tpde::CCAssignment{});
		builder.add_arg(CallArg{node.operands[0]});
		auto add_extended = [&](uint64_t bits, uint32_t size, uint8_t extension) {
			tpde::CCAssignment assignment{};
			assignment.int_ext = extension;
			assignment.align = static_cast<uint8_t>(size);
			builder.add_arg(ValuePart{bits, size,
				tpde::x64::PlatformConfig::GP_BANK}, assignment);
		};
		add_extended(UINT64_C(0xfe), 1, 8);
		add_extended(UINT64_C(0x80), 1, UINT8_C(0x80) | 8);
		add_extended(UINT64_C(0xfedc), 2, 16);
		add_extended(UINT64_C(0x8001), 2, UINT8_C(0x80) | 16);
		add_extended(UINT64_C(0xfedcba98), 4, 32);
		add_extended(UINT64_C(0x89abcdef), 4, UINT8_C(0x80) | 32);
		tpde::CCAssignment wide_assignment{};
		wide_assignment.align = 8;
		builder.add_arg(ValuePart{UINT64_C(0xfedcba9876543210), 8,
			tpde::x64::PlatformConfig::GP_BANK}, wide_assignment);
		builder.add_arg(ValuePart{UINT64_C(0xfedcba9876543211), 8,
			tpde::x64::PlatformConfig::GP_BANK}, wide_assignment);
		builder.add_arg(ValuePart{UINT64_C(0x0123456789abcdef), 8,
			tpde::x64::PlatformConfig::GP_BANK}, wide_assignment);
		builder.add_arg(ValuePart{UINT64_C(0x8877665544332211), 8,
			tpde::x64::PlatformConfig::GP_BANK}, wide_assignment);
		for (uint64_t bits : {
				UINT64_C(0x3ff8000000000000), UINT64_C(0xc002000000000000),
				UINT64_C(0x4009000000000000), UINT64_C(0xc012000000000000),
				UINT64_C(0x4017000000000000), UINT64_C(0xc01b800000000000),
				UINT64_C(0x401c000000000000), UINT64_C(0xc020400000000000),
				UINT64_C(0x4022800000000000), UINT64_C(0xc025000000000000)}) {
			builder.add_arg(ValuePart{bits, 8,
				tpde::x64::PlatformConfig::FP_BANK}, wide_assignment);
		}
		builder.call(ValuePart{
			reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
				ZEND_NATIVE_HELPER_ABI_CONFORMANCE)), 8,
			tpde::x64::PlatformConfig::GP_BANK});
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
		builder.add_ret(status, tpde::CCAssignment{});
		auto status_reg = status.cur_reg_or_load(this);
		ASM(CMP64ri, status_reg, ZEND_NATIVE_ABI_CONFORMANCE_RESULT);
		auto matched = text_writer.label_create();
		generate_raw_jump(Jump::je, matched);
		status.reset(this);
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(ValuePart{ZEND_NATIVE_BAILOUT, 4,
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
		return_builder.ret();
		label_place(matched);
		return true;
	}
	if (mir.source_effect == ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR) {
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
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_ECHO_DOUBLE)), 8,
				tpde::x64::PlatformConfig::GP_BANK});
		} else {
			if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				builder.add_arg(ValuePart{uint64_t{0}, 8,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			} else {
				builder.add_arg(CallArg{node.operands[0]});
			}
			builder.add_arg(ValuePart{
				static_cast<uint32_t>(exact_type), 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_ECHO_INTEGER)), 8,
				tpde::x64::PlatformConfig::GP_BANK});
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
	auto integer_binary = [&](auto emit) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		auto [result_ref, result] = result_ref_single(node.result);
		auto left_reg = left.load_to_reg();
		auto right_reg = right.load_to_reg();
		auto result_reg = result.alloc_try_reuse(left);
		if (result_reg != left_reg) {
			mov(result_reg, left_reg, 8);
		}
		emit(result_reg, right_reg);
		result.set_modified();
		return true;
	};
	auto integer_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		auto [result_ref, result] = result_ref_single(node.result);
		ASM(CMP64rr, left.load_to_reg(), right.load_to_reg());
		auto result_reg = result.alloc_reg();
		generate_raw_set(condition, result_reg);
		result.set_modified();
		return true;
	};
	auto floating_binary = [&](auto emit) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		auto [result_ref, result] = result_ref_single(node.result);
		auto left_reg = left.load_to_reg();
		auto right_reg = right.load_to_reg();
		auto result_reg = result.alloc_try_reuse(left);
		if (result_reg != left_reg) {
			mov(result_reg, left_reg, 8);
		}
		emit(result_reg, right_reg);
		result.set_modified();
		return true;
	};
	auto floating_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		auto [result_ref, result] = result_ref_single(node.result);
		ASM(SSE_UCOMISDrr, left.load_to_reg(), right.load_to_reg());
		auto result_reg = result.alloc_reg();
		generate_raw_set(condition, result_reg);
		result.set_modified();
		return true;
	};
	auto execute_value_operation = [&](
			zend_native_runtime_helper_id helper,
			ValuePart *frame_argument = nullptr) {
		const bool explicit_operands =
			helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN
			|| helper == ZEND_NATIVE_HELPER_VALUE_QM_ASSIGN
			|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV
			|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP
			|| (helper >= ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R
				&& helper <= ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_UNSET);
		const bool explicit_auxiliary =
			helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP;
		if (node.operands.size() != 1
				|| (explicit_operands
					? !mir.has_value_operation
					: mir.source_opline_index == UINT32_MAX)) {
			return false;
		}
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		if (frame_argument != nullptr) {
			builder.add_arg(
				std::move(*frame_argument), tpde::CCAssignment{});
		} else {
			builder.add_arg(CallArg{node.operands[0]});
		}
		if (explicit_operands) {
			const zend_mir_executable_value_ref &operation =
				mir.value_operation;
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(operation.op1), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(operation.op2), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(operation.result), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			if (explicit_auxiliary) {
				builder.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.auxiliary), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
			}
			builder.add_arg(ValuePart{operation.extended_value, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_opcode, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
		} else {
			builder.add_arg(ValuePart{mir.source_opline_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
		}
		builder.call(ValuePart{
			reinterpret_cast<uintptr_t>(adaptor->runtime_helper(helper)), 8,
			tpde::x64::PlatformConfig::GP_BANK});
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
		builder.add_ret(status, tpde::CCAssignment{});
		auto status_reg = status.cur_reg_or_load(this);
		ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::je, continued);
		if (zend_mir_id_is_valid(mir.exception_block_id)) {
			auto propagate = text_writer.label_create();
			ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
			generate_raw_jump(Jump::jne, propagate);
			generate_exception_branch(
				adaptor->block_ref(mir.exception_block_id));
			label_place(propagate);
		}
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status), tpde::CCAssignment{});
		return_builder.ret();
		label_place(continued);
		return true;
	};
	auto copy_nonrefcounted_slot = [&](
			const zend_mir_source_operand_ref &source_operand,
			zend_mir_storage_id source_storage,
			zend_mir_storage_id target_storage,
			zend_mir_storage_id result_storage,
			bool move_source,
			zend_native_runtime_helper_id slow_helper) {
		if (source_storage == ZEND_MIR_ID_INVALID
				|| target_storage == ZEND_MIR_ID_INVALID
				|| source_storage == target_storage
				|| (result_storage != ZEND_MIR_ID_INVALID
					&& (result_storage == source_storage
						|| result_storage == target_storage))) {
			return execute_value_operation(slow_helper);
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		const uint64_t target_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + target_storage) * sizeof(zval);
		const uint64_t result_offset = result_storage == ZEND_MIR_ID_INVALID
			? 0 : (uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
		if (source_offset > INT32_MAX || target_offset > INT32_MAX
				|| result_offset > INT32_MAX) {
			return false;
		}

		/*
		 * The guarded helper call mutates TPDE's global register state even
		 * though it is emitted on only one local machine-code edge.  Spill all
		 * live assignments before introducing that edge, then keep the frame
		 * pointer in an untracked scratch register.  Both arms consequently
		 * reach the join with the same allocator state.
		 */
		for (auto reg_id : register_file.used_regs()) {
			tpde::Reg reg{reg_id};
			if (!register_file.is_fixed(reg)
					&& register_file.reg_local_idx(reg)
						!= INVALID_VAL_LOCAL_IDX) {
				evict_reg(reg);
			}
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg source_slot{this};
		ScratchReg target_slot{this};
		ScratchReg source_type{this};
		ScratchReg target_type{this};
		ScratchReg low_word{this};
		ScratchReg high_word{this};
		auto source_slot_reg = source_slot.alloc_gp();
		auto target_slot_reg = target_slot.alloc_gp();
		auto source_type_reg = source_type.alloc_gp();
		auto target_type_reg = target_type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();

		ASM(MOV64rr, source_slot_reg, frame_reg);
		ASM(ADD64ri, source_slot_reg, static_cast<int32_t>(source_offset));
		ASM(MOV64rr, target_slot_reg, frame_reg);
		ASM(ADD64ri, target_slot_reg, static_cast<int32_t>(target_offset));
		ASM(MOV32rm, source_type_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		if (source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(CMP32ri, source_type_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
		}
		ASM(CMP32ri, source_type_reg, IS_DOUBLE);
		generate_raw_jump(Jump::ja, slow);
		ASM(MOV32rm, target_type_reg,
			FE_MEM(target_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, target_type_reg, IS_DOUBLE);
		generate_raw_jump(Jump::ja, slow);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV64rr, target_type_reg, frame_reg);
			ASM(ADD64ri, target_type_reg, static_cast<int32_t>(result_offset));
			ASM(MOV32rm, target_type_reg,
				FE_MEM(target_type_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP32ri, target_type_reg, IS_DOUBLE);
			generate_raw_jump(Jump::ja, slow);
		}
		ASM(MOV64rm, low_word_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG, 0));
		ASM(MOV64rm, high_word_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG, 8));
		ASM(MOV64mr, FE_MEM(target_slot_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV64mr, FE_MEM(target_slot_reg, 0, FE_NOREG, 8), high_word_reg);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV64rr, target_slot_reg, frame_reg);
			ASM(ADD64ri, target_slot_reg, static_cast<int32_t>(result_offset));
			ASM(MOV64mr,
				FE_MEM(target_slot_reg, 0, FE_NOREG, 0), low_word_reg);
			ASM(MOV64mr,
				FE_MEM(target_slot_reg, 0, FE_NOREG, 8), high_word_reg);
		}
		if (move_source) {
			ASM(MOV32ri, source_type_reg, IS_UNDEF);
			ASM(MOV32mr,
				FE_MEM(source_slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
				source_type_reg);
		}
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		source_slot.reset();
		target_slot.reset();
		source_type.reset();
		target_type.reset();
		low_word.reset();
		high_word.reset();
		ValuePart frame_argument{tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(slow_helper, &frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};
	auto read_integer_array = [&]() {
		zend_tpde_integer_array_read layout;

		if (!zend_tpde_integer_array_read_at(mir, &layout)
				|| layout.container_offset > INT32_MAX
				|| layout.key_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R);
		}
		for (auto reg_id : register_file.used_regs()) {
			tpde::Reg reg{reg_id};
			if (!register_file.is_fixed(reg)
					&& register_file.reg_local_idx(reg)
						!= INVALID_VAL_LOCAL_IDX) {
				evict_reg(reg);
			}
		}
		auto slow = text_writer.label_create();
		auto packed = text_writer.label_create();
		auto mixed_loop = text_writer.label_create();
		auto mixed_next = text_writer.label_create();
		auto found = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg slot{this};
		ScratchReg type{this};
		ScratchReg array{this};
		ScratchReg key{this};
		ScratchReg limit{this};
		ScratchReg element{this};
		ScratchReg low_word{this};
		ScratchReg high_word{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto key_reg = key.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.container_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg, static_cast<int32_t>(layout.key_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_LONG);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, key_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.result_offset));
		ASM(MOV32rm, limit_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, limit_reg, IS_UNDEF);
		generate_raw_jump(Jump::jne, slow);

		ASM(MOV32rm, type_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, u))));
		ASM(AND32ri, type_reg, HASH_FLAG_PACKED);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, packed);

		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arData))));
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nTableMask))));
		ASM(MOV32rr, type_reg, key_reg);
		ASM(OR32rr, type_reg, limit_reg);
		ASM(MOVSXr64r32, type_reg, type_reg);
		ASM(MOV32rm, limit_reg,
			FE_MEM(element_reg, 4, type_reg, 0));
		label_place(mixed_loop);
		ASM(CMP32ri, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rr, slot_reg, limit_reg);
		ASM(SHL64ri, slot_reg, 5);
		ASM(ADD64rr, slot_reg, element_reg);
		ASM(MOV64rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, h))));
		ASM(CMP64rr, type_reg, key_reg);
		generate_raw_jump(Jump::jne, mixed_next);
		ASM(MOV64rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, key))));
		ASM(TEST64rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, found);
		label_place(mixed_next);
		ASM(MOV32rm, limit_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(Bucket, val) + offsetof(zval, u2.next))));
		generate_raw_jump(Jump::jmp, mixed_loop);

		label_place(packed);
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumUsed))));
		ASM(CMP64rr, key_reg, limit_reg);
		generate_raw_jump(Jump::jae, slow);

		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arPacked))));
		ASM(SHL64ri, key_reg, 4);
		ASM(ADD64rr, element_reg, key_reg);
		ASM(MOV64rr, slot_reg, element_reg);
		label_place(found);
		ASM(MOV64rr, element_reg, slot_reg);
		ASM(MOV32rm, type_reg,
			FE_MEM(element_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.result_offset));
		ASM(MOV64rm, low_word_reg,
			FE_MEM(element_reg, 0, FE_NOREG, 0));
		ASM(MOV64rm, high_word_reg,
			FE_MEM(element_reg, 0, FE_NOREG, 8));
		ASM(MOV64mr, FE_MEM(slot_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV64mr, FE_MEM(slot_reg, 0, FE_NOREG, 8), high_word_reg);
		ASM(AND32ri, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, done);
		ASM(MOV32rm, limit_reg,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))));
		ASM(ADD32ri, limit_reg, 1);
		ASM(MOV32mr,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			limit_reg);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		array.reset();
		key.reset();
		limit.reset();
		element.reset();
		low_word.reset();
		high_word.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R, &frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};
	auto isset_integer_array = [&]() {
		zend_tpde_integer_array_isset layout;

		if (!zend_tpde_integer_array_isset_at(mir, &layout)
				|| layout.container_offset > INT32_MAX
				|| layout.key_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM);
		}
		for (auto reg_id : register_file.used_regs()) {
			tpde::Reg reg{reg_id};
			if (!register_file.is_fixed(reg)
					&& register_file.reg_local_idx(reg)
						!= INVALID_VAL_LOCAL_IDX) {
				evict_reg(reg);
			}
		}
		auto slow = text_writer.label_create();
		auto packed = text_writer.label_create();
		auto mixed_loop = text_writer.label_create();
		auto mixed_next = text_writer.label_create();
		auto found = text_writer.label_create();
		auto inspect_element = text_writer.label_create();
		auto answer_false = text_writer.label_create();
		auto answer_true = text_writer.label_create();
		auto store_answer = text_writer.label_create();
		auto not_reference = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg slot{this};
		ScratchReg type{this};
		ScratchReg array{this};
		ScratchReg key{this};
		ScratchReg limit{this};
		ScratchReg element{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto key_reg = key.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.container_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg, static_cast<int32_t>(layout.key_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_LONG);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, key_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg, static_cast<int32_t>(layout.result_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_UNDEF);
		generate_raw_jump(Jump::jne, slow);

		ASM(MOV32rm, type_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, u))));
		ASM(AND32ri, type_reg, HASH_FLAG_PACKED);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, packed);

		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arData))));
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nTableMask))));
		ASM(MOV32rr, type_reg, key_reg);
		ASM(OR32rr, type_reg, limit_reg);
		ASM(MOVSXr64r32, type_reg, type_reg);
		ASM(MOV32rm, limit_reg,
			FE_MEM(element_reg, 4, type_reg, 0));
		label_place(mixed_loop);
		ASM(CMP32ri, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::je, answer_false);
		ASM(MOV64rr, slot_reg, limit_reg);
		ASM(SHL64ri, slot_reg, 5);
		ASM(ADD64rr, slot_reg, element_reg);
		ASM(MOV64rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, h))));
		ASM(CMP64rr, type_reg, key_reg);
		generate_raw_jump(Jump::jne, mixed_next);
		ASM(MOV64rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, key))));
		ASM(TEST64rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, found);
		label_place(mixed_next);
		ASM(MOV32rm, limit_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(Bucket, val) + offsetof(zval, u2.next))));
		generate_raw_jump(Jump::jmp, mixed_loop);

		label_place(packed);
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumUsed))));
		ASM(CMP64rr, key_reg, limit_reg);
		generate_raw_jump(Jump::jae, answer_false);
		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arPacked))));
		ASM(SHL64ri, key_reg, 4);
		ASM(ADD64rr, element_reg, key_reg);
		generate_raw_jump(Jump::jmp, inspect_element);
		label_place(found);
		ASM(MOV64rr, element_reg, slot_reg);
		label_place(inspect_element);
		ASM(MOV32rm, type_reg,
			FE_MEM(element_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_REFERENCE);
		generate_raw_jump(Jump::jne, not_reference);
		ASM(MOV64rm, element_reg,
			FE_MEM(element_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, type_reg,
			FE_MEM(element_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_reference, val)
						+ offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		label_place(not_reference);
		ASM(CMP32ri, type_reg, IS_NULL);
		generate_raw_jump(Jump::ja, answer_true);

		label_place(answer_false);
		ASM(MOV64ri, element_reg, 0);
		ASM(MOV32ri, type_reg, IS_FALSE);
		generate_raw_jump(Jump::jmp, store_answer);
		label_place(answer_true);
		ASM(MOV64ri, element_reg, 1);
		ASM(MOV32ri, type_reg, IS_TRUE);
		label_place(store_answer);
		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.result_offset));
		ASM(MOV64mr,
			FE_MEM(slot_reg, 0, FE_NOREG, 0), element_reg);
		ASM(MOV32mr,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		array.reset();
		key.reset();
		limit.reset();
		element.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM,
				&frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};
	auto append_packed_array = [&]() {
		zend_tpde_packed_array_append layout;

		if (!zend_tpde_packed_array_append_at(mir, &layout)
				|| layout.container_offset > INT32_MAX
				|| layout.value_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM);
		}
		for (auto reg_id : register_file.used_regs()) {
			tpde::Reg reg{reg_id};
			if (!register_file.is_fixed(reg)
					&& register_file.reg_local_idx(reg)
						!= INVALID_VAL_LOCAL_IDX) {
				evict_reg(reg);
			}
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg slot{this};
		ScratchReg type{this};
		ScratchReg array{this};
		ScratchReg count{this};
		ScratchReg limit{this};
		ScratchReg element{this};
		ScratchReg low_word{this};
		ScratchReg high_word{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto count_reg = count.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.container_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, count_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))));
		ASM(CMP32ri, count_reg, 1);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV32rm, type_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_refcounted_h, u))));
		ASM(AND32ri, type_reg, IS_ARRAY_IMMUTABLE);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV32rm, type_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, u))));
		ASM(AND32ri, type_reg, HASH_FLAG_PACKED);
		ASM(TEST32rr, type_reg, type_reg);
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

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg, static_cast<int32_t>(layout.value_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, limit_reg, type_reg);
		ASM(AND32ri, limit_reg, Z_TYPE_MASK);
		ASM(CMP32ri, limit_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);
		ASM(CMP32ri, limit_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);
		ASM(CMP32ri, limit_reg, IS_INDIRECT);
		generate_raw_jump(Jump::je, slow);
		if (layout.has_result) {
			ASM(MOV64rr, element_reg, frame_reg);
			ASM(ADD64ri, element_reg,
				static_cast<int32_t>(layout.result_offset));
			ASM(MOV32rm, limit_reg,
				FE_MEM(element_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP32ri, limit_reg, IS_UNDEF);
			generate_raw_jump(Jump::jne, slow);
		}

		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arPacked))));
		ASM(SHL64ri, count_reg, 4);
		ASM(ADD64rr, element_reg, count_reg);
		ASM(SHR64ri, count_reg, 4);
		ASM(MOV64rm, low_word_reg,
			FE_MEM(slot_reg, 0, FE_NOREG, 0));
		ASM(MOV64rm, high_word_reg,
			FE_MEM(slot_reg, 0, FE_NOREG, 8));
		ASM(MOV64mr,
			FE_MEM(element_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV64mr,
			FE_MEM(element_reg, 0, FE_NOREG, 8), high_word_reg);
		if (layout.move_value) {
			ASM(MOV32mi,
				FE_MEM(slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
				IS_UNDEF);
		} else {
			auto copied = text_writer.label_create();
			ASM(MOV32rr, limit_reg, type_reg);
			ASM(AND32ri, limit_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			ASM(TEST32rr, limit_reg, limit_reg);
			generate_raw_jump(Jump::je, copied);
			ASM(MOV32rm, limit_reg,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))));
			ASM(ADD32ri, limit_reg, 1);
			ASM(MOV32mr,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				limit_reg);
			label_place(copied);
		}
		ASM(ADD32ri, count_reg, 1);
		ASM(MOV32mr,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumUsed))),
			count_reg);
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumOfElements))));
		ASM(ADD32ri, limit_reg, 1);
		ASM(MOV32mr,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNumOfElements))),
			limit_reg);
		ASM(MOV64mr,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nNextFreeElement))),
			count_reg);
		if (layout.has_result) {
			ASM(MOV64rr, slot_reg, frame_reg);
			ASM(ADD64ri, slot_reg,
				static_cast<int32_t>(layout.result_offset));
			ASM(MOV64mr,
				FE_MEM(slot_reg, 0, FE_NOREG, 0), low_word_reg);
			ASM(MOV64mr,
				FE_MEM(slot_reg, 0, FE_NOREG, 8), high_word_reg);
			auto result_copied = text_writer.label_create();
			ASM(MOV32rr, limit_reg, type_reg);
			ASM(AND32ri, limit_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			ASM(TEST32rr, limit_reg, limit_reg);
			generate_raw_jump(Jump::je, result_copied);
			ASM(MOV32rm, limit_reg,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))));
			ASM(ADD32ri, limit_reg, 1);
			ASM(MOV32mr,
				FE_MEM(low_word_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				limit_reg);
			label_place(result_copied);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		array.reset();
		count.reset();
		limit.reset();
		element.reset();
		low_word.reset();
		high_word.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM, &frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};

	if ((mir.record.opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
				&& mir.record.opcode
					<= ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED)
			|| (mir.record.opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				&& mir.record.opcode
					<= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL)
			|| mir.record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			|| mir.record.opcode == ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
			|| mir.record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME) {
		zend_native_runtime_helper_id helper;
		if (mir.record.opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R) {
			helper = static_cast<zend_native_runtime_helper_id>(
				static_cast<uint32_t>(mir.record.opcode)
					- static_cast<uint32_t>(ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
					+ static_cast<uint32_t>(ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R));
		} else {
			helper = static_cast<zend_native_runtime_helper_id>(
				static_cast<uint32_t>(mir.record.opcode)
					- static_cast<uint32_t>(
						ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS)
					+ static_cast<uint32_t>(
						ZEND_NATIVE_HELPER_OBJECT_DECLARE_ANON_CLASS));
		}
		return execute_value_operation(helper);
	}
	switch (mir.record.opcode) {
		case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_MAKE_REF);
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN_REF);
		case ZEND_MIR_OPCODE_VALUE_SEPARATE:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_SEPARATE);
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_COPY_TMP);
		case ZEND_MIR_OPCODE_VALUE_FREE:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FREE);
		case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_UNSET_CV);
		case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_CHECK_VAR);
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
			if (mir.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
				return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN);
			}
			return copy_nonrefcounted_slot(
				mir.value_operation.op2,
				mir.value_operation.op2_storage_id,
				mir.value_operation.op1_storage_id,
				mir.value_operation.result_storage_id,
				mir.value_operation.op2.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP,
				ZEND_NATIVE_HELPER_VALUE_ASSIGN);
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			return copy_nonrefcounted_slot(
				mir.value_operation.op1,
				mir.value_operation.op1_storage_id,
				mir.value_operation.result_storage_id,
				ZEND_MIR_ID_INVALID,
				mir.value_operation.op1.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
					|| mir.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_VAR,
				ZEND_NATIVE_HELPER_VALUE_QM_ASSIGN);
		case ZEND_MIR_OPCODE_VALUE_CONCAT:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_CONCAT);
		case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FAST_CONCAT);
		case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ROPE_INIT);
		case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ROPE_ADD);
		case ZEND_MIR_OPCODE_VALUE_ROPE_END:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ROPE_END);
		case ZEND_MIR_OPCODE_VALUE_INIT_ARRAY:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY);
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_ELEMENT:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT);
		case ZEND_MIR_OPCODE_VALUE_ADD_ARRAY_UNPACK:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_UNPACK);
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_R:
			return read_integer_array();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_W:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_W);
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_RW);
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_IS);
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_FUNC_ARG);
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_UNSET);
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM:
			return append_packed_array();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_DIM_OP:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP);
		case ZEND_MIR_OPCODE_VALUE_UNSET_DIM:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_UNSET_DIM);
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_DIM:
			return isset_integer_array();
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_OP:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN_OP);
		case ZEND_MIR_OPCODE_VALUE_FE_FREE:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FE_FREE);
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_BINARY_OP);
		case ZEND_MIR_OPCODE_VALUE_UNARY_OP:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_UNARY_OP);
		case ZEND_MIR_OPCODE_VALUE_CAST:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_CAST);
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV);
		case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_LIST);
		case ZEND_MIR_OPCODE_VALUE_INCDEC:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_INCDEC);
		case ZEND_MIR_OPCODE_COPY:
		case ZEND_MIR_OPCODE_CANONICALIZE:
		case ZEND_MIR_OPCODE_I1_TO_I64:
			return copy_result();
		case ZEND_MIR_OPCODE_STATEPOINT:
			if ((mir.record.effects & ZEND_MIR_EFFECT_MASK(
					ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) != 0) {
				if (node.operands.size() != 1
						|| mir.source_opline_index == UINT32_MAX) {
					return false;
				}
				tpde::x64::CCAssignerSysV assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[0]});
				builder.add_arg(ValuePart{mir.source_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_INTERRUPT_POLL)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
				return true;
			}
			[[fallthrough]];
		case ZEND_MIR_OPCODE_SCALAR_DROP:
			for (IRValueRef operand : node.operands) {
				auto consumed = val_ref(operand);
				(void) consumed;
			}
			return true;
		case ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW:
			return integer_binary([&](auto dst, auto src) { ASM(ADD64rr, dst, src); });
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
			return integer_binary([&](auto dst, auto src) { ASM(SUB64rr, dst, src); });
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			return integer_binary([&](auto dst, auto src) { ASM(IMUL64rr, dst, src); });
		case ZEND_MIR_OPCODE_I64_BIT_OR:
			return integer_binary([&](auto dst, auto src) { ASM(OR64rr, dst, src); });
		case ZEND_MIR_OPCODE_I64_BIT_AND:
			return integer_binary([&](auto dst, auto src) { ASM(AND64rr, dst, src); });
		case ZEND_MIR_OPCODE_I64_BIT_XOR:
		case ZEND_MIR_OPCODE_I1_XOR:
			return integer_binary([&](auto dst, auto src) { ASM(XOR64rr, dst, src); });
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
			generate_raw_set(mir.record.opcode == ZEND_MIR_OPCODE_I1_NOT
				? Jump::je : Jump::jne, result_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_I64_EQ:
		case ZEND_MIR_OPCODE_I1_EQ:
			return integer_compare(Jump::je);
		case ZEND_MIR_OPCODE_I64_LT:
			return integer_compare(Jump::jl);
		case ZEND_MIR_OPCODE_I64_LE:
			return integer_compare(Jump::jle);
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
			if (mir.record.opcode == ZEND_MIR_OPCODE_I64_SHL_CHECKED) {
				ASM(SHL64rr, result_reg, cx);
			} else {
				ASM(SAR64rr, result_reg, cx);
			}
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_F64_ADD:
			return floating_binary([&](auto dst, auto src) { ASM(SSE_ADDSDrr, dst, src); });
		case ZEND_MIR_OPCODE_F64_SUB:
			return floating_binary([&](auto dst, auto src) { ASM(SSE_SUBSDrr, dst, src); });
		case ZEND_MIR_OPCODE_F64_MUL:
			return floating_binary([&](auto dst, auto src) { ASM(SSE_MULSDrr, dst, src); });
		case ZEND_MIR_OPCODE_F64_EQ:
			return floating_compare(Jump::je);
		case ZEND_MIR_OPCODE_F64_LT:
			return floating_compare(Jump::jb);
		case ZEND_MIR_OPCODE_F64_LE:
			return floating_compare(Jump::jbe);
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
				adaptor->block_ref(mir.record.block_id))[0]);
			return true;
		case ZEND_MIR_OPCODE_COND_BRANCH: {
			auto [condition_ref, condition] = unary();
			auto condition_reg = condition.load_to_reg();
			ASM(TEST64rr, condition_reg, condition_reg);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			generate_cond_branch(Jump::jne, successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
		case ZEND_MIR_OPCODE_ITERATOR_BRANCH: {
			if (node.operands.size() != 1
					|| (mir.record.opcode
							== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
						? !mir.has_value_operation
						: mir.source_opline_index == UINT32_MAX)) {
				return false;
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			if (mir.record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
				const zend_mir_executable_value_ref &operation =
					mir.value_operation;
				builder.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op2), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{operation.extended_value, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{operation.source_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{operation.source_position_id, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
			} else {
				builder.add_arg(ValuePart{mir.source_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
			}
			const auto helper = mir.record.opcode
				== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				? ZEND_NATIVE_HELPER_VALUE_COND_BRANCH
				: ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH;
			builder.call(ValuePart{reinterpret_cast<uintptr_t>(
				adaptor->runtime_helper(helper)), 8,
				tpde::x64::PlatformConfig::GP_BANK});
			ValuePart decision{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(decision, tpde::CCAssignment{});
			auto decision_reg = decision.cur_reg_or_load(this);
			ASM(CMP32ri, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
			auto valid = text_writer.label_create();
			generate_raw_jump(Jump::jl, valid);
			/* Release the helper return register before constructing an early
			 * native return.  On the valid edge the generated return sequence is
			 * skipped, so the physical decision register still carries 0 or 1. */
			decision.reset(this);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			return_builder.ret();
			label_place(valid);
			ASM(TEST32rr, decision_reg, decision_reg);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			generate_cond_branch(Jump::jne, successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_CALL_DIRECT_USER: {
			const zend_tpde_instruction &call =
				adaptor->mir_instruction(instruction);
			if (call.direct_call != nullptr) {
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(ValuePart{
					reinterpret_cast<uintptr_t>(call.entry_cell), 8,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					reinterpret_cast<uintptr_t>(call.direct_call), 8,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_DIRECT_USER_CALL)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
				ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK};
				builder.add_ret(status, tpde::CCAssignment{});
				builder.add_ret(payload, tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::je, continued);
				if (zend_mir_id_is_valid(call.exception_block_id)) {
					auto propagate = text_writer.label_create();
					ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
					generate_raw_jump(Jump::jne, propagate);
					generate_exception_branch(
						adaptor->block_ref(call.exception_block_id));
					label_place(propagate);
				}
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(std::move(status), tpde::CCAssignment{});
				return_builder.ret();
				label_place(continued);
				if (node.has_result) {
					auto [result_ref, result] = result_ref_single(node.result);
					if (val_parts(node.result).bank
							== tpde::x64::PlatformConfig::FP_BANK) {
						auto payload_reg = payload.cur_reg_or_load(this);
						ScratchReg converted{this};
						auto result_reg = converted.alloc(
							tpde::x64::PlatformConfig::FP_BANK);
						ASM(SSE_MOVQ_G2Xrr, result_reg, payload_reg);
						payload.reset(this);
						result.set_value(std::move(converted));
					} else {
						result.set_value(std::move(payload));
					}
				} else {
					payload.reset(this);
				}
				return true;
			}
			const bool source_arguments = call.operand_count == 0
				&& call.call_argument_count != 0;
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(ValuePart{
					reinterpret_cast<uintptr_t>(call.entry_cell), 8,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					source_arguments ? call.call_argument_count : call.operand_count, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_site.source_init_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_USER_CALL_BEGIN)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
			}
			for (uint32_t index = 0;
					index < (source_arguments
						? call.call_argument_count : call.operand_count); ++index) {
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				if (source_arguments) {
					const zend_mir_call_argument_ref &argument =
						adaptor->plan()->call_arguments[
							call.call_argument_offset + index];
					builder.add_arg(ValuePart{argument.ordinal, 4,
						tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
					builder.add_arg(ValuePart{argument.send_opline_index, 4,
						tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
					builder.add_arg(ValuePart{
						argument.source_mode
								== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
							? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
							: argument.ownership
									== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
								? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
								: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE,
						4, tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
					builder.call(ValuePart{
						reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
							ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT)),
						8, tpde::x64::PlatformConfig::GP_BANK});
					continue;
				}
				IRValueRef operand = node.operands[index];
				builder.add_arg(ValuePart{index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(CallArg{operand});
				if (adaptor->exact_type(operand) == ZEND_MIR_SCALAR_TYPE_F64) {
					builder.call(ValuePart{
						reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
							ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE)),
						8, tpde::x64::PlatformConfig::GP_BANK});
				} else {
					if (!zend_mir_scalar_type_is_exact(adaptor->exact_type(operand))) {
						return false;
					}
					builder.add_arg(ValuePart{
						static_cast<uint32_t>(adaptor->exact_type(operand)), 4,
						tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
					builder.call(ValuePart{
						reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
							ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER)),
						8, tpde::x64::PlatformConfig::GP_BANK});
				}
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(ValuePart{
				reinterpret_cast<uintptr_t>(call.entry_cell), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{call.call_site.source_do_opline_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE)), 8,
				tpde::x64::PlatformConfig::GP_BANK});
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::je, continued);
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				auto propagate = text_writer.label_create();
				ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
				generate_raw_jump(Jump::jne, propagate);
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
				label_place(propagate);
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			if (node.has_result) {
				tpde::x64::CCAssignerSysV result_assigner{false};
				CallBuilder result_builder{*this, result_assigner};
				result_builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				result_builder.add_arg(ValuePart{
					call.call_site.source_do_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				result_builder.add_arg(ValuePart{
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				result_builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK};
				result_builder.add_ret(payload, tpde::CCAssignment{});
				auto [result_ref, result] = result_ref_single(node.result);
				if (val_parts(node.result).bank == tpde::x64::PlatformConfig::FP_BANK) {
					auto payload_reg = payload.cur_reg_or_load(this);
					ScratchReg converted{this};
					auto result_reg = converted.alloc(
						tpde::x64::PlatformConfig::FP_BANK);
					ASM(SSE_MOVQ_G2Xrr, result_reg, payload_reg);
					payload.reset(this);
					result.set_value(std::move(converted));
				} else {
					result.set_value(std::move(payload));
				}
			}
			return true;
		}
		case ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL: {
			const zend_tpde_instruction &call =
				adaptor->mir_instruction(instruction);
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(ValuePart{
					reinterpret_cast<uintptr_t>(call.internal_call_cell), 8,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_argument_count, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_site.source_init_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
			}
			for (uint32_t index = 0; index < call.call_argument_count; ++index) {
				const zend_mir_call_argument_ref &argument =
					adaptor->plan()->call_arguments[
						call.call_argument_offset + index];
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(ValuePart{argument.ordinal, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{argument.send_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					argument.source_mode
							== ZEND_MIR_SOURCE_CALL_ARGUMENT_PLACEHOLDER
						? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
						: argument.ownership
								== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
							? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
							: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE,
					4, tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(ValuePart{
				reinterpret_cast<uintptr_t>(call.internal_call_cell), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{call.call_site.source_do_opline_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE)), 8,
				tpde::x64::PlatformConfig::GP_BANK});
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::je, continued);
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				auto propagate = text_writer.label_create();
				ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
				generate_raw_jump(Jump::jne, propagate);
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
				label_place(propagate);
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			if (node.has_result) {
				tpde::x64::CCAssignerSysV result_assigner{false};
				CallBuilder result_builder{*this, result_assigner};
				result_builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				result_builder.add_arg(ValuePart{
					call.call_site.source_do_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				result_builder.add_arg(ValuePart{
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				result_builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR)), 8,
					tpde::x64::PlatformConfig::GP_BANK});
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK};
				result_builder.add_ret(payload, tpde::CCAssignment{});
				auto [result_ref, result] = result_ref_single(node.result);
				if (val_parts(node.result).bank
						== tpde::x64::PlatformConfig::FP_BANK) {
					auto payload_reg = payload.cur_reg_or_load(this);
					ScratchReg converted{this};
					auto result_reg = converted.alloc(
						tpde::x64::PlatformConfig::FP_BANK);
					ASM(SSE_MOVQ_G2Xrr, result_reg, payload_reg);
					payload.reset(this);
					result.set_value(std::move(converted));
				} else {
					result.set_value(std::move(payload));
				}
			}
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_ENTER: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_FINALLY_ENTER)),
				8, tpde::x64::PlatformConfig::GP_BANK});
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::je, continued);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_CALL: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_FINALLY_CALL)),
				8, tpde::x64::PlatformConfig::GP_BANK});
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			if (successors.size() != 2) {
				return false;
			}
			generate_exception_branch(successors[0]);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_RETURN: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_FINALLY_RETURN)),
				8, tpde::x64::PlatformConfig::GP_BANK});
			ValuePart continuation{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(continuation, tpde::CCAssignment{});
			auto continuation_reg = continuation.cur_reg_or_load(this);
			const zend_tpde_plan *plan = adaptor->plan();
			for (uint32_t i = 0; i < plan->instruction_count; ++i) {
				const zend_mir_instruction_record &call =
					plan->instructions[i].record;
				zend_mir_block_id target;
				if (call.opcode != ZEND_MIR_OPCODE_FINALLY_CALL
						|| plan->view->successor_count(
							plan->view->context, call.block_id) != 2
						|| !plan->view->successor_at(
							plan->view->context, call.block_id, 1, &target)) {
					continue;
				}
				ASM(CMP32ri, continuation_reg, call.source_position_id);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::jne, continued);
				generate_exception_branch(adaptor->block_ref(target));
				label_place(continued);
			}
			for (uint32_t i = 0; i < plan->instruction_count; ++i) {
				const zend_mir_instruction_record &handler =
					plan->instructions[i].record;
				if ((handler.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
						&& handler.opcode != ZEND_MIR_OPCODE_FINALLY_ENTER)
						|| handler.block_id == plan->function.entry_block_id
						|| !zend_mir_id_is_valid(handler.source_position_id)) {
					continue;
				}
				ASM(CMP32ri, continuation_reg,
					ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
						| handler.source_position_id);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::jne, continued);
				generate_exception_branch(adaptor->block_ref(handler.block_id));
				label_place(continued);
			}
			continuation.reset(this);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_CATCH_ENTER: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_CATCH_ENTER)),
				8, tpde::x64::PlatformConfig::GP_BANK});
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			if (successors.size() == 2) {
				generate_cond_branch(Jump::je, successors[0], successors[1]);
				status.reset(this);
				return true;
			}
			if (successors.size() != 1) {
				return false;
			}
			auto propagate = text_writer.label_create();
			generate_raw_jump(Jump::jne, propagate);
			generate_exception_branch(successors[0]);
			label_place(propagate);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_RETURN: {
			{
			auto [value_ref, value] = val_ref_single(node.operands[0]);
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_reg = frame.load_to_reg();
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
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETURNED, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL)),
				8, tpde::x64::PlatformConfig::GP_BANK});
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, ::tpde::CCAssignment{});
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL: {
			if (node.operands.size() != 1
					|| mir.source_opline_index == UINT32_MAX) {
				return false;
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.source_opline_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL)),
				8, tpde::x64::PlatformConfig::GP_BANK});
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, ::tpde::CCAssignment{});
			if (zend_mir_id_is_valid(mir.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(mir.exception_block_id));
				status.reset(this);
				return true;
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		default:
			return false;
	}
}

struct X64ImageState {
	Adaptor adaptor;
	ZendCompilerX64 compiler;

	explicit X64ImageState(const zend_tpde_plan *plan)
		: adaptor{plan}, compiler{&adaptor} {}
};

struct X64PublishedState {
	tpde::elf::ElfMapper mapper;
};

void destroy_x64_state(void *state) {
	delete static_cast<X64ImageState *>(state);
}

void destroy_x64_published_state(void *state) {
	delete static_cast<X64PublishedState *>(state);
}

bool elf_has_writable_executable_section(const std::vector<tpde::u8> &object) {
	using namespace tpde::elf;
	if (object.size() < sizeof(Elf64_Ehdr)) {
		return true;
	}
	const auto *header = reinterpret_cast<const Elf64_Ehdr *>(object.data());
	if (header->e_shentsize != sizeof(Elf64_Shdr)
			|| header->e_shoff > object.size()
			|| header->e_shnum >
				(object.size() - header->e_shoff) / sizeof(Elf64_Shdr)) {
		return true;
	}
	const auto *sections = reinterpret_cast<const Elf64_Shdr *>(
		object.data() + header->e_shoff);
	for (uint32_t i = 0; i < header->e_shnum; ++i) {
		if ((sections[i].sh_flags & (SHF_WRITE | SHF_EXECINSTR))
				== (SHF_WRITE | SHF_EXECINSTR)) {
			return true;
		}
	}
	return false;
}

} // namespace

zend_result zend_tpde_emit_linux_x64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	auto state = std::make_unique<X64ImageState>(plan);
	if (!state->adaptor.valid() || !state->compiler.compile()) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"TPDE rejected the ZNMIR x86-64 adaptor graph");
		return FAILURE;
	}
	const auto &text = state->compiler.assembler.get_section(
		state->compiler.assembler.get_default_section(tpde::SectionKind::Text)).data;
	if (text.empty() || !zend_tpde_image_append(image, text.data(), text.size())) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to retain TPDE x86-64 text for inspection");
		return FAILURE;
	}
	image->target_state = state.release();
	image->destroy_target_state = destroy_x64_state;
	return SUCCESS;
}

zend_result zend_tpde_map_linux_x64(
	const zend_native_image *image,
	zend_native_code *code,
	zend_native_diagnostic *diag) {
#if defined(__linux__) && defined(__x86_64__)
	if (image == nullptr || image->target_state == nullptr || code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Linux TPDE mapper requires compiled assembler state");
		return FAILURE;
	}
	auto *compiled = static_cast<X64ImageState *>(image->target_state);
	std::vector<tpde::u8> object = compiled->compiler.assembler.build_object_file();
	if (elf_has_writable_executable_section(object)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"TPDE ELF image contains a writable executable section");
		return FAILURE;
	}
	auto published = std::make_unique<X64PublishedState>();
	if (!published->mapper.map(compiled->compiler.assembler,
			[](std::string_view) -> void * { return nullptr; })) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"TPDE ELF symbol or relocation mapping failed");
		return FAILURE;
	}
	auto [mapping, mapping_size] = published->mapper.get_mapped_range();
	void *entry = published->mapper.get_sym_addr(compiled->compiler.func_syms[0]);
	if (mapping == nullptr || mapping_size == 0 || entry == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"TPDE ELF entry symbol was not mapped");
		return FAILURE;
	}
	code->mapping = mapping;
	code->mapping_size = mapping_size;
	code->entry = reinterpret_cast<zend_native_frame_entry_t>(entry);
	code->unwind_registered = true;
	code->target_state = published.release();
	code->destroy_target_state = destroy_x64_published_state;
	return SUCCESS;
#else
	(void) image;
	(void) code;
	zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
		"linux-amd64-prod publication requires native Linux x86-64");
	return FAILURE;
#endif
}
