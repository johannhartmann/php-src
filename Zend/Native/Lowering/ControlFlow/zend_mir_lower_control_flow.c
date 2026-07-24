#include <stdlib.h>
#include <string.h>

#include "../zend_mir_lowering_zend.h"
#include "../Frontend/zend_mir_zend_source.h"
#include "../Scalar/Logic/zend_mir_logic.h"
#include "../../MIR/Verify/zend_mir_verify_control_flow.h"
#include "../../MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "zend_mir_control_flow_internal.h"

typedef struct _zend_mir_w04_phi_analysis {
	uint32_t phi_count;
	uint32_t input_count;
	uint32_t *input_offsets;
	uint32_t *input_indices;
	bool *dynamic_members;
} zend_mir_w04_phi_analysis;

static void zend_mir_w04_phi_analysis_destroy(
	zend_mir_w04_phi_analysis *analysis)
{
	if (analysis == NULL) {
		return;
	}
	free(analysis->dynamic_members);
	free(analysis->input_indices);
	free(analysis->input_offsets);
	memset(analysis, 0, sizeof(*analysis));
}

static zend_mir_lowering_result zend_mir_w04_result(
	zend_mir_lowering_status status, zend_mir_lowering_diagnostic_code code)
{
	zend_mir_lowering_result result;
	memset(&result, 0, sizeof(result));
	result.status = status;
	result.diagnostic_code = code;
	return result;
}

static zend_mir_lowering_result zend_mir_w04_abort(
	zend_mir_lowering_context *context, zend_mir_module *module,
	zend_mir_control_flow_map_storage *storage,
	zend_mir_w04_phi_analysis *phi_analysis,
	zend_mir_lowering_status status, zend_mir_lowering_diagnostic_code code)
{
	if (module != NULL) {
		context->module_ops.destroy(context->module_ops.context, module);
	}
	zend_mir_w04_phi_analysis_destroy(phi_analysis);
	zend_mir_control_flow_map_storage_destroy(storage);
	context->busy = false;
	context->current_provider = NULL;
	context->current_opcode = NULL;
	context->values_predeclared = false;
	return zend_mir_w04_result(status, code);
}

static bool zend_mir_w04_raw_fact_for_ssa(
	const zend_mir_lowering_context *context, uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *fact_out,
	zend_mir_representation *representation_out)
{
	zend_mir_value_id value_id;
	if (context == NULL || fact_out == NULL
			|| representation_out == NULL
			|| ssa_variable_id > ZEND_MIR_VALUE_ORIGINAL_MAX) {
		return false;
	}
	value_id = zend_mir_value_from_original_ssa(ssa_variable_id);
	if (!zend_mir_lowering_context_value_fact(context, value_id, fact_out)) {
		return false;
	}
	*representation_out =
		zend_mir_scalar_type_representation(fact_out->exact_type);
	return *representation_out != ZEND_MIR_REPRESENTATION_INVALID;
}

static uint32_t zend_mir_w04_phi_component_find(
	uint32_t *parents, uint32_t member)
{
	uint32_t root = member;
	while (parents[root] != root) {
		root = parents[root];
	}
	while (parents[member] != member) {
		uint32_t next = parents[member];
		parents[member] = root;
		member = next;
	}
	return root;
}

static void zend_mir_w04_phi_component_union(
	uint32_t *parents, uint8_t *ranks, uint32_t left, uint32_t right)
{
	uint32_t left_root = zend_mir_w04_phi_component_find(parents, left);
	uint32_t right_root = zend_mir_w04_phi_component_find(parents, right);

	if (left_root == right_root) {
		return;
	}
	if (ranks[left_root] < ranks[right_root]) {
		parents[left_root] = right_root;
	} else {
		parents[right_root] = left_root;
		if (ranks[left_root] == ranks[right_root]) {
			ranks[left_root]++;
		}
	}
}

static bool zend_mir_w09_phi_is_dynamic(
	const zend_mir_lowering_context *context,
	const zend_mir_w04_phi_analysis *analysis,
	const zend_mir_source_phi_ref *phi)
{
	zend_mir_value_fact_ref result_fact;
	zend_mir_representation result_representation;
	uint32_t i;

	if (context == NULL || analysis == NULL || phi == NULL
			|| phi->id >= analysis->phi_count
			|| !zend_mir_w04_raw_fact_for_ssa(context,
				phi->result_ssa_variable_id, &result_fact,
				&result_representation)
			|| result_representation == ZEND_MIR_REPRESENTATION_ZVAL) {
		return true;
	}
	for (i = analysis->input_offsets[phi->id];
		i < analysis->input_offsets[phi->id + 1]; i++) {
		zend_mir_source_phi_input_ref input;
		zend_mir_value_fact_ref input_fact;
		zend_mir_representation input_representation;

		if (!context->source->phi_input_at(
				context->source->context,
				analysis->input_indices[i], &input)) {
			return true;
		}
		if (!zend_mir_w04_raw_fact_for_ssa(context,
				input.source_ssa_variable_id, &input_fact,
				&input_representation)
				|| input_representation != result_representation
				|| input_fact.exact_type != result_fact.exact_type) {
			return true;
		}
	}
	return false;
}

/*
 * W09 executes refcounted and dynamically typed value operations against the
 * real source frame.  Zend SSA can still contain merge nodes for those slots
 * after the private scalar prerequisite projection removes the defining value
 * opcodes.  Keep the original PHI topology and stable IDs, but represent the
 * entire connected dynamic-PHI component as zvals without inventing scalar
 * facts for it.
 */
static bool zend_mir_w04_phi_analysis_init(
	const zend_mir_lowering_context *context,
	zend_mir_w04_phi_analysis *analysis)
{
	uint32_t *parents = NULL;
	uint32_t *cursors = NULL;
	uint8_t *ranks = NULL;
	bool *members = NULL;
	bool *component_dynamic = NULL;
	uint32_t ssa_count;
	uint32_t i;

	if (context == NULL || analysis == NULL || context->source == NULL
			|| context->source->ssa_count == NULL
			|| context->source->phi_count == NULL
			|| context->source->phi_at == NULL
			|| context->source->phi_input_count == NULL
			|| context->source->phi_input_at == NULL) {
		return false;
	}
	memset(analysis, 0, sizeof(*analysis));
	ssa_count = context->source->ssa_count(context->source->context);
	analysis->phi_count =
		context->source->phi_count(context->source->context);
	analysis->input_count =
		context->source->phi_input_count(context->source->context);
	if (analysis->phi_count == UINT32_MAX) {
		return false;
	}
	analysis->input_offsets = calloc(
		(size_t) analysis->phi_count + 1, sizeof(*analysis->input_offsets));
	if (analysis->input_count != 0) {
		analysis->input_indices = malloc(
			(size_t) analysis->input_count * sizeof(*analysis->input_indices));
	}
	if (analysis->input_offsets == NULL
			|| (analysis->input_count != 0
				&& analysis->input_indices == NULL)) {
		goto failed;
	}
	for (i = 0; i < analysis->input_count; i++) {
		zend_mir_source_phi_input_ref input;
		if (!context->source->phi_input_at(
				context->source->context, i, &input)
				|| input.phi_id >= analysis->phi_count
				|| analysis->input_offsets[input.phi_id + 1]
					== UINT32_MAX) {
			goto failed;
		}
		analysis->input_offsets[input.phi_id + 1]++;
	}
	for (i = 1; i <= analysis->phi_count; i++) {
		if (analysis->input_offsets[i]
				> UINT32_MAX - analysis->input_offsets[i - 1]) {
			goto failed;
		}
		analysis->input_offsets[i] += analysis->input_offsets[i - 1];
	}
	if (analysis->input_offsets[analysis->phi_count]
			!= analysis->input_count) {
		goto failed;
	}
	if (analysis->phi_count != 0) {
		cursors = malloc(
			(size_t) analysis->phi_count * sizeof(*cursors));
		if (cursors == NULL) {
			goto failed;
		}
		memcpy(cursors, analysis->input_offsets,
			(size_t) analysis->phi_count * sizeof(*cursors));
	}
	for (i = 0; i < analysis->input_count; i++) {
		zend_mir_source_phi_input_ref input;
		uint32_t position;
		if (!context->source->phi_input_at(
				context->source->context, i, &input)) {
			goto failed;
		}
		position = cursors[input.phi_id]++;
		if (position >= analysis->input_count) {
			goto failed;
		}
		analysis->input_indices[position] = i;
	}
	free(cursors);
	cursors = NULL;

	if (context->zend_source == NULL || !context->zend_source->w09
			|| ssa_count == 0 || analysis->phi_count == 0) {
		return true;
	}
	parents = malloc((size_t) ssa_count * sizeof(*parents));
	ranks = calloc(ssa_count, sizeof(*ranks));
	members = calloc(ssa_count, sizeof(*members));
	component_dynamic = calloc(ssa_count, sizeof(*component_dynamic));
	analysis->dynamic_members =
		calloc(ssa_count, sizeof(*analysis->dynamic_members));
	if (parents == NULL || ranks == NULL || members == NULL
			|| component_dynamic == NULL
			|| analysis->dynamic_members == NULL) {
		goto failed;
	}
	for (i = 0; i < ssa_count; i++) {
		parents[i] = i;
	}
	for (i = 0; i < analysis->phi_count; i++) {
		zend_mir_source_phi_ref phi;
		uint32_t j;
		if (!context->source->phi_at(
				context->source->context, i, &phi)
				|| phi.id != i
				|| phi.result_ssa_variable_id >= ssa_count) {
			goto failed;
		}
		members[phi.result_ssa_variable_id] = true;
		for (j = analysis->input_offsets[i];
			j < analysis->input_offsets[i + 1]; j++) {
			zend_mir_source_phi_input_ref input;
			if (!context->source->phi_input_at(
					context->source->context,
					analysis->input_indices[j], &input)
					|| input.source_ssa_variable_id >= ssa_count) {
				goto failed;
			}
			members[input.source_ssa_variable_id] = true;
			zend_mir_w04_phi_component_union(
				parents, ranks, phi.result_ssa_variable_id,
				input.source_ssa_variable_id);
		}
	}
	for (i = 0; i < analysis->phi_count; i++) {
		zend_mir_source_phi_ref phi;
		if (!context->source->phi_at(
				context->source->context, i, &phi)) {
			goto failed;
		}
		if (zend_mir_w09_phi_is_dynamic(context, analysis, &phi)) {
			component_dynamic[zend_mir_w04_phi_component_find(
				parents, phi.result_ssa_variable_id)] = true;
		}
	}
	for (i = 0; i < ssa_count; i++) {
		if (members[i]) {
			analysis->dynamic_members[i] =
				component_dynamic[zend_mir_w04_phi_component_find(
					parents, i)];
		}
	}
	free(component_dynamic);
	free(members);
	free(ranks);
	free(parents);
	return true;

failed:
	free(component_dynamic);
	free(members);
	free(ranks);
	free(parents);
	free(cursors);
	zend_mir_w04_phi_analysis_destroy(analysis);
	return false;
}

static bool zend_mir_w04_fact_for_ssa(
	const zend_mir_lowering_context *context,
	const zend_mir_w04_phi_analysis *analysis,
	uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *fact_out,
	zend_mir_representation *representation_out)
{
	if ((analysis != NULL && analysis->dynamic_members != NULL
			&& context != NULL && context->source != NULL
			&& context->source->ssa_count != NULL
			&& ssa_variable_id
				< context->source->ssa_count(context->source->context)
			&& analysis->dynamic_members[ssa_variable_id])
			|| (context != NULL && context->zend_source != NULL
				&& context->zend_source->w09 && context->source != NULL
				&& context->source->ssa_count != NULL
				&& ssa_variable_id
					< context->source->ssa_count(context->source->context)
				&& !zend_mir_w04_raw_fact_for_ssa(context, ssa_variable_id,
					fact_out, representation_out))) {
		/*
		 * W09 executes the canonical source zval when Zend's analysis cannot
		 * prove one exact non-refcounted scalar type.  Undefined CV inputs are
		 * one important example: COALESCE must observe them without a warning,
		 * and its merge result still needs a declared MIR value.  Predeclare all
		 * such source SSA identities as zvals instead of silently omitting them.
		 */
		memset(fact_out, 0, sizeof(*fact_out));
		fact_out->id = ZEND_MIR_ID_INVALID;
		fact_out->value_id =
			zend_mir_value_from_original_ssa(ssa_variable_id);
		*representation_out = ZEND_MIR_REPRESENTATION_ZVAL;
		return zend_mir_id_is_valid(fact_out->value_id);
	}
	return zend_mir_w04_raw_fact_for_ssa(
		context, ssa_variable_id, fact_out, representation_out);
}

static bool zend_mir_w04_fact_for_operand(
	const zend_mir_lowering_context *context,
	const zend_mir_w04_phi_analysis *analysis,
	const zend_mir_source_operand_ref *operand,
	zend_mir_value_fact_ref *fact_out,
	zend_mir_representation *representation_out)
{
	zend_mir_value_id value_id;
	if (context == NULL || context->source == NULL || operand == NULL
			|| fact_out == NULL || representation_out == NULL) {
		return false;
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_SSA) {
		return zend_mir_w04_fact_for_ssa(context, analysis,
			operand->ssa_variable_id, fact_out, representation_out);
	}
	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| context->source->literal_count == NULL
			|| operand->index
				>= context->source->literal_count(context->source->context)
			|| operand->index > ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX) {
		return false;
	}
	value_id = zend_mir_value_from_synthetic(operand->index);
	if (!zend_mir_lowering_context_value_fact(context, value_id, fact_out)) {
		return false;
	}
	*representation_out =
		zend_mir_scalar_type_representation(fact_out->exact_type);
	return *representation_out != ZEND_MIR_REPRESENTATION_INVALID;
}

static bool zend_mir_w04_validate_branch_proofs(
	const zend_mir_lowering_context *context,
	const zend_mir_w04_phi_analysis *analysis)
{
	uint32_t opcode_count;
	uint32_t literal_count;
	uint32_t i;
	if (context == NULL || context->source == NULL
			|| context->source->opcode_count == NULL
			|| context->source->opcode_at == NULL
			|| context->source->literal_count == NULL) {
		return false;
	}
	opcode_count = context->source->opcode_count(context->source->context);
	literal_count = context->source->literal_count(context->source->context);
	if (literal_count > ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX
			|| opcode_count
				> (ZEND_MIR_VALUE_SYNTHETIC_PAYLOAD_MAX - literal_count) / 3) {
		return false;
	}
	for (i = 0; i < opcode_count; i++) {
		zend_mir_source_opcode_ref opcode;
		zend_mir_w04_branch_kind kind;
		zend_mir_value_fact_ref input_fact;
		zend_mir_representation input_representation;
		if (!context->source->opcode_at(
				context->source->context, i, &opcode)) {
			return false;
		}
		kind = zend_mir_w04_branch_kind_for_opcode(
			opcode.zend_opcode_number);
		if (kind == ZEND_MIR_W04_BRANCH_KIND_INVALID) {
			continue;
		}
		if (kind == ZEND_MIR_W04_BRANCH_UNCONDITIONAL) {
			if (opcode.op1.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| opcode.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| opcode.result.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				return false;
			}
			continue;
		}
		if (kind == ZEND_MIR_W10_BRANCH_THROW) {
			if (opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| opcode.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| opcode.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				return false;
			}
			continue;
		}
		if (kind == ZEND_MIR_W04_BRANCH_CATCH
				|| kind == ZEND_MIR_W08_BRANCH_FINALLY_CALL
				|| kind == ZEND_MIR_W08_BRANCH_FINALLY_RETURN
				|| kind == ZEND_MIR_W09_BRANCH_ITERATOR) {
			/*
			 * FAST_CALL/FAST_RET operands are Zend's private finally-state
			 * slot and try-table index. They remain source-backed metadata,
			 * not scalar MIR values.
			 */
			if (kind == ZEND_MIR_W04_BRANCH_CATCH
					&& (opcode.op1.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| opcode.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
						|| opcode.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
				return false;
			}
			if (kind == ZEND_MIR_W09_BRANCH_ITERATOR
					&& opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				return false;
			}
			continue;
		}
		if (context->zend_source != NULL && context->zend_source->w09) {
			if (opcode.op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| opcode.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| ((kind == ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT
							|| kind == ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT
							|| kind == ZEND_MIR_W09_BRANCH_COALESCE
							|| kind == ZEND_MIR_W09_BRANCH_JMP_SET
							|| kind == ZEND_MIR_W10_BRANCH_JMP_NULL)
						? opcode.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
						: opcode.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
				return false;
			}
			continue;
		}
		if (opcode.op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
				|| !zend_mir_w04_fact_for_operand(
					context, analysis, &opcode.op1,
					&input_fact, &input_representation)
				|| (input_fact.flags
					& ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0
				|| (input_fact.exact_type == ZEND_MIR_SCALAR_TYPE_F64
					&& (input_fact.flags
						& ZEND_MIR_VALUE_FACT_FINITE) == 0)) {
			return false;
		}
		if (kind == ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT
				|| kind == ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT) {
			zend_mir_value_fact_ref result_fact;
			zend_mir_representation result_representation;
			if (opcode.result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
					|| !zend_mir_w04_fact_for_ssa(context, analysis,
						opcode.result.ssa_variable_id, &result_fact,
						&result_representation)
					|| result_fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1
					|| result_representation != ZEND_MIR_REPRESENTATION_I1
					|| (result_fact.flags
						& ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
				return false;
			}
		} else if (opcode.result.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_predeclare_values(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_w04_phi_analysis *analysis)
{
	uint32_t i;
	for (i = 0; i < context->source->ssa_count(context->source->context); i++) {
		zend_mir_source_ssa_ref ssa;
		zend_mir_value_fact_ref fact;
		zend_mir_representation representation;
		zend_mir_value_fact_id fact_id;
		if (!context->source->ssa_at(context->source->context, i, &ssa)) {
			return false;
		}
		if (!zend_mir_w04_fact_for_ssa(
				context, analysis, ssa.ssa_variable_id,
				&fact, &representation)) {
			continue;
		}
		if (!mutator->add_value(mutator->context,
				zend_mir_value_from_original_ssa(ssa.ssa_variable_id),
				representation, ZEND_MIR_OWNERSHIP_STATE_OWNED)) {
			return false;
		}
		if (fact.exact_type != ZEND_MIR_SCALAR_TYPE_NONE
				&& (mutator->add_value_fact == NULL
				|| !mutator->add_value_fact(
					mutator->context, &fact, &fact_id))) {
			return false;
		}
	}
	context->values_predeclared = true;
	return true;
}

static bool zend_mir_w04_validate_scalar_phis(
	const zend_mir_lowering_context *context,
	const zend_mir_w04_phi_analysis *analysis)
{
	uint32_t i;
	for (i = 0; i < analysis->phi_count; i++) {
		zend_mir_source_phi_ref phi;
		zend_mir_value_fact_ref result_fact;
		zend_mir_representation result_representation;
		uint32_t j;
		if (!context->source->phi_at(context->source->context, i, &phi)
				|| !zend_mir_w04_fact_for_ssa(context, analysis,
					phi.result_ssa_variable_id, &result_fact,
					&result_representation)) {
			return false;
		}
		for (j = analysis->input_offsets[i];
			j < analysis->input_offsets[i + 1]; j++) {
			zend_mir_source_phi_input_ref input;
			zend_mir_value_fact_ref input_fact;
			zend_mir_representation input_representation;
			if (!context->source->phi_input_at(
					context->source->context,
					analysis->input_indices[j], &input)) {
				return false;
			}
			if (!zend_mir_w04_fact_for_ssa(context, analysis,
					input.source_ssa_variable_id, &input_fact,
					&input_representation)
					|| input_representation != result_representation
					|| (phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
						&& result_fact.exact_type
							!= ZEND_MIR_SCALAR_TYPE_NONE
						&& input_fact.exact_type
								!= result_fact.exact_type)) {
				return false;
			}
		}
		if (phi.kind == ZEND_MIR_SOURCE_PHI_PI_RANGE
				&& !(context->zend_source != NULL
					&& context->zend_source->w09
					&& result_representation == ZEND_MIR_REPRESENTATION_ZVAL)
				&& (result_fact.exact_type != ZEND_MIR_SCALAR_TYPE_I64
					|| (result_fact.flags
						& ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) == 0
					|| ((phi.constraint.flags
							& ZEND_MIR_SOURCE_PHI_RANGE_MIN_UNBOUNDED) == 0
						&& result_fact.integer_min
							< phi.constraint.range_min)
					|| ((phi.constraint.flags
							& ZEND_MIR_SOURCE_PHI_RANGE_MAX_UNBOUNDED) == 0
						&& result_fact.integer_max
								> phi.constraint.range_max))) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_emit_phis(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_w04_phi_analysis *analysis)
{
	uint32_t i;
	for (i = 0; i < analysis->phi_count; i++) {
		zend_mir_source_phi_ref phi;
		zend_mir_control_flow_phi_mapping mapping;
		zend_mir_instruction_record record;
		zend_mir_block_id block_id;
		zend_mir_value_fact_ref result_fact;
		zend_mir_representation representation;
		uint32_t j;
		if (!context->source->phi_at(context->source->context, i, &phi)
				|| !zend_mir_control_flow_map_find_block(
					&storage->public_map, phi.block_id, &block_id)
				|| !zend_mir_w04_fact_for_ssa(context, analysis,
					phi.result_ssa_variable_id, &result_fact,
					&representation)) {
			return false;
		}
		memset(&record, 0, sizeof(record));
		record.id = ZEND_MIR_ID_INVALID;
		record.block_id = block_id;
		record.opcode = phi.kind == ZEND_MIR_SOURCE_PHI_MERGE
			? ZEND_MIR_OPCODE_PHI : ZEND_MIR_OPCODE_COPY;
		record.representation = representation;
		record.result_id =
			zend_mir_value_from_original_ssa(phi.result_ssa_variable_id);
		record.frame_state_id = ZEND_MIR_ID_INVALID;
		record.source_position_id = ZEND_MIR_ID_INVALID;
		if (!mutator->add_instruction(mutator->context, &record,
				&mapping.mir_phi_instruction_id)) {
			return false;
		}
		for (j = analysis->input_offsets[i];
				j < analysis->input_offsets[i + 1]; j++) {
			zend_mir_source_phi_input_ref input;
			if (!context->source->phi_input_at(
					context->source->context,
					analysis->input_indices[j], &input)) {
				return false;
			}
			if (!mutator->add_operand(mutator->context,
						mapping.mir_phi_instruction_id,
						zend_mir_value_from_original_ssa(
							input.source_ssa_variable_id))) {
				return false;
			}
		}
		mapping.source_phi_id = phi.id;
		mapping.mir_result_value_id = record.result_id;
		if (!zend_mir_control_flow_map_add_phi(storage, &mapping)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w04_emit_bool_identity(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	const zend_mir_w04_phi_analysis *analysis,
	const zend_mir_source_opcode_ref *opcode)
{
	zend_mir_value_fact_ref input_fact;
	zend_mir_value_fact_ref result_fact;
	zend_mir_representation input_representation;
	zend_mir_representation result_representation;
	zend_mir_instruction_record record;
	zend_mir_instruction_id instruction_id;
	zend_mir_value_id input_id;
	if (context == NULL || mutator == NULL || opcode == NULL
			|| opcode->zend_opcode_number != ZEND_MIR_LOGIC_ZEND_BOOL
			|| opcode->op1.kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| opcode->op2.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED
			|| opcode->result.kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| !zend_mir_w04_fact_for_operand(
				context, analysis, &opcode->op1,
				&input_fact, &input_representation)
			|| !zend_mir_w04_fact_for_ssa(context, analysis,
				opcode->result.ssa_variable_id, &result_fact,
				&result_representation)
			|| input_fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1
			|| result_fact.exact_type != ZEND_MIR_SCALAR_TYPE_I1
			|| input_representation != ZEND_MIR_REPRESENTATION_I1
			|| result_representation != ZEND_MIR_REPRESENTATION_I1
			|| (input_fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0
			|| (result_fact.flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0) {
		return false;
	}
	input_id = zend_mir_value_from_original_ssa(opcode->op1.ssa_variable_id);
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = zend_mir_lowering_context_block_id(context);
	record.opcode = ZEND_MIR_OPCODE_COPY;
	record.representation = ZEND_MIR_REPRESENTATION_I1;
	record.result_id =
		zend_mir_value_from_original_ssa(opcode->result.ssa_variable_id);
	record.frame_state_id = ZEND_MIR_ID_INVALID;
	record.source_position_id = opcode->source_position_id;
	return zend_mir_id_is_valid(input_id)
		&& zend_mir_id_is_valid(record.result_id)
		&& mutator->add_instruction != NULL
		&& mutator->add_operand != NULL
		&& mutator->add_instruction(
			mutator->context, &record, &instruction_id)
		&& mutator->add_operand(
			mutator->context, instruction_id, input_id);
}

static bool zend_mir_w04_lower_blocks(
	zend_mir_lowering_context *context, zend_mir_mutator *mutator,
	zend_mir_control_flow_map_storage *storage,
	const zend_mir_w04_phi_analysis *analysis)
{
	uint32_t i;
	for (i = 0; i < context->source->block_count(context->source->context); i++) {
		zend_mir_source_block_ref block;
		zend_mir_source_opcode_ref opcode;
		zend_mir_source_opcode_ref *terminator_source = NULL;
		zend_mir_block_id block_id;
		uint32_t j;
		if (!context->source->block_at(context->source->context, i, &block)) {
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if (!zend_mir_control_flow_map_find_block(
					&storage->public_map, block.id, &block_id)
				|| !zend_mir_lowering_context_set_block_id(context, block_id)) {
			return false;
		}
		if ((block.flags & ZEND_MIR_SOURCE_BLOCK_FINALLY_ENTRY) != 0) {
			zend_mir_instruction_record finally_enter;
			zend_mir_instruction_id finally_enter_id;

			memset(&finally_enter, 0, sizeof(finally_enter));
			finally_enter.id = ZEND_MIR_ID_INVALID;
			finally_enter.block_id = block_id;
			finally_enter.opcode = ZEND_MIR_OPCODE_FINALLY_ENTER;
			finally_enter.representation = ZEND_MIR_REPRESENTATION_VOID;
			finally_enter.result_id = ZEND_MIR_ID_INVALID;
			finally_enter.frame_state_id = ZEND_MIR_ID_INVALID;
			finally_enter.source_position_id = block.first_opcode_ordinal;
			if (mutator->add_instruction == NULL
					|| !mutator->add_instruction(mutator->context,
						&finally_enter, &finally_enter_id)) {
				return false;
			}
		}
		for (j = 0; j < block.opcode_count; j++) {
			const zend_mir_lowering_provider *provider;
			zend_mir_lowering_status status;
			if (!context->source->opcode_at(context->source->context,
					block.first_opcode_ordinal + j, &opcode)) {
				return false;
			}
			if (zend_mir_w04_branch_kind_for_opcode(opcode.zend_opcode_number)
					!= ZEND_MIR_W04_BRANCH_KIND_INVALID) {
				if (j + 1 != block.opcode_count) {
					return false;
				}
				terminator_source = &opcode;
				continue;
			}
			if (opcode.zend_opcode_number == ZEND_MIR_LOGIC_ZEND_BOOL
					&& zend_mir_w04_emit_bool_identity(
						context, mutator, analysis, &opcode)) {
				continue;
			}
			provider = zend_mir_lowering_registry_find(
				context->registry, opcode.zend_opcode_number);
			if (provider == NULL) {
				return false;
			}
			context->current_provider = provider;
			context->current_opcode = &opcode;
			context->provider_status = ZEND_MIR_LOWERING_SUCCESS;
			context->provider_diagnostic = ZEND_MIRL_OK;
			status = provider->lower(context, &opcode, mutator);
			if (status != ZEND_MIR_LOWERING_SUCCESS
					|| context->provider_status != ZEND_MIR_LOWERING_SUCCESS) {
				return false;
			}
		}
		if (!zend_mir_w04_emit_terminator(
				context, mutator, terminator_source, &block, storage)) {
			return false;
		}
	}
	context->current_provider = NULL;
	context->current_opcode = NULL;
	return true;
}

zend_mir_lowering_result zend_mir_lower_w04_zend_source(
	zend_mir_lowering_context *context,
	zend_mir_mutator *requested_mutator,
	zend_mir_control_flow_map *map)
{
	zend_mir_control_flow_map_storage storage;
	zend_mir_w04_phi_analysis phi_analysis;
	zend_mir_w04_validation validation;
	zend_mir_module *module = NULL;
	zend_mir_mutator *mutator;
	const zend_mir_view *view;
	uint32_t i;
	memset(&storage, 0, sizeof(storage));
	memset(&phi_analysis, 0, sizeof(phi_analysis));
	memset(&validation, 0, sizeof(validation));
	validation.diagnostic = ZEND_MIRL_W04_MALFORMED_CFG;
	if (map != NULL) {
		memset(map, 0, sizeof(*map));
	}
	if (context == NULL || map == NULL || context->busy
			|| context->registry == NULL || !context->registry->complete) {
		return zend_mir_w04_result(ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_W04_MALFORMED_CFG);
	}
	if (!(context->shape.has_try_regions
			? zend_mir_w04_validate_source_for_protected_control_flow(
				context->source, &validation)
			: zend_mir_w04_validate_source(context->source, &validation))) {
		return zend_mir_w04_result(
			ZEND_MIR_LOWERING_REJECTED, validation.diagnostic);
	}
	if (!zend_mir_w04_phi_analysis_init(context, &phi_analysis)) {
		return zend_mir_w04_result(ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_MUTATION_FAILED);
	}
	if (!zend_mir_w04_validate_scalar_phis(context, &phi_analysis)) {
		zend_mir_w04_phi_analysis_destroy(&phi_analysis);
		return zend_mir_w04_result(ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_W04_UNSUPPORTED_PHI_PI);
	}
	if (!zend_mir_w04_validate_branch_proofs(context, &phi_analysis)) {
		zend_mir_w04_phi_analysis_destroy(&phi_analysis);
		return zend_mir_w04_result(ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_W04_BRANCH_PROOF_FAILED);
	}
	context->busy = true;
	module = context->module_ops.create(
		context->module_ops.context, context->module_id, context->diagnostics);
	if (module == NULL) {
		return zend_mir_w04_abort(context, NULL, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	mutator = context->module_ops.mutator(context->module_ops.context, module);
	if (mutator == NULL || (requested_mutator != NULL && requested_mutator != mutator)
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| !zend_mir_control_flow_map_storage_init(&storage,
				context->source->block_count(context->source->context),
				context->source->edge_count(context->source->context),
				context->source->phi_count(context->source->context))
			|| !mutator->add_function(mutator->context,
				context->function_symbol_id, &context->function_id)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	for (i = 0; i < context->source->block_count(context->source->context); i++) {
		zend_mir_source_block_ref source_block;
		zend_mir_control_flow_block_mapping mapping;
		if (!context->source->block_at(context->source->context, i, &source_block)) {
			return zend_mir_w04_abort(
				context, module, &storage, &phi_analysis,
				ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
		}
		if ((source_block.flags & ZEND_MIR_SOURCE_BLOCK_REACHABLE) == 0) {
			continue;
		}
		if (!mutator->add_block(mutator->context,
				context->function_id, &mapping.mir_block_id)) {
			return zend_mir_w04_abort(
				context, module, &storage, &phi_analysis,
				ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
		}
		mapping.source_block_id = source_block.id;
		if (!zend_mir_control_flow_map_add_block(&storage, &mapping)) {
			return zend_mir_w04_abort(
				context, module, &storage, &phi_analysis,
				ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_W04_SOURCE_MIR_MAPPING_FAILED);
		}
	}
	if (!zend_mir_w04_predeclare_values(context, mutator, &phi_analysis)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	if (!zend_mir_control_flow_map_find_block(&storage.public_map,
			validation.entry_block_id, &context->block_id)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	if (!zend_mir_w04_emit_phis(
			context, mutator, &storage, &phi_analysis)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	if (!mutator->set_entry_block(mutator->context,
			context->function_id, context->block_id)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	if (!zend_mir_w04_lower_blocks(
			context, mutator, &storage, &phi_analysis)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	if (!mutator->seal_function(mutator->context, context->function_id)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	if (!context->module_ops.finalize(context->module_ops.context, module)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_FINALIZE_FAILED);
	}
	view = context->module_ops.view(context->module_ops.context, module);
	if (view == NULL || !context->module_ops.verify_stage1(
			context->module_ops.context, view, context->diagnostics)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_STAGE1_VERIFY_FAILED);
	}
	if (!context->module_ops.verify_stage2(
			context->module_ops.context, view, context->diagnostics)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_STAGE2_VERIFY_FAILED);
	}
	if (!zend_mir_verify_w04_control_flow(
			view, context->source, &storage.public_map, context->diagnostics)) {
		return zend_mir_w04_abort(
			context, module, &storage, &phi_analysis,
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_STAGE3_VERIFY_FAILED);
	}
	context->busy = false;
	context->values_predeclared = false;
	{
		zend_mir_lowering_result result =
			zend_mir_w04_result(ZEND_MIR_LOWERING_SUCCESS, ZEND_MIRL_OK);
		result.guarantees = ZEND_MIR_LOWERING_GUARANTEE_W04_ALL;
		result.module = module;
		/* The mapping is deliberately invalidated immediately after stage 3. */
		zend_mir_w04_phi_analysis_destroy(&phi_analysis);
		zend_mir_control_flow_map_storage_destroy(&storage);
		memset(map, 0, sizeof(*map));
		return result;
	}
}
