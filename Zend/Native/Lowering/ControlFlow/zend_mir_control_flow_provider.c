#include <stdlib.h>
#include <string.h>

#include "../Frontend/zend_mir_zend_source.h"
#include "../../MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "../../MIR/Semantics/zend_mir_effect_summary.h"
#include "zend_mir_control_flow_internal.h"

static bool zend_mir_w04_add_instruction(
	zend_mir_mutator *mutator, zend_mir_block_id block_id,
	zend_mir_opcode opcode, zend_mir_representation representation,
	zend_mir_value_id result_id, zend_mir_source_position_id source_position_id,
	zend_mir_instruction_id *out)
{
	zend_mir_instruction_record record;
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = block_id;
	record.opcode = opcode;
	record.representation = representation;
	record.result_id = result_id;
	record.frame_state_id = ZEND_MIR_ID_INVALID;
	record.source_position_id = source_position_id;
	return mutator != NULL && mutator->add_instruction != NULL
		&& mutator->add_instruction(mutator->context, &record, out);
}

static bool zend_mir_w04_operand_value_fact(
	const zend_mir_lowering_context *context,
	const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out,
	zend_mir_value_fact_ref *fact_out, zend_mir_representation *out)
{
	zend_mir_value_id value_id;
	if (context == NULL || operand == NULL || value_id_out == NULL
			|| fact_out == NULL || out == NULL) {
		return false;
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_SSA
			&& operand->ssa_variable_id <= ZEND_MIR_VALUE_ORIGINAL_MAX) {
		value_id =
			zend_mir_value_from_original_ssa(operand->ssa_variable_id);
	} else if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& operand->index
				< context->source->literal_count(context->source->context)
			&& operand->index <= ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX) {
		value_id = zend_mir_value_from_synthetic(operand->index);
	} else {
		return false;
	}
	if (!zend_mir_lowering_context_value_fact(context, value_id, fact_out)) {
		return false;
	}
	*out = zend_mir_scalar_type_representation(fact_out->exact_type);
	*value_id_out = value_id;
	return zend_mir_id_is_valid(value_id)
		&& *out != ZEND_MIR_REPRESENTATION_INVALID;
}

static bool zend_mir_w04_value_fact(
	const zend_mir_lowering_context *context, uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *fact_out, zend_mir_representation *out)
{
	zend_mir_source_operand_ref operand;
	zend_mir_value_id value_id;
	memset(&operand, 0, sizeof(operand));
	operand.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	operand.ssa_variable_id = ssa_variable_id;
	return zend_mir_w04_operand_value_fact(
		context, &operand, &value_id, fact_out, out);
}

static bool zend_mir_w04_value_representation(
	const zend_mir_lowering_context *context, uint32_t ssa_variable_id,
	zend_mir_representation *out)
{
	zend_mir_value_fact_ref fact;
	return zend_mir_w04_value_fact(
		context, ssa_variable_id, &fact, out);
}

static bool zend_mir_w04_condition_value(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode, zend_mir_value_id *out)
{
	zend_mir_value_fact_ref input_fact;
	zend_mir_value_fact_ref result_fact;
	zend_mir_representation input_representation;
	zend_mir_value_fact_id fact_id;
	zend_mir_instruction_id instruction_id;
	zend_mir_value_id input;
	zend_mir_value_id result;
	zend_mir_opcode conversion;
	uint32_t literal_count;
	uint32_t opcode_count;
	uint32_t payload;
	if (context == NULL || mutator == NULL || opcode == NULL || out == NULL
			|| !zend_mir_w04_operand_value_fact(context, &opcode->op1,
				&input, &input_fact, &input_representation)) {
		return false;
	}
	if (input_fact.exact_type == ZEND_MIR_SCALAR_TYPE_I1) {
		*out = input;
		return true;
	}
	literal_count = context->source->literal_count(context->source->context);
	opcode_count = context->source->opcode_count(context->source->context);
	if (opcode->opline_index >= opcode_count
			|| opcode_count > (UINT32_MAX - literal_count) / 2
			|| literal_count + opcode_count * 2 > UINT32_MAX - opcode->opline_index) {
		return false;
	}
	payload = literal_count + opcode_count * 2 + opcode->opline_index;
	result = zend_mir_value_from_synthetic(payload);
	if (!zend_mir_id_is_valid(result) || mutator->add_value == NULL
			|| mutator->add_value_fact == NULL
			|| !mutator->add_value(mutator->context, result,
				ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_OWNERSHIP_STATE_OWNED)) {
		return false;
	}
	memset(&result_fact, 0, sizeof(result_fact));
	result_fact.id = ZEND_MIR_ID_INVALID;
	result_fact.value_id = result;
	result_fact.exact_type = ZEND_MIR_SCALAR_TYPE_I1;
	result_fact.flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	result_fact.provenance = ZEND_MIR_FACT_PROVENANCE_CONTRACT;
	result_fact.provenance_source_position_id = opcode->source_position_id;
	if (!mutator->add_value_fact(
			mutator->context, &result_fact, &fact_id)) {
		return false;
	}
	if (input_fact.exact_type == ZEND_MIR_SCALAR_TYPE_NULL) {
		zend_mir_constant_record constant;
		if (mutator->add_constant == NULL) {
			return false;
		}
		memset(&constant, 0, sizeof(constant));
		constant.value_id = result;
		constant.representation = ZEND_MIR_REPRESENTATION_I1;
		constant.kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
		constant.symbol_id = ZEND_MIR_ID_INVALID;
		if (!mutator->add_constant(mutator->context, &constant)
				|| !zend_mir_w04_add_instruction(mutator,
					zend_mir_lowering_context_block_id(context),
					ZEND_MIR_OPCODE_CONSTANT, ZEND_MIR_REPRESENTATION_I1,
					result, opcode->source_position_id, &instruction_id)) {
			return false;
		}
		*out = result;
		return true;
	}
	conversion = input_fact.exact_type == ZEND_MIR_SCALAR_TYPE_I64
		? ZEND_MIR_OPCODE_I64_TO_I1
		: input_fact.exact_type == ZEND_MIR_SCALAR_TYPE_F64
			? ZEND_MIR_OPCODE_F64_TO_I1 : ZEND_MIR_OPCODE_INVALID;
	if (conversion == ZEND_MIR_OPCODE_INVALID
			|| !zend_mir_w04_add_instruction(mutator,
				zend_mir_lowering_context_block_id(context), conversion,
				ZEND_MIR_REPRESENTATION_I1, result,
				opcode->source_position_id, &instruction_id)
			|| mutator->add_operand == NULL
			|| !mutator->add_operand(
				mutator->context, instruction_id, input)) {
		return false;
	}
	*out = result;
	return true;
}

static bool zend_mir_w04_source_edge_count(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id block_id, uint32_t *count)
{
	uint32_t i;
	uint32_t found = 0;
	if (source == NULL || count == NULL || source->edge_count == NULL
			|| source->edge_at == NULL) {
		return false;
	}
	for (i = 0; i < source->edge_count(source->context); i++) {
		zend_mir_source_edge_ref edge;
		if (!source->edge_at(source->context, i, &edge)) {
			return false;
		}
		if (edge.from_block_id == block_id) {
			if (found == UINT32_MAX) {
				return false;
			}
			found++;
		}
	}
	*count = found;
	return true;
}

static bool zend_mir_w04_collect_source_edges(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id block_id,
	zend_mir_source_edge_ref *edges, uint32_t count)
{
	uint32_t i;
	uint32_t found = 0;
	if (source == NULL || (count != 0 && edges == NULL)) {
		return false;
	}
	for (i = 0; i < count; i++) {
		edges[i].id = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < source->edge_count(source->context); i++) {
		zend_mir_source_edge_ref edge;
		if (!source->edge_at(source->context, i, &edge)) {
			return false;
		}
		if (edge.from_block_id != block_id) {
			continue;
		}
		if (edge.successor_index >= count
				|| edges[edge.successor_index].id != ZEND_MIR_ID_INVALID) {
			return false;
		}
		edges[edge.successor_index] = edge;
		found++;
	}
	return found == count;
}

static bool zend_mir_w04_source_block(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id id, zend_mir_source_block_ref *out)
{
	if (id >= source->block_count(source->context)) {
		return false;
	}
	return source->block_at(source->context, id, out) && out->id == id;
}

static bool zend_mir_w04_collect_current_slot_values(
	const zend_mir_lowering_context *context,
	zend_mir_source_block_id at_block,
	zend_mir_value_id *values_out, uint32_t slot_count)
{
	const zend_mir_lowering_source_view *source;
	zend_mir_source_block_id entry_block = ZEND_MIR_ID_INVALID;
	zend_mir_source_block_id *phi_blocks = NULL;
	zend_mir_source_block_id *selected_blocks = NULL;
	uint32_t *dominance_depth = NULL;
	uint32_t *phi_ranks = NULL;
	uint32_t *slot_offsets = NULL;
	uint64_t *selected_ranks = NULL;
	uint32_t block_count;
	uint32_t phi_count;
	uint32_t ssa_count;
	uint32_t current;
	uint32_t depth;
	uint32_t i;
	bool success = false;

	if (context == NULL || context->source == NULL
			|| (slot_count != 0 && values_out == NULL)) {
		return false;
	}
	source = context->source;
	if (source->block_count == NULL || source->block_at == NULL
			|| source->opcode_count == NULL || source->opcode_at == NULL
			|| source->ssa_count == NULL || source->ssa_at == NULL
			|| source->phi_count == NULL || source->phi_at == NULL) {
		return false;
	}
	block_count = source->block_count(source->context);
	phi_count = source->phi_count(source->context);
	ssa_count = source->ssa_count(source->context);
	if (block_count == 0 || at_block >= block_count) {
		return false;
	}
	for (i = 0; i < slot_count; i++) {
		values_out[i] = ZEND_MIR_ID_INVALID;
	}
	dominance_depth = calloc(block_count, sizeof(*dominance_depth));
	slot_offsets = malloc(3 * sizeof(*slot_offsets));
	if (dominance_depth == NULL || slot_offsets == NULL) {
		goto done;
	}
	for (i = 0; i < 3; i++) {
		slot_offsets[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < slot_count; i++) {
		zend_mir_source_slot_ref slot;
		uint32_t kind;
		if (!zend_mir_zend_source_slot_at(context->zend_source, i, &slot)
				|| slot.slot_id != i || slot.kind < ZEND_MIR_SOURCE_SLOT_CV
				|| slot.kind > ZEND_MIR_SOURCE_SLOT_VAR) {
			goto done;
		}
		kind = (uint32_t) slot.kind;
		if (slot_offsets[kind] == ZEND_MIR_ID_INVALID) {
			if (slot.kind_index > i) {
				goto done;
			}
			slot_offsets[kind] = i - slot.kind_index;
		}
		if (slot.kind_index > UINT32_MAX - slot_offsets[kind]
				|| slot_offsets[kind] + slot.kind_index != i) {
			goto done;
		}
	}
	current = at_block;
	depth = block_count;
	for (i = 0; i < block_count; i++) {
		zend_mir_source_block_ref block;
		if (current >= block_count || dominance_depth[current] != 0
				|| !zend_mir_w04_source_block(source, current, &block)) {
			goto done;
		}
		dominance_depth[current] = depth--;
		if (block.immediate_dominator == ZEND_MIR_ID_INVALID
				|| block.immediate_dominator == current) {
			break;
		}
		current = block.immediate_dominator;
	}
	if (i == block_count) {
		goto done;
	}
	for (i = 0; i < block_count; i++) {
		zend_mir_source_block_ref block;
		if (!zend_mir_w04_source_block(source, i, &block)) {
			goto done;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_ENTRY) != 0) {
			if (entry_block != ZEND_MIR_ID_INVALID) {
				goto done;
			}
			entry_block = block.id;
		}
	}
	if (entry_block == ZEND_MIR_ID_INVALID) {
		goto done;
	}
	if (ssa_count != 0) {
		phi_blocks = malloc(ssa_count * sizeof(*phi_blocks));
		phi_ranks = calloc(ssa_count, sizeof(*phi_ranks));
	}
	if (slot_count != 0) {
		selected_blocks = malloc(slot_count * sizeof(*selected_blocks));
		selected_ranks = calloc(slot_count, sizeof(*selected_ranks));
	}
	if ((ssa_count != 0 && (phi_blocks == NULL || phi_ranks == NULL))
			|| (slot_count != 0
				&& (selected_blocks == NULL || selected_ranks == NULL))) {
		goto done;
	}
	for (i = 0; i < ssa_count; i++) {
		phi_blocks[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < slot_count; i++) {
		selected_blocks[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < phi_count; i++) {
		zend_mir_source_phi_ref phi;
		if (!source->phi_at(source->context, i, &phi)
				|| phi.result_ssa_variable_id >= ssa_count
				|| phi.block_id >= block_count
				|| phi_blocks[phi.result_ssa_variable_id]
					!= ZEND_MIR_ID_INVALID) {
			goto done;
		}
		phi_blocks[phi.result_ssa_variable_id] = phi.block_id;
		phi_ranks[phi.result_ssa_variable_id] = i + 1;
	}
	for (i = 0; i < ssa_count; i++) {
		zend_mir_source_ssa_ref ssa;
		zend_mir_value_fact_ref fact;
		zend_mir_representation representation;
		zend_mir_source_block_id definition_block;
		uint64_t definition_rank;
		uint32_t kind;
		uint32_t slot_id;
		bool later;
		if (!source->ssa_at(source->context, i, &ssa)) {
			goto done;
		}
		if (ssa.ssa_variable_id >= ssa_count
				|| ssa.source_slot_kind < ZEND_MIR_SOURCE_SLOT_CV
				|| ssa.source_slot_kind > ZEND_MIR_SOURCE_SLOT_VAR) {
			goto done;
		}
		if (!zend_mir_w04_value_fact(
				context, ssa.ssa_variable_id, &fact, &representation)) {
			continue;
		}
		if (phi_blocks[ssa.ssa_variable_id] != ZEND_MIR_ID_INVALID) {
			definition_block = phi_blocks[ssa.ssa_variable_id];
			definition_rank = phi_ranks[ssa.ssa_variable_id];
		} else if (ssa.definition_opline_index == ZEND_MIR_ID_INVALID) {
			definition_block = entry_block;
			definition_rank = 0;
		} else {
			zend_mir_source_opcode_ref opcode;
			if (ssa.definition_opline_index
						>= source->opcode_count(source->context)
					|| !source->opcode_at(source->context,
						ssa.definition_opline_index, &opcode)
					|| opcode.block_id >= block_count) {
				goto done;
			}
			definition_block = opcode.block_id;
			definition_rank =
				(uint64_t) phi_count + ssa.definition_opline_index + 1;
		}
		if (dominance_depth[definition_block] == 0) {
			continue;
		}
		kind = (uint32_t) ssa.source_slot_kind;
		if (slot_offsets[kind] == ZEND_MIR_ID_INVALID
				|| ssa.source_slot > UINT32_MAX - slot_offsets[kind]) {
			goto done;
		}
		slot_id = slot_offsets[kind] + ssa.source_slot;
		if (slot_id >= slot_count) {
			goto done;
		}
		later = selected_blocks[slot_id] == ZEND_MIR_ID_INVALID
			|| (definition_block == selected_blocks[slot_id]
				&& definition_rank > selected_ranks[slot_id])
			|| (definition_block != selected_blocks[slot_id]
				&& dominance_depth[definition_block]
					> dominance_depth[selected_blocks[slot_id]]);
		if (later) {
			selected_blocks[slot_id] = definition_block;
			selected_ranks[slot_id] = definition_rank;
			values_out[slot_id] =
				zend_mir_value_from_original_ssa(ssa.ssa_variable_id);
		}
	}
	success = true;

done:
	free(selected_ranks);
	free(slot_offsets);
	free(phi_ranks);
	free(dominance_depth);
	free(selected_blocks);
	free(phi_blocks);
	return success;
}

static zend_mir_frame_slot_kind zend_mir_w04_frame_slot_kind(
	zend_mir_source_slot_kind kind)
{
	switch (kind) {
		case ZEND_MIR_SOURCE_SLOT_CV:
			return ZEND_MIR_FRAME_SLOT_KIND_CV;
		case ZEND_MIR_SOURCE_SLOT_TMP:
			return ZEND_MIR_FRAME_SLOT_KIND_TMP;
		case ZEND_MIR_SOURCE_SLOT_VAR:
			return ZEND_MIR_FRAME_SLOT_KIND_VAR;
		default:
			return ZEND_MIR_FRAME_SLOT_KIND_INVALID;
	}
}

static bool zend_mir_w10_add_effect(
	zend_mir_effect_summary *summary, zend_mir_effect effect)
{
	zend_mir_effect_summary atomic;
	zend_mir_effect_summary composed;

	if (!zend_mir_effect_summary_from_effect(effect, &atomic)
			|| !zend_mir_effect_summary_compose(
				&composed, summary, &atomic)) {
		return false;
	}
	*summary = composed;
	return true;
}

static bool zend_mir_w10_throw_semantics(zend_mir_effect_summary *summary)
{
	const zend_mir_memory_domain_mask frame_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_LOCALS)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_TEMPS);
	const zend_mir_memory_domain_mask heap_domains =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_ZVAL)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_OBJECT)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_HEAP_REFERENCE)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_GC_METADATA)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_EXCEPTION);

	zend_mir_effect_summary_empty(summary);
	if (!zend_mir_w10_add_effect(summary, ZEND_MIR_EFFECT_READ_MEMORY)
			|| !zend_mir_w10_add_effect(summary, ZEND_MIR_EFFECT_WRITE_MEMORY)
			|| !zend_mir_w10_add_effect(summary, ZEND_MIR_EFFECT_ALLOCATE)
			|| !zend_mir_w10_add_effect(summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
			|| !zend_mir_w10_add_effect(summary, ZEND_MIR_EFFECT_THROW)) {
		return false;
	}
	summary->reads |= frame_domains | heap_domains;
	summary->writes |= frame_domains | heap_domains;
	return zend_mir_effect_summary_init(summary, summary->effects,
		summary->reads, summary->writes, summary->barriers, 0, 0);
}

static bool zend_mir_w10_emit_throw_frame(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	zend_mir_source_block_id source_block_id,
	zend_mir_frame_state_id *frame_id_out)
{
	const zend_mir_zend_source *zend_source =
		context != NULL ? context->zend_source : NULL;
	zend_mir_frame_state_ref frame;
	zend_mir_source_map_ref source_map;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_map_id source_map_id;
	zend_mir_value_id *slot_values = NULL;
	uint32_t first_slot = 0;
	uint32_t materialized_slot_count = 0;
	/*
	 * W11P gives every SSA value a canonical Zend-frame storage location.
	 * The physical frame is therefore already the authoritative state at a
	 * throw boundary; repeating the whole frame in every MIR frame record is
	 * redundant and makes large functions quadratic in frame size.
	 */
	uint32_t slot_count = zend_source != NULL && zend_source->w11
		? 0 : zend_mir_zend_source_slot_count(zend_source);
	uint32_t i;
	bool success = false;

	if (zend_source == NULL || opcode == NULL || frame_id_out == NULL
			|| mutator == NULL
			|| mutator->add_frame_slot == NULL
			|| mutator->add_frame_state == NULL
			|| mutator->add_source_map == NULL) {
		return false;
	}
	if (slot_count != 0) {
		slot_values = malloc(slot_count * sizeof(*slot_values));
		if (slot_values == NULL
				|| !zend_mir_w04_collect_current_slot_values(
					context, source_block_id, slot_values, slot_count)) {
			goto done;
		}
	}
	for (i = 0; i < slot_count; i++) {
		zend_mir_source_slot_ref source_slot;
		zend_mir_frame_slot_ref frame_slot;
		zend_mir_value_id value_id = slot_values[i];
		uint32_t slot_index;

		if (!zend_mir_id_is_valid(value_id)) {
			continue;
		}
		if (!zend_mir_zend_source_slot_at(zend_source, i, &source_slot)) {
			goto done;
		}
		memset(&frame_slot, 0, sizeof(frame_slot));
		frame_slot.slot_id = source_slot.slot_id;
		frame_slot.index = source_slot.kind_index;
		frame_slot.kind = zend_mir_w04_frame_slot_kind(source_slot.kind);
		frame_slot.representation =
			ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
		frame_slot.materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
		frame_slot.ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		frame_slot.value_id = value_id;
		if (frame_slot.kind == ZEND_MIR_FRAME_SLOT_KIND_INVALID
				|| !mutator->add_frame_slot(
					mutator->context, &frame_slot, &slot_index)
				|| (materialized_slot_count != 0
					&& slot_index
						!= first_slot + materialized_slot_count)) {
			goto done;
		}
		if (materialized_slot_count == 0) {
			first_slot = slot_index;
		}
		materialized_slot_count++;
	}
	memset(&frame, 0, sizeof(frame));
	frame.id = ZEND_MIR_ID_INVALID;
	frame.function_id = zend_mir_lowering_context_function_id(context);
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame.opline_index = opcode->opline_index;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame.slots.offset = materialized_slot_count == 0 ? 0 : first_slot;
	frame.slots.count = materialized_slot_count;
	frame.return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame.return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.return_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.exception_continuation = frame.return_continuation;
	frame.bailout_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame.bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame.suspend_state_id = ZEND_MIR_ID_INVALID;
	frame.code_version_id = 0;
	frame.resume.allowed = false;
	frame.resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame.resume.resume_id = ZEND_MIR_ID_INVALID;
	frame.resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame.resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR;
	frame.canonical = true;
	if (!mutator->add_frame_state(mutator->context, &frame, &frame_id)) {
		goto done;
	}
	memset(&source_map, 0, sizeof(source_map));
	source_map.id = ZEND_MIR_ID_INVALID;
	source_map.source_position_id = opcode->source_position_id;
	source_map.op_array_id = zend_source->op_array_id;
	source_map.opline_index = opcode->opline_index;
	source_map.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	source_map.owner_frame_id = frame_id;
	if (!mutator->add_source_map(
			mutator->context, &source_map, &source_map_id)) {
		goto done;
	}
	*frame_id_out = frame_id;
	success = true;

done:
	free(slot_values);
	return success;
}

static bool zend_mir_w04_emit_edge_statepoint(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	zend_mir_source_block_id source_block_id,
	zend_mir_block_id edge_block,
	zend_mir_instruction_id *statepoint_id)
{
	const zend_mir_zend_source *zend_source =
		context != NULL ? context->zend_source : NULL;
	zend_mir_frame_state_ref frame;
	zend_mir_source_map_ref source_map;
	zend_mir_instruction_record record;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_map_id source_map_id;
	zend_mir_instruction_id branch_id;
	zend_mir_value_id *slot_values = NULL;
	uint32_t first_slot = 0;
	uint32_t materialized_slot_count = 0;
	/*
	 * Canonical W11P locations keep interrupt-visible values in the Zend
	 * frame. Empty statepoint snapshots avoid duplicating those same physical
	 * locations in every loop edge while retaining the source/frame identity.
	 */
	uint32_t slot_count = zend_source != NULL && zend_source->w11
		? 0 : zend_mir_zend_source_slot_count(zend_source);
	uint32_t i;
	bool success = false;
	if (zend_source == NULL || opcode == NULL
			|| mutator == NULL || statepoint_id == NULL
			|| mutator->add_frame_slot == NULL
			|| mutator->add_frame_state == NULL
			|| mutator->add_source_map == NULL) {
		return false;
	}
	if (slot_count != 0) {
		slot_values = malloc(slot_count * sizeof(*slot_values));
		if (slot_values == NULL
				|| !zend_mir_w04_collect_current_slot_values(
					context, source_block_id, slot_values, slot_count)) {
			goto done;
		}
	}
	for (i = 0; i < slot_count; i++) {
		zend_mir_source_slot_ref source_slot;
		zend_mir_frame_slot_ref frame_slot;
		zend_mir_value_id value_id = slot_values[i];
		uint32_t slot_index;
		if (!zend_mir_id_is_valid(value_id)) {
			continue;
		}
		if (!zend_mir_zend_source_slot_at(zend_source, i, &source_slot)) {
			goto done;
		}
		memset(&frame_slot, 0, sizeof(frame_slot));
		frame_slot.slot_id = source_slot.slot_id;
		frame_slot.index = source_slot.kind_index;
		frame_slot.kind = zend_mir_w04_frame_slot_kind(source_slot.kind);
		frame_slot.representation =
			ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
		frame_slot.materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
		frame_slot.ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		frame_slot.value_id = value_id;
		if (frame_slot.kind == ZEND_MIR_FRAME_SLOT_KIND_INVALID
				|| !mutator->add_frame_slot(
					mutator->context, &frame_slot, &slot_index)
				|| (materialized_slot_count != 0
					&& slot_index
						!= first_slot + materialized_slot_count)) {
			goto done;
		}
		if (materialized_slot_count == 0) {
			first_slot = slot_index;
		}
		materialized_slot_count++;
	}
	memset(&frame, 0, sizeof(frame));
	frame.id = ZEND_MIR_ID_INVALID;
	frame.function_id = zend_mir_lowering_context_function_id(context);
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame.opline_index = opcode == NULL ? 0 : opcode->opline_index;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame.slots.offset = materialized_slot_count == 0 ? 0 : first_slot;
	frame.slots.count = materialized_slot_count;
	frame.return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame.return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.return_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.exception_continuation = frame.return_continuation;
	frame.bailout_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame.bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame.suspend_state_id = ZEND_MIR_ID_INVALID;
	frame.code_version_id = 0;
	frame.resume.allowed = false;
	frame.resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame.resume.resume_id = ZEND_MIR_ID_INVALID;
	frame.resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame.resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_OBSERVER;
	frame.canonical = true;
	if (!mutator->add_frame_state(mutator->context, &frame, &frame_id)) {
		goto done;
	}
	memset(&source_map, 0, sizeof(source_map));
	source_map.id = ZEND_MIR_ID_INVALID;
	source_map.source_position_id = opcode->source_position_id;
	source_map.op_array_id = zend_source->op_array_id;
	source_map.opline_index = opcode->opline_index;
	source_map.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	source_map.owner_frame_id = frame_id;
	if (!mutator->add_source_map(
			mutator->context, &source_map, &source_map_id)) {
		goto done;
	}
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = edge_block;
	record.opcode = ZEND_MIR_OPCODE_STATEPOINT;
	record.representation = ZEND_MIR_REPRESENTATION_VOID;
	record.result_id = ZEND_MIR_ID_INVALID;
	record.frame_state_id = frame_id;
	record.source_position_id =
		opcode == NULL ? ZEND_MIR_ID_INVALID : opcode->source_position_id;
	record.effects = ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY);
	record.reads =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_CALL_CHAIN)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(
			ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT);
	record.writes =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT);
	record.barriers = ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_SAFEPOINT)
		| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER)
		| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_INTERRUPT);
	if (!mutator->add_instruction(mutator->context, &record, statepoint_id)) {
		goto done;
	}
	for (i = 0; i < slot_count; i++) {
		if (zend_mir_id_is_valid(slot_values[i])
					&& (mutator->add_operand == NULL
						|| !mutator->add_operand(
							mutator->context, *statepoint_id,
							slot_values[i]))) {
			goto done;
		}
	}
	if (!zend_mir_w04_add_instruction(mutator, edge_block,
				ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
				ZEND_MIR_ID_INVALID, record.source_position_id, &branch_id)) {
		goto done;
	}
	success = true;

done:
	free(slot_values);
	return success;
}

static bool zend_mir_w04_emit_terminator_edges(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	zend_mir_control_flow_map_storage *map,
	zend_mir_w04_branch_kind kind,
	zend_mir_instruction_id terminator,
	uint32_t edge_count)
{
	zend_mir_source_edge_ref *edges = NULL;
	zend_mir_control_flow_edge_mapping *mappings = NULL;
	uint32_t i;
	bool success = false;

	if (edge_count != 0) {
		edges = calloc(edge_count, sizeof(*edges));
		mappings = calloc(edge_count, sizeof(*mappings));
		if (edges == NULL || mappings == NULL) {
			goto done;
		}
	}
	if (!zend_mir_w04_collect_source_edges(
			context->source, block->id, edges, edge_count)) {
		goto done;
	}
	for (i = 0; i < edge_count; i++) {
		uint32_t mir_index = i;
		zend_mir_block_id target;
		if (edge_count == 2) {
			mir_index = zend_mir_w04_mir_successor_for_source(kind, i);
			if (mir_index > 1) {
				goto done;
			}
		}
		if (!zend_mir_control_flow_map_find_block(
				&map->public_map, edges[i].to_block_id, &target)) {
			goto done;
		}
		memset(&mappings[i], 0, sizeof(mappings[i]));
		mappings[i].source_edge_id = edges[i].id;
		mappings[i].mir_from_block_id =
			zend_mir_lowering_context_block_id(context);
		mappings[i].mir_to_block_id = target;
		mappings[i].terminator_instruction_id = terminator;
		mappings[i].edge_statepoint_instruction_id = ZEND_MIR_ID_INVALID;
		mappings[i].mir_successor_index = mir_index;
		if (zend_mir_w04_edge_requires_statepoint(&edges[i])) {
			zend_mir_source_opcode_ref implicit_edge_opcode;
			const zend_mir_source_opcode_ref *statepoint_opcode = opcode;
			zend_mir_block_id edge_block;

			if (statepoint_opcode == NULL) {
				if (block->opcode_count == 0
						|| !context->source->opcode_at(
							context->source->context,
							block->first_opcode_ordinal
								+ block->opcode_count - 1,
							&implicit_edge_opcode)) {
					goto done;
				}
				statepoint_opcode = &implicit_edge_opcode;
			}
			if (!mutator->add_block(mutator->context,
					zend_mir_lowering_context_function_id(context),
					&edge_block)
					|| !zend_mir_w04_emit_edge_statepoint(
						context, mutator, statepoint_opcode, block->id,
						edge_block,
						&mappings[i].edge_statepoint_instruction_id)
					|| !mutator->add_edge(
						mutator->context, edge_block, target)) {
				goto done;
			}
			mappings[i].mir_to_block_id = edge_block;
		}
	}
	for (i = 0; i < edge_count; i++) {
		uint32_t source_index = i;
		if (edge_count == 2) {
			source_index =
				mappings[0].mir_successor_index == i ? 0 : 1;
		}
		if (!mutator->add_edge(mutator->context,
				mappings[source_index].mir_from_block_id,
				mappings[source_index].mir_to_block_id)) {
			goto done;
		}
	}
	for (i = 0; i < edge_count; i++) {
		if (!zend_mir_control_flow_map_add_edge(map, &mappings[i])) {
			goto done;
		}
	}
	success = true;

done:
	free(mappings);
	free(edges);
	return success;
}

bool zend_mir_w04_emit_terminator(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	zend_mir_control_flow_map_storage *map)
{
	zend_mir_instruction_id terminator;
	zend_mir_value_id condition = ZEND_MIR_ID_INVALID;
	zend_mir_w04_branch_kind kind = ZEND_MIR_W04_BRANCH_KIND_INVALID;
	bool source_condition = false;
	bool machine_condition = false;
	uint32_t edge_count = 0;
	if (context == NULL || mutator == NULL || block == NULL || map == NULL
			|| !zend_mir_w04_source_edge_count(
				context->source, block->id, &edge_count)) {
		return false;
	}
	if (opcode != NULL) {
		kind = zend_mir_w04_branch_kind_for_opcode(opcode->zend_opcode_number);
		if (zend_mir_id_is_valid(opcode->op1.ssa_variable_id)) {
			zend_mir_representation representation;

			machine_condition = zend_mir_w04_value_representation(
				context, opcode->op1.ssa_variable_id, &representation)
				&& representation != ZEND_MIR_REPRESENTATION_ZVAL
				&& representation != ZEND_MIR_REPRESENTATION_VOID
				&& representation != ZEND_MIR_REPRESENTATION_CONTROL;
		}
	}
	source_condition = context->zend_source != NULL
		&& context->zend_source->w09 && edge_count == 2
		&& (!machine_condition
			|| kind == ZEND_MIR_W12_BRANCH_BIND_STATIC
			|| kind == ZEND_MIR_W12_BRANCH_FRAMELESS)
		&& kind != ZEND_MIR_W04_BRANCH_CATCH
		&& kind != ZEND_MIR_W08_BRANCH_FINALLY_CALL
		&& kind != ZEND_MIR_W09_BRANCH_ITERATOR
		&& kind != ZEND_MIR_W12_BRANCH_MULTIWAY;
	if ((kind == ZEND_MIR_W04_BRANCH_UNCONDITIONAL && edge_count != 1)
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
				&& (opcode == NULL
					|| (opcode->zend_opcode_number
							== ZEND_MIR_W12_OPCODE_MATCH
						? edge_count < 2 : edge_count < 3)))
			|| (kind == ZEND_MIR_W10_BRANCH_THROW && edge_count != 0)
			|| (kind == ZEND_MIR_W04_BRANCH_KIND_INVALID && edge_count > 1)) {
		return false;
	}
	if (kind == ZEND_MIR_W10_BRANCH_THROW) {
		zend_mir_effect_summary summary;
		zend_mir_frame_state_id frame_id;
		zend_mir_instruction_record record;

		if (opcode == NULL || block->opcode_count == 0
				|| !zend_mir_w10_throw_semantics(&summary)
				|| !zend_mir_w10_emit_throw_frame(
					context, mutator, opcode, block->id, &frame_id)) {
			return false;
		}
		memset(&record, 0, sizeof(record));
		record.id = ZEND_MIR_ID_INVALID;
		record.block_id = zend_mir_lowering_context_block_id(context);
		record.opcode = ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL;
		record.representation = ZEND_MIR_REPRESENTATION_VOID;
		record.result_id = ZEND_MIR_ID_INVALID;
		record.frame_state_id = frame_id;
		record.source_position_id = opcode->source_position_id;
		record.effects = summary.effects;
		record.reads = summary.reads;
		record.writes = summary.writes;
		record.barriers = summary.barriers;
		return mutator->add_instruction != NULL
			&& mutator->add_instruction(
				mutator->context, &record, &terminator);
	}
	if (edge_count == 0 && kind != ZEND_MIR_W08_BRANCH_FINALLY_RETURN) {
		if (opcode != NULL || block->opcode_count != 0) {
			return true;
		}
		return zend_mir_w04_add_instruction(mutator,
			zend_mir_lowering_context_block_id(context),
			ZEND_MIR_OPCODE_UNREACHABLE, ZEND_MIR_REPRESENTATION_CONTROL,
			ZEND_MIR_ID_INVALID, ZEND_MIR_ID_INVALID, &terminator);
	}
	if (edge_count == 2 && kind != ZEND_MIR_W04_BRANCH_CATCH
			&& kind != ZEND_MIR_W08_BRANCH_FINALLY_CALL
			&& kind != ZEND_MIR_W09_BRANCH_ITERATOR
			&& kind != ZEND_MIR_W12_BRANCH_MULTIWAY && !source_condition
			&& !zend_mir_w04_condition_value(
				context, mutator, opcode, &condition)) {
		return false;
	}
	if (!source_condition
			&& (kind == ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT
			|| kind == ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT)) {
		zend_mir_instruction_id copy_id;
		zend_mir_representation representation;
		if (opcode == NULL
				|| opcode->op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA
				|| opcode->result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
				|| !zend_mir_w04_value_representation(context,
					opcode->result.ssa_variable_id, &representation)
				|| representation != ZEND_MIR_REPRESENTATION_I1
				|| !zend_mir_id_is_valid(condition)
				|| !zend_mir_w04_add_instruction(mutator,
					zend_mir_lowering_context_block_id(context),
					ZEND_MIR_OPCODE_COPY, representation,
					zend_mir_value_from_original_ssa(
						opcode->result.ssa_variable_id),
					opcode->source_position_id, &copy_id)
				|| !mutator->add_operand(mutator->context, copy_id,
					condition)) {
			return false;
		}
	}
	if (!zend_mir_w04_add_instruction(mutator,
			zend_mir_lowering_context_block_id(context),
			kind == ZEND_MIR_W04_BRANCH_CATCH
				? ZEND_MIR_OPCODE_CATCH_ENTER
				: kind == ZEND_MIR_W08_BRANCH_FINALLY_CALL
					? ZEND_MIR_OPCODE_FINALLY_CALL
				: kind == ZEND_MIR_W08_BRANCH_FINALLY_RETURN
					? ZEND_MIR_OPCODE_FINALLY_RETURN
				: kind == ZEND_MIR_W09_BRANCH_ITERATOR
					? ZEND_MIR_OPCODE_ITERATOR_BRANCH
				: kind == ZEND_MIR_W12_BRANCH_MULTIWAY
					? ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
				: kind == ZEND_MIR_W12_BRANCH_BIND_STATIC
					? ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				: kind == ZEND_MIR_W12_BRANCH_FRAMELESS
					? ZEND_MIR_OPCODE_VALUE_FRAMELESS_BRANCH
				: source_condition
					? ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				: edge_count == 1 ? ZEND_MIR_OPCODE_BRANCH
				: ZEND_MIR_OPCODE_COND_BRANCH,
			ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID,
			opcode == NULL ? ZEND_MIR_ID_INVALID : opcode->source_position_id,
			&terminator)) {
		return false;
	}
	if (edge_count == 2 && kind != ZEND_MIR_W04_BRANCH_CATCH
			&& kind != ZEND_MIR_W08_BRANCH_FINALLY_CALL
			&& kind != ZEND_MIR_W09_BRANCH_ITERATOR
			&& kind != ZEND_MIR_W12_BRANCH_MULTIWAY && !source_condition) {
		if (!zend_mir_id_is_valid(condition)
				|| !mutator->add_operand(
					mutator->context, terminator, condition)) {
			return false;
		}
	}
	return zend_mir_w04_emit_terminator_edges(
		context, mutator, opcode, block, map, kind, terminator, edge_count);
}
