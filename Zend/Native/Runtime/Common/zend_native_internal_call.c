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
			|| receiver_kind > ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE) {
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
				|| !ARG_MAY_BE_SENT_BY_REF(function, argument_number)) {
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

void zend_native_interrupt_poll(zend_execute_data *execute_data)
{
	if (execute_data != NULL
			&& UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(execute_data);
	}
}
