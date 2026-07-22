#include <string.h>

#include "zend_mir_control_flow_internal.h"

static bool zend_mir_w04_source_contract_ok(
	const zend_mir_lowering_source_view *source)
{
	return source != NULL
		&& source->contract_version == ZEND_MIR_W04_CONTRACT_VERSION
		&& source->opcode_count != NULL && source->opcode_at != NULL
		&& source->ssa_count != NULL && source->ssa_at != NULL
		&& source->ssa_use_count != NULL && source->ssa_use_at != NULL
		&& source->ssa_def_count != NULL && source->ssa_def_at != NULL
		&& source->literal_count != NULL && source->literal_at != NULL
		&& source->block_count != NULL && source->block_at != NULL
		&& source->edge_count != NULL && source->edge_at != NULL
		&& source->phi_count != NULL && source->phi_at != NULL
		&& source->phi_input_count != NULL && source->phi_input_at != NULL;
}

static bool zend_mir_w04_block_by_id(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out)
{
	uint32_t i;
	for (i = 0; i < source->block_count(source->context); i++) {
		zend_mir_source_block_ref block;
		if (!source->block_at(source->context, i, &block)) {
			return false;
		}
		if (block.id == id) {
			if (out != NULL) {
				*out = block;
			}
			return true;
		}
	}
	return false;
}

static bool zend_mir_w04_ssa_exists(
	const zend_mir_lowering_source_view *source, uint32_t ssa_variable_id)
{
	zend_mir_source_ssa_ref ssa;
	return ssa_variable_id <= ZEND_MIR_VALUE_ORIGINAL_MAX
		&& ssa_variable_id < source->ssa_count(source->context)
		&& source->ssa_at(source->context, ssa_variable_id, &ssa)
		&& ssa.ssa_variable_id == ssa_variable_id
		&& ssa.source_slot_kind >= ZEND_MIR_SOURCE_SLOT_CV
		&& ssa.source_slot_kind <= ZEND_MIR_SOURCE_SLOT_VAR;
}

static bool zend_mir_w04_operand_is_valid(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_operand_ref *operand)
{
	switch (operand->kind) {
		case ZEND_MIR_SOURCE_OPERAND_UNUSED:
			return true;
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			return operand->index < source->literal_count(source->context);
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
			return operand->slot_kind >= ZEND_MIR_SOURCE_SLOT_CV
				&& operand->slot_kind <= ZEND_MIR_SOURCE_SLOT_VAR;
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			return zend_mir_w04_ssa_exists(
				source, operand->ssa_variable_id);
		default:
			return false;
	}
}

static bool zend_mir_w04_validate_linear_source(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	uint32_t opcode_count = source->opcode_count(source->context);
	uint32_t i;
	for (i = 0; i < source->ssa_count(source->context); i++) {
		zend_mir_source_ssa_ref ssa;
		if (!source->ssa_at(source->context, i, &ssa)
				|| ssa.ssa_variable_id != i
				|| ssa.ssa_variable_id > ZEND_MIR_VALUE_ORIGINAL_MAX
				|| ssa.source_slot_kind < ZEND_MIR_SOURCE_SLOT_CV
				|| ssa.source_slot_kind > ZEND_MIR_SOURCE_SLOT_VAR
				|| (ssa.definition_opline_index != ZEND_MIR_ID_INVALID
					&& ssa.definition_opline_index >= opcode_count)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	for (i = 0; i < source->ssa_use_count(source->context); i++) {
		zend_mir_source_ssa_use_ref use;
		if (!source->ssa_use_at(source->context, i, &use)
				|| !zend_mir_w04_ssa_exists(source, use.ssa_variable_id)
				|| use.opline_index >= opcode_count
				|| use.operand_index > 2) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	for (i = 0; i < source->ssa_def_count(source->context); i++) {
		zend_mir_source_ssa_def_ref definition;
		if (!source->ssa_def_at(source->context, i, &definition)
				|| !zend_mir_w04_ssa_exists(
					source, definition.ssa_variable_id)
				|| definition.opline_index >= opcode_count
				|| !zend_mir_w04_operand_is_valid(
					source, &definition.destination)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	for (i = 0; i < source->literal_count(source->context); i++) {
		zend_mir_source_literal_ref literal;
		if (!source->literal_at(source->context, i, &literal)
				|| literal.literal_index != i
				|| literal.kind < ZEND_MIR_SOURCE_LITERAL_NULL
				|| literal.kind > ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_dominates(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id dominator, zend_mir_source_block_id block)
{
	uint32_t remaining = source->block_count(source->context);
	while (remaining-- != 0) {
		zend_mir_source_block_ref record;
		if (block == dominator) {
			return true;
		}
		if (!zend_mir_w04_block_by_id(source, block, &record)
				|| record.immediate_dominator == ZEND_MIR_ID_INVALID
				|| record.immediate_dominator == block) {
			return false;
		}
		block = record.immediate_dominator;
	}
	return false;
}

static bool zend_mir_w04_has_protected_root(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id block)
{
	uint32_t remaining = source->block_count(source->context);
	while (remaining-- != 0) {
		zend_mir_source_block_ref record;
		if (!zend_mir_w04_block_by_id(source, block, &record)) {
			return false;
		}
		if (record.immediate_dominator == ZEND_MIR_ID_INVALID) {
			return (record.flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0;
		}
		if (record.immediate_dominator == block) {
			return false;
		}
		block = record.immediate_dominator;
	}
	return false;
}

static bool zend_mir_w04_ssa_definition_block(
	const zend_mir_lowering_source_view *source, uint32_t ssa_variable_id,
	zend_mir_source_block_id *block_id, bool *is_live_in)
{
	zend_mir_source_ssa_ref ssa;
	uint32_t i;
	if (block_id == NULL || is_live_in == NULL
			|| !source->ssa_at(source->context, ssa_variable_id, &ssa)
			|| ssa.ssa_variable_id != ssa_variable_id) {
		return false;
	}
	for (i = 0; i < source->phi_count(source->context); i++) {
		zend_mir_source_phi_ref phi;
		if (!source->phi_at(source->context, i, &phi)) {
			return false;
		}
		if (phi.result_ssa_variable_id == ssa_variable_id) {
			*block_id = phi.block_id;
			*is_live_in = false;
			return true;
		}
	}
	if (ssa.definition_opline_index == ZEND_MIR_ID_INVALID) {
		*block_id = ZEND_MIR_ID_INVALID;
		*is_live_in = true;
		return true;
	}
	{
		zend_mir_source_opcode_ref definition;
		if (!source->opcode_at(source->context,
				ssa.definition_opline_index, &definition)) {
			return false;
		}
		*block_id = definition.block_id;
		*is_live_in = false;
		return true;
	}
}

static bool zend_mir_w04_validate_blocks(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation,
	bool allow_protected_control_flow)
{
	uint32_t block_count = source->block_count(source->context);
	uint32_t opcode_count = source->opcode_count(source->context);
	uint32_t entries = 0;
	uint32_t i;
	if (block_count == 0 || block_count > ZEND_MIR_ID_MAX) {
		validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
		return false;
	}
	for (i = 0; i < block_count; i++) {
		zend_mir_source_block_ref block;
		uint32_t j;
		if (!source->block_at(source->context, i, &block)
				|| block.id != i
				|| block.first_opcode_ordinal > opcode_count
				|| block.opcode_count > opcode_count - block.first_opcode_ordinal
				|| (block.flags
					& ~(ZEND_MIR_SOURCE_BLOCK_ENTRY
						| ZEND_MIR_SOURCE_BLOCK_REACHABLE
						| ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER
						| ZEND_MIR_SOURCE_BLOCK_PROTECTED
						| ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE
						| ZEND_MIR_SOURCE_BLOCK_CATCH_ENTRY
						| ZEND_MIR_SOURCE_BLOCK_FINALLY_ENTRY)) != 0) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE) != 0) {
			validation->diagnostic = ZEND_MIRL_W04_IRREDUCIBLE_LOOP;
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0
				&& !allow_protected_control_flow) {
			validation->diagnostic = ZEND_MIRL_W04_PROTECTED_REGION;
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_ENTRY) != 0) {
			entries++;
			validation->entry_block_id = block.id;
			if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0
					|| block.immediate_dominator != ZEND_MIR_ID_INVALID) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				return false;
			}
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_ENTRY) == 0
				&& !((block.flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0
					&& block.immediate_dominator == ZEND_MIR_ID_INVALID)
				&& (block.immediate_dominator >= block_count
					|| block.immediate_dominator == block.id)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		if (block.loop_header != ZEND_MIR_ID_INVALID) {
			zend_mir_source_block_ref header;
			if (block.loop_header >= block_count
					|| !zend_mir_w04_block_by_id(
						source, block.loop_header, &header)
					|| (header.flags
						& ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER) == 0
					|| !zend_mir_w04_dominates(
						source, block.loop_header, block.id)) {
				validation->diagnostic = ZEND_MIRL_W04_IRREDUCIBLE_LOOP;
				return false;
			}
		}
		for (j = 0; j < block.opcode_count; j++) {
			zend_mir_source_opcode_ref opcode;
			if (!source->opcode_at(source->context,
					block.first_opcode_ordinal + j, &opcode)
					|| opcode.block_id != block.id
					|| !zend_mir_w04_operand_is_valid(source, &opcode.op1)
					|| !zend_mir_w04_operand_is_valid(source, &opcode.op2)
					|| !zend_mir_w04_operand_is_valid(
						source, &opcode.result)) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				return false;
			}
		}
	}
	for (i = 0; i < opcode_count; i++) {
		zend_mir_source_opcode_ref opcode;
		zend_mir_source_block_ref block;
		if (!source->opcode_at(source->context, i, &opcode)
				|| !zend_mir_w04_block_by_id(source, opcode.block_id, &block)
				|| (block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0
				|| i < block.first_opcode_ordinal
				|| i - block.first_opcode_ordinal >= block.opcode_count) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	if (entries != 1) {
		validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
		return false;
	}
	for (i = 0; i < block_count; i++) {
		zend_mir_source_block_ref block;
		if (!source->block_at(source->context, i, &block)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) != 0
				&& !zend_mir_w04_dominates(
					source, validation->entry_block_id, block.id)
				&& !zend_mir_w04_has_protected_root(source, block.id)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_validate_edges(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	uint32_t edge_count = source->edge_count(source->context);
	uint32_t block_count = source->block_count(source->context);
	uint32_t i;
	for (i = 0; i < edge_count; i++) {
		zend_mir_source_edge_ref edge;
		uint32_t j;
		uint32_t same_from = 0;
		uint32_t same_to = 0;
		if (!source->edge_at(source->context, i, &edge)
				|| edge.id != i
				|| edge.from_block_id >= block_count
				|| edge.to_block_id >= block_count
				|| (edge.flags
					& ~(ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
						| ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
						| ZEND_MIR_SOURCE_EDGE_BACKEDGE
						| ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY)) != 0
				|| (edge.flags & (ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
						| ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP)) == 0) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		{
			zend_mir_source_block_ref from;
			zend_mir_source_block_ref to;
			if (!zend_mir_w04_block_by_id(source, edge.from_block_id, &from)
					|| !zend_mir_w04_block_by_id(
						source, edge.to_block_id, &to)
					|| (from.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0
					|| (to.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				return false;
			}
		}
		for (j = 0; j < edge_count; j++) {
			zend_mir_source_edge_ref peer;
			if (!source->edge_at(source->context, j, &peer)) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				return false;
			}
			if (peer.from_block_id == edge.from_block_id) {
				if (peer.successor_index == edge.successor_index && j != i) {
					validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
					return false;
				}
				same_from++;
			}
			if (peer.to_block_id == edge.to_block_id) {
				if (peer.predecessor_index == edge.predecessor_index && j != i) {
					validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
					return false;
				}
				same_to++;
			}
		}
		if (same_from > 2
				|| edge.successor_index >= same_from
				|| edge.predecessor_index >= same_to) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		if ((edge.flags & ZEND_MIR_SOURCE_EDGE_BACKEDGE) != 0) {
			zend_mir_source_block_ref header;
			if (!zend_mir_w04_dominates(
					source, edge.to_block_id, edge.from_block_id)
					|| !zend_mir_w04_block_by_id(
						source, edge.to_block_id, &header)
					|| (header.flags
						& ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER) == 0) {
				validation->diagnostic = ZEND_MIRL_W04_IRREDUCIBLE_LOOP;
				return false;
			}
		}
		if (((edge.flags & ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY) != 0)
				!= ((edge.flags & ZEND_MIR_SOURCE_EDGE_BACKEDGE) != 0)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_validate_phis(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	uint32_t phi_count = source->phi_count(source->context);
	uint32_t input_count = source->phi_input_count(source->context);
	uint32_t block_count = source->block_count(source->context);
	uint32_t i;
	for (i = 0; i < phi_count; i++) {
		zend_mir_source_phi_ref phi;
		zend_mir_source_ssa_ref result_ssa;
		uint32_t predecessor_count = 0;
		uint32_t j;
		uint32_t seen = 0;
		if (!source->phi_at(source->context, i, &phi)
				|| phi.id != i || phi.block_id >= block_count
				|| !zend_mir_w04_ssa_exists(
					source, phi.result_ssa_variable_id)
				|| phi.source_slot_kind < ZEND_MIR_SOURCE_SLOT_CV
				|| phi.source_slot_kind > ZEND_MIR_SOURCE_SLOT_VAR
				|| phi.kind < ZEND_MIR_SOURCE_PHI_MERGE
				|| phi.kind > ZEND_MIR_SOURCE_PHI_PI_RANGE
				|| !source->ssa_at(source->context,
					phi.result_ssa_variable_id, &result_ssa)
				|| result_ssa.source_slot_kind != phi.source_slot_kind
				|| result_ssa.source_slot != phi.source_slot_index) {
			validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
			return false;
		}
		for (j = 0; j < i; j++) {
			zend_mir_source_phi_ref earlier;
			if (!source->phi_at(source->context, j, &earlier)
					|| earlier.result_ssa_variable_id
						== phi.result_ssa_variable_id
					|| (earlier.block_id == phi.block_id
						&& earlier.kind != ZEND_MIR_SOURCE_PHI_MERGE
						&& phi.kind == ZEND_MIR_SOURCE_PHI_MERGE)) {
				validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
				return false;
			}
		}
		if (phi.kind == ZEND_MIR_SOURCE_PHI_PI_TYPE
				&& phi.constraint.type_mask == 0) {
			validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
			return false;
		}
		if (phi.kind == ZEND_MIR_SOURCE_PHI_PI_RANGE
				&& (phi.constraint.flags
					& ~(ZEND_MIR_SOURCE_PHI_RANGE_MIN_UNBOUNDED
						| ZEND_MIR_SOURCE_PHI_RANGE_MAX_UNBOUNDED
						| ZEND_MIR_SOURCE_PHI_RANGE_NEGATED)) != 0) {
			validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
			return false;
		}
		if (phi.kind == ZEND_MIR_SOURCE_PHI_PI_RANGE
				&& ((phi.constraint.min_ssa_variable_id
						!= ZEND_MIR_ID_INVALID
						&& !zend_mir_w04_ssa_exists(source,
							phi.constraint.min_ssa_variable_id))
					|| (phi.constraint.max_ssa_variable_id
						!= ZEND_MIR_ID_INVALID
						&& !zend_mir_w04_ssa_exists(source,
							phi.constraint.max_ssa_variable_id)))) {
			validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
			return false;
		}
		for (j = 0; j < source->edge_count(source->context); j++) {
			zend_mir_source_edge_ref edge;
			if (!source->edge_at(source->context, j, &edge)) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				return false;
			}
			if (edge.to_block_id == phi.block_id) {
				predecessor_count++;
			}
		}
		for (j = 0; j < input_count; j++) {
			zend_mir_source_phi_input_ref input;
			zend_mir_source_edge_ref edge;
			zend_mir_source_block_id definition_block;
			uint32_t k;
			bool is_live_in;
			bool predecessor_found = false;
			if (!source->phi_input_at(source->context, j, &input)) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				return false;
			}
			if (input.phi_id != phi.id) {
				continue;
			}
			if ((phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
					&& input.input_index != seen)
					|| !zend_mir_w04_ssa_exists(
						source, input.source_ssa_variable_id)) {
				validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
				return false;
			}
			for (k = 0; k < source->edge_count(source->context); k++) {
				if (!source->edge_at(source->context, k, &edge)) {
					return false;
				}
				if (edge.to_block_id == phi.block_id
						&& edge.predecessor_index == input.input_index
						&& edge.from_block_id == input.predecessor_block_id) {
					predecessor_found = true;
					break;
				}
			}
			if (!predecessor_found) {
				validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
				return false;
			}
			if (!zend_mir_w04_ssa_definition_block(source,
					input.source_ssa_variable_id, &definition_block,
					&is_live_in)
					|| (!is_live_in
						&& !zend_mir_w04_dominates(source,
							definition_block,
							input.predecessor_block_id))) {
				validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
				return false;
			}
			seen++;
		}
		if ((phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
					&& (seen == 0 || seen != predecessor_count))
				|| (phi.kind != ZEND_MIR_SOURCE_PHI_MERGE && seen != 1)) {
			validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_validate_source_impl(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation,
	bool allow_protected_control_flow)
{
	if (validation == NULL) {
		return false;
	}
	memset(validation, 0, sizeof(*validation));
	validation->entry_block_id = ZEND_MIR_ID_INVALID;
	validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
	if (!zend_mir_w04_source_contract_ok(source)
			|| !zend_mir_w04_validate_linear_source(source, validation)
			|| !zend_mir_w04_validate_blocks(
				source, validation, allow_protected_control_flow)
			|| !zend_mir_w04_validate_edges(source, validation)
			|| !zend_mir_w04_validate_phis(source, validation)) {
		return false;
	}
	validation->proofs = ZEND_MIR_W04_PROOF_SOURCE_CFG_COMPLETE
		| ZEND_MIR_W04_PROOF_REDUCIBLE_CFG
		| ZEND_MIR_W04_PROOF_BRANCH_SUCCESSOR_ORDER
		| ZEND_MIR_W04_PROOF_PHI_PREDECESSOR_ORDER
		| ZEND_MIR_W04_PROOF_EDGE_STATEPOINTS;
	{
		uint32_t i;
		bool protected_region = false;
		for (i = 0; i < source->block_count(source->context); i++) {
			zend_mir_source_block_ref block;
			if (!source->block_at(source->context, i, &block)) {
				return false;
			}
			protected_region |=
				(block.flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0;
		}
		if (!protected_region) {
			validation->proofs |= ZEND_MIR_W04_PROOF_NO_PROTECTED_REGION;
		}
	}
	validation->diagnostic = ZEND_MIRL_OK;
	return true;
}

bool zend_mir_w04_validate_source(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	return zend_mir_w04_validate_source_impl(source, validation, false);
}

bool zend_mir_w04_validate_source_for_protected_control_flow(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	return zend_mir_w04_validate_source_impl(source, validation, true);
}

zend_mir_w04_branch_kind zend_mir_w04_branch_kind_for_opcode(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_MIR_W04_OPCODE_JMP:
			return ZEND_MIR_W04_BRANCH_UNCONDITIONAL;
		case ZEND_MIR_W04_OPCODE_JMPZ:
			return ZEND_MIR_W04_BRANCH_IF_FALSE;
		case ZEND_MIR_W04_OPCODE_JMPNZ:
			return ZEND_MIR_W04_BRANCH_IF_TRUE;
		case ZEND_MIR_W04_OPCODE_JMPZ_EX:
			return ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT;
		case ZEND_MIR_W04_OPCODE_JMPNZ_EX:
			return ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT;
		case ZEND_MIR_W08_OPCODE_CATCH:
			return ZEND_MIR_W04_BRANCH_CATCH;
		default:
			return ZEND_MIR_W04_BRANCH_KIND_INVALID;
	}
}
