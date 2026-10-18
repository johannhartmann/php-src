/* C-only bailout boundary for generated native frame execution. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_generators.h"
#include "Zend/Native/Runtime/Common/zend_native_execute.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_dtrace.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_type_info.h"

#include <stdio.h>


/* See zend_native_execute.h for where the state lives. */
static zend_always_inline void zend_native_execution_state_init(
	zend_native_execution_state *state, bool generator_frame)
{
	if (UNEXPECTED(generator_frame)) {
		state->previous_stack = NULL;
		state->previous_stack_top = NULL;
		return;
	}
	state->previous_stack = EG(vm_stack);
	state->previous_stack_top = EG(vm_stack_top);
}

static zend_always_inline void zend_native_execution_state_free(
	zend_native_execution_state *state)
{
	zend_vm_stack previous_stack = state->previous_stack;
	zval *previous_stack_top = state->previous_stack_top;

	if (UNEXPECTED(previous_stack == NULL)) {
		return;
	}
	while (UNEXPECTED(EG(vm_stack) != previous_stack)) {
		zend_vm_stack page = EG(vm_stack);

		EG(vm_stack) = page->prev;
		efree(page);
	}
	EG(vm_stack_top) = previous_stack_top;
	EG(vm_stack_end) = previous_stack->end;
}

/* i_free_compiled_variables(), inline as in the VM's leave helper. */
static zend_always_inline void zend_native_execute_free_compiled_variables(
	zend_execute_data *execute_data)
{
	zval *cv = ZEND_CALL_VAR_NUM(execute_data, 0);
	uint32_t count = execute_data->func->op_array.last_var;

	while (count != 0) {
		i_zval_ptr_dtor(cv);
		cv++;
		count--;
	}
}

static void zend_native_execution_cleanup_frame_ex(
	zend_execute_data *execute_data, bool cleanup_unfinished)
{
	uint32_t call_info = ZEND_CALL_INFO(execute_data);

	/* zend_leave_helper() publishes the caller before destroying compiled
	 * variables. A variable destructor may reenter PHP and inspect the active
	 * backtrace, so the dying frame must no longer be visible at that point. */
	if (EG(current_execute_data) == execute_data) {
		EG(current_execute_data) = execute_data->prev_execute_data;
	}

	/*
	 * The VM's leave helper destroys the compiled variables of a user frame;
	 * native entries return to C, so this boundary does it, exactly once. An
	 * exceptional or bailout exit also releases the live temporaries at the
	 * current source opline. A completed return consumed them in program
	 * order and need not have published its last opline, so it is not
	 * unfinished execution.
	 */
	if (cleanup_unfinished && execute_data->opline != NULL) {
		const zend_op_array *op_array = &execute_data->func->op_array;
		const zend_op *cleanup_opline = execute_data->opline;

		/* Throwing replaces EX(opline) with the HANDLE_EXCEPTION sentinel,
		 * which native frames never dispatch: the live-range cleanup needs
		 * the source opline. */
		if (cleanup_opline->opcode == ZEND_HANDLE_EXCEPTION
				&& EG(opline_before_exception) != NULL) {
			cleanup_opline = EG(opline_before_exception);
		}
		if (cleanup_opline >= op_array->opcodes
				&& cleanup_opline < op_array->opcodes + op_array->last) {
			zend_native_cleanup_unfinished_exception(execute_data,
				(uint32_t) (cleanup_opline - op_array->opcodes), 0);
		}
	}
	if ((call_info & ZEND_CALL_CODE) != 0) {
		/*
		 * Include/eval frames borrow the caller's symbol table. As the
		 * nested ZEND_CALL_CODE leave path, up to the stack frame release:
		 * publish the CVs back into the table and have the stack frame's
		 * owner reattach the caller once this frame is released.
		 */
		if (execute_data->func->op_array.last_var > 0) {
			zend_detach_symbol_table(execute_data);
			ZEND_ADD_CALL_FLAG(execute_data, ZEND_CALL_NEEDS_REATTACH);
		}
		return;
	}
	/* The VM's leave order: the CVs, the symbol table, the extra named
	 * and then the extra positional arguments. */
	zend_native_execute_free_compiled_variables(execute_data);
	if ((call_info & ZEND_CALL_HAS_SYMBOL_TABLE) != 0) {
		zend_clean_and_cache_symbol_table(execute_data->symbol_table);
		execute_data->symbol_table = NULL;
		ZEND_DEL_CALL_FLAG(execute_data, ZEND_CALL_HAS_SYMBOL_TABLE);
	}
	if ((ZEND_CALL_INFO(execute_data)
			& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
		zend_free_extra_named_params(execute_data->extra_named_params);
		execute_data->extra_named_params = NULL;
		ZEND_DEL_CALL_FLAG(execute_data, ZEND_CALL_HAS_EXTRA_NAMED_PARAMS);
	}
	zend_vm_stack_free_extra_args(execute_data);
}

void zend_native_execution_cleanup_frame(zend_execute_data *execute_data)
{
	zend_native_execution_cleanup_frame_ex(execute_data, true);
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

static void zend_native_execution_initialize_return_value(
	zend_execute_data *execute_data, zend_native_status status)
{
	/*
	 * A user opcode handler may leave a non-generator frame without a RETURN;
	 * a frame that reports ZEND_NATIVE_RETURNED still has an initialized
	 * result.
	 */
	if (status == ZEND_NATIVE_RETURNED
			&& execute_data->return_value != NULL
			&& Z_ISUNDEF_P(execute_data->return_value)) {
		ZVAL_NULL(execute_data->return_value);
	}
}

/*
 * Whether a frame's returned value has its declared return type, as the
 * callee's VERIFY_RETURN_TYPE or VERIFY_NEVER_TYPE checks it; the error is
 * reported otherwise. A scalar is coerced in place as the VM coerces it.
 */
static bool zend_native_execute_verify_return(zend_execute_data *execute_data)
{
	const zend_arg_info *return_info = execute_data->func->common.arg_info - 1;
	const uint32_t type_mask = ZEND_TYPE_FULL_MASK(return_info->type);
	zval *return_value = execute_data->return_value;

	if ((type_mask & MAY_BE_NEVER) != 0) {
		zend_verify_never_error(execute_data->func);
		return false;
	}
	if ((type_mask & MAY_BE_VOID) == 0
			&& (return_value == NULL || Z_ISUNDEF_P(return_value)
				|| (!ZEND_TYPE_CONTAINS_CODE(
						return_info->type, Z_TYPE_P(return_value))
					&& !zend_check_type_ex(&return_info->type,
						return_value, true, false)))) {
		zend_verify_return_error(execute_data->func,
			return_value == NULL || Z_ISUNDEF_P(return_value)
				? NULL : return_value);
		return false;
	}
	return true;
}

zend_native_status zend_native_execution_finish_direct_frame(
	zend_execute_data *execute_data, zend_native_status status,
	zval *discarded_return)
{
	bool frame_returned = status == ZEND_NATIVE_RETURNED
		&& EG(exception) == NULL;

	if (status == ZEND_NATIVE_GENERATOR_CREATED
			|| status == ZEND_NATIVE_SUSPENDED
			|| status == ZEND_NATIVE_GENERATOR_RETURNED) {
		return status;
	}
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		status = ZEND_NATIVE_EXCEPTION;
	}
	if (status == ZEND_NATIVE_BAILOUT) {
		zend_native_call_direct_abandon(execute_data);
		return status;
	}
	if (status == ZEND_NATIVE_EXCEPTION) {
		zend_native_call_direct_unwind(execute_data);
	}
	zend_native_execution_initialize_return_value(execute_data, status);
	if (status == ZEND_NATIVE_RETURNED
			&& (execute_data->func->common.fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0
			&& !zend_native_execute_verify_return(execute_data)) {
		status = ZEND_NATIVE_EXCEPTION;
	}
	if (discarded_return != NULL) {
		zend_native_release_discarded_return(discarded_return);
	}
	if (UNEXPECTED(atomic_load(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(execute_data);
		if (EG(exception) != NULL) {
			status = ZEND_NATIVE_EXCEPTION;
		}
	}
	zend_native_execution_cleanup_frame_ex(execute_data, !frame_returned);
	/* A throwing destructor of a CV or an extra argument fails the call:
	 * its result dies, as ZEND_HANDLE_EXCEPTION frees the throwing opline's
	 * result. A discarded result is already released. */
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		status = ZEND_NATIVE_EXCEPTION;
		if (execute_data->return_value != NULL) {
			zval_ptr_dtor_nogc(execute_data->return_value);
			ZVAL_UNDEF(execute_data->return_value);
		}
	}
	return status;
}

/*
 * Releasing CVs, the receiver or a discarded return value may run a
 * destructor that bails out, for example on an uncaught exception during
 * shutdown. Report that as this frame's bailout so callers still release
 * their activation state before propagating it.
 */
static void zend_native_execute_frame_release(
	zend_execute_data *execute_data, zend_native_execution_state *state,
	bool frame_returned, bool observer_already_started)
{
	zend_try {
		if (state->original_return_value == NULL
				&& state->status != ZEND_NATIVE_BAILOUT) {
			zend_native_release_discarded_return(&state->discarded_return);
		}
		if (state->status != ZEND_NATIVE_BAILOUT) {
			zend_native_execution_cleanup_frame_ex(execute_data, !frame_returned);
		}
		if (observer_already_started
				&& state->status != ZEND_NATIVE_BAILOUT) {
			uint32_t call_info = ZEND_CALL_INFO(execute_data);

			if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
				OBJ_RELEASE(Z_OBJ(execute_data->This));
			} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
				OBJ_RELEASE(ZEND_CLOSURE_OBJECT(execute_data->func));
			}
		}
	} zend_catch {
		state->status = ZEND_NATIVE_BAILOUT;
	} zend_end_try();
}

/* The status a frame returned with: unwind or abandon its pending calls,
 * initialize its result and verify its return type. Only a returned frame
 * changes status (to an exception), so a bailout abandons once. */
static zend_always_inline bool zend_native_execute_frame_returned(
	zend_execute_data *execute_data, zend_native_execution_state *state)
{
	bool frame_returned = state->status == ZEND_NATIVE_RETURNED
		&& EG(exception) == NULL;

	if (state->status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		state->status = ZEND_NATIVE_EXCEPTION;
	}
	if (state->status == ZEND_NATIVE_EXCEPTION) {
		zend_native_call_direct_unwind(execute_data);
	} else if (state->status == ZEND_NATIVE_BAILOUT) {
		zend_native_call_direct_abandon(execute_data);
	}
	zend_native_execution_initialize_return_value(
		execute_data, state->status);
	if (state->status == ZEND_NATIVE_RETURNED
			&& (execute_data->func->common.fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0
			&& !zend_native_execute_verify_return(execute_data)) {
		state->status = ZEND_NATIVE_EXCEPTION;
	}
	return frame_returned;
}

/* Every parameter supplied, none variadic, each argument of a type its
 * declaration's mask names: zend_native_frame_prepare() would pass the
 * frame unchanged. */
static zend_always_inline bool zend_native_execute_frame_received(
	const zend_execute_data *execute_data)
{
	const zend_op_array *op_array = &execute_data->func->op_array;

	if (ZEND_NATIVE_FRAME_PREPARED(execute_data)) {
		return true;
	}
	if (ZEND_CALL_NUM_ARGS(execute_data) < op_array->num_args
			|| (op_array->fn_flags & ZEND_ACC_VARIADIC) != 0) {
		return false;
	}
	for (uint32_t index = 0; index < op_array->num_args; index++) {
		const zend_type type = op_array->arg_info[index].type;

		if (ZEND_TYPE_IS_SET(type)
				&& !ZEND_TYPE_CONTAINS_CODE(type,
					Z_TYPE_P(ZEND_CALL_ARG(execute_data, index + 1)))) {
			return false;
		}
	}
	return true;
}

/*
 * A frame that returned as most do: a value, no exception or interrupt,
 * a return type the value's type is in (the mask; anything else takes the
 * full check), and nothing beyond its compiled variables to release. Its
 * return value is initialized.
 */
static zend_always_inline bool zend_native_execute_frame_plain_return(
	zend_execute_data *execute_data, zend_native_status status)
{
	zval *return_value = execute_data->return_value;

	if (status != ZEND_NATIVE_RETURNED || EG(exception) != NULL
			|| atomic_load(&EG(vm_interrupt))
			|| (ZEND_CALL_INFO(execute_data)
				& (ZEND_CALL_CODE | ZEND_CALL_HAS_SYMBOL_TABLE
					| ZEND_CALL_HAS_EXTRA_NAMED_PARAMS
					| ZEND_CALL_FREE_EXTRA_ARGS)) != 0) {
		return false;
	}
	if ((execute_data->func->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE)
			!= 0) {
		const zend_type type = execute_data->func->common.arg_info[-1].type;

		if ((ZEND_TYPE_FULL_MASK(type) & MAY_BE_VOID) == 0
				&& (Z_ISUNDEF_P(return_value)
					|| !ZEND_TYPE_CONTAINS_CODE(type, Z_TYPE_P(return_value)))) {
			return false;
		}
	}
	if (Z_ISUNDEF_P(return_value)) {
		ZVAL_NULL(return_value);
	}
	return true;
}

/* zend_native_execution_cleanup_frame_ex() of a returned plain frame. */
static zend_always_inline void zend_native_execute_frame_release_plain(
	zend_execute_data *execute_data)
{
	if (EG(current_execute_data) == execute_data) {
		EG(current_execute_data) = execute_data->prev_execute_data;
	}
	zend_native_execute_free_compiled_variables(execute_data);
}

/* What follows a bailout out of the entry of a lean frame, as
 * zend_native_execute_frame_state() continues after it. */
static void zend_native_execute_frame_settle(
	zend_execute_data *execute_data, zend_native_execution_state *state,
	bool observer_already_started)
{
	const bool frame_returned =
		zend_native_execute_frame_returned(execute_data, state);

	if (state->status != ZEND_NATIVE_BAILOUT
			&& UNEXPECTED(atomic_load(&EG(vm_interrupt)))) {
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
	zend_native_execute_frame_release(execute_data, state, frame_returned,
		observer_already_started);
}

/*
 * The common entry from C (a callback, an include, a magic method): no
 * generator, no observer, no DTrace probe. One zend_try covers the frame;
 * a bailout continues as zend_native_execute_frame_state() would at the
 * point where it happened, and one out of the return processing, which
 * that function leaves unprotected, propagates.
 */
static zend_never_inline zend_native_status zend_native_execute_frame_lean(
	zend_native_frame_entry_t entry, zend_execute_data *execute_data,
	bool observer_already_started, zend_native_execution_state *state)
{
	/* Without observers every entry's context is the shared one, which
	 * generated code only reads (see zend_native_execute_frame_entered()). */
	zend_native_execution_context *context =
		(zend_native_execution_context *)
			zend_native_execution_context_shared();
	volatile uint32_t phase = 0;
	volatile bool frame_returned = false;

	zend_native_execution_state_init(state, false);
	state->status = ZEND_NATIVE_BAILOUT;
	state->original_return_value = execute_data->return_value;
	state->observer_started = observer_already_started;
	state->observer_finished = true;
#ifdef HAVE_DTRACE
	state->dtrace_frame_started = false;
#endif
	ZEND_ASSERT(!context->observers_enabled && !context->deopt_resume);
	if (state->original_return_value == NULL) {
		ZVAL_UNDEF(&state->discarded_return);
		execute_data->return_value = &state->discarded_return;
	}
	zend_try {
		state->status = !zend_native_execute_frame_received(execute_data)
				&& zend_native_frame_receive(execute_data) == FAILURE
			? (EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT)
			: entry(execute_data, context);
		if (EXPECTED(zend_native_execute_frame_plain_return(
				execute_data, state->status))) {
			/* What the general processing below does for it. */
			phase = 3;
			if (state->original_return_value == NULL) {
				zend_native_release_discarded_return(
					&state->discarded_return);
			}
			zend_native_execute_frame_release_plain(execute_data);
			if (observer_already_started) {
				uint32_t call_info = ZEND_CALL_INFO(execute_data);

				if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
					OBJ_RELEASE(Z_OBJ(execute_data->This));
				} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
					OBJ_RELEASE(ZEND_CLOSURE_OBJECT(execute_data->func));
				}
			}
			phase = 4;
			goto done;
		}
		phase = 1;
		frame_returned =
			zend_native_execute_frame_returned(execute_data, state);
		phase = 2;
		if (state->original_return_value == NULL
				&& state->status != ZEND_NATIVE_BAILOUT) {
			zend_native_release_discarded_return(&state->discarded_return);
		}
		if (state->status != ZEND_NATIVE_BAILOUT
				&& UNEXPECTED(atomic_load(&EG(vm_interrupt)))) {
			zend_fcall_interrupt(execute_data);
			if (EG(exception) != NULL) {
				state->status = ZEND_NATIVE_EXCEPTION;
			}
		}
		phase = 3;
		if (state->status != ZEND_NATIVE_BAILOUT) {
			zend_native_execution_cleanup_frame_ex(execute_data, !frame_returned);
			if (observer_already_started) {
				uint32_t call_info = ZEND_CALL_INFO(execute_data);

				if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
					OBJ_RELEASE(Z_OBJ(execute_data->This));
				} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
					OBJ_RELEASE(ZEND_CLOSURE_OBJECT(execute_data->func));
				}
			}
		}
		phase = 4;
done:
		;
	} zend_catch {
		switch (phase) {
			case 0:
				state->status = EG(exception) != NULL
					? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
				zend_native_execute_frame_settle(
					execute_data, state, observer_already_started);
				break;
			case 2:
				state->status = EG(exception) != NULL
					? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
				zend_native_execute_frame_release(execute_data, state,
					frame_returned, observer_already_started);
				break;
			case 3:
				state->status = ZEND_NATIVE_BAILOUT;
				break;
			default:
				zend_bailout();
		}
	} zend_end_try();
	if (state->original_return_value == NULL) {
		execute_data->return_value = NULL;
	}
	{
		zend_native_status status = state->status;
		zend_native_execution_state_free(state);
		return status;
	}
}

static zend_never_inline zend_native_status zend_native_execute_frame_state(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic,
	bool observer_already_started,
	zend_native_execution_state *state)
{
	zend_native_execution_context context;
	zend_native_frame_entry_t entry;
	bool frame_returned;
	bool generator_frame;

	entry = zend_native_code_frame_entry(code);
	if (code == NULL || execute_data == NULL || entry == NULL
			|| !zend_native_code_is_executable(code)
			|| execute_data->func == NULL) {
		zend_native_execution_diagnostic(diagnostic,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"Zend frame does not match the compiled native entry");
		return ZEND_NATIVE_EXCEPTION;
	}

	generator_frame =
		(ZEND_CALL_INFO(execute_data) & ZEND_CALL_GENERATOR) != 0;
	zend_native_execution_state_init(state, generator_frame);
	state->status = ZEND_NATIVE_BAILOUT;
	state->original_return_value = execute_data->return_value;
	state->observer_started = observer_already_started;
	state->observer_finished = false;
#ifdef HAVE_DTRACE
	state->dtrace_frame_started = false;
#endif
	zend_native_execution_context_init(&context);
	if (state->original_return_value == NULL) {
		ZVAL_UNDEF(&state->discarded_return);
		execute_data->return_value = &state->discarded_return;
	}

	zend_try {
#ifdef HAVE_DTRACE
		if (zend_dtrace_enabled) {
			zend_dtrace_user_frame_begin(&state->dtrace_frame);
			state->dtrace_frame_started = true;
		}
#endif
		if (!state->observer_started) {
			state->observer_started = true;
			ZEND_OBSERVER_FCALL_BEGIN(execute_data);
		}
		/*
		 * RECV initialization and argument type verification are observable:
		 * as in the VM, the fcall observer has begun even when a type error
		 * or a default value keeps the machine entry from running.
		 */
		if (zend_native_frame_prepare(execute_data) == FAILURE) {
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} else {
			state->status = entry(execute_data, &context);
		}
	} zend_catch {
		state->status = EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	} zend_end_try();

#ifdef HAVE_DTRACE
	/*
	 * Exactly one return probe for every native invocation (the executor,
	 * direct native calls, generator resumes) that returned here; as in the
	 * VM's DTrace wrapper, a bailout has none.
	 */
	if (state->dtrace_frame_started
			&& state->status != ZEND_NATIVE_BAILOUT) {
		zend_dtrace_user_frame_end(&state->dtrace_frame);
		state->dtrace_frame_started = false;
	}
#endif

	/*
	 * Generator frames are owned by zend_generator after CREATE: suspension
	 * keeps the observer and frame alive, GENERATOR_RETURN has already closed
	 * and freed the heap frame. None of these takes the function-return
	 * cleanup.
	 */
	if (state->status == ZEND_NATIVE_GENERATOR_CREATED
			|| state->status == ZEND_NATIVE_SUSPENDED
			|| state->status == ZEND_NATIVE_GENERATOR_RETURNED
			|| generator_frame) {
		zend_native_status status = state->status;
		if (generator_frame && status == ZEND_NATIVE_EXCEPTION) {
			zend_native_generator_uncaught_exception(execute_data);
			state->observer_finished = true;
		}
		if (status == ZEND_NATIVE_BAILOUT) {
			zend_native_call_direct_abandon(execute_data);
		}
		zend_native_execution_state_free(state);
		return status;
	}
	/* A machine entry may return through its ordinary epilogue after a helper
	 * has raised. Such a frame did not complete its opcode stream and still
	 * owns live temporaries and pending calls at its published opline. Errors
	 * raised later by return verification remain completed-return frames. */
	frame_returned = zend_native_execute_frame_returned(execute_data, state);

	/*
	 * Unlike the VM, this boundary catches bailouts, so it sends the end
	 * notification itself rather than leaving it to request-shutdown observer
	 * unwinding. Observers are extension code that may throw or bail out,
	 * hence a zend_try of its own.
	 */
	if (state->observer_started && !state->observer_finished
			&& !ZEND_OBSERVER_ENABLED) {
		/* ZEND_OBSERVER_FCALL_END() notifies nothing. */
		state->observer_finished = true;
	}
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
	if (state->status == ZEND_NATIVE_BAILOUT) {
		/* An observer may itself have bailed out after creating native calls. */
		zend_native_call_direct_abandon(execute_data);
	}
	/* The VM's observed RETURN frees a discarded value after the end
	 * notification. */
	if (state->original_return_value == NULL
			&& state->status != ZEND_NATIVE_BAILOUT) {
		zend_try {
			zend_native_release_discarded_return(&state->discarded_return);
		} zend_catch {
			state->status = ZEND_NATIVE_BAILOUT;
		} zend_end_try();
	}
	if (state->status != ZEND_NATIVE_BAILOUT
			&& UNEXPECTED(atomic_load(&EG(vm_interrupt)))) {
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

	/* As in zend_native_execute_frame_release(). */
	zend_try {
		if (state->status != ZEND_NATIVE_BAILOUT) {
			zend_native_execution_cleanup_frame_ex(execute_data, !frame_returned);
		}
		/* zend_leave_helper() would release the closure or receiver of a
		 * frame entered through zend_execute_ex. The native reentry hook
		 * that replaces it leaves the stack frame to its caller, but not
		 * this reference. Direct native-to-native calls release their
		 * target at the call site and must not get here. */
		if (observer_already_started
				&& state->status != ZEND_NATIVE_BAILOUT) {
			uint32_t call_info = ZEND_CALL_INFO(execute_data);

			if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
				OBJ_RELEASE(Z_OBJ(execute_data->This));
			} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
				OBJ_RELEASE(ZEND_CLOSURE_OBJECT(execute_data->func));
			}
		}
	} zend_catch {
		state->status = ZEND_NATIVE_BAILOUT;
	} zend_end_try();
	if (state->original_return_value == NULL) {
		execute_data->return_value = NULL;
	}
	{
		zend_native_status status = state->status;
		zend_native_execution_state_free(state);
		return status;
	}
}

static zend_always_inline zend_native_status zend_native_execute_frame_impl(
	const zend_native_code *code,
	zend_native_frame_entry_t known_entry,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic,
	bool observer_already_started)
{
	zend_native_execution_state state;
	zend_native_frame_entry_t entry;

	/* The common entry skips the general frame state machine. */
	if (EXPECTED(code != NULL && execute_data != NULL
			&& execute_data->func != NULL
			&& (ZEND_CALL_INFO(execute_data) & ZEND_CALL_GENERATOR) == 0
			&& (execute_data->func->common.fn_flags & ZEND_ACC_GENERATOR) == 0
			&& !ZEND_OBSERVER_ENABLED
#ifdef HAVE_DTRACE
			&& !zend_dtrace_enabled
#endif
			&& (entry = known_entry != NULL ? known_entry
				: zend_native_code_executable_entry(code)) != NULL)) {
		return zend_native_execute_frame_lean(entry, execute_data,
			observer_already_started, &state);
	}
	return zend_native_execute_frame_state(code, execute_data, diagnostic,
		observer_already_started, &state);
}

zend_native_status zend_native_execute_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_execute_frame_impl(
		code, NULL, execute_data, diagnostic, false);
}

zend_native_status zend_native_execute_frame_with_entry(
	const zend_native_code *code,
	zend_native_frame_entry_t entry,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_execute_frame_impl(
		code, entry, execute_data, diagnostic, false);
}

/*
 * The entry from C without a zend_try of its own (execute_ex's direct
 * path): a bailout record stands in for it. When a bailout passes the
 * frame, zend_native_execute_frame_entered_unwind() does, before the jump,
 * what the lean entry's catch does at the same phase, and releases the
 * cell execute_ex retained; the bailout then continues.
 */
typedef struct _zend_native_entered_frame {
	zend_native_bailout_record record;
	zend_execute_data *execute_data;
	zend_native_execution_state *state;
	zend_native_entry_cell *cell;
	uint32_t phase;
	bool frame_returned;
} zend_native_entered_frame;

static void zend_native_execute_frame_entered_unwind(
	zend_native_bailout_record *record)
{
	zend_native_entered_frame *frame = (zend_native_entered_frame *) record;
	zend_execute_data *execute_data = frame->execute_data;
	zend_native_execution_state *state = frame->state;

	switch (frame->phase) {
		case 0:
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
			zend_native_execute_frame_settle(execute_data, state, true);
			break;
		case 2:
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
			zend_native_execute_frame_release(execute_data, state,
				frame->frame_returned, true);
			break;
		case 3:
			state->status = ZEND_NATIVE_BAILOUT;
			break;
		default:
			/* The return processing: the frame is left as it is. */
			zend_native_entry_cell_release_active(frame->cell);
			return;
	}
	if (state->original_return_value == NULL) {
		execute_data->return_value = NULL;
	}
	zend_native_execution_state_free(state);
	zend_native_entry_cell_release_active(frame->cell);
}

zend_native_status zend_native_execute_frame_entered(
	zend_native_frame_entry_t entry, zend_execute_data *execute_data,
	zend_native_entry_cell *cell)
{
	zend_native_execution_state state;
	zend_native_entered_frame frame;
	zend_native_execution_context *context =
		(zend_native_execution_context *)
			zend_native_execution_context_shared();
	zend_native_status status;
#if ZEND_DEBUG
	/* Generated code only reads its context. */
	zend_native_execution_context shared_before;

	memcpy(&shared_before, context, sizeof(shared_before));
#endif

	zend_native_execution_state_init(&state, false);
	state.status = ZEND_NATIVE_BAILOUT;
	state.original_return_value = execute_data->return_value;
	state.observer_started = true;
	state.observer_finished = true;
#ifdef HAVE_DTRACE
	state.dtrace_frame_started = false;
#endif
	if (state.original_return_value == NULL) {
		ZVAL_UNDEF(&state.discarded_return);
		execute_data->return_value = &state.discarded_return;
	}
	frame.execute_data = execute_data;
	frame.state = &state;
	frame.cell = cell;
	frame.phase = 0;
	frame.frame_returned = false;
	ZEND_NATIVE_BAILOUT_RECORD_LINK(
		&frame.record, zend_native_execute_frame_entered_unwind);

	state.status = !zend_native_execute_frame_received(execute_data)
			&& zend_native_frame_receive(execute_data) == FAILURE
		? (EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT)
		: entry(execute_data, context);
	if (EXPECTED(zend_native_execute_frame_plain_return(
			execute_data, state.status))) {
		frame.phase = 3;
		if (state.original_return_value == NULL) {
			zend_native_release_discarded_return(&state.discarded_return);
		}
		zend_native_execute_frame_release_plain(execute_data);
	} else {
		frame.phase = 1;
		frame.frame_returned =
			zend_native_execute_frame_returned(execute_data, &state);
		frame.phase = 2;
		if (state.original_return_value == NULL
				&& state.status != ZEND_NATIVE_BAILOUT) {
			zend_native_release_discarded_return(&state.discarded_return);
		}
		if (state.status != ZEND_NATIVE_BAILOUT
				&& UNEXPECTED(atomic_load(&EG(vm_interrupt)))) {
			zend_fcall_interrupt(execute_data);
			if (EG(exception) != NULL) {
				state.status = ZEND_NATIVE_EXCEPTION;
			}
		}
		frame.phase = 3;
		if (state.status != ZEND_NATIVE_BAILOUT) {
			zend_native_execution_cleanup_frame_ex(
				execute_data, !frame.frame_returned);
		}
	}
	if (state.status != ZEND_NATIVE_BAILOUT) {
		uint32_t call_info = ZEND_CALL_INFO(execute_data);

		if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
			OBJ_RELEASE(Z_OBJ(execute_data->This));
		} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
			OBJ_RELEASE(ZEND_CLOSURE_OBJECT(execute_data->func));
		}
	}
	ZEND_NATIVE_BAILOUT_RECORD_UNLINK(&frame.record);
	if (state.original_return_value == NULL) {
		execute_data->return_value = NULL;
	}
#if ZEND_DEBUG
	ZEND_ASSERT(memcmp(&shared_before, context,
		sizeof(shared_before)) == 0);
#endif
	status = state.status;
	zend_native_execution_state_free(&state);
	return status;
}

zend_native_status zend_native_execute_observed_frame(
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_execute_frame_impl(
		code, NULL, execute_data, diagnostic, true);
}
