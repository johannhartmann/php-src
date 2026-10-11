#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zend_mir_control_flow_internal.h"
#include "../zend_mir_dominator_forest.h"

static bool zend_mir_source_contract_ok(
	const zend_mir_lowering_source_view *source)
{
	return source != NULL
		&& source->contract_version == ZEND_MIR_CONTRACT_VERSION
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

static bool zend_mir_ssa_exists(
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

#if !defined(NDEBUG)
static bool zend_mir_operand_is_valid(
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
			return zend_mir_ssa_exists(
				source, operand->ssa_variable_id);
		default:
			return false;
	}
}

static bool zend_mir_validate_linear_source(
	const zend_mir_lowering_source_view *source,
	zend_mir_validation *validation)
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
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
	for (i = 0; i < source->ssa_use_count(source->context); i++) {
		zend_mir_source_ssa_use_ref use;
		if (!source->ssa_use_at(source->context, i, &use)
				|| !zend_mir_ssa_exists(source, use.ssa_variable_id)
				|| use.opline_index >= opcode_count
				|| use.operand_index > 2) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
	for (i = 0; i < source->ssa_def_count(source->context); i++) {
		zend_mir_source_ssa_def_ref definition;
		if (!source->ssa_def_at(source->context, i, &definition)
				|| !zend_mir_ssa_exists(
					source, definition.ssa_variable_id)
				|| definition.opline_index >= opcode_count
				|| !zend_mir_operand_is_valid(
					source, &definition.destination)) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
	for (i = 0; i < source->literal_count(source->context); i++) {
		zend_mir_source_literal_ref literal;
		if (!source->literal_at(source->context, i, &literal)
				|| literal.literal_index != i
				|| literal.kind < ZEND_MIR_SOURCE_LITERAL_NULL
				|| literal.kind > ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
	return true;
}
#endif

static size_t zend_mir_graph_reserve(
	size_t *offset, size_t count, size_t size, size_t alignment,
	bool *overflow)
{
	const size_t start = (*offset + alignment - 1) & ~(alignment - 1);

	if (start < *offset
			|| (count != 0 && size > (SIZE_MAX - start) / count)) {
		*overflow = true;
		return 0;
	}
	*offset = start + count * size;
	return start;
}

void zend_mir_source_graph_destroy(zend_mir_source_graph *graph)
{
	if (graph == NULL) {
		return;
	}
	free(graph->storage);
	memset(graph, 0, sizeof(*graph));
}

/* Reads the counts and makes the one allocation every array lives in. */
static bool zend_mir_source_graph_init(
	zend_mir_source_graph *graph,
	const zend_mir_lowering_source_view *source,
	const zend_mir_dominator_forest *dominators)
{
	size_t offset = 0;
	size_t blocks_at;
	size_t edges_at;
	size_t phis_at;
	size_t inputs_at;
	size_t edge_offsets_at;
	size_t predecessor_offsets_at;
	size_t incoming_at;
	size_t input_offsets_at;
	size_t phi_by_ssa_at;
	size_t edge_pi_at;
	bool overflow = false;
	unsigned char *storage;

	memset(graph, 0, sizeof(*graph));
	graph->block_count = source->block_count(source->context);
	graph->edge_count = source->edge_count(source->context);
	graph->phi_count = source->phi_count(source->context);
	graph->input_count = source->phi_input_count(source->context);
	graph->ssa_count = source->ssa_count(source->context);
	if (dominators == NULL || dominators->count != graph->block_count) {
		return false;
	}
	graph->dominators = dominators;
	blocks_at = zend_mir_graph_reserve(&offset, graph->block_count,
		sizeof(*graph->blocks), alignof(zend_mir_source_block_ref),
		&overflow);
	edges_at = zend_mir_graph_reserve(&offset, graph->edge_count,
		sizeof(*graph->edges), alignof(zend_mir_source_edge_ref),
		&overflow);
	phis_at = zend_mir_graph_reserve(&offset, graph->phi_count,
		sizeof(*graph->phis), alignof(zend_mir_source_phi_ref),
		&overflow);
	inputs_at = zend_mir_graph_reserve(&offset, graph->input_count,
		sizeof(*graph->inputs), alignof(zend_mir_source_phi_input_ref),
		&overflow);
	edge_offsets_at = zend_mir_graph_reserve(&offset,
		(size_t) graph->block_count + 1, sizeof(uint32_t),
		alignof(uint32_t), &overflow);
	predecessor_offsets_at = zend_mir_graph_reserve(&offset,
		(size_t) graph->block_count + 1, sizeof(uint32_t),
		alignof(uint32_t), &overflow);
	incoming_at = zend_mir_graph_reserve(&offset, graph->edge_count,
		sizeof(uint32_t), alignof(uint32_t), &overflow);
	input_offsets_at = zend_mir_graph_reserve(&offset,
		(size_t) graph->phi_count + 1, sizeof(uint32_t),
		alignof(uint32_t), &overflow);
	phi_by_ssa_at = zend_mir_graph_reserve(&offset,
		graph->phi_count == 0 ? 0 : graph->ssa_count, sizeof(uint32_t),
		alignof(uint32_t), &overflow);
	edge_pi_at = zend_mir_graph_reserve(&offset, graph->input_count,
		sizeof(uint32_t), alignof(uint32_t), &overflow);
	if (overflow) {
		return false;
	}
	storage = malloc(offset);
	if (storage == NULL) {
		return false;
	}
	graph->storage = storage;
	graph->blocks = (zend_mir_source_block_ref *) (storage + blocks_at);
	graph->edges = (zend_mir_source_edge_ref *) (storage + edges_at);
	graph->phis = (zend_mir_source_phi_ref *) (storage + phis_at);
	graph->inputs = (zend_mir_source_phi_input_ref *) (storage + inputs_at);
	graph->edge_offsets = (uint32_t *) (storage + edge_offsets_at);
	graph->predecessor_offsets =
		(uint32_t *) (storage + predecessor_offsets_at);
	graph->incoming_from = (uint32_t *) (storage + incoming_at);
	graph->input_offsets = (uint32_t *) (storage + input_offsets_at);
	graph->phi_by_ssa = (uint32_t *) (storage + phi_by_ssa_at);
	graph->edge_pi_sources = (uint32_t *) (storage + edge_pi_at);
	return true;
}

static bool zend_mir_source_graph_load_blocks(
	zend_mir_source_graph *graph,
	const zend_mir_lowering_source_view *source)
{
	uint32_t i;

	for (i = 0; i < graph->block_count; i++) {
		zend_mir_source_block_ref *block = &graph->blocks[i];

		if (!source->block_at(source->context, i, block)
				|| block->id != i) {
			return false;
		}
		graph->protected_region |=
			(block->flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0;
		graph->irreducible |=
			(block->flags & ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE) != 0;
	}
	return true;
}

/*
 * Groups the edges by from block in successor order and records which
 * block each predecessor slot of a block comes from. The overlay numbers
 * edges, successors and predecessors densely, so only a malformed view can
 * fail here.
 */
static bool zend_mir_source_graph_load_edges(
	zend_mir_source_graph *graph,
	const zend_mir_lowering_source_view *source)
{
	const uint32_t block_count = graph->block_count;
	uint32_t *offsets = graph->edge_offsets;
	uint32_t *predecessors = graph->predecessor_offsets;
	uint32_t i;

	memset(offsets, 0, ((size_t) block_count + 1) * sizeof(*offsets));
	memset(predecessors, 0,
		((size_t) block_count + 1) * sizeof(*predecessors));
	for (i = 0; i < graph->edge_count; i++) {
		zend_mir_source_edge_ref edge;

		if (!source->edge_at(source->context, i, &edge)
				|| edge.id != i
				|| edge.from_block_id >= block_count
				|| edge.to_block_id >= block_count
				|| edge.predecessor_index >= graph->edge_count) {
			return false;
		}
		offsets[edge.from_block_id + 1]++;
		if (predecessors[edge.to_block_id + 1] <= edge.predecessor_index) {
			predecessors[edge.to_block_id + 1] = edge.predecessor_index + 1;
		}
		graph->edges[i].id = ZEND_MIR_ID_INVALID;
	}
	for (i = 1; i <= block_count; i++) {
		offsets[i] += offsets[i - 1];
		if (predecessors[i] > graph->edge_count - predecessors[i - 1]) {
			return false;
		}
		predecessors[i] += predecessors[i - 1];
	}
	for (i = 0; i < predecessors[block_count]; i++) {
		graph->incoming_from[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < graph->edge_count; i++) {
		zend_mir_source_edge_ref edge;
		uint32_t position;

		if (!source->edge_at(source->context, i, &edge)) {
			return false;
		}
		position = offsets[edge.from_block_id] + edge.successor_index;
		if (edge.successor_index
					>= offsets[edge.from_block_id + 1]
						- offsets[edge.from_block_id]
				|| graph->edges[position].id != ZEND_MIR_ID_INVALID) {
			return false;
		}
		graph->edges[position] = edge;
		graph->incoming_from[predecessors[edge.to_block_id]
			+ edge.predecessor_index] = edge.from_block_id;
	}
	return true;
}

static bool zend_mir_validate_blocks(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_graph *graph,
	zend_mir_validation *validation,
	bool allow_protected_control_flow)
{
	uint32_t block_count = graph->block_count;
	uint32_t opcode_count = source->opcode_count(source->context);
	uint32_t entries = 0;
	uint32_t i;
	if (block_count == 0 || block_count > ZEND_MIR_ID_MAX) {
		validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
		return false;
	}
	for (i = 0; i < block_count; i++) {
		const zend_mir_source_block_ref *block = &graph->blocks[i];
		if (block->first_opcode_ordinal > opcode_count
				|| block->opcode_count
					> opcode_count - block->first_opcode_ordinal
				|| (block->flags
					& ~(ZEND_MIR_SOURCE_BLOCK_ENTRY
						| ZEND_MIR_SOURCE_BLOCK_REACHABLE
						| ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER
						| ZEND_MIR_SOURCE_BLOCK_PROTECTED
						| ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE
						| ZEND_MIR_SOURCE_BLOCK_CATCH_ENTRY
						| ZEND_MIR_SOURCE_BLOCK_FINALLY_ENTRY)) != 0) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
		if ((block->flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0
				&& !allow_protected_control_flow) {
			validation->diagnostic = ZEND_MIRL_PROTECTED_REGION;
			return false;
		}
		if ((block->flags & ZEND_MIR_SOURCE_BLOCK_ENTRY) != 0) {
			entries++;
			validation->entry_block_id = block->id;
			if ((block->flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0
					|| block->immediate_dominator != ZEND_MIR_ID_INVALID) {
				validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
				return false;
			}
		}
		if ((block->flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if ((block->flags & ZEND_MIR_SOURCE_BLOCK_ENTRY) == 0
				&& !((block->flags & ZEND_MIR_SOURCE_BLOCK_PROTECTED) != 0
					&& block->immediate_dominator == ZEND_MIR_ID_INVALID)
				&& (block->immediate_dominator >= block_count
					|| block->immediate_dominator == block->id)) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
		/* The frontend proved the dominator chains acyclic, so the forest
		 * answers what walking this block's chain would. */
		if (block->loop_header != ZEND_MIR_ID_INVALID
				&& (block->loop_header >= block_count
					|| (graph->blocks[block->loop_header].flags
						& ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER) == 0
					|| !zend_mir_dominator_forest_dominates(
						graph->dominators, block->loop_header,
						block->id))) {
			validation->diagnostic = ZEND_MIRL_IRREDUCIBLE_LOOP;
			return false;
		}
#if !defined(NDEBUG)
		for (uint32_t j = 0; j < block->opcode_count; j++) {
			zend_mir_source_opcode_ref opcode;
			if (!source->opcode_at(source->context,
					block->first_opcode_ordinal + j, &opcode)
					|| opcode.block_id != block->id
					|| !zend_mir_operand_is_valid(source, &opcode.op1)
					|| !zend_mir_operand_is_valid(source, &opcode.op2)
					|| !zend_mir_operand_is_valid(
						source, &opcode.result)) {
				validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
				return false;
			}
		}
#endif
	}
	if (entries != 1) {
		validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
		return false;
	}
#if !defined(NDEBUG)
	for (i = 0; i < opcode_count; i++) {
		zend_mir_source_opcode_ref opcode;
		const zend_mir_source_block_ref *block;
		if (!source->opcode_at(source->context, i, &opcode)
				|| opcode.block_id >= block_count) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
		block = &graph->blocks[opcode.block_id];
		if (i < block->first_opcode_ordinal
				|| i - block->first_opcode_ordinal >= block->opcode_count) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
	{
		uint32_t *roots = malloc((size_t) block_count * sizeof(*roots));
		uint32_t *path = malloc((size_t) block_count * sizeof(*path));
		bool valid = true;

		if (roots == NULL || path == NULL) {
			free(path);
			free(roots);
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
		for (i = 0; i < block_count; i++) {
			roots[i] = ZEND_MIR_ID_INVALID;
		}
		for (i = 0; i < block_count; i++) {
			uint32_t current;
			uint32_t path_length = 0;
			uint32_t root;
			uint32_t j;

			if ((graph->blocks[i].flags
					& ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
				continue;
			}
			current = i;
			while (roots[current] == ZEND_MIR_ID_INVALID) {
				const zend_mir_source_block_ref *record =
					&graph->blocks[current];

				if (record->immediate_dominator == ZEND_MIR_ID_INVALID) {
					roots[current] = current;
					break;
				}
				if (record->immediate_dominator >= block_count
						|| record->immediate_dominator == current
						|| path_length == block_count) {
					valid = false;
					break;
				}
				path[path_length++] = current;
				current = record->immediate_dominator;
			}
			if (!valid) {
				break;
			}
			root = roots[current];
			for (j = 0; j < path_length; j++) {
				roots[path[j]] = root;
			}
			if (root != validation->entry_block_id
					&& (graph->blocks[root].flags
						& ZEND_MIR_SOURCE_BLOCK_PROTECTED) == 0) {
				valid = false;
				break;
			}
		}
		free(path);
		free(roots);
		if (!valid) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
#endif
	return true;
}

#if !defined(NDEBUG)
static bool zend_mir_validate_edges(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_graph *graph,
	zend_mir_validation *validation)
{
	uint32_t edge_count = source->edge_count(source->context);
	uint32_t block_count = graph->block_count;
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
						| ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP)) == 0
				|| (graph->blocks[edge.from_block_id].flags
					& ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0
				|| (graph->blocks[edge.to_block_id].flags
					& ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			goto done;
		}
		if (successor_counts[edge.from_block_id] == UINT32_MAX
				|| predecessor_counts[edge.to_block_id] == UINT32_MAX) {
			goto done;
		}
		successor_counts[edge.from_block_id]++;
		predecessor_counts[edge.to_block_id]++;
		if ((edge.flags & ZEND_MIR_SOURCE_EDGE_BACKEDGE) != 0
				&& (!zend_mir_dominator_forest_dominates(graph->dominators,
						edge.to_block_id, edge.from_block_id)
					|| (graph->blocks[edge.to_block_id].flags
						& ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER) == 0)) {
			validation->diagnostic = ZEND_MIRL_IRREDUCIBLE_LOOP;
			goto done;
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
	if (!valid && validation->diagnostic != ZEND_MIRL_IRREDUCIBLE_LOOP) {
		validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
	}
	return valid;
}
#endif

static bool zend_mir_validate_terminators(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_graph *graph,
	zend_mir_validation *validation)
{
	uint32_t i;

	for (i = 0; i < graph->block_count; i++) {
		const zend_mir_source_block_ref *block = &graph->blocks[i];
		zend_mir_source_opcode_ref opcode;
		zend_mir_branch_kind kind = ZEND_MIR_BRANCH_KIND_INVALID;
		uint32_t opcode_number = 0;
		uint32_t j;

		if ((block->flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		for (j = block->opcode_count; j != 0; j--) {
			if (!source->opcode_at(source->context,
					block->first_opcode_ordinal + j - 1, &opcode)) {
				validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
				return false;
			}
			if (opcode.zend_opcode_number != ZEND_MIR_OPCODE_NOP) {
				opcode_number = opcode.zend_opcode_number;
				kind = zend_mir_branch_kind_for_opcode(opcode_number);
				break;
			}
		}
		if (!zend_mir_branch_edge_count_is_valid(kind, opcode_number,
				graph->edge_offsets[i + 1] - graph->edge_offsets[i])) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			return false;
		}
	}
	return true;
}

/* Whether the view has an edge from block from to block to. */
static bool zend_mir_graph_has_edge(
	const zend_mir_source_graph *graph, uint32_t from, uint32_t to)
{
	uint32_t i;

	if (from >= graph->block_count) {
		return false;
	}
	for (i = graph->edge_offsets[from]; i < graph->edge_offsets[from + 1];
			i++) {
		if (graph->edges[i].to_block_id == to) {
			return true;
		}
	}
	return false;
}

/*
 * Loads the phis and groups their inputs by phi, then checks every input:
 * a merge input names its predecessor slot in order, a pi input an edge
 * into the pi's block, and a value defined in the function dominates the
 * predecessor, unless it is a pi of the same block taken on that edge.
 */
static bool zend_mir_validate_phis(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_graph *graph,
	zend_mir_validation *validation)
{
	const uint32_t phi_count = graph->phi_count;
	const uint32_t input_count = graph->input_count;
	const uint32_t block_count = graph->block_count;
	const zend_mir_dominator_forest *dominators = graph->dominators;
	zend_mir_source_phi_ref *phis = graph->phis;
	zend_mir_source_phi_input_ref *inputs = graph->inputs;
	uint32_t *phi_by_ssa = graph->phi_by_ssa;
	uint32_t *input_offsets = graph->input_offsets;
	uint32_t *cursors = NULL;
	uint8_t *block_has_pi = NULL;
	bool valid = false;
	uint32_t i;

	validation->diagnostic = ZEND_MIRL_UNSUPPORTED_PHI_PI;
	if (phi_count == 0) {
		return input_count == 0;
	}
	cursors = malloc((size_t) phi_count * sizeof(*cursors));
	block_has_pi = calloc(block_count, sizeof(*block_has_pi));
	if (cursors == NULL || block_has_pi == NULL) {
		validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
		goto done;
	}
	for (i = 0; i < graph->ssa_count; i++) {
		phi_by_ssa[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < phi_count; i++) {
		zend_mir_source_ssa_ref result_ssa;
		zend_mir_source_phi_ref *phi = &phis[i];
		if (!source->phi_at(source->context, i, phi)
				|| phi->id != i || phi->block_id >= block_count
				|| !zend_mir_ssa_exists(
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
					&& !zend_mir_ssa_exists(source,
						phi->constraint.min_ssa_variable_id))
					|| (phi->constraint.max_ssa_variable_id
						!= ZEND_MIR_ID_INVALID
					&& !zend_mir_ssa_exists(source,
						phi->constraint.max_ssa_variable_id)))) {
			goto done;
		}
	}
	memset(input_offsets, 0, ((size_t) phi_count + 1) * sizeof(*input_offsets));
	for (i = 0; i < input_count; i++) {
		zend_mir_source_phi_input_ref input;
		if (!source->phi_input_at(source->context, i, &input)
				|| input.phi_id >= phi_count
				|| input_offsets[input.phi_id + 1] == UINT32_MAX) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			goto done;
		}
		input_offsets[input.phi_id + 1]++;
	}
	for (i = 1; i <= phi_count; i++) {
		input_offsets[i] += input_offsets[i - 1];
	}
	memcpy(cursors, input_offsets, (size_t) phi_count * sizeof(*cursors));
	for (i = 0; i < input_count; i++) {
		zend_mir_source_phi_input_ref input;
		if (!source->phi_input_at(source->context, i, &input)) {
			validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
			goto done;
		}
		inputs[cursors[input.phi_id]++] = input;
	}
	for (i = 0; i < phi_count; i++) {
		const zend_mir_source_phi_ref *phi = &phis[i];
		const uint32_t predecessor_count =
			graph->predecessor_offsets[phi->block_id + 1]
				- graph->predecessor_offsets[phi->block_id];
		uint32_t j;
		uint32_t seen = 0;
		for (j = input_offsets[i]; j < input_offsets[i + 1]; j++) {
			const zend_mir_source_phi_input_ref *input = &inputs[j];
			zend_mir_source_block_id definition_block;
			bool is_live_in;
			bool predecessor_found;
			uint32_t producer_index;

			if ((phi->kind == ZEND_MIR_SOURCE_PHI_MERGE
					&& input->input_index != seen)
					|| !zend_mir_ssa_exists(
						source, input->source_ssa_variable_id)) {
				goto done;
			}
			if (phi->kind == ZEND_MIR_SOURCE_PHI_MERGE) {
				predecessor_found = input->input_index < predecessor_count
					&& graph->incoming_from[
						graph->predecessor_offsets[phi->block_id]
							+ input->input_index]
						== input->predecessor_block_id;
			} else {
				predecessor_found = zend_mir_graph_has_edge(graph,
					input->predecessor_block_id, phi->block_id);
			}
			if (!predecessor_found) {
				goto done;
			}
			producer_index = phi_by_ssa[input->source_ssa_variable_id];
			graph->edge_pi_sources[j] = ZEND_MIR_ID_INVALID;
			if (producer_index != ZEND_MIR_ID_INVALID
					&& phis[producer_index].kind != ZEND_MIR_SOURCE_PHI_MERGE
					&& phis[producer_index].block_id == phi->block_id
					&& input_offsets[producer_index + 1]
						- input_offsets[producer_index] == 1) {
				const zend_mir_source_phi_input_ref *producer_input =
					&inputs[input_offsets[producer_index]];

				if (producer_input->predecessor_block_id
							== input->predecessor_block_id
						&& producer_input->input_index == 0) {
					graph->edge_pi_sources[j] =
						producer_input->source_ssa_variable_id;
				}
			}
			if (producer_index != ZEND_MIR_ID_INVALID) {
				definition_block = phis[producer_index].block_id;
				is_live_in = false;
			} else {
				zend_mir_source_ssa_ref ssa;
				if (!source->ssa_at(source->context,
						input->source_ssa_variable_id, &ssa)) {
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
			if (!is_live_in
					&& !zend_mir_dominator_forest_dominates(dominators,
						definition_block, input->predecessor_block_id)
					&& graph->edge_pi_sources[j] == ZEND_MIR_ID_INVALID) {
				goto done;
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
	free(cursors);
	return valid;
}

static bool zend_mir_validate_source_impl(
	const zend_mir_lowering_source_view *source,
	const zend_mir_dominator_forest *dominators,
	zend_mir_source_graph *graph,
	zend_mir_validation *validation,
	bool allow_protected_control_flow)
{
	if (validation == NULL || graph == NULL) {
		return false;
	}
	memset(graph, 0, sizeof(*graph));
	memset(validation, 0, sizeof(*validation));
	validation->entry_block_id = ZEND_MIR_ID_INVALID;
	validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
	/*
	 * The frontend validated the Zend CFG, SSA and slots, and the source view
	 * derives operands, edges and their flags from them by construction. Only
	 * debug builds recheck those invariants here; release builds keep the
	 * block, terminator and phi checks that can defer real code.
	 */
	if (!zend_mir_source_contract_ok(source)
#if !defined(NDEBUG)
			|| !zend_mir_validate_linear_source(source, validation)
#endif
			) {
		return false;
	}
	if (!zend_mir_source_graph_init(graph, source, dominators)
			|| !zend_mir_source_graph_load_blocks(graph, source)) {
		validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
		return false;
	}
	if (!zend_mir_validate_blocks(
				source, graph, validation, allow_protected_control_flow)
#if !defined(NDEBUG)
			|| !zend_mir_validate_edges(source, graph, validation)
#endif
			) {
		return false;
	}
	if (!zend_mir_source_graph_load_edges(graph, source)) {
		validation->diagnostic = ZEND_MIRL_MALFORMED_CFG;
		return false;
	}
	if (!zend_mir_validate_terminators(source, graph, validation)
			|| !zend_mir_validate_phis(source, graph, validation)) {
		return false;
	}
	validation->proofs = ZEND_MIR_PROOF_SOURCE_CFG_COMPLETE
		| ZEND_MIR_PROOF_BRANCH_SUCCESSOR_ORDER
		| ZEND_MIR_PROOF_PHI_PREDECESSOR_ORDER
		| ZEND_MIR_PROOF_EDGE_STATEPOINTS;
	if (!graph->protected_region) {
		validation->proofs |= ZEND_MIR_PROOF_NO_PROTECTED_REGION;
	}
	if (!graph->irreducible) {
		validation->proofs |= ZEND_MIR_PROOF_REDUCIBLE_CFG;
	}
	validation->diagnostic = ZEND_MIRL_OK;
	return true;
}

bool zend_mir_validate_source(
	const zend_mir_lowering_source_view *source,
	const zend_mir_dominator_forest *dominators,
	zend_mir_source_graph *graph,
	zend_mir_validation *validation)
{
	return zend_mir_validate_source_impl(
		source, dominators, graph, validation, false);
}

bool zend_mir_validate_source_for_protected_control_flow(
	const zend_mir_lowering_source_view *source,
	const zend_mir_dominator_forest *dominators,
	zend_mir_source_graph *graph,
	zend_mir_validation *validation)
{
	return zend_mir_validate_source_impl(
		source, dominators, graph, validation, true);
}

zend_mir_branch_kind zend_mir_branch_kind_for_opcode(uint32_t opcode)
{
	switch (opcode) {
		case ZEND_MIR_OPCODE_JMP:
			return ZEND_MIR_BRANCH_UNCONDITIONAL;
		case ZEND_MIR_OPCODE_JMPZ:
			return ZEND_MIR_BRANCH_IF_FALSE;
		case ZEND_MIR_OPCODE_JMPNZ:
			return ZEND_MIR_BRANCH_IF_TRUE;
		case ZEND_MIR_OPCODE_JMPZ_EX:
			return ZEND_MIR_BRANCH_IF_FALSE_WITH_RESULT;
		case ZEND_MIR_OPCODE_JMPNZ_EX:
			return ZEND_MIR_BRANCH_IF_TRUE_WITH_RESULT;
		case ZEND_MIR_OPCODE_CATCH:
			return ZEND_MIR_BRANCH_CATCH;
		case ZEND_MIR_OPCODE_FAST_CALL:
			return ZEND_MIR_BRANCH_FINALLY_CALL;
		case ZEND_MIR_OPCODE_FAST_RET:
			return ZEND_MIR_BRANCH_FINALLY_RETURN;
		case ZEND_MIR_OPCODE_JMP_SET:
			return ZEND_MIR_BRANCH_JMP_SET;
		case ZEND_MIR_OPCODE_COALESCE:
			return ZEND_MIR_BRANCH_COALESCE;
		case ZEND_MIR_OPCODE_JMP_NULL:
			return ZEND_MIR_BRANCH_JMP_NULL;
		case ZEND_MIR_SOURCE_OPCODE_THROW:
			return ZEND_MIR_BRANCH_THROW;
		case ZEND_MIR_OPCODE_ASSERT_CHECK:
			return ZEND_MIR_BRANCH_ASSERT_CHECK;
		case ZEND_MIR_OPCODE_SWITCH_LONG:
		case ZEND_MIR_OPCODE_SWITCH_STRING:
		case ZEND_MIR_OPCODE_MATCH:
			return ZEND_MIR_BRANCH_MULTIWAY;
		case ZEND_MIR_OPCODE_BIND_INIT_STATIC_OR_JMP:
			return ZEND_MIR_BRANCH_BIND_STATIC;
		case ZEND_MIR_OPCODE_JMP_FRAMELESS:
			return ZEND_MIR_BRANCH_FRAMELESS;
		case ZEND_MIR_OPCODE_FE_RESET_R:
		case ZEND_MIR_OPCODE_FE_FETCH_R:
		case ZEND_MIR_OPCODE_FE_RESET_RW:
		case ZEND_MIR_OPCODE_FE_FETCH_RW:
			return ZEND_MIR_BRANCH_ITERATOR;
		default:
			return ZEND_MIR_BRANCH_KIND_INVALID;
	}
}

bool zend_mir_branch_edge_count_is_valid(
	zend_mir_branch_kind kind, uint32_t opcode, uint32_t edge_count)
{
	return !((kind == ZEND_MIR_BRANCH_UNCONDITIONAL && edge_count != 1)
		|| ((kind >= ZEND_MIR_BRANCH_IF_FALSE
				&& kind <= ZEND_MIR_BRANCH_IF_TRUE_WITH_RESULT)
			&& edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_CATCH
			&& edge_count != 1 && edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_FINALLY_CALL && edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_FINALLY_RETURN && edge_count != 0)
		|| (kind == ZEND_MIR_BRANCH_ITERATOR && edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_ASSERT_CHECK && edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_BIND_STATIC && edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_FRAMELESS && edge_count != 2)
		|| (kind == ZEND_MIR_BRANCH_MULTIWAY
			&& (opcode == ZEND_MIR_OPCODE_MATCH
				? edge_count < 2 : edge_count < 3))
		|| (kind == ZEND_MIR_BRANCH_THROW && edge_count != 0)
		|| (kind == ZEND_MIR_BRANCH_KIND_INVALID && edge_count > 1));
}
