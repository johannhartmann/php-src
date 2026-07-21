// SPDX-License-Identifier: PHP-3.01

#include "Zend/Native/TPDE/Common/zend_tpde_internal.hpp"
#include "Zend/Native/TPDE/DarwinA64/zend_tpde_apple_a64_abi.hpp"

#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

constexpr uint32_t X0 = 0;
constexpr uint32_t X1 = 1;
constexpr uint32_t X2 = 2;
constexpr uint32_t X8 = 8;
constexpr uint32_t X9 = 9;
constexpr uint32_t X10 = 10;
constexpr uint32_t X29 = 29;
constexpr uint32_t X30 = 30;

struct branch_patch {
	size_t offset;
	zend_mir_block_id target;
	uint32_t base;
	uint32_t bits;
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

bool emit32(emitter *out, uint32_t instruction) {
	if (!zend_tpde_image_u32(out->image, instruction)) {
		zend_tpde_set_diagnostic(out->diag,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to grow the Darwin A64 text image");
		return false;
	}
	return true;
}

int32_t block_index(const zend_tpde_plan *plan, zend_mir_block_id id) {
	for (uint32_t i = 0; i < plan->block_count; ++i) {
		if (plan->blocks[i].id == id) {
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

bool add_patch(emitter *out, zend_mir_block_id target, uint32_t base,
		uint32_t bits) {
	if (out->patch_count == out->patch_capacity) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Darwin A64 branch patch count exceeds the checked bound");
		return false;
	}
	out->patches[out->patch_count++] = {
		out->image->text_size, target, base, bits
	};
	return emit32(out, base);
}

bool mov_imm64(emitter *out, uint32_t rd, uint64_t value) {
	if (!emit32(out, UINT32_C(0xd2800000)
			| (static_cast<uint32_t>(value & UINT64_C(0xffff)) << 5) | rd)) {
		return false;
	}
	for (uint32_t hw = 1; hw < 4; ++hw) {
		uint32_t part = static_cast<uint32_t>((value >> (hw * 16)) & UINT64_C(0xffff));
		if (part != 0 && !emit32(out, UINT32_C(0xf2800000) | (hw << 21)
				| (part << 5) | rd)) {
			return false;
		}
	}
	return true;
}

bool load_slot(emitter *out, uint32_t reg, uint32_t slot) {
	return emit32(out, UINT32_C(0xf9400000) | (slot << 10) | (X1 << 5) | reg);
}

bool store_slot(emitter *out, uint32_t reg, uint32_t slot) {
	return emit32(out, UINT32_C(0xf9000000) | (slot << 10) | (X1 << 5) | reg);
}

bool load_value(emitter *out, zend_mir_value_id id, uint32_t reg) {
	int32_t index = zend_tpde_value_index(out->plan, id);
	if (index < 0) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Darwin A64 instruction references an unknown value");
		return false;
	}
	return load_slot(out, reg, static_cast<uint32_t>(index));
}

bool store_result(emitter *out, const zend_tpde_instruction *instruction,
		uint32_t reg) {
	int32_t index = zend_tpde_value_index(out->plan, instruction->record.result_id);
	if (index < 0) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Darwin A64 instruction result is unknown");
		return false;
	}
	return store_slot(out, reg, static_cast<uint32_t>(index));
}

bool emit_cset(emitter *out, uint32_t rd, uint32_t condition) {
	return emit32(out, UINT32_C(0x9a9f07e0) | ((condition ^ 1U) << 12) | rd);
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
			zend_tpde_set_diagnostic(out->diag,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"Darwin A64 cannot read a predecessor edge");
			return false;
		}
		if (predecessor == from) {
			predecessor_index = i;
			break;
		}
	}
	if (predecessor_index == UINT32_MAX) {
		zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Darwin A64 branch is absent from the predecessor table");
		return false;
	}

	/* Copy all PHI inputs to temporaries before assigning any PHI result. */
	for (uint32_t i = 0; i < out->plan->instruction_count; ++i) {
		const zend_tpde_instruction *phi = &out->plan->instructions[i];
		if (phi->record.block_id != to || phi->record.opcode != ZEND_MIR_OPCODE_PHI) {
			continue;
		}
		zend_mir_value_id input = zend_tpde_operand_at(out->plan, phi,
			predecessor_index);
		int32_t result_index = zend_tpde_value_index(out->plan, phi->record.result_id);
		if (!zend_mir_id_is_valid(input) || result_index < 0
				|| predecessor_index >= phi->operand_count
				|| !load_value(out, input, X8)
				|| !store_slot(out, X8, out->plan->value_count
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
				|| !load_slot(out, X8, out->plan->value_count
					+ static_cast<uint32_t>(result_index))
				|| !store_slot(out, X8, static_cast<uint32_t>(result_index))) {
			return false;
		}
	}
	return true;
}

bool emit_edge(emitter *out, zend_mir_block_id from, zend_mir_block_id to) {
	return emit_phi_edge(out, from, to)
		&& add_patch(out, to, UINT32_C(0x14000000), 26);
}

bool load_binary(emitter *out, const zend_tpde_instruction *instruction) {
	return instruction->operand_count == 2
		&& load_value(out, zend_tpde_operand_at(out->plan, instruction, 0), X8)
		&& load_value(out, zend_tpde_operand_at(out->plan, instruction, 1), X9);
}

bool load_unary(emitter *out, const zend_tpde_instruction *instruction) {
	return instruction->operand_count == 1
		&& load_value(out, zend_tpde_operand_at(out->plan, instruction, 0), X8);
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
	if (value_index < 0 || !emit32(out, UINT32_C(0xf9000048))) { /* str x8, [x2] */
		return false;
	}
	if (!mov_imm64(out, X8, scalar_kind(out->plan->values[value_index].representation))
			|| !emit32(out, UINT32_C(0xb9000848)) /* str w8, [x2, #8] */
			|| !emit32(out, UINT32_C(0xa8c17bfd)) /* ldp x29, x30, [sp], #16 */
			|| !emit32(out, UINT32_C(0xd65f03c0))) { /* ret */
		return false;
	}
	return true;
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
			return load_unary(out, instruction) && store_result(out, instruction, X8);
		case ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x8b000000) | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0xcb000000) | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9b007c00) | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_MOD_NONZERO:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9ac00c00) | (X9 << 16) | (X8 << 5) | X10)
				/* msub x10, x10, x9, x8: dividend - quotient * divisor */
				&& emit32(out, UINT32_C(0x9b008000) | (X9 << 16) | (X8 << 10)
					| (X10 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_SHL_CHECKED:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9ac02000) | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9ac02800) | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_BIT_OR:
		case ZEND_MIR_OPCODE_I64_BIT_AND:
		case ZEND_MIR_OPCODE_I64_BIT_XOR: {
			uint32_t base = record.opcode == ZEND_MIR_OPCODE_I64_BIT_OR
				? UINT32_C(0xaa000000)
				: record.opcode == ZEND_MIR_OPCODE_I64_BIT_AND
					? UINT32_C(0x8a000000) : UINT32_C(0xca000000);
			return load_binary(out, instruction)
				&& emit32(out, base | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		}
		case ZEND_MIR_OPCODE_I64_BIT_NOT:
			return load_unary(out, instruction)
				&& emit32(out, UINT32_C(0xaa2003e0) | (X8 << 16) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I1_NOT:
			return load_unary(out, instruction) && mov_imm64(out, X9, 0)
				&& emit32(out, UINT32_C(0xeb00001f) | (X9 << 16) | (X8 << 5))
				&& emit_cset(out, X10, 0) && store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I1_XOR:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0xca000000) | (X9 << 16) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_EQ:
		case ZEND_MIR_OPCODE_I64_LT:
		case ZEND_MIR_OPCODE_I64_LE:
		case ZEND_MIR_OPCODE_I1_EQ: {
			uint32_t condition = record.opcode == ZEND_MIR_OPCODE_I64_LT ? 11
				: record.opcode == ZEND_MIR_OPCODE_I64_LE ? 13 : 0;
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0xeb00001f) | (X9 << 16) | (X8 << 5))
				&& emit_cset(out, X10, condition)
				&& store_result(out, instruction, X10);
		}
		case ZEND_MIR_OPCODE_I64_CMP:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0xeb00001f) | (X9 << 16) | (X8 << 5))
				&& emit_cset(out, X9, 11) && emit_cset(out, X10, 12)
				&& emit32(out, UINT32_C(0xcb000000) | (X9 << 16) | (X10 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_F64_ADD:
		case ZEND_MIR_OPCODE_F64_SUB:
		case ZEND_MIR_OPCODE_F64_MUL: {
			uint32_t base = record.opcode == ZEND_MIR_OPCODE_F64_ADD
				? UINT32_C(0x1e602800)
				: record.opcode == ZEND_MIR_OPCODE_F64_SUB
					? UINT32_C(0x1e603800) : UINT32_C(0x1e600800);
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9e670000) | (X8 << 5) | X8)
				&& emit32(out, UINT32_C(0x9e670000) | (X9 << 5) | X9)
				&& emit32(out, base | (X9 << 16) | (X8 << 5) | X10)
				&& emit32(out, UINT32_C(0x9e660000) | (X10 << 5) | X10)
				&& store_result(out, instruction, X10);
		}
		case ZEND_MIR_OPCODE_F64_EQ:
		case ZEND_MIR_OPCODE_F64_LT:
		case ZEND_MIR_OPCODE_F64_LE: {
			uint32_t condition = record.opcode == ZEND_MIR_OPCODE_F64_LT ? 11
				: record.opcode == ZEND_MIR_OPCODE_F64_LE ? 13 : 0;
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9e670000) | (X8 << 5) | X8)
				&& emit32(out, UINT32_C(0x9e670000) | (X9 << 5) | X9)
				&& emit32(out, UINT32_C(0x1e602000) | (X9 << 16) | (X8 << 5))
				&& emit_cset(out, X10, condition)
				&& store_result(out, instruction, X10);
		}
		case ZEND_MIR_OPCODE_F64_CMP:
			return load_binary(out, instruction)
				&& emit32(out, UINT32_C(0x9e670000) | (X8 << 5) | X8)
				&& emit32(out, UINT32_C(0x9e670000) | (X9 << 5) | X9)
				&& emit32(out, UINT32_C(0x1e602000) | (X9 << 16) | (X8 << 5))
				&& emit_cset(out, X9, 11) && emit_cset(out, X10, 12)
				&& emit32(out, UINT32_C(0xcb000000) | (X9 << 16) | (X10 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_TO_F64:
		case ZEND_MIR_OPCODE_I1_TO_F64:
			return load_unary(out, instruction)
				&& emit32(out, UINT32_C(0x9e620000) | (X8 << 5) | X10)
				&& emit32(out, UINT32_C(0x9e660000) | (X10 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_F64_TO_I64_CHECKED:
			return load_unary(out, instruction)
				&& emit32(out, UINT32_C(0x9e670000) | (X8 << 5) | X8)
				&& emit32(out, UINT32_C(0x9e780000) | (X8 << 5) | X10)
				&& store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I64_TO_I1:
			return load_unary(out, instruction) && mov_imm64(out, X9, 0)
				&& emit32(out, UINT32_C(0xeb00001f) | (X9 << 16) | (X8 << 5))
				&& emit_cset(out, X10, 1) && store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_F64_TO_I1:
			return load_unary(out, instruction)
				&& emit32(out, UINT32_C(0x9e670000) | (X8 << 5) | X8)
				&& emit32(out, UINT32_C(0x1e602008) | (X8 << 5))
				&& emit_cset(out, X10, 1) && store_result(out, instruction, X10);
		case ZEND_MIR_OPCODE_I1_TO_I64:
			return load_unary(out, instruction) && store_result(out, instruction, X8);
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
						record.block_id, 1, &false_target)) {
				return false;
			}
			size_t cbz_offset = out->image->text_size;
			if (!emit32(out, UINT32_C(0xb4000000) | X8)
					|| !emit_edge(out, record.block_id, true_target)) {
				return false;
			}
			size_t false_offset = out->image->text_size;
			int64_t delta = static_cast<int64_t>(false_offset)
				- static_cast<int64_t>(cbz_offset);
			if ((delta & 3) != 0 || delta / 4 < -(INT64_C(1) << 18)
					|| delta / 4 >= (INT64_C(1) << 18)) {
				return false;
			}
			uint32_t encoded = UINT32_C(0xb4000000)
				| ((static_cast<uint32_t>(delta / 4) & UINT32_C(0x7ffff)) << 5) | X8;
			std::memcpy(out->image->text + cbz_offset, &encoded, sizeof(encoded));
			return emit_edge(out, record.block_id, false_target);
		}
		case ZEND_MIR_OPCODE_RETURN:
			return emit_return(out, instruction);
		default:
			zend_tpde_set_diagnostic(out->diag,
				ZEND_NATIVE_DIAGNOSTIC_UNSUPPORTED_OPCODE,
				"opcode is outside the executable W03/W04 Darwin A64 slice");
			return false;
	}
}

bool initialize_values(emitter *out) {
	for (uint32_t i = 0; i < out->plan->value_count; ++i) {
		const zend_tpde_value &value = out->plan->values[i];
		if (value.argument_index >= 0) {
			uint32_t offset = static_cast<uint32_t>(value.argument_index) * 2;
			if (!emit32(out, UINT32_C(0xf9400000) | (offset << 10)
					| (X0 << 5) | X8)
					|| !store_slot(out, X8, i)) {
				return false;
			}
		} else if (value.constant) {
			if (!mov_imm64(out, X8, value.constant_bits) || !store_slot(out, X8, i)) {
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
				"Darwin A64 branch target is absent");
			return false;
		}
		int64_t delta = static_cast<int64_t>(out->labels[target_index])
			- static_cast<int64_t>(patch.offset);
		int64_t words = delta / 4;
		int64_t limit = INT64_C(1) << (patch.bits - 1);
		if ((delta & 3) != 0 || words < -limit || words >= limit) {
			zend_tpde_set_diagnostic(out->diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"Darwin A64 branch displacement is outside the encoder range");
			return false;
		}
		uint32_t mask = (UINT32_C(1) << patch.bits) - 1;
		uint32_t encoded = patch.base | (static_cast<uint32_t>(words) & mask);
		std::memcpy(out->image->text + patch.offset, &encoded, sizeof(encoded));
	}
	return true;
}

} // namespace

zend_result zend_tpde_emit_darwin_arm64(
	const zend_tpde_plan *plan,
	zend_native_image *image,
	zend_native_diagnostic *diag) {
	static_assert(
		zend::native::tpde::CCAssignerAppleA64::valid_fixed_entry());
	/* 12-bit scaled LDR/STR offsets keep the baseline encoder compact. */
	if (plan->value_count > 2047 || plan->argument_count > 2047
			|| plan->instruction_count > UINT32_MAX / 3) {
		zend_tpde_set_diagnostic(diag, ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"Darwin A64 module exceeds the checked baseline displacement bound");
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
			"unable to allocate Darwin A64 relocation state");
		return FAILURE;
	}
	for (uint32_t i = 0; i < plan->block_count; ++i) {
		out.labels[i] = SIZE_MAX;
	}

	bool ok = emit32(&out, UINT32_C(0xa9bf7bfd)) /* stp x29, x30, [sp, #-16]! */
		&& emit32(&out, UINT32_C(0x910003fd)) /* mov x29, sp */
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
			"Darwin A64 code emission failed a checked MIR invariant");
	}
	return ok ? SUCCESS : FAILURE;
}
