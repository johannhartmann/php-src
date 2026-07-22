/* Native user-function calls over real Zend execution frames. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"

#include <string.h>

void zend_native_entry_cell_init(
	zend_native_entry_cell *cell, zend_function *function)
{
	ZEND_ASSERT(cell != NULL);
	memset(cell, 0, sizeof(*cell));
	cell->state = ZEND_NATIVE_ENTRY_UNCOMPILED;
	cell->function = function;
}

zend_result zend_native_entry_cell_begin_compile(zend_native_entry_cell *cell)
{
	if (cell == NULL || cell->function == NULL
			|| cell->function->type != ZEND_USER_FUNCTION
			|| cell->state != ZEND_NATIVE_ENTRY_UNCOMPILED) {
		return FAILURE;
	}
	cell->state = ZEND_NATIVE_ENTRY_COMPILING;
	return SUCCESS;
}

zend_result zend_native_entry_cell_publish(
	zend_native_entry_cell *cell, const zend_native_code *code)
{
	if (cell == NULL || code == NULL
			|| cell->state != ZEND_NATIVE_ENTRY_COMPILING) {
		return FAILURE;
	}
	cell->code = code;
	cell->generation++;
	cell->state = ZEND_NATIVE_ENTRY_READY;
	return SUCCESS;
}

void zend_native_entry_cell_fail(zend_native_entry_cell *cell)
{
	if (cell != NULL && cell->active_calls == 0
			&& (cell->state == ZEND_NATIVE_ENTRY_COMPILING
				|| cell->state == ZEND_NATIVE_ENTRY_READY)) {
		cell->code = NULL;
		cell->state = ZEND_NATIVE_ENTRY_FAILED;
	}
}

zend_result zend_native_entry_cell_reset(zend_native_entry_cell *cell)
{
	if (cell == NULL || cell->active_calls != 0) {
		return FAILURE;
	}
	cell->code = NULL;
	cell->state = ZEND_NATIVE_ENTRY_UNCOMPILED;
	return SUCCESS;
}

void zend_native_entry_cell_set_frame_probe(
	zend_native_entry_cell *cell,
	zend_native_frame_probe_t probe,
	void *context)
{
	ZEND_ASSERT(cell != NULL && cell->active_calls == 0);
	cell->frame_probe = probe;
	cell->frame_probe_context = context;
}

zend_result zend_native_frame_prepare(zend_execute_data *execute_data)
{
	zend_op_array *op_array;
	uint32_t supplied;
	uint32_t ordinal;

	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->func->type != ZEND_USER_FUNCTION) {
		return FAILURE;
	}
	op_array = &execute_data->func->op_array;
	supplied = ZEND_CALL_NUM_ARGS(execute_data);
	if (supplied < op_array->required_num_args || supplied > op_array->num_args) {
		return FAILURE;
	}
	for (ordinal = supplied; ordinal < op_array->num_args; ordinal++) {
		const zend_op *receive = &op_array->opcodes[ordinal];
		zval *argument = ZEND_CALL_ARG(execute_data, ordinal + 1);

		if (receive->opcode != ZEND_RECV_INIT
				|| receive->op1.num != ordinal + 1
				|| receive->op2_type != IS_CONST) {
			return FAILURE;
		}
		ZVAL_COPY(argument, RT_CONSTANT(receive, receive->op2));
	}
	if (op_array->num_args != 0 && op_array->arg_info == NULL) {
		return FAILURE;
	}
	for (ordinal = 0; ordinal < op_array->num_args; ordinal++) {
		const zend_arg_info *argument_info = &op_array->arg_info[ordinal];
		zval *argument = ZEND_CALL_ARG(execute_data, ordinal + 1);

		if (ZEND_TYPE_IS_SET(argument_info->type)
				&& !zend_check_type_ex(
					&argument_info->type, argument, false, false)) {
			zend_verify_arg_error(
				execute_data->func, argument_info, ordinal + 1, argument);
			return FAILURE;
		}
	}
	return SUCCESS;
}

static ZEND_COLD ZEND_NORETURN void zend_native_call_abort(const char *message)
{
	zend_throw_error(NULL, "%s", message);
	zend_bailout();
}

void zend_native_call_begin(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	uint32_t argument_count,
	uint32_t source_opline_index)
{
	zend_execute_data *call;
	zend_function *function;

	if (caller == NULL || cell == NULL || caller->call != NULL) {
		zend_native_call_abort("Invalid pending native call state");
	}
	if (cell->state != ZEND_NATIVE_ENTRY_READY || cell->code == NULL
			|| cell->function == NULL
			|| cell->function->type != ZEND_USER_FUNCTION) {
		zend_native_call_abort("Native callee entry is not ready");
	}
	function = cell->function;
	if (caller->func == NULL || caller->func->type != ZEND_USER_FUNCTION
			|| source_opline_index >= caller->func->op_array.last) {
		zend_native_call_abort("Native call source position is invalid");
	}
	if (argument_count < function->common.required_num_args
			|| argument_count > function->common.num_args) {
		zend_native_call_abort("Native callee argument count is invalid");
	}
#ifdef ZEND_CHECK_STACK_LIMIT
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		zend_bailout();
	}
#endif
	call = zend_vm_stack_push_call_frame(
		ZEND_CALL_NESTED_FUNCTION, function, argument_count, NULL);
	caller->call = call;
	caller->opline = &caller->func->op_array.opcodes[source_opline_index];
	zend_init_func_execute_data(call, &function->op_array, NULL);
	EG(current_execute_data) = caller;
}

void zend_native_call_set_integer_argument(
	zend_execute_data *caller,
	uint32_t ordinal,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type)
{
	zend_execute_data *call;
	zval *argument;

	ZEND_ASSERT(caller != NULL && caller->call != NULL);
	call = caller->call;
	ZEND_ASSERT(ordinal < ZEND_CALL_NUM_ARGS(call));
	argument = ZEND_CALL_ARG(call, ordinal + 1);
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			ZVAL_NULL(argument);
			break;
		case ZEND_MIR_SCALAR_TYPE_I1:
			ZVAL_BOOL(argument, payload_bits != 0);
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			ZVAL_LONG(argument, (zend_long) payload_bits);
			break;
		default:
			ZEND_UNREACHABLE();
	}
}

void zend_native_call_set_double_argument(
	zend_execute_data *caller, uint32_t ordinal, double value)
{
	zend_execute_data *call;

	ZEND_ASSERT(caller != NULL && caller->call != NULL);
	call = caller->call;
	ZEND_ASSERT(ordinal < ZEND_CALL_NUM_ARGS(call));
	ZVAL_DOUBLE(ZEND_CALL_ARG(call, ordinal + 1), value);
}

uint64_t zend_native_call_invoke_finish(
	zend_execute_data *caller, zend_native_entry_cell *cell)
{
	uint64_t payload_bits = 0;
	zend_execute_data *call;
	zval return_value;
	zend_native_status status;

	if (caller == NULL || cell == NULL || caller->call == NULL
			|| cell->state != ZEND_NATIVE_ENTRY_READY || cell->code == NULL) {
		zend_native_call_abort("Invalid native invocation state");
	}
	call = caller->call;
	if (cell->frame_probe != NULL) {
		cell->frame_probe(cell->frame_probe_context, caller, call);
	}
	ZVAL_UNDEF(&return_value);
	call->return_value = &return_value;
	cell->active_calls++;
	EG(current_execute_data) = call;
	ZEND_OBSERVER_FCALL_BEGIN(call);
	status = zend_native_execute_frame(cell->code, call, NULL);
	if (status != ZEND_NATIVE_BAILOUT) {
		ZEND_OBSERVER_FCALL_END(call,
			status == ZEND_NATIVE_RETURNED && EG(exception) == NULL
				? &return_value : NULL);
	}
	if (status != ZEND_NATIVE_BAILOUT
			&& UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(call);
		if (EG(exception) != NULL) {
			status = ZEND_NATIVE_EXCEPTION;
		}
	}
	EG(current_execute_data) = caller;
	cell->active_calls--;
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		status = ZEND_NATIVE_EXCEPTION;
	}
	if (status == ZEND_NATIVE_RETURNED) {
		switch (Z_TYPE(return_value)) {
			case IS_NULL:
				payload_bits = 0;
				break;
			case IS_FALSE:
				payload_bits = 0;
				break;
			case IS_TRUE:
				payload_bits = 1;
				break;
			case IS_LONG:
				payload_bits = (uint64_t) Z_LVAL(return_value);
				break;
			case IS_DOUBLE:
				memcpy(&payload_bits, &Z_DVAL(return_value),
					sizeof(payload_bits));
				break;
			default:
				zend_throw_error(
					NULL, "Native callee returned a non-scalar value");
				status = ZEND_NATIVE_EXCEPTION;
				break;
		}
	}
	if (!Z_ISUNDEF(return_value)) {
		zval_ptr_dtor(&return_value);
	}
	zend_vm_stack_free_call_frame(call);
	caller->call = NULL;
	if (status != ZEND_NATIVE_RETURNED) {
		zend_bailout();
	}
	return payload_bits;
}

static void zend_native_echo_zval(
	zend_execute_data *execute_data, const zval *value)
{
	zend_string *string;

	if (execute_data == NULL || execute_data != EG(current_execute_data)) {
		zend_native_call_abort("Invalid native echo frame");
	}
	string = zval_get_string_func(value);
	if (ZSTR_LEN(string) != 0) {
		zend_write(ZSTR_VAL(string), ZSTR_LEN(string));
	}
	zend_string_release_ex(string, false);
	if (UNEXPECTED(EG(exception) != NULL)) {
		zend_bailout();
	}
}

void zend_native_echo_integer(
	zend_execute_data *execute_data,
	uint64_t payload_bits,
	zend_mir_scalar_type_mask exact_type)
{
	zval value;

	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			ZVAL_NULL(&value);
			break;
		case ZEND_MIR_SCALAR_TYPE_I1:
			ZVAL_BOOL(&value, payload_bits != 0);
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			ZVAL_LONG(&value, (zend_long) payload_bits);
			break;
		case ZEND_MIR_SCALAR_TYPE_F64:
			memcpy(&value.value.dval, &payload_bits, sizeof(payload_bits));
			Z_TYPE_INFO(value) = IS_DOUBLE;
			break;
		default:
			zend_native_call_abort("Invalid native echo scalar type");
	}
	zend_native_echo_zval(execute_data, &value);
}

void zend_native_echo_double(
	zend_execute_data *execute_data, double payload)
{
	zval value;

	ZVAL_DOUBLE(&value, payload);
	zend_native_echo_zval(execute_data, &value);
}
