// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"

#include <fadec-enc2.h>

#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

enum register_id : uint8_t {
	RAX = 0,
	RCX = 1,
	RDX = 2,
	RSI = 6,
	RDI = 7,
	R8 = 8,
	R9 = 9,
	R10 = 10,
	R11 = 11
};

struct branch_patch {
	size_t displacement_offset;
	size_t next_instruction;
	zend_mir_block_id target;
};

struct emitter {
	const zend_tpde_plan *plan;
	zend_native_image *image;
	size_t *labels;
	branch_patch *patches;
	uint32_t patch_count;
	uint32_t patch_capacity;
	zend_native_diagnostic *diag;
};

bool emit_bytes(emitter *out, const void *bytes, size_t length) {
	if (!zend_tpde_image_append(out->image, bytes, length)) {
		zend_tpde_set_diagnostic(out->diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to grow the Linux x86-64 text image");
		return false;
	}
	return true;
}

FeRegGP fadec_register(register_id reg) {
	return FE_GP(static_cast<unsigned char>(reg));
}

template <typename Function, typename... Operands>
bool emit_fadec(emitter *out, Function function, Operands... operands) {
	uint8_t bytes[16]{};
	unsigned length = function(bytes, 0, operands...);
	if (length == 0 || length > 15) {
		zend_tpde_set_diagnostic(out->diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"the pinned TPDE/Fadec x86-64 encoder rejected an instruction");
		return false;
	}
	return emit_bytes(out, bytes, length);
}

bool mov_imm64(emitter *out, register_id reg, uint64_t value) {
	return emit_fadec(out, fe64_MOV64ri, fadec_register(reg),
		static_cast<int64_t>(value));
}

bool load_memory(emitter *out, register_id reg, register_id base,
		uint32_t displacement) {
	return emit_fadec(out, fe64_MOV64rm, fadec_register(reg),
		FE_MEM(fadec_register(base), 0, FE_NOREG,
			static_cast<int32_t>(displacement)));
}

bool store_memory(emitter *out, register_id base, uint32_t displacement,
		register_id reg) {
	return emit_fadec(out, fe64_MOV64mr,
		FE_MEM(fadec_register(base), 0, FE_NOREG,
			static_cast<int32_t>(displacement)), fadec_register(reg));
}

bool load_slot(emitter *out, register_id reg, uint32_t slot) {
	return load_memory(out, reg, RSI, slot * 8);
}

bool store_slot(emitter *out, register_id reg, uint32_t slot) {
	return store_memory(out, RSI, slot * 8, reg);
}

bool load_value(emitter *out, zend_mir_value_id id, register_id reg) {
	int32_t index = zend_tpde_value_index(out->plan, id);
	if (index < 0) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Linux x86-64 instruction references an unknown value");
		return false;
	}
	return load_slot(out, reg, static_cast<uint32_t>(index));
}

bool store_result(emitter *out, const zend_tpde_instruction *instruction,
		register_id reg) {
	int32_t index = zend_tpde_value_index(out->plan, instruction->record.result_id);
	if (index < 0) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Linux x86-64 instruction result is unknown");
		return false;
	}
	return store_slot(out, reg, static_cast<uint32_t>(index));
}

bool binary_reg(emitter *out, uint8_t opcode, register_id destination,
		register_id source) {
	switch (opcode) {
		case 0x01:
			return emit_fadec(out, fe64_ADD64rr, fadec_register(destination),
				fadec_register(source));
		case 0x09:
			return emit_fadec(out, fe64_OR64rr, fadec_register(destination),
				fadec_register(source));
		case 0x21:
			return emit_fadec(out, fe64_AND64rr, fadec_register(destination),
				fadec_register(source));
		case 0x29:
			return emit_fadec(out, fe64_SUB64rr, fadec_register(destination),
				fadec_register(source));
		case 0x31:
			return emit_fadec(out, fe64_XOR64rr, fadec_register(destination),
				fadec_register(source));
		case 0x39:
			return emit_fadec(out, fe64_CMP64rr, fadec_register(destination),
				fadec_register(source));
		default:
			return false;
	}
}

bool cmp_regs(emitter *out, register_id left, register_id right) {
	return binary_reg(out, UINT8_C(0x39), left, right);
}

bool setcc_rax(emitter *out, uint8_t condition) {
	bool encoded;
	switch (condition) {
		case 2:
			encoded = emit_fadec(out, (fe64_SETC8r), FE_AX);
			break;
		case 4:
			encoded = emit_fadec(out, (fe64_SETZ8r), FE_AX);
			break;
		case 5:
			encoded = emit_fadec(out, (fe64_SETNZ8r), FE_AX);
			break;
		case 6:
			encoded = emit_fadec(out, (fe64_SETBE8r), FE_AX);
			break;
		case 7:
			encoded = emit_fadec(out, (fe64_SETA8r), FE_AX);
			break;
		case 12:
			encoded = emit_fadec(out, (fe64_SETL8r), FE_AX);
			break;
		case 14:
			encoded = emit_fadec(out, (fe64_SETLE8r), FE_AX);
			break;
		case 15:
			encoded = emit_fadec(out, (fe64_SETG8r), FE_AX);
			break;
		default:
			return false;
	}
	return encoded && emit_fadec(out, (fe64_MOVZXr64r8), FE_AX, FE_AX);
}

bool mov_reg(emitter *out, register_id destination, register_id source) {
	return emit_fadec(out, fe64_MOV64rr, fadec_register(destination),
		fadec_register(source));
}

int32_t block_index(const zend_tpde_plan *plan, zend_mir_block_id id) {
	for (uint32_t i = 0; i < plan->block_count; ++i) {
		if (plan->blocks[i].id == id) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

bool add_jump_patch(emitter *out, zend_mir_block_id target) {
	if (out->patch_count == out->patch_capacity) {
		return false;
	}
	uint8_t bytes[16]{};
	unsigned length = fe64_JMP(bytes, FE_JMPL, bytes + 5);
	if (length != 5) {
		zend_tpde_set_diagnostic(out->diag,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"the pinned TPDE/Fadec encoder rejected a long jump");
		return false;
	}
	size_t displacement = out->image->text_size + length - 4;
	if (!emit_bytes(out, bytes, length)) {
		return false;
	}
	out->patches[out->patch_count++] = {
		displacement, out->image->text_size, target
	};
	return true;
}

bool emit_phi_edge(emitter *out, zend_mir_block_id from,
		zend_mir_block_id to) {
	uint32_t predecessors = out->plan->view->predecessor_count(
		out->plan->view->context, to);
	uint32_t predecessor_index = UINT32_MAX;
	for (uint32_t i = 0; i < predecessors; ++i) {
		zend_mir_block_id predecessor;
		if (!out->plan->view->predecessor_at(out->plan->view->context, to, i,
				&predecessor)) {
			return false;
		}
		if (predecessor == from) {
			predecessor_index = i;
			break;
		}
	}
	if (predecessor_index == UINT32_MAX) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Linux x86-64 branch is absent from the predecessor table");
		return false;
	}
	for (uint32_t i = 0; i < out->plan->instruction_count; ++i) {
		const zend_tpde_instruction *phi = &out->plan->instructions[i];
		if (phi->record.block_id != to || phi->record.opcode != ZEND_MIR_OPCODE_PHI) {
			continue;
		}
		zend_mir_value_id input = zend_tpde_operand_at(out->plan, phi,
			predecessor_index);
		int32_t result_index = zend_tpde_value_index(out->plan, phi->record.result_id);
		if (predecessor_index >= phi->operand_count || result_index < 0
				|| !load_value(out, input, RAX)
				|| !store_slot(out, RAX, out->plan->value_count
					+ static_cast<uint32_t>(result_index))) {
			return false;
		}
	}
	for (uint32_t i = 0; i < out->plan->instruction_count; ++i) {
		const zend_tpde_instruction *phi = &out->plan->instructions[i];
		if (phi->record.block_id != to || phi->record.opcode != ZEND_MIR_OPCODE_PHI) {
			continue;
		}
		int32_t result_index = zend_tpde_value_index(out->plan, phi->record.result_id);
		if (result_index < 0
				|| !load_slot(out, RAX, out->plan->value_count
					+ static_cast<uint32_t>(result_index))
				|| !store_slot(out, RAX, static_cast<uint32_t>(result_index))) {
			return false;
		}
	}
	return true;
}

bool emit_edge(emitter *out, zend_mir_block_id from, zend_mir_block_id to) {
	return emit_phi_edge(out, from, to) && add_jump_patch(out, to);
}

bool load_unary(emitter *out, const zend_tpde_instruction *instruction) {
	return instruction->operand_count == 1
		&& load_value(out, zend_tpde_operand_at(out->plan, instruction, 0), RAX);
}

bool load_binary(emitter *out, const zend_tpde_instruction *instruction) {
	return instruction->operand_count == 2
		&& load_value(out, zend_tpde_operand_at(out->plan, instruction, 0), RAX)
		&& load_value(out, zend_tpde_operand_at(out->plan, instruction, 1), RCX);
}

uint32_t scalar_kind(zend_mir_representation representation) {
	switch (representation) {
		case ZEND_MIR_REPRESENTATION_I1:
			return ZEND_NATIVE_SCALAR_BOOL;
		case ZEND_MIR_REPRESENTATION_DOUBLE:
			return ZEND_NATIVE_SCALAR_DOUBLE;
		case ZEND_MIR_REPRESENTATION_I8:
		case ZEND_MIR_REPRESENTATION_I16:
		case ZEND_MIR_REPRESENTATION_I32:
		case ZEND_MIR_REPRESENTATION_I64:
			return ZEND_NATIVE_SCALAR_LONG;
		default:
			return ZEND_NATIVE_SCALAR_NULL;
	}
}

bool emit_return(emitter *out, const zend_tpde_instruction *instruction) {
	if (!load_unary(out, instruction)) {
		return false;
	}
	int32_t value_index = zend_tpde_value_index(out->plan,
		zend_tpde_operand_at(out->plan, instruction, 0));
	uint32_t kind = value_index < 0 ? ZEND_NATIVE_SCALAR_NULL
		: scalar_kind(out->plan->values[value_index].representation);
	return store_memory(out, R11, 0, RAX)
		&& emit_fadec(out, fe64_MOV32mi,
			FE_MEM(FE_R11, 0, FE_NOREG, 8), static_cast<int32_t>(kind))
		&& emit_fadec(out, fe64_POPr, FE_BP)
		&& emit_fadec(out, fe64_RET);
}

bool emit_f64_binary(emitter *out, const zend_tpde_instruction *instruction,
		uint8_t opcode) {
	if (!load_binary(out, instruction)
			|| !emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM0, FE_AX)
			|| !emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM1, FE_CX)) {
		return false;
	}
	bool encoded = opcode == 0x58
		? emit_fadec(out, fe64_SSE_ADDSDrr, FE_XMM0, FE_XMM1)
		: opcode == 0x5c
			? emit_fadec(out, fe64_SSE_SUBSDrr, FE_XMM0, FE_XMM1)
			: opcode == 0x59
				? emit_fadec(out, fe64_SSE_MULSDrr, FE_XMM0, FE_XMM1)
				: false;
	return encoded
		&& emit_fadec(out, fe64_SSE_MOVQ_X2Grr, FE_AX, FE_XMM0)
		&& store_result(out, instruction, RAX);
}

bool emit_compare_result(emitter *out, const zend_tpde_instruction *instruction,
		uint8_t condition) {
	return load_binary(out, instruction) && cmp_regs(out, RAX, RCX)
		&& setcc_rax(out, condition) && store_result(out, instruction, RAX);
}

bool emit_f64_compare(emitter *out, const zend_tpde_instruction *instruction,
		uint8_t condition) {
	return load_binary(out, instruction)
		&& emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM0, FE_AX)
		&& emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM1, FE_CX)
		&& emit_fadec(out, fe64_SSE_UCOMISDrr, FE_XMM0, FE_XMM1)
		&& setcc_rax(out, condition) && store_result(out, instruction, RAX);
}

bool emit_instruction(emitter *out, const zend_tpde_instruction *instruction) {
	const zend_mir_instruction_record &record = instruction->record;
	switch (record.opcode) {
		case ZEND_MIR_OPCODE_CONSTANT:
		case ZEND_MIR_OPCODE_PHI:
		case ZEND_MIR_OPCODE_STATEPOINT:
		case ZEND_MIR_OPCODE_SCALAR_DROP:
			return true;
		case ZEND_MIR_OPCODE_COPY:
		case ZEND_MIR_OPCODE_CANONICALIZE:
		case ZEND_MIR_OPCODE_I1_TO_I64:
			return load_unary(out, instruction) && store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW:
			return load_binary(out, instruction) && binary_reg(out, 0x01, RAX, RCX)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
			return load_binary(out, instruction) && binary_reg(out, 0x29, RAX, RCX)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			return load_binary(out, instruction)
				&& emit_fadec(out, fe64_IMUL64rr, FE_AX, FE_CX)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_MOD_NONZERO:
			return load_binary(out, instruction)
				&& emit_fadec(out, fe64_CQO)
				&& emit_fadec(out, fe64_IDIV64r, FE_CX)
				&& store_result(out, instruction, RDX);
		case ZEND_MIR_OPCODE_I64_SHL_CHECKED:
			return load_binary(out, instruction)
				&& emit_fadec(out, fe64_SHL64rr, FE_AX, FE_CX)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED:
			return load_binary(out, instruction)
				&& emit_fadec(out, fe64_SAR64rr, FE_AX, FE_CX)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_BIT_OR:
		case ZEND_MIR_OPCODE_I64_BIT_AND:
		case ZEND_MIR_OPCODE_I64_BIT_XOR:
		case ZEND_MIR_OPCODE_I1_XOR: {
			uint8_t opcode = record.opcode == ZEND_MIR_OPCODE_I64_BIT_OR ? 0x09
				: record.opcode == ZEND_MIR_OPCODE_I64_BIT_AND ? 0x21 : 0x31;
			return load_binary(out, instruction) && binary_reg(out, opcode, RAX, RCX)
				&& store_result(out, instruction, RAX);
		}
		case ZEND_MIR_OPCODE_I64_BIT_NOT:
			return load_unary(out, instruction)
				&& emit_fadec(out, fe64_NOT64r, FE_AX)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I1_NOT:
			return load_unary(out, instruction)
				&& emit_fadec(out, fe64_TEST64rr, FE_AX, FE_AX)
				&& setcc_rax(out, 4) && store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_EQ:
		case ZEND_MIR_OPCODE_I1_EQ:
			return emit_compare_result(out, instruction, 4);
		case ZEND_MIR_OPCODE_I64_LT:
			return emit_compare_result(out, instruction, 12);
		case ZEND_MIR_OPCODE_I64_LE:
			return emit_compare_result(out, instruction, 14);
		case ZEND_MIR_OPCODE_I64_CMP:
			if (!load_binary(out, instruction) || !cmp_regs(out, RAX, RCX)
					|| !setcc_rax(out, 12) || !mov_reg(out, R8, RAX)
					|| !cmp_regs(out, RAX, RAX) /* flags are replaced below */
					|| !load_binary(out, instruction) || !cmp_regs(out, RAX, RCX)
					|| !setcc_rax(out, 15) || !binary_reg(out, 0x29, RAX, R8)) {
				return false;
			}
			return store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_F64_ADD:
			return emit_f64_binary(out, instruction, 0x58);
		case ZEND_MIR_OPCODE_F64_SUB:
			return emit_f64_binary(out, instruction, 0x5c);
		case ZEND_MIR_OPCODE_F64_MUL:
			return emit_f64_binary(out, instruction, 0x59);
		case ZEND_MIR_OPCODE_F64_EQ:
			return emit_f64_compare(out, instruction, 4);
		case ZEND_MIR_OPCODE_F64_LT:
			return emit_f64_compare(out, instruction, 2);
		case ZEND_MIR_OPCODE_F64_LE:
			return emit_f64_compare(out, instruction, 6);
		case ZEND_MIR_OPCODE_F64_CMP: {
			if (!load_binary(out, instruction)
					|| !emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM0, FE_AX)
					|| !emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM1, FE_CX)
					|| !emit_fadec(out, fe64_SSE_UCOMISDrr, FE_XMM0, FE_XMM1)
					|| !setcc_rax(out, 2) || !mov_reg(out, R8, RAX)
					|| !load_binary(out, instruction)
					|| !emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM0, FE_AX)
					|| !emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM1, FE_CX)
					|| !emit_fadec(out, fe64_SSE_UCOMISDrr, FE_XMM0, FE_XMM1)
					|| !setcc_rax(out, 7) || !binary_reg(out, 0x29, RAX, R8)) {
				return false;
			}
			return store_result(out, instruction, RAX);
		}
		case ZEND_MIR_OPCODE_I64_TO_F64:
		case ZEND_MIR_OPCODE_I1_TO_F64:
			return load_unary(out, instruction)
				&& emit_fadec(out, fe64_SSE_CVTSI2SD64rr, FE_XMM0, FE_AX)
				&& emit_fadec(out, fe64_SSE_MOVQ_X2Grr, FE_AX, FE_XMM0)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_F64_TO_I64_CHECKED:
			return load_unary(out, instruction)
				&& emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM0, FE_AX)
				&& emit_fadec(out, fe64_SSE_CVTTSD2SI64rr, FE_AX, FE_XMM0)
				&& store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_I64_TO_I1:
			return load_unary(out, instruction)
				&& emit_fadec(out, fe64_TEST64rr, FE_AX, FE_AX)
				&& setcc_rax(out, 5) && store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_F64_TO_I1:
			return load_unary(out, instruction)
				&& emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM0, FE_AX)
				&& mov_imm64(out, RCX, 0)
				&& emit_fadec(out, fe64_SSE_MOVQ_G2Xrr, FE_XMM1, FE_CX)
				&& emit_fadec(out, fe64_SSE_UCOMISDrr, FE_XMM0, FE_XMM1)
				&& setcc_rax(out, 5) && store_result(out, instruction, RAX);
		case ZEND_MIR_OPCODE_BRANCH: {
			zend_mir_block_id target;
			if (out->plan->view->successor_count(out->plan->view->context,
					record.block_id) != 1
					|| !out->plan->view->successor_at(out->plan->view->context,
						record.block_id, 0, &target)) {
				return false;
			}
			return emit_edge(out, record.block_id, target);
		}
		case ZEND_MIR_OPCODE_COND_BRANCH: {
			zend_mir_block_id true_target, false_target;
			if (!load_unary(out, instruction)
					|| out->plan->view->successor_count(out->plan->view->context,
						record.block_id) != 2
					|| !out->plan->view->successor_at(out->plan->view->context,
						record.block_id, 0, &true_target)
					|| !out->plan->view->successor_at(out->plan->view->context,
						record.block_id, 1, &false_target)
					|| !emit_fadec(out, fe64_TEST64rr, FE_AX, FE_AX)) {
				return false;
			}
			uint8_t bytes[16]{};
			unsigned length = fe64_JZ(bytes, FE_JMPL, bytes + 6);
			if (length != 6) {
				zend_tpde_set_diagnostic(out->diag,
					ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
					"the pinned TPDE/Fadec encoder rejected a long conditional jump");
				return false;
			}
			size_t displacement_offset = out->image->text_size + length - 4;
			if (!emit_bytes(out, bytes, length)
					|| !emit_edge(out, record.block_id, true_target)) {
				return false;
			}
			size_t false_offset = out->image->text_size;
			int64_t delta = static_cast<int64_t>(false_offset)
				- static_cast<int64_t>(displacement_offset + 4);
			if (delta < INT32_MIN || delta > INT32_MAX) {
				return false;
			}
			int32_t rel32 = static_cast<int32_t>(delta);
			std::memcpy(out->image->text + displacement_offset, &rel32, sizeof(rel32));
			return emit_edge(out, record.block_id, false_target);
		}
		case ZEND_MIR_OPCODE_RETURN:
			return emit_return(out, instruction);
		default:
			zend_tpde_set_diagnostic(out->diag,
				ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
				"opcode is outside the executable W03/W04 Linux x86-64 slice");
			return false;
	}
}

bool initialize_values(emitter *out) {
	for (uint32_t i = 0; i < out->plan->value_count; ++i) {
		const zend_tpde_value &value = out->plan->values[i];
		if (value.argument_index >= 0) {
			if (!load_memory(out, RAX, RDI,
					static_cast<uint32_t>(value.argument_index) * 16)
					|| !store_slot(out, RAX, i)) {
				return false;
			}
		} else if (value.constant) {
			if (!mov_imm64(out, RAX, value.constant_bits) || !store_slot(out, RAX, i)) {
				return false;
			}
		}
	}
	return true;
}

bool patch_branches(emitter *out) {
	for (uint32_t i = 0; i < out->patch_count; ++i) {
		const branch_patch &patch = out->patches[i];
		int32_t target_index = block_index(out->plan, patch.target);
		if (target_index < 0 || out->labels[target_index] == SIZE_MAX) {
			zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"Linux x86-64 branch target is absent");
			return false;
		}
		int64_t delta = static_cast<int64_t>(out->labels[target_index])
			- static_cast<int64_t>(patch.next_instruction);
		if (delta < INT32_MIN || delta > INT32_MAX) {
			zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"Linux x86-64 branch displacement exceeds rel32");
			return false;
		}
		int32_t rel32 = static_cast<int32_t>(delta);
		std::memcpy(out->image->text + patch.displacement_offset,
			&rel32, sizeof(rel32));
	}
	return true;
}

} // namespace

zend_result zend_tpde_emit_linux_x64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	if (plan->value_count > UINT32_MAX / 16
			|| plan->argument_count > UINT32_MAX / 16
			|| plan->instruction_count > UINT32_MAX / 3) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Linux x86-64 module exceeds the checked displacement bound");
		return FAILURE;
	}
	emitter out{};
	out.plan = plan;
	out.image = image;
	out.diag = diag;
	out.patch_capacity = plan->instruction_count * 3;
	out.labels = static_cast<size_t *>(std::malloc(plan->block_count * sizeof(size_t)));
	out.patches = static_cast<branch_patch *>(std::calloc(
		out.patch_capacity == 0 ? 1 : out.patch_capacity, sizeof(branch_patch)));
	if (out.labels == nullptr || out.patches == nullptr) {
		std::free(out.labels);
		std::free(out.patches);
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate Linux x86-64 relocation state");
		return FAILURE;
	}
	for (uint32_t i = 0; i < plan->block_count; ++i) {
		out.labels[i] = SIZE_MAX;
	}
	bool ok = emit_fadec(&out, fe64_PUSHr, FE_BP)
		&& emit_fadec(&out, fe64_MOV64rr, FE_BP, FE_SP)
		&& emit_fadec(&out, fe64_MOV64rr, FE_R11, FE_DX)
		&& initialize_values(&out);
	for (uint32_t block = 0; ok && block < plan->block_count; ++block) {
		out.labels[block] = image->text_size;
		for (uint32_t i = 0; ok && i < plan->instruction_count; ++i) {
			if (plan->instructions[i].record.block_id == plan->blocks[block].id) {
				ok = emit_instruction(&out, &plan->instructions[i]);
			}
		}
	}
	ok = ok && patch_branches(&out);
	std::free(out.labels);
	std::free(out.patches);
	if (!ok && (diag == nullptr || diag->code == ZEND_NATIVE_DIAGNOSTIC_OK)) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Linux x86-64 code emission failed a checked MIR invariant");
	}
	return ok ? SUCCESS : FAILURE;
}
