/* Exact source-backed zval and reference semantics for native frames. */

#include "Zend/Native/Runtime/Common/zend_native_values.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"

static const zend_op *zend_native_value_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode)
{
	const zend_op *opline;

	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->func->type != ZEND_USER_FUNCTION
			|| source_opline_index >= execute_data->func->op_array.last) {
		return NULL;
	}
	opline = &execute_data->func->op_array.opcodes[source_opline_index];
	if (opline->opcode != expected_opcode) {
		return NULL;
	}
	execute_data->opline = opline;
	return opline;
}

static zval *zend_native_value_slot(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand)
{
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
