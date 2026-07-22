// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_apple_a64_abi.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"

#include <tpde/ELF.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
# include <libkern/OSCacheControl.h>
# include <pthread.h>
# include <sys/mman.h>
# include <unistd.h>

extern "C" void __register_frame(void *);
extern "C" void __deregister_frame(void *);
extern "C" void __unw_add_dynamic_eh_frame_section(uintptr_t)
	__attribute__((weak_import));
extern "C" void __unw_remove_dynamic_eh_frame_section(uintptr_t)
	__attribute__((weak_import));
#endif

namespace {

using Adaptor = zend::native::tpde::ZendIRAdaptor;
using IRValueRef = zend::native::tpde::IRValueRef;
using IRInstRef = zend::native::tpde::IRInstRef;
using IRBlockRef = zend::native::tpde::IRBlockRef;
using IRFuncRef = zend::native::tpde::IRFuncRef;
using DarwinConfig = zend::native::tpde::DarwinA64PlatformConfig;
using DarwinAssembler = zend::native::tpde::AssemblerDarwinA64;

class ZendCompilerA64 final
	: public ::tpde::a64::CompilerA64<Adaptor, ZendCompilerA64,
		::tpde::CompilerBase, DarwinConfig> {
	using Base = ::tpde::a64::CompilerA64<Adaptor, ZendCompilerA64,
		::tpde::CompilerBase, DarwinConfig>;

public:
	struct ValRefSpecial {
		uint8_t mode = 4;
		uint8_t bank = 0;
		uint8_t padding[6]{};
		uint64_t bits = 0;
	};

	struct ValueParts {
		::tpde::RegBank bank;
		uint32_t count() const { return 1; }
		uint32_t size_bytes(uint32_t) const { return 8; }
		::tpde::RegBank reg_bank(uint32_t) const { return bank; }
	};

	explicit ZendCompilerA64(Adaptor *adaptor) : Base{adaptor} {}

	void generate_exception_branch(IRBlockRef target) {
		auto index = static_cast<uint32_t>(this->analyzer.block_idx(target));
		generate_raw_jump(Jump::jmp, this->block_labels[index]);
	}

	bool cur_func_may_emit_calls() const { return adaptor->plan()->may_emit_calls; }
	::tpde::SymRef cur_personality_func() const { return {}; }
	ValueParts val_parts(IRValueRef value) const {
		return {adaptor->representation(value) == ZEND_MIR_REPRESENTATION_DOUBLE
			? DarwinConfig::FP_BANK : DarwinConfig::GP_BANK};
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
		return ValuePart{value.bits, 8, ::tpde::RegBank{value.bank}};
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

bool ZendCompilerA64::compile_inst(IRInstRef instruction, InstRange) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	if (node.kind == Adaptor::InstKind::LoadFrame) {
		auto [source_ref, source] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto source_reg = source.load_to_reg();
		auto result_reg = result.alloc_reg();
		mov(result_reg, source_reg, 8);
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::LoadArgument) {
		auto [base_ref, base] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto base_reg = base.load_to_reg();
		auto result_reg = result.alloc_reg();
		uint32_t offset = static_cast<uint32_t>(
			(ZEND_CALL_FRAME_SLOT + node.argument_index) * sizeof(zval));
		switch (adaptor->exact_type(node.result)) {
			case ZEND_MIR_SCALAR_TYPE_NULL:
				materialize_constant(
					uint64_t{0}, DarwinConfig::GP_BANK, 8, result_reg);
				break;
			case ZEND_MIR_SCALAR_TYPE_I1:
				load_off(result_reg, base_reg,
					offset + static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
				ASM(CMPwi, result_reg, IS_TRUE);
				generate_raw_set(Jump::Jeq, result_reg);
				break;
			case ZEND_MIR_SCALAR_TYPE_I64:
			case ZEND_MIR_SCALAR_TYPE_F64:
				load_off(result_reg, base_reg, offset, 8);
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
		ASM(ADDxi, slot_reg, frame_reg,
			static_cast<uint32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval)));
		ValuePart slot_pointer{DarwinConfig::GP_BANK, 8};
		slot_pointer.set_value(this, std::move(slot));

		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(std::move(frame), ::tpde::CCAssignment{});
		builder.add_arg(std::move(slot_pointer), ::tpde::CCAssignment{});
		builder.add_arg(CallArg{node.operands[0]});
		auto add_extended = [&](uint64_t bits, uint32_t size, uint8_t extension) {
			::tpde::CCAssignment assignment{};
			assignment.int_ext = extension;
			assignment.align = static_cast<uint8_t>(size);
			builder.add_arg(ValuePart{bits, size, DarwinConfig::GP_BANK}, assignment);
		};
		add_extended(UINT64_C(0xfe), 1, 8);
		add_extended(UINT64_C(0x80), 1, UINT8_C(0x80) | 8);
		add_extended(UINT64_C(0xfedc), 2, 16);
		add_extended(UINT64_C(0x8001), 2, UINT8_C(0x80) | 16);
		add_extended(UINT64_C(0xfedcba98), 4, 32);
		add_extended(UINT64_C(0x89abcdef), 4, UINT8_C(0x80) | 32);
		::tpde::CCAssignment wide_assignment{};
		wide_assignment.align = 8;
		builder.add_arg(ValuePart{UINT64_C(0xfedcba9876543210), 8,
			DarwinConfig::GP_BANK}, wide_assignment);
		builder.add_arg(ValuePart{UINT64_C(0xfedcba9876543211), 8,
			DarwinConfig::GP_BANK}, wide_assignment);
		builder.add_arg(ValuePart{UINT64_C(0x0123456789abcdef), 8,
			DarwinConfig::GP_BANK}, wide_assignment);
		builder.add_arg(ValuePart{UINT64_C(0x8877665544332211), 8,
			DarwinConfig::GP_BANK}, wide_assignment);
		for (uint64_t bits : {
				UINT64_C(0x3ff8000000000000), UINT64_C(0xc002000000000000),
				UINT64_C(0x4009000000000000), UINT64_C(0xc012000000000000),
				UINT64_C(0x4017000000000000), UINT64_C(0xc01b800000000000),
				UINT64_C(0x401c000000000000), UINT64_C(0xc020400000000000),
				UINT64_C(0x4022800000000000), UINT64_C(0xc025000000000000)}) {
			builder.add_arg(ValuePart{bits, 8, DarwinConfig::FP_BANK},
				wide_assignment);
		}
		builder.call(ValuePart{
			reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
				ZEND_NATIVE_HELPER_ABI_CONFORMANCE)), 8,
			DarwinConfig::GP_BANK});
		ValuePart status{DarwinConfig::GP_BANK};
		builder.add_ret(status, ::tpde::CCAssignment{});
		auto status_reg = status.cur_reg_or_load(this);
		ASM(CMPxi, status_reg, ZEND_NATIVE_ABI_CONFORMANCE_RESULT);
		auto matched = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, matched);
		status.reset(this);
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(ValuePart{ZEND_NATIVE_BAILOUT, 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
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
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
		if (exact_type == ZEND_MIR_SCALAR_TYPE_F64) {
			builder.add_arg(CallArg{node.operands[0]});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_ECHO_DOUBLE)), 8,
				DarwinConfig::GP_BANK});
		} else {
			if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				builder.add_arg(ValuePart{uint64_t{0}, 8, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
			} else {
				builder.add_arg(CallArg{node.operands[0]});
			}
			builder.add_arg(ValuePart{
				static_cast<uint32_t>(exact_type), 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_ECHO_INTEGER)), 8,
				DarwinConfig::GP_BANK});
		}
		return true;
	}
	auto unary = [&]() { return val_ref_single(node.operands[0]); };
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
		emit(result_reg, left_reg, right_reg);
		result.set_modified();
		return true;
	};
	auto integer_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		auto [result_ref, result] = result_ref_single(node.result);
		ASM(CMPx, left.load_to_reg(), right.load_to_reg());
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
		emit(result_reg, left_reg, right_reg);
		result.set_modified();
		return true;
	};
	auto floating_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		auto [result_ref, result] = result_ref_single(node.result);
		ASM(FCMP_d, left.load_to_reg(), right.load_to_reg());
		auto result_reg = result.alloc_reg();
		generate_raw_set(condition, result_reg);
		result.set_modified();
		return true;
	};
	auto execute_value_operation = [&](zend_native_runtime_helper_id helper) {
		if (node.operands.size() != 1 || mir.source_opline_index == UINT32_MAX) {
			return false;
		}
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{node.operands[0]});
		builder.add_arg(ValuePart{mir.source_opline_index, 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.call(ValuePart{
			reinterpret_cast<uintptr_t>(adaptor->runtime_helper(helper)), 8,
			DarwinConfig::GP_BANK});
		ValuePart status{DarwinConfig::GP_BANK};
		builder.add_ret(status, ::tpde::CCAssignment{});
		auto status_reg = status.cur_reg_or_load(this);
		ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, continued);
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status), ::tpde::CCAssignment{});
		return_builder.ret();
		label_place(continued);
		return true;
	};

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
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN);
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_QM_ASSIGN);
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
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[0]});
				builder.add_arg(ValuePart{mir.source_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_INTERRUPT_POLL)), 8,
					DarwinConfig::GP_BANK});
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
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(ADDx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(SUBx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(MULx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_BIT_OR:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(ORRx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_BIT_AND:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(ANDx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_BIT_XOR:
		case ZEND_MIR_OPCODE_I1_XOR:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(EORx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_BIT_NOT: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto source_reg = source.load_to_reg();
			auto result_reg = result.alloc_try_reuse(source);
			ASM(MVNx, result_reg, source_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_I1_NOT:
		case ZEND_MIR_OPCODE_I64_TO_I1: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			ASM(CMPxi, source.load_to_reg(), 0);
			auto result_reg = result.alloc_reg();
			generate_raw_set(mir.record.opcode == ZEND_MIR_OPCODE_I1_NOT
				? Jump::Jeq : Jump::Jne, result_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_I64_EQ:
		case ZEND_MIR_OPCODE_I1_EQ:
			return integer_compare(Jump::Jeq);
		case ZEND_MIR_OPCODE_I64_LT:
			return integer_compare(Jump::Jlt);
		case ZEND_MIR_OPCODE_I64_LE:
			return integer_compare(Jump::Jle);
		case ZEND_MIR_OPCODE_I64_CMP: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			ASM(CMPx, left.load_to_reg(), right.load_to_reg());
			ScratchReg less{this};
			ScratchReg greater{this};
			auto less_reg = less.alloc_gp();
			auto greater_reg = greater.alloc_gp();
			generate_raw_set(Jump::Jlt, less_reg);
			generate_raw_set(Jump::Jgt, greater_reg);
			ASM(SUBx, greater_reg, greater_reg, less_reg);
			auto [result_ref, result] = result_ref_single(node.result);
			result.set_value(std::move(greater));
			return true;
		}
		case ZEND_MIR_OPCODE_I64_MOD_NONZERO: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			ScratchReg quotient{this};
			auto quotient_reg = quotient.alloc_gp();
			auto left_reg = left.load_to_reg();
			auto right_reg = right.load_to_reg();
			ASM(SDIVx, quotient_reg, left_reg, right_reg);
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(MSUBx, result_reg, quotient_reg, right_reg, left_reg);
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_I64_SHL_CHECKED:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(LSLVx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED:
			return integer_binary([&](auto dst, auto left, auto right) {
				ASM(ASRVx, dst, left, right);
			});
		case ZEND_MIR_OPCODE_F64_ADD:
			return floating_binary([&](auto dst, auto left, auto right) {
				ASM(FADDd, dst, left, right);
			});
		case ZEND_MIR_OPCODE_F64_SUB:
			return floating_binary([&](auto dst, auto left, auto right) {
				ASM(FSUBd, dst, left, right);
			});
		case ZEND_MIR_OPCODE_F64_MUL:
			return floating_binary([&](auto dst, auto left, auto right) {
				ASM(FMULd, dst, left, right);
			});
		case ZEND_MIR_OPCODE_F64_EQ:
			return floating_compare(Jump::Jeq);
		case ZEND_MIR_OPCODE_F64_LT:
			return floating_compare(Jump::Jlt);
		case ZEND_MIR_OPCODE_F64_LE:
			return floating_compare(Jump::Jle);
		case ZEND_MIR_OPCODE_F64_CMP: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			ASM(FCMP_d, left.load_to_reg(), right.load_to_reg());
			ScratchReg less{this};
			ScratchReg greater{this};
			auto less_reg = less.alloc_gp();
			auto greater_reg = greater.alloc_gp();
			generate_raw_set(Jump::Jlt, less_reg);
			generate_raw_set(Jump::Jgt, greater_reg);
			ASM(SUBx, greater_reg, greater_reg, less_reg);
			auto [result_ref, result] = result_ref_single(node.result);
			result.set_value(std::move(greater));
			return true;
		}
		case ZEND_MIR_OPCODE_I64_TO_F64:
		case ZEND_MIR_OPCODE_I1_TO_F64: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(SCVTFdx, result_reg, source.load_to_reg());
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_F64_TO_I64_CHECKED: {
			auto [source_ref, source] = unary();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(FCVTZSxd, result_reg, source.load_to_reg());
			result.set_modified();
			return true;
		}
		case ZEND_MIR_OPCODE_F64_TO_I1: {
			auto [source_ref, source] = unary();
			ScratchReg bits{this};
			auto bits_reg = bits.alloc_gp();
			ASM(FMOVxd, bits_reg, source.load_to_reg());
			ASM(LSLxi, bits_reg, bits_reg, 1);
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(CMPxi, bits_reg, 0);
			generate_raw_set(Jump::Jne, result_reg);
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
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			generate_cond_branch(Jump{Jump::Cbnz, condition_reg, false},
				successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_CALL_DIRECT_USER: {
			const zend_tpde_instruction &call =
				adaptor->mir_instruction(instruction);
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(ValuePart{
					reinterpret_cast<uintptr_t>(call.entry_cell), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.operand_count, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_site.source_init_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_USER_CALL_BEGIN)), 8,
					DarwinConfig::GP_BANK});
			}
			for (uint32_t index = 0; index < call.operand_count; ++index) {
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				IRValueRef operand = node.operands[index];
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(ValuePart{index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(CallArg{operand});
				if (adaptor->exact_type(operand) == ZEND_MIR_SCALAR_TYPE_F64) {
					builder.call(ValuePart{
						reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
							ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE)),
						8, DarwinConfig::GP_BANK});
				} else {
					if (!zend_mir_scalar_type_is_exact(adaptor->exact_type(operand))) {
						return false;
					}
					builder.add_arg(ValuePart{
						static_cast<uint32_t>(adaptor->exact_type(operand)), 4,
						DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
					builder.call(ValuePart{
						reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
							ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER)),
						8, DarwinConfig::GP_BANK});
				}
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(ValuePart{
				reinterpret_cast<uintptr_t>(call.entry_cell), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{call.call_site.source_do_opline_index, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE)), 8,
				DarwinConfig::GP_BANK});
			ValuePart status{DarwinConfig::GP_BANK};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, continued);
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				auto propagate = text_writer.label_create();
				ASM(CMPxi, status_reg, ZEND_NATIVE_EXCEPTION);
				generate_raw_jump(Jump::Jne, propagate);
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
				label_place(propagate);
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			if (node.has_result) {
				zend::native::tpde::CCAssignerAppleA64 result_assigner;
				CallBuilder result_builder{*this, result_assigner};
				result_builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				result_builder.add_arg(ValuePart{
					call.call_site.source_do_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				result_builder.add_arg(ValuePart{
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				result_builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR)), 8,
					DarwinConfig::GP_BANK});
				ValuePart payload{DarwinConfig::GP_BANK};
				result_builder.add_ret(payload, ::tpde::CCAssignment{});
				auto [result_ref, result] = result_ref_single(node.result);
				if (val_parts(node.result).bank == DarwinConfig::FP_BANK) {
					auto payload_reg = payload.cur_reg_or_load(this);
					ScratchReg converted{this};
					auto result_reg = converted.alloc(DarwinConfig::FP_BANK);
					ASM(FMOVdx, result_reg, payload_reg);
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
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[0]});
				builder.add_arg(ValuePart{
					reinterpret_cast<uintptr_t>(call.internal_call_cell), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_argument_count, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_site.source_init_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN)), 8,
					DarwinConfig::GP_BANK});
			}
			for (uint32_t index = 0; index < call.call_argument_count; ++index) {
				const zend_mir_call_argument_ref &argument =
					adaptor->plan()->call_arguments[
						call.call_argument_offset + index];
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[index + 1]});
				builder.add_arg(ValuePart{argument.ordinal, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{argument.send_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					argument.ownership
						== ZEND_MIR_CALL_ARGUMENT_SOURCE_ZVAL_BY_REFERENCE
						? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
						: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE,
					4, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT)), 8,
					DarwinConfig::GP_BANK});
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[call.call_argument_count + 1]});
			builder.add_arg(ValuePart{
				reinterpret_cast<uintptr_t>(call.internal_call_cell), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{call.call_site.source_do_opline_index, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE)), 8,
				DarwinConfig::GP_BANK});
			ValuePart status{DarwinConfig::GP_BANK};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, continued);
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				auto propagate = text_writer.label_create();
				ASM(CMPxi, status_reg, ZEND_NATIVE_EXCEPTION);
				generate_raw_jump(Jump::Jne, propagate);
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
				label_place(propagate);
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			if (node.has_result) {
				zend::native::tpde::CCAssignerAppleA64 result_assigner;
				CallBuilder result_builder{*this, result_assigner};
				result_builder.add_arg(CallArg{
					node.operands[call.call_argument_count + 2]});
				result_builder.add_arg(ValuePart{
					call.call_site.source_do_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				result_builder.add_arg(ValuePart{
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				result_builder.call(ValuePart{
					reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR)), 8,
					DarwinConfig::GP_BANK});
				ValuePart payload{DarwinConfig::GP_BANK};
				result_builder.add_ret(payload, ::tpde::CCAssignment{});
				auto [result_ref, result] = result_ref_single(node.result);
				if (val_parts(node.result).bank == DarwinConfig::FP_BANK) {
					auto payload_reg = payload.cur_reg_or_load(this);
					ScratchReg converted{this};
					auto result_reg = converted.alloc(DarwinConfig::FP_BANK);
					ASM(FMOVdx, result_reg, payload_reg);
					payload.reset(this);
					result.set_value(std::move(converted));
				} else {
					result.set_value(std::move(payload));
				}
			}
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_ENTER: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_FINALLY_ENTER)),
				8, DarwinConfig::GP_BANK});
			ValuePart status{DarwinConfig::GP_BANK};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, continued);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_CALL: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_FINALLY_CALL)),
				8, DarwinConfig::GP_BANK});
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			if (successors.size() != 2) {
				return false;
			}
			generate_exception_branch(successors[0]);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_RETURN: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_FINALLY_RETURN)),
				8, DarwinConfig::GP_BANK});
			ValuePart continuation{DarwinConfig::GP_BANK};
			builder.add_ret(continuation, ::tpde::CCAssignment{});
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
				ASM(CMPxi, continuation_reg, call.source_position_id);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::Jne, continued);
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
				ScratchReg expected{this};
				auto expected_reg = expected.alloc_gp();
				materialize_constant(
					ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
						| handler.source_position_id,
					DarwinConfig::GP_BANK, 4, expected_reg);
				ASM(CMPx, continuation_reg, expected_reg);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::Jne, continued);
				generate_exception_branch(adaptor->block_ref(handler.block_id));
				label_place(continued);
			}
			continuation.reset(this);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_CATCH_ENTER: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_CATCH_ENTER)),
				8, DarwinConfig::GP_BANK});
			ValuePart status{DarwinConfig::GP_BANK};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(mir.record.block_id));
			if (successors.size() == 2) {
				generate_cond_branch(Jump::Jeq, successors[0], successors[1]);
				status.reset(this);
				return true;
			}
			if (successors.size() != 1) {
				return false;
			}
			auto propagate = text_writer.label_create();
			generate_raw_jump(Jump::Jne, propagate);
			generate_exception_branch(successors[0]);
			label_place(propagate);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
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
			load_off(pointer_reg, frame_reg,
				static_cast<uint32_t>(offsetof(zend_execute_data, return_value)), 8);
			auto no_result = text_writer.label_create();
			generate_raw_jump(Jump{Jump::Cbz, pointer_reg, false}, no_result);
			auto value_reg = value.load_to_reg();
			store_off(pointer_reg, 0, value_reg, 8);
			ScratchReg kind{this};
			auto kind_reg = kind.alloc_gp();
			uint32_t type = zval_type(*adaptor, node.operands[0]);
			materialize_constant(type, DarwinConfig::GP_BANK, 4, kind_reg);
			if (type == IS_FALSE) {
				ASM(ADDx, kind_reg, kind_reg, value_reg);
			}
			store_off(pointer_reg, 8, kind_reg, 4);
			label_place(no_result);
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETURNED, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{mir.record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(ValuePart{
				reinterpret_cast<uintptr_t>(adaptor->runtime_helper(
					ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL)),
				8, DarwinConfig::GP_BANK});
			ValuePart status{DarwinConfig::GP_BANK};
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

struct A64ImageState {
	Adaptor adaptor;
	ZendCompilerA64 compiler;

	explicit A64ImageState(const zend_tpde_plan *plan)
		: adaptor{plan}, compiler{&adaptor} {}
};

void destroy_a64_image_state(void *state) {
	delete static_cast<A64ImageState *>(state);
}

#if defined(__APPLE__) && defined(__aarch64__)

struct MappedSection {
	void *mapping = nullptr;
	size_t mapping_size = 0;
	uint32_t flags = 0;
};

struct A64PublishedState {
	std::vector<MappedSection> sections;
	void *unwind_section = nullptr;
	bool unwind_registered = false;

	~A64PublishedState() {
		if (unwind_registered) {
			if (__unw_remove_dynamic_eh_frame_section != nullptr) {
				__unw_remove_dynamic_eh_frame_section(
					reinterpret_cast<uintptr_t>(unwind_section));
			} else {
				__deregister_frame(unwind_section);
			}
			unwind_registered = false;
		}
		for (const MappedSection &section : sections) {
			if (section.mapping != nullptr) {
				munmap(section.mapping, section.mapping_size);
			}
		}
	}
};

void destroy_a64_published_state(void *state) {
	delete static_cast<A64PublishedState *>(state);
}

bool signed_range(int64_t value, unsigned bits) {
	const int64_t minimum = -(INT64_C(1) << (bits - 1));
	const int64_t maximum = (INT64_C(1) << (bits - 1)) - 1;
	return value >= minimum && value <= maximum;
}

bool apply_relocation(uint8_t *location, uint32_t type,
	uintptr_t symbol, int32_t addend) {
	using namespace ::tpde::elf;
	const uintptr_t place = reinterpret_cast<uintptr_t>(location);
	const uintptr_t target = symbol + static_cast<intptr_t>(addend);
	auto load32 = [&]() {
		uint32_t value;
		std::memcpy(&value, location, sizeof(value));
		return value;
	};
	auto store32 = [&](uint32_t value) {
		std::memcpy(location, &value, sizeof(value));
	};
	switch (type) {
		case R_AARCH64_ABS64:
			std::memcpy(location, &target, sizeof(target));
			return true;
		case R_AARCH64_PREL32: {
			int64_t delta = static_cast<int64_t>(target - place);
			if (!signed_range(delta, 32)) return false;
			int32_t value = static_cast<int32_t>(delta);
			std::memcpy(location, &value, sizeof(value));
			return true;
		}
		case R_AARCH64_JUMP26:
		case R_AARCH64_CALL26: {
			int64_t delta = static_cast<int64_t>(target - place);
			if ((delta & 3) != 0 || !signed_range(delta >> 2, 26)) return false;
			store32((load32() & UINT32_C(0xfc000000))
				| (static_cast<uint32_t>(delta >> 2) & UINT32_C(0x03ffffff)));
			return true;
		}
		case R_AARCH64_ADR_PREL_PG_HI21:
		case R_AARCH64_ADR_PREL_PG_HI21_NC: {
			int64_t pages = (static_cast<int64_t>(target & ~uintptr_t{0xfff})
				- static_cast<int64_t>(place & ~uintptr_t{0xfff})) >> 12;
			if (type == R_AARCH64_ADR_PREL_PG_HI21 && !signed_range(pages, 21)) {
				return false;
			}
			uint32_t instruction = load32() & UINT32_C(0x9f00001f);
			uint32_t encoded = static_cast<uint32_t>(pages) & UINT32_C(0x1fffff);
			instruction |= (encoded & 3) << 29;
			instruction |= ((encoded >> 2) & UINT32_C(0x7ffff)) << 5;
			store32(instruction);
			return true;
		}
		case R_AARCH64_ADD_ABS_LO12_NC:
			store32((load32() & ~UINT32_C(0x003ffc00))
				| ((static_cast<uint32_t>(target) & UINT32_C(0xfff)) << 10));
			return true;
		case R_AARCH64_LDST8_ABS_LO12_NC:
		case R_AARCH64_LDST16_ABS_LO12_NC:
		case R_AARCH64_LDST32_ABS_LO12_NC:
		case R_AARCH64_LDST64_ABS_LO12_NC:
		case R_AARCH64_LDST128_ABS_LO12_NC: {
			unsigned shift = type == R_AARCH64_LDST8_ABS_LO12_NC ? 0
				: type == R_AARCH64_LDST16_ABS_LO12_NC ? 1
				: type == R_AARCH64_LDST32_ABS_LO12_NC ? 2
				: type == R_AARCH64_LDST64_ABS_LO12_NC ? 3 : 4;
			store32((load32() & ~UINT32_C(0x003ffc00))
				| (((static_cast<uint32_t>(target) >> shift) & UINT32_C(0xfff)) << 10));
			return true;
		}
		default:
			return false;
	}
}

#endif

} // namespace

zend_result zend_tpde_emit_darwin_arm64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	auto state = std::make_unique<A64ImageState>(plan);
	if (!state->adaptor.valid() || !state->compiler.compile()) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"TPDE rejected the ZNMIR arm64 adaptor graph");
		return FAILURE;
	}
	std::vector<::tpde::u8> finalized =
		state->compiler.assembler.build_object_file();
	const ::tpde::DataSection &text = state->compiler.assembler.get_section(
		state->compiler.assembler.get_default_section(::tpde::SectionKind::Text));
	if (finalized.empty() || text.data.empty() || !zend_tpde_image_append(
			image, text.data.data(), text.data.size())) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to retain the finalized TPDE arm64 image");
		return FAILURE;
	}
	image->target_state = state.release();
	image->destroy_target_state = destroy_a64_image_state;
	return SUCCESS;
}

zend_result zend_tpde_map_darwin_arm64(
	const zend_native_image *image,
	zend_native_code *code,
	zend_native_diagnostic *diag) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (image == nullptr || image->target_state == nullptr || code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Darwin TPDE mapper requires compiled assembler state");
		return FAILURE;
	}
	long page_size_value = sysconf(_SC_PAGESIZE);
	if (page_size_value <= 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin page size is unavailable");
		return FAILURE;
	}
	size_t page_size = static_cast<size_t>(page_size_value);
	auto *compiled = static_cast<A64ImageState *>(image->target_state);
	DarwinAssembler &assembler = compiled->compiler.assembler;
	::tpde::SecRef unwind_ref = assembler.get_default_section(
		::tpde::SectionKind::EHFrame);
	auto published = std::make_unique<A64PublishedState>();
	published->sections.resize(assembler.section_count());
	bool has_executable = false;

	for (size_t i = 1; i < assembler.section_count(); ++i) {
		if (!assembler.section_present(i)) continue;
		const ::tpde::DataSection &section = assembler.get_section(
			::tpde::SecRef{static_cast<uint32_t>(i)});
		if ((section.flags & DarwinAssembler::SECTION_ALLOC) == 0
				|| section.size() == 0) continue;
		size_t logical_size = section.size();
		if (i == unwind_ref.id()) {
			if (logical_size > SIZE_MAX - sizeof(uint32_t)) {
				zend_tpde_set_diagnostic(diag,
					ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin unwind section size overflows its terminator");
				return FAILURE;
			}
			logical_size += sizeof(uint32_t);
		}
		if (logical_size > SIZE_MAX - (page_size - 1)) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin section size overflows page alignment");
			return FAILURE;
		}
		size_t mapping_size = (logical_size + page_size - 1)
			& ~(page_size - 1);
		bool executable = (section.flags & DarwinAssembler::SECTION_EXEC) != 0;
		int map_flags = MAP_PRIVATE | MAP_ANON | (executable ? MAP_JIT : 0);
		void *mapping = mmap(nullptr, mapping_size,
			executable ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE,
			map_flags, -1, 0);
		if (mapping == MAP_FAILED) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin section mapping failed");
			return FAILURE;
		}
		published->sections[i] = {mapping, mapping_size, section.flags};
		assembler.get_section(::tpde::SecRef{static_cast<uint32_t>(i)}).addr =
			reinterpret_cast<uintptr_t>(mapping);
		has_executable |= executable;
	}

	if (has_executable) pthread_jit_write_protect_np(0);
	for (size_t i = 1; i < assembler.section_count(); ++i) {
		if (!assembler.section_present(i)
				|| published->sections[i].mapping == nullptr) continue;
		const ::tpde::DataSection &section = assembler.get_section(
			::tpde::SecRef{static_cast<uint32_t>(i)});
		if (!section.is_virtual && !section.data.empty()) {
			std::memcpy(published->sections[i].mapping,
				section.data.data(), section.data.size());
		}
	}

	for (size_t i = 1; i < assembler.section_count(); ++i) {
		if (!assembler.section_present(i)
				|| published->sections[i].mapping == nullptr) continue;
		const ::tpde::DataSection &section = assembler.get_section(
			::tpde::SecRef{static_cast<uint32_t>(i)});
		if (section.is_virtual) continue;
		for (const ::tpde::Relocation &relocation : section.relocations()) {
			if (relocation.symbol.id() >= assembler.symbol_count()
					|| relocation.offset > section.size()
					|| section.size() - relocation.offset < sizeof(uint32_t)) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE relocation is outside its section");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
			const DarwinAssembler::Symbol &symbol = assembler.symbol(relocation.symbol);
			if (!symbol.defined || symbol.section.id() >= published->sections.size()
					|| published->sections[symbol.section.id()].mapping == nullptr) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE image contains an unresolved symbol");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
			uint8_t *location = static_cast<uint8_t *>(
				published->sections[i].mapping) + relocation.offset;
			uintptr_t symbol_address = reinterpret_cast<uintptr_t>(
				published->sections[symbol.section.id()].mapping) + symbol.offset;
			if (!apply_relocation(location, relocation.type,
					symbol_address, relocation.addend)) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE relocation kind or range is unsupported");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
		}
	}

	void *entry = nullptr;
	::tpde::SymRef entry_ref = compiled->compiler.func_syms[0];
	if (entry_ref.id() < assembler.symbol_count()) {
		const DarwinAssembler::Symbol &symbol = assembler.symbol(entry_ref);
		if (symbol.defined && symbol.section.id() < published->sections.size()
				&& published->sections[symbol.section.id()].mapping != nullptr) {
			entry = static_cast<uint8_t *>(
				published->sections[symbol.section.id()].mapping) + symbol.offset;
		}
	}
	if (entry == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE entry symbol was not mapped");
		if (has_executable) pthread_jit_write_protect_np(1);
		return FAILURE;
	}

	for (const MappedSection &section : published->sections) {
		if (section.mapping == nullptr) continue;
		if ((section.flags & DarwinAssembler::SECTION_EXEC) != 0) {
			sys_icache_invalidate(section.mapping, section.mapping_size);
		}
	}
	if (has_executable) pthread_jit_write_protect_np(1);
	for (const MappedSection &section : published->sections) {
		if (section.mapping == nullptr) continue;
		/* MAP_JIT deliberately retains RWX as its maximum VM protection.  Apple
		 * enforces the effective W^X state per thread through
		 * pthread_jit_write_protect_np; mprotect(RX) is rejected for MAP_JIT
		 * mappings on supported Darwin versions. */
		if ((section.flags & DarwinAssembler::SECTION_EXEC) != 0) continue;
		int protection = PROT_READ;
		if ((section.flags & DarwinAssembler::SECTION_WRITE) != 0) {
			protection |= PROT_WRITE;
		}
		if (mprotect(section.mapping, section.mapping_size, protection) != 0) {
			zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin final section protection failed");
			return FAILURE;
		}
	}
	if (!unwind_ref.valid() || unwind_ref.id() >= published->sections.size()
			|| !assembler.section_present(unwind_ref.id())
			|| assembler.get_section(unwind_ref).data.empty()
			|| published->sections[unwind_ref.id()].mapping == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image has no publishable unwind information");
		return FAILURE;
	}
	published->unwind_section = published->sections[unwind_ref.id()].mapping;
	if (__unw_add_dynamic_eh_frame_section != nullptr) {
		__unw_add_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(published->unwind_section));
	} else {
		__register_frame(published->unwind_section);
	}
	published->unwind_registered = true;

	const MappedSection &entry_section = published->sections[
		assembler.symbol(entry_ref).section.id()];
	code->mapping = entry_section.mapping;
	code->mapping_size = entry_section.mapping_size;
	code->entry = reinterpret_cast<zend_native_frame_entry_t>(entry);
	code->unwind_registered = published->unwind_registered;
	code->target_state = published.release();
	code->destroy_target_state = destroy_a64_published_state;
	return SUCCESS;
#else
	(void) image;
	(void) code;
	zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_TARGET_MISMATCH,
		"darwin-arm64-dev publication requires native Apple Silicon");
	return FAILURE;
#endif
}
