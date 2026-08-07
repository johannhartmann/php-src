// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_apple_a64_abi.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_encodegen_a64.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"

#include <tpde/ELF.hpp>

#include <algorithm>
#include <array>
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
	std::vector<::tpde::Label> user_opcode_result_reload_labels_;
	std::optional<::tpde::Label> catch_dispatch_label_;
	uint32_t current_continuation_block_ = UINT32_MAX;
	bool continuation_edge_emitted_ = false;

	struct TargetBranchAssignment {
		::tpde::ValLocalIdx local_idx;
		uint32_t part;
	};
	using TargetBranchState = std::vector<TargetBranchAssignment>;
	TargetBranchState generator_gateway_state_;

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
	static constexpr uint32_t NUM_FIXED_ASSIGNMENTS[
		DarwinConfig::NUM_BANKS] = {10, 5};

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
				? DarwinConfig::FP_BANK : DarwinConfig::GP_BANK;
		}
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

	void add_offset(AsmReg destination, AsmReg base, uint64_t offset) {
		add_unsigned_offset(destination, base, offset);
	}

	void store_constant(
		AsmReg base, uint32_t offset, uint64_t value, uint32_t size) {
		ScratchReg constant{this};
		auto constant_reg = constant.alloc_gp();
		materialize_constant(
			value, DarwinConfig::GP_BANK, size, constant_reg);
		store_off(base, offset, constant_reg, size);
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
	AsmReg canonical_frame_register() {
		return canonical_value_register(
			IRValueRef{Adaptor::FRAME_VALUE});
	}
	ValuePart copy_fixed_argument(AsmReg source) {
		ScratchReg copy{this};
		auto copy_reg = copy.alloc_gp();
		mov(copy_reg, source, sizeof(void *));
		ValuePart value{DarwinConfig::GP_BANK, sizeof(void *)};
		value.set_value(this, std::move(copy));
		return value;
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
	template <typename BranchJump>
	void generate_branch_to_block(
			BranchJump jump, IRBlockRef target,
			bool needs_split, bool last_inst) {
		continuation_edge_emitted_ =
			continuation_edge_emitted_
			|| static_cast<uint32_t>(target)
				== current_continuation_block_;
		Base::generate_branch_to_block(
			jump, target, needs_split, last_inst);
	}
	void generate_uncond_branch(IRBlockRef target) {
		continuation_edge_emitted_ =
			continuation_edge_emitted_
			|| static_cast<uint32_t>(target)
				== current_continuation_block_;
		Base::generate_uncond_branch(target);
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
		ASM(CMPwi, decision.cur_reg(), 0);
		if (next == nonzero_target
				|| (next != zero_target && nonzero_needs_split)) {
			generate_branch_to_block(
				Jump::Jeq, zero_target, zero_needs_split, false);
			generate_branch_to_block(
				Jump::jmp, nonzero_target, false, true);
		} else if (next == zero_target) {
			generate_branch_to_block(
				Jump::Jne, nonzero_target,
				nonzero_needs_split, false);
			generate_branch_to_block(
				Jump::jmp, zero_target, false, true);
		} else {
			ZEND_ASSERT(!nonzero_needs_split);
			generate_branch_to_block(
				Jump::Jne, nonzero_target, false, false);
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

	bool cur_func_may_emit_calls() const {
		return adaptor->cur_func_may_emit_calls();
	}
	::tpde::SymRef cur_personality_func() const { return {}; }
	bool try_force_fixed_assignment(IRValueRef value) const {
		return value == IRValueRef{Adaptor::FRAME_VALUE}
			|| (!adaptor->typed_body()
				&& value == IRValueRef{
					Adaptor::EXECUTION_CONTEXT_ARGUMENT});
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
				? DarwinConfig::FP_BANK : DarwinConfig::GP_BANK,
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
		return ValuePart{value.bits, 8, ::tpde::RegBank{value.bank}};
	}
	void define_func_idx(IRFuncRef function, uint32_t index) {
		(void) function;
		(void) index;
	}
	void start_func(uint32_t index) {
		generator_resume_labels_.clear();
		generator_gateway_state_.clear();
		user_opcode_labels_.clear();
		user_opcode_dispatch_labels_.clear();
		user_opcode_result_reload_labels_.clear();
		catch_dispatch_label_.reset();
		Base::start_func(index);
	}
	void finish_func(uint32_t index) {
		if (catch_dispatch_label_.has_value()) {
			label_place(*catch_dispatch_label_);
			for (uint32_t i = 0; i < adaptor->plan()->instruction_count; ++i) {
				const zend_mir_instruction_record handler =
					zend_tpde_instruction_record_at(
						adaptor->plan(), &adaptor->plan()->instructions[i]);
				if ((handler.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
						&& handler.opcode != ZEND_MIR_OPCODE_FINALLY_ENTER)
						|| handler.block_id
							== adaptor->plan()->function.entry_block_id
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
				const auto expected_reg =
					::tpde::a64::AsmReg{::tpde::a64::AsmReg::R16};
				materialize_constant(
					ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
						| handler.source_position_id,
					DarwinConfig::GP_BANK, 4, expected_reg);
				ASM(CMPx,
					::tpde::a64::AsmReg{::tpde::a64::AsmReg::R0},
					expected_reg);
				const auto continued = text_writer.label_create();
				generate_raw_jump(Jump::Jne, continued);
				generate_exception_branch(handler_block);
				label_place(continued);
			}
			gen_func_epilog();
		}
		Base::finish_func(index);
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
		switch (descriptor->kind) {
			case ZEND_TPDE_MACHINE_REFERENCE_FRAME_SLOT:
				add_unsigned_offset(destination, canonical_frame_register(),
					static_cast<uint64_t>(descriptor->displacement));
				return;
			case ZEND_TPDE_MACHINE_REFERENCE_CONTEXT_FIELD:
			{
				auto context_ref = val_ref(
					IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
				auto context = context_ref.part(0);
				add_unsigned_offset(destination, context.load_to_reg(),
					static_cast<uint64_t>(descriptor->displacement));
				return;
			}
			case ZEND_TPDE_MACHINE_REFERENCE_LITERAL: {
				load_off(destination, canonical_frame_register(),
					static_cast<uint32_t>(
						offsetof(zend_execute_data, func)), 8);
				load_off(destination, destination,
					static_cast<uint32_t>(
						offsetof(zend_op_array, literals)), 8);
				const uint64_t literal_offset =
					static_cast<uint64_t>(
						descriptor->stable_storage_or_layout_id)
						* descriptor->scale
					+ static_cast<uint64_t>(descriptor->displacement);
				add_unsigned_offset(destination, destination, literal_offset);
				return;
			}
			default:
				ZEND_UNREACHABLE();
		}
	}

	void emit_integer_dispatch(
		const zend_tpde_multi_branch_case *branch_cases,
		uint32_t branch_case_count,
		std::span<const ::tpde::Label> labels,
		::tpde::a64::AsmReg value_reg,
		::tpde::a64::AsmReg temp_reg,
		::tpde::Label default_label);
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
	const zend_tpde_multi_branch_case *branch_cases,
	uint32_t branch_case_count,
	std::span<const ::tpde::Label> labels,
	::tpde::a64::AsmReg value_reg,
	::tpde::a64::AsmReg temp_reg,
	::tpde::Label default_label)
{
	std::vector<zend_tpde_integer_case> cases;
	int64_t low = 0;
	uint64_t range = 0;
	const zend_tpde_integer_dispatch_kind kind =
		zend_tpde_integer_dispatch(
			branch_cases, branch_case_count, &cases, &low, &range);
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

bool ZendCompilerA64::emit_machine_zval_type_info(
		zend_tpde_machine_value_kind kind,
		AsmReg payload_reg,
		AsmReg type_info_reg) {
	if (kind == ZEND_TPDE_MACHINE_VALUE_STRING_PTR) {
		const uint32_t type =
			zend_tpde_machine_value_zval_type(kind);
		const auto borrowed = text_writer.label_create();
		const auto ready = text_writer.label_create();
		load_off(type_info_reg, payload_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, type_info_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::Jne, borrowed);
		materialize_constant(
			type | (IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT),
			DarwinConfig::GP_BANK, 4, type_info_reg);
		generate_raw_jump(Jump::jmp, ready);
		label_place(borrowed);
		materialize_constant(
			type, DarwinConfig::GP_BANK, 4, type_info_reg);
		label_place(ready);
		return true;
	}
	if (kind == ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
		const auto immutable = text_writer.label_create();
		const auto ready = text_writer.label_create();

		load_off(type_info_reg, payload_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, type_info_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::Jne, immutable);
		materialize_constant(
			IS_ARRAY_EX, DarwinConfig::GP_BANK, 4, type_info_reg);
		generate_raw_jump(Jump::jmp, ready);
		label_place(immutable);
		materialize_constant(
			IS_ARRAY, DarwinConfig::GP_BANK, 4, type_info_reg);
		label_place(ready);
		return true;
	}
	const uint32_t type_info =
		zend_tpde_machine_value_zval_type_info(kind);
	if (type_info == IS_UNDEF) {
		return false;
	}
	materialize_constant(
		type_info, DarwinConfig::GP_BANK, 4, type_info_reg);
	return true;
}

bool ZendCompilerA64::emit_pointer_addref(
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
		load_off(header_reg, payload_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, header_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::Jne, copied);
		ScratchReg count{this};
		auto count_reg = count.alloc_gp();
		load_off(count_reg, payload_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, count_reg, count_reg, 1);
		store_off(payload_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			count_reg, 4);
		label_place(copied);
		return true;
	}
	ScratchReg count{this};
	auto count_reg = count.alloc_gp();
	load_off(count_reg, payload_reg,
		static_cast<uint32_t>(
			offsetof(zend_refcounted_h, refcount)), 4);
	ASM(ADDwi, count_reg, count_reg, 1);
	store_off(payload_reg,
		static_cast<uint32_t>(
			offsetof(zend_refcounted_h, refcount)),
		count_reg, 4);
	return true;
}

bool ZendCompilerA64::emit_materializations(
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
				|| offset > static_cast<uint64_t>(UINT32_MAX)
					- sizeof(zval)) {
			return false;
		}
		const IRValueRef value =
			node.liveness_operands[
				node.materialization_operand_index + index];
		const zend_tpde_machine_value_kind machine_kind =
			adaptor->machine_kind(value);
		auto value_ref = val_ref(value);
		auto payload = value_ref.part(0);
		AsmReg boolean_payload_reg = AsmReg::make_invalid();
		AsmReg payload_reg = AsmReg::make_invalid();
		if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_F64
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_BOOL
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
			payload_reg = payload.load_to_reg();
			if (machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
				boolean_payload_reg = payload_reg;
			}
			store_off(frame_reg, static_cast<uint32_t>(offset),
				payload_reg, 8);
		} else if (!EncodeBase::encode_zend_native_store_u64(
				GenericValuePart{GenericValuePart::Expr{
					frame_reg, static_cast<int64_t>(offset)}},
				GenericValuePart{std::move(payload)})) {
			return false;
		}
		const uint32_t type_offset = static_cast<uint32_t>(
			offset + offsetof(zval, u1.type_info));
		if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto type_info = value_ref.part(1);
			auto type_info_reg = type_info.load_to_reg();
			store_off(frame_reg, type_offset, type_info_reg, 4);
		} else if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_BOOL) {
			ScratchReg type_info{this};
			auto type_info_reg = type_info.alloc_gp();
			ASM(ORRx, type_info_reg, boolean_payload_reg,
				boolean_payload_reg);
			ASM(ADDwi, type_info_reg, type_info_reg, IS_FALSE);
			store_off(frame_reg, type_offset, type_info_reg, 4);
		} else {
			ScratchReg type_info_value{this};
			auto type_info_reg = type_info_value.alloc_gp();
			if (!emit_machine_zval_type_info(
					machine_kind, payload_reg, type_info_reg)) {
				return false;
			}
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

	if (register_boxed) {
		/*
		 * The preceding fast node may leave a TMP register-authoritative.
		 * Materialize that value before the boxed branch observes the
		 * canonical Zend frame or delegates uncommon truthiness to the helper.
		 */
		auto boxed = val_ref(node.operands[1]);
		auto payload = boxed.part(0);
		auto type_info = boxed.part(1);
		store_off(frame_reg, layout.operand_offset,
			payload.load_to_reg(), 8);
		store_off(frame_reg,
			layout.operand_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			type_info.load_to_reg(), 4);
	}

	if (register_string && layout.has_result) {
		auto [string_ref, string] = val_ref_single(node.operands[1]);
		auto string_reg = string.load_to_reg();
		store_off(frame_reg, layout.operand_offset, string_reg, 8);
		ScratchReg string_type{this};
		auto string_type_reg = string_type.alloc_gp();
		if (!emit_machine_zval_type_info(
				ZEND_TPDE_MACHINE_VALUE_STRING_PTR,
				string_reg, string_type_reg)) {
			return false;
		}
		store_off(frame_reg,
			layout.operand_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			string_type_reg, 4);
	}

	/*
	 * JMPZ_EX/JMPNZ_EX consume their source TMP before publishing the boolean
	 * result.  The generic helper owns that lifetime transition; the scalar
	 * truthiness fast path only observes the slot and must not overwrite a
	 * refcounted source value directly. Register-authoritative values have been
	 * materialized above so the helper observes the canonical frame value.
	 */
	if (layout.has_result) {
		generate_raw_jump(Jump::jmp, slow);
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
			load_off(type_reg, string_reg,
				static_cast<uint32_t>(offsetof(zend_string, len)), 8);
			generate_raw_jump(Jump{Jump::Cbz, type_reg, false}, falsey);
			ASM(CMPxi, type_reg, 1);
			generate_raw_jump(Jump::Jne, truthy);
			load_off(type_reg, string_reg,
				static_cast<uint32_t>(offsetof(zend_string, val)), 1);
			ASM(CMPwi, type_reg, '0');
			generate_raw_jump(Jump::Jeq, falsey);
			generate_raw_jump(Jump::jmp, truthy);
		}
	} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
				|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I64) {
		if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			load_off(value_reg, frame_reg,
				layout.operand_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, value_reg, value_reg, Z_TYPE_MASK);
			ASM(CMPwi, value_reg, IS_TRUE);
			generate_raw_jump(Jump::Jeq, truthy);
			generate_raw_jump(Jump::jmp, falsey);
		} else {
			load_off(value_reg, frame_reg, layout.operand_offset, 8);
			generate_raw_jump(
				Jump{Jump::Cbnz, value_reg, false}, truthy);
			generate_raw_jump(Jump::jmp, falsey);
		}
	} else {
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
	}

	if (register_string && !layout.has_result) {
		frame.reset();
		type.reset();
		value.reset();
		decision.reset();
		auto spilled = spill_before_branch();
		begin_branch_region();
		label_place(truthy);
		generate_branch_to_block(
			Jump::jmp, successors[0], false, false);
		label_place(falsey);
		generate_branch_to_block(
			Jump::jmp, successors[1], false, false);
		label_place(slow);
		generate_branch_to_block(
			Jump::jmp, successors[2], false, false);
		end_branch_region();
		release_spilled_regs(spilled);
		return true;
	}

	label_place(truthy);
	if (layout.has_result && layout.source_opcode == ZEND_JMPNZ_EX) {
		store_constant(frame_reg, layout.result_offset, 1, 8);
		store_constant(frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			IS_TRUE, 4);
	}
	materialize_constant(
		uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
	generate_raw_jump(Jump::jmp, ready);
	label_place(falsey);
	if (layout.has_result && layout.source_opcode == ZEND_JMPZ_EX) {
		store_constant(frame_reg, layout.result_offset, 0, 8);
		store_constant(frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			IS_FALSE, 4);
	}
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
	if (zend_mir_id_is_valid(mir.exception_block_id)) {
		generate_exception_branch(
			adaptor->block_ref(mir.exception_block_id));
	} else {
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(ValuePart{
			ZEND_NATIVE_EXCEPTION, 4,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		return_builder.ret();
	}
	label_place(valid);
	if (node.has_result) {
		auto result = result_ref(node.result);
		auto value = result.part(0);
		auto value_reg = value.alloc_reg();
		mov(value_reg, decision_reg, 4);
		value.set_modified();
		return true;
	}
	generate_cond_branch(
		Jump{Jump::Cbnz, decision_reg, false},
		successors[0], successors[1]);
	return true;
}

bool ZendCompilerA64::compile_boxed_cond_cold_branch(
		IRInstRef instruction) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.argument_index});
	if (node.operands.size() != 1 || successors.size() != 2) {
		return false;
	}
	auto [decision_ref, decision] = val_ref_single(node.operands[0]);
	generate_cond_branch(
		Jump{Jump::Cbnz, decision.load_to_reg(), false},
		successors[0], successors[1]);
	return true;
}

bool ZendCompilerA64::reload_generator_values(
		IRInstRef instruction, std::vector<ValueRef> &locked_values) {
	auto context_use = val_ref(
		IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT});
	if (!context_use.has_assignment() || context_use.variable_ref()) {
		return false;
	}
	auto context_value = context_use.part(0);
	const auto context_reg = context_value.load_to_reg();
	ValuePart resumed_frame{DarwinConfig::GP_BANK, 8};
	const auto frame_reg = resumed_frame.alloc_reg(this);
	load_off(frame_reg, context_reg,
		static_cast<uint32_t>(offsetof(
			zend_native_execution_context, current_execute_data)), 8);
	load_off(frame_reg, frame_reg, 0, 8);
	for (const IRValueRef operand :
			adaptor->generator_resume_values(instruction)) {
		const zend_mir_storage_id storage =
			adaptor->canonical_storage(operand);
		const zend_tpde_machine_value_kind machine_kind =
			adaptor->machine_kind(operand);
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		if (!zend_mir_id_is_valid(storage)
				|| !zend_tpde_machine_value_is_register_authoritative(
					machine_kind)
				|| offset + offsetof(zval, u1.type_info) > UINT32_MAX) {
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
				load_off(value_reg, frame_reg,
					static_cast<uint32_t>(
						offset + offsetof(zval, u1.type_info)), 4);
				ASM(CMPwi, value_reg, IS_TRUE);
				generate_raw_set(Jump::Jeq, value_reg);
			} else if (role == ZEND_TPDE_MACHINE_PART_VALUE
					|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				load_off(value_reg, frame_reg,
					static_cast<uint32_t>(offset), 8);
			} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
				load_off(value_reg, frame_reg,
					static_cast<uint32_t>(
						offset + offsetof(zval, u1.type_info)), 4);
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

bool ZendCompilerA64::compile_inst_impl(
	IRInstRef instruction, InstRange remaining_instructions) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	std::vector<ValueRef> generator_reload_locks;
	if (!emit_materializations(instruction)) {
		return false;
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
	if (node.kind == Adaptor::InstKind::TypedCallGuard) {
		if (node.operands.size() < 2
				|| node.argument_index == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		auto context_use = val_ref(node.operands[0]);
		auto context = context_use.part(0);
		const zend_tpde_machine_reference *observer_reference = nullptr;
		if (!adaptor->machine_reference(
				node.operands[1], &observer_reference)
				|| observer_reference->kind
					!= ZEND_TPDE_MACHINE_REFERENCE_CONTEXT_FIELD
				|| observer_reference->access_width != sizeof(bool)
				|| observer_reference->displacement < 0
				|| static_cast<uint64_t>(
					observer_reference->displacement) > UINT32_MAX) {
			return false;
		}
		ScratchReg observed{this};
		auto observed_reg = observed.alloc_gp();
		load_off(observed_reg, context.load_to_reg(),
			static_cast<uint32_t>(observer_reference->displacement),
			observer_reference->access_width);
		generate_cond_branch(
			Jump{Jump::Cbnz, observed_reg, false},
			IRBlockRef{node.argument_index},
			IRBlockRef{node.continuation_block});
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
		load_off(result.alloc_reg(), string.load_to_reg(),
			static_cast<uint32_t>(offsetof(zend_string, len)), 8);
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
						ASM(FMOVxd, value_reg, source_reg);
					} else {
						ASM(ORRx, value_reg, source_reg, source_reg);
					}
					break;
				case ZEND_TPDE_MACHINE_PART_TYPE_INFO:
					if (input_type == ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(ADDwi, value_reg, source_reg, IS_FALSE);
					} else if (input_type == ZEND_MIR_SCALAR_TYPE_I64) {
						materialize_constant(
							static_cast<uint64_t>(IS_LONG),
							DarwinConfig::GP_BANK, 4, value_reg);
					} else {
						materialize_constant(
							static_cast<uint64_t>(IS_DOUBLE),
							DarwinConfig::GP_BANK, 4, value_reg);
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
			auto payload_reg = payload.load_to_reg();
			ASM(ORRx, result.alloc_reg(), payload_reg, payload_reg);
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
		load_off(result.alloc_reg(), source.load_to_reg(),
			static_cast<uint32_t>(offsetof(zend_reference, val.value)), 8);
		result.set_modified();
		return true;
	}
	if (node.kind == Adaptor::InstKind::LoadFrame) {
		auto [source_ref, source] = val_ref_single(node.operands[0]);
		auto [result_ref, result] = result_ref_single(node.result);
		auto source_reg = source.load_to_reg();
		auto result_reg = result.alloc_reg();
		mov(result_reg, source_reg, 8);
		if (node.kind == Adaptor::InstKind::LoadFrame
				&& adaptor->plan()->entry_undef_temporary_count != 0) {
			auto initialized = text_writer.label_create();
			ScratchReg call_info{this};
			auto call_info_reg = call_info.alloc_gp();
			load_off(call_info_reg, result_reg,
				static_cast<uint32_t>(
					offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, call_info_reg, call_info_reg, ZEND_CALL_GENERATOR);
			ASM(CMPxi, call_info_reg, 0);
			generate_raw_jump(Jump::Jne, initialized);
			ScratchReg zero{this};
			auto zero_reg = zero.alloc_gp();
			materialize_constant(
				UINT64_C(0), DarwinConfig::GP_BANK, 8, zero_reg);
			for (uint32_t required = 0;
					required
						< adaptor->plan()->entry_undef_temporary_count;
					++required) {
				const uint32_t index =
					adaptor->plan()->entry_undef_temporary_indices[
						required];
				const uint32_t offset = static_cast<uint32_t>(
					(uint64_t{ZEND_CALL_FRAME_SLOT}
						+ adaptor->plan()->source_frame_variable_count + index)
					* sizeof(zval));
				store_off(result_reg, offset + sizeof(uint64_t), zero_reg, 8);
			}
			label_place(initialized);
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
				plan->source_opcodes[source].opcode);
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

				label_place(long_label);
				load_off(value_reg, slot_reg, 0, 8);
				emit_integer_dispatch(
					layout.cases, layout.case_count, case_labels,
					value_reg, constant_reg, default_label);

				label_place(string_label);
				load_off(value_reg, slot_reg, 0, 8);
				for (uint32_t case_index = 0;
						case_index < layout.case_count; ++case_index) {
					const zend_tpde_multi_branch_case &branch_case =
						layout.cases[case_index];
					if (branch_case.string_key != nullptr) {
						auto next_case = text_writer.label_create();
						const uint64_t length = branch_case.string_length;
						load_off(probe_reg, value_reg,
							static_cast<uint32_t>(
								offsetof(zend_string, len)), 8);
						materialize_constant(
							&length, DarwinConfig::GP_BANK, 8,
							constant_reg);
						ASM(CMPx, probe_reg, constant_reg);
						generate_raw_jump(Jump::Jne, next_case);
						size_t offset = 0;
						while (offset < branch_case.string_length) {
							const uint32_t width =
								branch_case.string_length - offset >= 8 ? 8
								: branch_case.string_length - offset >= 4 ? 4
								: branch_case.string_length - offset >= 2 ? 2 : 1;
							uint64_t expected = 0;
							memcpy(&expected,
								branch_case.string_key + offset, width);
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
	if (node.kind == Adaptor::InstKind::UserCallInit
			|| node.kind == Adaptor::InstKind::UserCallSend
			|| node.kind == Adaptor::InstKind::UserCallCheck
			|| node.kind == Adaptor::InstKind::UserCallExpand
			|| node.kind == Adaptor::InstKind::UserCallDo) {
		const zend_tpde_source_call_phase_entry *phase =
			zend_tpde_source_call_phase_at(
				adaptor->plan(), node.source_position);
		uint8_t required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_NONE;
		switch (node.kind) {
			case Adaptor::InstKind::UserCallInit:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_INIT;
				break;
			case Adaptor::InstKind::UserCallSend:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_SEND;
				break;
			case Adaptor::InstKind::UserCallCheck:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_CHECK;
				break;
			case Adaptor::InstKind::UserCallExpand:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_EXPAND;
				break;
			case Adaptor::InstKind::UserCallDo:
				required_phase = ZEND_TPDE_SOURCE_CALL_PHASE_DO;
				break;
			default:
				return false;
		}
		const bool send_phase =
			node.kind == Adaptor::InstKind::UserCallSend;
		const bool indexed_phase = send_phase
			|| node.kind == Adaptor::InstKind::UserCallCheck;
		if (phase == nullptr
				|| phase->instruction_index != node.mir_instruction_index
				|| (phase->phases & required_phase) == 0
				|| (indexed_phase
					? node.argument_index != phase->argument_index
					: node.argument_index != UINT32_MAX)
				|| (send_phase
					? node.operands.size()
						!= (((phase->operand_flags
							& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) != 0)
							? 3 : 2)
					: node.operands.size() != 2)) {
			return false;
		}
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
			const uint64_t argument_count = call.user_call->argument_count;
			const uint64_t frame_header_size =
				static_cast<uint64_t>(ZEND_CALL_FRAME_SLOT) * sizeof(zval);
			const uint64_t activation_offset =
				(frame_header_size + alignof(zend_native_direct_activation) - 1)
				& ~(static_cast<uint64_t>(
					alignof(zend_native_direct_activation)) - 1);
			const uint64_t placement_offset = activation_offset
				+ sizeof(zend_native_direct_activation);
			const uint64_t target_count = argument_count * 2 + 1;
			const uint64_t raw_setup_size = placement_offset
				+ argument_count * sizeof(zend_native_user_call_placement)
				+ target_count * sizeof(uint32_t);
			const uint64_t setup_size =
				(raw_setup_size + sizeof(zval) - 1)
				& ~(static_cast<uint64_t>(sizeof(zval)) - 1);
			const zend_mir_source_operand_ref &result_operand =
				call.user_call->do_result;
			const bool uses_discarded_return = result_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED;
			uint64_t result_storage = 0;
			if (!uses_discarded_return) {
				if ((result_operand.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
						&& result_operand.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
						|| (result_operand.slot_kind != ZEND_MIR_SOURCE_SLOT_CV
							&& result_operand.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_TMP
							&& result_operand.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_VAR)) {
					return false;
				}
				result_storage = result_operand.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV
					? result_operand.index
					: static_cast<uint64_t>(
						adaptor->plan()->source_frame_variable_count)
						+ result_operand.index;
			}
			const uint64_t result_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + result_storage) * sizeof(zval);
			if (setup_size > UINT32_MAX || activation_offset > UINT32_MAX
					|| placement_offset - activation_offset > UINT32_MAX
					|| argument_count > UINT32_MAX
					|| (!uses_discarded_return && result_offset > UINT32_MAX)) {
				return false;
			}

			auto initialize_setup = [&](auto setup_reg) {
				ScratchReg activation{this};
				ScratchReg scratch{this};
				auto activation_reg = activation.alloc_gp();
				auto scratch_reg = scratch.alloc_gp();
				mov(activation_reg, setup_reg, 8);
				add_offset(activation_reg, activation_reg,
					static_cast<uint32_t>(activation_offset));
				for (uint32_t offset = 0;
						offset < sizeof(zend_native_direct_activation);
						offset += sizeof(uint64_t)) {
					store_constant(activation_reg, offset, 0, 8);
				}
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, setup_frame)),
					setup_reg, 8);
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, caller)),
					canonical_frame_register(), 8);
				load_off(scratch_reg, canonical_frame_register(),
					static_cast<uint32_t>(offsetof(zend_execute_data, call)), 8);
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, pending_call)),
					scratch_reg, 8);
				auto descriptor = image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id);
				auto descriptor_scratch =
					std::move(descriptor).into_scratch(this);
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, descriptor)),
					descriptor_scratch.cur_reg(), 8);
				descriptor_scratch.reset();
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, setup_size)),
					setup_size, 4);
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation,
						placement_capacity)), argument_count, 4);
				mov(scratch_reg, activation_reg, 8);
				add_offset(scratch_reg, scratch_reg,
					static_cast<uint32_t>(
						placement_offset - activation_offset));
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							placements)), scratch_reg, 8);
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, setup_record)), 1, 1);
				load_off(scratch_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				ScratchReg previous{this};
				auto previous_reg = previous.alloc_gp();
				load_off(previous_reg, scratch_reg, 0, 8);
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, previous)),
					previous_reg, 8);
				store_off(scratch_reg, 0, activation_reg, 8);
			};

			/* The fast and growth paths are target-local control flow, so TPDE's
			 * block allocator cannot reconcile their register assignments.  Give
			 * every live value canonical backing before the split and discard the
			 * emitted slow-path register state at the join. */
			auto setup_spilled = spill_target_branch_state();
			auto slow_setup = text_writer.label_create();
			auto setup_ready = text_writer.label_create();
			{
				ScratchReg top_address{this};
				ScratchReg setup{this};
				ScratchReg available{this};
				auto top_address_reg = top_address.alloc_gp();
				auto setup_reg = setup.alloc_gp();
				auto available_reg = available.alloc_gp();
				load_off(top_address_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_stack_top)), 8);
				load_off(setup_reg, top_address_reg, 0, 8);
				load_off(available_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_stack_end)), 8);
				load_off(available_reg, available_reg, 0, 8);
				ASM(SUBx, available_reg, available_reg, setup_reg);
				compare_unsigned_immediate(available_reg, setup_size);
				generate_raw_jump(Jump::Jcc, slow_setup);
				store_constant(setup_reg,
					static_cast<uint32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info)), 0, 4);
				add_offset(available_reg, setup_reg,
					static_cast<uint32_t>(setup_size));
				store_off(top_address_reg, 0, available_reg, 8);
				initialize_setup(setup_reg);
				generate_raw_jump(Jump::jmp, setup_ready);
			}
			label_place(slow_setup);
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(ValuePart{setup_size, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RESERVE));
				ValuePart setup_value{DarwinConfig::GP_BANK, 8};
				builder.add_ret(setup_value, ::tpde::CCAssignment{});
				auto setup_scratch =
					std::move(setup_value).into_scratch(this);
				initialize_setup(setup_scratch.cur_reg());
			}
			label_place(setup_ready);
			reconcile_target_branch_state(setup_spilled);
			ValuePart resolution_status{DarwinConfig::GP_BANK, 4};
			{
				ScratchReg activation{this};
				auto activation_reg = activation.alloc_gp();
				load_off(activation_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				load_off(activation_reg, activation_reg, 0, 8);
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				ValuePart activation_value{DarwinConfig::GP_BANK, 8};
				activation_value.set_value(this, std::move(activation));
				builder.add_arg(
					std::move(activation_value), ::tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					call.call_site->target_id), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_USER_CALL_RESOLVE));
				builder.add_ret(resolution_status, ::tpde::CCAssignment{});
			}
			auto resolution_spilled = spill_target_branch_state();
			auto resolved = text_writer.label_create();
			auto status_reg = resolution_status.cur_reg_or_load(this);
			ASM(CMPwi, status_reg,
				ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS);
			generate_raw_jump(Jump::Jeq, resolved);
			resolution_status.reset(this);
			{
				ScratchReg resolution{this};
				auto resolution_reg = resolution.alloc_gp();
				load_off(resolution_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				load_off(resolution_reg, resolution_reg, 0, 8);
				add_offset(resolution_reg, resolution_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)));
				ScratchReg ownership{this};
				auto ownership_reg = ownership.alloc_gp();
				load_off(ownership_reg, resolution_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_user_call_resolution, ownership)), 4);
				auto ownership_free = text_writer.label_create();
				ASM(CMPwi, ownership_reg, 0);
				generate_raw_jump(Jump::Jeq, ownership_free);
				ownership.reset();
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					ValuePart resolution_value{DarwinConfig::GP_BANK, 8};
					resolution_value.set_value(this, std::move(resolution));
					builder.add_arg(
						std::move(resolution_value), ::tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_RELEASE_RESOLUTION));
				}
				label_place(ownership_free);
			}
			{
				ScratchReg active_address{this};
				ScratchReg activation{this};
				ScratchReg previous{this};
				auto active_address_reg = active_address.alloc_gp();
				auto activation_reg = activation.alloc_gp();
				auto previous_reg = previous.alloc_gp();
				load_off(active_address_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				load_off(activation_reg, active_address_reg, 0, 8);
				load_off(previous_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, previous)), 8);
				store_off(active_address_reg, 0, previous_reg, 8);
				previous.reset();
				active_address.reset();
				ScratchReg setup{this};
				auto setup_reg = setup.alloc_gp();
				load_off(setup_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, setup_frame)), 8);
				load_off(setup_reg, setup_reg,
					static_cast<uint32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info)), 4);
				ASM(TSTwi, setup_reg, ZEND_CALL_ALLOCATED);
				auto pop_allocated = text_writer.label_create();
				auto popped = text_writer.label_create();
				generate_raw_jump(Jump::Jne, pop_allocated);
				setup.reset();
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, setup_record)), 0, 1);
				ScratchReg inline_setup{this};
				ScratchReg top_address{this};
				auto inline_setup_reg = inline_setup.alloc_gp();
				auto top_address_reg = top_address.alloc_gp();
				load_off(inline_setup_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, setup_frame)), 8);
				load_off(top_address_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_stack_top)), 8);
				store_off(top_address_reg, 0, inline_setup_reg, 8);
				inline_setup.reset();
				top_address.reset();
				generate_raw_jump(Jump::jmp, popped);
				label_place(pop_allocated);
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					ValuePart activation_value{DarwinConfig::GP_BANK, 8};
					activation_value.set_value(this, std::move(activation));
					builder.add_arg(
						std::move(activation_value), ::tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_FRAME_ACTIVATION_POP));
				}
				label_place(popped);
			}
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
			} else {
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_EXCEPTION, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(resolved);
			resolution_status.reset(this);
			reconcile_target_branch_state(resolution_spilled);

			auto initialize_callee = [&](auto callee_reg,
					auto activation_reg, bool allocated) {
				ScratchReg scratch{this};
				auto scratch_reg = scratch.alloc_gp();
				for (uint32_t offset = 0; offset < frame_header_size;
						offset += sizeof(uint64_t)) {
					store_constant(callee_reg, offset, 0, 8);
				}
				load_off(scratch_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							object_or_called_scope)), 8);
				store_off(callee_reg,
					static_cast<uint32_t>(offsetof(zend_execute_data, This)),
					scratch_reg, 8);
				load_off(scratch_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							call_info)), 4);
				if (allocated) {
					ASM(ORRwi, scratch_reg, scratch_reg, ZEND_CALL_ALLOCATED);
				}
				store_off(callee_reg,
					static_cast<uint32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info)), scratch_reg, 4);
				load_off(scratch_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							argument_count)), 4);
				store_off(callee_reg,
					static_cast<uint32_t>(offsetof(zend_execute_data, This)
						+ offsetof(zval, u2.num_args)), scratch_reg, 4);
				load_off(scratch_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							function)), 8);
				store_off(callee_reg,
					static_cast<uint32_t>(offsetof(zend_execute_data, func)),
					scratch_reg, 8);
				load_off(scratch_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, pending_call)), 8);
				store_off(callee_reg,
					static_cast<uint32_t>(offsetof(
						zend_execute_data, prev_execute_data)),
					scratch_reg, 8);
				if (uses_discarded_return) {
					mov(scratch_reg, activation_reg, 8);
					add_offset(scratch_reg, scratch_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							discarded_return)));
				} else {
					mov(scratch_reg, canonical_frame_register(), 8);
					add_offset(scratch_reg, scratch_reg,
						static_cast<uint32_t>(result_offset));
				}
				store_off(callee_reg,
					static_cast<uint32_t>(offsetof(
						zend_execute_data, return_value)), scratch_reg, 8);
				store_off(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, callee)), callee_reg, 8);
				store_off(canonical_frame_register(),
					static_cast<uint32_t>(offsetof(zend_execute_data, call)),
					callee_reg, 8);
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, raw_arguments_owned)),
					1, 1);
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation,
						uses_discarded_return)),
					uses_discarded_return ? 1 : 0, 1);
				store_constant(activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, dynamic_target)), 1, 1);
				ScratchReg argument_ptr{this};
				ScratchReg remaining{this};
				auto argument_ptr_reg = argument_ptr.alloc_gp();
				auto remaining_reg = remaining.alloc_gp();
				mov(argument_ptr_reg, callee_reg, 8);
				add_offset(argument_ptr_reg, argument_ptr_reg,
					static_cast<uint32_t>(frame_header_size));
				load_off(remaining_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							argument_count)), 4);
				auto arguments_done = text_writer.label_create();
				auto argument_loop = text_writer.label_create();
				ASM(CMPwi, remaining_reg, 0);
				generate_raw_jump(Jump::Jeq, arguments_done);
				label_place(argument_loop);
				store_constant(argument_ptr_reg,
					static_cast<uint32_t>(offsetof(zval, u1.type_info)),
					IS_UNDEF, 4);
				add_offset(argument_ptr_reg, argument_ptr_reg, sizeof(zval));
				ASM(SUBSwi, remaining_reg, remaining_reg, 1);
				generate_raw_jump(Jump::Jne, argument_loop);
				label_place(arguments_done);
			};

			auto callee_spilled = spill_target_branch_state();
			auto slow_callee = text_writer.label_create();
			auto callee_ready = text_writer.label_create();
			{
				ScratchReg active_address{this};
				ScratchReg activation{this};
				ScratchReg top_address{this};
				ScratchReg callee{this};
				ScratchReg available{this};
				ScratchReg size{this};
				auto active_address_reg = active_address.alloc_gp();
				auto activation_reg = activation.alloc_gp();
				auto top_address_reg = top_address.alloc_gp();
				auto callee_reg = callee.alloc_gp();
				auto available_reg = available.alloc_gp();
				auto size_reg = size.alloc_gp();
				load_off(active_address_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				load_off(activation_reg, active_address_reg, 0, 8);
				load_off(top_address_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_stack_top)), 8);
				load_off(callee_reg, top_address_reg, 0, 8);
				load_off(available_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_stack_end)), 8);
				load_off(available_reg, available_reg, 0, 8);
				ASM(SUBx, available_reg, available_reg, callee_reg);
				load_off(size_reg, activation_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							frame_size)), 4);
				ASM(CMPx, available_reg, size_reg);
				generate_raw_jump(Jump::Jlo, slow_callee);
				ASM(ADDx, available_reg, callee_reg, size_reg);
				store_off(top_address_reg, 0, available_reg, 8);
				initialize_callee(callee_reg, activation_reg, false);
				generate_raw_jump(Jump::jmp, callee_ready);
			}
			label_place(slow_callee);
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), ::tpde::CCAssignment{});
				ScratchReg size{this};
				auto size_reg = size.alloc_gp();
				load_off(size_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				load_off(size_reg, size_reg, 0, 8);
				load_off(size_reg, size_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							frame_size)), 4);
				ValuePart size_value{DarwinConfig::GP_BANK, 4};
				size_value.set_value(this, std::move(size));
				builder.add_arg(std::move(size_value), ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_RESERVE_DYNAMIC_FRAME));
				ValuePart callee_value{DarwinConfig::GP_BANK, 8};
				builder.add_ret(callee_value, ::tpde::CCAssignment{});
				auto callee_scratch =
					std::move(callee_value).into_scratch(this);
				ScratchReg activation{this};
				auto activation_reg = activation.alloc_gp();
				load_off(activation_reg,
					context_register(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, active_direct_call)), 8);
				load_off(activation_reg, activation_reg, 0, 8);
				initialize_callee(
					callee_scratch.cur_reg(), activation_reg, true);
			}
			label_place(callee_ready);
			reconcile_target_branch_state(callee_spilled);
			return true;
		}
		const zend_tpde_instruction &call =
			adaptor->plan()->instructions[node.mir_instruction_index];
		if (call.user_call == nullptr
				|| call.user_call->argument_count != call.call_argument_count) {
			return false;
		}
		auto load_active_activation = [&](auto destination_reg) {
			load_off(destination_reg,
				context_register(),
				static_cast<uint32_t>(offsetof(
					zend_native_execution_context, active_direct_call)), 8);
			load_off(destination_reg, destination_reg, 0, 8);
		};
		auto emit_phase_failure = [&]() {
			{
				ScratchReg activation{this};
				auto activation_reg = activation.alloc_gp();
				load_active_activation(activation_reg);
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				ValuePart activation_value{DarwinConfig::GP_BANK, 8};
				activation_value.set_value(this, std::move(activation));
				builder.add_arg(
					std::move(activation_value), ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE));
			}
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					node.source_position, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION));
				ValuePart status{DarwinConfig::GP_BANK, 4};
				builder.add_ret(status, ::tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				auto prepared = text_writer.label_create();
				ASM(CMPwi, status_reg, SUCCESS);
				generate_raw_jump(Jump::Jeq, prepared);
				status.reset(this);
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_BAILOUT, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_builder.ret();
				label_place(prepared);
				status.reset(this);
			}
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
			} else {
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_EXCEPTION, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_builder.ret();
			}
		};
		if (node.kind == Adaptor::InstKind::UserCallSend) {
			if (node.argument_index >= call.user_call->argument_count) {
				return false;
			}
			auto deferred = text_writer.label_create();
			auto completed = text_writer.label_create();
			ScratchReg activation{this};
			ScratchReg placement{this};
			ScratchReg flags{this};
			auto activation_reg = activation.alloc_gp();
			auto placement_reg = placement.alloc_gp();
			auto flags_reg = flags.alloc_gp();
			load_active_activation(activation_reg);
			load_off(placement_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)
					+ offsetof(zend_native_user_call_resolution,
						placements)), 8);
			activation.reset();
			add_offset(placement_reg, placement_reg,
				node.argument_index
					* static_cast<uint32_t>(
						sizeof(zend_native_user_call_placement)));
			load_off(flags_reg, placement_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_user_call_placement, flags)), 4);
			ASM(TSTwi, flags_reg,
				ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION);
			generate_raw_jump(Jump::Jne, deferred);
			flags.reset();
			ValuePart send_status{DarwinConfig::GP_BANK, 4};
			if ((phase->operand_flags
					& ZEND_TPDE_SOURCE_CALL_OPERAND_DIRECT_VALUE) != 0) {
				if (node.operands.size() != 3) {
					return false;
				}
				ScratchReg target{this};
				auto target_reg = target.alloc_gp();
				load_off(target_reg, placement_reg,
					static_cast<uint32_t>(offsetof(
						zend_native_user_call_placement, target_index)), 4);
				placement.reset();
				if (target_reg == AsmReg{AsmReg::R0}) {
					ScratchReg relocated{this};
					auto relocated_reg = relocated.alloc_gp();
					mov(relocated_reg, target_reg, 4);
					target.reset();
					target = std::move(relocated);
					target_reg = relocated_reg;
				}
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), ::tpde::CCAssignment{});
				ValuePart target_value{DarwinConfig::GP_BANK, 4};
				target_value.set_value(this, std::move(target));
				builder.add_arg(
					std::move(target_value), ::tpde::CCAssignment{});
				builder.add_arg(CallArg{node.operands[2]});
				if (adaptor->exact_type(node.operands[2])
						== ZEND_MIR_SCALAR_TYPE_F64) {
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE));
				} else {
					builder.add_arg(ValuePart{
						static_cast<uint32_t>(
							adaptor->exact_type(node.operands[2])),
						4, DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER));
				}
				/* Scalar setters are infallible after resolved placement. */
				generate_raw_jump(Jump::jmp, completed);
			} else {
				placement.reset();
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), ::tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id),
					::tpde::CCAssignment{});
				builder.add_arg(ValuePart{node.argument_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_SET_SOURCE_ARGUMENT));
				builder.add_ret(send_status, ::tpde::CCAssignment{});
				auto status_reg = send_status.cur_reg_or_load(this);
				ASM(CMPwi, status_reg, SUCCESS);
				generate_raw_jump(Jump::Jeq, completed);
				send_status.reset(this);
				emit_phase_failure();
			}
			label_place(deferred);
			{
				ScratchReg runtime_activation{this};
				auto runtime_activation_reg =
					runtime_activation.alloc_gp();
				load_active_activation(runtime_activation_reg);
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				ValuePart activation_value{DarwinConfig::GP_BANK, 8};
				activation_value.set_value(
					this, std::move(runtime_activation));
				builder.add_arg(
					std::move(activation_value), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{node.argument_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_USER_CALL_SEND_RESOLVED_ARGUMENT));
				ValuePart runtime_status{DarwinConfig::GP_BANK, 4};
				builder.add_ret(runtime_status, ::tpde::CCAssignment{});
				auto runtime_status_reg =
					runtime_status.cur_reg_or_load(this);
				ASM(CMPwi, runtime_status_reg, SUCCESS);
				generate_raw_jump(Jump::Jeq, completed);
				runtime_status.reset(this);
				emit_phase_failure();
			}
			label_place(completed);
			return true;
		}
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
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{node.argument_index, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CHECK_FUNC_ARG_RESOLVED));
			ValuePart checked{DarwinConfig::GP_BANK, 4};
			builder.add_ret(checked, ::tpde::CCAssignment{});
			auto checked_reg = checked.cur_reg_or_load(this);
			auto by_value = text_writer.label_create();
			auto updated = text_writer.label_create();
			ASM(CMPwi, checked_reg, ZEND_NATIVE_CHECK_FUNC_ARG_EXCEPTION);
			auto valid = text_writer.label_create();
			generate_raw_jump(Jump::Jne, valid);
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
			load_off(callee_reg, active_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, callee)), 8);
			load_off(call_info_reg, callee_reg,
				static_cast<uint32_t>(offsetof(zend_execute_data, This)
					+ offsetof(zval, u1.type_info)), 4);
			ASM(CMPwi, checked_reg, ZEND_NATIVE_CHECK_FUNC_ARG_BY_VALUE);
			generate_raw_jump(Jump::Jeq, by_value);
			ASM(ORRwi, call_info_reg, call_info_reg,
				ZEND_CALL_SEND_ARG_BY_REF);
			generate_raw_jump(Jump::jmp, updated);
			label_place(by_value);
			ASM(ANDwi, call_info_reg, call_info_reg,
				~ZEND_CALL_SEND_ARG_BY_REF);
			label_place(updated);
			store_off(callee_reg,
				static_cast<uint32_t>(offsetof(zend_execute_data, This)
					+ offsetof(zval, u1.type_info)), call_info_reg, 4);
			return true;
		}
		if (node.kind == Adaptor::InstKind::UserCallExpand) {
			ScratchReg activation{this};
			ScratchReg placement_flags{this};
			auto activation_reg = activation.alloc_gp();
			auto placement_flags_reg = placement_flags.alloc_gp();
			load_active_activation(activation_reg);
			load_off(placement_flags_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)
					+ offsetof(zend_native_user_call_resolution,
						placement_flags)), 4);
			ASM(TSTwi, placement_flags_reg,
				ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION);
			auto complete = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, complete);
			placement_flags.reset();
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_USER_CALL_EXPAND_ARGUMENTS));
			ValuePart expanded{DarwinConfig::GP_BANK, 8};
			builder.add_ret(expanded, ::tpde::CCAssignment{});
			auto expanded_reg = expanded.cur_reg_or_load(this);
			ASM(CMPxi, expanded_reg, 0);
			generate_raw_jump(Jump::Jne, complete);
			expanded.reset(this);
			emit_phase_failure();
			label_place(complete);
			return true;
		}
		if (node.kind != Adaptor::InstKind::UserCallDo) {
			return false;
		}
		auto release_active = [&]() {
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE));
		};
		auto branch_released_exception = [&]() {
			ScratchReg exception{this};
			auto exception_reg = exception.alloc_gp();
			load_off(exception_reg,
				context_register(),
				static_cast<uint32_t>(offsetof(
					zend_native_execution_context, exception)), 8);
			load_off(exception_reg, exception_reg, 0, 8);
			auto no_exception = text_writer.label_create();
			ASM(CMPxi, exception_reg, 0);
			generate_raw_jump(Jump::Jeq, no_exception);
			exception.reset();
			{
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					node.source_position, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION));
				ValuePart status{DarwinConfig::GP_BANK, 4};
				builder.add_ret(status, ::tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				auto prepared = text_writer.label_create();
				ASM(CMPwi, status_reg, SUCCESS);
				generate_raw_jump(Jump::Jeq, prepared);
				status.reset(this);
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_BAILOUT, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_builder.ret();
				label_place(prepared);
				status.reset(this);
			}
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
			} else {
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_EXCEPTION, 4, DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(no_exception);
		};

		/* Publish the canonical DO source position before any DO transition. */
		{
			ScratchReg opline{this};
			auto opline_reg = opline.alloc_gp();
			load_off(opline_reg, canonical_frame_register(),
				static_cast<uint32_t>(offsetof(zend_execute_data, func)), 8);
			load_off(opline_reg, opline_reg,
				static_cast<uint32_t>(offsetof(zend_op_array, opcodes)), 8);
			add_offset(opline_reg, opline_reg,
				static_cast<uint64_t>(node.source_position) * sizeof(zend_op));
			store_off(canonical_frame_register(),
				static_cast<uint32_t>(offsetof(zend_execute_data, opline)),
				opline_reg, 8);
		}

		auto normalized = text_writer.label_create();
		auto no_call = text_writer.label_create();
		auto internal_call = text_writer.label_create();
		auto user_call = text_writer.label_create();
		auto do_succeeded = text_writer.label_create();
		{
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(copy_fixed_argument(
				canonical_frame_register()), ::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id),
				::tpde::CCAssignment{});
			ScratchReg activation{this};
			ScratchReg target_kind{this};
			auto activation_reg = activation.alloc_gp();
			auto target_kind_reg = target_kind.alloc_gp();
			load_active_activation(activation_reg);
			load_off(target_kind_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)
					+ offsetof(zend_native_user_call_resolution,
						target_kind)), 4);
			ASM(CMPwi, target_kind_reg,
				ZEND_NATIVE_USER_CALL_TARGET_NO_CALL);
			generate_raw_jump(Jump::Jeq, no_call);
			ASM(CMPwi, target_kind_reg,
				ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE);
			generate_raw_jump(Jump::Jne, normalized);
			target_kind.reset();
			ScratchReg callee{this};
			auto callee_reg = callee.alloc_gp();
			load_off(callee_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, callee)), 8);
			ScratchReg resolution{this};
			auto resolution_reg = resolution.alloc_gp();
			mov(resolution_reg, activation_reg, 8);
			add_offset(resolution_reg, resolution_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)));
			activation.reset();
			ValuePart callee_value{DarwinConfig::GP_BANK, 8};
			callee_value.set_value(this, std::move(callee));
			builder.add_arg(
				std::move(callee_value), ::tpde::CCAssignment{});
			ValuePart resolution_value{DarwinConfig::GP_BANK, 8};
			resolution_value.set_value(this, std::move(resolution));
			builder.add_arg(
				std::move(resolution_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_USER_CALL_NORMALIZE_RESOLUTION));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPwi, status_reg,
				ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS);
			generate_raw_jump(Jump::Jeq, normalized);
			status.reset(this);
			emit_phase_failure();
		}
		label_place(normalized);
		{
			ScratchReg activation{this};
			ScratchReg target_kind{this};
			auto activation_reg = activation.alloc_gp();
			auto target_kind_reg = target_kind.alloc_gp();
			load_active_activation(activation_reg);
			load_off(target_kind_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)
					+ offsetof(zend_native_user_call_resolution,
						target_kind)), 4);
			ASM(CMPwi, target_kind_reg,
				ZEND_NATIVE_USER_CALL_TARGET_INTERNAL);
			generate_raw_jump(Jump::Jeq, internal_call);
			generate_raw_jump(Jump::jmp, user_call);
		}

		label_place(no_call);
		release_active();
		branch_released_exception();
		generate_raw_jump(Jump::jmp, do_succeeded);

		label_place(internal_call);
		{
			ScratchReg activation{this};
			ScratchReg callee{this};
			ScratchReg entry{this};
			auto activation_reg = activation.alloc_gp();
			auto callee_reg = callee.alloc_gp();
			auto entry_reg = entry.alloc_gp();
			load_active_activation(activation_reg);
			store_constant(activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, internal_target)), 1, 1);
			load_off(callee_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, callee)), 8);
			load_off(entry_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)
					+ offsetof(zend_native_user_call_resolution,
						invoke_entry)), 8);
			activation.reset();
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart callee_value{DarwinConfig::GP_BANK, 8};
			callee_value.set_value(this, std::move(callee));
			builder.add_arg(
				std::move(callee_value), ::tpde::CCAssignment{});
			builder.add_arg(context_argument(), ::tpde::CCAssignment{});
			ValuePart entry_value{DarwinConfig::GP_BANK, 8};
			entry_value.set_value(this, std::move(entry));
			builder.call(std::move(entry_value));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
			auto returned = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, returned);
			status.reset(this);
			emit_phase_failure();
			label_place(returned);
			status.reset(this);
			release_active();
			branch_released_exception();
			generate_raw_jump(Jump::jmp, do_succeeded);
		}

		label_place(user_call);
		ValuePart prepared{DarwinConfig::GP_BANK, 4};
		{
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FRAME_PREPARE));
			builder.add_ret(prepared, ::tpde::CCAssignment{});
		}
		{
			auto prepared_reg = prepared.cur_reg_or_load(this);
			ASM(CMPwi, prepared_reg, SUCCESS);
			auto prepare_ok = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, prepare_ok);
			prepared.reset(this);
			emit_phase_failure();
			label_place(prepare_ok);
			prepared.reset(this);
		}
		ValuePart begin_status{DarwinConfig::GP_BANK, 4};
		{
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_FRAME_OBSERVER_BEGIN));
			builder.add_ret(begin_status, ::tpde::CCAssignment{});
		}
		auto invoke_user = text_writer.label_create();
		auto observer_end = text_writer.label_create();
		{
			auto begin_reg = begin_status.cur_reg_or_load(this);
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			store_off(activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, status)), begin_reg, 4);
			activation.reset();
			ASM(CMPwi, begin_reg, ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::Jeq, invoke_user);
			begin_status.reset(this);
			generate_raw_jump(Jump::jmp, observer_end);
		}
		label_place(invoke_user);
		begin_status.reset(this);
		{
			ScratchReg activation{this};
			ScratchReg callee{this};
			ScratchReg entry{this};
			auto activation_reg = activation.alloc_gp();
			auto callee_reg = callee.alloc_gp();
			auto entry_reg = entry.alloc_gp();
			load_active_activation(activation_reg);
			load_off(callee_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, callee)), 8);
			load_off(entry_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, resolution)
					+ offsetof(zend_native_user_call_resolution,
						invoke_entry)), 8);
			activation.reset();
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart callee_value{DarwinConfig::GP_BANK, 8};
			callee_value.set_value(this, std::move(callee));
			builder.add_arg(
				std::move(callee_value), ::tpde::CCAssignment{});
			builder.add_arg(context_argument(), ::tpde::CCAssignment{});
			ValuePart entry_value{DarwinConfig::GP_BANK, 8};
			entry_value.set_value(this, std::move(entry));
			builder.call(std::move(entry_value));
			ValuePart status{DarwinConfig::GP_BANK, 4};
			builder.add_ret(status, ::tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ScratchReg active{this};
			auto active_reg = active.alloc_gp();
			load_active_activation(active_reg);
			store_off(active_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, status)), status_reg, 4);
			active.reset();
			status.reset(this);
		}
		label_place(observer_end);
		{
			ScratchReg activation{this};
			ScratchReg status{this};
			auto activation_reg = activation.alloc_gp();
			auto status_reg = status.alloc_gp();
			load_active_activation(activation_reg);
			load_off(status_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, status)), 4);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			ValuePart status_value{DarwinConfig::GP_BANK, 4};
			status_value.set_value(this, std::move(status));
			builder.add_arg(
				std::move(status_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_FRAME_OBSERVER_END));
		}
		ValuePart finalized{DarwinConfig::GP_BANK, 4};
		{
			ScratchReg activation{this};
			ScratchReg status{this};
			auto activation_reg = activation.alloc_gp();
			auto status_reg = status.alloc_gp();
			load_active_activation(activation_reg);
			load_off(status_reg, activation_reg,
				static_cast<uint32_t>(offsetof(
					zend_native_direct_activation, status)), 4);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{DarwinConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), ::tpde::CCAssignment{});
			ValuePart status_value{DarwinConfig::GP_BANK, 4};
			status_value.set_value(this, std::move(status));
			builder.add_arg(
				std::move(status_value), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FRAME_FINALIZE));
			builder.add_ret(finalized, ::tpde::CCAssignment{});
		}
		{
			auto finalized_reg = finalized.cur_reg_or_load(this);
			auto user_returned = text_writer.label_create();
			ASM(CMPwi, finalized_reg, ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::Jeq, user_returned);
			ASM(CMPwi, finalized_reg, ZEND_NATIVE_GENERATOR_CREATED);
			generate_raw_jump(Jump::Jeq, user_returned);
			finalized.reset(this);
			emit_phase_failure();
			label_place(user_returned);
			finalized.reset(this);
			release_active();
			branch_released_exception();
			generate_raw_jump(Jump::jmp, do_succeeded);
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
			if (offset > UINT32_MAX - sizeof(zval)) {
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
				load_off(payload.alloc_reg(), canonical_frame_register(),
					static_cast<uint32_t>(offset), 8);
				load_off(type_info.alloc_reg(), canonical_frame_register(),
					static_cast<uint32_t>(offset + offsetof(zval, u1.type_info)), 4);
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] = result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				if (adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_F64) {
					ScratchReg payload{this};
					auto payload_reg = payload.alloc_gp();
					load_off(payload_reg, canonical_frame_register(),
						static_cast<uint32_t>(offset), 8);
					ASM(FMOVdx, result_reg, payload_reg);
				} else if (adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_BOOL) {
					load_off(result_reg, canonical_frame_register(),
						static_cast<uint32_t>(
							offset + offsetof(zval, u1.type_info)), 4);
					ASM(SUBwi, result_reg, result_reg, IS_FALSE);
				} else {
					load_off(result_reg, canonical_frame_register(),
						static_cast<uint32_t>(offset), 8);
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
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		builder.add_arg(CallArg{node.operands[0]});
		if (call.entry_cell != nullptr) {
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site->target_id), ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				UINT64_C(0), 8, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
		} else {
			builder.add_arg(ValuePart{
				UINT64_C(0), 8, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site->target_id), ::tpde::CCAssignment{});
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
					add_unsigned_offset(
						target_reg, target_reg, byte_offset);
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
					add_unsigned_offset(
						target_reg, target_reg, byte_offset);
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
					|| !zend_tpde_machine_value_is_register_authoritative(
						machine_kind)
					|| offset + offsetof(zval, u1.type_info)
						> UINT32_MAX) {
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
					load_off(value_reg, frame_reg,
						static_cast<uint32_t>(
							offset + offsetof(zval, u1.type_info)), 4);
					ASM(CMPwi, value_reg, IS_TRUE);
					generate_raw_set(Jump::Jeq, value_reg);
				} else if (role == ZEND_TPDE_MACHINE_PART_VALUE
						|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					load_off(value_reg, frame_reg,
						static_cast<uint32_t>(offset), 8);
				} else if (role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					load_off(value_reg, frame_reg,
						static_cast<uint32_t>(
							offset + offsetof(zval, u1.type_info)), 4);
				} else {
					return false;
				}
				value_part.set_modified();
			}
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
			return_builder.ret_local_path();
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
		if (frame_offset > UINT32_MAX - sizeof(zval)) {
			return false;
		}
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		if (frame_slot) {
			add_unsigned_offset(result_reg, canonical_frame_register(),
				frame_offset);
		} else {
			auto [address_ref, address] =
				val_ref_single(node.operands[0]);
			auto address_reg = address.load_to_reg();
			ASM(ORRx, result_reg, address_reg, address_reg);
		}
		ScratchReg type{this};
		ScratchReg referenced{this};
		auto type_reg = type.alloc_gp();
		auto referenced_reg = referenced.alloc_gp();
		load_off(type_reg, result_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		load_off(referenced_reg, result_reg, 0, 8);
		ASM(ADDxi, referenced_reg, referenced_reg,
			static_cast<uint32_t>(offsetof(zend_reference, val)));
		ASM(CMPwi, type_reg, IS_REFERENCE);
		generate_raw_select(
			Jump::Jeq, result_reg, referenced_reg, result_reg, true);
		result.set_modified();
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
		if (frame_slot && frame_offset > UINT32_MAX - sizeof(zval)) {
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
					load_off(value_reg, canonical_frame_register(),
						static_cast<uint32_t>(frame_offset),
						parts.size_bytes(part));
				} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					if (node.exact_type
							== ZEND_MIR_SCALAR_TYPE_I64
						|| node.exact_type
							== ZEND_MIR_SCALAR_TYPE_F64) {
						materialize_constant(
							zval_type(node.exact_type),
							DarwinConfig::GP_BANK, 4, value_reg);
					} else {
						load_off(value_reg, canonical_frame_register(),
							static_cast<uint32_t>(frame_offset
								+ offsetof(zval, u1.type_info)), 4);
					}
				} else {
					return false;
				}
				value.set_modified();
			}
			return true;
		}
		if (frame_offset > UINT32_MAX - sizeof(zval)) {
			return false;
		}
		auto emit = [&](AsmReg address, uint32_t offset) {
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
					load_off(value_reg, address,
						offset
						+ (role == ZEND_TPDE_MACHINE_PART_PAYLOAD
							? 0
							: static_cast<uint32_t>(
								offsetof(zval, u1.type_info))),
						parts.size_bytes(part));
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
					switch (result_kind) {
						case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
						case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
						case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
						case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
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
				|| offset > UINT32_MAX
				|| offset + offsetof(zval, u1.type_info) > UINT32_MAX) {
			return false;
		}
		if (node.kind == Adaptor::InstKind::GuardedCold) {
			auto input_value = val_ref(node.operands[0]);
			auto frame_value = val_ref(node.operands[1]);
			ScratchReg slot_argument{this};
			auto slot_argument_reg = slot_argument.alloc_gp();
			add_unsigned_offset(slot_argument_reg, canonical_frame_register(),
				static_cast<uint32_t>(offset));
			ValuePart slot_pointer{DarwinConfig::GP_BANK, 8};
			slot_pointer.set_value(this, std::move(slot_argument));
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(
				std::move(slot_pointer), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(mir.runtime_helper));
			ScratchReg store_slot{this};
			ScratchReg store_type{this};
			auto store_slot_reg = store_slot.alloc_gp();
			auto store_type_reg = store_type.alloc_gp();
			add_unsigned_offset(store_slot_reg, canonical_frame_register(),
				static_cast<uint32_t>(offset));
			load_off(store_type_reg, store_slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			auto store_slot_resolved = text_writer.label_create();
			ASM(CMPwi, store_type_reg, IS_REFERENCE_EX);
			generate_raw_jump(Jump::Jne, store_slot_resolved);
			load_off(store_slot_reg, store_slot_reg, 0, 8);
			ASM(ADDxi, store_slot_reg, store_slot_reg,
				static_cast<uint32_t>(offsetof(zend_reference, val)));
			label_place(store_slot_resolved);
			if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				materialize_constant(
					UINT64_C(0), DarwinConfig::GP_BANK, 8, store_type_reg);
				store_off(store_slot_reg, 0, store_type_reg, 8);
				materialize_constant(
					static_cast<uint64_t>(IS_NULL),
					DarwinConfig::GP_BANK, 4, store_type_reg);
			} else {
				auto input = input_value.part(0);
				auto input_reg = input.load_to_reg();
				store_off(store_slot_reg, 0, input_reg, 8);
				materialize_constant(
					zval_type(exact_type),
					DarwinConfig::GP_BANK, 4, store_type_reg);
				if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
					ASM(ADDx, store_type_reg, store_type_reg, input_reg);
				}
			}
			store_off(store_slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				store_type_reg, 4);
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
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() < 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
			return false;
		}
		auto [frame_ref, frame] =
			val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
		auto frame_reg = frame.load_to_reg();
		auto slow_release = text_writer.label_create();
		auto store_value = text_writer.label_create();
		auto slot_resolved = text_writer.label_create();
		auto finished = text_writer.label_create();
		ScratchReg old_type{this};
		ScratchReg counted{this};
		ScratchReg refcount{this};
		ScratchReg decision{this};
		auto slot_reg = counted.alloc_gp();
		auto old_type_reg = old_type.alloc_gp();
		auto refcount_reg = refcount.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		if (mir.zval_store_direct_scalar) {
			label_place(slot_resolved);
			generate_raw_jump(Jump::jmp, store_value);
		} else {
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
			 * Collectable values need gc_check_possible_root() even when
			 * shared. Keep that transition in the semantic slow path.
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
		}

		old_type.reset();
		counted.reset();
		refcount.reset();
		label_place(slow_release);
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, finished);

		label_place(store_value);
		auto store_slot_resolved = text_writer.label_create();
		ScratchReg store_slot{this};
		ScratchReg store_type{this};
		auto store_slot_reg = store_slot.alloc_gp();
		auto store_type_reg = store_type.alloc_gp();
		add_unsigned_offset(
			store_slot_reg, frame_reg, static_cast<uint32_t>(offset));
		if (!mir.zval_store_direct_scalar) {
			load_off(store_type_reg, store_slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			ASM(CMPwi, store_type_reg, IS_REFERENCE_EX);
			generate_raw_jump(Jump::Jne, store_slot_resolved);
			load_off(store_slot_reg, store_slot_reg, 0, 8);
			ASM(ADDxi, store_slot_reg, store_slot_reg,
				static_cast<uint32_t>(offsetof(zend_reference, val)));
		}
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
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(finished);
		std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
			{1, successors[1]},
		}};
		generate_switch(std::move(decision), 32, successors[0], cases);
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
		if (consumer.kind != Adaptor::InstKind::MIR
				|| consumer.operands.size() != 1
				|| consumer.operands[0] != node.result
				|| adaptor->instruction_record(next).opcode
					!= ZEND_MIR_OPCODE_COND_BRANCH
				|| this->analyzer.liveness_info(
					adaptor->val_local_idx(node.result)).ref_count != 2) {
			return false;
		}
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
			for (IRValueRef operand : node.operands) {
				auto consumed = val_ref(operand);
				(void) consumed;
			}
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
		const bool const_include_once =
			helper == ZEND_NATIVE_HELPER_DYNAMIC_INCLUDE_OR_EVAL
			&& operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& operation.result.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			&& (operation.extended_value == ZEND_INCLUDE_ONCE
				|| operation.extended_value == ZEND_REQUIRE_ONCE);
		zend::native::tpde::CCAssignerAppleA64 assigner;
		CallBuilder builder{*this, assigner};
		if (zend_tpde_helper_requires_undef_result(helper, operation)) {
			const uint64_t result_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ operation.result_storage_id) * sizeof(zval)
				+ offsetof(zval, u1.type_info);
			if (result_offset > UINT32_MAX - sizeof(uint32_t)) {
				return false;
			}
			ScratchReg undef_type{this};
			auto undef_type_reg = undef_type.alloc_gp();
			materialize_constant(
				uint64_t{IS_UNDEF}, DarwinConfig::GP_BANK, 4,
				undef_type_reg);
			store_off(canonical_frame_register(),
				static_cast<uint32_t>(result_offset), undef_type_reg, 4);
		}
		if (frame_argument != nullptr) {
			builder.add_arg(
				std::move(*frame_argument), ::tpde::CCAssignment{});
		} else {
			builder.add_arg(CallArg{node.operands.back()});
		}
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
		if (const_include_once) {
			builder.add_arg(ValuePart{operation.extended_value, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CONST_INCLUDE_ONCE));
		} else if (helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL) {
			builder.add_arg(ValuePart{source_opcode, 4,
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
		} else {
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
			builder.add_arg(ValuePart{source_opcode, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.call(runtime_symbol(helper));
		}
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
		return_builder.ret_local_path();
		label_place(continued);
		/*
		 * Some canonical W12 value operations, such as COUNT, execute only
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
					|| frame_offset > UINT32_MAX - sizeof(zval)) {
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
					load_off(value_reg, canonical_frame_register(),
						static_cast<uint32_t>(frame_offset), 8);
				} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					load_off(value_reg, canonical_frame_register(),
						static_cast<uint32_t>(frame_offset
							+ offsetof(zval, u1.type_info)), 4);
				} else {
					return false;
				}
				value.set_modified();
			}
		}
		return true;
	};
	auto execute_value_operation = [&](ValuePart *frame_argument = nullptr) {
		return execute_value_operation_with(
			frame_argument, mir.runtime_helper,
			mir.has_value_operation
				? mir.value_operation.source_opcode : UINT32_MAX);
	};
	if (node.kind == Adaptor::InstKind::GuardedCold
			&& record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
		auto materialize_cold_operand = [&](
				IRValueRef operand, zend_mir_storage_id storage) {
			if (!zend_mir_id_is_valid(storage)) {
				auto consumed = val_ref(operand);
				(void) consumed;
				return true;
			}
			const uint64_t offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
			if (offset > UINT32_MAX - sizeof(zval)) {
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
				auto part_reg = part_value.load_to_reg();
				const zend_tpde_machine_part_role role =
					parts.representation.parts[part].semantic_role;
				if (role == ZEND_TPDE_MACHINE_PART_VALUE
						|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
					payload_reg = part_reg;
					have_payload = true;
					store_off(canonical_frame_register(),
						static_cast<uint32_t>(offset), part_reg, 8);
				} else if (role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					store_off(canonical_frame_register(),
						static_cast<uint32_t>(
							offset + offsetof(zval, u1.type_info)),
						part_reg, 4);
				} else {
					return false;
				}
			}
			if (!have_payload) {
				return false;
			}
			if (kind != ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				ScratchReg type_info{this};
				auto type_info_reg = type_info.alloc_gp();
				if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
					ASM(ORRx, type_info_reg, payload_reg, payload_reg);
					ASM(ADDwi, type_info_reg, type_info_reg, IS_FALSE);
				} else {
					if (!emit_machine_zval_type_info(
							kind, payload_reg, type_info_reg)) {
						return false;
					}
				}
				store_off(canonical_frame_register(),
					static_cast<uint32_t>(
						offset + offsetof(zval, u1.type_info)),
					type_info_reg, 4);
			}
			return true;
		};
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP
				&& node.operands.size() >= 2
				&& (!materialize_cold_operand(node.operands[0],
						mir.value_operation.op1_storage_id)
					|| !materialize_cold_operand(node.operands[1],
						mir.value_operation.op2_storage_id))) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& node.assign_op_right_operand_index
					< node.operands.size()
				&& node.operands[node.assign_op_right_operand_index]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				&& !materialize_cold_operand(
					node.operands[node.assign_op_right_operand_index],
					mir.value_operation.op2_storage_id)) {
			return false;
		}
		if (record.opcode == ZEND_MIR_OPCODE_VALUE_ASSIGN_OP
				&& node.assign_op_left_operand_index
					< node.operands.size()
				&& node.operands[node.assign_op_left_operand_index]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				&& !materialize_cold_operand(
					node.operands[node.assign_op_left_operand_index],
					mir.value_operation.op1_storage_id)) {
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
					|| frame_offset > UINT32_MAX - sizeof(zval)
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
				load_off(result_reg, canonical_frame_register(),
					static_cast<uint32_t>(frame_offset), 8);
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
					load_off(value_reg, canonical_frame_register(),
						static_cast<uint32_t>(frame_offset), 8);
				} else if (role
						== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
					load_off(value_reg, canonical_frame_register(),
						static_cast<uint32_t>(
							frame_offset
								+ offsetof(zval, u1.type_info)),
						4);
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
		if (node.has_result) {
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
				const IRValueRef incoming =
					adaptor->val_as_phi(phi).incoming_val_for_block(
						IRBlockRef{node.control_block});
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
			bool move_source) {
		if (source_storage == ZEND_MIR_ID_INVALID
				|| target_storage == ZEND_MIR_ID_INVALID
				|| source_storage == target_storage
				|| (result_storage != ZEND_MIR_ID_INVALID
					&& (result_storage == source_storage
						|| result_storage == target_storage))) {
			return branch_to_guarded_cold();
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
			return branch_to_guarded_cold();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() < 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
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
		ScratchReg source_payload{this};
		ScratchReg low_word{this};
		ScratchReg probe{this};
		ScratchReg decision{this};
		auto source_type_reg = source_type.alloc_gp();
		auto target_type_reg = target_type.alloc_gp();
		auto source_payload_reg = source_payload.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto probe_reg = probe.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		const bool register_source =
			!node.operands.empty()
			&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			&& adaptor->machine_value_is_register_authoritative(
				node.operands[0])
			&& zend_tpde_machine_value_is_register_authoritative(
				adaptor->machine_kind(node.operands[0]));

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
							ASM(ORRx, source_payload_reg,
								value_reg, value_reg);
							break;
						case ZEND_TPDE_MACHINE_PART_TYPE_INFO:
							ASM(ORRw, source_type_reg,
								value_reg, value_reg);
							break;
						default:
							return false;
					}
				}
			} else {
				auto [source_ref, source] =
					val_ref_single(node.operands[0]);
				if (source_kind == ZEND_TPDE_MACHINE_VALUE_F64) {
					ASM(FMOVxd, source_payload_reg,
						source.load_to_reg());
				} else {
					const auto source_reg = source.load_to_reg();
					ASM(ORRx, source_payload_reg,
						source_reg, source_reg);
				}
				if (source_kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
					ASM(ORRx, source_type_reg,
						source_payload_reg, source_payload_reg);
					ASM(ADDwi, source_type_reg,
						source_type_reg, IS_FALSE);
				} else {
					if (!emit_machine_zval_type_info(
							source_kind, source_payload_reg,
							source_type_reg)) {
						return false;
					}
				}
			}
		} else {
			load_off(source_type_reg, frame_reg,
				static_cast<uint32_t>(
					source_offset + offsetof(zval, u1.type_info)), 4);
		}
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
		/*
		 * Immutable refcounted constants need the VM's duplication rules
		 * before they become a writable CV.  They also must never receive a
		 * native refcount write.  Keep that ownership transition on the
		 * semantic cold path.
		 */
		ASM(TSTwi, source_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		auto source_mutable = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, source_mutable);
		if (!register_source) {
			load_off(source_payload_reg, frame_reg,
				static_cast<uint32_t>(source_offset), 8);
		}
		load_off(probe_reg, source_payload_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, probe_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::Jne, slow);
		label_place(source_mutable);
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
		if (register_source) {
			ASM(ORRx, low_word_reg, source_payload_reg,
				source_payload_reg);
		} else {
			load_off(low_word_reg, frame_reg,
				static_cast<uint32_t>(source_offset), 8);
		}
		const uint32_t source_refcount_increments =
			(!move_source ? 1 : 0)
			+ (result_storage != ZEND_MIR_ID_INVALID ? 1 : 0);
		if (source_refcount_increments != 0) {
			ASM(TSTwi, source_type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			auto value_owned = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, value_owned);
			load_off(probe_reg, low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)), 4);
			ASM(ADDwi, probe_reg, probe_reg,
				source_refcount_increments);
			store_off(low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)),
				probe_reg, 4);
			label_place(value_owned);
		}
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
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		if (register_source) {
			/*
			 * The helper consumes canonical Zend slots.  Keep the source
			 * register-authoritative on the hot edge and materialize it only
			 * when the generated guard actually selects the cold edge.
			 */
			store_off(frame_reg, static_cast<uint32_t>(source_offset),
				source_payload_reg, 8);
			store_off(frame_reg,
				static_cast<uint32_t>(
					source_offset + offsetof(zval, u1.type_info)),
				source_type_reg, 4);
		}
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		source_type.reset();
		target_type.reset();
		source_payload.reset();
		low_word.reset();
		probe.reset();
		std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
			{1, successors[1]},
		}};
		generate_switch(std::move(decision), 32, successors[0], cases);
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
		if (source_offset > UINT32_MAX - sizeof(zval)
				|| result_offset > UINT32_MAX - sizeof(zval)) {
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
			return branch_to_guarded_cold();
		}
		const uint64_t source_offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + source_storage) * sizeof(zval);
		if (source_offset > UINT32_MAX - sizeof(zval)) {
			return branch_to_guarded_cold();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() < 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
			return false;
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
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto probe_reg = probe.alloc_gp();
		auto decision_reg = decision.alloc_gp();

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
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		auto done = text_writer.label_create();
		generate_raw_jump(Jump::jmp, done);
		label_place(slow);
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		type.reset();
		value.reset();
		probe.reset();
		std::array<std::pair<uint64_t, IRBlockRef>, 1> cases{{
			{1, successors[1]},
		}};
		generate_switch(std::move(decision), 32, successors[0], cases);
		return true;
	};
	auto read_array = [&]() {
		zend_tpde_array_read layout;
		const zend_tpde_machine_reference *element_reference =
			operation_machine_reference(
				ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT);

		if (!zend_tpde_array_read_at(mir, &layout)
				|| element_reference == nullptr
				|| !zend_mir_id_is_valid(
					element_reference->base_value_id)
				|| !zend_mir_id_is_valid(
					element_reference->index_value_id)
				|| element_reference->scale != sizeof(zval)
				|| element_reference->access_width != sizeof(zval)
				|| layout.container_offset > UINT32_MAX - 8
				|| layout.key_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
					!= node.argument_index) {
				return false;
			}
			if (!node.operands.empty()
					&& ((adaptor->exact_type(node.operands[0])
								== ZEND_MIR_SCALAR_TYPE_I64
							&& adaptor->representation(node.operands[0])
								== ZEND_MIR_REPRESENTATION_I64
							&& adaptor->machine_kind(node.operands[0])
								== ZEND_TPDE_MACHINE_VALUE_I64)
						|| adaptor->machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
						|| (adaptor->representation(node.operands[0])
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& adaptor->machine_kind(node.operands[0])
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
					&& ((adaptor->representation(node.result)
								== ZEND_MIR_REPRESENTATION_I64
							&& adaptor->exact_type(node.result)
								== ZEND_MIR_SCALAR_TYPE_I64
							&& adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_I64)
						|| (adaptor->representation(node.result)
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))) {
				auto slow = text_writer.label_create();
				auto mixed_loop = text_writer.label_create();
				auto mixed_next = text_writer.label_create();
				auto found = text_writer.label_create();
				auto done = text_writer.label_create();
				auto [frame_ref, frame] =
					val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
				auto frame_scratch = std::move(frame).into_scratch();
				auto frame_reg = frame_scratch.cur_reg();
				ScratchReg key_value{this};
				ScratchReg type{this};
				ScratchReg array{this};
				ScratchReg limit{this};
				ScratchReg element{this};
				ScratchReg slot{this};
				ScratchReg decision{this};
				auto key_reg = key_value.alloc_gp();
				auto type_reg = type.alloc_gp();
				auto array_reg = array.alloc_gp();
				auto limit_reg = limit.alloc_gp();
				auto element_reg = element.alloc_gp();
				auto decision_reg = decision.alloc_gp();
				const bool register_string_key =
					adaptor->machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR;

				if (adaptor->machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto key = val_ref(node.operands[0]);
					const ValueParts parts = val_parts(node.operands[0]);
					bool have_payload = false;
					bool have_type_info = false;
					for (uint32_t part = 0; part < parts.count(); ++part) {
						auto value = key.part(part);
						auto value_reg = value.load_to_reg();
						switch (parts.representation.parts[part]
								.semantic_role) {
							case ZEND_TPDE_MACHINE_PART_PAYLOAD:
								ASM(ORRx, key_reg, value_reg, value_reg);
								have_payload = true;
								break;
							case ZEND_TPDE_MACHINE_PART_TYPE_INFO:
								ASM(ORRw, type_reg, value_reg, value_reg);
								have_type_info = true;
								break;
							default:
								return false;
						}
					}
					if (!have_payload || !have_type_info) {
						return false;
					}
					ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_reg, IS_LONG);
					generate_raw_jump(Jump::Jne, slow);
				} else {
					auto [key_ref, key] =
						val_ref_single(node.operands[0]);
					auto source_key_reg = key.load_to_reg();
					ASM(ORRx, key_reg, source_key_reg, source_key_reg);
				}

				const bool register_receiver =
					node.operands.size() > 1
					&& node.machine_reference_operand_index != 1
					&& adaptor->machine_kind(node.operands[1])
						== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR;
				if (register_receiver) {
					auto [receiver_ref, receiver] =
						val_ref_single(node.operands[1]);
					auto receiver_reg = receiver.load_to_reg();
					ASM(ORRx, array_reg, receiver_reg, receiver_reg);
				} else if (layout.container_literal) {
					if (node.machine_reference_operand_index
							>= node.operands.size()) {
						return false;
					}
					auto [literal_ref, literal] = val_ref_single(
						node.operands[
							node.machine_reference_operand_index]);
					auto literal_reg = literal.load_to_reg();
					load_off(type_reg, literal_reg,
						static_cast<uint32_t>(
							offsetof(zval, u1.type_info)), 4);
					ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::Jne, slow);
					load_off(array_reg, literal_reg, 0, 8);
				} else {
					load_off(type_reg, frame_reg,
						layout.container_offset
							+ static_cast<uint32_t>(
								offsetof(zval, u1.type_info)),
						4);
					ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::Jne, slow);
					load_off(array_reg, frame_reg,
						layout.container_offset, 8);
				}
				load_off(type_reg, array_reg,
					static_cast<uint32_t>(offsetof(HashTable, u)), 4);
				ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
				if (register_string_key) {
					generate_raw_jump(Jump::Jne, slow);
					auto content_loop = text_writer.label_create();
					auto slot_reg = slot.alloc_gp();
					ScratchReg probe{this};
					auto probe_reg = probe.alloc_gp();
					load_off(element_reg, array_reg,
						static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
					load_off(type_reg, key_reg,
						static_cast<uint32_t>(offsetof(zend_string, h)), 8);
					load_off(limit_reg, array_reg,
						static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
					ASM(ORRw, limit_reg, type_reg, limit_reg);
					ASM(ADDx_sxtw, slot_reg, element_reg, limit_reg, 2);
					load_off(limit_reg, slot_reg, 0, 4);
					label_place(mixed_loop);
					ASM(CMPwi, limit_reg, HT_INVALID_IDX);
					generate_raw_jump(Jump::Jeq, slow);
					ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
					load_off(type_reg, key_reg,
						static_cast<uint32_t>(offsetof(zend_string, h)), 8);
					load_off(limit_reg, slot_reg,
						static_cast<uint32_t>(offsetof(Bucket, h)), 8);
					ASM(CMPx, limit_reg, type_reg);
					generate_raw_jump(Jump::Jne, mixed_next);
					load_off(limit_reg, slot_reg,
						static_cast<uint32_t>(offsetof(Bucket, key)), 8);
					ASM(CMPx, limit_reg, key_reg);
					generate_raw_jump(Jump::Jeq, found);
					load_off(decision_reg, limit_reg,
						static_cast<uint32_t>(offsetof(zend_string, len)), 8);
					load_off(array_reg, key_reg,
						static_cast<uint32_t>(offsetof(zend_string, len)), 8);
					ASM(CMPx, decision_reg, array_reg);
					generate_raw_jump(Jump::Jne, mixed_next);
					ASM(CMPxi, decision_reg, 0);
					generate_raw_jump(Jump::Jeq, found);
					ASM(ADDxi, limit_reg, limit_reg,
						static_cast<uint32_t>(offsetof(zend_string, val)));
					ASM(ADDxi, array_reg, key_reg,
						static_cast<uint32_t>(offsetof(zend_string, val)));
					label_place(content_loop);
					load_off(type_reg, limit_reg, 0, 1);
					load_off(probe_reg, array_reg, 0, 1);
					ASM(CMPw, type_reg, probe_reg);
					generate_raw_jump(Jump::Jne, mixed_next);
					ASM(ADDxi, limit_reg, limit_reg, 1);
					ASM(ADDxi, array_reg, array_reg, 1);
					ASM(SUBSxi, decision_reg, decision_reg, 1);
					generate_raw_jump(Jump::Jne, content_loop);
					generate_raw_jump(Jump::jmp, found);
					label_place(mixed_next);
					load_off(limit_reg, slot_reg,
						static_cast<uint32_t>(
							offsetof(Bucket, val) + offsetof(zval, u2.next)),
						4);
					generate_raw_jump(Jump::jmp, mixed_loop);
					label_place(found);
					ASM(ORRx, element_reg, slot_reg, slot_reg);
				} else {
					generate_raw_jump(Jump::Jeq, slow);
					load_off(limit_reg, array_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, nNumUsed)),
						4);
					ASM(CMPx, key_reg, limit_reg);
					generate_raw_jump(Jump::Jhs, slow);
					load_off(element_reg, array_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, arPacked)),
						8);
					ASM(ADDx_lsl, element_reg, element_reg, key_reg, 4);
				}
				load_off(type_reg, element_reg,
					static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
					4);
				ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
				ASM(CMPwi, type_reg, IS_LONG);
				generate_raw_jump(Jump::Jne, slow);
				if (adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto result = result_ref(node.result);
					auto payload = result.part(0);
					auto type_info = result.part(1);
					auto payload_reg = payload.alloc_reg();
					auto type_info_reg = type_info.alloc_reg();
					load_off(payload_reg, element_reg, 0, 8);
					materialize_constant(
						static_cast<uint64_t>(IS_LONG),
						DarwinConfig::GP_BANK, 4, type_info_reg);
					payload.set_modified();
					type_info.set_modified();
				} else {
					auto [result_ref, result] =
						result_ref_single(node.result);
					auto result_reg = result.alloc_reg();
					load_off(result_reg, element_reg, 0, 8);
					result.set_modified();
				}
				materialize_constant(
					uint64_t{0}, DarwinConfig::GP_BANK, 4,
					decision_reg);
				generate_raw_jump(Jump::jmp, done);
				label_place(slow);
				materialize_constant(
					uint64_t{1}, DarwinConfig::GP_BANK, 4,
					decision_reg);
				label_place(done);
				type.reset();
				array.reset();
				limit.reset();
				element.reset();
				slot.reset();
				generate_guarded_decision_branch(
					std::move(decision), guarded_successors[1],
					guarded_successors[0]);
				return true;
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
		ScratchReg decision{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto key_reg = key.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();
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
			1, DarwinConfig::GP_BANK, 4, high_word_reg);
		generate_raw_jump(Jump::jmp, key_ready);
		label_place(key_long);
		load_off(key_reg, frame_reg, layout.key_offset, 8);
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, high_word_reg);
		label_place(key_ready);

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			load_off(limit_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, limit_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}

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

		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				load_off(payload_reg, element_reg, 0, 8);
				load_off(type_info_reg, element_reg,
					static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
					4);
				payload.set_modified();
				type_info.set_modified();
				ASM(ORRx, low_word_reg, payload_reg, payload_reg);
		} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				switch (adaptor->exact_type(node.result)) {
					case ZEND_MIR_SCALAR_TYPE_I1:
						ASM(CMPwi, type_reg, IS_TRUE);
						generate_raw_set(Jump::Jeq, result_reg);
						break;
					case ZEND_MIR_SCALAR_TYPE_I64:
						load_off(result_reg, element_reg, 0, 8);
						break;
					case ZEND_MIR_SCALAR_TYPE_F64:
						load_off(low_word_reg, element_reg, 0, 8);
						ASM(FMOVdx, result_reg, low_word_reg);
						break;
					default:
						switch (adaptor->machine_kind(node.result)) {
							case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
							case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
							case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
							case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
							case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
								load_off(result_reg, element_reg, 0, 8);
								break;
							default:
								return false;
						}
				}
				result.set_modified();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			load_off(low_word_reg, element_reg, 0, 8);
			load_off(high_word_reg, element_reg, 8, 8);
			store_off(frame_reg, layout.result_offset, low_word_reg, 8);
			store_off(frame_reg, layout.result_offset + 8, high_word_reg, 8);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			generate_raw_jump(Jump::jmp, done);
		} else {
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jeq, done);
			load_off(limit_reg, low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)),
				4);
			ASM(ADDwi, limit_reg, limit_reg, 1);
			store_off(low_word_reg,
				static_cast<uint32_t>(
					offsetof(zend_refcounted_h, refcount)),
				limit_reg, 4);
		}
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
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
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
	auto isset_array = [&]() {
		zend_tpde_array_isset layout;

		if (!zend_tpde_array_isset_at(mir, &layout, true)
				|| layout.container_offset > UINT32_MAX - 8
				|| layout.key_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
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
		auto mixed_string_content_loop = text_writer.label_create();
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
		generate_raw_jump(
			Jump::Jeq, layout.is_empty ? answer_true : answer_false);
		ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
		load_off(key_kind_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, key_kind_reg, type_reg);
		generate_raw_jump(Jump::Jne, mixed_string_next);
		load_off(key_kind_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPx, key_kind_reg, key_reg);
		generate_raw_jump(Jump::Jeq, found);
		ASM(CMPxi, key_kind_reg, 0);
		generate_raw_jump(Jump::Jeq, mixed_string_next);
		load_off(decision_reg, key_kind_reg,
			static_cast<uint32_t>(offsetof(zend_string, len)), 8);
		load_off(array_reg, key_reg,
			static_cast<uint32_t>(offsetof(zend_string, len)), 8);
		ASM(CMPx, decision_reg, array_reg);
		generate_raw_jump(Jump::Jne, mixed_string_next);
		ASM(CMPxi, decision_reg, 0);
		generate_raw_jump(Jump::Jeq, found);
		ASM(ADDxi, key_kind_reg, key_kind_reg,
			static_cast<uint32_t>(offsetof(zend_string, val)));
		ASM(ADDxi, array_reg, key_reg,
			static_cast<uint32_t>(offsetof(zend_string, val)));
		label_place(mixed_string_content_loop);
		load_off(type_reg, key_kind_reg, 0, 1);
		load_off(limit_reg, array_reg, 0, 1);
		ASM(CMPw, type_reg, limit_reg);
		generate_raw_jump(Jump::Jne, mixed_string_next);
		ASM(ADDxi, key_kind_reg, key_kind_reg, 1);
		ASM(ADDxi, array_reg, array_reg, 1);
		ASM(SUBSxi, decision_reg, decision_reg, 1);
		generate_raw_jump(Jump::Jne, mixed_string_content_loop);
		generate_raw_jump(Jump::jmp, found);
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
		ASM(ADDxi, element_reg, element_reg,
			static_cast<uint32_t>(offsetof(zend_reference, val)));
		load_off(type_reg, element_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		label_place(not_reference);
		if (!layout.is_empty) {
			ASM(CMPwi, type_reg, IS_NULL);
			generate_raw_jump(Jump::Jhi, answer_true);
		} else {
			/*
			 * Mirror Zend's scalar truthiness rules for values whose boolean
			 * conversion is side-effect free. Objects and uncommon runtime
			 * kinds retain the canonical helper path.
			 */
			ASM(CMPwi, type_reg, IS_NULL);
			generate_raw_jump(Jump::Jle, answer_true);
			ASM(CMPwi, type_reg, IS_FALSE);
			generate_raw_jump(Jump::Jeq, answer_true);
			ASM(CMPwi, type_reg, IS_TRUE);
			generate_raw_jump(Jump::Jeq, answer_false);

			ASM(CMPwi, type_reg, IS_LONG);
			auto empty_not_long = text_writer.label_create();
			generate_raw_jump(Jump::Jne, empty_not_long);
			load_off(limit_reg, element_reg, 0, 8);
			generate_raw_jump(
				Jump{Jump::Cbz, limit_reg, false}, answer_true);
			generate_raw_jump(Jump::jmp, answer_false);

			label_place(empty_not_long);
			ASM(CMPwi, type_reg, IS_STRING);
			auto empty_not_string = text_writer.label_create();
			generate_raw_jump(Jump::Jne, empty_not_string);
			load_off(limit_reg, element_reg, 0, 8);
			load_off(type_reg, limit_reg,
				static_cast<uint32_t>(offsetof(zend_string, len)), 8);
			generate_raw_jump(
				Jump{Jump::Cbz, type_reg, false}, answer_true);
			ASM(CMPxi, type_reg, 1);
			generate_raw_jump(Jump::Jne, answer_false);
			load_off(type_reg, limit_reg,
				static_cast<uint32_t>(offsetof(zend_string, val)), 1);
			ASM(CMPwi, type_reg, '0');
			generate_raw_jump(Jump::Jeq, answer_true);
			generate_raw_jump(Jump::jmp, answer_false);

			label_place(empty_not_string);
			ASM(CMPwi, type_reg, IS_ARRAY);
			auto empty_not_array = text_writer.label_create();
			generate_raw_jump(Jump::Jne, empty_not_array);
			load_off(limit_reg, element_reg, 0, 8);
			load_off(type_reg, limit_reg,
				static_cast<uint32_t>(
					offsetof(HashTable, nNumOfElements)), 4);
			generate_raw_jump(
				Jump{Jump::Cbz, type_reg, false}, answer_true);
			generate_raw_jump(Jump::jmp, answer_false);

			label_place(empty_not_array);
			ASM(CMPwi, type_reg, IS_RESOURCE);
			generate_raw_jump(Jump::Jne, slow);
			load_off(limit_reg, element_reg, 0, 8);
			load_off(type_reg, limit_reg,
				static_cast<uint32_t>(offsetof(zend_resource, handle)), 4);
			generate_raw_jump(
				Jump{Jump::Cbz, type_reg, false}, answer_true);
			generate_raw_jump(Jump::jmp, answer_false);
		}

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
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
		return true;
	};
	auto append_packed_array = [&]() {
		zend_tpde_packed_array_append layout;
		const zend_tpde_machine_reference *element_reference =
			operation_machine_reference(
				ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT);

		if (!zend_tpde_packed_array_append_at(mir, &layout)
				|| element_reference == nullptr
				|| !zend_mir_id_is_valid(
					element_reference->base_value_id)
				|| zend_mir_id_is_valid(
					element_reference->index_value_id)
				|| element_reference->scale != sizeof(zval)
				|| element_reference->access_width != sizeof(zval)
				|| layout.container_offset > UINT32_MAX - 8
				|| layout.value_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return execute_value_operation();
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() < 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
			return false;
		}
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
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto array_reg = array.alloc_gp();
		auto count_reg = count.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto element_reg = element.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();
		auto decision_reg = decision.alloc_gp();

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

		if (!scalar_value) {
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
		}
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
		if (scalar_value) {
			auto [value_ref, value] = val_ref_single(node.operands[
				node.packed_append_value_operand_index]);
			auto value_reg = value.load_to_reg();
			mov(low_word_reg, value_reg, 8);
			materialize_constant(static_cast<uint64_t>(IS_LONG),
				DarwinConfig::GP_BANK, 8, high_word_reg);
		} else {
			load_off(low_word_reg, frame_reg, layout.value_offset, 8);
			load_off(high_word_reg, frame_reg, layout.value_offset + 8, 8);
		}
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
		} else if (!scalar_value) {
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
			if (!scalar_value) {
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
		}
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
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

		if (!zend_tpde_string_length_at(mir, &layout)) {
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
				mov(result_reg, operand_reg, 4);
				if (boolean.negate) {
					ASM(EORwi, result_reg, result_reg, 1);
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
				auto frame_scratch = std::move(frame).into_scratch();
				ScratchReg result{this};
				ScratchReg type{this};
				auto result_reg = result.alloc_gp();
				auto type_reg = type.alloc_gp();
				load_off(result_reg, frame_scratch.cur_reg(),
					boolean.operand_offset, 4);
				if (boolean.negate) {
					ASM(EORwi, result_reg, result_reg, 1);
				}
				store_off(frame_scratch.cur_reg(), boolean.result_offset,
					result_reg, 8);
				ASM(ADDwi, type_reg, result_reg, IS_FALSE);
				store_off(frame_scratch.cur_reg(),
					boolean.result_offset
						+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
					type_reg, 4);
				return true;
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
		if (layout.operand_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8
				|| (!frame_only && !register_string)) {
			return false;
		}
		if (normal_register_string) {
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_scratch = std::move(frame).into_scratch();
			auto string = val_ref(node.operands[0]);
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			uint64_t known_length = 0;
			if (adaptor->known_string_literal(
					node.operands[0], &known_length, nullptr)) {
				materialize_constant(known_length,
					DarwinConfig::GP_BANK, 8, result_reg);
			} else {
				auto string_reg = string.part(0).load_to_reg();
				load_off(result_reg, string_reg,
					static_cast<uint32_t>(offsetof(zend_string, len)), 8);
			}
			/*
			 * Source call arguments may remain canonical zvals even when the
			 * producer is register-authoritative.  Publish strlen's complete
			 * result before such a call can observe the temporary; later machine
			 * consumers still use result_reg directly.
			 */
			store_off(frame_scratch.cur_reg(), layout.result_offset,
				result_reg, 8);
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			materialize_constant(
				IS_LONG, DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_scratch.cur_reg(),
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
			result.set_modified();
			return true;
		}
		if (normal_frame_string) {
			auto [frame_ref, frame] =
				val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
			auto frame_scratch = std::move(frame).into_scratch();
			ScratchReg string{this};
			auto string_reg = string.alloc_gp();
			load_off(string_reg, frame_scratch.cur_reg(),
				layout.operand_offset, 8);
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			load_off(result_reg, string_reg,
				static_cast<uint32_t>(offsetof(zend_string, len)), 8);
			store_off(frame_scratch.cur_reg(), layout.result_offset,
				result_reg, 8);
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			materialize_constant(
				IS_LONG, DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_scratch.cur_reg(),
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
			result.set_modified();
			return true;
		}
		if (node.kind != Adaptor::InstKind::GuardedFast
				|| node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX) {
			return false;
		}
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() < 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
			return false;
		}
		if (register_string) {
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_scratch = std::move(frame).into_scratch();
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			uint64_t known_length = 0;
			if (adaptor->known_string_literal(
					node.operands[0], &known_length, nullptr)) {
				materialize_constant(known_length,
					DarwinConfig::GP_BANK, 8, result_reg);
			} else {
				auto [string_ref, string] =
					val_ref_single(node.operands[0]);
				auto string_reg = string.load_to_reg();
				load_off(result_reg, string_reg,
					static_cast<uint32_t>(offsetof(zend_string, len)), 8);
			}
			store_off(frame_scratch.cur_reg(), layout.result_offset,
				result_reg, 8);
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			materialize_constant(
				IS_LONG, DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_scratch.cur_reg(),
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
			result.set_modified();
			generate_uncond_branch(successors[0]);
			return true;
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

		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		load_off(result_reg, string_reg,
			static_cast<uint32_t>(offsetof(zend_string, len)), 8);
		result.set_modified();
		store_off(frame_reg, layout.result_offset, result_reg, 8);
		store_constant(frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			IS_LONG, 4);
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, ready);

		label_place(slow);
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(ready);
		type.reset();
		string.reset();
		generate_guarded_decision_branch(
			std::move(decision), successors[1], successors[0]);
		return true;
	};
	auto string_identity = [&]() {
		zend_tpde_string_identity layout;

		if (!zend_tpde_string_identity_at(mir, &layout)
				|| layout.left_offset > UINT32_MAX - 8
				|| layout.right_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
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

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			load_off(type_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}
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
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
		return true;
	};
	auto long_binary = [&]() {
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
						== ZEND_MIR_SOURCE_SLOT_VAR)) {
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
		if (!register_layout
				|| (!framed_layout && !register_result_layout)
				|| (framed_layout
					&& (layout.left.offset > UINT32_MAX - 8
						|| layout.right.offset > UINT32_MAX - 8
						|| layout.result_offset > UINT32_MAX - 8))
				|| !node.has_result
				|| node.operands.size() != 3
				|| node.operands[2]
					!= IRValueRef{Adaptor::FRAME_VALUE}) {
			return branch_to_guarded_cold();
		}
		if (register_layout) {
			layout.source_opcode =
				mir.value_operation.source_opcode;
		}
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
		const auto guarded_successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (node.control_block == UINT32_MAX
				|| node.continuation_block == UINT32_MAX
				|| guarded_successors.size() < 2
				|| static_cast<uint32_t>(guarded_successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(guarded_successors[1])
					!= node.argument_index) {
			return false;
		}
		auto slow = text_writer.label_create();
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
		if (!register_layout) {
			load_off(result_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, result_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}

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
					ASM(CMPwi, type_info_reg, IS_LONG);
					generate_raw_jump(Jump::Jne, slow);
					ASM(ORRx, target, payload_reg, payload_reg);
					return true;
				}
				auto [value_ref, value] = val_ref_single(operand);
				auto value_reg = value.load_to_reg();
				ASM(ORRx, target, value_reg, value_reg);
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
					ASM(ADDSx, result_reg, result_reg, right_reg);
					generate_raw_jump(Jump::Jvs, slow);
					break;
				case ZEND_SUB:
					ASM(SUBSx, result_reg, result_reg, right_reg);
					generate_raw_jump(Jump::Jvs, slow);
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
				case ZEND_SPACESHIP: {
					ScratchReg less{this};
					auto less_reg = less.alloc_gp();
					ASM(CMPx, result_reg, right_reg);
					generate_raw_set(Jump::Jlt, less_reg);
					generate_raw_set(Jump::Jgt, result_reg);
					ASM(SUBx, result_reg, result_reg, less_reg);
					break;
				}
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
		if (framed_layout || register_result_layout) {
			store_off(frame_reg, layout.result_offset, result_reg, 8);
			ScratchReg result_type{this};
			auto result_type_reg = result_type.alloc_gp();
			if (boolean_result) {
				ASM(ORRx, result_type_reg, result_reg, result_reg);
				ASM(ADDwi, result_type_reg, result_type_reg, IS_FALSE);
			} else {
				materialize_constant(
					static_cast<uint64_t>(IS_LONG),
					DarwinConfig::GP_BANK, 4, result_type_reg);
			}
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				result_type_reg, 4);
		}
		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto fast_result = result_ref(node.result);
			auto payload = fast_result.part(0);
			auto type_info = fast_result.part(1);
			auto payload_reg = payload.alloc_reg();
			auto type_info_reg = type_info.alloc_reg();
			ASM(ORRx, payload_reg, result_reg, result_reg);
			if (boolean_result) {
				ASM(ADDwi, type_info_reg, result_reg, IS_FALSE);
			} else {
				materialize_constant(
					static_cast<uint64_t>(IS_LONG),
					DarwinConfig::GP_BANK, 4, type_info_reg);
			}
			payload.set_modified();
			type_info.set_modified();
		} else {
			auto [fast_result_ref, fast_result] =
				result_ref_single(node.result);
			auto fast_result_reg = fast_result.alloc_reg();
			ASM(ORRx, fast_result_reg, result_reg, result_reg);
			fast_result.set_modified();
		}
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		result_value.reset();
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, done);
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
				|| layout.left_offset > UINT32_MAX - 8
				|| layout.right.offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return branch_to_guarded_cold();
		}
		if (layout.has_result != node.has_result
				&& !(node.mutation_result
					&& !layout.has_result && node.has_result)) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
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
				auto left_value_reg = left_value.load_to_reg();
				ASM(ORRx, left_reg, left_value_reg, left_value_reg);
			} else if (node.assign_op_left_operand_index < node.operands.size()
					&& adaptor->machine_kind(
						node.operands[node.assign_op_left_operand_index])
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto left_value = val_ref(
					node.operands[node.assign_op_left_operand_index]);
			auto payload = left_value.part(0);
			auto type_info = left_value.part(1);
			auto payload_reg = payload.load_to_reg();
			auto type_info_reg = type_info.load_to_reg();
			ASM(ORRx, left_reg, payload_reg, payload_reg);
			ASM(ORRw, type_reg, type_info_reg, type_info_reg);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
		} else {
			load_off(type_reg, frame_reg,
				layout.left_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
			load_off(left_reg, frame_reg, layout.left_offset, 8);
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
				auto right_value_reg = right_value.load_to_reg();
				ASM(ORRx, right_reg, right_value_reg, right_value_reg);
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
			ASM(ORRx, right_reg, payload_reg, payload_reg);
			ASM(ORRw, type_reg, type_info_reg, type_info_reg);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
		} else if (layout.right.literal) {
			if (node.machine_reference_operand_index
						>= node.operands.size()) {
				return branch_to_guarded_cold();
			}
			auto [literal_ref, literal] = val_ref_single(
				node.operands[node.machine_reference_operand_index]);
			auto literal_reg = literal.load_to_reg();
			load_off(type_reg, literal_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
			load_off(right_reg, literal_reg, 0, 8);
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
		if (node.mutation_result) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(ORRx, result_reg, left_reg, left_reg);
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
						ASM(ORRx, value_reg, left_reg, left_reg);
					} else if (role
							== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
						materialize_constant(
							static_cast<uint64_t>(IS_LONG),
							DarwinConfig::GP_BANK, 4, value_reg);
					} else {
						return false;
					}
					value.set_modified();
				}
			} else {
				return false;
			}
		}
		if (!(node.kind == Adaptor::InstKind::GuardedFast
					&& node.mutation_result
					&& mir.mutation_lazy_scalar)) {
			store_off(frame_reg, layout.left_offset, left_reg, 8);
			materialize_constant(
				static_cast<uint64_t>(IS_LONG),
				DarwinConfig::GP_BANK, 4, type_reg);
			store_off(frame_reg,
				layout.left_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
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
		type.reset();
		left.reset();
		right.reset();
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
		return true;
	};
	auto long_incdec = [&]() {
		zend_tpde_long_incdec layout;

		if (!zend_tpde_long_incdec_at(mir, &layout)
				|| layout.operand_offset > UINT32_MAX - 8
				|| layout.result_offset > UINT32_MAX - 8) {
			return branch_to_guarded_cold();
		}
		if (layout.has_result != node.has_result
				&& !(node.mutation_result
					&& !layout.has_result && node.has_result)) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
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
		ScratchReg value{this};
		ScratchReg limit{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto value_reg = value.alloc_gp();
		auto limit_reg = limit.alloc_gp();
		auto decision_reg = decision.alloc_gp();

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
			auto operand_reg = operand.load_to_reg();
			ASM(ORRx, value_reg, operand_reg, operand_reg);
		} else if (!node.operands.empty()
				&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				&& adaptor->machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto operand = val_ref(node.operands[0]);
			auto payload = operand.part(0);
			auto type_info = operand.part(1);
			auto payload_reg = payload.load_to_reg();
			auto type_info_reg = type_info.load_to_reg();
			ASM(ORRx, value_reg, payload_reg, payload_reg);
			ASM(ORRw, type_reg, type_info_reg, type_info_reg);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
		} else {
			load_off(type_reg, frame_reg,
				layout.operand_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
			load_off(value_reg, frame_reg, layout.operand_offset, 8);
		}
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
		if (node.mutation_result) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(ORRx, result_reg, value_reg, value_reg);
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
						ASM(ORRx, result_reg, value_reg, value_reg);
					} else if (role
							== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
						materialize_constant(
							static_cast<uint64_t>(IS_LONG),
							DarwinConfig::GP_BANK, 4, result_reg);
					} else {
						return false;
					}
					result_part.set_modified();
				}
			} else {
				return false;
			}
		}
		if (!(node.kind == Adaptor::InstKind::GuardedFast
					&& node.mutation_result
					&& mir.mutation_lazy_scalar)) {
			store_off(frame_reg, layout.operand_offset, value_reg, 8);
		}
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
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
		return true;
	};
	auto slot_isset_empty = [&]() {
		zend_tpde_slot_isset_empty layout;

		if (!zend_tpde_slot_isset_empty_at(mir, &layout)
				|| layout.operand_offset > UINT32_C(4095)
				|| layout.result_offset > UINT32_C(4095)) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result
				&& node.kind != Adaptor::InstKind::GuardedFast) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
			}
		}
		if (!node.has_result
				&& node.kind == Adaptor::InstKind::GuardedFast
				&& (node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL
					|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I1
					|| node.exact_type == ZEND_MIR_SCALAR_TYPE_I64)) {
			auto [frame_ref, frame] =
				val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
			auto frame_scratch = std::move(frame).into_scratch();
			auto frame_reg = frame_scratch.cur_reg();
			ScratchReg result{this};
			ScratchReg type{this};
			auto result_reg = result.alloc_gp();
			auto type_reg = type.alloc_gp();
			if (!layout.is_empty) {
				materialize_constant(
					node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL ? 0 : 1,
					DarwinConfig::GP_BANK, 4, result_reg);
			} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
				materialize_constant(
					uint64_t{1}, DarwinConfig::GP_BANK, 4, result_reg);
			} else {
				load_off(type_reg, frame_reg, layout.operand_offset,
					node.exact_type == ZEND_MIR_SCALAR_TYPE_I1 ? 4 : 8);
				if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
					ASM(CMPwi, type_reg, 0);
				} else {
					ASM(CMPxi, type_reg, 0);
				}
				generate_raw_set(Jump::Jeq, result_reg);
			}
			store_off(frame_reg, layout.result_offset, result_reg, 8);
			ASM(ADDwi, type_reg, result_reg, IS_FALSE);
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
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
					materialize_constant(
						type == ZEND_MIR_SCALAR_TYPE_NULL ? 0 : 1,
						DarwinConfig::GP_BANK, 4, result_reg);
				} else if (type == ZEND_MIR_SCALAR_TYPE_NULL) {
					materialize_constant(
						uint64_t{1}, DarwinConfig::GP_BANK, 4, result_reg);
				} else if (has_scalar_operand) {
					auto [operand_ref, value] = val_ref_single(operand);
					auto value_reg = value.load_to_reg();
					if (type == ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(CMPwi, value_reg, 0);
					} else {
						ASM(CMPxi, value_reg, 0);
					}
					generate_raw_set(
						invert_result ? Jump::Jne : Jump::Jeq, result_reg);
				} else {
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = std::move(frame).into_scratch();
					ScratchReg value{this};
					auto value_reg = value.alloc_gp();
					load_off(value_reg, frame_scratch.cur_reg(),
						layout.operand_offset,
						type == ZEND_MIR_SCALAR_TYPE_I1 ? 4 : 8);
					if (type == ZEND_MIR_SCALAR_TYPE_I1) {
						ASM(CMPwi, value_reg, 0);
					} else {
						ASM(CMPxi, value_reg, 0);
					}
					generate_raw_set(
						invert_result ? Jump::Jne : Jump::Jeq, result_reg);
				}
				result.set_modified();
				if (node.kind == Adaptor::InstKind::GuardedFast) {
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = std::move(frame).into_scratch();
					ScratchReg result_type{this};
					auto type_reg = result_type.alloc_gp();
					store_off(frame_scratch.cur_reg(), layout.result_offset,
						result_reg, 8);
					ASM(ADDwi, type_reg, result_reg, IS_FALSE);
					store_off(frame_scratch.cur_reg(),
						layout.result_offset
							+ static_cast<uint32_t>(
								offsetof(zval, u1.type_info)),
						type_reg, 4);
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
		auto frame_scratch = std::move(frame).into_scratch();
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
			} else {
				load_off(value_reg, frame_reg, layout.operand_offset,
					node.exact_type == ZEND_MIR_SCALAR_TYPE_I1 ? 4 : 8);
				generate_raw_jump(
					Jump{Jump::Cbnz, value_reg, false}, truthy);
				generate_raw_jump(Jump::jmp, falsey);
			}
		} else {
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
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			load_off(value_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(
						offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, value_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			if (node.has_result) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(ORRx, result_reg, type_reg, type_reg);
				result.set_modified();
			} else {
				store_off(frame_reg, layout.result_offset, type_reg, 8);
				ASM(ADDwi, value_reg, type_reg, IS_FALSE);
				store_off(frame_reg,
					layout.result_offset
						+ static_cast<uint32_t>(
							offsetof(zval, u1.type_info)),
					value_reg, 4);
			}
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
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
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

		if (!zend_tpde_object_property_read_at(mir, &layout)
				|| property_reference == nullptr
				|| property_reference->stable_storage_or_layout_id
					!= layout.cache_offset
				|| property_reference->access_width != sizeof(zval)) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result) {
			return branch_to_guarded_cold();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
			}
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
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		ScratchReg decision{this};
		AsmReg decision_reg;

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
		load_off(type_reg, cache_reg,
			layout.cache_offset + sizeof(void *), 8);
		ASM(CMPxi, type_reg, ZEND_FIRST_PROPERTY_OFFSET);
		generate_raw_jump(Jump::Jlt, slow);
		ASM(ADDx, property_reg, object_reg, type_reg);
		load_off(type_reg, property_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, offset_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			load_off(offset_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, offset_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			object.reset();
			cache.reset();
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				/*
				 * Keep the complete two-part boxed value authoritative on the
				 * guarded edge. Non-long properties take the semantic cold path;
				 * a long needs neither ownership transfer nor a refcount update.
				 */
				ASM(CMPwi, offset_reg, IS_LONG);
				generate_raw_jump(Jump::Jne, slow);
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				load_off(payload_reg, property_reg, 0, 8);
				materialize_constant(static_cast<uint64_t>(IS_LONG),
					DarwinConfig::GP_BANK, 4, type_info_reg);
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				switch (adaptor->exact_type(node.result)) {
					case ZEND_MIR_SCALAR_TYPE_I1:
						ASM(CMPwi, offset_reg, IS_TRUE);
						generate_raw_set(Jump::Jeq, result_reg);
						break;
					case ZEND_MIR_SCALAR_TYPE_I64:
						load_off(result_reg, property_reg, 0, 8);
						break;
					case ZEND_MIR_SCALAR_TYPE_F64:
						load_off(offset_reg, property_reg, 0, 8);
						ASM(FMOVdx, result_reg, offset_reg);
						break;
					default:
						switch (adaptor->machine_kind(node.result)) {
							case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
							case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
							case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
							case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
							case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
								load_off(result_reg, property_reg, 0, 8);
								break;
							default:
								return false;
						}
				}
				result.set_modified();
			}
			type.reset();
			offset.reset();
			property.reset();
			decision_reg = decision.alloc_gp();
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				generate_raw_jump(Jump::jmp, done);
			}
		} else {
			load_off(offset_reg, property_reg, 0, 8);
			store_off(frame_reg, layout.result_offset, offset_reg, 8);
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				type_reg, 4);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			generate_raw_jump(Jump::jmp, done);
		}
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, copied);
		load_off(offset_reg, property_reg, 0, 8);
		load_off(property_reg, offset_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, property_reg, property_reg, 1);
		store_off(offset_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)),
			property_reg, 4);
		label_place(copied);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			materialize_constant(
				uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		}
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
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
				|| property_reference->access_width != sizeof(zval)) {
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
		const auto successors =
			adaptor->block_succs(IRBlockRef{node.control_block});
		if (successors.size() < 2
				|| static_cast<uint32_t>(successors[0])
					!= node.continuation_block
				|| static_cast<uint32_t>(successors[1])
					!= node.argument_index) {
			return false;
		}
		auto slow = text_writer.label_create();
		auto property_metadata_valid = text_writer.label_create();
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
		ScratchReg decision{this};
		auto object_reg = object.alloc_gp();
		auto cache_reg = cache.alloc_gp();
		auto offset_reg = offset.alloc_gp();
		auto property_reg = property.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto decision_reg = decision.alloc_gp();

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
			Jump{Jump::Cbz, type_reg, false}, property_metadata_valid);
		load_off(cache_reg, type_reg,
			static_cast<uint32_t>(offsetof(zend_property_info, flags)), 4);
		ASM(TSTwi, cache_reg, ZEND_ACC_READONLY);
		generate_raw_jump(Jump::Jne, slow);
		ASM(TSTwi, cache_reg, ZEND_ACC_PPP_SET_MASK);
		generate_raw_jump(Jump::Jne, slow);
		load_off(cache_reg, type_reg,
			static_cast<uint32_t>(offsetof(zend_property_info, hooks)), 8);
		generate_raw_jump(
			Jump{Jump::Cbnz, cache_reg, false}, slow);
		load_off(cache_reg, type_reg,
			static_cast<uint32_t>(
				offsetof(zend_property_info, type)
					+ offsetof(zend_type, type_mask)), 4);
		ASM(TSTwi, cache_reg, 1u << IS_LONG);
		generate_raw_jump(Jump::Jeq, slow);
		if (!scalar_value) {
			load_off(cache_reg, frame_reg,
				layout.value_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, cache_reg, cache_reg, Z_TYPE_MASK);
			ASM(CMPwi, cache_reg, IS_LONG);
			generate_raw_jump(Jump::Jne, slow);
		}
		label_place(property_metadata_valid);
		ASM(ADDx, property_reg, object_reg, offset_reg);
		load_off(type_reg, property_reg,
			static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, offset_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);

		if (scalar_value) {
			materialize_constant(static_cast<uint64_t>(IS_LONG),
				DarwinConfig::GP_BANK, 4, type_reg);
		} else {
			load_off(type_reg, frame_reg,
				layout.value_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, offset_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, offset_reg, IS_REFERENCE);
			generate_raw_jump(Jump::Jeq, slow);
		}

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

		if (scalar_value) {
			auto [value_ref, value] = val_ref_single(node.operands[
				node.property_write_value_operand_index]);
			auto value_reg = value.load_to_reg();
			ASM(ORRx, low_word_reg, value_reg, value_reg);
		} else {
			load_off(low_word_reg, frame_reg, layout.value_offset, 8);
		}
		if (!scalar_value && !layout.move_value) {
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
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
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
	auto dynamic_fetch_read = [&]() {
		zend_tpde_dynamic_fetch_read layout;
		if (!zend_tpde_dynamic_fetch_read_at(mir, &layout)) {
			return branch_to_guarded_cold();
		}
		if (!node.has_result) {
			return branch_to_guarded_cold();
		}
		const bool register_name = node.operands.size() == 2
			&& adaptor->machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			&& node.operands[1] == IRValueRef{Adaptor::FRAME_VALUE};
		const bool direct_cv = layout.cv_index != UINT32_MAX;
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto guarded_successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			if (node.control_block == UINT32_MAX
					|| node.continuation_block == UINT32_MAX
					|| guarded_successors.size() < 2
					|| static_cast<uint32_t>(guarded_successors[0])
						!= node.continuation_block
					|| static_cast<uint32_t>(guarded_successors[1])
						!= node.argument_index) {
				return false;
			}
			if (!direct_cv) {
				return branch_to_guarded_cold();
			}
		}
		if (layout.direct_long && direct_cv
				&& node.kind == Adaptor::InstKind::GuardedFast) {
			const uint64_t value_offset =
				(uint64_t{ZEND_CALL_FRAME_SLOT} + layout.cv_index)
					* sizeof(zval);
			if (value_offset > UINT32_MAX - sizeof(zval)
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
			load_off(result_reg, frame_reg,
				static_cast<uint32_t>(value_offset), 8);
			result.set_modified();
			generate_branch_to_block(Jump::jmp,
				IRBlockRef{node.continuation_block}, false, true);
			return true;
		}
		auto slow = text_writer.label_create();
		auto symbol_table = text_writer.label_create();
		auto cv_loop = text_writer.label_create();
		auto loop = text_writer.label_create();
		auto next = text_writer.label_create();
		auto cv_value_ready = text_writer.label_create();
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
		ScratchReg decision{this};
		auto slot_reg = slot.alloc_gp();
		auto type_reg = type.alloc_gp();
		auto table_reg = table.alloc_gp();
		auto name_reg = name.alloc_gp();
		auto index_reg = index.alloc_gp();
		auto bucket_reg = bucket.alloc_gp();
		auto low_word_reg = low_word.alloc_gp();
		auto high_word_reg = high_word.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		if (direct_cv) {
			/* The frozen literal-name proof selects the canonical CV below. */
		} else if (register_name) {
			auto [name_ref, name_value] =
				val_ref_single(node.operands[0]);
			auto source_name_reg = name_value.load_to_reg();
			ASM(ORRx, name_reg, source_name_reg, source_name_reg);
		} else {
			load_off(type_reg, frame_reg,
				layout.name_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_STRING);
			generate_raw_jump(Jump::Jne, slow);
			load_off(name_reg, frame_reg, layout.name_offset, 8);
		}

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			load_off(type_reg, frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				4);
			ASM(TSTwi, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jne, slow);
		}
		if (direct_cv) {
			materialize_constant(layout.cv_index,
				DarwinConfig::GP_BANK, 4, index_reg);
			generate_raw_jump(Jump::jmp, cv_value_ready);
		}

		/*
		 * Compiled-variable entries in an attached local symbol table are
		 * indirect references to their canonical frame slots. Resolve a name
		 * that is itself one of the op_array's interned CV names directly; a
		 * non-CV dynamic name retains the complete symbol-table lookup below.
		 */
		load_off(bucket_reg, frame_reg,
			static_cast<uint32_t>(offsetof(zend_execute_data, func)), 8);
		load_off(index_reg, bucket_reg,
			static_cast<uint32_t>(offsetof(zend_op_array, last_var)), 4);
		ASM(CMPwi, index_reg, 0);
		generate_raw_jump(Jump::Jeq, symbol_table);
		load_off(bucket_reg, bucket_reg,
			static_cast<uint32_t>(offsetof(zend_op_array, vars)), 8);
		label_place(cv_loop);
		ASM(SUBwi, index_reg, index_reg, 1);
		ASM(ADDx_lsl, slot_reg, bucket_reg, index_reg, 3);
		load_off(high_word_reg, slot_reg, 0, 8);
		ASM(CMPx, high_word_reg, name_reg);
		generate_raw_jump(Jump::Jeq, cv_value_ready);
		ASM(CMPwi, index_reg, 0);
		generate_raw_jump(Jump::Jne, cv_loop);

		label_place(symbol_table);
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

		label_place(cv_value_ready);
		ASM(ADDx_lsl, slot_reg, frame_reg, index_reg, 4);
		ASM(ADDxi, slot_reg, slot_reg,
			static_cast<uint32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval)));
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

		if (node.kind == Adaptor::InstKind::GuardedFast) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				load_off(payload_reg, slot_reg, 0, 8);
				ASM(ORRx, type_info_reg, type_reg, type_reg);
				payload.set_modified();
				type_info.set_modified();
				ASM(ORRx, low_word_reg, payload_reg, payload_reg);
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				switch (adaptor->exact_type(node.result)) {
					case ZEND_MIR_SCALAR_TYPE_I1:
						ASM(CMPwi, type_reg, IS_TRUE);
						generate_raw_set(Jump::Jeq, result_reg);
						break;
					case ZEND_MIR_SCALAR_TYPE_I64:
						load_off(result_reg, slot_reg, 0, 8);
						break;
					case ZEND_MIR_SCALAR_TYPE_F64:
						load_off(low_word_reg, slot_reg, 0, 8);
						ASM(FMOVdx, result_reg, low_word_reg);
						break;
					default:
						switch (adaptor->machine_kind(node.result)) {
							case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
							case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
							case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
							case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
							case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
								load_off(result_reg, slot_reg, 0, 8);
								break;
							default:
								return false;
						}
				}
				result.set_modified();
			}
			materialize_constant(
				uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
		} else {
			load_off(low_word_reg, slot_reg, 0, 8);
			load_off(high_word_reg, slot_reg, 8, 8);
			store_off(frame_reg, layout.result_offset, low_word_reg, 8);
			store_off(frame_reg, layout.result_offset + 8, high_word_reg, 8);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			generate_raw_jump(Jump::jmp, done);
		}
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
		materialize_constant(
			uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
		label_place(done);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			const auto successors =
				adaptor->block_succs(IRBlockRef{node.control_block});
			generate_guarded_decision_branch(
				std::move(decision), successors[1], successors[0]);
		}
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
			/*
			 * The frozen source operand is an allocator-visible SSA use even
			 * when Zend's canonical-frame helper performs the semantic check.
			 * Consume it here before either eliding the proven typed-body check
			 * or emitting the ordinary helper call.
			 */
			for (IRValueRef operand : node.operands) {
				if (operand != IRValueRef{Adaptor::FRAME_VALUE}
						&& operand != IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT}) {
					auto verified = val_ref(operand);
				}
			}
			if (adaptor->typed_body()
					|| mir.runtime_helper == ZEND_NATIVE_HELPER_COUNT) {
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
			auto frame_reg = frame.load_to_reg();
			ScratchReg result_value{this};
			auto result_reg = result_value.alloc_gp();
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
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				ASM(ORRx, payload_reg, result_reg, result_reg);
				materialize_constant(
					IS_LONG, DarwinConfig::GP_BANK, 4, type_info_reg);
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto scalar_reg = result.alloc_reg();
				ASM(ORRx, scalar_reg, result_reg, result_reg);
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
				load_off(pending_reg, context_scratch.cur_reg(),
					static_cast<uint32_t>(offsetof(
						zend_native_execution_context, vm_interrupt)), 8);
				generate_raw_jump(
					Jump{Jump::Cbz, pending_reg, false}, done);
				load_off(pending_reg, pending_reg, 0, 1);
				ASM(CMPxi, pending_reg, 0);
				generate_raw_jump(Jump::Jne, slow);
				generate_raw_jump(Jump::jmp, done);
				label_place(slow);
				context_scratch.reset();
				pending.reset();
				if (!emit_materializations(instruction, true)) {
					return false;
				}
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[0]});
				builder.add_arg(ValuePart{mir.source_opline_index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(ZEND_NATIVE_HELPER_INTERRUPT_POLL));
				label_place(done);
				release_spilled_regs(spilled);
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
			if (can_fuse_compare_branch()) {
				return integer_compare(Jump::Jeq);
			}
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_eq_u64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_LT:
			if (can_fuse_compare_branch()) {
				return integer_compare(Jump::Jlt);
			}
			return encode_binary([&](auto &&left, auto &&right, auto &&result) {
				return EncodeBase::encode_zend_native_lt_i64(
					std::move(left), std::move(right), std::move(result));
			});
		case ZEND_MIR_OPCODE_I64_LE:
			if (can_fuse_compare_branch()) {
				return integer_compare(Jump::Jle);
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
			auto result_reg = result.alloc_reg();
			ScratchReg less{this};
			auto less_reg = less.alloc_gp();
			ASM(CMPx, left.load_to_reg(), right.load_to_reg());
			generate_raw_set(Jump::Jlt, less_reg);
			generate_raw_set(Jump::Jgt, result_reg);
			ASM(SUBx, result_reg, result_reg, less_reg);
			result.set_modified();
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
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ScratchReg less{this};
			auto less_reg = less.alloc_gp();
			ASM(FCMP_d, left.load_to_reg(), right.load_to_reg());
			generate_raw_set(Jump::Jlt, less_reg);
			generate_raw_set(Jump::Jgt, result_reg);
			ASM(SUBx, result_reg, result_reg, less_reg);
			result.set_modified();
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
				IRBlockRef{node.control_block})[0]);
			return true;
		case ZEND_MIR_OPCODE_COND_BRANCH: {
			auto [condition_ref, condition] = unary();
			auto condition_reg = condition.load_to_reg();
			const auto &successors = adaptor->block_succs(
				IRBlockRef{node.control_block});
			generate_cond_branch(Jump{Jump::Cbnz, condition_reg, false},
				successors[0], successors[1]);
			return true;
		}
		case ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH: {
			zend_tpde_multi_branch layout;
			if (!zend_tpde_multi_branch_at(
					adaptor->plan(), mir, record, &layout)
					|| node.operands.size() != 1) {
				return false;
			}
			const zend_tpde_plan *plan = adaptor->plan();
			std::vector<IRBlockRef> targets;
			std::vector<::tpde::Label> case_labels;
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
			auto frame_scratch = std::move(frame).into_scratch();
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
			const bool check_undefined_cv =
				layout.source_opcode == ZEND_MATCH
				&& (mir.value_operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT
					|| mir.value_operation.op1.kind
						== ZEND_MIR_SOURCE_OPERAND_SSA)
				&& mir.value_operation.op1.slot_kind
					== ZEND_MIR_SOURCE_SLOT_CV;
			if (check_undefined_cv) {
				ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
				frame_argument.set_value(this, std::move(frame_scratch));
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
			add_unsigned_offset(
				slot_reg,
				check_undefined_cv
					? canonical_frame_register() : frame_scratch.cur_reg(),
				layout.operand_offset);
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

			label_place(long_label);
			load_off(value_reg, slot_reg, 0, 8);
			emit_integer_dispatch(
				layout.cases, layout.case_count, case_labels,
				value_reg, constant_reg, default_label);

			label_place(string_label);
			load_off(value_reg, slot_reg, 0, 8);
			for (uint32_t case_index = 0;
					case_index < layout.case_count; ++case_index) {
				const zend_tpde_multi_branch_case &branch_case =
					layout.cases[case_index];
				if (branch_case.string_key != nullptr) {
					auto next_case = text_writer.label_create();
					const uint64_t length = branch_case.string_length;
					load_off(probe_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(zend_string, len)), 8);
					materialize_constant(
						&length, DarwinConfig::GP_BANK, 8,
						constant_reg);
					ASM(CMPx, probe_reg, constant_reg);
					generate_raw_jump(Jump::Jne, next_case);
					size_t offset = 0;
					while (offset < branch_case.string_length) {
						const uint32_t width =
							branch_case.string_length - offset >= 8 ? 8
							: branch_case.string_length - offset >= 4 ? 4
							: branch_case.string_length - offset >= 2 ? 2 : 1;
						uint64_t expected = 0;
						memcpy(&expected, branch_case.string_key + offset,
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
			if ((node.operands.size() != 1 && !register_boxed_condition)
					|| !mir.has_value_operation) {
				return false;
			}
			if (record.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				zend_tpde_array_iterator_reset reset_layout;

				if (zend_tpde_array_iterator_reset_at(mir, &reset_layout)) {
					const int32_t decision_slot =
						allocate_stack_slot(sizeof(uint32_t));
					if (decision_slot < 0) {
						return false;
					}
					auto slow = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = std::move(frame).into_scratch();
					auto spilled = spill_before_branch();
					release_spilled_regs(spilled);
					auto frame_reg = frame_scratch.cur_reg();
					ScratchReg array{this};
					ScratchReg type_info{this};
					ScratchReg high_word{this};
					ScratchReg refcount{this};
					auto array_reg = array.alloc_gp();
					auto type_info_reg = type_info.alloc_gp();
					auto high_word_reg = high_word.alloc_gp();
					auto refcount_reg = refcount.alloc_gp();

					load_off(type_info_reg, frame_reg,
						reset_layout.source_offset
							+ static_cast<uint32_t>(offsetof(
								zval, u1.type_info)),
						4);
					ASM(ANDwi, type_info_reg, type_info_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_info_reg, IS_ARRAY);
					generate_raw_jump(Jump::Jne, slow);
					load_off(array_reg, frame_reg,
						reset_layout.source_offset, 8);
					load_off(type_info_reg, array_reg,
						static_cast<uint32_t>(offsetof(
							zend_refcounted_h, u.type_info)), 4);
					ASM(TSTwi, type_info_reg, GC_IMMUTABLE);
					generate_raw_jump(Jump::Jne, slow);
					load_off(refcount_reg, array_reg,
						static_cast<uint32_t>(offsetof(
							zend_refcounted_h, refcount)), 4);
					ASM(ADDwi, refcount_reg, refcount_reg, 1);
					store_off(array_reg,
						static_cast<uint32_t>(offsetof(
							zend_refcounted_h, refcount)),
						refcount_reg, 4);
					load_off(high_word_reg, frame_reg,
						reset_layout.source_offset + 8, 8);
					store_off(frame_reg, reset_layout.holder_offset,
						array_reg, 8);
					store_off(frame_reg, reset_layout.holder_offset + 8,
						high_word_reg, 8);
					store_constant(frame_reg,
						reset_layout.holder_offset
							+ static_cast<uint32_t>(offsetof(zval, u2)),
						0, 4);
					store_constant(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot),
						ZEND_NATIVE_ITERATOR_NEXT, 4);
					generate_raw_jump(Jump::jmp, branch);
					array.reset();
					type_info.reset();
					high_word.reset();
					refcount.reset();
					label_place(slow);

					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, std::move(frame_scratch));
					builder.add_arg(std::move(frame_argument),
						::tpde::CCAssignment{});
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
					ASM(CMPxi, decision_reg,
						ZEND_NATIVE_ITERATOR_EXCEPTION);
					auto valid = text_writer.label_create();
					generate_raw_jump(Jump::Jlt, valid);
					decision.reset(this);
					if (zend_mir_id_is_valid(mir.exception_block_id)) {
						generate_exception_branch(
							adaptor->block_ref(mir.exception_block_id));
					} else {
						RetBuilder return_builder{*this, *cur_cc_assigner()};
						return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
							DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(valid);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot),
						decision_reg, 4);
					decision.reset(this);
					label_place(branch);
					const auto &successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					ScratchReg branch_decision{this};
					auto branch_decision_reg = branch_decision.alloc_gp();
					load_off(branch_decision_reg, AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot), 4);
					generate_cond_branch(
						Jump{Jump::Cbnz, branch_decision_reg, false},
						successors[0], successors[1]);
					return true;
				}

				zend_tpde_packed_iterator_fetch layout;

				if (zend_tpde_packed_iterator_fetch_at(mir, &layout)) {
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
						allocate_stack_slot(sizeof(uint32_t));
					if (decision_slot < 0) {
						return false;
					}
					auto slow = text_writer.label_create();
					auto destination_valid = text_writer.label_create();
					auto end = text_writer.label_create();
					auto branch = text_writer.label_create();
					auto [frame_ref, frame] =
						val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
					auto frame_scratch = std::move(frame).into_scratch();
					auto frame_reg = frame_scratch.cur_reg();
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

					load_off(type_reg, frame_reg,
						layout.holder_offset
							+ static_cast<uint32_t>(
								offsetof(zval, u1.type_info)),
						4);
					ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::Jne, slow);
					load_off(array_reg, frame_reg, layout.holder_offset, 8);
					load_off(type_reg, array_reg,
						static_cast<uint32_t>(offsetof(HashTable, u)), 4);
					ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
					generate_raw_jump(Jump::Jeq, slow);
					load_off(limit_reg, array_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, nNumUsed)),
						4);
					load_off(type_reg, array_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, nNumOfElements)),
						4);
					ASM(CMPw, limit_reg, type_reg);
					generate_raw_jump(Jump::Jne, slow);
					load_off(position_reg, frame_reg,
						layout.holder_offset
							+ static_cast<uint32_t>(offsetof(zval, u2.fe_pos)),
						4);
					ASM(CMPw, position_reg, limit_reg);
					generate_raw_jump(Jump::Jhs, end);

					if (!layout.destination_scalar_only) {
						load_off(type_reg, frame_reg,
							layout.destination_offset
								+ static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							4);
						ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
						ASM(CMPwi, type_reg, IS_UNDEF);
						generate_raw_jump(Jump::Jeq, destination_valid);
						ASM(CMPwi, type_reg, IS_LONG);
						generate_raw_jump(Jump::Jne, slow);
						label_place(destination_valid);
					}

					load_off(element_reg, array_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, arPacked)),
						8);
					ASM(ADDx_lsl, element_reg, element_reg, position_reg, 4);
					load_off(type_reg, element_reg,
						static_cast<uint32_t>(
							offsetof(zval, u1.type_info)),
						4);
					ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
					ASM(CMPwi, type_reg, IS_LONG);
					generate_raw_jump(Jump::Jne, slow);
					load_off(value_reg, element_reg, 0, 8);

					/* All guards precede the first observable mutation. */
					ASM(ADDwi, position_reg, position_reg, 1);
					store_off(frame_reg,
						layout.holder_offset
							+ static_cast<uint32_t>(offsetof(zval, u2.fe_pos)),
						position_reg, 4);
					store_off(frame_reg, layout.destination_offset,
						value_reg, 8);
					materialize_constant(static_cast<uint64_t>(IS_LONG),
						DarwinConfig::GP_BANK, 4, type_reg);
					store_off(frame_reg,
						layout.destination_offset
							+ static_cast<uint32_t>(
								offsetof(zval, u1.type_info)),
						type_reg, 4);
					materialize_constant(uint64_t{1}, DarwinConfig::GP_BANK,
						4, type_reg);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot), type_reg, 4);
					generate_raw_jump(Jump::jmp, branch);

					label_place(end);
					materialize_constant(uint64_t{0}, DarwinConfig::GP_BANK,
						4, type_reg);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot), type_reg, 4);
					generate_raw_jump(Jump::jmp, branch);

					label_place(slow);
					type.reset();
					array.reset();
					position.reset();
					limit.reset();
					element.reset();
					value.reset();
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{DarwinConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, std::move(frame_scratch));
					builder.add_arg(std::move(frame_argument),
						::tpde::CCAssignment{});
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
					ASM(CMPxi, decision_reg,
						ZEND_NATIVE_ITERATOR_EXCEPTION);
					auto valid = text_writer.label_create();
					generate_raw_jump(Jump::Jlt, valid);
					decision.reset(this);
					if (zend_mir_id_is_valid(mir.exception_block_id)) {
						generate_exception_branch(
							adaptor->block_ref(mir.exception_block_id));
					} else {
						RetBuilder return_builder{*this, *cur_cc_assigner()};
						return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
							DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(valid);
					store_off(AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot),
						decision_reg, 4);
					decision.reset(this);

					label_place(branch);
					if (node.has_result) {
						if (adaptor->machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_I64) {
							auto [result_ref, result] =
								result_ref_single(node.result);
							auto result_reg = result.alloc_reg();
							load_off(result_reg, canonical_frame_register(),
								layout.destination_offset, 8);
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
									load_off(value_reg, canonical_frame_register(),
										layout.destination_offset, 8);
								} else if (role
										== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
									load_off(value_reg, canonical_frame_register(),
										layout.destination_offset
											+ static_cast<uint32_t>(offsetof(
												zval, u1.type_info)),
										4);
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
					load_off(branch_decision_reg, AsmReg{AsmReg::FP},
						static_cast<uint32_t>(decision_slot), 4);
					generate_cond_branch(
						Jump{Jump::Cbnz, branch_decision_reg, false},
						successors[0], successors[1]);
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
					generate_cond_branch(
						Jump{Jump::Cbnz, condition_reg, false},
						successors[0], successors[1]);
					return true;
				}
				zend_tpde_value_condition layout;
				bool have_condition_layout =
					zend_tpde_value_condition_at(mir, &layout);
				if (!have_condition_layout && register_boxed_condition) {
					const zend_mir_executable_value_ref &operation =
						mir.value_operation;
					const bool has_result =
						operation.source_opcode == ZEND_JMPZ_EX
						|| operation.source_opcode == ZEND_JMPNZ_EX;
					const bool supported_opcode =
						operation.source_opcode == ZEND_JMPZ
						|| operation.source_opcode == ZEND_JMPNZ
						|| operation.source_opcode == ZEND_JMPZ_EX
						|| operation.source_opcode == ZEND_JMPNZ_EX;
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
							&& operand_offset <= UINT32_MAX
							&& result_offset <= UINT32_MAX) {
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
							|| operand_offset > UINT32_MAX - sizeof(zval)) {
						return false;
					}
					auto boxed = val_ref(node.operands[1]);
					const ValueParts parts = val_parts(node.operands[1]);
					if (parts.count() != 2) {
						return false;
					}
					auto payload = boxed.part(0);
					auto type_info = boxed.part(1);
					store_off(canonical_frame_register(),
						static_cast<uint32_t>(operand_offset),
						payload.load_to_reg(), 8);
					store_off(canonical_frame_register(),
						static_cast<uint32_t>(operand_offset)
							+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
						type_info.load_to_reg(), 4);
				}

				if (have_condition_layout) {
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
					std::optional<ValueRef> boxed_condition;
					std::optional<ValuePartRef> boxed_payload;
					std::optional<ValuePartRef> boxed_type_info;
					AsmReg boxed_payload_reg{};
					AsmReg boxed_type_info_reg{};
					if (register_boxed_condition) {
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
						if (register_boxed_condition) {
							ASM(ORRx, value_reg,
								boxed_payload_reg, boxed_payload_reg);
						} else {
							load_off(value_reg, frame_reg,
								layout.operand_offset, 8);
						}
					};

					/*
					 * Result-producing short-circuit branches must consume their
					 * source TMP.  Leave those to the native helper below instead
					 * of overwriting the source slot in the truthiness fast path.
					 */
					if (layout.has_result) {
						generate_raw_jump(Jump::jmp, slow);
					}

					if (register_boxed_condition) {
						ASM(ORRx, type_reg,
							boxed_type_info_reg, boxed_type_info_reg);
					} else {
						load_off(type_reg, frame_reg,
							layout.operand_offset
								+ static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							4);
					}
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
					load_condition_payload();
					generate_raw_jump(
						Jump{Jump::Cbnz, value_reg, false}, truthy);
					generate_raw_jump(Jump::jmp, falsey);

					label_place(not_long);
					ASM(CMPwi, type_reg, IS_STRING);
					auto not_string = text_writer.label_create();
					generate_raw_jump(Jump::Jne, not_string);
					load_condition_payload();
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
					load_condition_payload();
					load_off(type_reg, value_reg,
						static_cast<uint32_t>(
							offsetof(HashTable, nNumOfElements)), 4);
					generate_raw_jump(
						Jump{Jump::Cbnz, type_reg, false}, truthy);
					generate_raw_jump(Jump::jmp, falsey);
					label_place(not_array);
					ASM(CMPwi, type_reg, IS_RESOURCE);
					generate_raw_jump(Jump::Jne, slow);
					load_condition_payload();
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
						IRBlockRef{node.control_block});
					generate_raw_jump(Jump::jmp, branch);
					label_place(slow);
					if (register_boxed_condition) {
						store_off(frame_reg, layout.operand_offset,
							boxed_payload_reg, 8);
						store_off(frame_reg,
							layout.operand_offset
								+ static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							boxed_type_info_reg, 4);
						boxed_type_info.reset();
						boxed_payload.reset();
						boxed_condition.reset();
					}

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
					if (zend_mir_id_is_valid(mir.exception_block_id)) {
						generate_exception_branch(
							adaptor->block_ref(mir.exception_block_id));
					} else {
						RetBuilder return_builder{
							*this, *cur_cc_assigner()};
						return_builder.add(ValuePart{
							ZEND_NATIVE_EXCEPTION, 4,
							DarwinConfig::GP_BANK},
							::tpde::CCAssignment{});
						return_builder.ret();
					}
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
			const int32_t decision_slot = node.has_result
				? allocate_stack_slot(sizeof(uint32_t)) : -1;
			if (node.has_result && decision_slot < 0) {
				return false;
			}
			if (node.has_result) {
				store_off(AsmReg{AsmReg::FP},
					static_cast<uint32_t>(decision_slot),
					decision_reg, 4);
			}
			ASM(CMPxi, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
			auto valid = text_writer.label_create();
			generate_raw_jump(Jump::Jlt, valid);
			/* Release the helper return register before constructing an early
			 * native return.  On the valid edge the generated return sequence is
			 * skipped, so the physical decision register still carries 0 or 1. */
			decision.reset(this);
			if (zend_mir_id_is_valid(mir.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(mir.exception_block_id));
			} else {
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(valid);
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
				load_off(result_reg, canonical_frame_register(),
					static_cast<uint32_t>(frame_offset), 8);
				result.set_modified();
				ScratchReg branch_decision{this};
				auto branch_decision_reg =
					branch_decision.alloc_gp();
				load_off(branch_decision_reg, AsmReg{AsmReg::FP},
					static_cast<uint32_t>(decision_slot), 4);
				generate_cond_branch(
					Jump{Jump::Cbnz, branch_decision_reg, false},
					successors[0], successors[1]);
			} else {
				generate_cond_branch(
					Jump{Jump::Cbnz, decision_reg, false},
					successors[0], successors[1]);
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
					zend::native::tpde::CCAssignerAppleA64 body_assigner;
					CallBuilder body_builder{*this, body_assigner};
					for (uint32_t argument = 0;
							argument < argument_count; ++argument) {
						body_builder.add_arg(
							CallArg{node.operands[argument]});
					}
					body_builder.call(
						this->func_syms[typed_body_function]);
					const auto return_type =
						adaptor->typed_body_return_type(
							call.component_target_index);
					if (!return_type.valid) {
						return false;
					}
					const auto return_representation =
						zend_tpde_machine_representation(
							return_type.machine_kind, true);
					std::vector<ValuePart> body_results;
					body_results.reserve(return_representation.part_count);
					for (uint32_t part = 0;
							part < return_representation.part_count; ++part) {
						const auto &part_desc =
							return_representation.parts[part];
						body_results.emplace_back(
							part_desc.register_bank
									== ZEND_TPDE_MACHINE_REGISTER_FP
								? DarwinConfig::FP_BANK
								: DarwinConfig::GP_BANK,
							part_desc.bit_width / 8);
						body_builder.add_ret(
							body_results.back(), ::tpde::CCAssignment{});
					}
					if (node.has_result) {
						const ValueParts destination_parts =
							val_parts(node.result);
						if (destination_parts.count()
								!= body_results.size()) {
							return false;
						}
						auto destination = result_ref(node.result);
						for (uint32_t part = 0;
								part < body_results.size(); ++part) {
							destination.part(part).set_value(
								std::move(body_results[part]));
						}
					} else {
						for (auto &body_result : body_results) {
							body_result.reset(this);
						}
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
					const uint32_t inline_operand_count =
						node.inlined_user_body
							? (node.inlined_checked_source_opcode
									== UINT32_MAX ? 1 : 2)
							: 0;
					if (node.operands.size()
							< context_operand + inline_operand_count) {
						return false;
					}
					const uint32_t context_use_count =
						static_cast<uint32_t>(node.operands.size())
						- context_operand - inline_operand_count;
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
				auto load_generated_result = [&](AsmReg result_frame_reg) {
					if (node.has_result) {
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
							const ValueParts parts = val_parts(node.result);
							for (uint32_t part = 0;
									part < parts.count(); ++part) {
								auto value = result.part(part);
								auto value_reg = value.alloc_reg();
								const zend_tpde_machine_part_role role =
									parts.representation.parts[part]
										.semantic_role;
								if (role != ZEND_TPDE_MACHINE_PART_PAYLOAD
										&& role
											!= ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
									return;
								}
								load_off(value_reg, result_slot_reg,
									role == ZEND_TPDE_MACHINE_PART_PAYLOAD
										? 0
										: static_cast<uint32_t>(
											offsetof(zval, u1.type_info)),
									parts.size_bytes(part));
								value.set_modified();
							}
						} else {
							auto [result_ref, result] =
								result_ref_single(node.result);
							auto result_reg = result.alloc_reg();
							if (adaptor->exact_type(node.result)
									== ZEND_MIR_SCALAR_TYPE_I1) {
								load_off(result_reg, result_slot_reg,
									static_cast<uint32_t>(offsetof(
										zval, u1.type_info)), 4);
								ASM(ANDwi, result_reg, result_reg,
									Z_TYPE_MASK);
								ASM(CMPwi, result_reg, IS_TRUE);
								generate_raw_set(Jump::Jeq, result_reg);
							} else {
								load_off(result_reg, result_slot_reg, 0, 8);
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
							load_off(result_frame_reg,
								AsmReg{AsmReg::FP},
								static_cast<uint32_t>(
									leaf_caller_frame_slot),
								sizeof(void *));
						} else {
							auto [result_frame_ref,
								result_frame_value] =
									val_ref_single(node.operands[
										frame_operand + 6
											+ (variadic_frame ? 2 : 0)]);
							mov(result_frame_reg,
								result_frame_value.load_to_reg(),
								sizeof(void *));
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
					const uint32_t inline_operand_count =
						node.inlined_checked_source_opcode == UINT32_MAX
							? 1 : 2;
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
						auto context_scratch =
							std::move(context).into_scratch();
						ScratchReg observed{this};
						auto observed_reg = observed.alloc_gp();
						load_off(observed_reg, context_scratch.cur_reg(),
							static_cast<uint32_t>(offsetof(
								zend_native_execution_context,
								observers_enabled)), 1);
						ASM(CMPxi, observed_reg, 0);
						generate_raw_jump(
							Jump::Jne, call_slow_target());
					}
					if (node.inlined_checked_source_opcode != UINT32_MAX) {
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
							switch (
								node.inlined_checked_source_opcode) {
								case ZEND_ADD:
									ASM(ADDSx, computed_reg,
										left_reg, right_reg);
									break;
								case ZEND_SUB:
									ASM(SUBSx, computed_reg,
										left_reg, right_reg);
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
								Jump::Jvs,
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
								mov(result_reg, source_reg, 8);
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
							|| node.operands.size() != argument_count) {
						return false;
					}
					zend::native::tpde::CCAssignerAppleA64 body_assigner;
					CallBuilder body_builder{*this, body_assigner};
					for (uint32_t argument = 0;
							argument < argument_count; ++argument) {
						body_builder.add_arg(
							CallArg{node.operands[argument]});
					}
					body_builder.call(
						this->func_syms[typed_body_function]);
					const auto return_type =
						adaptor->typed_body_return_type(
							call.component_target_index);
					const auto return_representation =
						zend_tpde_machine_representation(
							return_type.machine_kind, true);
					std::vector<ValuePart> body_results;
					body_results.reserve(return_representation.part_count);
					for (uint32_t part = 0;
							part < return_representation.part_count; ++part) {
						const auto &part_desc =
							return_representation.parts[part];
						body_results.emplace_back(
							part_desc.register_bank
									== ZEND_TPDE_MACHINE_REGISTER_FP
								? DarwinConfig::FP_BANK
								: DarwinConfig::GP_BANK,
							part_desc.bit_width / 8);
						body_builder.add_ret(
							body_results.back(), ::tpde::CCAssignment{});
					}
					if (node.has_result) {
						auto destination = result_ref(node.result);
						for (uint32_t part = 0;
								part < body_results.size(); ++part) {
							destination.part(part).set_value(
								std::move(body_results[part]));
						}
					} else {
						for (auto &body_result : body_results) {
							body_result.reset(this);
						}
					}
					adaptor->mark_typed_body_call(
						call.direct_call->frame_size);
					generate_uncond_branch(
						IRBlockRef{node.continuation_block});
					return true;
				}
				if (generated_fast_path
						&& node.kind != Adaptor::InstKind::GuardedCold) {
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
							/*
							 * The synthetic TypedCallGuard which dominates this
							 * block has already rejected observer-enabled
							 * execution. Do not reload the same context byte in
							 * every hot-loop iteration.
							 */
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
								generate_raw_jump(Jump::Jvs, call_slow_target());
							} else {
								auto [inline_ref, inline_value] =
									val_ref_single(node.operands[
										node.inlined_operand_index]);
								mov(first_reg,
									inline_value.load_to_reg(), 8);
							}
							if (!node.has_result
									|| node.continuation_block == UINT32_MAX) {
								return false;
							}
							auto [inline_result_ref, inline_result] =
								result_ref_single(node.result);
							mov(inline_result.alloc_reg(), first_reg, 8);
							inline_result.set_modified();
							free_stack_slot(
								static_cast<uint32_t>(leaf_caller_frame_slot),
								sizeof(void *));
							free_stack_slot(
								static_cast<uint32_t>(leaf_private_frame_slot),
								call.direct_call->frame_size);
							generate_uncond_branch(
								IRBlockRef{node.continuation_block});
							return true;
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
						generate_raw_jump(Jump::Jne, call_slow_target());
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
								if (source_argument.send_opline_index
										>= adaptor->plan()
											->source_opcode_count) {
									return false;
								}
								const uint8_t source_argument_type =
									adaptor->plan()->source_opcodes[
										source_argument
											.send_opline_index].op1_type;
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
											== UINT32_MAX) {
										return false;
									}
								ScratchReg payload{this};
								auto payload_reg = payload.alloc_gp();
								if (descriptor_argument.exact_type
										== ZEND_MIR_SCALAR_TYPE_I1) {
									load_off(payload_reg, frame_reg,
										descriptor_argument.source_frame_offset
											+ static_cast<uint32_t>(offsetof(
												zval, u1.type_info)),
										4);
									ASM(CMPwi, payload_reg, IS_TRUE);
									generate_raw_set(Jump::Jeq, payload_reg);
								} else {
									load_off(payload_reg, frame_reg,
										descriptor_argument.source_frame_offset,
										8);
								}
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
									if (source_argument.source_operand.kind
												== ZEND_MIR_SOURCE_OPERAND_LITERAL
											|| node.operands[index]
												== IRValueRef{Adaptor::FRAME_VALUE}) {
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
										if (descriptor_argument.exact_type
												== ZEND_MIR_SCALAR_TYPE_I1) {
											ASM(CMPwi, high_word.cur_reg(), IS_TRUE);
											generate_raw_set(
												Jump::Jeq, low_word.cur_reg());
										}
										store_off(callee_reg, offset,
											low_word.cur_reg(), 8);
										store_off(callee_reg, offset + 8,
											high_word.cur_reg(), 4);
										store_constant(callee_reg,
											offset
												+ static_cast<uint32_t>(
													offsetof(zval, u2)),
											0, 4);
										if (copy_argument) {
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
										}
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
						/*
						 * The result slot may alias any source argument
						 * (`$value = leaf($value)`).  Snapshot every argument
						 * into the private frame before publishing and
						 * invalidating that slot.
						 */
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
						first.reset();
						second.reset();
						ValuePart callee_value{DarwinConfig::GP_BANK, 8};
						callee_value.set_value(
							this, std::move(callee_address));
						auto entry_image = image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
							call.call_site->target_id);
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
						generate_raw_jump(Jump::Jeq, call_slow_target());
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
					const uint64_t activation_size =
						(sizeof(zend_native_direct_activation) + sizeof(zval) - 1)
							/ sizeof(zval) * sizeof(zval);
					const uint64_t reservation_size =
						static_cast<uint64_t>(call.direct_call->frame_size)
							+ activation_size;
					if (reservation_size > UINT32_MAX) {
						return false;
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
						generate_raw_jump(Jump::Jeq, call_slow_target());
						load_callee_function(first_reg);
						load_off(second_reg, descriptor_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_direct_call_descriptor,
								expected_function)), 8);
						ASM(CMPx, first_reg, second_reg);
						generate_raw_jump(Jump::Jne, call_slow_target());
						load_off(first_reg, published_code_reg,
							static_cast<uint32_t>(
								offsetof(zend_native_code, executable)), 1);
						ASM(CMPxi, first_reg, 1);
						generate_raw_jump(Jump::Jne, call_slow_target());
						load_off(first_reg, cell_reg,
							static_cast<uint32_t>(offsetof(
								zend_native_entry_cell, frame_probe)), 8);
						ASM(CMPxi, first_reg, 0);
						generate_raw_jump(Jump::Jne, call_slow_target());
					}
					load_off(first_reg, context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							observers_enabled)), 1);
					ASM(CMPxi, first_reg, 0);
					generate_raw_jump(Jump::Jne, call_slow_target());
					load_off(first_reg, frame_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, call)), 8);
					ASM(CMPxi, first_reg, 0);
					generate_raw_jump(Jump::Jne, call_slow_target());
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
						generate_raw_jump(Jump::Jeq, call_slow_target());
					}
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
						load_off(first_reg, frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)
									+ offsetof(zval, u1.type_info)), 4);
						ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
						ASM(CMPwi, first_reg, IS_OBJECT);
						generate_raw_jump(Jump::Jne, call_slow_target());
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
						generate_raw_jump(Jump::Jeq, call_slow_target());
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
						generate_raw_jump(Jump::jmp, call_slow_target());
						label_place(called_scope_compatible);
					} else if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						const uint32_t receiver_offset =
							call.direct_call->receiver_source_frame_offset;
						load_off(first_reg, frame_reg,
							receiver_offset + static_cast<uint32_t>(
								offsetof(zval, u1.type_info)), 4);
						ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
						ASM(CMPwi, first_reg, IS_OBJECT);
						generate_raw_jump(Jump::Jne, call_slow_target());
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
						if (zend_mir_scalar_type_is_exact(
								argument.exact_type)
								|| argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
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
							call_slow_target());
						if (argument.mode
								== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
							ASM(CMPwi, first_reg, IS_UNDEF);
							generate_raw_jump(Jump::Jeq, call_slow_target());
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
						generate_raw_jump(Jump::Jls, call_slow_target());
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
					generate_raw_jump(Jump::Jcc, call_slow_target());

					ScratchReg callee_address{this};
					auto callee_reg = callee_address.alloc_gp();
					mov(callee_reg, first_reg, 8);
					mov(second_reg, callee_reg, 8);
					add_offset(second_reg, second_reg, reservation_size);
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
							call.direct_call->receiver_source_frame_offset, 8);
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) == 0) {
							load_off(first_reg, second_reg,
								static_cast<uint32_t>(offsetof(
									zend_refcounted_h, refcount)), 4);
							ASM(ADDwi, first_reg, first_reg, 1);
							store_off(second_reg,
								static_cast<uint32_t>(offsetof(
									zend_refcounted_h, refcount)),
								first_reg, 4);
						}
						store_off(callee_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, This)),
							second_reg, 8);
						if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0) {
							store_constant(frame_reg,
								call.direct_call->receiver_source_frame_offset
									+ static_cast<uint32_t>(offsetof(
										zval, u1.type_info)),
								IS_UNDEF, 4);
						}
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
							| (call.direct_call->receiver_kind
									== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT
								? ZEND_CALL_RELEASE_THIS : 0)
							| (release_extra_arguments
								? ZEND_CALL_FREE_EXTRA_ARGS : 0),
						4);
					store_constant(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, This)
								+ offsetof(zval, u2.num_args)),
						call.direct_call->frame_argument_count, 4);

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
						static_cast<uint64_t>(fixed_argument_count)
							* sizeof(zend_op));
					store_off(callee_reg,
						static_cast<uint32_t>(
							offsetof(zend_execute_data, opline)),
						second_reg, 8);

					/* Resolve the caller's canonical result zval. */
					if (result_unused) {
						add_offset(second_reg, callee_reg,
							static_cast<uint64_t>(call.direct_call->frame_size)
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
						const uint32_t offset = static_cast<uint32_t>(
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
										ASM(CMPwi, type_info.cur_reg(), IS_TRUE);
										generate_raw_set(Jump::Jeq, argument_reg);
									}
									store_off(callee_reg, offset,
										argument_reg, 8);
									store_constant(callee_reg, offset + 8,
										0, 8);
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										ScratchReg kind{this};
										auto kind_reg = kind.alloc_gp();
										materialize_constant(IS_FALSE,
											DarwinConfig::GP_BANK, 4, kind_reg);
										ASM(ADDx, kind_reg, kind_reg, argument_reg);
										store_off(callee_reg, offset + 8,
											kind_reg, 4);
									} else {
										store_constant(callee_reg, offset + 8,
											zval_type(
												descriptor_argument.exact_type),
											4);
									}
								} else {
									if (descriptor_argument.source_frame_offset
											== UINT32_MAX) {
										return false;
									}
									ScratchReg payload{this};
									auto payload_reg = payload.alloc_gp();
									if (descriptor_argument.exact_type
											== ZEND_MIR_SCALAR_TYPE_I1) {
										load_off(payload_reg, frame_reg,
											descriptor_argument.source_frame_offset
												+ static_cast<uint32_t>(offsetof(
													zval, u1.type_info)),
											4);
										ASM(CMPwi, payload_reg, IS_TRUE);
										generate_raw_set(Jump::Jeq, payload_reg);
									} else {
										load_off(payload_reg, frame_reg,
											descriptor_argument.source_frame_offset,
											8);
									}
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
							} else if (descriptor_argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
								ScratchReg source_address{this};
								ScratchReg low_word{this};
								ScratchReg high_word{this};
								ScratchReg type_info{this};
								auto source_address_reg = source_address.alloc_gp();
								auto low_word_reg = low_word.alloc_gp();
								auto high_word_reg = high_word.alloc_gp();
								auto type_info_reg = type_info.alloc_gp();
								load_off(source_address_reg, frame_reg,
									static_cast<uint32_t>(offsetof(
										zend_execute_data, func)), 8);
								load_off(source_address_reg, source_address_reg,
									static_cast<uint32_t>(offsetof(
										zend_op_array, literals)), 8);
								add_offset(source_address_reg, source_address_reg,
									static_cast<uint64_t>(
										descriptor_argument.source_operand.index)
										* sizeof(zval));
								load_off(low_word_reg, source_address_reg, 0, 8);
								load_off(high_word_reg, source_address_reg, 8, 8);
								store_off(callee_reg, offset, low_word_reg, 8);
								store_off(callee_reg, offset + 8, high_word_reg, 8);
								load_off(type_info_reg, source_address_reg,
									static_cast<uint32_t>(offsetof(
										zval, u1.type_info)), 4);
								ASM(TSTwi, type_info_reg,
									IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
								auto copied = text_writer.label_create();
								generate_raw_jump(Jump::Jeq, copied);
								load_off(type_info_reg, low_word_reg,
									static_cast<uint32_t>(offsetof(
										zend_refcounted_h, refcount)), 4);
								ASM(ADDwi, type_info_reg, type_info_reg, 1);
								store_off(low_word_reg,
									static_cast<uint32_t>(offsetof(
										zend_refcounted_h, refcount)),
									type_info_reg, 4);
								label_place(copied);
							} else if (register_pointer_argument) {
								auto pointer =
									std::move(argument).into_scratch();
								ScratchReg type_info{this};
								auto type_info_reg = type_info.alloc_gp();
								store_off(callee_reg, offset,
									pointer.cur_reg(), 8);
								if (!emit_machine_zval_type_info(
										argument_kind, pointer.cur_reg(),
										type_info_reg)) {
									return false;
								}
								store_off(callee_reg, offset + 8,
									type_info_reg, 4);
								store_constant(callee_reg,
									offset + static_cast<uint32_t>(
										offsetof(zval, u2)),
									0, 4);
								if (copy_argument) {
									if (!emit_pointer_addref(
											argument_kind, pointer.cur_reg())) {
										return false;
									}
								} else {
									if (descriptor_argument.source_frame_offset
											== UINT32_MAX) {
										return false;
									}
									store_constant(frame_reg,
										descriptor_argument.source_frame_offset
											+ static_cast<uint32_t>(offsetof(
												zval, u1.type_info)),
										IS_UNDEF, 4);
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
								if (copy_argument) {
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
									if (descriptor_argument.source_frame_offset
											== UINT32_MAX) {
										return false;
									}
									store_constant(frame_reg,
										descriptor_argument.source_frame_offset
											+ static_cast<uint32_t>(offsetof(
												zval, u1.type_info)),
										IS_UNDEF, 4);
								}
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
							if (copy_argument) {
								ASM(TSTwi, type_info_reg,
									IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
								auto copied = text_writer.label_create();
								generate_raw_jump(Jump::Jeq, copied);
								load_off(type_info_reg, low_word_reg,
									static_cast<uint32_t>(offsetof(
										zend_refcounted_h, refcount)), 4);
								ASM(ADDwi, type_info_reg, type_info_reg, 1);
								store_off(low_word_reg,
									static_cast<uint32_t>(offsetof(
										zend_refcounted_h, refcount)),
									type_info_reg, 4);
								label_place(copied);
							} else {
								store_constant(source_address_reg,
									static_cast<uint32_t>(offsetof(
										zval, u1.type_info)),
									IS_UNDEF, 4);
							}
						}
					}
					for (uint32_t index = 0;
							index < callee_argument_count; ++index) {
						const uint32_t literal_index =
							zend_native_direct_call_default_literals_const(
								call.direct_call)[index];
						if (literal_index == UINT32_MAX) {
							continue;
						}
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
					for (uint32_t index = fixed_argument_count;
							index < compiled_variable_count; ++index) {
						const uint32_t offset = static_cast<uint32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
						store_constant(callee_reg, offset, 0, 8);
						store_constant(callee_reg, offset + 8, 0, 8);
					}

					/* Complete and link the stable trailing activation. */
					mov(second_reg, callee_reg, 8);
					add_offset(second_reg, second_reg,
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
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							generator_created)), 0, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							setup_record)), 0, 1);
					store_constant(second_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation,
							pending_call)), 0, 8);
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
					if (variadic_frame) {
						callee_address.reset();
						published_code.reset();
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
						ValuePart receive_status{DarwinConfig::GP_BANK, 4};
						{
							zend::native::tpde::CCAssignerAppleA64 receive_assigner;
							CallBuilder receive_builder{*this, receive_assigner};
							receive_builder.add_arg(
								CallArg{node.operands[frame_operand + 6]});
							receive_builder.add_arg(ValuePart{
								ZEND_RECV_VARIADIC, 4, DarwinConfig::GP_BANK},
								::tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								fixed_argument_count + 1, 4, DarwinConfig::GP_BANK},
								::tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								UINT64_C(0), 8, DarwinConfig::GP_BANK},
								::tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								UINT64_C(0), 4, DarwinConfig::GP_BANK},
								::tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								static_cast<uint64_t>(ZEND_MIR_SOURCE_OPERAND_SLOT)
									| (static_cast<uint64_t>(fixed_argument_count) << 16),
								8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								fixed_argument_count, 4, DarwinConfig::GP_BANK},
								::tpde::CCAssignment{});
							receive_builder.call(runtime_symbol(
								ZEND_NATIVE_HELPER_RECEIVE_EXPLICIT_PENDING));
							receive_builder.add_ret(
								receive_status, ::tpde::CCAssignment{});
						}
						/* Exact descriptor guards make receive failure unreachable. */
						receive_status.reset(this);
						auto [receive_frame_ref, receive_frame] =
							val_ref_single(node.operands[frame_operand + 7]);
						auto receive_frame_reg = receive_frame.load_to_reg();
						ScratchReg received_callee{this};
						auto received_callee_reg = received_callee.alloc_gp();
						load_off(received_callee_reg, receive_frame_reg,
							static_cast<uint32_t>(
								offsetof(zend_execute_data, call)), 8);
						receive_frame.reset();
						published_code_reg = published_code.alloc_gp();
						if (local_component_call) {
							materialize_constant(UINT64_C(0), DarwinConfig::GP_BANK,
								8, published_code_reg);
						} else {
							auto receive_cell = image_symbol_value(
								ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
								call.call_site->target_id);
							auto receive_cell_scratch =
								std::move(receive_cell).into_scratch(this);
							load_off(published_code_reg,
								receive_cell_scratch.cur_reg(),
								static_cast<uint32_t>(offsetof(
									zend_native_entry_cell, code)), 8);
						}
						callee_value.set_value(
							this, std::move(received_callee));
					} else {
						callee_value.set_value(
							this, std::move(callee_address));
					}
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
						auto entry_argument_reg =
							entry_argument.alloc_specific(AsmReg::R15);
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
					load_off(activation_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call)), 8);
					load_off(activation_reg, activation_reg, 0, 8);
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
					ASM(TSTwi, probe_reg, ZEND_CALL_ALLOCATED);
					generate_raw_jump(Jump::Jne, complete_fast);
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
					if (call.direct_call->receiver_kind
							== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
						ScratchReg receiver_refcount{this};
						auto receiver_refcount_reg =
							receiver_refcount.alloc_gp();
						load_off(probe_reg, post_callee_reg,
							static_cast<uint32_t>(offsetof(
								zend_execute_data, This)), 8);
						load_off(receiver_refcount_reg, probe_reg,
							static_cast<uint32_t>(offsetof(
								zend_refcounted_h, refcount)), 4);
						ASM(SUBwi, receiver_refcount_reg,
							receiver_refcount_reg, 1);
						store_off(probe_reg,
							static_cast<uint32_t>(offsetof(
								zend_refcounted_h, refcount)),
							receiver_refcount_reg, 4);
					}

					/* Helper-free successful completion. */
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							current_execute_data)), 8);
					store_off(probe_reg, 0, post_frame_reg, 8);
					load_off(probe_reg, activation_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, pending_call)), 8);
					store_off(post_frame_reg,
						static_cast<uint32_t>(offsetof(
							zend_execute_data, call)), probe_reg, 8);
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
							call.call_site->target_id);
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
					load_off(probe_reg, post_context_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_execution_context,
							vm_stack_top)), 8);
					store_off(probe_reg, 0, post_callee_reg, 8);
					generate_raw_jump(Jump::jmp, successful);

					/* Rare completion retains full exception/interrupt cleanup. */
					label_place(complete_fast);
					load_off(activation_reg, activation_reg,
						static_cast<uint32_t>(offsetof(
							zend_native_direct_activation, status)), 4);
					ValuePart finish_status_argument{
						DarwinConfig::GP_BANK, 4};
					finish_status_argument.set_value(
						this, std::move(activation));
					{
						auto [finish_frame_ref, finish_frame] =
							val_ref_single(node.operands[frame_operand + 2]);
						(void) finish_frame_ref;
						finish_frame.reset();
					}
					post_frame_scratch.reset();
					post_context_scratch.reset();
					post_callee.reset();
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
					finish_builder.add_arg(
						std::move(finish_status_argument),
						::tpde::CCAssignment{});
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
				ValuePart callee{DarwinConfig::GP_BANK, 8};
				ValuePart entry{DarwinConfig::GP_BANK, 8};
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					builder.add_arg(copy_fixed_argument(
						canonical_frame_register()),
						::tpde::CCAssignment{});
					if (!split_cold) {
						auto frame_liveness =
							val_ref(node.operands[
								frame_operand + slow_enter_frame_use]);
						(void) frame_liveness;
					}
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site->target_id), ::tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					if (split_cold) {
						builder.add_arg(CallArg{
							node.operands[context_operand]});
					} else {
						builder.add_arg(copy_fixed_argument(
							canonical_value_register(IRValueRef{
								Adaptor::EXECUTION_CONTEXT_ARGUMENT})),
							::tpde::CCAssignment{});
						auto context_liveness =
							val_ref(node.operands[
								context_operand + slow_enter_context_use]);
						(void) context_liveness;
					}
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
					if (split_cold) {
						builder.add_arg(CallArg{
							node.operands[context_operand + 1]});
					} else {
						builder.add_arg(copy_fixed_argument(
							canonical_value_register(IRValueRef{
								Adaptor::EXECUTION_CONTEXT_ARGUMENT})),
							::tpde::CCAssignment{});
						auto context_liveness =
							val_ref(node.operands[
								context_operand + slow_entry_context_use]);
						(void) context_liveness;
					}
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
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()),
					::tpde::CCAssignment{});
				{
					auto frame_liveness =
						val_ref(node.operands[split_cold
							? frame_operand
							: frame_operand + slow_leave_frame_use]);
					(void) frame_liveness;
				}
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
					call.id), ::tpde::CCAssignment{});
				if (split_cold) {
					builder.add_arg(CallArg{
						node.operands[context_operand + 2]});
				} else {
					builder.add_arg(copy_fixed_argument(
						canonical_value_register(IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT})),
						::tpde::CCAssignment{});
					auto context_liveness =
						val_ref(node.operands[
							context_operand + slow_leave_context_use]);
					(void) context_liveness;
				}
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
					generate_raw_jump(Jump::jmp, successful);
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
					const ValueParts parts = val_parts(node.result);
					for (uint32_t part = 0;
							part < parts.count(); ++part) {
						auto value = result.part(part);
						auto value_reg = value.alloc_reg();
						const zend_tpde_machine_part_role role =
							parts.representation.parts[part].semantic_role;
						if (role != ZEND_TPDE_MACHINE_PART_PAYLOAD
								&& role
									!= ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
							return false;
						}
						load_off(value_reg, result_slot_reg,
							role == ZEND_TPDE_MACHINE_PART_PAYLOAD
								? 0
								: static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							parts.size_bytes(part));
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
						call.call_site->target_id), ::tpde::CCAssignment{});
					enter_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					enter_builder.add_arg(CallArg{
						node.operands[context_operand]});
					enter_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_RESOLVE));
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
					ZEND_NATIVE_HELPER_USER_CALL_RELEASE_RESOLUTION));
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
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(copy_fixed_argument(canonical_frame_register()),
				::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site->target_id), ::tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
				call.id), ::tpde::CCAssignment{});
			for (IRValueRef operand : node.operands) {
				auto liveness = val_ref(operand);
				(void) liveness;
			}
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CALL_CONVERT_EXPLICIT));
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
						zend_tpde_encode_value_operand(call.call_site->result_operand), 8,
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
			if (node.direct_internal_argument_transport) {
				const uint32_t argument_count =
					call.call_argument_count;
				const uint32_t frame_base = argument_count;
				{
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					builder.add_arg(
						CallArg{node.operands[frame_base]});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site->target_id), ::tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN));
				}
				for (uint32_t index = 0;
						index < argument_count; ++index) {
					const IRValueRef operand = node.operands[index];
					zend::native::tpde::CCAssignerAppleA64 assigner;
					CallBuilder builder{*this, assigner};
					const IRValueRef frame_operand =
						node.operands[frame_base + 1 + index];
					if (operand != IRValueRef{Adaptor::FRAME_VALUE}
							&& adaptor->machine_kind(operand)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
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
						if (offset > UINT32_MAX - offsetof(zval, u1.type_info)) {
							return false;
						}
						auto boxed = val_ref(operand);
						auto payload = boxed.part(0);
						auto type_info = boxed.part(1);
						auto [frame_ref, frame] = val_ref_single(frame_operand);
						auto frame_reg = frame.load_to_reg();
						store_off(frame_reg, static_cast<uint32_t>(offset),
							payload.load_to_reg(), 8);
						store_off(frame_reg,
							static_cast<uint32_t>(offset
								+ offsetof(zval, u1.type_info)),
							type_info.load_to_reg(), 4);
					}
					builder.add_arg(CallArg{frame_operand});
					if (operand == IRValueRef{Adaptor::FRAME_VALUE}) {
						auto source_liveness = val_ref(operand);
						(void) source_liveness;
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id), ::tpde::CCAssignment{});
						builder.add_arg(ValuePart{index, 4,
							DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
						continue;
					}
					if (adaptor->machine_kind(operand)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id), ::tpde::CCAssignment{});
						builder.add_arg(ValuePart{index, 4,
							DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
						continue;
					}
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id), ::tpde::CCAssignment{});
					builder.add_arg(ValuePart{index, 4,
						DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
					builder.add_arg(CallArg{operand});
					if (adaptor->exact_type(operand)
							== ZEND_MIR_SCALAR_TYPE_F64) {
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT));
					} else {
						builder.add_arg(ValuePart{
							static_cast<uint32_t>(
								adaptor->exact_type(operand)),
							4, DarwinConfig::GP_BANK},
							::tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT));
					}
				}
				zend::native::tpde::CCAssignerAppleA64 assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{
					node.operands[frame_base + 1 + argument_count]});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
					call.call_site->target_id), ::tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					call.id), ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE));
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
				return_builder.add(
					std::move(status), ::tpde::CCAssignment{});
				return_builder.ret();
				label_place(continued);
				if (node.has_result) {
					zend::native::tpde::CCAssignerAppleA64 result_assigner;
					CallBuilder result_builder{*this, result_assigner};
					result_builder.add_arg(CallArg{
						node.operands[frame_base + 2 + argument_count]});
					result_builder.add_arg(ValuePart{
						zend_tpde_encode_value_operand(
							call.call_site->result_operand),
						8, DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					result_builder.add_arg(ValuePart{
						static_cast<uint32_t>(
							adaptor->exact_type(node.result)),
						4, DarwinConfig::GP_BANK},
						::tpde::CCAssignment{});
					result_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
					ValuePart payload{DarwinConfig::GP_BANK, 8};
					result_builder.add_ret(
						payload, ::tpde::CCAssignment{});
					auto [result_ref, result] =
						result_ref_single(node.result);
					if (val_parts(node.result).bank
							== DarwinConfig::FP_BANK) {
						auto payload_reg =
							payload.cur_reg_or_load(this);
						ScratchReg converted{this};
						auto result_reg =
							converted.alloc(DarwinConfig::FP_BANK);
						ASM(FMOVdx, result_reg, payload_reg);
						payload.reset(this);
						result.set_value(std::move(converted));
					} else {
						result.set_value(std::move(payload));
					}
				}
				return true;
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site->target_id), ::tpde::CCAssignment{});
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
			if (plan->source_opcodes == nullptr
					|| record.source_position_id
						>= plan->source_opcode_count) {
				return false;
			}
			const zend_tpde_source_opcode &opline =
				plan->source_opcodes[record.source_position_id];
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
			store_off(frame_scratch.cur_reg(), opline.result_var,
				value_reg, 8);
			materialize_constant(record.source_position_id,
				DarwinConfig::GP_BANK, 4, value_reg);
			store_off(frame_scratch.cur_reg(),
				opline.result_var
					+ static_cast<uint32_t>(
						offsetof(zval, u2.opline_num)),
				value_reg, 4);
			value.reset();
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
				opline.op1_var
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
							|| zend_tpde_block_successor_count(
								plan, call.block_id) != 2
							|| !zend_tpde_block_successor_at(
								plan, call.block_id, 1, &target)) {
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
						|| zend_tpde_block_successor_count(
							plan, call.block_id) != 2
						|| !zend_tpde_block_successor_at(
							plan, call.block_id, 1, &target)) {
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
			if (zend_mir_id_is_valid(mir.exception_block_id)) {
				ASM(CMPxi, status_reg, ZEND_NATIVE_CATCH_EXCEPTION);
				auto no_exception = text_writer.label_create();
				generate_raw_jump(Jump::Jne, no_exception);
				generate_exception_branch(
					adaptor->block_ref(mir.exception_block_id));
				label_place(no_exception);
			}
			ASM(CMPxi, status_reg, ZEND_NATIVE_CATCH_MATCHED);
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
				generate_cond_branch(Jump::Jeq, successors[0], successors[1]);
				status.reset(this);
				return true;
			}
			if (successor_count != 1) {
				status.reset(this);
				return false;
			}
			auto propagate = text_writer.label_create();
			generate_raw_jump(Jump::Jne, propagate);
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
				RetBuilder return_builder{
					*this, *cur_cc_assigner()};
				return_builder.add(node.operands[0]);
				return_builder.ret();
				return true;
			}
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
					ASM(TSTwi, type_info_reg,
						IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
					generate_raw_jump(Jump::Jeq, copied);
					ScratchReg count{this};
					auto count_reg = count.alloc_gp();
					load_off(count_reg, payload_reg,
						static_cast<uint32_t>(offsetof(
							zend_refcounted_h, refcount)), 4);
					ASM(ADDwi, count_reg, count_reg, 1);
					store_off(payload_reg,
						static_cast<uint32_t>(offsetof(
							zend_refcounted_h, refcount)),
						count_reg, 4);
					label_place(copied);
					return_builder.add(
						std::move(payload), ::tpde::CCAssignment{});
					return_builder.add(
						std::move(type_info), ::tpde::CCAssignment{});
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
						std::move(returned), ::tpde::CCAssignment{});
				} else {
					return_builder.add(node.operands[0]);
				}
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
						&& return_source_offset > UINT32_MAX
							- static_cast<uint32_t>(
								offsetof(zval, u1.type_info))) {
					return false;
				}
				{
				auto [frame_ref, frame] =
					val_ref_single(node.operands[1]);
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
				ScratchReg pointer{this};
				auto pointer_reg = pointer.alloc_gp();
				load_off(pointer_reg, frame_reg,
					static_cast<uint32_t>(
						offsetof(zend_execute_data, return_value)),
					8);
				auto no_result = text_writer.label_create();
				generate_raw_jump(
					Jump{Jump::Cbz, pointer_reg, false}, no_result);
				if (kind == ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto returned = val_ref(returned_ref);
					const ValueParts parts = val_parts(returned_ref);
					std::vector<ValuePartRef> locked_parts;
					locked_parts.reserve(parts.count());
					AsmReg payload_reg{};
					AsmReg type_info_reg{};
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
						ASM(TSTwi, type_info_reg,
							IS_TYPE_REFCOUNTED
								<< Z_TYPE_FLAGS_SHIFT);
						generate_raw_jump(Jump::Jeq, copied);
						ScratchReg count{this};
						auto count_reg = count.alloc_gp();
						load_off(count_reg, payload_reg,
							static_cast<uint32_t>(offsetof(
								zend_refcounted_h, refcount)),
							4);
						ASM(ADDwi, count_reg, count_reg, 1);
						store_off(payload_reg,
							static_cast<uint32_t>(offsetof(
								zend_refcounted_h, refcount)),
							count_reg, 4);
						label_place(copied);
					}
					store_off(pointer_reg, 0, payload_reg, 8);
					store_off(pointer_reg,
						static_cast<uint32_t>(
							offsetof(zval, u1.type_info)),
						type_info_reg, 4);
				} else {
					uint64_t constant_bits = 0;
					ScratchReg constant_value{this};
					std::optional<ValueRef> returned_value;
					std::optional<ValuePartRef> returned_part;
					AsmReg value_reg;
					if (adaptor->constant(
							returned_ref, &constant_bits)) {
						value_reg = constant_value.alloc(
							val_parts(returned_ref).bank);
						materialize_constant(constant_bits,
							val_parts(returned_ref).bank, 8,
							value_reg);
					} else {
						auto [value_ref, value_part] =
							val_ref_single(returned_ref);
						returned_value.emplace(std::move(value_ref));
						returned_part.emplace(std::move(value_part));
						value_reg = returned_part->load_to_reg();
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
					store_off(pointer_reg, 0, value_reg, 8);
					ScratchReg type_info{this};
					auto type_info_reg = type_info.alloc_gp();
					if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
						ASM(ORRx, type_info_reg,
							value_reg, value_reg);
						ASM(ADDwi, type_info_reg,
							type_info_reg, IS_FALSE);
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
						materialize_constant(type_info_value,
							DarwinConfig::GP_BANK, 4,
							type_info_reg);
					}
					store_off(pointer_reg,
						static_cast<uint32_t>(
							offsetof(zval, u1.type_info)),
						type_info_reg, 4);
					}
					if (!copy_source) {
						ScratchReg undef{this};
						auto undef_reg = undef.alloc_gp();
						materialize_constant(
							static_cast<uint64_t>(IS_UNDEF),
							DarwinConfig::GP_BANK, 4, undef_reg);
						store_off(frame_reg,
							static_cast<uint32_t>(return_source_offset)
								+ static_cast<uint32_t>(
									offsetof(zval, u1.type_info)),
							undef_reg, 4);
					}
					label_place(no_result);
				}
					RetBuilder return_builder{
						*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_RETURNED, 4,
					DarwinConfig::GP_BANK},
					::tpde::CCAssignment{});
				return_builder.ret();
				return true;
			}
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
			if (node.operands.empty()
					|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}) {
				return false;
			}
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			builder.add_arg(
				copy_fixed_argument(canonical_frame_register()),
				::tpde::CCAssignment{});
			{
				auto frame_liveness = val_ref(node.operands[0]);
				(void) frame_liveness;
			}
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

bool ZendCompilerA64::compile_inst(
	IRInstRef instruction, InstRange remaining_instructions) {
	const Adaptor::InstNode &node = adaptor->node(instruction);
	current_continuation_block_ = node.continuation_block;
	continuation_edge_emitted_ = false;
	const bool compiled =
		compile_inst_impl(instruction, remaining_instructions);
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

struct A64ImageState {
	Adaptor adaptor;
	ZendCompilerA64 compiler;

	explicit A64ImageState(
		std::span<const zend_tpde_plan *const> plans,
		zend_native_image *image)
		: adaptor{plans, true}, compiler{&adaptor, image} {}
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
	image->metrics.direct_typed_body_sites =
		state->adaptor.typed_body_call_site_count();
	image->metrics.direct_call_frame_bytes -= std::min(
		image->metrics.direct_call_frame_bytes,
		state->adaptor.typed_body_frame_bytes_elided());
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
