// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"

#include <tpde/x64/CompilerX64.hpp>
#include <tpde/ElfMapper.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using Adaptor = zend::native::tpde::ZendIRAdaptor;
using IRValueRef = zend::native::tpde::IRValueRef;
using IRInstRef = zend::native::tpde::IRInstRef;
using IRBlockRef = zend::native::tpde::IRBlockRef;
using IRFuncRef = zend::native::tpde::IRFuncRef;

class ZendCompilerX64 final
	: public tpde::x64::CompilerX64<Adaptor, ZendCompilerX64> {
	using Base = tpde::x64::CompilerX64<Adaptor, ZendCompilerX64>;
	zend_native_image *image_;
	std::array<tpde::SymRef, ZEND_NATIVE_HELPER_COUNT> runtime_symbols_{};
	std::vector<tpde::SymRef> image_symbols_;
	std::vector<tpde::SymRef> image_slots_;

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

	explicit ZendCompilerX64(Adaptor *adaptor, zend_native_image *image)
		: Base{adaptor},
		  image_{image},
		  image_symbols_(image->symbol_count),
		  image_slots_(image->symbol_count) {}

	tpde::SymRef runtime_symbol(zend_native_runtime_helper_id id) {
		tpde::SymRef &reference =
			runtime_symbols_[static_cast<uint32_t>(id)];
		if (!reference.valid()) {
			const zend_native_image_symbol *symbol = zend_tpde_image_symbol_find(
				image_, ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
				static_cast<uint32_t>(id));
			if (symbol == nullptr) {
				return {};
			}
			reference = assembler.sym_add_undef(symbol->name,
				tpde::Assembler::SymBinding::GLOBAL);
		}
		return reference;
	}

	ValuePart image_symbol_value(
		zend_native_image_symbol_kind kind, uint32_t id) {
		const zend_native_image_symbol *symbol =
			zend_tpde_image_symbol_find(image_, kind, id);
		if (symbol == nullptr) {
			return ValuePart{tpde::x64::PlatformConfig::GP_BANK, 8};
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
		ValuePart target{tpde::x64::PlatformConfig::GP_BANK, 8};
		const auto target_reg = target.alloc_reg(this);
		text_writer.ensure_space(16);
		ASM(MOV64rm, target_reg, FE_MEM(FE_IP, 0, FE_NOREG, -1));
		reloc_text(slot, tpde::elf::R_X86_64_PC32,
			text_writer.offset() - 4, -4);
		return target;
	}

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
	if (node.kind == Adaptor::InstKind::LoadFrame
			|| node.kind == Adaptor::InstKind::LoadExecutionContext) {
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
	const zend_mir_instruction_record record =
		adaptor->instruction_record(instruction);
	if (!zend_mir_id_is_valid(record.id)) {
		return false;
	}
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
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_ABI_CONFORMANCE));
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
	if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR
			|| mir.source_effect == ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR) {
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
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_ECHO_DOUBLE));
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
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_ECHO_INTEGER));
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
			|| helper == ZEND_NATIVE_HELPER_VALUE_COPY_TMP
			|| helper == ZEND_NATIVE_HELPER_VALUE_FREE
			|| helper == ZEND_NATIVE_HELPER_VALUE_CONCAT
			|| helper == ZEND_NATIVE_HELPER_VALUE_FAST_CONCAT
			|| helper == ZEND_NATIVE_HELPER_VALUE_BINARY_OP
			|| helper == ZEND_NATIVE_HELPER_VALUE_CAST
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_OP
			|| helper == ZEND_NATIVE_HELPER_VALUE_INCDEC
			|| helper == ZEND_NATIVE_HELPER_VALUE_MAKE_REF
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_REF
			|| helper == ZEND_NATIVE_HELPER_VALUE_SEPARATE
			|| helper == ZEND_NATIVE_HELPER_VALUE_UNSET_CV
			|| helper == ZEND_NATIVE_HELPER_VALUE_CHECK_VAR
			|| helper == ZEND_NATIVE_HELPER_VALUE_TYPE_CHECK
			|| helper == ZEND_NATIVE_HELPER_VALUE_ROPE_INIT
			|| helper == ZEND_NATIVE_HELPER_VALUE_ROPE_ADD
			|| helper == ZEND_NATIVE_HELPER_VALUE_ROPE_END
			|| helper == ZEND_NATIVE_HELPER_VALUE_INIT_ARRAY
			|| helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_ELEMENT
			|| helper == ZEND_NATIVE_HELPER_VALUE_ADD_ARRAY_UNPACK
			|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV
			|| helper == ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP
			|| helper == ZEND_NATIVE_HELPER_VALUE_UNSET_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_FE_FREE
			|| helper == ZEND_NATIVE_HELPER_VALUE_FETCH_LIST
			|| helper == ZEND_NATIVE_HELPER_VALUE_UNARY_OP
			|| helper == ZEND_NATIVE_HELPER_VERIFY_RETURN_TYPE
			|| helper == ZEND_NATIVE_HELPER_VALUE_ECHO
			|| helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_THIS
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_FETCH_R
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_FETCH_UNSET)
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_ASSIGN
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_ASSIGN_OP)
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_UNSET
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_POST_DEC)
			|| helper == ZEND_NATIVE_HELPER_OBJECT_INSTANCEOF
			|| helper == ZEND_NATIVE_HELPER_OBJECT_CLONE
			|| (helper >= ZEND_NATIVE_HELPER_STATIC_FETCH_R
				&& helper <= ZEND_NATIVE_HELPER_STATIC_UNSET)
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS_CONSTANT
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS_NAME
			|| (helper >= ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R
				&& helper
					<= ZEND_NATIVE_HELPER_DYNAMIC_INCLUDE_OR_EVAL)
			|| (helper >= ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_R
				&& helper <= ZEND_NATIVE_HELPER_VALUE_FETCH_DIM_UNSET);
		const bool explicit_object_operands =
			helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_THIS
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_FETCH_R
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_FETCH_UNSET)
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_ASSIGN
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_ASSIGN_OP)
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_UNSET
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_POST_DEC)
			|| helper == ZEND_NATIVE_HELPER_OBJECT_INSTANCEOF
			|| helper == ZEND_NATIVE_HELPER_OBJECT_CLONE
			|| (helper >= ZEND_NATIVE_HELPER_STATIC_FETCH_R
				&& helper <= ZEND_NATIVE_HELPER_STATIC_UNSET)
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS_CONSTANT
			|| helper == ZEND_NATIVE_HELPER_OBJECT_FETCH_CLASS_NAME;
		const bool explicit_auxiliary =
			helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM
			|| helper == ZEND_NATIVE_HELPER_VALUE_ASSIGN_DIM_OP
			|| (helper >= ZEND_NATIVE_HELPER_OBJECT_ASSIGN
				&& helper <= ZEND_NATIVE_HELPER_OBJECT_ASSIGN_OP)
			|| (helper >= ZEND_NATIVE_HELPER_STATIC_ASSIGN
				&& helper <= ZEND_NATIVE_HELPER_STATIC_ASSIGN_OP)
			|| (helper >= ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R
				&& helper
					<= ZEND_NATIVE_HELPER_DYNAMIC_DECLARE_ATTRIBUTED_CONSTANT);
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
			auto encode_operand = [&](const zend_mir_source_operand_ref &operand,
					uint32_t unused_payload) {
				return explicit_object_operands
					? zend_tpde_encode_value_operand(operand, unused_payload)
					: zend_tpde_encode_value_operand(operand);
			};
			builder.add_arg(ValuePart{
				encode_operand(
					operation.op1, operation.op1_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			if (helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL) {
				builder.add_arg(ValuePart{operation.source_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{operation.source_position_id, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.call(runtime_symbol(helper));
				ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
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
			}
			builder.add_arg(ValuePart{
				encode_operand(
					operation.op2, operation.op2_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				encode_operand(
					operation.result, operation.result_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			if (explicit_auxiliary) {
				builder.add_arg(ValuePart{
					encode_operand(operation.auxiliary,
						operation.auxiliary_unused_payload), 8,
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
		builder.call(runtime_symbol(helper));
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
	auto copy_slot = [&](
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
		ScratchReg probe{this};
		auto source_slot_reg = source_slot.alloc_gp();
		auto target_slot_reg = target_slot.alloc_gp();
		auto source_type_reg = source_type.alloc_gp();
		auto target_type_reg = target_type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto probe_reg = probe.alloc_gp();

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
		if (source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR) {
			ASM(MOV32rr, probe_reg, source_type_reg);
			ASM(AND32ri, probe_reg, Z_TYPE_MASK);
			ASM(CMP32ri, probe_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
		}
		ASM(MOV32rm, target_type_reg,
			FE_MEM(target_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, probe_reg, target_type_reg);
		ASM(AND32ri, probe_reg, Z_TYPE_MASK);
		ASM(CMP32ri, probe_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV32rr, probe_reg, target_type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		auto target_checked = text_writer.label_create();
		generate_raw_jump(Jump::je, target_checked);
		/*
		 * GC_DTOR_NO_REF() must purple a shared collectable value.  Keep
		 * that transition in the semantic helper; strings and resources
		 * only need the refcount decrement performed here.
		 */
		ASM(MOV32rr, probe_reg, target_type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, low_word_reg,
			FE_MEM(target_slot_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, probe_reg,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))));
		ASM(CMP32ri, probe_reg, 1);
		generate_raw_jump(Jump::jle, slow);
		label_place(target_checked);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV64rr, probe_reg, frame_reg);
			ASM(ADD64ri, probe_reg, static_cast<int32_t>(result_offset));
			ASM(MOV32rm, probe_reg,
				FE_MEM(probe_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP32ri, probe_reg, IS_DOUBLE);
			generate_raw_jump(Jump::ja, slow);
		}
		ASM(MOV32rr, probe_reg, target_type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		auto target_released = text_writer.label_create();
		generate_raw_jump(Jump::je, target_released);
		ASM(MOV64rm, low_word_reg,
			FE_MEM(target_slot_reg, 0, FE_NOREG, 0));
		ASM(SUB32mi,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(target_released);
		ASM(MOV64rm, low_word_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG, 0));
		ASM(MOV32rr, probe_reg, source_type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		auto value_owned = text_writer.label_create();
		generate_raw_jump(Jump::je, value_owned);
		ASM(ADD32mi,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			(!move_source ? 1 : 0)
				+ (result_storage != ZEND_MIR_ID_INVALID ? 1 : 0));
		label_place(value_owned);
		ASM(MOV64mr, FE_MEM(target_slot_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV32mr,
			FE_MEM(target_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			source_type_reg);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV64rr, target_slot_reg, frame_reg);
			ASM(ADD64ri, target_slot_reg, static_cast<int32_t>(result_offset));
			ASM(MOV64mr,
				FE_MEM(target_slot_reg, 0, FE_NOREG, 0), low_word_reg);
			ASM(MOV32mr,
				FE_MEM(target_slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
				source_type_reg);
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
		probe.reset();
		ValuePart frame_argument{tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(slow_helper, &frame_argument)) {
			return false;
		}
		label_place(done);
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
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_COPY_TMP);
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
		if (source_offset > INT32_MAX || result_offset > INT32_MAX) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_COPY_TMP);
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		ScratchReg source_slot{this};
		ScratchReg result_slot{this};
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg probe{this};
		auto source_slot_reg = source_slot.alloc_gp();
		auto result_slot_reg = result_slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto probe_reg = probe.alloc_gp();

		ASM(MOV64rr, source_slot_reg, frame_reg);
		ASM(ADD64ri, source_slot_reg, static_cast<int32_t>(source_offset));
		ASM(MOV64rr, result_slot_reg, frame_reg);
		ASM(ADD64ri, result_slot_reg, static_cast<int32_t>(result_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV64rm, value_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG, 0));
		ASM(MOV32rr, probe_reg, type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		auto copied = text_writer.label_create();
		generate_raw_jump(Jump::je, copied);
		ASM(ADD32mi,
			FE_MEM(value_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(copied);
		ASM(MOV64mr,
			FE_MEM(result_slot_reg, 0, FE_NOREG, 0), value_reg);
		ASM(MOV32mr,
			FE_MEM(result_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
		return true;
	};
	auto free_temporary_slot = [&]() {
		const zend_mir_storage_id source_storage =
			mir.value_operation.op1_storage_id;
		if (source_storage == ZEND_MIR_ID_INVALID) {
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FREE);
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		if (source_offset > INT32_MAX) {
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FREE);
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
		auto released = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg source_slot{this};
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg probe{this};
		auto source_slot_reg = source_slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto probe_reg = probe.alloc_gp();

		ASM(MOV64rr, source_slot_reg, frame_reg);
		ASM(ADD64ri, source_slot_reg, static_cast<int32_t>(source_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, probe_reg, type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		generate_raw_jump(Jump::je, released);
		ASM(MOV64rm, value_reg,
			FE_MEM(source_slot_reg, 0, FE_NOREG, 0));
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
			FE_MEM(source_slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
		auto done = text_writer.label_create();
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		source_slot.reset();
		type.reset();
		value.reset();
		probe.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_FREE, &frame_argument)) {
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
	auto string_length = [&]() {
		zend_tpde_string_length layout;

		if (!zend_tpde_string_length_at(mir, &layout)
				|| layout.operand_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_UNARY_OP);
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
		ScratchReg string{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto string_reg = string.alloc_gp();

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.operand_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, string_reg,
			FE_MEM(slot_reg, 0, FE_NOREG, 0));

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.result_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, type_reg, IS_UNDEF);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, string_reg,
			FE_MEM(string_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, len))));
		ASM(MOV64mr,
			FE_MEM(slot_reg, 0, FE_NOREG, 0), string_reg);
		ASM(MOV32mi,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			IS_LONG);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		string.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_UNARY_OP, &frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};
	auto slot_isset_empty = [&]() {
		zend_tpde_slot_isset_empty layout;

		if (!zend_tpde_slot_isset_empty_at(mir, &layout)
				|| layout.operand_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV);
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
		auto truthy = text_writer.label_create();
		auto falsey = text_writer.label_create();
		auto store = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg slot{this};
		ScratchReg type{this};
		ScratchReg value{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();

		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.operand_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(TEST64rr, value_reg, value_reg);
			generate_raw_jump(Jump::jne, truthy);
			generate_raw_jump(Jump::jmp, falsey);

			label_place(not_long);
			ASM(CMP32ri, type_reg, IS_STRING);
			auto not_string = text_writer.label_create();
			generate_raw_jump(Jump::jne, not_string);
			ASM(MOV64rm, value_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(MOV64rm, slot_reg,
				FE_MEM(value_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_string, len))));
			ASM(TEST64rr, slot_reg, slot_reg);
			generate_raw_jump(Jump::je, falsey);
			ASM(CMP64ri, slot_reg, 1);
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
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
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
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
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

		label_place(truthy);
		ASM(MOV32ri, type_reg,
			layout.is_empty ? IS_FALSE : IS_TRUE);
		generate_raw_jump(Jump::jmp, store);
		label_place(falsey);
		ASM(MOV32ri, type_reg,
			layout.is_empty ? IS_TRUE : IS_FALSE);
		label_place(store);
		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(layout.result_offset));
		ASM(MOV32mr,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		value.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_VALUE_ISSET_ISEMPTY_CV,
				&frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};
	auto object_property_read = [&]() {
		zend_tpde_object_property_read layout;

		if (!zend_tpde_object_property_read_at(mir, &layout)
				|| layout.receiver_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX
				|| layout.cache_offset > INT32_MAX - 3 * sizeof(void *)) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_OBJECT_FETCH_R);
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
		auto copied = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg receiver{this};
		ScratchReg object{this};
		ScratchReg cache{this};
		ScratchReg offset{this};
		ScratchReg property{this};
		ScratchReg type{this};
		ScratchReg low_word{this};
		auto receiver_reg = receiver.alloc_gp();
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();

		ASM(MOV64rr, receiver_reg, frame_reg);
		ASM(ADD64ri, receiver_reg,
			static_cast<int32_t>(layout.receiver_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_OBJECT);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, object_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG, 0));
		ASM(MOV64rm, cache_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_execute_data, run_time_cache))));
		ASM(TEST64rr, cache_reg, cache_reg);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rm, type_reg,
			FE_MEM(object_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_object, ce))));
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
		ASM(MOV64rr, property_reg, object_reg);
		ASM(ADD64rr, property_reg, offset_reg);
		ASM(MOV32rm, type_reg,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, offset_reg, type_reg);
		ASM(AND32ri, offset_reg, Z_TYPE_MASK);
		ASM(CMP32ri, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);
		ASM(CMP32ri, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);

		ASM(MOV64rr, receiver_reg, frame_reg);
		ASM(ADD64ri, receiver_reg,
			static_cast<int32_t>(layout.result_offset));
		ASM(MOV32rm, offset_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(CMP32ri, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, low_word_reg,
			FE_MEM(property_reg, 0, FE_NOREG, 0));
		ASM(MOV64mr,
			FE_MEM(receiver_reg, 0, FE_NOREG, 0), low_word_reg);
		ASM(MOV32mr,
			FE_MEM(receiver_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))),
			type_reg);
		ASM(AND32ri, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, copied);
		ASM(ADD32mi,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(copied);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		receiver.reset();
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		low_word.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_OBJECT_FETCH_R, &frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};
	auto object_property_write = [&]() {
		zend_tpde_object_property_write layout;

		if (!zend_tpde_object_property_write_at(mir, &layout)
				|| layout.receiver_offset > INT32_MAX
				|| layout.value_offset > INT32_MAX
				|| layout.cache_offset > INT32_MAX - 3 * sizeof(void *)) {
			return execute_value_operation(
				ZEND_NATIVE_HELPER_OBJECT_ASSIGN);
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
		auto old_released = text_writer.label_create();
		auto value_owned = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg receiver{this};
		ScratchReg object{this};
		ScratchReg cache{this};
		ScratchReg offset{this};
		ScratchReg property{this};
		ScratchReg type{this};
		ScratchReg low_word{this};
		auto receiver_reg = receiver.alloc_gp();
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();

		ASM(MOV64rr, receiver_reg, frame_reg);
		ASM(ADD64ri, receiver_reg,
			static_cast<int32_t>(layout.receiver_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_OBJECT);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, object_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, receiver_reg,
			FE_MEM(object_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_object, extra_flags))));
		ASM(TEST32ri, receiver_reg,
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
		ASM(MOV64rm, receiver_reg,
			FE_MEM(type_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_class_entry, create_object))));
		ASM(TEST64rr, receiver_reg, receiver_reg);
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
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rr, property_reg, object_reg);
		ASM(ADD64rr, property_reg, offset_reg);
		ASM(MOV32rm, type_reg,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, offset_reg, type_reg);
		ASM(AND32ri, offset_reg, Z_TYPE_MASK);
		ASM(CMP32ri, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);
		ASM(CMP32ri, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);

		ASM(MOV64rr, receiver_reg, frame_reg);
		ASM(ADD64ri, receiver_reg,
			static_cast<int32_t>(layout.value_offset));
		ASM(MOV32rm, type_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, offset_reg, type_reg);
		ASM(AND32ri, offset_reg, Z_TYPE_MASK);
		ASM(CMP32ri, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);

		ASM(MOV32rm, offset_reg,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(AND32ri, offset_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, offset_reg, offset_reg);
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

		ASM(MOV64rm, low_word_reg,
			FE_MEM(receiver_reg, 0, FE_NOREG, 0));
		if (!layout.move_value) {
			ASM(MOV32rr, offset_reg, type_reg);
			ASM(AND32ri, offset_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			ASM(TEST32rr, offset_reg, offset_reg);
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
				FE_MEM(receiver_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))),
				IS_UNDEF);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		receiver.reset();
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		low_word.reset();
		ValuePart frame_argument{
			tpde::x64::PlatformConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(
				ZEND_NATIVE_HELPER_OBJECT_ASSIGN, &frame_argument)) {
			return false;
		}
		label_place(done);
		return true;
	};

	if ((record.opcode >= ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS
				&& record.opcode
					<= ZEND_MIR_OPCODE_OBJECT_DECLARE_CLASS_DELAYED)
			|| (record.opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R
				&& record.opcode
					<= ZEND_MIR_OPCODE_DYNAMIC_INCLUDE_OR_EVAL)
			|| record.opcode == ZEND_MIR_OPCODE_VALUE_TYPE_CHECK
			|| record.opcode == ZEND_MIR_OPCODE_CALL_FRAMELESS_INTERNAL
			|| record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_CLASS_NAME) {
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R) {
			return object_property_read();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN) {
			return object_property_write();
		}
		zend_native_runtime_helper_id helper;
		if (record.opcode >= ZEND_MIR_OPCODE_DYNAMIC_FETCH_R) {
			helper = static_cast<zend_native_runtime_helper_id>(
				static_cast<uint32_t>(record.opcode)
					- static_cast<uint32_t>(ZEND_MIR_OPCODE_DYNAMIC_FETCH_R)
					+ static_cast<uint32_t>(ZEND_NATIVE_HELPER_DYNAMIC_FETCH_R));
		} else {
			helper = static_cast<zend_native_runtime_helper_id>(
				static_cast<uint32_t>(record.opcode)
					- static_cast<uint32_t>(
						ZEND_MIR_OPCODE_OBJECT_DECLARE_ANON_CLASS)
					+ static_cast<uint32_t>(
						ZEND_NATIVE_HELPER_OBJECT_DECLARE_ANON_CLASS));
		}
		return execute_value_operation(helper);
	}
	switch (record.opcode) {
		case ZEND_MIR_OPCODE_VALUE_MAKE_REF:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_MAKE_REF);
		case ZEND_MIR_OPCODE_VALUE_ASSIGN_REF:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN_REF);
		case ZEND_MIR_OPCODE_VALUE_SEPARATE:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_SEPARATE);
		case ZEND_MIR_OPCODE_VALUE_COPY_TMP:
			return copy_temporary_slot();
		case ZEND_MIR_OPCODE_VALUE_FREE:
			return free_temporary_slot();
		case ZEND_MIR_OPCODE_VALUE_UNSET_CV:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_UNSET_CV);
		case ZEND_MIR_OPCODE_VALUE_CHECK_VAR:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_CHECK_VAR);
		case ZEND_MIR_OPCODE_VALUE_ASSIGN:
			if (mir.value_operation.op1.slot_kind
					!= ZEND_MIR_SOURCE_SLOT_CV) {
				return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ASSIGN);
			}
			return copy_slot(
				mir.value_operation.op2,
				mir.value_operation.op2_storage_id,
				mir.value_operation.op1_storage_id,
				mir.value_operation.result_storage_id,
				mir.value_operation.op2.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP,
				ZEND_NATIVE_HELPER_VALUE_ASSIGN);
		case ZEND_MIR_OPCODE_VALUE_QM_ASSIGN:
			return copy_slot(
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
			return string_length();
		case ZEND_MIR_OPCODE_VALUE_CAST:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_CAST);
		case ZEND_MIR_OPCODE_VALUE_ISSET_ISEMPTY_CV:
			return slot_isset_empty();
		case ZEND_MIR_OPCODE_VALUE_FETCH_LIST:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_FETCH_LIST);
		case ZEND_MIR_OPCODE_VALUE_INCDEC:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_INCDEC);
		case ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE:
			return execute_value_operation(
				ZEND_NATIVE_HELPER_VERIFY_RETURN_TYPE);
		case ZEND_MIR_OPCODE_VALUE_ECHO:
			return execute_value_operation(ZEND_NATIVE_HELPER_VALUE_ECHO);
		case ZEND_MIR_OPCODE_COPY:
		case ZEND_MIR_OPCODE_CANONICALIZE:
		case ZEND_MIR_OPCODE_I1_TO_I64:
			return copy_result();
		case ZEND_MIR_OPCODE_STATEPOINT:
			if ((record.effects & ZEND_MIR_EFFECT_MASK(
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
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_INTERRUPT_POLL));
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
			generate_raw_set(record.opcode == ZEND_MIR_OPCODE_I1_NOT
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
			if (record.opcode == ZEND_MIR_OPCODE_I64_SHL_CHECKED) {
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
				adaptor->block_ref(record.block_id))[0]);
			return true;
		case ZEND_MIR_OPCODE_COND_BRANCH: {
			auto [condition_ref, condition] = unary();
			auto condition_reg = condition.load_to_reg();
			ASM(TEST64rr, condition_reg, condition_reg);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
			generate_cond_branch(Jump::jne, successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
		case ZEND_MIR_OPCODE_ITERATOR_BRANCH: {
			if (node.operands.size() != 1 || !mir.has_value_operation) {
				return false;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
				zend_tpde_value_condition layout;

				if (zend_tpde_value_condition_at(mir, &layout)
						&& layout.operand_offset <= INT32_MAX) {
					for (auto reg_id : register_file.used_regs()) {
						tpde::Reg reg{reg_id};
						if (!register_file.is_fixed(reg)
								&& register_file.reg_local_idx(reg)
									!= INVALID_VAL_LOCAL_IDX) {
							evict_reg(reg);
						}
					}
					auto slow = text_writer.label_create();
					auto truthy = text_writer.label_create();
					auto falsey = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = std::move(frame).into_scratch();
					auto frame_reg = frame_scratch.cur_reg();
					ScratchReg slot{this};
					ScratchReg type{this};
					ScratchReg value{this};
					auto slot_reg = slot.alloc_gp();
					auto type_reg = type.alloc_gp();
					auto value_reg = value.alloc_gp();

					ASM(MOV64rr, slot_reg, frame_reg);
					ASM(ADD64ri, slot_reg,
						static_cast<int32_t>(layout.operand_offset));
					ASM(MOV32rm, type_reg,
						FE_MEM(slot_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
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
					ASM(MOV64rm, value_reg,
						FE_MEM(slot_reg, 0, FE_NOREG, 0));
					ASM(TEST64rr, value_reg, value_reg);
					generate_raw_jump(Jump::jne, truthy);
					generate_raw_jump(Jump::jmp, falsey);

					label_place(not_long);
					ASM(CMP32ri, type_reg, IS_STRING);
					auto not_string = text_writer.label_create();
					generate_raw_jump(Jump::jne, not_string);
					ASM(MOV64rm, value_reg,
						FE_MEM(slot_reg, 0, FE_NOREG, 0));
					ASM(MOV64rm, slot_reg,
						FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_string, len))));
					ASM(TEST64rr, slot_reg, slot_reg);
					generate_raw_jump(Jump::je, falsey);
					ASM(CMP64ri, slot_reg, 1);
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
						FE_MEM(slot_reg, 0, FE_NOREG, 0));
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
					ASM(MOV64rm, value_reg,
						FE_MEM(slot_reg, 0, FE_NOREG, 0));
					ASM(MOV32rm, type_reg,
						FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_resource, handle))));
					ASM(TEST32rr, type_reg, type_reg);
					generate_raw_jump(Jump::jne, truthy);
					generate_raw_jump(Jump::jmp, falsey);

					slot.reset();
					type.reset();
					value.reset();
					const auto &successors = adaptor->block_succs(
						adaptor->block_ref(record.block_id));
					label_place(slow);

					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, std::move(frame_scratch));
					builder.add_arg(
						std::move(frame_argument), tpde::CCAssignment{});
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
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_VALUE_COND_BRANCH));
					ValuePart decision{
						tpde::x64::PlatformConfig::GP_BANK};
					builder.add_ret(decision, tpde::CCAssignment{});
					auto decision_reg =
						decision.cur_reg_or_load(this);
					ASM(CMP32ri, decision_reg,
						ZEND_NATIVE_ITERATOR_EXCEPTION);
					auto valid = text_writer.label_create();
					generate_raw_jump(Jump::jl, valid);
					decision.reset(this);
					RetBuilder return_builder{
						*this, *cur_cc_assigner()};
					return_builder.add(ValuePart{
						ZEND_NATIVE_EXCEPTION, 4,
						tpde::x64::PlatformConfig::GP_BANK},
						tpde::CCAssignment{});
					return_builder.ret();
					label_place(valid);
					generate_raw_jump(Jump::jmp, branch);
					label_place(truthy);
					ASM(MOV32ri, decision_reg, 1);
					generate_raw_jump(Jump::jmp, branch);
					label_place(falsey);
					ASM(MOV32ri, decision_reg, 0);
					label_place(branch);
					ASM(TEST32rr, decision_reg, decision_reg);
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
			const auto helper = record.opcode
				== ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				? ZEND_NATIVE_HELPER_VALUE_COND_BRANCH
				: ZEND_NATIVE_HELPER_VALUE_ITERATOR_BRANCH;
			builder.call(runtime_symbol(helper));
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
				adaptor->block_ref(record.block_id));
			generate_cond_branch(Jump::jne, successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_CALL_DIRECT_USER: {
			const zend_tpde_instruction &call =
				adaptor->mir_instruction(instruction);
			if (call.direct_call != nullptr) {
				const bool generated_fast_path =
					(call.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0;
				const bool result_unused =
					call.direct_call->result_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED;
				const uint32_t argument_count = call.call_argument_count;
				const uint32_t frame_operand =
					generated_fast_path ? argument_count : 0;
				const uint32_t frame_use_count =
					generated_fast_path ? 6 + node.has_result : 2;
				const uint32_t context_operand = frame_operand
					+ frame_use_count;
				auto slow_path = text_writer.label_create();
				auto successful = text_writer.label_create();
				if (generated_fast_path) {
					const uint32_t activation_size = static_cast<uint32_t>(
						(sizeof(zend_native_direct_activation)
							+ sizeof(zval) - 1) / sizeof(zval) * sizeof(zval));
					const uint64_t reservation_size =
						static_cast<uint64_t>(call.direct_call->frame_size)
							+ activation_size;
					if (reservation_size > INT32_MAX) {
						return false;
					}
					for (auto reg_id : register_file.used_regs()) {
						tpde::Reg reg{reg_id};
						if (!register_file.is_fixed(reg)
								&& register_file.reg_local_idx(reg)
									!= INVALID_VAL_LOCAL_IDX) {
							evict_reg(reg);
						}
					}
					auto [frame_ref, frame] =
						val_ref_single(node.operands[frame_operand]);
					auto frame_scratch = std::move(frame).into_scratch();
					auto frame_reg = frame_scratch.cur_reg();
					auto [context_ref, context] =
						val_ref_single(node.operands[context_operand]);
					auto context_scratch = std::move(context).into_scratch();
					auto context_reg = context_scratch.cur_reg();
					auto cell_value = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site.target_id);
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
					auto first_reg = first.alloc_gp();
					auto second_reg = second.alloc_gp();

					ASM(CMP32mi,
						FE_MEM(cell_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_entry_cell, state))),
						ZEND_NATIVE_ENTRY_READY);
					generate_raw_jump(Jump::jne, slow_path);
					ASM(MOV64rm, first_reg,
						FE_MEM(cell_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_entry_cell, function))));
					ASM(MOV64rm, second_reg,
						FE_MEM(descriptor_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_call_descriptor,
								expected_function))));
					ASM(CMP64rr, first_reg, second_reg);
					generate_raw_jump(Jump::jne, slow_path);
					ASM(MOV64rm, second_reg,
						FE_MEM(cell_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_entry_cell, code))));
					ASM(TEST64rr, second_reg, second_reg);
					generate_raw_jump(Jump::je, slow_path);
					ASM(CMP8mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_code, executable))),
						1);
					generate_raw_jump(Jump::jne, slow_path);
					ASM(MOV64rm, first_reg,
						FE_MEM(cell_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_entry_cell, frame_probe))));
					ASM(TEST64rr, first_reg, first_reg);
					generate_raw_jump(Jump::jne, slow_path);
					ASM(CMP8mi,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								observers_enabled))),
						0);
					generate_raw_jump(Jump::jne, slow_path);
					ASM(MOV64rm, first_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))));
					ASM(TEST64rr, first_reg, first_reg);
					generate_raw_jump(Jump::jne, slow_path);

					/*
					 * A boxed CV can be copied inline while it is a defined,
					 * non-reference zval. References require ZVAL_COPY_DEREF
					 * semantics and remain on the canonical slow path. Guard
					 * every boxed source before publishing or reserving a frame.
					 */
					for (uint32_t index = 0; index < argument_count; ++index) {
						const zend_native_direct_call_argument &argument =
							call.direct_call->arguments[index];
						if (zend_mir_scalar_type_is_exact(
								argument.exact_type)) {
							continue;
						}
						const int32_t source_offset =
							static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ argument.source_operand.index)
								* sizeof(zval)
								+ offsetof(zval, u1.type_info));
						ASM(MOV32rm, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG, source_offset));
						ASM(AND32ri, first_reg, Z_TYPE_MASK);
						ASM(CMP32ri, first_reg, IS_UNDEF);
						generate_raw_jump(Jump::je, slow_path);
						ASM(CMP32ri, first_reg, IS_REFERENCE);
						generate_raw_jump(Jump::je, slow_path);
					}

					/*
					 * Keep recursive Native calls on Zend's C-stack safety
					 * contract.  The slow path owns the canonical overflow
					 * error and bailout; successful calls stay helper-free.
					 */
					ASM(MOV64rm, first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								stack_limit))));
					{
						auto stack_guarded = text_writer.label_create();
						ASM(TEST64rr, first_reg, first_reg);
						generate_raw_jump(Jump::je, stack_guarded);
						ASM(MOV64rm, first_reg,
							FE_MEM(first_reg, 0, FE_NOREG, 0));
						ASM(MOV64rr, second_reg, FE_SP);
						ASM(CMP64rr, second_reg, first_reg);
						generate_raw_jump(Jump::jbe, slow_path);
						label_place(stack_guarded);
					}

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
					generate_raw_jump(Jump::jb, slow_path);

					ScratchReg callee_address{this};
					auto callee_reg = callee_address.alloc_gp();
					ASM(MOV64rr, callee_reg, first_reg);
					ASM(MOV64rr, second_reg, callee_reg);
					ASM(ADD64ri, second_reg,
						static_cast<int32_t>(reservation_size));
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
						ASM(MOV64rm, function_reg,
							FE_MEM(cell_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_entry_cell, function))));
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
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, run_time_cache))),
						0);
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, extra_named_params))),
						0);
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, This))),
						0);
					ASM(MOV32mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, This)
								+ offsetof(zval, u1.type_info))),
						ZEND_CALL_NESTED_FUNCTION);
					ASM(MOV32mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, This)
								+ offsetof(zval, u2.num_args))),
						static_cast<int32_t>(argument_count));

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
					if (argument_count != 0) {
						ASM(ADD64ri, second_reg,
							static_cast<int32_t>(
								argument_count * sizeof(zend_op)));
					}
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, opline))),
						second_reg);

					/* Resolve the caller's canonical result zval. */
					if (result_unused) {
						ASM(MOV64rr, second_reg, callee_reg);
						ASM(ADD64ri, second_reg, static_cast<int32_t>(
							call.direct_call->frame_size
								+ offsetof(zend_native_direct_activation,
									discarded_return)));
					} else {
						ASM(MOV64rr, second_reg, frame_reg);
						if (call.direct_call->result_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV) {
							ASM(ADD64ri, second_reg, static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ call.direct_call->result_operand.index)
								* sizeof(zval)));
						} else {
							ScratchReg slot{this};
							auto slot_reg = slot.alloc_gp();
							ASM(MOV64rm, slot_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_execute_data, func))));
							ASM(MOV32rm, slot_reg,
								FE_MEM(slot_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_op_array, last_var))));
							ASM(ADD64ri, slot_reg, static_cast<int32_t>(
								ZEND_CALL_FRAME_SLOT
									+ call.direct_call->result_operand.index));
							ASM(SHL64ri, slot_reg, 4);
							ASM(ADD64rr, second_reg, slot_reg);
						}
					}
					ASM(MOV64mr,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, return_value))),
						second_reg);
					ASM(MOV32mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))),
						IS_UNDEF);

					for (uint32_t index = 0; index < argument_count; ++index) {
						auto [argument_ref, argument] =
							val_ref_single(node.operands[index]);
						const int32_t offset = static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
						const zend_native_direct_call_argument &descriptor_argument =
							call.direct_call->arguments[index];
						if (zend_mir_scalar_type_is_exact(
								descriptor_argument.exact_type)) {
							auto argument_reg = argument.load_to_reg();
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
								FE_MEM(callee_reg, 0, FE_NOREG, offset + 8), 0);
							const uint32_t type =
								zval_type(*adaptor, node.operands[index]);
							if (type == IS_FALSE) {
								ScratchReg kind{this};
								auto kind_reg = kind.alloc_gp();
								mov(kind_reg, argument_reg, 8);
								ASM(ADD64ri, kind_reg, IS_FALSE);
								ASM(MOV32mr,
									FE_MEM(callee_reg, 0, FE_NOREG, offset + 8),
									kind_reg);
							} else {
								ASM(MOV32mi,
									FE_MEM(callee_reg, 0, FE_NOREG, offset + 8),
									static_cast<int32_t>(type));
							}
						} else {
							auto source_frame_reg = argument.load_to_reg();
							const int32_t source_offset =
								static_cast<int32_t>(
									(ZEND_CALL_FRAME_SLOT
										+ descriptor_argument.source_operand.index)
									* sizeof(zval));
							ScratchReg source_address{this};
							ScratchReg low_word{this};
							ScratchReg high_word{this};
							ScratchReg type_info{this};
							auto source_address_reg =
								source_address.alloc_gp();
							auto low_word_reg = low_word.alloc_gp();
							auto high_word_reg = high_word.alloc_gp();
							auto type_info_reg = type_info.alloc_gp();
							ASM(MOV64rr, source_address_reg, source_frame_reg);
							ASM(ADD64ri, source_address_reg, source_offset);
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
							ASM(MOV32rm, type_info_reg,
								FE_MEM(source_address_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zval, u1.type_info))));
							ASM(AND32ri, type_info_reg,
								IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
							ASM(TEST32rr, type_info_reg, type_info_reg);
							auto copied = text_writer.label_create();
							generate_raw_jump(Jump::je, copied);
							ASM(ADD32mi,
								FE_MEM(low_word_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							label_place(copied);
						}
					}

					/* Link bailout metadata after the frame. */
					ASM(MOV64rr, second_reg, callee_reg);
					ASM(ADD64ri, second_reg,
						static_cast<int32_t>(call.direct_call->frame_size));
					ASM(MOV64mr,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, caller))),
						frame_reg);
					ASM(MOV64mr,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, callee))),
						callee_reg);
					ASM(MOV64mr,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, cell))),
						cell_reg);
					ASM(MOV64mr,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, descriptor))),
						descriptor_reg);
					ASM(MOV64rm, first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								active_direct_call))));
					ASM(MOV64rm, descriptor_reg,
						FE_MEM(first_reg, 0, FE_NOREG, 0));
					ASM(MOV64mr,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, previous))),
						descriptor_reg);
					ASM(MOV64mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								discarded_return))),
						0);
					ASM(MOV64mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								discarded_return) + 8)),
						0);
					ASM(MOV32mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								discarded_return)
								+ offsetof(zval, u1.type_info))),
						IS_UNDEF);
					ASM(MOV64mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, status))),
						0);
					ASM(MOV8mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								uses_discarded_return))),
						result_unused ? 1 : 0);
					ASM(MOV8mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								raw_arguments_owned))),
						0);
					ASM(MOV8mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								frame_initialized))),
						1);
					ASM(MOV8mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								frame_requires_finish))),
						1);
					ASM(MOV8mi,
						FE_MEM(second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								cell_active))),
						1);
					ASM(MOV64mr,
						FE_MEM(first_reg, 0, FE_NOREG, 0), second_reg);
					ASM(MOV64rm, first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								current_execute_data))));
					ASM(MOV64mr,
						FE_MEM(first_reg, 0, FE_NOREG, 0), callee_reg);
					ASM(ADD32mi,
						FE_MEM(cell_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_entry_cell, active_calls))),
						1);

					/* Load the published entry and call it directly. */
					first.reset();
					second.reset();
					ValuePart callee_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					callee_value.set_value(
						this, std::move(callee_address));
					ScratchReg entry_argument{this};
					auto entry_argument_reg = entry_argument.alloc_gp();
					ASM(MOV64rm, entry_argument_reg,
						FE_MEM(cell_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_entry_cell, code))));
					ASM(MOV64rm, entry_argument_reg,
						FE_MEM(entry_argument_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_native_code, entry))));
					ValuePart entry_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					entry_value.set_value(this, std::move(entry_argument));
					frame_scratch.reset();
					context_scratch.reset();
					cell_scratch.reset();
					descriptor_scratch.reset();
					tpde::x64::CCAssignerSysV fast_assigner{false};
					CallBuilder fast_builder{*this, fast_assigner};
					fast_builder.add_arg(
						std::move(callee_value), tpde::CCAssignment{});
					fast_builder.add_arg(
						CallArg{node.operands[context_operand + 1]});
					fast_builder.call(std::move(entry_value));
					ValuePart fast_status{
						tpde::x64::PlatformConfig::GP_BANK, 4};
					fast_builder.add_ret(fast_status, tpde::CCAssignment{});

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
					ASM(MOV64rr, activation_reg, post_callee_reg);
					ASM(ADD64ri, activation_reg,
						static_cast<int32_t>(call.direct_call->frame_size));
					ASM(MOV32mr,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, status))),
						fast_status.cur_reg_or_load(this));
					fast_status.reset(this);
					auto complete_fast = text_writer.label_create();
					ASM(CMP32mi,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, status))),
						ZEND_NATIVE_RETURNED);
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
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_execute_data, return_value))));
					if (result_unused) {
						ASM(CMP32mi, FE_MEM(probe_reg, 0, FE_NOREG, 8),
							IS_DOUBLE);
						generate_raw_jump(Jump::ja, complete_fast);
					} else if (call.direct_call->result_type
							== ZEND_MIR_SCALAR_TYPE_NONE) {
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
								*adaptor, node.result)));
						generate_raw_jump(Jump::jne, complete_fast);
					}

					/*
					 * Mirror zend_free_compiled_variables() without a helper for
					 * shareable argument CVs. Check the whole frame first so a
					 * later complex destructor cannot observe partially released
					 * arguments on the complete_fast path.
					 */
					{
						ScratchReg counted{this};
						auto counted_reg = counted.alloc_gp();
						for (uint32_t index = 0;
								index < argument_count; ++index) {
							const int32_t offset = static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
							ASM(MOV32rm, probe_reg,
								FE_MEM(post_callee_reg, 0, FE_NOREG,
									offset + static_cast<int32_t>(
										offsetof(zval, u1.type_info))));
							ASM(AND32ri, probe_reg,
								IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
							ASM(TEST32rr, probe_reg, probe_reg);
							auto checked = text_writer.label_create();
							generate_raw_jump(Jump::je, checked);
							ASM(MOV64rm, counted_reg,
								FE_MEM(post_callee_reg, 0, FE_NOREG, offset));
							ASM(CMP32mi,
								FE_MEM(counted_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							generate_raw_jump(Jump::je, complete_fast);
							label_place(checked);
						}
						for (uint32_t index = 0;
								index < argument_count; ++index) {
							const int32_t offset = static_cast<int32_t>(
								(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
							ASM(MOV32rm, probe_reg,
								FE_MEM(post_callee_reg, 0, FE_NOREG,
									offset + static_cast<int32_t>(
										offsetof(zval, u1.type_info))));
							ASM(AND32ri, probe_reg,
								IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
							ASM(TEST32rr, probe_reg, probe_reg);
							auto released = text_writer.label_create();
							generate_raw_jump(Jump::je, released);
							ASM(MOV64rm, counted_reg,
								FE_MEM(post_callee_reg, 0, FE_NOREG, offset));
							ASM(SUB32mi,
								FE_MEM(counted_reg, 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_refcounted_h, refcount))),
								1);
							label_place(released);
						}
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
					auto fast_cell = image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site.target_id);
					auto fast_cell_scratch =
						std::move(fast_cell).into_scratch(this);
					ASM(SUB32mi,
						FE_MEM(fast_cell_scratch.cur_reg(), 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_entry_cell, active_calls))),
						1);
					ASM(MOV64mi,
						FE_MEM(post_frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_execute_data, call))),
						0);
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_top))));
					ASM(MOV64mr,
						FE_MEM(probe_reg, 0, FE_NOREG, 0), post_callee_reg);
					post_frame_scratch.reset();
					post_context_scratch.reset();
					post_callee.reset();
					activation.reset();
					probe.reset();
					fast_cell_scratch.reset();
					generate_raw_jump(Jump::jmp, successful);

					/* Rare completion retains full exception/interrupt cleanup. */
					label_place(complete_fast);
					post_frame_scratch.reset();
					post_context_scratch.reset();
					post_callee.reset();
					activation.reset();
					probe.reset();
					tpde::x64::CCAssignerSysV finish_assigner{false};
					CallBuilder finish_builder{*this, finish_assigner};
					finish_builder.add_arg(
						CallArg{node.operands[frame_operand + 3]});
					finish_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
					finish_builder.add_arg(
						CallArg{node.operands[context_operand + 3]});
					{
						auto [finish_frame_ref, finish_frame] =
							val_ref_single(node.operands[frame_operand + 2]);
						auto finish_frame_reg = finish_frame.load_to_reg();
						ScratchReg finish_activation{this};
						auto finish_activation_reg =
							finish_activation.alloc_gp();
						ASM(MOV64rm, finish_activation_reg,
							FE_MEM(finish_frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, call))));
						ASM(ADD64ri, finish_activation_reg,
							static_cast<int32_t>(
								call.direct_call->frame_size));
						ASM(MOV32rm, finish_activation_reg,
							FE_MEM(finish_activation_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_direct_activation, status))));
						finish_frame.reset();
						ValuePart finish_status_argument{
							tpde::x64::PlatformConfig::GP_BANK, 4};
						finish_status_argument.set_value(
							this, std::move(finish_activation));
						finish_builder.add_arg(
							std::move(finish_status_argument),
							tpde::CCAssignment{});
					}
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
					auto finish_status_reg =
						finish_status.cur_reg_or_load(this);
					ASM(CMP32ri, finish_status_reg, ZEND_NATIVE_RETURNED);
					auto finish_returned = text_writer.label_create();
					generate_raw_jump(Jump::je, finish_returned);
					if (zend_mir_id_is_valid(call.exception_block_id)) {
						auto propagate = text_writer.label_create();
						ASM(CMP32ri, finish_status_reg,
							ZEND_NATIVE_EXCEPTION);
						generate_raw_jump(Jump::jne, propagate);
						generate_exception_branch(
							adaptor->block_ref(call.exception_block_id));
						label_place(propagate);
					}
					{
						RetBuilder return_builder{
							*this, *cur_cc_assigner()};
						return_builder.add(
							std::move(finish_status), tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(finish_returned);
					finish_status.reset(this);
					generate_raw_jump(Jump::jmp, successful);
					label_place(slow_path);
				}
				ValuePart callee{tpde::x64::PlatformConfig::GP_BANK};
				ValuePart entry{tpde::x64::PlatformConfig::GP_BANK};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(CallArg{
						node.operands[frame_operand
							+ (generated_fast_path ? 4 : 0)]});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site.target_id), tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
					builder.add_arg(CallArg{
						node.operands[context_operand
							+ (generated_fast_path ? 4 : 0)]});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DIRECT_USER_CALL_ENTER));
					builder.add_ret(callee, tpde::CCAssignment{});
					builder.add_ret(entry, tpde::CCAssignment{});
				}
				ScratchReg entry_copy{this};
				auto entry_copy_reg =
					entry_copy.alloc_specific(tpde::x64::AsmReg::R11);
				mov(entry_copy_reg, entry.cur_reg_or_load(this), sizeof(void *));
				entry.reset(this);
				ValuePart entry_target{
					tpde::x64::PlatformConfig::GP_BANK};
				entry_target.set_value(this, std::move(entry_copy));
				ValuePart entry_status{tpde::x64::PlatformConfig::GP_BANK};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(std::move(callee), tpde::CCAssignment{});
					builder.add_arg(CallArg{
						node.operands[context_operand
							+ (generated_fast_path ? 5 : 1)]});
					builder.call(std::move(entry_target));
					builder.add_ret(entry_status, tpde::CCAssignment{});
				}
				ScratchReg entry_status_copy{this};
				auto entry_status_copy_reg =
					entry_status_copy.alloc_specific(tpde::x64::AsmReg::CX);
				mov(entry_status_copy_reg,
					entry_status.cur_reg_or_load(this), sizeof(uint64_t));
				entry_status.reset(this);
				ValuePart entry_status_argument{
					tpde::x64::PlatformConfig::GP_BANK};
				entry_status_argument.set_value(
					this, std::move(entry_status_copy));
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{
					node.operands[frame_operand
						+ (generated_fast_path ? 5 : 1)]});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
					call.id), tpde::CCAssignment{});
				builder.add_arg(CallArg{
					node.operands[context_operand
						+ (generated_fast_path ? 6 : 2)]});
				builder.add_arg(
					std::move(entry_status_argument), tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE));
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
				if (generated_fast_path) {
					payload.reset(this);
					generate_raw_jump(Jump::jmp, successful);
					label_place(successful);
					if (node.has_result) {
						auto [result_frame_ref, result_frame] =
							val_ref_single(node.operands[frame_operand + 6]);
						auto result_frame_reg = result_frame.load_to_reg();
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
							ScratchReg slot_index{this};
							auto slot_index_reg = slot_index.alloc_gp();
							ASM(MOV64rm, slot_index_reg,
								FE_MEM(result_frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_execute_data, func))));
							ASM(MOV32rm, slot_index_reg,
								FE_MEM(slot_index_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_op_array, last_var))));
							ASM(ADD64ri, slot_index_reg,
								static_cast<int32_t>(
									ZEND_CALL_FRAME_SLOT
										+ call.direct_call->result_operand.index));
							ASM(SHL64ri, slot_index_reg, 4);
							ASM(ADD64rr, result_slot_reg, slot_index_reg);
						}
						auto [result_ref, result] =
							result_ref_single(node.result);
						auto result_reg = result.alloc_reg();
						if (val_parts(node.result).bank
								== tpde::x64::PlatformConfig::FP_BANK) {
							ASM(SSE_MOVSDrm, result_reg,
								FE_MEM(result_slot_reg, 0, FE_NOREG, 0));
						} else {
							ASM(MOV64rm, result_reg,
								FE_MEM(result_slot_reg, 0, FE_NOREG, 0));
						}
						result.set_modified();
					}
				} else if (node.has_result) {
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
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					call.call_site.target_id), tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					source_arguments ? call.call_argument_count : call.operand_count, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_site.source_init_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_BEGIN));
			}
			for (uint32_t index = 0;
					index < (source_arguments
						? call.call_argument_count : call.operand_count); ++index) {
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				if (source_arguments) {
					zend_mir_call_argument_ref argument;
					if (!zend_tpde_call_argument_at(adaptor->plan(),
							call.call_argument_offset + index, &argument)) {
						return false;
					}
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
					builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT));
					continue;
				}
				IRValueRef operand = node.operands[index];
				builder.add_arg(ValuePart{index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(CallArg{operand});
				if (adaptor->exact_type(operand) == ZEND_MIR_SCALAR_TYPE_F64) {
					builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE));
				} else {
					if (!zend_mir_scalar_type_is_exact(adaptor->exact_type(operand))) {
						return false;
					}
					builder.add_arg(ValuePart{
						static_cast<uint32_t>(adaptor->exact_type(operand)), 4,
						tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
					builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER));
				}
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site.target_id), tpde::CCAssignment{});
			builder.add_arg(ValuePart{call.call_site.source_do_opline_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE));
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
				result_builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
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
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
					call.call_site.target_id), tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_argument_count, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.add_arg(ValuePart{call.call_site.source_init_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN));
			}
			for (uint32_t index = 0; index < call.call_argument_count; ++index) {
				zend_mir_call_argument_ref argument;
				if (!zend_tpde_call_argument_at(adaptor->plan(),
						call.call_argument_offset + index, &argument)) {
					return false;
				}
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
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT));
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site.target_id), tpde::CCAssignment{});
			builder.add_arg(ValuePart{call.call_site.source_do_opline_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE));
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
				result_builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
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
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_ENTER));
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
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_CALL));
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
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
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_RETURN));
			ValuePart continuation{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(continuation, tpde::CCAssignment{});
			auto continuation_reg = continuation.cur_reg_or_load(this);
			const zend_tpde_plan *plan = adaptor->plan();
			for (uint32_t i = 0; i < plan->instruction_count; ++i) {
				const zend_mir_instruction_record call =
					zend_tpde_instruction_record_at(
						plan, &plan->instructions[i]);
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
				const zend_mir_instruction_record handler =
					zend_tpde_instruction_record_at(
						plan, &plan->instructions[i]);
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
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CATCH_ENTER));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
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
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK};
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

struct X64ImageState {
	Adaptor adaptor;
	ZendCompilerX64 compiler;

	explicit X64ImageState(
		const zend_tpde_plan *plan, zend_native_image *image)
		: adaptor{plan}, compiler{&adaptor, image} {}
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
	auto state = std::make_unique<X64ImageState>(plan, image);
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
			[image](std::string_view name) -> void * {
				const void *address = nullptr;
				std::string stable_name{name};
				return zend_tpde_image_resolve_symbol(
						image, stable_name.c_str(), &address)
					? const_cast<void *>(address) : nullptr;
			})) {
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
