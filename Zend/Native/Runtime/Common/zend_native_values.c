/* Exact source-backed zval and reference semantics for native frames. */

#include "Zend/Native/Runtime/Common/zend_native_values.h"

#include "Zend/zend_API.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_iterators.h"
#include "Zend/zend_operators.h"

static const zend_op *zend_native_value_source_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline;

	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->func->type != ZEND_USER_FUNCTION
			|| source_opline_index >= execute_data->func->op_array.last) {
		return NULL;
	}
	opline = &execute_data->func->op_array.opcodes[source_opline_index];
	execute_data->opline = opline;
	return opline;
}

static const zend_op *zend_native_value_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode)
{
	const zend_op *opline = zend_native_value_source_opline(
		execute_data, source_opline_index);

	return opline != NULL && opline->opcode == expected_opcode
		? opline : NULL;
}

static zval *zend_native_value_slot(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand)
{
	operand_type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (operand_type != IS_CV && operand_type != IS_VAR
			&& operand_type != IS_TMP_VAR) {
		return NULL;
	}
	return ZEND_CALL_VAR(execute_data, operand.var);
}

static zval *zend_native_value_read(
	zend_execute_data *execute_data, const zend_op *opline,
	uint8_t operand_type, znode_op operand)
{
	zval *value;

	if (operand_type == IS_CONST) {
		return RT_CONSTANT(opline, operand);
	}
	value = zend_native_value_slot(execute_data, operand_type, operand);
	if (value != NULL && operand_type == IS_VAR
			&& Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	return value;
}

static zend_native_status zend_native_value_status(void)
{
	return EG(exception) == NULL ? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

static zval *zend_native_value_read_r(
	zend_execute_data *execute_data, const zend_op *opline,
	uint8_t operand_type, znode_op operand)
{
	zval *value = zend_native_value_read(
		execute_data, opline, operand_type, operand);

	if (value != NULL && operand_type == IS_CV
			&& UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(operand.var);

		if (variable_index >= execute_data->func->op_array.last_var) {
			return NULL;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		return &EG(uninitialized_zval);
	}
	return value;
}

static void zend_native_value_consume_operand(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand,
	zval *preserve)
{
	zval *slot;

	operand_type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (operand_type != IS_TMP_VAR && operand_type != IS_VAR) {
		return;
	}
	slot = zend_native_value_slot(execute_data, operand_type, operand);
	if (slot != NULL && slot != preserve && !Z_ISUNDEF_P(slot)) {
		zval_ptr_dtor_nogc(slot);
		ZVAL_UNDEF(slot);
	}
}

zend_native_status zend_native_value_make_ref(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_MAKE_REF);
	zval *source;
	zval *result;

	if (opline == NULL
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| opline->result_type == IS_UNUSED) {
		return ZEND_NATIVE_EXCEPTION;
	}
	source = zend_native_value_slot(
		execute_data, opline->op1_type, opline->op1);
	result = zend_native_value_slot(
		execute_data, opline->result_type, opline->result);
	if (source == NULL || result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CV) {
		if (UNEXPECTED(Z_TYPE_P(source) == IS_UNDEF)) {
			ZVAL_NEW_EMPTY_REF(source);
			Z_SET_REFCOUNT_P(source, 2);
			ZVAL_NULL(Z_REFVAL_P(source));
			ZVAL_REF(result, Z_REF_P(source));
		} else {
			if (Z_ISREF_P(source)) {
				Z_ADDREF_P(source);
			} else {
				ZVAL_MAKE_REF_EX(source, 2);
			}
			ZVAL_REF(result, Z_REF_P(source));
		}
	} else if (EXPECTED(Z_TYPE_P(source) == IS_INDIRECT)) {
		source = Z_INDIRECT_P(source);
		if (EXPECTED(!Z_ISREF_P(source))) {
			ZVAL_MAKE_REF_EX(source, 2);
		} else {
			GC_ADDREF(Z_REF_P(source));
		}
		ZVAL_REF(result, Z_REF_P(source));
	} else {
		ZVAL_COPY_VALUE(result, source);
		ZVAL_UNDEF(source);
	}
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_assign_ref(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ASSIGN_REF);
	zend_refcounted *garbage = NULL;
	zend_reference *reference;
	zval *variable;
	zval *value;
	zval *value_slot;

	if (opline == NULL
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| (opline->op2_type != IS_CV && opline->op2_type != IS_VAR)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	variable = zend_native_value_slot(
		execute_data, opline->op1_type, opline->op1);
	value_slot = zend_native_value_slot(
		execute_data, opline->op2_type, opline->op2);
	if (variable == NULL || value_slot == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_VAR) {
		if (UNEXPECTED(Z_TYPE_P(variable) != IS_INDIRECT)) {
			zend_throw_error(NULL,
				"Cannot assign by reference to an array dimension of an object");
			return ZEND_NATIVE_EXCEPTION;
		}
		variable = Z_INDIRECT_P(variable);
	}
	value = value_slot;
	if (opline->op2_type == IS_VAR && Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	if (UNEXPECTED(opline->op2_type == IS_VAR
			&& opline->extended_value == ZEND_RETURNS_FUNCTION
			&& !Z_ISREF_P(value))) {
		zend_error(E_NOTICE, "Only variables should be assigned by reference");
		if (EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (Z_REFCOUNTED_P(variable)) {
			garbage = Z_COUNTED_P(variable);
		}
		ZVAL_COPY(variable, value);
	} else if (!Z_ISREF_P(value)) {
		ZVAL_NEW_REF(value, value);
		if (Z_REFCOUNTED_P(variable)) {
			garbage = Z_COUNTED_P(variable);
		}
		reference = Z_REF_P(value);
		GC_ADDREF(reference);
		ZVAL_REF(variable, reference);
	} else if (variable != value) {
		reference = Z_REF_P(value);
		GC_ADDREF(reference);
		if (Z_REFCOUNTED_P(variable)) {
			garbage = Z_COUNTED_P(variable);
		}
		ZVAL_REF(variable, reference);
	}
	if (opline->result_type != IS_UNUSED) {
		zval *result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_COPY(result, variable);
	}
	if (garbage != NULL) {
		GC_DTOR(garbage);
	}
	/* ZEND_ASSIGN_REF consumes both VAR operands.  In particular, a direct
	 * user-call result may carry the sole temporary reference container; if it
	 * is left live here, the later frame cleanup cannot see it after its SSA
	 * lifetime ended and the reference leaks. */
	if (opline->op2_type == IS_VAR && !Z_ISUNDEF_P(value_slot)) {
		zval_ptr_dtor(value_slot);
		ZVAL_UNDEF(value_slot);
	}
	if (opline->op1_type == IS_VAR && !Z_ISUNDEF_P(
			zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1))) {
		zval *variable_slot = zend_native_value_slot(
			execute_data, opline->op1_type, opline->op1);
		zval_ptr_dtor(variable_slot);
		ZVAL_UNDEF(variable_slot);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_separate(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_SEPARATE);
	zval *value;

	if (opline == NULL || opline->op1_type != IS_VAR
			|| (value = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (UNEXPECTED(Z_ISREF_P(value)) && Z_REFCOUNT_P(value) == 1) {
		ZVAL_UNREF(value);
	}
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_copy_tmp(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_COPY_TMP);
	zval *source;
	zval *result;

	if (opline == NULL || opline->op1_type != IS_TMP_VAR
			|| opline->result_type == IS_UNUSED
			|| (source = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_COPY(result, source);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_free(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_FREE);
	zval *value;

	if (opline == NULL
			|| (value = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (!Z_ISUNDEF_P(value)) {
		zval_ptr_dtor_nogc(value);
		ZVAL_UNDEF(value);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_unset_cv(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_UNSET_CV);
	zval *value;

	if (opline == NULL || opline->op1_type != IS_CV
			|| (value = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_REFCOUNTED_P(value)) {
		zend_refcounted *garbage = Z_COUNTED_P(value);
		ZVAL_UNDEF(value);
		GC_DTOR(garbage);
	} else {
		ZVAL_UNDEF(value);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_check_var(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_CHECK_VAR);
	zval *value;
	uint32_t variable_index;

	if (opline == NULL || opline->op1_type != IS_CV
			|| (value = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (UNEXPECTED(Z_TYPE_INFO_P(value) == IS_UNDEF)) {
		variable_index = EX_VAR_TO_NUM(opline->op1.var);
		if (variable_index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_assign(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ASSIGN);
	zend_refcounted *garbage = NULL;
	zval *result;
	zval *value;
	zval *variable;

	if (opline == NULL
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| (opline->op2_type != IS_CONST && opline->op2_type != IS_TMP_VAR
				&& opline->op2_type != IS_CV)
			|| (variable = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (value = zend_native_value_read(
				execute_data, opline, opline->op2_type, opline->op2)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_VAR) {
		if (Z_TYPE_P(variable) != IS_INDIRECT) {
			return ZEND_NATIVE_EXCEPTION;
		}
		variable = Z_INDIRECT_P(variable);
	}
	if (opline->op2_type == IS_CV && UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op2.var);
		if (variable_index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		value = &EG(uninitialized_zval);
	}
	value = zend_assign_to_variable_ex(variable, value, opline->op2_type,
		ZEND_CALL_USES_STRICT_TYPES(execute_data), &garbage);
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_COPY(result, value);
	}
	if (garbage != NULL) {
		GC_DTOR_NO_REF(garbage);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_assign_op(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ASSIGN_OP);
	binary_op_type operation;
	zval computed;
	zval *result;
	zval *value;
	zval *variable;

	if (opline == NULL
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| (opline->op2_type != IS_CONST && opline->op2_type != IS_TMP_VAR
				&& opline->op2_type != IS_CV)
			|| (variable = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (value = zend_native_value_read(
				execute_data, opline, opline->op2_type, opline->op2)) == NULL
			|| (operation = get_binary_op(opline->extended_value)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_VAR) {
		if (Z_TYPE_P(variable) != IS_INDIRECT) {
			return ZEND_NATIVE_EXCEPTION;
		}
		variable = Z_INDIRECT_P(variable);
	}
	if (Z_ISREF_P(variable)) {
		variable = Z_REFVAL_P(variable);
	}
	if (operation(&computed, variable, value) != SUCCESS) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zval_ptr_dtor_nogc(variable);
	ZVAL_COPY_VALUE(variable, &computed);
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_COPY(result, variable);
	}
	if (opline->op2_type == IS_TMP_VAR) {
		zval *temporary = zend_native_value_slot(
			execute_data, opline->op2_type, opline->op2);
		if (temporary != NULL && temporary != variable
				&& Z_TYPE_P(temporary) != IS_UNDEF) {
			zval_ptr_dtor_nogc(temporary);
			ZVAL_UNDEF(temporary);
		}
	}
	return zend_native_value_status();
}

static bool zend_native_value_is_binary_opcode(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_ADD:
		case ZEND_SUB:
		case ZEND_MUL:
		case ZEND_DIV:
		case ZEND_MOD:
		case ZEND_POW:
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
			return true;
		default:
			return false;
	}
}

zend_native_status zend_native_value_binary_op(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_source_opline(
		execute_data, source_opline_index);
	binary_op_type operation;
	zval *left;
	zval *right;
	zval *result;
	zval *strict_left;
	zval *strict_right;
	zend_result operation_status;

	if (opline == NULL || !zend_native_value_is_binary_opcode(opline->opcode)
			|| opline->result_type == IS_UNUSED
			|| (left = zend_native_value_read_r(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (right = zend_native_value_read_r(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	operation = get_binary_op(opline->opcode);
	if (operation == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	strict_left = left;
	strict_right = right;
	if (opline->opcode == ZEND_IS_IDENTICAL
			|| opline->opcode == ZEND_IS_NOT_IDENTICAL) {
		ZVAL_DEREF(strict_left);
		ZVAL_DEREF(strict_right);
		left = strict_left;
		right = strict_right;
	}
	operation_status = operation(result, left, right);
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, result);
	zend_native_value_consume_operand(
		execute_data, opline->op2_type, opline->op2, result);
	return operation_status == SUCCESS
		? zend_native_value_status() : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_value_unary_op(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_source_opline(
		execute_data, source_opline_index);
	unary_op_type operation;
	zval *operand;
	zval *result;
	zend_result operation_status = SUCCESS;

	if (opline == NULL || (opline->opcode != ZEND_BW_NOT
			&& opline->opcode != ZEND_BOOL_NOT
			&& opline->opcode != ZEND_BOOL)
			|| opline->result_type == IS_UNUSED
			|| (operand = zend_native_value_read_r(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->opcode == ZEND_BOOL) {
		ZVAL_BOOL(result, zend_is_true(operand));
	} else {
		operation = get_unary_op(opline->opcode);
		if (operation == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		operation_status = operation(result, operand);
	}
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, result);
	return operation_status == SUCCESS
		? zend_native_value_status() : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_value_cast(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_CAST);
	zval *operand;
	zval *result;
	zval *value;

	if (opline == NULL || opline->result_type == IS_UNUSED
			|| (operand = zend_native_value_read_r(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	value = operand;
	ZVAL_DEREF(value);
	switch (opline->extended_value) {
		case IS_LONG:
			ZVAL_LONG(result, zval_get_long(value));
			break;
		case IS_DOUBLE:
			ZVAL_DOUBLE(result, zval_get_double(value));
			break;
		case IS_STRING:
			ZVAL_STR(result, zval_get_string(value));
			break;
		case IS_ARRAY:
			if (Z_TYPE_P(value) == IS_ARRAY) {
				ZVAL_COPY(result, value);
			} else {
				zend_cast_zval_to_array(result, value, opline->op1_type);
			}
			break;
		case IS_OBJECT:
			if (Z_TYPE_P(value) == IS_OBJECT) {
				ZVAL_COPY(result, value);
			} else {
				zend_cast_zval_to_object(result, value, opline->op1_type);
			}
			break;
		default:
			return ZEND_NATIVE_EXCEPTION;
	}
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_value_status();
}

zend_native_iterator_branch_result zend_native_value_cond_branch(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_source_opline(
		execute_data, source_opline_index);
	zval *value;
	bool truth;

	if (opline == NULL || (opline->opcode != ZEND_JMPZ
			&& opline->opcode != ZEND_JMPNZ
			&& opline->opcode != ZEND_JMPZ_EX
			&& opline->opcode != ZEND_JMPNZ_EX)
			|| (value = zend_native_value_read_r(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	truth = zend_is_true(value);
	if (EG(exception) != NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (opline->opcode == ZEND_JMPZ_EX
			|| opline->opcode == ZEND_JMPNZ_EX) {
		zval *result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		ZVAL_BOOL(result, truth);
	}
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, NULL);
	return truth ? ZEND_NATIVE_ITERATOR_NEXT : ZEND_NATIVE_ITERATOR_END;
}

zend_native_status zend_native_value_isset_isempty_cv(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ISSET_ISEMPTY_CV);
	zval *value;
	zval *result;
	bool truth;

	if (opline == NULL || opline->op1_type != IS_CV
			|| (value = zend_native_value_slot(execute_data,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((opline->extended_value & ZEND_ISEMPTY) != 0) {
		truth = !zend_is_true(value);
	} else {
		truth = Z_TYPE_P(value) > IS_NULL
			&& (!Z_ISREF_P(value) || Z_TYPE_P(Z_REFVAL_P(value)) != IS_NULL);
	}
	ZVAL_BOOL(result, truth);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_qm_assign(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_QM_ASSIGN);
	zval *result;
	zval *value;

	if (opline == NULL || opline->result_type == IS_UNUSED
			|| (opline->op1_type != IS_CONST && opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_VAR && opline->op1_type != IS_CV)
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL
			|| (value = zend_native_value_read(
				execute_data, opline, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CV && UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);
		if (variable_index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		ZVAL_NULL(result);
		return zend_native_value_status();
	}
	if (opline->op1_type == IS_CV) {
		ZVAL_COPY_DEREF(result, value);
	} else if (opline->op1_type == IS_VAR && Z_ISREF_P(value)) {
		ZVAL_COPY_VALUE(result, Z_REFVAL_P(value));
		if (UNEXPECTED(Z_DELREF_P(value) == 0)) {
			efree_size(Z_REF_P(value), sizeof(zend_reference));
		} else if (Z_OPT_REFCOUNTED_P(result)) {
			Z_ADDREF_P(result);
		}
	} else {
		ZVAL_COPY_VALUE(result, value);
		if (opline->op1_type == IS_CONST && Z_OPT_REFCOUNTED_P(result)) {
			Z_ADDREF_P(result);
		}
	}
	return zend_native_value_status();
}

static zend_native_status zend_native_value_concat_impl(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, expected_opcode);
	zval *left;
	zval *result;
	zval *right;
	zend_result status;

	if (opline == NULL || opline->result_type == IS_UNUSED
			|| (opline->op1_type != IS_CONST && opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_CV)
			|| (opline->op2_type != IS_CONST && opline->op2_type != IS_TMP_VAR
				&& opline->op2_type != IS_CV)
			|| (left = zend_native_value_read(
				execute_data, opline, opline->op1_type, opline->op1)) == NULL
			|| (right = zend_native_value_read(
				execute_data, opline, opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	status = concat_function(result, left, right);
	if (opline->op1_type == IS_TMP_VAR) {
		zval_ptr_dtor_nogc(left);
		ZVAL_UNDEF(left);
	}
	if (opline->op2_type == IS_TMP_VAR) {
		zval_ptr_dtor_nogc(right);
		ZVAL_UNDEF(right);
	}
	return status == SUCCESS ? zend_native_value_status()
		: ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_value_concat(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_value_concat_impl(
		execute_data, source_opline_index, ZEND_CONCAT);
}

zend_native_status zend_native_value_fast_concat(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_value_concat_impl(
		execute_data, source_opline_index, ZEND_FAST_CONCAT);
}

static zend_string *zend_native_value_rope_piece(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *value = zend_native_value_read(
		execute_data, opline, opline->op2_type, opline->op2);

	if (value == NULL) {
		return NULL;
	}
	if (opline->op2_type == IS_CONST) {
		return zend_string_copy(Z_STR_P(value));
	}
	if (Z_TYPE_P(value) == IS_STRING) {
		return opline->op2_type == IS_CV
			? zend_string_copy(Z_STR_P(value)) : Z_STR_P(value);
	}
	if (opline->op2_type == IS_CV && Z_TYPE_P(value) == IS_UNDEF) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op2.var);
		if (variable_index >= execute_data->func->op_array.last_var) {
			return NULL;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			return NULL;
		}
	}
	zend_string *string = zval_get_string_func(value);
	if (opline->op2_type == IS_TMP_VAR) {
		zval_ptr_dtor_nogc(value);
		ZVAL_UNDEF(value);
	}
	return string;
}

static zend_native_status zend_native_value_rope_store(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode, bool initialize)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, expected_opcode);
	zend_string **rope;
	zend_string *piece;

	if (opline == NULL || (opline->op2_type != IS_CONST
			&& opline->op2_type != IS_TMP_VAR && opline->op2_type != IS_CV)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	rope = (zend_string **) zend_native_value_slot(execute_data,
		initialize ? opline->result_type : opline->op1_type,
		initialize ? opline->result : opline->op1);
	if (rope == NULL || (!initialize && opline->op1_type != IS_TMP_VAR)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	piece = zend_native_value_rope_piece(execute_data, opline);
	if (piece == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	rope[initialize ? 0 : opline->extended_value] = piece;
	return zend_native_value_status();
}

zend_native_status zend_native_value_rope_init(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_value_rope_store(
		execute_data, source_opline_index, ZEND_ROPE_INIT, true);
}

zend_native_status zend_native_value_rope_add(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_value_rope_store(
		execute_data, source_opline_index, ZEND_ROPE_ADD, false);
}

zend_native_status zend_native_value_rope_end(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ROPE_END);
	zend_string **rope;
	zend_string *piece;
	zval *result;
	size_t length = 0;
	uint32_t flags = ZSTR_COPYABLE_CONCAT_PROPERTIES;
	uint32_t index;
	char *target;

	if (opline == NULL || opline->op1_type != IS_TMP_VAR
			|| (rope = (zend_string **) zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	piece = zend_native_value_rope_piece(execute_data, opline);
	if (piece == NULL) {
		for (index = 0; index < opline->extended_value; index++) {
			zend_string_release_ex(rope[index], false);
		}
		ZVAL_UNDEF(result);
		return ZEND_NATIVE_EXCEPTION;
	}
	rope[opline->extended_value] = piece;
	for (index = 0; index <= opline->extended_value; index++) {
		if (length > ZSTR_MAX_LEN - ZSTR_LEN(rope[index])) {
			zend_error_noreturn(E_ERROR, "Integer overflow in memory allocation");
		}
		length += ZSTR_LEN(rope[index]);
		flags &= ZSTR_GET_COPYABLE_CONCAT_PROPERTIES(rope[index]);
	}
	ZVAL_STR(result, zend_string_alloc(length, false));
	GC_ADD_FLAGS(Z_STR_P(result), flags);
	target = Z_STRVAL_P(result);
	for (index = 0; index <= opline->extended_value; index++) {
		memcpy(target, ZSTR_VAL(rope[index]), ZSTR_LEN(rope[index]));
		target += ZSTR_LEN(rope[index]);
		zend_string_release_ex(rope[index], false);
	}
	*target = '\0';
	return ZEND_NATIVE_RETURNED;
}

typedef enum _zend_native_array_key_kind {
	ZEND_NATIVE_ARRAY_KEY_INVALID = 0,
	ZEND_NATIVE_ARRAY_KEY_LONG = 1,
	ZEND_NATIVE_ARRAY_KEY_STRING = 2
} zend_native_array_key_kind;

typedef struct _zend_native_array_key {
	zend_native_array_key_kind kind;
	zend_ulong index;
	zend_string *string;
} zend_native_array_key;

static bool zend_native_array_key_from_zval(
	const zval *source, uint8_t operand_type, bool deprecate_null,
	zend_native_array_key *key)
{
	const zval *value = source;
	zend_ulong index;

	if (key == NULL || value == NULL) {
		return false;
	}
	while (Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	memset(key, 0, sizeof(*key));
	switch (Z_TYPE_P(value)) {
		case IS_LONG:
			key->kind = ZEND_NATIVE_ARRAY_KEY_LONG;
			key->index = (zend_ulong) Z_LVAL_P(value);
			return true;
		case IS_STRING:
			if (operand_type != IS_CONST
					&& ZEND_HANDLE_NUMERIC(Z_STR_P(value), index)) {
				key->kind = ZEND_NATIVE_ARRAY_KEY_LONG;
				key->index = index;
			} else {
				key->kind = ZEND_NATIVE_ARRAY_KEY_STRING;
				key->string = Z_STR_P(value);
			}
			return true;
		case IS_UNDEF:
		case IS_NULL:
			if (deprecate_null) {
				zend_error(E_DEPRECATED,
					"Using null as an array offset is deprecated, use an empty string instead");
				if (EG(exception) != NULL) {
					return false;
				}
			}
			key->kind = ZEND_NATIVE_ARRAY_KEY_STRING;
			key->string = ZSTR_EMPTY_ALLOC();
			return true;
		case IS_DOUBLE:
			key->kind = ZEND_NATIVE_ARRAY_KEY_LONG;
			key->index = (zend_ulong) zend_dval_to_lval_safe(Z_DVAL_P(value));
			return EG(exception) == NULL;
		case IS_FALSE:
		case IS_TRUE:
			key->kind = ZEND_NATIVE_ARRAY_KEY_LONG;
			key->index = Z_TYPE_P(value) == IS_TRUE ? 1 : 0;
			return true;
		case IS_RESOURCE:
			zend_use_resource_as_offset(value);
			if (EG(exception) != NULL) {
				return false;
			}
			key->kind = ZEND_NATIVE_ARRAY_KEY_LONG;
			key->index = (zend_ulong) Z_RES_HANDLE_P(value);
			return true;
		default:
			zend_type_error("Illegal offset type");
			return false;
	}
}

static zval *zend_native_array_find(
	HashTable *table, const zend_native_array_key *key)
{
	return key->kind == ZEND_NATIVE_ARRAY_KEY_LONG
		? zend_hash_index_find(table, key->index)
		: zend_hash_find(table, key->string);
}

static zval *zend_native_array_update(
	HashTable *table, const zend_native_array_key *key, zval *value)
{
	return key->kind == ZEND_NATIVE_ARRAY_KEY_LONG
		? zend_hash_index_update(table, key->index, value)
		: zend_hash_update(table, key->string, value);
}

static zval *zend_native_array_write_slot(
	HashTable *table, const zend_native_array_key *key, bool warn_missing)
{
	zval *value = zend_native_array_find(table, key);

	if (value != NULL) {
		return value;
	}
	if (warn_missing) {
		if (key->kind == ZEND_NATIVE_ARRAY_KEY_LONG) {
			value = zend_undefined_offset_write(table, (zend_long) key->index);
		} else {
			value = zend_undefined_index_write(table, key->string);
		}
		return value;
	}
	return zend_native_array_update(table, key, &EG(uninitialized_zval));
}

static void zend_native_array_warn_missing(const zend_native_array_key *key)
{
	if (key->kind == ZEND_NATIVE_ARRAY_KEY_LONG) {
		zend_error(E_WARNING, "Undefined array key " ZEND_LONG_FMT,
			(zend_long) key->index);
	} else {
		zend_error(E_WARNING, "Undefined array key \"%s\"",
			ZSTR_VAL(key->string));
	}
}

static bool zend_native_value_take(
	zend_execute_data *execute_data, const zend_op *opline,
	uint8_t operand_type, znode_op operand, bool preserve_reference, zval *out)
{
	zval *slot;
	zval *value = zend_native_value_read(
		execute_data, opline, operand_type, operand);

	if (value == NULL || out == NULL) {
		return false;
	}
	if (preserve_reference && (operand_type == IS_CV || operand_type == IS_VAR)) {
		slot = zend_native_value_slot(execute_data, operand_type, operand);
		if (slot == NULL) {
			return false;
		}
		if (operand_type == IS_VAR && Z_TYPE_P(slot) == IS_INDIRECT) {
			slot = Z_INDIRECT_P(slot);
		}
		if (!Z_ISREF_P(slot)) {
			ZVAL_MAKE_REF(slot);
		}
		ZVAL_COPY(out, slot);
		return true;
	}
	if (operand_type == IS_TMP_VAR) {
		ZVAL_COPY_VALUE(out, value);
		ZVAL_UNDEF(value);
		if (Z_ISREF_P(out) && Z_REFCOUNT_P(out) == 1) {
			ZVAL_UNREF(out);
		}
		return true;
	}
	ZVAL_COPY_DEREF(out, value);
	if (operand_type == IS_VAR) {
		slot = zend_native_value_slot(execute_data, operand_type, operand);
		if (slot != NULL && Z_TYPE_P(slot) != IS_INDIRECT) {
			zval_ptr_dtor_nogc(slot);
			ZVAL_UNDEF(slot);
		}
	}
	return true;
}

static bool zend_native_array_add_source_element(
	zend_execute_data *execute_data, const zend_op *opline, HashTable *table)
{
	zend_native_array_key key;
	zval value;
	zval *offset;
	zval *inserted;
	bool by_reference =
		(opline->extended_value & ZEND_ARRAY_ELEMENT_REF) != 0;

	if (opline->op1_type == IS_UNUSED
			|| !zend_native_value_take(execute_data, opline,
				opline->op1_type, opline->op1, by_reference, &value)) {
		return false;
	}
	if (opline->op2_type == IS_UNUSED) {
		inserted = zend_hash_next_index_insert(table, &value);
	} else {
		offset = zend_native_value_read(execute_data, opline,
			opline->op2_type, opline->op2);
		if (offset == NULL || !zend_native_array_key_from_zval(
				offset, opline->op2_type, true, &key)) {
			zval_ptr_dtor_nogc(&value);
			return false;
		}
		inserted = zend_native_array_update(table, &key, &value);
	}
	if (inserted == NULL) {
		zval_ptr_dtor_nogc(&value);
		zend_cannot_add_element();
		return false;
	}
	return EG(exception) == NULL;
}

zend_native_status zend_native_value_init_array(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_INIT_ARRAY);
	zval *result;
	uint32_t size;

	if (opline == NULL || opline->result_type == IS_UNUSED
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	size = opline->extended_value >> ZEND_ARRAY_SIZE_SHIFT;
	ZVAL_ARR(result, zend_new_array(size));
	if ((opline->extended_value & ZEND_ARRAY_NOT_PACKED) != 0) {
		zend_hash_real_init_mixed(Z_ARRVAL_P(result));
	}
	if (opline->op1_type != IS_UNUSED
			&& !zend_native_array_add_source_element(
				execute_data, opline, Z_ARRVAL_P(result))) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_add_array_element(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ADD_ARRAY_ELEMENT);
	zval *result;

	if (opline == NULL || (result = zend_native_value_slot(execute_data,
			opline->result_type, opline->result)) == NULL
			|| Z_TYPE_P(result) != IS_ARRAY
			|| !zend_native_array_add_source_element(
				execute_data, opline, Z_ARRVAL_P(result))) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_add_array_unpack(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ADD_ARRAY_UNPACK);
	zval *source;
	zval *result;
	HashTable *source_table;
	HashTable *result_table;
	zend_string *key;
	zval *entry;

	if (opline == NULL
			|| (source = zend_native_value_read(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(source);
	if (Z_TYPE_P(source) != IS_ARRAY || Z_TYPE_P(result) != IS_ARRAY) {
		zend_throw_error(NULL, "Only arrays and Traversables can be unpacked, %s given",
			zend_zval_value_name(source));
		return ZEND_NATIVE_EXCEPTION;
	}
	source_table = Z_ARRVAL_P(source);
	result_table = Z_ARRVAL_P(result);
	ZEND_HASH_FOREACH_STR_KEY_VAL(source_table, key, entry) {
		zval copy;
		if (Z_ISREF_P(entry) && Z_REFCOUNT_P(entry) == 1) {
			entry = Z_REFVAL_P(entry);
		}
		ZVAL_COPY(&copy, entry);
		if (key != NULL) {
			zend_hash_update(result_table, key, &copy);
		} else if (zend_hash_next_index_insert(result_table, &copy) == NULL) {
			zval_ptr_dtor_nogc(&copy);
			zend_cannot_add_element();
			break;
		}
	} ZEND_HASH_FOREACH_END();
	if (opline->op1_type == IS_TMP_VAR) {
		zval_ptr_dtor_nogc(source);
		ZVAL_UNDEF(source);
	}
	return zend_native_value_status();
}

typedef enum _zend_native_dim_mode {
	ZEND_NATIVE_DIM_R,
	ZEND_NATIVE_DIM_W,
	ZEND_NATIVE_DIM_RW,
	ZEND_NATIVE_DIM_IS,
	ZEND_NATIVE_DIM_FUNC_ARG,
	ZEND_NATIVE_DIM_UNSET
} zend_native_dim_mode;

static bool zend_native_string_offset(
	const zval *dimension, int type, zend_long *offset)
{
	const zval *value = dimension;

try_again:
	switch (Z_TYPE_P(value)) {
		case IS_LONG:
			*offset = Z_LVAL_P(value);
			return true;
		case IS_STRING:
		{
			bool trailing_data = false;

			if (is_numeric_string_ex(
					Z_STRVAL_P(value), Z_STRLEN_P(value), offset, NULL,
					true, NULL, &trailing_data) == IS_LONG) {
				if (trailing_data && type != BP_VAR_UNSET) {
					zend_error(E_WARNING, "Illegal string offset \"%s\"",
						Z_STRVAL_P(value));
				}
				return EG(exception) == NULL;
			}
			zend_illegal_container_offset(
				ZSTR_KNOWN(ZEND_STR_STRING), value, type);
			return false;
		}
		case IS_DOUBLE:
			zend_error(E_WARNING, "String offset cast occurred");
			*offset = zend_dval_to_lval_silent(Z_DVAL_P(value));
			return EG(exception) == NULL;
		case IS_UNDEF:
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
			zend_error(E_WARNING, "String offset cast occurred");
			if (EG(exception) != NULL) {
				return false;
			}
			*offset = zval_get_long((zval *) value);
			return EG(exception) == NULL;
		case IS_REFERENCE:
			value = Z_REFVAL_P(value);
			goto try_again;
		default:
			zend_illegal_container_offset(
				ZSTR_KNOWN(ZEND_STR_STRING), value, type);
			return false;
	}
}

static bool zend_native_assign_string_offset(
	zend_execute_data *execute_data, const zend_op *opline,
	zval *container, zval *dimension, zval *source, zval **assigned)
{
	zend_string *string;
	zend_string *converted = NULL;
	zend_long offset;
	size_t source_length;
	zend_uchar character;

	if (opline->op2_type == IS_UNUSED) {
		zend_throw_error(NULL, "[] operator not supported for strings");
		return false;
	}

	string = Z_STR_P(container);
	GC_ADDREF(string);
	if (!zend_native_string_offset(dimension, BP_VAR_W, &offset)) {
		if (GC_DELREF(string) == 0) {
			zend_string_efree(string);
		}
		return false;
	}
	if (GC_DELREF(string) == 0) {
		zend_string_efree(string);
		return false;
	}

	if (offset < -(zend_long) ZSTR_LEN(string)) {
		zend_error(E_WARNING, "Illegal string offset " ZEND_LONG_FMT, offset);
		return false;
	}
	if (offset < 0) {
		offset += (zend_long) ZSTR_LEN(string);
	}

	if (Z_TYPE_P(source) == IS_STRING) {
		source_length = Z_STRLEN_P(source);
		character = source_length == 0
			? 0 : (zend_uchar) Z_STRVAL_P(source)[0];
	} else {
		GC_ADDREF(string);
		converted = zval_try_get_string(source);
		if (GC_DELREF(string) == 0) {
			zend_string_efree(string);
			if (converted != NULL) {
				zend_string_release_ex(converted, false);
			}
			return false;
		}
		if (converted == NULL) {
			return false;
		}
		source_length = ZSTR_LEN(converted);
		character = source_length == 0
			? 0 : (zend_uchar) ZSTR_VAL(converted)[0];
		zend_string_release_ex(converted, false);
	}

	if (source_length == 0) {
		zend_throw_error(NULL,
			"Cannot assign an empty string to a string offset");
		return false;
	}
	if (source_length != 1) {
		GC_ADDREF(string);
		zend_error(E_WARNING,
			"Only the first byte will be assigned to the string offset");
		if (GC_DELREF(string) == 0) {
			zend_string_efree(string);
			return false;
		}
		if (EG(exception) != NULL) {
			return false;
		}
	}

	Z_STR_P(container) = zend_string_separate(Z_STR_P(container), false);
	string = Z_STR_P(container);

	if ((size_t) offset >= ZSTR_LEN(string)) {
		zend_long old_length = (zend_long) ZSTR_LEN(string);

		ZVAL_NEW_STR(container,
			zend_string_extend(string, (size_t) offset + 1, false));
		memset(Z_STRVAL_P(container) + old_length, ' ',
			(size_t) (offset - old_length));
		Z_STRVAL_P(container)[offset + 1] = '\0';
	} else {
		zend_string_forget_hash_val(string);
	}
	Z_STRVAL_P(container)[offset] = character;

	if (opline->result_type != IS_UNUSED) {
		zval *result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);

		if (result == NULL) {
			return false;
		}
		ZVAL_CHAR(result, character);
		*assigned = result;
	} else {
		*assigned = NULL;
	}
	return true;
}

static zend_native_status zend_native_value_fetch_dim_impl(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode, zend_native_dim_mode mode)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, expected_opcode);
	zend_native_array_key key;
	zval *container;
	zval *offset;
	zval *result;
	zval *value;
	zend_long string_offset;
	bool write;

	if (opline == NULL
			|| (container = zend_native_value_read(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CV || opline->op1_type == IS_VAR) {
		ZVAL_DEREF(container);
	}
	write = mode == ZEND_NATIVE_DIM_W || mode == ZEND_NATIVE_DIM_RW
		|| mode == ZEND_NATIVE_DIM_UNSET;
	if (mode == ZEND_NATIVE_DIM_FUNC_ARG && execute_data->call != NULL
			&& (ZEND_CALL_INFO(execute_data->call)
				& ZEND_CALL_SEND_ARG_BY_REF) != 0) {
		write = true;
		mode = ZEND_NATIVE_DIM_W;
	} else if (mode == ZEND_NATIVE_DIM_FUNC_ARG) {
		mode = ZEND_NATIVE_DIM_R;
	}
	if (write && Z_TYPE_P(container) <= IS_FALSE) {
		ZVAL_ARR(container, zend_new_array(8));
	}
	if (Z_TYPE_P(container) == IS_STRING) {
		if (!write) {
			offset = zend_native_value_read(execute_data, opline,
				opline->op2_type, opline->op2);
			if (offset == NULL) {
				return ZEND_NATIVE_EXCEPTION;
			}
			zend_fetch_dimension_const(result, container, offset,
				mode == ZEND_NATIVE_DIM_IS ? BP_VAR_IS : BP_VAR_R);
			if (opline->op1_type == IS_TMP_VAR) {
				zval_ptr_dtor_nogc(container);
				ZVAL_UNDEF(container);
			}
			return zend_native_value_status();
		}
		if (opline->op2_type != IS_UNUSED) {
			offset = zend_native_value_read(execute_data, opline,
				opline->op2_type, opline->op2);
			if (offset == NULL
					|| !zend_native_string_offset(
						offset, BP_VAR_RW, &string_offset)) {
				return ZEND_NATIVE_EXCEPTION;
			}
		}
		zend_wrong_string_offset_error();
		ZVAL_UNDEF(result);
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		if (write) {
			zend_throw_error(NULL, "Cannot use a scalar value as an array");
		} else {
			zend_throw_error(NULL, "Cannot access offset of type mixed on %s",
				zend_zval_value_name(container));
		}
		ZVAL_UNDEF(result);
		return ZEND_NATIVE_EXCEPTION;
	}
	if (write) {
		SEPARATE_ARRAY(container);
	}
	if (opline->op2_type == IS_UNUSED) {
		if (!write || mode == ZEND_NATIVE_DIM_UNSET) {
			zend_throw_error(NULL, "Cannot use [] for reading");
			return ZEND_NATIVE_EXCEPTION;
		}
		value = zend_hash_next_index_insert(
			Z_ARRVAL_P(container), &EG(uninitialized_zval));
	} else {
		offset = zend_native_value_read(execute_data, opline,
			opline->op2_type, opline->op2);
		if (offset == NULL || !zend_native_array_key_from_zval(offset,
				opline->op2_type, mode != ZEND_NATIVE_DIM_UNSET, &key)) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (mode == ZEND_NATIVE_DIM_W) {
			value = zend_native_array_write_slot(
				Z_ARRVAL_P(container), &key, false);
		} else if (mode == ZEND_NATIVE_DIM_RW) {
			value = zend_native_array_write_slot(
				Z_ARRVAL_P(container), &key, true);
		} else {
			value = zend_native_array_find(Z_ARRVAL_P(container), &key);
		}
	}
	if (write) {
		if (value == NULL) {
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_INDIRECT(result, value);
	} else if (value == NULL) {
		if (mode == ZEND_NATIVE_DIM_R) {
			zend_native_array_warn_missing(&key);
		}
		ZVAL_UNDEF(result);
	} else {
		ZVAL_COPY_DEREF(result, value);
	}
	if (!write && opline->op1_type == IS_TMP_VAR) {
		zval_ptr_dtor_nogc(container);
		ZVAL_UNDEF(container);
	}
	return zend_native_value_status();
}

#define ZEND_NATIVE_DIM_WRAPPER(name, opcode, mode) \
	zend_native_status name( \
			zend_execute_data *execute_data, uint32_t source_opline_index) \
	{ \
		return zend_native_value_fetch_dim_impl( \
			execute_data, source_opline_index, opcode, mode); \
	}

ZEND_NATIVE_DIM_WRAPPER(zend_native_value_fetch_dim_r,
	ZEND_FETCH_DIM_R, ZEND_NATIVE_DIM_R)
ZEND_NATIVE_DIM_WRAPPER(zend_native_value_fetch_dim_w,
	ZEND_FETCH_DIM_W, ZEND_NATIVE_DIM_W)
ZEND_NATIVE_DIM_WRAPPER(zend_native_value_fetch_dim_rw,
	ZEND_FETCH_DIM_RW, ZEND_NATIVE_DIM_RW)
ZEND_NATIVE_DIM_WRAPPER(zend_native_value_fetch_dim_is,
	ZEND_FETCH_DIM_IS, ZEND_NATIVE_DIM_IS)
ZEND_NATIVE_DIM_WRAPPER(zend_native_value_fetch_dim_func_arg,
	ZEND_FETCH_DIM_FUNC_ARG, ZEND_NATIVE_DIM_FUNC_ARG)
ZEND_NATIVE_DIM_WRAPPER(zend_native_value_fetch_dim_unset,
	ZEND_FETCH_DIM_UNSET, ZEND_NATIVE_DIM_UNSET)

#undef ZEND_NATIVE_DIM_WRAPPER

zend_native_status zend_native_value_fetch_list(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_source_opline(
		execute_data, source_opline_index);
	zend_native_array_key key;
	zval *container_slot;
	zval *container;
	zval *offset;
	zval *result;
	zval *element;
	bool writable;

	if (opline == NULL || (opline->opcode != ZEND_FETCH_LIST_R
			&& opline->opcode != ZEND_FETCH_LIST_W)
			|| (container_slot = zend_native_value_slot(execute_data,
				opline->op1_type, opline->op1)) == NULL
			|| (container = zend_native_value_read_r(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (offset = zend_native_value_read_r(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL
			|| !zend_native_array_key_from_zval(
				offset, opline->op2_type, true, &key)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	writable = opline->opcode == ZEND_FETCH_LIST_W;
	if (writable && opline->op1_type == IS_VAR
			&& Z_TYPE_P(container_slot) != IS_INDIRECT
			&& !Z_ISREF_P(container)) {
		zend_error(E_NOTICE,
			"Attempting to set reference to non referenceable value");
		writable = false;
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) != IS_ARRAY) {
		zend_error(E_WARNING, "Cannot use %s as array",
			zend_zval_type_name(container));
		ZVAL_NULL(result);
		zend_native_value_consume_operand(
			execute_data, opline->op2_type, opline->op2, result);
		return zend_native_value_status();
	}
	if (writable) {
		SEPARATE_ARRAY(container);
		element = zend_native_array_write_slot(
			Z_ARRVAL_P(container), &key, false);
		if (element == NULL) {
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_INDIRECT(result, element);
	} else {
		element = zend_native_array_find(Z_ARRVAL_P(container), &key);
		if (element == NULL) {
			zend_native_array_warn_missing(&key);
			ZVAL_NULL(result);
		} else {
			ZVAL_COPY_DEREF(result, element);
		}
	}
	zend_native_value_consume_operand(
		execute_data, opline->op2_type, opline->op2, result);
	return zend_native_value_status();
}

static zend_native_status zend_native_value_assign_dim_impl(
	zend_execute_data *execute_data, uint32_t source_opline_index, bool compound)
{
	const zend_op *opline = zend_native_value_opline(execute_data,
		source_opline_index, compound ? ZEND_ASSIGN_DIM_OP : ZEND_ASSIGN_DIM);
	const zend_op *data;
	zend_native_array_key key;
	zval *container;
	zval *offset;
	zval *target;
	zval *result;
	zval source;
	zval computed;
	zval *assigned;
	binary_op_type operation;
	zend_long string_offset;

	if (opline == NULL || source_opline_index + 1 >= execute_data->func->op_array.last
			|| (data = opline + 1)->opcode != ZEND_OP_DATA
			|| (container = zend_native_value_read(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) == IS_STRING) {
		if (compound) {
			if (opline->op2_type != IS_UNUSED) {
				offset = zend_native_value_read(execute_data, opline,
					opline->op2_type, opline->op2);
				if (offset == NULL
						|| !zend_native_string_offset(
							offset, BP_VAR_RW, &string_offset)) {
					return ZEND_NATIVE_EXCEPTION;
				}
			}
			zend_wrong_string_offset_error();
			return ZEND_NATIVE_EXCEPTION;
		}
		offset = zend_native_value_read(execute_data, opline,
			opline->op2_type, opline->op2);
		if (opline->op2_type != IS_UNUSED && offset == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (!zend_native_value_take(execute_data, data,
				data->op1_type, data->op1, false, &source)) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (!zend_native_assign_string_offset(execute_data, opline,
				container, offset, &source, &assigned)) {
			zval_ptr_dtor_nogc(&source);
			return ZEND_NATIVE_EXCEPTION;
		}
		zval_ptr_dtor_nogc(&source);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) <= IS_FALSE) {
		if (Z_TYPE_P(container) == IS_FALSE) {
			zend_false_to_array_deprecated();
			if (EG(exception) != NULL) {
				return ZEND_NATIVE_EXCEPTION;
			}
		}
		ZVAL_ARR(container, zend_new_array(8));
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		zend_throw_error(NULL, "Cannot use a scalar value as an array");
		return ZEND_NATIVE_EXCEPTION;
	}
	SEPARATE_ARRAY(container);
	if (opline->op2_type == IS_UNUSED) {
		target = zend_hash_next_index_insert(
			Z_ARRVAL_P(container), &EG(uninitialized_zval));
	} else {
		offset = zend_native_value_read(execute_data, opline,
			opline->op2_type, opline->op2);
		if (offset == NULL || !zend_native_array_key_from_zval(
				offset, opline->op2_type, true, &key)) {
			return ZEND_NATIVE_EXCEPTION;
		}
		target = zend_native_array_write_slot(
			Z_ARRVAL_P(container), &key, compound);
	}
	if (target == NULL || !zend_native_value_take(execute_data, data,
			data->op1_type, data->op1, false, &source)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_ISREF_P(target)) {
		target = Z_REFVAL_P(target);
	}
	if (compound) {
		operation = get_binary_op(opline->extended_value);
		if (operation == NULL
				|| operation(&computed, target, &source) != SUCCESS) {
			zval_ptr_dtor_nogc(&source);
			return ZEND_NATIVE_EXCEPTION;
		}
		zval_ptr_dtor_nogc(&source);
		zval_ptr_dtor_nogc(target);
		ZVAL_COPY_VALUE(target, &computed);
		assigned = target;
	} else {
		zval_ptr_dtor_nogc(target);
		ZVAL_COPY_VALUE(target, &source);
		assigned = target;
	}
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_COPY(result, assigned);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_assign_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_value_assign_dim_impl(
		execute_data, source_opline_index, false);
}

zend_native_status zend_native_value_assign_dim_op(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_value_assign_dim_impl(
		execute_data, source_opline_index, true);
}

zend_native_status zend_native_value_unset_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_UNSET_DIM);
	zend_native_array_key key;
	zval *container;
	zval *offset;

	if (opline == NULL
			|| (container = zend_native_value_read(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (offset = zend_native_value_read(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) != IS_ARRAY) {
		if (Z_TYPE_P(container) == IS_STRING) {
			zend_throw_error(NULL, "Cannot unset string offsets");
		}
		return zend_native_value_status();
	}
	SEPARATE_ARRAY(container);
	if (!zend_native_array_key_from_zval(
			offset, opline->op2_type, false, &key)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (key.kind == ZEND_NATIVE_ARRAY_KEY_LONG) {
		zend_hash_index_del(Z_ARRVAL_P(container), key.index);
	} else {
		zend_hash_del(Z_ARRVAL_P(container), key.string);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_isset_isempty_dim(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_ISSET_ISEMPTY_DIM_OBJ);
	zend_native_array_key key;
	zval *container;
	zval *offset;
	zval *result;
	zval *value = NULL;
	bool answer;

	if (opline == NULL
			|| (container = zend_native_value_read(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (offset = zend_native_value_read(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) == IS_STRING) {
		zval string_value;

		zend_fetch_dimension_const(
			&string_value, container, offset, BP_VAR_IS);
		if (EG(exception) != NULL) {
			if (!Z_ISUNDEF(string_value)) {
				zval_ptr_dtor_nogc(&string_value);
			}
			return ZEND_NATIVE_EXCEPTION;
		}
		if ((opline->extended_value & ZEND_ISEMPTY) != 0) {
			answer = !i_zend_is_true(&string_value);
		} else {
			answer = Z_TYPE(string_value) > IS_NULL;
		}
		zval_ptr_dtor_nogc(&string_value);
		ZVAL_BOOL(result, answer);
		return ZEND_NATIVE_RETURNED;
	}
	if (Z_TYPE_P(container) == IS_ARRAY
			&& zend_native_array_key_from_zval(
				offset, opline->op2_type, false, &key)) {
		value = zend_native_array_find(Z_ARRVAL_P(container), &key);
	} else if (EG(exception) != NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((opline->extended_value & ZEND_ISEMPTY) != 0) {
		answer = value == NULL || !i_zend_is_true(value);
	} else {
		answer = value != NULL && Z_TYPE_P(value) > IS_NULL
			&& (!Z_ISREF_P(value) || Z_TYPE_P(Z_REFVAL_P(value)) != IS_NULL);
	}
	ZVAL_BOOL(result, answer);
	return ZEND_NATIVE_RETURNED;
}

static const zend_op *zend_native_iterator_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline;

	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->func->type != ZEND_USER_FUNCTION
			|| source_opline_index >= execute_data->func->op_array.last) {
		return NULL;
	}
	opline = &execute_data->func->op_array.opcodes[source_opline_index];
	if (opline->opcode != ZEND_FE_RESET_R
			&& opline->opcode != ZEND_FE_RESET_RW
			&& opline->opcode != ZEND_FE_FETCH_R
			&& opline->opcode != ZEND_FE_FETCH_RW) {
		return NULL;
	}
	execute_data->opline = opline;
	return opline;
}

static void zend_native_iterator_release_operand(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand)
{
	zval *value;

	if (operand_type != IS_TMP_VAR && operand_type != IS_VAR) {
		return;
	}
	value = zend_native_value_slot(execute_data, operand_type, operand);
	if (value != NULL && Z_TYPE_P(value) != IS_UNDEF) {
		zval_ptr_dtor_nogc(value);
		ZVAL_UNDEF(value);
	}
}

static zend_native_iterator_branch_result zend_native_iterator_reset_array(
	zend_execute_data *execute_data, const zend_op *opline,
	zval *source_slot, zval *array, bool by_reference)
{
	zval *result = zend_native_value_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (!by_reference) {
		ZVAL_COPY_VALUE(result, array);
		if (opline->op1_type != IS_TMP_VAR) {
			Z_TRY_ADDREF_P(result);
		} else {
			ZVAL_UNDEF(source_slot);
		}
		Z_FE_POS_P(result) = 0;
		return ZEND_NATIVE_ITERATOR_NEXT;
	}

	if (opline->op1_type == IS_VAR || opline->op1_type == IS_CV) {
		if (source_slot == array) {
			ZVAL_NEW_REF(source_slot, source_slot);
			array = Z_REFVAL_P(source_slot);
		}
		Z_ADDREF_P(source_slot);
		ZVAL_COPY_VALUE(result, source_slot);
	} else {
		ZVAL_NEW_REF(result, array);
		array = Z_REFVAL_P(result);
		if (opline->op1_type == IS_TMP_VAR) {
			ZVAL_UNDEF(source_slot);
		}
	}
	if (opline->op1_type == IS_CONST) {
		ZVAL_ARR(array, zend_array_dup(Z_ARRVAL_P(array)));
	} else {
		SEPARATE_ARRAY(array);
	}
	Z_FE_ITER_P(result) = zend_hash_iterator_add(Z_ARRVAL_P(array), 0);
	if (opline->op1_type == IS_VAR) {
		zend_native_iterator_release_operand(
			execute_data, opline->op1_type, opline->op1);
	}
	return ZEND_NATIVE_ITERATOR_NEXT;
}

static zend_native_iterator_branch_result zend_native_iterator_reset_object(
	zend_execute_data *execute_data, const zend_op *opline,
	zval *source_slot, zval *object, bool by_reference)
{
	zend_object *zobj = Z_OBJ_P(object);
	zval *result = zend_native_value_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (zobj->ce->get_iterator != NULL) {
		zend_object_iterator *iterator = zobj->ce->get_iterator(
			zobj->ce, object, by_reference);
		bool empty;

		if (iterator == NULL || EG(exception) != NULL) {
			if (iterator != NULL) {
				OBJ_RELEASE(&iterator->std);
			}
			if (EG(exception) == NULL) {
				zend_throw_exception_ex(NULL, 0,
					"Object of type %s did not create an Iterator",
					ZSTR_VAL(zobj->ce->name));
			}
			ZVAL_UNDEF(result);
			zend_native_iterator_release_operand(
				execute_data, opline->op1_type, opline->op1);
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		iterator->index = 0;
		if (iterator->funcs->rewind != NULL) {
			iterator->funcs->rewind(iterator);
		}
		if (EG(exception) != NULL) {
			OBJ_RELEASE(&iterator->std);
			ZVAL_UNDEF(result);
			zend_native_iterator_release_operand(
				execute_data, opline->op1_type, opline->op1);
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		empty = iterator->funcs->valid(iterator) != SUCCESS;
		if (EG(exception) != NULL) {
			OBJ_RELEASE(&iterator->std);
			ZVAL_UNDEF(result);
			zend_native_iterator_release_operand(
				execute_data, opline->op1_type, opline->op1);
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		iterator->index = -1;
		ZVAL_OBJ(result, &iterator->std);
		Z_FE_ITER_P(result) = (uint32_t) -1;
		zend_native_iterator_release_operand(
			execute_data, opline->op1_type, opline->op1);
		return empty ? ZEND_NATIVE_ITERATOR_END : ZEND_NATIVE_ITERATOR_NEXT;
	}

	if (by_reference) {
		if (opline->op1_type == IS_VAR || opline->op1_type == IS_CV) {
			if (source_slot == object) {
				ZVAL_NEW_REF(source_slot, source_slot);
				object = Z_REFVAL_P(source_slot);
			}
			Z_ADDREF_P(source_slot);
			ZVAL_COPY_VALUE(result, source_slot);
		} else {
			ZVAL_COPY_VALUE(result, object);
			if (opline->op1_type != IS_TMP_VAR) {
				Z_ADDREF_P(result);
			} else {
				ZVAL_UNDEF(source_slot);
			}
		}
		Z_FE_ITER_P(result) = zend_hash_iterator_add(
			Z_OBJPROP_P(object), 0);
	} else {
		HashTable *properties = zobj->properties != NULL
			? zobj->properties : zobj->handlers->get_properties(zobj);
		if (properties != NULL && GC_REFCOUNT(properties) > 1
				&& !(GC_FLAGS(properties) & IS_ARRAY_IMMUTABLE)) {
			GC_DELREF(properties);
			properties = zobj->properties = zend_array_dup(properties);
		}
		ZVAL_COPY_VALUE(result, object);
		if (opline->op1_type != IS_TMP_VAR) {
			Z_ADDREF_P(result);
		} else {
			ZVAL_UNDEF(source_slot);
		}
		if (properties == NULL || zend_hash_num_elements(properties) == 0) {
			Z_FE_ITER_P(result) = (uint32_t) -1;
			return ZEND_NATIVE_ITERATOR_END;
		}
		Z_FE_ITER_P(result) = zend_hash_iterator_add(properties, 0);
	}
	return zend_hash_num_elements(Z_OBJPROP_P(object)) == 0
		? ZEND_NATIVE_ITERATOR_END : ZEND_NATIVE_ITERATOR_NEXT;
}

static zend_native_iterator_branch_result zend_native_iterator_reset(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *source_slot = zend_native_value_read(
		execute_data, opline, opline->op1_type, opline->op1);
	zval *value = source_slot;
	bool by_reference = opline->opcode == ZEND_FE_RESET_RW;

	if (source_slot == NULL || opline->result_type == IS_UNUSED) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	while (Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	if (Z_TYPE_P(value) == IS_ARRAY) {
		return zend_native_iterator_reset_array(
			execute_data, opline, source_slot, value, by_reference);
	}
	if (Z_TYPE_P(value) == IS_OBJECT && opline->op1_type != IS_CONST) {
		return zend_native_iterator_reset_object(
			execute_data, opline, source_slot, value, by_reference);
	}
	zend_error(E_WARNING,
		"foreach() argument must be of type array|object, %s given",
		zend_zval_value_name(value));
	{
		zval *result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result != NULL) {
			ZVAL_UNDEF(result);
			Z_FE_ITER_P(result) = (uint32_t) -1;
		}
	}
	zend_native_iterator_release_operand(
		execute_data, opline->op1_type, opline->op1);
	return EG(exception) == NULL
		? ZEND_NATIVE_ITERATOR_END : ZEND_NATIVE_ITERATOR_EXCEPTION;
}

static bool zend_native_iterator_set_key(
	zend_execute_data *execute_data, const zend_op *opline,
	zend_ulong index, zend_string *key)
{
	zval *result;
	const char *class_name;
	const char *property_name;
	size_t property_length;

	if (opline->result_type == IS_UNUSED) {
		return true;
	}
	result = zend_native_value_slot(
		execute_data, opline->result_type, opline->result);
	if (result == NULL) {
		return false;
	}
	if (key == NULL) {
		ZVAL_LONG(result, index);
	} else if (ZSTR_VAL(key)[0] != '\0') {
		ZVAL_STR_COPY(result, key);
	} else if (zend_unmangle_property_name_ex(
			key, &class_name, &property_name, &property_length) == SUCCESS) {
		ZVAL_STRINGL(result, property_name, property_length);
	} else {
		return false;
	}
	return true;
}

static bool zend_native_iterator_assign_value(
	zend_execute_data *execute_data, const zend_op *opline,
	zval *value, bool by_reference)
{
	zval *destination = zend_native_value_slot(
		execute_data, opline->op2_type, opline->op2);

	if (destination == NULL) {
		return false;
	}
	if (by_reference) {
		if (!Z_ISREF_P(value)) {
			zval original;
			ZVAL_COPY_VALUE(&original, value);
			ZVAL_NEW_EMPTY_REF(value);
			ZVAL_COPY_VALUE(Z_REFVAL_P(value), &original);
		}
		if (destination != value) {
			zend_reference *reference = Z_REF_P(value);
			GC_ADDREF(reference);
			zval_ptr_dtor_nogc(destination);
			ZVAL_REF(destination, reference);
		}
		return true;
	}
	if (opline->op2_type == IS_CV) {
		zend_assign_to_variable(destination, value, IS_CV,
			ZEND_CALL_USES_STRICT_TYPES(execute_data));
	} else {
		ZVAL_COPY_DEREF(destination, value);
	}
	return EG(exception) == NULL;
}

static zend_native_iterator_branch_result zend_native_iterator_fetch_array(
	zend_execute_data *execute_data, const zend_op *opline,
	zval *holder, zval *array, bool by_reference)
{
	HashTable *table = Z_ARRVAL_P(array);
	HashPosition position = by_reference
		? zend_hash_iterator_pos_ex(Z_FE_ITER_P(holder), array)
		: Z_FE_POS_P(holder);
	zval *value;
	Bucket *bucket = NULL;

	while (position < table->nNumUsed) {
		if (HT_IS_PACKED(table)) {
			value = &table->arPacked[position];
		} else {
			bucket = &table->arData[position];
			value = &bucket->val;
		}
		if (Z_TYPE_P(value) != IS_UNDEF) {
			break;
		}
		position++;
	}
	if (position >= table->nNumUsed) {
		return ZEND_NATIVE_ITERATOR_END;
	}
	if (by_reference) {
		EG(ht_iterators)[Z_FE_ITER_P(holder)].pos = position + 1;
	} else {
		Z_FE_POS_P(holder) = position + 1;
	}
	if (!zend_native_iterator_set_key(execute_data, opline,
			HT_IS_PACKED(table) ? position : bucket->h,
			HT_IS_PACKED(table) ? NULL : bucket->key)
			|| !zend_native_iterator_assign_value(
				execute_data, opline, value, by_reference)) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	return ZEND_NATIVE_ITERATOR_NEXT;
}

static zend_native_iterator_branch_result zend_native_iterator_fetch_object(
	zend_execute_data *execute_data, const zend_op *opline,
	zval *holder, zval *object, bool by_reference)
{
	zend_object_iterator *iterator = zend_iterator_unwrap(object);
	zval *value;

	if (iterator != NULL) {
		const zend_object_iterator_funcs *funcs = iterator->funcs;
		if (++iterator->index > 0) {
			funcs->move_forward(iterator);
			if (EG(exception) != NULL) {
				return ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
			if (funcs->valid(iterator) == FAILURE) {
				return EG(exception) == NULL
					? ZEND_NATIVE_ITERATOR_END
					: ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
		}
		value = funcs->get_current_data(iterator);
		if (value == NULL || EG(exception) != NULL) {
			return EG(exception) == NULL
				? ZEND_NATIVE_ITERATOR_END
				: ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		if (opline->result_type != IS_UNUSED) {
			zval *key = zend_native_value_slot(
				execute_data, opline->result_type, opline->result);
			if (key == NULL) {
				return ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
			if (funcs->get_current_key != NULL) {
				funcs->get_current_key(iterator, key);
			} else {
				ZVAL_LONG(key, iterator->index);
			}
			if (EG(exception) != NULL) {
				return ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
		}
		return zend_native_iterator_assign_value(
			execute_data, opline, value, by_reference)
			? ZEND_NATIVE_ITERATOR_NEXT
			: ZEND_NATIVE_ITERATOR_EXCEPTION;
	}

	{
		HashTable *table = Z_OBJPROP_P(object);
		HashPosition position = zend_hash_iterator_pos(
			Z_FE_ITER_P(holder), table);
		Bucket *bucket;
		while (position < table->nNumUsed) {
			bucket = &table->arData[position++];
			value = &bucket->val;
			if (Z_TYPE_P(value) == IS_INDIRECT) {
				value = Z_INDIRECT_P(value);
			}
			if (Z_TYPE_P(value) == IS_UNDEF
					|| (bucket->key != NULL
						&& zend_check_property_access(Z_OBJ_P(object),
							bucket->key, Z_TYPE(bucket->val) != IS_INDIRECT)
							!= SUCCESS)) {
				continue;
			}
			EG(ht_iterators)[Z_FE_ITER_P(holder)].pos = position;
			if (!zend_native_iterator_set_key(execute_data, opline,
					bucket->h, bucket->key)
					|| !zend_native_iterator_assign_value(
						execute_data, opline, value, by_reference)) {
				return ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
			return ZEND_NATIVE_ITERATOR_NEXT;
		}
	}
	return ZEND_NATIVE_ITERATOR_END;
}

zend_native_iterator_branch_result zend_native_value_iterator_branch(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_iterator_opline(
		execute_data, source_opline_index);
	zval *holder;
	zval *value;
	bool by_reference;

	if (opline == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (opline->opcode == ZEND_FE_RESET_R
			|| opline->opcode == ZEND_FE_RESET_RW) {
		return zend_native_iterator_reset(execute_data, opline);
	}
	holder = zend_native_value_slot(
		execute_data, opline->op1_type, opline->op1);
	if (holder == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	by_reference = opline->opcode == ZEND_FE_FETCH_RW;
	value = holder;
	while (Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	if (Z_TYPE_P(value) == IS_ARRAY) {
		return zend_native_iterator_fetch_array(
			execute_data, opline, holder, value, by_reference);
	}
	if (Z_TYPE_P(value) == IS_OBJECT) {
		return zend_native_iterator_fetch_object(
			execute_data, opline, holder, value, by_reference);
	}
	zend_error(E_WARNING,
		"foreach() argument must be of type array|object, %s given",
		zend_zval_value_name(value));
	return EG(exception) == NULL
		? ZEND_NATIVE_ITERATOR_END : ZEND_NATIVE_ITERATOR_EXCEPTION;
}

zend_native_status zend_native_value_fe_free(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_value_opline(
		execute_data, source_opline_index, ZEND_FE_FREE);
	zval *value;

	if (opline == NULL
			|| (value = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(value) != IS_ARRAY
			&& Z_FE_ITER_P(value) != (uint32_t) -1) {
		zend_hash_iterator_del(Z_FE_ITER_P(value));
	}
	if (Z_TYPE_P(value) != IS_UNDEF) {
		zval_ptr_dtor_nogc(value);
		ZVAL_UNDEF(value);
	}
	return zend_native_value_status();
}
