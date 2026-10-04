#include "zend_mir_zend_source_internal.h"

typedef struct _zend_mir_frontend_operand_lookup {
	uint32_t use_count;
	uint32_t def_count;
	zend_mir_source_ssa_use_ref *uses;
	zend_mir_source_ssa_def_ref *defs;
} zend_mir_frontend_operand_lookup;

bool zend_mir_frontend_normalize_operand_type(
	uint8_t operand_type,
	uint32_t operand_index,
	uint8_t *normalized_type)
{
	uint8_t smart_branch_flags =
		operand_type & (IS_SMART_BRANCH_JMPZ | IS_SMART_BRANCH_JMPNZ);
	uint8_t base_type =
		operand_type & ~(IS_SMART_BRANCH_JMPZ | IS_SMART_BRANCH_JMPNZ);

	if (normalized_type == NULL
			|| (smart_branch_flags != 0
				&& (operand_index != ZEND_MIR_FRONTEND_RESULT
					|| base_type != IS_TMP_VAR
					|| smart_branch_flags
						== (IS_SMART_BRANCH_JMPZ | IS_SMART_BRANCH_JMPNZ)))
			|| (base_type != IS_UNUSED && base_type != IS_CONST
				&& base_type != IS_CV && base_type != IS_TMP_VAR
				&& base_type != IS_VAR)) {
		return false;
	}
	*normalized_type = base_type;
	return true;
}

static bool zend_mir_frontend_operand_parts(
	const zend_op *opline,
	const zend_ssa_op *ssa_op,
	uint32_t operand_index,
	const znode_op **node,
	uint8_t *operand_type,
	int *use,
	int *def)
{
	if (opline == NULL || ssa_op == NULL || node == NULL
			|| operand_type == NULL || use == NULL || def == NULL) {
		return false;
	}
	switch (operand_index) {
		case ZEND_MIR_FRONTEND_OP1:
			*node = &opline->op1;
			if (!zend_mir_frontend_normalize_operand_type(
					opline->op1_type, operand_index, operand_type)) {
				return false;
			}
			*use = ssa_op->op1_use;
			*def = ssa_op->op1_def;
			return true;
		case ZEND_MIR_FRONTEND_OP2:
			*node = &opline->op2;
			if (!zend_mir_frontend_normalize_operand_type(
					opline->op2_type, operand_index, operand_type)) {
				return false;
			}
			*use = ssa_op->op2_use;
			*def = ssa_op->op2_def;
			return true;
		case ZEND_MIR_FRONTEND_RESULT:
			*node = &opline->result;
			if (!zend_mir_frontend_normalize_operand_type(
					opline->result_type, operand_index, operand_type)) {
				return false;
			}
			*use = ssa_op->result_use;
			*def = ssa_op->result_def;
			return true;
		default:
			return false;
	}
}

static bool zend_mir_frontend_operand_ref(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t opline_index,
	uint32_t operand_index,
	int forced_ssa_id,
	zend_mir_source_operand_ref *out)
{
	const zend_op *opline;
	const zend_ssa_op *ssa_op;
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;
	int ssa_id;
	uint32_t literal_index;

	if (op_array == NULL || ssa == NULL || out == NULL
			|| opline_index >= op_array->last) {
		return false;
	}
	opline = &op_array->opcodes[opline_index];
	ssa_op = &ssa->ops[opline_index];
	if (!zend_mir_frontend_operand_parts(
			opline, ssa_op, operand_index, &node, &operand_type, &use, &def)) {
		return false;
	}

	out->kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	out->slot_kind = ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	out->index = ZEND_MIR_ID_INVALID;
	out->ssa_variable_id = ZEND_MIR_ID_INVALID;

	if (operand_type == IS_UNUSED) {
		return true;
	}
	if (operand_type == IS_CONST) {
		if (!zend_mir_frontend_literal_index(
				op_array, opline, node, &literal_index)) {
			return false;
		}
		out->kind = ZEND_MIR_SOURCE_OPERAND_LITERAL;
		out->index = literal_index;
		return true;
	}
	if (!zend_mir_frontend_decode_slot(
			op_array, node, operand_type, &out->index, &out->slot_kind)) {
		return false;
	}

	ssa_id = forced_ssa_id;
	if (ssa_id < 0) {
		ssa_id = operand_index == ZEND_MIR_FRONTEND_RESULT
			? (def >= 0 ? def : use)
			: (use >= 0 ? use : def);
	}
	if (ssa_id >= 0) {
		out->kind = ZEND_MIR_SOURCE_OPERAND_SSA;
		out->ssa_variable_id = (uint32_t) ssa_id;
	} else {
		out->kind = ZEND_MIR_SOURCE_OPERAND_SLOT;
	}
	return true;
}

bool zend_mir_frontend_opcode_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_opcode_ref *out)
{
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	const zend_op *opline;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->opcode_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	ssa = zend_mir_source_ssa(source);
	opline = &op_array->opcodes[index];
	out->opline_index = index;
	out->zend_opcode_number = opline->opcode;
	out->extended_value = opline->extended_value;
	if (source->calls_enabled && source->call_op_array != NULL
			&& index < ((const zend_op_array *) source->call_op_array)->last
			&& ((const zend_op_array *) source->call_op_array)
				->opcodes[index].opcode == ZEND_CATCH) {
		const zend_op *original =
			&((const zend_op_array *) source->call_op_array)->opcodes[index];
		out->zend_opcode_number = ZEND_CATCH;
		out->extended_value = original->extended_value;
	}
	out->source_position_id = index;
	out->block_id = ssa->cfg.map != NULL
		? ssa->cfg.map[index] : 0;
	if (!zend_mir_frontend_operand_ref(
			op_array, ssa, index, ZEND_MIR_FRONTEND_OP1, -1, &out->op1)
		|| !zend_mir_frontend_operand_ref(
			op_array, ssa, index, ZEND_MIR_FRONTEND_OP2, -1, &out->op2)
		|| !zend_mir_frontend_operand_ref(
			op_array, ssa, index, ZEND_MIR_FRONTEND_RESULT, -1, &out->result)) {
		return false;
	}
	if (source->calls_enabled
			&& zend_mir_zend_source_return_source_zval(source, index)) {
		const zend_op_array *original_op_array = source->call_op_array;
		const zend_op *original = &original_op_array->opcodes[index];

		if (original->op1_type == IS_CONST) {
			return true;
		}
		if (!zend_mir_frontend_decode_slot(
				original_op_array, &original->op1, original->op1_type,
				&out->op1.index, &out->op1.slot_kind)) {
			return false;
		}
		out->op1.kind = ZEND_MIR_SOURCE_OPERAND_SLOT;
		out->op1.ssa_variable_id = ZEND_MIR_ID_INVALID;
	}
	return true;
}

bool zend_mir_frontend_ssa_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_ref *out)
{
	const zend_op_array *op_array;
	const zend_ssa *ssa;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->ssa_count) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	ssa = zend_mir_source_ssa(source);
	out->ssa_variable_id = index;
	out->definition_opline_index = ssa->vars[index].definition < 0
		? ZEND_MIR_ID_INVALID : (uint32_t) ssa->vars[index].definition;
	if (source->slot_index != NULL) {
		return zend_mir_frontend_indexed_ssa_slot(
			source->slot_index, index,
			&out->source_slot, &out->source_slot_kind);
	}
	return zend_mir_frontend_ssa_slot(
		op_array, ssa, index, &out->source_slot, &out->source_slot_kind);
}

bool zend_mir_frontend_build_operand_index(zend_mir_zend_source *source)
{
	zend_mir_frontend_operand_lookup *index;
	const zend_op_array *op_array = zend_mir_source_op_array(source);
	const zend_ssa *ssa = zend_mir_source_ssa(source);
	uint32_t i;
	uint32_t operand;
	uint32_t use_index = 0;
	uint32_t def_index = 0;
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;

	if (!zend_mir_source_is_initialized(source) || source->operand_index != NULL) {
		return false;
	}
	index = calloc(1, sizeof(*index));
	if (index == NULL) {
		return false;
	}
	index->use_count = source->ssa_use_count;
	index->def_count = source->ssa_def_count;
	/* Direct sources deliberately publish no operand references even though
	 * their backing SSA still contains uses and definitions. Keep an empty
	 * lookup for them instead of trying to reconcile hidden operands with
	 * zero public counts. */
	if (index->use_count == 0 && index->def_count == 0) {
		source->operand_index = index;
		return true;
	}
#if SIZE_MAX <= UINT32_MAX
	if ((size_t) index->use_count > SIZE_MAX / sizeof(*index->uses)
			|| (size_t) index->def_count > SIZE_MAX / sizeof(*index->defs)) {
		zend_mir_frontend_release_operand_index(index);
		return false;
	}
#endif
	if (index->use_count != 0) {
		index->uses = malloc(index->use_count * sizeof(*index->uses));
	}
	if (index->def_count != 0) {
		index->defs = malloc(index->def_count * sizeof(*index->defs));
	}
	if ((index->use_count != 0 && index->uses == NULL)
			|| (index->def_count != 0 && index->defs == NULL)) {
		zend_mir_frontend_release_operand_index(index);
		return false;
	}
	for (i = 0; i < op_array->last; i++) {
		for (operand = 0; operand < 3; operand++) {
			if (!zend_mir_frontend_operand_parts(
					&op_array->opcodes[i], &ssa->ops[i], operand,
					&node, &operand_type, &use, &def)) {
				goto failed;
			}
			if (use >= 0) {
				if (use_index >= index->use_count) {
					goto failed;
				}
				index->uses[use_index].ssa_variable_id = (uint32_t) use;
				index->uses[use_index].opline_index = i;
				index->uses[use_index].operand_index = operand;
				use_index++;
			}
			if (def >= 0) {
				if (def_index >= index->def_count) {
					goto failed;
				}
				index->defs[def_index].ssa_variable_id = (uint32_t) def;
				index->defs[def_index].opline_index = i;
				if (!zend_mir_frontend_operand_ref(
						op_array, ssa, i, operand, def,
						&index->defs[def_index].destination)) {
					goto failed;
				}
				def_index++;
			}
		}
	}
	if (use_index != index->use_count || def_index != index->def_count) {
		goto failed;
	}
	source->operand_index = index;
	return true;

failed:
	zend_mir_frontend_release_operand_index(index);
	return false;
}

void zend_mir_frontend_release_operand_index(void *opaque_index)
{
	zend_mir_frontend_operand_lookup *index = opaque_index;

	if (index == NULL) {
		return;
	}
	free(index->defs);
	free(index->uses);
	free(index);
}

bool zend_mir_frontend_ssa_use_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_use_ref *out)
{
	const zend_mir_frontend_operand_lookup *operand_index;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->ssa_use_count) {
		return false;
	}
	operand_index = source->operand_index;
	if (operand_index == NULL || index >= operand_index->use_count) {
		return false;
	}
	*out = operand_index->uses[index];
	return true;
}

bool zend_mir_frontend_ssa_def_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_def_ref *out)
{
	const zend_mir_frontend_operand_lookup *operand_index;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->ssa_def_count) {
		return false;
	}
	operand_index = source->operand_index;
	if (operand_index == NULL || index >= operand_index->def_count) {
		return false;
	}
	*out = operand_index->defs[index];
	return true;
}
