/* Exact source-backed zval and reference semantics for native frames. */

#include "Zend/Native/Runtime/Common/zend_native_values.h"

#include "Zend/zend_API.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_operators.h"

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

	if (opline == NULL || source_opline_index + 1 >= execute_data->func->op_array.last
			|| (data = opline + 1)->opcode != ZEND_OP_DATA
			|| (container = zend_native_value_read(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(container);
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
