// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_compiler_common.hpp"
#include "Zend/Native/TPDE/Common/zend_tpde_ir_adaptor.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_apple_a64_abi.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_encodegen_a64.hpp"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_object_handlers.h"
#include "Zend/zend_observer.h"

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

/* The arm64 backend does no profile-guided layout. */
extern "C" bool zend_tpde_layout_training(void)
{
	return false;
}

extern "C" uint64_t zend_tpde_layout_training_session(void)
{
	return 0;
}

uint32_t zend_tpde_layout_rare_function(uint32_t, const zend_tpde_plan *)
{
	return 0;
}

extern "C" void zend_tpde_layout_training_flush(void)
{
}

extern "C" bool zend_tpde_layout_switch(void)
{
	return false;
}

extern "C" bool zend_tpde_layout_take_restart(void)
{
	return false;
}

extern "C" void zend_tpde_layout_automatic(void)
{
}
#endif

namespace {

using Adaptor = zend::native::tpde::ZendComponentIRAdaptor;
using IRValueRef = zend::native::tpde::IRValueRef;
using IRInstRef = zend::native::tpde::IRInstRef;
using IRBlockRef = zend::native::tpde::IRBlockRef;
using IRFuncRef = zend::native::tpde::IRFuncRef;
using DarwinConfig = zend::native::tpde::DarwinA64PlatformConfig;
using zend::native::tpde::ZendCompilerCommon;
using zend::native::tpde::frame_slot_offset;

class ZendCompilerA64 final
	: public ::tpde::a64::CompilerA64<Adaptor, ZendCompilerA64,
		ZendCompilerCommon, DarwinConfig>,
	  public tpde_encodegen::EncodeCompiler<Adaptor, ZendCompilerA64,
		ZendCompilerCommon, DarwinConfig> {
	using Base = ::tpde::a64::CompilerA64<Adaptor, ZendCompilerA64,
		ZendCompilerCommon, DarwinConfig>;
	using EncodeBase = tpde_encodegen::EncodeCompiler<Adaptor, ZendCompilerA64,
		ZendCompilerCommon, DarwinConfig>;
	friend ZendCompilerCommon<Adaptor, ZendCompilerA64, DarwinConfig>;

public:
	using AsmReg = typename Base::AsmReg;
	using GenericValuePart = typename Base::GenericValuePart;
	using ScratchReg = typename Base::ScratchReg;
	using ValuePart = typename Base::ValuePart;
	using ValuePartRef = typename Base::ValuePartRef;

private:
	zend_native_image *image_;
	/* A source operand in the runtime helpers' encoding. */
	uint64_t encode_source_operand(const zend_mir_source_operand_ref &operand,
			uint32_t unused_payload = ZEND_MIR_ID_INVALID) const {
		return zend_tpde_encode_value_operand(operand,
			ir().plan()->source_frame_variable_count, unused_payload);
	}
	/* An operand of a value-operation helper; only helpers with unused
	 * operand payloads receive the unused payload. */
	uint64_t encode_helper_operand(bool explicit_object_operands,
			const zend_mir_source_operand_ref &operand,
			uint32_t unused_payload) const {
		return explicit_object_operands
			? encode_source_operand(operand, unused_payload)
			: encode_source_operand(operand);
	}
	std::vector<::tpde::SymRef> image_symbols_;
	std::vector<::tpde::SymRef> image_slots_;
	std::vector<::tpde::Label> generator_resume_labels_;
	std::vector<::tpde::Label> user_opcode_labels_;
	std::vector<::tpde::Label> user_opcode_dispatch_labels_;
	std::vector<::tpde::Label> user_opcode_result_reload_labels_;
	std::optional<::tpde::Label> catch_dispatch_label_;
	uint32_t current_continuation_block_ = UINT32_MAX;
	bool continuation_edge_emitted_ = false;
	/*
	 * The instruction compile_inst_impl() compiles. The members compiling
	 * its parts read it through inst_context_, set for that call only.
	 */
	struct InstContext {
		IRInstRef instruction;
		InstRange remaining_instructions;
		const Adaptor::InstNode &node;
		const zend_tpde_instruction *mir = nullptr;
		const zend_mir_instruction_record *record = nullptr;
	};
	InstContext *inst_context_ = nullptr;
	/* The phase compile_user_call() compiles, for its phase members. */
	struct UserCallContext {
		const zend_tpde_source_call_phase_entry *phase;
		ValueRef &context_liveness;
		const zend_tpde_instruction *call = nullptr;
		/* The frame setup layout of the INIT phase. */
		const zend_tpde_user_call_setup *setup = nullptr;
	};
	UserCallContext *user_call_context_ = nullptr;
	/* The dispatch compile_user_opcode_gateway() emits, for the members
	 * emitting its dispatch cases. */
	struct UserOpcodeGatewayContext {
		const zend_tpde_plan *plan;
		std::span<const uint32_t> next_landings;
		::tpde::Label exception;
	};
	UserOpcodeGatewayContext *user_opcode_gateway_context_ = nullptr;
	/*
	 * The direct user call compile_call_direct_user() compiles, for the
	 * members compiling its paths.
	 */
	struct DirectUserCallContext {
		const zend_tpde_instruction &call;
		const DirectUserCallShape &shape;
		bool local_component_call;
		bool generated_fast_path;
		bool private_inline_body;
		uint32_t typed_body_function;
		bool result_unused;
		bool generation_leased;
		uint32_t argument_count;
		::tpde::Label slow_path;
		::tpde::Label successful;
		/* The leaf path's private frame and saved caller frame slots. */
		int32_t leaf_private_frame_slot = 0;
		int32_t leaf_caller_frame_slot = 0;
	};
	DirectUserCallContext *direct_user_call_context_ = nullptr;
	/*
	 * The frame reservation and the registers the frame path of a direct
	 * user call holds across its steps.
	 */
	struct DirectUserFrameContext {
		uint64_t reservation_size;
		AsmReg frame_reg;
		AsmReg context_reg;
		AsmReg cell_reg;
		AsmReg descriptor_reg;
		AsmReg first_reg;
		AsmReg second_reg;
		AsmReg published_code_reg;
		std::optional<ScratchReg> &run_time_cache;
		AsmReg callee_reg = AsmReg::make_invalid();
	};
	DirectUserFrameContext *direct_user_frame_context_ = nullptr;
	/* Points a context member at a context for its own scope. */
	template <typename Context>
	struct ContextScope {
		Context *&current;
		Context *outer;
		ContextScope(Context *&slot, Context *context)
			: current{slot}, outer{slot} {
			slot = context;
		}
		~ContextScope() { current = outer; }
	};

	/* The jumps of ZendCompilerCommon. */
	static constexpr auto JUMP_ALWAYS = Jump::jmp;
	static constexpr auto JUMP_EQUAL = Jump::Jeq;
	static constexpr auto JUMP_NOT_EQUAL = Jump::Jne;
	/* Memory operand offsets are unsigned 32-bit. */
	static constexpr uint64_t DISPLACEMENT_LIMIT = UINT32_MAX;
	void emit_decision_test(AsmReg reg) {
		ASM(CMPwi, reg, 0);
	}

	/*
	 * A direct call stack guard (ZendCompilerCommon): SUB with a 12-bit
	 * immediate, located by the label before it.
	 */
	void emit_direct_call_stack_guard_position(
			AsmReg destination, uint32_t target_function) {
		const auto patch_label = text_writer.label_create();
		label_place(patch_label);
		ASM(SUBxi, destination, AsmReg{AsmReg::SP}, 0);
		add_direct_call_stack_guard(patch_label, target_function, destination);
	}

	void patch_direct_call_stack_guard(
			uint32_t offset, AsmReg destination, uint32_t frame_size) {
		ZEND_ASSERT(frame_size <= UINT32_C(4095)
			|| (frame_size < UINT32_C(16) * 1024 * 1024
				&& frame_size % UINT32_C(4096) == 0));
		auto *instruction = reinterpret_cast<uint32_t *>(
			text_writer.begin_ptr() + offset);
		*instruction = de64_SUBxi(
			destination, AsmReg{AsmReg::SP}, frame_size);
	}

	TargetBranchState generator_gateway_state_;

public:
	static constexpr uint32_t NUM_FIXED_ASSIGNMENTS[
		DarwinConfig::NUM_BANKS] = {10, 5};

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
		/* Emitters call only runtime helpers planning required. */
		const zend_native_image_symbol *symbol =
			kind == ZEND_NATIVE_IMAGE_SYMBOL_RUNTIME_HELPER
				? zend_tpde_required_runtime_symbol(image_,
					static_cast<zend_native_runtime_helper_id>(id))
				: zend_tpde_image_symbol_find(
					image_, kind, id, adaptor->current_function_index());
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

	/* destination = base + offset with the unshifted 12-bit immediate form
	 * of ADD, or through a materialized constant above 4095. */
	void add_imm12_offset(AsmReg destination, AsmReg base, uint64_t offset) {
		if (offset <= UINT32_C(4095)) {
			ASM(ADDxi, destination, base, static_cast<uint32_t>(offset));
		} else {
			ScratchReg amount{this};
			auto amount_reg = amount.alloc_gp();
			materialize_constant(offset, DarwinConfig::GP_BANK, 8, amount_reg);
			ASM(ADDx, destination, base, amount_reg);
		}
	}

	/* Stands for the canonical frame register, looked up again at each use:
	 * a scratch allocation in between may have evicted it. */
	struct CanonicalFrame {};
	AsmReg frame_register(AsmReg frame) const { return frame; }
	AsmReg frame_register(CanonicalFrame) { return canonical_frame_register(); }
	/* EX(opline) = the opline byte_offset bytes into the opcodes of the
	 * function frame executes, built in scratch. The offset is added with
	 * add_imm12_offset(), or with add_offset() for shifted_immediate. */
	template <typename Frame>
	void emit_publish_opline(Frame frame, AsmReg scratch,
			uint64_t byte_offset, bool shifted_immediate = false) {
		load_off(scratch, frame_register(frame),
			static_cast<uint32_t>(offsetof(zend_execute_data, func)), 8);
		load_off(scratch, scratch,
			static_cast<uint32_t>(offsetof(zend_op_array, opcodes)), 8);
		if (shifted_immediate) {
			add_offset(scratch, scratch, byte_offset);
		} else {
			add_imm12_offset(scratch, scratch, byte_offset);
		}
		store_off(frame_register(frame),
			static_cast<uint32_t>(offsetof(zend_execute_data, opline)),
			scratch, 8);
	}
	/*
	 * Calls the typed body function of component member target with the
	 * node's first argument_count operands and defines the node's result from
	 * the returned parts. With validate, an invalid return type or a result
	 * of another part count fails; without, neither is checked.
	 */
	bool emit_typed_body_call(const Adaptor::InstNode &node,
			uint32_t function, uint32_t target, uint32_t argument_count,
			bool validate) {
		zend::native::tpde::CCAssignerAppleA64 body_assigner;
		CallBuilder body_builder{*this, body_assigner};
		for (uint32_t argument = 0; argument < argument_count; ++argument) {
			body_builder.add_arg(CallArg{node.operands[argument]});
		}
		body_builder.call(this->func_syms[function]);
		const auto return_type = adaptor->typed_body_return_type(target);
		if (validate && !return_type.valid) {
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
					? DarwinConfig::FP_BANK
					: DarwinConfig::GP_BANK,
				part_desc.bit_width / 8);
			body_builder.add_ret(body_results.back(), ::tpde::CCAssignment{});
		}
		if (node.has_result) {
			if (validate
					&& val_parts(node.result).count() != body_results.size()) {
				return false;
			}
			auto destination = result_ref(node.result);
			for (uint32_t part = 0; part < body_results.size(); ++part) {
				destination.part(part).set_value(std::move(body_results[part]));
			}
		} else {
			for (auto &body_result : body_results) {
				body_result.reset(this);
			}
		}
		return true;
	}
	/* Copies the payload and type_info parts of boxed, the caller's reference
	 * to value, to payload_reg and type_reg; false when the representation
	 * lacks either part. */
	bool load_boxed_parts(ValueRef &boxed, IRValueRef value,
			AsmReg payload_reg, AsmReg type_reg) {
		const ValueParts parts = val_parts(value);
		bool have_payload = false;
		bool have_type_info = false;
		for (uint32_t part = 0; part < parts.count(); ++part) {
			auto part_value = boxed.part(part);
			auto value_reg = part_value.load_to_reg();
			switch (parts.representation.parts[part].semantic_role) {
				case ZEND_TPDE_MACHINE_PART_PAYLOAD:
					ASM(ORRx, payload_reg, value_reg, value_reg);
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
		return have_payload && have_type_info;
	}
	/* Loads a register operand into payload_reg: the payload of a boxed
	 * value, whose type must be boxed_type or branch to slow, otherwise the
	 * pointer or integer itself. */
	bool load_register_operand(IRValueRef value, AsmReg payload_reg,
			AsmReg type_reg, uint32_t boxed_type, ::tpde::Label slow) {
		if (ir().machine_kind(value)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto boxed = val_ref(value);
			if (!load_boxed_parts(boxed, value, payload_reg, type_reg)) {
				return false;
			}
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, boxed_type);
			generate_raw_jump(Jump::Jne, slow);
		} else {
			auto [value_ref, value_part] = val_ref_single(value);
			auto value_reg = value_part.load_to_reg();
			ASM(ORRx, payload_reg, value_reg, value_reg);
		}
		return true;
	}
	/* Loads the array in the frame slot at offset into array_reg; a slot
	 * holding no array branches to slow. */
	void load_frame_array(AsmReg frame_reg, uint32_t offset, AsmReg type_reg,
			AsmReg array_reg, ::tpde::Label slow) {
		load_off(type_reg, frame_reg,
			offset + static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_ARRAY);
		generate_raw_jump(Jump::Jne, slow);
		load_off(array_reg, frame_reg, offset, 8);
	}
	/* An integer key in the frame slot at key_offset branches to key_long; a
	 * string key is loaded into key_reg with kind_reg = 1 and continues at
	 * key_ready; any other key branches to slow. */
	void load_frame_string_key(AsmReg frame_reg, uint32_t key_offset,
			AsmReg type_reg, AsmReg key_reg, AsmReg kind_reg,
			::tpde::Label key_long, ::tpde::Label key_ready,
			::tpde::Label slow) {
		load_off(type_reg, frame_reg,
			key_offset + static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			4);
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jeq, key_long);
		ASM(CMPwi, type_reg, IS_STRING);
		generate_raw_jump(Jump::Jne, slow);
		load_off(key_reg, frame_reg, key_offset, 8);
		materialize_constant(1, DarwinConfig::GP_BANK, 4, kind_reg);
		generate_raw_jump(Jump::jmp, key_ready);
	}
	/* Follows the hash chain of the integer key_reg in the hash array_reg,
	 * with its arData in element_reg: at found, slot_reg holds the bucket; a
	 * missing key branches to missing. limit_reg and type_reg are scratch. */
	void emit_hash_long_lookup(AsmReg array_reg, AsmReg key_reg,
			AsmReg element_reg, AsmReg limit_reg, AsmReg slot_reg,
			AsmReg type_reg, ::tpde::Label loop, ::tpde::Label next,
			::tpde::Label found, ::tpde::Label missing) {
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nTableMask)), 4);
		ASM(ORRw, limit_reg, key_reg, limit_reg);
		ASM(ADDx_sxtw, slot_reg, element_reg, limit_reg, 2);
		load_off(limit_reg, slot_reg, 0, 4);
		label_place(loop);
		ASM(CMPwi, limit_reg, HT_INVALID_IDX);
		generate_raw_jump(Jump::Jeq, missing);
		ASM(ADDx_lsl, slot_reg, element_reg, limit_reg, 5);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		ASM(CMPx, type_reg, key_reg);
		generate_raw_jump(Jump::Jne, next);
		load_off(type_reg, slot_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		ASM(CMPxi, type_reg, 0);
		generate_raw_jump(Jump::Jeq, found);
		label_place(next);
		load_off(limit_reg, slot_reg,
			static_cast<uint32_t>(
				offsetof(Bucket, val) + offsetof(zval, u2.next)),
			4);
		generate_raw_jump(Jump::jmp, loop);
	}
	/* element_reg = the element key_reg of the packed array_reg; a key at or
	 * above nNumUsed branches to missing. limit_reg is scratch. */
	void emit_packed_element(AsmReg array_reg, AsmReg key_reg,
			AsmReg limit_reg, AsmReg element_reg, ::tpde::Label missing) {
		load_off(limit_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, nNumUsed)), 4);
		ASM(CMPx, key_reg, limit_reg);
		generate_raw_jump(Jump::Jhs, missing);
		load_off(element_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, arPacked)), 8);
		ASM(ADDx_lsl, element_reg, element_reg, key_reg, 4);
	}
	/* Branches to slow unless type_reg holds the type_info of a plain
	 * element: undefined, indirect and reference elements are left to the
	 * helper. kind_reg is scratch. */
	void emit_plain_element_check(AsmReg type_reg, AsmReg kind_reg,
			::tpde::Label slow) {
		ASM(ANDwi, kind_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, kind_reg, IS_UNDEF);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, kind_reg, IS_INDIRECT);
		generate_raw_jump(Jump::Jeq, slow);
		ASM(CMPwi, kind_reg, IS_REFERENCE);
		generate_raw_jump(Jump::Jeq, slow);
	}
	/* GC_REFCOUNT(counted) += amount, through scratch. */
	void emit_gc_addref(AsmReg counted, AsmReg scratch, uint32_t amount = 1) {
		load_off(scratch, counted,
			static_cast<uint32_t>(offsetof(zend_refcounted_h, refcount)), 4);
		ASM(ADDwi, scratch, scratch, amount);
		store_off(counted,
			static_cast<uint32_t>(offsetof(zend_refcounted_h, refcount)),
			scratch, 4);
	}
	/* GC_REFCOUNT(counted) -= 1, through scratch. */
	void emit_gc_delref(AsmReg counted, AsmReg scratch) {
		load_off(scratch, counted,
			static_cast<uint32_t>(offsetof(zend_refcounted_h, refcount)), 4);
		ASM(SUBwi, scratch, scratch, 1);
		store_off(counted,
			static_cast<uint32_t>(offsetof(zend_refcounted_h, refcount)),
			scratch, 4);
	}
	/* Stores one part of a boxed value at part_offset from frame. With
	 * reload_spilled, a part whose assignment is stack_valid() is reloaded
	 * from the stack instead of trusting its register. */
	template <typename Frame>
	void store_boxed_part(Frame frame, ValuePartRef &part,
			uint32_t part_offset, uint32_t size, bool reload_spilled = true) {
		if (reload_spilled && part.has_assignment()
				&& part.assignment().stack_valid()) {
			auto assignment = part.assignment();
			ScratchReg scratch{this};
			auto reg = scratch.alloc_gp();
			load_from_stack(reg, assignment.frame_off(),
				assignment.part_size());
			store_off(frame_register(frame), part_offset, reg, size);
		} else {
			store_off(frame_register(frame), part_offset,
				part.load_to_reg(), size);
		}
	}
	/*
	 * One rule holds for a bool in a frame slot: its type_info, IS_FALSE or
	 * IS_TRUE, is authoritative, and no reader may depend on its payload.
	 * Readers compare the type_info through compare_bool_slot(); writers
	 * store through store_bool_slot().
	 */
	/* Compares the type_info of the bool in the zval at base + offset with
	 * IS_TRUE, through scratch: equal for true. */
	void compare_bool_slot(AsmReg scratch, AsmReg base, uint64_t offset) {
		load_off(scratch, base,
			static_cast<uint32_t>(offset + offsetof(zval, u1.type_info)), 4);
		ASM(CMPwi, scratch, IS_TRUE);
	}
	/* How store_bool_slot() forms type_info IS_FALSE + value in its type
	 * register; each writer keeps the instructions it emitted before. */
	enum class BoolTypeInfo {
		/* ADD type, value, #IS_FALSE */
		ADD,
		/* MOV type, value; ADD type, type, #IS_FALSE */
		COPY_ADD,
		/* MOV type, #IS_FALSE; ADD type, type, value */
		MATERIALIZE_ADD,
		/* The type register holds the type_info already. */
		GIVEN,
	};
	/*
	 * Writes the bool in value_reg, 0 or 1, to the zval at base + offset as
	 * type_info IS_FALSE + value, formed by form in type, which is allocated
	 * after the stores before it unless it holds a register. store_payload
	 * also stores value_reg as the payload first; by the rule above that
	 * store is dead. zero_second_word zeroes type_info and u2 before the
	 * type_info store.
	 */
	void store_bool_slot(AsmReg base, uint32_t offset, AsmReg value_reg,
			ScratchReg &type, bool store_payload,
			BoolTypeInfo form = BoolTypeInfo::ADD,
			bool zero_second_word = false) {
		if (store_payload) {
			store_off(base, offset, value_reg, 8);
		}
		if (zero_second_word) {
			store_constant(base, offset + 8, 0, 8);
		}
		const AsmReg type_reg =
			type.has_reg() ? type.cur_reg() : type.alloc_gp();
		switch (form) {
			case BoolTypeInfo::ADD:
				ASM(ADDwi, type_reg, value_reg, IS_FALSE);
				break;
			case BoolTypeInfo::COPY_ADD:
				ASM(ORRx, type_reg, value_reg, value_reg);
				ASM(ADDwi, type_reg, type_reg, IS_FALSE);
				break;
			case BoolTypeInfo::MATERIALIZE_ADD:
				materialize_constant(IS_FALSE,
					DarwinConfig::GP_BANK, 4, type_reg);
				ASM(ADDx, type_reg, type_reg, value_reg);
				break;
			case BoolTypeInfo::GIVEN:
				break;
		}
		store_off(base,
			offset + static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			type_reg, 4);
	}
	/* Writes the bool value, known when compiling, to the zval at base +
	 * offset: with store_payload the payload 1 or 0, then type_info IS_TRUE
	 * or IS_FALSE. */
	void store_bool_slot(AsmReg base, uint32_t offset, bool value,
			bool store_payload) {
		if (store_payload) {
			store_constant(base, offset, value ? 1 : 0, 8);
		}
		store_constant(base,
			offset + static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			value ? IS_TRUE : IS_FALSE, 4);
	}
	/* A helper wrote a scalar result to its frame slot. Zend writes a bool
	 * as type_info only, so its value comes from IS_TRUE, not the payload. */
	void load_slot_scalar(AsmReg dst, zend_tpde_machine_value_kind kind,
			uint64_t frame_offset) {
		if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
			compare_bool_slot(dst, canonical_frame_register(), frame_offset);
			generate_raw_set(Jump::Jeq, dst);
			return;
		}
		load_off(dst, canonical_frame_register(),
			static_cast<uint32_t>(frame_offset), 8);
	}
	/* Loads a value part from the zval at base + offset; a bool's value
	 * comes from its type. */
	bool load_slot_part(AsmReg reg, AsmReg base, uint64_t offset,
			zend_tpde_machine_value_kind kind,
			zend_tpde_machine_part_role role) {
		if (kind == ZEND_TPDE_MACHINE_VALUE_BOOL
				&& role == ZEND_TPDE_MACHINE_PART_VALUE) {
			compare_bool_slot(reg, base, offset);
			generate_raw_set(Jump::Jeq, reg);
		} else if (role == ZEND_TPDE_MACHINE_PART_VALUE
				|| role == ZEND_TPDE_MACHINE_PART_PAYLOAD) {
			load_off(reg, base, static_cast<uint32_t>(offset), 8);
		} else if (role == ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
			load_off(reg, base,
				static_cast<uint32_t>(
					offset + offsetof(zval, u1.type_info)), 4);
		} else {
			return false;
		}
		return true;
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
	void generate_uncond_branch(IRBlockRef target) {
		continuation_edge_emitted_ =
			continuation_edge_emitted_
			|| static_cast<uint32_t>(target)
				== current_continuation_block_;
		Base::generate_uncond_branch(target);
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
	void start_func(uint32_t index) {
		/* A function that failed to compile left its guard patches. */
		current_direct_call_stack_guard_patches_.clear();
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
			for_each_exception_handler(true, [&](IRBlockRef handler_block,
					uint32_t flagged_position) {
				const auto expected_reg =
					::tpde::a64::AsmReg{::tpde::a64::AsmReg::R16};
				materialize_constant(flagged_position,
					DarwinConfig::GP_BANK, 4, expected_reg);
				ASM(CMPx,
					::tpde::a64::AsmReg{::tpde::a64::AsmReg::R0},
					expected_reg);
				const auto continued = text_writer.label_create();
				generate_raw_jump(Jump::Jne, continued);
				generate_exception_branch(handler_block);
				label_place(continued);
			});
			gen_func_epilog();
		}
		auto final_frame_size =
			::tpde::util::align_up(this->stack.frame_size, 16);
		if (final_frame_size > 4095) {
			final_frame_size =
				::tpde::util::align_up(final_frame_size, 4096);
		}
		const auto callee_saved =
			cur_cc_assigner()->get_ccinfo().callee_saved_regs;
		const uint64_t saved_registers =
			this->register_file.clobbered & callee_saved;
		const bool needs_stack_frame =
			this->stack.frame_used || this->stack.generated_call
			|| this->stack.has_dynamic_alloca || saved_registers != 0
			|| (this->register_file.clobbered
				& (UINT64_C(1) << AsmReg::LR));
		const uint32_t machine_frame_size = needs_stack_frame
			? static_cast<uint32_t>(final_frame_size) : 0;
		Base::finish_func(index);
		resolve_direct_call_stack_guards(index, machine_frame_size);
	}
	void load_address_of_var_reference(
			AsmReg destination, ::tpde::AssignmentPartRef reference) {
		const IRValueRef value =
			ir().frame_slot_reference(reference.variable_ref_data());
		const zend_tpde_machine_reference *descriptor = nullptr;
		if (!ir().machine_reference(value, &descriptor)) {
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
	void emit_truthiness_ladder(AsmReg type_reg, AsmReg value_reg,
		AsmReg frame_reg, uint32_t operand_offset,
		std::optional<AsmReg> boxed_payload_reg, ::tpde::Label truthy,
		::tpde::Label falsey, ::tpde::Label slow, bool slow_after_resource);
	bool compile_boxed_cond_guard(IRInstRef instruction);
	bool compile_boxed_cond_cold(IRInstRef instruction);
	bool compile_boxed_cond_cold_branch(IRInstRef instruction);
	bool reload_generator_values(
		IRInstRef instruction, std::vector<ValueRef> &locked_values);
	/* The parts compile_inst_impl() dispatches to, by node kind and opcode. */
	bool compile_typed_call_guard();
	bool compile_string_length_value();
	bool compile_scalar_select();
	bool compile_box_scalar();
	bool compile_unbox_scalar();
	bool compile_unbox_pointer();
	bool compile_unbox_reference_scalar();
	bool compile_load_frame();
	bool compile_user_opcode_landing();
	bool compile_user_opcode_dispatch();
	bool compile_user_opcode_gateway();
	void compare_position(AsmReg position_reg, uint32_t source);
	void jump_to_source(uint32_t source);
	void emit_user_opcode_dispatch_case(
		const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_finally_return(
		const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_catch(const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_receive(const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_call_fragment(
		const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_multi_branch(
		const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_branch(const UserOpcodeDispatchCase &dispatch_case);
	void emit_user_opcode_operation(
		const UserOpcodeDispatchCase &dispatch_case);
	bool compile_user_call();
	auto context_register();
	auto context_argument();
	template <typename Reg>
	void load_active_activation(Reg destination_reg);
	void emit_phase_failure();
	bool compile_user_call_init();
	void initialize_user_call_setup(AsmReg setup_reg);
	void initialize_user_call_callee(AsmReg callee_reg,
		AsmReg activation_reg, bool allocated);
	bool compile_user_call_send();
	bool compile_user_call_check();
	bool compile_user_call_expand();
	bool compile_user_call_do();
	void release_active_activation();
	void branch_released_exception();
	bool compile_user_opcode_call_fragment();
	bool compile_generator_gateway();
	bool compile_generator_resume();
	bool compile_zval_guard_arguments();
	bool compile_zval_reference_resolve();
	bool compile_zval_boxed_store();
	bool compile_zval_payload_load();
	bool compile_zval_store();
	bool compile_echo_scalar();
	bool compile_guarded_cold();
	bool compile_func_num_args();
	bool compile_value_multi_branch();
	bool compile_value_cond_branch();
	bool compile_iterator_reset_branch(
		const zend_tpde_array_iterator_reset &reset_layout);
	bool compile_packed_iterator_branch(
		const zend_tpde_packed_iterator_fetch &layout);
	bool compile_value_truthiness_branch(
		const zend_tpde_value_condition &layout,
		bool register_boxed_condition);
	bool compile_call_direct_user();
	bool compiled_variable_used(uint32_t variable_index);
	void load_generated_result(AsmReg result_frame_reg);
	void finish_generated_result();
	::tpde::Label call_slow_target();
	void load_callee_function(AsmReg destination);
	bool compile_direct_user_inlined_body();
	bool compile_direct_user_typed_body();
	std::optional<bool> compile_direct_user_leaf_path();
	bool compile_direct_user_leaf_inlined_body(AsmReg first_reg);
	bool store_direct_user_leaf_arguments(AsmReg frame_reg, AsmReg callee_reg);
	bool compile_direct_user_frame_path();
	void guard_direct_user_callee();
	bool guard_direct_user_arguments();
	void reserve_direct_user_frame();
	void initialize_direct_user_frame();
	bool store_direct_user_frame_arguments();
	void initialize_direct_user_locals();
	void link_direct_user_activation();
	void complete_direct_user_frame_call(ValuePart &fast_status);
	bool compile_direct_user_slow_call();
	bool compile_call_direct_internal();
	bool compile_finally_call();
	bool compile_finally_return();
	bool compile_catch_enter();
	bool compile_return();
	bool compile_return_source_zval();
	bool compile_mir_opcode();
	bool compile_machine_opcode();
	/* Operand access and value-operation fast paths of those parts. */
	auto unary();
	auto binary();
	bool copy_result();
	template <typename Encode>
	auto encode_binary(Encode encode);
	bool can_fuse_compare_branch();
	bool fuse_compare_branch(Jump condition);
	bool integer_compare(Jump condition);
	bool floating_compare(Jump condition);
	bool materialize_boxed_boundary_operand(uint32_t operand_index,
		zend_mir_storage_id storage);
	bool execute_value_operation_with(ValuePart *frame_argument,
		zend_native_runtime_helper_id helper, uint32_t source_opcode);
	bool execute_value_operation(ValuePart *frame_argument = nullptr);
	bool branch_to_guarded_cold();
	const zend_tpde_machine_reference *operation_machine_reference(
		zend_tpde_machine_reference_kind expected);
	bool copy_slot(
		const zend_mir_source_operand_ref &source_operand,
		zend_mir_storage_id source_storage,
		zend_mir_storage_id target_storage,
		zend_mir_storage_id result_storage,
		bool move_source);
	bool copy_temporary_slot();
	bool free_temporary_slot();
	bool read_array();
	bool read_array_register_operands(const zend_tpde_array_read &layout,
		std::span<const IRBlockRef> guarded_successors);
	bool isset_array();
	bool append_packed_array();
	bool string_length();
	bool string_identity();
	bool long_binary();
	bool long_assign_op();
	bool long_incdec();
	bool slot_isset_empty();
	bool object_property_read();
	bool object_property_write();
	bool dynamic_fetch_read();
	bool compile_inst_impl(IRInstRef instruction, InstRange);
	bool compile_inst(IRInstRef instruction, InstRange);
};

uint32_t zval_type(const Adaptor &adaptor, IRValueRef value) {
	return zend_tpde_zval_type(adaptor.active().exact_type(value));
}

uint32_t zval_type(zend_mir_scalar_type_mask type) {
	return zend_tpde_zval_type(type);
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
	if (!zend_tpde_machine_kind_is_pointer(kind)) {
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
		emit_gc_addref(payload_reg, count_reg);
		label_place(copied);
		return true;
	}
	ScratchReg count{this};
	auto count_reg = count.alloc_gp();
	emit_gc_addref(payload_reg, count_reg);
	return true;
}

bool ZendCompilerA64::emit_materializations(
		IRInstRef instruction, bool interrupt_slow_path) {
	return for_each_materialization(instruction, interrupt_slow_path,
			[&](AsmReg frame_reg, IRValueRef value,
				zend_mir_storage_id, uint64_t offset) {
		const zend_tpde_machine_value_kind machine_kind =
			ir().machine_kind(value);
		auto value_ref = val_ref(value);
		auto payload = value_ref.part(0);
		if (machine_kind == ZEND_TPDE_MACHINE_VALUE_BOOL) {
			ScratchReg type_info{this};
			store_bool_slot(frame_reg, static_cast<uint32_t>(offset),
				payload.load_to_reg(), type_info, true,
				BoolTypeInfo::COPY_ADD);
			return true;
		}
		AsmReg payload_reg = AsmReg::make_invalid();
		if (machine_kind
				== ZEND_TPDE_MACHINE_VALUE_F64
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
				|| machine_kind
					== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR) {
			payload_reg = payload.load_to_reg();
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
		} else {
			ScratchReg type_info_value{this};
			auto type_info_reg = type_info_value.alloc_gp();
			if (!emit_machine_zval_type_info(
					machine_kind, payload_reg, type_info_reg)) {
				return false;
			}
			store_off(frame_reg, type_offset, type_info_reg, 4);
		}
		return true;
	});
}

/*
 * The truthiness ladder from false on, for the zval whose masked type is in
 * type_reg: false is falsey and true truthy, a long, a string, an array and a
 * resource decide by their value, any other type branches to slow. The
 * payload is copied into value_reg from boxed_payload_reg, or loaded from
 * frame_reg + operand_offset without it. With slow_after_resource, another
 * type branches past the resource test to a jump to slow.
 */
void ZendCompilerA64::emit_truthiness_ladder(AsmReg type_reg,
		AsmReg value_reg, AsmReg frame_reg, uint32_t operand_offset,
		std::optional<AsmReg> boxed_payload_reg, ::tpde::Label truthy,
		::tpde::Label falsey, ::tpde::Label slow, bool slow_after_resource) {
	auto load_payload = [&]() {
		if (boxed_payload_reg.has_value()) {
			ASM(ORRx, value_reg, *boxed_payload_reg, *boxed_payload_reg);
		} else {
			load_off(value_reg, frame_reg, operand_offset, 8);
		}
	};
	ASM(CMPwi, type_reg, IS_FALSE);
	generate_raw_jump(Jump::Jeq, falsey);
	ASM(CMPwi, type_reg, IS_TRUE);
	generate_raw_jump(Jump::Jeq, truthy);
	ASM(CMPwi, type_reg, IS_LONG);
	auto not_long = text_writer.label_create();
	generate_raw_jump(Jump::Jne, not_long);
	load_payload();
	generate_raw_jump(
		Jump{Jump::Cbnz, value_reg, false}, truthy);
	generate_raw_jump(Jump::jmp, falsey);

	label_place(not_long);
	ASM(CMPwi, type_reg, IS_STRING);
	auto not_string = text_writer.label_create();
	generate_raw_jump(Jump::Jne, not_string);
	load_payload();
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
	load_payload();
	load_off(type_reg, value_reg,
		static_cast<uint32_t>(offsetof(HashTable, nNumOfElements)), 4);
	generate_raw_jump(Jump{Jump::Cbnz, type_reg, false}, truthy);
	generate_raw_jump(Jump::jmp, falsey);

	label_place(not_array);
	ASM(CMPwi, type_reg, IS_RESOURCE);
	auto not_resource = slow_after_resource
		? text_writer.label_create() : slow;
	generate_raw_jump(Jump::Jne, not_resource);
	load_payload();
	load_off(type_reg, value_reg,
		static_cast<uint32_t>(offsetof(zend_resource, handle)), 4);
	generate_raw_jump(Jump{Jump::Cbnz, type_reg, false}, truthy);
	generate_raw_jump(Jump::jmp, falsey);
	if (slow_after_resource) {
		label_place(not_resource);
		generate_raw_jump(Jump::jmp, slow);
	}
}

bool ZendCompilerA64::compile_boxed_cond_guard(IRInstRef instruction) {
	const Adaptor::InstNode &node = ir().node(instruction);
	const zend_tpde_instruction &mir =
		ir().mir_instruction(instruction);
	zend_tpde_value_condition layout;
	const bool register_string = node.operands.size() == 2
		&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE}
		&& ir().machine_kind(node.operands[1])
			== ZEND_TPDE_MACHINE_VALUE_STRING_PTR;
	const bool register_boxed = node.operands.size() == 2
		&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE}
		&& ir().machine_kind(node.operands[1])
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
	 * refcounted source value directly.
	 */
	if (layout.has_result) {
		generate_raw_jump(Jump::jmp, slow);
	} else if (register_string) {
		bool literal_truthy = false;
		if (ir().known_string_literal(
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
		emit_truthiness_ladder(type_reg, value_reg, frame_reg,
			layout.operand_offset, std::nullopt, truthy, falsey, slow,
			false);
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
		store_bool_slot(frame_reg, layout.result_offset, true, true);
	}
	materialize_constant(
		uint64_t{1}, DarwinConfig::GP_BANK, 4, decision_reg);
	generate_raw_jump(Jump::jmp, ready);
	label_place(falsey);
	if (layout.has_result && layout.source_opcode == ZEND_JMPZ_EX) {
		store_bool_slot(frame_reg, layout.result_offset, false, true);
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
	const Adaptor::InstNode &node = ir().node(instruction);
	const zend_tpde_instruction &mir =
		ir().mir_instruction(instruction);
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
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.op2), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
	ASM(CMPwi, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
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
	const Adaptor::InstNode &node = ir().node(instruction);
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
	if (!reload_generator_resume_values(instruction, frame_reg, false,
			[&](ValueRef &&value) {
				locked_values.push_back(std::move(value));
			})) {
		return false;
	}
	resumed_frame.reset(this);
	return true;
}

auto ZendCompilerA64::unary() {
	const Adaptor::InstNode &node = inst_context_->node;
	return val_ref_single(node.operands[0]);
}

auto ZendCompilerA64::binary() {
	const Adaptor::InstNode &node = inst_context_->node;
	return std::pair{val_ref_single(node.operands[0]),
		val_ref_single(node.operands[1])};
}

bool ZendCompilerA64::copy_result() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (ir().machine_kind(node.result)
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
}

template <typename Encode>
auto ZendCompilerA64::encode_binary(Encode encode) {
	const Adaptor::InstNode &node = inst_context_->node;
	auto left = val_ref(node.operands[0]);
	auto right = val_ref(node.operands[1]);
	auto result = result_ref(node.result);
	return encode(left.part(0), right.part(0), result.part(0));
}

bool ZendCompilerA64::can_fuse_compare_branch() {
	InstRange &remaining_instructions =
		inst_context_->remaining_instructions;
	const Adaptor::InstNode &node = inst_context_->node;
	if (remaining_instructions.from == remaining_instructions.to) {
		return false;
	}
	const IRInstRef next = *remaining_instructions.from;
	const Adaptor::InstNode &consumer = ir().node(next);
	/* A generator continuation's terminal reloads its resume values;
	 * a fused branch would never run that reload. */
	return consumer.kind == Adaptor::InstKind::MIR
		&& consumer.operands.size() == 1
		&& consumer.operands[0] == node.result
		&& ir().instruction_record(next).opcode
			== ZEND_MIR_OPCODE_COND_BRANCH
		&& ir().generator_resume_values(next).empty()
		&& this->analyzer.liveness_info(
			adaptor->val_local_idx(node.result)).ref_count == 2;
}

bool ZendCompilerA64::fuse_compare_branch(Jump condition) {
	InstRange &remaining_instructions =
		inst_context_->remaining_instructions;
	if (!can_fuse_compare_branch()) {
		return false;
	}
	const IRInstRef next = *remaining_instructions.from;
	const Adaptor::InstNode &consumer = ir().node(next);
	const auto &successors = adaptor->block_succs(
		IRBlockRef{consumer.control_block});
	if (successors.size() != 2) {
		return false;
	}
	generate_cond_branch(condition, successors[0], successors[1]);
	adaptor->mark_fused(next);
	return true;
}

bool ZendCompilerA64::integer_compare(Jump condition) {
	const Adaptor::InstNode &node = inst_context_->node;
	auto [left_pair, right_pair] = binary();
	auto &[left_ref, left] = left_pair;
	auto &[right_ref, right] = right_pair;
	uint64_t immediate;
	auto left_reg = left.load_to_reg();
	if (ir().constant(node.operands[1], &immediate)) {
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
}

bool ZendCompilerA64::floating_compare(Jump condition) {
	const Adaptor::InstNode &node = inst_context_->node;
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
}

bool ZendCompilerA64::materialize_boxed_boundary_operand(uint32_t operand_index,
		zend_mir_storage_id storage) {
	const Adaptor::InstNode &node = inst_context_->node;
	if (operand_index == UINT32_MAX) {
		return true;
	}
	if (!zend_mir_id_is_valid(storage)
			|| operand_index >= node.liveness_operands.size()) {
		return false;
	}
	const uint64_t offset =
		(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
	if (offset > UINT32_MAX - sizeof(zval)) {
		return false;
	}
	const IRValueRef operand = node.liveness_operands[operand_index];
	if (ir().machine_kind(operand)
			!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		return false;
	}
	auto boxed = val_ref(operand);
	auto payload = boxed.part(0);
	auto type_info = boxed.part(1);
	const bool reload_spilled =
		node.kind == Adaptor::InstKind::GuardedCold;
	store_boxed_part(CanonicalFrame{}, payload,
		static_cast<uint32_t>(offset), 8, reload_spilled);
	store_boxed_part(CanonicalFrame{}, type_info,
		static_cast<uint32_t>(offset + offsetof(zval, u1.type_info)), 4,
		reload_spilled);
	return true;
}

bool ZendCompilerA64::execute_value_operation_with(ValuePart *frame_argument,
		zend_native_runtime_helper_id helper, uint32_t source_opcode) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
	builder.add_arg(ValuePart{
		encode_helper_operand(explicit_object_operands,
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
			encode_helper_operand(explicit_object_operands,
				operation.op2, operation.op2_unused_payload), 8,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		builder.add_arg(ValuePart{
			encode_helper_operand(explicit_object_operands,
				operation.result, operation.result_unused_payload), 8,
			DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		if (explicit_auxiliary) {
			builder.add_arg(ValuePart{
				encode_helper_operand(explicit_object_operands,
					operation.auxiliary,
					operation.auxiliary_unused_payload)
					| zend_tpde_auxiliary_cache_bits(helper, operation), 8,
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
	ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
	auto continued = text_writer.label_create();
	generate_raw_jump(Jump::Jeq, continued);
	if (zend_mir_id_is_valid(mir.exception_block_id)) {
		auto propagate = text_writer.label_create();
		ASM(CMPwi, status_reg, ZEND_NATIVE_EXCEPTION);
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
	 * Some canonical value operations, such as COUNT, execute only
	 * through this helper path. When the adaptor selected a boxed machine
	 * result for an optimized direct-call argument, snapshot the complete
	 * result before OPcache reuses the temporary frame slot.
	 */
	if (node.kind != Adaptor::InstKind::GuardedCold && node.has_result
			&& ir().machine_kind(node.result)
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
}

bool ZendCompilerA64::execute_value_operation(ValuePart *frame_argument) {
	const zend_tpde_instruction &mir = *inst_context_->mir;
	return execute_value_operation_with(
		frame_argument, mir.runtime_helper,
		mir.has_value_operation
			? mir.value_operation.source_opcode : UINT32_MAX);
}

bool ZendCompilerA64::branch_to_guarded_cold() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.kind != Adaptor::InstKind::GuardedFast
			|| node.argument_index == UINT32_MAX) {
		return false;
	}
	for (IRValueRef operand : node.operands) {
		if (ir().machine_reference(operand, nullptr)) {
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
}

const zend_tpde_machine_reference *
ZendCompilerA64::operation_machine_reference(
		zend_tpde_machine_reference_kind expected) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_machine_reference *reference = nullptr;
	return ir().operation_machine_reference(
				node.mir_instruction_index, &reference)
			&& reference->kind == expected
		? reference : nullptr;
}

bool ZendCompilerA64::copy_slot(
		const zend_mir_source_operand_ref &source_operand,
		zend_mir_storage_id source_storage,
		zend_mir_storage_id target_storage,
		zend_mir_storage_id result_storage,
		bool move_source) {
	const Adaptor::InstNode &node = inst_context_->node;
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
			|| node.continuation_block == UINT32_MAX
			|| !adaptor->guarded_successors_match(node)) {
		return false;
	}
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.control_block});

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
		&& ir().machine_value_is_register_authoritative(
			node.operands[0]);

	if (register_source) {
		const zend_tpde_machine_value_kind source_kind =
			ir().machine_kind(node.operands[0]);
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
	emit_gc_delref(low_word_reg, probe_reg);
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
		emit_gc_addref(low_word_reg, probe_reg, source_refcount_increments);
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
}

bool ZendCompilerA64::copy_temporary_slot() {
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
	emit_gc_addref(value_reg, probe_reg);
	label_place(copied);
	store_off(frame_reg, static_cast<uint32_t>(result_offset),
		value_reg, 8);
	store_off(frame_reg,
		static_cast<uint32_t>(
			result_offset + offsetof(zval, u1.type_info)),
		type_reg, 4);
	return true;
}

bool ZendCompilerA64::free_temporary_slot() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
			|| node.continuation_block == UINT32_MAX
			|| !adaptor->guarded_successors_match(node)) {
		return false;
	}
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.control_block});
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
}

bool ZendCompilerA64::read_array() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	zend_tpde_array_read layout;
	const zend_tpde_machine_reference *element_reference =
		operation_machine_reference(
			ZEND_TPDE_MACHINE_REFERENCE_PACKED_ELEMENT);

	if (!zend_tpde_array_read_at(mir, &layout)
			|| element_reference == nullptr
			|| (!layout.container_literal
				&& !zend_mir_id_is_valid(
					element_reference->base_value_id))
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
		if (!adaptor->guarded_successors_match(node)) {
			return false;
		}
		if (!node.operands.empty()
				&& ((ir().exact_type(node.operands[0])
							== ZEND_MIR_SCALAR_TYPE_I64
						&& ir().representation(node.operands[0])
							== ZEND_MIR_REPRESENTATION_I64
						&& ir().machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| ir().machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					|| (ir().representation(node.operands[0])
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& ir().machine_kind(node.operands[0])
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
				&& ((ir().representation(node.result)
							== ZEND_MIR_REPRESENTATION_I64
						&& ir().exact_type(node.result)
							== ZEND_MIR_SCALAR_TYPE_I64
						&& ir().machine_kind(node.result)
							== ZEND_TPDE_MACHINE_VALUE_I64)
					|| (ir().representation(node.result)
							== ZEND_MIR_REPRESENTATION_ZVAL
						&& ir().machine_kind(node.result)
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))) {
			return read_array_register_operands(layout,
				guarded_successors);
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

	load_frame_array(frame_reg, layout.container_offset, type_reg,
		array_reg, slow);

	load_frame_string_key(frame_reg, layout.key_offset, type_reg, key_reg,
		high_word_reg, key_long, key_ready, slow);
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
	emit_hash_long_lookup(array_reg, key_reg, element_reg, limit_reg,
		slot_reg, type_reg, mixed_loop, mixed_next, found,
		slow);

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
	emit_packed_element(array_reg, key_reg, limit_reg, element_reg,
		slow);
	ASM(ORRx, slot_reg, element_reg, element_reg);
	label_place(found);
	/*
	 * Mixed buckets begin with zval, so slot_reg is the value address
	 * for both layouts after the packed path copies its address here.
	 */
	ASM(ORRx, element_reg, slot_reg, slot_reg);
	load_off(type_reg, element_reg,
		static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
	/* An undefined, indirect or reference element is left to the
	 * helper, which dereferences like FETCH_DIM_R. */
	emit_plain_element_check(type_reg, limit_reg, slow);

	if (ir().machine_kind(node.result)
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
			switch (ir().exact_type(node.result)) {
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
					switch (ir().machine_kind(node.result)) {
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
			&& ir().machine_kind(node.result)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		generate_raw_jump(Jump::jmp, done);
	} else {
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, done);
		emit_gc_addref(low_word_reg, limit_reg);
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
}

/* The guarded fast read of a register key or receiver, or of a frame
 * array by a register key. */
bool ZendCompilerA64::read_array_register_operands(
		const zend_tpde_array_read &layout,
		std::span<const IRBlockRef> guarded_successors) {
	const Adaptor::InstNode &node = inst_context_->node;
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
		ir().machine_kind(node.operands[0])
			== ZEND_TPDE_MACHINE_VALUE_STRING_PTR;

	if (!load_register_operand(node.operands[0], key_reg,
			type_reg, IS_LONG, slow)) {
		return false;
	}

	const bool register_receiver =
		node.operands.size() > 1
		&& node.machine_reference_operand_index != 1
		&& (ir().machine_kind(node.operands[1])
				== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| (ir().representation(node.operands[1])
					== ZEND_MIR_REPRESENTATION_ZVAL
				&& ir().machine_kind(node.operands[1])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL));
	if (register_receiver) {
		if (!load_register_operand(node.operands[1], array_reg,
				type_reg, IS_ARRAY, slow)) {
			return false;
		}
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
		load_frame_array(frame_reg, layout.container_offset, type_reg,
			array_reg, slow);
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
		generate_raw_jump(
			Jump{Jump::Cbz, type_reg, false}, slow);
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
		generate_raw_jump(
			Jump{Jump::Cbz, limit_reg, false}, mixed_next);
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
		emit_packed_element(array_reg, key_reg, limit_reg, element_reg,
			slow);
	}
	load_off(type_reg, element_reg,
		static_cast<uint32_t>(
			offsetof(zval, u1.type_info)),
		4);
	if (ir().machine_kind(node.result)
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		/* A hole, an indirect or a reference element is left to
		 * the helper. */
		emit_plain_element_check(type_reg, limit_reg, slow);
		auto result = result_ref(node.result);
		auto payload = result.part(0);
		auto type_info = result.part(1);
		auto payload_reg = payload.alloc_reg();
		auto type_info_reg = type_info.alloc_reg();
		load_off(payload_reg, element_reg, 0, 8);
		ASM(ORRw, type_info_reg, type_reg, type_reg);
		payload.set_modified();
		type_info.set_modified();

		auto owned = text_writer.label_create();
		ASM(TSTwi, type_info_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, owned);
		ScratchReg count{this};
		auto count_reg = count.alloc_gp();
		emit_gc_addref(payload_reg, count_reg);
		label_place(owned);
	} else {
		ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jne, slow);
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

bool ZendCompilerA64::isset_array() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
	const bool register_key = !node.operands.empty()
		&& ((ir().exact_type(node.operands[0])
					== ZEND_MIR_SCALAR_TYPE_I64
				&& ir().representation(node.operands[0])
					== ZEND_MIR_REPRESENTATION_I64
				&& ir().machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_I64)
			|| ir().machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| (ir().representation(node.operands[0])
					== ZEND_MIR_REPRESENTATION_ZVAL
				&& ir().machine_kind(node.operands[0])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL));
	const bool register_receiver = register_key
		&& node.operands.size() > 1
		&& (ir().machine_kind(node.operands[1])
				== ZEND_TPDE_MACHINE_VALUE_ARRAY_PTR
			|| (ir().representation(node.operands[1])
					== ZEND_MIR_REPRESENTATION_ZVAL
				&& ir().machine_kind(node.operands[1])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL));
	if (node.kind == Adaptor::InstKind::GuardedFast) {
		if (!adaptor->guarded_successors_match(node)) {
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

	if (register_receiver) {
		if (!load_register_operand(node.operands[1], array_reg,
				type_reg, IS_ARRAY, slow)) {
			return false;
		}
	} else {
		load_frame_array(frame_reg, layout.container_offset, type_reg,
			array_reg, slow);
	}

	if (register_key) {
		if (ir().machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto key_value = val_ref(node.operands[0]);
			if (!load_boxed_parts(key_value, node.operands[0], key_reg,
					type_reg)) {
				return false;
			}
			ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
			ASM(CMPwi, type_reg, IS_LONG);
			generate_raw_jump(Jump::Jeq, key_long);
			ASM(CMPwi, type_reg, IS_STRING);
			generate_raw_jump(Jump::Jne, slow);
			materialize_constant(
				1, DarwinConfig::GP_BANK, 4, key_kind_reg);
			generate_raw_jump(Jump::jmp, key_ready);
		} else {
			auto [source_key_ref, source_key] =
				val_ref_single(node.operands[0]);
			auto source_key_reg = source_key.load_to_reg();
			ASM(ORRx, key_reg, source_key_reg, source_key_reg);
			materialize_constant(
				ir().machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
					? uint64_t{1} : uint64_t{0},
				DarwinConfig::GP_BANK, 4, key_kind_reg);
			generate_raw_jump(Jump::jmp, key_ready);
		}
	} else {
		load_frame_string_key(frame_reg, layout.key_offset, type_reg, key_reg,
			key_kind_reg, key_long, key_ready, slow);
	}
	label_place(key_long);
	if (!register_key) {
		load_off(key_reg, frame_reg, layout.key_offset, 8);
	}
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
	emit_hash_long_lookup(array_reg, key_reg, element_reg, limit_reg,
		slot_reg, type_reg, mixed_loop, mixed_next, found,
		layout.is_empty ? answer_true : answer_false);

	/* A key that may be numeric (first byte a digit or '-'; the empty
	 * string's first byte is its terminator) or has no hash yet is left
	 * to the helper. */
	label_place(mixed_string);
	load_off(decision_reg, key_reg,
		static_cast<uint32_t>(offsetof(zend_string, val)), 1);
	ASM(CMPwi, decision_reg, '-');
	generate_raw_jump(Jump::Jeq, slow);
	ASM(SUBwi, decision_reg, decision_reg, '0');
	ASM(CMPwi, decision_reg, 9);
	generate_raw_jump(Jump::Jls, slow);
	load_off(element_reg, array_reg,
		static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
	load_off(type_reg, key_reg,
		static_cast<uint32_t>(offsetof(zend_string, h)), 8);
	generate_raw_jump(Jump{Jump::Cbz, type_reg, false}, slow);
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
	emit_packed_element(array_reg, key_reg, limit_reg, element_reg,
		layout.is_empty ? answer_true : answer_false);
	generate_raw_jump(Jump::jmp, inspect_element);
	label_place(found);
	ASM(ORRx, element_reg, slot_reg, slot_reg);
	label_place(inspect_element);
	load_off(type_reg, element_reg,
		static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
	ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
	ASM(CMPwi, type_reg, IS_INDIRECT);
	generate_raw_jump(Jump::Jeq, slow);
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
		store_bool_slot(frame_reg, layout.result_offset, element_reg, type,
			true, BoolTypeInfo::GIVEN);
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
}

bool ZendCompilerA64::append_packed_array() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
			|| node.continuation_block == UINT32_MAX
			|| !adaptor->guarded_successors_match(node)) {
		return false;
	}
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.control_block});
	const bool scalar_value =
		node.packed_append_value_operand_index < node.operands.size()
		&& ir().representation(node.operands[
			node.packed_append_value_operand_index])
			== ZEND_MIR_REPRESENTATION_I64
		&& ir().exact_type(node.operands[
			node.packed_append_value_operand_index])
			== ZEND_MIR_SCALAR_TYPE_I64
		&& ir().machine_kind(node.operands[
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

	load_frame_array(frame_reg, layout.container_offset, type_reg,
		array_reg, slow);
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
		emit_gc_addref(low_word_reg, limit_reg);
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
			emit_gc_addref(low_word_reg, limit_reg);
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
}

bool ZendCompilerA64::string_length() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
			&& ir().exact_type(node.operands[0])
				== ZEND_MIR_SCALAR_TYPE_I1
			&& ir().machine_kind(node.operands[0])
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
			type.alloc_gp();
			compare_bool_slot(result_reg, frame_scratch.cur_reg(),
				boolean.operand_offset);
			generate_raw_set(Jump::Jeq, result_reg);
			if (boolean.negate) {
				ASM(EORwi, result_reg, result_reg, 1);
			}
			store_bool_slot(frame_scratch.cur_reg(), boolean.result_offset,
				result_reg, type, true);
			return true;
		}
		return execute_value_operation();
	}
	const bool frame_only = node.operands.size() == 1
		&& node.operands[0] == IRValueRef{Adaptor::FRAME_VALUE};
	const bool register_string = node.operands.size() == 2
		&& ir().machine_kind(node.operands[0])
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
		if (ir().known_string_literal(
				node.operands[0], &known_length, nullptr)) {
			materialize_constant(known_length,
				DarwinConfig::GP_BANK, 8, result_reg);
		} else {
			auto string_reg = string.part(0).load_to_reg();
			load_off(result_reg, string_reg,
				static_cast<uint32_t>(offsetof(zend_string, len)), 8);
		}
		/*
		 * A source call argument may read the result TMP as a canonical
		 * zval although the producer is register-authoritative, so publish
		 * the complete zval; machine consumers keep using result_reg.
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
			|| node.continuation_block == UINT32_MAX
			|| !adaptor->guarded_successors_match(node)) {
		return false;
	}
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.control_block});
	if (register_string) {
		auto [frame_ref, frame] = val_ref_single(node.operands[1]);
		auto frame_scratch = std::move(frame).into_scratch();
		auto [result_ref, result] = result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		uint64_t known_length = 0;
		if (ir().known_string_literal(
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
}

bool ZendCompilerA64::string_identity() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		if (!adaptor->guarded_successors_match(node)) {
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
		if (ir().machine_kind(node.result)
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
			layout.inverted ? uint64_t{0} : uint64_t{1},
			DarwinConfig::GP_BANK, 8, left_reg);
		materialize_constant(
			layout.inverted ? IS_FALSE : IS_TRUE,
			DarwinConfig::GP_BANK, 4, type_reg);
		store_bool_slot(frame_reg, layout.result_offset, left_reg, type,
			true, BoolTypeInfo::GIVEN);
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
	} else {
		materialize_constant(
			layout.inverted ? uint64_t{0} : uint64_t{1},
			DarwinConfig::GP_BANK, 8, left_reg);
		materialize_constant(
			layout.inverted ? IS_FALSE : IS_TRUE,
			DarwinConfig::GP_BANK, 4, type_reg);
		store_bool_slot(frame_reg, layout.result_offset, left_reg, type,
			true, BoolTypeInfo::GIVEN);
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
}

bool ZendCompilerA64::long_binary() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	zend_tpde_long_binary layout{};
	const bool framed_layout =
		zend_tpde_long_binary_at(mir, &layout);
	auto register_long_operand = [&](IRValueRef operand) {
		return ir().exact_type(operand)
					== ZEND_MIR_SCALAR_TYPE_I64
			|| (ir().exact_type(operand)
					== ZEND_MIR_SCALAR_TYPE_NONE
				&& ir().machine_kind(operand)
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
	if (!adaptor->guarded_successors_match(node)) {
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
			if (ir().machine_kind(operand)
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
		ScratchReg result_type{this};
		if (boolean_result) {
			store_bool_slot(frame_reg, layout.result_offset, result_reg,
				result_type, true, BoolTypeInfo::COPY_ADD);
		} else {
			store_off(frame_reg, layout.result_offset, result_reg, 8);
			auto result_type_reg = result_type.alloc_gp();
			materialize_constant(
				static_cast<uint64_t>(IS_LONG),
				DarwinConfig::GP_BANK, 4, result_type_reg);
			store_off(frame_reg,
				layout.result_offset
					+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				result_type_reg, 4);
		}
	}
	if (ir().machine_kind(node.result)
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
	label_place(done);
	const auto successors =
		adaptor->block_succs(IRBlockRef{node.control_block});
	generate_guarded_decision_branch(
		std::move(decision), successors[1], successors[0]);
	return true;
}

bool ZendCompilerA64::long_assign_op() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		if (!adaptor->guarded_successors_match(node)) {
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
				&& ir().representation(
					node.operands[node.assign_op_left_operand_index])
					== ZEND_MIR_REPRESENTATION_I64
				&& ir().exact_type(
					node.operands[node.assign_op_left_operand_index])
					== ZEND_MIR_SCALAR_TYPE_I64
				&& ir().machine_kind(
					node.operands[node.assign_op_left_operand_index])
					== ZEND_TPDE_MACHINE_VALUE_I64) {
			auto [left_ref, left_value] =
				val_ref_single(
					node.operands[node.assign_op_left_operand_index]);
			auto left_value_reg = left_value.load_to_reg();
			ASM(ORRx, left_reg, left_value_reg, left_value_reg);
		} else if (node.assign_op_left_operand_index < node.operands.size()
				&& ir().machine_kind(
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
				&& ir().representation(
					node.operands[node.assign_op_right_operand_index])
					== ZEND_MIR_REPRESENTATION_I64
				&& ir().exact_type(
					node.operands[node.assign_op_right_operand_index])
					== ZEND_MIR_SCALAR_TYPE_I64
				&& ir().machine_kind(
					node.operands[node.assign_op_right_operand_index])
					== ZEND_TPDE_MACHINE_VALUE_I64) {
			auto [right_ref, right_value] =
				val_ref_single(
					node.operands[node.assign_op_right_operand_index]);
			auto right_value_reg = right_value.load_to_reg();
			ASM(ORRx, right_reg, right_value_reg, right_value_reg);
		} else if (node.assign_op_right_operand_index < node.operands.size()
				&& ir().machine_kind(
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
		if (ir().machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_I64) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(ORRx, result_reg, left_reg, left_reg);
			result.set_modified();
		} else if (ir().machine_kind(node.result)
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
}

bool ZendCompilerA64::long_incdec() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		if (!adaptor->guarded_successors_match(node)) {
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
			&& ir().representation(node.operands[0])
				== ZEND_MIR_REPRESENTATION_I64
			&& ir().exact_type(node.operands[0])
				== ZEND_MIR_SCALAR_TYPE_I64
			&& ir().machine_kind(node.operands[0])
				== ZEND_TPDE_MACHINE_VALUE_I64) {
		auto [operand_ref, operand] =
			val_ref_single(node.operands[0]);
		auto operand_reg = operand.load_to_reg();
		ASM(ORRx, value_reg, operand_reg, operand_reg);
	} else if (!node.operands.empty()
			&& node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			&& ir().machine_kind(node.operands[0])
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
		if (ir().machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_I64) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ASM(ORRx, result_reg, value_reg, value_reg);
			result.set_modified();
		} else if (ir().machine_kind(node.result)
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
}

bool ZendCompilerA64::slot_isset_empty() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		if (!adaptor->guarded_successors_match(node)) {
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
		} else if (node.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			compare_bool_slot(type_reg, frame_reg, layout.operand_offset);
			generate_raw_set(Jump::Jne, result_reg);
		} else {
			load_off(type_reg, frame_reg, layout.operand_offset, 8);
			ASM(CMPxi, type_reg, 0);
			generate_raw_set(Jump::Jeq, result_reg);
		}
		store_bool_slot(frame_reg, layout.result_offset, result_reg, type,
			true);
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
			? ir().exact_type(operand) : node.exact_type;
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
				if (type == ZEND_MIR_SCALAR_TYPE_I1) {
					/* Equal here is true, not zero. */
					compare_bool_slot(value_reg, frame_scratch.cur_reg(),
						layout.operand_offset);
					generate_raw_set(
						invert_result ? Jump::Jeq : Jump::Jne, result_reg);
				} else {
					load_off(value_reg, frame_scratch.cur_reg(),
						layout.operand_offset, 8);
					ASM(CMPxi, value_reg, 0);
					generate_raw_set(
						invert_result ? Jump::Jne : Jump::Jeq, result_reg);
				}
			}
			result.set_modified();
			if (node.kind == Adaptor::InstKind::GuardedFast) {
				auto [frame_ref, frame] =
					val_ref_single(IRValueRef{Adaptor::FRAME_VALUE});
				auto frame_scratch = std::move(frame).into_scratch();
				ScratchReg result_type{this};
				result_type.alloc_gp();
				store_bool_slot(frame_scratch.cur_reg(), layout.result_offset,
					result_reg, result_type, true);
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
			compare_bool_slot(value_reg, frame_reg, layout.operand_offset);
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
		emit_truthiness_ladder(type_reg, value_reg, frame_reg,
			layout.operand_offset, std::nullopt, truthy, falsey, slow,
			true);
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
			store_bool_slot(frame_reg, layout.result_offset, type_reg, value,
				true);
		}
		materialize_constant(
			uint64_t{0}, DarwinConfig::GP_BANK, 4, decision_reg);
	} else {
		store_bool_slot(frame_reg, layout.result_offset,
			AsmReg::make_invalid(), type, false, BoolTypeInfo::GIVEN);
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
}

bool ZendCompilerA64::object_property_read() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		if (!adaptor->guarded_successors_match(node)) {
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
		if (ir().machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			/*
			 * The two-part boxed result stays register-authoritative on
			 * the guarded edge. A property read copies its value, so a
			 * refcounted payload is addref'ed before it reaches the
			 * consumer.
			 */
			auto result = result_ref(node.result);
			auto payload = result.part(0);
			auto type_info = result.part(1);
			auto payload_reg = payload.alloc_reg();
			auto type_info_reg = type_info.alloc_reg();
			load_off(payload_reg, property_reg, 0, 8);
			ASM(ORRw, type_info_reg, type_reg, type_reg);
			payload.set_modified();
			type_info.set_modified();

			auto owned = text_writer.label_create();
			ASM(TSTwi, type_info_reg,
				IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
			generate_raw_jump(Jump::Jeq, owned);
			ScratchReg count{this};
			auto count_reg = count.alloc_gp();
			emit_gc_addref(payload_reg, count_reg);
			label_place(owned);
		} else {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			switch (ir().exact_type(node.result)) {
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
					switch (ir().machine_kind(node.result)) {
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
		generate_raw_jump(Jump::jmp, done);
	} else {
		load_off(offset_reg, property_reg, 0, 8);
		store_off(frame_reg, layout.result_offset, offset_reg, 8);
		store_off(frame_reg,
			layout.result_offset
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			type_reg, 4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, copied);
		load_off(offset_reg, property_reg, 0, 8);
		emit_gc_addref(offset_reg, property_reg);
		label_place(copied);
		generate_raw_jump(Jump::jmp, done);
	}

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
}

bool ZendCompilerA64::object_property_write() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		&& ir().machine_kind(node.operands[
			node.property_write_value_operand_index])
			== ZEND_TPDE_MACHINE_VALUE_I64
		&& ir().exact_type(node.operands[
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
		emit_gc_addref(low_word_reg, offset_reg);
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
}

bool ZendCompilerA64::dynamic_fetch_read() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	zend_tpde_dynamic_fetch_read layout;
	if (!zend_tpde_dynamic_fetch_read_at(mir, &layout)) {
		return branch_to_guarded_cold();
	}
	if (!node.has_result) {
		return branch_to_guarded_cold();
	}
	const bool register_name = node.operands.size() == 2
		&& ir().machine_kind(node.operands[0])
			== ZEND_TPDE_MACHINE_VALUE_STRING_PTR
		&& node.operands[1] == IRValueRef{Adaptor::FRAME_VALUE};
	const bool direct_cv = layout.cv_index != UINT32_MAX;
	if (node.kind == Adaptor::InstKind::GuardedFast) {
		if (!adaptor->guarded_successors_match(node)) {
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
				|| ir().representation(node.result)
					!= ZEND_MIR_REPRESENTATION_I64
				|| ir().exact_type(node.result)
					!= ZEND_MIR_SCALAR_TYPE_I64
				|| ir().machine_kind(node.result)
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
	/* FETCH_R dereferences: the helper copies a reference's value. */
	ASM(CMPwi, index_reg, IS_REFERENCE);
	generate_raw_jump(Jump::Jeq, slow);

	if (node.kind == Adaptor::InstKind::GuardedFast) {
		if (ir().machine_kind(node.result)
				== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			auto result = result_ref(node.result);
			auto payload = result.part(0);
			auto type_info = result.part(1);
			auto payload_reg = payload.alloc_reg();
			auto type_info_reg = type_info.alloc_reg();
			load_off(payload_reg, slot_reg, 0, 8);
			ASM(ORRw, type_info_reg, type_reg, type_reg);
			payload.set_modified();
			type_info.set_modified();
			ASM(ORRx, low_word_reg, payload_reg, payload_reg);
		} else {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			switch (ir().exact_type(node.result)) {
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
					switch (ir().machine_kind(node.result)) {
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
			&& ir().machine_kind(node.result)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		generate_raw_jump(Jump::jmp, done);
	}
	ASM(TSTwi, type_reg,
		IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
	generate_raw_jump(Jump::Jeq, done);
	emit_gc_addref(low_word_reg, index_reg);
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
}

bool ZendCompilerA64::compile_typed_call_guard() {
	const Adaptor::InstNode &node = inst_context_->node;
	ZEND_ASSERT(node.argument_index != UINT32_MAX
		&& node.continuation_block != UINT32_MAX);
	const IRBlockRef cold{node.argument_index};
	const IRBlockRef hot{node.continuation_block};
	/* Observers are part of the system id (emit_observer_exit()). */
	if (node.operands.size() == node.materialization_count) {
		generate_uncond_branch(ZEND_OBSERVER_ENABLED ? cold : hot);
		return true;
	}
	const uint32_t guarded_operand_offset =
		1 + node.materialization_count;
	if (node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			|| guarded_operand_offset > node.operands.size()
			|| (node.operands.size() - guarded_operand_offset) % 2 != 0) {
		return false;
	}

	/*
	 * A guarded boxed argument owns the copy produced by its fast read.
	 * The typed hot call releases that copy after use.  If a runtime type
	 * or observer guard selects the canonical cold call instead, transfer
	 * the same ownership into the source frame slot consumed by that path.
	 */
	auto [frame_ref, frame] = val_ref_single(node.operands[0]);
	auto frame_reg = frame.load_to_reg();
	std::vector<ValueRef> guarded_values;
	std::vector<AsmReg> guarded_payload_regs;
	std::vector<AsmReg> guarded_type_regs;
	std::vector<uint32_t> guarded_expected_types;
	std::vector<uint32_t> guarded_offsets;
	const uint32_t guarded_count =
		static_cast<uint32_t>(
			(node.operands.size() - guarded_operand_offset) / 2);
	guarded_values.reserve(guarded_count);
	guarded_payload_regs.reserve(guarded_count);
	guarded_type_regs.reserve(guarded_count);
	guarded_expected_types.reserve(guarded_count);
	guarded_offsets.reserve(guarded_count);
	for (uint32_t operand = guarded_operand_offset;
			operand < node.operands.size(); operand += 2) {
		if (ir().machine_kind(node.operands[operand])
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
			return false;
		}
		uint64_t expected_type;
		if (!ir().constant(
				node.operands[operand + 1], &expected_type)
				|| expected_type > UINT32_MAX) {
			return false;
		}
		const zend_mir_storage_id storage_id =
			ir().canonical_storage(node.operands[operand]);
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage_id) * sizeof(zval);
		if (!zend_mir_id_is_valid(storage_id)
				|| offset > static_cast<uint64_t>(UINT32_MAX)
					- sizeof(zval)) {
			return false;
		}
		auto boxed = val_ref(node.operands[operand]);
		const ValueParts parts = val_parts(node.operands[operand]);
		int32_t payload_part = -1;
		int32_t type_part = -1;
		for (uint32_t part = 0; part < parts.count(); ++part) {
			if (parts.representation.parts[part].semantic_role
					== ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				payload_part = static_cast<int32_t>(part);
			} else if (parts.representation.parts[part].semantic_role
					== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
				type_part = static_cast<int32_t>(part);
			}
		}
		if (payload_part < 0 || type_part < 0) {
			return false;
		}
		guarded_payload_regs.push_back(
			boxed.part(static_cast<uint32_t>(payload_part)).load_to_reg());
		guarded_type_regs.push_back(
			boxed.part(static_cast<uint32_t>(type_part)).load_to_reg());
		guarded_expected_types.push_back(
			static_cast<uint32_t>(expected_type));
		guarded_offsets.push_back(static_cast<uint32_t>(offset));
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
		ASM(ANDwi, masked_type_reg,
			guarded_type_regs[index], Z_TYPE_MASK);
		ASM(CMPwi, masked_type_reg, guarded_expected_types[index]);
		generate_raw_jump(Jump::Jne, cold_transfer);
	}
	generate_raw_jump(Jump::jmp, hot_branch);

	label_place(cold_transfer);
	for (uint32_t index = 0; index < guarded_count; ++index) {
		store_off(frame_reg, guarded_offsets[index],
			guarded_payload_regs[index], 8);
		store_off(frame_reg,
			guarded_offsets[index]
				+ static_cast<uint32_t>(offsetof(zval, u1.type_info)),
			guarded_type_regs[index], 4);
	}
	generate_branch_to_block(
		Jump::jmp, cold, branch_needs_split(cold), false);
	label_place(hot_branch);
	generate_branch_to_block(Jump::jmp, hot, false, true);
	end_branch_region();
	release_spilled_regs(spilled);
	return true;
}

bool ZendCompilerA64::compile_string_length_value() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1 || !node.has_result
			|| ir().machine_kind(node.operands[0])
				!= ZEND_TPDE_MACHINE_VALUE_STRING_PTR
			|| ir().exact_type(node.result)
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

bool ZendCompilerA64::compile_scalar_select() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 3 || !node.has_result
			|| ir().exact_type(node.operands[0])
				!= ZEND_MIR_SCALAR_TYPE_I1
			|| ir().machine_kind(node.operands[0])
				!= ZEND_TPDE_MACHINE_VALUE_BOOL
			|| ir().exact_type(node.operands[1])
				!= ir().exact_type(node.result)
			|| ir().exact_type(node.operands[2])
				!= ir().exact_type(node.result)
			|| ir().machine_kind(node.operands[1])
				!= ir().machine_kind(node.result)
			|| ir().machine_kind(node.operands[2])
				!= ir().machine_kind(node.result)
			|| !zend_tpde_machine_kind_is_long_or_bool(
				ir().machine_kind(node.result))) {
		return false;
	}
	auto [condition_ref, condition] = val_ref_single(node.operands[0]);
	auto [true_ref, true_value] = val_ref_single(node.operands[1]);
	auto [false_ref, false_value] = val_ref_single(node.operands[2]);
	auto [result_ref, result] = result_ref_single(node.result);
	auto result_reg = result.alloc_reg();
	ASM(CMPwi, condition.load_to_reg(), 0);
	generate_raw_select(Jump::Jne, result_reg,
		true_value.load_to_reg(), false_value.load_to_reg(), true);
	result.set_modified();
	return true;
}

bool ZendCompilerA64::compile_box_scalar() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1 || !node.has_result
			|| ir().machine_kind(node.result)
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
		return false;
	}
	const zend_mir_scalar_type_mask input_type =
		ir().exact_type(node.operands[0]);
	if (!((input_type == ZEND_MIR_SCALAR_TYPE_I64
					&& ir().machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_I64)
				|| (input_type == ZEND_MIR_SCALAR_TYPE_I1
					&& ir().machine_kind(node.operands[0])
						== ZEND_TPDE_MACHINE_VALUE_BOOL)
				|| (input_type == ZEND_MIR_SCALAR_TYPE_F64
					&& ir().machine_kind(node.operands[0])
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

bool ZendCompilerA64::compile_unbox_scalar() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1 || !node.has_result
			|| ir().machine_kind(node.operands[0])
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			|| !zend_tpde_machine_kind_is_long_or_bool(
				ir().machine_kind(node.result))) {
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

bool ZendCompilerA64::compile_unbox_pointer() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_machine_value_kind result_kind =
		ir().machine_kind(node.result);
	if (node.operands.size() != 1 || !node.has_result
			|| ir().machine_kind(node.operands[0])
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			|| !zend_tpde_machine_kind_is_pointer(result_kind)) {
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

bool ZendCompilerA64::compile_unbox_reference_scalar() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1 || !node.has_result
			|| ir().machine_kind(node.operands[0])
				!= ZEND_TPDE_MACHINE_VALUE_REFERENCE_PTR
			|| ir().machine_kind(node.result)
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

bool ZendCompilerA64::compile_load_frame() {
	const Adaptor::InstNode &node = inst_context_->node;
	auto [source_ref, source] = val_ref_single(node.operands[0]);
	auto [result_ref, result] = result_ref_single(node.result);
	auto source_reg = source.load_to_reg();
	auto result_reg = result.alloc_reg();
	mov(result_reg, source_reg, 8);
	if (ir().plan()->entry_undef_temporary_count != 0) {
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
					< ir().plan()->entry_undef_temporary_count;
				++required) {
			const uint32_t index =
				ir().plan()->entry_undef_temporary_indices[
					required];
			const uint32_t offset = static_cast<uint32_t>(
				(uint64_t{ZEND_CALL_FRAME_SLOT}
					+ ir().plan()->source_frame_variable_count + index)
				* sizeof(zval));
			store_off(result_reg, offset + sizeof(uint64_t), zero_reg, 8);
		}
		label_place(initialized);
	}
	result.set_modified();
	return true;
}

bool ZendCompilerA64::compile_user_opcode_landing() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = ir().plan();
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

bool ZendCompilerA64::compile_user_opcode_dispatch() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.argument_index >= user_opcode_dispatch_labels_.size()) {
		return false;
	}
	label_place(user_opcode_dispatch_labels_[node.argument_index]);
	return true;
}

bool ZendCompilerA64::compile_user_opcode_gateway() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = ir().plan();
	const auto dispatch_sources =
		ir().user_opcode_dispatch_to_sources();
	std::vector<UserOpcodeDispatchCase> dispatch_cases;
	if (!plan_user_opcode_dispatch(
			node, user_opcode_labels_.size(), dispatch_cases)) {
		return false;
	}
	const uint32_t source_position = node.argument_index;
	const auto &next_landings =
		ir().user_opcode_next_landings();
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
	ASM(CMPwi, action_reg, ZEND_USER_OPCODE_CONTINUE);
	generate_raw_jump(Jump::Jeq, continued);
	ASM(CMPwi, action_reg, ZEND_USER_OPCODE_RETURN);
	generate_raw_jump(Jump::Jeq, return_action);
	ASM(CMPwi, action_reg, ZEND_USER_OPCODE_LEAVE);
	generate_raw_jump(Jump::Jeq, returned);
	ASM(CMPwi, action_reg, ZEND_USER_OPCODE_DISPATCH);
	generate_raw_jump(Jump::Jeq, dispatch);
	generate_raw_jump(Jump::jmp, dispatch_to);
	action.reset(this);
	label_place(continued);
	ASM(ADDwi, position_reg, position_reg, 1);
	for (uint32_t source = 0;
			source < user_opcode_labels_.size(); ++source) {
		if (next_landings[source] != source) {
			continue;
		}
		compare_position(position_reg, source);
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
		compare_position(position_reg, source);
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
		compare_position(position_reg, source);
		generate_raw_jump(Jump::Jne, next_candidate);
		ASM(CMPwi, selected_opcode_reg,
			plan->source_opcodes[source].opcode);
		generate_raw_jump(
			Jump::Jeq, user_opcode_dispatch_labels_[source]);
		label_place(next_candidate);
	}
	/* Per dispatch source, a label per target (plan_user_opcode_dispatch()). */
	std::vector<::tpde::Label> dispatch_labels;
	dispatch_labels.reserve(dispatch_cases.size());
	for (size_t source_index = 0;
			source_index < dispatch_sources.size(); ++source_index) {
		const uint32_t source = dispatch_sources[source_index];
		auto next_candidate = text_writer.label_create();
		compare_position(position_reg, source);
		generate_raw_jump(Jump::Jne, next_candidate);
		for (size_t target_index = 0;
				target_index < plan->user_opcode_target_count;
				++target_index) {
			const zend_tpde_user_opcode_target &target_case =
				plan->user_opcode_targets[target_index];
			auto target = text_writer.label_create();
			ASM(CMPwi, selected_opcode_reg, target_case.opcode);
			generate_raw_jump(Jump::Jeq, target);
			dispatch_labels.push_back(target);
		}
		label_place(next_candidate);
	}
	generate_raw_jump(Jump::jmp, exception);
	position.reset();
	selected_opcode.reset();
	UserOpcodeGatewayContext gateway{
		.plan = plan,
		.next_landings = next_landings,
		.exception = exception,
	};
	const ContextScope<UserOpcodeGatewayContext> gateway_scope{
		user_opcode_gateway_context_, &gateway};
	for (size_t case_index = 0; case_index < dispatch_cases.size();
			++case_index) {
		const UserOpcodeDispatchCase &dispatch_case =
			dispatch_cases[case_index];
		label_place(dispatch_labels[case_index]);
		emit_user_opcode_dispatch_case(dispatch_case);
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

/* The code at the dispatch label of one user opcode dispatch case. */
void ZendCompilerA64::emit_user_opcode_dispatch_case(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = user_opcode_gateway_context_->plan;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_NOP) {
		auto [frame_ref, frame] =
			val_ref_single(
				node.operands[dispatch_case.frame_operand]);
		frame.reset();
		jump_to_source(dispatch_case.source + 1);
		return;
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
		return;
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
			return;
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
		return;
	}
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_FINALLY_RETURN) {
		emit_user_opcode_finally_return(dispatch_case);
		return;
	}
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_CATCH) {
		emit_user_opcode_catch(dispatch_case);
		return;
	}
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_RECEIVE) {
		emit_user_opcode_receive(dispatch_case);
		return;
	}
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_CALL_FRAGMENT) {
		emit_user_opcode_call_fragment(dispatch_case);
		return;
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
			encode_source_operand(operation.op1), 8,
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
		return;
	}
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_THROW) {
		zend::native::tpde::CCAssignerAppleA64 throw_assigner;
		CallBuilder throw_call{*this, throw_assigner};
		throw_call.add_arg(
			CallArg{node.operands[dispatch_case.frame_operand]});
		throw_call.add_arg(ValuePart{
			encode_source_operand(operation.op1), 8,
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
		return;
	}
	if (dispatch_case.kind
			== ZEND_TPDE_USER_OPCODE_TARGET_MULTI_BRANCH) {
		emit_user_opcode_multi_branch(dispatch_case);
		return;
	}
	if (dispatch_case.kind
				== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_NEXT_OP2
			|| dispatch_case.kind
				== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_OP2
			|| dispatch_case.kind
				== ZEND_TPDE_USER_OPCODE_TARGET_BRANCH_END_EXTENDED) {
		emit_user_opcode_branch(dispatch_case);
		return;
	}
	emit_user_opcode_operation(dispatch_case);
}

/* Compares the user opcode position in position_reg with source. */
void ZendCompilerA64::compare_position(AsmReg position_reg,
		uint32_t source) {
	if (source <= UINT32_C(0xfff)) {
		compare_unsigned_immediate(position_reg, source);
	} else {
		ScratchReg constant{this};
		auto constant_reg = constant.alloc_gp();
		materialize_constant(
			source, DarwinConfig::GP_BANK, 4, constant_reg);
		ASM(CMPx, position_reg, constant_reg);
	}
}

/* Jumps to the landing of the user opcode at source, or to the
 * exception exit when it has none. */
void ZendCompilerA64::jump_to_source(uint32_t source) {
	const std::span<const uint32_t> &next_landings =
		user_opcode_gateway_context_->next_landings;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
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
}

/* FINALLY_RETURN: to the recorded continuation, or through the helper
 * to the handler of a pending exception. */
void ZendCompilerA64::emit_user_opcode_finally_return(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = user_opcode_gateway_context_->plan;
	const std::span<const uint32_t> &next_landings =
		user_opcode_gateway_context_->next_landings;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
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
		return;
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
		encode_source_operand(operation.op1), 8,
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
		ASM(CMPw, selected_reg, expected_reg);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::Jne, continued);
		jump_to_source(handler.source_position_id);
		label_place(continued);
	}
	selected.reset(this);
	generate_raw_jump(Jump::jmp, exception);
}

/* CATCH through the helper, to the catch or the next handler. */
void ZendCompilerA64::emit_user_opcode_catch(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = user_opcode_gateway_context_->plan;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
	zend::native::tpde::CCAssignerAppleA64 catch_assigner;
	CallBuilder catch_call{*this, catch_assigner};
	catch_call.add_arg(
		CallArg{node.operands[dispatch_case.frame_operand]});
	catch_call.add_arg(ValuePart{
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	catch_call.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
}

/* An argument receive through the helper. */
void ZendCompilerA64::emit_user_opcode_receive(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
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
		encode_source_operand(
			operation.op2, operation.op2_unused_payload),
		8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	receive_call.add_arg(ValuePart{
		operation.op2_unused_payload, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	receive_call.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
}

/* A call fragment through the helper. */
void ZendCompilerA64::emit_user_opcode_call_fragment(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
	zend::native::tpde::CCAssignerAppleA64 fragment_assigner;
	CallBuilder fragment_call{*this, fragment_assigner};
	fragment_call.add_arg(
		CallArg{node.operands[dispatch_case.frame_operand]});
	fragment_call.add_arg(ValuePart{
		dispatch_case.target_opcode, 4, DarwinConfig::GP_BANK},
		::tpde::CCAssignment{});
	fragment_call.add_arg(ValuePart{
		encode_source_operand(
			operation.op1, operation.op1_unused_payload),
		8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	fragment_call.add_arg(ValuePart{
		operation.op1_unused_payload, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	fragment_call.add_arg(ValuePart{
		encode_source_operand(
			operation.op2, operation.op2_unused_payload),
		8, DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	fragment_call.add_arg(ValuePart{
		operation.op2_unused_payload, 4,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	fragment_call.add_arg(ValuePart{
		encode_source_operand(
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
}

/* SWITCH_LONG, SWITCH_STRING or MATCH over the operand in the frame. */
void ZendCompilerA64::emit_user_opcode_multi_branch(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = user_opcode_gateway_context_->plan;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
	zend_tpde_user_multi_branch layout;
	if (!zend_tpde_user_multi_branch_at(
			plan, operation, dispatch_case.target_opcode,
			&layout)) {
		auto [frame_ref, frame] = val_ref_single(
			node.operands[dispatch_case.frame_operand]);
		frame.reset();
		generate_raw_jump(Jump::jmp, exception);
		return;
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
}

/* A branch whose helper picks the branch target or the next opcode. */
void ZendCompilerA64::emit_user_opcode_branch(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_plan *plan = user_opcode_gateway_context_->plan;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
	zend::native::tpde::CCAssignerAppleA64 branch_assigner;
	CallBuilder branch_call{*this, branch_assigner};
	branch_call.add_arg(
		CallArg{node.operands[dispatch_case.frame_operand]});
	branch_call.add_arg(ValuePart{
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	branch_call.add_arg(ValuePart{
		encode_source_operand(operation.op2), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	branch_call.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
}

/* Any other opcode through its value-operation helper. */
void ZendCompilerA64::emit_user_opcode_operation(
		const UserOpcodeDispatchCase &dispatch_case) {
	const Adaptor::InstNode &node = inst_context_->node;
	const std::span<const uint32_t> &next_landings =
		user_opcode_gateway_context_->next_landings;
	const ::tpde::Label exception = user_opcode_gateway_context_->exception;
	const zend_mir_executable_value_ref &operation =
		*dispatch_case.operation;
	const bool explicit_object_operands =
		zend_tpde_helper_has_unused_operand_payloads(
			dispatch_case.helper);
	const bool explicit_auxiliary =
		zend_tpde_helper_has_explicit_auxiliary(
			dispatch_case.helper);
	zend::native::tpde::CCAssignerAppleA64 assigner;
	CallBuilder operation_call{*this, assigner};
	operation_call.add_arg(
		CallArg{node.operands[dispatch_case.frame_operand]});
	operation_call.add_arg(ValuePart{
		encode_helper_operand(explicit_object_operands,
			operation.op1, operation.op1_unused_payload), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	operation_call.add_arg(ValuePart{
		encode_helper_operand(explicit_object_operands,
			operation.op2, operation.op2_unused_payload), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	operation_call.add_arg(ValuePart{
		encode_helper_operand(explicit_object_operands,
			operation.result, operation.result_unused_payload), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	if (explicit_auxiliary) {
		operation_call.add_arg(ValuePart{
			encode_helper_operand(explicit_object_operands,
				operation.auxiliary,
				operation.auxiliary_unused_payload)
				| zend_tpde_auxiliary_cache_bits(
					dispatch_case.helper, operation), 8,
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
	ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
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
		RetBuilder return_builder{*this, *cur_cc_assigner()};
		return_builder.add(
			std::move(status), ::tpde::CCAssignment{});
		return_builder.ret();
	}
	label_place(completed);
	if (ir().user_opcode_result_reload_source(
				dispatch_case.source)
			&& dispatch_case.source
				< user_opcode_result_reload_labels_.size()) {
		generate_raw_jump(Jump::jmp,
			user_opcode_result_reload_labels_[
				dispatch_case.source]);
		return;
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

bool ZendCompilerA64::compile_user_call() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_source_call_phase_entry *phase =
		ir().source_call_phase(node);
	if (phase == nullptr) {
		return false;
	}
	const bool send_phase =
		node.kind == Adaptor::InstKind::UserCallSend;
	auto frame_liveness = val_ref(node.operands[0]);
	auto context_liveness = val_ref(node.operands[1]);
	/*
	 * Source-call phases contain target-local fast/slow branches which are
	 * invisible to TPDE's IR CFG.  Spill every live non-fixed assignment
	 * before either path can clobber it, so the join reloads the same
	 * canonical value on both paths.
	 */
	(void) spill_before_branch(true);
	UserCallContext user_call{phase, context_liveness};
	const ContextScope<UserCallContext> user_call_scope{
		user_call_context_, &user_call};
	if (node.kind == Adaptor::InstKind::UserCallInit) {
		return compile_user_call_init();
	}
	const zend_tpde_instruction &call =
		ir().plan()->instructions[node.mir_instruction_index];
	if (call.user_call == nullptr
			|| call.user_call->argument_count != call.call_argument_count) {
		return false;
	}
	user_call.call = &call;
	if (node.kind == Adaptor::InstKind::UserCallSend) {
		return compile_user_call_send();
	}
	if (node.kind == Adaptor::InstKind::UserCallCheck) {
		return compile_user_call_check();
	}
	if (node.kind == Adaptor::InstKind::UserCallExpand) {
		return compile_user_call_expand();
	}
	if (node.kind != Adaptor::InstKind::UserCallDo) {
		return false;
	}
	return compile_user_call_do();
}

auto ZendCompilerA64::context_register() {
	ValueRef &context_liveness = user_call_context_->context_liveness;
	auto context = context_liveness.part_unowned(0);
	auto reg = context.load_to_reg();
	context.reset();
	return reg;
}

auto ZendCompilerA64::context_argument() {
	ValueRef &context_liveness = user_call_context_->context_liveness;
	auto context = context_liveness.part_unowned(0);
	auto argument = copy_fixed_argument(context.load_to_reg());
	context.reset();
	return argument;
}

template <typename Reg>
void ZendCompilerA64::load_active_activation(Reg destination_reg) {
	load_off(destination_reg,
		context_register(),
		static_cast<uint32_t>(offsetof(
			zend_native_execution_context, active_direct_call)), 8);
	load_off(destination_reg, destination_reg, 0, 8);
}

void ZendCompilerA64::emit_phase_failure() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = *user_call_context_->call;
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
}

bool ZendCompilerA64::compile_user_call_init() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call =
		ir().plan()->instructions[node.mir_instruction_index];
	if (call.user_call == nullptr) {
		return false;
	}
	zend_tpde_user_call_setup setup;
	if (!zend_tpde_user_call_setup_layout(ir().plan(),
			call.user_call, UINT32_MAX, &setup)) {
		return false;
	}
	user_call_context_->setup = &setup;
	const uint64_t setup_size = setup.setup_size;

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
		initialize_user_call_setup(setup_reg);
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
		initialize_user_call_setup(setup_scratch.cur_reg());
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
		initialize_user_call_callee(callee_reg, activation_reg, false);
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
		initialize_user_call_callee(
			callee_scratch.cur_reg(), activation_reg, true);
	}
	label_place(callee_ready);
	reconcile_target_branch_state(callee_spilled);
	return true;
}

/* Initializes the activation behind the setup frame at setup_reg and
 * makes it the active direct call. */
void ZendCompilerA64::initialize_user_call_setup(AsmReg setup_reg) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call =
		ir().plan()->instructions[node.mir_instruction_index];
	const zend_tpde_user_call_setup &setup = *user_call_context_->setup;
	const uint64_t argument_count = setup.argument_count;
	const uint64_t activation_offset = setup.activation_offset;
	const uint64_t placement_offset = setup.placement_offset;
	const uint64_t setup_size = setup.setup_size;
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
		ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.record.id);
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
}

/* Initializes the callee frame at callee_reg from the resolution of the
 * activation at activation_reg; allocated marks a frame the runtime
 * reserved outside the VM stack page. */
void ZendCompilerA64::initialize_user_call_callee(AsmReg callee_reg,
		AsmReg activation_reg, bool allocated) {
	const zend_tpde_user_call_setup &setup = *user_call_context_->setup;
	const uint64_t frame_header_size = setup.frame_header_size;
	const bool uses_discarded_return = setup.uses_discarded_return;
	const uint64_t result_offset = setup.result_offset;
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
}

bool ZendCompilerA64::compile_user_call_send() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_source_call_phase_entry *&phase =
		user_call_context_->phase;
	const zend_tpde_instruction &call = *user_call_context_->call;
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
		if (ir().exact_type(node.operands[2])
				== ZEND_MIR_SCALAR_TYPE_F64) {
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_USER_CALL_SET_DOUBLE));
		} else {
			builder.add_arg(ValuePart{
				static_cast<uint32_t>(
					ir().exact_type(node.operands[2])),
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
			ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.record.id),
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

bool ZendCompilerA64::compile_user_call_check() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = *user_call_context_->call;
	if (node.source_position >= ir().plan()->source_opcode_count) {
		return false;
	}
	const uint8_t source_opcode =
		ir().plan()->source_opcodes[node.source_position].opcode;
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
	/* The failure path freed checked_reg: keep the decision in a
	 * register the scratch registers below cannot reuse. */
	ScratchReg decision{this};
	auto decision_reg = decision.alloc_gp();
	if (decision_reg != checked_reg) {
		ASM(MOVw, decision_reg, checked_reg);
	}
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
	ASM(CMPwi, decision_reg, ZEND_NATIVE_CHECK_FUNC_ARG_BY_VALUE);
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

bool ZendCompilerA64::compile_user_call_expand() {
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

bool ZendCompilerA64::compile_user_call_do() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = *user_call_context_->call;

	/* Publish the canonical DO source position before any DO transition. */
	{
		ScratchReg opline{this};
		auto opline_reg = opline.alloc_gp();
		emit_publish_opline(CanonicalFrame{}, opline_reg,
			static_cast<uint64_t>(node.source_position) * sizeof(zend_op),
			true);
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
			ZEND_NATIVE_IMAGE_SYMBOL_USER_CALL_DESCRIPTOR, call.record.id),
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
	release_active_activation();
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
		release_active_activation();
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
		release_active_activation();
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
				ir().plan()->source_frame_variable_count)
				+ result_operand.index;
		const uint64_t offset =
			(uint64_t{ZEND_CALL_FRAME_SLOT} + storage) * sizeof(zval);
		if (offset > UINT32_MAX - sizeof(zval)) {
			return false;
		}
		if (ir().machine_kind(node.result)
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
			if (ir().machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_F64) {
				ScratchReg payload{this};
				auto payload_reg = payload.alloc_gp();
				load_off(payload_reg, canonical_frame_register(),
					static_cast<uint32_t>(offset), 8);
				ASM(FMOVdx, result_reg, payload_reg);
			} else if (ir().machine_kind(node.result)
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

/* Releases the active activation of the user call. */
void ZendCompilerA64::release_active_activation() {
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

/* After the active activation is released: a pending exception goes to
 * the call's handler or returns. */
void ZendCompilerA64::branch_released_exception() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = *user_call_context_->call;
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
}

bool ZendCompilerA64::compile_user_opcode_call_fragment() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1
			|| node.mir_instruction_index >=
				ir().plan()->instruction_count) {
		return false;
	}
	const zend_tpde_instruction &call =
		ir().plan()->instructions[node.mir_instruction_index];
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
		call.record.id), ::tpde::CCAssignment{});
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

bool ZendCompilerA64::compile_generator_gateway() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 2
			|| node.operands[0] != IRValueRef{Adaptor::FRAME_VALUE}
			|| node.operands[1]
				!= IRValueRef{Adaptor::EXECUTION_CONTEXT_ARGUMENT}
			|| ir().generator_resume_targets().empty()) {
		return false;
	}
	while (generator_resume_labels_.size()
			< ir().generator_resume_targets().size()) {
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
				index < ir().generator_resume_targets().size(); ++index) {
			const uint64_t byte_offset =
				uint64_t{ir().generator_resume_targets()[index]}
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
				index < ir().generator_resume_targets().size(); ++index) {
			const uint64_t byte_offset =
				uint64_t{ir().generator_resume_targets()[index] - 1}
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
				ir().generator_resume_exception_blocks()[index];
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

bool ZendCompilerA64::compile_generator_resume() {
	IRInstRef &instruction = inst_context_->instruction;
	const Adaptor::InstNode &node = inst_context_->node;
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
	/* Lock the frame assignment across the reloads, as in
	 * emit_materializations(). */
	const auto frame_local = adaptor->val_local_idx(
		IRValueRef{Adaptor::FRAME_VALUE});
	auto *frame_assignment = val_assignment(frame_local);
	ZEND_ASSERT(frame_assignment != nullptr);
	ValuePartRef frame_value{
		this, frame_local, frame_assignment, 0, false};
	auto frame_reg = frame_value.load_to_reg();
	return reload_generator_resume_values(instruction, frame_reg, true,
		[](ValueRef &&) {});
}

bool ZendCompilerA64::compile_zval_guard_arguments() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1
			|| node.operands[0]
				!= IRValueRef{Adaptor::FRAME_VALUE}
			|| ir().argument_guards().empty()) {
		return false;
	}
	auto [frame_ref, frame] = val_ref_single(node.operands[0]);
	auto frame_reg = frame.load_to_reg();
	auto mismatch = text_writer.label_create();
	for (const Adaptor::ArgumentGuard &guard :
			ir().argument_guards()) {
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

bool ZendCompilerA64::compile_zval_reference_resolve() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1) {
		return false;
	}
	zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
	const bool frame_slot = ir().frame_slot_reference(
		node.operands[0], &storage_id);
	const uint64_t frame_offset = frame_slot
		? frame_slot_offset(storage_id)
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

bool ZendCompilerA64::compile_zval_boxed_store() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 2
			|| node.operands[1] != IRValueRef{Adaptor::FRAME_VALUE}
			|| ir().machine_kind(node.operands[0])
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			|| !zend_mir_id_is_valid(node.storage_id)) {
		return false;
	}
	const uint64_t offset = frame_slot_offset(node.storage_id);
	if (offset > UINT32_MAX - sizeof(zval)) {
		return false;
	}
	auto boxed = val_ref(node.operands[0]);
	auto payload = boxed.part(0);
	auto type_info = boxed.part(1);
	auto [frame_ref, frame] = val_ref_single(node.operands[1]);
	auto frame_reg = frame.load_to_reg();
	store_boxed_part(frame_reg, payload, static_cast<uint32_t>(offset), 8);
	store_boxed_part(frame_reg, type_info,
		static_cast<uint32_t>(offset + offsetof(zval, u1.type_info)), 4);
	return true;
}

bool ZendCompilerA64::compile_zval_payload_load() {
	const Adaptor::InstNode &node = inst_context_->node;
	if (node.operands.size() != 1
			|| ((!zend_mir_scalar_type_is_exact(node.exact_type)
					|| node.exact_type == ZEND_MIR_SCALAR_TYPE_NULL)
				&& zend_tpde_machine_value_zval_type(
					ir().machine_kind(node.result))
					== IS_UNDEF
				&& ir().machine_kind(node.result)
					!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL)) {
		return false;
	}
	const zend_tpde_machine_value_kind result_kind =
		ir().machine_kind(node.result);
	zend_mir_storage_id storage_id = ZEND_MIR_ID_INVALID;
	const bool frame_slot = ir().frame_slot_reference(
		node.operands[0], &storage_id);
	const uint64_t frame_offset = frame_slot
		? frame_slot_offset(storage_id)
		: 0;
	if (frame_offset > static_cast<uint64_t>(INT64_MAX)
			- sizeof(zval)) {
		return false;
	}
	if (result_kind != ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
			&& (node.exact_type == ZEND_MIR_SCALAR_TYPE_I64
			|| zend_tpde_machine_kind_is_pointer(result_kind))) {
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

bool ZendCompilerA64::compile_zval_store() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
		ir().exact_type(input);
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
			store_off(store_slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				store_type_reg, 4);
		} else if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
			auto input = input_value.part(0);
			store_bool_slot(store_slot_reg, 0, input.load_to_reg(),
				store_type, true, BoolTypeInfo::MATERIALIZE_ADD);
		} else {
			auto input = input_value.part(0);
			auto input_reg = input.load_to_reg();
			store_off(store_slot_reg, 0, input_reg, 8);
			materialize_constant(
				zval_type(exact_type),
				DarwinConfig::GP_BANK, 4, store_type_reg);
			store_off(store_slot_reg,
				static_cast<uint32_t>(offsetof(zval, u1.type_info)),
				store_type_reg, 4);
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
	} else if (exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
		auto [value_ref, value] = val_ref_single(input);
		ScratchReg kind{this};
		store_bool_slot(store_slot_reg, 0, value.load_to_reg(), kind, true,
			BoolTypeInfo::MATERIALIZE_ADD);
	} else {
		auto [value_ref, value] = val_ref_single(input);
		auto value_reg = value.load_to_reg();
		store_off(store_slot_reg, 0, value_reg, 8);
		ScratchReg kind{this};
		auto kind_reg = kind.alloc_gp();
		materialize_constant(
			zval_type(exact_type), DarwinConfig::GP_BANK, 4, kind_reg);
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

bool ZendCompilerA64::compile_echo_scalar() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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

bool ZendCompilerA64::compile_guarded_cold() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
	auto materialize_cold_operand = [&](
			IRValueRef operand, zend_mir_storage_id storage) {
		if (!zend_mir_id_is_valid(storage)
				|| zend_tpde_reference_cv_operand_slot(mir, storage)) {
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
			ir().machine_kind(operand);
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
			if (part_value.has_assignment()
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
			&& ir().user_opcode_result_reload_source(
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
					? !((ir().representation(node.result)
								== ZEND_MIR_REPRESENTATION_I64
							&& ir().exact_type(node.result)
								== ZEND_MIR_SCALAR_TYPE_I64
							&& ir().machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_I64)
						|| (ir().representation(node.result)
								== ZEND_MIR_REPRESENTATION_ZVAL
							&& ir().machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))
					: !((ir().representation(node.result)
									== ZEND_MIR_REPRESENTATION_I64
								&& ir().exact_type(node.result)
									== ZEND_MIR_SCALAR_TYPE_I64
								&& ir().machine_kind(node.result)
									== ZEND_TPDE_MACHINE_VALUE_I64)
							|| (ir().representation(node.result)
									== ZEND_MIR_REPRESENTATION_I1
								&& ir().exact_type(node.result)
									== ZEND_MIR_SCALAR_TYPE_I1
								&& ir().machine_kind(node.result)
									== ZEND_TPDE_MACHINE_VALUE_BOOL)
							|| ir().machine_kind(node.result)
								== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL))) {
			return false;
		}
		if (zend_tpde_machine_kind_is_long_or_bool(
				ir().machine_kind(node.result))) {
			auto [result_ref, result] =
				result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			load_slot_scalar(result_reg,
				ir().machine_kind(node.result), frame_offset);
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

bool ZendCompilerA64::compile_func_num_args() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
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
	if (ir().machine_kind(node.result)
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

bool ZendCompilerA64::compile_value_multi_branch() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
	zend_tpde_multi_branch layout;
	if (!zend_tpde_multi_branch_at(
			ir().plan(), mir, record, &layout)
			|| node.operands.size() != 1) {
		return false;
	}
	const zend_tpde_plan *plan = ir().plan();
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

bool ZendCompilerA64::compile_value_cond_branch() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
	const bool register_machine_condition =
		record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
		&& node.operands.size() == 1
		&& node.operands[0]
			!= IRValueRef{Adaptor::FRAME_VALUE}
		&& zend_tpde_machine_kind_is_long_or_bool(
			ir().machine_kind(node.operands[0]));
	const bool register_boxed_condition =
		record.opcode == ZEND_MIR_OPCODE_VALUE_COND_BRANCH
		&& node.operands.size() == 2
		&& node.operands[0]
			== IRValueRef{Adaptor::FRAME_VALUE}
		&& ir().machine_kind(node.operands[1])
			== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL;
	if ((node.operands.size() != 1 && !register_boxed_condition)
			|| !mir.has_value_operation) {
		return false;
	}
	if (record.opcode == ZEND_MIR_OPCODE_ITERATOR_BRANCH) {
		zend_tpde_array_iterator_reset reset_layout;

		if (zend_tpde_array_iterator_reset_at(mir, &reset_layout)) {
			return compile_iterator_reset_branch(reset_layout);
		}

		zend_tpde_packed_iterator_fetch layout;

		if (zend_tpde_packed_iterator_fetch_at(mir, &layout, true)) {
			return compile_packed_iterator_branch(layout);
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
			return compile_value_truthiness_branch(layout,
				register_boxed_condition);
		}
	}
	zend::native::tpde::CCAssignerAppleA64 assigner;
	CallBuilder builder{*this, assigner};
	builder.add_arg(CallArg{node.operands[0]});
	const zend_mir_executable_value_ref &operation =
		mir.value_operation;
	builder.add_arg(ValuePart{
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.op2), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
	ASM(CMPwi, decision_reg, ZEND_NATIVE_ITERATOR_EXCEPTION);
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
			(ir().representation(node.result)
					== ZEND_MIR_REPRESENTATION_I64
				&& ir().exact_type(node.result)
					== ZEND_MIR_SCALAR_TYPE_I64
				&& ir().machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_I64)
			|| (ir().representation(node.result)
					== ZEND_MIR_REPRESENTATION_I1
				&& ir().exact_type(node.result)
					== ZEND_MIR_SCALAR_TYPE_I1
				&& ir().machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOOL);
		const zend_tpde_machine_value_kind result_kind =
			ir().machine_kind(node.result);
		const bool pointer_result =
			ir().representation(node.result)
					== ZEND_MIR_REPRESENTATION_SEMANTIC_POINTER
			&& zend_tpde_machine_kind_is_pointer(result_kind);
		if (!zend_mir_id_is_valid(storage)
				|| frame_offset > UINT32_MAX - sizeof(zval)
				|| (!scalar_result && !pointer_result)) {
			return false;
		}
		auto [result_ref, result] =
			result_ref_single(node.result);
		auto result_reg = result.alloc_reg();
		load_slot_scalar(result_reg, result_kind, frame_offset);
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

/* An iterator reset: a mutable array in its frame slot is held inline,
 * the helper resets anything else. */
bool ZendCompilerA64::compile_iterator_reset_branch(
		const zend_tpde_array_iterator_reset &reset_layout) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
	emit_gc_addref(array_reg, refcount_reg);
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
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.op2), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
	ASM(CMPwi, decision_reg,
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

/* An iterator fetch over the held array: the next element is copied
 * inline unless a destructor or the GC must run, else the helper fetches. */
bool ZendCompilerA64::compile_packed_iterator_branch(
		const zend_tpde_packed_iterator_fetch &layout) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	if (node.has_result
			&& !((ir().machine_kind(node.result)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& val_parts(node.result).count() == 2)
				|| (layout.destination_scalar_only
					&& ir().machine_kind(node.result)
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
	auto packed = text_writer.label_create();
	auto packed_scan = text_writer.label_create();
	auto mixed_scan = text_writer.label_create();
	auto found = text_writer.label_create();
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
	ScratchReg value_type{this};
	ScratchReg key{this};
	ScratchReg key_value{this};
	auto type_reg = type.alloc_gp();
	auto array_reg = array.alloc_gp();
	auto position_reg = position.alloc_gp();
	auto limit_reg = limit.alloc_gp();
	auto element_reg = element.alloc_gp();
	auto value_reg = value.alloc_gp();
	auto value_type_reg = value_type.alloc_gp();
	auto key_reg = key.alloc_gp();
	auto key_value_reg = key_value.alloc_gp();

	load_frame_array(frame_reg, layout.holder_offset, type_reg,
		array_reg, slow);
	load_off(limit_reg, array_reg,
		static_cast<uint32_t>(
			offsetof(HashTable, nNumUsed)),
		4);
	load_off(position_reg, frame_reg,
		layout.holder_offset
			+ static_cast<uint32_t>(offsetof(zval, u2.fe_pos)),
		4);
	load_off(type_reg, array_reg,
		static_cast<uint32_t>(offsetof(HashTable, u)), 4);
	ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
	generate_raw_jump(Jump::Jne, packed);

	label_place(mixed_scan);
	ASM(CMPw, position_reg, limit_reg);
	generate_raw_jump(Jump::Jhs, end);
	load_off(element_reg, array_reg,
		static_cast<uint32_t>(offsetof(HashTable, arData)), 8);
	ASM(ADDx_lsl, element_reg, element_reg, position_reg, 5);
	load_off(type_reg, element_reg,
		static_cast<uint32_t>(
			offsetof(Bucket, val)
				+ offsetof(zval, u1.type_info)),
		4);
	ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
	ASM(CMPwi, type_reg, IS_UNDEF);
	generate_raw_jump(Jump::Jne, found);
	ASM(ADDwi, position_reg, position_reg, 1);
	generate_raw_jump(Jump::jmp, mixed_scan);

	label_place(packed);
	label_place(packed_scan);
	ASM(CMPw, position_reg, limit_reg);
	generate_raw_jump(Jump::Jhs, end);
	load_off(element_reg, array_reg,
		static_cast<uint32_t>(offsetof(HashTable, arPacked)), 8);
	ASM(ADDx_lsl, element_reg, element_reg, position_reg, 4);
	load_off(type_reg, element_reg,
		static_cast<uint32_t>(offsetof(zval, u1.type_info)), 4);
	ASM(ANDwi, type_reg, type_reg, Z_TYPE_MASK);
	ASM(CMPwi, type_reg, IS_UNDEF);
	generate_raw_jump(Jump::Jne, found);
	ASM(ADDwi, position_reg, position_reg, 1);
	generate_raw_jump(Jump::jmp, packed_scan);

	label_place(found);

	load_off(value_type_reg, element_reg,
		static_cast<uint32_t>(
			offsetof(zval, u1.type_info)),
		4);
	ASM(ANDwi, type_reg, value_type_reg, Z_TYPE_MASK);
	ASM(CMPwi, type_reg, IS_UNDEF);
	generate_raw_jump(Jump::Jeq, slow);
	ASM(CMPwi, type_reg, IS_REFERENCE);
	generate_raw_jump(Jump::Jeq, slow);
	if (layout.destination_scalar_only) {
		ASM(CMPwi, type_reg, IS_LONG);
		generate_raw_jump(Jump::Jne, slow);
	}
	load_off(value_reg, element_reg, 0, 8);

	if (!layout.destination_scalar_only) {
		/*
		 * The inline ownership transition is deliberately limited
		 * to mutable, non-collectable payloads.  Collectable values
		 * require the GC protocol in the semantic helper, while an
		 * immutable payload must never receive a refcount write.
		 */
		auto value_source_valid = text_writer.label_create();
		ASM(TSTwi, value_type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, value_source_valid);
		ASM(TSTwi, value_type_reg,
			IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		load_off(type_reg, value_reg,
			static_cast<uint32_t>(offsetof(
				zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, type_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::Jne, slow);
		label_place(value_source_valid);
	}

	if (layout.has_key) {
		load_off(type_reg, array_reg,
			static_cast<uint32_t>(offsetof(HashTable, u)), 4);
		ASM(TSTwi, type_reg, HASH_FLAG_PACKED);
		auto packed_key = text_writer.label_create();
		auto key_ready = text_writer.label_create();
		generate_raw_jump(Jump::Jne, packed_key);
		load_off(key_reg, element_reg,
			static_cast<uint32_t>(offsetof(Bucket, key)), 8);
		load_off(key_value_reg, element_reg,
			static_cast<uint32_t>(offsetof(Bucket, h)), 8);
		generate_raw_jump(Jump::jmp, key_ready);
		label_place(packed_key);
		materialize_constant(uint64_t{0},
			DarwinConfig::GP_BANK, 8, key_reg);
		ASM(ORRx, key_value_reg,
			position_reg, position_reg);
		label_place(key_ready);
	}

	if (!layout.destination_scalar_only) {
		/*
		 * Reusing the foreach CV is cheap for scalars and for
		 * shared non-collectable values.  Destruction of a
		 * collectable value, or of the final reference, retains
		 * the semantic helper because it must run the GC/dtor
		 * protocol before any observable iterator mutation.
		 */
		load_off(type_reg, frame_reg,
			layout.destination_offset
				+ static_cast<uint32_t>(
					offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, destination_valid);
		ASM(TSTwi, type_reg,
			IS_TYPE_COLLECTABLE << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jne, slow);
		load_off(array_reg, frame_reg,
			layout.destination_offset, 8);
		load_off(type_reg, array_reg,
			static_cast<uint32_t>(offsetof(
				zend_refcounted_h, u.type_info)), 4);
		ASM(TSTwi, type_reg, GC_IMMUTABLE);
		generate_raw_jump(Jump::Jne, slow);
		load_off(type_reg, array_reg,
			static_cast<uint32_t>(
				offsetof(zend_refcounted_h, refcount)), 4);
		ASM(CMPwi, type_reg, 1);
		generate_raw_jump(Jump::Jls, slow);
		label_place(destination_valid);
	}

	/*
	 * FE_FETCH_R produces its optional key in a dead TMP/VAR.
	 * The bytes in that physical slot do not own a previous
	 * value and therefore must neither be inspected nor released.
	 *
	 * All semantic fallback guards precede the ownership and
	 * iterator mutations below. Acquire the new value and key
	 * before releasing aliases held by the reused destination CV.
	 */
	auto value_owned = text_writer.label_create();
	ASM(TSTwi, value_type_reg,
		IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
	generate_raw_jump(Jump::Jeq, value_owned);
	emit_gc_addref(value_reg, type_reg);
	label_place(value_owned);
	if (layout.has_key) {
		auto numeric_key = text_writer.label_create();
		ASM(CMPxi, key_reg, 0);
		generate_raw_jump(Jump::Jeq, numeric_key);
		emit_pointer_addref(
			ZEND_TPDE_MACHINE_VALUE_STRING_PTR, key_reg);
		label_place(numeric_key);
	}

	if (!layout.destination_scalar_only) {
		auto destination_released = text_writer.label_create();
		load_off(type_reg, frame_reg,
			layout.destination_offset
				+ static_cast<uint32_t>(
					offsetof(zval, u1.type_info)),
			4);
		ASM(TSTwi, type_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, destination_released);
		load_off(array_reg, frame_reg,
			layout.destination_offset, 8);
		emit_gc_delref(array_reg, type_reg);
		label_place(destination_released);
	}
	/* Publish the fully validated iterator step. */
	ASM(ADDwi, position_reg, position_reg, 1);
	store_off(frame_reg,
		layout.holder_offset
			+ static_cast<uint32_t>(offsetof(zval, u2.fe_pos)),
		position_reg, 4);
	store_off(frame_reg, layout.destination_offset,
		value_reg, 8);
	store_off(frame_reg,
		layout.destination_offset
			+ static_cast<uint32_t>(
				offsetof(zval, u1.type_info)),
		value_type_reg, 4);
	if (layout.has_key) {
		auto string_key = text_writer.label_create();
		auto key_stored = text_writer.label_create();
		ASM(CMPxi, key_reg, 0);
		generate_raw_jump(Jump::Jne, string_key);
		store_off(frame_reg, layout.key_offset,
			key_value_reg, 8);
		materialize_constant(static_cast<uint64_t>(IS_LONG),
			DarwinConfig::GP_BANK, 4, type_reg);
		store_off(frame_reg,
			layout.key_offset
				+ static_cast<uint32_t>(
					offsetof(zval, u1.type_info)),
			type_reg, 4);
		generate_raw_jump(Jump::jmp, key_stored);
		label_place(string_key);
		store_off(frame_reg, layout.key_offset, key_reg, 8);
		emit_machine_zval_type_info(
			ZEND_TPDE_MACHINE_VALUE_STRING_PTR,
			key_reg, type_reg);
		store_off(frame_reg,
			layout.key_offset
				+ static_cast<uint32_t>(
					offsetof(zval, u1.type_info)),
			type_reg, 4);
		label_place(key_stored);
	}
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
	value_type.reset();
	key.reset();
	key_value.reset();
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
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.op2), 8,
		DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
	ASM(CMPwi, decision_reg,
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
		if (ir().machine_kind(node.result)
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

/* A value condition decided by the inline truthiness ladder, with
 * the helper for the other types and for result-producing jumps. */
bool ZendCompilerA64::compile_value_truthiness_branch(
		const zend_tpde_value_condition &layout,
		bool register_boxed_condition) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
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
	emit_truthiness_ladder(type_reg, value_reg, frame_reg,
		layout.operand_offset,
		register_boxed_condition
			? std::optional<AsmReg>{boxed_payload_reg}
			: std::nullopt,
		truthy, falsey, slow, false);

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
		encode_source_operand(operation.op1), 8,
		DarwinConfig::GP_BANK},
		::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.op2), 8,
		DarwinConfig::GP_BANK},
		::tpde::CCAssignment{});
	builder.add_arg(ValuePart{
		encode_source_operand(operation.result), 8,
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
	ASM(CMPwi, decision_reg,
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

bool ZendCompilerA64::compile_call_direct_user() {
	IRInstRef &instruction = inst_context_->instruction;
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call =
		ir().mir_instruction(instruction);
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
				&& ir().typed_component_call(instruction)
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
		if (ir().typed_body()
				&& typed_body_function != UINT32_MAX) {
			if (typed_body_function >= this->func_syms.size()
					|| node.operands.size() != argument_count) {
				return false;
			}
			if (!emit_typed_body_call(node, typed_body_function,
					call.component_target_index, argument_count,
					true)) {
				return false;
			}
			adaptor->mark_typed_body_call(
				call.direct_call->frame_size);
			return true;
		}
		DirectUserCallShape shape;
		if (!direct_user_call_shape(node, call, generated_fast_path,
				private_inline_body, typed_body_function, argument_count,
				shape)) {
			return false;
		}
		const bool typed_body_call = shape.typed_body_call;
		auto slow_path = text_writer.label_create();
		auto successful = text_writer.label_create();
		DirectUserCallContext direct_call{
			.call = call,
			.shape = shape,
			.local_component_call = local_component_call,
			.generated_fast_path = generated_fast_path,
			.private_inline_body = private_inline_body,
			.typed_body_function = typed_body_function,
			.result_unused = result_unused,
			.generation_leased = generation_leased,
			.argument_count = argument_count,
			.slow_path = slow_path,
			.successful = successful,
		};
		const ContextScope<DirectUserCallContext> direct_call_scope{
			direct_user_call_context_, &direct_call};
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& !typed_body_call) {
			auto spilled = spill_before_branch();
			release_spilled_regs(spilled);
		}
		if (node.kind == Adaptor::InstKind::GuardedFast
				&& node.inlined_user_body) {
			return compile_direct_user_inlined_body();
		}
		if (typed_body_call) {
			return compile_direct_user_typed_body();
		}
		if (generated_fast_path
				&& node.kind != Adaptor::InstKind::GuardedCold) {
			if (private_inline_body) {
				const std::optional<bool> leaf_result =
					compile_direct_user_leaf_path();
				if (leaf_result.has_value()) {
					return *leaf_result;
				}
			} else if (!compile_direct_user_frame_path()) {
				return false;
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
		return compile_direct_user_slow_call();
	}
	if (call.user_call != nullptr
			&& call.user_call->do_opcode != ZEND_CALLABLE_CONVERT
			&& call.user_call->do_opcode
				!= ZEND_CALLABLE_CONVERT_PARTIAL) {
		/* The adaptor emits a call without a direct-call descriptor as
		 * source-call fragments; it never reaches this node. */
		ZEND_ASSERT(0);
		return false;
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
		call.record.id), ::tpde::CCAssignment{});
	for (IRValueRef operand : node.operands) {
		auto liveness = val_ref(operand);
		(void) liveness;
	}
	builder.call(runtime_symbol(
		ZEND_NATIVE_HELPER_CALL_CONVERT_EXPLICIT));
	ValuePart status{DarwinConfig::GP_BANK, 4};
	builder.add_ret(status, ::tpde::CCAssignment{});
	auto status_reg = status.cur_reg_or_load(this);
	ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
	auto continued = text_writer.label_create();
	generate_raw_jump(Jump::Jeq, continued);
	if (zend_mir_id_is_valid(call.exception_block_id)) {
		auto propagate = text_writer.label_create();
		ASM(CMPwi, status_reg, ZEND_NATIVE_EXCEPTION);
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
				encode_source_operand(call.call_site->result_operand), 8,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
		result_builder.add_arg(ValuePart{
			static_cast<uint32_t>(ir().exact_type(node.result)), 4,
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

/* Whether the callee may use its compiled variable variable_index. */
bool ZendCompilerA64::compiled_variable_used(uint32_t variable_index) {
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool local_component_call =
		direct_user_call_context_->local_component_call;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t owned_argument_variable_count =
		shape.owned_argument_variable_count;
	return variable_index < owned_argument_variable_count
		|| !local_component_call
		|| ir().component_compiled_variable_used(
			call.component_target_index,
			variable_index);
}

/* Defines the node's result from the callee's result zval in the frame
 * at result_frame_reg. */
void ZendCompilerA64::load_generated_result(AsmReg result_frame_reg) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	if (node.has_result) {
		ScratchReg result_slot{this};
		auto result_slot_reg = result_slot.alloc_gp();
		mov(result_slot_reg, result_frame_reg, 8);
		if (call.direct_call->result_operand.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV) {
			add_imm12_offset(result_slot_reg, result_slot_reg,
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
		if (ir().machine_kind(node.result)
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
			if (ir().exact_type(node.result)
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
}

/* Defines the result of a generated fast call and frees the leaf
 * path's stack slots. */
void ZendCompilerA64::finish_generated_result() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool private_inline_body =
		direct_user_call_context_->private_inline_body;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const bool variadic_frame = shape.variadic_frame;
	const uint32_t frame_operand = shape.layout.frame_operand;
	int32_t &leaf_private_frame_slot =
		direct_user_call_context_->leaf_private_frame_slot;
	int32_t &leaf_caller_frame_slot =
		direct_user_call_context_->leaf_caller_frame_slot;
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
}

/* Where a failed guard of the direct user call's fast path goes. */
::tpde::Label ZendCompilerA64::call_slow_target() {
	const Adaptor::InstNode &node = inst_context_->node;
	const ::tpde::Label slow_path = direct_user_call_context_->slow_path;
	if (node.kind != Adaptor::InstKind::GuardedFast
			|| node.argument_index == UINT32_MAX) {
		return slow_path;
	}
	return this->block_labels[static_cast<uint32_t>(
		this->analyzer.block_idx(
			IRBlockRef{node.argument_index}))];
}

/* Loads the callee function, from the descriptor of a component-local
 * call or from the entry cell. */
void ZendCompilerA64::load_callee_function(AsmReg destination) {
	const bool local_component_call =
		direct_user_call_context_->local_component_call;
	const AsmReg cell_reg = direct_user_frame_context_->cell_reg;
	const AsmReg descriptor_reg = direct_user_frame_context_->descriptor_reg;
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
}

/* A guarded fast node whose callee body the adaptor inlined. */
bool ZendCompilerA64::compile_direct_user_inlined_body() {
	const Adaptor::InstNode &node = inst_context_->node;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t context_operand = shape.layout.context_operand;
	if (node.continuation_block == UINT32_MAX
			|| node.inlined_operand_index
				>= node.operands.size()) {
		return false;
	}
	const auto checked_steps =
		ir().inlined_checked_steps(node);
	if (!checked_steps.empty()
			&& node.inlined_checked_operand_count
				!= checked_steps.size() + 1) {
		return false;
	}
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
	 * A split observer guard already routed observer-visible
	 * execution into the materialized cold block; otherwise
	 * the guard stays in this block, which then keeps the
	 * context operand. Neither layout creates a private Zend
	 * frame on the register-only edge.
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
			if (node.inlined_operand_index + 1
					>= node.operands.size()) {
				return false;
			}
			auto [left_ref, left] = val_ref_single(
				node.operands[node.inlined_operand_index]);
			auto [right_ref, right] = val_ref_single(
				node.operands[
					node.inlined_operand_index + 1]);
			ScratchReg computed{this};
			auto computed_reg = computed.alloc_gp();
			auto emit_checked = [&](uint32_t source_opcode,
					AsmReg left_reg, AsmReg right_reg) {
				switch (source_opcode) {
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
				generate_raw_jump(
					Jump::Jvs, call_slow_target());
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
			if (!node.has_result) {
				return false;
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
			switch (checked_steps[0].source_opcode) {
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

/* A call of the callee's typed body function. */
bool ZendCompilerA64::compile_direct_user_typed_body() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const uint32_t typed_body_function =
		direct_user_call_context_->typed_body_function;
	const uint32_t argument_count = direct_user_call_context_->argument_count;
	const zend_tpde_plan *body_plan =
		ir().component_plan(
			call.component_target_index);
	if (body_plan == nullptr
			|| body_plan->argument_count != argument_count
			|| typed_body_function
				>= this->func_syms.size()
			|| node.continuation_block == UINT32_MAX
			|| node.operands.size() < argument_count) {
		return false;
	}
	if (!emit_typed_body_call(node, typed_body_function,
			call.component_target_index, argument_count,
			false)) {
		return false;
	}
	for (uint32_t operand = argument_count;
			operand < node.operands.size(); ++operand) {
		if (ir().machine_kind(node.operands[operand])
				!= ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				|| ir().ownership(node.operands[operand])
					!= ZEND_MIR_OWNERSHIP_STATE_OWNED) {
			return false;
		}
		auto boxed = val_ref(node.operands[operand]);
		const ValueParts parts = val_parts(node.operands[operand]);
		int32_t payload_part = -1;
		int32_t type_part = -1;
		for (uint32_t part = 0; part < parts.count(); ++part) {
			if (parts.representation.parts[part].semantic_role
					== ZEND_TPDE_MACHINE_PART_PAYLOAD) {
				payload_part = static_cast<int32_t>(part);
			} else if (parts.representation.parts[part].semantic_role
					== ZEND_TPDE_MACHINE_PART_TYPE_INFO) {
				type_part = static_cast<int32_t>(part);
			}
		}
		if (payload_part < 0 || type_part < 0) {
			return false;
		}
		auto payload = boxed.part(
			static_cast<uint32_t>(payload_part));
		auto type_info = boxed.part(
			static_cast<uint32_t>(type_part));
		auto payload_reg = payload.load_to_reg();
		auto type_info_reg = type_info.load_to_reg();
		auto released = text_writer.label_create();
		ASM(TSTwi, type_info_reg,
			IS_TYPE_REFCOUNTED << Z_TYPE_FLAGS_SHIFT);
		generate_raw_jump(Jump::Jeq, released);
		ScratchReg count{this};
		auto count_reg = count.alloc_gp();
		emit_gc_delref(payload_reg, count_reg);
		label_place(released);
	}
	adaptor->mark_typed_body_call(
		call.direct_call->frame_size);
	generate_uncond_branch(
		IRBlockRef{node.continuation_block});
	return true;
}

/*
 * The fast path of a leaf callee in a private frame on the native stack.
 * A result ends compile_call_direct_user(); without one, the successful
 * call continues at the completion compile_call_direct_user() shares.
 */
std::optional<bool> ZendCompilerA64::compile_direct_user_leaf_path() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool result_unused = direct_user_call_context_->result_unused;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t frame_operand = shape.layout.frame_operand;
	const uint32_t context_operand = shape.layout.context_operand;
	const ::tpde::Label successful = direct_user_call_context_->successful;
	int32_t &leaf_private_frame_slot =
		direct_user_call_context_->leaf_private_frame_slot;
	int32_t &leaf_caller_frame_slot =
		direct_user_call_context_->leaf_caller_frame_slot;
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
		call.record.id);
	auto descriptor_scratch =
		std::move(descriptor_value).into_scratch(this);
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
		return compile_direct_user_leaf_inlined_body(first_reg);
	}
	/*
	 * A leaf binding names an exact, already-published
	 * immutable callee. The compiler declines this
	 * representation when a frame probe is installed, so
	 * those compile-time invariants do not need to be
	 * reloaded at every loop iteration.
	 */
	emit_observer_exit(call_slow_target());
	ScratchReg callee_address{this};
	auto callee_reg = callee_address.alloc_gp();
		add_imm12_offset(callee_reg, AsmReg{AsmReg::FP},
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

	if (!store_direct_user_leaf_arguments(frame_reg, callee_reg)) {
		return false;
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
			add_imm12_offset(second_reg, second_reg,
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
		entry_argument.alloc_specific(AsmReg::R15);
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
	ASM(CMPwi, fast_status_reg, ZEND_NATIVE_RETURNED);
	auto leaf_returned = text_writer.label_create();
	generate_raw_jump(Jump::Jeq, leaf_returned);
	ASM(CMPwi, fast_status_reg, ZEND_NATIVE_RETRY);
	generate_raw_jump(Jump::Jeq, call_slow_target());
	if (zend_mir_id_is_valid(call.exception_block_id)) {
		auto propagate = text_writer.label_create();
		ASM(CMPwi, fast_status_reg,
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
	return std::nullopt;
}

/* The leaf fast path of a callee body the adaptor inlined. */
bool ZendCompilerA64::compile_direct_user_leaf_inlined_body(
		AsmReg first_reg) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	int32_t &leaf_private_frame_slot =
		direct_user_call_context_->leaf_private_frame_slot;
	int32_t &leaf_caller_frame_slot =
		direct_user_call_context_->leaf_caller_frame_slot;
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
	const auto checked_steps =
		ir().inlined_checked_steps(node);
	if (!checked_steps.empty()) {
		if (checked_steps.size() > 1) {
			const uint32_t checked_operand_count =
				node.inlined_checked_operand_count;
			if (checked_operand_count
					!= checked_steps.size() + 1
					|| node.operands.size()
						< checked_operand_count
					|| node.inlined_operand_index
						> node.operands.size()
							- checked_operand_count) {
				return false;
			}
			auto [left_ref, left] = val_ref_single(
				node.operands[
					node.inlined_operand_index]);
			auto [right_ref, right] = val_ref_single(
				node.operands[
					node.inlined_operand_index + 1]);
			auto emit_checked = [&](uint32_t source_opcode,
					AsmReg left_reg, AsmReg right_reg) {
				switch (source_opcode) {
					case ZEND_ADD:
						ASM(ADDSx, first_reg,
							left_reg, right_reg);
						break;
					case ZEND_SUB:
						ASM(SUBSx, first_reg,
							left_reg, right_reg);
						break;
					default:
						return false;
				}
				generate_raw_jump(
					Jump::Jvs, call_slow_target());
				return true;
			};
			if (!emit_checked(
					checked_steps[0].source_opcode,
					left.load_to_reg(),
					right.load_to_reg())) {
				return false;
			}
			for (uint32_t step = 1;
					step < checked_steps.size(); ++step) {
				auto [other_ref, other] = val_ref_single(
					node.operands[
						node.inlined_operand_index
							+ step + 1]);
				const auto other_reg = other.load_to_reg();
				if (!emit_checked(
						checked_steps[step].source_opcode,
						checked_steps[step].accumulator_is_left
							? first_reg : other_reg,
						checked_steps[step].accumulator_is_left
							? other_reg : first_reg)) {
					return false;
				}
			}
		} else {
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
			switch (checked_steps[0].source_opcode) {
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
			generate_raw_jump(
				Jump::Jvs, call_slow_target());
		}
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

/* Copies the arguments into the leaf callee's private frame. */
bool ZendCompilerA64::store_direct_user_leaf_arguments(
		AsmReg frame_reg, AsmReg callee_reg) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const uint32_t argument_count = direct_user_call_context_->argument_count;
	for (uint32_t index = 0;
			index < argument_count; ++index) {
		zend_mir_call_argument_ref source_argument;
			if (!zend_tpde_call_argument_at(ir().plan(),
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
					>= ir().plan()
						->source_opcode_count) {
				return false;
			}
			const uint8_t source_argument_type =
				ir().plan()->source_opcodes[
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
				if (descriptor_argument.exact_type
						== ZEND_MIR_SCALAR_TYPE_I1) {
					ScratchReg kind{this};
					store_bool_slot(callee_reg, offset, payload_reg, kind, true,
						BoolTypeInfo::MATERIALIZE_ADD, true);
				} else {
					store_off(callee_reg, offset,
						payload_reg, 8);
					store_constant(callee_reg, offset + 8,
						0, 8);
					store_constant(callee_reg,
						offset + 8,
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
						if (descriptor_argument.exact_type
								== ZEND_MIR_SCALAR_TYPE_I1) {
							store_bool_slot(callee_reg, offset,
								descriptor_argument.scalar_bits != 0, true);
						} else {
							store_constant(callee_reg, offset,
								descriptor_argument.scalar_bits, 8);
							store_constant(callee_reg, offset + 8,
								zval_type(
									descriptor_argument.exact_type), 4);
						}
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
				} else if (ir().machine_kind(
						node.operands[index])
							== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
						&& ir()
							.machine_value_is_register_authoritative(
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
						store_bool_slot(callee_reg, offset, low_word.cur_reg(),
							high_word, true, BoolTypeInfo::GIVEN);
					} else {
						store_off(callee_reg, offset,
							low_word.cur_reg(), 8);
						store_off(callee_reg, offset + 8,
							high_word.cur_reg(), 4);
					}
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
						emit_gc_addref(low_word.cur_reg(),
							type_info_reg);
						label_place(copied);
					}
				} else {
					auto argument_reg =
						argument.load_to_reg();
					const uint32_t type =
						zval_type(*adaptor,
							node.operands[index]);
					if (type == IS_FALSE) {
						ScratchReg kind{this};
						store_bool_slot(callee_reg, offset, argument_reg, kind,
							true, BoolTypeInfo::MATERIALIZE_ADD, true);
					} else {
						store_off(callee_reg, offset,
							argument_reg, 8);
						store_constant(callee_reg,
							offset + 8, 0, 8);
						store_constant(callee_reg,
							offset + 8, type, 4);
					}
				}
			}
		}
	return true;
}

/*
 * The fast path of a callee in a Zend frame reserved on the VM stack. Its
 * steps share the registers it holds through direct_user_frame_context_.
 */
bool ZendCompilerA64::compile_direct_user_frame_path() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool local_component_call =
		direct_user_call_context_->local_component_call;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const bool variadic_frame = shape.variadic_frame;
	const uint32_t fixed_argument_count = shape.fixed_argument_count;
	const uint32_t frame_operand = shape.layout.frame_operand;
	const uint32_t context_operand = shape.layout.context_operand;
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
		call.record.id);
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
	DirectUserFrameContext frame_context{
		.reservation_size = reservation_size,
		.frame_reg = frame_reg,
		.context_reg = context_reg,
		.cell_reg = cell_reg,
		.descriptor_reg = descriptor_reg,
		.first_reg = first_reg,
		.second_reg = second_reg,
		.published_code_reg = published_code_reg,
		.run_time_cache = run_time_cache,
	};
	const ContextScope<DirectUserFrameContext> frame_context_scope{
		direct_user_frame_context_, &frame_context};

	guard_direct_user_callee();
	if (!guard_direct_user_arguments()) {
		return false;
	}
	reserve_direct_user_frame();

	ScratchReg callee_address{this};
	auto callee_reg = callee_address.alloc_gp();
	frame_context.callee_reg = callee_reg;
	initialize_direct_user_frame();
	if (!store_direct_user_frame_arguments()) {
		return false;
	}
	initialize_direct_user_locals();
	link_direct_user_activation();

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
				uint64_t{IS_CV} | ((uint64_t{ZEND_CALL_FRAME_SLOT}
						+ fixed_argument_count) * sizeof(zval) << 8),
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

	complete_direct_user_frame_call(fast_status);
	return true;
}

/* Guards the published callee code, the free call slot, the run-time
 * cache and the receiver; a failed guard takes the slow path. */
void ZendCompilerA64::guard_direct_user_callee() {
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool local_component_call =
		direct_user_call_context_->local_component_call;
	const AsmReg frame_reg = direct_user_frame_context_->frame_reg;
	const AsmReg context_reg = direct_user_frame_context_->context_reg;
	const AsmReg cell_reg = direct_user_frame_context_->cell_reg;
	const AsmReg descriptor_reg = direct_user_frame_context_->descriptor_reg;
	const AsmReg first_reg = direct_user_frame_context_->first_reg;
	const AsmReg second_reg = direct_user_frame_context_->second_reg;
	const AsmReg published_code_reg =
		direct_user_frame_context_->published_code_reg;
	std::optional<ScratchReg> &run_time_cache =
		direct_user_frame_context_->run_time_cache;
	if (local_component_call) {
		materialize_constant(
			UINT64_C(0), DarwinConfig::GP_BANK, 8,
			published_code_reg);
	} else {
		add_imm12_offset(published_code_reg, cell_reg,
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
	emit_observer_exit(call_slow_target());
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
}

/* Guards the type of each argument whose copy the fast path inlines. */
bool ZendCompilerA64::guard_direct_user_arguments() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const uint32_t argument_count = direct_user_call_context_->argument_count;
	const AsmReg frame_reg = direct_user_frame_context_->frame_reg;
	const AsmReg first_reg = direct_user_frame_context_->first_reg;
	const AsmReg second_reg = direct_user_frame_context_->second_reg;
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
			if (argument.source_frame_offset == UINT32_MAX) {
				return false;
			}
			const bool register_boxed_argument =
				node.operands[index]
					!= IRValueRef{Adaptor::FRAME_VALUE}
				&& ir().machine_kind(node.operands[index])
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& ir().machine_value_is_register_authoritative(
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
				load_off(first_reg, frame_reg,
					argument.source_frame_offset
						+ static_cast<uint32_t>(offsetof(
							zval, u1.type_info)),
					4);
			}
			ASM(ANDwi, first_reg, first_reg, Z_TYPE_MASK);
			if (argument_guard_type == ZEND_MIR_SCALAR_TYPE_I1) {
				auto value_type_valid =
					text_writer.label_create();
				ASM(CMPwi, first_reg, IS_FALSE);
				generate_raw_jump(
					Jump::Jeq, value_type_valid);
				ASM(CMPwi, first_reg, IS_TRUE);
				generate_raw_jump(
					Jump::Jne, call_slow_target());
				label_place(value_type_valid);
			} else {
				ASM(CMPwi, first_reg,
					zval_type(argument_guard_type));
				generate_raw_jump(
					Jump::Jne, call_slow_target());
			}
			continue;
		}
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
		} else if (zend_mir_scalar_type_is_exact(
				argument_guard_type)) {
			if (argument.source_frame_offset == UINT32_MAX) {
				return false;
			}
			load_off(second_reg, frame_reg,
				argument.source_frame_offset, 8);
			load_off(second_reg, second_reg,
				static_cast<uint32_t>(
					offsetof(zend_reference, val)
						+ offsetof(zval, u1.type_info)),
				4);
			ASM(ANDwi, second_reg, second_reg, Z_TYPE_MASK);
			if (argument_guard_type == ZEND_MIR_SCALAR_TYPE_I1) {
				auto reference_type_valid =
					text_writer.label_create();
				ASM(CMPwi, second_reg, IS_FALSE);
				generate_raw_jump(
					Jump::Jeq, reference_type_valid);
				ASM(CMPwi, second_reg, IS_TRUE);
				generate_raw_jump(
					Jump::Jne, call_slow_target());
				label_place(reference_type_valid);
			} else {
				ASM(CMPwi, second_reg,
					zval_type(argument_guard_type));
				generate_raw_jump(
					Jump::Jne, call_slow_target());
			}
		}
	}
	return true;
}

/* Guards the native stack and reserves the callee frame on the VM
 * stack. */
void ZendCompilerA64::reserve_direct_user_frame() {
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool local_component_call =
		direct_user_call_context_->local_component_call;
	const uint64_t reservation_size =
		direct_user_frame_context_->reservation_size;
	const AsmReg context_reg = direct_user_frame_context_->context_reg;
	const AsmReg first_reg = direct_user_frame_context_->first_reg;
	const AsmReg second_reg = direct_user_frame_context_->second_reg;
	/*
	 * Guard the native C stack before recursive entry.  The
	 * slow path raises the canonical Zend overflow error.
	 */
#ifdef ZEND_CHECK_STACK_LIMIT
	load_off(first_reg, context_reg,
		static_cast<uint32_t>(offsetof(
			zend_native_execution_context,
			stack_limit)), 8);
	load_off(first_reg, first_reg, 0, 8);
	if (local_component_call) {
		emit_direct_call_stack_guard_position(
			second_reg, call.component_target_index);
	} else {
		ASM(ADDxi, second_reg, AsmReg{AsmReg::SP}, 0);
	}
	ASM(CMPx, second_reg, first_reg);
	generate_raw_jump(Jump::Jls, call_slow_target());
#endif

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
}

/* Initializes the callee's Zend frame header and result. */
void ZendCompilerA64::initialize_direct_user_frame() {
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool result_unused = direct_user_call_context_->result_unused;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t fixed_argument_count = shape.fixed_argument_count;
	const bool release_extra_arguments = shape.release_extra_arguments;
	const uint64_t reservation_size =
		direct_user_frame_context_->reservation_size;
	const AsmReg frame_reg = direct_user_frame_context_->frame_reg;
	const AsmReg context_reg = direct_user_frame_context_->context_reg;
	const AsmReg descriptor_reg = direct_user_frame_context_->descriptor_reg;
	const AsmReg first_reg = direct_user_frame_context_->first_reg;
	const AsmReg second_reg = direct_user_frame_context_->second_reg;
	std::optional<ScratchReg> &run_time_cache =
		direct_user_frame_context_->run_time_cache;
	const AsmReg callee_reg = direct_user_frame_context_->callee_reg;
	mov(callee_reg, first_reg, 8);
	mov(second_reg, callee_reg, 8);
	add_imm12_offset(second_reg, second_reg, reservation_size);
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
			emit_gc_addref(second_reg, first_reg);
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
	emit_publish_opline(frame_reg, second_reg,
		static_cast<uint64_t>(
			call.direct_call->source_position)
			* sizeof(zend_op));
	emit_publish_opline(callee_reg, second_reg,
		static_cast<uint64_t>(fixed_argument_count)
			* sizeof(zend_op));

	/* Resolve the caller's canonical result zval. */
	if (result_unused) {
		add_imm12_offset(second_reg, callee_reg,
			static_cast<uint64_t>(call.direct_call->frame_size)
				+ offsetof(zend_native_direct_activation,
					discarded_return));
	} else {
		mov(second_reg, frame_reg, 8);
		if (call.direct_call->result_operand.slot_kind
				== ZEND_MIR_SOURCE_SLOT_CV) {
			add_imm12_offset(second_reg, second_reg,
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
}

/* Copies the arguments into the callee frame. */
bool ZendCompilerA64::store_direct_user_frame_arguments() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const uint32_t argument_count = direct_user_call_context_->argument_count;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t fixed_argument_count = shape.fixed_argument_count;
	const uint32_t first_extra_argument_slot =
		shape.first_extra_argument_slot;
	const AsmReg frame_reg = direct_user_frame_context_->frame_reg;
	const AsmReg callee_reg = direct_user_frame_context_->callee_reg;
	for (uint32_t index = 0; index < argument_count; ++index) {
		zend_mir_call_argument_ref source_argument;
		if (!zend_tpde_call_argument_at(ir().plan(),
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
				>= ir().plan()->source_opcode_count) {
			return false;
		}
		const uint8_t source_argument_type =
			ir().plan()->source_opcodes[
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
			ir().machine_kind(node.operands[index]);
		const bool register_pointer_argument =
			ir().machine_value_is_register_authoritative(
				node.operands[index])
			&& zend_tpde_machine_kind_is_pointer(argument_kind);
			if (zend_mir_scalar_type_is_exact(
					descriptor_argument.exact_type)) {
				if (descriptor_argument.source_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
					if (descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_I1) {
						store_bool_slot(callee_reg, offset,
							descriptor_argument.scalar_bits != 0, true);
					} else {
						store_constant(callee_reg, offset,
							descriptor_argument.scalar_bits, 8);
						store_constant(callee_reg, offset + 8,
							zval_type(
								descriptor_argument.exact_type), 4);
					}
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
					if (descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_I1) {
						ScratchReg kind{this};
						store_bool_slot(callee_reg, offset, argument_reg,
							kind, true, BoolTypeInfo::MATERIALIZE_ADD, true);
					} else {
						store_off(callee_reg, offset,
							argument_reg, 8);
						store_constant(callee_reg, offset + 8,
							0, 8);
						store_constant(callee_reg,
							offset + 8,
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
					if (descriptor_argument.exact_type
							== ZEND_MIR_SCALAR_TYPE_I1) {
						ScratchReg kind{this};
						store_bool_slot(callee_reg, offset, payload_reg,
							kind, true, BoolTypeInfo::MATERIALIZE_ADD, true);
					} else {
						store_off(callee_reg, offset,
							payload_reg, 8);
						store_constant(callee_reg, offset + 8,
							0, 8);
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
				add_imm12_offset(source_address_reg,
					source_address_reg,
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
				emit_gc_addref(low_word_reg, type_info_reg);
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
			} else if (ir().machine_kind(
					node.operands[index])
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& ir()
						.machine_value_is_register_authoritative(
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
					emit_gc_addref(low_word.cur_reg(),
						type_info_reg);
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
			add_imm12_offset(source_address_reg,
				source_frame_reg,
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
				emit_gc_addref(low_word_reg, type_info_reg);
				label_place(copied);
			} else {
				store_constant(source_address_reg,
					static_cast<uint32_t>(offsetof(
						zval, u1.type_info)),
					IS_UNDEF, 4);
			}
		}
	}
	return true;
}

/* Initializes the default arguments and the other compiled
 * variables. */
void ZendCompilerA64::initialize_direct_user_locals() {
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t callee_argument_count = shape.callee_argument_count;
	const uint32_t fixed_argument_count = shape.fixed_argument_count;
	const uint32_t compiled_variable_count =
		shape.compiled_variable_count;
	const AsmReg callee_reg = direct_user_frame_context_->callee_reg;
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
		add_imm12_offset(source_address_reg,
			source_address_reg,
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
		emit_gc_addref(low_word_reg, type_info_reg);
		label_place(copied);
	}
	for (uint32_t index = fixed_argument_count;
			index < compiled_variable_count; ++index) {
		const uint32_t offset = static_cast<uint32_t>(
			(ZEND_CALL_FRAME_SLOT + index) * sizeof(zval));
		store_constant(callee_reg, offset, 0, 8);
		store_constant(callee_reg, offset + 8, 0, 8);
	}
}

/* Completes and links the activation behind the callee frame. */
void ZendCompilerA64::link_direct_user_activation() {
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool local_component_call =
		direct_user_call_context_->local_component_call;
	const bool result_unused = direct_user_call_context_->result_unused;
	const bool generation_leased =
		direct_user_call_context_->generation_leased;
	const AsmReg frame_reg = direct_user_frame_context_->frame_reg;
	const AsmReg context_reg = direct_user_frame_context_->context_reg;
	const AsmReg cell_reg = direct_user_frame_context_->cell_reg;
	const AsmReg descriptor_reg = direct_user_frame_context_->descriptor_reg;
	const AsmReg first_reg = direct_user_frame_context_->first_reg;
	const AsmReg second_reg = direct_user_frame_context_->second_reg;
	const AsmReg published_code_reg =
		direct_user_frame_context_->published_code_reg;
	const AsmReg callee_reg = direct_user_frame_context_->callee_reg;
	/* Complete and link the stable trailing activation. */
	mov(second_reg, callee_reg, 8);
	add_imm12_offset(second_reg, second_reg,
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
}

/* After the native call: the helper-free completion of a plain return,
 * or the canonical finisher. */
void ZendCompilerA64::complete_direct_user_frame_call(
		ValuePart &fast_status) {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool result_unused = direct_user_call_context_->result_unused;
	const bool generation_leased =
		direct_user_call_context_->generation_leased;
	const uint32_t argument_count = direct_user_call_context_->argument_count;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const uint32_t fixed_argument_count = shape.fixed_argument_count;
	const uint32_t first_extra_argument_slot =
		shape.first_extra_argument_slot;
	const uint32_t compiled_variable_count =
		shape.compiled_variable_count;
	const uint32_t frame_operand = shape.layout.frame_operand;
	const uint32_t context_operand = shape.layout.context_operand;
	const ::tpde::Label successful = direct_user_call_context_->successful;
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
		/* The callee may have dropped the caller's own
		 * reference: the last one is released by the
		 * canonical finisher. */
		ASM(CMPwi, receiver_refcount_reg, 1);
		generate_raw_jump(Jump::Jeq, complete_fast);
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
		call.record.id), ::tpde::CCAssignment{});
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

/* The call through the runtime enter and leave helpers. */
bool ZendCompilerA64::compile_direct_user_slow_call() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call = direct_user_call_context_->call;
	const bool generated_fast_path =
		direct_user_call_context_->generated_fast_path;
	const DirectUserCallShape &shape = direct_user_call_context_->shape;
	const bool split_cold = shape.split_cold;
	const uint32_t frame_operand = shape.layout.frame_operand;
	const uint32_t context_operand = shape.layout.context_operand;
	const uint32_t slow_enter_frame_use =
		shape.layout.slow_enter_frame_use;
	const uint32_t slow_enter_context_use =
		shape.layout.slow_enter_context_use;
	const uint32_t slow_entry_context_use =
		shape.layout.slow_entry_context_use;
	const uint32_t slow_leave_frame_use =
		shape.layout.slow_leave_frame_use;
	const uint32_t slow_leave_context_use =
		shape.layout.slow_leave_context_use;
	const ::tpde::Label successful = direct_user_call_context_->successful;
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
			call.record.id), ::tpde::CCAssignment{});
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
		call.record.id), ::tpde::CCAssignment{});
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
		label_place(successful);
		finish_generated_result();
	} else if (node.has_result
			&& ir().machine_kind(node.result)
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
			add_imm12_offset(result_slot_reg, result_slot_reg,
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

bool ZendCompilerA64::compile_call_direct_internal() {
	IRInstRef &instruction = inst_context_->instruction;
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &call =
		ir().mir_instruction(instruction);
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
				call.record.id), ::tpde::CCAssignment{});
			builder.call(runtime_symbol(
				ZEND_NATIVE_HELPER_INTERNAL_CALL_BEGIN));
		}
		for (uint32_t index = 0;
				index < argument_count; ++index) {
			const IRValueRef operand = node.operands[index];
			const zend_native_direct_internal_call_argument &argument =
				call.direct_internal_call->arguments[index];
			const bool direct_boxed_argument =
				operand != IRValueRef{Adaptor::FRAME_VALUE}
				&& ir().machine_kind(operand)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
				&& ir().machine_value_is_register_authoritative(
					operand)
				&& (argument.source_opcode == ZEND_SEND_VAL
					|| argument.source_opcode == ZEND_SEND_VAL_EX);
			zend::native::tpde::CCAssignerAppleA64 assigner;
			CallBuilder builder{*this, assigner};
			const IRValueRef frame_operand =
				node.operands[frame_base + 1 + index];
			if (operand != IRValueRef{Adaptor::FRAME_VALUE}
					&& ir().machine_kind(operand)
						== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL
					&& !direct_boxed_argument) {
				const zend_mir_source_operand_ref &source =
					argument.source_operand;
				if ((source.kind != ZEND_MIR_SOURCE_OPERAND_SLOT
						&& source.kind != ZEND_MIR_SOURCE_OPERAND_SSA)
						|| (source.slot_kind != ZEND_MIR_SOURCE_SLOT_TMP
							&& source.slot_kind
								!= ZEND_MIR_SOURCE_SLOT_VAR)
						|| source.index >= ir().plan()
							->source_temporary_count) {
					return false;
				}
				const uint64_t storage = static_cast<uint64_t>(
					ir().plan()->source_frame_variable_count)
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
				store_boxed_part(frame_reg, payload,
					static_cast<uint32_t>(offset), 8);
				store_boxed_part(frame_reg, type_info,
					static_cast<uint32_t>(offset
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
					call.record.id), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				for (uint32_t part = 0; part < 2; ++part) {
					builder.add_arg(boxed.part(part),
						::tpde::CCAssignment{});
				}
				boxed.reset();
				ASM(ORRwi, AsmReg{AsmReg::R4}, AsmReg{AsmReg::R4},
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
					call.record.id), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
				continue;
			}
			if (ir().machine_kind(operand)
					== ZEND_TPDE_MACHINE_VALUE_BOXED_ZVAL) {
				builder.add_arg(image_symbol_value(
					ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
					call.record.id), ::tpde::CCAssignment{});
				builder.add_arg(ValuePart{index, 4,
					DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_SOURCE_ARGUMENT));
				continue;
			}
			builder.add_arg(image_symbol_value(
				ZEND_NATIVE_IMAGE_SYMBOL_DIRECT_INTERNAL_CALL_DESCRIPTOR,
				call.record.id), ::tpde::CCAssignment{});
			builder.add_arg(ValuePart{index, 4,
				DarwinConfig::GP_BANK}, ::tpde::CCAssignment{});
			builder.add_arg(CallArg{operand});
			if (ir().exact_type(operand)
					== ZEND_MIR_SCALAR_TYPE_F64) {
				builder.call(runtime_symbol(
					ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_SET_DOUBLE_ARGUMENT));
			} else {
				builder.add_arg(ValuePart{
					static_cast<uint32_t>(
						ir().exact_type(operand)),
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
			call.record.id), ::tpde::CCAssignment{});
		builder.call(runtime_symbol(
			ZEND_NATIVE_HELPER_INTERNAL_CALL_FINISH_SOURCE));
		ValuePart status{DarwinConfig::GP_BANK, 4};
		builder.add_ret(status, ::tpde::CCAssignment{});
		auto status_reg = status.cur_reg_or_load(this);
		ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::Jeq, continued);
		if (zend_mir_id_is_valid(call.exception_block_id)) {
			auto propagate = text_writer.label_create();
			ASM(CMPwi, status_reg, ZEND_NATIVE_EXCEPTION);
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
				encode_source_operand(
					call.call_site->result_operand),
				8, DarwinConfig::GP_BANK},
				::tpde::CCAssignment{});
			result_builder.add_arg(ValuePart{
				static_cast<uint32_t>(
					ir().exact_type(node.result)),
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
		call.record.id), ::tpde::CCAssignment{});
	builder.call(runtime_symbol(call.internal_call_cell != nullptr
			&& zend_native_internal_call_descriptor_plain(
				call.internal_call_cell, call.direct_internal_call)
		? ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL_PLAIN
		: ZEND_NATIVE_HELPER_DIRECT_INTERNAL_CALL));
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

bool ZendCompilerA64::compile_finally_call() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_mir_instruction_record &record = *inst_context_->record;
	const zend_tpde_plan *plan = ir().plan();
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

bool ZendCompilerA64::compile_finally_return() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_mir_instruction_record &record = *inst_context_->record;
	const zend_tpde_plan *plan = ir().plan();
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
			ir().user_opcode_next_landings();
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
		for_each_finally_continuation([&](uint32_t source_position,
				IRBlockRef continuation) {
			compare_unsigned_immediate(
				direct_continuation_reg, source_position);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::Jne, continued);
			generate_exception_branch(continuation);
			label_place(continued);
		});
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
	for_each_finally_continuation([&](uint32_t source_position,
			IRBlockRef continuation_block) {
		compare_unsigned_immediate(continuation_reg, source_position);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::Jne, continued);
		generate_exception_branch(continuation_block);
		label_place(continued);
	});
	/* Unlike the catch dispatch, this also compares handlers TPDE does
	 * not compile. */
	for_each_exception_handler(false, [&](IRBlockRef handler_block,
			uint32_t flagged_position) {
		ScratchReg expected{this};
		auto expected_reg = expected.alloc_gp();
		materialize_constant(flagged_position,
			DarwinConfig::GP_BANK, 4, expected_reg);
		ASM(CMPx, continuation_reg, expected_reg);
		auto continued = text_writer.label_create();
		generate_raw_jump(Jump::Jne, continued);
		generate_exception_branch(handler_block);
		label_place(continued);
	});
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

bool ZendCompilerA64::compile_catch_enter() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
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
		ASM(CMPwi, status_reg, ZEND_NATIVE_CATCH_EXCEPTION);
		auto no_exception = text_writer.label_create();
		generate_raw_jump(Jump::Jne, no_exception);
		generate_exception_branch(
			adaptor->block_ref(mir.exception_block_id));
		label_place(no_exception);
	}
	ASM(CMPwi, status_reg, ZEND_NATIVE_CATCH_MATCHED);
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

bool ZendCompilerA64::compile_return() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_mir_instruction_record &record = *inst_context_->record;
	if (ir().typed_body()) {
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
	emit_publish_opline(frame_reg, source_position_reg,
		uint64_t{record.source_position_id} * sizeof(zend_op));
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

bool ZendCompilerA64::compile_return_source_zval() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
	if (ir().typed_body()) {
		if (node.operands.size() != 1) {
			return false;
		}
		const auto kind =
			ir().machine_kind(node.operands[0]);
		const zend_mir_ownership_state ownership =
			ir().ownership(node.operands[0]);
		const zend_mir_refcount_state refcount_state =
			ir().refcount_state(node.operands[0]);
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
			emit_gc_addref(payload_reg, count_reg);
			label_place(copied);
			return_builder.add(
				std::move(payload), ::tpde::CCAssignment{});
			return_builder.add(
				std::move(type_info), ::tpde::CCAssignment{});
		} else if (return_addref
				&& zend_tpde_machine_kind_is_pointer(kind)) {
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
			ir().machine_kind(returned_ref);
		const zend_mir_ownership_state ownership =
			ir().ownership(returned_ref);
		const zend_mir_refcount_state refcount_state =
			ir().refcount_state(returned_ref);
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
		emit_publish_opline(frame_reg, source_position_reg,
			uint64_t{record.source_position_id} * sizeof(zend_op));
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
				emit_gc_addref(payload_reg, count_reg);
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
			if (ir().constant(
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
					&& zend_tpde_machine_kind_is_pointer(kind)) {
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
			emit_publish_opline(frame_reg, source_position_reg,
				uint64_t{record.source_position_id} * sizeof(zend_op));
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
		encode_source_operand(mir.value_operation.op1), 8,
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

bool ZendCompilerA64::compile_mir_opcode() {
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
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
		case ZEND_MIR_OPCODE_VALUE_BINARY_OP: {
			zend_tpde_string_identity framed_string_identity{};
			if (zend_tpde_string_identity_at(
					mir, &framed_string_identity)) {
				return string_identity();
			}
			return long_binary();
		}
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
			if (ir().typed_body()
					|| mir.runtime_helper == ZEND_NATIVE_HELPER_COUNT) {
				return true;
			}
			return execute_value_operation();
		case ZEND_MIR_OPCODE_VALUE_ECHO:
			return execute_value_operation();
		case ZEND_MIR_OPCODE_FUNC_NUM_ARGS:
			return compile_func_num_args();
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
		default:
			return compile_machine_opcode();
	}
}

/* The scalar machine operations and the control flow of
 * compile_mir_opcode(). */
bool ZendCompilerA64::compile_machine_opcode() {
	IRInstRef &instruction = inst_context_->instruction;
	const Adaptor::InstNode &node = inst_context_->node;
	const zend_tpde_instruction &mir = *inst_context_->mir;
	const zend_mir_instruction_record &record = *inst_context_->record;
	switch (record.opcode) {
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
				 * The interrupt poll's fast/slow branch is invisible to TPDE's
				 * IR CFG, and the slow-path call invalidates caller-saved
				 * assignments. Spill live values before the branch so both paths
				 * reach the join with the same canonical copies.
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
		/* FCMP with a NaN operand sets C and V: MI and LS are false then,
		 * as PHP's < and <= are. */
		case ZEND_MIR_OPCODE_F64_LT:
			return floating_compare(Jump::Jmi);
		case ZEND_MIR_OPCODE_F64_LE:
			return floating_compare(Jump::Jls);
		case ZEND_MIR_OPCODE_F64_CMP: {
			auto [left_pair, right_pair] = binary();
			auto &[left_ref, left] = left_pair;
			auto &[right_ref, right] = right_pair;
			auto [result_ref, result] = result_ref_single(node.result);
			auto result_reg = result.alloc_reg();
			ScratchReg less{this};
			auto less_reg = less.alloc_gp();
			ASM(FCMP_d, left.load_to_reg(), right.load_to_reg());
			/* An unordered compare is 1, like ZEND_THREEWAY_COMPARE. */
			generate_raw_set(Jump::Jmi, less_reg);
			generate_raw_set(Jump::Jhi, result_reg);
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
		case ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH:
			return compile_value_multi_branch();
		case ZEND_MIR_OPCODE_VALUE_COND_BRANCH:
		case ZEND_MIR_OPCODE_ITERATOR_BRANCH:
		case ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH:
		case ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH:
			return compile_value_cond_branch();
		case ZEND_MIR_OPCODE_CALL_DIRECT_USER:
			return compile_call_direct_user();
		case ZEND_MIR_OPCODE_CALL_DIRECT_INTERNAL:
			return compile_call_direct_internal();
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
			ASM(CMPwi, status_reg, ZEND_NATIVE_RETURNED);
			auto continued = text_writer.label_create();
			generate_raw_jump(Jump::Jeq, continued);
			RetBuilder return_builder{*this, *cur_cc_assigner()};
			return_builder.add(std::move(status), ::tpde::CCAssignment{});
			return_builder.ret();
			label_place(continued);
			return true;
		}
		case ZEND_MIR_OPCODE_FINALLY_CALL:
			return compile_finally_call();
		case ZEND_MIR_OPCODE_FINALLY_RETURN:
			return compile_finally_return();
		case ZEND_MIR_OPCODE_CATCH_ENTER:
			return compile_catch_enter();
		case ZEND_MIR_OPCODE_RETURN:
			return compile_return();
		case ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL:
			return compile_return_source_zval();
		default:
			return false;
	}
}

bool ZendCompilerA64::compile_inst_impl(
	IRInstRef instruction, InstRange remaining_instructions) {
	const Adaptor::InstNode &node = ir().node(instruction);
	std::vector<ValueRef> generator_reload_locks;
	InstContext context{instruction, remaining_instructions, node};
	const ContextScope<InstContext> context_scope{
		inst_context_, &context};
	if (!emit_materializations(instruction)) {
		return false;
	}
	if (node.kind != Adaptor::InstKind::GeneratorResume
			&& !ir().generator_resume_values(instruction).empty()
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
		return compile_typed_call_guard();
	}
	if (node.kind == Adaptor::InstKind::StringLengthValue) {
		return compile_string_length_value();
	}
	if (node.kind == Adaptor::InstKind::ScalarSelect) {
		return compile_scalar_select();
	}
	if (node.kind == Adaptor::InstKind::BoxScalar) {
		return compile_box_scalar();
	}
	if (node.kind == Adaptor::InstKind::UnboxScalar) {
		return compile_unbox_scalar();
	}
	if (node.kind == Adaptor::InstKind::UnboxPointer) {
		return compile_unbox_pointer();
	}
	if (node.kind == Adaptor::InstKind::UnboxReferenceScalar) {
		return compile_unbox_reference_scalar();
	}
	if (node.kind == Adaptor::InstKind::LoadFrame) {
		return compile_load_frame();
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeLanding) {
		return compile_user_opcode_landing();
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeDispatch) {
		return compile_user_opcode_dispatch();
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeGateway) {
		return compile_user_opcode_gateway();
	}
	if (node.kind == Adaptor::InstKind::UserCallInit
			|| node.kind == Adaptor::InstKind::UserCallSend
			|| node.kind == Adaptor::InstKind::UserCallCheck
			|| node.kind == Adaptor::InstKind::UserCallExpand
			|| node.kind == Adaptor::InstKind::UserCallDo) {
		return compile_user_call();
	}
	if (node.kind == Adaptor::InstKind::UserOpcodeCallFragment) {
		return compile_user_opcode_call_fragment();
	}
	if (node.kind == Adaptor::InstKind::GeneratorGateway) {
		return compile_generator_gateway();
	}
	if (node.kind == Adaptor::InstKind::GeneratorResume) {
		return compile_generator_resume();
	}
	if (node.kind == Adaptor::InstKind::ZvalGuardArguments) {
		return compile_zval_guard_arguments();
	}
	if (node.kind == Adaptor::InstKind::ZvalReferenceResolve) {
		return compile_zval_reference_resolve();
	}
	if (node.kind == Adaptor::InstKind::ZvalBoxedStore) {
		return compile_zval_boxed_store();
	}
	if (node.kind == Adaptor::InstKind::ZvalPayloadLoad) {
		return compile_zval_payload_load();
	}
	const zend_tpde_instruction &mir = ir().mir_instruction(instruction);
	const zend_mir_instruction_record record =
		ir().instruction_record(instruction);
	context.mir = &mir;
	context.record = &record;
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
		return compile_zval_store();
	}
	if (record.opcode == ZEND_MIR_OPCODE_ECHO_SCALAR) {
		return compile_echo_scalar();
	}
	if (node.kind == Adaptor::InstKind::GuardedCold
			&& record.opcode != ZEND_MIR_OPCODE_CALL_DIRECT_USER) {
		return compile_guarded_cold();
	}
	/* Very large components can contain hundreds of thousands of guarded
	 * value operations.  To bound code size they skip the speculative fast
	 * sequence and branch straight to the cold block, which calls the
	 * operation's runtime helper. */
	if (ir().compact_guarded_value_operation(instruction)) {
		return branch_to_guarded_cold();
	}

	return compile_mir_opcode();
}

bool ZendCompilerA64::compile_inst(
	IRInstRef instruction, InstRange remaining_instructions) {
	const Adaptor::InstNode &node = ir().node(instruction);
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
		: adaptor{plans}, compiler{&adaptor, image} {}
};

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
