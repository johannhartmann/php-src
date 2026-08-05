// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/LinuxX64/zend_tpde_encodegen_x64.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"

#include <tpde/x64/CompilerX64.hpp>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
	std::array<tpde::SymRef, ZEND_NATIVE_HELPER_COUNT> runtime_symbols_{};
	std::vector<tpde::SymRef> image_symbols_;
	std::vector<tpde::SymRef> image_slots_;
	std::vector<tpde::Label> generator_resume_labels_;
	std::vector<tpde::Label> user_opcode_labels_;
	std::vector<tpde::Label> user_opcode_dispatch_labels_;
	std::vector<tpde::Label> user_opcode_result_reload_labels_;
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

	void reset() {
		Base::reset();
		EncodeBase::reset();
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
	AsmReg canonical_frame_register() {
		return canonical_value_register(
			IRValueRef{Adaptor::FRAME_VALUE});
	}
	ValuePart copy_fixed_argument(AsmReg source) {
		ScratchReg copy{this};
		auto copy_reg = copy.alloc_gp();
		ASM(MOV64rr, copy_reg, source);
		ValuePart value{
			tpde::x64::PlatformConfig::GP_BANK, sizeof(void *)};
		value.set_value(this, std::move(copy));
		return value;
	}

	ValuePart image_symbol_value(
		zend_native_image_symbol_kind kind, uint32_t id) {
		const zend_native_image_symbol *symbol =
			zend_tpde_image_symbol_find(
				image_, kind, id, adaptor->current_function_index());
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

	bool cur_func_may_emit_calls() const { return adaptor->plan()->may_emit_calls; }
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
		generator_resume_labels_.clear();
		generator_gateway_state_.clear();
		user_opcode_labels_.clear();
		user_opcode_dispatch_labels_.clear();
		user_opcode_result_reload_labels_.clear();
		Base::start_func(index);
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

	if (register_boxed) {
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

	if (register_string) {
		bool literal_truthy = false;
		if (!layout.has_result && adaptor->known_string_literal(
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
			ASM(MOV32rm, value_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset
						+ offsetof(zval, u1.type_info))));
			ASM(AND32ri, value_reg, Z_TYPE_MASK);
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
		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset
					+ offsetof(zval, u1.type_info))));
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
	if (layout.has_result && layout.source_opcode == ZEND_JMPZ_EX) {
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
	tpde::x64::CCAssignerSysV assigner{false};
	CallBuilder builder{*this, assigner};
	builder.add_arg(CallArg{node.operands[0]});
	const zend_mir_executable_value_ref &operation = mir.value_operation;
	builder.add_arg(ValuePart{
		zend_tpde_encode_value_operand(operation.op1), 8,
		tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		zend_tpde_encode_value_operand(operation.op2), 8,
		tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		zend_tpde_encode_value_operand(operation.result), 8,
		tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	builder.add_arg(ValuePart{operation.extended_value, 4,
		tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	builder.add_arg(ValuePart{operation.source_opcode, 4,
		tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	builder.add_arg(ValuePart{operation.source_position_id, 4,
		tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
	builder.call(runtime_symbol(mir.runtime_helper));
	ValuePart decision{tpde::x64::PlatformConfig::GP_BANK};
	builder.add_ret(decision, tpde::CCAssignment{});
	auto decision_scratch = std::move(decision).into_scratch(this);
	auto decision_reg = decision_scratch.cur_reg();
	ASM(CMP32ri, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
	auto valid = text_writer.label_create();
	generate_raw_jump(Jump::jl, valid);
	decision_scratch.reset();
	if (zend_mir_id_is_valid(mir.exception_block_id)) {
		generate_exception_branch(
			adaptor->block_ref(mir.exception_block_id));
	} else {
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(ValuePart{
			ZEND_NATIVE_EXCEPTION, 4,
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
		return_builder.ret();
	}
	label_place(valid);
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
				|| !zend_tpde_machine_value_is_register_authoritative(
					machine_kind)
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
				|| observer_reference->displacement > INT32_MAX) {
			return false;
		}
		ASM(CMP8mi,
			FE_MEM(context.load_to_reg(), 0, FE_NOREG,
				static_cast<int32_t>(
					observer_reference->displacement)),
			0);
		generate_cond_branch(
			Jump::jne,
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
		if (node.kind == Adaptor::InstKind::LoadFrame
				&& adaptor->plan()->entry_undef_temporary_count != 0) {
			auto initialized = text_writer.label_create();
			ScratchReg call_info{this};
			auto call_info_reg = call_info.alloc_gp();
			ASM(MOV32rm, call_info_reg,
				FE_MEM(result_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_execute_data, This)
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, call_info_reg, ZEND_CALL_GENERATOR);
			generate_raw_jump(Jump::jne, initialized);
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
		builder.add_arg(ValuePart{source_position, 4,
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
		generate_raw_jump(Jump::jmp, exception);
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
				auto frame_scratch = std::move(frame).into_scratch();
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
				auto frame_scratch = std::move(frame).into_scratch();
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
				finally_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				finally_call.add_arg(ValuePart{
					operation.op2_unused_payload, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				finally_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				catch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				catch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				catch_call.add_arg(ValuePart{
					operation.extended_value, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				catch_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				receive_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					operation.op1_unused_payload, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.op2, operation.op2_unused_payload),
					8, tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					operation.op2_unused_payload, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				receive_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				fragment_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.op1, operation.op1_unused_payload),
					8, tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.op1_unused_payload, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.op2, operation.op2_unused_payload),
					8, tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.op2_unused_payload, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(
						operation.result,
						operation.result_unused_payload),
					8, tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.result_unused_payload, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					operation.extended_value, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				fragment_call.call(runtime_symbol(dispatch_case.helper));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				fragment_call.add_ret(status, tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
				auto completed = text_writer.label_create();
				generate_raw_jump(Jump::je, completed);
				if (dispatch_case.instruction != nullptr
						&& zend_mir_id_is_valid(
							dispatch_case.instruction->exception_block_id)) {
					auto propagate = text_writer.label_create();
					ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
					generate_raw_jump(Jump::jne, propagate);
					generate_exception_branch(adaptor->block_ref(
						dispatch_case.instruction->exception_block_id));
					label_place(propagate);
				}
				{
					RetBuilder return_builder{
						*this, *cur_cc_assigner()};
					return_builder.add(
						std::move(status), tpde::CCAssignment{});
					return_builder.ret();
				}
				label_place(completed);
				jump_to_source(dispatch_case.source + 1);
				continue;
			}
			if (dispatch_case.kind
					== ZEND_TPDE_USER_OPCODE_TARGET_RETURN) {
				tpde::x64::CCAssignerSysV return_assigner{false};
				CallBuilder return_call{*this, return_assigner};
				return_call.add_arg(
					CallArg{node.operands[dispatch_case.frame_operand]});
				return_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_call.add_arg(ValuePart{
					operation.extended_value, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				throw_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				throw_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				throw_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				ASM(MOV64rr, slot_reg, frame_scratch.cur_reg());
				ASM(ADD64ri, slot_reg,
					static_cast<int32_t>(layout.operand_offset));
				ASM(MOV32rm, type_reg,
					FE_MEM(slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
				ASM(AND32ri, type_reg, Z_TYPE_MASK);
				auto spilled = spill_before_branch();
				begin_branch_region();
				auto dereferenced = text_writer.label_create();
				ASM(CMP32ri, type_reg, IS_REFERENCE);
				generate_raw_jump(Jump::jne, dereferenced);
				ASM(MOV64rm, slot_reg,
					FE_MEM(slot_reg, 0, FE_NOREG, 0));
				ASM(ADD64ri, slot_reg,
					static_cast<int32_t>(offsetof(zend_reference, val)));
				ASM(MOV32rm, type_reg,
					FE_MEM(slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
				ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
				branch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op1), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.op2), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					zend_tpde_encode_value_operand(operation.result), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					operation.extended_value, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					dispatch_case.target_opcode, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				branch_call.add_arg(ValuePart{
					dispatch_case.source, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
					? zend_tpde_encode_value_operand(
						operand, unused_payload)
					: zend_tpde_encode_value_operand(operand);
			};
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder operation_call{*this, assigner};
			operation_call.add_arg(
				CallArg{node.operands[dispatch_case.frame_operand]});
			operation_call.add_arg(ValuePart{
				encode_operand(
					operation.op1, operation.op1_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				encode_operand(
					operation.op2, operation.op2_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				encode_operand(
					operation.result, operation.result_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			if (explicit_auxiliary) {
				operation_call.add_arg(ValuePart{
					encode_operand(operation.auxiliary,
						operation.auxiliary_unused_payload), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
			}
			operation_call.add_arg(ValuePart{
				operation.extended_value, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				dispatch_case.target_opcode, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			operation_call.add_arg(ValuePart{
				dispatch_case.source, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			operation_call.call(runtime_symbol(dispatch_case.helper));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			operation_call.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			auto completed = text_writer.label_create();
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::je, completed);
			if (dispatch_case.instruction != nullptr
					&& zend_mir_id_is_valid(
						dispatch_case.instruction->exception_block_id)) {
				auto propagate = text_writer.label_create();
				ASM(CMP32ri, status_reg, ZEND_NATIVE_EXCEPTION);
				generate_raw_jump(Jump::jne, propagate);
				generate_exception_branch(adaptor->block_ref(
					dispatch_case.instruction->exception_block_id));
				label_place(propagate);
			}
			{
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(
					std::move(status), tpde::CCAssignment{});
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
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETURNED, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			return_builder.ret();
		}
		label_place(exception);
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
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
			if (setup_size > INT32_MAX || activation_offset > INT32_MAX
					|| placement_offset - activation_offset > INT32_MAX
					|| argument_count > UINT32_MAX
					|| (!uses_discarded_return && result_offset > INT32_MAX)) {
				return false;
			}

			auto initialize_setup = [&](auto setup_reg) {
				ScratchReg activation{this};
				ScratchReg scratch{this};
				auto activation_reg = activation.alloc_gp();
				auto scratch_reg = scratch.alloc_gp();
				ASM(MOV64rr, activation_reg, setup_reg);
				ASM(ADD64ri, activation_reg,
					static_cast<int32_t>(activation_offset));
				for (uint32_t offset = 0;
						offset < sizeof(zend_native_direct_activation);
						offset += sizeof(uint64_t)) {
					ASM(MOV64mi,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offset)), 0);
				}
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, setup_frame))),
					setup_reg);
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, caller))),
					canonical_frame_register());
				ASM(MOV64rm, scratch_reg,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, call))));
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, pending_call))),
					scratch_reg);
				auto descriptor = image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id);
				auto descriptor_scratch =
					std::move(descriptor).into_scratch(this);
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, descriptor))),
					descriptor_scratch.cur_reg());
				descriptor_scratch.reset();
				ASM(MOV32mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, setup_size))),
					static_cast<int32_t>(setup_size));
				ASM(MOV32mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation,
							placement_capacity))),
					static_cast<int32_t>(argument_count));
				ASM(MOV64rr, scratch_reg, activation_reg);
				ASM(ADD64ri, scratch_reg,
					static_cast<int32_t>(placement_offset - activation_offset));
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation,
							resolution)
							+ offsetof(zend_native_user_call_resolution,
								placements))),
					scratch_reg);
				ASM(MOV8mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, setup_record))),
					1);
				ASM(MOV64rm, scratch_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ScratchReg previous{this};
				auto previous_reg = previous.alloc_gp();
				ASM(MOV64rm, previous_reg,
					FE_MEM(scratch_reg, 0, FE_NOREG, 0));
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, previous))),
					previous_reg);
				ASM(MOV64mr,
					FE_MEM(scratch_reg, 0, FE_NOREG, 0), activation_reg);
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
				ASM(MOV64rm, top_address_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_stack_top))));
				ASM(MOV64rm, setup_reg,
					FE_MEM(top_address_reg, 0, FE_NOREG, 0));
				ASM(MOV64rm, available_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_stack_end))));
				ASM(MOV64rm, available_reg,
					FE_MEM(available_reg, 0, FE_NOREG, 0));
				ASM(SUB64rr, available_reg, setup_reg);
				ASM(CMP64ri, available_reg, static_cast<int32_t>(setup_size));
				generate_raw_jump(Jump::jb, slow_setup);
				ASM(MOV32mi,
					FE_MEM(setup_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, This)
							+ offsetof(zval, u1.type_info))), 0);
				ASM(ADD64ri, available_reg,
					static_cast<int32_t>(setup_size));
				ASM(MOV64mr,
					FE_MEM(top_address_reg, 0, FE_NOREG, 0), available_reg);
				initialize_setup(setup_reg);
				generate_raw_jump(Jump::jmp, setup_ready);
			}
			label_place(slow_setup);
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(ValuePart{setup_size, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RESERVE));
				ValuePart setup_value{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				builder.add_ret(setup_value, tpde::CCAssignment{});
				auto setup_scratch =
					std::move(setup_value).into_scratch(this);
				initialize_setup(setup_scratch.cur_reg());
			}
			label_place(setup_ready);
			reconcile_target_branch_state(setup_spilled);
			ValuePart resolution_status{
				tpde::x64::PlatformConfig::GP_BANK, 4};
			{
				ScratchReg activation{this};
				auto activation_reg = activation.alloc_gp();
				ASM(MOV64rm, activation_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ASM(MOV64rm, activation_reg,
					FE_MEM(activation_reg, 0, FE_NOREG, 0));
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				ValuePart activation_value{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				activation_value.set_value(this, std::move(activation));
				builder.add_arg(
					std::move(activation_value), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
					call.call_site.target_id), tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_USER_CALL_RESOLVE));
				builder.add_ret(resolution_status, tpde::CCAssignment{});
			}
			auto resolution_spilled = spill_target_branch_state();
			auto resolved = text_writer.label_create();
			auto status_reg = resolution_status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg,
				ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS);
			generate_raw_jump(Jump::je, resolved);
			resolution_status.reset(this);
			{
				ScratchReg resolution{this};
				auto resolution_reg = resolution.alloc_gp();
				ASM(MOV64rm, resolution_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ASM(MOV64rm, resolution_reg,
					FE_MEM(resolution_reg, 0, FE_NOREG, 0));
				ASM(ADD64ri, resolution_reg,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)));
				ASM(CMP32mi,
					FE_MEM(resolution_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_user_call_resolution, ownership))), 0);
				auto ownership_free = text_writer.label_create();
				generate_raw_jump(Jump::je, ownership_free);
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart resolution_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					resolution_value.set_value(this, std::move(resolution));
					builder.add_arg(
						std::move(resolution_value), tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_RELEASE_RESOLUTION));
				}
				label_place(ownership_free);
			}
			{
				ScratchReg active_address{this};
				ScratchReg activation{this};
				ScratchReg previous{this};
				ScratchReg setup{this};
				auto active_address_reg = active_address.alloc_gp();
				auto activation_reg = activation.alloc_gp();
				auto previous_reg = previous.alloc_gp();
				auto setup_reg = setup.alloc_gp();
				ASM(MOV64rm, active_address_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ASM(MOV64rm, activation_reg,
					FE_MEM(active_address_reg, 0, FE_NOREG, 0));
				ASM(MOV64rm, previous_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, previous))));
				ASM(MOV64mr,
					FE_MEM(active_address_reg, 0, FE_NOREG, 0), previous_reg);
				previous.reset();
				active_address.reset();
				ASM(MOV64rm, setup_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, setup_frame))));
				ASM(TEST32mi,
					FE_MEM(setup_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, This)
							+ offsetof(zval, u1.type_info))),
					ZEND_CALL_ALLOCATED);
				auto pop_allocated = text_writer.label_create();
				auto popped = text_writer.label_create();
				generate_raw_jump(Jump::jne, pop_allocated);
				ASM(MOV8mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, setup_record))),
					0);
				ScratchReg top_address{this};
				auto top_address_reg = top_address.alloc_gp();
				ASM(MOV64rm, top_address_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_stack_top))));
				ASM(MOV64mr,
					FE_MEM(top_address_reg, 0, FE_NOREG, 0), setup_reg);
				setup.reset();
				top_address.reset();
				generate_raw_jump(Jump::jmp, popped);
				label_place(pop_allocated);
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart activation_value{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					activation_value.set_value(this, std::move(activation));
					builder.add_arg(
						std::move(activation_value), tpde::CCAssignment{});
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
				return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(resolved);
			resolution_status.reset(this);
			reconcile_target_branch_state(resolution_spilled);
			auto initialize_callee = [&](auto callee_reg,
					auto activation_reg, bool allocated) {
				ScratchReg scratch{this};
				auto scratch_reg = scratch.alloc_gp();
				for (uint32_t offset = 0;
						offset < frame_header_size;
						offset += sizeof(uint64_t)) {
					ASM(MOV64mi,
						FE_MEM(callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(offset)), 0);
				}
				ASM(MOV64rm, scratch_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								object_or_called_scope))));
				ASM(MOV64mr,
					FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, This))), scratch_reg);
				ASM(MOV32rm, scratch_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								call_info))));
				if (allocated) {
					ASM(OR32ri, scratch_reg, ZEND_CALL_ALLOCATED);
				}
				ASM(MOV32mr,
					FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, This)
							+ offsetof(zval, u1.type_info))), scratch_reg);
				ASM(MOV32rm, scratch_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								argument_count))));
				ASM(MOV32mr,
					FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, This)
							+ offsetof(zval, u2.num_args))), scratch_reg);
				ASM(MOV64rm, scratch_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								function))));
				ASM(MOV64mr,
					FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, func))), scratch_reg);
				ASM(MOV64rm, scratch_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, pending_call))));
				ASM(MOV64mr,
					FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, prev_execute_data))),
					scratch_reg);
				if (uses_discarded_return) {
					ASM(MOV64rr, scratch_reg, activation_reg);
					ASM(ADD64ri, scratch_reg,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation,
							discarded_return)));
				} else {
					ASM(MOV64rr, scratch_reg, canonical_frame_register());
					ASM(ADD64ri, scratch_reg,
						static_cast<int32_t>(result_offset));
				}
				ASM(MOV64mr,
					FE_MEM(callee_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, return_value))), scratch_reg);
				ASM(MOV64mr,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, callee))), callee_reg);
				ASM(MOV64mr,
					FE_MEM(canonical_frame_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_execute_data, call))), callee_reg);
				ASM(MOV8mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation,
							raw_arguments_owned))), 1);
				ASM(MOV8mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation,
							uses_discarded_return))),
					uses_discarded_return ? 1 : 0);
				ASM(MOV8mi,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation,
							dynamic_target))), 1);

				ScratchReg argument_ptr{this};
				ScratchReg remaining{this};
				auto argument_ptr_reg = argument_ptr.alloc_gp();
				auto remaining_reg = remaining.alloc_gp();
				ASM(MOV64rr, argument_ptr_reg, callee_reg);
				ASM(ADD64ri, argument_ptr_reg,
					static_cast<int32_t>(frame_header_size));
				ASM(MOV32rm, remaining_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								argument_count))));
				auto arguments_done = text_writer.label_create();
				auto argument_loop = text_writer.label_create();
				ASM(TEST32rr, remaining_reg, remaining_reg);
				generate_raw_jump(Jump::je, arguments_done);
				label_place(argument_loop);
				ASM(MOV32mi,
					FE_MEM(argument_ptr_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
					IS_UNDEF);
				ASM(ADD64ri, argument_ptr_reg,
					static_cast<int32_t>(sizeof(zval)));
				ASM(SUB32ri, remaining_reg, 1);
				generate_raw_jump(Jump::jne, argument_loop);
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
				auto active_address_reg = active_address.alloc_gp();
				auto activation_reg = activation.alloc_gp();
				auto top_address_reg = top_address.alloc_gp();
				auto callee_reg = callee.alloc_gp();
				auto available_reg = available.alloc_gp();
				ASM(MOV64rm, active_address_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ASM(MOV64rm, activation_reg,
					FE_MEM(active_address_reg, 0, FE_NOREG, 0));
				ASM(MOV64rm, top_address_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_stack_top))));
				ASM(MOV64rm, callee_reg,
					FE_MEM(top_address_reg, 0, FE_NOREG, 0));
				ASM(MOV64rm, available_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_stack_end))));
				ASM(MOV64rm, available_reg,
					FE_MEM(available_reg, 0, FE_NOREG, 0));
				ASM(SUB64rr, available_reg, callee_reg);
				ASM(MOV32rm, active_address_reg,
					FE_MEM(activation_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								frame_size))));
				ASM(CMP64rr, available_reg, active_address_reg);
				generate_raw_jump(Jump::jb, slow_callee);
				ASM(ADD64rr, available_reg, active_address_reg);
				ASM(MOV64mr,
					FE_MEM(top_address_reg, 0, FE_NOREG, 0), available_reg);
				initialize_callee(callee_reg, activation_reg, false);
				generate_raw_jump(Jump::jmp, callee_ready);
			}
			label_place(slow_callee);
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), tpde::CCAssignment{});
				ScratchReg size{this};
				auto size_reg = size.alloc_gp();
				ASM(MOV64rm, size_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ASM(MOV64rm, size_reg,
					FE_MEM(size_reg, 0, FE_NOREG, 0));
				ASM(MOV32rm, size_reg,
					FE_MEM(size_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_direct_activation, resolution)
							+ offsetof(zend_native_user_call_resolution,
								frame_size))));
				ValuePart size_value{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				size_value.set_value(this, std::move(size));
				builder.add_arg(std::move(size_value), tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_CALL_RESERVE_DYNAMIC_FRAME));
				ValuePart callee_value{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				builder.add_ret(callee_value, tpde::CCAssignment{});
				auto callee_scratch =
					std::move(callee_value).into_scratch(this);
				ScratchReg activation{this};
				auto activation_reg = activation.alloc_gp();
				ASM(MOV64rm, activation_reg,
					FE_MEM(context_register(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context,
							active_direct_call))));
				ASM(MOV64rm, activation_reg,
					FE_MEM(activation_reg, 0, FE_NOREG, 0));
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
			ASM(MOV64rm, destination_reg,
				FE_MEM(context_register(), 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_execution_context, active_direct_call))));
			ASM(MOV64rm, destination_reg,
				FE_MEM(destination_reg, 0, FE_NOREG, 0));
		};
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
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					node.source_position, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				builder.add_ret(status, tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				auto prepared = text_writer.label_create();
				ASM(CMP32ri, status_reg, SUCCESS);
				generate_raw_jump(Jump::je, prepared);
				status.reset(this);
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_BAILOUT, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
				label_place(prepared);
				status.reset(this);
			}
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
			} else {
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				if (target_reg == tpde::x64::AsmReg{tpde::x64::AsmReg::DI}) {
					ScratchReg relocated{this};
					auto relocated_reg = relocated.alloc_gp();
					ASM(MOV32rr, relocated_reg, target_reg);
					target.reset();
					target = std::move(relocated);
					target_reg = relocated_reg;
				}
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), tpde::CCAssignment{});
				ValuePart target_value{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				target_value.set_value(this, std::move(target));
				builder.add_arg(
					std::move(target_value), tpde::CCAssignment{});
				builder.add_arg(CallArg{node.operands[2]});
				if (adaptor->exact_type(node.operands[2])
						== ZEND_MIR_SCALAR_TYPE_F64) {
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE));
				} else {
					builder.add_arg(ValuePart{
						static_cast<uint32_t>(
							adaptor->exact_type(node.operands[2])),
						4, tpde::x64::PlatformConfig::GP_BANK},
						tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_SET_INTEGER));
				}
				generate_raw_jump(Jump::jmp, completed);
			} else {
				placement.reset();
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id),
					tpde::CCAssignment{});
				builder.add_arg(ValuePart{node.argument_index, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
				builder.add_arg(ValuePart{node.argument_index, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
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
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), tpde::CCAssignment{});
			builder.add_arg(ValuePart{node.argument_index, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
		}
		if (node.kind == Adaptor::InstKind::UserCallExpand) {
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			ASM(TEST32mi,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							placement_flags))),
				ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION);
			auto complete = text_writer.label_create();
			generate_raw_jump(Jump::je, complete);
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_USER_CALL_EXPAND_ARGUMENTS));
			ValuePart expanded{tpde::x64::PlatformConfig::GP_BANK, 8};
			builder.add_ret(expanded, tpde::CCAssignment{});
			auto expanded_reg = expanded.cur_reg_or_load(this);
			ASM(TEST64rr, expanded_reg, expanded_reg);
			generate_raw_jump(Jump::jne, complete);
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
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_FRAME_ACTIVATION_RELEASE));
		};
		auto branch_released_exception = [&]() {
			ScratchReg exception{this};
			auto exception_reg = exception.alloc_gp();
			ASM(MOV64rm, exception_reg,
				FE_MEM(context_register(), 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_execution_context, exception))));
			ASM(MOV64rm, exception_reg,
				FE_MEM(exception_reg, 0, FE_NOREG, 0));
			auto no_exception = text_writer.label_create();
			ASM(TEST64rr, exception_reg, exception_reg);
			generate_raw_jump(Jump::je, no_exception);
			exception.reset();
			{
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()), tpde::CCAssignment{});
				builder.add_arg(ValuePart{
					node.source_position, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_PREPARE_FINALLY_EXCEPTION));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				builder.add_ret(status, tpde::CCAssignment{});
				auto status_reg = status.cur_reg_or_load(this);
				auto prepared = text_writer.label_create();
				ASM(CMP32ri, status_reg, SUCCESS);
				generate_raw_jump(Jump::je, prepared);
				status.reset(this);
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{
					ZEND_NATIVE_BAILOUT, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
				label_place(prepared);
				status.reset(this);
			}
			if (zend_mir_id_is_valid(call.exception_block_id)) {
				generate_exception_branch(
					adaptor->block_ref(call.exception_block_id));
			} else {
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(no_exception);
		};

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

		auto normalized = text_writer.label_create();
		auto no_call = text_writer.label_create();
		auto internal_call = text_writer.label_create();
		auto user_call = text_writer.label_create();
		auto do_succeeded = text_writer.label_create();
		{
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(copy_fixed_argument(
				canonical_frame_register()), tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.id),
				tpde::CCAssignment{});
			ScratchReg activation{this};
			ScratchReg target_kind{this};
			auto activation_reg = activation.alloc_gp();
			auto target_kind_reg = target_kind.alloc_gp();
			load_active_activation(activation_reg);
			ASM(MOV32rm, target_kind_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							target_kind))));
			ASM(CMP32ri, target_kind_reg,
				ZEND_NATIVE_USER_CALL_TARGET_NO_CALL);
			generate_raw_jump(Jump::je, no_call);
			ASM(CMP32ri, target_kind_reg,
				ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE);
			generate_raw_jump(Jump::jne, normalized);
			target_kind.reset();
			ScratchReg callee{this};
			auto callee_reg = callee.alloc_gp();
			ASM(MOV64rm, callee_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, callee))));
			ScratchReg resolution{this};
			auto resolution_reg = resolution.alloc_gp();
			ASM(MOV64rr, resolution_reg, activation_reg);
			ASM(ADD64ri, resolution_reg,
				static_cast<int32_t>(offsetof(
					zend_native_direct_activation, resolution)));
			activation.reset();
			ValuePart callee_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			callee_value.set_value(this, std::move(callee));
			builder.add_arg(
				std::move(callee_value), tpde::CCAssignment{});
			ValuePart resolution_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			resolution_value.set_value(this, std::move(resolution));
			builder.add_arg(
				std::move(resolution_value), tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_USER_CALL_NORMALIZE_RESOLUTION));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg,
				ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS);
			generate_raw_jump(Jump::je, normalized);
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
			ASM(MOV32rm, target_kind_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							target_kind))));
			ASM(CMP32ri, target_kind_reg,
				ZEND_NATIVE_USER_CALL_TARGET_INTERNAL);
			generate_raw_jump(Jump::je, internal_call);
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
			ASM(MOV8mi,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, internal_target))), 1);
			ASM(MOV64rm, callee_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, callee))));
			ASM(MOV64rm, entry_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							invoke_entry))));
			activation.reset();
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart callee_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			callee_value.set_value(this, std::move(callee));
			builder.add_arg(
				std::move(callee_value), tpde::CCAssignment{});
			builder.add_arg(context_argument(), tpde::CCAssignment{});
			ValuePart entry_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			entry_value.set_value(this, std::move(entry));
			builder.call(std::move(entry_value));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ASM(CMP32ri, status_reg, ZEND_NATIVE_RETURNED);
			auto returned = text_writer.label_create();
			generate_raw_jump(Jump::je, returned);
			status.reset(this);
			emit_phase_failure();
			label_place(returned);
			status.reset(this);
			release_active();
			branch_released_exception();
			generate_raw_jump(Jump::jmp, do_succeeded);
		}

		label_place(user_call);
		ValuePart prepared{tpde::x64::PlatformConfig::GP_BANK, 4};
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
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FRAME_PREPARE));
			builder.add_ret(prepared, tpde::CCAssignment{});
		}
		{
			auto prepared_reg = prepared.cur_reg_or_load(this);
			ASM(CMP32ri, prepared_reg, SUCCESS);
			auto prepare_ok = text_writer.label_create();
			generate_raw_jump(Jump::je, prepare_ok);
			prepared.reset(this);
			emit_phase_failure();
			label_place(prepare_ok);
			prepared.reset(this);
		}
		ValuePart begin_status{
			tpde::x64::PlatformConfig::GP_BANK, 4};
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
				ZEND_NATIVE_HELPER_FRAME_OBSERVER_BEGIN));
			builder.add_ret(begin_status, tpde::CCAssignment{});
		}
		auto invoke_user = text_writer.label_create();
		auto observer_end = text_writer.label_create();
		{
			auto begin_reg = begin_status.cur_reg_or_load(this);
			ScratchReg activation{this};
			auto activation_reg = activation.alloc_gp();
			load_active_activation(activation_reg);
			ASM(MOV32mr,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, status))), begin_reg);
			activation.reset();
			ASM(CMP32ri, begin_reg, ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::je, invoke_user);
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
			ASM(MOV64rm, callee_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, callee))));
			ASM(MOV64rm, entry_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, resolution)
						+ offsetof(zend_native_user_call_resolution,
							invoke_entry))));
			activation.reset();
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart callee_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			callee_value.set_value(this, std::move(callee));
			builder.add_arg(
				std::move(callee_value), tpde::CCAssignment{});
			builder.add_arg(context_argument(), tpde::CCAssignment{});
			ValuePart entry_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			entry_value.set_value(this, std::move(entry));
			builder.call(std::move(entry_value));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(status, tpde::CCAssignment{});
			auto status_reg = status.cur_reg_or_load(this);
			ScratchReg active{this};
			auto active_reg = active.alloc_gp();
			load_active_activation(active_reg);
			ASM(MOV32mr,
				FE_MEM(active_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, status))), status_reg);
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
			ASM(MOV32rm, status_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, status))));
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), tpde::CCAssignment{});
			ValuePart status_value{
				tpde::x64::PlatformConfig::GP_BANK, 4};
			status_value.set_value(this, std::move(status));
			builder.add_arg(
				std::move(status_value), tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_FRAME_OBSERVER_END));
		}
		ValuePart finalized{tpde::x64::PlatformConfig::GP_BANK, 4};
		{
			ScratchReg activation{this};
			ScratchReg status{this};
			auto activation_reg = activation.alloc_gp();
			auto status_reg = status.alloc_gp();
			load_active_activation(activation_reg);
			ASM(MOV32rm, status_reg,
				FE_MEM(activation_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(
						zend_native_direct_activation, status))));
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			ValuePart activation_value{
				tpde::x64::PlatformConfig::GP_BANK, 8};
			activation_value.set_value(this, std::move(activation));
			builder.add_arg(
				std::move(activation_value), tpde::CCAssignment{});
			ValuePart status_value{
				tpde::x64::PlatformConfig::GP_BANK, 4};
			status_value.set_value(this, std::move(status));
			builder.add_arg(
				std::move(status_value), tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FRAME_FINALIZE));
			builder.add_ret(finalized, tpde::CCAssignment{});
		}
		{
			auto finalized_reg = finalized.cur_reg_or_load(this);
			auto user_returned = text_writer.label_create();
			ASM(CMP32ri, finalized_reg, ZEND_NATIVE_RETURNED);
			generate_raw_jump(Jump::je, user_returned);
			ASM(CMP32ri, finalized_reg, ZEND_NATIVE_GENERATOR_CREATED);
			generate_raw_jump(Jump::je, user_returned);
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
				call.call_site.target_id), tpde::CCAssignment{});
			builder.add_arg(ValuePart{UINT64_C(0), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
		} else {
			builder.add_arg(ValuePart{UINT64_C(0), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site.target_id), tpde::CCAssignment{});
		}
		builder.add_arg(image_symbol_value(
			ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
			call.id), tpde::CCAssignment{});
		builder.add_arg(ValuePart{node.argument_index, 4,
			tpde::x64::PlatformConfig::GP_BANK},
			tpde::CCAssignment{});
		builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_FRAGMENT));
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
		ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
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
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(
				std::move(status), tpde::CCAssignment{});
			return_builder.ret();
		}
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
		{
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
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
		if (frame_offset + offsetof(zval, u1.type_info) > INT32_MAX) {
			return false;
		}
		auto emit = [&](AsmReg address, int32_t offset) {
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(MOV32rm, result_reg,
				FE_MEM(address, 0, FE_NOREG,
					offset + static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(AND32ri, result_reg, Z_TYPE_MASK);
			result.set_modified();
		};
		if (frame_slot) {
			emit(canonical_frame_register(),
				static_cast<int32_t>(frame_offset));
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
						> INT32_MAX) {
				return false;
			}
			ScratchReg type{this};
			auto type_reg = type.alloc_gp();
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offset + offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
		if (offset + offsetof(zval, u1.type_info) > INT32_MAX) {
			return false;
		}
		auto [frame_ref, frame] = val_ref_single(node.operands[0]);
		auto frame_reg = frame.load_to_reg();
		ScratchReg type{this};
		auto type_reg = type.alloc_gp();
		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offset + offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		auto matched = text_writer.label_create();
		if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			ASM(CMP32ri, type_reg, IS_FALSE);
			generate_raw_jump(Jump::je, matched);
			ASM(CMP32ri, type_reg, IS_TRUE);
		} else {
			ASM(CMP32ri, type_reg,
				static_cast<int32_t>(zval_type(node.exact_type)));
		}
		generate_raw_jump(Jump::je, matched);
		type.reset();
		frame.reset();
		frame_ref.reset();
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
		ASM(MOV32rm, type_reg,
			FE_MEM(result_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
					if (node.exact_type == ZEND_MIR_SCALAR_TYPE_F64) {
						ASM(SSE_MOVSDrm, value_reg,
							FE_MEM(canonical_frame_register(), 0,
								FE_NOREG, static_cast<int32_t>(frame_offset)));
					} else {
						ASM(MOV64rm, value_reg,
							FE_MEM(canonical_frame_register(), 0,
								FE_NOREG, static_cast<int32_t>(frame_offset)));
					}
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
		builder.add_arg(ValuePart{
			static_cast<uint32_t>(record.source_position_id), 4,
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
			ASM(MOV64rr, slot_reg, frame_reg);
			ASM(ADD64ri, slot_reg, static_cast<int32_t>(offset));
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
			ASM(MOV64rm, refcount_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(MOV32rm, old_type_reg,
				FE_MEM(refcount_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))));
			ASM(CMP32ri, old_type_reg, 1);
			generate_raw_jump(Jump::jbe, slow_release);
			ASM(SUB32mi,
				FE_MEM(refcount_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zend_refcounted_h, refcount))),
				1);
			generate_raw_jump(Jump::jmp, store_value);
		}

		old_type.reset();
		counted.reset();
		refcount.reset();
		label_place(slow_release);
		ASM(MOV32ri, decision_reg, 1);
		generate_raw_jump(Jump::jmp, finished);

		label_place(store_value);
		auto store_slot_resolved = text_writer.label_create();
		ScratchReg store_slot{this};
		ScratchReg store_type{this};
		auto store_slot_reg = store_slot.alloc_gp();
		auto store_type_reg = store_type.alloc_gp();
		ASM(MOV64rr, store_slot_reg, frame_reg);
		ASM(ADD64ri, store_slot_reg, static_cast<int32_t>(offset));
		if (!mir.zval_store_direct_scalar) {
			ASM(MOV32rm, store_type_reg,
				FE_MEM(store_slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))));
			ASM(CMP32ri, store_type_reg, IS_REFERENCE_EX);
			generate_raw_jump(Jump::jne, store_slot_resolved);
			ASM(MOV64rm, store_slot_reg,
				FE_MEM(store_slot_reg, 0, FE_NOREG, 0));
			ASM(ADD64ri, store_slot_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
		}
		label_place(store_slot_resolved);
		if (exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
			ASM(MOV64mi,
				FE_MEM(store_slot_reg, 0, FE_NOREG, 0),
				0);
			ASM(MOV32mi,
				FE_MEM(store_slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(offsetof(zval, u1.type_info))),
				IS_NULL);
		} else {
			auto [value_ref, value] = val_ref_single(input);
			auto value_reg = value.load_to_reg();
			if (val_parts(input).bank
					== tpde::x64::PlatformConfig::FP_BANK) {
				ASM(SSE_MOVSDmr,
					FE_MEM(store_slot_reg, 0, FE_NOREG, 0),
					value_reg);
			} else {
				ASM(MOV64mr,
					FE_MEM(store_slot_reg, 0, FE_NOREG, 0),
					value_reg);
			}
			if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
				ScratchReg kind{this};
				auto kind_reg = kind.alloc_gp();
				ASM(MOV64rr, kind_reg, value_reg);
				ASM(ADD64ri, kind_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(store_slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
					kind_reg);
			} else {
				ASM(MOV32mi,
					FE_MEM(store_slot_reg, 0, FE_NOREG,
						static_cast<int32_t>(offsetof(zval, u1.type_info))),
					static_cast<int32_t>(zval_type(exact_type)));
			}
		}
		ASM(MOV32ri, decision_reg, 0);
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
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
		builder.add_arg(std::move(frame), tpde::CCAssignment{});

		ScratchReg slot{this};
		auto slot_reg = slot.alloc_gp();
		ASM(MOV64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval)));
		ValuePart slot_pointer{tpde::x64::PlatformConfig::GP_BANK, 8};
		slot_pointer.set_value(this, std::move(slot));

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
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
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
			builder.call(runtime_symbol(mir.runtime_helper));
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
	auto floating_compare = [&](Jump condition) {
		auto [left_pair, right_pair] = binary();
		auto &[left_ref, left] = left_pair;
		auto &[right_ref, right] = right_pair;
		ASM(SSE_UCOMISDrr, left.load_to_reg(), right.load_to_reg());
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
		tpde::x64::CCAssignerSysV assigner{false};
		CallBuilder builder{*this, assigner};
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
		if (frame_argument != nullptr) {
			builder.add_arg(
				std::move(*frame_argument), tpde::CCAssignment{});
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
			tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
		if (const_include_once) {
			builder.add_arg(ValuePart{operation.extended_value, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CONST_INCLUDE_ONCE));
		} else if (helper == ZEND_NATIVE_HELPER_THROW_SOURCE_ZVAL) {
			builder.add_arg(ValuePart{source_opcode, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
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
			builder.add_arg(ValuePart{
				encode_operand(
					operation.op2, operation.op2_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				encode_operand(
					operation.result, operation.result_unused_payload), 8,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			if (explicit_auxiliary) {
				builder.add_arg(ValuePart{
					encode_operand(operation.auxiliary,
						operation.auxiliary_unused_payload), 8,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
			}
			builder.add_arg(ValuePart{operation.extended_value, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.add_arg(ValuePart{source_opcode, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.add_arg(ValuePart{operation.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK},
				tpde::CCAssignment{});
			builder.call(runtime_symbol(helper));
		}
		ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
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
		return_builder.ret_local_path();
		label_place(continued);
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
				auto part_reg = part_value.load_to_reg();
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
		} else {
			ASM(MOV32rm, source_type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						source_offset + offsetof(zval, u1.type_info))));
		}
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
		/*
		 * Immutable refcounted constants need the VM's duplication rules
		 * before they become a writable CV.  They also must never receive a
		 * native refcount write.  Keep that ownership transition on the
		 * semantic cold path.
		 */
		ASM(MOV32rr, probe_reg, source_type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
		auto source_mutable = text_writer.label_create();
		generate_raw_jump(Jump::je, source_mutable);
		if (!register_source) {
			ASM(MOV64rm, source_payload_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(source_offset)));
		}
		ASM(MOV32rm, probe_reg,
			FE_MEM(source_payload_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, u.type_info))));
		ASM(TEST32ri, probe_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::jne, slow);
		label_place(source_mutable);
		ASM(MOV32rm, target_type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					target_offset + offsetof(zval, u1.type_info))));
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
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(target_offset)));
		ASM(MOV32rm, probe_reg,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))));
		ASM(CMP32ri, probe_reg, 1);
		generate_raw_jump(Jump::jle, slow);
		label_place(target_checked);
		if (result_storage != ZEND_MIR_ID_INVALID) {
			ASM(MOV32rm, probe_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						result_offset + offsetof(zval, u1.type_info))));
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
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(target_offset)));
		ASM(SUB32mi,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(target_released);
		if (register_source) {
			ASM(MOV64rr, low_word_reg, source_payload_reg);
		} else {
			ASM(MOV64rm, low_word_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(source_offset)));
		}
		const uint32_t source_refcount_increments =
			(!move_source ? 1 : 0)
			+ (result_storage != ZEND_MIR_ID_INVALID ? 1 : 0);
		if (source_refcount_increments != 0) {
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
				source_refcount_increments);
			label_place(value_owned);
		}
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(target_offset)),
			low_word_reg);
		ASM(MOV32mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					target_offset + offsetof(zval, u1.type_info))),
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					source_offset + offsetof(zval, u1.type_info))));
		ASM(MOV32rr, probe_reg, type_reg);
		ASM(AND32ri, probe_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, probe_reg, probe_reg);
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
				|| layout.container_offset > INT32_MAX - 8
				|| layout.key_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
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
								ASM(MOV64rr, key_reg, value_reg);
								have_payload = true;
								break;
							case ZEND_TPDE_MACHINE_PART_TYPE_INFO:
								ASM(MOV32rr, type_reg, value_reg);
								have_type_info = true;
								break;
							default:
								return false;
						}
					}
					if (!have_payload || !have_type_info) {
						return false;
					}
					ASM(AND32ri, type_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_reg, IS_LONG);
					generate_raw_jump(Jump::jne, slow);
				} else {
					auto [key_ref, key] =
						val_ref_single(node.operands[0]);
					ASM(MOV64rr, key_reg, key.load_to_reg());
				}

				const bool register_receiver =
					node.operands.size() > 1
					&& node.machine_reference_operand_index != 1
					&& adaptor->machine_kind(node.operands[1])
						== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR;
				if (register_receiver) {
					auto [receiver_ref, receiver] =
						val_ref_single(node.operands[1]);
					ASM(MOV64rr, array_reg, receiver.load_to_reg());
				} else if (layout.container_literal) {
					if (node.machine_reference_operand_index
							>= node.operands.size()) {
						return false;
					}
					auto [literal_ref, literal] = val_ref_single(
						node.operands[
							node.machine_reference_operand_index]);
					auto literal_reg = literal.load_to_reg();
					ASM(MOV32rm, type_reg,
						FE_MEM(literal_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
					ASM(AND32ri, type_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rm, array_reg,
						FE_MEM(literal_reg, 0, FE_NOREG, 0));
				} else {
					ASM(MOV32rm, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								layout.container_offset
									+ offsetof(zval, u1.type_info))));
					ASM(AND32ri, type_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rm, array_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								layout.container_offset)));
				}
				ASM(MOV32rm, type_reg,
					FE_MEM(array_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(HashTable, u))));
				ASM(TEST32ri, type_reg, HASH_FLAG_PACKED);
				if (register_string_key) {
					generate_raw_jump(Jump::jne, slow);
					auto content_loop = text_writer.label_create();
					auto slot_reg = slot.alloc_gp();
					ScratchReg probe{this};
					auto probe_reg = probe.alloc_gp();
					ASM(MOV64rm, element_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(HashTable, arData))));
					ASM(MOV64rm, type_reg,
						FE_MEM(key_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_string, h))));
					ASM(MOV32rm, limit_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(HashTable, nTableMask))));
					ASM(OR32rr, limit_reg, type_reg);
					ASM(MOVSXr64r32, limit_reg, limit_reg);
					ASM(MOV32rm, limit_reg,
						FE_MEM(element_reg, 4, limit_reg, 0));
					label_place(mixed_loop);
					ASM(CMP32ri, limit_reg, HT_INVALID_IDX);
					generate_raw_jump(Jump::je, slow);
					ASM(MOV64rr, slot_reg, limit_reg);
					ASM(SHL64ri, slot_reg, 5);
					ASM(ADD64rr, slot_reg, element_reg);
					ASM(MOV64rm, type_reg,
						FE_MEM(key_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_string, h))));
					ASM(MOV64rm, limit_reg,
						FE_MEM(slot_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(Bucket, h))));
					ASM(CMP64rr, limit_reg, type_reg);
					generate_raw_jump(Jump::jne, mixed_next);
					ASM(MOV64rm, limit_reg,
						FE_MEM(slot_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(Bucket, key))));
					ASM(CMP64rr, limit_reg, key_reg);
					generate_raw_jump(Jump::je, found);
					ASM(MOV64rm, decision_reg,
						FE_MEM(limit_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_string, len))));
					ASM(MOV64rm, array_reg,
						FE_MEM(key_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(zend_string, len))));
					ASM(CMP64rr, decision_reg, array_reg);
					generate_raw_jump(Jump::jne, mixed_next);
					ASM(TEST64rr, decision_reg, decision_reg);
					generate_raw_jump(Jump::je, found);
					ASM(ADD64ri, limit_reg,
						static_cast<int32_t>(offsetof(zend_string, val)));
					ASM(MOV64rr, array_reg, key_reg);
					ASM(ADD64ri, array_reg,
						static_cast<int32_t>(offsetof(zend_string, val)));
					label_place(content_loop);
					ASM(MOVZXr32m8, type_reg,
						FE_MEM(limit_reg, 0, FE_NOREG, 0));
					ASM(MOVZXr32m8, probe_reg,
						FE_MEM(array_reg, 0, FE_NOREG, 0));
					ASM(CMP32rr, type_reg, probe_reg);
					generate_raw_jump(Jump::jne, mixed_next);
					ASM(ADD64ri, limit_reg, 1);
					ASM(ADD64ri, array_reg, 1);
					ASM(SUB32ri, decision_reg, 1);
					generate_raw_jump(Jump::jne, content_loop);
					generate_raw_jump(Jump::jmp, found);
					label_place(mixed_next);
					ASM(MOV32rm, limit_reg,
						FE_MEM(slot_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(Bucket, val)
								+ offsetof(zval, u2.next))));
					generate_raw_jump(Jump::jmp, mixed_loop);
					label_place(found);
					ASM(MOV64rr, element_reg, slot_reg);
				} else {
					generate_raw_jump(Jump::je, slow);
					ASM(MOV32rm, limit_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(HashTable, nNumUsed))));
					ASM(CMP64rr, key_reg, limit_reg);
					generate_raw_jump(Jump::jae, slow);
					ASM(MOV64rm, element_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(HashTable, arPacked))));
					ASM(SHL64ri, key_reg, 4);
					ASM(ADD64rr, element_reg, key_reg);
				}
				ASM(MOV32rm, type_reg,
					FE_MEM(element_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
				ASM(AND32ri, type_reg, Z_TYPE_MASK);
				ASM(CMP32ri, type_reg, IS_LONG);
				generate_raw_jump(Jump::jne, slow);
				if (adaptor->machine_kind(node.result)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
					auto result = result_ref(node.result);
					auto payload = result.part(0);
					auto type_info = result.part(1);
					auto payload_reg = payload.alloc_reg();
					auto type_info_reg = type_info.alloc_reg();
					ASM(MOV64rm, payload_reg,
						FE_MEM(element_reg, 0, FE_NOREG, 0));
					ASM(MOV32ri, type_info_reg, IS_LONG);
					payload.set_modified();
					type_info.set_modified();
				} else {
					auto [result_ref, result] =
						result_ref_single(node.result);
					auto result_reg = result.alloc_reg();
					ASM(MOV64rm, result_reg,
						FE_MEM(element_reg, 0, FE_NOREG, 0));
					result.set_modified();
				}
				ASM(MOV32ri, decision_reg, 0);
				generate_raw_jump(Jump::jmp, done);
				label_place(slow);
				ASM(MOV32ri, decision_reg, 1);
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.container_offset
						+ offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.container_offset)));

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.key_offset + offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_LONG);
		generate_raw_jump(Jump::je, key_long);
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, key_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.key_offset)));
		ASM(MOV32ri, high_word_reg, 1);
		generate_raw_jump(Jump::jmp, key_ready);
		label_place(key_long);
		ASM(MOV64rm, key_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.key_offset)));
		ASM(MOV32ri, high_word_reg, 0);
		label_place(key_ready);

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32rm, limit_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, limit_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
		}

		ASM(MOV32rm, type_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, u))));
		ASM(AND32ri, type_reg, HASH_FLAG_PACKED);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, packed);

		ASM(TEST32rr, high_word_reg, high_word_reg);
		generate_raw_jump(Jump::jne, mixed_string);
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

		label_place(mixed_string);
		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arData))));
		ASM(MOV64rm, type_reg,
			FE_MEM(key_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, h))));
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nTableMask))));
		ASM(MOV32rr, high_word_reg, type_reg);
		ASM(OR32rr, high_word_reg, limit_reg);
		ASM(MOVSXr64r32, high_word_reg, high_word_reg);
		ASM(MOV32rm, limit_reg,
			FE_MEM(element_reg, 4, high_word_reg, 0));
		label_place(mixed_string_loop);
		ASM(CMP32ri, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rr, slot_reg, limit_reg);
		ASM(SHL64ri, slot_reg, 5);
		ASM(ADD64rr, slot_reg, element_reg);
		ASM(MOV64rm, high_word_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, h))));
		ASM(CMP64rr, high_word_reg, type_reg);
		generate_raw_jump(Jump::jne, mixed_string_next);
		ASM(MOV64rm, high_word_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, key))));
		ASM(CMP64rr, high_word_reg, key_reg);
		generate_raw_jump(Jump::je, found);
		label_place(mixed_string_next);
		ASM(MOV32rm, limit_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(Bucket, val) + offsetof(zval, u2.next))));
		generate_raw_jump(Jump::jmp, mixed_string_loop);

		label_place(packed);
		ASM(TEST32rr, high_word_reg, high_word_reg);
		generate_raw_jump(Jump::jne, slow);
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

		if (adaptor->machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				ASM(MOV64rm, payload_reg,
					FE_MEM(element_reg, 0, FE_NOREG, 0));
				ASM(MOV32rm, type_info_reg,
					FE_MEM(element_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							offsetof(zval, u1.type_info))));
				payload.set_modified();
				type_info.set_modified();
				ASM(MOV64rr, low_word_reg, payload_reg);
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
							FE_MEM(element_reg, 0, FE_NOREG, 0));
						break;
					case ZEND_MIR_SCALAR_TYPE_F64:
						ASM(SSE_MOVSDrm, result_reg,
							FE_MEM(element_reg, 0, FE_NOREG, 0));
						break;
					default:
						switch (adaptor->machine_kind(node.result)) {
							case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
							case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
							case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
							case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
							case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
								ASM(MOV64rm, result_reg,
									FE_MEM(element_reg, 0, FE_NOREG, 0));
								break;
							default:
								return false;
						}
				}
				result.set_modified();
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			ASM(MOV32ri, decision_reg, 0);
		} else {
			ASM(MOV64rm, low_word_reg,
				FE_MEM(element_reg, 0, FE_NOREG, 0));
			ASM(MOV64rm, high_word_reg,
				FE_MEM(element_reg, 0, FE_NOREG, 8));
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				low_word_reg);
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset + 8)),
				high_word_reg);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			generate_raw_jump(Jump::jmp, done);
		} else {
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
		ASM(MOV32ri, decision_reg, 1);
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

		if (!zend_tpde_array_isset_at(mir, &layout)
				|| layout.container_offset > INT32_MAX - 8
				|| layout.key_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.container_offset
						+ offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.container_offset)));

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.key_offset + offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_LONG);
		generate_raw_jump(Jump::je, key_long);
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, key_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.key_offset)));
		ASM(MOV32ri, key_kind_reg, 1);
		generate_raw_jump(Jump::jmp, key_ready);
		label_place(key_long);
		ASM(MOV64rm, key_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.key_offset)));
		ASM(MOV32ri, key_kind_reg, 0);
		label_place(key_ready);

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.result_offset + offsetof(zval, u1.type_info))));
		ASM(TEST32ri, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::jne, slow);

		ASM(MOV32rm, type_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, u))));
		ASM(AND32ri, type_reg, HASH_FLAG_PACKED);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::jne, packed);

		ASM(TEST32rr, key_kind_reg, key_kind_reg);
		generate_raw_jump(Jump::jne, mixed_string);
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

		label_place(mixed_string);
		ASM(MOV64rm, element_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arData))));
		ASM(MOV64rm, type_reg,
			FE_MEM(key_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, h))));
		ASM(MOV32rm, limit_reg,
			FE_MEM(array_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nTableMask))));
		ASM(MOV32rr, key_kind_reg, type_reg);
		ASM(OR32rr, key_kind_reg, limit_reg);
		ASM(MOVSXr64r32, key_kind_reg, key_kind_reg);
		ASM(MOV32rm, limit_reg,
			FE_MEM(element_reg, 4, key_kind_reg, 0));
		label_place(mixed_string_loop);
		ASM(CMP32ri, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rr, slot_reg, limit_reg);
		ASM(SHL64ri, slot_reg, 5);
		ASM(ADD64rr, slot_reg, element_reg);
		ASM(MOV64rm, key_kind_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, h))));
		ASM(CMP64rr, key_kind_reg, type_reg);
		generate_raw_jump(Jump::jne, mixed_string_next);
		ASM(MOV64rm, key_kind_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, key))));
		ASM(CMP64rr, key_kind_reg, key_reg);
		generate_raw_jump(Jump::je, found);
		label_place(mixed_string_next);
		ASM(MOV32rm, limit_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(Bucket, val) + offsetof(zval, u2.next))));
		generate_raw_jump(Jump::jmp, mixed_string_loop);

		label_place(packed);
		ASM(TEST32rr, key_kind_reg, key_kind_reg);
		generate_raw_jump(Jump::jne, slow);
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
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32ri, type_reg, IS_FALSE);
		}
		generate_raw_jump(Jump::jmp, store_answer);
		label_place(answer_true);
		ASM(MOV64ri, element_reg, 1);
		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32ri, type_reg, IS_TRUE);
		}
		label_place(store_answer);
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(MOV64rr, result_reg, element_reg);
			result.set_modified();
			ASM(MOV32ri, decision_reg, 0);
		} else {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				element_reg);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))),
				type_reg);
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
				|| layout.container_offset > INT32_MAX - 8
				|| layout.value_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.container_offset
						+ offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, array_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.container_offset)));
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

		if (!scalar_value) {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))));
			ASM(MOV32rr, limit_reg, type_reg);
			ASM(AND32ri, limit_reg, Z_TYPE_MASK);
			ASM(CMP32ri, limit_reg, IS_UNDEF);
			generate_raw_jump(Jump::je, slow);
			ASM(CMP32ri, limit_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
			ASM(CMP32ri, limit_reg, IS_INDIRECT);
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
				auto frame_scratch = std::move(frame).into_scratch();
				auto frame_reg = frame_scratch.cur_reg();
				ScratchReg result{this};
				auto result_reg = result.alloc_gp();
				ASM(MOV32rm, result_reg,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(boolean.operand_offset)));
				if (boolean.negate) {
					ASM(XOR32ri, result_reg, 1);
				}
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(boolean.result_offset)),
					result_reg);
				ScratchReg type{this};
				auto type_reg = type.alloc_gp();
				ASM(MOV32rr, type_reg, result_reg);
				ASM(ADD32ri, type_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(boolean.result_offset
							+ offsetof(zval, u1.type_info))), type_reg);
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
		if (layout.operand_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8
				|| (!frame_only && !register_string)) {
			return false;
		}
		if (normal_register_string) {
			auto [frame_ref, frame] = val_ref_single(node.operands[1]);
			auto frame_scratch = std::move(frame).into_scratch();
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
			auto frame_scratch = std::move(frame).into_scratch();
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
		auto frame_scratch = std::move(frame).into_scratch();
		auto frame_reg = frame_scratch.cur_reg();
		ScratchReg type{this};
		ScratchReg string{this};
		ScratchReg decision{this};
		auto type_reg = type.alloc_gp();
		auto string_reg = string.alloc_gp();
		auto decision_reg = decision.alloc_gp();

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.operand_offset + offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, string_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.operand_offset)));

		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		ASM(MOV64rm, result_reg,
			FE_MEM(string_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, len))));
		result.set_modified();
		ASM(MOV64mr,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.result_offset)),
			result_reg);
		ASM(MOV32mi,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.result_offset
					+ offsetof(zval, u1.type_info))),
			IS_LONG);
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
	auto string_identity = [&]() {
		zend_tpde_string_identity layout;

		if (!zend_tpde_string_identity_at(mir, &layout)
				|| layout.left_offset > INT32_MAX - 8
				|| layout.right_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.left_offset + offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, left_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.left_offset)));

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.right_offset + offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_STRING);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, right_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.right_offset)));
		ASM(CMP64rr, left_reg, right_reg);
		generate_raw_jump(Jump::jne, slow);

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				auto type_info_reg = type_info.alloc_reg();
				ASM(MOV64ri, payload_reg, layout.inverted ? 0 : 1);
				ASM(MOV32ri, type_info_reg,
					layout.inverted ? IS_FALSE : IS_TRUE);
				payload.set_modified();
				type_info.set_modified();
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64ri, result_reg, layout.inverted ? 0 : 1);
				result.set_modified();
			}
			ASM(MOV32ri, decision_reg, 0);
		} else {
			ASM(MOV64ri, left_reg, layout.inverted ? 0 : 1);
			ASM(MOV32ri, type_reg, layout.inverted ? IS_FALSE : IS_TRUE);
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				left_reg);
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
		left.reset();
		right.reset();
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
					&& (layout.left.offset > INT32_MAX - 8
						|| layout.right.offset > INT32_MAX - 8
						|| layout.result_offset > INT32_MAX - 8))
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
		auto result_reg = result_value.alloc_gp();
		auto decision_reg = decision.alloc_gp();
		if (!register_layout) {
			ASM(MOV32rm, result_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, result_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
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
		ASM(MOV32ri, decision_reg, 0);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		result_value.reset();
		ASM(MOV32ri, decision_reg, 1);
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
			ASM(MOV32rr, type_reg, type_info.load_to_reg());
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		} else {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.left_offset
							+ offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
			ASM(MOV32rr, type_reg, type_info_reg);
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		} else if (layout.right.literal) {
			if (node.machine_reference_operand_index
						>= node.operands.size()) {
				return branch_to_guarded_cold();
			}
			auto [literal_ref, literal] = val_ref_single(
				node.operands[node.machine_reference_operand_index]);
			auto literal_reg = literal.load_to_reg();
			ASM(MOV32rm, type_reg,
				FE_MEM(literal_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, right_reg,
				FE_MEM(literal_reg, 0, FE_NOREG, 0));
		} else {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.right.offset
							+ offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
		if (!(node.kind == Adaptor::InstKind::GuardedFast
					&& node.mutation_result
					&& mir.mutation_lazy_scalar)) {
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
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				ASM(MOV64rr, result_reg, left_reg);
				result.set_modified();
			} else {
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(layout.result_offset)),
					left_reg);
				ASM(MOV32mi,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							layout.result_offset
								+ offsetof(zval, u1.type_info))),
					IS_LONG);
			}
		}
		if (layout.consume_right) {
			ASM(MOV32mi,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.right.offset
						+ offsetof(zval, u1.type_info))),
				IS_UNDEF);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			ASM(MOV32ri, decision_reg, 0);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		left.reset();
		right.reset();
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
	auto long_incdec = [&]() {
		zend_tpde_long_incdec layout;

		if (!zend_tpde_long_incdec_at(mir, &layout)
				|| layout.operand_offset > INT32_MAX - 8
				|| layout.result_offset > INT32_MAX - 8) {
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
			ASM(MOV64rr, value_reg, operand.load_to_reg());
		} else if (!node.operands.empty()
				&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
				&& adaptor->machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto operand = val_ref(node.operands[0]);
			auto payload = operand.part(0);
			auto type_info = operand.part(1);
			ASM(MOV64rr, value_reg, payload.load_to_reg());
			ASM(MOV32rr, type_reg, type_info.load_to_reg());
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
			ASM(CMP32ri, type_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		} else {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.operand_offset
							+ offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
				if (node.kind == Adaptor::InstKind::GuardedFast) {
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
		if (!(node.kind == Adaptor::InstKind::GuardedFast
					&& node.mutation_result
					&& mir.mutation_lazy_scalar)) {
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.operand_offset)),
				value_reg);
		}
		if (layout.has_result) {
			if (!layout.post) {
				if (node.kind == Adaptor::InstKind::GuardedFast) {
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
			if (node.kind != Adaptor::InstKind::GuardedFast) {
				ASM(MOV32ri, type_reg, IS_LONG);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							layout.result_offset
								+ offsetof(zval, u1.type_info))),
					type_reg);
			}
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			ASM(MOV32ri, decision_reg, 0);
		}
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		type.reset();
		value.reset();
		limit.reset();
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
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				result_reg);
			ASM(MOV32rr, type_reg, result_reg);
			ASM(ADD32ri, type_reg, IS_FALSE);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset + offsetof(zval, u1.type_info))),
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
					auto frame_scratch = std::move(frame).into_scratch();
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
					auto frame_scratch = std::move(frame).into_scratch();
					ScratchReg result_type{this};
					auto type_reg = result_type.alloc_gp();
					ASM(MOV64mr,
						FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
							static_cast<int32_t>(layout.result_offset)),
						result_reg);
					ASM(MOV32rr, type_reg, result_reg);
					ASM(ADD32ri, type_reg, IS_FALSE);
					ASM(MOV32mr,
						FE_MEM(frame_scratch.cur_reg(), 0, FE_NOREG,
							static_cast<int32_t>(layout.result_offset
								+ offsetof(zval, u1.type_info))),
						type_reg);
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
		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.operand_offset
						+ offsetof(zval, u1.type_info))));
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
				ASM(MOV64mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(layout.result_offset)),
					type_reg);
				ASM(MOV32rr, value_reg, type_reg);
				ASM(ADD32ri, value_reg, IS_FALSE);
				ASM(MOV32mr,
					FE_MEM(frame_reg, 0, FE_NOREG,
						static_cast<int32_t>(
							layout.result_offset
								+ offsetof(zval, u1.type_info))),
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

		if (!zend_tpde_object_property_read_at(mir, &layout)
				|| property_reference == nullptr
				|| property_reference->stable_storage_or_layout_id
					!= layout.cache_offset
				|| property_reference->access_width != sizeof(zval)
				|| layout.receiver_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX
				|| layout.cache_offset > INT32_MAX - 3 * sizeof(void *)) {
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.receiver_offset
						+ offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
		ASM(CMP32ri, type_reg, IS_OBJECT);
		generate_raw_jump(Jump::jne, slow);
		ASM(MOV64rm, object_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(layout.receiver_offset)));
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
		ASM(MOV64rm, type_reg,
			FE_MEM(cache_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.cache_offset + sizeof(void *))));
		ASM(CMP64ri, type_reg, ZEND_FIRST_PROPERTY_OFFSET);
		generate_raw_jump(Jump::jl, slow);
		ASM(MOV64rr, property_reg, object_reg);
		ASM(ADD64rr, property_reg, type_reg);
		ASM(MOV32rm, type_reg,
			FE_MEM(property_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, offset_reg, type_reg);
		ASM(AND32ri, offset_reg, Z_TYPE_MASK);
		ASM(CMP32ri, offset_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);
		ASM(CMP32ri, offset_reg, IS_REFERENCE);
		generate_raw_jump(Jump::je, slow);

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32rm, offset_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, offset_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			object.reset();
			cache.reset();
			type.reset();
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				/*
				 * The boxed register result is synthesized for an immediate
				 * scalar assign-op. Specialize the successful edge to long and
				 * let every other PHP value use the existing cold helper.
				 */
				ASM(CMP32ri, offset_reg, IS_LONG);
				generate_raw_jump(Jump::jne, slow);
				auto result = result_ref(node.result);
				auto payload = result.part(0);
				auto type_info = result.part(1);
				auto payload_reg = payload.alloc_reg();
				ASM(MOV64rm, payload_reg,
					FE_MEM(property_reg, 0, FE_NOREG, 0));
				/* The guard already leaves the exact type tag in offset. */
				type_info.set_value(std::move(offset));
				payload.set_modified();
			} else {
				auto [result_ref, result] =
					result_ref_single(node.result);
				auto result_reg = result.alloc_reg();
				switch (adaptor->exact_type(node.result)) {
					case ZEND_MIR_SCALAR_TYPE_I1:
						ASM(CMP32ri, offset_reg, IS_TRUE);
						generate_raw_set(Jump::je, result_reg);
						break;
					case ZEND_MIR_SCALAR_TYPE_I64:
						ASM(MOV64rm, result_reg,
							FE_MEM(property_reg, 0, FE_NOREG, 0));
						break;
					case ZEND_MIR_SCALAR_TYPE_F64:
						ASM(SSE_MOVSDrm, result_reg,
							FE_MEM(property_reg, 0, FE_NOREG, 0));
						break;
					default:
						switch (adaptor->machine_kind(node.result)) {
							case ZEND_TPDE_MACHINE_VALUE_STRING_PTR:
							case ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR:
							case ZEND_TPDE_MACHINE_VALUE_OBJECT_PTR:
							case ZEND_TPDE_MACHINE_VALUE_RESOURCE_PTR:
							case ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR:
								ASM(MOV64rm, result_reg,
									FE_MEM(property_reg, 0, FE_NOREG, 0));
								break;
							default:
								return false;
						}
				}
				result.set_modified();
			}
			offset.reset();
			property.reset();
			decision_reg = decision.alloc_gp();
			ASM(MOV32ri, decision_reg, 0);
			if (adaptor->machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				generate_raw_jump(Jump::jmp, done);
			}
		} else {
			ASM(MOV64rm, offset_reg,
				FE_MEM(property_reg, 0, FE_NOREG, 0));
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				offset_reg);
			ASM(MOV32mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))),
				type_reg);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			generate_raw_jump(Jump::jmp, done);
		}
		ASM(AND32ri, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, copied);
		ASM(MOV64rm, offset_reg,
			FE_MEM(property_reg, 0, FE_NOREG, 0));
		ASM(ADD32mi,
			FE_MEM(offset_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
		label_place(copied);
		generate_raw_jump(Jump::jmp, done);

		label_place(slow);
		object.reset();
		cache.reset();
		offset.reset();
		property.reset();
		type.reset();
		if (node.kind == Adaptor::InstKind::GuardedFast) {
			ASM(MOV32ri, decision_reg, 1);
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

		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					layout.receiver_offset
						+ offsetof(zval, u1.type_info))));
		ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
			ASM(MOV32rm, cache_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))));
			ASM(AND32ri, cache_reg, Z_TYPE_MASK);
			ASM(CMP32ri, cache_reg, IS_LONG);
			generate_raw_jump(Jump::jne, slow);
		}
		label_place(property_metadata_valid);
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

		if (scalar_value) {
			ASM(MOV32ri, type_reg, IS_LONG);
		} else {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))));
			ASM(MOV32rr, offset_reg, type_reg);
			ASM(AND32ri, offset_reg, Z_TYPE_MASK);
			ASM(CMP32ri, offset_reg, IS_REFERENCE);
			generate_raw_jump(Jump::je, slow);
		}

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

		if (scalar_value) {
			auto [value_ref, value] = val_ref_single(node.operands[
				node.property_write_value_operand_index]);
			auto value_reg = value.load_to_reg();
			ASM(MOV64rr, low_word_reg, value_reg);
		} else {
			ASM(MOV64rm, low_word_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.value_offset)));
		}
		if (!scalar_value && !layout.move_value) {
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
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.value_offset
							+ offsetof(zval, u1.type_info))),
				IS_UNDEF);
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
	auto dynamic_fetch_read = [&]() {
		zend_tpde_dynamic_fetch_read layout;

		if (!zend_tpde_dynamic_fetch_read_at(mir, &layout)
				|| layout.name_offset > INT32_MAX
				|| layout.result_offset > INT32_MAX) {
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
		auto slow = text_writer.label_create();
		auto symbol_table = text_writer.label_create();
		auto cv_loop = text_writer.label_create();
		auto loop = text_writer.label_create();
		auto next = text_writer.label_create();
		auto cv_value_ready = text_writer.label_create();
		auto value_ready = text_writer.label_create();
		auto not_indirect = text_writer.label_create();
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
			ASM(MOV64rr, name_reg, source_name_reg);
		} else {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.name_offset
							+ offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
			ASM(CMP32ri, type_reg, IS_STRING);
			generate_raw_jump(Jump::jne, slow);
			ASM(MOV64rm, name_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.name_offset)));
		}

		if (node.kind != Adaptor::InstKind::GuardedFast) {
			ASM(MOV32rm, type_reg,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						layout.result_offset
							+ offsetof(zval, u1.type_info))));
			ASM(TEST32ri, type_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::jne, slow);
		}
		if (direct_cv) {
			ASM(MOV32ri, index_reg, layout.cv_index);
			generate_raw_jump(Jump::jmp, cv_value_ready);
		}

		/* Keep the Darwin and Linux CV-name fast paths structurally equal. */
		ASM(MOV64rm, bucket_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_execute_data, func))));
		ASM(MOV32rm, index_reg,
			FE_MEM(bucket_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_op_array, last_var))));
		ASM(TEST32rr, index_reg, index_reg);
		generate_raw_jump(Jump::je, symbol_table);
		ASM(MOV64rm, bucket_reg,
			FE_MEM(bucket_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_op_array, vars))));
		label_place(cv_loop);
		ASM(SUB32ri, index_reg, 1);
		ASM(MOV64rm, high_word_reg,
			FE_MEM(bucket_reg, 3, index_reg, 0));
		ASM(CMP64rr, high_word_reg, name_reg);
		generate_raw_jump(Jump::je, cv_value_ready);
		ASM(TEST32rr, index_reg, index_reg);
		generate_raw_jump(Jump::jne, cv_loop);

		label_place(symbol_table);
		ASM(MOV32rm, type_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_execute_data, This)
						+ offsetof(zval, u1.type_info))));
		ASM(TEST32ri, type_reg, ZEND_CALL_HAS_SYMBOL_TABLE);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rm, table_reg,
			FE_MEM(frame_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_execute_data, symbol_table))));
		ASM(TEST64rr, table_reg, table_reg);
		generate_raw_jump(Jump::je, slow);

		ASM(MOV64rm, bucket_reg,
			FE_MEM(table_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, arData))));
		ASM(MOV64rm, type_reg,
			FE_MEM(name_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zend_string, h))));
		ASM(MOV32rm, index_reg,
			FE_MEM(table_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(HashTable, nTableMask))));
		ASM(MOV32rr, high_word_reg, type_reg);
		ASM(OR32rr, high_word_reg, index_reg);
		ASM(MOVSXr64r32, high_word_reg, high_word_reg);
		ASM(MOV32rm, index_reg,
			FE_MEM(bucket_reg, 4, high_word_reg, 0));
		label_place(loop);
		ASM(CMP32ri, index_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::je, slow);
		ASM(MOV64rr, slot_reg, index_reg);
		ASM(SHL64ri, slot_reg, 5);
		ASM(ADD64rr, slot_reg, bucket_reg);
		ASM(MOV64rm, high_word_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, h))));
		ASM(CMP64rr, high_word_reg, type_reg);
		generate_raw_jump(Jump::jne, next);
		ASM(MOV64rm, high_word_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(Bucket, key))));
		ASM(CMP64rr, high_word_reg, name_reg);
		generate_raw_jump(Jump::je, value_ready);
		label_place(next);
		ASM(MOV32rm, index_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(Bucket, val) + offsetof(zval, u2.next))));
		generate_raw_jump(Jump::jmp, loop);

		label_place(cv_value_ready);
		ASM(MOV64rr, slot_reg, index_reg);
		ASM(SHL64ri, slot_reg, 4);
		ASM(ADD64rr, slot_reg, frame_reg);
		ASM(ADD64ri, slot_reg,
			static_cast<int32_t>(ZEND_CALL_FRAME_SLOT * sizeof(zval)));
		label_place(value_ready);
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		ASM(MOV32rr, index_reg, type_reg);
		ASM(AND32ri, index_reg, Z_TYPE_MASK);
		ASM(CMP32ri, index_reg, IS_INDIRECT);
		generate_raw_jump(Jump::jne, not_indirect);
		ASM(MOV64rm, slot_reg, FE_MEM(slot_reg, 0, FE_NOREG, 0));
		ASM(MOV32rm, type_reg,
			FE_MEM(slot_reg, 0, FE_NOREG,
				static_cast<int32_t>(offsetof(zval, u1.type_info))));
		label_place(not_indirect);
		ASM(MOV32rr, index_reg, type_reg);
		ASM(AND32ri, index_reg, Z_TYPE_MASK);
		ASM(CMP32ri, index_reg, IS_UNDEF);
		generate_raw_jump(Jump::je, slow);

		if (node.kind == Adaptor::InstKind::GuardedFast) {
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
				ASM(MOV64rr, low_word_reg, payload_reg);
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
			ASM(MOV32ri, decision_reg, 0);
		} else {
			ASM(MOV64rm, low_word_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(MOV64rm, high_word_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 8));
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset)),
				low_word_reg);
			ASM(MOV64mr,
				FE_MEM(frame_reg, 0, FE_NOREG,
					static_cast<int32_t>(layout.result_offset + 8)),
				high_word_reg);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& adaptor->machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			generate_raw_jump(Jump::jmp, done);
		}
		ASM(AND32ri, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		ASM(TEST32rr, type_reg, type_reg);
		generate_raw_jump(Jump::je, done);
		ASM(ADD32mi,
			FE_MEM(low_word_reg, 0, FE_NOREG,
				static_cast<int32_t>(
					offsetof(zend_refcounted_h, refcount))),
			1);
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
				ASM(MOV64rm, pending_reg,
					FE_MEM(context_scratch.cur_reg(), 0, FE_NOREG,
						static_cast<int32_t>(offsetof(
							zend_native_execution_context, vm_interrupt))));
				ASM(TEST64rr, pending_reg, pending_reg);
				generate_raw_jump(Jump::je, done);
				ASM(CMP8mi, FE_MEM(pending_reg, 0, FE_NOREG, 0), 0);
				generate_raw_jump(Jump::jne, slow);
				generate_raw_jump(Jump::jmp, done);
				label_place(slow);
				context_scratch.reset();
				pending.reset();
				if (!emit_materializations(instruction, true)) {
					return false;
				}
				tpde::x64::CCAssignerSysV assigner;
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{node.operands[0]});
				builder.add_arg(ValuePart{mir.source_opline_index, 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
				ValuePart frame_argument{
					tpde::x64::PlatformConfig::GP_BANK, 8};
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
			ASM(MOV64rr, slot_reg,
				check_undefined_cv
					? canonical_frame_register() : frame_scratch.cur_reg());
			ASM(ADD64ri, slot_reg,
				static_cast<int32_t>(layout.operand_offset));
			ASM(MOV32rm, type_reg,
				FE_MEM(slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
			auto spilled = spill_before_branch();
			begin_branch_region();
			auto dereferenced = text_writer.label_create();
			ASM(CMP32ri, type_reg, IS_REFERENCE);
			generate_raw_jump(Jump::jne, dereferenced);
			ASM(MOV64rm, slot_reg,
				FE_MEM(slot_reg, 0, FE_NOREG, 0));
			ASM(ADD64ri, slot_reg,
				static_cast<int32_t>(offsetof(zend_reference, val)));
			ASM(MOV32rm, type_reg,
				FE_MEM(slot_reg, 0, FE_NOREG,
					static_cast<int32_t>(
						offsetof(zval, u1.type_info))));
			ASM(AND32ri, type_reg, Z_TYPE_MASK);
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
			if (node.operands.size() != 1 || !mir.has_value_operation) {
				return false;
			}
			if (record.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
				zend_tpde_array_iterator_reset reset_layout;

				if (zend_tpde_array_iterator_reset_at(mir, &reset_layout)
						&& reset_layout.source_offset <= INT32_MAX
						&& reset_layout.holder_offset <= INT32_MAX - 16) {
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

					ASM(MOV32rm, type_info_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(reset_layout.source_offset
								+ offsetof(zval, u1.type_info))));
					ASM(AND32ri, type_info_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_info_reg, IS_ARRAY);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rm, array_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(reset_layout.source_offset)));
					ASM(MOV32rm, type_info_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, u.type_info))));
					ASM(TEST32ri, type_info_reg, GC_IMMUTABLE);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV32rm, refcount_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, refcount))));
					ASM(ADD32ri, refcount_reg, 1);
					ASM(MOV32mr,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_refcounted_h, refcount))),
						refcount_reg);
					ASM(MOV64rm, high_word_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								reset_layout.source_offset + 8)));
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
					refcount.reset();
					label_place(slow);

					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, std::move(frame_scratch));
					builder.add_arg(std::move(frame_argument),
						tpde::CCAssignment{});
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
					builder.call(runtime_symbol(mir.runtime_helper));
					ValuePart decision{
						tpde::x64::PlatformConfig::GP_BANK, 4};
					builder.add_ret(decision, tpde::CCAssignment{});
					auto decision_reg = decision.cur_reg_or_load(this);
					ASM(CMP32ri, decision_reg,
						ZEND_NATIVE_ITERATOR_EXCEPTION);
					auto valid = text_writer.label_create();
					generate_raw_jump(Jump::jl, valid);
					decision.reset(this);
					if (zend_mir_id_is_valid(mir.exception_block_id)) {
						generate_exception_branch(
							adaptor->block_ref(mir.exception_block_id));
					} else {
						RetBuilder return_builder{*this, *cur_cc_assigner()};
						return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
							tpde::x64::PlatformConfig::GP_BANK},
							tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(valid);
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

				if (zend_tpde_packed_iterator_fetch_at(mir, &layout)
						&& layout.holder_offset <= INT32_MAX
						&& layout.destination_offset <= INT32_MAX) {
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

					ASM(MOV32rm, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset
								+ offsetof(zval, u1.type_info))));
					ASM(AND32ri, type_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_reg, IS_ARRAY);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rm, array_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset)));
					ASM(MOV32rm, type_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(HashTable, u))));
					ASM(TEST32ri, type_reg, HASH_FLAG_PACKED);
					generate_raw_jump(Jump::je, slow);
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
					generate_raw_jump(Jump::jae, end);

					if (!layout.destination_scalar_only) {
						ASM(MOV32rm, type_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(layout.destination_offset
									+ offsetof(zval, u1.type_info))));
						ASM(AND32ri, type_reg, Z_TYPE_MASK);
						ASM(CMP32ri, type_reg, IS_UNDEF);
						generate_raw_jump(Jump::je, destination_valid);
						ASM(CMP32ri, type_reg, IS_LONG);
						generate_raw_jump(Jump::jne, slow);
						label_place(destination_valid);
					}

					ASM(MOV64rm, element_reg,
						FE_MEM(array_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(HashTable, arPacked))));
					ASM(SHL64ri, position_reg, 4);
					ASM(ADD64rr, element_reg, position_reg);
					ASM(SHR64ri, position_reg, 4);
					ASM(MOV32rm, type_reg,
						FE_MEM(element_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zval, u1.type_info))));
					ASM(AND32ri, type_reg, Z_TYPE_MASK);
					ASM(CMP32ri, type_reg, IS_LONG);
					generate_raw_jump(Jump::jne, slow);
					ASM(MOV64rm, value_reg,
						FE_MEM(element_reg, 0, FE_NOREG, 0));

					/* All guards precede the first observable mutation. */
					ASM(ADD32ri, position_reg, 1);
					ASM(MOV32mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.holder_offset
								+ offsetof(zval, u2.fe_pos))),
						position_reg);
					ASM(MOV64mr,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.destination_offset)),
						value_reg);
					ASM(MOV32mi,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(layout.destination_offset
								+ offsetof(zval, u1.type_info))),
						IS_LONG);
					ASM(MOV32mi,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot), 1);
					generate_raw_jump(Jump::jmp, branch);

					label_place(end);
					ASM(MOV32mi,
						FE_MEM(FE_BP, 0, FE_NOREG, decision_slot), 0);
					generate_raw_jump(Jump::jmp, branch);

					label_place(slow);
					type.reset();
					array.reset();
					position.reset();
					limit.reset();
					element.reset();
					value.reset();
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					ValuePart frame_argument{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					frame_argument.set_value(
						this, std::move(frame_scratch));
					builder.add_arg(std::move(frame_argument),
						tpde::CCAssignment{});
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
					builder.call(runtime_symbol(mir.runtime_helper));
					ValuePart decision{
						tpde::x64::PlatformConfig::GP_BANK};
					builder.add_ret(decision, tpde::CCAssignment{});
					auto decision_reg = decision.cur_reg_or_load(this);
					ASM(CMP32ri, decision_reg,
						ZEND_NATIVE_ITERATOR_EXCEPTION);
					auto valid = text_writer.label_create();
					generate_raw_jump(Jump::jl, valid);
					decision.reset(this);
					if (zend_mir_id_is_valid(mir.exception_block_id)) {
						generate_exception_branch(
							adaptor->block_ref(mir.exception_block_id));
					} else {
						RetBuilder return_builder{*this, *cur_cc_assigner()};
						return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
							tpde::x64::PlatformConfig::GP_BANK},
							tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(valid);
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
				zend_tpde_value_condition layout;

				if (zend_tpde_value_condition_at(mir, &layout)
						&& layout.operand_offset <= INT32_MAX) {
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

					ASM(MOV32rm, type_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								layout.operand_offset
									+ offsetof(zval, u1.type_info))));
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
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								layout.operand_offset)));
					ASM(TEST64rr, value_reg, value_reg);
					generate_raw_jump(Jump::jne, truthy);
					generate_raw_jump(Jump::jmp, falsey);

					label_place(not_long);
					ASM(CMP32ri, type_reg, IS_STRING);
					auto not_string = text_writer.label_create();
					generate_raw_jump(Jump::jne, not_string);
					ASM(MOV64rm, value_reg,
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								layout.operand_offset)));
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
							static_cast<int32_t>(
								layout.operand_offset)));
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
						FE_MEM(frame_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								layout.operand_offset)));
					ASM(MOV32rm, type_reg,
						FE_MEM(value_reg, 0, FE_NOREG,
							static_cast<int32_t>(
								offsetof(zend_resource, handle))));
					ASM(TEST32rr, type_reg, type_reg);
					generate_raw_jump(Jump::jne, truthy);
					generate_raw_jump(Jump::jmp, falsey);

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
					type.reset();
					value.reset();
					const auto &successors = adaptor->block_succs(
						IRBlockRef{node.control_block});
					generate_raw_jump(Jump::jmp, branch);
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
					builder.call(runtime_symbol(mir.runtime_helper));
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
					if (zend_mir_id_is_valid(mir.exception_block_id)) {
						generate_exception_branch(
							adaptor->block_ref(mir.exception_block_id));
					} else {
						RetBuilder return_builder{
							*this, *cur_cc_assigner()};
						return_builder.add(ValuePart{
							ZEND_NATIVE_EXCEPTION, 4,
							tpde::x64::PlatformConfig::GP_BANK},
							tpde::CCAssignment{});
						return_builder.ret();
					}
					label_place(valid);
					ASM(MOV32mr,
						FE_MEM(FE_BP, 0, FE_NOREG,
							decision_slot),
						decision_reg);
					decision.reset(this);
					generate_raw_jump(Jump::jmp, branch);
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
			builder.call(runtime_symbol(mir.runtime_helper));
			ValuePart decision{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(decision, tpde::CCAssignment{});
			auto decision_reg = decision.cur_reg_or_load(this);
			const int32_t decision_slot = node.has_result
				? allocate_stack_slot(sizeof(uint32_t)) : -1;
			if (node.has_result && decision_slot < 0) {
				return false;
			}
			if (node.has_result) {
				ASM(MOV32mr,
					FE_MEM(FE_BP, 0, FE_NOREG, decision_slot),
					decision_reg);
			}
			ASM(CMP32ri, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
			auto valid = text_writer.label_create();
			generate_raw_jump(Jump::jl, valid);
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
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
					tpde::x64::CCAssignerSysV body_assigner{false};
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
								? tpde::x64::PlatformConfig::FP_BANK
								: tpde::x64::PlatformConfig::GP_BANK,
							part_desc.bit_width / 8);
						body_builder.add_ret(
							body_results.back(), tpde::CCAssignment{});
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
						ASM(CMP8mi,
							FE_MEM(context_scratch.cur_reg(), 0,
								FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_execution_context,
									observers_enabled))),
							0);
						generate_raw_jump(
							Jump::jne, call_slow_target());
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
							ASM(MOV64rr, computed_reg, left_reg);
							switch (
								node.inlined_checked_source_opcode) {
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
							|| node.operands.size() != argument_count) {
						return false;
					}
					tpde::x64::CCAssignerSysV body_assigner{false};
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
								? tpde::x64::PlatformConfig::FP_BANK
								: tpde::x64::PlatformConfig::GP_BANK,
							part_desc.bit_width / 8);
						body_builder.add_ret(
							body_results.back(), tpde::CCAssignment{});
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
								ASM(MOV64rr, first_reg,
									left.load_to_reg());
								switch (
									node.inlined_checked_source_opcode) {
									case ZEND_ADD:
										ASM(ADD64rr, first_reg,
											right.load_to_reg());
										break;
									case ZEND_SUB:
										ASM(SUB64rr, first_reg,
											right.load_to_reg());
										break;
									default:
										return false;
								}
								generate_raw_jump(Jump::jo, call_slow_target());
							} else {
								auto [inline_ref, inline_value] =
									val_ref_single(node.operands[
										node.inlined_operand_index]);
								ASM(MOV64rr, first_reg,
									inline_value.load_to_reg());
							}
							if (!node.has_result
									|| node.continuation_block == UINT32_MAX) {
								return false;
							}
							auto [inline_result_ref, inline_result] =
								result_ref_single(node.result);
							ASM(MOV64rr,
								inline_result.alloc_reg(), first_reg);
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
							generate_raw_jump(Jump::jbe, call_slow_target());
							label_place(stack_guarded);
						}
						ASM(CMP8mi,
							FE_MEM(context_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_execution_context,
									observers_enabled))),
							0);
						generate_raw_jump(Jump::jne, call_slow_target());
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
									ScratchReg type_info{this};
									auto type_info_reg = type_info.alloc_gp();
									ASM(MOV32rr, type_info_reg,
										high_word.cur_reg());
									ASM(AND32ri, type_info_reg,
										IS_TYPE_REFCOUNTED
											<< Z_TYPE_FLAGS_SHIFT);
									ASM(TEST32rr, type_info_reg,
										type_info_reg);
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
								ScratchReg slot{this};
								auto slot_reg = slot.alloc_gp();
								ASM(MOV64rm, slot_reg,
									FE_MEM(frame_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_execute_data, func))));
								ASM(MOV32rm, slot_reg,
									FE_MEM(slot_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zend_op_array, last_var))));
								ASM(ADD64ri, slot_reg,
									static_cast<int32_t>(
										ZEND_CALL_FRAME_SLOT
											+ call.direct_call
												->result_operand.index));
								ASM(SHL64ri, slot_reg, 4);
								ASM(ADD64rr, second_reg, slot_reg);
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
						frame_scratch.reset();
						context_scratch.reset();
						cell_scratch.reset();
						descriptor_scratch.reset();
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
					ScratchReg published_code{this};
					auto first_reg = first.alloc_gp();
					auto second_reg = second.alloc_gp();
					auto published_code_reg = published_code.alloc_gp();
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

					if (local_component_call) {
						ASM(MOV64ri, published_code_reg, 0);
					} else {
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
					ASM(CMP8mi,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								observers_enabled))),
						0);
					generate_raw_jump(Jump::jne, call_slow_target());
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
						ASM(MOV64rr, first_reg, cache_reg);
						ASM(AND64ri, first_reg, 1);
						ASM(TEST64rr, first_reg, first_reg);
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
						ASM(MOV32rm, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This)
										+ offsetof(zval, u1.type_info))));
						ASM(AND32ri, first_reg, Z_TYPE_MASK);
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
						ASM(MOV32rm, second_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(
									offsetof(zend_execute_data, This)
										+ offsetof(zval, u1.type_info))));
						ASM(AND32ri, second_reg, Z_TYPE_MASK);
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
						ASM(MOV32rm, first_reg,
							FE_MEM(frame_reg, 0, FE_NOREG,
								receiver_offset + static_cast<int32_t>(
									offsetof(zval, u1.type_info))));
						ASM(AND32ri, first_reg, Z_TYPE_MASK);
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
						if (zend_mir_scalar_type_is_exact(
								argument.exact_type)
								|| argument.source_operand.kind
									== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
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
						}
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
						generate_raw_jump(Jump::jbe, call_slow_target());
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
					generate_raw_jump(Jump::jb, call_slow_target());

					auto callee_reg = fast_callee_argument_register.cur_reg();
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
							ASM(MOV32rm, first_reg,
								FE_MEM(frame_reg, 0, FE_NOREG,
									static_cast<int32_t>(
										offsetof(zend_execute_data, This)
											+ offsetof(zval, u1.type_info))));
							ASM(AND32ri, first_reg, Z_TYPE_MASK);
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
						ASM(ADD64ri, second_reg, static_cast<int32_t>(
							call.direct_call->frame_size + offsetof(
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
					/*
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
									== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
								ScratchReg source_address{this};
								ScratchReg low_word{this};
								ScratchReg high_word{this};
								ScratchReg type_info{this};
								auto source_address_reg = source_address.alloc_gp();
								auto low_word_reg = low_word.alloc_gp();
								auto high_word_reg = high_word.alloc_gp();
								auto type_info_reg = type_info.alloc_gp();
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
								ASM(MOV32rm, type_info_reg,
									FE_MEM(source_address_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zval, u1.type_info))));
								ASM(AND32ri, type_info_reg,
									IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
								ASM(TEST32rr, type_info_reg, type_info_reg);
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
									ScratchReg type_info{this};
									auto type_info_reg = type_info.alloc_gp();
									ASM(MOV32rr, type_info_reg,
										high_word.cur_reg());
									ASM(AND32ri, type_info_reg,
										IS_TYPE_REFCOUNTED
											<< Z_TYPE_FLAGS_SHIFT);
									ASM(TEST32rr, type_info_reg,
										type_info_reg);
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
							} else {
								auto source_frame_reg = argument.load_to_reg();
							if (descriptor_argument.source_frame_offset
									> INT32_MAX) {
								return false;
							}
							const int32_t source_offset =
								static_cast<int32_t>(
									descriptor_argument.source_frame_offset);
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
							if (copy_argument) {
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
							} else {
								ASM(MOV32mi,
									FE_MEM(source_address_reg, 0, FE_NOREG,
										static_cast<int32_t>(offsetof(
											zval, u1.type_info))),
									IS_UNDEF);
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
						const int32_t offset = static_cast<int32_t>(
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
					for (uint32_t index = fixed_argument_count;
							index < compiled_variable_count; ++index) {
						const int32_t offset = static_cast<int32_t>(
							(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
						ASM(MOV64mi,
							FE_MEM(callee_reg, 0, FE_NOREG, offset), 0);
						ASM(MOV64mi,
							FE_MEM(callee_reg, 0, FE_NOREG, offset + 8), 0);
					}

					/* Complete and link the stable trailing activation. */
					ScratchReg metadata_first{this};
					ScratchReg metadata_second{this};
					auto metadata_first_reg = metadata_first.alloc_gp();
					auto metadata_second_reg = metadata_second.alloc_gp();
					ASM(MOV64rr, metadata_second_reg, callee_reg);
					ASM(ADD64ri, metadata_second_reg,
						static_cast<int32_t>(call.direct_call->frame_size));
					ASM(MOV64mr,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, caller))),
						frame_reg);
					ASM(MOV64mr,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, callee))),
						callee_reg);
					if (local_component_call) {
						ASM(MOV64mi,
							FE_MEM(metadata_second_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_direct_activation, cell))),
							0);
					} else {
						ASM(MOV64mr,
							FE_MEM(metadata_second_reg, 0, FE_NOREG,
								static_cast<int32_t>(offsetof(
									zend_native_direct_activation, cell))),
							cell_reg);
					}
					ASM(MOV64mr,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, code))),
						published_code_reg);
					ASM(MOV64mr,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, descriptor))),
						descriptor_reg);
					ASM(MOV64rm, metadata_first_reg,
						FE_MEM(context_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_execution_context,
								active_direct_call))));
					ASM(MOV64rm, descriptor_reg,
						FE_MEM(metadata_first_reg, 0, FE_NOREG, 0));
					ASM(MOV64mr,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, previous))),
						descriptor_reg);
					ASM(MOV64mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								discarded_return))),
						0);
					ASM(MOV64mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								discarded_return) + 8)),
						0);
					ASM(MOV32mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								discarded_return)
								+ offsetof(zval, u1.type_info))),
						IS_UNDEF);
					ASM(MOV64mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, status))),
						0);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								uses_discarded_return))),
						result_unused ? 1 : 0);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								raw_arguments_owned))),
						0);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								frame_initialized))),
						1);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								frame_requires_finish))),
						1);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								cell_active))),
						generation_leased ? 0 : 1);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								dynamic_target))),
						0);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								internal_target))),
						0);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								generator_created))),
						0);
					ASM(MOV8mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								setup_record))),
						0);
					ASM(MOV64mi,
						FE_MEM(metadata_second_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation,
								pending_call))),
						0);
					ASM(MOV64mr,
						FE_MEM(metadata_first_reg, 0, FE_NOREG, 0),
						metadata_second_reg);
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
							receive_builder.add_arg(ValuePart{
								ZEND_RECV_VARIADIC, 4,
								tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								fixed_argument_count + 1, 4,
								tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								UINT64_C(0), 8, tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								UINT64_C(0), 4, tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								static_cast<uint64_t>(ZEND_MIR_SOURCE_OPERAND_SLOT)
									| (static_cast<uint64_t>(fixed_argument_count) << 16),
								8, tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
							receive_builder.add_arg(ValuePart{
								fixed_argument_count, 4,
								tpde::x64::PlatformConfig::GP_BANK},
								tpde::CCAssignment{});
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
						published_code_reg = published_code.alloc_gp();
						if (local_component_call) {
							ASM(MOV64ri, published_code_reg, 0);
						} else {
							auto receive_cell = image_symbol_value(
								ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
								call.call_site.target_id);
							auto receive_cell_scratch =
								std::move(receive_cell).into_scratch(this);
							ASM(MOV64rm, published_code_reg,
								FE_MEM(receive_cell_scratch.cur_reg(), 0, FE_NOREG,
									static_cast<int32_t>(offsetof(
										zend_native_entry_cell, code))));
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
					ASM(MOV64rm, probe_reg,
						FE_MEM(post_callee_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_execute_data, return_value))));
					if (result_unused) {
						ASM(CMP32mi, FE_MEM(probe_reg, 0, FE_NOREG, 8),
							IS_DOUBLE);
						generate_raw_jump(Jump::ja, complete_fast);
					} else if ((call.direct_call->flags
								& ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT)
								== 0
							|| call.direct_call->result_type
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
							call.call_site.target_id);
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
					ASM(MOV64mr,
						FE_MEM(probe_reg, 0, FE_NOREG, 0), post_callee_reg);
					generate_raw_jump(Jump::jmp, successful);

					/* Rare completion retains full exception/interrupt cleanup. */
					label_place(complete_fast);
					ASM(MOV32rm, activation_reg,
						FE_MEM(activation_reg, 0, FE_NOREG,
							static_cast<int32_t>(offsetof(
								zend_native_direct_activation, status))));
					ValuePart finish_status_argument{
						tpde::x64::PlatformConfig::GP_BANK, 4};
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
					tpde::x64::CCAssignerSysV finish_assigner{false};
					CallBuilder finish_builder{*this, finish_assigner};
					finish_builder.add_arg(
						CallArg{node.operands[frame_operand + 3]});
					finish_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
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
						canonical_frame_register()),
						tpde::CCAssignment{});
					if (!split_cold) {
						auto frame_liveness =
							val_ref(node.operands[
								frame_operand + slow_enter_frame_use]);
						(void) frame_liveness;
					}
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
						call.call_site.target_id), tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
					if (split_cold) {
						builder.add_arg(CallArg{
							node.operands[context_operand]});
					} else {
						builder.add_arg(copy_fixed_argument(
							canonical_value_register(IRValueRef{
								Adaptor::EXECUTION_CONTEXT_ARGUMENT})),
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
				ScratchReg entry_copy{this};
				auto entry_copy_reg =
					entry_copy.alloc_specific(tpde::x64::AsmReg::R11);
				mov(entry_copy_reg, entry.cur_reg_or_load(this), sizeof(void *));
				entry.reset(this);
				ValuePart entry_target{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				entry_target.set_value(this, std::move(entry_copy));
				ValuePart entry_status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(std::move(callee), tpde::CCAssignment{});
					if (split_cold) {
						builder.add_arg(CallArg{
							node.operands[context_operand + 1]});
					} else {
						builder.add_arg(copy_fixed_argument(
							canonical_value_register(IRValueRef{
								Adaptor::EXECUTION_CONTEXT_ARGUMENT})),
							tpde::CCAssignment{});
						auto context_liveness =
							val_ref(node.operands[
								context_operand + slow_entry_context_use]);
						(void) context_liveness;
					}
					builder.call(std::move(entry_target));
					builder.add_ret(entry_status, tpde::CCAssignment{});
				}
				ScratchReg entry_status_copy{this};
				auto entry_status_copy_reg =
					entry_status_copy.alloc_specific(tpde::x64::AsmReg::CX);
				mov(entry_status_copy_reg,
					entry_status.cur_reg_or_load(this),
					sizeof(zend_native_status));
				entry_status.reset(this);
				ValuePart entry_status_argument{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				entry_status_argument.set_value(
					this, std::move(entry_status_copy));
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(copy_fixed_argument(
					canonical_frame_register()),
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
					call.id), tpde::CCAssignment{});
				if (split_cold) {
					builder.add_arg(CallArg{
						node.operands[context_operand + 2]});
				} else {
					builder.add_arg(copy_fixed_argument(
						canonical_value_register(IRValueRef{
							Adaptor::EXECUTION_CONTEXT_ARGUMENT})),
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
									+ call.direct_call
										->result_operand.index));
						ASM(SHL64ri, slot_index_reg, 4);
						ASM(ADD64rr, result_slot_reg, slot_index_reg);
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
					auto [result_ref, result] =
						result_ref_single(node.result);
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
						call.call_site.target_id), tpde::CCAssignment{});
					enter_builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
					enter_builder.add_arg(CallArg{
						node.operands[context_operand]});
					enter_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_USER_CALL_RESOLVE));
					enter_builder.add_ret(callee, tpde::CCAssignment{});
					enter_builder.add_ret(entry, tpde::CCAssignment{});
				}
				ScratchReg entry_copy{this};
				auto entry_copy_reg =
					entry_copy.alloc_specific(tpde::x64::AsmReg::R11);
				mov(entry_copy_reg, entry.cur_reg_or_load(this), sizeof(void *));
				entry.reset(this);
				ValuePart entry_target{
					tpde::x64::PlatformConfig::GP_BANK, 8};
				entry_target.set_value(this, std::move(entry_copy));
				ValuePart entry_status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder entry_builder{*this, assigner};
					entry_builder.add_arg(
						std::move(callee), tpde::CCAssignment{});
					entry_builder.add_arg(CallArg{
						node.operands[context_operand + 1]});
					entry_builder.call(std::move(entry_target));
					entry_builder.add_ret(
						entry_status, tpde::CCAssignment{});
				}
				ScratchReg entry_status_copy{this};
				auto entry_status_copy_reg =
					entry_status_copy.alloc_specific(tpde::x64::AsmReg::CX);
				mov(entry_status_copy_reg,
					entry_status.cur_reg_or_load(this),
					sizeof(zend_native_status));
				entry_status.reset(this);
				ValuePart entry_status_argument{
					tpde::x64::PlatformConfig::GP_BANK, 4};
				entry_status_argument.set_value(
					this, std::move(entry_status_copy));
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder leave_builder{*this, assigner};
				leave_builder.add_arg(
					CallArg{node.operands[frame_operand + 1]});
				leave_builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
					call.id), tpde::CCAssignment{});
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
				return_builder.add(
					std::move(status), tpde::CCAssignment{});
				return_builder.ret();
				label_place(continued);
				if (node.has_result) {
					auto [result_ref, result] =
						result_ref_single(node.result);
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
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(copy_fixed_argument(canonical_frame_register()),
				tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_ENTRY_CELL,
				call.call_site.target_id), tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR,
				call.id), tpde::CCAssignment{});
			for (IRValueRef operand : node.operands) {
				auto liveness = val_ref(operand);
				(void) liveness;
			}
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_CALL_CONVERT_EXPLICIT));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
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
						zend_tpde_encode_value_operand(call.call_site.result_operand), 8,
						tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				result_builder.add_arg(ValuePart{
					static_cast<uint32_t>(adaptor->exact_type(node.result)), 4,
					tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
				result_builder.call(runtime_symbol(ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
				ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
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
			if (node.direct_internal_argument_transport) {
				const uint32_t argument_count =
					call.call_argument_count;
				const uint32_t frame_base = argument_count;
				{
					tpde::x64::CCAssignerSysV assigner{false};
					CallBuilder builder{*this, assigner};
					builder.add_arg(
						CallArg{node.operands[frame_base]});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
						call.call_site.target_id), tpde::CCAssignment{});
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
					builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN));
				}
				for (uint32_t index = 0;
						index < argument_count; ++index) {
					const IRValueRef operand = node.operands[index];
					tpde::x64::CCAssignerSysV assigner{false};
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
						if (offset > static_cast<uint64_t>(INT32_MAX)
								- offsetof(zval, u1.type_info)) {
							return false;
						}
						auto boxed = val_ref(operand);
						auto payload = boxed.part(0);
						auto type_info = boxed.part(1);
						auto [frame_ref, frame] = val_ref_single(frame_operand);
						auto frame_reg = frame.load_to_reg();
						ASM(MOV64mr,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(offset)),
							payload.load_to_reg());
						ASM(MOV32mr,
							FE_MEM(frame_reg, 0, FE_NOREG,
								static_cast<int32_t>(offset
									+ offsetof(zval, u1.type_info))),
							type_info.load_to_reg());
					}
					builder.add_arg(CallArg{frame_operand});
					if (operand == IRValueRef{Adaptor::FRAME_VALUE}) {
						auto source_liveness = val_ref(operand);
						(void) source_liveness;
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id), tpde::CCAssignment{});
						builder.add_arg(ValuePart{index, 4,
							tpde::x64::PlatformConfig::GP_BANK},
							tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
						continue;
					}
					if (adaptor->machine_kind(operand)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
						builder.add_arg(image_symbol_value(
							ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
							call.id), tpde::CCAssignment{});
						builder.add_arg(ValuePart{index, 4,
							tpde::x64::PlatformConfig::GP_BANK},
							tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
						continue;
					}
					builder.add_arg(image_symbol_value(
						ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
						call.id), tpde::CCAssignment{});
					builder.add_arg(ValuePart{index, 4,
						tpde::x64::PlatformConfig::GP_BANK},
						tpde::CCAssignment{});
					builder.add_arg(CallArg{operand});
					if (adaptor->exact_type(operand)
							== ZEND_MIR_SCALAR_TYPE_F64) {
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT));
					} else {
						builder.add_arg(ValuePart{
							static_cast<uint32_t>(
								adaptor->exact_type(operand)),
							4, tpde::x64::PlatformConfig::GP_BANK},
							tpde::CCAssignment{});
						builder.call(runtime_symbol(
							ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_INTEGER_ARGUMENT));
					}
				}
				tpde::x64::CCAssignerSysV assigner{false};
				CallBuilder builder{*this, assigner};
				builder.add_arg(CallArg{
					node.operands[frame_base + 1 + argument_count]});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
					call.call_site.target_id), tpde::CCAssignment{});
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					call.id), tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE));
				ValuePart status{
					tpde::x64::PlatformConfig::GP_BANK, 4};
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
				return_builder.add(
					std::move(status), tpde::CCAssignment{});
				return_builder.ret();
				label_place(continued);
				if (node.has_result) {
					tpde::x64::CCAssignerSysV result_assigner{false};
					CallBuilder result_builder{*this, result_assigner};
					result_builder.add_arg(CallArg{
						node.operands[frame_base + 2 + argument_count]});
					result_builder.add_arg(ValuePart{
						zend_tpde_encode_value_operand(
							call.call_site.result_operand),
						8, tpde::x64::PlatformConfig::GP_BANK},
						tpde::CCAssignment{});
					result_builder.add_arg(ValuePart{
						static_cast<uint32_t>(
							adaptor->exact_type(node.result)),
						4, tpde::x64::PlatformConfig::GP_BANK},
						tpde::CCAssignment{});
					result_builder.call(runtime_symbol(
						ZEND_NATIVE_HELPER_CALL_READ_SOURCE_SCALAR));
					ValuePart payload{
						tpde::x64::PlatformConfig::GP_BANK, 8};
					result_builder.add_ret(
						payload, tpde::CCAssignment{});
					auto [result_ref, result] =
						result_ref_single(node.result);
					if (val_parts(node.result).bank
							== tpde::x64::PlatformConfig::FP_BANK) {
						auto payload_reg =
							payload.cur_reg_or_load(this);
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
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{IRValueRef{Adaptor::FRAME_VALUE}});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_INTERNAL_CALL_CELL,
				call.call_site.target_id), tpde::CCAssignment{});
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
				call.id), tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 8};
			ValuePart payload{tpde::x64::PlatformConfig::GP_BANK, 8};
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
		case ZEND_MIR_OPCODE_FINALLY_ENTER: {
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[0]});
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_ENTER));
			ValuePart status{tpde::x64::PlatformConfig::GP_BANK, 4};
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
			auto frame_scratch = std::move(frame).into_scratch();
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
			auto frame_scratch = std::move(frame).into_scratch();
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
					ASM(CMP32ri, direct_continuation_reg,
						call.source_position_id);
					auto continued = text_writer.label_create();
					generate_raw_jump(Jump::jne, continued);
					generate_exception_branch(adaptor->block_ref(target));
					label_place(continued);
				}
			}
			direct_continuation.reset();
			{
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_EXCEPTION, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
			}
			label_place(slow_exception);
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(CallArg{node.operands[1]});
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
			builder.call(runtime_symbol(ZEND_NATIVE_HELPER_FINALLY_RETURN));
			ValuePart continuation{tpde::x64::PlatformConfig::GP_BANK, 4};
			builder.add_ret(continuation, tpde::CCAssignment{});
			auto continuation_reg = continuation.cur_reg_or_load(this);
			auto generator_returned = text_writer.label_create();
			ASM(CMP32ri, continuation_reg,
				ZEND_NATIVE_FINALLY_GENERATOR_RETURNED);
			generate_raw_jump(Jump::je, generator_returned);
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
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
			for (uint32_t i = 0; i < adaptor->plan()->instruction_count; ++i) {
				const zend_mir_instruction_record handler =
					zend_tpde_instruction_record_at(
						adaptor->plan(), &adaptor->plan()->instructions[i]);
				if ((handler.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
						&& handler.opcode != ZEND_MIR_OPCODE_FINALLY_ENTER)
						|| handler.block_id
							== adaptor->plan()->function.entry_block_id
						|| !zend_mir_id_is_valid(
							handler.source_position_id)) {
					continue;
				}
				const IRBlockRef handler_block =
					adaptor->block_ref(handler.block_id);
				if (static_cast<uint32_t>(
						this->analyzer.block_idx(handler_block))
						>= this->block_labels.size()) {
					continue;
				}
				ASM(CMP32ri, status_reg,
					ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
						| handler.source_position_id);
				auto continued = text_writer.label_create();
				generate_raw_jump(Jump::jne, continued);
				generate_exception_branch(handler_block);
				label_place(continued);
			}
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), tpde::CCAssignment{});
			return_builder.ret();
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
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(ValuePart{ZEND_NATIVE_RETURNED, 4,
				tpde::x64::PlatformConfig::GP_BANK}, tpde::CCAssignment{});
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
				RetBuilder return_builder{*this, *cur_cc_assigner()};
				return_builder.add(ValuePart{ZEND_NATIVE_RETURNED, 4,
					tpde::x64::PlatformConfig::GP_BANK},
					tpde::CCAssignment{});
				return_builder.ret();
				return true;
			}
			if (node.operands.empty()
					|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}) {
				return false;
			}
			tpde::x64::CCAssignerSysV assigner{false};
			CallBuilder builder{*this, assigner};
			builder.add_arg(
				copy_fixed_argument(canonical_frame_register()),
				tpde::CCAssignment{});
			{
				auto frame_liveness = val_ref(node.operands[0]);
				(void) frame_liveness;
			}
			builder.add_arg(ValuePart{record.source_position_id, 4,
				tpde::x64::PlatformConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{
				zend_tpde_encode_value_operand(mir.value_operation.op1), 8,
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
	if (object.empty()
			|| elf_has_writable_executable_section(object)
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
