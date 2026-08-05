#include "zend_mir_zend_source_internal.h"

typedef struct _zend_mir_frontend_slot_index_entry {
	uint32_t slot;
	zend_mir_source_slot_kind kind;
	bool valid;
	bool seen;
} zend_mir_frontend_slot_index_entry;

typedef struct _zend_mir_frontend_slot_index {
	uint32_t ssa_count;
	uint32_t physical_count;
	zend_mir_frontend_slot_index_entry *entries;
	uint32_t *peer_by_physical_slot;
} zend_mir_frontend_slot_index;

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

static bool zend_mir_frontend_slot_operand_parts(
	const zend_op_array *op_array, const zend_ssa *ssa,
	uint32_t opline_index,
	uint32_t operand_index,
	const znode_op **node, uint8_t *operand_type, int *use, int *def)
{
	const zend_op *opline;
	const zend_ssa_op *ssa_op;

	opline = &op_array->opcodes[opline_index];
	ssa_op = &ssa->ops[opline_index];
	switch (operand_index) {
			case ZEND_MIR_FRONTEND_OP1:
				*node = &opline->op1;
				if (!zend_mir_frontend_normalize_operand_type(
						opline->op1_type, operand_index, operand_type)) {
					return false;
				}
				*use = ssa_op->op1_use;
				*def = ssa_op->op1_def;
				break;
			case ZEND_MIR_FRONTEND_OP2:
				*node = &opline->op2;
				if (!zend_mir_frontend_normalize_operand_type(
						opline->op2_type, operand_index, operand_type)) {
					return false;
				}
				*use = ssa_op->op2_use;
				*def = ssa_op->op2_def;
				break;
			case ZEND_MIR_FRONTEND_RESULT:
				*node = &opline->result;
				if (!zend_mir_frontend_normalize_operand_type(
						opline->result_type, operand_index, operand_type)) {
					return false;
				}
				*use = ssa_op->result_use;
				*def = ssa_op->result_def;
				break;
		default:
			return false;
	}
	return true;
}

static bool zend_mir_frontend_match_ssa_operand(
	const zend_op_array *op_array, const zend_ssa *ssa,
	uint32_t opline_index, uint32_t operand_index,
	uint32_t ssa_variable_id, uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;

	if (!zend_mir_frontend_slot_operand_parts(
			op_array, ssa, opline_index, operand_index,
			&node, &operand_type, &use, &def)) {
		return false;
	}
	if ((use < 0 || (uint32_t) use != ssa_variable_id)
			&& (def < 0 || (uint32_t) def != ssa_variable_id)) {
		return false;
	}
	return zend_mir_frontend_decode_slot(
		op_array, node, operand_type, slot, slot_kind);
}

static bool zend_mir_frontend_index_slot_occurrence(
	zend_mir_frontend_slot_index *index, const zend_op_array *op_array,
	const zend_ssa *ssa, int ssa_id, const znode_op *node,
	uint8_t operand_type)
{
	zend_mir_frontend_slot_index_entry *entry;
	uint32_t slot;
	uint32_t physical_slot;
	zend_mir_source_slot_kind kind;

	if (ssa_id < 0) {
		return true;
	}
	if ((uint32_t) ssa_id >= index->ssa_count
			|| !zend_mir_frontend_decode_slot(
				op_array, node, operand_type, &slot, &kind)) {
		return false;
	}
	entry = &index->entries[ssa_id];
	physical_slot = kind == ZEND_MIR_SOURCE_SLOT_CV
		? slot : (uint32_t) op_array->last_var + slot;
	if (ssa->vars[ssa_id].var < 0
			|| (uint32_t) ssa->vars[ssa_id].var != physical_slot
			|| (entry->seen && (entry->slot != slot || entry->kind != kind))) {
		return false;
	}
	entry->slot = slot;
	entry->kind = kind;
	entry->seen = true;
	entry->valid = true;
	return true;
}

void *zend_mir_frontend_build_slot_index(
	const zend_op_array *op_array, const zend_ssa *ssa)
{
	zend_mir_frontend_slot_index *index;
	const znode_op *node;
	uint32_t physical_count;
	uint32_t i;
	uint32_t operand;
	uint8_t operand_type;
	int use;
	int def;

	if (op_array == NULL || ssa == NULL || ssa->vars_count < 0
			|| (ssa->vars_count != 0 && ssa->vars == NULL)
			|| (op_array->last != 0
				&& (op_array->opcodes == NULL || ssa->ops == NULL))
			|| !zend_mir_frontend_total_physical_slots(
				op_array, &physical_count)) {
		return NULL;
	}
	index = calloc(1, sizeof(*index));
	if (index == NULL) {
		return NULL;
	}
	index->ssa_count = (uint32_t) ssa->vars_count;
	index->physical_count = physical_count;
#if SIZE_MAX <= UINT32_MAX
	if ((size_t) index->ssa_count > SIZE_MAX / sizeof(*index->entries)
			|| (size_t) physical_count
				> SIZE_MAX / sizeof(*index->peer_by_physical_slot)) {
		zend_mir_frontend_release_slot_index(index);
		return NULL;
	}
#endif
	if (index->ssa_count != 0) {
		index->entries = calloc(index->ssa_count, sizeof(*index->entries));
	}
	if (physical_count != 0) {
		index->peer_by_physical_slot = malloc(
			physical_count * sizeof(*index->peer_by_physical_slot));
	}
	if ((index->ssa_count != 0 && index->entries == NULL)
			|| (physical_count != 0 && index->peer_by_physical_slot == NULL)) {
		zend_mir_frontend_release_slot_index(index);
		return NULL;
	}
	for (i = 0; i < physical_count; i++) {
		index->peer_by_physical_slot[i] = ZEND_MIR_ID_INVALID;
	}
	for (i = 0; i < index->ssa_count; i++) {
		if (ssa->vars[i].var < 0
				|| (uint32_t) ssa->vars[i].var >= physical_count) {
			zend_mir_frontend_release_slot_index(index);
			return NULL;
		}
		if ((uint32_t) ssa->vars[i].var < (uint32_t) op_array->last_var) {
			index->entries[i].slot = (uint32_t) ssa->vars[i].var;
			index->entries[i].kind = ZEND_MIR_SOURCE_SLOT_CV;
			index->entries[i].seen = true;
			index->entries[i].valid = true;
		}
	}
	for (i = 0; i < op_array->last; i++) {
		for (operand = 0; operand < 3; operand++) {
			if (!zend_mir_frontend_slot_operand_parts(
					op_array, ssa, i, operand, &node, &operand_type,
					&use, &def)
					|| !zend_mir_frontend_index_slot_occurrence(
						index, op_array, ssa, use, node, operand_type)
					|| (def != use
						&& !zend_mir_frontend_index_slot_occurrence(
							index, op_array, ssa, def, node, operand_type))) {
				zend_mir_frontend_release_slot_index(index);
				return NULL;
			}
		}
	}
	for (i = 0; i < index->ssa_count; i++) {
		uint32_t physical_slot = (uint32_t) ssa->vars[i].var;

		if (index->entries[i].valid
				&& index->peer_by_physical_slot[physical_slot]
					== ZEND_MIR_ID_INVALID) {
			index->peer_by_physical_slot[physical_slot] = i;
		}
	}
	return index;
}

void zend_mir_frontend_release_slot_index(void *opaque_index)
{
	zend_mir_frontend_slot_index *index = opaque_index;

	if (index == NULL) {
		return;
	}
	free(index->peer_by_physical_slot);
	free(index->entries);
	free(index);
}

bool zend_mir_frontend_indexed_ssa_slot(
	const void *opaque_index, uint32_t ssa_variable_id, uint32_t *slot,
	zend_mir_source_slot_kind *slot_kind)
{
	const zend_mir_frontend_slot_index *index = opaque_index;
	const zend_mir_frontend_slot_index_entry *entry;

	if (index == NULL || slot == NULL || slot_kind == NULL
			|| ssa_variable_id >= index->ssa_count) {
		return false;
	}
	entry = &index->entries[ssa_variable_id];
	if (!entry->valid) {
		return false;
	}
	*slot = entry->slot;
	*slot_kind = entry->kind;
	return true;
}

bool zend_mir_frontend_indexed_dead_ssa_peer_slot(
	const void *opaque_index, const zend_ssa *ssa, uint32_t ssa_variable_id,
	uint32_t *slot, zend_mir_source_slot_kind *slot_kind)
{
	const zend_mir_frontend_slot_index *index = opaque_index;
	const zend_ssa_var *variable;
	uint32_t peer;

	if (index == NULL || ssa == NULL || ssa->vars == NULL
			|| slot == NULL || slot_kind == NULL
			|| ssa_variable_id >= index->ssa_count) {
		return false;
	}
	variable = &ssa->vars[ssa_variable_id];
	if (variable->var < 0 || (uint32_t) variable->var >= index->physical_count
			|| variable->definition != -1 || variable->definition_phi != NULL
			|| variable->use_chain != -1 || variable->phi_use_chain != NULL
			|| variable->sym_use_chain != NULL) {
		return false;
	}
	peer = index->peer_by_physical_slot[variable->var];
	return peer != ZEND_MIR_ID_INVALID && peer != ssa_variable_id
		&& zend_mir_frontend_indexed_ssa_slot(index, peer, slot, slot_kind);
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
	void *index;
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
	index = zend_mir_frontend_build_slot_index(op_array, ssa);
	if (index == NULL) {
		goto invalid;
	}
	for (i = 0; i < (uint32_t) ssa->vars_count; i++) {
		if (!zend_mir_frontend_indexed_ssa_slot(
				index, i, &ignored_slot, &ignored_kind)) {
			zend_mir_frontend_release_slot_index(index);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
				ZEND_MIR_FRONTEND_OPERAND_NONE, i);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	zend_mir_frontend_release_slot_index(index);
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
