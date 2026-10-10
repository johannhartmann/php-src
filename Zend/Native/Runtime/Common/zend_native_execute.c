/* C-only bailout boundary for generated native frame execution. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_generators.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_dtrace.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_type_info.h"

#include <stdio.h>

typedef struct _zend_native_execution_state {
	zend_vm_stack previous_stack;
	zval *previous_stack_top;
	zend_native_status status;
	zval discarded_return;
	zval *original_return_value;
	bool observer_started;
	bool observer_finished;
#ifdef HAVE_DTRACE
	zend_dtrace_user_frame dtrace_frame;
	bool dtrace_frame_started;
#endif
} zend_native_execution_state;

/*
 * The state lives in the C frame of zend_native_execute_frame_impl(), not in
 * the function that calls setjmp(), so the catcher never reads an
 * indeterminate automatic after longjmp. It must not live on the VM stack:
 * nothing may follow the newest frame there, because a tier-2 host entry grows
 * that frame in place. A generator frame lies below its thawed call frames and
 * leaves the VM stack to them.
 */
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
	 * The VM's leave helper destroys every compiled variable before releasing
	 * a user frame.  Native entries return to C instead, so this boundary owns
	 * the equivalent cleanup exactly once. An exceptional or bailout exit also
	 * releases live temporaries at the current source opline. A completed native
	 * return has already consumed those temporaries in program order, and its
	 * final scalar instruction need not cross a helper solely to publish an
	 * opline, so it must not be treated as unfinished execution.
	 */
	if (cleanup_unfinished && execute_data->opline != NULL) {
		const zend_op_array *op_array = &execute_data->func->op_array;
		const zend_op *cleanup_opline = execute_data->opline;

		/* Throwing replaces EX(opline) with the global HANDLE_EXCEPTION
		 * sentinel. Native frames return directly to this boundary instead of
		 * dispatching that handler, so recover the source opline before applying
		 * Zend's live-range cleanup. */
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
	zend_vm_stack_free_extra_args(execute_data);
	if ((ZEND_CALL_INFO(execute_data)
			& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
		zend_free_extra_named_params(execute_data->extra_named_params);
		execute_data->extra_named_params = NULL;
		ZEND_DEL_CALL_FLAG(execute_data, ZEND_CALL_HAS_EXTRA_NAMED_PARAMS);
	}
	if ((call_info & ZEND_CALL_CODE) != 0) {
		/*
		 * Include/eval frames borrow the caller's symbol table. Mirror the
		 * nested ZEND_CALL_CODE leave path up to, but not including, stack
		 * frame release: publish frame CVs back into the table and tell the
		 * owner of the stack frame that the caller must be reattached only
		 * after this code frame has been released.
		 */
		if (execute_data->func->op_array.last_var > 0) {
			zend_detach_symbol_table(execute_data);
			ZEND_ADD_CALL_FLAG(execute_data, ZEND_CALL_NEEDS_REATTACH);
		}
		return;
	}
	/* i_free_compiled_variables(), inline as in the VM's leave helper. */
	{
		zval *cv = ZEND_CALL_VAR_NUM(execute_data, 0);
		uint32_t count = execute_data->func->op_array.last_var;

		while (count != 0) {
			i_zval_ptr_dtor(cv);
			cv++;
			count--;
		}
	}
	if ((call_info & ZEND_CALL_HAS_SYMBOL_TABLE) != 0) {
		zend_clean_and_cache_symbol_table(execute_data->symbol_table);
		execute_data->symbol_table = NULL;
		ZEND_DEL_CALL_FLAG(execute_data, ZEND_CALL_HAS_SYMBOL_TABLE);
	}
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
	 * A user opcode handler may leave a non-generator frame without running a
	 * RETURN opcode.  The public native execution contract still promises an
	 * initialized result whenever the frame reports ZEND_NATIVE_RETURNED.
	 */
	if (status == ZEND_NATIVE_RETURNED
			&& execute_data->return_value != NULL
			&& Z_ISUNDEF_P(execute_data->return_value)) {
		ZVAL_NULL(execute_data->return_value);
	}
}

zend_native_status zend_native_execution_finish_direct_frame(
	zend_execute_data *execute_data, zend_native_status status)
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
				& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
		const zend_arg_info *return_info =
			execute_data->func->common.arg_info - 1;
		uint32_t type_mask = ZEND_TYPE_FULL_MASK(return_info->type);
		zval *return_value = execute_data->return_value;

		if ((type_mask & MAY_BE_NEVER) != 0) {
			zend_verify_never_error(execute_data->func);
			status = ZEND_NATIVE_EXCEPTION;
		} else if ((type_mask & MAY_BE_VOID) == 0
				&& (return_value == NULL || Z_ISUNDEF_P(return_value)
					|| !zend_check_type_ex(
						&return_info->type, return_value, true, false))) {
			zend_verify_return_error(execute_data->func,
				return_value == NULL || Z_ISUNDEF_P(return_value)
					? NULL : return_value);
			status = ZEND_NATIVE_EXCEPTION;
		}
	}
	if (UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(execute_data);
		if (EG(exception) != NULL) {
			status = ZEND_NATIVE_EXCEPTION;
		}
	}
	zend_native_execution_cleanup_frame_ex(execute_data, !frame_returned);
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
		if (state->original_return_value == NULL
				&& state->status != ZEND_NATIVE_BAILOUT
				&& !Z_ISUNDEF(state->discarded_return)) {
			zval_ptr_dtor(&state->discarded_return);
		}
	} zend_catch {
		state->status = ZEND_NATIVE_BAILOUT;
	} zend_end_try();
}

/* The status a frame returned with: unwind or abandon its pending calls,
 * initialize its result and verify its return type. */
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
	if (state->status == ZEND_NATIVE_BAILOUT) {
		zend_native_call_direct_abandon(execute_data);
	}
	return frame_returned;
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
	zend_native_execution_context context;
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
	zend_native_execution_context_init(&context);
	if (state->original_return_value == NULL) {
		ZVAL_UNDEF(&state->discarded_return);
		execute_data->return_value = &state->discarded_return;
	}
	zend_try {
		state->status = zend_native_frame_prepare(execute_data) == FAILURE
			? (EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT)
			: entry(execute_data, &context);
		phase = 1;
		frame_returned =
			zend_native_execute_frame_returned(execute_data, state);
		phase = 2;
		if (state->status != ZEND_NATIVE_BAILOUT
				&& UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
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
			if (state->original_return_value == NULL
					&& !Z_ISUNDEF(state->discarded_return)) {
				zval_ptr_dtor(&state->discarded_return);
			}
		}
		phase = 4;
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
			zend_dtrace_user_frame_begin(
				execute_data, &state->dtrace_frame);
			state->dtrace_frame_started = true;
		}
#endif
		if (!state->observer_started) {
			state->observer_started = true;
			ZEND_OBSERVER_FCALL_BEGIN(execute_data);
		}
		/*
		 * RECV initialization and argument type verification are observable
		 * execution.  The VM starts the fcall observer before those opcodes,
		 * so native frames must do the same when a type error or default-value
		 * resolution prevents the machine entry from running.
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
	 * The global native executor, direct native calls and generator resumes
	 * all converge here.  Emit exactly one matching return probe for every
	 * native invocation that returned to this boundary. A bailout has the
	 * same non-returning probe semantics as the VM DTrace wrapper.
	 */
	if (state->dtrace_frame_started
			&& state->status != ZEND_NATIVE_BAILOUT) {
		zend_dtrace_user_frame_end(&state->dtrace_frame);
		state->dtrace_frame_started = false;
	}
#endif

	/*
	 * Generator frames are owned by zend_generator after CREATE. Suspension
	 * deliberately keeps the observer and frame alive, while GENERATOR_RETURN
	 * has already closed and freed the heap frame. Never pass any of these
	 * states through ordinary function-return cleanup.
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
	frame_returned = state->status == ZEND_NATIVE_RETURNED
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

	/*
	 * Releasing CVs, the receiver or a discarded return value may run a
	 * destructor that bails out, for example on an uncaught exception during
	 * shutdown.  Report that as this frame's bailout so callers still release
	 * their activation state before propagating it.
	 */
	zend_try {
		if (state->status != ZEND_NATIVE_BAILOUT) {
			zend_native_execution_cleanup_frame_ex(execute_data, !frame_returned);
		}
		/* A frame entered through zend_execute_ex is normally finalized by
		 * zend_leave_helper(), which also releases the retained closure or
		 * receiver.  The request-local native reentry hook replaces that helper;
		 * its caller still owns the stack frame itself, but not this call-target
		 * reference.  Direct native-to-native calls release their target at their
		 * call site and therefore must not pass through this branch. */
		if (observer_already_started
				&& state->status != ZEND_NATIVE_BAILOUT) {
			uint32_t call_info = ZEND_CALL_INFO(execute_data);

			if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
				OBJ_RELEASE(Z_OBJ(execute_data->This));
			} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
				OBJ_RELEASE(ZEND_CLOSURE_OBJECT(execute_data->func));
			}
		}

		if (state->original_return_value == NULL
				&& state->status != ZEND_NATIVE_BAILOUT
				&& !Z_ISUNDEF(state->discarded_return)) {
			zval_ptr_dtor(&state->discarded_return);
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
			&& (entry = zend_native_code_executable_entry(code)) != NULL)) {
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
