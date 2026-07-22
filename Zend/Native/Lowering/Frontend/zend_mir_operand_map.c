#include "zend_mir_zend_source_internal.h"


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

static bool zend_mir_frontend_valid_ssa_id(const zend_ssa *ssa, int id)
{
	return id == -1 || (id >= 0 && id < ssa->vars_count
		&& (uint32_t) id <= ZEND_MIR_VALUE_ORIGINAL_MAX);
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

static bool zend_mir_frontend_validate_operand(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t opline_index,
	uint32_t operand_index,
	uint32_t *use_count,
	uint32_t *def_count)
{
	const zend_op *opline = &op_array->opcodes[opline_index];
	const zend_ssa_op *ssa_op = &ssa->ops[opline_index];
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;
	uint32_t slot;
	uint32_t physical_slot;
	zend_mir_source_slot_kind slot_kind;
	uint32_t literal_index;

	if (!zend_mir_frontend_operand_parts(
			opline, ssa_op, operand_index, &node, &operand_type, &use, &def)
			|| !zend_mir_frontend_valid_ssa_id(ssa, use)
			|| !zend_mir_frontend_valid_ssa_id(ssa, def)) {
		return false;
	}

	if (use >= 0) {
		if (*use_count == ZEND_MIR_ID_MAX) {
			return false;
		}
		(*use_count)++;
	}
	if (def >= 0) {
		if (*def_count == ZEND_MIR_ID_MAX
				|| ssa->vars[def].definition != (int) opline_index) {
			return false;
		}
		(*def_count)++;
	}

	switch (operand_type) {
		case IS_UNUSED:
			return use == -1 && def == -1;
		case IS_CONST:
			return use == -1 && def == -1
				&& operand_index != ZEND_MIR_FRONTEND_RESULT
				&& zend_mir_frontend_literal_index(
					op_array, opline, node, &literal_index);
		case IS_CV:
		case IS_TMP_VAR:
		case IS_VAR:
			if (!zend_mir_frontend_decode_slot(
					op_array, node, operand_type, &slot, &slot_kind)) {
				return false;
			}
			physical_slot = slot_kind == ZEND_MIR_SOURCE_SLOT_CV
				? slot : (uint32_t) op_array->last_var + slot;
			return (use < 0 || (uint32_t) ssa->vars[use].var == physical_slot)
				&& (def < 0 || (uint32_t) ssa->vars[def].var == physical_slot);
		default:
			return false;
	}
}

static bool zend_mir_frontend_definition_is_complete(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t ssa_variable_id)
{
	uint32_t i;
	uint32_t operand_index;
	uint32_t occurrences = 0;
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;

	for (i = 0; i < op_array->last; i++) {
		for (operand_index = 0; operand_index < 3; operand_index++) {
			if (!zend_mir_frontend_operand_parts(
					&op_array->opcodes[i], &ssa->ops[i], operand_index,
					&node, &operand_type, &use, &def)) {
				return false;
			}
			if (def >= 0 && (uint32_t) def == ssa_variable_id) {
				occurrences++;
			}
		}
	}
	return ssa->vars[ssa_variable_id].definition < 0
		? occurrences == 0 : occurrences == 1;
}

static zend_mir_lowering_status zend_mir_frontend_validate_operands_impl(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *use_count,
	uint32_t *def_count,
	bool allow_phi_definitions)
{
	uint32_t i;
	uint32_t operand_index;

	if (op_array == NULL || ssa == NULL || use_count == NULL || def_count == NULL
			|| op_array->last > ZEND_MIR_ID_MAX
			|| (op_array->last != 0
				&& (op_array->opcodes == NULL || ssa->ops == NULL))
			|| ssa->vars_count < 0
			|| (ssa->vars_count != 0 && ssa->vars == NULL)
			|| (uint32_t) ssa->vars_count > ZEND_MIR_VALUE_ORIGINAL_MAX + UINT32_C(1)) {
		goto invalid;
	}
	*use_count = 0;
	*def_count = 0;
	for (i = 0; i < (uint32_t) ssa->vars_count; i++) {
		if (ssa->vars[i].definition < -1
				|| (ssa->vars[i].definition >= 0
					&& (uint32_t) ssa->vars[i].definition >= op_array->last)
				|| (!allow_phi_definitions
					&& ssa->vars[i].definition_phi != NULL)
				|| (ssa->vars[i].definition_phi != NULL
					&& (ssa->vars[i].definition != -1
						|| ssa->vars[i].definition_phi->ssa_var != (int) i))) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
				ZEND_MIR_FRONTEND_OPERAND_NONE, i);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	for (i = 0; i < op_array->last; i++) {
		if (op_array->opcodes[i].opcode > ZEND_VM_LAST_OPCODE) {
			goto invalid_opline;
		}
		for (operand_index = 0; operand_index < 3; operand_index++) {
			if (!zend_mir_frontend_validate_operand(
					op_array, ssa, i, operand_index, use_count, def_count)) {
				goto invalid_operand;
			}
		}
	}
	for (i = 0; i < (uint32_t) ssa->vars_count; i++) {
		if (!zend_mir_frontend_definition_is_complete(op_array, ssa, i)) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
				ZEND_MIR_FRONTEND_OPERAND_NONE, i);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;

invalid_operand:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
		op_array_id, i, operand_index, ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
invalid_opline:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
		op_array_id, i, ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
invalid:
	zend_mir_frontend_set_diagnostic(
		diagnostic, ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
		op_array_id, ZEND_MIR_ID_INVALID, ZEND_MIR_FRONTEND_OPERAND_NONE,
		ZEND_MIR_ID_INVALID);
	return ZEND_MIR_LOWERING_REJECTED;
}

zend_mir_lowering_status zend_mir_frontend_validate_operands(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *use_count,
	uint32_t *def_count)
{
	return zend_mir_frontend_validate_operands_impl(
		op_array, ssa, op_array_id, diagnostic, use_count, def_count, false);
}

zend_mir_lowering_status zend_mir_frontend_validate_operands_w04(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	uint32_t *use_count,
	uint32_t *def_count)
{
	return zend_mir_frontend_validate_operands_impl(
		op_array, ssa, op_array_id, diagnostic, use_count, def_count, true);
}

static bool zend_mir_frontend_opcode_is_supported(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_NOP:
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_MOD:
		case ZEND_SL:
		case ZEND_SR:
		case ZEND_BW_OR:
		case ZEND_BW_AND:
		case ZEND_BW_XOR:
		case ZEND_BW_NOT:
		case ZEND_BOOL_NOT:
		case ZEND_BOOL_XOR:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_QM_ASSIGN:
		case ZEND_CAST:
		case ZEND_BOOL:
		case ZEND_RETURN:
		case ZEND_FREE:
		case ZEND_SPACESHIP:
			return true;
		default:
			return false;
	}
}

static bool zend_mir_frontend_is_scalar_source_type(uint8_t operand_type)
{
	return operand_type == IS_CONST || operand_type == IS_CV
		|| operand_type == IS_TMP_VAR || operand_type == IS_VAR;
}

static bool zend_mir_frontend_is_scalar_result_type(uint8_t operand_type)
{
	uint8_t normalized_type;

	return zend_mir_frontend_normalize_operand_type(
			operand_type, ZEND_MIR_FRONTEND_RESULT, &normalized_type)
		&& (normalized_type == IS_TMP_VAR || normalized_type == IS_VAR);
}

static bool zend_mir_frontend_opcode_operands_match(const zend_op *opline)
{
	switch (opline->opcode) {
		case ZEND_NOP:
			return opline->op1_type == IS_UNUSED
				&& opline->op2_type == IS_UNUSED
				&& opline->result_type == IS_UNUSED;
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_MOD:
		case ZEND_SL:
		case ZEND_SR:
		case ZEND_BW_OR:
		case ZEND_BW_AND:
		case ZEND_BW_XOR:
		case ZEND_BOOL_XOR:
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
		case ZEND_SPACESHIP:
			return zend_mir_frontend_is_scalar_source_type(opline->op1_type)
				&& zend_mir_frontend_is_scalar_source_type(opline->op2_type)
				&& zend_mir_frontend_is_scalar_result_type(
					opline->result_type);
		case ZEND_BW_NOT:
		case ZEND_BOOL_NOT:
		case ZEND_QM_ASSIGN:
		case ZEND_CAST:
		case ZEND_BOOL:
			return zend_mir_frontend_is_scalar_source_type(opline->op1_type)
				&& opline->op2_type == IS_UNUSED
				&& zend_mir_frontend_is_scalar_result_type(
					opline->result_type);
		case ZEND_RETURN:
			return zend_mir_frontend_is_scalar_source_type(opline->op1_type)
				&& opline->op2_type == IS_UNUSED
				&& opline->result_type == IS_UNUSED;
		case ZEND_FREE:
			return (opline->op1_type == IS_TMP_VAR
					|| opline->op1_type == IS_VAR)
				&& opline->op2_type == IS_UNUSED
				&& opline->result_type == IS_UNUSED;
		default:
			return false;
	}
}

static zend_mir_lowering_diagnostic_code zend_mir_frontend_deferred_code(
	uint8_t opcode)
{
	switch (opcode) {
		case ZEND_JMP:
		case ZEND_JMPZ:
		case ZEND_JMPNZ:
		case ZEND_JMPZ_EX:
		case ZEND_JMPNZ_EX:
		case ZEND_RETURN_BY_REF:
		case ZEND_ASSERT_CHECK:
		case ZEND_JMP_SET:
		case ZEND_FAST_CALL:
		case ZEND_FAST_RET:
		case ZEND_COALESCE:
		case ZEND_GENERATOR_RETURN:
		case ZEND_SWITCH_LONG:
		case ZEND_SWITCH_STRING:
		case ZEND_MATCH:
		case ZEND_JMP_NULL:
		case ZEND_BIND_INIT_STATIC_OR_JMP:
		case ZEND_JMP_FRAMELESS:
			return ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED;
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_DO_FCALL:
		case ZEND_INIT_FCALL:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INCLUDE_OR_EVAL:
		case ZEND_EXT_FCALL_BEGIN:
		case ZEND_EXT_FCALL_END:
		case ZEND_TICKS:
		case ZEND_CATCH:
		case ZEND_THROW:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_DO_ICALL:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DECLARE_FUNCTION:
		case ZEND_DECLARE_LAMBDA_FUNCTION:
		case ZEND_DECLARE_CONST:
		case ZEND_DECLARE_CLASS:
		case ZEND_DECLARE_CLASS_DELAYED:
		case ZEND_DECLARE_ANON_CLASS:
		case ZEND_HANDLE_EXCEPTION:
		case ZEND_CALL_TRAMPOLINE:
		case ZEND_DISCARD_EXCEPTION:
		case ZEND_GET_CALLED_CLASS:
		case ZEND_MATCH_ERROR:
		case ZEND_VERIFY_NEVER_TYPE:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_FRAMELESS_ICALL_0:
		case ZEND_FRAMELESS_ICALL_1:
		case ZEND_FRAMELESS_ICALL_2:
		case ZEND_FRAMELESS_ICALL_3:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_DECLARE_ATTRIBUTED_CONST:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
		case ZEND_SEND_PLACEHOLDER:
		case ZEND_GENERATOR_CREATE:
		case ZEND_USER_OPCODE:
		case ZEND_YIELD:
		case ZEND_YIELD_FROM:
			return ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
		case ZEND_DIV:
		case ZEND_CONCAT:
		case ZEND_POW:
		case ZEND_ASSIGN:
		case ZEND_ASSIGN_DIM:
		case ZEND_ASSIGN_OBJ:
		case ZEND_ASSIGN_STATIC_PROP:
		case ZEND_ASSIGN_OP:
		case ZEND_ASSIGN_DIM_OP:
		case ZEND_ASSIGN_OBJ_OP:
		case ZEND_ASSIGN_STATIC_PROP_OP:
		case ZEND_ASSIGN_REF:
		case ZEND_ASSIGN_OBJ_REF:
		case ZEND_ASSIGN_STATIC_PROP_REF:
		case ZEND_PRE_INC:
		case ZEND_PRE_DEC:
		case ZEND_POST_INC:
		case ZEND_POST_DEC:
		case ZEND_PRE_INC_STATIC_PROP:
		case ZEND_PRE_DEC_STATIC_PROP:
		case ZEND_POST_INC_STATIC_PROP:
		case ZEND_POST_DEC_STATIC_PROP:
		case ZEND_CASE:
		case ZEND_CHECK_VAR:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_FAST_CONCAT:
		case ZEND_ROPE_INIT:
		case ZEND_ROPE_ADD:
		case ZEND_ROPE_END:
		case ZEND_BEGIN_SILENCE:
		case ZEND_END_SILENCE:
		case ZEND_RECV:
		case ZEND_RECV_INIT:
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_NEW:
		case ZEND_INIT_ARRAY:
		case ZEND_ADD_ARRAY_ELEMENT:
		case ZEND_UNSET_VAR:
		case ZEND_UNSET_DIM:
		case ZEND_UNSET_OBJ:
		case ZEND_FE_RESET_R:
		case ZEND_FE_FETCH_R:
		case ZEND_FETCH_R:
		case ZEND_FETCH_DIM_R:
		case ZEND_FETCH_OBJ_R:
		case ZEND_FETCH_W:
		case ZEND_FETCH_DIM_W:
		case ZEND_FETCH_OBJ_W:
		case ZEND_FETCH_RW:
		case ZEND_FETCH_DIM_RW:
		case ZEND_FETCH_OBJ_RW:
		case ZEND_FETCH_IS:
		case ZEND_FETCH_DIM_IS:
		case ZEND_FETCH_OBJ_IS:
		case ZEND_FETCH_FUNC_ARG:
		case ZEND_FETCH_DIM_FUNC_ARG:
		case ZEND_FETCH_OBJ_FUNC_ARG:
		case ZEND_FETCH_UNSET:
		case ZEND_FETCH_DIM_UNSET:
		case ZEND_FETCH_OBJ_UNSET:
		case ZEND_FETCH_LIST_R:
		case ZEND_FETCH_CONSTANT:
		case ZEND_CHECK_FUNC_ARG:
		case ZEND_EXT_STMT:
		case ZEND_EXT_NOP:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_FETCH_CLASS:
		case ZEND_CLONE:
		case ZEND_ISSET_ISEMPTY_VAR:
		case ZEND_ISSET_ISEMPTY_DIM_OBJ:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_STRLEN:
		case ZEND_DEFINED:
		case ZEND_TYPE_CHECK:
		case ZEND_VERIFY_RETURN_TYPE:
		case ZEND_FE_RESET_RW:
		case ZEND_FE_FETCH_RW:
		case ZEND_FE_FREE:
		case ZEND_PRE_INC_OBJ:
		case ZEND_PRE_DEC_OBJ:
		case ZEND_POST_INC_OBJ:
		case ZEND_POST_DEC_OBJ:
		case ZEND_ECHO:
		case ZEND_OP_DATA:
		case ZEND_INSTANCEOF:
		case ZEND_MAKE_REF:
		case ZEND_ADD_ARRAY_UNPACK:
		case ZEND_ISSET_ISEMPTY_PROP_OBJ:
		case ZEND_UNSET_CV:
		case ZEND_ISSET_ISEMPTY_CV:
		case ZEND_FETCH_LIST_W:
		case ZEND_SEPARATE:
		case ZEND_FETCH_CLASS_NAME:
		case ZEND_RECV_VARIADIC:
		case ZEND_SEND_UNPACK:
		case ZEND_COPY_TMP:
		case ZEND_BIND_GLOBAL:
		case ZEND_FUNC_NUM_ARGS:
		case ZEND_FUNC_GET_ARGS:
		case ZEND_FETCH_STATIC_PROP_R:
		case ZEND_FETCH_STATIC_PROP_W:
		case ZEND_FETCH_STATIC_PROP_RW:
		case ZEND_FETCH_STATIC_PROP_IS:
		case ZEND_FETCH_STATIC_PROP_FUNC_ARG:
		case ZEND_FETCH_STATIC_PROP_UNSET:
		case ZEND_UNSET_STATIC_PROP:
		case ZEND_ISSET_ISEMPTY_STATIC_PROP:
		case ZEND_FETCH_CLASS_CONSTANT:
		case ZEND_BIND_LEXICAL:
		case ZEND_BIND_STATIC:
		case ZEND_FETCH_THIS:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_ISSET_ISEMPTY_THIS:
		case ZEND_IN_ARRAY:
		case ZEND_COUNT:
		case ZEND_GET_CLASS:
		case ZEND_GET_TYPE:
		case ZEND_ARRAY_KEY_EXISTS:
		case ZEND_CASE_STRICT:
		case ZEND_CHECK_UNDEF_ARGS:
		case ZEND_FETCH_GLOBALS:
		case ZEND_TYPE_ASSERT:
			return ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
		default:
			return ZEND_MIRL_DEFERRED_OPCODE;
	}
}

zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope(
	const zend_op_array *op_array,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	uint32_t i;
	zend_mir_lowering_diagnostic_code code;

	if (op_array == NULL || op_array->last > ZEND_MIR_ID_MAX
			|| (op_array->last != 0 && op_array->opcodes == NULL)) {
		zend_mir_frontend_set_diagnostic(
			diagnostic, ZEND_MIR_LOWERING_REJECTED,
			ZEND_MIRL_INVALID_SOURCE, op_array_id, ZEND_MIR_ID_INVALID,
			ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
		return ZEND_MIR_LOWERING_REJECTED;
	}
	for (i = 0; i < op_array->last; i++) {
		if (op_array->opcodes[i].opcode > ZEND_VM_LAST_OPCODE) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id, i,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_REJECTED;
		}
		if (!zend_mir_frontend_opcode_is_supported(
				op_array->opcodes[i].opcode)) {
			code = zend_mir_frontend_deferred_code(op_array->opcodes[i].opcode);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED, code, op_array_id, i,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (!zend_mir_frontend_opcode_operands_match(&op_array->opcodes[i])) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_INVALID_SOURCE, op_array_id, i,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_diagnostic_code zend_mir_frontend_deferred_code_w04(
	uint8_t opcode)
{
	switch (opcode) {
		case ZEND_ASSERT_CHECK:
		case ZEND_JMP_FRAMELESS:
			return ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
		case ZEND_RETURN_BY_REF:
		case ZEND_JMP_SET:
		case ZEND_COALESCE:
		case ZEND_JMP_NULL:
		case ZEND_BIND_INIT_STATIC_OR_JMP:
			return ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
		default:
			return zend_mir_frontend_deferred_code(opcode);
	}
}

static bool zend_mir_frontend_w04_branch(uint8_t opcode)
{
	return opcode == ZEND_JMP || opcode == ZEND_JMPZ
		|| opcode == ZEND_JMPNZ || opcode == ZEND_JMPZ_EX
		|| opcode == ZEND_JMPNZ_EX || opcode == ZEND_FAST_CALL
		|| opcode == ZEND_FAST_RET;
}

static bool zend_mir_frontend_w09_iterator_branch(uint8_t opcode)
{
	return opcode == ZEND_FE_RESET_R || opcode == ZEND_FE_FETCH_R
		|| opcode == ZEND_FE_RESET_RW || opcode == ZEND_FE_FETCH_RW;
}

static bool zend_mir_frontend_w09_iterator_operands_match(
	const zend_op *opline)
{
	switch (opline->opcode) {
		case ZEND_FE_RESET_R:
			return (opline->op1_type == IS_CONST
					|| opline->op1_type == IS_TMP_VAR
					|| opline->op1_type == IS_CV)
				&& opline->op2_type == IS_UNUSED
				&& opline->result_type == IS_TMP_VAR;
		case ZEND_FE_RESET_RW:
			return (opline->op1_type == IS_CONST
					|| opline->op1_type == IS_TMP_VAR
					|| opline->op1_type == IS_VAR
					|| opline->op1_type == IS_CV)
				&& opline->op2_type == IS_UNUSED
				&& opline->result_type == IS_VAR;
		case ZEND_FE_FETCH_R:
			return opline->op1_type == IS_TMP_VAR
				&& opline->op2_type != IS_UNUSED
				&& (opline->result_type == IS_UNUSED
					|| opline->result_type == IS_TMP_VAR);
		case ZEND_FE_FETCH_RW:
			return opline->op1_type == IS_VAR
				&& opline->op2_type != IS_UNUSED
				&& (opline->result_type == IS_UNUSED
					|| opline->result_type == IS_TMP_VAR);
		default:
			return false;
	}
}

static bool zend_mir_frontend_w04_smart_branch(
	const zend_op_array *op_array, uint32_t opline_index)
{
	const zend_op *producer = &op_array->opcodes[opline_index];
	const zend_op *branch;
	uint8_t smart_branch_flags =
		producer->result_type & (IS_SMART_BRANCH_JMPZ | IS_SMART_BRANCH_JMPNZ);
	uint8_t expected_opcode;

	if (smart_branch_flags == 0) {
		return true;
	}
	if (opline_index + 1 >= op_array->last
			|| (smart_branch_flags
				== (IS_SMART_BRANCH_JMPZ | IS_SMART_BRANCH_JMPNZ))) {
		return false;
	}
	switch (producer->opcode) {
		case ZEND_IS_IDENTICAL:
		case ZEND_IS_NOT_IDENTICAL:
		case ZEND_IS_EQUAL:
		case ZEND_IS_NOT_EQUAL:
		case ZEND_IS_SMALLER:
		case ZEND_IS_SMALLER_OR_EQUAL:
			break;
		default:
			return false;
	}
	expected_opcode = smart_branch_flags == IS_SMART_BRANCH_JMPZ
		? ZEND_JMPZ : ZEND_JMPNZ;
	branch = &op_array->opcodes[opline_index + 1];
	return branch->opcode == expected_opcode
		&& branch->op1_type == IS_TMP_VAR
		&& branch->op1.var == producer->result.var;
}

static zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope_w04_impl(
	const zend_op_array *op_array, zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic, bool allow_iterator_branches)
{
	uint32_t i;
	if (op_array == NULL || op_array->last > ZEND_MIR_ID_MAX
			|| (op_array->last != 0 && op_array->opcodes == NULL)) {
		return ZEND_MIR_LOWERING_REJECTED;
	}
	for (i = 0; i < op_array->last; i++) {
		const zend_op *opline = &op_array->opcodes[i];
		bool valid;
		if (opline->opcode > ZEND_VM_LAST_OPCODE
				|| (!zend_mir_frontend_opcode_is_supported(opline->opcode)
					&& !zend_mir_frontend_w04_branch(opline->opcode)
					&& !(allow_iterator_branches
						&& zend_mir_frontend_w09_iterator_branch(
							opline->opcode)))) {
			zend_mir_frontend_set_diagnostic(diagnostic,
				ZEND_MIR_LOWERING_DEFERRED,
				zend_mir_frontend_deferred_code_w04(opline->opcode),
				op_array_id, i, ZEND_MIR_FRONTEND_OPERAND_NONE,
				ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (!zend_mir_frontend_w04_smart_branch(op_array, i)) {
			zend_mir_frontend_set_diagnostic(diagnostic,
				ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_W04_BRANCH_PROOF_FAILED, op_array_id, i,
				ZEND_MIR_FRONTEND_RESULT, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_REJECTED;
		}
		if (allow_iterator_branches
				&& zend_mir_frontend_w09_iterator_branch(opline->opcode)) {
			valid = zend_mir_frontend_w09_iterator_operands_match(opline);
		} else if (!zend_mir_frontend_w04_branch(opline->opcode)) {
			valid = zend_mir_frontend_opcode_operands_match(opline);
		} else if (opline->opcode == ZEND_JMP) {
			valid = opline->op1_type == IS_UNUSED
				&& opline->op2_type == IS_UNUSED
				&& opline->result_type == IS_UNUSED;
		} else if (opline->opcode == ZEND_FAST_CALL) {
			valid = opline->op1_type == IS_UNUSED
				&& opline->result_type == IS_TMP_VAR;
		} else if (opline->opcode == ZEND_FAST_RET) {
			valid = opline->op1_type == IS_TMP_VAR
				&& opline->result_type == IS_UNUSED;
		} else {
			valid = zend_mir_frontend_is_scalar_source_type(opline->op1_type)
				&& opline->op2_type == IS_UNUSED
				&& ((opline->opcode == ZEND_JMPZ_EX
						|| opline->opcode == ZEND_JMPNZ_EX)
					? zend_mir_frontend_is_scalar_result_type(
						opline->result_type)
					: opline->result_type == IS_UNUSED);
		}
		if (!valid) {
			zend_mir_frontend_set_diagnostic(diagnostic,
				ZEND_MIR_LOWERING_REJECTED,
				ZEND_MIRL_W04_BRANCH_PROOF_FAILED, op_array_id, i,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_REJECTED;
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope_w04(
	const zend_op_array *op_array, zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_validate_opcode_scope_w04_impl(
		op_array, op_array_id, diagnostic, false);
}

zend_mir_lowering_status zend_mir_frontend_validate_opcode_scope_w09(
	const zend_op_array *op_array, zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_validate_opcode_scope_w04_impl(
		op_array, op_array_id, diagnostic, true);
}

static bool zend_mir_frontend_operand_has_exact_scalar(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t opline_index,
	uint32_t operand_index,
	uint32_t *missing_ssa_id)
{
	zend_mir_source_operand_ref operand;
	zend_mir_value_fact_ref fact;
	zend_mir_source_literal_ref literal;

	if (!zend_mir_frontend_operand_ref(
			op_array, ssa, opline_index, operand_index, -1, &operand)) {
		return false;
	}
	switch (operand.kind) {
		case ZEND_MIR_SOURCE_OPERAND_UNUSED:
			return true;
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			return zend_mir_frontend_canonical_literal_for_index(
				op_array, operand.index, &literal);
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			if (!zend_mir_frontend_fact_for_ssa(
					op_array, ssa, operand.ssa_variable_id, &fact)) {
				*missing_ssa_id = operand.ssa_variable_id;
				return false;
			}
			return true;
		default:
			return false;
	}
}

zend_mir_lowering_status zend_mir_frontend_validate_eligibility(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	uint32_t i;
	uint32_t operand_index;
	uint32_t missing_ssa_id;
	zend_mir_lowering_diagnostic_code code;

	for (i = 0; i < op_array->last; i++) {
		if (!zend_mir_frontend_opcode_is_supported(op_array->opcodes[i].opcode)) {
			code = zend_mir_frontend_deferred_code(op_array->opcodes[i].opcode);
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED, code, op_array_id, i,
				ZEND_MIR_FRONTEND_OPERAND_NONE, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (op_array->opcodes[i].opcode == ZEND_RETURN
				&& (op_array->fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0) {
			zend_mir_frontend_set_diagnostic(
				diagnostic, ZEND_MIR_LOWERING_DEFERRED,
				ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED, op_array_id, i,
				ZEND_MIR_FRONTEND_OP1, ZEND_MIR_ID_INVALID);
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (op_array->opcodes[i].opcode == ZEND_NOP) {
			continue;
		}
		for (operand_index = 0; operand_index < 3; operand_index++) {
			missing_ssa_id = ZEND_MIR_ID_INVALID;
			if (!zend_mir_frontend_operand_has_exact_scalar(
					op_array, ssa, i, operand_index, &missing_ssa_id)) {
				zend_mir_frontend_set_diagnostic(
					diagnostic, ZEND_MIR_LOWERING_DEFERRED,
					ZEND_MIRL_MISSING_PROOF, op_array_id, i, operand_index,
					missing_ssa_id);
				return ZEND_MIR_LOWERING_DEFERRED;
			}
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_status zend_mir_frontend_validate_eligibility_w04_impl(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_op_array *original_op_array,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic,
	bool allow_source_zval_return, bool allow_any_source_zval_return)
{
	uint32_t i;
	for (i = 0; i < op_array->last; i++) {
		uint32_t operand_index;
		if (!zend_mir_frontend_opcode_is_supported(op_array->opcodes[i].opcode)
				&& !zend_mir_frontend_w04_branch(op_array->opcodes[i].opcode)
				&& !(allow_any_source_zval_return
					&& zend_mir_frontend_w09_iterator_branch(
						op_array->opcodes[i].opcode))) {
			return ZEND_MIR_LOWERING_DEFERRED;
		}
		if (op_array->opcodes[i].opcode == ZEND_NOP
				|| op_array->opcodes[i].opcode == ZEND_JMP
				|| op_array->opcodes[i].opcode == ZEND_FAST_CALL
				|| op_array->opcodes[i].opcode == ZEND_FAST_RET
				|| (allow_any_source_zval_return
					&& zend_mir_frontend_w09_iterator_branch(
						op_array->opcodes[i].opcode))) {
			continue;
		}
		for (operand_index = 0; operand_index < 3; operand_index++) {
			uint32_t missing_ssa_id = ZEND_MIR_ID_INVALID;
			if (allow_any_source_zval_return
					&& original_op_array != NULL
					&& i < original_op_array->last
					&& original_op_array->opcodes[i].opcode == ZEND_RETURN
					&& operand_index == ZEND_MIR_FRONTEND_OP1) {
				uint8_t type = original_op_array->opcodes[i].op1_type;
				if (type == IS_CONST || type == IS_CV || type == IS_VAR
						|| type == IS_TMP_VAR) {
					continue;
				}
			}
			if (allow_source_zval_return
					&& op_array->opcodes[i].opcode == ZEND_RETURN
					&& operand_index == ZEND_MIR_FRONTEND_OP1) {
				zend_mir_source_operand_ref operand;
				if (!zend_mir_frontend_operand_ref(
						op_array, ssa, i, operand_index, -1, &operand)) {
					return ZEND_MIR_LOWERING_REJECTED;
				}
				if (operand.kind == ZEND_MIR_SOURCE_OPERAND_SLOT) {
					continue;
				}
			}
			if (!zend_mir_frontend_operand_has_exact_scalar(
					op_array, ssa, i, operand_index, &missing_ssa_id)) {
				zend_mir_frontend_set_diagnostic(diagnostic,
					ZEND_MIR_LOWERING_DEFERRED,
					ZEND_MIRL_MISSING_PROOF, op_array_id, i, operand_index,
					missing_ssa_id);
				return ZEND_MIR_LOWERING_DEFERRED;
			}
		}
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

zend_mir_lowering_status zend_mir_frontend_validate_eligibility_w04(
	const zend_op_array *op_array, const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_validate_eligibility_w04_impl(
		op_array, ssa, NULL, op_array_id, diagnostic, false, false);
}

zend_mir_lowering_status zend_mir_frontend_validate_eligibility_w08(
	const zend_op_array *op_array, const zend_ssa *ssa,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_validate_eligibility_w04_impl(
		op_array, ssa, NULL, op_array_id, diagnostic, true, false);
}

zend_mir_lowering_status zend_mir_frontend_validate_eligibility_w09(
	const zend_op_array *op_array, const zend_ssa *ssa,
	const zend_op_array *original_op_array,
	zend_mir_op_array_id op_array_id,
	zend_mir_frontend_diagnostic *diagnostic)
{
	return zend_mir_frontend_validate_eligibility_w04_impl(
		op_array, ssa, original_op_array, op_array_id, diagnostic, true, true);
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
	if (source->w08 && source->w05 && source->call_op_array != NULL
			&& index < ((const zend_op_array *) source->call_op_array)->last
			&& ((const zend_op_array *) source->call_op_array)
				->opcodes[index].opcode == ZEND_CATCH) {
		const zend_op *original =
			&((const zend_op_array *) source->call_op_array)->opcodes[index];
		out->zend_opcode_number = ZEND_CATCH;
		out->extended_value = original->extended_value;
	}
	out->source_position_id = index;
	out->block_id = source->w04 && ssa->cfg.map != NULL
		? ssa->cfg.map[index] : 0;
	if (!zend_mir_frontend_operand_ref(
			op_array, ssa, index, ZEND_MIR_FRONTEND_OP1, -1, &out->op1)
		|| !zend_mir_frontend_operand_ref(
			op_array, ssa, index, ZEND_MIR_FRONTEND_OP2, -1, &out->op2)
		|| !zend_mir_frontend_operand_ref(
			op_array, ssa, index, ZEND_MIR_FRONTEND_RESULT, -1, &out->result)) {
		return false;
	}
	if (source->w08 && source->w05
			&& zend_mir_zend_source_w08_return_source_zval(source, index)) {
		const zend_op_array *original_op_array = source->call_op_array;
		const zend_op *original = &original_op_array->opcodes[index];

		if (original->op1_type == IS_CONST && source->w09) {
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
	return zend_mir_frontend_ssa_slot(
		op_array, ssa, index, &out->source_slot, &out->source_slot_kind);
}

static bool zend_mir_frontend_nth_use_or_def(
	const zend_mir_zend_source *source,
	uint32_t requested,
	bool want_def,
	uint32_t *opline_index,
	uint32_t *operand_index,
	int *ssa_id)
{
	const zend_op_array *op_array = zend_mir_source_op_array(source);
	const zend_ssa *ssa = zend_mir_source_ssa(source);
	uint32_t i;
	uint32_t operand;
	uint32_t current = 0;
	const znode_op *node;
	uint8_t operand_type;
	int use;
	int def;

	for (i = 0; i < op_array->last; i++) {
		for (operand = 0; operand < 3; operand++) {
			if (!zend_mir_frontend_operand_parts(
					&op_array->opcodes[i], &ssa->ops[i], operand,
					&node, &operand_type, &use, &def)) {
				return false;
			}
			*ssa_id = want_def ? def : use;
			if (*ssa_id >= 0 && current++ == requested) {
				*opline_index = i;
				*operand_index = operand;
				return true;
			}
		}
	}
	return false;
}

bool zend_mir_frontend_ssa_use_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_use_ref *out)
{
	uint32_t opline_index;
	uint32_t operand_index;
	int ssa_id;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->ssa_use_count
			|| !zend_mir_frontend_nth_use_or_def(
				source, index, false, &opline_index, &operand_index, &ssa_id)) {
		return false;
	}
	out->ssa_variable_id = (uint32_t) ssa_id;
	out->opline_index = opline_index;
	out->operand_index = operand_index;
	return true;
}

bool zend_mir_frontend_ssa_def_at(
	const zend_mir_zend_source *source,
	uint32_t index,
	zend_mir_source_ssa_def_ref *out)
{
	const zend_op_array *op_array;
	const zend_ssa *ssa;
	uint32_t opline_index;
	uint32_t operand_index;
	int ssa_id;

	if (!zend_mir_source_is_initialized(source) || out == NULL
			|| index >= source->ssa_def_count
			|| !zend_mir_frontend_nth_use_or_def(
				source, index, true, &opline_index, &operand_index, &ssa_id)) {
		return false;
	}
	op_array = zend_mir_source_op_array(source);
	ssa = zend_mir_source_ssa(source);
	out->ssa_variable_id = (uint32_t) ssa_id;
	out->opline_index = opline_index;
	return zend_mir_frontend_operand_ref(
		op_array, ssa, opline_index, operand_index, ssa_id, &out->destination);
}
