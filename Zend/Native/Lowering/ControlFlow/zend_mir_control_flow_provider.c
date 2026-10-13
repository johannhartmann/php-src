#include <stdlib.h>
#include <string.h>

#include "../Frontend/zend_mir_zend_source.h"
#include "../../MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "../../MIR/Semantics/zend_mir_effect_summary.h"
#include "zend_mir_control_flow_internal.h"

static bool zend_mir_cf_add_instruction(
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

static bool zend_mir_operand_value_fact(
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

static bool zend_mir_cf_value_fact(
	const zend_mir_lowering_context *context, uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *fact_out, zend_mir_representation *out)
{
	zend_mir_source_operand_ref operand;
	zend_mir_value_id value_id;
	memset(&operand, 0, sizeof(operand));
	operand.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	operand.ssa_variable_id = ssa_variable_id;
	return zend_mir_operand_value_fact(
		context, &operand, &value_id, fact_out, out);
}

static bool zend_mir_value_representation(
	const zend_mir_lowering_context *context, uint32_t ssa_variable_id,
	zend_mir_representation *out)
{
	zend_mir_value_fact_ref fact;
	return zend_mir_cf_value_fact(
		context, ssa_variable_id, &fact, out);
}

static bool zend_mir_condition_value(
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
			|| !zend_mir_operand_value_fact(context, &opcode->op1,
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
				|| !zend_mir_cf_add_instruction(mutator,
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
			|| !zend_mir_cf_add_instruction(mutator,
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

static bool zend_mir_object_add_effect(
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

static bool zend_mir_throw_semantics(zend_mir_effect_summary *summary)
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
	if (!zend_mir_object_add_effect(summary, ZEND_MIR_EFFECT_READ_MEMORY)
			|| !zend_mir_object_add_effect(summary, ZEND_MIR_EFFECT_WRITE_MEMORY)
			|| !zend_mir_object_add_effect(summary, ZEND_MIR_EFFECT_ALLOCATE)
			|| !zend_mir_object_add_effect(summary, ZEND_MIR_EFFECT_RUN_DESTRUCTOR)
			|| !zend_mir_object_add_effect(summary, ZEND_MIR_EFFECT_THROW)) {
		return false;
	}
	summary->reads |= frame_domains | heap_domains;
	summary->writes |= frame_domains | heap_domains;
	return zend_mir_effect_summary_init(summary, summary->effects,
		summary->reads, summary->writes, summary->barriers, 0, 0);
}

/*
 * Every SSA value has a canonical Zend-frame storage location, so the physical
 * frame is the authoritative state at a throw or interrupt boundary. The frame
 * record keeps only the source and frame identity; repeating the frame slots in
 * every record would make large functions quadratic in frame size.
 */
static bool zend_mir_emit_canonical_frame(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	zend_mir_safepoint_class safepoint_class,
	zend_mir_frame_state_id *frame_id_out)
{
	const zend_mir_zend_source *zend_source = context->zend_source;
	zend_mir_frame_state_ref frame;
	zend_mir_source_map_ref source_map;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_map_id source_map_id;

	memset(&frame, 0, sizeof(frame));
	frame.id = ZEND_MIR_ID_INVALID;
	frame.function_id = zend_mir_lowering_context_function_id(context);
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame.opline_index = opcode->opline_index;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
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
	frame.safepoint_class = safepoint_class;
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
	*frame_id_out = frame_id;
	return true;
}

static bool zend_mir_emit_throw_frame(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	zend_mir_frame_state_id *frame_id_out)
{
	if (context == NULL || context->zend_source == NULL || opcode == NULL
			|| frame_id_out == NULL || mutator == NULL
			|| mutator->add_frame_slot == NULL
			|| mutator->add_frame_state == NULL
			|| mutator->add_source_map == NULL) {
		return false;
	}
	return zend_mir_emit_canonical_frame(context, mutator, opcode,
		ZEND_MIR_SAFEPOINT_CLASS_DESTRUCTOR, frame_id_out);
}

static bool zend_mir_emit_edge_statepoint(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	zend_mir_block_id edge_block,
	zend_mir_instruction_id *statepoint_id)
{
	zend_mir_instruction_record record;
	zend_mir_frame_state_id frame_id;
	zend_mir_instruction_id branch_id;

	if (context == NULL || context->zend_source == NULL || opcode == NULL
			|| mutator == NULL || statepoint_id == NULL
			|| mutator->add_frame_slot == NULL
			|| mutator->add_frame_state == NULL
			|| mutator->add_source_map == NULL
			|| !zend_mir_emit_canonical_frame(context, mutator, opcode,
				ZEND_MIR_SAFEPOINT_CLASS_OBSERVER, &frame_id)) {
		return false;
	}
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = edge_block;
	record.opcode = ZEND_MIR_OPCODE_STATEPOINT;
	record.representation = ZEND_MIR_REPRESENTATION_VOID;
	record.result_id = ZEND_MIR_ID_INVALID;
	record.frame_state_id = frame_id;
	record.source_position_id = opcode->source_position_id;
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
	return mutator->add_instruction(
			mutator->context, &record, statepoint_id)
		&& zend_mir_cf_add_instruction(mutator, edge_block,
			ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_REPRESENTATION_CONTROL,
			ZEND_MIR_ID_INVALID, record.source_position_id, &branch_id);
}

static bool zend_mir_edge_statepoint_opcode(
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

static bool zend_mir_emit_terminator_edges(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	const zend_mir_source_edge_ref *edges,
	zend_mir_control_flow_map_storage *map,
	zend_mir_branch_kind kind,
	zend_mir_instruction_id terminator,
	uint32_t edge_count)
{
	/* Only multiway branches have more than two edges. */
	zend_mir_control_flow_edge_mapping local_mappings[2];
	zend_mir_control_flow_edge_mapping *mappings = local_mappings;
	uint32_t i;
	bool success = false;

	if (edge_count > 2) {
		mappings = calloc(edge_count, sizeof(*mappings));
		if (mappings == NULL) {
			goto done;
		}
	}
	for (i = 0; i < edge_count; i++) {
		uint32_t mir_index = i;
		zend_mir_block_id target;
		if (edge_count == 2) {
			mir_index = zend_mir_mir_successor_for_source(kind, i);
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
		if (zend_mir_control_flow_cycle_edge_requires_statepoint(
				&map->cycle_analysis, edges[i].id)) {
			zend_mir_source_opcode_ref statepoint_source_opcode;
			const zend_mir_source_opcode_ref *statepoint_opcode = opcode;
			zend_mir_block_id edge_block;

			if (statepoint_opcode == NULL
					&& !zend_mir_edge_statepoint_opcode(
						context->source, block, &statepoint_source_opcode)) {
				goto done;
			}
			if (statepoint_opcode == NULL) {
				statepoint_opcode = &statepoint_source_opcode;
			}
			if (!mutator->add_block(mutator->context,
					zend_mir_lowering_context_function_id(context),
					&edge_block)
					|| !zend_mir_emit_edge_statepoint(
						context, mutator, statepoint_opcode, edge_block,
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
	if (mappings != local_mappings) {
		free(mappings);
	}
	return success;
}

bool zend_mir_emit_terminator(
	zend_mir_lowering_context *context,
	zend_mir_mutator *mutator,
	const zend_mir_source_opcode_ref *opcode,
	const zend_mir_source_block_ref *block,
	const zend_mir_source_edge_ref *edges,
	uint32_t edge_count,
	bool machine_condition,
	zend_mir_control_flow_map_storage *map)
{
	zend_mir_instruction_id terminator;
	zend_mir_value_id condition = ZEND_MIR_ID_INVALID;
	zend_mir_branch_kind kind = ZEND_MIR_BRANCH_KIND_INVALID;
	bool source_condition = false;
	if (context == NULL || mutator == NULL || block == NULL || map == NULL
			|| (edge_count != 0 && edges == NULL)) {
		return false;
	}
	if (opcode != NULL) {
		kind = zend_mir_branch_kind_for_opcode(opcode->zend_opcode_number);
	}
	source_condition = context->zend_source != NULL
		&& edge_count == 2
		&& (!machine_condition
			|| kind == ZEND_MIR_BRANCH_BIND_STATIC
			|| kind == ZEND_MIR_BRANCH_FRAMELESS)
		&& kind != ZEND_MIR_BRANCH_CATCH
		&& kind != ZEND_MIR_BRANCH_FINALLY_CALL
		&& kind != ZEND_MIR_BRANCH_ITERATOR
		&& kind != ZEND_MIR_BRANCH_MULTIWAY;
#if !defined(NDEBUG)
	/* Validation proved the edge count against the same terminator: the
	 * block's last non-NOP opcode, which lowering reaches as the branch it
	 * stops at, or a non-branch when it finds none. */
	if (!zend_mir_branch_edge_count_is_valid(
			kind, opcode == NULL ? 0 : opcode->zend_opcode_number, edge_count)) {
		return false;
	}
#endif
	if (kind == ZEND_MIR_BRANCH_THROW) {
		zend_mir_effect_summary summary;
		zend_mir_frame_state_id frame_id;
		zend_mir_instruction_record record;

		if (opcode == NULL || block->opcode_count == 0
				|| !zend_mir_throw_semantics(&summary)
				|| !zend_mir_emit_throw_frame(
					context, mutator, opcode, &frame_id)) {
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
	if (edge_count == 0 && kind != ZEND_MIR_BRANCH_FINALLY_RETURN) {
		if (opcode != NULL || block->opcode_count != 0) {
			return true;
		}
		return zend_mir_cf_add_instruction(mutator,
			zend_mir_lowering_context_block_id(context),
			ZEND_MIR_OPCODE_UNREACHABLE, ZEND_MIR_REPRESENTATION_CONTROL,
			ZEND_MIR_ID_INVALID, ZEND_MIR_ID_INVALID, &terminator);
	}
	if (edge_count == 2 && kind != ZEND_MIR_BRANCH_CATCH
			&& kind != ZEND_MIR_BRANCH_FINALLY_CALL
			&& kind != ZEND_MIR_BRANCH_ITERATOR
			&& kind != ZEND_MIR_BRANCH_MULTIWAY && !source_condition
			&& !zend_mir_condition_value(
				context, mutator, opcode, &condition)) {
		return false;
	}
	if (!source_condition
			&& (kind == ZEND_MIR_BRANCH_IF_FALSE_WITH_RESULT
				|| kind == ZEND_MIR_BRANCH_IF_TRUE_WITH_RESULT
				|| kind == ZEND_MIR_BRANCH_JMP_SET
				|| kind == ZEND_MIR_BRANCH_COALESCE
				|| kind == ZEND_MIR_BRANCH_JMP_NULL)) {
		zend_mir_instruction_id copy_id;
		zend_mir_value_fact_ref input_fact;
		zend_mir_value_id input;
		zend_mir_representation input_representation;
		zend_mir_representation representation;
		const bool boolean_result =
			kind == ZEND_MIR_BRANCH_IF_FALSE_WITH_RESULT
			|| kind == ZEND_MIR_BRANCH_IF_TRUE_WITH_RESULT;
		if (opcode == NULL
				|| opcode->op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA
				|| opcode->result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
				|| !zend_mir_operand_value_fact(context, &opcode->op1,
					&input, &input_fact, &input_representation)
				|| !zend_mir_value_representation(context,
					opcode->result.ssa_variable_id, &representation)
				|| (boolean_result
					? representation != ZEND_MIR_REPRESENTATION_I1
					: representation != input_representation)
				|| !zend_mir_id_is_valid(condition)
				|| !zend_mir_cf_add_instruction(mutator,
					zend_mir_lowering_context_block_id(context),
					ZEND_MIR_OPCODE_COPY, representation,
					zend_mir_value_from_original_ssa(
						opcode->result.ssa_variable_id),
					opcode->source_position_id, &copy_id)
				|| !mutator->add_operand(mutator->context, copy_id,
					boolean_result ? condition : input)) {
			return false;
		}
	}
	if (!zend_mir_cf_add_instruction(mutator,
			zend_mir_lowering_context_block_id(context),
			kind == ZEND_MIR_BRANCH_CATCH
				? ZEND_MIR_OPCODE_CATCH_ENTER
				: kind == ZEND_MIR_BRANCH_FINALLY_CALL
					? ZEND_MIR_OPCODE_FINALLY_CALL
				: kind == ZEND_MIR_BRANCH_FINALLY_RETURN
					? ZEND_MIR_OPCODE_FINALLY_RETURN
				: kind == ZEND_MIR_BRANCH_ITERATOR
					? ZEND_MIR_OPCODE_ITERATOR_BRANCH
				: kind == ZEND_MIR_BRANCH_MULTIWAY && edge_count > 1
					? ZEND_MIR_OPCODE_VALUE_MULTI_BRANCH
				: kind == ZEND_MIR_BRANCH_BIND_STATIC
					? ZEND_MIR_OPCODE_VALUE_BIND_STATIC_BRANCH
				: kind == ZEND_MIR_BRANCH_FRAMELESS
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
	if (edge_count == 2 && kind != ZEND_MIR_BRANCH_CATCH
			&& kind != ZEND_MIR_BRANCH_FINALLY_CALL
			&& kind != ZEND_MIR_BRANCH_ITERATOR
			&& kind != ZEND_MIR_BRANCH_MULTIWAY && !source_condition) {
		if (!zend_mir_id_is_valid(condition)
				|| !mutator->add_operand(
					mutator->context, terminator, condition)) {
			return false;
		}
	}
	return zend_mir_emit_terminator_edges(
		context, mutator, opcode, block, edges, map, kind, terminator,
		edge_count);
}
