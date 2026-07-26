/* Exact Zend generator lifecycle semantics for native frames. */

#include "Zend/Native/Runtime/Common/zend_native_generators.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_generators.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_observer.h"

#include "Zend/Native/Lowering/zend_mir_lowering_source.h"

#include <string.h>

typedef struct _zend_native_generator_operation {
	uint8_t op1_type;
	uint8_t op2_type;
	uint8_t result_type;
	znode_op op1;
	znode_op op2;
	znode_op result;
	const zend_op *opline;
} zend_native_generator_operation;

static bool zend_native_generator_decode_operand(
	zend_execute_data *execute_data, uint64_t encoded,
	uint8_t *operand_type, znode_op *operand)
{
	zend_mir_source_operand_kind kind =
		(zend_mir_source_operand_kind) (encoded & UINT64_C(0xff));
	zend_mir_source_slot_kind slot_kind =
		(zend_mir_source_slot_kind) ((encoded >> 8) & UINT64_C(0xff));
	uint32_t index = (uint32_t) (encoded >> 16);
	uint32_t physical_slot;

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
	operand->var =
		((uint32_t) ZEND_CALL_FRAME_SLOT + physical_slot) * sizeof(zval);
	return true;
}

static bool zend_native_generator_init_operation(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t source_opcode, uint32_t source_position_id,
	uint8_t expected_opcode, zend_native_generator_operation *operation)
{
	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opcode != expected_opcode
			|| source_position_id >= execute_data->func->op_array.last
			|| !zend_native_generator_decode_operand(
				execute_data, op1, &operation->op1_type, &operation->op1)
			|| !zend_native_generator_decode_operand(
				execute_data, op2, &operation->op2_type, &operation->op2)
			|| !zend_native_generator_decode_operand(
				execute_data, result,
				&operation->result_type, &operation->result)) {
		return false;
	}
	operation->opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	execute_data->opline = operation->opline;
	return true;
}

static zend_always_inline zval *zend_native_generator_operand(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	if (type == IS_CONST) {
		return &execute_data->func->op_array.literals[operand.constant];
	}
	return type == IS_UNUSED ? NULL : ZEND_CALL_VAR(execute_data, operand.var);
}

static zend_always_inline void zend_native_generator_free_operand(
	zval *value, uint8_t type)
{
	if (type == IS_TMP_VAR || type == IS_VAR) {
		zval_ptr_dtor_nogc(value);
	}
}

static zend_always_inline zend_generator *zend_native_running_generator(
	zend_execute_data *execute_data)
{
	return (ZEND_CALL_INFO(execute_data) & ZEND_CALL_GENERATOR) != 0
		? (zend_generator *) execute_data->return_value : NULL;
}

void zend_native_generator_release_generation(zend_generator *generator)
{
	zend_native_entry_cell *cell;

	if (generator == NULL
			|| (cell = generator->native_entry_cell) == NULL) {
		return;
	}
	ZEND_ASSERT(cell->generation == generator->native_entry_generation);
	zend_native_entry_cell_release_suspended(cell);
	generator->native_entry_cell = NULL;
	generator->native_entry_generation = 0;
}

zend_native_status zend_native_generator_create(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_generator_operation operation;
	zend_generator *generator;
	zend_execute_data *generator_frame;
	zend_native_entry_cell *entry_cell;
	uint32_t num_args;
	uint32_t used_stack;
	uint32_t copied_stack;
	uint32_t call_info;

	(void) extended_value;
	if (!zend_native_generator_init_operation(
			execute_data, op1, op2, result, source_opcode,
			source_position_id, ZEND_GENERATOR_CREATE, &operation)
			|| execute_data->return_value == NULL) {
		zend_throw_error(NULL, "Invalid native generator creation frame");
		return ZEND_NATIVE_EXCEPTION;
	}
	entry_cell = zend_native_reentry_resolve(execute_data->func);
	if (entry_cell == NULL
			|| zend_native_entry_cell_load(entry_cell) == NULL) {
		zend_throw_error(NULL,
			"Native generator has no published code generation");
		return ZEND_NATIVE_EXCEPTION;
	}
	object_init_ex(execute_data->return_value, zend_ce_generator);
	num_args = ZEND_CALL_NUM_ARGS(execute_data);
	if (EXPECTED(num_args <= execute_data->func->op_array.num_args)) {
		used_stack = (ZEND_CALL_FRAME_SLOT
			+ execute_data->func->op_array.last_var
			+ execute_data->func->op_array.T) * sizeof(zval);
		copied_stack = (ZEND_CALL_FRAME_SLOT
			+ execute_data->func->op_array.last_var) * sizeof(zval);
	} else {
		used_stack = (ZEND_CALL_FRAME_SLOT + num_args
			+ execute_data->func->op_array.last_var
			+ execute_data->func->op_array.T
			- execute_data->func->op_array.num_args) * sizeof(zval);
		copied_stack = used_stack;
	}
	generator_frame = (zend_execute_data *) emalloc(used_stack);
	memcpy(generator_frame, execute_data, copied_stack);
	if (copied_stack < used_stack) {
		memset((char *) generator_frame + copied_stack, 0,
			used_stack - copied_stack);
	}

	generator = (zend_generator *) Z_OBJ_P(execute_data->return_value);
	generator->func = generator_frame->func;
	generator->execute_data = generator_frame;
	generator->frozen_call_stack = NULL;
	generator->execute_fake.opline = NULL;
	generator->execute_fake.func = NULL;
	generator->execute_fake.prev_execute_data = NULL;
	ZVAL_OBJ(&generator->execute_fake.This, (zend_object *) generator);

	generator_frame->opline = operation.opline;
	generator_frame->return_value = (zval *) generator;
	call_info = Z_TYPE_INFO(generator_frame->This);
	if ((call_info & Z_TYPE_MASK) == IS_OBJECT
			&& (!(call_info & (ZEND_CALL_CLOSURE | ZEND_CALL_RELEASE_THIS))
				|| UNEXPECTED(zend_execute_ex != execute_ex))) {
		ZEND_ADD_CALL_FLAG_EX(call_info, ZEND_CALL_RELEASE_THIS);
		Z_ADDREF(generator_frame->This);
	}
	ZEND_ADD_CALL_FLAG_EX(call_info,
		ZEND_CALL_TOP_FUNCTION | ZEND_CALL_ALLOCATED | ZEND_CALL_GENERATOR);
	Z_TYPE_INFO(generator_frame->This) = call_info;
	generator_frame->prev_execute_data = NULL;
	generator->native_entry_cell = entry_cell;
	generator->native_entry_generation =
		generator->native_entry_cell->generation;
	zend_native_entry_cell_retain_suspended(
		generator->native_entry_cell);
	EG(current_execute_data) = execute_data->prev_execute_data;
	return ZEND_NATIVE_GENERATOR_CREATED;
}

zend_native_status zend_native_generator_yield(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_generator_operation operation;
	zend_generator *generator;
	zval *value;

	if (!zend_native_generator_init_operation(
			execute_data, op1, op2, result, source_opcode,
			source_position_id, ZEND_YIELD, &operation)
			|| (generator = zend_native_running_generator(execute_data))
				== NULL) {
		zend_throw_error(NULL, "Invalid native generator yield frame");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (UNEXPECTED(generator->flags & ZEND_GENERATOR_FORCED_CLOSE)) {
		zend_throw_error(NULL,
			"Cannot yield from finally in a force-closed generator");
		return ZEND_NATIVE_EXCEPTION;
	}
	zval_ptr_dtor(&generator->value);
	zval_ptr_dtor(&generator->key);

	value = zend_native_generator_operand(
		execute_data, operation.op1_type, operation.op1);
	if (operation.op1_type == IS_UNUSED) {
		ZVAL_NULL(&generator->value);
	} else if ((execute_data->func->op_array.fn_flags
				& ZEND_ACC_RETURN_REFERENCE) != 0) {
		if ((operation.op1_type & (IS_CONST | IS_TMP_VAR)) != 0) {
			zend_error(E_NOTICE,
				"Only variable references should be yielded by reference");
			ZVAL_COPY_VALUE(&generator->value, value);
			if (operation.op1_type == IS_CONST
					&& Z_OPT_REFCOUNTED(generator->value)) {
				Z_ADDREF(generator->value);
			}
		} else if (operation.op1_type == IS_VAR
				&& extended_value == ZEND_RETURNS_FUNCTION
				&& !Z_ISREF_P(value)) {
			zend_error(E_NOTICE,
				"Only variable references should be yielded by reference");
			ZVAL_COPY(&generator->value, value);
			zend_native_generator_free_operand(value, operation.op1_type);
		} else {
			if (Z_ISREF_P(value)) {
				Z_ADDREF_P(value);
			} else {
				ZVAL_MAKE_REF_EX(value, 2);
			}
			ZVAL_REF(&generator->value, Z_REF_P(value));
			zend_native_generator_free_operand(value, operation.op1_type);
		}
	} else if (operation.op1_type == IS_CONST) {
		ZVAL_COPY(&generator->value, value);
	} else if (operation.op1_type == IS_TMP_VAR) {
		ZVAL_COPY_VALUE(&generator->value, value);
	} else if ((operation.op1_type & (IS_VAR | IS_CV))
			&& Z_ISREF_P(value)) {
		ZVAL_COPY(&generator->value, Z_REFVAL_P(value));
		if (operation.op1_type == IS_VAR) {
			zval_ptr_dtor_nogc(value);
		}
	} else {
		ZVAL_COPY_VALUE(&generator->value, value);
		if (operation.op1_type == IS_CV && Z_OPT_REFCOUNTED_P(value)) {
			Z_ADDREF_P(value);
		}
	}

	if (operation.op2_type != IS_UNUSED) {
		zval *key = zend_native_generator_operand(
			execute_data, operation.op2_type, operation.op2);
		if ((operation.op2_type & (IS_CV | IS_VAR))
				&& Z_TYPE_P(key) == IS_REFERENCE) {
			key = Z_REFVAL_P(key);
		}
		ZVAL_COPY(&generator->key, key);
		zend_native_generator_free_operand(
			zend_native_generator_operand(
				execute_data, operation.op2_type, operation.op2),
			operation.op2_type);
		if (Z_TYPE(generator->key) == IS_LONG
				&& Z_LVAL(generator->key)
					> generator->largest_used_integer_key) {
			generator->largest_used_integer_key = Z_LVAL(generator->key);
		}
	} else {
		generator->largest_used_integer_key++;
		ZVAL_LONG(&generator->key, generator->largest_used_integer_key);
	}
	if (operation.result_type != IS_UNUSED) {
		generator->send_target = zend_native_generator_operand(
			execute_data, operation.result_type, operation.result);
		ZVAL_NULL(generator->send_target);
	} else {
		generator->send_target = NULL;
	}
	return ZEND_NATIVE_SUSPENDED;
}

zend_native_status zend_native_generator_yield_from(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_generator_operation operation;
	zend_generator *generator;
	zval *value;

	(void) extended_value;
	if (!zend_native_generator_init_operation(
			execute_data, op1, op2, result, source_opcode,
			source_position_id, ZEND_YIELD_FROM, &operation)
			|| (generator = zend_native_running_generator(execute_data))
				== NULL) {
		zend_throw_error(NULL, "Invalid native generator delegation frame");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (UNEXPECTED(generator->flags & ZEND_GENERATOR_FORCED_CLOSE)) {
		zend_throw_error(NULL,
			"Cannot use \"yield from\" in a force-closed generator");
		return ZEND_NATIVE_EXCEPTION;
	}
	value = zend_native_generator_operand(
		execute_data, operation.op1_type, operation.op1);
	if ((operation.op1_type & (IS_VAR | IS_CV))
			&& Z_TYPE_P(value) == IS_REFERENCE) {
		value = Z_REFVAL_P(value);
	}
	if (Z_TYPE_P(value) == IS_ARRAY) {
		ZVAL_COPY(&generator->values, value);
		Z_FE_POS(generator->values) = 0;
		zend_native_generator_free_operand(
			zend_native_generator_operand(
				execute_data, operation.op1_type, operation.op1),
			operation.op1_type);
	} else if (operation.op1_type != IS_CONST
			&& Z_TYPE_P(value) == IS_OBJECT
			&& Z_OBJCE_P(value)->get_iterator != NULL) {
		zend_class_entry *ce = Z_OBJCE_P(value);
		if (ce == zend_ce_generator) {
			zend_generator *delegate = (zend_generator *) Z_OBJ_P(value);
			Z_ADDREF_P(value);
			zend_native_generator_free_operand(
				zend_native_generator_operand(
					execute_data, operation.op1_type, operation.op1),
				operation.op1_type);
			if (UNEXPECTED(delegate->execute_data == NULL)) {
				zend_throw_error(NULL,
					"Generator passed to yield from was aborted without proper return and is unable to continue");
				zval_ptr_dtor(value);
				return ZEND_NATIVE_EXCEPTION;
			}
			if (Z_ISUNDEF(delegate->retval)) {
				if (UNEXPECTED(
						zend_generator_get_current(delegate) == generator)) {
					zend_throw_error(NULL,
						"Impossible to yield from the Generator being currently run");
					zval_ptr_dtor(value);
					return ZEND_NATIVE_EXCEPTION;
				}
				zend_generator_yield_from(generator, delegate);
			} else {
				if (operation.result_type != IS_UNUSED) {
					ZVAL_COPY(zend_native_generator_operand(
						execute_data, operation.result_type,
						operation.result), &delegate->retval);
				}
				return ZEND_NATIVE_RETURNED;
			}
		} else {
			zend_object_iterator *iterator =
				ce->get_iterator(ce, value, 0);
			zend_native_generator_free_operand(
				zend_native_generator_operand(
					execute_data, operation.op1_type, operation.op1),
				operation.op1_type);
			if (UNEXPECTED(iterator == NULL)
					|| UNEXPECTED(EG(exception) != NULL)) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Object of type %s did not create an Iterator",
						ZSTR_VAL(ce->name));
				}
				return ZEND_NATIVE_EXCEPTION;
			}
			iterator->index = 0;
			if (iterator->funcs->rewind != NULL) {
				iterator->funcs->rewind(iterator);
				if (UNEXPECTED(EG(exception) != NULL)) {
					OBJ_RELEASE(&iterator->std);
					return ZEND_NATIVE_EXCEPTION;
				}
			}
			ZVAL_OBJ(&generator->values, &iterator->std);
		}
	} else {
		zend_throw_error(NULL,
			"Can use \"yield from\" only with arrays and Traversables");
		zend_native_generator_free_operand(
			zend_native_generator_operand(
				execute_data, operation.op1_type, operation.op1),
			operation.op1_type);
		return ZEND_NATIVE_EXCEPTION;
	}
	if (operation.result_type != IS_UNUSED) {
		ZVAL_NULL(zend_native_generator_operand(
			execute_data, operation.result_type, operation.result));
	}
	generator->send_target = NULL;
	return ZEND_NATIVE_SUSPENDED;
}

zend_native_status zend_native_generator_return(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_generator_operation operation;
	zend_generator *generator;
	zval *return_value;

	(void) extended_value;
	if (!zend_native_generator_init_operation(
			execute_data, op1, op2, result, source_opcode,
			source_position_id, ZEND_GENERATOR_RETURN, &operation)
			|| (generator = zend_native_running_generator(execute_data))
				== NULL || operation.op1_type == IS_UNUSED) {
		zend_throw_error(NULL, "Invalid native generator return frame");
		return ZEND_NATIVE_EXCEPTION;
	}
	return_value = zend_native_generator_operand(
		execute_data, operation.op1_type, operation.op1);
	if ((operation.op1_type & (IS_CONST | IS_TMP_VAR)) != 0) {
		ZVAL_COPY_VALUE(&generator->retval, return_value);
		if (operation.op1_type == IS_CONST
				&& Z_OPT_REFCOUNTED(generator->retval)) {
			Z_ADDREF(generator->retval);
		}
	} else if (operation.op1_type == IS_CV) {
		ZVAL_COPY_DEREF(&generator->retval, return_value);
	} else if (Z_ISREF_P(return_value)) {
		zend_refcounted *reference = Z_COUNTED_P(return_value);
		return_value = Z_REFVAL_P(return_value);
		ZVAL_COPY_VALUE(&generator->retval, return_value);
		if (GC_DELREF(reference) == 0) {
			efree_size(reference, sizeof(zend_reference));
		} else if (Z_OPT_REFCOUNTED_P(return_value)) {
			Z_ADDREF_P(return_value);
		}
	} else {
		ZVAL_COPY_VALUE(&generator->retval, return_value);
	}
	ZEND_OBSERVER_FCALL_END(generator->execute_data, &generator->retval);
	EG(current_execute_data) = execute_data->prev_execute_data;
	zend_generator_close(generator, true);
	return ZEND_NATIVE_GENERATOR_RETURNED;
}

zend_native_status zend_native_generator_user_opcode_return(
	zend_execute_data *execute_data)
{
	zend_generator *generator = zend_native_running_generator(execute_data);

	if (generator == NULL) {
		zend_throw_error(NULL, "Invalid native generator user-opcode return");
		return ZEND_NATIVE_EXCEPTION;
	}
	EG(current_execute_data) = execute_data->prev_execute_data;
	zend_generator_close(generator, true);
	return ZEND_NATIVE_GENERATOR_RETURNED;
}

void zend_native_generator_uncaught_exception(
	zend_execute_data *execute_data)
{
	zend_generator *generator = zend_native_running_generator(execute_data);

	if (generator == NULL) {
		return;
	}
	ZEND_OBSERVER_FCALL_END(execute_data, NULL);
	EG(current_execute_data) = execute_data->prev_execute_data;
	/*
	 * Unlike GENERATOR_RETURN, an exceptional exit can leave source
	 * temporaries live at the throwing opline. The unfinished close performs
	 * their exact Zend cleanup before releasing the heap frame.
	 */
	zend_generator_close(generator, false);
}
