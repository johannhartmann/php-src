/* Exact source-backed zval and reference semantics for native frames. */

#include "Zend/Native/Runtime/Common/zend_native_values.h"

#include "Zend/zend_API.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_extensions.h"
#include "Zend/zend_fibers.h"
#include "Zend/zend_frameless_function.h"
#include "Zend/zend_ini.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_iterators.h"
#include "Zend/zend_operators.h"

#include "Zend/Native/Lowering/zend_mir_lowering_source.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include <stdlib.h>
#include <string.h>

typedef struct _zend_native_explicit_value_operation {
	uint8_t opcode;
	uint8_t op1_type;
	uint8_t op2_type;
	uint8_t result_type;
	uint8_t auxiliary_type;
	znode_op op1;
	znode_op op2;
	znode_op result;
	znode_op auxiliary;
	uint32_t extended_value;
	uint32_t source_position_id;
} zend_native_explicit_value_operation;

static zval *zend_native_value_slot(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand);

void zend_native_zval_copy_deref_or_dup(zval *target, const zval *source)
{
	/* Self-aliasing is reserved for acquiring an owner after a shallow move. */
	ZEND_ASSERT(target != source || !Z_ISREF_P(source));
	if (Z_ISREF_P(source)) {
		source = Z_REFVAL_P(source);
	}
	ZVAL_COPY_OR_DUP(target, source);
}

static bool zend_native_value_decode_explicit_operand(
	zend_execute_data *execute_data, uint64_t encoded,
	uint8_t *operand_type, znode_op *operand)
{
	zend_mir_source_operand_kind kind =
		(zend_mir_source_operand_kind) (encoded & UINT64_C(0xff));
	zend_mir_source_slot_kind slot_kind =
		(zend_mir_source_slot_kind) ((encoded >> 8) & UINT64_C(0xff));
	uint32_t index = (uint32_t) (encoded >> 16);
	uint32_t physical_slot;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| operand_type == NULL || operand == NULL) {
		return false;
	}
	memset(operand, 0, sizeof(*operand));
	if (kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		*operand_type = IS_UNUSED;
		return index == ZEND_MIR_ID_INVALID;
	}
	if (kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		if (index >= execute_data->func->op_array.last_literal) {
			return false;
		}
		*operand_type = IS_CONST;
		operand->constant = index;
		return true;
	}
	if (kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return false;
	}
	switch (slot_kind) {
		case ZEND_MIR_SOURCE_SLOT_CV:
			if (index >= (uint32_t) execute_data->func->op_array.last_var) {
				return false;
			}
			*operand_type = IS_CV;
			physical_slot = index;
			break;
		case ZEND_MIR_SOURCE_SLOT_TMP:
			if (index >= execute_data->func->op_array.T) {
				return false;
			}
			*operand_type = IS_TMP_VAR;
			physical_slot =
				(uint32_t) execute_data->func->op_array.last_var + index;
			break;
		case ZEND_MIR_SOURCE_SLOT_VAR:
			if (index >= execute_data->func->op_array.T) {
				return false;
			}
			*operand_type = IS_VAR;
			physical_slot =
				(uint32_t) execute_data->func->op_array.last_var + index;
			break;
		default:
			return false;
	}
	if (physical_slot > (UINT32_MAX / sizeof(zval))
			- (uint32_t) ZEND_CALL_FRAME_SLOT) {
		return false;
	}
	operand->var =
		((uint32_t) ZEND_CALL_FRAME_SLOT + physical_slot) * sizeof(zval);
	return true;
}

static bool zend_native_value_init_explicit_operation(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode,
	zend_native_explicit_value_operation *operation)
{
	if (operation == NULL || source_opcode != expected_opcode
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position_id >= execute_data->func->op_array.last
			|| !zend_native_value_decode_explicit_operand(
				execute_data, op1, &operation->op1_type, &operation->op1)
			|| !zend_native_value_decode_explicit_operand(
				execute_data, op2, &operation->op2_type, &operation->op2)
			|| !zend_native_value_decode_explicit_operand(
				execute_data, result,
				&operation->result_type, &operation->result)) {
		return false;
	}
	operation->opcode = (uint8_t) source_opcode;
	operation->extended_value = extended_value;
	operation->source_position_id = source_position_id;
	/*
	 * Source identity remains available for diagnostics, observers and
	 * exceptions. No semantic operand is read from this zend_op.
	 */
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	return true;
}

static bool zend_native_value_init_explicit_dim_assignment(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result, uint64_t auxiliary,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode,
	zend_native_explicit_value_operation *operation)
{
	return zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result, extended_value, source_opcode,
			source_position_id, expected_opcode, operation)
		&& zend_native_value_decode_explicit_operand(
			execute_data, auxiliary, &operation->auxiliary_type,
			&operation->auxiliary)
		&& operation->auxiliary_type != IS_UNUSED;
}

static zval *zend_native_value_read_explicit(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *operation,
	uint8_t operand_type, znode_op operand)
{
	zval *value;

	if (operand_type == IS_CONST) {
		return operand.constant < execute_data->func->op_array.last_literal
			? &execute_data->func->op_array.literals[operand.constant] : NULL;
	}
	value = zend_native_value_slot(
		execute_data, operand_type, operand);
	if (value != NULL && operand_type == IS_VAR
			&& Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	(void) operation;
	return value;
}

static zend_always_inline zval *zend_native_value_object_dimension_offset(
	uint8_t operand_type, zval *offset)
{
	if (offset != NULL && operand_type == IS_CONST
			&& Z_EXTRA_P(offset) == ZEND_EXTRA_VALUE) {
		return offset + 1;
	}
	return offset;
}

static zval *zend_native_value_read_r_explicit(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *operation,
	uint8_t operand_type, znode_op operand)
{
	zval *value = zend_native_value_read_explicit(
		execute_data, operation, operand_type, operand);

	if (value != NULL && operand_type == IS_CV
			&& UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(operand.var);
		if (variable_index >= execute_data->func->op_array.last_var) {
			return NULL;
		}
		zend_error_unchecked(E_WARNING, "Undefined variable $%S",
			execute_data->func->op_array.vars[variable_index]);
		if (EG(exception) != NULL) {
			(void) zend_native_prepare_finally_exception(
				execute_data, operation->source_position_id);
			return NULL;
		}
		return &EG(uninitialized_zval);
	}
	if (value != NULL
			&& (operand_type == IS_VAR || operand_type == IS_CV)) {
		ZVAL_DEREF(value);
	}
	return value;
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

static zend_native_status zend_native_value_status(void)
{
	return EG(exception) == NULL ? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
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

static void zend_native_value_consume_write_container(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand,
	zval *result)
{
	zval *container;

	operand_type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (operand_type != IS_VAR) {
		return;
	}
	container = zend_native_value_slot(execute_data, operand_type, operand);
	if (container == NULL || Z_ISUNDEF_P(container)) {
		return;
	}
	if (Z_REFCOUNTED_P(container)) {
		zend_refcounted *refcounted = Z_COUNTED_P(container);

		if (GC_DELREF(refcounted) == 0) {
			if (result != NULL && Z_TYPE_P(result) == IS_INDIRECT) {
				ZVAL_COPY(result, Z_INDIRECT_P(result));
			}
			rc_dtor_func(refcounted);
		}
	}
	ZVAL_UNDEF(container);
}

zend_native_status zend_native_value_make_ref(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *source;
	zval *result = NULL;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_MAKE_REF, &operation)
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_refcounted *garbage = NULL;
	zend_reference *reference;
	zval *variable;
	zval *value;
	zval *value_slot;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ASSIGN_REF, &operation)
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
	/* A failed writable fetch must retain its original exception. The VM never
	 * advances into ASSIGN_REF after that helper reports failure. */
	if (UNEXPECTED(EG(exception) != NULL)) {
		goto cleanup_operands;
	}
	if (opline->op1_type == IS_VAR) {
		if (UNEXPECTED(Z_TYPE_P(variable) != IS_INDIRECT)) {
			zend_throw_error(NULL,
				"Cannot assign by reference to an array dimension of an object");
			goto cleanup_operands;
		}
		variable = Z_INDIRECT_P(variable);
	}
	value = value_slot;
	if (opline->op2_type == IS_VAR && Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	if (opline->op2_type == IS_CV && Z_TYPE_P(value) == IS_UNDEF) {
		ZVAL_NULL(value);
	}
	if (UNEXPECTED(opline->op2_type == IS_VAR
			&& opline->extended_value == ZEND_RETURNS_FUNCTION
			&& !Z_ISREF_P(value))) {
		zend_error(E_NOTICE, "Only variables should be assigned by reference");
		if (EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		/* Match zend_wrong_assign_to_variable_reference(): write through an
		 * existing reference instead of replacing the reference container. */
		Z_TRY_ADDREF_P(value);
		variable = zend_assign_to_variable_ex(
			variable, value, IS_TMP_VAR,
			ZEND_CALL_USES_STRICT_TYPES(execute_data), &garbage);
		if (variable == NULL) {
			variable = &EG(uninitialized_zval);
		}
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

cleanup_operands:
	/* ZEND_ASSIGN_REF consumes both VAR operands.  In particular, a direct
	 * user-call result may carry the sole temporary reference container; if it
	 * is left live here, the later frame cleanup cannot see it after its SSA
	 * lifetime ended and the reference leaks. Match FREE_OP by releasing these
	 * temporary containers without registering their still-live values as GC
	 * roots. */
	if (opline->op2_type == IS_VAR && !Z_ISUNDEF_P(value_slot)) {
		zval_ptr_dtor_nogc(value_slot);
		ZVAL_UNDEF(value_slot);
	}
	if (opline->op1_type == IS_VAR && !Z_ISUNDEF_P(
			zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1))) {
		zval *variable_slot = zend_native_value_slot(
			execute_data, opline->op1_type, opline->op1);
		zval_ptr_dtor_nogc(variable_slot);
		ZVAL_UNDEF(variable_slot);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_separate(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_SEPARATE, &operation)
			|| opline->op1_type != IS_VAR
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *source;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_COPY_TMP, &operation)
			|| operation.op1_type != IS_TMP_VAR
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type == IS_UNUSED
			|| (source = zend_native_value_slot(
				execute_data, operation.op1_type, operation.op1)) == NULL
			|| (result = zend_native_value_slot(
				execute_data, operation.result_type,
				operation.result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_native_zval_copy_deref_or_dup(result, source);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_free(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result, extended_value, source_opcode,
			source_position_id, ZEND_FREE, &operation)
			|| (operation.op1_type != IS_TMP_VAR
				&& operation.op1_type != IS_VAR)
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED
			|| (value = zend_native_value_slot(
				execute_data, operation.op1_type, operation.op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (!Z_ISUNDEF_P(value)) {
		zval_ptr_dtor_nogc(value);
		ZVAL_UNDEF(value);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_echo(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result, extended_value, source_opcode,
			source_position_id, ZEND_ECHO, &operation)
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED
			|| (value = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(value) == IS_STRING) {
		if (Z_STRLEN_P(value) != 0) {
			zend_write(Z_STRVAL_P(value), Z_STRLEN_P(value));
		}
	} else {
		zend_string *string = zval_get_string_func(value);

		if (ZSTR_LEN(string) != 0) {
			zend_write(ZSTR_VAL(string), ZSTR_LEN(string));
		} else if (operation.op1_type == IS_CV
				&& UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
			uint32_t variable_index = EX_VAR_TO_NUM(operation.op1.var);

			if (variable_index >= execute_data->func->op_array.last_var) {
				zend_string_release_ex(string, false);
				return ZEND_NATIVE_EXCEPTION;
			}
			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		}
		zend_string_release_ex(string, false);
	}
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, NULL);
	return zend_native_value_status();
}

zend_native_status zend_native_value_func_num_args(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_FUNC_NUM_ARGS, &operation)
			|| operation.op1_type != IS_UNUSED
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type == IS_UNUSED
			|| (result = zend_native_value_slot(
				execute_data, operation.result_type,
				operation.result)) == NULL
			|| !Z_ISUNDEF_P(result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_LONG(result, ZEND_CALL_NUM_ARGS(execute_data));
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_func_get_args(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *result;
	zval *skip_value;
	uint32_t arg_count;
	uint32_t first_extra_arg;
	uint32_t result_size;
	uint32_t skip;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_FUNC_GET_ARGS, &operation)
			|| (operation.op1_type != IS_UNUSED
				&& operation.op1_type != IS_CONST)
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type == IS_UNUSED
			|| (result = zend_native_value_slot(
				execute_data, operation.result_type,
				operation.result)) == NULL
			|| !Z_ISUNDEF_P(result)) {
		return ZEND_NATIVE_EXCEPTION;
	}

	arg_count = ZEND_CALL_NUM_ARGS(execute_data);
	if (operation.op1_type == IS_CONST) {
		skip_value = zend_native_value_read_explicit(
			execute_data, &operation, operation.op1_type, operation.op1);
		if (skip_value == NULL || Z_TYPE_P(skip_value) != IS_LONG) {
			return ZEND_NATIVE_EXCEPTION;
		}
		skip = (uint32_t) Z_LVAL_P(skip_value);
		result_size = arg_count < skip ? 0 : arg_count - skip;
	} else {
		skip = 0;
		result_size = arg_count;
	}

	if (result_size == 0) {
		ZVAL_EMPTY_ARRAY(result);
		return ZEND_NATIVE_RETURNED;
	}

	first_extra_arg = execute_data->func->op_array.num_args;
	ZVAL_ARR(result, zend_new_array(result_size));
	zend_hash_real_init_packed(Z_ARRVAL_P(result));
	ZEND_HASH_FILL_PACKED(Z_ARRVAL_P(result)) {
		zval *argument;
		zval *value;
		zval copy;
		uint32_t index = skip;

		argument = ZEND_CALL_VAR_NUM(execute_data, index);
		if (arg_count > first_extra_arg) {
			while (index < first_extra_arg) {
				value = argument;
				if (EXPECTED(Z_TYPE_INFO_P(value) != IS_UNDEF)) {
					zend_native_zval_copy_deref_or_dup(&copy, value);
					ZEND_HASH_FILL_SET(&copy);
				} else {
					ZEND_HASH_FILL_SET_NULL();
				}
				ZEND_HASH_FILL_NEXT();
				argument++;
				index++;
			}
			if (skip < first_extra_arg) {
				skip = 0;
			} else {
				skip -= first_extra_arg;
			}
			argument = ZEND_CALL_VAR_NUM(
				execute_data,
				execute_data->func->op_array.last_var
					+ execute_data->func->op_array.T + skip);
		}
		while (index < arg_count) {
			value = argument;
			if (EXPECTED(Z_TYPE_INFO_P(value) != IS_UNDEF)) {
				zend_native_zval_copy_deref_or_dup(&copy, value);
				ZEND_HASH_FILL_SET(&copy);
			} else {
				ZEND_HASH_FILL_SET_NULL();
			}
			ZEND_HASH_FILL_NEXT();
			argument++;
			index++;
		}
	} ZEND_HASH_FILL_END();
	Z_ARRVAL_P(result)->nNumOfElements = result_size;
	return ZEND_NATIVE_RETURNED;
}

static bool zend_native_w12_value_result(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *operation,
	zval **result)
{
	return operation->result_type != IS_UNUSED
		&& (*result = zend_native_value_slot(
			execute_data, operation->result_type, operation->result)) != NULL
		&& (operation->result_type == IS_CV || Z_ISUNDEF_P(*result));
}

static void zend_native_w12_value_prepare_result(
	const zend_native_explicit_value_operation *operation, zval *result)
{
	/* TMP/VAR result slots are dead and must be undefined on entry. A CV may
	 * retain the value written by an earlier loop iteration, so release that
	 * owner only after the opcode has finished reading all of its operands. */
	if (operation->result_type == IS_CV && !Z_ISUNDEF_P(result)) {
		zval_ptr_dtor_nogc(result);
		ZVAL_UNDEF(result);
	}
}

zend_native_status zend_native_value_count(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *value;
	zval *result;
	zend_long count = 0;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_COUNT, &operation)
			|| operation.op2_type != IS_UNUSED
			|| (value = zend_native_value_read_r_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(value);
	if (Z_TYPE_P(value) == IS_ARRAY) {
		count = zend_hash_num_elements(Z_ARRVAL_P(value));
	} else if (Z_TYPE_P(value) == IS_OBJECT) {
		zend_object *object = Z_OBJ_P(value);

		if (object->handlers->count_elements != NULL
				&& object->handlers->count_elements(
					object, &count) == SUCCESS) {
			/* Handler supplied the count. */
		} else if (EG(exception) == NULL
				&& zend_class_implements_interface(
					object->ce, zend_ce_countable)) {
			zend_function *count_function = zend_hash_find_ptr(
				&object->ce->function_table, ZSTR_KNOWN(ZEND_STR_COUNT));
			zval retval;

			GC_ADDREF(object);
			zend_call_known_instance_method_with_0_params(
				count_function, object, &retval);
			OBJ_RELEASE(object);
			if (EG(exception) == NULL) {
				count = zval_get_long(&retval);
			}
			zval_ptr_dtor(&retval);
		} else if (EG(exception) == NULL) {
			zend_type_error(
				"%s(): Argument #1 ($value) must be of type Countable|array, %s given",
				extended_value ? "sizeof" : "count",
				zend_zval_value_name(value));
		}
	} else {
		zend_type_error(
			"%s(): Argument #1 ($value) must be of type Countable|array, %s given",
			extended_value ? "sizeof" : "count",
			zend_zval_value_name(value));
	}
	zend_native_w12_value_prepare_result(&operation, result);
	ZVAL_LONG(result, count);
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, result);
	return zend_native_value_status();
}

zend_native_status zend_native_value_get_type(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zend_string *type;
	zval *value;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_GET_TYPE, &operation)
			|| operation.op2_type != IS_UNUSED
			|| (value = zend_native_value_read_r_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(value);
	type = zend_zval_get_legacy_type(value);
	zend_native_w12_value_prepare_result(&operation, result);
	if (EXPECTED(type != NULL)) {
		ZVAL_INTERNED_STR(result, type);
	} else {
		ZVAL_STRING(result, "unknown type");
	}
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, result);
	return zend_native_value_status();
}

static bool zend_native_array_key_may_reenter(
	const zval *source, bool deprecate_null);

static bool zend_native_array_key_exists(HashTable *table, zval *key)
{
	zend_ulong index;

	ZVAL_DEREF(key);
	switch (Z_TYPE_P(key)) {
		case IS_STRING:
			if (ZEND_HANDLE_NUMERIC(Z_STR_P(key), index)) {
				return zend_hash_index_exists(table, index);
			}
			return zend_hash_exists(table, Z_STR_P(key));
		case IS_LONG:
			return zend_hash_index_exists(table, Z_LVAL_P(key));
		case IS_DOUBLE:
			index = zend_dval_to_lval_safe(Z_DVAL_P(key));
			return EG(exception) == NULL
				&& zend_hash_index_exists(table, index);
		case IS_FALSE:
			return zend_hash_index_exists(table, 0);
		case IS_TRUE:
			return zend_hash_index_exists(table, 1);
		case IS_RESOURCE:
			zend_use_resource_as_offset(key);
			return zend_hash_index_exists(table, Z_RES_HANDLE_P(key));
		case IS_NULL:
			zend_error(E_DEPRECATED,
				"Using null as the key parameter for array_key_exists() is deprecated, use an empty string instead");
			return zend_hash_exists(table, ZSTR_EMPTY_ALLOC());
		default:
			zend_illegal_container_offset(
				ZSTR_KNOWN(ZEND_STR_ARRAY), key, BP_VAR_RW);
			return false;
	}
}

zend_native_status zend_native_value_array_key_exists(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *key;
	zval *subject;
	zval *result;
	HashTable *table;
	bool exists = false;
	bool protect;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ARRAY_KEY_EXISTS,
			&operation)
			|| (key = zend_native_value_read_r_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| (subject = zend_native_value_read_r_explicit(
				execute_data, &operation,
				operation.op2_type, operation.op2)) == NULL
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(subject);
	if (EXPECTED(Z_TYPE_P(subject) == IS_ARRAY)) {
		table = Z_ARRVAL_P(subject);
		protect = !(GC_FLAGS(table) & IS_ARRAY_IMMUTABLE)
			&& zend_native_array_key_may_reenter(key, true);
		if (protect) {
			GC_ADDREF(table);
		}
		exists = zend_native_array_key_exists(table, key);
		if (protect && GC_DELREF(table) == 0) {
			zend_array_destroy(table);
			exists = false;
		}
	} else {
		zend_type_error(
			"array_key_exists(): Argument #2 ($array) must be of type array, %s given",
			zend_zval_value_name(subject));
	}
	zend_native_w12_value_prepare_result(&operation, result);
	ZVAL_BOOL(result, exists);
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, result);
	zend_native_value_consume_operand(
		execute_data, operation.op2_type, operation.op2, result);
	return zend_native_value_status();
}

zend_native_status zend_native_value_in_array(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *needle;
	zval *table_value;
	zval *result;
	bool found = false;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_IN_ARRAY, &operation)
			|| (needle = zend_native_value_read_r_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| (table_value = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op2_type, operation.op2)) == NULL
			|| Z_TYPE_P(table_value) != IS_ARRAY
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(needle);
	if (Z_TYPE_P(needle) == IS_STRING) {
		found = zend_hash_exists(
			Z_ARRVAL_P(table_value), Z_STR_P(needle));
	} else if (extended_value) {
		if (Z_TYPE_P(needle) == IS_LONG) {
			found = zend_hash_index_exists(
				Z_ARRVAL_P(table_value), Z_LVAL_P(needle));
		}
	} else if (Z_TYPE_P(needle) <= IS_FALSE) {
		found = zend_hash_exists(
			Z_ARRVAL_P(table_value), ZSTR_EMPTY_ALLOC());
	} else {
		zend_string *key;
		zval key_value;

		ZEND_HASH_MAP_FOREACH_STR_KEY(
				Z_ARRVAL_P(table_value), key) {
			ZVAL_STR(&key_value, key);
			if (zend_compare(needle, &key_value) == 0) {
				found = true;
				break;
			}
		} ZEND_HASH_FOREACH_END();
	}
	zend_native_w12_value_prepare_result(&operation, result);
	ZVAL_BOOL(result, found);
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, result);
	return zend_native_value_status();
}

zend_native_status zend_native_value_isset_this(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ISSET_ISEMPTY_THIS,
			&operation)
			|| operation.op1_type != IS_UNUSED
			|| operation.op2_type != IS_UNUSED
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_native_w12_value_prepare_result(&operation, result);
	ZVAL_BOOL(result,
		(extended_value & ZEND_ISEMPTY)
			^ (Z_TYPE(execute_data->This) == IS_OBJECT));
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_get_called_class(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_GET_CALLED_CLASS,
			&operation)
			|| operation.op1_type != IS_UNUSED
			|| operation.op2_type != IS_UNUSED
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE(execute_data->This) == IS_OBJECT) {
		zend_native_w12_value_prepare_result(&operation, result);
		ZVAL_STR_COPY(result, Z_OBJCE(execute_data->This)->name);
	} else if (Z_CE(execute_data->This) != NULL) {
		zend_native_w12_value_prepare_result(&operation, result);
		ZVAL_STR_COPY(result, Z_CE(execute_data->This)->name);
	} else {
		zend_throw_error(
			NULL, "get_called_class() must be called from within a class");
		zend_native_w12_value_prepare_result(&operation, result);
		ZVAL_UNDEF(result);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_begin_silence(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_BEGIN_SILENCE, &operation)
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_native_w12_value_prepare_result(&operation, result);
	ZVAL_LONG(result, EG(error_reporting));
	if (!E_HAS_ONLY_FATAL_ERRORS(EG(error_reporting))) {
		EG(error_reporting) &= E_FATAL_ERRORS;
	}
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_end_silence(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *saved;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_END_SILENCE, &operation)
			|| operation.result_type != IS_UNUSED
			|| (saved = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| Z_TYPE_P(saved) != IS_LONG) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (E_HAS_ONLY_FATAL_ERRORS(EG(error_reporting))
			&& !E_HAS_ONLY_FATAL_ERRORS(Z_LVAL_P(saved))) {
		EG(error_reporting) = Z_LVAL_P(saved);
	}
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, NULL);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_match_error(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_MATCH_ERROR, &operation)
			|| (value = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_match_unhandled_error(value);
	zend_native_value_consume_operand(
		execute_data, operation.op1_type, operation.op1, NULL);
	return ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_value_verify_never_type(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_VERIFY_NEVER_TYPE,
			&operation)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_verify_never_error(execute_data->func);
	return ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_value_defined(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *name;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_DEFINED, &operation)
			|| operation.op1_type != IS_CONST
			|| operation.op2_type != IS_UNUSED
			|| (name = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| Z_TYPE_P(name) != IS_STRING
			|| !zend_native_w12_value_result(
				execute_data, &operation, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_native_w12_value_prepare_result(&operation, result);
	ZVAL_BOOL(result,
		zend_hash_find_known_hash(
			EG(zend_constants), Z_STR_P(name)) != NULL);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_ticks(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_TICKS, &operation)
			|| operation.op1_type != IS_UNUSED
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED
			|| extended_value == 0) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (++EG(ticks_count) >= extended_value) {
		EG(ticks_count) = 0;
		if (zend_ticks_function != NULL) {
			zend_fiber_switch_block();
			zend_ticks_function((int) extended_value);
			zend_fiber_switch_unblock();
		}
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_type_assert(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *function_name;
	zval *value;
	zend_function *function;
	zend_arg_info *arginfo;
	uint16_t argument_number;
	uint8_t expected_type;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_TYPE_ASSERT, &operation)
			|| operation.op1_type != IS_CONST
			|| operation.result_type != IS_UNUSED
			|| (function_name = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| Z_TYPE_P(function_name) != IS_STRING
			|| (value = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op2_type, operation.op2)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	expected_type = (uint8_t) (extended_value & UINT32_C(0xff));
	if (EXPECTED(Z_TYPE_P(value) == expected_type)) {
		return ZEND_NATIVE_RETURNED;
	}
	argument_number = (uint16_t) (extended_value >> 16);
	function = zend_hash_find_ptr(
		EG(function_table), Z_STR_P(function_name));
	if (function == NULL || function->type == ZEND_USER_FUNCTION
			|| function->common.arg_info == NULL
			|| argument_number == 0
			|| argument_number > function->common.num_args) {
		return ZEND_NATIVE_EXCEPTION;
	}
	arginfo = &function->common.arg_info[argument_number - 1];
	if (!zend_check_type_ex(
			&arginfo->type, value, false, true)) {
		zend_string *expected = zend_type_to_string(arginfo->type);

		zend_argument_type_error_ex(
			function, argument_number,
			"must be of type %s, %s given",
			ZSTR_VAL(expected), zend_zval_value_name(value));
		zend_string_release(expected);
	}
	return zend_native_value_status();
}

static void zend_native_extension_statement(
	const zend_extension *extension, zend_execute_data *execute_data)
{
	if (extension->statement_handler != NULL) {
		extension->statement_handler(execute_data);
	}
}

static void zend_native_extension_fcall_begin(
	const zend_extension *extension, zend_execute_data *execute_data)
{
	if (extension->fcall_begin_handler != NULL) {
		extension->fcall_begin_handler(execute_data);
	}
}

static void zend_native_extension_fcall_end(
	const zend_extension *extension, zend_execute_data *execute_data)
{
	if (extension->fcall_end_handler != NULL) {
		extension->fcall_end_handler(execute_data);
	}
}

static zend_native_status zend_native_value_extension_event(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode,
	llist_apply_with_arg_func_t callback)
{
	zend_native_explicit_value_operation operation;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, expected_opcode, &operation)
			|| (expected_opcode != ZEND_EXT_STMT
				&& operation.op1_type != IS_UNUSED)
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (!EG(no_extensions)) {
		zend_llist_apply_with_argument(
			&zend_extensions, callback, execute_data);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_ext_stmt(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_extension_event(
		execute_data, op1, op2, result_operand, extended_value,
		source_opcode, source_position_id, ZEND_EXT_STMT,
		(llist_apply_with_arg_func_t) zend_native_extension_statement);
}

zend_native_status zend_native_value_ext_fcall_begin(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_extension_event(
		execute_data, op1, op2, result_operand, extended_value,
		source_opcode, source_position_id, ZEND_EXT_FCALL_BEGIN,
		(llist_apply_with_arg_func_t) zend_native_extension_fcall_begin);
}

zend_native_status zend_native_value_ext_fcall_end(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_extension_event(
		execute_data, op1, op2, result_operand, extended_value,
		source_opcode, source_position_id, ZEND_EXT_FCALL_END,
		(llist_apply_with_arg_func_t) zend_native_extension_fcall_end);
}

zend_native_status zend_native_value_ext_nop(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_EXT_NOP, &operation)
			|| operation.op1_type != IS_UNUSED
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return ZEND_NATIVE_RETURNED;
}

void zend_native_zval_store_integer(
	zval *slot, uint64_t payload, uint32_t exact_type)
{
	ZEND_ASSERT(slot != NULL);
	if (Z_ISREF_P(slot)) {
		slot = Z_REFVAL_P(slot);
	}
	zval_ptr_dtor_nogc(slot);
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			ZVAL_NULL(slot);
			break;
		case ZEND_MIR_SCALAR_TYPE_I1:
			ZVAL_BOOL(slot, payload != 0);
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			ZVAL_LONG(slot, (zend_long) payload);
			break;
		default:
			ZEND_UNREACHABLE();
	}
}

void zend_native_zval_store_double(zval *slot, double value)
{
	ZEND_ASSERT(slot != NULL);
	if (Z_ISREF_P(slot)) {
		slot = Z_REFVAL_P(slot);
	}
	zval_ptr_dtor_nogc(slot);
	ZVAL_DOUBLE(slot, value);
}

void zend_native_zval_release_slow(zval *slot)
{
	ZEND_ASSERT(slot != NULL);
	if (Z_ISREF_P(slot)) {
		slot = Z_REFVAL_P(slot);
	}
	ZEND_ASSERT(Z_REFCOUNTED_P(slot));
	zval_ptr_dtor_nogc(slot);
}

zend_native_status zend_native_value_unset_cv(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_UNSET_CV, &operation)
			|| opline->op1_type != IS_CV
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;
	uint32_t variable_index;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_CHECK_VAR, &operation)
			|| opline->op1_type != IS_CV
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_refcounted *garbage = NULL;
	zval materialized;
	zval *result;
	zval *value;
	zval *value_slot;
	zval *variable;
	uint8_t value_type;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ASSIGN, &operation)
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| (opline->op2_type != IS_CONST && opline->op2_type != IS_TMP_VAR
				&& opline->op2_type != IS_CV)
			|| (variable = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (value_slot = zend_native_value_read_explicit(
				execute_data, opline, opline->op2_type, opline->op2)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	value = value_slot;
	value_type = opline->op2_type;
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
	if (value_type == IS_CONST || value_type == IS_CV) {
		zend_native_zval_copy_deref_or_dup(&materialized, value);
		value = &materialized;
		value_type = IS_TMP_VAR;
	}
	value = zend_assign_to_variable_ex(variable, value, value_type,
		ZEND_CALL_USES_STRICT_TYPES(execute_data), &garbage);
	/*
	 * zend_copy_to_variable() transfers TMP operands without incrementing their
	 * refcount.  The VM treats that source slot as dead immediately; the native
	 * frame must make the same lifetime transition explicitly because a later
	 * opcode may reuse and destroy the physical slot.
	 */
	if (opline->op2_type == IS_TMP_VAR && value_slot != variable) {
		ZVAL_UNDEF(value_slot);
	}
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			if (garbage != NULL) {
				GC_DTOR_NO_REF(garbage);
			}
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_native_zval_copy_deref_or_dup(result, value);
	}
	if (garbage != NULL) {
		GC_DTOR_NO_REF(garbage);
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_assign_op(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation_record;
	const zend_native_explicit_value_operation *opline = &operation_record;
	binary_op_type operation;
	zval computed;
	zval *result;
	zval *value;
	zval *variable;
	zval *variable_slot;
	zend_reference *reference = NULL;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ASSIGN_OP,
			&operation_record)
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| (opline->op2_type != IS_CONST && opline->op2_type != IS_TMP_VAR
				&& opline->op2_type != IS_CV)
			|| (variable_slot = zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (value = zend_native_value_read_r_explicit(
				execute_data, opline, opline->op2_type, opline->op2)) == NULL
			|| (operation = get_binary_op(opline->extended_value)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (EG(exception) != NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	variable = variable_slot;
	if (opline->op1_type == IS_VAR) {
		if (Z_TYPE_P(variable) != IS_INDIRECT) {
			return ZEND_NATIVE_EXCEPTION;
		}
		variable = Z_INDIRECT_P(variable);
	} else if (UNEXPECTED(Z_TYPE_P(variable) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

		if (variable_index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_NULL(variable);
	}
	if (Z_ISREF_P(variable)) {
		reference = Z_REF_P(variable);
		variable = Z_REFVAL_P(variable);
	}
	if (reference != NULL && ZEND_REF_HAS_TYPE_SOURCES(reference)
			&& !(opline->extended_value == ZEND_CONCAT
				&& Z_TYPE_P(variable) == IS_STRING)) {
		if (operation(&computed, variable, value) != SUCCESS) {
			zend_native_value_consume_operand(
				execute_data, opline->op2_type, opline->op2, variable);
			return ZEND_NATIVE_EXCEPTION;
		}
		if (!zend_verify_ref_assignable_zval(
				reference, &computed,
				ZEND_CALL_USES_STRICT_TYPES(execute_data))) {
			zval_ptr_dtor(&computed);
			zend_native_value_consume_operand(
				execute_data, opline->op2_type, opline->op2, variable);
			return ZEND_NATIVE_EXCEPTION;
		}
		zval_ptr_dtor_nogc(variable);
		ZVAL_COPY_VALUE(variable, &computed);
	} else if (operation(variable, variable, value) != SUCCESS) {
		zend_native_value_consume_operand(
			execute_data, opline->op2_type, opline->op2, variable);
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			zend_native_value_consume_operand(
				execute_data, opline->op2_type, opline->op2, variable);
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_COPY(result, variable);
	}
	zend_native_value_consume_operand(
		execute_data, opline->op2_type, opline->op2, variable);
	if (opline->op1_type == IS_VAR) {
		ZVAL_UNDEF(variable_slot);
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation_record;
	const zend_native_explicit_value_operation *opline = &operation_record;
	binary_op_type operation;
	zval *left;
	zval *right;
	zval *result;
	zval *strict_left;
	zval *strict_right;
	zend_result operation_status;

	if (source_opcode > UINT8_MAX
			|| !zend_native_value_is_binary_opcode((uint8_t) source_opcode)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation_record)
			|| opline->result_type == IS_UNUSED
			|| (left = zend_native_value_read_r_explicit(
				execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (right = zend_native_value_read_r_explicit(
				execute_data, opline,
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

zend_native_status zend_native_value_case(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation_record;
	const zend_native_explicit_value_operation *opline = &operation_record;
	zval *left;
	zval *right;
	zval *result;
	bool matched;

	if ((source_opcode != ZEND_CASE && source_opcode != ZEND_CASE_STRICT)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation_record)
			|| opline->result_type == IS_UNUSED
			|| (left = zend_native_value_read_r_explicit(
				execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (right = zend_native_value_read_r_explicit(
				execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(left);
	ZVAL_DEREF(right);
	matched = source_opcode == ZEND_CASE_STRICT
		? fast_is_identical_function(left, right)
		: zend_compare(left, right) == 0;
	ZVAL_BOOL(result, matched);
	zend_native_value_consume_operand(
		execute_data, opline->op2_type, opline->op2, result);
	return zend_native_value_status();
}

zend_native_status zend_native_value_unary_op(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation_record;
	const zend_native_explicit_value_operation *opline = &operation_record;
	unary_op_type operation;
	zval *operand;
	zval *result;
	zend_result operation_status = SUCCESS;

	if ((source_opcode != ZEND_BW_NOT
			&& source_opcode != ZEND_BOOL_NOT
			&& source_opcode != ZEND_BOOL
			&& source_opcode != ZEND_STRLEN)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation_record)
			|| opline->result_type == IS_UNUSED
			|| (operand = zend_native_value_read_r_explicit(
				execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->opcode == ZEND_STRLEN) {
		if ((opline->op1_type == IS_VAR || opline->op1_type == IS_CV)
				&& Z_ISREF_P(operand)) {
			operand = Z_REFVAL_P(operand);
		}
		if (EXPECTED(Z_TYPE_P(operand) == IS_STRING)) {
			ZVAL_LONG(result, Z_STRLEN_P(operand));
		} else if (!ZEND_CALL_USES_STRICT_TYPES(execute_data)) {
			zend_string *string;
			zval temporary;

			if (UNEXPECTED(Z_TYPE_P(operand) == IS_NULL)) {
				zend_error(E_DEPRECATED,
					"strlen(): Passing null to parameter #1 ($string) of type string is deprecated");
				ZVAL_LONG(result, 0);
			} else {
				zend_native_zval_copy_deref_or_dup(&temporary, operand);
				string = zend_parse_arg_str_weak(&temporary, 1);
				if (string != NULL) {
					ZVAL_LONG(result, ZSTR_LEN(string));
				} else {
					ZVAL_UNDEF(result);
				}
				zval_ptr_dtor(&temporary);
			}
			if (Z_ISUNDEF_P(result) && EG(exception) == NULL) {
				zend_type_error(
					"strlen(): Argument #1 ($string) must be of type string, %s given",
					zend_zval_value_name(operand));
			}
		} else {
			zend_type_error(
				"strlen(): Argument #1 ($string) must be of type string, %s given",
				zend_zval_value_name(operand));
			ZVAL_UNDEF(result);
		}
	} else if (opline->opcode == ZEND_BOOL) {
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

zend_native_status zend_native_value_type_check(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;
	zval *result;
	bool matches = false;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_TYPE_CHECK, &operation)
			|| opline->result_type == IS_UNUSED
			|| (opline->op1_type != IS_CONST
				&& opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_VAR
				&& opline->op1_type != IS_CV)
			|| (value = zend_native_value_read_explicit(
				execute_data, opline, opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CV && UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

		if (variable_index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		matches = (opline->extended_value & MAY_BE_NULL) != 0;
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
		}
	} else {
		if ((opline->op1_type == IS_CV || opline->op1_type == IS_VAR)
				&& Z_ISREF_P(value)) {
			value = Z_REFVAL_P(value);
		}
		if ((opline->extended_value >> (uint32_t) Z_TYPE_P(value)) & 1u) {
			matches = opline->extended_value != MAY_BE_RESOURCE
				|| zend_rsrc_list_get_rsrc_type(Z_RES_P(value)) != NULL;
		}
	}
	ZVAL_BOOL(result, matches);
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_value_status();
}

static zend_property_info *zend_native_get_prop_not_accepting_double(
	zend_reference *reference)
{
	zend_property_info *property_info;

	ZEND_REF_FOREACH_TYPE_SOURCES(reference, property_info) {
		if (!(ZEND_TYPE_FULL_MASK(property_info->type) & MAY_BE_DOUBLE)) {
			return property_info;
		}
	} ZEND_REF_FOREACH_TYPE_SOURCES_END();
	return NULL;
}

static zend_long zend_native_throw_incdec_ref_error(
	const zend_property_info *property_info, bool increment)
{
	zend_string *type_string = zend_type_to_string(property_info->type);

	if (increment) {
		zend_type_error(
			"Cannot increment a reference held by property %s::$%s of type %s past its maximal value",
			ZSTR_VAL(property_info->ce->name),
			zend_get_unmangled_property_name(property_info->name),
			ZSTR_VAL(type_string));
	} else {
		zend_type_error(
			"Cannot decrement a reference held by property %s::$%s of type %s past its minimal value",
			ZSTR_VAL(property_info->ce->name),
			zend_get_unmangled_property_name(property_info->name),
			ZSTR_VAL(type_string));
	}
	zend_string_release(type_string);
	return increment ? ZEND_LONG_MAX : ZEND_LONG_MIN;
}

static zend_long zend_native_throw_incdec_prop_error(
	const zend_property_info *property_info, bool increment)
{
	zend_string *type_string = zend_type_to_string(property_info->type);

	if (increment) {
		zend_type_error(
			"Cannot increment property %s::$%s of type %s past its maximal value",
			ZSTR_VAL(property_info->ce->name),
			zend_get_unmangled_property_name(property_info->name),
			ZSTR_VAL(type_string));
	} else {
		zend_type_error(
			"Cannot decrement property %s::$%s of type %s past its minimal value",
			ZSTR_VAL(property_info->ce->name),
			zend_get_unmangled_property_name(property_info->name),
			ZSTR_VAL(type_string));
	}
	zend_string_release(type_string);
	return increment ? ZEND_LONG_MAX : ZEND_LONG_MIN;
}

static void zend_native_incdec_typed_reference(
	zend_execute_data *execute_data, zend_reference *reference,
	zval *copy, bool increment)
{
	zval temporary;
	zval *value = &reference->val;

	if (copy == NULL) {
		copy = &temporary;
	}
	ZVAL_COPY(copy, value);
	if (increment) {
		increment_function(value);
	} else {
		decrement_function(value);
	}
	if (UNEXPECTED(Z_TYPE_P(value) == IS_DOUBLE)
			&& Z_TYPE_P(copy) == IS_LONG) {
		zend_property_info *property_info =
			zend_native_get_prop_not_accepting_double(reference);

		if (UNEXPECTED(property_info != NULL)) {
			ZVAL_LONG(value, zend_native_throw_incdec_ref_error(
				property_info, increment));
		}
	} else if (UNEXPECTED(!zend_verify_ref_assignable_zval(
			reference, value, ZEND_CALL_USES_STRICT_TYPES(execute_data)))) {
		zval_ptr_dtor(value);
		ZVAL_COPY_VALUE(value, copy);
		ZVAL_UNDEF(copy);
	} else if (copy == &temporary) {
		zval_ptr_dtor(&temporary);
	}
}

static void zend_native_incdec_typed_property(
	zend_execute_data *execute_data,
	const zend_property_info *property_info, zval *value,
	zval *copy, bool increment)
{
	zval temporary;

	if (copy == NULL) {
		copy = &temporary;
	}
	ZVAL_COPY(copy, value);
	if (increment) {
		increment_function(value);
	} else {
		decrement_function(value);
	}
	if (UNEXPECTED(Z_TYPE_P(value) == IS_DOUBLE)
			&& Z_TYPE_P(copy) == IS_LONG
			&& !(ZEND_TYPE_FULL_MASK(property_info->type) & MAY_BE_DOUBLE)) {
		ZVAL_LONG(value, zend_native_throw_incdec_prop_error(
			property_info, increment));
	} else if (UNEXPECTED(!zend_verify_property_type(
			property_info, value,
			ZEND_CALL_USES_STRICT_TYPES(execute_data)))) {
		zval_ptr_dtor(value);
		ZVAL_COPY_VALUE(value, copy);
		ZVAL_UNDEF(copy);
	} else if (copy == &temporary) {
		zval_ptr_dtor(&temporary);
	}
}

void zend_native_incdec_property_zval(
	zend_execute_data *execute_data, zval *property,
	const zend_property_info *property_info, zval *result,
	bool post, bool increment)
{
	zval *value = property;

	if (EXPECTED(Z_TYPE_P(value) == IS_LONG)) {
		if (post && result != NULL) {
			ZVAL_LONG(result, Z_LVAL_P(value));
		}
		if (increment) {
			fast_long_increment_function(value);
		} else {
			fast_long_decrement_function(value);
		}
		if (UNEXPECTED(Z_TYPE_P(value) != IS_LONG)
				&& property_info != NULL
				&& !(ZEND_TYPE_FULL_MASK(property_info->type) & MAY_BE_DOUBLE)) {
			ZVAL_LONG(value, zend_native_throw_incdec_prop_error(
				property_info, increment));
		}
	} else {
		if (Z_ISREF_P(value)) {
			zend_reference *reference = Z_REF_P(value);

			value = Z_REFVAL_P(value);
			if (ZEND_REF_HAS_TYPE_SOURCES(reference)) {
				zend_native_incdec_typed_reference(
					execute_data, reference,
					post ? result : NULL, increment);
				goto complete;
			}
		}
		if (property_info != NULL) {
			zend_native_incdec_typed_property(
				execute_data, property_info, value,
				post ? result : NULL, increment);
		} else {
			if (post && result != NULL) {
				ZVAL_COPY(result, value);
			}
			if (increment) {
				increment_function(value);
			} else {
				decrement_function(value);
			}
		}
	}

complete:
	if (!post && result != NULL) {
		ZVAL_COPY(result, value);
	}
}

zend_native_status zend_native_value_incdec(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation_record;
	const zend_native_explicit_value_operation *opline = &operation_record;
	zend_reference *reference = NULL;
	zval *result = NULL;
	zval *slot;
	zval *value;
	bool increment;
	bool post;
	zend_result status;

	if ((source_opcode != ZEND_PRE_INC && source_opcode != ZEND_PRE_DEC
			&& source_opcode != ZEND_POST_INC
			&& source_opcode != ZEND_POST_DEC)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation_record)
			|| (opline->op1_type != IS_CV && opline->op1_type != IS_VAR)
			|| (slot = zend_native_value_slot(execute_data,
				opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	value = slot;
	if (opline->op1_type == IS_VAR) {
		if (Z_TYPE_P(value) == IS_INDIRECT) {
			value = Z_INDIRECT_P(value);
		}
	}
	if (opline->op1_type == IS_CV && Z_TYPE_P(value) == IS_UNDEF) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);
		if (variable_index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_NULL(value);
	}
	if (Z_ISREF_P(value)) {
		reference = Z_REF_P(value);
		value = Z_REFVAL_P(value);
	}
	increment = opline->opcode == ZEND_PRE_INC
		|| opline->opcode == ZEND_POST_INC;
	post = opline->opcode == ZEND_POST_INC
		|| opline->opcode == ZEND_POST_DEC;
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	if (post && result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (reference != NULL && ZEND_REF_HAS_TYPE_SOURCES(reference)) {
		zend_native_incdec_typed_reference(
			execute_data, reference, post ? result : NULL, increment);
		status = EG(exception) == NULL ? SUCCESS : FAILURE;
	} else {
		if (post) {
			ZVAL_COPY(result, value);
		}
		status = increment
			? increment_function(value) : decrement_function(value);
	}
	if (!post && result != NULL) {
		ZVAL_COPY(result, value);
	}
	if (opline->op1_type == IS_VAR) {
		if (Z_TYPE_P(slot) == IS_INDIRECT) {
			ZVAL_UNDEF(slot);
		} else {
			zval_ptr_dtor_nogc(slot);
			ZVAL_UNDEF(slot);
		}
	}
	return status == SUCCESS
		? zend_native_value_status() : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_value_cast(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *operand;
	zval *result;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_CAST, &operation)
			|| opline->result_type == IS_UNUSED
			|| (operand = zend_native_value_read_r_explicit(
				execute_data, opline,
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
				zend_native_zval_copy_deref_or_dup(result, value);
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;
	bool truth;

	if ((source_opcode != ZEND_JMPZ && source_opcode != ZEND_JMPNZ
			&& source_opcode != ZEND_JMPZ_EX
			&& source_opcode != ZEND_JMPNZ_EX
			&& source_opcode != ZEND_JMP_SET
			&& source_opcode != ZEND_COALESCE
			&& source_opcode != ZEND_JMP_NULL
			&& source_opcode != ZEND_ASSERT_CHECK)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation)
			|| (source_opcode != ZEND_ASSERT_CHECK
				&& (value = (opline->opcode == ZEND_COALESCE
					|| (opline->opcode == ZEND_JMP_NULL
						&& (opline->extended_value
							& ZEND_JMP_NULL_BP_VAR_IS) != 0)
				? zend_native_value_read_explicit(execute_data, opline,
					opline->op1_type, opline->op1)
				: zend_native_value_read_r_explicit(execute_data, opline,
				opline->op1_type, opline->op1))) == NULL)) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (opline->opcode == ZEND_ASSERT_CHECK) {
		if (opline->op1_type != IS_UNUSED
				|| opline->op2_type != IS_UNUSED) {
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		truth = EG(assertions) <= 0;
		if (truth && opline->result_type != IS_UNUSED) {
			zval *result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result);

			if (result == NULL) {
				return ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
			ZVAL_TRUE(result);
		}
		return truth
			? ZEND_NATIVE_ITERATOR_NEXT : ZEND_NATIVE_ITERATOR_END;
	}
	if (opline->opcode == ZEND_COALESCE || opline->opcode == ZEND_JMP_SET) {
		zend_reference *reference = NULL;
		zval *result;
		zval *slot = opline->op1_type == IS_CONST ? NULL
			: zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1);

		if ((opline->op1_type == IS_CV || opline->op1_type == IS_VAR)
				&& Z_ISREF_P(value)) {
			reference = opline->op1_type == IS_VAR ? Z_REF_P(value) : NULL;
			value = Z_REFVAL_P(value);
		}
		truth = opline->opcode == ZEND_COALESCE
			? Z_TYPE_P(value) > IS_NULL : zend_is_true(value);
		if (EG(exception) != NULL) {
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, NULL);
			result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result);
			if (result != NULL) {
				ZVAL_UNDEF(result);
			}
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		if (truth) {
			result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result);
			if (result == NULL) {
				return ZEND_NATIVE_ITERATOR_EXCEPTION;
			}
			if (opline->op1_type == IS_CONST
					|| opline->op1_type == IS_CV) {
				zend_native_zval_copy_deref_or_dup(result, value);
			} else if (reference != NULL) {
				ZVAL_COPY_VALUE(result, value);
				if (GC_DELREF(reference) == 0) {
					efree_size(reference, sizeof(zend_reference));
				} else {
					zend_native_zval_copy_deref_or_dup(result, result);
				}
			} else {
				ZVAL_COPY_VALUE(result, value);
			}
			if (slot != NULL && opline->op1_type != IS_CV) {
				ZVAL_UNDEF(slot);
			}
		} else {
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, NULL);
		}
		return truth ? ZEND_NATIVE_ITERATOR_NEXT : ZEND_NATIVE_ITERATOR_END;
	}
	if (opline->opcode == ZEND_JMP_NULL) {
		zval *result;
		uint32_t short_circuiting_type;

		if ((opline->op1_type == IS_CV || opline->op1_type == IS_VAR)
				&& Z_ISREF_P(value)) {
			value = Z_REFVAL_P(value);
		}
		truth = Z_TYPE_P(value) <= IS_NULL;
		if (!truth) {
			return ZEND_NATIVE_ITERATOR_END;
		}
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		short_circuiting_type =
			opline->extended_value & ZEND_SHORT_CIRCUITING_CHAIN_MASK;
		if (short_circuiting_type == ZEND_SHORT_CIRCUITING_CHAIN_EXPR) {
			ZVAL_NULL(result);
		} else if (short_circuiting_type
				== ZEND_SHORT_CIRCUITING_CHAIN_ISSET) {
			ZVAL_FALSE(result);
		} else if (short_circuiting_type
				== ZEND_SHORT_CIRCUITING_CHAIN_EMPTY) {
			ZVAL_TRUE(result);
		} else {
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
		return ZEND_NATIVE_ITERATOR_NEXT;
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
		/*
		 * Zend commonly reuses one TMP slot for the condition and the
		 * short-circuit result. Consume the old value before publishing the
		 * bool so an aliased result is not immediately destroyed again.
		 */
		zend_native_value_consume_operand(
			execute_data, opline->op1_type, opline->op1, NULL);
		ZVAL_BOOL(result, truth);
		return truth ? ZEND_NATIVE_ITERATOR_NEXT : ZEND_NATIVE_ITERATOR_END;
	}
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, NULL);
	return truth ? ZEND_NATIVE_ITERATOR_NEXT : ZEND_NATIVE_ITERATOR_END;
}

zend_native_iterator_branch_result zend_native_value_bind_static_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	HashTable *static_variables;
	zval *variable;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id,
			ZEND_BIND_INIT_STATIC_OR_JMP, &operation)
			|| operation.op1_type != IS_CV
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED
			|| (variable = zend_native_value_slot(
				execute_data, operation.op1_type, operation.op1)) == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	static_variables =
		ZEND_MAP_PTR_GET(execute_data->func->op_array.static_variables_ptr);
	if (static_variables == NULL) {
		return ZEND_NATIVE_ITERATOR_END;
	}
	value = (zval *) ((char *) static_variables->arData + extended_value);
	if (Z_TYPE_P(value) == IS_NULL) {
		return ZEND_NATIVE_ITERATOR_END;
	}
	if (Z_TYPE_P(value) != IS_REFERENCE) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	zval_ptr_dtor(variable);
	Z_ADDREF_P(value);
	ZVAL_REF(variable, Z_REF_P(value));
	return zend_native_value_status() == ZEND_NATIVE_RETURNED
		? ZEND_NATIVE_ITERATOR_NEXT : ZEND_NATIVE_ITERATOR_EXCEPTION;
}

zend_native_iterator_branch_result zend_native_value_frameless_branch(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	zval *function_name;
	zend_jmp_fl_result result;
	void **cache_slot;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_JMP_FRAMELESS, &operation)
			|| operation.op1_type != IS_CONST
			|| operation.op2_type != IS_UNUSED
			|| operation.result_type != IS_UNUSED
			|| execute_data->run_time_cache == NULL
			|| extended_value > execute_data->func->op_array.cache_size
			|| sizeof(void *) > execute_data->func->op_array.cache_size
				- extended_value
			|| (function_name = zend_native_value_read_explicit(
				execute_data, &operation,
				operation.op1_type, operation.op1)) == NULL
			|| Z_TYPE_P(function_name) != IS_STRING) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	cache_slot = (void **) (
		(char *) execute_data->run_time_cache + extended_value);
	result = (zend_jmp_fl_result) (uintptr_t) *cache_slot;
	if (result == ZEND_JMP_FL_UNPRIMED) {
		result = zend_hash_find_known_hash(
			EG(function_table), Z_STR_P(function_name)) == NULL
			? ZEND_JMP_FL_HIT : ZEND_JMP_FL_MISS;
		*cache_slot = (void *) (uintptr_t) result;
	}
	if (result == ZEND_JMP_FL_HIT) {
		return ZEND_NATIVE_ITERATOR_NEXT;
	}
	return result == ZEND_JMP_FL_MISS
		? ZEND_NATIVE_ITERATOR_END : ZEND_NATIVE_ITERATOR_EXCEPTION;
}

zend_native_status zend_native_value_isset_isempty_cv(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;
	zval *result;
	bool truth;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ISSET_ISEMPTY_CV,
			&operation)
			|| opline->op1_type != IS_CV
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
			&& (!Z_ISREF_P(value)
				|| Z_TYPE_P(Z_REFVAL_P(value)) > IS_NULL);
	}
	ZVAL_BOOL(result, truth);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_value_qm_assign(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *result;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_QM_ASSIGN, &operation)
			|| opline->result_type == IS_UNUSED
			|| (opline->op1_type != IS_CONST && opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_VAR && opline->op1_type != IS_CV)
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL
			|| (value = zend_native_value_read_explicit(
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
		zend_native_zval_copy_deref_or_dup(result, value);
	} else if (opline->op1_type == IS_VAR && Z_ISREF_P(value)) {
		ZVAL_COPY_VALUE(result, Z_REFVAL_P(value));
		if (UNEXPECTED(Z_DELREF_P(value) == 0)) {
			efree_size(Z_REF_P(value), sizeof(zend_reference));
		} else {
			zend_native_zval_copy_deref_or_dup(result, result);
		}
	} else {
		ZVAL_COPY_VALUE(result, value);
		if (opline->op1_type == IS_CONST) {
			zend_native_zval_copy_deref_or_dup(result, result);
		}
	}
	/*
	 * TMP/VAR operands are moved by ZEND_QM_ASSIGN.  The VM relies on the
	 * live-range allocator before reusing that physical slot; native helpers
	 * also inspect and destroy the previous slot contents when producing the
	 * next value.  Mark the moved-from slot undefined so that a later result
	 * cannot release the value now owned by the destination.
	 */
	if ((opline->op1_type == IS_TMP_VAR || opline->op1_type == IS_VAR)
			&& value != result) {
		ZVAL_UNDEF(value);
	}
	return zend_native_value_status();
}

static zend_native_status zend_native_verify_return_exception(
	zend_execute_data *execute_data, uint32_t source_position_id)
{
	if (EG(exception) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_prepare_finally_exception(
		execute_data, source_position_id) == SUCCESS
		? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
}

zend_native_status zend_native_value_verify_return_type(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	const zend_arg_info *return_info;
	zend_reference *reference = NULL;
	zval *retval_ref;
	zval *retval_ptr;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_VERIFY_RETURN_TYPE,
			&operation)
			|| opline->op2_type != IS_UNUSED
			|| execute_data->func->common.arg_info == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_UNUSED) {
		zend_verify_return_error(execute_data->func, NULL);
		return zend_native_verify_return_exception(
			execute_data, source_position_id);
	}
	if (opline->op1_type != IS_CONST && opline->op1_type != IS_TMP_VAR
			&& opline->op1_type != IS_VAR && opline->op1_type != IS_CV) {
		return ZEND_NATIVE_EXCEPTION;
	}
	retval_ref = retval_ptr = zend_native_value_read_explicit(
		execute_data, opline, opline->op1_type, opline->op1);
	if (retval_ptr == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CONST) {
		zval *result;

		if (opline->result_type == IS_UNUSED
				|| (result = zend_native_value_slot(
					execute_data, opline->result_type,
					opline->result)) == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		/*
		 * A constant VERIFY_RETURN_TYPE result is a fresh TMP/VAR definition.
		 * Native frames deliberately do not clear every dead temporary at
		 * entry, so the physical slot may contain stale stack bytes even
		 * though no zval is live there. Publish the copied constant directly;
		 * inspecting or destroying the old bytes would turn a register-first
		 * producer into a whole-frame initialization requirement.
		 */
		zend_native_zval_copy_deref_or_dup(result, retval_ptr);
		retval_ref = retval_ptr = result;
	} else if (opline->op1_type == IS_VAR
			|| opline->op1_type == IS_CV) {
		ZVAL_DEREF(retval_ptr);
	}

	return_info = execute_data->func->common.arg_info - 1;
	if (EXPECTED(ZEND_TYPE_CONTAINS_CODE(
			return_info->type, Z_TYPE_P(retval_ref)))) {
		return ZEND_NATIVE_RETURNED;
	}
	if (opline->op1_type == IS_CV
			&& UNEXPECTED(Z_ISUNDEF_P(retval_ptr))) {
		retval_ref = retval_ptr = zend_native_value_read_r_explicit(
			execute_data, opline, opline->op1_type, opline->op1);
		if (retval_ptr == NULL || EG(exception) != NULL) {
			return zend_native_verify_return_exception(
				execute_data, source_position_id);
		}
		if ((ZEND_TYPE_FULL_MASK(return_info->type) & MAY_BE_NULL) != 0) {
			return ZEND_NATIVE_RETURNED;
		}
	}
	if (UNEXPECTED(retval_ref != retval_ptr)) {
		if ((execute_data->func->op_array.fn_flags
				& ZEND_ACC_RETURN_REFERENCE) != 0) {
			reference = Z_REF_P(retval_ref);
		} else {
			if (Z_REFCOUNT_P(retval_ref) == 1) {
				ZVAL_UNREF(retval_ref);
			} else {
				Z_DELREF_P(retval_ref);
				ZVAL_COPY(retval_ref, retval_ptr);
			}
			retval_ptr = retval_ref;
		}
		if (EXPECTED(ZEND_TYPE_CONTAINS_CODE(
				return_info->type, Z_TYPE_P(retval_ptr)))) {
			return ZEND_NATIVE_RETURNED;
		}
	}
	if (UNEXPECTED(!zend_check_user_type_slow(
			&return_info->type, retval_ptr, reference, true))) {
		zend_verify_return_error(execute_data->func, retval_ptr);
		return zend_native_verify_return_exception(
			execute_data, source_position_id);
	}
	return zend_native_value_status();
}

static zend_native_status zend_native_value_concat_impl(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *left;
	zval *result;
	zval *right;
	zend_result status;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, expected_opcode, &operation)
			|| opline->result_type == IS_UNUSED
			|| (opline->op1_type != IS_CONST && opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_CV)
			|| (opline->op2_type != IS_CONST && opline->op2_type != IS_TMP_VAR
				&& opline->op2_type != IS_CV)
			|| (left = zend_native_value_read_r_explicit(
				execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (right = zend_native_value_read_r_explicit(
				execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_concat_impl(execute_data, op1, op2, result,
		extended_value, source_opcode, source_position_id, ZEND_CONCAT);
}

zend_native_status zend_native_value_fast_concat(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_concat_impl(execute_data, op1, op2, result,
		extended_value, source_opcode, source_position_id, ZEND_FAST_CONCAT);
}

static zend_string *zend_native_value_rope_piece(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline)
{
	zval *value = zend_native_value_read_explicit(
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode, bool initialize)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_string **rope;
	zend_string *piece;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, expected_opcode, &operation)
			|| (opline->op2_type != IS_CONST
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
	if (UNEXPECTED(EG(exception) != NULL)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_rope_init(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_rope_store(
		execute_data, op1, op2, result, extended_value, source_opcode,
		source_position_id, ZEND_ROPE_INIT, true);
}

zend_native_status zend_native_value_rope_add(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_rope_store(
		execute_data, op1, op2, result, extended_value, source_opcode,
		source_position_id, ZEND_ROPE_ADD, false);
}

zend_native_status zend_native_value_rope_end(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_string **rope;
	zend_string *piece;
	zval *result;
	size_t length = 0;
	uint32_t flags = ZSTR_COPYABLE_CONCAT_PROPERTIES;
	uint32_t index;
	char *target;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ROPE_END, &operation)
			|| opline->op1_type != IS_TMP_VAR
			|| (rope = (zend_string **) zend_native_value_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	piece = zend_native_value_rope_piece(execute_data, opline);
	if (piece == NULL) {
		ZVAL_UNDEF(result);
		return ZEND_NATIVE_EXCEPTION;
	}
	rope[opline->extended_value] = piece;
	if (UNEXPECTED(EG(exception) != NULL)) {
		ZVAL_UNDEF(result);
		return ZEND_NATIVE_EXCEPTION;
	}
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
	int access_type,
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
			zend_illegal_container_offset(
				ZSTR_KNOWN(ZEND_STR_ARRAY), value, access_type);
			return false;
	}
}

static bool zend_native_array_key_may_reenter(
	const zval *source, bool deprecate_null)
{
	const zval *value = source;

	while (value != NULL && Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	return value != NULL
		&& (Z_TYPE_P(value) == IS_DOUBLE
			|| Z_TYPE_P(value) == IS_RESOURCE
			|| (deprecate_null && Z_TYPE_P(value) <= IS_NULL));
}

static bool zend_native_array_key_from_zval_protected(
	HashTable *table, bool write,
	const zval *source, uint8_t operand_type, bool deprecate_null,
	int access_type,
	zend_native_array_key *key, bool *table_valid)
{
	bool protect = table != NULL
		&& !(GC_FLAGS(table) & IS_ARRAY_IMMUTABLE)
		&& zend_native_array_key_may_reenter(source, deprecate_null);
	bool converted;

	*table_valid = true;
	if (protect) {
		GC_ADDREF(table);
	}
	converted = zend_native_array_key_from_zval(
		source, operand_type, deprecate_null, access_type, key);
	if (protect) {
		uint32_t refcount = GC_DELREF(table);

		if (refcount == 0) {
			zend_array_destroy(table);
			*table_valid = false;
		} else if (write && refcount != 1) {
			*table_valid = false;
		}
	}
	return converted;
}

static bool zend_native_array_key_from_explicit_zval_protected(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *operation,
	HashTable *table, bool write,
	zval *source, uint8_t operand_type, znode_op operand,
	bool deprecate_null, int access_type,
	zend_native_array_key *key, bool *table_valid)
{
	if (operand_type == IS_CV && Z_TYPE_P(source) == IS_UNDEF) {
		bool protect = table != NULL
			&& !(GC_FLAGS(table) & IS_ARRAY_IMMUTABLE);

		if (protect) {
			GC_ADDREF(table);
		}
		source = zend_native_value_read_r_explicit(
			execute_data, operation, operand_type, operand);
		if (protect) {
			uint32_t refcount = GC_DELREF(table);

			if (refcount == 0) {
				zend_array_destroy(table);
				*table_valid = false;
				return false;
			}
			if (write && refcount != 1) {
				*table_valid = false;
				return false;
			}
		}
		if (source == NULL || EG(exception) != NULL) {
			return false;
		}
	}
	return zend_native_array_key_from_zval_protected(
		table, write, source, operand_type, deprecate_null,
		access_type, key, table_valid);
}

static bool zend_native_value_promote_to_array(
	zval *container, uint32_t size, HashTable **table)
{
	HashTable *new_table = zend_new_array(size);
	uint8_t old_type = Z_TYPE_P(container);

	ZVAL_ARR(container, new_table);
	if (old_type == IS_FALSE) {
		GC_ADDREF(new_table);
		zend_false_to_array_deprecated();
		if (GC_DELREF(new_table) == 0) {
			zend_array_destroy(new_table);
			*table = NULL;
			return false;
		}
	}
	*table = new_table;
	return true;
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

static bool zend_native_value_take_explicit(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *operation,
	uint8_t operand_type, znode_op operand, bool preserve_reference, zval *out)
{
	zval *slot;
	zval *value = preserve_reference
		? zend_native_value_read_explicit(
			execute_data, operation, operand_type, operand)
		: zend_native_value_read_r_explicit(
			execute_data, operation, operand_type, operand);

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
		if (operand_type == IS_VAR) {
			zval *source_slot = zend_native_value_slot(
				execute_data, operand_type, operand);

			if (source_slot != NULL
					&& Z_TYPE_P(source_slot) != IS_INDIRECT) {
				zval_ptr_dtor_nogc(source_slot);
				ZVAL_UNDEF(source_slot);
			}
		}
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
	zend_native_zval_copy_deref_or_dup(out, value);
	if (operand_type == IS_VAR) {
		slot = zend_native_value_slot(execute_data, operand_type, operand);
		if (slot != NULL && Z_TYPE_P(slot) != IS_INDIRECT) {
			zval_ptr_dtor_nogc(slot);
			ZVAL_UNDEF(slot);
		}
	}
	return true;
}

static bool zend_native_array_add_explicit_element(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline, HashTable *table)
{
	zend_native_array_key key;
	zval value;
	zval *offset;
	zval *inserted;
	bool by_reference =
		(opline->extended_value & ZEND_ARRAY_ELEMENT_REF) != 0;

	if (opline->op1_type == IS_UNUSED
			|| !zend_native_value_take_explicit(execute_data, opline,
				opline->op1_type, opline->op1, by_reference, &value)) {
		return false;
	}
	if (opline->op2_type == IS_UNUSED) {
		inserted = zend_hash_next_index_insert(table, &value);
	} else {
		offset = zend_native_value_read_explicit(execute_data, opline,
			opline->op2_type, opline->op2);
		if (offset == NULL) {
			zval_ptr_dtor_nogc(&value);
			return false;
		}
		if (!zend_native_array_key_from_zval(
				offset, opline->op2_type, true, BP_VAR_W, &key)) {
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, NULL);
			zval_ptr_dtor_nogc(&value);
			return false;
		}
		inserted = zend_native_array_update(table, &key, &value);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, NULL);
	}
	if (inserted == NULL) {
		zval_ptr_dtor_nogc(&value);
		zend_cannot_add_element();
		return false;
	}
	return EG(exception) == NULL;
}

zend_native_status zend_native_value_init_array(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *result;
	uint32_t size;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_INIT_ARRAY, &operation)
			|| opline->result_type == IS_UNUSED
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
			&& !zend_native_array_add_explicit_element(
				execute_data, opline, Z_ARRVAL_P(result))) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_add_array_element(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *result;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ADD_ARRAY_ELEMENT,
			&operation)
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL
			|| Z_TYPE_P(result) != IS_ARRAY
			|| !zend_native_array_add_explicit_element(
				execute_data, opline, Z_ARRVAL_P(result))) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_value_status();
}

zend_native_status zend_native_value_add_array_unpack(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *source;
	zval *result;
	HashTable *source_table;
	HashTable *result_table;
	zend_string *key;
	zval *entry;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_ADD_ARRAY_UNPACK,
			&operation)
			|| (source = zend_native_value_read_r_explicit(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(source);
	if (Z_TYPE_P(result) != IS_ARRAY) {
		zend_throw_error(NULL, "Invalid native array unpack destination");
		return ZEND_NATIVE_EXCEPTION;
	}
	result_table = Z_ARRVAL_P(result);
	if (Z_TYPE_P(source) == IS_OBJECT) {
		zend_class_entry *class_entry = Z_OBJCE_P(source);
		zend_object_iterator *iterator;
		const zend_object_iterator_funcs *functions;

		if (class_entry->get_iterator == NULL) {
			zend_type_error(
				"Only arrays and Traversables can be unpacked, %s given",
				zend_zval_value_name(source));
			goto finish;
		}
		iterator = class_entry->get_iterator(class_entry, source, 0);
		if (iterator == NULL) {
			if (EG(exception) == NULL) {
				zend_throw_exception_ex(NULL, 0,
					"Object of type %s did not create an Iterator",
					ZSTR_VAL(class_entry->name));
			}
			goto finish;
		}
		functions = iterator->funcs;
		if (functions->rewind != NULL) {
			functions->rewind(iterator);
		}
		while (EG(exception) == NULL
				&& functions->valid(iterator) == SUCCESS) {
			zval *value;
			zval key_value;
			zend_string *string_key = NULL;
			zend_ulong numeric_key;
			zval copy;

			if (EG(exception) != NULL) {
				break;
			}
			value = functions->get_current_data(iterator);
			if (EG(exception) != NULL || value == NULL) {
				break;
			}
			if (functions->get_current_key != NULL) {
				ZVAL_UNDEF(&key_value);
				functions->get_current_key(iterator, &key_value);
				if (EG(exception) != NULL) {
					if (!Z_ISUNDEF(key_value)) {
						zval_ptr_dtor(&key_value);
					}
					break;
				}
				if (Z_TYPE(key_value) != IS_LONG
						&& Z_TYPE(key_value) != IS_STRING) {
					zend_throw_error(NULL,
						"Keys must be of type int|string during array unpacking");
					zval_ptr_dtor(&key_value);
					break;
				}
				if (Z_TYPE(key_value) == IS_STRING
						&& !ZEND_HANDLE_NUMERIC(
							Z_STR(key_value), numeric_key)) {
					string_key = Z_STR(key_value);
				}
			} else {
				ZVAL_UNDEF(&key_value);
			}

			ZVAL_DEREF(value);
			zend_native_zval_copy_deref_or_dup(&copy, value);
			if (string_key != NULL) {
				zend_hash_update(result_table, string_key, &copy);
				zval_ptr_dtor_str(&key_value);
			} else {
				zval_ptr_dtor(&key_value);
				if (zend_hash_next_index_insert(result_table, &copy) == NULL) {
					zend_cannot_add_element();
					zval_ptr_dtor_nogc(&copy);
					break;
				}
			}
			functions->move_forward(iterator);
		}
		zend_iterator_dtor(iterator);
		goto finish;
	}
	if (Z_TYPE_P(source) != IS_ARRAY) {
		zend_throw_error(NULL, "Only arrays and Traversables can be unpacked, %s given",
			zend_zval_value_name(source));
		goto finish;
	}
	source_table = Z_ARRVAL_P(source);
	ZEND_HASH_FOREACH_STR_KEY_VAL(source_table, key, entry) {
		zval copy;
		if (Z_ISREF_P(entry)) {
			if (Z_REFCOUNT_P(entry) == 1) {
				entry = Z_REFVAL_P(entry);
				zend_native_zval_copy_deref_or_dup(&copy, entry);
			} else {
				/* Shared references remain aliases, matching ZEND_ADD_ARRAY_UNPACK. */
				ZVAL_COPY(&copy, entry);
			}
		} else {
			zend_native_zval_copy_deref_or_dup(&copy, entry);
		}
		if (key != NULL) {
			zend_hash_update(result_table, key, &copy);
		} else if (zend_hash_next_index_insert(result_table, &copy) == NULL) {
			zval_ptr_dtor_nogc(&copy);
			zend_cannot_add_element();
			break;
		}
	} ZEND_HASH_FOREACH_END();

finish:
	zend_native_value_consume_operand(
		execute_data, opline->op1_type, opline->op1, result);
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
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *operation,
	zval *container, zval *dimension, zval *source, zval **assigned)
{
	zend_string *string;
	zend_string *converted = NULL;
	zend_long offset;
	size_t source_length;
	zend_uchar character;

	if (operation->op2_type == IS_UNUSED) {
		zend_throw_error(NULL, "[] operator not supported for strings");
		return false;
	}

	ZVAL_NEW_STR(container,
		zend_string_separate(Z_STR_P(container), false));
	string = Z_STR_P(container);
	zend_string_addref(string);
	if (operation->op2_type == IS_CV
			&& Z_TYPE_P(dimension) == IS_UNDEF) {
		dimension = zend_native_value_read_r_explicit(
			execute_data, operation,
			operation->op2_type, operation->op2);
	}
	bool valid_offset = dimension != NULL && EG(exception) == NULL
		&& zend_native_string_offset(dimension, BP_VAR_W, &offset);
	if (zend_string_delref(string) == 0) {
		zend_string_efree(string);
		return false;
	}
	if (!valid_offset) {
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
		zend_string_addref(string);
		converted = zval_try_get_string(source);
		if (zend_string_delref(string) == 0) {
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
		zend_string_addref(string);
		zend_error(E_WARNING,
			"Only the first byte will be assigned to the string offset");
		if (zend_string_delref(string) == 0) {
			zend_string_efree(string);
			return false;
		}
		if (EG(exception) != NULL) {
			return false;
		}
	}

	zend_string_forget_hash_val(string);

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

	if (operation->result_type != IS_UNUSED) {
		zval *result = zend_native_value_slot(
			execute_data, operation->result_type, operation->result);

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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode,
	zend_native_dim_mode mode)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_native_array_key key;
	HashTable *table = NULL;
	zval *container;
	zval *offset;
	zval *result;
	zval *value;
	zend_reference *container_reference = NULL;
	zend_object *protected_object = NULL;
	zend_object *container_warning_exception = NULL;
	zval protected_container;
	zend_long string_offset;
	bool table_valid = true;
	bool write;
	bool func_arg_by_ref;

	func_arg_by_ref = mode == ZEND_NATIVE_DIM_FUNC_ARG
		&& execute_data->call != NULL
		&& (ZEND_CALL_INFO(execute_data->call)
			& ZEND_CALL_SEND_ARG_BY_REF) != 0;
	write = mode == ZEND_NATIVE_DIM_W || mode == ZEND_NATIVE_DIM_RW
		|| mode == ZEND_NATIVE_DIM_UNSET;
	if (func_arg_by_ref) {
		write = true;
		mode = ZEND_NATIVE_DIM_W;
	} else if (mode == ZEND_NATIVE_DIM_FUNC_ARG) {
		mode = ZEND_NATIVE_DIM_R;
	}
	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, expected_opcode, &operation)
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	/* The VM rejects an append fetch in read context before evaluating the
	 * container.  Optimized op arrays can expose this otherwise invalid shape,
	 * so preserve that ordering instead of first diagnosing an undefined CV. */
	if (opline->op2_type == IS_UNUSED
			&& (!write || mode == ZEND_NATIVE_DIM_UNSET)) {
		zend_throw_error(NULL, "Cannot use [] for reading");
		ZVAL_UNDEF(result);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, NULL);
		return zend_native_value_status();
	}
	container = zend_native_value_read_explicit(execute_data, opline,
		opline->op1_type, opline->op1);
	if (container == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (func_arg_by_ref
			&& (opline->op1_type == IS_CONST
				|| opline->op1_type == IS_TMP_VAR)) {
		zend_throw_error(NULL,
			"Cannot use temporary expression in write context");
		ZVAL_UNDEF(result);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, NULL);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, NULL);
		return zend_native_value_status();
	}
	if ((mode == ZEND_NATIVE_DIM_R || mode == ZEND_NATIVE_DIM_RW
			|| mode == ZEND_NATIVE_DIM_UNSET)
			&& opline->op1_type == IS_CV
			&& Z_TYPE_P(container) == IS_UNDEF) {
		if (mode == ZEND_NATIVE_DIM_RW) {
			uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

			if (variable_index >= execute_data->func->op_array.last_var) {
				return ZEND_NATIVE_EXCEPTION;
			}
			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
			container_warning_exception = EG(exception);
		} else if (zend_native_value_read_r_explicit(execute_data, opline,
				opline->op1_type, opline->op1) == NULL
				|| EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	{
		zval *guarded_container = container;

		ZVAL_DEREF(guarded_container);
		if (Z_TYPE_P(guarded_container) == IS_OBJECT) {
			protected_object = Z_OBJ_P(guarded_container);
			GC_ADDREF(protected_object);
		}
	}
	offset = opline->op2_type == IS_UNUSED ? NULL
		: zend_native_value_read_explicit(
			execute_data, opline, opline->op2_type, opline->op2);
	if (opline->op2_type != IS_UNUSED
			&& (offset == NULL
				|| (EG(exception) != NULL
					&& EG(exception) != container_warning_exception))) {
		if (protected_object != NULL) {
			if (GC_DELREF(protected_object) == 0) {
				zend_objects_store_del(protected_object);
			}
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	if (protected_object != NULL) {
		ZVAL_OBJ(&protected_container, protected_object);
		container = &protected_container;
	}
	if (write && Z_ISREF_P(container)) {
		container_reference = Z_REF_P(container);
	}
	if (opline->op1_type == IS_CV || opline->op1_type == IS_VAR) {
		ZVAL_DEREF(container);
	}
	if (mode == ZEND_NATIVE_DIM_UNSET && Z_TYPE_P(container) <= IS_FALSE) {
		/* BP_VAR_UNSET never promotes an undefined, null, or false container.
		 * In particular, an optimized later assignment may legitimately assume
		 * that an undefined CV is still undefined after this fetch. */
		if (Z_TYPE_P(container) == IS_FALSE) {
			zend_false_to_array_deprecated();
		}
		ZVAL_NULL(result);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, result);
		zend_native_value_consume_write_container(execute_data,
			opline->op1_type, opline->op1, result);
		return zend_native_value_status();
	}
	if (write && Z_TYPE_P(container) <= IS_FALSE
			&& ((container_reference != NULL
				&& ZEND_REF_HAS_TYPE_SOURCES(container_reference)
				&& !zend_verify_ref_array_assignable(container_reference))
				|| !zend_native_value_promote_to_array(container, 8, &table))) {
		ZVAL_NULL(result);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, result);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, result);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) == IS_STRING) {
		if (!write) {
			zval string_container;
			zend_string *string = Z_STR_P(container);

			if (opline->op2_type == IS_CV
					&& Z_TYPE_P(offset) == IS_UNDEF) {
				zend_string_addref(string);
				offset = zend_native_value_read_r_explicit(
					execute_data, opline, opline->op2_type, opline->op2);
				if (zend_string_delref(string) == 0) {
					zend_string_efree(string);
					ZVAL_NULL(result);
					goto fetch_dim_complete;
				}
				if (offset == NULL || EG(exception) != NULL) {
					goto fetch_dim_error;
				}
			}
			ZVAL_STR(&string_container, string);
			zend_fetch_dimension_const(result, &string_container, offset,
				mode == ZEND_NATIVE_DIM_IS ? BP_VAR_IS : BP_VAR_R);
		fetch_dim_complete:
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, result);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
			return zend_native_value_status();
		}
		if (opline->op2_type == IS_UNUSED) {
			zend_throw_error(NULL, "[] operator not supported for strings");
			goto fetch_dim_error;
		}
		if (!zend_native_string_offset(
				offset,
				mode == ZEND_NATIVE_DIM_UNSET ? BP_VAR_UNSET : BP_VAR_RW,
				&string_offset)) {
			goto fetch_dim_error;
		}
		zend_wrong_string_offset_error();
		goto fetch_dim_error;
	}
	if (Z_TYPE_P(container) == IS_OBJECT) {
		zend_object *object = Z_OBJ_P(container);
		zval *returned;
		zval undefined_offset;

		if (opline->op2_type == IS_CV
				&& Z_TYPE_P(offset) == IS_UNDEF) {
			offset = zend_native_value_read_r_explicit(
				execute_data, opline, opline->op2_type, opline->op2);
			if (offset == NULL || EG(exception) != NULL) {
				if (GC_DELREF(object) == 0) {
					zend_objects_store_del(object);
				}
				goto fetch_dim_error;
			}
		}
		offset = zend_native_value_object_dimension_offset(
			opline->op2_type, offset);
		if (offset != NULL && Z_TYPE_P(offset) == IS_UNDEF) {
			/* ArrayAccess observes an undefined dimension as a supplied null
			 * argument.  Do not let an UNDEF payload look like an omitted argument
			 * when the userland offsetGet() method enters natively. */
			ZVAL_NULL(&undefined_offset);
			offset = &undefined_offset;
		}
		if (protected_object == NULL) {
			GC_ADDREF(object);
		}
		ZVAL_UNDEF(result);
		returned = object->handlers->read_dimension(
			object, offset,
			mode == ZEND_NATIVE_DIM_W ? BP_VAR_W
				: mode == ZEND_NATIVE_DIM_RW ? BP_VAR_RW
				: mode == ZEND_NATIVE_DIM_UNSET ? BP_VAR_UNSET
				: mode == ZEND_NATIVE_DIM_IS ? BP_VAR_IS : BP_VAR_R,
			result);
		if (write) {
			if (returned == &EG(uninitialized_zval)) {
				ZVAL_NULL(result);
				zend_error(E_NOTICE,
					"Indirect modification of overloaded element of %s has no effect",
					ZSTR_VAL(object->ce->name));
			} else if (returned != NULL && Z_TYPE_P(returned) != IS_UNDEF) {
				if (!Z_ISREF_P(returned)) {
					if (result != returned) {
						zend_native_zval_copy_deref_or_dup(result, returned);
						returned = result;
					}
					if (Z_TYPE_P(returned) != IS_OBJECT) {
						zend_error(E_NOTICE,
							"Indirect modification of overloaded element of %s has no effect",
							ZSTR_VAL(object->ce->name));
					}
				} else if (Z_REFCOUNT_P(returned) == 1) {
					ZVAL_UNREF(returned);
				}
				if (result != returned) {
					ZVAL_INDIRECT(result, returned);
				}
			} else {
				ZVAL_UNDEF(result);
			}
		} else if (returned != NULL) {
			if (result != returned) {
				zend_native_zval_copy_deref_or_dup(result, returned);
			} else if (Z_ISREF_P(returned)) {
				zend_unwrap_reference(result);
			}
		} else {
			ZVAL_NULL(result);
		}
		if (GC_DELREF(object) == 0) {
			zend_objects_store_del(object);
		}
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, result);
		if (write) {
			zend_native_value_consume_write_container(execute_data,
				opline->op1_type, opline->op1, result);
		} else {
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
		}
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		if (opline->op2_type == IS_CV
				&& Z_TYPE_P(offset) == IS_UNDEF) {
			offset = zend_native_value_read_r_explicit(
				execute_data, opline, opline->op2_type, opline->op2);
			if (offset == NULL || EG(exception) != NULL) {
				goto fetch_dim_error;
			}
		}
		if (mode == ZEND_NATIVE_DIM_IS) {
			ZVAL_NULL(result);
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, result);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
			return ZEND_NATIVE_RETURNED;
		} else if (write) {
			if (mode == ZEND_NATIVE_DIM_UNSET) {
				zend_throw_error(NULL,
					"Cannot unset offset in a non-array variable");
			} else {
				zend_throw_error(NULL,
					"Cannot use a scalar value as an array");
			}
		} else {
			zend_error(E_WARNING, "Trying to access array offset on %s",
				zend_zval_value_name(container));
			ZVAL_NULL(result);
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, result);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
			return zend_native_value_status();
		}
		goto fetch_dim_error;
	}
	if (write) {
		SEPARATE_ARRAY(container);
	}
	table = Z_ARRVAL_P(container);
	if (opline->op2_type == IS_UNUSED) {
		ZEND_ASSERT(write && mode != ZEND_NATIVE_DIM_UNSET);
		value = zend_hash_next_index_insert(table, &EG(uninitialized_zval));
	} else {
		if (!zend_native_array_key_from_explicit_zval_protected(
				execute_data, opline, table, write,
				offset, opline->op2_type, opline->op2, true,
				mode == ZEND_NATIVE_DIM_IS || mode == ZEND_NATIVE_DIM_UNSET
					? BP_VAR_R
					: write ? BP_VAR_RW : BP_VAR_R,
				&key, &table_valid)) {
			goto fetch_dim_error;
		}
		if (!table_valid) {
			value = NULL;
		} else if (mode == ZEND_NATIVE_DIM_W) {
			value = zend_native_array_write_slot(
				table, &key, false);
		} else if (mode == ZEND_NATIVE_DIM_RW) {
			value = zend_native_array_write_slot(
				table, &key, true);
		} else {
			value = zend_native_array_find(table, &key);
		}
	}
	if (write) {
		if (value == NULL) {
			if (opline->op2_type == IS_UNUSED) {
				zend_cannot_add_element();
			}
			ZVAL_NULL(result);
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, result);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
			return zend_native_value_status();
		}
		ZVAL_INDIRECT(result, value);
	} else if (value == NULL) {
		if (mode == ZEND_NATIVE_DIM_R) {
			zend_native_array_warn_missing(&key);
		}
		ZVAL_NULL(result);
	} else {
		zend_native_zval_copy_deref_or_dup(result, value);
	}
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, result);
	if (write) {
		zend_native_value_consume_write_container(execute_data,
			opline->op1_type, opline->op1, result);
	} else {
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, result);
	}
	return zend_native_value_status();

fetch_dim_error:
	ZVAL_UNDEF(result);
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, NULL);
	zend_native_value_consume_operand(execute_data,
		opline->op1_type, opline->op1, NULL);
	return zend_native_value_status();
}

#define ZEND_NATIVE_DIM_WRAPPER(name, opcode, mode) \
	zend_native_status name( \
			zend_execute_data *execute_data, \
			uint64_t op1, uint64_t op2, uint64_t result, \
			uint32_t extended_value, uint32_t source_opcode, \
			uint32_t source_position_id) \
	{ \
		return zend_native_value_fetch_dim_impl( \
			execute_data, op1, op2, result, extended_value, source_opcode, \
			source_position_id, opcode, mode); \
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_native_array_key key;
	zval *container_slot;
	zval *container;
	zval *offset;
	zval *result;
	zval *element;
	zend_reference *container_reference = NULL;
	HashTable *promoted_table = NULL;
	zend_long string_offset;
	bool writable;

	if ((source_opcode != ZEND_FETCH_LIST_R
			&& source_opcode != ZEND_FETCH_LIST_W)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation)
			|| (container = zend_native_value_read_explicit(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (offset = zend_native_value_read_r_explicit(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	writable = opline->opcode == ZEND_FETCH_LIST_W;
	container_slot = writable
		? zend_native_value_slot(execute_data, opline->op1_type, opline->op1)
		: NULL;
	if (writable && container_slot == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (writable && opline->op1_type == IS_VAR
			&& Z_TYPE_P(container_slot) != IS_INDIRECT
			&& !Z_ISREF_P(container)) {
		zend_error(E_NOTICE,
			"Attempting to set reference to non referenceable value");
		writable = false;
	}
	if (writable && Z_ISREF_P(container)) {
		container_reference = Z_REF_P(container);
	}
	ZVAL_DEREF(container);
	if (writable && Z_TYPE_P(container) <= IS_FALSE) {
		if (container_reference != NULL
				&& ZEND_REF_HAS_TYPE_SOURCES(container_reference)
				&& !zend_verify_ref_array_assignable(container_reference)) {
			ZVAL_UNDEF(result);
			zend_native_value_consume_operand(
				execute_data, opline->op2_type, opline->op2, NULL);
			return zend_native_value_status();
		}
		if (!zend_native_value_promote_to_array(
				container, 8, &promoted_table)) {
			ZVAL_UNDEF(result);
			zend_native_value_consume_operand(
				execute_data, opline->op2_type, opline->op2, NULL);
			return zend_native_value_status();
		}
	}
	if (writable && Z_TYPE_P(container) == IS_STRING) {
		if (zend_native_string_offset(offset, BP_VAR_W, &string_offset)) {
			zend_wrong_string_offset_error();
		}
		ZVAL_UNDEF(result);
		zend_native_value_consume_operand(
			execute_data, opline->op2_type, opline->op2, NULL);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) == IS_OBJECT) {
		zend_object *object = Z_OBJ_P(container);
		zval *returned;

		offset = zend_native_value_object_dimension_offset(
			opline->op2_type, offset);
		GC_ADDREF(object);
		ZVAL_UNDEF(result);
		returned = object->handlers->read_dimension(
			object, offset, writable ? BP_VAR_W : BP_VAR_R, result);
		if (writable) {
			if (returned == &EG(uninitialized_zval)) {
				ZVAL_NULL(result);
				zend_error(E_NOTICE,
					"Indirect modification of overloaded element of %s has no effect",
					ZSTR_VAL(object->ce->name));
			} else if (returned != NULL && Z_TYPE_P(returned) != IS_UNDEF) {
				if (!Z_ISREF_P(returned)) {
					if (result != returned) {
						zend_native_zval_copy_deref_or_dup(result, returned);
						returned = result;
					}
					if (Z_TYPE_P(returned) != IS_OBJECT) {
						zend_error(E_NOTICE,
							"Indirect modification of overloaded element of %s has no effect",
							ZSTR_VAL(object->ce->name));
					}
				} else if (Z_REFCOUNT_P(returned) == 1) {
					ZVAL_UNREF(returned);
				}
				if (result != returned) {
					ZVAL_INDIRECT(result, returned);
				}
			} else {
				ZVAL_UNDEF(result);
			}
		} else if (returned != NULL) {
			if (result != returned) {
				zend_native_zval_copy_deref_or_dup(result, returned);
			} else if (Z_ISREF_P(returned)) {
				zend_unwrap_reference(result);
			}
		} else {
			ZVAL_NULL(result);
		}
		if (GC_DELREF(object) == 0) {
			zend_objects_store_del(object);
		}
		zend_native_value_consume_operand(
			execute_data, opline->op2_type, opline->op2, result);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		if (Z_TYPE_P(container) > IS_NULL) {
			zend_error(E_WARNING, "Cannot use %s as array",
				zend_zval_type_name(container));
		}
		ZVAL_NULL(result);
		zend_native_value_consume_operand(
			execute_data, opline->op2_type, opline->op2, result);
		return zend_native_value_status();
	}
	if (!zend_native_array_key_from_zval(
			offset, opline->op2_type, true,
			writable ? BP_VAR_W : BP_VAR_R, &key)) {
		return ZEND_NATIVE_EXCEPTION;
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
			zend_native_zval_copy_deref_or_dup(result, element);
		}
	}
	zend_native_value_consume_operand(
		execute_data, opline->op2_type, opline->op2, result);
	return zend_native_value_status();
}

static zend_native_status zend_native_value_assign_dim_impl(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand, uint64_t auxiliary,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, bool compound)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_native_array_key key;
	HashTable *table = NULL;
	zval *container;
	zval *offset;
	zval *target;
	zval *result = NULL;
	zval source;
	zval computed;
	zval *assigned;
	zend_refcounted *garbage = NULL;
	zend_reference *container_reference = NULL;
	binary_op_type binary_operation;
	zend_long string_offset;
	bool table_valid = true;

	if (!zend_native_value_init_explicit_dim_assignment(
			execute_data, op1, op2, result_operand, auxiliary,
			extended_value, source_opcode, source_position_id,
			compound ? ZEND_ASSIGN_DIM_OP : ZEND_ASSIGN_DIM, &operation)
			|| (container = zend_native_value_read_explicit(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (compound && opline->op1_type == IS_CV
			&& Z_TYPE_P(container) == IS_UNDEF) {
		if (zend_native_value_read_r_explicit(execute_data, opline,
				opline->op1_type, opline->op1) == NULL
				|| EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	if (Z_ISREF_P(container)) {
		container_reference = Z_REF_P(container);
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) == IS_STRING) {
		if (compound) {
			if (opline->op2_type == IS_UNUSED) {
				zend_throw_error(NULL,
					"[] operator not supported for strings");
				goto assign_dim_error;
			}
			if (opline->op2_type != IS_UNUSED) {
				offset = zend_native_value_read_r_explicit(execute_data, opline,
					opline->op2_type, opline->op2);
				if (offset == NULL || EG(exception) != NULL
						|| !zend_native_string_offset(
							offset, BP_VAR_RW, &string_offset)) {
					goto assign_dim_error;
				}
			}
			zend_wrong_string_offset_error();
			goto assign_dim_error;
		}
		offset = opline->op2_type == IS_UNUSED ? NULL
			: zend_native_value_read_explicit(execute_data, opline,
				opline->op2_type, opline->op2);
		if (opline->op2_type != IS_UNUSED
				&& offset == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (!zend_native_value_take_explicit(execute_data, opline,
				opline->auxiliary_type, opline->auxiliary, false, &source)) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (!zend_native_assign_string_offset(execute_data, opline,
				container, offset, &source, &assigned)) {
			zval_ptr_dtor_nogc(&source);
			if (opline->result_type != IS_UNUSED) {
				result = zend_native_value_slot(
					execute_data, opline->result_type, opline->result);
				if (result != NULL) {
					if (EG(exception) != NULL) {
						ZVAL_UNDEF(result);
					} else {
						ZVAL_NULL(result);
					}
				}
			}
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, result);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
			return zend_native_value_status();
		}
		zval_ptr_dtor_nogc(&source);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, assigned);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, assigned);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) == IS_OBJECT) {
		zend_object *object = Z_OBJ_P(container);
		zval read_value;
		zval *current = NULL;

		/* Reading an undefined offset may invoke user code that overwrites the
		 * container slot.  Keep the original object alive while resolving every
		 * remaining operand and perform the detached write on that object. */
		GC_ADDREF(object);
		offset = opline->op2_type == IS_UNUSED ? NULL
			: zend_native_value_read_r_explicit(execute_data, opline,
				opline->op2_type, opline->op2);
		offset = zend_native_value_object_dimension_offset(
			opline->op2_type, offset);
		if ((opline->op2_type != IS_UNUSED
				&& (offset == NULL || EG(exception) != NULL))
				|| !zend_native_value_take_explicit(execute_data, opline,
					opline->auxiliary_type, opline->auxiliary,
					false, &source)) {
			if (GC_DELREF(object) == 0) {
				zend_objects_store_del(object);
			}
			goto assign_dim_error;
		}
		if (compound) {
			ZVAL_UNDEF(&read_value);
			current = object->handlers->read_dimension(
				object, offset, BP_VAR_R, &read_value);
			binary_operation = get_binary_op(opline->extended_value);
			if (current == NULL || binary_operation == NULL
					|| binary_operation(
						&computed, current, &source) != SUCCESS) {
				if (current == NULL && EG(exception) == NULL) {
					zend_throw_error(NULL, "Cannot use object of type %s as array",
						ZSTR_VAL(object->ce->name));
				}
				if (current == &read_value && !Z_ISUNDEF(read_value)) {
					zval_ptr_dtor_nogc(&read_value);
				}
				zval_ptr_dtor_nogc(&source);
				if (GC_DELREF(object) == 0) {
					zend_objects_store_del(object);
				}
				zend_native_value_consume_operand(execute_data,
					opline->op2_type, opline->op2, NULL);
				zend_native_value_consume_operand(execute_data,
					opline->op1_type, opline->op1, NULL);
				return ZEND_NATIVE_EXCEPTION;
			}
			object->handlers->write_dimension(object, offset, &computed);
			assigned = &computed;
			if (current == &read_value && !Z_ISUNDEF(read_value)) {
				zval_ptr_dtor_nogc(&read_value);
			}
		} else {
			object->handlers->write_dimension(object, offset, &source);
			assigned = &source;
		}
		if (opline->result_type != IS_UNUSED) {
			result = zend_native_value_slot(
				execute_data, opline->result_type, opline->result);
			if (result == NULL) {
				zval_ptr_dtor_nogc(assigned);
				if (compound) {
					zval_ptr_dtor_nogc(&source);
				}
				if (GC_DELREF(object) == 0) {
					zend_objects_store_del(object);
				}
				zend_native_value_consume_operand(execute_data,
					opline->op2_type, opline->op2, NULL);
				zend_native_value_consume_operand(execute_data,
					opline->op1_type, opline->op1, NULL);
				return ZEND_NATIVE_EXCEPTION;
			}
			ZVAL_COPY(result, assigned);
		}
		zval_ptr_dtor_nogc(assigned);
		if (compound) {
			zval_ptr_dtor_nogc(&source);
		}
		if (GC_DELREF(object) == 0) {
			zend_objects_store_del(object);
		}
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, result);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, result);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) <= IS_FALSE
			&& ((container_reference != NULL
				&& ZEND_REF_HAS_TYPE_SOURCES(container_reference)
				&& !zend_verify_ref_array_assignable(container_reference))
				|| !zend_native_value_promote_to_array(container, 8, &table))) {
		goto assign_dim_error;
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		zend_throw_error(NULL, "Cannot use a scalar value as an array");
		goto assign_dim_error;
	}
	SEPARATE_ARRAY(container);
	table = Z_ARRVAL_P(container);
	if (opline->op2_type == IS_UNUSED && !compound) {
		zval *source_slot = NULL;
		bool protect_table = false;
		bool table_owned = true;

		/* ASSIGN_DIM [] transfers the RHS directly into the new bucket. An
		 * intermediate EG(uninitialized_zval) bucket can shallow-copy a typed
		 * reference and gives zend_assign_to_variable_ex() false authority. */
		if (opline->auxiliary_type == IS_CV) {
			source_slot = zend_native_value_read_explicit(
				execute_data, opline,
				opline->auxiliary_type, opline->auxiliary);
			protect_table = source_slot != NULL
				&& Z_TYPE_P(source_slot) == IS_UNDEF
				&& !(GC_FLAGS(table) & IS_ARRAY_IMMUTABLE);
		}
		if (protect_table) {
			/* The undefined-CV warning may run user code that replaces the
			 * destination.  Keep its former array alive until the warning has
			 * completed, then only append if the destination still owns it. */
			GC_ADDREF(table);
		}
		if (!zend_native_value_take_explicit(
				execute_data, opline, opline->auxiliary_type,
				opline->auxiliary, false, &source)) {
			if (protect_table && GC_DELREF(table) == 0) {
				zend_array_destroy(table);
			}
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, NULL);
			return ZEND_NATIVE_EXCEPTION;
		}
		if (protect_table) {
			table_owned = Z_TYPE_P(container) == IS_ARRAY
				&& Z_ARRVAL_P(container) == table;
			if (GC_DELREF(table) == 0) {
				zend_array_destroy(table);
				table_owned = false;
			}
			if (!table_owned) {
				zval_ptr_dtor_nogc(&source);
				goto assign_dim_error_without_auxiliary;
			}
		}
		target = zend_hash_next_index_insert(table, &source);
		if (UNEXPECTED(target == NULL)) {
			zend_cannot_add_element();
			zval_ptr_dtor_nogc(&source);
			goto assign_dim_error_without_auxiliary;
		}
		assigned = target;
	} else {
		if (opline->op2_type == IS_UNUSED) {
			target = zend_hash_next_index_insert(
				table, &EG(uninitialized_zval));
			if (UNEXPECTED(target == NULL)) {
				zend_cannot_add_element();
			}
		} else {
			offset = zend_native_value_read_explicit(execute_data, opline,
				opline->op2_type, opline->op2);
			if (offset == NULL
					|| !zend_native_array_key_from_explicit_zval_protected(
					execute_data, opline, table, true,
					offset, opline->op2_type, opline->op2,
					true, BP_VAR_W,
					&key, &table_valid)) {
				goto assign_dim_error;
			}
			if (!table_valid) {
				goto assign_dim_error;
			}
			target = zend_native_array_write_slot(table, &key, compound);
		}
		if (target == NULL) {
			goto assign_dim_error;
		}
		if (!zend_native_value_take_explicit(
				execute_data, opline, opline->auxiliary_type,
				opline->auxiliary, false, &source)) {
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, NULL);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, NULL);
			return ZEND_NATIVE_EXCEPTION;
		}
		if (compound) {
			zval *current = target;

			if (Z_ISREF_P(current)) {
				current = Z_REFVAL_P(current);
			}
			binary_operation = get_binary_op(opline->extended_value);
			if (binary_operation == NULL
					|| binary_operation(
						&computed, current, &source) != SUCCESS) {
				zval_ptr_dtor_nogc(&source);
				zend_native_value_consume_operand(execute_data,
					opline->op2_type, opline->op2, NULL);
				zend_native_value_consume_operand(execute_data,
					opline->op1_type, opline->op1, NULL);
				return ZEND_NATIVE_EXCEPTION;
			}
			zval_ptr_dtor_nogc(&source);
			assigned = zend_assign_to_variable_ex(
				target, &computed, IS_TMP_VAR,
				ZEND_CALL_USES_STRICT_TYPES(execute_data), &garbage);
		} else {
			assigned = zend_assign_to_variable_ex(
				target, &source, IS_TMP_VAR,
				ZEND_CALL_USES_STRICT_TYPES(execute_data), &garbage);
		}
	}
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result == NULL) {
			if (garbage != NULL) {
				GC_DTOR_NO_REF(garbage);
			}
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, NULL);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, NULL);
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_COPY(result, assigned);
	}
	if (garbage != NULL) {
		GC_DTOR_NO_REF(garbage);
	}
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, result);
	zend_native_value_consume_operand(execute_data,
		opline->op1_type, opline->op1, result);
	return zend_native_value_status();

assign_dim_error_without_auxiliary:
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result != NULL) {
			ZVAL_NULL(result);
		}
	}
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, result);
	zend_native_value_consume_operand(execute_data,
		opline->op1_type, opline->op1, result);
	return zend_native_value_status();

assign_dim_error:
	if (zend_native_value_take_explicit(execute_data, opline,
			opline->auxiliary_type, opline->auxiliary, false, &source)) {
		zval_ptr_dtor_nogc(&source);
	}
	if (opline->result_type != IS_UNUSED) {
		result = zend_native_value_slot(
			execute_data, opline->result_type, opline->result);
		if (result != NULL) {
			ZVAL_NULL(result);
		}
	}
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, result);
	zend_native_value_consume_operand(execute_data,
		opline->op1_type, opline->op1, result);
	return zend_native_value_status();
}

zend_native_status zend_native_value_assign_dim(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result, uint64_t auxiliary,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_assign_dim_impl(
		execute_data, op1, op2, result, auxiliary, extended_value,
		source_opcode, source_position_id, false);
}

zend_native_status zend_native_value_assign_dim_op(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result, uint64_t auxiliary,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	return zend_native_value_assign_dim_impl(
		execute_data, op1, op2, result, auxiliary, extended_value,
		source_opcode, source_position_id, true);
}

zend_native_status zend_native_value_unset_dim(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_native_array_key key;
	HashTable *table;
	zval *container;
	zval *offset;
	bool table_valid;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_UNSET_DIM, &operation)
			|| (container = zend_native_value_read_explicit(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (offset = zend_native_value_read_explicit(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CV
			&& Z_TYPE_P(container) == IS_UNDEF) {
		container = zend_native_value_read_r_explicit(
			execute_data, opline, opline->op1_type, opline->op1);
		if (container == NULL || EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) == IS_OBJECT) {
		offset = zend_native_value_object_dimension_offset(
			opline->op2_type, offset);
		Z_OBJ_HT_P(container)->unset_dimension(Z_OBJ_P(container), offset);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, NULL);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, NULL);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) != IS_ARRAY) {
		if (Z_TYPE_P(container) == IS_STRING) {
			zend_throw_error(NULL, "Cannot unset string offsets");
		} else if (Z_TYPE_P(container) > IS_FALSE) {
			zend_throw_error(NULL,
				"Cannot unset offset in a non-array variable");
		} else if (Z_TYPE_P(container) == IS_FALSE) {
			zend_false_to_array_deprecated();
		}
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, NULL);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, NULL);
		return zend_native_value_status();
	}
	SEPARATE_ARRAY(container);
	table = Z_ARRVAL_P(container);
	if (!zend_native_array_key_from_zval_protected(
			table, true, offset, opline->op2_type, false,
			BP_VAR_UNSET, &key, &table_valid)
			|| !table_valid) {
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, NULL);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, NULL);
		return zend_native_value_status();
	}
	if (key.kind == ZEND_NATIVE_ARRAY_KEY_LONG) {
		zend_hash_index_del(table, key.index);
	} else {
		zend_hash_del(table, key.string);
	}
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, NULL);
	zend_native_value_consume_operand(execute_data,
		opline->op1_type, opline->op1, NULL);
	return zend_native_value_status();
}

zend_native_status zend_native_value_isset_isempty_dim(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zend_native_array_key key;
	HashTable *table;
	zval *container;
	zval *offset;
	zval *result;
	zval *value = NULL;
	bool answer;
	bool table_valid;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id,
			ZEND_ISSET_ISEMPTY_DIM_OBJ, &operation)
			|| (container = zend_native_value_read_explicit(execute_data, opline,
				opline->op1_type, opline->op1)) == NULL
			|| (offset = zend_native_value_read_r_explicit(execute_data, opline,
				opline->op2_type, opline->op2)) == NULL
			|| (result = zend_native_value_slot(execute_data,
				opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (UNEXPECTED(EG(exception) != NULL)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_DEREF(container);
	if (Z_TYPE_P(container) == IS_OBJECT) {
		offset = zend_native_value_object_dimension_offset(
			opline->op2_type, offset);
		answer = (opline->extended_value & ZEND_ISEMPTY) != 0
			? !Z_OBJ_HT_P(container)->has_dimension(
				Z_OBJ_P(container), offset, 1)
			: Z_OBJ_HT_P(container)->has_dimension(
				Z_OBJ_P(container), offset, 0);
		ZVAL_BOOL(result, answer);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, result);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, result);
		return zend_native_value_status();
	}
	if (Z_TYPE_P(container) == IS_STRING) {
		zend_long string_offset;
		bool is_empty = (opline->extended_value & ZEND_ISEMPTY) != 0;

		ZVAL_DEREF(offset);
		if (Z_TYPE_P(offset) == IS_LONG) {
			string_offset = Z_LVAL_P(offset);
		} else if (Z_TYPE_P(offset) < IS_STRING
				|| (Z_TYPE_P(offset) == IS_STRING
					&& is_numeric_string(
						Z_STRVAL_P(offset), Z_STRLEN_P(offset),
						NULL, NULL, 0) == IS_LONG)) {
			string_offset = zval_get_long_ex(offset, true);
		} else {
			answer = is_empty;
			goto string_result;
		}
		if (string_offset < 0) {
			string_offset += (zend_long) Z_STRLEN_P(container);
		}
		if (string_offset >= 0
				&& (size_t) string_offset < Z_STRLEN_P(container)) {
			answer = is_empty
				? Z_STRVAL_P(container)[string_offset] == '0'
				: true;
		} else {
			answer = is_empty;
		}
string_result:
		ZVAL_BOOL(result, answer);
		zend_native_value_consume_operand(execute_data,
			opline->op2_type, opline->op2, result);
		zend_native_value_consume_operand(execute_data,
			opline->op1_type, opline->op1, result);
		return ZEND_NATIVE_RETURNED;
	}
	if (Z_TYPE_P(container) == IS_ARRAY) {
		table = Z_ARRVAL_P(container);
		if (!zend_native_array_key_from_zval_protected(
				table, false, offset, opline->op2_type, true,
				BP_VAR_IS, &key, &table_valid)) {
			zend_native_value_consume_operand(execute_data,
				opline->op2_type, opline->op2, result);
			zend_native_value_consume_operand(execute_data,
				opline->op1_type, opline->op1, result);
			return ZEND_NATIVE_EXCEPTION;
		}
		if (table_valid) {
			value = zend_native_array_find(table, &key);
		}
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
	zend_native_value_consume_operand(execute_data,
		opline->op2_type, opline->op2, result);
	zend_native_value_consume_operand(execute_data,
		opline->op1_type, opline->op1, result);
	return ZEND_NATIVE_RETURNED;
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
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline,
	zval *source_slot, zval *array, bool by_reference)
{
	zval *result = zend_native_value_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (!by_reference) {
		if (opline->op1_type == IS_TMP_VAR) {
			ZVAL_COPY_VALUE(result, array);
			ZVAL_UNDEF(source_slot);
		} else {
			zend_native_zval_copy_deref_or_dup(result, array);
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
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline,
	zval *source_slot, zval *object, bool by_reference)
{
	zend_object *zobj = Z_OBJ_P(object);
	zval *result = zend_native_value_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (UNEXPECTED(zend_object_is_lazy(zobj))) {
		zobj = zend_lazy_object_init(zobj);
		if (UNEXPECTED(EG(exception) != NULL)) {
			ZVAL_UNDEF(result);
			zend_native_iterator_release_operand(
				execute_data, opline->op1_type, opline->op1);
			return ZEND_NATIVE_ITERATOR_EXCEPTION;
		}
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
			if (opline->op1_type == IS_VAR) {
				zend_native_iterator_release_operand(
					execute_data, opline->op1_type, opline->op1);
			}
			return ZEND_NATIVE_ITERATOR_END;
		}
		Z_FE_ITER_P(result) = zend_hash_iterator_add(properties, 0);
	}
	if (opline->op1_type == IS_VAR) {
		zend_native_iterator_release_operand(
			execute_data, opline->op1_type, opline->op1);
	}
	return zend_hash_num_elements(Z_OBJPROP_P(object)) == 0
		? ZEND_NATIVE_ITERATOR_END : ZEND_NATIVE_ITERATOR_NEXT;
}

static zend_native_iterator_branch_result zend_native_iterator_reset(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline)
{
	zval *source_slot = zend_native_value_read_explicit(
		execute_data, opline, opline->op1_type, opline->op1);
	zval *value = source_slot;
	bool by_reference = opline->opcode == ZEND_FE_RESET_RW;

	if (source_slot == NULL || opline->result_type == IS_UNUSED) {
		return ZEND_NATIVE_ITERATOR_EXCEPTION;
	}
	if (opline->op1_type == IS_CV && Z_TYPE_P(source_slot) == IS_UNDEF
			&& zend_native_value_read_r_explicit(execute_data, opline,
				opline->op1_type, opline->op1) == NULL) {
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
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline,
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
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline,
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
		zval copy;

		zend_native_zval_copy_deref_or_dup(&copy, value);
		zend_assign_to_variable(destination, &copy, IS_TMP_VAR,
			ZEND_CALL_USES_STRICT_TYPES(execute_data));
	} else {
		zend_native_zval_copy_deref_or_dup(destination, value);
	}
	return EG(exception) == NULL;
}

static zend_native_iterator_branch_result zend_native_iterator_fetch_array(
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline,
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
	zend_execute_data *execute_data,
	const zend_native_explicit_value_operation *opline,
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
			bool declared_property = false;

			bucket = &table->arData[position++];
			value = &bucket->val;
			if (Z_TYPE_P(value) == IS_INDIRECT) {
				value = Z_INDIRECT_P(value);
				declared_property = true;
			}
			if (Z_TYPE_P(value) == IS_UNDEF
					|| (bucket->key != NULL
						&& zend_check_property_access(Z_OBJ_P(object),
							bucket->key, Z_TYPE(bucket->val) != IS_INDIRECT)
							!= SUCCESS)) {
				continue;
			}
			if (by_reference && declared_property && !Z_ISREF_P(value)) {
				zend_property_info *prop_info =
					zend_get_property_info_for_slot(Z_OBJ_P(object), value);

				if (prop_info != NULL) {
					if (UNEXPECTED(prop_info->flags & ZEND_ACC_READONLY)) {
						zend_throw_error(NULL,
							"Cannot acquire reference to readonly property %s::$%s",
							ZSTR_VAL(prop_info->ce->name),
							ZSTR_VAL(bucket->key));
						return ZEND_NATIVE_ITERATOR_EXCEPTION;
					}
					if (ZEND_TYPE_IS_SET(prop_info->type)) {
						ZVAL_NEW_REF(value, value);
						ZEND_REF_ADD_TYPE_SOURCE(Z_REF_P(value), prop_info);
					}
				}
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *holder;
	zval *value;
	bool by_reference;

	if ((source_opcode != ZEND_FE_RESET_R
			&& source_opcode != ZEND_FE_RESET_RW
			&& source_opcode != ZEND_FE_FETCH_R
			&& source_opcode != ZEND_FE_FETCH_RW)
			|| !zend_native_value_init_explicit_operation(
				execute_data, op1, op2, result_operand, extended_value,
				source_opcode, source_position_id, (uint8_t) source_opcode,
				&operation)) {
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
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_value_operation operation;
	const zend_native_explicit_value_operation *opline = &operation;
	zval *value;

	if (!zend_native_value_init_explicit_operation(
			execute_data, op1, op2, result_operand, extended_value,
			source_opcode, source_position_id, ZEND_FE_FREE, &operation)
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
