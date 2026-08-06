#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../Verify/zend_mir_verify_control_flow.h"
#include "../../Lowering/zend_mir_control_flow.h"
#include "zend_mir_control_flow_internal.h"
#include "../zend_mir_id_index.h"

typedef struct _zend_mir_w04_verify_indexes {
	zend_mir_id_index_entry *blocks;
	zend_mir_id_index_entry *instructions;
	zend_mir_id_index_entry *frames;
	zend_mir_id_index_entry *map_sources;
	zend_mir_id_index_entry *map_blocks;
	zend_mir_id_index_entry *source_pi_results;
	zend_mir_source_phi_ref *source_phis;
	zend_mir_source_phi_input_ref *source_phi_inputs;
	zend_mir_source_edge_ref *source_edges;
	uint32_t *source_phi_input_offsets;
	uint32_t *source_phi_input_indices;
	uint32_t *source_predecessor_offsets;
	uint32_t *source_predecessor_indices;
	uint32_t *source_successor_counts;
	uint32_t block_capacity;
	uint32_t instruction_capacity;
	uint32_t frame_capacity;
	uint32_t map_capacity;
	uint32_t source_pi_capacity;
	bool map_sources_unique;
	bool map_blocks_unique;
} zend_mir_w04_verify_indexes;

static void zend_mir_w04_verify_indexes_destroy(
		zend_mir_w04_verify_indexes *indexes)
{
	free(indexes->source_predecessor_indices);
	free(indexes->source_predecessor_offsets);
	free(indexes->source_phi_input_indices);
	free(indexes->source_phi_input_offsets);
	free(indexes->source_edges);
	free(indexes->source_phi_inputs);
	free(indexes->source_phis);
	free(indexes->source_pi_results);
	free(indexes->map_blocks);
	free(indexes->map_sources);
	free(indexes->source_successor_counts);
	free(indexes->frames);
	free(indexes->instructions);
	free(indexes->blocks);
	memset(indexes, 0, sizeof(*indexes));
}

static bool zend_mir_w04_verify_indexes_init(
		zend_mir_w04_verify_indexes *indexes, const zend_mir_view *view,
		const zend_mir_lowering_source_view *source,
		const zend_mir_control_flow_map *map)
{
	uint32_t block_count = view->block_count(view->context);
	uint32_t instruction_count = view->instruction_count(view->context);
	uint32_t frame_count = view->frame_state_count(view->context);
	uint32_t map_count = map->block_count(map->context);
	uint32_t source_block_count = source->block_count(source->context);
	uint32_t source_edge_count = source->edge_count(source->context);
	uint32_t source_phi_count = source->phi_count(source->context);
	uint32_t source_phi_input_count = source->phi_input_count(source->context);
	uint32_t *cursors = NULL;
	uint32_t cursor_count = source_block_count > source_phi_count
		? source_block_count : source_phi_count;
	uint32_t i;

	memset(indexes, 0, sizeof(*indexes));
	if (!zend_mir_id_index_capacity(block_count, &indexes->block_capacity)
			|| !zend_mir_id_index_capacity(
				instruction_count, &indexes->instruction_capacity)
			|| !zend_mir_id_index_capacity(frame_count, &indexes->frame_capacity)
			|| !zend_mir_id_index_capacity(map_count, &indexes->map_capacity)) {
		return false;
	}
	if (!zend_mir_id_index_capacity(
			source_phi_count, &indexes->source_pi_capacity)) {
		return false;
	}
	indexes->blocks = calloc(indexes->block_capacity, sizeof(*indexes->blocks));
	indexes->instructions = calloc(
		indexes->instruction_capacity, sizeof(*indexes->instructions));
	indexes->frames = calloc(indexes->frame_capacity, sizeof(*indexes->frames));
	indexes->map_sources = calloc(
		indexes->map_capacity, sizeof(*indexes->map_sources));
	indexes->map_blocks = calloc(
		indexes->map_capacity, sizeof(*indexes->map_blocks));
	indexes->source_pi_results = calloc(indexes->source_pi_capacity,
		sizeof(*indexes->source_pi_results));
	indexes->source_phis = source_phi_count == 0 ? NULL
		: malloc((size_t) source_phi_count * sizeof(*indexes->source_phis));
	indexes->source_phi_inputs = source_phi_input_count == 0 ? NULL
		: malloc((size_t) source_phi_input_count
			* sizeof(*indexes->source_phi_inputs));
	indexes->source_edges = source_edge_count == 0 ? NULL
		: malloc((size_t) source_edge_count * sizeof(*indexes->source_edges));
	indexes->source_phi_input_offsets = calloc(
		(size_t) source_phi_count + 1,
		sizeof(*indexes->source_phi_input_offsets));
	indexes->source_phi_input_indices = source_phi_input_count == 0 ? NULL
		: malloc((size_t) source_phi_input_count
			* sizeof(*indexes->source_phi_input_indices));
	indexes->source_predecessor_offsets = calloc(
		(size_t) source_block_count + 1,
		sizeof(*indexes->source_predecessor_offsets));
	indexes->source_predecessor_indices = source_edge_count == 0 ? NULL
		: malloc((size_t) source_edge_count
			* sizeof(*indexes->source_predecessor_indices));
	indexes->source_successor_counts = calloc(
		source_block_count == 0 ? 1 : source_block_count,
		sizeof(*indexes->source_successor_counts));
	if (indexes->blocks == NULL || indexes->instructions == NULL
			|| indexes->frames == NULL || indexes->map_sources == NULL
			|| indexes->map_blocks == NULL
			|| indexes->source_pi_results == NULL
			|| (source_phi_count != 0 && indexes->source_phis == NULL)
			|| (source_phi_input_count != 0
				&& (indexes->source_phi_inputs == NULL
					|| indexes->source_phi_input_indices == NULL))
			|| (source_edge_count != 0
				&& (indexes->source_edges == NULL
					|| indexes->source_predecessor_indices == NULL))
			|| indexes->source_phi_input_offsets == NULL
			|| indexes->source_predecessor_offsets == NULL
			|| indexes->source_successor_counts == NULL) {
		goto failure;
	}
	if (cursor_count != 0) {
		cursors = malloc((size_t) cursor_count * sizeof(*cursors));
		if (cursors == NULL) {
			goto failure;
		}
	}
	for (i = 0; i < block_count; i++) {
		zend_mir_block_record block;
		if (!view->block_at(view->context, i, &block)
				|| !zend_mir_id_index_insert(indexes->blocks,
					indexes->block_capacity, block.id, i, NULL)) {
			goto failure;
		}
	}
	for (i = 0; i < instruction_count; i++) {
		zend_mir_instruction_record instruction;
		if (!view->instruction_at(view->context, i, &instruction)
				|| !zend_mir_id_index_insert(indexes->instructions,
					indexes->instruction_capacity, instruction.id, i, NULL)) {
			goto failure;
		}
	}
	for (i = 0; i < frame_count; i++) {
		zend_mir_frame_state_ref frame;
		if (!view->frame_state_at(view->context, i, &frame)
				|| !zend_mir_id_index_insert(indexes->frames,
					indexes->frame_capacity, frame.id, i, NULL)) {
			goto failure;
		}
	}
	for (i = 0; i < map_count; i++) {
		zend_mir_control_flow_block_mapping mapping;
		if (!map->block_at(map->context, i, &mapping)
				|| !zend_mir_id_index_insert(indexes->map_sources,
					indexes->map_capacity, mapping.source_block_id, i,
					&indexes->map_sources_unique)
				|| !zend_mir_id_index_insert(indexes->map_blocks,
					indexes->map_capacity, mapping.mir_block_id, i,
					&indexes->map_blocks_unique)) {
			goto failure;
		}
	}
	for (i = 0; i < source_phi_count; i++) {
		zend_mir_source_phi_ref phi;
		bool duplicate = false;

		if (!source->phi_at(source->context, i, &phi) || phi.id != i
				|| phi.block_id >= source_block_count) {
			goto failure;
		}
		indexes->source_phis[i] = phi;
		if (phi.kind != ZEND_MIR_SOURCE_PHI_MERGE
				&& (!zend_mir_id_index_insert(indexes->source_pi_results,
					indexes->source_pi_capacity,
					phi.result_ssa_variable_id, i, &duplicate)
					|| duplicate)) {
			goto failure;
		}
	}
	for (i = 0; i < source_phi_input_count; i++) {
		zend_mir_source_phi_input_ref input;

		if (!source->phi_input_at(source->context, i, &input)
				|| input.phi_id >= source_phi_count
				|| indexes->source_phi_input_offsets[input.phi_id + 1]
					== UINT32_MAX) {
			goto failure;
		}
		indexes->source_phi_inputs[i] = input;
		indexes->source_phi_input_offsets[input.phi_id + 1]++;
	}
	for (i = 0; i < source_phi_count; i++) {
		indexes->source_phi_input_offsets[i + 1] +=
			indexes->source_phi_input_offsets[i];
	}
	if (source_phi_count != 0) {
		memcpy(cursors, indexes->source_phi_input_offsets,
			(size_t) source_phi_count * sizeof(*cursors));
	}
	for (i = 0; i < source_phi_input_count; i++) {
		zend_mir_source_phi_input_ref *input = &indexes->source_phi_inputs[i];
		indexes->source_phi_input_indices[cursors[input->phi_id]++] = i;
	}
	for (i = 0; i < source_edge_count; i++) {
		zend_mir_source_edge_ref edge;

		if (!source->edge_at(source->context, i, &edge)
				|| edge.from_block_id >= source_block_count
				|| edge.to_block_id >= source_block_count
				|| indexes->source_successor_counts[edge.from_block_id]
					== UINT32_MAX
				|| indexes->source_predecessor_offsets[edge.to_block_id + 1]
					== UINT32_MAX) {
			goto failure;
		}
		indexes->source_edges[i] = edge;
		indexes->source_successor_counts[edge.from_block_id]++;
		indexes->source_predecessor_offsets[edge.to_block_id + 1]++;
	}
	for (i = 0; i < source_block_count; i++) {
		indexes->source_predecessor_offsets[i + 1] +=
			indexes->source_predecessor_offsets[i];
	}
	if (source_block_count != 0) {
		memcpy(cursors, indexes->source_predecessor_offsets,
			(size_t) source_block_count * sizeof(*cursors));
	}
	for (i = 0; i < source_edge_count; i++) {
		zend_mir_source_edge_ref *edge = &indexes->source_edges[i];
		indexes->source_predecessor_indices[cursors[edge->to_block_id]++] = i;
	}
	indexes->map_sources_unique = !indexes->map_sources_unique;
	indexes->map_blocks_unique = !indexes->map_blocks_unique;
	free(cursors);
	return true;

failure:
	free(cursors);
	zend_mir_w04_verify_indexes_destroy(indexes);
	return false;
}

static uint32_t zend_mir_w04_verify_cycle_block_count(const void *context)
{
	const zend_mir_lowering_source_view *source = context;

	return source->block_count(source->context);
}

static bool zend_mir_w04_verify_cycle_block_at(
	const void *context, uint32_t index, bool *irreducible)
{
	const zend_mir_lowering_source_view *source = context;
	zend_mir_source_block_ref block;

	if (irreducible == NULL
			|| !source->block_at(source->context, index, &block)
			|| block.id != index) {
		return false;
	}
	*irreducible =
		(block.flags & ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE) != 0;
	return true;
}

static uint32_t zend_mir_w04_verify_cycle_edge_count(const void *context)
{
	const zend_mir_lowering_source_view *source = context;

	return source->edge_count(source->context);
}

static bool zend_mir_w04_verify_cycle_edge_at(
	const void *context, uint32_t index,
	uint32_t *from_block_id, uint32_t *to_block_id,
	bool *requires_statepoint)
{
	const zend_mir_lowering_source_view *source = context;
	zend_mir_source_edge_ref edge;

	if (from_block_id == NULL || to_block_id == NULL
			|| requires_statepoint == NULL
			|| !source->edge_at(source->context, index, &edge)
			|| edge.id != index) {
		return false;
	}
	*from_block_id = edge.from_block_id;
	*to_block_id = edge.to_block_id;
	*requires_statepoint = zend_mir_w04_edge_requires_statepoint(&edge);
	return true;
}

static bool zend_mir_w04_verify_analyze_cycles(
	zend_mir_control_flow_cycle_analysis *analysis,
	const zend_mir_lowering_source_view *source)
{
	zend_mir_control_flow_cycle_graph graph;

	memset(&graph, 0, sizeof(graph));
	graph.context = source;
	graph.block_count = zend_mir_w04_verify_cycle_block_count;
	graph.block_at = zend_mir_w04_verify_cycle_block_at;
	graph.edge_count = zend_mir_w04_verify_cycle_edge_count;
	graph.edge_at = zend_mir_w04_verify_cycle_edge_at;
	return zend_mir_control_flow_cycle_analysis_init(analysis, &graph);
}

static zend_mir_w04_branch_kind zend_mir_w04_verify_branch_kind(uint32_t opcode)
{
	switch (opcode) {
		case 42:
			return ZEND_MIR_W04_BRANCH_UNCONDITIONAL;
		case 43:
			return ZEND_MIR_W04_BRANCH_IF_FALSE;
		case 44:
			return ZEND_MIR_W04_BRANCH_IF_TRUE;
		case 46:
			return ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT;
		case 47:
			return ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT;
		case 107:
			return ZEND_MIR_W04_BRANCH_CATCH;
		case 162:
			return ZEND_MIR_W08_BRANCH_FINALLY_CALL;
		case 163:
			return ZEND_MIR_W08_BRANCH_FINALLY_RETURN;
		case 152:
			return ZEND_MIR_W09_BRANCH_JMP_SET;
		case 169:
			return ZEND_MIR_W09_BRANCH_COALESCE;
		case 198:
			return ZEND_MIR_W10_BRANCH_JMP_NULL;
		case 151:
			return ZEND_MIR_W12_BRANCH_ASSERT_CHECK;
		case 187:
		case 188:
		case 195:
			return ZEND_MIR_W12_BRANCH_MULTIWAY;
		case 203:
			return ZEND_MIR_W12_BRANCH_BIND_STATIC;
		case 208:
			return ZEND_MIR_W12_BRANCH_FRAMELESS;
		case 77:
		case 78:
		case 125:
		case 126:
			return ZEND_MIR_W09_BRANCH_ITERATOR;
		default:
			return ZEND_MIR_W04_BRANCH_KIND_INVALID;
	}
}

static bool zend_mir_w04_emit_verify(
	zend_mir_diagnostic_sink *sink, zend_mir_verify_w04_code code,
	const char *token, const char *detail)
{
	zend_mir_diagnostic diagnostic;
	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = code == ZEND_MIR_VERIFY_W04_PHI_MISMATCH
		? ZEND_MIR_DIAGNOSTIC_INVALID_PHI : ZEND_MIR_DIAGNOSTIC_INVALID_CFG;
	diagnostic.severity = ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.function_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.block_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = ZEND_MIR_ID_INVALID;
	(void) snprintf(diagnostic.message, sizeof(diagnostic.message),
		"%s %s", token, detail);
	(void) zend_mir_diagnostic_sink_emit(sink, &diagnostic);
	return false;
}

static bool zend_mir_w04_view_has_block(
	const zend_mir_view *view, const zend_mir_w04_verify_indexes *indexes,
	zend_mir_block_id block_id)
{
	zend_mir_block_record block;
	int32_t index = zend_mir_id_index_find(
		indexes->blocks, indexes->block_capacity, block_id);
	return index >= 0
		&& view->block_at(view->context, (uint32_t) index, &block)
		&& block.id == block_id;
}

static bool zend_mir_w04_view_instruction(
	const zend_mir_view *view, const zend_mir_w04_verify_indexes *indexes,
	zend_mir_instruction_id id,
	zend_mir_instruction_record *out)
{
	int32_t index = zend_mir_id_index_find(
		indexes->instructions, indexes->instruction_capacity, id);
	return index >= 0
		&& view->instruction_at(view->context, (uint32_t) index, out)
		&& out->id == id;
}

static bool zend_mir_w04_view_frame_state(
	const zend_mir_view *view, const zend_mir_w04_verify_indexes *indexes,
	zend_mir_frame_state_id id,
	zend_mir_frame_state_ref *out)
{
	int32_t index = zend_mir_id_index_find(
		indexes->frames, indexes->frame_capacity, id);
	return index >= 0
		&& view->frame_state_at(view->context, (uint32_t) index, out)
		&& out->id == id;
}

static bool zend_mir_w04_view_has_source_map(
	const zend_mir_view *view, zend_mir_frame_state_id frame_id,
	zend_mir_source_position_id source_position_id, uint32_t opline_index)
{
	uint32_t matches = 0;
	uint32_t owner_maps = 0;
	uint32_t i;
	for (i = 0; i < view->source_map_count(view->context); i++) {
		zend_mir_source_map_ref source_map;
		if (!view->source_map_at(view->context, i, &source_map)) {
			return false;
		}
		if (source_map.owner_frame_id != frame_id) {
			continue;
		}
		owner_maps++;
		if (source_map.source_position_id == source_position_id
				&& source_map.opline_index == opline_index
				&& source_map.opline_phase == ZEND_MIR_OPLINE_PHASE_BEFORE) {
			matches++;
		}
	}
	return owner_maps == 1 && matches == 1;
}

static bool zend_mir_w04_verify_statepoint_frame(
	const zend_mir_view *view,
	const zend_mir_instruction_record *statepoint,
	const zend_mir_frame_state_ref *frame,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t frame_slot_count = view->frame_slot_count(view->context);
	uint32_t operand_count =
		view->instruction_operand_count(view->context, statepoint->id);
	uint32_t materialized_count = 0;
	zend_mir_frame_slot_kind previous_kind =
		ZEND_MIR_FRAME_SLOT_KIND_INVALID;
	uint32_t previous_index = 0;
	uint32_t i;

	if (frame->slots.offset > frame_slot_count
			|| frame->slots.count
				> frame_slot_count - frame->slots.offset) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
			ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
			"interrupt frame has an invalid slot span");
	}
	for (i = 0; i < frame->slots.count; i++) {
		zend_mir_frame_slot_ref slot;
		uint32_t j;
		if (!view->frame_slot_at(
				view->context, frame->slots.offset + i, &slot)
				|| slot.kind < ZEND_MIR_FRAME_SLOT_KIND_CV
				|| slot.kind > ZEND_MIR_FRAME_SLOT_KIND_VAR
				|| slot.representation
					!= ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL
				|| slot.ownership
					!= ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED
				|| slot.rooted || slot.cleanup_required
				|| (i != 0 && (slot.kind < previous_kind
					|| (slot.kind == previous_kind
						&& slot.index <= previous_index)))) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
				ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
				"interrupt frame slot layout is not canonical");
		}
		for (j = 0; j < i; j++) {
			zend_mir_frame_slot_ref earlier;
			if (!view->frame_slot_at(
					view->context, frame->slots.offset + j, &earlier)
					|| earlier.slot_id == slot.slot_id) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
					ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
					"interrupt frame contains a duplicate slot");
			}
		}
		if (slot.materialization == ZEND_MIR_MATERIALIZATION_MATERIALIZED) {
			zend_mir_value_id operand;
			if (!zend_mir_id_is_valid(slot.value_id)
					|| materialized_count >= operand_count
					|| !view->instruction_operand_at(view->context,
						statepoint->id, materialized_count, &operand)
					|| operand != slot.value_id) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
					ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
					"interrupt frame operand differs from its slot value");
			}
			materialized_count++;
		} else if (slot.materialization != ZEND_MIR_MATERIALIZATION_UNDEF
				|| zend_mir_id_is_valid(slot.value_id)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
				ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
				"interrupt frame has a noncanonical undefined slot");
		}
		previous_kind = slot.kind;
		previous_index = slot.index;
	}
	if (materialized_count != operand_count) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
			ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
			"interrupt statepoint operand cardinality differs from its frame");
	}
	return true;
}

static bool zend_mir_w04_map_block(
	const zend_mir_control_flow_map *map,
	const zend_mir_w04_verify_indexes *indexes,
	zend_mir_source_block_id source_id,
	zend_mir_block_id *mir_id)
{
	zend_mir_control_flow_block_mapping mapping;
	int32_t index = zend_mir_id_index_find(
		indexes->map_sources, indexes->map_capacity, source_id);
	if (index < 0 || mir_id == NULL
			|| !map->block_at(map->context, (uint32_t) index, &mapping)
			|| mapping.source_block_id != source_id) {
		return false;
	}
	*mir_id = mapping.mir_block_id;
	return true;
}

static bool zend_mir_w04_source_block(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out);

static bool zend_mir_w04_statepoint_source_opcode(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_block_ref *block,
	zend_mir_source_opcode_ref *opcode)
{
	uint32_t opcode_count;
	uint32_t opcode_index;

	if (source == NULL || block == NULL || opcode == NULL
			|| source->opcode_count == NULL || source->opcode_at == NULL) {
		return false;
	}
	opcode_count = source->opcode_count(source->context);
	opcode_index = block->opcode_count == 0
		? block->first_opcode_ordinal
		: block->first_opcode_ordinal + block->opcode_count - 1;
	return opcode_index < opcode_count
		&& source->opcode_at(source->context, opcode_index, opcode);
}

static bool zend_mir_w04_expected_successor(
	const zend_mir_lowering_source_view *source,
	const zend_mir_w04_verify_indexes *indexes,
	const zend_mir_source_edge_ref *edge, uint32_t *expected)
{
	zend_mir_source_block_ref block;
	zend_mir_source_opcode_ref opcode;
	if (!zend_mir_w04_source_block(source, edge->from_block_id, &block)
			|| indexes == NULL
			|| indexes->source_successor_counts == NULL) {
		return false;
	}
	if (block.opcode_count == 0) {
		*expected = edge->successor_index;
		return true;
	}
	if (!source->opcode_at(source->context,
			block.first_opcode_ordinal + block.opcode_count - 1, &opcode)) {
		return false;
	}
	{
		zend_mir_w04_branch_kind kind =
			zend_mir_w04_verify_branch_kind(opcode.zend_opcode_number);
		uint32_t successor_count =
			indexes->source_successor_counts[edge->from_block_id];
		if (kind == ZEND_MIR_W04_BRANCH_UNCONDITIONAL
				|| kind == ZEND_MIR_W12_BRANCH_MULTIWAY
				|| kind == ZEND_MIR_W04_BRANCH_KIND_INVALID) {
			*expected = edge->successor_index;
		} else if (successor_count == 1) {
			*expected = 0;
		} else {
			*expected = zend_mir_w04_mir_successor_for_source(
				kind, edge->successor_index);
		}
	}
	return *expected != UINT32_MAX;
}

static bool zend_mir_w04_verify_blocks(
	const zend_mir_view *view, const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map,
	const zend_mir_w04_verify_indexes *indexes,
	const zend_mir_control_flow_cycle_analysis *cycle_analysis,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t expected = 0;
	uint32_t edge_blocks = 0;
	uint32_t i;
	for (i = 0; i < map->block_count(map->context); i++) {
		zend_mir_control_flow_block_mapping mapping;
		if (!map->block_at(map->context, i, &mapping)
				|| !zend_mir_id_is_valid(mapping.source_block_id)
				|| !zend_mir_id_is_valid(mapping.mir_block_id)
				|| !zend_mir_w04_view_has_block(
					view, indexes, mapping.mir_block_id)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
				ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
				"block mapping contains an invalid record");
		}
	}
	if (!indexes->map_sources_unique || !indexes->map_blocks_unique) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
			ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
			"block mapping is not one-to-one");
	}
	for (i = 0; i < source->block_count(source->context); i++) {
		zend_mir_source_block_ref block;
		zend_mir_block_id mir;
		if (!source->block_at(source->context, i, &block)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
				ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
				"source block enumeration changed");
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		expected++;
		if (!zend_mir_w04_map_block(map, indexes, block.id, &mir)
				|| !zend_mir_w04_view_has_block(view, indexes, mir)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
				ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
				"reachable source block lacks one MIR block");
		}
	}
	if (map->block_count(map->context) != expected) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
			ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
			"block mapping cardinality differs from reachable source CFG");
	}
	for (i = 0; i < source->edge_count(source->context); i++) {
		zend_mir_source_edge_ref edge;
		if (!source->edge_at(source->context, i, &edge)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
				ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
				"source edge enumeration changed");
		}
		if (zend_mir_control_flow_cycle_edge_requires_statepoint(
				cycle_analysis, edge.id)) {
			edge_blocks++;
		}
	}
	if (expected > UINT32_MAX - edge_blocks
			|| view->block_count(view->context) != expected + edge_blocks) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
			ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
			"MIR block cardinality includes an unexpected block");
	}
	return true;
}

static uint32_t zend_mir_w04_statepoint_count_in_block(
	const zend_mir_view *view, zend_mir_block_id block_id, bool *valid)
{
	uint32_t count = 0;
	uint32_t i;
	*valid = false;
	for (i = 0; i < view->instruction_count(view->context); i++) {
		zend_mir_instruction_record instruction;
		if (!view->instruction_at(view->context, i, &instruction)) {
			return 0;
		}
		if (instruction.block_id == block_id
				&& instruction.opcode == ZEND_MIR_OPCODE_STATEPOINT) {
			count++;
		}
	}
	*valid = true;
	return count;
}

static bool zend_mir_w04_verify_edges(
	const zend_mir_view *view, const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map,
	const zend_mir_w04_verify_indexes *indexes,
	const zend_mir_control_flow_cycle_analysis *cycle_analysis,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t i;
	if (map->edge_count(map->context) != source->edge_count(source->context)) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_EDGE_MISMATCH,
			ZEND_MIRV_TOKEN_W04_EDGE_MISMATCH,
			"edge mapping cardinality differs from source CFG");
	}
	for (i = 0; i < source->edge_count(source->context); i++) {
		zend_mir_source_edge_ref edge;
		zend_mir_control_flow_edge_mapping mapping;
		zend_mir_block_id source_from;
		zend_mir_block_id source_to;
		zend_mir_block_id actual_to;
		zend_mir_instruction_record terminator;
		uint32_t expected_successor;
		if (!source->edge_at(source->context, i, &edge)
				|| !map->edge_at(map->context, i, &mapping)
				|| mapping.source_edge_id != edge.id
				|| !zend_mir_w04_map_block(
					map, indexes, edge.from_block_id, &source_from)
				|| !zend_mir_w04_map_block(
					map, indexes, edge.to_block_id, &source_to)
				|| mapping.mir_from_block_id != source_from
				|| !zend_mir_w04_expected_successor(
					source, indexes, &edge, &expected_successor)
				|| mapping.mir_successor_index != expected_successor
				|| mapping.mir_successor_index
					>= view->successor_count(view->context, source_from)
				|| !view->successor_at(view->context, source_from,
					mapping.mir_successor_index, &actual_to)
				|| actual_to != mapping.mir_to_block_id) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_EDGE_MISMATCH,
				ZEND_MIRV_TOKEN_W04_EDGE_MISMATCH,
				"mapped edge target or successor ordinal differs");
		}
		{
			zend_mir_source_block_ref source_block;
			zend_mir_source_opcode_ref source_opcode;
			zend_mir_w04_branch_kind branch_kind =
				ZEND_MIR_W04_BRANCH_KIND_INVALID;
			if (!zend_mir_w04_source_block(
					source, edge.from_block_id, &source_block)) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH,
					ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH,
					"branch source terminator is unavailable");
			}
			if (source_block.opcode_count != 0) {
				if (!source->opcode_at(source->context,
						source_block.first_opcode_ordinal
							+ source_block.opcode_count - 1,
						&source_opcode)) {
					return zend_mir_w04_emit_verify(diagnostics,
						ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH,
						ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH,
						"branch source terminator is unavailable");
				}
				branch_kind = zend_mir_w04_verify_branch_kind(
					source_opcode.zend_opcode_number);
			}
			if (!zend_mir_w04_view_instruction(
					view, indexes, mapping.terminator_instruction_id, &terminator)
					|| terminator.block_id != source_from
					|| (branch_kind == ZEND_MIR_W04_BRANCH_CATCH
						? terminator.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
						: branch_kind == ZEND_MIR_W08_BRANCH_FINALLY_CALL
							? terminator.opcode != ZEND_MIR_OPCODE_FINALLY_CALL
						: branch_kind == ZEND_MIR_W08_BRANCH_FINALLY_RETURN
							? terminator.opcode != ZEND_MIR_OPCODE_FINALLY_RETURN
						: branch_kind == ZEND_MIR_W09_BRANCH_ITERATOR
							? terminator.opcode != ZEND_MIR_OPCODE_ITERATOR_BRANCH
						: branch_kind == ZEND_MIR_W12_BRANCH_MULTIWAY
							? terminator.opcode
								!= ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
						: branch_kind == ZEND_MIR_W12_BRANCH_BIND_STATIC
							? terminator.opcode
								!= ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
						: branch_kind == ZEND_MIR_W12_BRANCH_FRAMELESS
							? terminator.opcode
								!= ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
						: (view->successor_count(view->context, source_from) == 1
							? terminator.opcode != ZEND_MIR_OPCODE_BRANCH
							: terminator.opcode != ZEND_MIR_OPCODE_COND_BRANCH
								&& terminator.opcode
									!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH))) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH,
					ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH,
					"terminator kind does not match source branch");
			}
			if (terminator.opcode != ZEND_MIR_OPCODE_VALUE_COND_BRANCH
					&& (branch_kind == ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT
					|| branch_kind
						== ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT)) {
				zend_mir_value_id condition;
				bool copy_found = false;
				uint32_t instruction_index;
				if (source_opcode.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA
						|| view->instruction_operand_count(
							view->context, terminator.id) != 1
						|| !view->instruction_operand_at(
							view->context, terminator.id, 0, &condition)) {
					return zend_mir_w04_emit_verify(diagnostics,
						ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH,
						ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH,
						"extended branch lacks its boolean condition");
				}
				for (instruction_index = 0;
					instruction_index
						< view->instruction_count(view->context);
					instruction_index++) {
					zend_mir_instruction_record candidate;
					zend_mir_value_id operand;
					if (!view->instruction_at(view->context,
							instruction_index, &candidate)) {
						return false;
					}
					if (candidate.block_id != source_from
							|| candidate.opcode != ZEND_MIR_OPCODE_COPY
							|| candidate.result_id
								!= zend_mir_value_from_original_ssa(
									source_opcode.result.ssa_variable_id)) {
						continue;
					}
					if (copy_found
						|| (candidate.representation
								!= ZEND_MIR_REPRESENTATION_I1
							&& candidate.representation
								!= ZEND_MIR_REPRESENTATION_ZVAL)
							|| view->instruction_operand_count(
								view->context, candidate.id) != 1
							|| !view->instruction_operand_at(
								view->context, candidate.id, 0, &operand)
							|| operand != condition) {
						return zend_mir_w04_emit_verify(diagnostics,
							ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH,
							ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH,
							"extended branch result is not its boolean condition");
					}
					copy_found = true;
				}
				if (!copy_found) {
					return zend_mir_w04_emit_verify(diagnostics,
						ZEND_MIR_VERIFY_W04_BRANCH_MISMATCH,
						ZEND_MIRV_TOKEN_W04_BRANCH_MISMATCH,
						"extended branch result copy is missing");
				}
			}
		}
		if (zend_mir_control_flow_cycle_edge_requires_statepoint(
				cycle_analysis, edge.id)) {
			zend_mir_instruction_record statepoint;
			zend_mir_frame_state_ref frame;
			zend_mir_source_block_ref source_block;
			zend_mir_source_opcode_ref source_opcode;
			bool statepoint_enumeration_valid;
			uint32_t statepoint_count =
				zend_mir_w04_statepoint_count_in_block(
					view, mapping.mir_to_block_id,
					&statepoint_enumeration_valid);
			if (!zend_mir_id_is_valid(
					mapping.edge_statepoint_instruction_id)
					|| !statepoint_enumeration_valid
					|| statepoint_count != 1
					|| !zend_mir_w04_view_instruction(view, indexes,
						mapping.edge_statepoint_instruction_id, &statepoint)
					|| statepoint.opcode != ZEND_MIR_OPCODE_STATEPOINT
					|| statepoint.block_id != mapping.mir_to_block_id
					|| !zend_mir_id_is_valid(statepoint.frame_state_id)
					|| !zend_mir_w04_view_frame_state(
						view, indexes, statepoint.frame_state_id, &frame)
					|| !frame.canonical
					|| frame.safepoint_class
						!= ZEND_MIR_SAFEPOINT_CLASS_OBSERVER
					|| (statepoint.effects
						& ZEND_MIR_EFFECT_MASK(
							ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY)) == 0
					|| (statepoint.reads
						& ZEND_MIR_MEMORY_DOMAIN_MASK(
							ZEND_MIR_MEMORY_DOMAIN_FRAME_CALL_CHAIN)) == 0
					|| (statepoint.reads
						& ZEND_MIR_MEMORY_DOMAIN_MASK(
							ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT)) == 0
					|| (statepoint.writes
						& ZEND_MIR_MEMORY_DOMAIN_MASK(
							ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT)) == 0
					|| (statepoint.barriers
						& ZEND_MIR_BARRIER_MASK(
							ZEND_MIR_BARRIER_SAFEPOINT)) == 0
					|| (statepoint.barriers
						& ZEND_MIR_BARRIER_MASK(
							ZEND_MIR_BARRIER_OBSERVER)) == 0
					|| (statepoint.barriers
						& ZEND_MIR_BARRIER_MASK(
							ZEND_MIR_BARRIER_INTERRUPT)) == 0
					|| view->successor_count(
						view->context, mapping.mir_to_block_id) != 1
					|| !view->successor_at(view->context,
						mapping.mir_to_block_id, 0, &actual_to)
					|| actual_to != source_to
					|| !zend_mir_w04_source_block(
						source, edge.from_block_id, &source_block)
					|| !zend_mir_w04_statepoint_source_opcode(
						source, &source_block, &source_opcode)
					|| statepoint.source_position_id
						!= source_opcode.source_position_id
					|| frame.opline_index != source_opcode.opline_index
					|| !zend_mir_w04_view_has_source_map(
						view, frame.id, statepoint.source_position_id,
						source_opcode.opline_index)) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
					ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
					"interrupt edge lacks its mapped statepoint");
			}
			if (!zend_mir_w04_verify_statepoint_frame(
					view, &statepoint, &frame, diagnostics)) {
				return false;
			}
		} else if (mapping.mir_to_block_id != source_to
				|| zend_mir_id_is_valid(
					mapping.edge_statepoint_instruction_id)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_EDGE_STATEPOINT_MISMATCH,
				ZEND_MIRV_TOKEN_W04_EDGE_STATEPOINT_MISMATCH,
				"non-interrupt edge has an edge statepoint");
		}
	}
	return true;
}

static bool zend_mir_w04_source_block(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out)
{
	return id < source->block_count(source->context)
		&& source->block_at(source->context, id, out)
		&& out->id == id;
}

static bool zend_mir_w04_source_dominates(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id dominator, zend_mir_source_block_id block)
{
	uint32_t remaining = source->block_count(source->context);
	while (remaining-- != 0) {
		zend_mir_source_block_ref record;
		if (block == dominator) {
			return true;
		}
		if (!zend_mir_w04_source_block(source, block, &record)
				|| record.immediate_dominator == ZEND_MIR_ID_INVALID
				|| record.immediate_dominator == block) {
			return false;
		}
		block = record.immediate_dominator;
	}
	return false;
}

static bool zend_mir_w04_verify_loops(
	const zend_mir_lowering_source_view *source,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t i;
	for (i = 0; i < source->edge_count(source->context); i++) {
		zend_mir_source_edge_ref edge;
		zend_mir_source_block_ref header;
		if (!source->edge_at(source->context, i, &edge)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_LOOP_MISMATCH,
				ZEND_MIRV_TOKEN_W04_LOOP_MISMATCH,
				"source edge enumeration changed");
		}
		if ((edge.flags & ZEND_MIR_SOURCE_EDGE_BACKEDGE) == 0) {
			continue;
		}
		if ((edge.flags & ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY) == 0
				|| !zend_mir_w04_source_block(
					source, edge.to_block_id, &header)
				|| (header.flags & ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER) == 0
				|| !zend_mir_w04_source_dominates(
					source, edge.to_block_id, edge.from_block_id)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_LOOP_MISMATCH,
				ZEND_MIRV_TOKEN_W04_LOOP_MISMATCH,
				"backedge is not a source-backed natural loop edge");
		}
	}
	return true;
}

static bool zend_mir_w04_resolve_indexed_edge_pi_input(
	const zend_mir_w04_verify_indexes *indexes,
	const zend_mir_source_phi_ref *consumer,
	const zend_mir_source_phi_input_ref *consumer_input,
	uint32_t *source_ssa_variable_id)
{
	int32_t producer_index;
	const zend_mir_source_phi_ref *producer;
	const zend_mir_source_phi_input_ref *producer_input;
	uint32_t begin;
	uint32_t end;

	if (consumer->kind != ZEND_MIR_SOURCE_PHI_MERGE) {
		return false;
	}
	producer_index = zend_mir_id_index_find(indexes->source_pi_results,
		indexes->source_pi_capacity, consumer_input->source_ssa_variable_id);
	if (producer_index < 0) {
		return false;
	}
	producer = &indexes->source_phis[producer_index];
	if (producer->block_id != consumer->block_id) {
		return false;
	}
	begin = indexes->source_phi_input_offsets[producer_index];
	end = indexes->source_phi_input_offsets[producer_index + 1];
	if (end - begin != 1) {
		return false;
	}
	producer_input = &indexes->source_phi_inputs[
		indexes->source_phi_input_indices[begin]];
	if (producer_input->predecessor_block_id
			!= consumer_input->predecessor_block_id
			|| producer_input->input_index != 0) {
		return false;
	}
	if (source_ssa_variable_id != NULL) {
		*source_ssa_variable_id = producer_input->source_ssa_variable_id;
	}
	return true;
}

static bool zend_mir_w04_verify_phi_order(
	const zend_mir_view *view, const zend_mir_control_flow_map *map,
	const zend_mir_w04_verify_indexes *indexes,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint8_t *saw_non_phi;
	uint32_t map_count = map->block_count(map->context);
	uint32_t instruction_count = view->instruction_count(view->context);
	uint32_t i;

	saw_non_phi = calloc(map_count == 0 ? 1 : map_count,
		sizeof(*saw_non_phi));
	if (saw_non_phi == NULL) {
		return false;
	}
	for (i = 0; i < map_count; i++) {
		zend_mir_control_flow_block_mapping mapping;

		if (!map->block_at(map->context, i, &mapping)
				|| zend_mir_id_index_find(indexes->map_blocks,
					indexes->map_capacity, mapping.mir_block_id) != (int32_t) i) {
			free(saw_non_phi);
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
				ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
				"block mapping changed during PHI verification");
		}
	}
	for (i = 0; i < instruction_count; i++) {
		zend_mir_instruction_record instruction;
		int32_t map_index;

		if (!view->instruction_at(view->context, i, &instruction)) {
			free(saw_non_phi);
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
				ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
				"instruction enumeration changed");
		}
		map_index = zend_mir_id_index_find(indexes->map_blocks,
			indexes->map_capacity, instruction.block_id);
		if (map_index < 0) {
			continue;
		}
		if (instruction.opcode == ZEND_MIR_OPCODE_PHI) {
			if (saw_non_phi[map_index]) {
				free(saw_non_phi);
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
					ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
					"regular PHI follows a non-PHI instruction");
			}
		} else {
			saw_non_phi[map_index] = 1;
		}
	}
	free(saw_non_phi);
	return true;
}

static bool zend_mir_w04_verify_phis(
	const zend_mir_view *view, const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map,
	const zend_mir_w04_verify_indexes *indexes,
	zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t i;
	if (map->phi_count(map->context) != source->phi_count(source->context)) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
			ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
			"PHI mapping cardinality differs");
	}
	for (i = 0; i < source->phi_count(source->context); i++) {
		zend_mir_source_phi_ref phi;
		zend_mir_control_flow_phi_mapping mapping;
		zend_mir_instruction_record instruction;
		zend_mir_block_id expected_block;
		uint32_t expected_operands = 0;
		uint32_t j;
		if (!source->phi_at(source->context, i, &phi)
				|| !map->phi_at(map->context, i, &mapping)
				|| mapping.source_phi_id != phi.id
				|| !zend_mir_w04_view_instruction(
					view, indexes, mapping.mir_phi_instruction_id, &instruction)
				|| !zend_mir_w04_map_block(
					map, indexes, phi.block_id, &expected_block)
				|| instruction.block_id != expected_block
				|| instruction.result_id != mapping.mir_result_value_id
				|| instruction.result_id
					!= zend_mir_value_from_original_ssa(
						phi.result_ssa_variable_id)
				|| instruction.opcode != (phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
					? ZEND_MIR_OPCODE_PHI : ZEND_MIR_OPCODE_COPY)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
				ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
				"PHI/Pi instruction mapping differs");
		}
		for (j = indexes->source_phi_input_offsets[i];
				j < indexes->source_phi_input_offsets[i + 1]; j++) {
			const zend_mir_source_phi_input_ref *input =
				&indexes->source_phi_inputs[
					indexes->source_phi_input_indices[j]];
			zend_mir_value_id operand;
			uint32_t source_ssa_variable_id;
			bool predecessor_found = false;
			uint32_t k;
			uint32_t predecessor_begin =
				indexes->source_predecessor_offsets[phi.block_id];
			uint32_t predecessor_end =
				indexes->source_predecessor_offsets[phi.block_id + 1];

			source_ssa_variable_id = input->source_ssa_variable_id;
			(void) zend_mir_w04_resolve_indexed_edge_pi_input(
				indexes, &phi, input, &source_ssa_variable_id);
			for (k = predecessor_begin; k < predecessor_end; k++) {
				const zend_mir_source_edge_ref *edge = &indexes->source_edges[
					indexes->source_predecessor_indices[k]];
				if (edge->to_block_id == phi.block_id
						&& (phi.kind != ZEND_MIR_SOURCE_PHI_MERGE
							|| edge->predecessor_index == input->input_index)
						&& edge->from_block_id
							== input->predecessor_block_id) {
					predecessor_found = true;
					break;
				}
			}
			if (!predecessor_found
					|| (phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
						&& input->input_index != expected_operands)
					|| !view->instruction_operand_at(view->context,
						instruction.id, expected_operands, &operand)
					|| operand != zend_mir_value_from_original_ssa(
						source_ssa_variable_id)) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
					ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
					"PHI/Pi operand does not match its source predecessor");
			}
			expected_operands++;
		}
		if (view->instruction_operand_count(view->context, instruction.id)
				!= expected_operands) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
				ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
				"PHI operand cardinality differs");
		}
	}
	return zend_mir_w04_verify_phi_order(view, map, indexes, diagnostics);
}

bool zend_mir_verify_w04_control_flow(
	const zend_mir_view *view,
	const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_control_flow_cycle_analysis cycle_analysis;
	zend_mir_w04_verify_indexes indexes;
	bool verified;

	memset(&cycle_analysis, 0, sizeof(cycle_analysis));
	memset(&indexes, 0, sizeof(indexes));
	if (view == NULL || source == NULL || map == NULL
			|| view->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| source->contract_version != ZEND_MIR_W04_CONTRACT_VERSION
			|| map->contract_version != ZEND_MIR_W04_CONTRACT_VERSION
			|| view->block_count == NULL || view->block_at == NULL
			|| view->instruction_count == NULL || view->instruction_at == NULL
			|| view->instruction_operand_count == NULL
			|| view->instruction_operand_at == NULL
			|| view->frame_state_count == NULL
			|| view->frame_state_at == NULL
			|| view->frame_slot_count == NULL
			|| view->frame_slot_at == NULL
			|| view->source_map_count == NULL
			|| view->source_map_at == NULL
			|| view->successor_count == NULL || view->successor_at == NULL
			|| source->block_count == NULL || source->block_at == NULL
			|| source->edge_count == NULL || source->edge_at == NULL
			|| source->phi_count == NULL || source->phi_at == NULL
			|| source->phi_input_count == NULL || source->phi_input_at == NULL
			|| map->block_count == NULL || map->block_at == NULL
			|| map->edge_count == NULL || map->edge_at == NULL
			|| map->phi_count == NULL || map->phi_at == NULL) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
			ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
			"control-flow verifier contract is incomplete");
	}
	if (!zend_mir_w04_verify_indexes_init(&indexes, view, source, map)) {
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
			ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
			"control-flow verifier index construction failed");
	}
	if (!zend_mir_w04_verify_analyze_cycles(&cycle_analysis, source)) {
		zend_mir_w04_verify_indexes_destroy(&indexes);
		return zend_mir_w04_emit_verify(diagnostics,
			ZEND_MIR_VERIFY_W04_LOOP_MISMATCH,
			ZEND_MIRV_TOKEN_W04_LOOP_MISMATCH,
			"source cycle analysis failed");
	}
	verified = zend_mir_w04_verify_blocks(
			view, source, map, &indexes, &cycle_analysis, diagnostics)
		&& zend_mir_w04_verify_edges(
			view, source, map, &indexes, &cycle_analysis, diagnostics)
		&& zend_mir_w04_verify_phis(
			view, source, map, &indexes, diagnostics)
		&& zend_mir_w04_verify_loops(source, diagnostics);
	zend_mir_control_flow_cycle_analysis_destroy(&cycle_analysis);
	zend_mir_w04_verify_indexes_destroy(&indexes);
	return verified;
}
