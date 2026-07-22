#include <stdio.h>
#include <string.h>

#include "../Verify/zend_mir_verify_control_flow.h"
#include "../../Lowering/zend_mir_control_flow.h"

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
	const zend_mir_view *view, zend_mir_block_id block_id)
{
	uint32_t i;
	for (i = 0; i < view->block_count(view->context); i++) {
		zend_mir_block_record block;
		if (!view->block_at(view->context, i, &block)) {
			return false;
		}
		if (block.id == block_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w04_view_instruction(
	const zend_mir_view *view, zend_mir_instruction_id id,
	zend_mir_instruction_record *out)
{
	uint32_t i;
	for (i = 0; i < view->instruction_count(view->context); i++) {
		zend_mir_instruction_record instruction;
		if (!view->instruction_at(view->context, i, &instruction)) {
			return false;
		}
		if (instruction.id == id) {
			*out = instruction;
			return true;
		}
	}
	return false;
}

static bool zend_mir_w04_view_frame_state(
	const zend_mir_view *view, zend_mir_frame_state_id id,
	zend_mir_frame_state_ref *out)
{
	uint32_t i;
	for (i = 0; i < view->frame_state_count(view->context); i++) {
		zend_mir_frame_state_ref frame;
		if (!view->frame_state_at(view->context, i, &frame)) {
			return false;
		}
		if (frame.id == id) {
			*out = frame;
			return true;
		}
	}
	return false;
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
	const zend_mir_control_flow_map *map, zend_mir_source_block_id source_id,
	zend_mir_block_id *mir_id)
{
	uint32_t i;
	for (i = 0; i < map->block_count(map->context); i++) {
		zend_mir_control_flow_block_mapping mapping;
		if (!map->block_at(map->context, i, &mapping)) {
			return false;
		}
		if (mapping.source_block_id == source_id) {
			*mir_id = mapping.mir_block_id;
			return true;
		}
	}
	return false;
}

static bool zend_mir_w04_source_block(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out);

static bool zend_mir_w04_expected_successor(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_edge_ref *edge, uint32_t *expected)
{
	zend_mir_source_block_ref block;
	zend_mir_source_opcode_ref opcode;
	uint32_t i;
	for (i = 0; i < source->block_count(source->context); i++) {
		if (!source->block_at(source->context, i, &block)) {
			return false;
		}
		if (block.id == edge->from_block_id) {
			break;
		}
	}
	if (i == source->block_count(source->context)) {
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
		uint32_t successor_count = 0;
		uint32_t edge_index;
		for (edge_index = 0;
			edge_index < source->edge_count(source->context); edge_index++) {
			zend_mir_source_edge_ref candidate;
			if (!source->edge_at(source->context, edge_index, &candidate)) {
				return false;
			}
			if (candidate.from_block_id == block.id) {
				successor_count++;
			}
		}
		if (kind == ZEND_MIR_W04_BRANCH_UNCONDITIONAL
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
	const zend_mir_control_flow_map *map, zend_mir_diagnostic_sink *diagnostics)
{
	uint32_t expected = 0;
	uint32_t edge_blocks = 0;
	uint32_t i;
	for (i = 0; i < map->block_count(map->context); i++) {
		zend_mir_control_flow_block_mapping mapping;
		uint32_t j;
		if (!map->block_at(map->context, i, &mapping)
				|| !zend_mir_id_is_valid(mapping.source_block_id)
				|| !zend_mir_id_is_valid(mapping.mir_block_id)
				|| !zend_mir_w04_view_has_block(view, mapping.mir_block_id)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
				ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
				"block mapping contains an invalid record");
		}
		for (j = 0; j < i; j++) {
			zend_mir_control_flow_block_mapping earlier;
			if (!map->block_at(map->context, j, &earlier)
					|| earlier.source_block_id == mapping.source_block_id
					|| earlier.mir_block_id == mapping.mir_block_id) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_BLOCK_MISMATCH,
					ZEND_MIRV_TOKEN_W04_BLOCK_MISMATCH,
					"block mapping is not one-to-one");
			}
		}
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
		if (!zend_mir_w04_map_block(map, block.id, &mir)
				|| !zend_mir_w04_view_has_block(view, mir)) {
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
		if (zend_mir_w04_edge_requires_statepoint(&edge)) {
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
	const zend_mir_control_flow_map *map, zend_mir_diagnostic_sink *diagnostics)
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
				|| !zend_mir_w04_map_block(map, edge.from_block_id, &source_from)
				|| !zend_mir_w04_map_block(map, edge.to_block_id, &source_to)
				|| mapping.mir_from_block_id != source_from
				|| !zend_mir_w04_expected_successor(
					source, &edge, &expected_successor)
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
					view, mapping.terminator_instruction_id, &terminator)
					|| terminator.block_id != source_from
					|| (branch_kind == ZEND_MIR_W04_BRANCH_CATCH
						? terminator.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
						: branch_kind == ZEND_MIR_W08_BRANCH_FINALLY_CALL
							? terminator.opcode != ZEND_MIR_OPCODE_FINALLY_CALL
						: branch_kind == ZEND_MIR_W08_BRANCH_FINALLY_RETURN
							? terminator.opcode != ZEND_MIR_OPCODE_FINALLY_RETURN
						: branch_kind == ZEND_MIR_W09_BRANCH_ITERATOR
							? terminator.opcode != ZEND_MIR_OPCODE_ITERATOR_BRANCH
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
							|| candidate.representation
								!= ZEND_MIR_REPRESENTATION_I1
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
		if (zend_mir_w04_edge_requires_statepoint(&edge)) {
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
					|| !zend_mir_w04_view_instruction(view,
						mapping.edge_statepoint_instruction_id, &statepoint)
					|| statepoint.opcode != ZEND_MIR_OPCODE_STATEPOINT
					|| statepoint.block_id != mapping.mir_to_block_id
					|| !zend_mir_id_is_valid(statepoint.frame_state_id)
					|| !zend_mir_w04_view_frame_state(
						view, statepoint.frame_state_id, &frame)
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
					|| source_block.opcode_count == 0
					|| !source->opcode_at(source->context,
						source_block.first_opcode_ordinal
							+ source_block.opcode_count - 1,
						&source_opcode)
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
	uint32_t i;
	for (i = 0; i < source->block_count(source->context); i++) {
		zend_mir_source_block_ref block;
		if (!source->block_at(source->context, i, &block)) {
			return false;
		}
		if (block.id == id) {
			*out = block;
			return true;
		}
	}
	return false;
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

static bool zend_mir_w04_verify_phis(
	const zend_mir_view *view, const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map, zend_mir_diagnostic_sink *diagnostics)
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
					view, mapping.mir_phi_instruction_id, &instruction)
				|| !zend_mir_w04_map_block(
					map, phi.block_id, &expected_block)
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
		for (j = 0; j < source->phi_input_count(source->context); j++) {
			zend_mir_source_phi_input_ref input;
			zend_mir_value_id operand;
			bool predecessor_found = false;
			uint32_t k;
			if (!source->phi_input_at(source->context, j, &input)) {
				return false;
			}
			if (input.phi_id != phi.id) {
				continue;
			}
			for (k = 0; k < source->edge_count(source->context); k++) {
				zend_mir_source_edge_ref edge;
				if (!source->edge_at(source->context, k, &edge)) {
					return false;
				}
				if (edge.to_block_id == phi.block_id
						&& edge.predecessor_index == input.input_index
						&& edge.from_block_id
							== input.predecessor_block_id) {
					predecessor_found = true;
					break;
				}
			}
			if (!predecessor_found
					|| (phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
						&& input.input_index != expected_operands)
					|| !view->instruction_operand_at(view->context,
						instruction.id, expected_operands, &operand)
					|| operand != zend_mir_value_from_original_ssa(
						input.source_ssa_variable_id)) {
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
	for (i = 0; i < map->block_count(map->context); i++) {
		zend_mir_control_flow_block_mapping block_mapping;
		bool saw_non_phi = false;
		uint32_t j;
		if (!map->block_at(map->context, i, &block_mapping)) {
			return zend_mir_w04_emit_verify(diagnostics,
				ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
				ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
				"block mapping changed during PHI verification");
		}
		for (j = 0; j < view->instruction_count(view->context); j++) {
			zend_mir_instruction_record instruction;
			if (!view->instruction_at(view->context, j, &instruction)) {
				return zend_mir_w04_emit_verify(diagnostics,
					ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
					ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
					"instruction enumeration changed");
			}
			if (instruction.block_id != block_mapping.mir_block_id) {
				continue;
			}
			if (instruction.opcode == ZEND_MIR_OPCODE_PHI) {
				if (saw_non_phi) {
					return zend_mir_w04_emit_verify(diagnostics,
						ZEND_MIR_VERIFY_W04_PHI_MISMATCH,
						ZEND_MIRV_TOKEN_W04_PHI_MISMATCH,
						"regular PHI follows a non-PHI instruction");
				}
			} else {
				saw_non_phi = true;
			}
		}
	}
	return true;
}

bool zend_mir_verify_w04_control_flow(
	const zend_mir_view *view,
	const zend_mir_lowering_source_view *source,
	const zend_mir_control_flow_map *map,
	zend_mir_diagnostic_sink *diagnostics)
{
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
	return zend_mir_w04_verify_blocks(view, source, map, diagnostics)
		&& zend_mir_w04_verify_edges(view, source, map, diagnostics)
		&& zend_mir_w04_verify_phis(view, source, map, diagnostics)
		&& zend_mir_w04_verify_loops(source, diagnostics);
}
