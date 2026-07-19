#include "zend_mir_zend_source_internal.h"

static bool zend_mir_frontend_total_physical_slots(
	const zend_op_array *op_array, uint32_t *out)
{
	uint32_t cv_count;

	if (op_array == NULL || out == NULL || op_array->last_var < 0) {
		return false;
	}
	cv_count = (uint32_t) op_array->last_var;
	if (cv_count > ZEND_MIR_ID_MAX - op_array->T) {
		return false;
	}
	*out = cv_count + op_array->T;
	return true;
}

static bool zend_mir_frontend_dead_ssa_has_physical_slot(
	const zend_op_array *op_array, const zend_ssa *ssa, uint32_t index)
{
	uint32_t physical_count;
	const zend_ssa_var *variable;

	if (op_array == NULL || ssa == NULL
			|| index >= (uint32_t) ssa->vars_count
			|| !zend_mir_frontend_total_physical_slots(
				op_array, &physical_count)) {
		return false;
	}
	variable = &ssa->vars[index];
	return variable->var >= 0
		&& (uint32_t) variable->var < physical_count
		&& variable->definition == -1
		&& variable->definition_phi == NULL
		&& variable->use_chain == -1
		&& variable->phi_use_chain == NULL
		&& variable->sym_use_chain == NULL;
}

bool zend_mir_frontend_decode_slot(
	const zend_op_array *op_array,
	const znode_op *node,
	uint8_t operand_type,
	uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	uint32_t physical_count;
	uint32_t frame_slots;
	uint32_t encoded_slots;
	uint32_t physical_slot;
	uint32_t cv_count;

	if (node == NULL || slot == NULL || slot_kind == NULL
			|| !zend_mir_frontend_total_physical_slots(
				op_array, &physical_count)
			|| node->var % sizeof(zval) != 0) {
		return false;
	}

	frame_slots = (uint32_t) ZEND_CALL_FRAME_SLOT;
	encoded_slots = node->var / (uint32_t) sizeof(zval);
	if (encoded_slots < frame_slots) {
		return false;
	}
	physical_slot = encoded_slots - frame_slots;
	if (physical_slot >= physical_count) {
		return false;
	}

	cv_count = (uint32_t) op_array->last_var;
	switch (operand_type) {
		case IS_CV:
			if (physical_slot >= cv_count) {
				return false;
			}
			*slot = physical_slot;
			*slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
			return true;
		case IS_TMP_VAR:
			if (physical_slot < cv_count) {
				return false;
			}
			*slot = physical_slot - cv_count;
			*slot_kind = ZEND_MIR_SOURCE_SLOT_TMP;
			return true;
		case IS_VAR:
			if (physical_slot < cv_count) {
				return false;
			}
			*slot = physical_slot - cv_count;
			*slot_kind = ZEND_MIR_SOURCE_SLOT_VAR;
			return true;
		default:
			return false;
	}
}

static bool zend_mir_frontend_match_ssa_operand(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t opline_index,
	uint32_t operand_index,
	uint32_t ssa_variable_id,
	uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	const zend_op *opline;
	const zend_ssa_op *ssa_op;
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;

	opline = &op_array->opcodes[opline_index];
	ssa_op = &ssa->ops[opline_index];
		switch (operand_index) {
			case ZEND_MIR_FRONTEND_OP1:
				node = &opline->op1;
				if (!zend_mir_frontend_normalize_operand_type(
						opline->op1_type, operand_index, &operand_type)) {
					return false;
				}
				use = ssa_op->op1_use;
				def = ssa_op->op1_def;
				break;
			case ZEND_MIR_FRONTEND_OP2:
				node = &opline->op2;
				if (!zend_mir_frontend_normalize_operand_type(
						opline->op2_type, operand_index, &operand_type)) {
					return false;
				}
				use = ssa_op->op2_use;
				def = ssa_op->op2_def;
				break;
			case ZEND_MIR_FRONTEND_RESULT:
				node = &opline->result;
				if (!zend_mir_frontend_normalize_operand_type(
						opline->result_type, operand_index, &operand_type)) {
					return false;
				}
				use = ssa_op->result_use;
				def = ssa_op->result_def;
				break;
		default:
			return false;
	}
	if ((use < 0 || (uint32_t) use != ssa_variable_id)
			&& (def < 0 || (uint32_t) def != ssa_variable_id)) {
		return false;
	}
	return zend_mir_frontend_decode_slot(
		op_array, node, operand_type, slot, slot_kind);
}

bool zend_mir_frontend_ssa_slot(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t ssa_variable_id,
	uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	uint32_t physical_count;
	uint32_t physical_slot;
	uint32_t opline_index;
	uint32_t operand_index;
	uint32_t candidate_slot;
	zend_mir_source_slot_kind candidate_kind;
	bool found = false;

	if (op_array == NULL || ssa == NULL || slot == NULL || slot_kind == NULL
			|| ssa_variable_id >= (uint32_t) ssa->vars_count
			|| !zend_mir_frontend_total_physical_slots(
				op_array, &physical_count)
			|| ssa->vars[ssa_variable_id].var < 0) {
		return false;
	}
	physical_slot = (uint32_t) ssa->vars[ssa_variable_id].var;
	if (physical_slot >= physical_count) {
		return false;
	}
	if (physical_slot < (uint32_t) op_array->last_var) {
		*slot = physical_slot;
		*slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
		return true;
	}

	for (opline_index = 0; opline_index < op_array->last; opline_index++) {
		for (operand_index = 0; operand_index < 3; operand_index++) {
			if (!zend_mir_frontend_match_ssa_operand(
					op_array, ssa, opline_index, operand_index,
					ssa_variable_id, &candidate_slot, &candidate_kind)) {
				continue;
			}
			if (candidate_slot != physical_slot - (uint32_t) op_array->last_var
					|| (found && candidate_kind != *slot_kind)) {
				return false;
			}
			*slot = candidate_slot;
			*slot_kind = candidate_kind;
			found = true;
		}
	}
	return found;
}

zend_mir_lowering_status zend_mir_frontend_validate_slots(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *slot_count)
{
	uint32_t cv_count;
	uint32_t i;
	uint32_t ignored_slot;
	zend_mir_source_slot_kind ignored_kind;

	if (slot_count == NULL || op_array == NULL || ssa == NULL
			|| op_array->last_var < 0) {
		goto invalid;
	}
	cv_count = (uint32_t) op_array->last_var;
	if (op_array->T > (ZEND_MIR_ID_MAX - cv_count) / 2) {
		goto invalid;
	}
	*slot_count = cv_count + op_array->T * 2;
	for (i = 0; i < (uint32_t) ssa->vars_count; i++) {
		if (!zend_mir_frontend_ssa_slot(
				op_array, ssa, i, &ignored_slot, &ignored_kind)
				&& !zend_mir_frontend_dead_ssa_has_physical_slot(
					op_array, ssa, i)) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
				ZEND_MIR_FRONTEND_OPERAND_NONE, i);
			return ZEND_MIR_LOWERING_REJECTED;
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

bool zend_mir_frontend_slot_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_slot_ref *out)
{
	const zend_op_array *op_array;
	uint32_t cv_count;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->slot_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	cv_count = (uint32_t) op_array->last_var;
	out->slot_id = index;
	if (index < cv_count) {
		out->kind = ZEND_MIR_SOURCE_SLOT_CV;
		out->kind_index = index;
	} else if (index < cv_count + op_array->T) {
		out->kind = ZEND_MIR_SOURCE_SLOT_TMP;
		out->kind_index = index - cv_count;
	} else {
		out->kind = ZEND_MIR_SOURCE_SLOT_VAR;
		out->kind_index = index - cv_count - op_array->T;
	}
	return true;
}
