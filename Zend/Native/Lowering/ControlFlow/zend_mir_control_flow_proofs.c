#include <stdlib.h>
#include <string.h>

#include "zend_mir_control_flow_internal.h"

static int zend_mir_w04_compare_edge_pairs(
	const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *) left;
	const uint64_t b = *(const uint64_t *) right;

	return (a > b) - (a < b);
}

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
	zend_mir_source_block_ref block;

	if (id >= source->block_count(source->context)
			|| !source->block_at(source->context, id, &block)
			|| block.id != id) {
		return false;
	}
	if (out != NULL) {
		*out = block;
	}
	return true;
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
	{
		uint32_t *roots = malloc((size_t) block_count * sizeof(*roots));
		uint32_t *path = malloc((size_t) block_count * sizeof(*path));
		bool valid = true;

		if (roots == NULL || path == NULL) {
			free(path);
			free(roots);
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		for (i = 0; i < block_count; i++) {
			roots[i] = ZEND_MIR_ID_INVALID;
		}
		for (i = 0; i < block_count; i++) {
			zend_mir_source_block_ref block;
			uint32_t current;
			uint32_t path_length = 0;
			uint32_t root;
			uint32_t j;

			if (!source->block_at(source->context, i, &block)) {
				valid = false;
				break;
			}
			if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
				continue;
			}
			current = block.id;
			while (roots[current] == ZEND_MIR_ID_INVALID) {
				zend_mir_source_block_ref record;

				if (!source->block_at(source->context, current, &record)
						|| record.id != current) {
					valid = false;
					break;
				}
				if (record.immediate_dominator == ZEND_MIR_ID_INVALID) {
					roots[current] = current;
					break;
				}
				if (record.immediate_dominator >= block_count
						|| record.immediate_dominator == current
						|| path_length == block_count) {
					valid = false;
					break;
				}
				path[path_length++] = current;
				current = record.immediate_dominator;
			}
			if (!valid) {
				break;
			}
			root = roots[current];
			for (j = 0; j < path_length; j++) {
				roots[path[j]] = root;
			}
			if (root != validation->entry_block_id) {
				zend_mir_source_block_ref root_block;

				if (!source->block_at(source->context, root, &root_block)
						|| (root_block.flags
							& ZEND_MIR_SOURCE_BLOCK_PROTECTED) == 0) {
					valid = false;
					break;
				}
			}
		}
		free(path);
		free(roots);
		if (!valid) {
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
	uint32_t *successor_counts = NULL;
	uint32_t *predecessor_counts = NULL;
	uint32_t *successor_offsets = NULL;
	uint32_t *predecessor_offsets = NULL;
	uint8_t *successor_seen = NULL;
	uint32_t *predecessor_from = NULL;
	uint32_t running_successors = 0;
	uint32_t running_predecessors = 0;
	bool valid = false;
	uint32_t i;

	successor_counts = calloc(block_count, sizeof(*successor_counts));
	predecessor_counts = calloc(block_count, sizeof(*predecessor_counts));
	successor_offsets = calloc(block_count, sizeof(*successor_offsets));
	predecessor_offsets = calloc(block_count, sizeof(*predecessor_offsets));
	successor_seen = calloc(edge_count == 0 ? 1 : edge_count,
		sizeof(*successor_seen));
	predecessor_from = calloc(edge_count == 0 ? 1 : edge_count,
		sizeof(*predecessor_from));
	if (successor_counts == NULL || predecessor_counts == NULL
			|| successor_offsets == NULL || predecessor_offsets == NULL
			|| successor_seen == NULL || predecessor_from == NULL) {
		goto done;
	}
	for (i = 0; i < edge_count; i++) {
		zend_mir_source_edge_ref edge;
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
			goto done;
		}
		{
			zend_mir_source_block_ref from;
			zend_mir_source_block_ref to;
			if (!zend_mir_w04_block_by_id(source, edge.from_block_id, &from)
					|| !zend_mir_w04_block_by_id(
						source, edge.to_block_id, &to)
					|| (from.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0
					|| (to.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
				goto done;
			}
		}
		if (successor_counts[edge.from_block_id] == UINT32_MAX
				|| predecessor_counts[edge.to_block_id] == UINT32_MAX) {
			goto done;
		}
		successor_counts[edge.from_block_id]++;
		predecessor_counts[edge.to_block_id]++;
		if ((edge.flags & ZEND_MIR_SOURCE_EDGE_BACKEDGE) != 0) {
			zend_mir_source_block_ref header;
			if (!zend_mir_w04_dominates(
					source, edge.to_block_id, edge.from_block_id)
					|| !zend_mir_w04_block_by_id(
						source, edge.to_block_id, &header)
					|| (header.flags
						& ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER) == 0) {
				validation->diagnostic = ZEND_MIRL_W04_IRREDUCIBLE_LOOP;
				goto done;
			}
		}
		if (((edge.flags & ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY) != 0)
				!= ((edge.flags & ZEND_MIR_SOURCE_EDGE_BACKEDGE) != 0)) {
			goto done;
		}
	}
	for (i = 0; i < block_count; i++) {
		successor_offsets[i] = running_successors;
		predecessor_offsets[i] = running_predecessors;
		running_successors += successor_counts[i];
		running_predecessors += predecessor_counts[i];
	}
	if (running_successors != edge_count
			|| running_predecessors != edge_count) {
		goto done;
	}
	for (i = 0; i < edge_count; i++) {
		zend_mir_source_edge_ref edge;
		uint32_t successor_slot;
		uint32_t predecessor_slot;

		if (!source->edge_at(source->context, i, &edge)
				|| edge.successor_index
					>= successor_counts[edge.from_block_id]
				|| edge.predecessor_index
					>= predecessor_counts[edge.to_block_id]) {
			goto done;
		}
		successor_slot = successor_offsets[edge.from_block_id]
			+ edge.successor_index;
		predecessor_slot = predecessor_offsets[edge.to_block_id]
			+ edge.predecessor_index;
		if (successor_seen[successor_slot]
				|| (predecessor_from[predecessor_slot] != 0
					&& predecessor_from[predecessor_slot]
						!= edge.from_block_id + 1)) {
			goto done;
		}
		successor_seen[successor_slot] = 1;
		predecessor_from[predecessor_slot] = edge.from_block_id + 1;
	}
	valid = true;

done:
	free(predecessor_from);
	free(successor_seen);
	free(predecessor_offsets);
	free(successor_offsets);
	free(predecessor_counts);
	free(successor_counts);
	if (!valid && validation->diagnostic != ZEND_MIRL_W04_IRREDUCIBLE_LOOP) {
		validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
	}
	return valid;
}

static bool zend_mir_w04_validate_terminators(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	uint32_t block_count = source->block_count(source->context);
	uint32_t edge_count = source->edge_count(source->context);
	uint32_t *outgoing_counts;
	uint32_t i;

	outgoing_counts = calloc(block_count, sizeof(*outgoing_counts));
	if (outgoing_counts == NULL) {
		validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
		return false;
	}
	for (i = 0; i < edge_count; i++) {
		zend_mir_source_edge_ref edge;

		if (!source->edge_at(source->context, i, &edge)
				|| edge.from_block_id >= block_count
				|| outgoing_counts[edge.from_block_id] == UINT32_MAX) {
			free(outgoing_counts);
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		outgoing_counts[edge.from_block_id]++;
	}
	for (i = 0; i < block_count; i++) {
		zend_mir_source_block_ref block;
		zend_mir_source_opcode_ref opcode;
		zend_mir_w04_branch_kind kind = ZEND_MIR_W04_BRANCH_KIND_INVALID;
		uint32_t opcode_number = 0;
		uint32_t j;

		if (!source->block_at(source->context, i, &block)) {
			free(outgoing_counts);
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if (block.opcode_count != 0) {
			for (j = block.opcode_count; j != 0; j--) {
				if (!source->opcode_at(source->context,
						block.first_opcode_ordinal + j - 1, &opcode)) {
					free(outgoing_counts);
					validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
					return false;
				}
				if (opcode.zend_opcode_number != ZEND_MIR_W03_OPCODE_NOP) {
					opcode_number = opcode.zend_opcode_number;
					kind = zend_mir_w04_branch_kind_for_opcode(opcode_number);
					break;
				}
			}
		}
		if (!zend_mir_w04_branch_edge_count_is_valid(
				kind, opcode_number, outgoing_counts[block.id])) {
			free(outgoing_counts);
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			return false;
		}
	}
	free(outgoing_counts);
	return true;
}

bool zend_mir_w04_resolve_edge_pi_input(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_phi_ref *consumer,
	const zend_mir_source_phi_input_ref *consumer_input,
	uint32_t *source_ssa_variable_id)
{
	uint32_t producer_index;

	if (source == NULL || consumer == NULL || consumer_input == NULL
			|| source->phi_count == NULL || source->phi_at == NULL
			|| source->phi_input_count == NULL
			|| source->phi_input_at == NULL
			|| consumer->kind != ZEND_MIR_SOURCE_PHI_MERGE) {
		return false;
	}
	for (producer_index = 0;
			producer_index < source->phi_count(source->context);
			producer_index++) {
		zend_mir_source_phi_ref producer;
		uint32_t input_index;
		uint32_t producer_input_count = 0;
		uint32_t producer_source_ssa_variable_id = ZEND_MIR_ID_INVALID;

		if (!source->phi_at(
				source->context, producer_index, &producer)) {
			return false;
		}
		if (producer.kind == ZEND_MIR_SOURCE_PHI_MERGE
				|| producer.block_id != consumer->block_id
				|| producer.result_ssa_variable_id
					!= consumer_input->source_ssa_variable_id) {
			continue;
		}
		for (input_index = 0;
			input_index < source->phi_input_count(source->context);
			input_index++) {
			zend_mir_source_phi_input_ref producer_input;

			if (!source->phi_input_at(
					source->context, input_index, &producer_input)) {
				return false;
			}
			if (producer_input.phi_id != producer.id) {
				continue;
			}
			producer_input_count++;
			if (producer_input.predecessor_block_id
						!= consumer_input->predecessor_block_id
					|| producer_input.input_index != 0) {
				return false;
			}
			producer_source_ssa_variable_id =
				producer_input.source_ssa_variable_id;
		}
		if (producer_input_count != 1) {
			return false;
		}
		if (source_ssa_variable_id != NULL) {
			*source_ssa_variable_id = producer_source_ssa_variable_id;
		}
		return true;
	}
	return false;
}

static bool zend_mir_w04_validate_phis(
	const zend_mir_lowering_source_view *source,
	zend_mir_w04_validation *validation)
{
	uint32_t phi_count = source->phi_count(source->context);
	uint32_t input_count = source->phi_input_count(source->context);
	uint32_t block_count = source->block_count(source->context);
	uint32_t edge_count = source->edge_count(source->context);
	uint32_t ssa_count = source->ssa_count(source->context);
	zend_mir_source_phi_ref *phis = NULL;
	zend_mir_source_block_ref *blocks = NULL;
	uint32_t *phi_by_ssa = NULL;
	uint32_t *input_offsets = NULL;
	uint32_t *input_indices = NULL;
	uint32_t *predecessor_offsets = NULL;
	uint32_t *incoming_from = NULL;
	uint64_t *edge_pairs = NULL;
	uint32_t *child_offsets = NULL;
	uint32_t *children = NULL;
	uint32_t *work = NULL;
	uint32_t *preorder = NULL;
	uint32_t *subtree_end = NULL;
	uint32_t *roots = NULL;
	uint32_t *stack = NULL;
	uint32_t *stack_cursor = NULL;
	uint8_t *block_has_pi = NULL;
	uint32_t time = 0;
	bool valid = false;
	uint32_t i;

	validation->diagnostic = ZEND_MIRL_W04_UNSUPPORTED_PHI_PI;
	if (phi_count == 0) {
		return input_count == 0;
	}
	phis = malloc((size_t) phi_count * sizeof(*phis));
	blocks = malloc((size_t) block_count * sizeof(*blocks));
	phi_by_ssa = malloc((size_t) ssa_count * sizeof(*phi_by_ssa));
	input_offsets = calloc((size_t) phi_count + 1,
		sizeof(*input_offsets));
	input_indices = input_count == 0 ? NULL
		: malloc((size_t) input_count * sizeof(*input_indices));
	predecessor_offsets = calloc((size_t) block_count + 1,
		sizeof(*predecessor_offsets));
	incoming_from = edge_count == 0 ? NULL
		: malloc((size_t) edge_count * sizeof(*incoming_from));
	edge_pairs = edge_count == 0 ? NULL
		: malloc((size_t) edge_count * sizeof(*edge_pairs));
	child_offsets = calloc((size_t) block_count + 1,
		sizeof(*child_offsets));
	children = malloc((size_t) block_count * sizeof(*children));
	work = malloc((size_t) (block_count > phi_count
		? block_count : phi_count) * sizeof(*work));
	preorder = malloc((size_t) block_count * sizeof(*preorder));
	subtree_end = malloc((size_t) block_count * sizeof(*subtree_end));
	roots = malloc((size_t) block_count * sizeof(*roots));
	stack = malloc((size_t) block_count * sizeof(*stack));
	stack_cursor = malloc((size_t) block_count * sizeof(*stack_cursor));
	block_has_pi = calloc(block_count, sizeof(*block_has_pi));
	if (phis == NULL || blocks == NULL || phi_by_ssa == NULL
			|| input_offsets == NULL
			|| (input_count != 0 && input_indices == NULL)
			|| predecessor_offsets == NULL
			|| (edge_count != 0
				&& (incoming_from == NULL || edge_pairs == NULL))
			|| child_offsets == NULL || children == NULL || work == NULL
			|| preorder == NULL || subtree_end == NULL || roots == NULL
			|| stack == NULL || stack_cursor == NULL
			|| block_has_pi == NULL) {
		validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
		goto done;
	}
	for (i = 0; i < ssa_count; i++) {
		phi_by_ssa[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < block_count; i++) {
		if (!source->block_at(source->context, i, &blocks[i])
				|| blocks[i].id != i) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			goto done;
		}
		preorder[i] = ZEND_MIR_ID_INVALID;
		subtree_end[i] = ZEND_MIR_ID_INVALID;
		roots[i] = ZEND_MIR_ID_INVALID;
		if (blocks[i].immediate_dominator < block_count
				&& blocks[i].immediate_dominator != i) {
			child_offsets[blocks[i].immediate_dominator + 1]++;
		}
	}
	for (i = 1; i <= block_count; i++) {
		child_offsets[i] += child_offsets[i - 1];
	}
	memcpy(work, child_offsets, (size_t) block_count * sizeof(*work));
	for (i = 0; i < block_count; i++) {
		if (blocks[i].immediate_dominator < block_count
				&& blocks[i].immediate_dominator != i) {
			children[work[blocks[i].immediate_dominator]++] = i;
		}
	}
	for (i = 0; i < block_count; i++) {
		uint32_t depth;
		if (blocks[i].immediate_dominator < block_count
				&& blocks[i].immediate_dominator != i) {
			continue;
		}
		depth = 1;
		stack[0] = i;
		stack_cursor[0] = child_offsets[i];
		preorder[i] = time++;
		roots[i] = i;
		while (depth != 0) {
			const uint32_t block = stack[depth - 1];
			uint32_t *next_child = &stack_cursor[depth - 1];
			if (*next_child < child_offsets[block + 1]) {
				const uint32_t child = children[(*next_child)++];
				if (preorder[child] != ZEND_MIR_ID_INVALID) {
					continue;
				}
				preorder[child] = time++;
				roots[child] = i;
				stack[depth] = child;
				stack_cursor[depth] = child_offsets[child];
				depth++;
			} else {
				subtree_end[block] = time;
				depth--;
			}
		}
	}
	for (i = 0; i < edge_count; i++) {
		zend_mir_source_edge_ref edge;
		if (!source->edge_at(source->context, i, &edge)
				|| edge.to_block_id >= block_count
				|| edge.from_block_id >= block_count
				|| edge.predecessor_index == UINT32_MAX) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			goto done;
		}
		if (predecessor_offsets[edge.to_block_id + 1]
				<= edge.predecessor_index) {
			predecessor_offsets[edge.to_block_id + 1]
				= edge.predecessor_index + 1;
		}
		edge_pairs[i] = ((uint64_t) edge.to_block_id << 32)
			| edge.from_block_id;
	}
	for (i = 1; i <= block_count; i++) {
		predecessor_offsets[i] += predecessor_offsets[i - 1];
	}
	memcpy(work, predecessor_offsets,
		(size_t) block_count * sizeof(*work));
	for (i = 0; i < predecessor_offsets[block_count]; i++) {
		incoming_from[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < edge_count; i++) {
		zend_mir_source_edge_ref edge;
		if (!source->edge_at(source->context, i, &edge)
				|| edge.predecessor_index
					>= predecessor_offsets[edge.to_block_id + 1]
						- predecessor_offsets[edge.to_block_id]) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			goto done;
		}
		incoming_from[predecessor_offsets[edge.to_block_id]
			+ edge.predecessor_index] = edge.from_block_id;
	}
	if (edge_count > 1) {
		qsort(edge_pairs, edge_count, sizeof(*edge_pairs),
			zend_mir_w04_compare_edge_pairs);
	}
	for (i = 0; i < phi_count; i++) {
		zend_mir_source_ssa_ref result_ssa;
		zend_mir_source_phi_ref *phi = &phis[i];
		if (!source->phi_at(source->context, i, phi)
				|| phi->id != i || phi->block_id >= block_count
				|| !zend_mir_w04_ssa_exists(
					source, phi->result_ssa_variable_id)
				|| phi->source_slot_kind < ZEND_MIR_SOURCE_SLOT_CV
				|| phi->source_slot_kind > ZEND_MIR_SOURCE_SLOT_VAR
				|| phi->kind < ZEND_MIR_SOURCE_PHI_MERGE
				|| phi->kind > ZEND_MIR_SOURCE_PHI_PI_RANGE
				|| !source->ssa_at(source->context,
					phi->result_ssa_variable_id, &result_ssa)
				|| result_ssa.source_slot_kind != phi->source_slot_kind
				|| result_ssa.source_slot != phi->source_slot_index
				|| phi_by_ssa[phi->result_ssa_variable_id]
					!= ZEND_MIR_ID_INVALID
				|| (phi->kind == ZEND_MIR_SOURCE_PHI_MERGE
					&& block_has_pi[phi->block_id] != 0)) {
			goto done;
		}
		phi_by_ssa[phi->result_ssa_variable_id] = i;
		if (phi->kind != ZEND_MIR_SOURCE_PHI_MERGE) {
			block_has_pi[phi->block_id] = 1;
		}
		if (phi->kind == ZEND_MIR_SOURCE_PHI_PI_TYPE
				&& phi->constraint.type_mask == 0) {
			goto done;
		}
		if (phi->kind == ZEND_MIR_SOURCE_PHI_PI_RANGE
				&& (phi->constraint.flags
					& ~(ZEND_MIR_SOURCE_PHI_RANGE_MIN_UNBOUNDED
						| ZEND_MIR_SOURCE_PHI_RANGE_MAX_UNBOUNDED
						| ZEND_MIR_SOURCE_PHI_RANGE_NEGATED)) != 0) {
			goto done;
		}
		if (phi->kind == ZEND_MIR_SOURCE_PHI_PI_RANGE
				&& ((phi->constraint.min_ssa_variable_id
						!= ZEND_MIR_ID_INVALID
					&& !zend_mir_w04_ssa_exists(source,
						phi->constraint.min_ssa_variable_id))
					|| (phi->constraint.max_ssa_variable_id
						!= ZEND_MIR_ID_INVALID
					&& !zend_mir_w04_ssa_exists(source,
						phi->constraint.max_ssa_variable_id)))) {
			goto done;
		}
	}
	for (i = 0; i < input_count; i++) {
		zend_mir_source_phi_input_ref input;
		if (!source->phi_input_at(source->context, i, &input)
				|| input.phi_id >= phi_count
				|| input_offsets[input.phi_id + 1] == UINT32_MAX) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			goto done;
		}
		input_offsets[input.phi_id + 1]++;
	}
	for (i = 1; i <= phi_count; i++) {
		input_offsets[i] += input_offsets[i - 1];
	}
	memcpy(work, input_offsets, (size_t) phi_count * sizeof(*work));
	for (i = 0; i < input_count; i++) {
		zend_mir_source_phi_input_ref input;
		uint32_t position;
		if (!source->phi_input_at(source->context, i, &input)) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			goto done;
		}
		position = work[input.phi_id]++;
		if (position >= input_count) {
			validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
			goto done;
		}
		input_indices[position] = i;
	}
	for (i = 0; i < phi_count; i++) {
		const zend_mir_source_phi_ref *phi = &phis[i];
		const uint32_t predecessor_count =
			predecessor_offsets[phi->block_id + 1]
				- predecessor_offsets[phi->block_id];
		uint32_t j;
		uint32_t seen = 0;
		for (j = input_offsets[i]; j < input_offsets[i + 1]; j++) {
			zend_mir_source_phi_input_ref input;
			zend_mir_source_block_id definition_block;
			bool is_live_in;
			bool predecessor_found = false;
			if (!source->phi_input_at(
					source->context, input_indices[j], &input)) {
				validation->diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
				goto done;
			}
			if ((phi->kind == ZEND_MIR_SOURCE_PHI_MERGE
					&& input.input_index != seen)
					|| !zend_mir_w04_ssa_exists(
						source, input.source_ssa_variable_id)) {
				goto done;
			}
			if (phi->kind == ZEND_MIR_SOURCE_PHI_MERGE) {
				predecessor_found = input.input_index < predecessor_count
					&& incoming_from[predecessor_offsets[phi->block_id]
						+ input.input_index] == input.predecessor_block_id;
			} else {
				const uint64_t key = ((uint64_t) phi->block_id << 32)
					| input.predecessor_block_id;
				uint32_t left = 0;
				uint32_t right = edge_count;
				while (left < right) {
					const uint32_t middle = left + (right - left) / 2;
					if (edge_pairs[middle] < key) {
						left = middle + 1;
					} else {
						right = middle;
					}
				}
				predecessor_found = left < edge_count
					&& edge_pairs[left] == key;
			}
			if (!predecessor_found) {
				goto done;
			}
			{
				const uint32_t producer_index =
					phi_by_ssa[input.source_ssa_variable_id];
				if (producer_index != ZEND_MIR_ID_INVALID) {
					definition_block = phis[producer_index].block_id;
					is_live_in = false;
				} else {
					zend_mir_source_ssa_ref ssa;
					if (!source->ssa_at(source->context,
							input.source_ssa_variable_id, &ssa)) {
						goto done;
					}
					is_live_in = ssa.definition_opline_index
						== ZEND_MIR_ID_INVALID;
					if (!is_live_in) {
						zend_mir_source_opcode_ref definition;
						if (!source->opcode_at(source->context,
								ssa.definition_opline_index, &definition)) {
							goto done;
						}
						definition_block = definition.block_id;
					}
				}
			}
			if (!is_live_in) {
				const bool dominates = definition_block < block_count
					&& input.predecessor_block_id < block_count
					&& roots[definition_block]
						== roots[input.predecessor_block_id]
					&& preorder[definition_block]
						<= preorder[input.predecessor_block_id]
					&& preorder[input.predecessor_block_id]
						< subtree_end[definition_block];
				bool edge_pi = false;
				const uint32_t producer_index =
					phi_by_ssa[input.source_ssa_variable_id];
				if (!dominates && producer_index != ZEND_MIR_ID_INVALID
						&& phis[producer_index].kind
							!= ZEND_MIR_SOURCE_PHI_MERGE
						&& phis[producer_index].block_id == phi->block_id
						&& input_offsets[producer_index + 1]
							- input_offsets[producer_index] == 1) {
					zend_mir_source_phi_input_ref producer_input;
					if (!source->phi_input_at(source->context,
							input_indices[input_offsets[producer_index]],
							&producer_input)) {
						goto done;
					}
					edge_pi = producer_input.predecessor_block_id
							== input.predecessor_block_id
						&& producer_input.input_index == 0;
				}
				if (!dominates && !edge_pi) {
					goto done;
				}
			}
			seen++;
		}
		if ((phi->kind == ZEND_MIR_SOURCE_PHI_MERGE
					&& (seen == 0 || seen != predecessor_count))
				|| (phi->kind != ZEND_MIR_SOURCE_PHI_MERGE && seen != 1)) {
			goto done;
		}
	}
	valid = true;

done:
	free(block_has_pi);
	free(stack_cursor);
	free(stack);
	free(roots);
	free(subtree_end);
	free(preorder);
	free(work);
	free(children);
	free(child_offsets);
	free(edge_pairs);
	free(incoming_from);
	free(predecessor_offsets);
	free(input_indices);
	free(input_offsets);
	free(phi_by_ssa);
	free(blocks);
	free(phis);
	return valid;
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
			|| !zend_mir_w04_validate_terminators(source, validation)
			|| !zend_mir_w04_validate_phis(source, validation)) {
		return false;
	}
	validation->proofs = ZEND_MIR_W04_PROOF_SOURCE_CFG_COMPLETE
		| ZEND_MIR_W04_PROOF_BRANCH_SUCCESSOR_ORDER
		| ZEND_MIR_W04_PROOF_PHI_PREDECESSOR_ORDER
		| ZEND_MIR_W04_PROOF_EDGE_STATEPOINTS;
	{
		uint32_t i;
		bool protected_region = false;
		bool irreducible = false;
		for (i = 0; i < source->block_count(source->context); i++) {
			zend_mir_source_block_ref block;
			if (!source->block_at(source->context, i, &block)) {
				return false;
			}
			protected_region |=
				(block.flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0;
			irreducible |=
				(block.flags & ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE) != 0;
		}
		if (!protected_region) {
			validation->proofs |= ZEND_MIR_W04_PROOF_NO_PROTECTED_REGION;
		}
		if (!irreducible) {
			validation->proofs |= ZEND_MIR_W04_PROOF_REDUCIBLE_CFG;
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
		case ZEND_MIR_W08_OPCODE_FAST_CALL:
			return ZEND_MIR_W08_BRANCH_FINALLY_CALL;
		case ZEND_MIR_W08_OPCODE_FAST_RET:
			return ZEND_MIR_W08_BRANCH_FINALLY_RETURN;
		case ZEND_MIR_W09_OPCODE_JMP_SET:
			return ZEND_MIR_W09_BRANCH_JMP_SET;
		case ZEND_MIR_W09_OPCODE_COALESCE:
			return ZEND_MIR_W09_BRANCH_COALESCE;
		case ZEND_MIR_W10_OPCODE_JMP_NULL:
			return ZEND_MIR_W10_BRANCH_JMP_NULL;
		case ZEND_MIR_W10_OPCODE_THROW:
			return ZEND_MIR_W10_BRANCH_THROW;
		case ZEND_MIR_W12_OPCODE_ASSERT_CHECK:
			return ZEND_MIR_W12_BRANCH_ASSERT_CHECK;
		case ZEND_MIR_W12_OPCODE_SWITCH_LONG:
		case ZEND_MIR_W12_OPCODE_SWITCH_STRING:
		case ZEND_MIR_W12_OPCODE_MATCH:
			return ZEND_MIR_W12_BRANCH_MULTIWAY;
		case ZEND_MIR_W12_OPCODE_BIND_INIT_STATIC_OR_JMP:
			return ZEND_MIR_W12_BRANCH_BIND_STATIC;
		case ZEND_MIR_W12_OPCODE_JMP_FRAMELESS:
			return ZEND_MIR_W12_BRANCH_FRAMELESS;
		case ZEND_MIR_W09_OPCODE_FE_RESET_R:
		case ZEND_MIR_W09_OPCODE_FE_FETCH_R:
		case ZEND_MIR_W09_OPCODE_FE_RESET_RW:
		case ZEND_MIR_W09_OPCODE_FE_FETCH_RW:
			return ZEND_MIR_W09_BRANCH_ITERATOR;
		default:
			return ZEND_MIR_W04_BRANCH_KIND_INVALID;
	}
}

bool zend_mir_w04_branch_edge_count_is_valid(
	zend_mir_w04_branch_kind kind, uint32_t opcode, uint32_t edge_count)
{
	return !((kind == ZEND_MIR_W04_BRANCH_UNCONDITIONAL && edge_count != 1)
		|| ((kind >= ZEND_MIR_W04_BRANCH_IF_FALSE
				&& kind <= ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT)
			&& edge_count != 2)
		|| (kind == ZEND_MIR_W04_BRANCH_CATCH
			&& edge_count != 1 && edge_count != 2)
		|| (kind == ZEND_MIR_W08_BRANCH_FINALLY_CALL && edge_count != 2)
		|| (kind == ZEND_MIR_W08_BRANCH_FINALLY_RETURN && edge_count != 0)
		|| (kind == ZEND_MIR_W09_BRANCH_ITERATOR && edge_count != 2)
		|| (kind == ZEND_MIR_W12_BRANCH_ASSERT_CHECK && edge_count != 2)
		|| (kind == ZEND_MIR_W12_BRANCH_BIND_STATIC && edge_count != 2)
		|| (kind == ZEND_MIR_W12_BRANCH_FRAMELESS && edge_count != 2)
		|| (kind == ZEND_MIR_W12_BRANCH_MULTIWAY
			&& (opcode == ZEND_MIR_W12_OPCODE_MATCH
				? edge_count < 2 : edge_count < 3))
		|| (kind == ZEND_MIR_W10_BRANCH_THROW && edge_count != 0)
		|| (kind == ZEND_MIR_W04_BRANCH_KIND_INVALID && edge_count > 1));
}
