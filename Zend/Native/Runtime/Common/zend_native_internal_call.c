/* Direct calls to compile-time resolved Zend internal functions. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"

#include <string.h>

typedef struct _zend_native_internal_execution_state {
	zend_execute_data *caller;
	zend_execute_data *call;
	zval *return_value;
	zend_native_status status;
} zend_native_internal_execution_state;

static bool zend_native_internal_count_is_valid(
	const zend_function *function, uint32_t argument_count)
{
	return argument_count >= function->common.required_num_args
		&& (argument_count <= function->common.num_args
			|| (function->common.fn_flags & ZEND_ACC_VARIADIC) != 0);
}

zend_result zend_native_internal_call_cell_init(
	zend_native_internal_call_cell *cell,
	zend_function *function,
	zend_class_entry *called_scope,
	zend_native_internal_receiver_kind receiver_kind)
{
	if (cell == NULL || function == NULL
			|| function->type != ZEND_INTERNAL_FUNCTION
			|| receiver_kind > ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
		return FAILURE;
	}
	if (function->common.scope == NULL
			&& receiver_kind != ZEND_NATIVE_INTERNAL_RECEIVER_NONE) {
		return FAILURE;
	}
	if (receiver_kind == ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE
			&& (called_scope == NULL
				|| !instanceof_function(called_scope, function->common.scope))) {
		return FAILURE;
	}
	cell->function = function;
	cell->called_scope = called_scope;
	cell->receiver_kind = receiver_kind;
	return SUCCESS;
}

zend_result zend_native_internal_call_begin(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	uint32_t argument_count,
	uint32_t source_opline_index)
{
	zend_execute_data *call;
	zend_function *function;
	const zend_op *source_opline;
	void *object_or_called_scope = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;

	if (caller == NULL || caller->call != NULL || caller->func == NULL
			|| caller->func->type != ZEND_USER_FUNCTION
			|| cell == NULL || cell->function == NULL
			|| cell->function->type != ZEND_INTERNAL_FUNCTION
			|| source_opline_index >= caller->func->op_array.last) {
		return FAILURE;
	}
	function = cell->function;
	if (!zend_native_internal_count_is_valid(function, argument_count)) {
		return FAILURE;
	}
	source_opline = &caller->func->op_array.opcodes[source_opline_index];
	if (cell->receiver_kind == ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
		if ((ZEND_CALL_INFO(caller) & ZEND_CALL_HAS_THIS) == 0
				|| !instanceof_function(
					Z_OBJCE(caller->This), function->common.scope)) {
			return FAILURE;
		}
		object_or_called_scope = Z_OBJ(caller->This);
		call_info |= ZEND_CALL_HAS_THIS;
	} else if (cell->receiver_kind
			== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE) {
		object_or_called_scope = cell->called_scope;
	} else if (cell->receiver_kind
			== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
		zval *receiver;

		if (source_opline->opcode != ZEND_INIT_METHOD_CALL
				|| (source_opline->op1_type != IS_CV
					&& source_opline->op1_type != IS_VAR
					&& source_opline->op1_type != IS_TMP_VAR)) {
			return FAILURE;
		}
		receiver = ZEND_CALL_VAR(caller, source_opline->op1.var);
		ZVAL_DEREF(receiver);
		if (Z_TYPE_P(receiver) != IS_OBJECT
				|| !instanceof_function(
					Z_OBJCE_P(receiver), function->common.scope)) {
			return FAILURE;
		}
		object_or_called_scope = Z_OBJ_P(receiver);
		GC_ADDREF((zend_object *) object_or_called_scope);
		call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
	}

#ifdef ZEND_CHECK_STACK_LIMIT
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		return FAILURE;
	}
#endif
	call = zend_vm_stack_push_call_frame(
		call_info, function, argument_count, object_or_called_scope);
	call->prev_execute_data = caller;
	caller->call = call;
	caller->opline = &caller->func->op_array.opcodes[source_opline_index];
	return SUCCESS;
}

zend_result zend_native_call_set_zval_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	const zval *value,
	zend_native_call_argument_mode mode)
{
	zend_execute_data *call;
	zend_function *function;
	zval *target;
	const zval *source = value;
	uint32_t argument_number;

	if (caller == NULL || caller->call == NULL || value == NULL
			|| mode > ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		return FAILURE;
	}
	call = caller->call;
	function = call->func;
	if (function == NULL || function->type != ZEND_INTERNAL_FUNCTION
			|| ordinal >= ZEND_CALL_NUM_ARGS(call)) {
		return FAILURE;
	}
	argument_number = ordinal + 1;
	if (mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		if (!Z_ISREF_P(source)
				|| !ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
			return FAILURE;
		}
	} else if (ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
		return FAILURE;
	} else if (Z_ISREF_P(source)
			&& (function->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
		source = Z_REFVAL_P(source);
	}
	target = ZEND_CALL_ARG(call, argument_number);
	ZVAL_COPY(target, source);
	return SUCCESS;
}

static zval *zend_native_source_argument(
	zend_execute_data *caller, uint32_t send_opline_index, bool *mutable_value,
	uint8_t *operand_type)
{
	const zend_op *send;

	if (mutable_value == NULL || operand_type == NULL
			|| caller == NULL || caller->func == NULL
			|| caller->func->type != ZEND_USER_FUNCTION
			|| send_opline_index >= caller->func->op_array.last) {
		return NULL;
	}
	send = &caller->func->op_array.opcodes[send_opline_index];
	*mutable_value = false;
	*operand_type = send->op1_type;
	switch (send->opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
			break;
		default:
			return NULL;
	}
	switch (send->op1_type) {
		case IS_CONST:
			return (zval *) RT_CONSTANT(send, send->op1);
		case IS_CV:
		case IS_VAR:
		case IS_TMP_VAR:
			*mutable_value = true;
			return ZEND_CALL_VAR(caller, send->op1.var);
		default:
			return NULL;
	}
}

zend_result zend_native_call_set_source_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	uint32_t send_opline_index,
	zend_native_call_argument_mode mode)
{
	bool mutable_value;
	uint8_t operand_type;
	zend_execute_data *call;
	zend_function *function;
	zval *target;
	uint32_t argument_number;
	zval *value = zend_native_source_argument(
		caller, send_opline_index, &mutable_value, &operand_type);

	if (value == NULL || caller->call == NULL
			|| mode > ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		return FAILURE;
	}
	call = caller->call;
	function = call->func;
	if (function == NULL || function->type != ZEND_INTERNAL_FUNCTION
			|| ordinal >= ZEND_CALL_NUM_ARGS(call)) {
		return FAILURE;
	}
	argument_number = ordinal + 1;
	target = ZEND_CALL_ARG(call, argument_number);
	/*
	 * SEND_VAR_EX and SEND_REF materialize a reference in the canonical
	 * caller slot when the resolved parameter requires one.  The native
	 * path must perform the same mutation before the argument is copied into
	 * the internal call frame; literals can never become by-reference args.
	 */
	if (mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE && !Z_ISREF_P(value)) {
		if (!mutable_value) {
			return FAILURE;
		}
		ZVAL_MAKE_REF(value);
	}
	if (mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		if (!ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
			return FAILURE;
		}
		ZVAL_COPY(target, value);
		return SUCCESS;
	}
	if (ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
		return FAILURE;
	}
	if (operand_type == IS_CONST) {
		ZVAL_COPY(target, value);
	} else if (operand_type == IS_CV) {
		ZVAL_COPY_DEREF(target, value);
	} else if (operand_type == IS_VAR || operand_type == IS_TMP_VAR) {
		if (Z_ISREF_P(value)) {
			ZVAL_COPY_DEREF(target, value);
			zval_ptr_dtor(value);
		} else {
			ZVAL_COPY_VALUE(target, value);
		}
		ZVAL_UNDEF(value);
	} else {
		return FAILURE;
	}
	return SUCCESS;
}

zend_native_status zend_native_internal_call_invoke_finish(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	zval *return_value)
{
	zend_native_internal_execution_state *state;
	zend_native_status status;

	if (caller == NULL || caller->call == NULL || cell == NULL
			|| cell->function == NULL || caller->call->func != cell->function
			|| return_value == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	state = emalloc(sizeof(*state));
	state->caller = caller;
	state->call = caller->call;
	state->return_value = return_value;
	state->status = ZEND_NATIVE_BAILOUT;
	ZVAL_NULL(state->return_value);
	EG(current_execute_data) = state->call;
	ZEND_OBSERVER_FCALL_BEGIN(state->call);

	zend_try {
		if (EXPECTED(zend_execute_internal == NULL)) {
			state->call->func->internal_function.handler(
				state->call, state->return_value);
		} else {
			zend_execute_internal(state->call, state->return_value);
		}
		state->status = EG(exception) == NULL
			? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
	} zend_catch {
		state->status = EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	} zend_end_try();

	if (state->status != ZEND_NATIVE_BAILOUT) {
		ZEND_OBSERVER_FCALL_END(state->call,
			state->status == ZEND_NATIVE_RETURNED
				? state->return_value : NULL);
		if (UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
			zend_fcall_interrupt(state->call);
			if (EG(exception) != NULL) {
				state->status = ZEND_NATIVE_EXCEPTION;
			}
		}
	}
	EG(current_execute_data) = state->caller;
	zend_vm_stack_free_args(state->call);
	if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_RELEASE_THIS) != 0) {
		OBJ_RELEASE(Z_OBJ(state->call->This));
	}
	zend_vm_stack_free_call_frame(state->call);
	state->caller->call = NULL;
	if (state->status != ZEND_NATIVE_RETURNED
			&& !Z_ISUNDEF_P(state->return_value)) {
		zval_ptr_dtor(state->return_value);
		ZVAL_UNDEF(state->return_value);
	}
	status = state->status;
	efree(state);
	return status;
}

zend_native_status zend_native_internal_call_invoke_finish_source(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	uint32_t do_opline_index)
{
	const zend_op *opline;
	zval temporary;
	zval *return_value;
	zend_native_status status;

	if (caller == NULL || caller->func == NULL
			|| caller->func->type != ZEND_USER_FUNCTION
			|| do_opline_index >= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &caller->func->op_array.opcodes[do_opline_index];
	if (opline->opcode != ZEND_DO_ICALL
			&& opline->opcode != ZEND_DO_FCALL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	caller->opline = opline;
	if (opline->result_type == IS_UNUSED) {
		ZVAL_UNDEF(&temporary);
		return_value = &temporary;
	} else if (opline->result_type == IS_VAR
			|| opline->result_type == IS_TMP_VAR) {
		return_value = ZEND_CALL_VAR(caller, opline->result.var);
		ZVAL_UNDEF(return_value);
	} else {
		return ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_internal_call_invoke_finish(
		caller, cell, return_value);
	if (return_value == &temporary && !Z_ISUNDEF(temporary)) {
		zval_ptr_dtor(&temporary);
	}
	return status;
}

uint64_t zend_native_call_read_source_scalar(
	zend_execute_data *caller,
	uint32_t do_opline_index,
	zend_mir_scalar_type_mask exact_type)
{
	const zend_op *opline;
	const zval *value;
	uint64_t payload_bits = 0;
	bool matches = false;

	if (caller == NULL || caller->func == NULL
			|| caller->func->type != ZEND_USER_FUNCTION
			|| do_opline_index >= caller->func->op_array.last) {
		goto mismatch;
	}
	opline = &caller->func->op_array.opcodes[do_opline_index];
	if ((opline->opcode != ZEND_DO_ICALL
			&& opline->opcode != ZEND_DO_FCALL)
			|| (opline->result_type != IS_VAR
				&& opline->result_type != IS_TMP_VAR)) {
		goto mismatch;
	}
	value = ZEND_CALL_VAR(caller, opline->result.var);
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			matches = Z_TYPE_P(value) == IS_NULL;
			break;
		case ZEND_MIR_SCALAR_TYPE_I1:
			matches = Z_TYPE_P(value) == IS_FALSE || Z_TYPE_P(value) == IS_TRUE;
			payload_bits = Z_TYPE_P(value) == IS_TRUE;
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			matches = Z_TYPE_P(value) == IS_LONG;
			if (matches) {
				payload_bits = (uint64_t) Z_LVAL_P(value);
			}
			break;
		case ZEND_MIR_SCALAR_TYPE_F64:
			matches = Z_TYPE_P(value) == IS_DOUBLE;
			if (matches) {
				memcpy(&payload_bits, &Z_DVAL_P(value), sizeof(payload_bits));
			}
			break;
		default:
			break;
	}
	if (matches) {
		return payload_bits;
	}

mismatch:
	zend_throw_error(NULL,
		"Native internal call violated its exact scalar result contract");
	zend_bailout();
	return 0;
}

zend_native_status zend_native_catch_enter(
	zend_execute_data *execute_data, uint32_t catch_opline_index)
{
	const zend_op *opline;
	zend_class_entry *catch_ce;
	zend_class_entry *exception_ce;
	zend_object *exception;
	uint32_t cache_offset;

	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->func->type != ZEND_USER_FUNCTION
			|| catch_opline_index >= execute_data->func->op_array.last) {
		return ZEND_NATIVE_BAILOUT;
	}
	opline = &execute_data->func->op_array.opcodes[catch_opline_index];
	if (opline->opcode != ZEND_CATCH || EG(exception) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->opline = opline;
	cache_offset = opline->extended_value & ~ZEND_LAST_CATCH;
	catch_ce = CACHED_PTR(cache_offset);
	if (catch_ce == NULL) {
		catch_ce = zend_fetch_class_by_name(
			Z_STR_P(RT_CONSTANT(opline, opline->op1)),
			Z_STR_P(RT_CONSTANT(opline, opline->op1) + 1),
			ZEND_FETCH_CLASS_NO_AUTOLOAD | ZEND_FETCH_CLASS_SILENT);
		CACHE_PTR(cache_offset, catch_ce);
	}
	exception_ce = EG(exception)->ce;
	if (exception_ce != catch_ce
			&& (catch_ce == NULL
				|| !instanceof_function(exception_ce, catch_ce))) {
		if ((opline->extended_value & ZEND_LAST_CATCH) != 0) {
			zend_rethrow_exception(execute_data);
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	exception = EG(exception);
	EG(exception) = NULL;
	if (opline->result_type != IS_UNUSED) {
		zval tmp;
		ZVAL_OBJ(&tmp, exception);
		zend_assign_to_variable(
			ZEND_CALL_VAR(execute_data, opline->result.var),
			&tmp, IS_TMP_VAR, true);
	} else {
		OBJ_RELEASE(exception);
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

void zend_native_interrupt_poll(zend_execute_data *execute_data)
{
	if (execute_data != NULL
			&& UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(execute_data);
	}
}
