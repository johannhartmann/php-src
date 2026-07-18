/*
   +----------------------------------------------------------------------+
   | Zend Engine                                                          |
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   +----------------------------------------------------------------------+
*/

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/zend_compile.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic.h"
#include "Zend/Native/Lowering/Scalar/Numeric/zend_mir_lower_numeric.h"
#include "Zend/Native/Lowering/StraightLine/zend_mir_straight_line.h"

#include "zend_mir_lowering_internal.h"

#define ZEND_MIR_W03_MODULE_ID UINT32_C(0)
#define ZEND_MIR_W03_OP_ARRAY_ID UINT32_C(0)
#define ZEND_MIR_W03_FILE_SYMBOL_ID UINT32_C(0)
#define ZEND_MIR_W03_FUNCTION_SYMBOL_ID UINT32_C(0)
#define ZEND_MIR_W03_CODE_VERSION_ID UINT32_C(1)
#define ZEND_MIR_W03_NOP_FAMILY_ID UINT32_C(1)

typedef struct _zend_mir_w03_integration zend_mir_w03_integration;

struct _zend_mir_w03_integration {
	zend_op_array projected_op_array;
	zend_ssa projected_ssa;
	zend_op *projected_opcodes;
	zval *projected_literals;
	zend_ssa_op *projected_ssa_ops;
	zend_ssa_var *projected_ssa_vars;
	zend_mir_zend_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_value_fact_ref *facts;
	uint32_t fact_count;
	zend_mir_logic_value_binding *logic_bindings;
	uint32_t logic_binding_count;
	zend_mir_logic_opcode_proof *logic_proofs;
	uint32_t logic_proof_count;
	zend_mir_straight_line_value *lifetime_values;
	zend_mir_straight_line_lifetime lifetime;
	zend_mir_straight_line_slot *slots;
	zend_mir_straight_line_entry entry;
	zend_mir_straight_line_provider_context lifetime_context;
	zend_mir_numeric_provider_context numeric_context;
	zend_mir_numeric_provider_set numeric_providers;
	zend_mir_logic_context logic_context;
	zend_mir_lowering_provider logic_provider;
	zend_mir_lowering_provider lifetime_provider;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_context lowering_context;
	zend_mir_lowering_module_ops target_module_ops;
	zend_mir_lowering_module_ops module_ops;
	zend_mir_module *module;
	zend_mir_mutator *target_mutator;
	zend_mir_mutator mutator;
	bool module_seeded;
};

static zend_mir_lowering_result zend_mir_w03_result(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code)
{
	zend_mir_lowering_result result;

	result.status = status;
	result.diagnostic_code = code;
	result.guarantees = 0;
	result.module = NULL;
	return result;
}

static bool zend_mir_w03_checked_add(
	uint32_t left, uint32_t right, uint32_t *out)
{
	if (out == NULL || left > UINT32_MAX - right) {
		return false;
	}
	*out = left + right;
	return true;
}

static void *zend_mir_w03_calloc(uint32_t count, size_t size)
{
	if (count == 0) {
		return NULL;
	}
	if (size != 0 && count > SIZE_MAX / size) {
		return NULL;
	}
	return calloc(count, size);
}

static void zend_mir_w03_clear_ssa_operand(zend_ssa *ssa, int *operand)
{
	if (*operand >= 0 && *operand < ssa->vars_count) {
		ssa->vars[*operand].definition = -1;
	}
	*operand = -1;
}

/*
 * Required arguments are already materialized in the entry frame when W03
 * lowering begins. Project their RECV definitions to NOP/live-in values so
 * the frozen opcode profile remains unchanged and no call semantics enter MIR.
 */
static bool zend_mir_w03_prepare_source(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_op_array **projected_op_array,
	const zend_ssa **projected_ssa)
{
	uint32_t index;
	bool has_recv = false;

	if (integration == NULL || op_array == NULL || ssa == NULL
			|| projected_op_array == NULL || projected_ssa == NULL) {
		return false;
	}
	for (index = 0; index < op_array->last; index++) {
		if (op_array->opcodes[index].opcode == ZEND_RECV) {
			has_recv = true;
			break;
		}
	}
	if (!has_recv) {
		*projected_op_array = op_array;
		*projected_ssa = ssa;
		return true;
	}
	if ((op_array->last != 0 && (op_array->opcodes == NULL || ssa->ops == NULL))
			|| (op_array->last_literal != 0 && op_array->literals == NULL)
			|| ssa->vars_count < 0
			|| (ssa->vars_count != 0 && ssa->vars == NULL)) {
		return false;
	}
	{
		size_t opcode_bytes =
			(size_t) op_array->last * sizeof(*integration->projected_opcodes);
		size_t aligned_opcode_bytes;
		size_t literal_bytes =
			(size_t) op_array->last_literal * sizeof(*integration->projected_literals);
		size_t storage_bytes;

		if (opcode_bytes > SIZE_MAX - 15) {
			return false;
		}
		aligned_opcode_bytes = (opcode_bytes + 15) & ~(size_t) 15;
		if (aligned_opcode_bytes > SIZE_MAX - literal_bytes) {
			return false;
		}
		storage_bytes = aligned_opcode_bytes + literal_bytes;
		integration->projected_opcodes =
			calloc(1, storage_bytes == 0 ? 1 : storage_bytes);
		if (integration->projected_opcodes != NULL
				&& op_array->last_literal != 0) {
			integration->projected_literals = (zval *) (
				(char *) integration->projected_opcodes
				+ aligned_opcode_bytes);
		}
	}
	integration->projected_ssa_ops = zend_mir_w03_calloc(
		op_array->last, sizeof(*integration->projected_ssa_ops));
	integration->projected_ssa_vars = zend_mir_w03_calloc(
		(uint32_t) ssa->vars_count, sizeof(*integration->projected_ssa_vars));
	if (integration->projected_opcodes == NULL
			|| integration->projected_ssa_ops == NULL
			|| (ssa->vars_count != 0
				&& integration->projected_ssa_vars == NULL)) {
		return false;
	}
	memcpy(
		integration->projected_opcodes, op_array->opcodes,
		(size_t) op_array->last * sizeof(*integration->projected_opcodes));
	memcpy(
		integration->projected_ssa_ops, ssa->ops,
		(size_t) op_array->last * sizeof(*integration->projected_ssa_ops));
	if (ssa->vars_count != 0) {
		memcpy(
			integration->projected_ssa_vars, ssa->vars,
			(size_t) ssa->vars_count * sizeof(*integration->projected_ssa_vars));
	}
	integration->projected_op_array = *op_array;
	integration->projected_op_array.opcodes = integration->projected_opcodes;
	integration->projected_op_array.literals = integration->projected_literals;
	if (op_array->last_literal != 0) {
		memcpy(
			integration->projected_literals, op_array->literals,
			(size_t) op_array->last_literal
				* sizeof(*integration->projected_literals));
	}
	integration->projected_ssa = *ssa;
	integration->projected_ssa.ops = integration->projected_ssa_ops;
	integration->projected_ssa.vars = integration->projected_ssa_vars;

	for (index = 0; index < op_array->last; index++) {
		const zend_op *original_opline = &op_array->opcodes[index];
		zend_op *opline = &integration->projected_opcodes[index];
		zend_ssa_op *ssa_op = &integration->projected_ssa_ops[index];
		ptrdiff_t literal_index;
		uint32_t lineno;

		if (opline->op1_type == IS_CONST) {
			literal_index =
				RT_CONSTANT(original_opline, original_opline->op1)
				- op_array->literals;
			if (literal_index < 0
					|| (uint32_t) literal_index >= op_array->last_literal) {
				return false;
			}
#if ZEND_USE_ABS_CONST_ADDR
			opline->op1.zv =
				&integration->projected_literals[literal_index];
#else
			opline->op1.constant = (uint32_t) (
				(char *) &integration->projected_literals[literal_index]
				- (char *) opline);
#endif
		}
		if (opline->op2_type == IS_CONST) {
			literal_index =
				RT_CONSTANT(original_opline, original_opline->op2)
				- op_array->literals;
			if (literal_index < 0
					|| (uint32_t) literal_index >= op_array->last_literal) {
				return false;
			}
#if ZEND_USE_ABS_CONST_ADDR
			opline->op2.zv =
				&integration->projected_literals[literal_index];
#else
			opline->op2.constant = (uint32_t) (
				(char *) &integration->projected_literals[literal_index]
				- (char *) opline);
#endif
		}
		if (opline->opcode != ZEND_RECV) {
			continue;
		}
		zend_mir_w03_clear_ssa_operand(
			&integration->projected_ssa, &ssa_op->op1_def);
		zend_mir_w03_clear_ssa_operand(
			&integration->projected_ssa, &ssa_op->op2_def);
		zend_mir_w03_clear_ssa_operand(
			&integration->projected_ssa, &ssa_op->result_def);
		lineno = opline->lineno;
		memset(opline, 0, sizeof(*opline));
		opline->opcode = ZEND_NOP;
		opline->lineno = lineno;
		ssa_op->op1_use = -1;
		ssa_op->op2_use = -1;
		ssa_op->result_use = -1;
		ssa_op->op1_def = -1;
		ssa_op->op2_def = -1;
		ssa_op->result_def = -1;
		ssa_op->op1_use_chain = -1;
		ssa_op->op2_use_chain = -1;
		ssa_op->res_use_chain = -1;
	}
	*projected_op_array = &integration->projected_op_array;
	*projected_ssa = &integration->projected_ssa;
	return true;
}

static const zend_mir_value_fact_ref *zend_mir_w03_fact_for_value(
	const zend_mir_w03_integration *integration,
	zend_mir_value_id value_id)
{
	uint32_t index;

	if (integration == NULL) {
		return NULL;
	}
	for (index = 0; index < integration->fact_count; index++) {
		if (integration->facts[index].value_id == value_id) {
			return &integration->facts[index];
		}
	}
	return NULL;
}

static bool zend_mir_w03_source_position(
	const void *context, zend_mir_source_position_id requested_id,
	zend_mir_source_position_ref *out)
{
	const zend_mir_w03_integration *integration = context;

	return integration != NULL
		&& zend_mir_zend_source_position_at(
			&integration->source, requested_id, out);
}

static bool zend_mir_w03_resolve_operand(
	const void *context, const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out)
{
	const zend_mir_w03_integration *integration = context;
	zend_mir_frame_slot_kind slot_kind;
	uint32_t index;

	if (integration == NULL || operand == NULL || value_id_out == NULL) {
		return false;
	}
	switch (operand->kind) {
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			*value_id_out = zend_mir_value_from_synthetic(operand->index);
			return zend_mir_id_is_valid(*value_id_out);
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			*value_id_out = zend_mir_value_from_original_ssa(
				operand->ssa_variable_id);
			return zend_mir_id_is_valid(*value_id_out);
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
			switch (operand->slot_kind) {
				case ZEND_MIR_SOURCE_SLOT_CV:
					slot_kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
					break;
				case ZEND_MIR_SOURCE_SLOT_TMP:
					slot_kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
					break;
				case ZEND_MIR_SOURCE_SLOT_VAR:
					slot_kind = ZEND_MIR_FRAME_SLOT_KIND_VAR;
					break;
				default:
					return false;
			}
			for (index = 0; index < integration->entry.slot_count; index++) {
				const zend_mir_straight_line_slot *slot =
					&integration->entry.slots[index];

				if (slot->kind == slot_kind
						&& slot->index == operand->index
						&& slot->materialization
							== ZEND_MIR_MATERIALIZATION_MATERIALIZED
						&& zend_mir_id_is_valid(slot->value_id)) {
					*value_id_out = slot->value_id;
					return true;
				}
			}
			return false;
		default:
			return false;
	}
}

static bool zend_mir_w03_value_fact(
	const void *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *fact_out)
{
	const zend_mir_w03_integration *integration = context;
	const zend_mir_value_fact_ref *fact =
		zend_mir_w03_fact_for_value(integration, value_id);

	if (fact == NULL || fact_out == NULL) {
		return false;
	}
	*fact_out = *fact;
	return true;
}

static bool zend_mir_w03_literal_fact(
	const zend_mir_source_literal_ref *literal,
	zend_mir_value_fact_ref *fact,
	zend_mir_constant_record *constant)
{
	double value;

	if (literal == NULL || fact == NULL || constant == NULL) {
		return false;
	}
	memset(fact, 0, sizeof(*fact));
	memset(constant, 0, sizeof(*constant));
	fact->id = ZEND_MIR_ID_INVALID;
	fact->value_id = zend_mir_value_from_synthetic(literal->literal_index);
	fact->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	fact->provenance = ZEND_MIR_FACT_PROVENANCE_LITERAL;
	fact->provenance_source_position_id = 0;
	constant->value_id = fact->value_id;
	constant->symbol_id = ZEND_MIR_ID_INVALID;

	switch (literal->kind) {
		case ZEND_MIR_SOURCE_LITERAL_NULL:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_NULL;
			constant->representation = ZEND_MIR_REPRESENTATION_ZVAL;
			constant->kind = ZEND_MIR_CONSTANT_KIND_NULL_VALUE;
			break;
		case ZEND_MIR_SOURCE_LITERAL_FALSE:
		case ZEND_MIR_SOURCE_LITERAL_TRUE:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_I1;
			constant->representation = ZEND_MIR_REPRESENTATION_I1;
			constant->kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
			constant->payload_bits =
				literal->kind == ZEND_MIR_SOURCE_LITERAL_TRUE ? 1 : 0;
			break;
		case ZEND_MIR_SOURCE_LITERAL_LONG_BITS:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_I64;
			fact->flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
			fact->integer_min = (int64_t) literal->payload_bits;
			fact->integer_max = (int64_t) literal->payload_bits;
			if ((int64_t) literal->payload_bits != 0) {
				fact->flags |= ZEND_MIR_VALUE_FACT_NONZERO;
			}
			constant->representation = ZEND_MIR_REPRESENTATION_I64;
			constant->kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
			constant->payload_bits = literal->payload_bits;
			break;
		case ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS:
			fact->exact_type = ZEND_MIR_SCALAR_TYPE_F64;
			memcpy(&value, &literal->payload_bits, sizeof(value));
			if (isfinite(value)) {
				fact->flags |= ZEND_MIR_VALUE_FACT_FINITE;
			}
			constant->representation = ZEND_MIR_REPRESENTATION_DOUBLE;
			constant->kind = ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS;
			constant->payload_bits = literal->payload_bits;
			break;
		default:
			return false;
	}
	return zend_mir_id_is_valid(fact->value_id);
}

static bool zend_mir_w03_prepare_facts(
	zend_mir_w03_integration *integration)
{
	uint32_t literal_count =
		integration->source_view.literal_count(integration->source_view.context);
	uint32_t ssa_fact_count =
		zend_mir_zend_source_value_fact_count(&integration->source);
	uint32_t capacity;
	uint32_t index;

	if (!zend_mir_w03_checked_add(
			literal_count, ssa_fact_count, &capacity)) {
		return false;
	}
	integration->facts = zend_mir_w03_calloc(
		capacity == 0 ? 1 : capacity, sizeof(*integration->facts));
	if (integration->facts == NULL) {
		return false;
	}
	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;
		zend_mir_constant_record constant;

		if (!integration->source_view.literal_at(
				integration->source_view.context, index, &literal)
				|| !zend_mir_w03_literal_fact(
					&literal, &integration->facts[integration->fact_count],
					&constant)) {
			return false;
		}
		integration->fact_count++;
	}
	for (index = 0; index < ssa_fact_count; index++) {
		if (!zend_mir_zend_source_value_fact_at(
				&integration->source, index,
				&integration->facts[integration->fact_count])) {
			return false;
		}
		integration->fact_count++;
	}
	return true;
}

static bool zend_mir_w03_operand_equal(
	const zend_mir_source_operand_ref *left,
	const zend_mir_source_operand_ref *right)
{
	return left->kind == right->kind
		&& left->slot_kind == right->slot_kind
		&& left->index == right->index
		&& left->ssa_variable_id == right->ssa_variable_id;
}

static bool zend_mir_w03_add_logic_binding(
	zend_mir_w03_integration *integration,
	const zend_mir_source_operand_ref *operand)
{
	zend_mir_logic_value_binding *binding;
	zend_mir_value_id value_id;
	const zend_mir_value_fact_ref *fact;
	uint32_t index;

	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return true;
	}
	for (index = 0; index < integration->logic_binding_count; index++) {
		if (zend_mir_w03_operand_equal(
				&integration->logic_bindings[index].source, operand)) {
			return true;
		}
	}
	if (!zend_mir_w03_resolve_operand(integration, operand, &value_id)) {
		return false;
	}
	fact = zend_mir_w03_fact_for_value(integration, value_id);
	if (fact == NULL) {
		return false;
	}
	binding =
		&integration->logic_bindings[integration->logic_binding_count++];
	memset(binding, 0, sizeof(*binding));
	binding->source = *operand;
	binding->value_id = value_id;
	binding->has_fact = true;
	binding->fact = *fact;
	return true;
}

static bool zend_mir_w03_prepare_logic(
	zend_mir_w03_integration *integration)
{
	uint32_t opcode_count =
		integration->source_view.opcode_count(integration->source_view.context);
	uint32_t binding_capacity;
	uint32_t index;

	if (opcode_count > UINT32_MAX / 3) {
		return false;
	}
	binding_capacity = opcode_count * 3;
	integration->logic_bindings = zend_mir_w03_calloc(
		binding_capacity == 0 ? 1 : binding_capacity,
		sizeof(*integration->logic_bindings));
	integration->logic_proofs = zend_mir_w03_calloc(
		opcode_count == 0 ? 1 : opcode_count,
		sizeof(*integration->logic_proofs));
	if (integration->logic_bindings == NULL
			|| integration->logic_proofs == NULL) {
		return false;
	}
	for (index = 0; index < opcode_count; index++) {
		zend_mir_source_opcode_ref opcode;
		zend_mir_logic_opcode_proof *proof;
		uint32_t temporary_payload;

		if (!integration->source_view.opcode_at(
				integration->source_view.context, index, &opcode)
				|| !zend_mir_w03_add_logic_binding(
					integration, &opcode.op1)
				|| !zend_mir_w03_add_logic_binding(
					integration, &opcode.op2)
				|| !zend_mir_w03_add_logic_binding(
					integration, &opcode.result)
				|| !zend_mir_w03_checked_add(
					integration->source.literal_count,
					opcode_count, &temporary_payload)
				|| !zend_mir_w03_checked_add(
					temporary_payload, index, &temporary_payload)) {
			return false;
		}
		proof = &integration->logic_proofs[integration->logic_proof_count++];
		proof->opline_index = opcode.opline_index;
		proof->proofs = ZEND_MIR_LOGIC_PROOF_ALL;
		proof->temporary_value_id =
			zend_mir_value_from_synthetic(temporary_payload);
		if (!zend_mir_id_is_valid(proof->temporary_value_id)) {
			return false;
		}
	}
	integration->logic_context.bindings = integration->logic_bindings;
	integration->logic_context.binding_count =
		integration->logic_binding_count;
	integration->logic_context.opcode_proofs = integration->logic_proofs;
	integration->logic_context.opcode_proof_count =
		integration->logic_proof_count;
	zend_mir_logic_provider_init(
		&integration->logic_provider, &integration->logic_context);
	return true;
}

static zend_mir_frame_slot_kind zend_mir_w03_slot_kind(
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

static bool zend_mir_w03_is_qm_result(
	const zend_mir_w03_integration *integration,
	zend_mir_value_id value_id)
{
	uint32_t opcode_count =
		integration->source_view.opcode_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < opcode_count; index++) {
		zend_mir_source_opcode_ref opcode;

		if (!integration->source_view.opcode_at(
				integration->source_view.context, index, &opcode)) {
			return false;
		}
		if (opcode.zend_opcode_number == ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN
				&& opcode.result.kind == ZEND_MIR_SOURCE_OPERAND_SSA
				&& zend_mir_value_from_original_ssa(
					opcode.result.ssa_variable_id) == value_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_w03_track_lifetime_values(
	zend_mir_w03_integration *integration)
{
	uint32_t capacity;
	uint32_t index;

	if (!zend_mir_w03_checked_add(
			integration->fact_count,
			integration->source.opcode_count, &capacity)
			|| !zend_mir_w03_checked_add(capacity, 1, &capacity)) {
		return false;
	}
	integration->lifetime_values = zend_mir_w03_calloc(
		capacity, sizeof(*integration->lifetime_values));
	if (integration->lifetime_values == NULL
			|| !zend_mir_straight_line_lifetime_init(
				&integration->lifetime, integration->lifetime_values,
				capacity)) {
		return false;
	}
	for (index = 0; index < integration->fact_count; index++) {
		const zend_mir_value_fact_ref *fact = &integration->facts[index];
		zend_mir_straight_line_value value;

		if (zend_mir_w03_is_qm_result(integration, fact->value_id)) {
			continue;
		}
		memset(&value, 0, sizeof(value));
		value.value_id = fact->value_id;
		value.representation =
			zend_mir_scalar_type_representation(fact->exact_type);
		value.ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
		value.exact_type = fact->exact_type;
		value.fact_flags = fact->flags;
		value.integer_min = fact->integer_min;
		value.integer_max = fact->integer_max;
		if (!zend_mir_straight_line_track_value(
				&integration->lifetime, &value)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w03_initial_slot_value(
	const zend_mir_w03_integration *integration,
	const zend_mir_source_slot_ref *source_slot,
	zend_mir_value_id *value_id_out)
{
	uint32_t ssa_count =
		integration->source_view.ssa_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;

		if (!integration->source_view.ssa_at(
				integration->source_view.context, index, &ssa)) {
			return false;
		}
		if (ssa.definition_opline_index == ZEND_MIR_ID_INVALID
				&& ssa.source_slot_kind == source_slot->kind
				&& ssa.source_slot == source_slot->kind_index) {
			zend_mir_value_id candidate =
				zend_mir_value_from_original_ssa(
				ssa.ssa_variable_id);

			if (zend_mir_id_is_valid(candidate)
					&& zend_mir_w03_fact_for_value(
						integration, candidate) != NULL) {
				*value_id_out = candidate;
				return true;
			}
		}
	}
	return false;
}

static bool zend_mir_w03_prepare_slots(
	zend_mir_w03_integration *integration,
	const zend_op_array *op_array)
{
	uint32_t slot_count =
		zend_mir_zend_source_slot_count(&integration->source);
	uint32_t index;

	integration->slots = zend_mir_w03_calloc(
		slot_count == 0 ? 1 : slot_count, sizeof(*integration->slots));
	if (integration->slots == NULL) {
		return false;
	}
	for (index = 0; index < slot_count; index++) {
		zend_mir_source_slot_ref source_slot;
		zend_mir_straight_line_slot *slot = &integration->slots[index];
		zend_mir_value_id value_id;
		const zend_mir_value_fact_ref *fact;

		if (!zend_mir_zend_source_slot_at(
				&integration->source, index, &source_slot)) {
			return false;
		}
		memset(slot, 0, sizeof(*slot));
		slot->slot_id = source_slot.slot_id;
		slot->index = source_slot.kind_index;
		slot->kind = zend_mir_w03_slot_kind(source_slot.kind);
		slot->value_id = ZEND_MIR_ID_INVALID;
		slot->value_representation = ZEND_MIR_REPRESENTATION_INVALID;
		slot->materialization = ZEND_MIR_MATERIALIZATION_UNDEF;
		slot->ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
		if (slot->kind == ZEND_MIR_FRAME_SLOT_KIND_INVALID) {
			return false;
		}
		if (zend_mir_w03_initial_slot_value(
				integration, &source_slot, &value_id)) {
			fact = zend_mir_w03_fact_for_value(integration, value_id);
			if (fact == NULL) {
				return false;
			}
			slot->value_id = value_id;
			slot->value_representation =
				zend_mir_scalar_type_representation(fact->exact_type);
			slot->materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
		}
	}
	memset(&integration->entry, 0, sizeof(integration->entry));
	integration->entry.function_kind = op_array->function_name == NULL
		? ZEND_MIR_FUNCTION_KIND_MAIN : ZEND_MIR_FUNCTION_KIND_USER;
	integration->entry.op_array_id = ZEND_MIR_W03_OP_ARRAY_ID;
	integration->entry.code_version_id = ZEND_MIR_W03_CODE_VERSION_ID;
	integration->entry.slots = slot_count == 0 ? NULL : integration->slots;
	integration->entry.slot_count = slot_count;
	integration->entry.source_context = integration;
	integration->entry.source_position_at = zend_mir_w03_source_position;
	integration->entry.resolve_operand = zend_mir_w03_resolve_operand;
	return true;
}

static bool zend_mir_w03_prepare_providers(
	zend_mir_w03_integration *integration)
{
	zend_mir_lowering_diagnostic_code diagnostic;
	zend_mir_lowering_provider provider;
	uint32_t index;

	memset(&integration->numeric_context, 0,
		sizeof(integration->numeric_context));
	integration->numeric_context.source = &integration->source_view;
	integration->numeric_context.source_context = integration;
	integration->numeric_context.resolve_operand = zend_mir_w03_resolve_operand;
	integration->numeric_context.value_fact = zend_mir_w03_value_fact;
	integration->numeric_context.source_position = zend_mir_w03_source_position;
	integration->numeric_context.proofs =
		ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK
		| ZEND_MIR_NUMERIC_PROOF_NO_CALLS
		| ZEND_MIR_NUMERIC_PROOF_NO_REENTRY
		| ZEND_MIR_NUMERIC_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_NUMERIC_PROOF_NO_EXCEPTION;
	if (!zend_mir_numeric_provider_set_init(
			&integration->numeric_context,
			&integration->numeric_providers)) {
		return false;
	}

	memset(&integration->lifetime_context, 0,
		sizeof(integration->lifetime_context));
	integration->lifetime_context.source = &integration->source_view;
	integration->lifetime_context.lifetime = &integration->lifetime;
	integration->lifetime_context.entry = &integration->entry;
	integration->lifetime_context.proofs =
		ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY
		| ZEND_MIR_STRAIGHT_LINE_PROOF_EXACT_SCALAR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NOT_BY_REFERENCE
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_OBSERVER
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_EXCEPTION;
	if (!zend_mir_lifetime_provider_init(
			&integration->lifetime_context,
			&integration->lifetime_provider)
			|| !zend_mir_lowering_registry_init(
				&integration->registry, zend_mir_lowering_w03_profile(),
				&diagnostic)
			|| !zend_mir_lowering_register_nop(
				&integration->registry, ZEND_MIR_W03_NOP_FAMILY_ID,
				&diagnostic)) {
		return false;
	}
	for (index = 0;
			index < zend_mir_numeric_provider_count(
				&integration->numeric_providers);
			index++) {
		if (!zend_mir_numeric_provider_at(
				&integration->numeric_providers, index, &provider)
				|| !zend_mir_lowering_registry_register(
					&integration->registry, &provider, &diagnostic)) {
			return false;
		}
	}
	return zend_mir_lowering_registry_register(
			&integration->registry, &integration->logic_provider, &diagnostic)
		&& zend_mir_lowering_registry_register(
			&integration->registry, &integration->lifetime_provider,
			&diagnostic)
		&& zend_mir_lowering_registry_validate(
			&integration->registry, &diagnostic);
}

static bool zend_mir_w03_forward_add_function(
	void *context, zend_mir_symbol_id symbol_id, zend_mir_function_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_function(
		integration->target_mutator->context, symbol_id, out);
}

static bool zend_mir_w03_forward_add_block(
	void *context, zend_mir_function_id function_id, zend_mir_block_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_block(
		integration->target_mutator->context, function_id, out);
}

static bool zend_mir_w03_forward_add_value(
	void *context, zend_mir_value_id requested_id,
	zend_mir_representation representation, zend_mir_ownership_state ownership)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_value(
		integration->target_mutator->context, requested_id,
		representation, ownership);
}

static bool zend_mir_w03_forward_add_constant(
	void *context, const zend_mir_constant_record *constant)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_constant(
		integration->target_mutator->context, constant);
}

static bool zend_mir_w03_forward_add_instruction(
	void *context, const zend_mir_instruction_record *record,
	zend_mir_instruction_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_instruction(
		integration->target_mutator->context, record, out);
}

static bool zend_mir_w03_forward_add_operand(
	void *context, zend_mir_instruction_id instruction_id,
	zend_mir_value_id value_id)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_operand(
		integration->target_mutator->context, instruction_id, value_id);
}

static bool zend_mir_w03_forward_add_edge(
	void *context, zend_mir_block_id from, zend_mir_block_id to)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_edge(
		integration->target_mutator->context, from, to);
}

static bool zend_mir_w03_forward_add_source_position(
	void *context, const zend_mir_source_position_ref *source_position,
	zend_mir_source_position_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_source_position(
		integration->target_mutator->context, source_position, out);
}

static bool zend_mir_w03_forward_add_frame_slot(
	void *context, const zend_mir_frame_slot_ref *slot, uint32_t *index_out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_frame_slot(
		integration->target_mutator->context, slot, index_out);
}

static bool zend_mir_w03_forward_add_root(
	void *context, uint32_t slot_id, uint32_t *index_out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_root(
		integration->target_mutator->context, slot_id, index_out);
}

static bool zend_mir_w03_forward_add_cleanup(
	void *context, const zend_mir_cleanup_ref *cleanup, uint32_t *index_out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_cleanup(
		integration->target_mutator->context, cleanup, index_out);
}

static bool zend_mir_w03_forward_add_frame_state(
	void *context, const zend_mir_frame_state_ref *frame_state,
	zend_mir_frame_state_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_frame_state(
		integration->target_mutator->context, frame_state, out);
}

static bool zend_mir_w03_forward_seal_function(
	void *context, zend_mir_function_id function_id)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->seal_function(
		integration->target_mutator->context, function_id);
}

static bool zend_mir_w03_forward_add_source_map(
	void *context, const zend_mir_source_map_ref *source_map,
	zend_mir_source_map_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_source_map(
		integration->target_mutator->context, source_map, out);
}

static bool zend_mir_w03_forward_add_value_fact(
	void *context, const zend_mir_value_fact_ref *fact,
	zend_mir_value_fact_id *out)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_mutator->add_value_fact(
		integration->target_mutator->context, fact, out);
}

static bool zend_mir_w03_emit_constants(
	zend_mir_w03_integration *integration, zend_mir_block_id block_id)
{
	uint32_t literal_count =
		integration->source_view.literal_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;
		const zend_mir_value_fact_ref *fact;
		zend_mir_instruction_record instruction;
		zend_mir_instruction_id instruction_id;

		if (!integration->source_view.literal_at(
				integration->source_view.context, index, &literal)) {
			return false;
		}
		fact = zend_mir_w03_fact_for_value(
			integration, zend_mir_value_from_synthetic(literal.literal_index));
		if (fact == NULL) {
			return false;
		}
		memset(&instruction, 0, sizeof(instruction));
		instruction.id = ZEND_MIR_ID_INVALID;
		instruction.block_id = block_id;
		instruction.opcode = ZEND_MIR_OPCODE_CONSTANT;
		instruction.representation =
			zend_mir_scalar_type_representation(fact->exact_type);
		instruction.result_id = fact->value_id;
		instruction.frame_state_id = ZEND_MIR_ID_INVALID;
		instruction.source_position_id = 0;
		if (!integration->target_mutator->add_instruction(
				integration->target_mutator->context, &instruction,
				&instruction_id)) {
			return false;
		}
	}
	return true;
}

static bool zend_mir_w03_set_entry_block(
	void *context, zend_mir_function_id function_id,
	zend_mir_block_id block_id)
{
	zend_mir_w03_integration *integration = context;
	zend_mir_source_opcode_ref first_opcode;
	zend_mir_lowering_diagnostic_code diagnostic;

	if (!integration->target_mutator->set_entry_block(
			integration->target_mutator->context, function_id, block_id)
			|| !integration->source_view.opcode_at(
				integration->source_view.context, 0, &first_opcode)
			|| zend_mir_lower_entry_state(
				&integration->lowering_context, &first_opcode,
				&integration->mutator, &integration->lifetime_context,
				&diagnostic) != ZEND_MIR_LOWERING_SUCCESS
			|| !zend_mir_w03_emit_constants(integration, block_id)) {
		return false;
	}
	return true;
}

static void zend_mir_w03_init_mutator(
	zend_mir_w03_integration *integration,
	zend_mir_mutator *target_mutator)
{
	memset(&integration->mutator, 0, sizeof(integration->mutator));
	integration->target_mutator = target_mutator;
	integration->mutator.contract_version = target_mutator->contract_version;
	integration->mutator.context = integration;
	integration->mutator.diagnostics = target_mutator->diagnostics;
	integration->mutator.add_function = zend_mir_w03_forward_add_function;
	integration->mutator.add_block = zend_mir_w03_forward_add_block;
	integration->mutator.set_entry_block = zend_mir_w03_set_entry_block;
	integration->mutator.add_value = zend_mir_w03_forward_add_value;
	integration->mutator.add_constant = zend_mir_w03_forward_add_constant;
	integration->mutator.add_instruction =
		zend_mir_w03_forward_add_instruction;
	integration->mutator.add_operand = zend_mir_w03_forward_add_operand;
	integration->mutator.add_edge = zend_mir_w03_forward_add_edge;
	integration->mutator.add_source_position =
		zend_mir_w03_forward_add_source_position;
	integration->mutator.add_frame_slot =
		zend_mir_w03_forward_add_frame_slot;
	integration->mutator.add_root = zend_mir_w03_forward_add_root;
	integration->mutator.add_cleanup = zend_mir_w03_forward_add_cleanup;
	integration->mutator.add_frame_state =
		zend_mir_w03_forward_add_frame_state;
	integration->mutator.seal_function = zend_mir_w03_forward_seal_function;
	integration->mutator.add_source_map =
		zend_mir_w03_forward_add_source_map;
	integration->mutator.add_value_fact =
		zend_mir_w03_forward_add_value_fact;
}

static bool zend_mir_w03_seed_module(zend_mir_w03_integration *integration)
{
	uint32_t source_count =
		zend_mir_zend_source_position_count(&integration->source);
	uint32_t literal_count =
		integration->source_view.literal_count(integration->source_view.context);
	uint32_t ssa_count =
		integration->source_view.ssa_count(integration->source_view.context);
	uint32_t index;

	for (index = 0; index < source_count; index++) {
		zend_mir_source_position_ref source_position;
		zend_mir_source_position_id source_id;

		if (!zend_mir_zend_source_position_at(
				&integration->source, index, &source_position)
				|| !integration->target_mutator->add_source_position(
					integration->target_mutator->context, &source_position,
					&source_id)
				|| source_id != source_position.id) {
			return false;
		}
	}
	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;
		zend_mir_value_fact_ref fact;
		zend_mir_constant_record constant;
		zend_mir_value_fact_id fact_id;

		if (!integration->source_view.literal_at(
				integration->source_view.context, index, &literal)
				|| !zend_mir_w03_literal_fact(
					&literal, &fact, &constant)
				|| !integration->target_mutator->add_value(
					integration->target_mutator->context, fact.value_id,
					constant.representation, ZEND_MIR_OWNERSHIP_STATE_OWNED)
				|| !integration->target_mutator->add_constant(
					integration->target_mutator->context, &constant)
				|| !integration->target_mutator->add_value_fact(
					integration->target_mutator->context, &fact, &fact_id)) {
			return false;
		}
	}
	for (index = 0; index < ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;
		zend_mir_value_id value_id;
		const zend_mir_value_fact_ref *fact;
		zend_mir_value_fact_id fact_id;

		if (!integration->source_view.ssa_at(
				integration->source_view.context, index, &ssa)) {
			return false;
		}
		if (ssa.definition_opline_index != ZEND_MIR_ID_INVALID) {
			continue;
		}
		value_id = zend_mir_value_from_original_ssa(ssa.ssa_variable_id);
		fact = zend_mir_w03_fact_for_value(integration, value_id);
		if (fact == NULL) {
			continue;
		}
		if (!integration->target_mutator->add_value(
					integration->target_mutator->context, value_id,
					zend_mir_scalar_type_representation(fact->exact_type),
					ZEND_MIR_OWNERSHIP_STATE_OWNED)
				|| !integration->target_mutator->add_value_fact(
					integration->target_mutator->context, fact, &fact_id)) {
			return false;
		}
	}
	return true;
}

static zend_mir_module *zend_mir_w03_module_create(
	void *context, zend_mir_module_id module_id,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration *integration = context;

	integration->module = integration->target_module_ops.create(
		integration->target_module_ops.context, module_id, diagnostics);
	if (integration->module == NULL) {
		return NULL;
	}
	integration->target_mutator = integration->target_module_ops.mutator(
		integration->target_module_ops.context, integration->module);
	if (integration->target_mutator == NULL) {
		return integration->module;
	}
	zend_mir_w03_init_mutator(integration, integration->target_mutator);
	integration->module_seeded = zend_mir_w03_seed_module(integration);
	return integration->module;
}

static void zend_mir_w03_module_destroy(
	void *context, zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	integration->target_module_ops.destroy(
		integration->target_module_ops.context, module);
	integration->module = NULL;
	integration->target_mutator = NULL;
	integration->module_seeded = false;
}

static zend_mir_mutator *zend_mir_w03_module_mutator(
	void *context, zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	return module == integration->module && integration->target_mutator != NULL
			&& integration->module_seeded
		? &integration->mutator : NULL;
}

static const zend_mir_view *zend_mir_w03_module_view(
	void *context, const zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_module_ops.view(
		integration->target_module_ops.context, module);
}

static bool zend_mir_w03_module_finalize(
	void *context, zend_mir_module *module)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_module_ops.finalize(
		integration->target_module_ops.context, module);
}

static bool zend_mir_w03_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_module_ops.verify_stage1(
		integration->target_module_ops.context, view, diagnostics);
}

static bool zend_mir_w03_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration *integration = context;

	return integration->target_module_ops.verify_stage2(
		integration->target_module_ops.context, view, diagnostics);
}

static void zend_mir_w03_prepare_module_ops(
	zend_mir_w03_integration *integration,
	const zend_mir_lowering_module_ops *target)
{
	integration->target_module_ops = *target;
	memset(&integration->module_ops, 0, sizeof(integration->module_ops));
	integration->module_ops.context = integration;
	integration->module_ops.create = zend_mir_w03_module_create;
	integration->module_ops.destroy = zend_mir_w03_module_destroy;
	integration->module_ops.mutator = zend_mir_w03_module_mutator;
	integration->module_ops.view = zend_mir_w03_module_view;
	integration->module_ops.finalize = zend_mir_w03_module_finalize;
	integration->module_ops.verify_stage1 = zend_mir_w03_verify_stage1;
	integration->module_ops.verify_stage2 = zend_mir_w03_verify_stage2;
}

static void zend_mir_w03_release(zend_mir_w03_integration *integration)
{
	free(integration->slots);
	free(integration->lifetime_values);
	free(integration->logic_proofs);
	free(integration->logic_bindings);
	free(integration->facts);
	free(integration->projected_ssa_vars);
	free(integration->projected_ssa_ops);
	free(integration->projected_opcodes);
}

zend_mir_lowering_result zend_mir_lower_w03_zend_source(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_mir_w03_integration integration;
	zend_mir_frontend_diagnostic frontend_diagnostic;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_status status;
	zend_mir_lowering_result result;
	const zend_op_array *source_op_array;
	const zend_ssa *source_ssa;

	memset(&integration, 0, sizeof(integration));
	memset(&frontend_diagnostic, 0, sizeof(frontend_diagnostic));
	if (module_ops == NULL) {
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	if (!zend_mir_w03_prepare_source(
			&integration, op_array, ssa, &source_op_array, &source_ssa)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	status = zend_mir_zend_source_init(
		&integration.source, source_op_array, source_ssa,
		ZEND_MIR_W03_OP_ARRAY_ID,
		ZEND_MIR_W03_FILE_SYMBOL_ID, &frontend_diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(status, frontend_diagnostic.code);
	}
	if (!zend_mir_zend_source_view(
			&integration.source, &integration.source_view)
			|| !zend_mir_w03_prepare_facts(&integration)
			|| !zend_mir_w03_track_lifetime_values(&integration)
			|| !zend_mir_w03_prepare_slots(&integration, source_op_array)
			|| !zend_mir_w03_prepare_logic(&integration)
			|| !zend_mir_w03_prepare_providers(&integration)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_MUTATION_FAILED);
	}
	zend_mir_w03_prepare_module_ops(&integration, module_ops);
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 1;
	shape.ssa_complete = true;
	if (!zend_mir_lowering_context_init(
			&integration.lowering_context, &integration.source_view, &shape,
			&integration.registry, &integration.module_ops, diagnostics,
			ZEND_MIR_W03_MODULE_ID, ZEND_MIR_W03_FUNCTION_SYMBOL_ID, NULL)) {
		zend_mir_w03_release(&integration);
		return zend_mir_w03_result(
			ZEND_MIR_LOWERING_FAILED, ZEND_MIRL_INVALID_SOURCE);
	}
	result = zend_mir_lower_source(&integration.lowering_context, NULL);
	zend_mir_w03_release(&integration);
	return result;
}
