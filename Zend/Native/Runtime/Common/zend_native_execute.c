/* C-only bailout boundary for generated native frame execution. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_type_info.h"

#include <stdio.h>

typedef struct _zend_native_execution_state {
	zend_native_status status;
	zval discarded_return;
	zval *original_return_value;
	bool observer_started;
	bool observer_finished;
} zend_native_execution_state;

static void zend_native_execution_cleanup_frame(zend_execute_data *execute_data)
{
	/*
	 * The VM's leave helper destroys every compiled variable before releasing
	 * a user frame.  Native entries return to C instead, so this boundary owns
	 * the equivalent cleanup exactly once. Release live temporaries at the
	 * current source opline on every exit; the return helper has already moved
	 * its result and marked the source slot undefined.
	 */
	if (execute_data->opline != NULL) {
		const zend_op_array *op_array = &execute_data->func->op_array;
		if (execute_data->opline >= op_array->opcodes
				&& execute_data->opline < op_array->opcodes + op_array->last) {
			zend_cleanup_unfinished_execution(execute_data,
				(uint32_t) (execute_data->opline - op_array->opcodes), 0);
		}
	}
	zend_free_compiled_variables(execute_data);
}

static void zend_native_execution_diagnostic(
	zend_native_diagnostic *diagnostic,
	zend_native_diagnostic_code code,
	const char *message)
{
	if (diagnostic == NULL) {
		return;
	}
	diagnostic->code = code;
	snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
}

static zend_native_status zend_native_execute_frame_impl(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic,
	bool observer_already_started)
{
	zend_native_execution_state *state;
	zend_native_frame_entry_t entry;
	uint32_t argument_count;

	entry = zend_native_code_frame_entry(code);
	argument_count = zend_native_code_argument_count(code);
	if (code == NULL || execute_data == NULL || entry == NULL
			|| !zend_native_code_is_executable(code)
			|| execute_data->func == NULL
			|| ZEND_CALL_NUM_ARGS(execute_data) > argument_count
			|| zend_native_frame_prepare(execute_data) == FAILURE) {
		zend_native_execution_diagnostic(diagnostic,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Zend frame does not match the compiled native entry");
		return ZEND_NATIVE_EXCEPTION;
	}

	/*
	 * State changed after setjmp lives on the heap. The catcher therefore does
	 * not inspect an indeterminate non-volatile automatic after longjmp.
	 */
	state = emalloc(sizeof(*state));
	state->status = ZEND_NATIVE_BAILOUT;
	state->original_return_value = execute_data->return_value;
	state->observer_started = observer_already_started;
	state->observer_finished = false;
	if (state->original_return_value == NULL) {
		ZVAL_UNDEF(&state->discarded_return);
		execute_data->return_value = &state->discarded_return;
	}

	zend_try {
		if (!state->observer_started) {
			state->observer_started = true;
			ZEND_OBSERVER_FCALL_BEGIN(execute_data);
		}
		state->status = entry(execute_data);
	} zend_catch {
		state->status = EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	} zend_end_try();

	if (state->status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		state->status = ZEND_NATIVE_EXCEPTION;
	}
	if (state->status == ZEND_NATIVE_RETURNED
			&& (execute_data->func->common.fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
		const zend_arg_info *return_info =
			execute_data->func->common.arg_info - 1;
		uint32_t type_mask = ZEND_TYPE_FULL_MASK(return_info->type);
		zval *return_value = execute_data->return_value;

		if ((type_mask & MAY_BE_NEVER) != 0) {
			zend_verify_never_error(execute_data->func);
			state->status = ZEND_NATIVE_EXCEPTION;
		} else if ((type_mask & MAY_BE_VOID) == 0
				&& (Z_ISUNDEF_P(return_value)
					|| !zend_check_type_ex(
						&return_info->type, return_value, true, false))) {
			zend_verify_return_error(execute_data->func,
				Z_ISUNDEF_P(return_value) ? NULL : return_value);
			state->status = ZEND_NATIVE_EXCEPTION;
		}
	}

	/*
	 * Unlike the VM, this boundary catches bailout locally.  It therefore also
	 * owns the matching end notification instead of relying on request-shutdown
	 * observer unwinding.  Keep it behind a second Zend bailout boundary because
	 * observers are extension code and may themselves throw or bail out.
	 */
	if (state->observer_started && !state->observer_finished) {
		zend_try {
			ZEND_OBSERVER_FCALL_END(execute_data,
				state->status == ZEND_NATIVE_RETURNED && EG(exception) == NULL
					? execute_data->return_value : NULL);
			state->observer_finished = true;
		} zend_catch {
			state->observer_finished = true;
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} zend_end_try();
	}
	if (state->status != ZEND_NATIVE_BAILOUT
			&& UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_try {
			zend_fcall_interrupt(execute_data);
			if (EG(exception) != NULL) {
				state->status = ZEND_NATIVE_EXCEPTION;
			}
		} zend_catch {
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} zend_end_try();
	}

	zend_native_execution_cleanup_frame(execute_data);

	if (state->original_return_value == NULL) {
		if (!Z_ISUNDEF(state->discarded_return)) {
			zval_ptr_dtor(&state->discarded_return);
		}
		execute_data->return_value = NULL;
	}
	{
		zend_native_status status = state->status;
		efree(state);
		return status;
	}
}

zend_native_status zend_native_execute_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_execute_frame_impl(
		code, execute_data, diagnostic, false);
}

zend_native_status zend_native_execute_observed_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_execute_frame_impl(
		code, execute_data, diagnostic, true);
}
