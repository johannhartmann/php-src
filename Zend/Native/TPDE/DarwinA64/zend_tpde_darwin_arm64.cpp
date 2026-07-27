// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/Common/zend_tpde_conditional_call.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_apple_a64_abi.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_encodegen_a64.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"

#include <tpde/ELF.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

using Adaptor = zend::native::tpde::ZendComponentIRAdaptor;
using IRValueRef = zend::native::tpde::IRValueRef;
using IRInstRef = zend::native::tpde::IRInstRef;
using IRBlockRef = zend::native::tpde::IRBlockRef;
using IRFuncRef = zend::native::tpde::IRFuncRef;
using DarwinConfig = zend::native::tpde::DarwinA64PlatformConfig;
using DarwinAssembler = zend::native::tpde::AssemblerDarwinA64;

class ZendCompilerA64 final
	: public ::tpde::a64::CompilerA64<Adaptor, ZendCompilerA64,
		::tpde::CompilerBase, DarwinConfig>,
	  public tpde_encodegen::EncodeCompiler<Adaptor, ZendCompilerA64,
		::tpde::CompilerBase, DarwinConfig> {
	using Base = ::tpde::a64::CompilerA64<Adaptor, ZendCompilerA64,
		::tpde::CompilerBase, DarwinConfig>;
	using EncodeBase = tpde_encodegen::EncodeCompiler<Adaptor, ZendCompilerA64,
		::tpde::CompilerBase, DarwinConfig>;
	zend_native_image *image_;
	std::vector<::tpde::SymRef> image_symbols_;
	std::vector<::tpde::SymRef> image_slots_;
	std::vector<::tpde::Label> generator_resume_labels_;
	std::vector<::tpde::Label> user_opcode_labels_;
	std::vector<::tpde::Label> user_opcode_dispatch_labels_;

public:
	struct ValRefSpecial {
		uint8_t mode = 4;
		uint8_t bank = 0;
		uint8_t padding[6]{};
		uint64_t bits = 0;
	};

	struct ValueParts {
		::tpde::RegBank bank;
		uint32_t part_count;
		uint32_t count() const { return part_count; }
		uint32_t size_bytes(uint32_t part) const {
			return part_count == 2 && part == 1 ? 4 : 8;
		}
		::tpde::RegBank reg_bank(uint32_t) const { return bank; }
	};

	explicit ZendCompilerA64(Adaptor *adaptor, zend_native_image *image)
		: Base{adaptor},
		  image_{image},
		  image_symbols_(image->symbol_count),
		  image_slots_(image->symbol_count) {}

	void reset() {
		Base::reset();
		EncodeBase::reset();
	}

	ValuePart image_symbol_value(
		zend_native_image_symbol_kind kind, uint32_t id) {
		const zend_native_image_symbol *symbol =
			zend_tpde_image_symbol_find(
				image_, kind, id,
				kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER
					? 0 : adaptor->current_function_index());
		if (symbol == nullptr) {
			return ValuePart{DarwinConfig::GP_BANK, 8};
		}
		const uint32_t index =
			static_cast<uint32_t>(symbol - image_->symbols);
		::tpde::SymRef &reference = image_symbols_[index];
		if (!reference.valid()) {
			reference = assembler.sym_add_undef(symbol->name,
				::tpde::Assembler::SymBinding::GLOBAL);
		}
		::tpde::SymRef &slot = image_slots_[index];
		if (!slot.valid()) {
			const std::array<::tpde::u8, sizeof(uintptr_t)> zero{};
			::tpde::SecRef section = assembler.get_default_section(
				::tpde::SectionKind::DataRelRO);
			uint32_t offset = 0;
			slot = assembler.sym_def_data(section, "", zero, alignof(uintptr_t),
				::tpde::Assembler::SymBinding::LOCAL, &offset);
			assembler.reloc_abs(section, reference, offset, 0);
		}
		ValuePart target{DarwinConfig::GP_BANK, 8};
		const auto target_reg = target.alloc_reg(this);
		text_writer.ensure_space(8);
		reloc_text(slot, ::tpde::elf::R_AARCH64_ADR_PREL_PG_HI21,
			text_writer.offset(), 0);
		ASM(ADRP, target_reg, 0, 0);
		reloc_text(slot, ::tpde::elf::R_AARCH64_LDST64_ABS_LO12_NC,
			text_writer.offset(), 0);
		ASM(LDRxu, target_reg, target_reg, 0);
		return target;
	}

	ValuePart runtime_symbol(zend_native_runtime_helper_id id) {
		return image_symbol_value(
			ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER,
			static_cast<uint32_t>(id));
	}

	void add_unsigned_offset(
		AsmReg destination, AsmReg base, uint64_t offset) {
		if (ASMIF(ADDxi, destination, base, offset)) {
			return;
		}
		ScratchReg amount{this};
		auto amount_reg = amount.alloc_gp();
		materialize_constant(
			offset, DarwinConfig::GP_BANK, 8, amount_reg);
		ASM(ADDx, destination, base, amount_reg);
	}

	AsmReg canonical_frame_register() {
		::tpde::ValueAssignment *assignment = val_assignment(
			adaptor->val_local_idx(IRValueRef{Adaptor::FRAME_VALUE}));
		ZEND_ASSERT(assignment != nullptr);
		::tpde::AssignmentPartRef frame{assignment, 0};
		ZEND_ASSERT(frame.register_valid());
		return AsmReg{frame.get_reg().id()};
	}

	void compare_unsigned_immediate(AsmReg value, uint64_t immediate) {
		if (ASMIF(CMPxi, value, immediate)) {
			return;
		}
		ScratchReg constant{this};
		auto constant_reg = constant.alloc_gp();
		materialize_constant(
			immediate, DarwinConfig::GP_BANK, 8, constant_reg);
		ASM(CMPx, value, constant_reg);
	}

	void generate_exception_branch(IRBlockRef target) {
		auto index = static_cast<uint32_t>(this->analyzer.block_idx(target));
		generate_raw_jump(Jump::jmp, this->block_labels[index]);
	}

	bool cur_func_may_emit_calls() const { return adaptor->plan()->may_emit_calls; }
	::tpde::SymRef cur_personality_func() const { return {}; }
	bool try_force_fixed_assignment(IRValueRef value) const {
		return value == IRValueRef{Adaptor::FRAME_VALUE}
			|| value == IRValueRef{Adaptor::EXECUTION_CONTEXT_VALUE};
	}
	ValueParts val_parts(IRValueRef value) const {
		const zend_tpde_machine_value_kind kind =
			adaptor->machine_kind(value);
		return {
			kind == ZEND_TPDE_MACHINE_VALUE_F64
				? DarwinConfig::FP_BANK : DarwinConfig::GP_BANK,
			kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& adaptor->machine_value_is_register_authoritative(value)
				? 2u : 1u};
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
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
		if (!adaptor->frame_slot_reference(value, &storage_id)) {
			ZEND_UNREACHABLE();
		}
		add_unsigned_offset(destination, canonical_frame_register(),
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval));
	}

	void emit_integer_dispatch(HashTable *jump_table,
		std::span<const ::tpde::Label> labels,
		::tpde::a64::AsmReg value_reg,
		::tpde::a64::AsmReg temp_reg,
		::tpde::Label default_label);
	bool emit_materializations(IRInstRef instruction);
	bool compile_boxed_cond_guard(IRInstRef instruction);
	bool compile_boxed_cond_cold(IRInstRef instruction);
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

uint32_t zval_type(zend_mir_scalar_type_mask type) {
	switch (type) {
		case ZEND_MIR_SCALAR_TYPE_NULL: return IS_NULL;
		case ZEND_MIR_SCALAR_TYPE_I1: return IS_FALSE;
		case ZEND_MIR_SCALAR_TYPE_I64: return IS_LONG;
		case ZEND_MIR_SCALAR_TYPE_F64: return IS_DOUBLE;
		default: return IS_UNDEF;
	}
}

void ZendCompilerA64::emit_integer_dispatch(
	HashTable *jump_table,
	std::span<const ::tpde::Label> labels,
	::tpde::a64::AsmReg value_reg,
	::tpde::a64::AsmReg temp_reg,
	::tpde::Label default_label)
{
	std::vector<zend_tpde_integer_case> cases;
	int64_t low = 0;
	uint64_t range = 0;
	const zend_tpde_integer_dispatch_kind kind =
		zend_tpde_integer_dispatch(jump_table, &cases, &low, &range);
	auto emit_compare = [&](uint64_t expected, ::tpde::Label target) {
		materialize_constant(
			&expected, DarwinConfig::GP_BANK, 8, temp_reg);
		ASM(CMPx, value_reg, temp_reg);
		generate_raw_jump(Jump::Jeq, target);
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
	materialize_constant(
		&low_bits, DarwinConfig::GP_BANK, 8, temp_reg);
	ASM(SUBx, value_reg, value_reg, temp_reg);
	if (kind == ZEND_TPDE_INTEGER_DISPATCH_JUMP_TABLE) {
		const uint64_t high_index = range - 1;
		materialize_constant(
			&high_index, DarwinConfig::GP_BANK, 8, temp_reg);
		ASM(CMPx, value_reg, temp_reg);
		generate_raw_jump(Jump::Jhi, default_label);
		auto &table = text_writer.create_jump_table(
			static_cast<uint32_t>(range), value_reg, temp_reg, false);
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
		const ::tpde::Label greater = text_writer.label_create();
		emit_compare(pivot, labels[cases[middle].label_index]);
		generate_raw_jump(Jump::Jhi, greater);
		self(begin, middle, self);
		label_place(greater);
		self(middle + 1, end, self);
	};
	emit_balanced(0, cases.size(), emit_balanced);
}

bool ZendCompilerA64::emit_materializations(IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const auto materializations = adaptor->materializations(instruction);
	if (materializations.empty()) {
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
	const AsmReg frame_reg = canonical_frame_register();
	for (uint32_t index = 0; index < materializations.size(); ++index) {
		const zend_tpde_materialization &materialization =
			materializations[index];
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + materialization.storage_id)
				* sizeof(zval);
		if (!zend_mir_id_is_valid(materialization.storage_id)
				|| offset > static_cast<uint64_t>(UINT32_MAX)
					- sizeof(zval)) {
			return false;
		}
		const IRValueRef value =
			node.liveness_operands[
				node.materialization_operand_index + index];
		auto value_ref = val_ref(value);
		auto payload = value_ref.part(0);
		auto payload_reg = payload.load_to_reg();
		store_off(frame_reg, static_cast<uint32_t>(offset),
			payload_reg, 8);
		const uint32_t type_offset = static_cast<uint32_t>(
			offset + offsetof(zval, u1.type_info));
		if (materialization.machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto type_info = value_ref.part(1);
			auto type_info_reg = type_info.load_to_reg();
			store_off(frame_reg, type_offset, type_info_reg, 4);
		} else if (materialization.machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOOL) {
			ScratchReg type_info{this};
			auto type_info_reg = type_info.alloc_gp();
			ASM(ORRx, type_info_reg, payload_reg, payload_reg);
			ASM(ADDwi, type_info_reg, type_info_reg, IS_FALSE);
			store_off(frame_reg, type_offset, type_info_reg, 4);
		} else {
			const uint32_t type_info =
				zend_tpde_machine_value_zval_type_info(
					materialization.machine_kind);
			if (type_info == IS_UNDEF) {
				return false;
			}
			ScratchReg type_info_value{this};
			auto type_info_reg = type_info_value.alloc_gp();
			materialize_constant(
				type_info, DarwinConfig::GP_BANK, 4, type_info_reg);
			store_off(frame_reg, type_offset, type_info_reg, 4);
		}
	}
	return true;
}

bool ZendCompilerA64::compile_boxed_cond_guard(IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const zend_tpde_instruction &mir =
		adaptor->mir_instruction(instruction);
	zend_tpde_value_condition layout;
	if (node.operands.size() != 1
			|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			|| !zend_tpde_value_condition_at(mir, &layout)
			|| node.argument_index == UINT32_MAX) {
		return false;
	}
	const auto successors = adaptor->block_succs(
		IRBlockRef{node.control_block});
	if (successors.size() != 3
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

	load_off(type_reg, frame_reg,
		layout.operand_offset
			+ static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
	ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
	ASM(CMPwi, type_reg, IS_NULL);
	generate_raw_jump(Jump::Jeq, falsey);
	ASM(CMPwi, type_reg, IS_FALSE);
	generate_raw_jump(Jump::Jeq, falsey);
	ASM(CMPwi, type_reg, IS_TRUE);
	generate_raw_jump(Jump::Jeq, truthy);
	ASM(CMPwi, type_reg, IS_LONG);
	auto not_long = text_writer.label_create();
	generate_raw_jump(Jump::Jne, not_long);
	load_off(value_reg, frame_reg, layout.operand_offset, 8);
	generate_raw_jump(
		Jump{Jump::Cbnz, value_reg, false}, truthy);
	generate_raw_jump(Jump::jmp, falsey);

	label_place(not_long);
	ASM(CMPwi, type_reg, IS_STRING);
	auto not_string = text_writer.label_create();
	generate_raw_jump(Jump::Jne, not_string);
	load_off(value_reg, frame_reg, layout.operand_offset, 8);
	load_off(type_reg, value_reg,
		static_cast<uint32_t>(offsetof(zend_string, len)), 8);
	generate_raw_jump(Jump{Jump::Cbz, type_reg, false}, falsey);
	ASM(CMPxi, type_reg, 1);
	generate_raw_jump(Jump::Jne, truthy);
	load_off(type_reg, value_reg,
		static_cast<uint32_t>(offsetof(zend_string, val)), 1);
	ASM(CMPwi, type_reg, '0');
	generate_raw_jump(Jump::Jeq, falsey);
	generate_raw_jump(Jump::jmp, truthy);

	label_place(not_string);
	ASM(CMPwi, type_reg, IS_ARRAY);
	auto not_array = text_writer.label_create();
	generate_raw_jump(Jump::Jne, not_array);
	load_off(value_reg, frame_reg, layout.operand_offset, 8);
	load_off(type_reg, value_reg,
		static_cast<uint32_t>(offsetof(HashTable, nNumOfElements)), 4);
	generate_raw_jump(Jump{Jump::Cbnz, type_reg, false}, truthy);
	generate_raw_jump(Jump::jmp, falsey);

	label_place(not_array);
	ASM(CMPwi, type_reg, IS_RESOURCE);
	generate_raw_jump(Jump::Jne, slow);
	load_off(value_reg, frame_reg, layout.operand_offset, 8);
	load_off(type_reg, value_reg,
		static_cast<uint32_t>(offsetof(zend_resource, handle)), 4);
	generate_raw_jump(Jump{Jump::Cbnz, type_reg, false}, truthy);
	generate_raw_jump(Jump::jmp, falsey);

	label_place(truthy);
	materialize_constant(
		uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
	generate_raw_jump(Jump::jmp, ready);
	label_place(falsey);
	materialize_constant(
		uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
	generate_raw_jump(Jump::jmp, ready);
	label_place(slow);
	materialize_constant(
		uint64_t{2}, DarwinConfig::GP_BANK, 4, decision_reg);
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

bool ZendCompilerA64::compile_boxed_cond_cold(IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const zend_tpde_instruction &mir =
		adaptor->mir_instruction(instruction);
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.argument_index});
	if (node.operands.size() != 1 || !mir.has_value_operation
			|| successors.size() != 2) {
		return false;
	}
	zend::native::tpde::CCAssignerAppleA64 assigner;
	CallBuilder builder{*this, assigner};
	builder.add_arg(CallArg{node.operands[0]});
	const zend_mir_executable_value_ref &operation = mir.value_operation;
	builder.add_arg(ValuePart{
		zend_tpde_encode_value_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		zend_tpde_encode_value_operand(operation.op2), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		zend_tpde_encode_value_operand(operation.result), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{operation.extended_value, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{operation.source_opcode, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{operation.source_position_id, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.call(runtime_symbol(mir.runtime_helper));
	ValuePart decision{DarwinConfig::GP_BANK, 4};
	builder.add_ret(decision, ::tpde::CCAssignment{});
	auto decision_scratch = std::move(decision).into_scratch(this);
	auto decision_reg = decision_scratch.cur_reg();
	ASM(CMPxi, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
	auto valid = text_writer.label_create();
	generate_raw_jump(Jump::Jlt, valid);
	decision_scratch.reset();
	RetBuilder return_builder{*this, *cur_cc_assigner()};
	return_builder.add(ValuePart{
		ZEND_NATIVE_EXCEPTION, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	return_builder.ret();
	label_place(valid);
	generate_cond_branch(
		Jump{Jump::Cbnz, decision_reg, false},
		successors[0], successors[1]);
	return true;
}

bool ZendCompilerA64::compile_inst(
	IRInstRef instruction, InstRange remaining_instructions) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	if (!emit_materializations(instruction)) {
		return false;
	}
	if (node.kind == Adaptor::InstKind::BoxedCondGuard) {
		return compile_boxed_cond_guard(instruction);
	}
	if (node.kind == Adaptor::InstKind::BoxedCondCold) {
		return compile_boxed_cond_cold(instruction);
	}
	if (node.kind == Adaptor::InstKind::LoadFrame
			|| node.kind == Adaptor::InstKind::LoadExecutionContext) {
		auto [source_ref, source] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto source_reg = source.load_to_reg();
		auto result_reg = result.alloc_reg();
		mov(result_reg, source_reg, 8);
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeLanding) {
		const zend_tpde_plan *plan = adaptor->plan();
		if (plan->source_op_array == nullptr
				|| node.argument_index >= plan->source_op_array->last) {
			return false;
		}
		while (user_opcode_labels_.size() < plan->source_op_array->last) {
			user_opcode_labels_.push_back(text_writer.label_create());
			user_opcode_dispatch_labels_.push_back(
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
		if (plan->source_op_array == nullptr
				|| node.operands.size() != 4 + dispatch_operand_count
				|| node.argument_index >= plan->source_op_array->last
				|| user_opcode_labels_.size()
					< plan->source_op_array->last) {
			return false;
		}
		const uint32_t source_position = node.argument_index;
		const auto &next_landings =
			adaptor->user_opcode_next_landings();
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{node.operands[0]});
		builder.add_arg(CallArg{node.operands[1]});
		builder.add_arg(ValuePart{
			source_position, 4, DarwinConfig::GP_BANK},
			::tpde::CCAssignment{});
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_OPCODE_INVOKE));
		ValuePart action{DarwinConfig::GP_BANK, 8};
		ValuePart selected_position{DarwinConfig::GP_BANK, 8};
		builder.add_ret(action, ::tpde::CCAssignment{});
		builder.add_ret(selected_position, ::tpde::CCAssignment{});
		auto action_reg = action.cur_reg_or_load(this);
		ScratchReg position{this};
		auto position_reg = position.alloc_specific(AsmReg::R2);
		mov(position_reg, selected_position.cur_reg_or_load(this), 4);
		selected_position.reset(this);
		ScratchReg selected_opcode{this};
		auto selected_opcode_reg =
			selected_opcode.alloc_specific(AsmReg::R3);
		mov(selected_opcode_reg, action_reg, 4);
		ASM(ANDwi, selected_opcode_reg, selected_opcode_reg,
			UINT32_C(0xff));
		auto return_action = text_writer.label_create();
		auto returned = text_writer.label_create();
		auto exception = text_writer.label_create();
		auto continued = text_writer.label_create();
		auto dispatch = text_writer.label_create();
		auto dispatch_to = text_writer.label_create();
		ASM(CMNwi, action_reg, 1);
		generate_raw_jump(Jump::Jeq, exception);
		ASM(CMPxi, action_reg, ZEND_USER_OPCODE_CONTINUE);
		generate_raw_jump(Jump::Jeq, continued);
		ASM(CMPxi, action_reg, ZEND_USER_OPCODE_RETURN);
		generate_raw_jump(Jump::Jeq, return_action);
		ASM(CMPxi, action_reg, ZEND_USER_OPCODE_LEAVE);
		generate_raw_jump(Jump::Jeq, returned);
		ASM(CMPxi, action_reg, ZEND_USER_OPCODE_DISPATCH);
		generate_raw_jump(Jump::Jeq, dispatch);
		generate_raw_jump(Jump::jmp, dispatch_to);
		action.reset(this);
		generate_raw_jump(Jump::jmp, exception);
		auto compare_position = [&](uint32_t source) {
			if (source <= UINT32_C(0xfff)) {
				compare_unsigned_immediate(position_reg, source);
			} else {
				ScratchReg constant{this};
				auto constant_reg = constant.alloc_gp();
				materialize_constant(
					source, DarwinConfig::GP_BANK, 4, constant_reg);
				ASM(CMPx, position_reg, constant_reg);
			}
		};
		label_place(continued);
		ASM(ADDwi, position_reg, position_reg, 1);
		for (uint32_t source = 0;
				source < user_opcode_labels_.size(); ++source) {
			if (next_landings[source] != source) {
				continue;
			}
			compare_position(source);
			generate_raw_jump(
				Jump::Jeq, user_opcode_labels_[source]);
		}
		generate_raw_jump(Jump::jmp, exception);
		label_place(dispatch);
		for (uint32_t source = 0;
				source < user_opcode_dispatch_labels_.size(); ++source) {
			if (next_landings[source] != source) {
				continue;
			}
			compare_position(source);
			generate_raw_jump(
				Jump::Jeq, user_opcode_dispatch_labels_[source]);
		}
		generate_raw_jump(Jump::jmp, exception);
		label_place(dispatch_to);
		for (uint32_t source = 0;
				source < user_opcode_dispatch_labels_.size(); ++source) {
			if (next_landings[source] != source) {
				continue;
			}
			auto next_candidate = text_writer.label_create();
			compare_position(source);
			generate_raw_jump(Jump::Jne, next_candidate);
			ASM(CMPwi, selected_opcode_reg,
				plan->source_op_array->opcodes[source].opcode);
			generate_raw_jump(
				Jump::Jeq, user_opcode_dispatch_labels_[source]);
			label_place(next_candidate);
		}
		struct DispatchToCase {
			::tpde::Label label;
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
			compare_position(source);
			generate_raw_jump(Jump::Jne, next_candidate);
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
				ASM(CMPwi, selected_opcode_reg, target_case.opcode);
				generate_raw_jump(Jump::Jeq, target);
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
				const uint32_t result_offset =
					static_cast<uint32_t>(
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.result_storage_id)
						* sizeof(zval));
				auto [frame_ref, frame] = val_ref_single(
					node.operands[dispatch_case.frame_operand]);
				auto frame_scratch = std::move(frame).into_scratch();
				ScratchReg value{this};
				auto value_reg = value.alloc_gp();
				materialize_constant(
					UINT64_C(0), DarwinConfig::GP_BANK, 8, value_reg);
				store_off(frame_scratch.cur_reg(), result_offset,
					value_reg, 8);
				materialize_constant(dispatch_case.source,
					DarwinConfig::GP_BANK, 4, value_reg);
				store_off(frame_scratch.cur_reg(),
					result_offset
						+ static_cast<uint32_t>(
							offsetof(zval, u2.opline_num)),
					value_reg, 4);
				value.reset();
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
							> (UINT32_MAX / sizeof(zval))
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
				const uint32_t operand_offset =
					static_cast<uint32_t>(
						(uint64_t{ZEND_CALL_FRAME_SLOT}
							+ operation.op1_storage_id)
						* sizeof(zval));
				auto slow_exception = text_writer.label_create();
				auto [frame_ref, frame] = val_ref_single(
					node.operands[dispatch_case.frame_operand]);
				auto frame_scratch = std::move(frame).into_scratch();
				ScratchReg continuation{this};
				auto continuation_reg = continuation.alloc_gp();
				load_off(continuation_reg, frame_scratch.cur_reg(),
					operand_offset
						+ static_cast<uint32_t>(
							offsetof(zval, u2.opline_num)),
					4);
				frame_scratch.reset();
				ASM(CMNwi, continuation_reg, 1);
				generate_raw_jump(Jump::Jeq, slow_exception);
				for (uint32_t source = 0;
						source + 1 < next_landings.size(); ++source) {
					const uint32_t landing = next_landings[source + 1];
					if (landing == UINT32_MAX
							|| landing >= user_opcode_labels_.size()) {
						continue;
					}
					if (source <= UINT32_C(0xfff)) {
						compare_unsigned_immediate(continuation_reg, source);
					} else {
						ScratchReg expected{this};
						auto expected_reg = expected.alloc_gp();
						materialize_constant(source,
							DarwinConfig::GP_BANK, 4, expected_reg);
						ASM(CMPx, continuation_reg, expected_reg);
					}
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::Jne, continued);
					generate_raw_jump(
						Jump::jmp, user_opcode_labels_[landing]);
					label_place(continued);
				}
				continuation.reset();
				generate_raw_jump(Jump::jmp, exception);
				label_place(slow_exception);
				zend::native::tpde::CCAssignerAppleA64 finally_assigner;
				CallBuilder finally_call{*this, finally_assigner};
				finally_call.add_arg(CallArg{
					node.operands[dispatch_case.slow_frame_operand]});
				finally_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				finally_call.add_arg(ValuePart{
					operation.op2_unused_payload, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				finally_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				finally_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart selected{DarwinConfig::GP_BANK, 4};
				finally_call.add_ret(selected, ::tpde::CCAssignment{});
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
					ScratchReg expected{this};
					auto expected_reg = expected.alloc_gp();
					materialize_constant(
						ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
							| handler.source_position_id,
						DarwinConfig::GP_BANK, 4, expected_reg);
					ASM(CMPx, selected_reg, expected_reg);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::Jne, continued);
					jump_to_source(handler.source_position_id);
					label_place(continued);
				}
				selected.reset(this);
				generate_raw_jump(Jump::jmp, exception);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_CATCH) {
				zend::native::tpde::CCAssignerAppleA64 catch_assigner;
				CallBuilder catch_call{*this, catch_assigner};
				catch_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				catch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				catch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				catch_call.add_arg(ValuePart{
					operation.extended_value, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				catch_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				catch_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart result{DarwinConfig::GP_BANK, 4};
				catch_call.add_ret(result, ::tpde::CCAssignment{});
				auto result_reg = result.cur_reg_or_load(this);
				auto catch_branch = text_writer.label_create();
				auto catch_matched = text_writer.label_create();
				ASM(CMPwi, result_reg, ZEND_NATIVE_CATCH_EXCEPTION);
				generate_raw_jump(Jump::Jeq, exception);
				ASM(CMPwi, result_reg, ZEND_NATIVE_CATCH_BRANCH);
				generate_raw_jump(Jump::Jeq, catch_branch);
				ASM(CMPwi, result_reg, ZEND_NATIVE_CATCH_MATCHED);
				generate_raw_jump(Jump::Jeq, catch_matched);
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
				zend::native::tpde::CCAssignerAppleA64 receive_assigner;
				CallBuilder receive_call{*this, receive_assigner};
				receive_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				receive_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					operation.op1_unused_payload, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.op2, operation.op2_unused_payload),
					8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					operation.op2_unused_payload, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				receive_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{DarwinConfig::GP_BANK, 4};
				receive_call.add_ret(status, ::tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
				auto received = text_writer.label_create();
				generate_raw_jump(Jump::Jeq, received);
				status.reset(this);
				generate_raw_jump(Jump::jmp, exception);
				label_place(received);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_CALL_FRAGMENT) {
				zend::native::tpde::CCAssignerAppleA64 fragment_assigner;
				CallBuilder fragment_call{*this, fragment_assigner};
				fragment_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				fragment_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.op1, operation.op1_unused_payload),
					8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.op1_unused_payload, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.op2, operation.op2_unused_payload),
					8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.op2_unused_payload, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.result,
						operation.result_unused_payload),
					8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.result_unused_payload, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.extended_value, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				fragment_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{DarwinConfig::GP_BANK, 4};
				fragment_call.add_ret(status, ::tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
				auto completed = text_writer.label_create();
				generate_raw_jump(Jump::Jeq, completed);
				if (dispatch_case.instruction != nullptr
						&& zend_mir_id_is_valid(
							dispatch_case.instruction->exception_block_id)) {
					auto propagate = text_writer.label_create();
					ASM(CMPwi, status_reg, ZEND_NATIVE_EXCEPTION);
					generate_raw_jump(Jump::Jne, propagate);
					generate_exception_branch(adaptor->block_ref(
						dispatch_case.instruction->exception_block_id));
					label_place(propagate);
				}
				{
					RetBuilder return_builder{
						*this, *cur_cc_assigner()};
					return_builder.add(
						std::move(status), ::tpde::CCAssignment{});
					return_builder.ret();
				}
				label_place(completed);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_RETURN) {
				zend::native::tpde::CCAssignerAppleA64 return_assigner;
				CallBuilder return_call{*this, return_assigner};
				return_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				return_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				return_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_call.add_arg(ValuePart{
					operation.extended_value, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{DarwinConfig::GP_BANK, 4};
				return_call.add_ret(status, ::tpde::CCAssignment{});
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(
					std::move(status), ::tpde::CCAssignment{});
				return_builder.ret();
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_THROW) {
				zend::native::tpde::CCAssignerAppleA64 throw_assigner;
				CallBuilder throw_call{*this, throw_assigner};
				throw_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				throw_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				throw_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				throw_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				throw_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{DarwinConfig::GP_BANK, 4};
				throw_call.add_ret(status, ::tpde::CCAssignment{});
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(
					std::move(status), ::tpde::CCAssignment{});
				return_builder.ret();
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_MULTI_BRANCH) {
				zend_tpde_user_multi_branch layout;
				if (!zend_tpde_user_multi_branch_at(
						plan, operation, dispatch_case.target_opcode,
						&layout)) {
					auto [frame_ref, frame] = val_ref_single(
						node.operands[dispatch_case.frame_operand]);
					frame.reset();
					generate_raw_jump(Jump::jmp, exception);
					continue;
				}
				std::vector<::tpde::Label> case_labels;
				case_labels.reserve(
					zend_hash_num_elements(layout.jump_table));
				for (uint32_t index = 0;
						index < zend_hash_num_elements(layout.jump_table);
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
				auto frame_scratch = std::move(frame).into_scratch();
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
				add_unsigned_offset(
					slot_reg, frame_scratch.cur_reg(), layout.operand_offset);
				load_off(type_reg, slot_reg,
					static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
				ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
				auto spilled = spill_before_branch();
				begin_branch_region();
				auto dereferenced = text_writer.label_create();
				ASM(CMPwi, type_reg, IS_REFERENCE);
				generate_raw_jump(Jump::Jne, dereferenced);
				load_off(slot_reg, slot_reg, 0, 8);
				ASM(ADDxi, slot_reg, slot_reg,
					static_cast<uint32_t>(offsetof(zend_reference, val)));
				load_off(type_reg, slot_reg,
					static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
				ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
				label_place(dereferenced);
				if (layout.target_opcode != ZEND_SWITCH_STRING) {
					ASM(CMPwi, type_reg, IS_LONG);
					generate_raw_jump(Jump::Jeq, long_label);
				}
				if (layout.target_opcode != ZEND_SWITCH_LONG) {
					ASM(CMPwi, type_reg, IS_STRING);
					generate_raw_jump(Jump::Jeq, string_label);
				}
				generate_raw_jump(Jump::jmp, fallback_label);

				uint32_t case_index = 0;
				zend_ulong numeric_key;
				zend_string *string_key;
				zval *jump_value;
				label_place(long_label);
				load_off(value_reg, slot_reg, 0, 8);
				emit_integer_dispatch(layout.jump_table, case_labels,
					value_reg, constant_reg, default_label);

				label_place(string_label);
				load_off(value_reg, slot_reg, 0, 8);
				case_index = 0;
				ZEND_HASH_FOREACH_KEY_VAL(
						layout.jump_table, numeric_key, string_key,
						jump_value) {
					if (string_key != nullptr) {
						auto next_case = text_writer.label_create();
						const uint64_t length = ZSTR_LEN(string_key);
						load_off(probe_reg, value_reg,
							static_cast<uint32_t>(
								offsetof(zend_string, len)), 8);
						materialize_constant(
							&length, DarwinConfig::GP_BANK, 8,
							constant_reg);
						ASM(CMPx, probe_reg, constant_reg);
						generate_raw_jump(Jump::Jne, next_case);
						size_t offset = 0;
						while (offset < ZSTR_LEN(string_key)) {
							const uint32_t width =
								ZSTR_LEN(string_key) - offset >= 8 ? 8
								: ZSTR_LEN(string_key) - offset >= 4 ? 4
								: ZSTR_LEN(string_key) - offset >= 2 ? 2 : 1;
							uint64_t expected = 0;
							memcpy(&expected,
								ZSTR_VAL(string_key) + offset, width);
							load_off(probe_reg, value_reg,
								static_cast<uint32_t>(
									offsetof(zend_string, val) + offset),
								width);
							materialize_constant(
								&expected, DarwinConfig::GP_BANK,
								width, constant_reg);
							ASM(CMPx, probe_reg, constant_reg);
							generate_raw_jump(Jump::Jne, next_case);
							offset += width;
						}
						generate_raw_jump(
							Jump::jmp, case_labels[case_index]);
						label_place(next_case);
					}
					case_index++;
				} ZEND_HASH_FOREACH_END();
				generate_raw_jump(Jump::jmp, default_label);

				case_index = 0;
				ZEND_HASH_FOREACH_VAL(
						layout.jump_table, jump_value) {
					label_place(case_labels[case_index++]);
					jump_to_source(zend_tpde_relative_source_target(
						plan->source_op_array, dispatch_case.source,
						Z_LVAL_P(jump_value)));
				} ZEND_HASH_FOREACH_END();
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
				zend::native::tpde::CCAssignerAppleA64 branch_assigner;
				CallBuilder branch_call{*this, branch_assigner};
				branch_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				branch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op2), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					operation.extended_value, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					dispatch_case.source, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				branch_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart result{DarwinConfig::GP_BANK, 4};
				branch_call.add_ret(result, ::tpde::CCAssignment{});
				auto result_reg = result.cur_reg_or_load(this);
				auto branch_target = text_writer.label_create();
				auto branch_following = text_writer.label_create();
				ASM(CMPwi, result_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
				generate_raw_jump(Jump::Jeq, exception);
				if (dispatch_case.kind
						== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2) {
					ASM(CMPwi, result_reg, ZEND_NATIVE_ITERATOR_NEXT);
				} else {
					ASM(CMPwi, result_reg, ZEND_NATIVE_ITERATOR_END);
				}
				generate_raw_jump(Jump::Jeq, branch_target);
				ASM(CMPwi, result_reg,
					dispatch_case.kind
							== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2
						? ZEND_NATIVE_ITERATOR_END
						: ZEND_NATIVE_ITERATOR_NEXT);
				generate_raw_jump(Jump::Jeq, branch_following);
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
					? zend_tpde_encode_value_operand(
						operand, unused_payload)
					: zend_tpde_encode_value_operand(operand);
			};
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder operation_call{*this, assigner};
			operation_call.add_arg(
				CallArg{node.operands[dispatch_case.frame_operand]});
			operation_call.add_arg(ValuePart{
				encode_operand(
					operation.op1, operation.op1_unused_payload), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				encode_operand(
					operation.op2, operation.op2_unused_payload), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				encode_operand(
					operation.result, operation.result_unused_payload), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			if (explicit_auxiliary) {
				operation_call.add_arg(ValuePart{
					encode_operand(operation.auxiliary,
						operation.auxiliary_unused_payload), 8,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			}
			operation_call.add_arg(ValuePart{
				operation.extended_value, 4, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				dispatch_case.source, 4, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			operation_call.call(runtime_symbol(dispatch_case.helper));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			operation_call.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			auto completed = text_writer.label_create();
			ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::Jeq, completed);
			if (dispatch_case.instruction != nullptr
					&& zend_mir_id_is_valid(
						dispatch_case.instruction->exception_block_id)) {
				auto propagate = text_writer.label_create();
				ASM(CMPxi, status_reg, ZEND_NATIVE_EXCEPTION);
				generate_raw_jump(Jump::Jne, propagate);
				generate_exception_branch(adaptor->block_ref(
					dispatch_case.instruction->exception_block_id));
				label_place(propagate);
			}
			{
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(
					std::move(status), ::tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(completed);
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
			load_off(call_info_reg, frame_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, call_info_reg, call_info_reg, ZEND_CALL_GENERATOR);
			ASM(CMPxi, call_info_reg, 0);
			generate_raw_jump(Jump::Jeq, returned);
			call_info.reset();
		}
		{
			zend::native::tpde::CCAssignerAppleA64 return_assigner;
			CallBuilder return_call{*this, return_assigner};
			return_call.add_arg(CallArg{node.operands[3]});
			return_call.call(runtime_symbol(
				ZEND_NATIVE_HELPER_GENERATOR_USER_OPCODE_RETURN));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			return_call.add_ret(status, ::tpde::CCAssignment{});
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status),
				::tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(returned);
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{
				ZEND_NATIVE_RETURNED, 4, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(exception);
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{
				ZEND_NATIVE_EXCEPTION, 4, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			return_builder.ret();
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
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{node.operands[0]});
		if (call.entry_cell != nullptr) {
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site.target_id), ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				UINT64_C(0), 8, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
		} else {
			builder.add_arg(ValuePart{
				UINT64_C(0), 8, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site.target_id), ::tpde::CCAssignment{});
		}
		builder.add_arg(image_symbol_value(
			ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
			call.id), ::tpde::CCAssignment{});
		builder.add_arg(ValuePart{
			node.argument_index, 4, DarwinConfig::GP_BANK},
			::tpde::CCAssignment{});
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_FRAGMENT));
		ValuePart status{DarwinConfig::GP_BANK, 8};
		ValuePart payload{DarwinConfig::GP_BANK, 8};
		builder.add_ret(status, ::tpde::CCAssignment{});
		builder.add_ret(payload, ::tpde::CCAssignment{});
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
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(
				std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(continued);
		if (node.has_result) {
			auto [result_ref, result] = result_ref_single(node.result);
			if (val_parts(node.result).bank == DarwinConfig::FP_BANK) {
				auto payload_reg = payload.cur_reg_or_load(this);
				ScratchReg converted{this};
				auto result_reg =
					converted.alloc(DarwinConfig::FP_BANK);
				ASM(FMOVdx, result_reg, payload_reg);
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
	if (node.kind == Adaptor::InstKind::GeneratorGateway) {
		if (node.operands.size() != 2
				|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				|| node.operands[1]
					!= IRValueRef{Adaptor::EXECUTION_CONTEXT_VALUE}
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
			ScratchReg call_info{this};
			auto call_info_reg = call_info.alloc_gp();
			load_off(call_info_reg, frame_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, call_info_reg, call_info_reg, ZEND_CALL_GENERATOR);
			ASM(CMPxi, call_info_reg, 0);
			generate_raw_jump(Jump::Jeq, normal);
			call_info.reset();
			ScratchReg opline{this};
			ScratchReg function{this};
			ScratchReg target{this};
			auto opline_reg = opline.alloc_gp();
			auto function_reg = function.alloc_gp();
			auto target_reg = target.alloc_gp();
			ScratchReg exception{this};
			auto exception_reg = exception.alloc_gp();
			load_off(opline_reg, frame_reg,
				static_cast<uint32_t>(offsetof(zend_execute_data, opline)), 8);
			load_off(function_reg, frame_reg,
				static_cast<uint32_t>(offsetof(zend_execute_data, func)), 8);
			load_off(exception_reg, context_reg,
				static_cast<uint32_t>(
					offsetof(zend_native_execution_context, exception)), 8);
			load_off(exception_reg, exception_reg, 0, 8);
			for (uint32_t index = 0;
					index < adaptor->generator_resume_targets().size(); ++index) {
				const uint64_t byte_offset =
					uint64_t{adaptor->generator_resume_targets()[index]}
						* sizeof(zend_op);
				if (byte_offset > UINT32_MAX) {
					return false;
				}
				load_off(target_reg, function_reg,
					static_cast<uint32_t>(
						offsetof(zend_function, op_array.opcodes)), 8);
				if (byte_offset != 0) {
					ASM(ADDxi, target_reg, target_reg,
						static_cast<uint32_t>(byte_offset));
				}
				ASM(CMPx, opline_reg, target_reg);
				generate_raw_jump(
					Jump::Jeq, generator_resume_labels_[index]);
			}
			ASM(CMPxi, exception_reg, 0);
			generate_raw_jump(Jump::Jeq, invalid);
			load_off(opline_reg, context_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_execution_context,
					opline_before_exception)), 8);
			load_off(opline_reg, opline_reg, 0, 8);
			for (uint32_t index = 0;
					index < adaptor->generator_resume_targets().size(); ++index) {
				const uint64_t byte_offset =
					uint64_t{adaptor->generator_resume_targets()[index] - 1}
						* sizeof(zend_op);
				if (byte_offset > UINT32_MAX) {
					return false;
				}
				load_off(target_reg, function_reg,
					static_cast<uint32_t>(
						offsetof(zend_function, op_array.opcodes)), 8);
				if (byte_offset != 0) {
					ASM(ADDxi, target_reg, target_reg,
						static_cast<uint32_t>(byte_offset));
				}
				auto next = text_writer.label_create();
				ASM(CMPx, opline_reg, target_reg);
				generate_raw_jump(Jump::Jne, next);
				const zend_mir_block_id exception_block =
					adaptor->generator_resume_exception_blocks()[index];
				if (zend_mir_id_is_valid(exception_block)) {
					store_off(frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, opline)),
						opline_reg, 8);
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
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
		}
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
		auto [frame_ref, frame] = val_ref_single(node.operands[0]);
		auto frame_reg = frame.load_to_reg();
		for (size_t index = 1; index < node.operands.size(); ++index) {
			const IRValueRef operand = node.operands[index];
			const zend_mir_storage_id storage =
				adaptor->canonical_storage(operand);
			const zend_mir_scalar_type_mask exact_type =
				adaptor->exact_type(operand);
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (!zend_mir_id_is_valid(storage)
					|| !zend_mir_scalar_type_is_exact(exact_type)
					|| exact_type == ZEND_MIR_SCALAR_TYPE_NULL
					|| offset + offsetof(zval, u1.type_info)
						> UINT32_MAX) {
				return false;
			}
			auto [value_ref, value] = val_ref_single(operand);
			if (!value.has_assignment()
					|| value.assignment().variable_ref()) {
				return false;
			}
			/*
			 * Resume redefines the machine value from its canonical Zend
			 * slot. Invalidate TPDE's spill copy before allocating so no
			 * stale reload is emitted ahead of the authoritative frame load.
			 */
			value.assignment().set_modified(true);
			auto value_reg = value.cur_reg_or_alloc();
			switch (exact_type) {
				case ZEND_MIR_SCALAR_TYPE_I1:
					load_off(value_reg, frame_reg,
						static_cast<uint32_t>(
							offset + offsetof(zval, u1.type_info)), 4);
					ASM(CMPwi, value_reg, IS_TRUE);
					generate_raw_set(Jump::Jeq, value_reg);
					break;
				case ZEND_MIR_SCALAR_TYPE_I64:
				case ZEND_MIR_SCALAR_TYPE_F64:
					load_off(value_reg, frame_reg,
						static_cast<uint32_t>(offset), 8);
					break;
				default:
					return false;
			}
			value.set_modified();
		}
		return true;
	}
	if (node.kind == Adaptor::InstKind::ZvalTypeLoad) {
		if (node.operands.size() != 1) {
			return false;
		}
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
		const bool frame_slot = adaptor->frame_slot_reference(
			node.operands[0], &storage_id);
		const uint64_t frame_offset = frame_slot
			? (uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval)
			: 0;
		if (frame_offset + offsetof(zval, u1.type_info) > UINT32_MAX) {
			return false;
		}
		auto emit = [&](AsmReg address, uint32_t offset) {
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			load_off(result_reg, address,
				offset + static_cast<uint32_t>(
					offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, result_reg, result_reg, Z_TYPE_MASK);
			result.set_modified();
		};
		if (frame_slot) {
			emit(canonical_frame_register(),
				static_cast<uint32_t>(frame_offset));
		} else {
			auto [address_ref, address] =
				val_ref_single(node.operands[0]);
			emit(address.load_to_reg(), 0);
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
						> UINT32_MAX) {
				return false;
			}
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			load_off(type_reg, frame_reg,
				static_cast<uint32_t>(
					offset + offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			if (guard.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				auto matched_boolean = text_writer.label_create();
				ASM(CMPwi, type_reg, IS_FALSE);
				generate_raw_jump(Jump::Jeq, matched_boolean);
				ASM(CMPwi, type_reg, IS_TRUE);
				generate_raw_jump(Jump::Jne, mismatch);
				type.reset();
				label_place(matched_boolean);
			} else {
				ASM(CMPwi, type_reg, expected_type);
				generate_raw_jump(Jump::Jne, mismatch);
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
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(matched);
		return true;
	}
	if (node.kind == Adaptor::InstKind::ZvalGuardType) {
		if (node.operands.size() != 1
				|| node.operands[0]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				|| !zend_mir_id_is_valid(node.storage_id)
				|| !zend_mir_scalar_type_is_exact(node.exact_type)
				|| node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
			return false;
		}
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + node.storage_id) * sizeof(zval);
		if (offset + offsetof(zval, u1.type_info) > UINT32_MAX) {
			return false;
		}
		auto [frame_ref, frame] = val_ref_single(node.operands[0]);
		auto frame_reg = frame.load_to_reg();
		ScratchReg type{this};
		auto type_reg = type.alloc_gp();
		load_off(type_reg, frame_reg,
			static_cast<uint32_t>(
				offset + offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		auto matched = text_writer.label_create();
		if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			ASM(CMPwi, type_reg, IS_FALSE);
			generate_raw_jump(Jump::Jeq, matched);
			ASM(CMPwi, type_reg, IS_TRUE);
		} else {
			ASM(CMPwi, type_reg, zval_type(node.exact_type));
		}
		generate_raw_jump(Jump::Jeq, matched);
		type.reset();
		frame.reset();
		frame_ref.reset();
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETRY, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(matched);
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
		zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
		const bool frame_slot = adaptor->frame_slot_reference(
			node.operands[0], &storage_id);
		const uint64_t frame_offset = frame_slot
			? (uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval)
			: 0;
		if (frame_offset > UINT32_MAX - sizeof(zval)) {
			return false;
		}
		auto emit = [&](AsmReg address, uint32_t offset) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result_value = result_ref(node.result);
				for (uint32_t part = 0; part < 2; ++part) {
					auto value = result_value.part(part);
					auto value_reg = value.alloc_reg();
					load_off(value_reg, address,
						offset + (part == 0
							? 0
							: static_cast<uint32_t>(
								offsetof(zval, u1.type_info))),
						part == 0 ? 8 : 4);
					value.set_modified();
				}
				return true;
			}
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			switch (node.exact_type) {
				case ZEND_MIR_SCALAR_TYPE_I1:
					load_off(result_reg, address,
						offset + static_cast<uint32_t>(
							offsetof(zval, u1.type_info)), 4);
					ASM(CMPwi, result_reg, IS_TRUE);
					generate_raw_set(Jump::Jeq, result_reg);
					break;
				case ZEND_MIR_SCALAR_TYPE_I64:
				case ZEND_MIR_SCALAR_TYPE_F64:
					load_off(result_reg, address, offset, 8);
					break;
				default:
					switch (adaptor->machine_kind(node.result)) {
						case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
						case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
						case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
						case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
							load_off(result_reg, address, offset, 8);
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
				static_cast<uint32_t>(frame_offset));
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
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(ValuePart{
			static_cast<uint32_t>(record.source_position_id), 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_SOURCE_PROBE));
	}
	if (record.opcode == ZEND_MIR_OPCODE_ZVAL_STORE) {
		if (node.operands.size() != 2
				|| node.operands[1] != IRValueRef{Adaptor::FRAME_VALUE}) {
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
				|| offset > UINT32_MAX
				|| offset + offsetof(zval, u1.type_info) > UINT32_MAX) {
			return false;
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		auto slow_release = text_writer.label_create();
		auto store_value = text_writer.label_create();
		auto slot_resolved = text_writer.label_create();
		ScratchReg old_type{this};
		ScratchReg counted{this};
		ScratchReg refcount{this};
		auto slot_reg = counted.alloc_gp();
		auto old_type_reg = old_type.alloc_gp();
		auto refcount_reg = refcount.alloc_gp();
		add_unsigned_offset(
			slot_reg, frame_reg, static_cast<uint32_t>(offset));
		load_off(old_type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(CMPwi, old_type_reg, IS_REFERENCE_EX);
		generate_raw_jump(Jump::Jne, slot_resolved);
		load_off(slot_reg, slot_reg, 0, 8);
		ASM(ADDxi, slot_reg, slot_reg,
			static_cast<uint32_t>(offsetof(zend_reference, val)));
		load_off(old_type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		label_place(slot_resolved);
		ASM(TSTwi, old_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, store_value);
		/*
		 * Collectable values need gc_check_possible_root() even when shared.
		 * Keep that transition in the semantic slow path. Strings, resources,
		 * and references can use the non-final decrement directly.
		 */
		ASM(TSTwi, old_type_reg,
			IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow_release);
		load_off(refcount_reg, slot_reg, 0, 8);
		load_off(old_type_reg, refcount_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(CMPwi, old_type_reg, 1);
		generate_raw_jump(Jump::Jls, slow_release);
		ASM(SUBwi, old_type_reg, old_type_reg, 1);
		store_off(refcount_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			old_type_reg, 4);
		generate_raw_jump(Jump::jmp, store_value);

		old_type.reset();
		counted.reset();
		refcount.reset();
		label_place(slow_release);
		{
			const auto register_state =
				zend::native::tpde::
					capture_conditional_call_register_state(*this);
			ScratchReg slot_argument{this};
			auto slot_argument_reg = slot_argument.alloc_gp();
			add_unsigned_offset(slot_argument_reg, frame_reg,
				static_cast<uint32_t>(offset));
			ValuePart slot_pointer{DarwinConfig::GP_BANK, 8};
			slot_pointer.set_value(this, std::move(slot_argument));
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(
					std::move(slot_pointer), ::tpde::CCAssignment{});
				builder.call(runtime_symbol(mir.runtime_helper));
			}
			zend::native::tpde::restore_conditional_call_register_state(
				*this, register_state);
		}

		label_place(store_value);
		auto store_slot_resolved = text_writer.label_create();
		ScratchReg store_slot{this};
		ScratchReg store_type{this};
		auto store_slot_reg = store_slot.alloc_gp();
		auto store_type_reg = store_type.alloc_gp();
		add_unsigned_offset(
			store_slot_reg, frame_reg, static_cast<uint32_t>(offset));
		load_off(store_type_reg, store_slot_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(CMPwi, store_type_reg, IS_REFERENCE_EX);
		generate_raw_jump(Jump::Jne, store_slot_resolved);
		load_off(store_slot_reg, store_slot_reg, 0, 8);
		ASM(ADDxi, store_slot_reg, store_slot_reg,
			static_cast<uint32_t>(offsetof(zend_reference, val)));
		label_place(store_slot_resolved);
		if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
			ScratchReg zero{this};
			auto zero_reg = zero.alloc_gp();
			materialize_constant(
				UINT64_C(0), DarwinConfig::GP_BANK, 8, zero_reg);
			store_off(store_slot_reg, 0, zero_reg, 8);
			ScratchReg kind{this};
			auto kind_reg = kind.alloc_gp();
			materialize_constant(
				static_cast<uint64_t>(IS_NULL),
				DarwinConfig::GP_BANK, 4, kind_reg);
			store_off(store_slot_reg,
				static_cast<uint32_t>(
					offsetof(zval, u1.type_info)),
				kind_reg, 4);
		} else {
			auto [value_ref, value] = val_ref_single(input);
			auto value_reg = value.load_to_reg();
			store_off(store_slot_reg, 0, value_reg, 8);
			ScratchReg kind{this};
			auto kind_reg = kind.alloc_gp();
			materialize_constant(
				zval_type(exact_type), DarwinConfig::GP_BANK, 4, kind_reg);
			if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				ASM(ADDx, kind_reg, kind_reg, value_reg);
			}
			store_off(store_slot_reg,
				static_cast<uint32_t>(
					offsetof(zval, u1.type_info)),
				kind_reg, 4);
		}
		return true;
	}
	if (mir.source_effect == ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE) {
		if (mir.source_effect_exact_type != ZEND_MIR_SCALAR_TYPE_I64
				|| node.operands.empty()) {
			return false;
		}
		auto [frame_ref, frame] = val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(std::move(frame), ::tpde::CCAssignment{});

		ScratchReg slot{this};
		auto slot_reg = slot.alloc_gp();
		add_unsigned_offset(slot_reg, frame_reg,
			static_cast<uint32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval)));
		ValuePart slot_pointer{DarwinConfig::GP_BANK, 8};
		slot_pointer.set_value(this, std::move(slot));

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
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_ABI_CONFORMANCE));
		ValuePart status{DarwinConfig::GP_BANK, 8};
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
	if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR
			|| mir.source_effect == ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR) {
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
			builder.call(runtime_symbol(mir.runtime_helper));
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
			builder.call(runtime_symbol(mir.runtime_helper));
		}
		return true;
	}
	auto unary = [&]() { return val_ref_single(node.operands[0]); };
	auto binary = [&]() {
		return std::pair{val_ref_single(node.operands[0]),
			val_ref_single(node.operands[1])};
	};
	auto copy_result = [&]() {
		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto source_value = val_ref(node.operands[0]);
			auto result_value = result_ref(node.result);
			for (uint32_t part = 0; part < 2; ++part) {
				auto source = source_value.part(part);
				auto result = result_value.part(part);
				auto source_reg = source.load_to_reg();
				auto result_reg = result.alloc_try_reuse(source);
				if (source_reg != result_reg) {
					mov(result_reg, source_reg, part == 0 ? 8 : 4);
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
	auto fuse_compare_branch = [&](Jump condition) {
		if (remaining_instructions.from == remaining_instructions.to) {
			return false;
		}
		const IRInstRef next = *remaining_instructions.from;
		const Adaptor::InstNode &consumer = adaptor->node(next);
		if (consumer.kind != Adaptor::InstKind::MIR
				|| consumer.operands.size() != 1
				|| consumer.operands[0] != node.result
				|| adaptor->instruction_record(next).opcode
					!= ZEND_MIR_OPCODE_COND_BRANCH
				|| this->analyzer.liveness_info(
					adaptor->val_local_idx(node.result)).ref_count != 2) {
			return false;
		}
		const zend_mir_instruction_record branch =
			adaptor->instruction_record(next);
		const auto &successors = adaptor->block_succs(
			adaptor->block_ref(branch.block_id));
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
		uint64_t immediate;
		auto left_reg = left.load_to_reg();
		if (adaptor->constant(node.operands[1], &immediate)) {
			compare_unsigned_immediate(left_reg, immediate);
		} else {
			ASM(CMPx, left_reg, right.load_to_reg());
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
	auto floating_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		ASM(FCMP_d, left.load_to_reg(), right.load_to_reg());
		if (fuse_compare_branch(condition)) {
			return true;
		}
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		generate_raw_set(condition, result_reg);
		result.set_modified();
		return true;
	};
	auto execute_value_operation = [&](ValuePart *frame_argument = nullptr) {
		const zend_native_runtime_helper_id helper = mir.runtime_helper;
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
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		if (frame_argument != nullptr) {
			builder.add_arg(
				std::move(*frame_argument), ::tpde::CCAssignment{});
		} else {
			builder.add_arg(CallArg{node.operands.back()});
		}
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
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		if (helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL) {
			builder.add_arg(ValuePart{operation.source_opcode, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(helper));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			builder.add_ret(status, ::tpde::CCAssignment{});
			if (zend_mir_id_is_valid(mir.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(mir.exception_block_id));
				status.reset(this);
				return true;
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(
				std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			return true;
		}
		builder.add_arg(ValuePart{
			encode_operand(
				operation.op2, operation.op2_unused_payload), 8,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.add_arg(ValuePart{
			encode_operand(
				operation.result, operation.result_unused_payload), 8,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		if (explicit_auxiliary) {
			builder.add_arg(ValuePart{
				encode_operand(operation.auxiliary,
					operation.auxiliary_unused_payload), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		}
		builder.add_arg(ValuePart{operation.extended_value, 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.add_arg(ValuePart{operation.source_opcode, 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.add_arg(ValuePart{operation.source_position_id, 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.call(runtime_symbol(helper));
		ValuePart status{DarwinConfig::GP_BANK, 4};
		builder.add_ret(status, ::tpde::CCAssignment{});
		auto status_reg = status.cur_reg_or_load(this);
		ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, continued);
		if (zend_mir_id_is_valid(mir.exception_block_id)) {
			auto propagate = text_writer.label_create();
			ASM(CMPxi, status_reg, ZEND_NATIVE_EXCEPTION);
			generate_raw_jump(Jump::Jne, propagate);
			generate_exception_branch(
				adaptor->block_ref(mir.exception_block_id));
			label_place(propagate);
		}
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(std::move(status), ::tpde::CCAssignment{});
		return_builder.ret();
		label_place(continued);
		return true;
	};
	if (node.kind == Adaptor::InstKind::GuardedCold) {
		if (node.continuation_block == UINT32_MAX
				|| !execute_value_operation()) {
			return false;
		}
		generate_branch_to_block(Jump::jmp,
			IRBlockRef{node.continuation_block}, false, true);
		return true;
	}
	auto copy_slot = [&](
			const zend_mir_source_operand_ref &source_operand,
			zend_mir_storage_id source_storage,
			zend_mir_storage_id target_storage,
			zend_mir_storage_id result_storage,
			bool move_source) {
		if (source_storage == ZEND_MIR_ID_INVALID
				|| target_storage == ZEND_MIR_ID_INVALID
				|| source_storage == target_storage
				|| (result_storage != ZEND_MIR_ID_INVALID
					&& (result_storage == source_storage
						|| result_storage == target_storage))) {
			return execute_value_operation();
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		const uint64_t target_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + target_storage) * sizeof(zval);
		const uint64_t result_offset = result_storage == ZEND_MIR_ID_INVALID
			? 0 : (uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
		if (source_offset > UINT32_MAX - sizeof(zval)
				|| target_offset > UINT32_MAX - sizeof(zval)
				|| result_offset > UINT32_MAX - sizeof(zval)) {
			return false;
		}

		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg source_type{this};
		ScratchReg target_type{this};
		ScratchReg low_word{this};
		ScratchReg probe{this};
		auto source_type_reg = source_type.alloc_gp();
		auto target_type_reg = target_type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto probe_reg = probe.alloc_gp();

		load_off(source_type_reg, frame_reg,
			static_cast<uint32_t>(
				source_offset + offsetof(zval, u1.type_info)), 4);
		if (source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
			ASM(CMPwi, source_type_reg, IS_UNDEF);
			generate_raw_jump(Jump::Jeq, slow);
		}
		if (source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				|| source_operand.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR) {
			ASM(ANDwi, probe_reg, source_type_reg, Z_TYPE_MASK);
			ASM(CMPwi, probe_reg, IS_REFERENCE);
			generate_raw_jump(Jump::Jeq, slow);
		}
		load_off(target_type_reg, frame_reg,
			static_cast<uint32_t>(
				target_offset + offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, probe_reg, target_type_reg, Z_TYPE_MASK);
		ASM(CMPwi, probe_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(TSTwi, target_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto target_checked = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, target_checked);
		/*
		 * GC_DTOR_NO_REF() must purple a shared collectable value.  Keep
		 * that transition in the semantic helper; strings and resources
		 * only need the refcount decrement performed here.
		 */
		ASM(TSTwi, target_type_reg,
			IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		load_off(low_word_reg, frame_reg,
			static_cast<uint32_t>(target_offset), 8);
		load_off(probe_reg, low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(CMPwi, probe_reg, 1);
		generate_raw_jump(Jump::Jle, slow);
		label_place(target_checked);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			load_off(probe_reg, frame_reg,
				static_cast<uint32_t>(
					result_offset + offsetof(zval, u1.type_info)), 4);
			ASM(CMPwi, probe_reg, IS_DOUBLE);
			generate_raw_jump(Jump::Jgt, slow);
		}
		ASM(TSTwi, target_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto target_released = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, target_released);
		load_off(low_word_reg, frame_reg,
			static_cast<uint32_t>(target_offset), 8);
		load_off(probe_reg, low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(SUBwi, probe_reg, probe_reg, 1);
		store_off(low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			probe_reg, 4);
		label_place(target_released);
		load_off(low_word_reg, frame_reg,
			static_cast<uint32_t>(source_offset), 8);
		ASM(TSTwi, source_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto value_owned = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, value_owned);
		load_off(probe_reg, low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, probe_reg, probe_reg,
			(!move_source ? 1 : 0)
				+ (result_storage != ZEND_MIR_ID_INVALID ? 1 : 0));
		store_off(low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			probe_reg, 4);
		label_place(value_owned);
		store_off(frame_reg, static_cast<uint32_t>(target_offset),
			low_word_reg, 8);
		store_off(frame_reg,
			static_cast<uint32_t>(
				target_offset + offsetof(zval, u1.type_info)),
			source_type_reg, 4);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			store_off(frame_reg, static_cast<uint32_t>(result_offset),
				low_word_reg, 8);
			store_off(frame_reg,
				static_cast<uint32_t>(
					result_offset + offsetof(zval, u1.type_info)),
				source_type_reg, 4);
		}
		if (move_source) {
			materialize_constant(
				static_cast<uint64_t>(IS_UNDEF),
				DarwinConfig::GP_BANK, 4, source_type_reg);
			store_off(frame_reg,
				static_cast<uint32_t>(
					source_offset + offsetof(zval, u1.type_info)),
				source_type_reg, 4);
		}
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		source_type.reset();
		target_type.reset();
		low_word.reset();
		probe.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
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
			return execute_value_operation();
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		const uint64_t result_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
		if (source_offset > UINT32_MAX - sizeof(zval)
				|| result_offset > UINT32_MAX - sizeof(zval)) {
			return execute_value_operation();
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

		load_off(type_reg, frame_reg,
			static_cast<uint32_t>(
				source_offset + offsetof(zval, u1.type_info)), 4);
		load_off(value_reg, frame_reg,
			static_cast<uint32_t>(source_offset), 8);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto copied = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, copied);
		load_off(probe_reg, value_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, probe_reg, probe_reg, 1);
		store_off(value_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			probe_reg, 4);
		label_place(copied);
		store_off(frame_reg, static_cast<uint32_t>(result_offset),
			value_reg, 8);
		store_off(frame_reg,
			static_cast<uint32_t>(
				result_offset + offsetof(zval, u1.type_info)),
			type_reg, 4);
		return true;
	};
	auto free_temporary_slot = [&]() {
		const zend_mir_storage_id source_storage =
			mir.value_operation.op1_storage_id;
		if (source_storage == ZEND_MIR_ID_INVALID) {
			return execute_value_operation();
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		if (source_offset > UINT32_MAX - sizeof(zval)) {
			return execute_value_operation();
		}
		auto slow = text_writer.label_create();
		auto released = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg probe{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto probe_reg = probe.alloc_gp();

		load_off(type_reg, frame_reg,
			static_cast<uint32_t>(
				source_offset + offsetof(zval, u1.type_info)), 4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, released);
		load_off(value_reg, frame_reg,
			static_cast<uint32_t>(source_offset), 8);
		load_off(probe_reg, value_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(CMPwi, probe_reg, 1);
		generate_raw_jump(Jump::Jle, slow);
		ASM(SUBwi, probe_reg, probe_reg, 1);
		store_off(value_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			probe_reg, 4);
		label_place(released);
		materialize_constant(
			static_cast<uint64_t>(IS_UNDEF),
			DarwinConfig::GP_BANK, 4, type_reg);
		store_off(frame_reg,
			static_cast<uint32_t>(
				source_offset + offsetof(zval, u1.type_info)),
			type_reg, 4);
		auto done = text_writer.label_create();
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		type.reset();
		value.reset();
		probe.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		label_place(done);
		return true;
	};
	auto read_array = [&]() {
		zend_tpde_array_read layout;

		if (!zend_tpde_array_read_at(mir, &layout)
				|| layout.container_offset > UINT32_MAX - 8
				|| layout.key_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		auto slow = text_writer.label_create();
		auto key_long = text_writer.label_create();
		auto key_ready = text_writer.label_create();
		auto packed = text_writer.label_create();
		auto mixed_loop = text_writer.label_create();
		auto mixed_next = text_writer.label_create();
		auto mixed_string = text_writer.label_create();
		auto mixed_string_loop = text_writer.label_create();
		auto mixed_string_next = text_writer.label_create();
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

		load_off(type_reg, frame_reg,
			layout.container_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::Jne, slow);
		load_off(array_reg, frame_reg, layout.container_offset, 8);

		load_off(type_reg, frame_reg,
			layout.key_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jeq, key_long);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(key_reg, frame_reg, layout.key_offset, 8);
		materialize_constant(
			1, DarwinConfig::GP_BANK, 4, high_word_reg);
		generate_raw_jump(Jump::jmp, key_ready);
		label_place(key_long);
		load_off(key_reg, frame_reg, layout.key_offset, 8);
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, high_word_reg);
		label_place(key_ready);

		load_off(limit_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, limit_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);

		load_off(type_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, u)), 4);
		ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
		generate_raw_jump(Jump::Jne, packed);

		ASM(CMPwi, high_word_reg, 0);
		generate_raw_jump(Jump::Jne, mixed_string);
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
		ASM(ORRw, limit_reg, key_reg, limit_reg);
		ASM(ADDx_sxtw, slot_reg, element_reg, limit_reg, 2);
		load_off(limit_reg, slot_reg, 0, 4);
		label_place(mixed_loop);
		ASM(CMPwi, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, type_reg, key_reg);
		generate_raw_jump(Jump::Jne, mixed_next);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPxi, type_reg, 0);
		generate_raw_jump(Jump::Jeq, found);
		label_place(mixed_next);
		load_off(limit_reg, slot_reg,
			static_cast<uint32_t>(
				offsetof(Bucket, val) + offsetof(zval, u2.next)),
			4);
		generate_raw_jump(Jump::jmp, mixed_loop);

		label_place(mixed_string);
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
		load_off(type_reg, key_reg,
			static_cast<uint32_t>(offsetof(zend_string, h)), 8);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
		ASM(ORRw, limit_reg, type_reg, limit_reg);
		ASM(ADDx_sxtw, slot_reg, element_reg, limit_reg, 2);
		load_off(limit_reg, slot_reg, 0, 4);
		label_place(mixed_string_loop);
		ASM(CMPwi, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
		load_off(high_word_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, high_word_reg, type_reg);
		generate_raw_jump(Jump::Jne, mixed_string_next);
		load_off(high_word_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPx, high_word_reg, key_reg);
		generate_raw_jump(Jump::Jeq, found);
		label_place(mixed_string_next);
		load_off(limit_reg, slot_reg,
			static_cast<uint32_t>(
				offsetof(Bucket, val) + offsetof(zval, u2.next)),
			4);
		generate_raw_jump(Jump::jmp, mixed_string_loop);

		label_place(packed);
		ASM(CMPwi, high_word_reg, 0);
		generate_raw_jump(Jump::Jne, slow);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumUsed)), 4);
		ASM(CMPx, key_reg, limit_reg);
		generate_raw_jump(Jump::Jhs, slow);

		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arPacked)), 8);
		ASM(ADDx_lsl, element_reg, element_reg, key_reg, 4);
		ASM(ORRx, slot_reg, element_reg, element_reg);
		label_place(found);
		/*
		 * Mixed buckets begin with zval, so slot_reg is the value address
		 * for both layouts after the packed path copies its address here.
		 */
		ASM(ORRx, element_reg, slot_reg, slot_reg);
		load_off(type_reg, element_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(CMPwi, type_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);

		load_off(low_word_reg, element_reg, 0, 8);
		load_off(high_word_reg, element_reg, 8, 8);
		store_off(frame_reg, layout.result_offset, low_word_reg, 8);
		store_off(frame_reg, layout.result_offset + 8, high_word_reg, 8);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, done);
		load_off(limit_reg, low_word_reg,
			static_cast<uint32_t>(offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, limit_reg, limit_reg, 1);
		store_off(low_word_reg,
			static_cast<uint32_t>(offsetof(zend_refcounted_h, refcount)),
			limit_reg, 4);
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
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		label_place(done);
		return true;
	};
	auto isset_array = [&]() {
		zend_tpde_array_isset layout;

		if (!zend_tpde_array_isset_at(mir, &layout)
				|| layout.container_offset > UINT32_MAX - 8
				|| layout.key_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() != 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
			}
		}
		auto slow = text_writer.label_create();
		auto key_long = text_writer.label_create();
		auto key_ready = text_writer.label_create();
		auto packed = text_writer.label_create();
		auto mixed_loop = text_writer.label_create();
		auto mixed_next = text_writer.label_create();
		auto mixed_string = text_writer.label_create();
		auto mixed_string_loop = text_writer.label_create();
		auto mixed_string_next = text_writer.label_create();
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
		ScratchReg key_kind{this};
		ScratchReg decision{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto key_reg = key.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto key_kind_reg = key_kind.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.container_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::Jne, slow);
		load_off(array_reg, frame_reg, layout.container_offset, 8);

		load_off(type_reg, frame_reg,
			layout.key_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jeq, key_long);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(key_reg, frame_reg, layout.key_offset, 8);
		materialize_constant(
			1, DarwinConfig::GP_BANK, 4, key_kind_reg);
		generate_raw_jump(Jump::jmp, key_ready);
		label_place(key_long);
		load_off(key_reg, frame_reg, layout.key_offset, 8);
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, key_kind_reg);
		label_place(key_ready);

		load_off(type_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);

		load_off(type_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, u)), 4);
		ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
		generate_raw_jump(Jump::Jne, packed);

		ASM(CMPwi, key_kind_reg, 0);
		generate_raw_jump(Jump::Jne, mixed_string);
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
		ASM(ORRw, limit_reg, key_reg, limit_reg);
		ASM(ADDx_sxtw, slot_reg, element_reg, limit_reg, 2);
		load_off(limit_reg, slot_reg, 0, 4);
		label_place(mixed_loop);
		ASM(CMPwi, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::Jeq, answer_false);
		ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, type_reg, key_reg);
		generate_raw_jump(Jump::Jne, mixed_next);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPxi, type_reg, 0);
		generate_raw_jump(Jump::Jeq, found);
		label_place(mixed_next);
		load_off(limit_reg, slot_reg,
			static_cast<uint32_t>(
				offsetof(Bucket, val) + offsetof(zval, u2.next)),
			4);
		generate_raw_jump(Jump::jmp, mixed_loop);

		label_place(mixed_string);
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
		load_off(type_reg, key_reg,
			static_cast<uint32_t>(offsetof(zend_string, h)), 8);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
		ASM(ORRw, limit_reg, type_reg, limit_reg);
		ASM(ADDx_sxtw, slot_reg, element_reg, limit_reg, 2);
		load_off(limit_reg, slot_reg, 0, 4);
		label_place(mixed_string_loop);
		ASM(CMPwi, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
		load_off(key_kind_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, key_kind_reg, type_reg);
		generate_raw_jump(Jump::Jne, mixed_string_next);
		load_off(key_kind_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPx, key_kind_reg, key_reg);
		generate_raw_jump(Jump::Jeq, found);
		label_place(mixed_string_next);
		load_off(limit_reg, slot_reg,
			static_cast<uint32_t>(
				offsetof(Bucket, val) + offsetof(zval, u2.next)),
			4);
		generate_raw_jump(Jump::jmp, mixed_string_loop);

		label_place(packed);
		ASM(CMPwi, key_kind_reg, 0);
		generate_raw_jump(Jump::Jne, slow);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumUsed)), 4);
		ASM(CMPx, key_reg, limit_reg);
		generate_raw_jump(Jump::Jhs, answer_false);
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arPacked)), 8);
		ASM(ADDx_lsl, element_reg, element_reg, key_reg, 4);
		generate_raw_jump(Jump::jmp, inspect_element);
		label_place(found);
		ASM(ORRx, element_reg, slot_reg, slot_reg);
		label_place(inspect_element);
		load_off(type_reg, element_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jne, not_reference);
		load_off(element_reg, element_reg, 0, 8);
		load_off(type_reg, element_reg,
			static_cast<uint32_t>(
				offsetof(zend_reference, val)
					+ offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		label_place(not_reference);
		ASM(CMPwi, type_reg, IS_NULL);
		generate_raw_jump(Jump::Jhi, answer_true);

		label_place(answer_false);
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 8, element_reg);
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				IS_FALSE, DarwinConfig::GP_BANK, 4, type_reg);
		}
		generate_raw_jump(Jump::jmp, store_answer);
		label_place(answer_true);
		materialize_constant(
			1, DarwinConfig::GP_BANK, 8, element_reg);
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				IS_TRUE, DarwinConfig::GP_BANK, 4, type_reg);
		}
		label_place(store_answer);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(ORRx, result_reg, element_reg, element_reg);
			result.set_modified();
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			store_off(frame_reg, layout.result_offset, element_reg, 8);
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		array.reset();
		key.reset();
		limit.reset();
		element.reset();
		key_kind.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			decision.reset();
			const auto register_state =
				zend::native::tpde::
					capture_conditional_call_register_state(*this);
			ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
			frame_argument.set_value(this, std::move(frame_scratch));
			if (!execute_value_operation(&frame_argument)) {
				return false;
			}
			zend::native::tpde::restore_conditional_call_register_state(
				*this, register_state);
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
				{1, successors[1]},
			}};
			generate_switch(
				std::move(decision), 32, successors[0], cases);
		}
		return true;
	};
	auto append_packed_array = [&]() {
		zend_tpde_packed_array_append layout;

		if (!zend_tpde_packed_array_append_at(mir, &layout)
				|| layout.container_offset > UINT32_MAX - 8
				|| layout.value_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg array{this};
		ScratchReg count{this};
		ScratchReg limit{this};
		ScratchReg element{this};
		ScratchReg low_word{this};
		ScratchReg high_word{this};
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto count_reg = count.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.container_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::Jne, slow);
		load_off(array_reg, frame_reg, layout.container_offset, 8);
		load_off(count_reg, array_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(CMPwi, count_reg, 1);
		generate_raw_jump(Jump::Jne, slow);
		load_off(type_reg, array_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, type_reg, IS_ARRAY_IMMUTABLE);
		generate_raw_jump(Jump::Jne, slow);
		load_off(type_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, u)), 4);
		ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
		generate_raw_jump(Jump::Jeq, slow);
		load_off(count_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumUsed)), 4);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableSize)), 4);
		ASM(CMPw, count_reg, limit_reg);
		generate_raw_jump(Jump::Jhs, slow);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(
				offsetof(HashTable, nNextFreeElement)), 8);
		ASM(CMPx, count_reg, limit_reg);
		generate_raw_jump(Jump::Jne, slow);

		load_off(type_reg, frame_reg,
			layout.value_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, limit_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, limit_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, limit_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, limit_reg, IS_INDIRECT);
		generate_raw_jump(Jump::Jeq, slow);
		if (layout.has_result) {
			load_off(limit_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(CMPwi, limit_reg, IS_UNDEF);
			generate_raw_jump(Jump::Jne, slow);
		}

		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arPacked)), 8);
		ASM(ADDx_lsl, element_reg, element_reg, count_reg, 4);
		load_off(low_word_reg, frame_reg, layout.value_offset, 8);
		load_off(high_word_reg, frame_reg, layout.value_offset + 8, 8);
		store_off(element_reg, 0, low_word_reg, 8);
		store_off(element_reg, 8, high_word_reg, 8);
		if (layout.move_value) {
			materialize_constant(
				static_cast<uint64_t>(IS_UNDEF),
				DarwinConfig::GP_BANK, 4, limit_reg);
			store_off(frame_reg,
				layout.value_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				limit_reg, 4);
		} else {
			auto copied = text_writer.label_create();
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jeq, copied);
			load_off(limit_reg, low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)), 4);
			ASM(ADDwi, limit_reg, limit_reg, 1);
			store_off(low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)),
				limit_reg, 4);
			label_place(copied);
		}
		ASM(ADDwi, count_reg, count_reg, 1);
		store_off(array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumUsed)),
			count_reg, 4);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumOfElements)), 4);
		ASM(ADDwi, limit_reg, limit_reg, 1);
		store_off(array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumOfElements)),
			limit_reg, 4);
		store_off(array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNextFreeElement)),
			count_reg, 8);
		if (layout.has_result) {
			store_off(frame_reg, layout.result_offset, low_word_reg, 8);
			store_off(
				frame_reg, layout.result_offset + 8, high_word_reg, 8);
			auto result_copied = text_writer.label_create();
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jeq, result_copied);
			load_off(limit_reg, low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)), 4);
			ASM(ADDwi, limit_reg, limit_reg, 1);
			store_off(low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)),
				limit_reg, 4);
			label_place(result_copied);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		array.reset();
		count.reset();
		limit.reset();
		element.reset();
		low_word.reset();
		high_word.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		label_place(done);
		return true;
	};
	auto string_length = [&]() {
		zend_tpde_string_length layout;

		if (!zend_tpde_string_length_at(mir, &layout)) {
			return execute_value_operation();
		}
		if (layout.operand_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8
				|| node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() != 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
			return false;
		}
		auto slow = text_writer.label_create();
		auto ready = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg string{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto string_reg = string.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.operand_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(string_reg, frame_reg, layout.operand_offset, 8);

		load_off(type_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		load_off(result_reg, string_reg,
			static_cast<uint32_t>(offsetof(zend_string, len)), 8);
		result.set_modified();
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, ready);

		label_place(slow);
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(ready);
		type.reset();
		string.reset();
		std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
			{1, successors[1]},
		}};
		generate_switch(std::move(decision), 32, successors[0], cases);
		return true;
	};
	auto string_identity = [&]() {
		zend_tpde_string_identity layout;

		if (!zend_tpde_string_identity_at(mir, &layout)
				|| layout.left_offset > UINT32_MAX - 8
				|| layout.right_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() != 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
			}
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg left{this};
		ScratchReg right{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto left_reg = left.alloc_gp();
		auto right_reg = right.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.left_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(left_reg, frame_reg, layout.left_offset, 8);

		load_off(type_reg, frame_reg,
			layout.right_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(right_reg, frame_reg, layout.right_offset, 8);
		ASM(CMPx, left_reg, right_reg);
		generate_raw_jump(Jump::Jne, slow);

		load_off(type_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				materialize_constant(
					layout.inverted ? uint64_t{0} : uint64_t{1},
					DarwinConfig::GP_BANK, 8, payload_reg);
				materialize_constant(
					layout.inverted ? IS_FALSE : IS_TRUE,
					DarwinConfig::GP_BANK, 4, type_info_reg);
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				materialize_constant(
					layout.inverted ? uint64_t{0} : uint64_t{1},
					DarwinConfig::GP_BANK, 8, result_reg);
				result.set_modified();
			}
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			materialize_constant(
				layout.inverted ? uint64_t{0} : uint64_t{1},
				DarwinConfig::GP_BANK, 8, left_reg);
			materialize_constant(
				layout.inverted ? IS_FALSE : IS_TRUE,
				DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_reg, layout.result_offset, left_reg, 8);
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		left.reset();
		right.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			decision.reset();
			const auto register_state =
				zend::native::tpde::
					capture_conditional_call_register_state(*this);
			ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
			frame_argument.set_value(this, std::move(frame_scratch));
			if (!execute_value_operation(&frame_argument)) {
				return false;
			}
			zend::native::tpde::restore_conditional_call_register_state(
				*this, register_state);
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
				{1, successors[1]},
			}};
			generate_switch(
				std::move(decision), 32, successors[0], cases);
		}
		return true;
	};
	auto long_binary = [&]() {
		zend_tpde_long_binary layout;

		if (!zend_tpde_long_binary_at(mir, &layout)
				|| layout.left.offset > UINT32_MAX - 8
				|| layout.right.offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8
				|| !node.has_result
				|| node.operands.size() != 3
				|| node.operands[2]
					!= IRValueRef{Adaptor::FRAME_VALUE}) {
			return string_identity();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() != 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
			}
		}
		auto slow = text_writer.label_create();
		auto retry_or_slow = text_writer.label_create();
		auto retry = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg result_value{this};
		ScratchReg decision{this};
		ScratchReg type{this};
		auto result_reg = result_value.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		load_off(result_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, result_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);

		{
			auto [left_ref, left] =
				val_ref_single(node.operands[0]);
			auto left_reg = left.load_to_reg();
			ASM(ORRx, result_reg, left_reg, left_reg);
		}
		const bool boolean_result =
			layout.source_opcode == ZEND_IS_IDENTICAL
			|| layout.source_opcode == ZEND_IS_NOT_IDENTICAL
			|| layout.source_opcode == ZEND_IS_EQUAL
			|| layout.source_opcode == ZEND_IS_NOT_EQUAL
			|| layout.source_opcode == ZEND_IS_SMALLER
			|| layout.source_opcode == ZEND_IS_SMALLER_OR_EQUAL;
		{
			auto [right_ref, right] =
				val_ref_single(node.operands[1]);
			auto right_reg = right.load_to_reg();
				switch (layout.source_opcode) {
				case ZEND_ADD:
					ASM(ADDSx, result_reg, result_reg, right_reg);
					generate_raw_jump(Jump::Jvs,
						node.kind == Adaptor::InstKind::GuardedFast
							? slow : retry_or_slow);
					break;
				case ZEND_SUB:
					ASM(SUBSx, result_reg, result_reg, right_reg);
					generate_raw_jump(Jump::Jvs,
						node.kind == Adaptor::InstKind::GuardedFast
							? slow : retry_or_slow);
					break;
				case ZEND_BW_OR:
					ASM(ORRx, result_reg, result_reg, right_reg);
					break;
				case ZEND_BW_AND:
					ASM(ANDx, result_reg, result_reg, right_reg);
					break;
				case ZEND_BW_XOR:
					ASM(EORx, result_reg, result_reg, right_reg);
					break;
				case ZEND_IS_IDENTICAL:
				case ZEND_IS_EQUAL:
					ASM(CMPx, result_reg, right_reg);
					generate_raw_set(Jump::Jeq, result_reg);
					break;
				case ZEND_IS_NOT_IDENTICAL:
				case ZEND_IS_NOT_EQUAL:
					ASM(CMPx, result_reg, right_reg);
					generate_raw_set(Jump::Jne, result_reg);
					break;
				case ZEND_IS_SMALLER:
					ASM(CMPx, result_reg, right_reg);
					generate_raw_set(Jump::Jlt, result_reg);
					break;
				case ZEND_IS_SMALLER_OR_EQUAL:
					ASM(CMPx, result_reg, right_reg);
					generate_raw_set(Jump::Jle, result_reg);
					break;
				default:
					return false;
			}
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			auto [fast_result_ref, fast_result] =
				result_ref_single(node.result);
			auto fast_result_reg = fast_result.alloc_reg();
			ASM(ORRx, fast_result_reg, result_reg, result_reg);
			fast_result.set_modified();
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			store_off(frame_reg, layout.result_offset, result_reg, 8);
			auto type_reg = type.alloc_gp();
			if (boolean_result) {
				ASM(ADDwi, type_reg, result_reg, IS_FALSE);
			} else {
				materialize_constant(
					static_cast<uint64_t>(IS_LONG),
					DarwinConfig::GP_BANK, 4, type_reg);
			}
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		generate_raw_jump(Jump::jmp, done);

		/*
		 * A private scalar frame is marked by call == 1.  It has not been
		 * published and the arithmetic result is still UNDEF, so retrying the
		 * complete call through the canonical Zend-frame path is atomic.
		 */
		label_place(retry_or_slow);
		load_off(result_reg, frame_reg,
			static_cast<uint32_t>(offsetof(zend_execute_data, call)), 8);
		ASM(CMPxi, result_reg, 1);
		generate_raw_jump(Jump::Jne, slow);
		generate_raw_jump(Jump::jmp, retry);

		label_place(slow);
		type.reset();
		result_value.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
			generate_raw_jump(Jump::jmp, done);
		} else {
		ScratchReg materialized_type{this};
		auto materialized_type_reg = materialized_type.alloc_gp();
		materialize_constant(
			static_cast<uint64_t>(IS_LONG),
			DarwinConfig::GP_BANK, 4, materialized_type_reg);
		if (!layout.left.literal) {
			auto [left_ref, left] =
				val_ref_single(node.operands[0]);
			store_off(frame_reg, layout.left.offset,
				left.load_to_reg(), 8);
			store_off(frame_reg,
				layout.left.offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				materialized_type_reg, 4);
		}
		if (!layout.right.literal) {
			auto [right_ref, right] =
				val_ref_single(node.operands[1]);
			store_off(frame_reg, layout.right.offset,
				right.load_to_reg(), 8);
			store_off(frame_reg,
				layout.right.offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				materialized_type_reg, 4);
		}
		materialized_type.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		generate_raw_jump(Jump::jmp, done);
		}

		label_place(retry);
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETRY, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
				{1, successors[1]},
			}};
			generate_switch(
				std::move(decision), 32, successors[0], cases);
		} else {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			load_off(result_reg, frame_reg,
				layout.result_offset, 8);
			result.set_modified();
		}
		return true;
	};
	auto long_assign_op = [&]() {
		zend_tpde_long_assign_op layout;

		if (!zend_tpde_long_assign_op_at(mir, &layout)
				|| layout.left_offset > UINT32_MAX - 8
				|| layout.right.offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() != 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index
					|| layout.has_result != node.has_result) {
				return false;
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
		ScratchReg left{this};
		ScratchReg right{this};
		ScratchReg decision{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto left_reg = left.alloc_gp();
		auto right_reg = right.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.left_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jne, slow);
		load_off(left_reg, frame_reg, layout.left_offset, 8);

		if (layout.right.literal) {
			load_off(slot_reg, frame_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, func)), 8);
			load_off(slot_reg, slot_reg,
				static_cast<uint32_t>(
					offsetof(zend_op_array, literals)), 8);
			add_unsigned_offset(slot_reg, slot_reg, layout.right.offset);
		} else {
			load_off(type_reg, frame_reg,
				layout.right.offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
			load_off(right_reg, frame_reg, layout.right.offset, 8);
		}
		if (layout.right.literal) {
			load_off(type_reg, slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
			load_off(right_reg, slot_reg, 0, 8);
		}

		if (layout.has_result) {
			load_off(type_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}

		switch (layout.source_opcode) {
			case ZEND_ADD:
				ASM(ADDSx, left_reg, left_reg, right_reg);
				generate_raw_jump(Jump::Jvs, slow);
				break;
			case ZEND_SUB:
				ASM(SUBSx, left_reg, left_reg, right_reg);
				generate_raw_jump(Jump::Jvs, slow);
				break;
			case ZEND_BW_OR:
				ASM(ORRx, left_reg, left_reg, right_reg);
				break;
			case ZEND_BW_AND:
				ASM(ANDx, left_reg, left_reg, right_reg);
				break;
			case ZEND_BW_XOR:
				ASM(EORx, left_reg, left_reg, right_reg);
				break;
			default:
				return false;
		}
		store_off(frame_reg, layout.left_offset, left_reg, 8);
		materialize_constant(
			static_cast<uint64_t>(IS_LONG),
			DarwinConfig::GP_BANK, 4, type_reg);
		store_off(frame_reg,
			layout.left_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			type_reg, 4);
		if (layout.has_result) {
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(ORRx, result_reg, left_reg, left_reg);
				result.set_modified();
			} else {
				store_off(frame_reg, layout.result_offset, left_reg, 8);
				store_off(frame_reg,
					layout.result_offset
						+ static_cast<uint32_t>(
							offsetof(zval, u1.type_info)),
					type_reg, 4);
			}
		}
		if (layout.consume_right) {
			materialize_constant(
				static_cast<uint64_t>(IS_UNDEF),
				DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_reg,
				layout.right.offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		left.reset();
		right.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			decision.reset();
			const auto register_state =
				zend::native::tpde::
					capture_conditional_call_register_state(*this);
			ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
			frame_argument.set_value(this, std::move(frame_scratch));
			if (!execute_value_operation(&frame_argument)) {
				return false;
			}
			zend::native::tpde::restore_conditional_call_register_state(
				*this, register_state);
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
				{1, successors[1]},
			}};
			generate_switch(
				std::move(decision), 32, successors[0], cases);
		}
		return true;
	};
	auto long_incdec = [&]() {
		zend_tpde_long_incdec layout;

		if (!zend_tpde_long_incdec_at(mir, &layout)
				|| layout.operand_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() != 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index
					|| layout.has_result != node.has_result) {
				return false;
			}
		}
		auto slow = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg limit{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.operand_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jne, slow);
		load_off(value_reg, frame_reg, layout.operand_offset, 8);
		materialize_constant(
			layout.increment
				? static_cast<uint64_t>(ZEND_LONG_MAX)
				: static_cast<uint64_t>(ZEND_LONG_MIN),
			DarwinConfig::GP_BANK, 8, limit_reg);
		ASM(CMPx, value_reg, limit_reg);
		generate_raw_jump(Jump::Jeq, slow);

		if (layout.has_result) {
			load_off(type_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
			if (layout.post) {
				if (node.kind == Adaptor::InstKind::GuardedFast) {
					auto [result_ref, result] =
						result_ref_single(node.result);
					auto result_reg = result.alloc_reg();
					ASM(ORRx, result_reg, value_reg, value_reg);
					result.set_modified();
				} else {
					store_off(
						frame_reg, layout.result_offset, value_reg, 8);
				}
			}
		}
		if (layout.increment) {
			ASM(ADDxi, value_reg, value_reg, 1);
		} else {
			ASM(SUBxi, value_reg, value_reg, 1);
		}
		store_off(frame_reg, layout.operand_offset, value_reg, 8);
		if (layout.has_result) {
			if (!layout.post) {
				if (node.kind == Adaptor::InstKind::GuardedFast) {
					auto [result_ref, result] =
						result_ref_single(node.result);
					auto result_reg = result.alloc_reg();
					ASM(ORRx, result_reg, value_reg, value_reg);
					result.set_modified();
				} else {
					store_off(
						frame_reg, layout.result_offset, value_reg, 8);
				}
			}
			if (node.kind != Adaptor::InstKind::GuardedFast) {
				materialize_constant(
					static_cast<uint64_t>(IS_LONG),
					DarwinConfig::GP_BANK, 4, type_reg);
				store_off(frame_reg,
					layout.result_offset
						+ static_cast<uint32_t>(
							offsetof(zval, u1.type_info)),
					type_reg, 4);
			}
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		value.reset();
		limit.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			decision.reset();
			const auto register_state =
				zend::native::tpde::
					capture_conditional_call_register_state(*this);
			ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
			frame_argument.set_value(this, std::move(frame_scratch));
			if (!execute_value_operation(&frame_argument)) {
				return false;
			}
			zend::native::tpde::restore_conditional_call_register_state(
				*this, register_state);
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
				{1, successors[1]},
			}};
			generate_switch(
				std::move(decision), 32, successors[0], cases);
		}
		return true;
	};
	auto slot_isset_empty = [&]() {
		zend_tpde_slot_isset_empty layout;

		if (!zend_tpde_slot_isset_empty_at(mir, &layout)
				|| layout.operand_offset > UINT32_C(4095)
				|| layout.result_offset > UINT32_C(4095)) {
			return execute_value_operation();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() != 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
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
		ScratchReg type{this};
		ScratchReg value{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.operand_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_NULL);
		generate_raw_jump(Jump::Jle, falsey);
		ASM(CMPwi, type_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);
		if (!layout.is_empty) {
			generate_raw_jump(Jump::jmp, truthy);
		} else {
			ASM(CMPwi, type_reg, IS_FALSE);
			generate_raw_jump(Jump::Jeq, falsey);
			ASM(CMPwi, type_reg, IS_TRUE);
			generate_raw_jump(Jump::Jeq, truthy);
			ASM(CMPwi, type_reg, IS_LONG);
			auto not_long = text_writer.label_create();
			generate_raw_jump(Jump::Jne, not_long);
			load_off(value_reg, frame_reg, layout.operand_offset, 8);
			generate_raw_jump(
				Jump{Jump::Cbnz, value_reg, false}, truthy);
			generate_raw_jump(Jump::jmp, falsey);

			label_place(not_long);
			ASM(CMPwi, type_reg, IS_STRING);
			auto not_string = text_writer.label_create();
			generate_raw_jump(Jump::Jne, not_string);
			load_off(value_reg, frame_reg, layout.operand_offset, 8);
			load_off(type_reg, value_reg,
				static_cast<uint32_t>(offsetof(zend_string, len)), 8);
			generate_raw_jump(
				Jump{Jump::Cbz, type_reg, false}, falsey);
			ASM(CMPxi, type_reg, 1);
			generate_raw_jump(Jump::Jne, truthy);
			load_off(type_reg, value_reg,
				static_cast<uint32_t>(offsetof(zend_string, val)), 1);
			ASM(CMPwi, type_reg, '0');
			generate_raw_jump(Jump::Jeq, falsey);
			generate_raw_jump(Jump::jmp, truthy);

			label_place(not_string);
			ASM(CMPwi, type_reg, IS_ARRAY);
			auto not_array = text_writer.label_create();
			generate_raw_jump(Jump::Jne, not_array);
			load_off(value_reg, frame_reg, layout.operand_offset, 8);
			load_off(type_reg, value_reg,
				static_cast<uint32_t>(
					offsetof(HashTable, nNumOfElements)), 4);
			generate_raw_jump(
				Jump{Jump::Cbnz, type_reg, false}, truthy);
			generate_raw_jump(Jump::jmp, falsey);

			label_place(not_array);
			ASM(CMPwi, type_reg, IS_RESOURCE);
			auto not_resource = text_writer.label_create();
			generate_raw_jump(Jump::Jne, not_resource);
			load_off(value_reg, frame_reg, layout.operand_offset, 8);
			load_off(type_reg, value_reg,
				static_cast<uint32_t>(
					offsetof(zend_resource, handle)), 4);
			generate_raw_jump(
				Jump{Jump::Cbnz, type_reg, false}, truthy);
			generate_raw_jump(Jump::jmp, falsey);
			label_place(not_resource);
			generate_raw_jump(Jump::jmp, slow);
		}

		label_place(truthy);
		materialize_constant(
			static_cast<uint64_t>(
				node.kind == Adaptor::InstKind::GuardedFast
					? (layout.is_empty ? 0 : 1)
					: (layout.is_empty ? IS_FALSE : IS_TRUE)),
			DarwinConfig::GP_BANK, 4, type_reg);
		generate_raw_jump(Jump::jmp, store);
		label_place(falsey);
		materialize_constant(
			static_cast<uint64_t>(
				node.kind == Adaptor::InstKind::GuardedFast
					? (layout.is_empty ? 1 : 0)
					: (layout.is_empty ? IS_TRUE : IS_FALSE)),
			DarwinConfig::GP_BANK, 4, type_reg);
		label_place(store);
		load_off(value_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, value_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(ORRx, result_reg, type_reg, type_reg);
			result.set_modified();
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		value.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			decision.reset();
			const auto register_state =
				zend::native::tpde::
					capture_conditional_call_register_state(*this);
			ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
			frame_argument.set_value(this, std::move(frame_scratch));
			if (!execute_value_operation(&frame_argument)) {
				return false;
			}
			zend::native::tpde::restore_conditional_call_register_state(
				*this, register_state);
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
				{1, successors[1]},
			}};
			generate_switch(
				std::move(decision), 32, successors[0], cases);
		}
		return true;
	};
	auto object_property_read = [&]() {
		zend_tpde_object_property_read layout;

		if (!zend_tpde_object_property_read_at(mir, &layout)) {
			return execute_value_operation();
		}
		auto slow = text_writer.label_create();
		auto copied = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg object{this};
		ScratchReg cache{this};
		ScratchReg offset{this};
		ScratchReg property{this};
		ScratchReg type{this};
		ScratchReg low_word{this};
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.receiver_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_OBJECT);
		generate_raw_jump(Jump::Jne, slow);
		load_off(object_reg, frame_reg, layout.receiver_offset, 8);
		load_off(cache_reg, frame_reg,
			static_cast<uint32_t>(
				offsetof(zend_execute_data, run_time_cache)), 8);
		generate_raw_jump(
			Jump{Jump::Cbz, cache_reg, false}, slow);
		load_off(type_reg, object_reg,
			static_cast<uint32_t>(offsetof(zend_object, ce)), 8);
		load_off(property_reg, cache_reg, layout.cache_offset, 8);
		ASM(CMPx, type_reg, property_reg);
		generate_raw_jump(Jump::Jne, slow);
		load_off(offset_reg, cache_reg,
			layout.cache_offset + sizeof(void *), 8);
		ASM(CMPxi, offset_reg, ZEND_FIRST_PROPERTY_OFFSET);
		generate_raw_jump(Jump::Jlt, slow);
		ASM(ADDx, property_reg, object_reg, offset_reg);
		load_off(type_reg, property_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, offset_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);

		load_off(offset_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, offset_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		load_off(low_word_reg, property_reg, 0, 8);
		store_off(frame_reg, layout.result_offset, low_word_reg, 8);
		store_off(frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			type_reg, 4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, copied);
		load_off(offset_reg, low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, offset_reg, offset_reg, 1);
		store_off(low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			offset_reg, 4);
		label_place(copied);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		low_word.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		label_place(done);
		return true;
	};
	auto object_property_write = [&]() {
		zend_tpde_object_property_write layout;

		if (!zend_tpde_object_property_write_at(mir, &layout)) {
			return execute_value_operation();
		}
		auto slow = text_writer.label_create();
		auto old_released = text_writer.label_create();
		auto value_owned = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg object{this};
		ScratchReg cache{this};
		ScratchReg offset{this};
		ScratchReg property{this};
		ScratchReg type{this};
		ScratchReg low_word{this};
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();

		load_off(type_reg, frame_reg,
			layout.receiver_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_OBJECT);
		generate_raw_jump(Jump::Jne, slow);
		load_off(object_reg, frame_reg, layout.receiver_offset, 8);
		load_off(offset_reg, object_reg,
			static_cast<uint32_t>(offsetof(zend_object, extra_flags)), 4);
		ASM(TSTwi, offset_reg,
			IS_OBJ_LAZY_UNINITIALIZED | IS_OBJ_LAZY_PROXY);
		generate_raw_jump(Jump::Jne, slow);
		load_off(cache_reg, frame_reg,
			static_cast<uint32_t>(
				offsetof(zend_execute_data, run_time_cache)), 8);
		generate_raw_jump(
			Jump{Jump::Cbz, cache_reg, false}, slow);
		load_off(type_reg, object_reg,
			static_cast<uint32_t>(offsetof(zend_object, ce)), 8);
		load_off(offset_reg, type_reg,
			static_cast<uint32_t>(
				offsetof(zend_class_entry, create_object)), 8);
		generate_raw_jump(
			Jump{Jump::Cbnz, offset_reg, false}, slow);
		load_off(property_reg, cache_reg, layout.cache_offset, 8);
		ASM(CMPx, type_reg, property_reg);
		generate_raw_jump(Jump::Jne, slow);
		load_off(offset_reg, cache_reg,
			layout.cache_offset + sizeof(void *), 8);
		ASM(CMPxi, offset_reg, ZEND_FIRST_PROPERTY_OFFSET);
		generate_raw_jump(Jump::Jlt, slow);
		load_off(type_reg, cache_reg,
			layout.cache_offset + 2 * sizeof(void *), 8);
		generate_raw_jump(
			Jump{Jump::Cbnz, type_reg, false}, slow);
		ASM(ADDx, property_reg, object_reg, offset_reg);
		load_off(type_reg, property_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, offset_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);

		load_off(type_reg, frame_reg,
			layout.value_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, offset_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);

		load_off(offset_reg, property_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(TSTwi, offset_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, old_released);
		load_off(cache_reg, property_reg, 0, 8);
		load_off(offset_reg, cache_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(CMPwi, offset_reg, 1);
		generate_raw_jump(Jump::Jle, slow);
		ASM(SUBwi, offset_reg, offset_reg, 1);
		store_off(cache_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			offset_reg, 4);
		label_place(old_released);

		load_off(low_word_reg, frame_reg, layout.value_offset, 8);
		if (!layout.move_value) {
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jeq, value_owned);
			load_off(offset_reg, low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)), 4);
			ASM(ADDwi, offset_reg, offset_reg, 1);
			store_off(low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)),
				offset_reg, 4);
			label_place(value_owned);
		}
		store_off(property_reg, 0, low_word_reg, 8);
		store_off(property_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			type_reg, 4);
		if (layout.move_value) {
			materialize_constant(
				static_cast<uint64_t>(IS_UNDEF),
				DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_reg,
				layout.value_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		low_word.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		label_place(done);
		return true;
	};
	auto dynamic_fetch_read = [&]() {
		zend_tpde_dynamic_fetch_read layout;

		if (!zend_tpde_dynamic_fetch_read_at(mir, &layout)) {
			return execute_value_operation();
		}
		auto slow = text_writer.label_create();
		auto loop = text_writer.label_create();
		auto next = text_writer.label_create();
		auto value_ready = text_writer.label_create();
		auto done = text_writer.label_create();
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg slot{this};
		ScratchReg type{this};
		ScratchReg table{this};
		ScratchReg name{this};
		ScratchReg index{this};
		ScratchReg bucket{this};
		ScratchReg low_word{this};
		ScratchReg high_word{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto table_reg = table.alloc_gp();
		auto name_reg = name.alloc_gp();
		auto index_reg = index.alloc_gp();
		auto bucket_reg = bucket.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();

		load_off(type_reg, frame_reg,
			static_cast<uint32_t>(
				offsetof(zend_execute_data, This)
					+ offsetof(zval, u1.type_info)), 4);
		ASM(TSTwi, type_reg, ZEND_CALL_HAS_SYMBOL_TABLE);
		generate_raw_jump(Jump::Jeq, slow);
		load_off(table_reg, frame_reg,
			static_cast<uint32_t>(
				offsetof(zend_execute_data, symbol_table)), 8);
		ASM(CMPxi, table_reg, 0);
		generate_raw_jump(Jump::Jeq, slow);

		load_off(type_reg, frame_reg,
			layout.name_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(name_reg, frame_reg, layout.name_offset, 8);

		load_off(type_reg, frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);

		load_off(bucket_reg, table_reg,
			static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
		load_off(type_reg, name_reg,
			static_cast<uint32_t>(offsetof(zend_string, h)), 8);
		load_off(index_reg, table_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
		ASM(ORRw, index_reg, type_reg, index_reg);
		ASM(ADDx_sxtw, slot_reg, bucket_reg, index_reg, 2);
		load_off(index_reg, slot_reg, 0, 4);
		label_place(loop);
		ASM(CMPwi, index_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(ADDx_lsl, slot_reg, bucket_reg, index_reg, 5);
		load_off(high_word_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, high_word_reg, type_reg);
		generate_raw_jump(Jump::Jne, next);
		load_off(high_word_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPx, high_word_reg, name_reg);
		generate_raw_jump(Jump::Jeq, value_ready);
		label_place(next);
		load_off(index_reg, slot_reg,
			static_cast<uint32_t>(
				offsetof(Bucket, val) + offsetof(zval, u2.next)), 4);
		generate_raw_jump(Jump::jmp, loop);

		label_place(value_ready);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, index_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, index_reg, IS_INDIRECT);
		auto not_indirect = text_writer.label_create();
		generate_raw_jump(Jump::Jne, not_indirect);
		load_off(slot_reg, slot_reg, 0, 8);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		label_place(not_indirect);
		ASM(ANDwi, index_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, index_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);

		load_off(low_word_reg, slot_reg, 0, 8);
		load_off(high_word_reg, slot_reg, 8, 8);
		store_off(frame_reg, layout.result_offset, low_word_reg, 8);
		store_off(frame_reg, layout.result_offset + 8, high_word_reg, 8);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, done);
		load_off(index_reg, low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, index_reg, index_reg, 1);
		store_off(low_word_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			index_reg, 4);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		slot.reset();
		type.reset();
		table.reset();
		name.reset();
		index.reset();
		bucket.reset();
		low_word.reset();
		high_word.reset();
		const auto register_state =
			zend::native::tpde::
				capture_conditional_call_register_state(*this);
		ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
		frame_argument.set_value(this, std::move(frame_scratch));
		if (!execute_value_operation(&frame_argument)) {
			return false;
		}
		zend::native::tpde::restore_conditional_call_register_state(
			*this, register_state);
		label_place(done);
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
					node.kind == Adaptor::InstKind::ZvalMove);
			}
			return false;
		case Adaptor::InstKind::ZvalReleaseFast:
			return record.opcode == ZEND_MIR_OPCODE_VALUE_FREE
				? free_temporary_slot() : false;
		case Adaptor::InstKind::SlowPathCall:
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
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_FETCH_R) {
			return object_property_read();
		}
		if (record.opcode == ZEND_MIR_OPCODE_OBJECT_ASSIGN) {
			return object_property_write();
		}
		if (record.opcode == ZEND_MIR_OPCODE_DYNAMIC_FETCH_R) {
			return dynamic_fetch_read();
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
			return copy_slot(
				mir.value_operation.op1,
				mir.value_operation.op1_storage_id,
				mir.value_operation.result_storage_id,
				ZEND_MIR_ID_INVALID,
				mir.value_operation.op1.slot_kind
					== ZEND_MIR_SOURCE_SLOT_TMP
					|| mir.value_operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_VAR);
		case ZEND_MIR_OPCODE_VALUE_CONCAT:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FAST_CONCAT:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ROPE_INIT:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ROPE_ADD:
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
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_RW:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_IS:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_FUNC_ARG:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_FETCH_DIM_UNSET:
			return execute_value_operation();
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
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP:
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
			if (mir.runtime_helper == ZEND_NATIVE_HELPER_COUNT) {
				return true;
			}
			return execute_value_operation();
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
			if (result_offset > UINT32_MAX - offsetof(zval, u1.type_info)) {
				return false;
			}
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto [result_ref, result] = result_ref_single(node.result);
			auto frame_reg = frame.load_to_reg();
			auto result_reg = result.alloc_reg();
			load_off(result_reg, frame_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, This)
						+ offsetof(zval, u2.num_args)),
				4);
			store_off(frame_reg, static_cast<uint32_t>(result_offset),
				result_reg, 8);
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			materialize_constant(
				IS_LONG, DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_reg,
				static_cast<uint32_t>(
					result_offset + offsetof(zval, u1.type_info)),
				type_reg, 4);
			result.set_modified();
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
				auto done = text_writer.label_create();
				auto slow = text_writer.label_create();
				auto [context_ref, context] =
					val_ref_single(node.operands[1]);
				auto context_scratch =
					std::move(context).into_scratch();
				ScratchReg pending{this};
				auto pending_reg = pending.alloc_gp();
				load_off(pending_reg, context_scratch.cur_reg(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_interrupt)), 8);
				load_off(pending_reg, pending_reg, 0, 1);
				ASM(CMPxi, pending_reg, 0);
				generate_raw_jump(Jump::Jne, slow);
				generate_raw_jump(Jump::jmp, done);
				label_place(slow);
				context_scratch.reset();
				pending.reset();
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[0]});
				builder.add_arg(ValuePart{mir.source_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_INTERRUPT_POLL));
				label_place(done);
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
			generate_raw_set(record.opcode == ZEND_MIR_OPCODE_I1_NOT
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
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			auto [result_ref, result] = result_ref_single(node.result);
			auto left_reg = left.load_to_reg();
			auto right_reg = right.load_to_reg();
			auto result_reg = result.alloc_try_reuse(left);
			if (record.opcode == ZEND_MIR_OPCODE_I64_SHL_CHECKED) {
				ASM(LSLVx, result_reg, left_reg, right_reg);
			} else {
				ASM(ASRVx, result_reg, left_reg, right_reg);
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
				adaptor->block_ref(record.block_id))[0]);
			return true;
		case ZEND_MIR_OPCODE_COND_BRANCH: {
			auto [condition_ref, condition] = unary();
			auto condition_reg = condition.load_to_reg();
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
			generate_cond_branch(Jump{Jump::Cbnz, condition_reg, false},
				successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH: {
			zend_tpde_multi_branch layout;
			if (node.operands.size() != 1
					|| !zend_tpde_multi_branch_at(
						adaptor->plan(), mir, record, &layout)) {
				return false;
			}
			const zend_tpde_plan *plan = adaptor->plan();
			std::vector<IRBlockRef> targets;
			std::vector<::tpde::Label> case_labels;
			targets.reserve(layout.successor_count);
			case_labels.reserve(zend_hash_num_elements(layout.jump_table));
			for (uint32_t i = 0; i < layout.successor_count; ++i) {
				zend_mir_block_id target_id;
				if (!plan->view->successor_at(
						plan->view->context, record.block_id, i,
						&target_id)) {
					return false;
				}
				IRBlockRef target = adaptor->block_ref(target_id);
				if (target == Adaptor::INVALID_BLOCK_REF) {
					return false;
				}
				targets.push_back(target);
			}
			for (uint32_t i = 0;
					i < zend_hash_num_elements(layout.jump_table); ++i) {
				case_labels.push_back(text_writer.label_create());
			}
			auto default_label = text_writer.label_create();
			auto fallback_label = layout.source_opcode == ZEND_MATCH
				? default_label : text_writer.label_create();
			auto long_label = text_writer.label_create();
			auto string_label = text_writer.label_create();
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_scratch = std::move(frame).into_scratch();
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
			add_unsigned_offset(
				slot_reg, frame_scratch.cur_reg(), layout.operand_offset);
			load_off(type_reg, slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			auto spilled = spill_before_branch();
			begin_branch_region();
			auto dereferenced = text_writer.label_create();
			ASM(CMPwi, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::Jne, dereferenced);
			load_off(slot_reg, slot_reg, 0, 8);
			ASM(ADDxi, slot_reg, slot_reg,
				static_cast<uint32_t>(offsetof(zend_reference, val)));
			load_off(type_reg, slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			label_place(dereferenced);
			if (layout.source_opcode != ZEND_SWITCH_STRING) {
				ASM(CMPwi, type_reg, IS_LONG);
				generate_raw_jump(Jump::Jeq, long_label);
			}
			if (layout.source_opcode != ZEND_SWITCH_LONG) {
				ASM(CMPwi, type_reg, IS_STRING);
				generate_raw_jump(Jump::Jeq, string_label);
			}
			generate_raw_jump(Jump::jmp, fallback_label);

			uint32_t case_index = 0;
			zend_ulong numeric_key;
			zend_string *string_key;
			zval *jump_value;
			label_place(long_label);
			load_off(value_reg, slot_reg, 0, 8);
			emit_integer_dispatch(layout.jump_table, case_labels,
				value_reg, constant_reg, default_label);

			label_place(string_label);
			load_off(value_reg, slot_reg, 0, 8);
			case_index = 0;
			ZEND_HASH_FOREACH_KEY_VAL(
					layout.jump_table, numeric_key, string_key, jump_value) {
				if (string_key != nullptr) {
					auto next_case = text_writer.label_create();
					const uint64_t length = ZSTR_LEN(string_key);
					load_off(probe_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(zend_string, len)), 8);
					materialize_constant(
						&length, DarwinConfig::GP_BANK, 8,
						constant_reg);
					ASM(CMPx, probe_reg, constant_reg);
					generate_raw_jump(Jump::Jne, next_case);
					size_t offset = 0;
					while (offset < ZSTR_LEN(string_key)) {
						const uint32_t width =
							ZSTR_LEN(string_key) - offset >= 8 ? 8
							: ZSTR_LEN(string_key) - offset >= 4 ? 4
							: ZSTR_LEN(string_key) - offset >= 2 ? 2 : 1;
						uint64_t expected = 0;
						memcpy(&expected, ZSTR_VAL(string_key) + offset,
							width);
						load_off(probe_reg, value_reg,
							static_cast<uint32_t>(
								offsetof(zend_string, val) + offset),
							width);
						materialize_constant(
							&expected, DarwinConfig::GP_BANK,
							width, constant_reg);
						ASM(CMPx, probe_reg, constant_reg);
						generate_raw_jump(Jump::Jne, next_case);
						offset += width;
					}
					generate_raw_jump(
						Jump::jmp, case_labels[case_index]);
					label_place(next_case);
				}
				case_index++;
			} ZEND_HASH_FOREACH_END();
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
			if (node.operands.size() != 1 || !mir.has_value_operation) {
				return false;
			}
			if (record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH) {
				zend_tpde_value_condition layout;

				if (zend_tpde_value_condition_at(mir, &layout)) {
					const int32_t decision_slot =
						allocate_stack_slot(sizeof(uint32_t));
					if (decision_slot < 0) {
						return false;
					}
					auto slow = text_writer.label_create();
					auto truthy = text_writer.label_create();
					auto falsey = text_writer.label_create();
					auto fast_ready = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = std::move(frame).into_scratch();
					auto frame_reg = frame_scratch.cur_reg();
					ScratchReg type{this};
					ScratchReg value{this};
					auto type_reg = type.alloc_gp();
					auto value_reg = value.alloc_gp();

					load_off(type_reg, frame_reg,
						layout.operand_offset
							+ static_cast<uint32_t>(
								offsetof(zval, u1.type_info)),
						4);
					ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_reg, IS_NULL);
					generate_raw_jump(Jump::Jeq, falsey);
					ASM(CMPwi, type_reg, IS_FALSE);
					generate_raw_jump(Jump::Jeq, falsey);
					ASM(CMPwi, type_reg, IS_TRUE);
					generate_raw_jump(Jump::Jeq, truthy);
					ASM(CMPwi, type_reg, IS_LONG);
					auto not_long = text_writer.label_create();
					generate_raw_jump(Jump::Jne, not_long);
					load_off(value_reg, frame_reg,
						layout.operand_offset, 8);
					generate_raw_jump(
						Jump{Jump::Cbnz, value_reg, false}, truthy);
					generate_raw_jump(Jump::jmp, falsey);

					label_place(not_long);
					ASM(CMPwi, type_reg, IS_STRING);
					auto not_string = text_writer.label_create();
					generate_raw_jump(Jump::Jne, not_string);
					load_off(value_reg, frame_reg,
						layout.operand_offset, 8);
					load_off(type_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(zend_string, len)), 8);
					generate_raw_jump(
						Jump{Jump::Cbz, type_reg, false}, falsey);
					ASM(CMPxi, type_reg, 1);
					generate_raw_jump(Jump::Jne, truthy);
					load_off(type_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(zend_string, val)), 1);
					ASM(CMPwi, type_reg, '0');
					generate_raw_jump(Jump::Jeq, falsey);
					generate_raw_jump(Jump::jmp, truthy);

					label_place(not_string);
					ASM(CMPwi, type_reg, IS_ARRAY);
					auto not_array = text_writer.label_create();
					generate_raw_jump(Jump::Jne, not_array);
					load_off(value_reg, frame_reg,
						layout.operand_offset, 8);
					load_off(type_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, nNumOfElements)), 4);
					generate_raw_jump(
						Jump{Jump::Cbnz, type_reg, false}, truthy);
					generate_raw_jump(Jump::jmp, falsey);
					label_place(not_array);
					ASM(CMPwi, type_reg, IS_RESOURCE);
					generate_raw_jump(Jump::Jne, slow);
					load_off(value_reg, frame_reg,
						layout.operand_offset, 8);
					load_off(type_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(zend_resource, handle)), 4);
					generate_raw_jump(
						Jump{Jump::Cbnz, type_reg, false}, truthy);
					generate_raw_jump(Jump::jmp, falsey);

					label_place(truthy);
					materialize_constant(
						uint64_t{1}, DarwinConfig::GP_BANK, 4, type_reg);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot),
						type_reg, 4);
					generate_raw_jump(Jump::jmp, fast_ready);
					label_place(falsey);
					materialize_constant(
						uint64_t{0}, DarwinConfig::GP_BANK, 4, type_reg);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot),
						type_reg, 4);
					label_place(fast_ready);
					type.reset();
					value.reset();
					const auto &successors = adaptor->block_succs(
						adaptor->block_ref(record.block_id));
					generate_raw_jump(Jump::jmp, branch);
					label_place(slow);

					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						DarwinConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, std::move(frame_scratch));
					builder.add_arg(
						std::move(frame_argument),
						::tpde::CCAssignment{});
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					builder.add_arg(ValuePart{
						zend_tpde_encode_value_operand(operation.op1), 8,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.add_arg(ValuePart{
						zend_tpde_encode_value_operand(operation.op2), 8,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.add_arg(ValuePart{
						zend_tpde_encode_value_operand(operation.result), 8,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.add_arg(ValuePart{operation.extended_value, 4,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.add_arg(ValuePart{operation.source_opcode, 4,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.add_arg(ValuePart{operation.source_position_id, 4,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.call(runtime_symbol(mir.runtime_helper));
					ValuePart decision{DarwinConfig::GP_BANK, 4};
					builder.add_ret(
						decision, ::tpde::CCAssignment{});
					auto decision_reg =
						decision.cur_reg_or_load(this);
					ASM(CMPxi, decision_reg,
						ZEND_NATIVE_ITERATOR_EXCEPTION);
					auto valid = text_writer.label_create();
					generate_raw_jump(Jump::Jlt, valid);
					decision.reset(this);
					RetBuilder return_builder{
						*this, *cur_cc_assigner()};
					return_builder.add(ValuePart{
						ZEND_NATIVE_EXCEPTION, 4,
						DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					return_builder.ret();
					label_place(valid);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot),
						decision_reg, 4);
					decision.reset(this);
					generate_raw_jump(Jump::jmp, branch);
					label_place(branch);
					ScratchReg branch_decision{this};
					auto branch_decision_reg =
						branch_decision.alloc_gp();
					load_off(branch_decision_reg,
						AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot), 4);
					generate_cond_branch(
						Jump{Jump::Cbnz, branch_decision_reg, false},
						successors[0], successors[1]);
					return true;
				}
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			const zend_mir_executable_value_ref &operation =
				mir.value_operation;
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(operation.op1), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(operation.op2), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(operation.result), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.extended_value, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_opcode, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(mir.runtime_helper));
			ValuePart decision{DarwinConfig::GP_BANK, 4};
			builder.add_ret(decision, ::tpde::CCAssignment{});
			auto decision_reg = decision.cur_reg_or_load(this);
			ASM(CMPxi, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
			auto valid = text_writer.label_create();
			generate_raw_jump(Jump::Jlt, valid);
			/* Release the helper return register before constructing an early
			 * native return.  On the valid edge the generated return sequence is
			 * skipped, so the physical decision register still carries 0 or 1. */
			decision.reset(this);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			return_builder.ret();
			label_place(valid);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
			generate_cond_branch(Jump{Jump::Cbnz, decision_reg, false},
				successors[0], successors[1]);
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
				const bool result_unused =
					call.direct_call->result_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_UNUSED;
				const bool generation_leased =
					local_component_call
					|| (call.direct_call->flags
						& ZEND_NATIVE_DIRECT_CALL_GENERATION_LEASED) != 0;
				const uint32_t argument_count = call.call_argument_count;
				const uint32_t callee_argument_count =
					generated_fast_path
						? call.direct_call->expected_function
							->op_array.num_args
						: argument_count;
				const uint32_t first_extra_argument_slot =
					generated_fast_path
						? static_cast<uint32_t>(
							call.direct_call->expected_function
								->op_array.last_var
							+ call.direct_call->expected_function
								->op_array.T)
						: argument_count;
				const uint32_t compiled_variable_count =
					generated_fast_path
						? static_cast<uint32_t>(
							call.direct_call->expected_function
								->op_array.last_var)
						: argument_count;
				auto compiled_variable_used =
					[&](uint32_t variable_index) {
						return !local_component_call
							|| adaptor->component_compiled_variable_used(
								call.component_target_index,
								variable_index);
					};
				bool release_extra_arguments = false;
				for (uint32_t index = callee_argument_count;
						generated_fast_path && index < argument_count; ++index) {
					release_extra_arguments =
						release_extra_arguments
						|| !zend_mir_scalar_type_is_exact(
							call.direct_call->arguments[index].exact_type);
				}
				const uint32_t frame_operand =
					generated_fast_path ? argument_count : 0;
				const uint32_t frame_use_count =
					generated_fast_path
						? (private_inline_body ? 3 : 6 + node.has_result)
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
				auto slow_path = text_writer.label_create();
				auto successful = text_writer.label_create();
				int32_t leaf_private_frame_slot = 0;
				int32_t leaf_caller_frame_slot = 0;
				auto add_offset = [this](
						AsmReg destination, AsmReg base, uint64_t offset) {
					if (offset <= UINT32_C(4095)) {
						ASM(ADDxi, destination, base,
							static_cast<uint32_t>(offset));
					} else {
						ScratchReg amount{this};
						auto amount_reg = amount.alloc_gp();
						materialize_constant(offset, DarwinConfig::GP_BANK,
							8, amount_reg);
						ASM(ADDx, destination, base, amount_reg);
					}
				};
				auto store_constant = [this](
						AsmReg base, uint32_t offset, uint64_t value,
						uint32_t size) {
					ScratchReg constant{this};
					auto constant_reg = constant.alloc_gp();
					materialize_constant(value, DarwinConfig::GP_BANK,
						size, constant_reg);
					store_off(base, offset, constant_reg, size);
				};
				if (generated_fast_path) {
					if (private_inline_body) {
						/*
						 * Preserve live machine values before the observer or
						 * retry branch. Otherwise only the generated fast path
						 * executes the spills emitted for its callee call.
						 */
						for (auto reg_id : register_file.used_regs()) {
							::tpde::Reg reg{reg_id};
							if (!register_file.is_fixed(reg)
									&& register_file.reg_local_idx(reg)
										!= INVALID_VAL_LOCAL_IDX) {
								evict_reg(reg);
							}
						}
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
						leaf_private_frame_slot = allocate_stack_slot(
							call.direct_call->frame_size);
						leaf_caller_frame_slot =
							allocate_stack_slot(sizeof(void *));
						if (leaf_private_frame_slot < 0
								|| leaf_caller_frame_slot < 0) {
							return false;
						}
						store_off(AsmReg{AsmReg::FP},
							static_cast<uint32_t>(leaf_caller_frame_slot),
							frame_reg, sizeof(void *));
						ScratchReg first{this};
						ScratchReg second{this};
						auto first_reg = first.alloc_gp();
						auto second_reg = second.alloc_gp();

						if (node.inlined_user_body) {
							load_off(first_reg, context_reg,
								static_cast<uint32_t>(offsetof(
									zend_native_execution_context,
									observers_enabled)), 1);
							ASM(CMPxi, first_reg, 0);
							generate_raw_jump(Jump::Jne, slow_path);
							if (node.inlined_operand_index
									>= node.operands.size()) {
								return false;
							}
							if (node.inlined_checked_source_opcode
									!= UINT32_MAX) {
								if (node.inlined_operand_index + 1
										>= node.operands.size()) {
									return false;
								}
								auto [left_ref, left] = val_ref_single(
									node.operands[
										node.inlined_operand_index]);
								auto [right_ref, right] = val_ref_single(
									node.operands[
										node.inlined_operand_index + 1]);
								switch (
									node.inlined_checked_source_opcode) {
									case ZEND_ADD:
										ASM(ADDSx, first_reg,
											left.load_to_reg(),
											right.load_to_reg());
										break;
									case ZEND_SUB:
										ASM(SUBSx, first_reg,
											left.load_to_reg(),
											right.load_to_reg());
										break;
									default:
										return false;
								}
								generate_raw_jump(Jump::Jvs, slow_path);
							} else {
								auto [inline_ref, inline_value] =
									val_ref_single(node.operands[
										node.inlined_operand_index]);
								mov(first_reg,
									inline_value.load_to_reg(), 8);
							}
							if (!result_unused) {
								mov(second_reg, frame_reg, 8);
								if (call.direct_call->result_operand.slot_kind
										== ZEND_MIR_SOURCE_SLOT_CV) {
									add_offset(second_reg, second_reg,
										static_cast<uint64_t>(
											ZEND_CALL_FRAME_SLOT
												+ call.direct_call
													->result_operand.index)
											* sizeof(zval));
								} else {
									ScratchReg slot_index{this};
									auto slot_index_reg = slot_index.alloc_gp();
									load_off(slot_index_reg, frame_reg,
										static_cast<uint32_t>(offsetof(
											zend_execute_data, func)), 8);
									load_off(slot_index_reg, slot_index_reg,
										static_cast<uint32_t>(offsetof(
											zend_op_array, last_var)), 4);
									add_unsigned_offset(slot_index_reg,
										slot_index_reg,
										ZEND_CALL_FRAME_SLOT
											+ call.direct_call
												->result_operand.index);
									ASM(LSLxi, slot_index_reg,
										slot_index_reg, 4);
									ASM(ADDx, second_reg,
										second_reg, slot_index_reg);
								}
								store_off(second_reg, 0, first_reg, 8);
								if (call.direct_call->result_type
										== ZEND_MIR_SCALAR_TYPE_I1) {
									ScratchReg kind{this};
									auto kind_reg = kind.alloc_gp();
									ASM(ADDwi, kind_reg, first_reg, IS_FALSE);
									store_off(second_reg,
										static_cast<uint32_t>(offsetof(
											zval, u1.type_info)),
										kind_reg, 4);
								} else {
									store_constant(second_reg,
										static_cast<uint32_t>(offsetof(
											zval, u1.type_info)),
										zval_type(call.direct_call->result_type),
										4);
								}
							}
							generate_raw_jump(Jump::jmp, successful);
						}
						/*
						 * A leaf binding names an exact, already-published
						 * immutable callee. The compiler declines this
						 * representation when a frame probe is installed, so
						 * those compile-time invariants do not need to be
						 * reloaded at every loop iteration.
						 */
						load_off(first_reg, context_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_execution_context,
								observers_enabled)), 1);
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(Jump::Jne, slow_path);
						ScratchReg callee_address{this};
						auto callee_reg = callee_address.alloc_gp();
							add_offset(callee_reg, AsmReg{AsmReg::FP},
								static_cast<uint32_t>(
									leaf_private_frame_slot));
						load_off(second_reg, cell_reg,
							static_cast<uint32_t>(
								offsetof(zend_native_entry_cell, function)), 8);
						store_off(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, func)),
							second_reg, 8);
						store_constant(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, call)), 1, 8);

						if (result_unused) {
							store_constant(callee_reg,
								static_cast<uint32_t>(offsetof(
									zend_execute_data, return_value)), 0, 8);
						} else {
							mov(second_reg, frame_reg, 8);
							if (call.direct_call->result_operand.slot_kind
									== ZEND_MIR_SOURCE_SLOT_CV) {
								add_offset(second_reg, second_reg,
									static_cast<uint64_t>(
										ZEND_CALL_FRAME_SLOT
											+ call.direct_call
												->result_operand.index)
										* sizeof(zval));
							} else {
								ScratchReg slot{this};
								auto slot_reg = slot.alloc_gp();
								load_off(slot_reg, frame_reg,
									static_cast<uint32_t>(offsetof(
										zend_execute_data, func)), 8);
								load_off(slot_reg, slot_reg,
									static_cast<uint32_t>(offsetof(
										zend_op_array, last_var)), 4);
								add_unsigned_offset(slot_reg, slot_reg,
									ZEND_CALL_FRAME_SLOT
										+ call.direct_call
											->result_operand.index);
								ASM(LSLxi, slot_reg, slot_reg, 4);
								ASM(ADDx, second_reg, second_reg, slot_reg);
							}
							store_off(callee_reg,
								static_cast<uint32_t>(offsetof(
									zend_execute_data, return_value)),
								second_reg, 8);
							store_constant(second_reg,
								static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
								IS_UNDEF, 4);
						}

						for (uint32_t index = 0;
								index < argument_count; ++index) {
							zend_mir_call_argument_ref source_argument;
								if (!zend_tpde_call_argument_at(adaptor->plan(),
										call.call_argument_offset + index,
										&source_argument)) {
									return false;
								}
								auto argument_value_ref =
									val_ref(node.operands[index]);
								auto argument = argument_value_ref.part(0);
								const uint32_t offset = static_cast<uint32_t>(
									(ZEND_CALL_FRAME_SLOT + index)
										* sizeof(zval));
								const zend_native_direct_call_argument
									&descriptor_argument =
										call.direct_call->arguments[index];
								if (zend_mir_id_is_valid(
										source_argument.value_id)
										&& source_argument.source_operand.kind
											!= ZEND_MIR_SOURCE_OPERAND_LITERAL
										&& zend_mir_scalar_type_is_exact(
											descriptor_argument.exact_type)) {
									if (descriptor_argument.source_frame_offset
											== UINT32_MAX) {
										return false;
									}
									ScratchReg payload{this};
									auto payload_reg = payload.alloc_gp();
									load_off(payload_reg, frame_reg,
										descriptor_argument.source_frame_offset,
										8);
									store_off(callee_reg, offset,
										payload_reg, 8);
									store_constant(callee_reg, offset + 8,
										0, 8);
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										ScratchReg kind{this};
										auto kind_reg = kind.alloc_gp();
										materialize_constant(IS_FALSE,
											DarwinConfig::GP_BANK, 4,
											kind_reg);
										ASM(ADDx, kind_reg, kind_reg,
											payload_reg);
										store_off(callee_reg, offset + 8,
											kind_reg, 4);
									} else {
										store_constant(callee_reg, offset + 8,
											zval_type(
												descriptor_argument.exact_type),
											4);
									}
								} else {
									if (!zend_mir_id_is_valid(
											source_argument.value_id)
											|| source_argument
												.source_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
										if (source_argument.source_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
											store_constant(callee_reg, offset,
												descriptor_argument.scalar_bits,
												8);
											store_constant(callee_reg,
												offset + 8,
												zval_type(descriptor_argument
													.exact_type)
													+ (descriptor_argument
																.exact_type
															== ZEND_MIR_SCALAR_TYPE_I1
														? static_cast<uint32_t>(
															descriptor_argument
																.scalar_bits)
														: 0),
												4);
										} else {
											auto source_frame_reg =
												argument.load_to_reg();
											const uint32_t source_offset =
												descriptor_argument
													.source_frame_offset;
											if (source_offset == UINT32_MAX) {
												return false;
											}
											ScratchReg low_word{this};
											ScratchReg high_word{this};
											auto low_word_reg =
												low_word.alloc_gp();
											auto high_word_reg =
												high_word.alloc_gp();
											load_off(low_word_reg,
												source_frame_reg,
												source_offset, 8);
											load_off(high_word_reg,
												source_frame_reg,
												source_offset + 8, 8);
											store_off(callee_reg, offset,
												low_word_reg, 8);
											store_off(callee_reg, offset + 8,
												high_word_reg, 8);
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
										store_off(callee_reg, offset,
											low_word.cur_reg(), 8);
										store_off(callee_reg, offset + 8,
											high_word.cur_reg(), 4);
										store_constant(callee_reg,
											offset
												+ static_cast<uint32_t>(
													offsetof(zval, u2)),
											0, 4);
										ScratchReg type_info{this};
										auto type_info_reg =
											type_info.alloc_gp();
										mov(type_info_reg,
											high_word.cur_reg(), 4);
										ASM(TSTwi, type_info_reg,
											IS_TYPE_REFCOUNTED
												<< Z_TYPE_FLAGS_SHIFT);
										auto copied =
											text_writer.label_create();
										generate_raw_jump(
											Jump::Jeq, copied);
										load_off(type_info_reg,
											low_word.cur_reg(),
											static_cast<uint32_t>(offsetof(
												zend_refcounted_h, refcount)),
											4);
										ASM(ADDwi, type_info_reg,
											type_info_reg, 1);
										store_off(low_word.cur_reg(),
											static_cast<uint32_t>(offsetof(
												zend_refcounted_h, refcount)),
											type_info_reg, 4);
										label_place(copied);
									} else {
										auto argument_reg =
											argument.load_to_reg();
										store_off(callee_reg, offset,
											argument_reg, 8);
										store_constant(callee_reg,
											offset + 8, 0, 8);
										const uint32_t type =
											zval_type(*adaptor,
												node.operands[index]);
										if (type == IS_FALSE) {
											ScratchReg kind{this};
											auto kind_reg = kind.alloc_gp();
											materialize_constant(IS_FALSE,
												DarwinConfig::GP_BANK, 4,
												kind_reg);
											ASM(ADDx, kind_reg, kind_reg,
												argument_reg);
											store_off(callee_reg,
												offset + 8, kind_reg, 4);
										} else {
											store_constant(callee_reg,
												offset + 8, type, 4);
										}
									}
								}
							}
						first.reset();
						second.reset();
						ValuePart callee_value{DarwinConfig::GP_BANK, 8};
						callee_value.set_value(
							this, std::move(callee_address));
						auto entry_image = image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
							call.call_site.target_id);
						auto entry_cell =
							std::move(entry_image).into_scratch(this);
						ScratchReg entry_argument{this};
						auto entry_argument_reg =
							entry_argument.alloc_gp();
						load_off(entry_argument_reg, entry_cell.cur_reg(),
							static_cast<uint32_t>(
								offsetof(zend_native_entry_cell, code)), 8);
						load_off(entry_argument_reg, entry_argument_reg,
							static_cast<uint32_t>(
								offsetof(zend_native_code, entry)), 8);
						ValuePart entry_value{DarwinConfig::GP_BANK, 8};
						entry_value.set_value(
							this, std::move(entry_argument));
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						entry_cell.reset();
						zend::native::tpde::CCAssignerAppleA64 fast_assigner;
						CallBuilder fast_builder{*this, fast_assigner};
						fast_builder.add_arg(
							std::move(callee_value), ::tpde::CCAssignment{});
						fast_builder.add_arg(
							CallArg{node.operands[context_operand + 1]});
						fast_builder.call(std::move(entry_value));
						ValuePart fast_status{DarwinConfig::GP_BANK, 4};
						fast_builder.add_ret(
							fast_status, ::tpde::CCAssignment{});
						auto fast_status_reg =
							fast_status.cur_reg_or_load(this);
						ASM(CMPxi, fast_status_reg, ZEND_NATIVE_RETURNED);
						auto leaf_returned = text_writer.label_create();
						generate_raw_jump(Jump::Jeq, leaf_returned);
						ASM(CMPxi, fast_status_reg, ZEND_NATIVE_RETRY);
						generate_raw_jump(Jump::Jeq, slow_path);
						if (zend_mir_id_is_valid(call.exception_block_id)) {
							auto propagate = text_writer.label_create();
							ASM(CMPxi, fast_status_reg,
								ZEND_NATIVE_EXCEPTION);
							generate_raw_jump(Jump::Jne, propagate);
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
								::tpde::CCAssignment{});
							return_builder.ret();
						}
						label_place(leaf_returned);
						fast_status.reset(this);
						generate_raw_jump(Jump::jmp, successful);
					} else {
					const uint32_t activation_size = static_cast<uint32_t>(
						(sizeof(zend_native_direct_activation)
							+ sizeof(zval) - 1) / sizeof(zval) * sizeof(zval));
					const uint64_t reservation_size =
						static_cast<uint64_t>(call.direct_call->frame_size)
							+ activation_size;
					if (reservation_size > UINT32_MAX) {
						return false;
					}
					for (auto reg_id : register_file.used_regs()) {
						::tpde::Reg reg{reg_id};
						if (!register_file.is_fixed(reg)
								&& register_file.reg_local_idx(reg)
									!= INVALID_VAL_LOCAL_IDX) {
							evict_reg(reg);
						}
					}
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
					ScratchReg published_code{this};
					auto first_reg = first.alloc_gp();
					auto second_reg = second.alloc_gp();
					auto published_code_reg = published_code.alloc_gp();
					std::optional<ScratchReg> run_time_cache;
					auto load_callee_function =
						[this, local_component_call, descriptor_reg, cell_reg](
							AsmReg destination) {
							load_off(destination,
								local_component_call
									? descriptor_reg : cell_reg,
								local_component_call
									? static_cast<uint32_t>(offsetof(
										zend_native_direct_call_descriptor,
										expected_function))
									: static_cast<uint32_t>(offsetof(
										zend_native_entry_cell, function)),
								8);
						};

					if (local_component_call) {
						materialize_constant(
							UINT64_C(0), DarwinConfig::GP_BANK, 8,
							published_code_reg);
					} else {
						add_offset(published_code_reg, cell_reg,
							static_cast<uint32_t>(
								offsetof(zend_native_entry_cell, code)));
						ASM(LDARx, published_code_reg, published_code_reg);
						ASM(CMPxi, published_code_reg, 0);
						generate_raw_jump(Jump::Jeq, slow_path);
						load_callee_function(first_reg);
						load_off(second_reg, descriptor_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_direct_call_descriptor,
								expected_function)), 8);
						ASM(CMPx, first_reg, second_reg);
						generate_raw_jump(Jump::Jne, slow_path);
						load_off(first_reg, published_code_reg,
							static_cast<uint32_t>(
								offsetof(zend_native_code, executable)), 1);
						ASM(CMPxi, first_reg, 1);
						generate_raw_jump(Jump::Jne, slow_path);
						load_off(first_reg, cell_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_entry_cell, frame_probe)), 8);
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(Jump::Jne, slow_path);
					}
					load_off(first_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							observers_enabled)), 1);
					ASM(CMPxi, first_reg, 0);
					generate_raw_jump(Jump::Jne, slow_path);
					load_off(first_reg, frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, call)), 8);
					ASM(CMPxi, first_reg, 0);
					generate_raw_jump(Jump::Jne, slow_path);
					if (call.direct_call->expected_function
							->op_array.cache_size != 0) {
						run_time_cache.emplace(this);
						auto cache_reg = run_time_cache->alloc_gp();
						load_callee_function(first_reg);
						load_off(cache_reg, first_reg,
							static_cast<uint32_t>(offsetof(
								zend_op_array, run_time_cache__ptr)), 8);
						mov(first_reg, cache_reg, 8);
						ASM(ANDxi, first_reg, first_reg, 1);
						ASM(CMPxi, first_reg, 0);
						auto cache_resolved = text_writer.label_create();
						generate_raw_jump(Jump::Jeq, cache_resolved);
						load_off(first_reg, context_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_execution_context,
								map_ptr_base_address)), 8);
						load_off(first_reg, first_reg, 0, 8);
						ASM(ADDx, cache_reg, cache_reg, first_reg);
						load_off(cache_reg, cache_reg, 0, 8);
						label_place(cache_resolved);
						ASM(CMPxi, cache_reg, 0);
						generate_raw_jump(Jump::Jeq, slow_path);
					}
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
						load_off(first_reg, frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)
									+ offsetof(zval, u1.type_info)), 4);
						ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
						ASM(CMPwi, first_reg, IS_OBJECT);
						generate_raw_jump(Jump::Jne, slow_path);
					} else if (call.direct_call->receiver_kind
								== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE
							&& (call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
								!= 0) {
						load_off(first_reg, frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)), 8);
						load_off(second_reg, frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)
									+ offsetof(zval, u1.type_info)), 4);
						ASM(ANDwi, second_reg, second_reg, Z_TYPE_MASK);
						ASM(CMPwi, second_reg, IS_OBJECT);
						auto called_scope_ready = text_writer.label_create();
						generate_raw_jump(Jump::Jne, called_scope_ready);
						load_off(first_reg, first_reg,
							static_cast<uint32_t>(offsetof(zend_object, ce)), 8);
						label_place(called_scope_ready);
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(Jump::Jeq, slow_path);
						load_callee_function(second_reg);
						load_off(second_reg, second_reg,
							static_cast<uint32_t>(
								offsetof(zend_op_array, scope)), 8);
						auto called_scope_compatible =
							text_writer.label_create();
						auto check_called_scope = text_writer.label_create();
						label_place(check_called_scope);
						ASM(CMPx, first_reg, second_reg);
						generate_raw_jump(
							Jump::Jeq, called_scope_compatible);
						load_off(first_reg, first_reg,
							static_cast<uint32_t>(
								offsetof(zend_class_entry, parent)), 8);
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(
							Jump::Jne, check_called_scope);
						generate_raw_jump(Jump::jmp, slow_path);
						label_place(called_scope_compatible);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						const uint32_t receiver_offset =
							static_cast<uint32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ call.direct_call->receiver_operand.index)
								* sizeof(zval));
						load_off(first_reg, frame_reg,
							receiver_offset + static_cast<uint32_t>(
								offsetof(zval, u1.type_info)), 4);
						ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
						ASM(CMPwi, first_reg, IS_OBJECT);
						generate_raw_jump(Jump::Jne, slow_path);
						load_off(first_reg, frame_reg, receiver_offset, 8);
						load_off(first_reg, first_reg,
							static_cast<uint32_t>(offsetof(zend_object, ce)), 8);
						load_callee_function(second_reg);
						load_off(second_reg, second_reg,
							static_cast<uint32_t>(
								offsetof(zend_op_array, scope)), 8);
						auto receiver_compatible = text_writer.label_create();
						auto check_receiver_class = text_writer.label_create();
						label_place(check_receiver_class);
						ASM(CMPx, first_reg, second_reg);
						generate_raw_jump(
							Jump::Jeq, receiver_compatible);
						load_off(first_reg, first_reg,
							static_cast<uint32_t>(
								offsetof(zend_class_entry, parent)), 8);
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(
							Jump::Jne, check_receiver_class);
						generate_raw_jump(Jump::jmp, slow_path);
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
						if (zend_mir_scalar_type_is_exact(
								argument.exact_type)) {
							continue;
						}
						const uint32_t source_offset =
							static_cast<uint32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ argument.source_operand.index)
								* sizeof(zval)
								+ offsetof(zval, u1.type_info));
						load_off(first_reg, frame_reg, source_offset, 4);
						ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
						ASM(CMPwi, first_reg, IS_REFERENCE);
						generate_raw_jump(
							argument.mode
									== ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
								? Jump::Jne : Jump::Jeq,
							slow_path);
						if (argument.mode
								== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
							ASM(CMPwi, first_reg, IS_UNDEF);
							generate_raw_jump(Jump::Jeq, slow_path);
						}
					}

					/*
					 * Guard the native C stack before recursive entry.  The
					 * existing slow path raises the canonical Zend overflow
					 * error; no helper is called on a successful call.
					 */
					load_off(first_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							stack_limit)), 8);
					{
						auto stack_guarded = text_writer.label_create();
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(Jump::Jeq, stack_guarded);
						load_off(first_reg, first_reg, 0, 8);
						ASM(ADDxi, second_reg, AsmReg{AsmReg::SP}, 0);
						ASM(CMPx, second_reg, first_reg);
						generate_raw_jump(Jump::Jls, slow_path);
						label_place(stack_guarded);
					}

					/* Reserve the current VM-stack page without a C transition. */
					load_off(first_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							vm_stack_top)), 8);
					load_off(first_reg, first_reg, 0, 8);
					load_off(second_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							vm_stack_end)), 8);
					load_off(second_reg, second_reg, 0, 8);
					ASM(SUBx, second_reg, second_reg, first_reg);
					compare_unsigned_immediate(
						second_reg, reservation_size);
					generate_raw_jump(Jump::Jcc, slow_path);

					ScratchReg callee_address{this};
					auto callee_reg = callee_address.alloc_gp();
					mov(callee_reg, first_reg, 8);
					add_offset(second_reg, callee_reg, reservation_size);
					{
						ScratchReg address{this};
						auto address_reg = address.alloc_gp();
						load_off(address_reg, context_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_execution_context,
								vm_stack_top)), 8);
						store_off(address_reg, 0, second_reg, 8);
					}
					store_off(frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, call)),
						callee_reg, 8);

					/* Initialize the exact Zend frame layout. */
					load_callee_function(second_reg);
					store_off(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, func)),
						second_reg, 8);
					store_constant(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, call)), 0, 8);
					store_off(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, prev_execute_data)),
						frame_reg, 8);
					store_constant(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, symbol_table)), 0, 8);
					if (run_time_cache.has_value()) {
						store_off(callee_reg,
							static_cast<uint32_t>(offsetof(
								zend_execute_data, run_time_cache)),
							run_time_cache->cur_reg(), 8);
						run_time_cache->reset();
					} else {
						store_constant(callee_reg,
							static_cast<uint32_t>(offsetof(
								zend_execute_data, run_time_cache)), 0, 8);
					}
					store_constant(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, extra_named_params)), 0, 8);
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
						load_off(second_reg, frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)), 8);
						store_off(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)),
							second_reg, 8);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE) {
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)
								!= 0) {
							load_off(second_reg, frame_reg,
								static_cast<uint32_t>(
									offsetof(zend_execute_data, This)), 8);
							load_off(first_reg, frame_reg,
								static_cast<uint32_t>(
									offsetof(zend_execute_data, This)
										+ offsetof(zval, u1.type_info)), 4);
							ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
							ASM(CMPwi, first_reg, IS_OBJECT);
							auto called_scope_ready =
								text_writer.label_create();
							generate_raw_jump(
								Jump::Jne, called_scope_ready);
							load_off(second_reg, second_reg,
								static_cast<uint32_t>(
									offsetof(zend_object, ce)), 8);
							label_place(called_scope_ready);
						} else {
							load_off(second_reg, descriptor_reg,
								static_cast<uint32_t>(offsetof(
									zend_native_direct_call_descriptor,
									called_scope)), 8);
						}
						store_off(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)),
							second_reg, 8);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						load_off(second_reg, frame_reg,
							static_cast<uint32_t>(
								(ZEND_CALL_FRAME_SLOT
									+ call.direct_call->receiver_operand.index)
								* sizeof(zval)), 8);
						store_off(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)),
							second_reg, 8);
					} else {
						store_constant(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)), 0, 8);
					}
					store_constant(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, This)
								+ offsetof(zval, u1.type_info)),
						ZEND_CALL_NESTED_FUNCTION
							| ((call.direct_call->receiver_kind
										== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS
									|| call.direct_call->receiver_kind
										== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT)
								? ZEND_CALL_HAS_THIS : 0)
							| (release_extra_arguments
								? ZEND_CALL_FREE_EXTRA_ARGS : 0),
						4);
					store_constant(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, This)
								+ offsetof(zval, u2.num_args)),
						argument_count, 4);

					/* Publish caller and callee source positions. */
					load_off(second_reg, frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, func)), 8);
					load_off(second_reg, second_reg,
						static_cast<uint32_t>(
							offsetof(zend_op_array, opcodes)), 8);
					add_offset(second_reg, second_reg,
						static_cast<uint64_t>(
							call.direct_call->source_position)
							* sizeof(zend_op));
					store_off(frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, opline)),
						second_reg, 8);
					load_off(second_reg, callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, func)), 8);
					load_off(second_reg, second_reg,
						static_cast<uint32_t>(
							offsetof(zend_op_array, opcodes)), 8);
					add_offset(second_reg, second_reg,
						static_cast<uint64_t>(callee_argument_count)
							* sizeof(zend_op));
					store_off(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, opline)),
						second_reg, 8);

					/* Resolve the caller's canonical result zval. */
					if (result_unused) {
						add_offset(second_reg, callee_reg,
							static_cast<uint64_t>(
								call.direct_call->frame_size)
								+ offsetof(zend_native_direct_activation,
									discarded_return));
					} else {
						mov(second_reg, frame_reg, 8);
						if (call.direct_call->result_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV) {
							add_offset(second_reg, second_reg,
								static_cast<uint64_t>(
									ZEND_CALL_FRAME_SLOT
										+ call.direct_call->result_operand.index)
									* sizeof(zval));
						} else {
							ScratchReg slot{this};
							auto slot_reg = slot.alloc_gp();
							load_off(slot_reg, frame_reg,
								static_cast<uint32_t>(
									offsetof(zend_execute_data, func)), 8);
							load_off(slot_reg, slot_reg,
								static_cast<uint32_t>(
									offsetof(zend_op_array, last_var)), 4);
							add_unsigned_offset(slot_reg, slot_reg,
								ZEND_CALL_FRAME_SLOT
									+ call.direct_call->result_operand.index);
							ASM(LSLxi, slot_reg, slot_reg, 4);
							ASM(ADDx, second_reg, second_reg, slot_reg);
						}
					}
					store_off(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, return_value)),
						second_reg, 8);
					store_constant(second_reg,
						static_cast<uint32_t>(
							offsetof(zval, u1.type_info)), IS_UNDEF, 4);

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
							const uint32_t frame_slot =
								index < callee_argument_count
								? index
								: first_extra_argument_slot
									+ index - callee_argument_count;
						const uint32_t offset = static_cast<uint32_t>(
							(ZEND_CALL_FRAME_SLOT + frame_slot)
								* sizeof(zval));
						const zend_native_direct_call_argument &descriptor_argument =
							call.direct_call->arguments[index];
							if (zend_mir_scalar_type_is_exact(
									descriptor_argument.exact_type)) {
								if (!zend_mir_id_is_valid(
										source_argument.value_id)
										|| source_argument.source_operand.kind
											== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
									if (descriptor_argument.source_operand.kind
											== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
									store_constant(callee_reg, offset,
										descriptor_argument.scalar_bits, 8);
									store_constant(callee_reg, offset + 8,
										zval_type(
											descriptor_argument.exact_type)
											+ (descriptor_argument.exact_type
												== ZEND_MIR_SCALAR_TYPE_I1
												? static_cast<uint32_t>(
													descriptor_argument
														.scalar_bits)
												: 0), 4);
								} else {
									auto source_frame_reg =
										argument.load_to_reg();
									const uint32_t source_offset =
										descriptor_argument
											.source_frame_offset;
									if (source_offset == UINT32_MAX) {
										return false;
									}
									ScratchReg low_word{this};
									ScratchReg high_word{this};
									auto low_word_reg = low_word.alloc_gp();
									auto high_word_reg = high_word.alloc_gp();
									load_off(low_word_reg, source_frame_reg,
										source_offset, 8);
									load_off(high_word_reg, source_frame_reg,
										source_offset + 8, 8);
									store_off(callee_reg, offset,
										low_word_reg, 8);
										store_off(callee_reg, offset + 8,
											high_word_reg, 8);
									}
								} else {
									if (descriptor_argument.source_frame_offset
											== UINT32_MAX) {
										return false;
									}
									ScratchReg payload{this};
									auto payload_reg = payload.alloc_gp();
									load_off(payload_reg, frame_reg,
										descriptor_argument.source_frame_offset,
										8);
									store_off(callee_reg, offset,
										payload_reg, 8);
									store_constant(callee_reg, offset + 8,
										0, 8);
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										ScratchReg kind{this};
										auto kind_reg = kind.alloc_gp();
										materialize_constant(IS_FALSE,
											DarwinConfig::GP_BANK, 4,
											kind_reg);
										ASM(ADDx, kind_reg, kind_reg,
											payload_reg);
										store_off(callee_reg, offset + 8,
											kind_reg, 4);
									} else {
										store_constant(callee_reg,
											offset + 8,
											zval_type(
												descriptor_argument.exact_type),
											4);
									}
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
								store_off(callee_reg, offset,
									low_word.cur_reg(), 8);
								store_off(callee_reg, offset + 8,
									high_word.cur_reg(), 4);
								store_constant(callee_reg,
									offset + static_cast<uint32_t>(
										offsetof(zval, u2)),
									0, 4);
								ScratchReg type_info{this};
								auto type_info_reg = type_info.alloc_gp();
								mov(type_info_reg,
									high_word.cur_reg(), 4);
								ASM(TSTwi, type_info_reg,
									IS_TYPE_REFCOUNTED
										<< Z_TYPE_FLAGS_SHIFT);
								auto copied = text_writer.label_create();
								generate_raw_jump(Jump::Jeq, copied);
								load_off(type_info_reg,
									low_word.cur_reg(),
									static_cast<uint32_t>(offsetof(
										zend_refcounted_h, refcount)), 4);
								ASM(ADDwi, type_info_reg,
									type_info_reg, 1);
								store_off(low_word.cur_reg(),
									static_cast<uint32_t>(offsetof(
										zend_refcounted_h, refcount)),
									type_info_reg, 4);
								label_place(copied);
							} else {
								auto source_frame_reg = argument.load_to_reg();
							const uint32_t source_offset =
								descriptor_argument.source_frame_offset;
							if (source_offset == UINT32_MAX) {
								return false;
							}
							ScratchReg source_address{this};
							ScratchReg low_word{this};
							ScratchReg high_word{this};
							ScratchReg type_info{this};
							auto source_address_reg =
								source_address.alloc_gp();
							auto low_word_reg = low_word.alloc_gp();
							auto high_word_reg = high_word.alloc_gp();
							auto type_info_reg = type_info.alloc_gp();
							add_offset(source_address_reg, source_frame_reg,
								source_offset);
							load_off(low_word_reg, source_address_reg, 0, 8);
							load_off(high_word_reg, source_address_reg, 8, 8);
							store_off(callee_reg, offset, low_word_reg, 8);
							store_off(callee_reg, offset + 8, high_word_reg, 8);
							load_off(type_info_reg, source_address_reg,
								static_cast<uint32_t>(
									offsetof(zval, u1.type_info)), 4);
							ASM(TSTwi, type_info_reg,
								IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
							auto copied = text_writer.label_create();
							generate_raw_jump(Jump::Jeq, copied);
							load_off(type_info_reg, low_word_reg,
								static_cast<uint32_t>(
									offsetof(zend_refcounted_h, refcount)), 4);
							ASM(ADDwi, type_info_reg, type_info_reg, 1);
							store_off(low_word_reg,
								static_cast<uint32_t>(
									offsetof(zend_refcounted_h, refcount)),
								type_info_reg, 4);
							label_place(copied);
						}
					}
					for (uint32_t index = argument_count;
							index < callee_argument_count; ++index) {
						const zend_op_array &op_array =
							call.direct_call->expected_function->op_array;
						const zend_op &receive = op_array.opcodes[index];
						const zval *default_value =
							RT_CONSTANT(&receive, receive.op2);
						const uint32_t literal_index =
							static_cast<uint32_t>(
								default_value - op_array.literals);
						const uint32_t offset = static_cast<uint32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
						ScratchReg source_address{this};
						ScratchReg low_word{this};
						ScratchReg high_word{this};
						ScratchReg type_info{this};
						auto source_address_reg = source_address.alloc_gp();
						auto low_word_reg = low_word.alloc_gp();
						auto high_word_reg = high_word.alloc_gp();
						auto type_info_reg = type_info.alloc_gp();

						load_callee_function(source_address_reg);
						load_off(source_address_reg, source_address_reg,
							static_cast<uint32_t>(
								offsetof(zend_op_array, literals)), 8);
						add_offset(source_address_reg, source_address_reg,
							static_cast<uint64_t>(literal_index)
								* sizeof(zval));
						load_off(low_word_reg, source_address_reg, 0, 8);
						load_off(high_word_reg, source_address_reg, 8, 8);
						store_off(callee_reg, offset, low_word_reg, 8);
						store_off(callee_reg, offset + 8, high_word_reg, 8);
						load_off(type_info_reg, source_address_reg,
							static_cast<uint32_t>(
								offsetof(zval, u1.type_info)), 4);
						ASM(TSTwi, type_info_reg,
							IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
						auto copied = text_writer.label_create();
						generate_raw_jump(Jump::Jeq, copied);
						load_off(type_info_reg, low_word_reg,
							static_cast<uint32_t>(
								offsetof(zend_refcounted_h, refcount)), 4);
						ASM(ADDwi, type_info_reg, type_info_reg, 1);
						store_off(low_word_reg,
							static_cast<uint32_t>(
								offsetof(zend_refcounted_h, refcount)),
							type_info_reg, 4);
						label_place(copied);
					}
					for (uint32_t index = callee_argument_count;
							index < compiled_variable_count; ++index) {
						if (!compiled_variable_used(index)) {
							continue;
						}
						const uint32_t offset = static_cast<uint32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
						store_constant(callee_reg, offset, 0, 8);
						store_constant(callee_reg, offset + 8, 0, 8);
					}

					/* Link bailout metadata after the frame. */
					add_offset(second_reg, callee_reg,
						call.direct_call->frame_size);
					store_off(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, caller)),
						frame_reg, 8);
					store_off(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, callee)),
						callee_reg, 8);
					if (local_component_call) {
						store_constant(second_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_direct_activation, cell)), 0, 8);
					} else {
						store_off(second_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_direct_activation, cell)),
							cell_reg, 8);
					}
					store_off(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, code)),
						published_code_reg, 8);
					store_off(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, descriptor)),
						descriptor_reg, 8);
					load_off(first_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call)), 8);
					load_off(descriptor_reg, first_reg, 0, 8);
					store_off(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, previous)),
						descriptor_reg, 8);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							discarded_return)), 0, 8);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							discarded_return) + 8), 0, 8);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							discarded_return)
							+ offsetof(zval, u1.type_info)),
						IS_UNDEF, 4);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, status)), 0, 8);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							uses_discarded_return)),
						result_unused ? 1 : 0, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							raw_arguments_owned)), 0, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							frame_initialized)), 1, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							frame_requires_finish)), 1, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							cell_active)), generation_leased ? 0 : 1, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							dynamic_target)), 0, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							internal_target)), 0, 1);
					store_off(first_reg, 0, second_reg, 8);
					load_off(first_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							current_execute_data)), 8);
					store_off(first_reg, 0, callee_reg, 8);
					if (!generation_leased) {
						load_off(first_reg, cell_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_entry_cell, active_calls)), 4);
						ASM(ADDxi, first_reg, first_reg, 1);
						store_off(cell_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_entry_cell, active_calls)),
							first_reg, 4);
					}

					/* Component-local edges bind directly to TPDE's function
					 * symbol. Cross-component edges retain the published
					 * Entry-Cell target. */
					first.reset();
					second.reset();
					ValuePart callee_value{DarwinConfig::GP_BANK, 8};
					callee_value.set_value(
						this, std::move(callee_address));
					ValuePart fast_status{DarwinConfig::GP_BANK, 4};
					if (local_component_call) {
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						published_code.reset();
						zend::native::tpde::CCAssignerAppleA64 fast_assigner;
						CallBuilder fast_builder{*this, fast_assigner};
						fast_builder.add_arg(
							std::move(callee_value), ::tpde::CCAssignment{});
						fast_builder.add_arg(
							CallArg{node.operands[context_operand + 1]});
						fast_builder.call(
							this->func_syms[call.component_target_index]);
						fast_builder.add_ret(
							fast_status, ::tpde::CCAssignment{});
					} else {
						ScratchReg entry_argument{this};
						auto entry_argument_reg = entry_argument.alloc_gp();
						load_off(entry_argument_reg, published_code_reg,
							static_cast<uint32_t>(
								offsetof(zend_native_code, entry)), 8);
						ValuePart entry_value{DarwinConfig::GP_BANK, 8};
						entry_value.set_value(
							this, std::move(entry_argument));
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						published_code.reset();
						zend::native::tpde::CCAssignerAppleA64 fast_assigner;
						CallBuilder fast_builder{*this, fast_assigner};
						fast_builder.add_arg(
							std::move(callee_value), ::tpde::CCAssignment{});
						fast_builder.add_arg(
							CallArg{node.operands[context_operand + 1]});
						fast_builder.call(std::move(entry_value));
						fast_builder.add_ret(
							fast_status, ::tpde::CCAssignment{});
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
					load_off(post_callee_reg, post_frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, call)), 8);
					add_offset(activation_reg, post_callee_reg,
						call.direct_call->frame_size);
					store_off(activation_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, status)),
						fast_status.cur_reg_or_load(this), 4);
					fast_status.reset(this);
					auto complete_fast = text_writer.label_create();
					load_off(probe_reg, activation_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, status)), 4);
					ASM(CMPxi, probe_reg, ZEND_NATIVE_RETURNED);
					generate_raw_jump(Jump::Jne, complete_fast);
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context, exception)), 8);
					load_off(probe_reg, probe_reg, 0, 8);
					ASM(CMPxi, probe_reg, 0);
					generate_raw_jump(Jump::Jne, complete_fast);
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							vm_interrupt)), 8);
					load_off(probe_reg, probe_reg, 0, 1);
					ASM(CMPxi, probe_reg, 0);
					generate_raw_jump(Jump::Jne, complete_fast);
					/*
					 * Dynamic local-symbol operations may attach a HashTable to
					 * an otherwise inlineable direct callee. Its destruction
					 * belongs to the canonical frame finisher, not the
					 * helper-free scalar/CV release loop below.
					 */
					load_off(probe_reg, post_callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, This)
								+ offsetof(zval, u1.type_info)), 4);
					ASM(TSTwi, probe_reg, ZEND_CALL_HAS_SYMBOL_TABLE);
					generate_raw_jump(Jump::Jne, complete_fast);
					load_off(probe_reg, post_callee_reg,
						static_cast<uint32_t>(offsetof(
							zend_execute_data, return_value)), 8);
					load_off(probe_reg, probe_reg,
						static_cast<uint32_t>(
							offsetof(zval, u1.type_info)), 4);
					if (result_unused) {
						ASM(CMPxi, probe_reg, IS_DOUBLE);
						generate_raw_jump(Jump::Jhi, complete_fast);
					} else if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT)
								== 0
							|| call.direct_call->result_type
								== ZEND_MIR_SCALAR_TYPE_NONE) {
						/* The callee already wrote the complete boxed zval. */
					} else if (call.direct_call->result_type
							== ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(CMPxi, probe_reg, IS_FALSE);
						generate_raw_jump(Jump::Jcc, complete_fast);
						ASM(CMPxi, probe_reg, IS_TRUE);
						generate_raw_jump(Jump::Jhi, complete_fast);
					} else {
						ASM(CMPxi, probe_reg,
							zval_type(call.direct_call->result_type));
						generate_raw_jump(Jump::Jne, complete_fast);
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
							const uint32_t offset = static_cast<uint32_t>(
								(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
							load_off(probe_reg, post_callee_reg,
								offset + static_cast<uint32_t>(
									offsetof(zval, u1.type_info)), 4);
							ASM(TSTwi, probe_reg,
								IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
							auto released = text_writer.label_create();
							generate_raw_jump(Jump::Jeq, released);
							load_off(counted_reg, post_callee_reg, offset, 8);
							load_off(probe_reg, counted_reg,
								static_cast<uint32_t>(
									offsetof(zend_refcounted_h, refcount)), 4);
							ASM(CMPwi, probe_reg, 1);
							generate_raw_jump(Jump::Jeq, complete_fast);
							ASM(SUBwi, probe_reg, probe_reg, 1);
							store_off(counted_reg,
								static_cast<uint32_t>(
									offsetof(zend_refcounted_h, refcount)),
								probe_reg, 4);
							store_constant(post_callee_reg,
								offset + static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
								IS_UNDEF, 4);
							label_place(released);
						}
						for (uint32_t index = callee_argument_count;
								index < argument_count; ++index) {
							const uint32_t frame_slot =
								first_extra_argument_slot
									+ index - callee_argument_count;
							const uint32_t offset = static_cast<uint32_t>(
								(ZEND_CALL_FRAME_SLOT + frame_slot)
									* sizeof(zval));
							load_off(probe_reg, post_callee_reg,
								offset + static_cast<uint32_t>(
									offsetof(zval, u1.type_info)), 4);
							ASM(TSTwi, probe_reg,
								IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
							auto released = text_writer.label_create();
							generate_raw_jump(Jump::Jeq, released);
							load_off(counted_reg, post_callee_reg, offset, 8);
							load_off(probe_reg, counted_reg,
								static_cast<uint32_t>(
									offsetof(zend_refcounted_h, refcount)), 4);
							ASM(CMPwi, probe_reg, 1);
							generate_raw_jump(Jump::Jeq, complete_fast);
							ASM(SUBwi, probe_reg, probe_reg, 1);
							store_off(counted_reg,
								static_cast<uint32_t>(
									offsetof(zend_refcounted_h, refcount)),
								probe_reg, 4);
							store_constant(post_callee_reg,
								offset + static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
								IS_UNDEF, 4);
							label_place(released);
						}
					}

					/* Helper-free successful completion. */
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							current_execute_data)), 8);
					store_off(probe_reg, 0, post_frame_reg, 8);
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call)), 8);
					load_off(activation_reg, activation_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, previous)), 8);
					store_off(probe_reg, 0, activation_reg, 8);
					if (!generation_leased) {
						auto fast_cell = image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
							call.call_site.target_id);
						auto fast_cell_scratch =
							std::move(fast_cell).into_scratch(this);
						load_off(probe_reg, fast_cell_scratch.cur_reg(),
							static_cast<uint32_t>(offsetof(
								zend_native_entry_cell, active_calls)), 4);
						ASM(SUBxi, probe_reg, probe_reg, 1);
						store_off(fast_cell_scratch.cur_reg(),
							static_cast<uint32_t>(offsetof(
								zend_native_entry_cell, active_calls)),
							probe_reg, 4);
						fast_cell_scratch.reset();
					}
					store_constant(post_frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, call)), 0, 8);
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							vm_stack_top)), 8);
					store_off(probe_reg, 0, post_callee_reg, 8);
					post_frame_scratch.reset();
					post_context_scratch.reset();
					post_callee.reset();
					activation.reset();
					probe.reset();
					generate_raw_jump(Jump::jmp, successful);

					/* Rare completion retains full exception/interrupt cleanup. */
					label_place(complete_fast);
					post_frame_scratch.reset();
					post_context_scratch.reset();
					post_callee.reset();
					activation.reset();
					probe.reset();
					zend::native::tpde::CCAssignerAppleA64 finish_assigner;
					CallBuilder finish_builder{*this, finish_assigner};
					finish_builder.add_arg(
						CallArg{node.operands[frame_operand + 3]});
					finish_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					finish_builder.add_arg(
						CallArg{node.operands[context_operand + 3]});
					{
						auto [finish_frame_ref, finish_frame] =
							val_ref_single(node.operands[frame_operand + 2]);
						auto finish_frame_reg = finish_frame.load_to_reg();
						ScratchReg finish_activation{this};
						auto finish_activation_reg =
							finish_activation.alloc_gp();
						load_off(finish_activation_reg, finish_frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, call)), 8);
						add_offset(finish_activation_reg,
							finish_activation_reg,
							call.direct_call->frame_size);
						load_off(finish_activation_reg,
							finish_activation_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_direct_activation, status)), 4);
						finish_frame.reset();
						ValuePart finish_status_argument{
							DarwinConfig::GP_BANK, 4};
						finish_status_argument.set_value(
							this, std::move(finish_activation));
						finish_builder.add_arg(
							std::move(finish_status_argument),
							::tpde::CCAssignment{});
					}
					finish_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE));
					ValuePart finish_status{DarwinConfig::GP_BANK, 8};
					ValuePart finish_payload{DarwinConfig::GP_BANK, 8};
					finish_builder.add_ret(
						finish_status, ::tpde::CCAssignment{});
					finish_builder.add_ret(
						finish_payload, ::tpde::CCAssignment{});
					finish_payload.reset(this);
					auto finish_status_reg =
						finish_status.cur_reg_or_load(this);
					ASM(CMPxi, finish_status_reg, ZEND_NATIVE_RETURNED);
					auto finish_returned = text_writer.label_create();
					generate_raw_jump(Jump::Jeq, finish_returned);
					if (zend_mir_id_is_valid(call.exception_block_id)) {
						auto propagate = text_writer.label_create();
						ASM(CMPxi, finish_status_reg,
							ZEND_NATIVE_EXCEPTION);
						generate_raw_jump(Jump::Jne, propagate);
						generate_exception_branch(
							adaptor->block_ref(call.exception_block_id));
						label_place(propagate);
					}
					{
						RetBuilder return_builder{
							*this, *cur_cc_assigner()};
						return_builder.add(
							std::move(finish_status), ::tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(finish_returned);
					finish_status.reset(this);
					generate_raw_jump(Jump::jmp, successful);
					}
					label_place(slow_path);
				}
				ValuePart callee{DarwinConfig::GP_BANK, 8};
				ValuePart entry{DarwinConfig::GP_BANK, 8};
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					builder.add_arg(CallArg{
						node.operands[frame_operand
							+ slow_enter_frame_use]});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site.target_id), ::tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					builder.add_arg(CallArg{
						node.operands[context_operand
							+ slow_enter_context_use]});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DIRECT_USER_CALL_ENTER));
					builder.add_ret(callee, ::tpde::CCAssignment{});
					builder.add_ret(entry, ::tpde::CCAssignment{});
				}
				ScratchReg entry_copy{this};
				auto entry_copy_reg =
					entry_copy.alloc_specific(AsmReg::R2);
				mov(entry_copy_reg, entry.cur_reg_or_load(this), sizeof(void *));
				entry.reset(this);
				ValuePart entry_target{DarwinConfig::GP_BANK, 8};
				entry_target.set_value(this, std::move(entry_copy));
				ValuePart entry_status{DarwinConfig::GP_BANK, 4};
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					builder.add_arg(std::move(callee), ::tpde::CCAssignment{});
					builder.add_arg(CallArg{
						node.operands[context_operand
							+ slow_entry_context_use]});
					builder.call(std::move(entry_target));
					builder.add_ret(entry_status, ::tpde::CCAssignment{});
				}
				ScratchReg entry_status_copy{this};
				auto entry_status_copy_reg =
					entry_status_copy.alloc_specific(AsmReg::R3);
				mov(entry_status_copy_reg,
					entry_status.cur_reg_or_load(this),
					sizeof(zend_native_status));
				entry_status.reset(this);
				ValuePart entry_status_argument{
					DarwinConfig::GP_BANK, 4};
				entry_status_argument.set_value(
					this, std::move(entry_status_copy));
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{
					node.operands[frame_operand
						+ slow_leave_frame_use]});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
					call.id), ::tpde::CCAssignment{});
				builder.add_arg(CallArg{
					node.operands[context_operand
						+ slow_leave_context_use]});
				builder.add_arg(
					std::move(entry_status_argument), ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DIRECT_USER_CALL_LEAVE));
				ValuePart status{DarwinConfig::GP_BANK, 8};
				ValuePart payload{DarwinConfig::GP_BANK, 8};
				builder.add_ret(status, ::tpde::CCAssignment{});
				builder.add_ret(payload, ::tpde::CCAssignment{});
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
				if (generated_fast_path) {
					payload.reset(this);
					generate_raw_jump(Jump::jmp, successful);
					label_place(successful);
					if (node.has_result) {
						ScratchReg result_frame{this};
						auto result_frame_reg = result_frame.alloc_gp();
						if (private_inline_body) {
							load_off(result_frame_reg,
								AsmReg{AsmReg::FP},
								static_cast<uint32_t>(
									leaf_caller_frame_slot),
								sizeof(void *));
						} else {
							auto [result_frame_ref,
								result_frame_value] =
									val_ref_single(node.operands[
										frame_operand + 6]);
							mov(result_frame_reg,
								result_frame_value.load_to_reg(),
								sizeof(void *));
						}
						ScratchReg result_slot{this};
						auto result_slot_reg = result_slot.alloc_gp();
						mov(result_slot_reg, result_frame_reg, 8);
						if (call.direct_call->result_operand.slot_kind
								== ZEND_MIR_SOURCE_SLOT_CV) {
							add_offset(result_slot_reg, result_slot_reg,
								static_cast<uint64_t>(
									ZEND_CALL_FRAME_SLOT
										+ call.direct_call->result_operand.index)
									* sizeof(zval));
						} else {
							ScratchReg slot_index{this};
							auto slot_index_reg = slot_index.alloc_gp();
							load_off(slot_index_reg, result_frame_reg,
								static_cast<uint32_t>(
									offsetof(zend_execute_data, func)), 8);
							load_off(slot_index_reg, slot_index_reg,
								static_cast<uint32_t>(
									offsetof(zend_op_array, last_var)), 4);
							add_unsigned_offset(slot_index_reg, slot_index_reg,
								ZEND_CALL_FRAME_SLOT
									+ call.direct_call->result_operand.index);
							ASM(LSLxi, slot_index_reg, slot_index_reg, 4);
							ASM(ADDx, result_slot_reg,
								result_slot_reg, slot_index_reg);
						}
						if (adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
							auto result = result_ref(node.result);
							for (uint32_t part = 0; part < 2; ++part) {
								auto value = result.part(part);
								auto value_reg = value.alloc_reg();
								load_off(value_reg, result_slot_reg,
									part == 0
										? 0
										: static_cast<uint32_t>(
											offsetof(zval, u1.type_info)),
									part == 0 ? 8 : 4);
								value.set_modified();
							}
						} else {
							auto [result_ref, result] =
								result_ref_single(node.result);
							auto result_reg = result.alloc_reg();
							load_off(result_reg, result_slot_reg, 0, 8);
							result.set_modified();
						}
					}
					if (private_inline_body) {
						free_stack_slot(
							static_cast<uint32_t>(leaf_caller_frame_slot),
							sizeof(void *));
						free_stack_slot(
							static_cast<uint32_t>(leaf_private_frame_slot),
							call.direct_call->frame_size);
					}
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
					mov(result_slot_reg, result_frame_reg, 8);
					if (call.direct_call->result_operand.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV) {
						add_offset(result_slot_reg, result_slot_reg,
							static_cast<uint64_t>(
								ZEND_CALL_FRAME_SLOT
									+ call.direct_call
										->result_operand.index)
								* sizeof(zval));
					} else {
						ScratchReg slot_index{this};
						auto slot_index_reg = slot_index.alloc_gp();
						load_off(slot_index_reg, result_frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, func)), 8);
						load_off(slot_index_reg, slot_index_reg,
							static_cast<uint32_t>(
								offsetof(zend_op_array, last_var)), 4);
						add_unsigned_offset(slot_index_reg, slot_index_reg,
							ZEND_CALL_FRAME_SLOT
								+ call.direct_call->result_operand.index);
						ASM(LSLxi, slot_index_reg, slot_index_reg, 4);
						ASM(ADDx, result_slot_reg,
							result_slot_reg, slot_index_reg);
					}
					auto result = result_ref(node.result);
					for (uint32_t part = 0; part < 2; ++part) {
						auto value = result.part(part);
						auto value_reg = value.alloc_reg();
						load_off(value_reg, result_slot_reg,
							part == 0
								? 0
								: static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							part == 0 ? 8 : 4);
						value.set_modified();
					}
				} else if (node.has_result) {
					auto [result_ref, result] =
						result_ref_single(node.result);
					if (val_parts(node.result).bank
							== DarwinConfig::FP_BANK) {
						auto payload_reg = payload.cur_reg_or_load(this);
						ScratchReg converted{this};
						auto result_reg = converted.alloc(DarwinConfig::FP_BANK);
						ASM(FMOVdx, result_reg, payload_reg);
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
			if (call.user_call != nullptr
					&& call.user_call->do_opcode != ZEND_CALLABLE_CONVERT
					&& call.user_call->do_opcode
						!= ZEND_CALLABLE_CONVERT_PARTIAL) {
				const uint32_t frame_operand = call.operand_count;
				const uint32_t context_operand = frame_operand + 2;
				ValuePart callee{DarwinConfig::GP_BANK, 8};
				ValuePart entry{DarwinConfig::GP_BANK, 8};
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder enter_builder{*this, assigner};
					enter_builder.add_arg(
						CallArg{node.operands[frame_operand]});
					enter_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site.target_id), ::tpde::CCAssignment{});
					enter_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					enter_builder.add_arg(CallArg{
						node.operands[context_operand]});
					enter_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_DYNAMIC_USER_CALL_ENTER));
					enter_builder.add_ret(callee, ::tpde::CCAssignment{});
					enter_builder.add_ret(entry, ::tpde::CCAssignment{});
				}
				ScratchReg entry_copy{this};
				auto entry_copy_reg = entry_copy.alloc_specific(AsmReg::R2);
				mov(entry_copy_reg, entry.cur_reg_or_load(this), sizeof(void *));
				entry.reset(this);
				ValuePart entry_target{DarwinConfig::GP_BANK, 8};
				entry_target.set_value(this, std::move(entry_copy));
				ValuePart entry_status{DarwinConfig::GP_BANK, 4};
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder entry_builder{*this, assigner};
					entry_builder.add_arg(
						std::move(callee), ::tpde::CCAssignment{});
					entry_builder.add_arg(CallArg{
						node.operands[context_operand + 1]});
					entry_builder.call(std::move(entry_target));
					entry_builder.add_ret(
						entry_status, ::tpde::CCAssignment{});
				}
				ScratchReg entry_status_copy{this};
				auto entry_status_copy_reg =
					entry_status_copy.alloc_specific(AsmReg::R3);
				mov(entry_status_copy_reg,
					entry_status.cur_reg_or_load(this),
					sizeof(zend_native_status));
				entry_status.reset(this);
				ValuePart entry_status_argument{
					DarwinConfig::GP_BANK, 4};
				entry_status_argument.set_value(
					this, std::move(entry_status_copy));
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder leave_builder{*this, assigner};
				leave_builder.add_arg(
					CallArg{node.operands[frame_operand + 1]});
				leave_builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
					call.id), ::tpde::CCAssignment{});
				leave_builder.add_arg(CallArg{
					node.operands[context_operand + 2]});
				leave_builder.add_arg(
					std::move(entry_status_argument),
					::tpde::CCAssignment{});
				leave_builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DYNAMIC_USER_CALL_LEAVE));
				ValuePart status{DarwinConfig::GP_BANK, 8};
				ValuePart payload{DarwinConfig::GP_BANK, 8};
				leave_builder.add_ret(status, ::tpde::CCAssignment{});
				leave_builder.add_ret(payload, ::tpde::CCAssignment{});
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
				return_builder.add(
					std::move(status), ::tpde::CCAssignment{});
				return_builder.ret();
				label_place(continued);
				if (node.has_result) {
					auto [result_ref, result] =
						result_ref_single(node.result);
					if (val_parts(node.result).bank
							== DarwinConfig::FP_BANK) {
						auto payload_reg = payload.cur_reg_or_load(this);
						ScratchReg converted{this};
						auto result_reg =
							converted.alloc(DarwinConfig::FP_BANK);
						ASM(FMOVdx, result_reg, payload_reg);
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
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					call.call_site.target_id), ::tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
					call.id), ::tpde::CCAssignment{});
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_BEGIN));
			}
			for (uint32_t index = 0;
					index < (source_arguments
						? call.call_argument_count : call.operand_count); ++index) {
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
				if (source_arguments) {
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					builder.add_arg(ValuePart{index, 4,
						DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
					builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT));
					continue;
				}
				IRValueRef operand = node.operands[index];
				builder.add_arg(ValuePart{index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.add_arg(CallArg{operand});
				if (adaptor->exact_type(operand) == ZEND_MIR_SCALAR_TYPE_F64) {
					builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE));
				} else {
					if (!zend_mir_scalar_type_is_exact(adaptor->exact_type(operand))) {
						return false;
					}
					builder.add_arg(ValuePart{
						static_cast<uint32_t>(adaptor->exact_type(operand)), 4,
						DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
					builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER));
				}
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site.target_id), ::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
				call.id), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_USER_CALL_FINISH_SOURCE));
			ValuePart status{DarwinConfig::GP_BANK, 4};
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
						zend_tpde_encode_value_operand(call.call_site.result_operand), 8,
						DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				result_builder.add_arg(ValuePart{
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				result_builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
				ValuePart payload{DarwinConfig::GP_BANK, 8};
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
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site.target_id), ::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
				call.id), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL));
			ValuePart status{DarwinConfig::GP_BANK, 8};
			ValuePart payload{DarwinConfig::GP_BANK, 8};
			builder.add_ret(status, ::tpde::CCAssignment{});
			builder.add_ret(payload, ::tpde::CCAssignment{});
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
			} else {
				payload.reset(this);
			}
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_ENTER: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_ENTER));
			ValuePart status{DarwinConfig::GP_BANK, 4};
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
			const zend_tpde_plan *plan = adaptor->plan();
			if (plan->source_op_array == nullptr
					|| record.source_position_id
						>= plan->source_op_array->last) {
				return false;
			}
			const zend_op &opline =
				plan->source_op_array->opcodes[record.source_position_id];
			if (opline.opcode != ZEND_FAST_CALL
					|| opline.result_type != IS_TMP_VAR) {
				return false;
			}
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_scratch = std::move(frame).into_scratch();
			ScratchReg value{this};
			auto value_reg = value.alloc_gp();
			materialize_constant(
				UINT64_C(0), DarwinConfig::GP_BANK, 8, value_reg);
			store_off(frame_scratch.cur_reg(), opline.result.var,
				value_reg, 8);
			materialize_constant(record.source_position_id,
				DarwinConfig::GP_BANK, 4, value_reg);
			store_off(frame_scratch.cur_reg(),
				opline.result.var
					+ static_cast<uint32_t>(
						offsetof(zval, u2.opline_num)),
				value_reg, 4);
			value.reset();
			frame_scratch.reset();
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
			if (successors.size() != 2) {
				return false;
			}
			generate_exception_branch(successors[0]);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_RETURN: {
			const zend_tpde_plan *plan = adaptor->plan();
			if (plan->source_op_array == nullptr
					|| record.source_position_id
						>= plan->source_op_array->last) {
				return false;
			}
			const zend_op &opline =
				plan->source_op_array->opcodes[record.source_position_id];
			if (opline.opcode != ZEND_FAST_RET
					|| opline.op1_type != IS_TMP_VAR) {
				return false;
			}
			auto slow_exception = text_writer.label_create();
			auto [frame_ref, frame] = val_ref_single(node.operands[0]);
			auto frame_scratch = std::move(frame).into_scratch();
			ScratchReg direct_continuation{this};
			auto direct_continuation_reg =
				direct_continuation.alloc_gp();
			load_off(direct_continuation_reg, frame_scratch.cur_reg(),
				opline.op1.var
					+ static_cast<uint32_t>(
						offsetof(zval, u2.opline_num)),
				4);
			frame_scratch.reset();
			ASM(CMNwi, direct_continuation_reg, 1);
			generate_raw_jump(Jump::Jeq, slow_exception);
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
					compare_unsigned_immediate(
						direct_continuation_reg, source);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::Jne, continued);
					generate_raw_jump(
						Jump::jmp, user_opcode_labels_[landing]);
					label_place(continued);
				}
			} else {
				for (uint32_t i = 0; i < plan->instruction_count; ++i) {
					const zend_mir_instruction_record call =
						zend_tpde_instruction_record_at(
							plan, &plan->instructions[i]);
					zend_mir_block_id target;
					if (call.opcode != ZEND_MIR_OPCODE_FINALLY_CALL
							|| plan->view->successor_count(
								plan->view->context, call.block_id) != 2
							|| !plan->view->successor_at(
								plan->view->context, call.block_id, 1,
								&target)) {
						continue;
					}
					compare_unsigned_immediate(
						direct_continuation_reg, call.source_position_id);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::Jne, continued);
					generate_exception_branch(adaptor->block_ref(target));
					label_place(continued);
				}
			}
			direct_continuation.reset();
			{
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(slow_exception);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[1]});
			builder.add_arg(ValuePart{record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_RETURN));
			ValuePart continuation{DarwinConfig::GP_BANK, 4};
			builder.add_ret(continuation, ::tpde::CCAssignment{});
			auto continuation_reg = continuation.cur_reg_or_load(this);
			auto generator_returned = text_writer.label_create();
			compare_unsigned_immediate(continuation_reg,
				ZEND_NATIVE_FINALLY_GENERATOR_RETURNED);
			generate_raw_jump(Jump::Jeq, generator_returned);
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
				compare_unsigned_immediate(
					continuation_reg, call.source_position_id);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::Jne, continued);
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
			label_place(generator_returned);
			RetBuilder generator_return_builder{
				*this, *cur_cc_assigner()};
			generator_return_builder.add(ValuePart{
				ZEND_NATIVE_GENERATOR_RETURNED, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			generator_return_builder.ret();
			return true;
		}
		case ZEND_MIR_OPCODE_CATCH_ENTER: {
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CATCH_ENTER));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPxi, status_reg, ZEND_NATIVE_RETURNED);
			const auto &successors = adaptor->block_succs(
				adaptor->block_ref(record.block_id));
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
			ScratchReg source_position{this};
			auto source_position_reg = source_position.alloc_gp();
			load_off(source_position_reg, frame_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, func)), 8);
			load_off(source_position_reg, source_position_reg,
				static_cast<uint32_t>(
					offsetof(zend_function, op_array.opcodes)), 8);
			const uint64_t source_offset =
				uint64_t{record.source_position_id} * sizeof(zend_op);
			if (source_offset <= UINT32_C(4095)) {
				ASM(ADDxi, source_position_reg, source_position_reg,
					static_cast<uint32_t>(source_offset));
			} else {
				ScratchReg offset{this};
				auto offset_reg = offset.alloc_gp();
				materialize_constant(source_offset,
					DarwinConfig::GP_BANK, 8, offset_reg);
				ASM(ADDx, source_position_reg,
					source_position_reg, offset_reg);
			}
			store_off(frame_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, opline)),
				source_position_reg, 8);
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
			if (mir.direct_scalar_return) {
				{
					auto [frame_ref, frame] =
						val_ref_single(node.operands[0]);
					auto frame_reg = frame.load_to_reg();
					ScratchReg source_position{this};
					auto source_position_reg = source_position.alloc_gp();
					load_off(source_position_reg, frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, func)), 8);
					load_off(source_position_reg, source_position_reg,
						static_cast<uint32_t>(
							offsetof(zend_function, op_array.opcodes)), 8);
					const uint64_t source_offset =
						uint64_t{record.source_position_id}
							* sizeof(zend_op);
					if (source_offset <= UINT32_C(4095)) {
						ASM(ADDxi, source_position_reg,
							source_position_reg,
							static_cast<uint32_t>(source_offset));
					} else {
						ScratchReg offset{this};
						auto offset_reg = offset.alloc_gp();
						materialize_constant(source_offset,
							DarwinConfig::GP_BANK, 8, offset_reg);
						ASM(ADDx, source_position_reg,
							source_position_reg, offset_reg);
					}
					store_off(frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, opline)),
						source_position_reg, 8);
					ScratchReg return_pointer{this};
					auto return_reg = return_pointer.alloc_gp();
					load_off(return_reg, frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, return_value)), 8);
					auto clear_source = text_writer.label_create();
					generate_raw_jump(
						Jump{Jump::Cbz, return_reg, false}, clear_source);
					ScratchReg payload{this};
					auto payload_reg = payload.alloc_gp();
					ScratchReg kind{this};
					auto kind_reg = kind.alloc_gp();
					load_off(payload_reg, frame_reg,
						mir.direct_scalar_return_offset, 8);
					load_off(kind_reg, frame_reg,
						mir.direct_scalar_return_offset
							+ static_cast<uint32_t>(
								offsetof(zval, u1.type_info)), 4);
					store_off(return_reg, 0, payload_reg, 8);
					store_off(return_reg,
						static_cast<uint32_t>(offsetof(zval, u1.type_info)),
						kind_reg, 4);
					label_place(clear_source);
					if (mir.value_operation.op1.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_CV) {
						materialize_constant(
							uint64_t{IS_UNDEF}, DarwinConfig::GP_BANK, 4, kind_reg);
						store_off(frame_reg,
							mir.direct_scalar_return_offset
								+ static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							kind_reg, 4);
					}
				}
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_RETURNED, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				return_builder.ret();
				return true;
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{record.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(mir.value_operation.op1), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{mir.value_operation.source_opcode, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{mir.value_operation.extended_value, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_RETURN_SOURCE_ZVAL));
			ValuePart status{DarwinConfig::GP_BANK, 4};
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

	explicit A64ImageState(
		std::span<const zend_tpde_plan *const> plans,
		zend_native_image *image)
		: adaptor{plans}, compiler{&adaptor, image} {}
};

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
				| (((static_cast<uint32_t>(target) & UINT32_C(0xfff)) >> shift) << 10));
			return true;
		}
		default:
			return false;
	}
}

struct SerializedRelocation {
	uint32_t offset;
	uint32_t symbol;
	uint32_t type;
	int32_t addend;
};

struct SerializedSection {
	bool present = false;
	uint32_t type = 0;
	uint32_t flags = 0;
	uint32_t alignment = 0;
	uint64_t size = 0;
	bool is_virtual = false;
	std::string_view name;
	const uint8_t *data = nullptr;
	std::vector<SerializedRelocation> relocations;
};

struct SerializedSymbol {
	std::string_view name;
	uint32_t section = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
	uint8_t binding = 0;
	uint8_t kind = 0;
	bool defined = false;
};

struct SerializedA64Object {
	std::vector<SerializedSection> sections;
	std::vector<SerializedSymbol> symbols;
	uint32_t unwind_section = UINT32_MAX;
};

class ObjectCursor {
	const uint8_t *current_;
	size_t remaining_;

public:
	ObjectCursor(const uint8_t *data, size_t size)
		: current_{data}, remaining_{size} {}

	template <typename T>
	bool read(T *out) {
		if (remaining_ < sizeof(T)) {
			return false;
		}
		std::memcpy(out, current_, sizeof(T));
		current_ += sizeof(T);
		remaining_ -= sizeof(T);
		return true;
	}

	bool skip(size_t size) {
		if (remaining_ < size) {
			return false;
		}
		current_ += size;
		remaining_ -= size;
		return true;
	}

	bool view(size_t size, const uint8_t **out) {
		if (remaining_ < size) {
			return false;
		}
		*out = current_;
		current_ += size;
		remaining_ -= size;
		return true;
	}

	size_t remaining() const { return remaining_; }
};

bool parse_a64_object(
	const zend_native_image *image, SerializedA64Object *out) {
	static constexpr std::array<uint8_t, 16> MAGIC{
		'Z', 'N', 'M', 'I', 'R', '-', 'T', 'P', 'D', 'E', '-', 'A', '6', '4', 0, 2};
	if (image->text == nullptr || image->text_size < MAGIC.size()
			|| std::memcmp(image->text, MAGIC.data(), MAGIC.size()) != 0) {
		return false;
	}
	ObjectCursor cursor{
		image->text + MAGIC.size(), image->text_size - MAGIC.size()};
	uint32_t section_count;
	uint32_t present_section_count;
	uint32_t symbol_count;
	if (!cursor.read(&section_count)
			|| !cursor.read(&present_section_count)
			|| !cursor.read(&symbol_count)
			|| !cursor.read(&out->unwind_section)
			|| section_count == 0
			|| present_section_count >= section_count
			|| symbol_count == 0
			|| out->unwind_section >= section_count
			|| section_count > image->text_size
			|| symbol_count > image->text_size) {
		return false;
	}
	out->sections.clear();
	out->sections.resize(section_count);
	out->symbols.clear();
	out->symbols.resize(symbol_count);
	for (uint32_t present = 0; present < present_section_count; ++present) {
		uint32_t index;
		SerializedSection section;
		uint32_t name_size;
		uint32_t relocation_count;
		uint8_t is_virtual;
		if (!cursor.read(&index)
				|| !cursor.read(&section.type)
				|| !cursor.read(&section.flags)
				|| !cursor.read(&section.alignment)
				|| !cursor.read(&section.size)
				|| !cursor.read(&name_size)
				|| !cursor.read(&relocation_count)
				|| !cursor.read(&is_virtual)
				|| !cursor.skip(7)
				|| index == 0 || index >= section_count
				|| out->sections[index].present
				|| is_virtual > 1
				|| (is_virtual && relocation_count != 0)
				|| section.size > SIZE_MAX
				|| relocation_count > image->text_size) {
			return false;
		}
		const uint8_t *name;
		if (!cursor.view(name_size, &name)) {
			return false;
		}
		section.name = std::string_view{
			reinterpret_cast<const char *>(name), name_size};
		section.is_virtual = is_virtual != 0;
		if (!section.is_virtual
				&& !cursor.view(static_cast<size_t>(section.size),
					&section.data)) {
			return false;
		}
		section.relocations.reserve(relocation_count);
		for (uint32_t relocation_index = 0;
				relocation_index < relocation_count; ++relocation_index) {
			SerializedRelocation relocation;
			if (!cursor.read(&relocation.offset)
					|| !cursor.read(&relocation.symbol)
					|| !cursor.read(&relocation.type)
					|| !cursor.read(&relocation.addend)) {
				return false;
			}
			section.relocations.push_back(relocation);
		}
		section.present = true;
		out->sections[index] = std::move(section);
	}
	for (uint32_t index = 0; index < symbol_count; ++index) {
		SerializedSymbol symbol;
		uint32_t name_size;
		uint8_t defined;
		if (!cursor.read(&name_size)
				|| !cursor.read(&symbol.section)
				|| !cursor.read(&symbol.offset)
				|| !cursor.read(&symbol.size)
				|| !cursor.read(&symbol.binding)
				|| !cursor.read(&symbol.kind)
				|| !cursor.read(&defined)
				|| !cursor.skip(5)
				|| defined > 1
				|| (defined && (symbol.section == 0
					|| symbol.section >= section_count))) {
			return false;
		}
		const uint8_t *name;
		if (!cursor.view(name_size, &name)) {
			return false;
		}
		symbol.name = std::string_view{
			reinterpret_cast<const char *>(name), name_size};
		symbol.defined = defined != 0;
		out->symbols[index] = symbol;
	}
	return cursor.remaining() == 0;
}

#endif

} // namespace

zend_result zend_tpde_emit_darwin_arm64(
	const zend_tpde_plan *const *plans,
	uint32_t plan_count,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	auto state = std::make_unique<A64ImageState>(
		std::span<const zend_tpde_plan *const>{plans, plan_count}, image);
	if (!state->adaptor.valid()) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"TPDE rejected the malformed ZNMIR arm64 adaptor graph");
		return FAILURE;
	}
	if (!state->compiler.compile()) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"TPDE failed to compile the ZNMIR arm64 adaptor graph");
		return FAILURE;
	}
	std::vector<::tpde::u8> finalized =
		state->compiler.assembler.build_object_file();
	if (finalized.empty() || !zend_tpde_image_append(
			image, finalized.data(), finalized.size())) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to retain the relocatable TPDE arm64 image");
		return FAILURE;
	}
	image->metrics.direct_leaf_scalar_sites =
		state->adaptor.inlined_user_body_count();
	return SUCCESS;
}

zend_result zend_tpde_map_darwin_arm64(
	const zend_native_image *image,
	zend_native_code *code,
	zend_native_diagnostic *diag) {
#if defined(__APPLE__) && defined(__aarch64__)
	if (image == nullptr || image->text == nullptr || image->text_size == 0
			|| code == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Darwin TPDE mapper requires a relocatable arm64 image");
		return FAILURE;
	}
	SerializedA64Object object;
	if (!parse_a64_object(image, &object)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image has an invalid object encoding");
		return FAILURE;
	}
	long page_size_value = sysconf(_SC_PAGESIZE);
	if (page_size_value <= 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin page size is unavailable");
		return FAILURE;
	}
	size_t page_size = static_cast<size_t>(page_size_value);
	const uint32_t unwind_section = object.unwind_section;
	auto published = std::make_unique<A64PublishedState>();
	published->sections.resize(object.sections.size());
	bool has_executable = false;

	for (size_t i = 1; i < object.sections.size(); ++i) {
		const SerializedSection &section = object.sections[i];
		if (!section.present
				|| (section.flags & DarwinAssembler::SECTION_ALLOC) == 0
				|| section.size == 0) {
			continue;
		}
		if ((section.flags & (DarwinAssembler::SECTION_WRITE
				| DarwinAssembler::SECTION_EXEC))
				== (DarwinAssembler::SECTION_WRITE
					| DarwinAssembler::SECTION_EXEC)) {
			zend_tpde_set_diagnostic(diag,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"Darwin TPDE image requests writable executable data");
			return FAILURE;
		}
		size_t logical_size = static_cast<size_t>(section.size);
		if (i == unwind_section) {
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
		has_executable |= executable;
	}

	if (has_executable) pthread_jit_write_protect_np(0);
	for (size_t i = 1; i < object.sections.size(); ++i) {
		const SerializedSection &section = object.sections[i];
		if (!section.present || published->sections[i].mapping == nullptr) {
			continue;
		}
		if (!section.is_virtual && section.size != 0) {
			std::memcpy(published->sections[i].mapping,
				section.data, static_cast<size_t>(section.size));
		}
	}

	for (size_t i = 1; i < object.sections.size(); ++i) {
		const SerializedSection &section = object.sections[i];
		if (!section.present || published->sections[i].mapping == nullptr) {
			continue;
		}
		if (section.is_virtual) continue;
		for (const SerializedRelocation &relocation : section.relocations) {
			const size_t relocation_width =
				relocation.type == ::tpde::elf::R_AARCH64_ABS64
				? sizeof(uint64_t) : sizeof(uint32_t);
			if (relocation.symbol >= object.symbols.size()
					|| relocation.offset > section.size
					|| section.size - relocation.offset < relocation_width) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE relocation is outside its section");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
			const SerializedSymbol &symbol = object.symbols[relocation.symbol];
			uint8_t *location = static_cast<uint8_t *>(
				published->sections[i].mapping) + relocation.offset;
			uintptr_t symbol_address;
			if (symbol.defined) {
				if (symbol.section >= published->sections.size()
						|| published->sections[symbol.section].mapping
							== nullptr) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
						"Darwin TPDE image contains an invalid local symbol");
					if (has_executable) pthread_jit_write_protect_np(1);
					return FAILURE;
				}
				symbol_address = reinterpret_cast<uintptr_t>(
					published->sections[symbol.section].mapping)
					+ symbol.offset;
			} else {
				const void *resolved = nullptr;
				std::string external_name{symbol.name};
				if (!zend_tpde_image_resolve_symbol(
						image, external_name.c_str(), &resolved)) {
					zend_tpde_set_diagnostic(diag,
						ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
						"Darwin TPDE image contains an unresolved external symbol");
					if (has_executable) pthread_jit_write_protect_np(1);
					return FAILURE;
				}
				symbol_address = reinterpret_cast<uintptr_t>(resolved);
			}
			if (!apply_relocation(location, relocation.type,
					symbol_address, relocation.addend)) {
				zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
					"Darwin TPDE relocation kind or range is unsupported");
				if (has_executable) pthread_jit_write_protect_np(1);
				return FAILURE;
			}
		}
	}
	if (image->component_entry_count == 0) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image contains no component entries");
		if (has_executable) pthread_jit_write_protect_np(1);
		return FAILURE;
	}
	code->component_entries = static_cast<zend_native_frame_entry_t *>(
		std::calloc(image->component_entry_count,
			sizeof(*code->component_entries)));
	if (code->component_entries == nullptr) {
		zend_tpde_set_diagnostic(diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Darwin component entry table");
		if (has_executable) pthread_jit_write_protect_np(1);
		return FAILURE;
	}
	uint32_t mapped_entry_count = 0;
	uint32_t entry_section_index = UINT32_MAX;
	for (const SerializedSymbol &symbol : object.symbols) {
		if (!symbol.defined
				|| symbol.section >= published->sections.size()
				|| published->sections[symbol.section].mapping == nullptr) {
			continue;
		}
		for (uint32_t index = 0;
				index < image->component_entry_count; ++index) {
			const std::string expected = index == 0
				? "zend_native_entry"
				: "zend_native_component_" + std::to_string(index);
			if (symbol.name != expected
					|| code->component_entries[index] != nullptr) {
				continue;
			}
			code->component_entries[index] =
				reinterpret_cast<zend_native_frame_entry_t>(
					static_cast<uint8_t *>(
						published->sections[symbol.section].mapping)
					+ symbol.offset);
			if (index == 0) {
				entry_section_index = symbol.section;
			}
			mapped_entry_count++;
			break;
		}
	}
	if (mapped_entry_count != image->component_entry_count) {
		std::free(code->component_entries);
		code->component_entries = nullptr;
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE component entry symbol was not mapped");
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
	if (unwind_section == UINT32_MAX
			|| unwind_section >= published->sections.size()
			|| object.sections[unwind_section].is_virtual
			|| object.sections[unwind_section].size == 0
			|| published->sections[unwind_section].mapping == nullptr) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"Darwin TPDE image has no publishable unwind information");
		return FAILURE;
	}
	published->unwind_section =
		published->sections[unwind_section].mapping;
	if (__unw_add_dynamic_eh_frame_section != nullptr) {
		__unw_add_dynamic_eh_frame_section(
			reinterpret_cast<uintptr_t>(published->unwind_section));
	} else {
		__register_frame(published->unwind_section);
	}
	published->unwind_registered = true;

	const MappedSection &entry_section =
		published->sections[entry_section_index];
	code->mapping = entry_section.mapping;
	code->mapping_size = entry_section.mapping_size;
	code->entry = code->component_entries[0];
	code->component_entry_count = image->component_entry_count;
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
