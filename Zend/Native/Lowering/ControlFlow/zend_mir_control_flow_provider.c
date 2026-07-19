#include <string.h>

#include "../Frontend/zend_mir_zend_source.h"
#include "../../MIR/Scalar/zend_mir_scalar_descriptors.h"
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

static bool zend_mir_w04_source_edges(
	const zend_mir_lowering_source_view *source,
	zend_mir_source_block_id block_id,
	zend_mir_source_edge_ref *edges, uint32_t *count)
{
	uint32_t i;
	uint32_t found = 0;
	edges[0].id = ZEND_MIR_ID_INVALID;
	edges[1].id = ZEND_MIR_ID_INVALID;
	for (i = 0; i < source->edge_count(source->context); i++) {
		zend_mir_source_edge_ref edge;
		if (!source->edge_at(source->context, i, &edge)) {
			return false;
		}
		if (edge.from_block_id == block_id) {
			if (edge.successor_index >= 2
					|| edges[edge.successor_index].id
						!= ZEND_MIR_ID_INVALID) {
				return false;
			}
			edges[edge.successor_index] = edge;
			found++;
		}
	}
	*count = found;
	return true;
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

static bool zend_mir_w04_ssa_definition(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_ssa_ref *ssa,
	zend_mir_source_block_id *block_out, uint32_t *rank_out)
{
	uint32_t i;
	for (i = 0; i < source->phi_count(source->context); i++) {
		zend_mir_source_phi_ref phi;
		if (!source->phi_at(source->context, i, &phi)) {
			return false;
		}
		if (phi.result_ssa_variable_id == ssa->ssa_variable_id) {
			*block_out = phi.block_id;
			*rank_out = i + 1;
			return true;
		}
	}
	if (ssa->definition_opline_index == ZEND_MIR_ID_INVALID) {
		zend_mir_source_block_ref entry;
		for (i = 0; i < source->block_count(source->context); i++) {
			if (!source->block_at(source->context, i, &entry)) {
				return false;
			}
			if ((entry.flags & ZEND_MIR_SOURCE_BLOCK_ENTRY) != 0) {
				*block_out = entry.id;
				*rank_out = 0;
				return true;
			}
		}
		return false;
	}
	{
		zend_mir_source_opcode_ref opcode;
		if (ssa->definition_opline_index
					>= source->opcode_count(source->context)
				|| !source->opcode_at(source->context,
					ssa->definition_opline_index, &opcode)) {
			return false;
		}
		*block_out = opcode.block_id;
		*rank_out = source->phi_count(source->context)
			+ ssa->definition_opline_index + 1;
		return true;
	}
}

static bool zend_mir_w04_current_slot_value(
	const zend_mir_lowering_context *context,
	const zend_mir_source_slot_ref *slot,
	zend_mir_source_block_id at_block,
	zend_mir_value_id *value_out)
{
	const zend_mir_lowering_source_view *source;
	bool found = false;
	zend_mir_source_block_id selected_block = ZEND_MIR_ID_INVALID;
	uint32_t selected_rank = 0;
	uint32_t i;
	if (context == NULL || context->source == NULL) {
		return false;
	}
	source = context->source;
	for (i = 0; i < source->ssa_count(source->context); i++) {
		zend_mir_source_ssa_ref ssa;
		zend_mir_value_fact_ref fact;
		zend_mir_representation representation;
		zend_mir_source_block_id definition_block;
		uint32_t definition_rank;
		bool later;
		if (!source->ssa_at(source->context, i, &ssa)) {
			return false;
		}
		if (ssa.source_slot_kind != slot->kind
				|| ssa.source_slot != slot->kind_index) {
			continue;
		}
		if (!zend_mir_w04_value_fact(
				context, ssa.ssa_variable_id, &fact, &representation)) {
			continue;
		}
		if (!zend_mir_w04_ssa_definition(
				source, &ssa, &definition_block, &definition_rank)
				|| !zend_mir_w04_source_dominates(
					source, definition_block, at_block)) {
			continue;
		}
		later = !found
			|| (definition_block == selected_block
				&& definition_rank > selected_rank)
			|| (definition_block != selected_block
				&& zend_mir_w04_source_dominates(
					source, selected_block, definition_block));
		if (later) {
			found = true;
			selected_block = definition_block;
			selected_rank = definition_rank;
			*value_out =
				zend_mir_value_from_original_ssa(ssa.ssa_variable_id);
		}
	}
	return true;
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
	uint32_t first_slot = 0;
	uint32_t slot_count = zend_mir_zend_source_slot_count(zend_source);
	uint32_t i;
	if (zend_source == NULL || opcode == NULL
			|| mutator->add_frame_slot == NULL
			|| mutator->add_frame_state == NULL
			|| mutator->add_source_map == NULL) {
		return false;
	}
	for (i = 0; i < slot_count; i++) {
		zend_mir_source_slot_ref source_slot;
		zend_mir_frame_slot_ref frame_slot;
		zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
		uint32_t slot_index;
		if (!zend_mir_zend_source_slot_at(zend_source, i, &source_slot)
				|| !zend_mir_w04_current_slot_value(context,
					&source_slot, source_block_id, &value_id)) {
			return false;
		}
		memset(&frame_slot, 0, sizeof(frame_slot));
		frame_slot.slot_id = source_slot.slot_id;
		frame_slot.index = source_slot.kind_index;
		frame_slot.kind = zend_mir_w04_frame_slot_kind(source_slot.kind);
		frame_slot.representation =
			ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
		frame_slot.materialization = zend_mir_id_is_valid(value_id)
			? ZEND_MIR_MATERIALIZATION_MATERIALIZED
			: ZEND_MIR_MATERIALIZATION_UNDEF;
		frame_slot.ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		frame_slot.value_id = value_id;
		if (frame_slot.kind == ZEND_MIR_FRAME_SLOT_KIND_INVALID
				|| !mutator->add_frame_slot(
					mutator->context, &frame_slot, &slot_index)
				|| (i != 0 && slot_index != first_slot + i)) {
			return false;
		}
		if (i == 0) {
			first_slot = slot_index;
		}
	}
	memset(&frame, 0, sizeof(frame));
	frame.id = ZEND_MIR_ID_INVALID;
	frame.function_id = zend_mir_lowering_context_function_id(context);
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame.opline_index = opcode == NULL ? 0 : opcode->opline_index;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame.slots.offset = slot_count == 0 ? 0 : first_slot;
	frame.slots.count = slot_count;
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
	frame.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_INTERRUPT;
	frame.canonical = true;
	if (!mutator->add_frame_state(mutator->context, &frame, &frame_id)) {
		return false;
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
		return false;
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
		| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_INTERRUPT);
	if (!mutator->add_instruction(mutator->context, &record, statepoint_id)) {
		return false;
	}
	for (i = 0; i < slot_count; i++) {
		zend_mir_source_slot_ref source_slot;
		zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
		if (!zend_mir_zend_source_slot_at(zend_source, i, &source_slot)
				|| !zend_mir_w04_current_slot_value(context,
					&source_slot, source_block_id, &value_id)
				|| (zend_mir_id_is_valid(value_id)
					&& (mutator->add_operand == NULL
						|| !mutator->add_operand(
							mutator->context, *statepoint_id, value_id)))) {
			return false;
		}
	}
	if (!zend_mir_w04_add_instruction(mutator, edge_block,
				ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
				ZEND_MIR_ID_INVALID, record.source_position_id, &branch_id)) {
		return false;
	}
	return true;
}

bool zend_mir_w04_emit_terminator(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	zend_mir_control_flow_map_storage *map)
{
	zend_mir_source_edge_ref edges[2];
	zend_mir_control_flow_edge_mapping mappings[2];
	zend_mir_instruction_id terminator;
	zend_mir_value_id condition = ZEND_MIR_ID_INVALID;
	zend_mir_w04_branch_kind kind = ZEND_MIR_W04_BRANCH_KIND_INVALID;
	uint32_t edge_count = 0;
	uint32_t i;
	if (context == NULL || mutator == NULL || block == NULL || map == NULL
			|| !zend_mir_w04_source_edges(context->source, block->id,
				edges, &edge_count)) {
		return false;
	}
	if (opcode != NULL) {
		kind = zend_mir_w04_branch_kind_for_opcode(opcode->zend_opcode_number);
	}
	if ((kind == ZEND_MIR_W04_BRANCH_UNCONDITIONAL && edge_count != 1)
			|| (kind >= ZEND_MIR_W04_BRANCH_IF_FALSE && edge_count != 2)
			|| (kind == ZEND_MIR_W04_BRANCH_KIND_INVALID && edge_count > 1)) {
		return false;
	}
	if (edge_count == 0) {
		if (opcode != NULL || block->opcode_count != 0) {
			return true;
		}
		return zend_mir_w04_add_instruction(mutator,
			zend_mir_lowering_context_block_id(context),
			ZEND_MIR_OPCODE_UNREACHABLE, ZEND_MIR_REPRESENTATION_CONTROL,
			ZEND_MIR_ID_INVALID, ZEND_MIR_ID_INVALID, &terminator);
	}
	if (edge_count == 2
			&& !zend_mir_w04_condition_value(
				context, mutator, opcode, &condition)) {
		return false;
	}
	if (kind == ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT
			|| kind == ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT) {
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
			edge_count == 1 ? ZEND_MIR_OPCODE_BRANCH
				: ZEND_MIR_OPCODE_COND_BRANCH,
			ZEND_MIR_REPRESENTATION_CONTROL, ZEND_MIR_ID_INVALID,
			opcode == NULL ? ZEND_MIR_ID_INVALID : opcode->source_position_id,
			&terminator)) {
		return false;
	}
	if (edge_count == 2) {
		if (!zend_mir_id_is_valid(condition)
				|| !mutator->add_operand(
					mutator->context, terminator, condition)) {
			return false;
		}
	}
	for (i = 0; i < edge_count; i++) {
		uint32_t mir_index = i;
		zend_mir_block_id target;
		if (edge_count == 2) {
			mir_index = zend_mir_w04_mir_successor_for_source(kind, i);
			if (mir_index > 1) {
				return false;
			}
		}
		if (!zend_mir_control_flow_map_find_block(
				&map->public_map, edges[i].to_block_id, &target)) {
			return false;
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
			zend_mir_block_id edge_block;
			if (!mutator->add_block(mutator->context,
					zend_mir_lowering_context_function_id(context),
					&edge_block)
					|| !zend_mir_w04_emit_edge_statepoint(
						context, mutator, opcode, block->id, edge_block,
						&mappings[i].edge_statepoint_instruction_id)
					|| !mutator->add_edge(mutator->context,
						edge_block, target)) {
				return false;
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
			return false;
		}
	}
	for (i = 0; i < edge_count; i++) {
		if (!zend_mir_control_flow_map_add_edge(map, &mappings[i])) {
			return false;
		}
	}
	return true;
}
