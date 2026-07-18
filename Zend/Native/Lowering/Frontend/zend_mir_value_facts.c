#include "zend_mir_zend_source_internal.h"

static zend_mir_scalar_type_mask zend_mir_frontend_exact_scalar_type(
	uint32_t zend_type)
{
	switch (zend_type) {
		case MAY_BE_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case MAY_BE_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case MAY_BE_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

static bool zend_mir_frontend_has_reference_or_pointer_fact(
	const zend_ssa_var *var, const zend_ssa_var_info *info)
{
	const uint32_t pointer_types = MAY_BE_STRING | MAY_BE_ARRAY | MAY_BE_OBJECT
		| MAY_BE_RESOURCE | MAY_BE_REF | MAY_BE_INDIRECT | MAY_BE_CLASS
		| MAY_BE_RC1 | MAY_BE_RCN | MAY_BE_ARRAY_KEY_ANY
		| MAY_BE_ARRAY_OF_ANY | MAY_BE_ARRAY_OF_REF;

	return var->alias != NO_ALIAS || info->guarded_reference
		|| info->indirect_reference || info->ce != NULL || info->is_instanceof
		|| (info->type & pointer_types) != 0;
}

bool zend_mir_frontend_fact_for_ssa(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t ssa_variable_id,
	zend_mir_value_fact_ref *out)
{
	const zend_ssa_var_info *info;
	zend_mir_scalar_type_mask exact_type;
	uint32_t i;
	uint32_t fact_id = 0;

	if (op_array == NULL || ssa == NULL || out == NULL || ssa->var_info == NULL
			|| ssa_variable_id >= (uint32_t) ssa->vars_count) {
		return false;
	}
	info = &ssa->var_info[ssa_variable_id];
	if (zend_mir_frontend_has_reference_or_pointer_fact(
			&ssa->vars[ssa_variable_id], info)) {
		return false;
	}
	exact_type = zend_mir_frontend_exact_scalar_type(info->type);
	if (!zend_mir_scalar_type_is_exact(exact_type)) {
		return false;
	}

	for (i = 0; i < ssa_variable_id; i++) {
		if (!zend_mir_frontend_has_reference_or_pointer_fact(
				&ssa->vars[i], &ssa->var_info[i])
				&& zend_mir_scalar_type_is_exact(
					zend_mir_frontend_exact_scalar_type(
						ssa->var_info[i].type))) {
			fact_id++;
		}
	}

	out->id = fact_id;
	out->value_id = zend_mir_value_from_original_ssa(ssa_variable_id);
	out->exact_type = exact_type;
	out->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	out->integer_min = 0;
	out->integer_max = 0;
	out->provenance = ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS;
	out->provenance_source_position_id =
		ssa->vars[ssa_variable_id].definition >= 0
		? (uint32_t) ssa->vars[ssa_variable_id].definition
		: (op_array->last == 0 ? ZEND_MIR_ID_INVALID : 0);

	if (exact_type == ZEND_MIR_SCALAR_TYPE_I64 && info->has_range
			&& !info->range.underflow && !info->range.overflow) {
		if (info->range.min > info->range.max) {
			return false;
		}
		out->flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
		out->integer_min = (int64_t) info->range.min;
		out->integer_max = (int64_t) info->range.max;
		out->provenance = ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS;
		if (info->range.max < 0 || info->range.min > 0) {
			out->flags |= ZEND_MIR_VALUE_FACT_NONZERO;
		}
	}
	return true;
}

zend_mir_lowering_status zend_mir_frontend_validate_facts(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *fact_count)
{
	uint32_t i;
	zend_mir_value_fact_ref fact;

	if (op_array == NULL || ssa == NULL || fact_count == NULL
			|| (ssa->vars_count != 0 && ssa->var_info == NULL)) {
		goto invalid;
	}
	*fact_count = 0;
	for (i = 0; i < (uint32_t) ssa->vars_count; i++) {
		if (zend_mir_frontend_has_reference_or_pointer_fact(
				&ssa->vars[i], &ssa->var_info[i])) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED,
				ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED, op_array_id,
				ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE, i);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (ssa->var_info[i].has_range
				&& !ssa->var_info[i].range.underflow
				&& !ssa->var_info[i].range.overflow
				&& ssa->var_info[i].range.min > ssa->var_info[i].range.max) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_CONTRADICTORY_FACT, op_array_id,
				ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE, i);
			return ZEND_MIR_LOWERING_REJECTED;
		}
		if (zend_mir_frontend_fact_for_ssa(op_array, ssa, i, &fact)) {
			if (*fact_count == ZEND_MIR_ID_MAX) {
				goto invalid;
			}
			(*fact_count)++;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;

invalid:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
		op_array_id, ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
}

bool zend_mir_frontend_value_fact_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_value_fact_ref *out)
{
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	uint32_t i;
	uint32_t current = 0;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->value_fact_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	ssa = zend_mir_source_ssa(source);
	for (i = 0; i < source->ssa_count; i++) {
		if (zend_mir_frontend_fact_for_ssa(op_array, ssa, i, out)) {
			if (current++ == index) {
				return true;
			}
		}
	}
	return false;
}
