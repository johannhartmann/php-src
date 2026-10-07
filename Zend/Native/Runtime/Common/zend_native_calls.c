/* Native user-function calls over real Zend execution frames. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_operands.h"
#include "Zend/Native/Runtime/Common/zend_native_values.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_API.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_frameless_function.h"
#include "Zend/zend_generators.h"
#include "Zend/zend_object_handlers.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_partial.h"

#include <string.h>
#include <unistd.h>

ZEND_TLS zend_native_reentry_scope *zend_native_active_reentry_scope;
/* Defined with the call-resolution cache below. */
static uint64_t zend_native_call_resolution_cache_epoch;

static zend_always_inline uint64_t
zend_native_call_resolution_cache_epoch_value(void)
{
	return zend_native_call_resolution_cache_epoch;
}

ZEND_TLS zend_native_direct_activation *zend_native_active_direct_call;

typedef struct _zend_native_frozen_call_activation {
	struct _zend_native_frozen_call_activation *next;
	zend_execute_data *frozen_call;
	zval *return_value;
	uint32_t setup_size;
	uint32_t activation_offset;
	uint32_t placements_offset;
	uint32_t tail_size;
	zend_native_direct_activation activation;
	unsigned char tail[];
} zend_native_frozen_call_activation;

static bool zend_native_call_preflight(
	const zend_execute_data *caller, const zend_function *function);
static void zend_native_call_direct_release(
	zend_native_direct_activation *activation);
static void zend_native_call_direct_abandon_activation(
	zend_native_direct_activation *activation);

void zend_native_call_publish_moved_frame(
	zend_execute_data *caller, zend_execute_data *call)
{
	zend_native_direct_activation *activation =
		zend_native_active_direct_call;
	zend_execute_data *previous_call;

	ZEND_ASSERT(caller != NULL && call != NULL);
	previous_call = caller->call;
	ZEND_ASSERT(previous_call != NULL);
	caller->call = call;
	if (activation != NULL && activation->setup_record
			&& activation->caller == caller
			&& activation->callee == previous_call) {
		activation->callee = call;
	}
}

zval zend_native_char_strings[256];

void zend_native_execution_context_init(
	zend_native_execution_context *context)
{
	ZEND_ASSERT(context != NULL);
	if (Z_TYPE(zend_native_char_strings[0]) != IS_STRING) {
		for (uint32_t byte = 0; byte < 256; byte++) {
			ZVAL_INTERNED_STR(&zend_native_char_strings[byte],
				ZSTR_CHAR((zend_uchar) byte));
		}
	}
	context->vm_stack = &EG(vm_stack);
	context->vm_stack_top = &EG(vm_stack_top);
	context->vm_stack_end = &EG(vm_stack_end);
	context->current_execute_data = &EG(current_execute_data);
	context->active_direct_call = (void **) &zend_native_active_direct_call;
	context->map_ptr_base_address = (void **) &CG(map_ptr_base);
	context->vm_interrupt = &EG(vm_interrupt);
	context->exception = &EG(exception);
	context->opline_before_exception = &EG(opline_before_exception);
#ifdef ZEND_CHECK_STACK_LIMIT
	context->stack_limit = &EG(stack_limit);
#else
	context->stack_limit = NULL;
#endif
	context->call_cache_epoch = zend_native_call_cache_epoch_address();
	context->frameless_handlers = (void *const *) zend_flf_handlers;
	context->symbol_table = &EG(symbol_table);
	context->observers_enabled = ZEND_OBSERVER_ENABLED;
	context->deopt_resume = false;
}

void *zend_native_call_fiber_suspend(void)
{
	zend_native_direct_activation *active = zend_native_active_direct_call;
	zend_native_direct_activation *activation;

	/* Native calls keep the active callee in caller->call until their generated
	 * leave sequence runs. Zend's suspended-execution GC interprets that field
	 * exclusively as the head of unfinished call setup. Publish the saved
	 * pending-call chain while this Fiber is inactive, then reconstruct the
	 * native execution links on resume. */
	for (activation = active; activation != NULL;
			activation = activation->previous) {
		zend_native_direct_activation *newer;
		bool caller_published = false;

		activation->fiber_published = false;
		for (newer = active; newer != activation; newer = newer->previous) {
			if (newer->fiber_published
					&& newer->caller == activation->caller) {
				caller_published = true;
				break;
			}
		}
		if (!caller_published && activation->caller != NULL
				&& activation->callee != NULL
				&& activation->caller->call == activation->callee) {
			activation->caller->call = activation->pending_call;
			activation->fiber_published = true;
		}
	}

	zend_native_active_direct_call = NULL;
	return active;
}

void zend_native_call_fiber_resume(void *active_direct_call)
{
	zend_native_direct_activation *activation =
		(zend_native_direct_activation *) active_direct_call;

	ZEND_ASSERT(zend_native_active_direct_call == NULL);
	for (; activation != NULL; activation = activation->previous) {
		if (activation->fiber_published) {
			ZEND_ASSERT(activation->caller->call
				== activation->pending_call);
			activation->caller->call = activation->callee;
			activation->fiber_published = false;
		}
	}
	zend_native_active_direct_call =
		(zend_native_direct_activation *) active_direct_call;
}

void zend_native_call_fiber_destroy(void)
{
	/* A dead Fiber never restores the C stack holding its generated call
	 * continuations. Release every activation before that stack disappears so
	 * prepared callees, invocation targets and code-generation leases retain
	 * the same lifetime as their Zend VM frames. */
	while (zend_native_active_direct_call != NULL) {
		zend_native_direct_activation *activation =
			zend_native_active_direct_call;

		if (activation->status == ZEND_NATIVE_BAILOUT) {
			zend_native_call_direct_abandon_activation(activation);
		} else {
			zend_native_call_direct_release(activation);
		}
	}
}

void zend_native_call_fiber_abandon(void *active_direct_call)
{
	zend_native_direct_activation *activation =
		(zend_native_direct_activation *) active_direct_call;

	/* The abandoned Fiber's VM and C stacks are about to be discarded as a
	 * whole, so their frames and zvals must not be touched. Drop only entry-cell
	 * activity accounting that otherwise keeps the request compiler alive. */
	for (; activation != NULL; activation = activation->previous) {
		if (activation->cell_active && activation->cell != NULL) {
			zend_native_entry_cell_release_active(activation->cell);
			activation->cell_active = false;
		}
	}
}

bool zend_native_generator_freeze_call(
	zend_generator *generator,
	zend_execute_data *execute_data,
	zend_execute_data *call,
	zend_execute_data *frozen_call)
{
	zend_native_direct_activation *activation =
		zend_native_active_direct_call;
	zend_native_frozen_call_activation *frozen;
	zend_execute_data *setup_frame;
	uint32_t activation_offset;
	uint32_t placements_offset = UINT32_MAX;
	uint32_t tail_size;

	if (generator == NULL || execute_data == NULL || call == NULL
			|| frozen_call == NULL || activation == NULL
			|| !activation->setup_record
			|| activation->caller != execute_data
			|| activation->callee != call
			|| (setup_frame = activation->setup_frame) == NULL) {
		return false;
	}
	if ((char *) activation < (char *) setup_frame
			|| (size_t) ((char *) activation - (char *) setup_frame)
				> UINT32_MAX) {
		return false;
	}
	activation_offset = (uint32_t) (
		(char *) activation - (char *) setup_frame);
	if (activation->setup_size < activation_offset
			|| activation->setup_size - activation_offset
				< sizeof(*activation)) {
		return false;
	}
	if (activation->resolution.placements != NULL) {
		if ((char *) activation->resolution.placements
				< (char *) setup_frame
				|| (size_t) ((char *) activation->resolution.placements
					- (char *) setup_frame) > UINT32_MAX) {
			return false;
		}
		placements_offset = (uint32_t) (
			(char *) activation->resolution.placements
				- (char *) setup_frame);
		if (placements_offset >= activation->setup_size) {
			return false;
		}
	}
	tail_size = activation->setup_size - activation_offset
		- (uint32_t) sizeof(*activation);
	frozen = (zend_native_frozen_call_activation *) emalloc(
		sizeof(*frozen) + tail_size);
	frozen->next = (zend_native_frozen_call_activation *)
		generator->native_frozen_call_stack;
	frozen->frozen_call = frozen_call;
	frozen->return_value = call->return_value;
	frozen->setup_size = activation->setup_size;
	frozen->activation_offset = activation_offset;
	frozen->placements_offset = placements_offset;
	frozen->tail_size = tail_size;
	frozen->activation = *activation;
	memcpy(frozen->tail, (char *) activation + sizeof(*activation),
		tail_size);
	generator->native_frozen_call_stack = frozen;

	/* The compact Zend snapshot now owns the raw arguments, while the frozen
	 * native record owns every resolution retain.  Detach the live chain before
	 * either stack frame disappears so the surrounding internal call becomes
	 * active again immediately. */
	zend_native_active_direct_call = activation->previous;
	zend_vm_stack_free_call_frame(call);
	zend_vm_stack_free_call_frame(setup_frame);
	return true;
}

bool zend_native_generator_frozen_call_stack_gc(
	zend_generator *generator,
	zend_execute_data *call,
	zend_get_gc_buffer *gc_buffer)
{
	zend_native_frozen_call_activation *frozen;
	zend_execute_data *frame;

	if (generator == NULL || call == NULL || gc_buffer == NULL
			|| generator->native_frozen_call_stack == NULL) {
		return false;
	}

	/* Native call planning may suspend before an inner INIT has materialized
	 * its Zend frame. The generic source-opline walk then pairs an outer frozen
	 * frame with that inner call and undercounts already-sent arguments. Use the
	 * exact native freeze pairing, but only when it covers the complete compact
	 * stack so mixed VM/native stacks retain the generic fallback. */
	for (frame = call; frame != NULL; frame = frame->prev_execute_data) {
		for (frozen = (zend_native_frozen_call_activation *)
				generator->native_frozen_call_stack;
				frozen != NULL && frozen->frozen_call != frame;
				frozen = frozen->next) {
		}
		if (frozen == NULL) {
			return false;
		}
	}
	for (frozen = (zend_native_frozen_call_activation *)
			generator->native_frozen_call_stack;
			frozen != NULL; frozen = frozen->next) {
		for (frame = call; frame != NULL && frame != frozen->frozen_call;
				frame = frame->prev_execute_data) {
		}
		if (frame == NULL) {
			return false;
		}
	}

	for (frame = call; frame != NULL; frame = frame->prev_execute_data) {
		uint32_t argument_count = ZEND_CALL_NUM_ARGS(frame);

		for (uint32_t i = 0; i < argument_count; i++) {
			zend_get_gc_buffer_add_zval(
				gc_buffer, ZEND_CALL_ARG(frame, i + 1));
		}
		if ((ZEND_CALL_INFO(frame) & ZEND_CALL_RELEASE_THIS) != 0) {
			zend_get_gc_buffer_add_obj(gc_buffer, Z_OBJ(frame->This));
		}
		if ((ZEND_CALL_INFO(frame)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zval *value;

			ZEND_HASH_FOREACH_VAL(frame->extra_named_params, value) {
				zend_get_gc_buffer_add_zval(gc_buffer, value);
			} ZEND_HASH_FOREACH_END();
		}
		if ((frame->func->common.fn_flags & ZEND_ACC_CLOSURE) != 0) {
			zend_get_gc_buffer_add_obj(
				gc_buffer, ZEND_CLOSURE_OBJECT(frame->func));
		}
	}

	for (frozen = (zend_native_frozen_call_activation *)
			generator->native_frozen_call_stack;
			frozen != NULL; frozen = frozen->next) {
		zend_native_direct_activation *activation = &frozen->activation;

		zend_get_gc_buffer_add_zval(
			gc_buffer, &activation->discarded_return);
		if ((activation->resolution.ownership
				& ZEND_NATIVE_USER_CALL_OWNS_EXTRA_NAMED_PARAMS) != 0
				&& activation->resolution.extra_named_params != NULL) {
			zval extra_named_params;

			ZVAL_ARR(&extra_named_params,
				activation->resolution.extra_named_params);
			zend_get_gc_buffer_add_zval(gc_buffer, &extra_named_params);
		}
	}
	return true;
}

void *zend_native_generator_restore_call_begin(
	zend_generator *generator,
	zend_execute_data *frozen_call,
	zend_execute_data *pending_call)
{
	zend_native_frozen_call_activation *frozen;
	zend_native_direct_activation *activation;
	zend_execute_data *setup_frame;
	uint32_t setup_call_info;

	if (generator == NULL || frozen_call == NULL
			|| (frozen = (zend_native_frozen_call_activation *)
				generator->native_frozen_call_stack) == NULL
			|| frozen->frozen_call != frozen_call) {
		return NULL;
	}
	setup_frame = (zend_execute_data *)
		zend_native_frame_activation_reserve(frozen->setup_size);
	if (setup_frame == NULL) {
		return NULL;
	}
	setup_call_info = ZEND_CALL_INFO(setup_frame);
	memset(setup_frame, 0, frozen->setup_size);
	ZEND_CALL_INFO(setup_frame) = setup_call_info;
	activation = (zend_native_direct_activation *) (
		(char *) setup_frame + frozen->activation_offset);
	*activation = frozen->activation;
	memcpy((char *) activation + sizeof(*activation), frozen->tail,
		frozen->tail_size);
	activation->setup_frame = setup_frame;
	activation->caller = generator->execute_data;
	activation->callee = NULL;
	activation->pending_call = pending_call;
	activation->previous = zend_native_active_direct_call;
	if (frozen->placements_offset != UINT32_MAX) {
		activation->resolution.placements =
			(zend_native_user_call_placement *) (
				(char *) setup_frame + frozen->placements_offset);
	}
	zend_native_active_direct_call = activation;
	return frozen;
}

void zend_native_generator_restore_call_finish(
	zend_generator *generator,
	void *restore_state,
	zend_execute_data *call)
{
	zend_native_frozen_call_activation *frozen =
		(zend_native_frozen_call_activation *) restore_state;
	zend_native_direct_activation *activation =
		zend_native_active_direct_call;

	if (frozen == NULL) {
		return;
	}
	ZEND_ASSERT(generator != NULL && call != NULL
		&& generator->native_frozen_call_stack == frozen
		&& activation != NULL
		&& activation->setup_record
		&& activation->caller == generator->execute_data
		&& activation->callee == NULL);
	activation->callee = call;
	call->return_value = activation->uses_discarded_return
		? &activation->discarded_return : frozen->return_value;
	generator->execute_data->call = call;
	generator->native_frozen_call_stack = frozen->next;
	efree(frozen);
}

static bool zend_native_generator_temporary_is_live(
	const zend_op_array *op_array, uint32_t temporary,
	uint32_t source_position)
{
	uint32_t variable = EX_NUM_TO_VAR(temporary);

	for (uint32_t i = 0; i < op_array->last_live_range; i++) {
		const zend_live_range *range = &op_array->live_range[i];

		if (range->start > source_position) {
			break;
		}
		if ((range->var & ~ZEND_LIVE_MASK) == variable
				&& source_position < range->end) {
			return true;
		}
	}
	return false;
}

static bool zend_native_generator_is_send(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
			return true;
		default:
			return false;
	}
}

void zend_native_generator_cleanup_suspended_arguments(
	zend_execute_data *execute_data)
{
	const zend_op_array *op_array = &execute_data->func->op_array;
	uintptr_t opline_address = (uintptr_t) execute_data->opline;
	uintptr_t first_opline_address = (uintptr_t) op_array->opcodes;
	uintptr_t end_opline_address = (uintptr_t) (
		op_array->opcodes + op_array->last);

	/* An exception replaces EX(opline) with the process-wide exception
	 * sentinel. Fatal shutdown can clear EG(opline_before_exception) before a
	 * suspended generator is closed, so never derive a source index until the
	 * frame once again publishes one of its own opcodes. */
	if (opline_address < first_opline_address
			|| opline_address >= end_opline_address
			|| (opline_address - first_opline_address) % sizeof(zend_op) != 0) {
		return;
	}
	uint32_t source_position = (uint32_t) (
		execute_data->opline - op_array->opcodes);

	/* Native call planning delays ownership transfer until the DO_* operation.
	 * If a generator suspends while evaluating arguments, values already
	 * consumed by SEND_* therefore remain in their source temporaries even
	 * though Zend's live ranges ended at the SEND. A VM call frame would own
	 * these values and cleanup_unfinished_calls() would release them before a
	 * forced finally. Control flow through that finally may leave EX(opline)
	 * after a syntactic DO_* that was never executed, so matching INIT/DO pairs
	 * from the final source position is not reliable. Release every initialized
	 * SEND temporary whose live range is no longer active; completed calls have
	 * already consumed their source slots, while cleanup_live_vars() continues
	 * to own all active ranges. */
	for (uint32_t i = 0; i < op_array->last; i++) {
		const zend_op *opline = &op_array->opcodes[i];
		uint32_t temporary;
		zval *value;

		if (!zend_native_generator_is_send(opline->opcode)
				|| (opline->op1_type & (IS_TMP_VAR | IS_VAR)) == 0) {
			continue;
		}
		temporary = EX_VAR_TO_NUM(opline->op1.var);
		value = ZEND_CALL_VAR(execute_data, opline->op1.var);
		if (Z_ISUNDEF_P(value)
				|| zend_native_generator_temporary_is_live(
					op_array, temporary, source_position)) {
			continue;
		}
		zval detached;
		ZVAL_COPY_VALUE(&detached, value);
		ZVAL_UNDEF(value);
		zval_ptr_dtor(&detached);
	}
}

void zend_native_generator_cleanup_call_stack(
	zend_execute_data *execute_data)
{
	if (execute_data != NULL) {
		zend_execute_data *current_execute_data = EG(current_execute_data);

		zend_native_generator_cleanup_suspended_arguments(execute_data);
		zend_native_call_direct_unwind(execute_data);
		/* Generator destruction runs in the caller's execution context.  The
		 * direct-call unwinder publishes each suspended generator caller while
		 * releasing its activation; do not leave that soon-to-be-freed heap frame
		 * installed as the thread's current frame. */
		EG(current_execute_data) = current_execute_data;
	}
}

static zval *zend_native_frameless_slot(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_CV && type != IS_VAR && type != IS_TMP_VAR) {
		return NULL;
	}
	return ZEND_CALL_VAR(execute_data, operand.var);
}

static bool zend_native_frameless_decode_operand(
	zend_execute_data *execute_data, uint64_t encoded,
	uint8_t *operand_type, znode_op *operand)
{
	return zend_native_decode_explicit_operand(
		execute_data, encoded, operand_type, operand);
}

zval *zend_native_call_explicit_slot(
	zend_execute_data *caller,
	uint64_t encoded_operand,
	uint8_t *operand_type)
{
	znode_op operand;

	if (operand_type == NULL
			|| !zend_native_frameless_decode_operand(
				caller, encoded_operand, operand_type, &operand)
			|| (*operand_type != IS_UNUSED
				&& *operand_type != IS_CV
				&& *operand_type != IS_VAR
				&& *operand_type != IS_TMP_VAR)) {
		return NULL;
	}
	return *operand_type == IS_UNUSED
		? NULL : zend_native_frameless_slot(caller, *operand_type, operand);
}

static zval *zend_native_frameless_argument(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	zval *value;

	if (type == IS_CONST) {
		value = operand.constant < execute_data->func->op_array.last_literal
			? &execute_data->func->op_array.literals[operand.constant] : NULL;
	} else {
		value = zend_native_frameless_slot(execute_data, type, operand);
	}
	if (value == NULL) {
		return NULL;
	}
	if (type == IS_CV && UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(operand.var);

		if (variable_index >= execute_data->func->op_array.last_var) {
			return NULL;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			return NULL;
		}
		value = &EG(uninitialized_zval);
	}
	if (Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	return value;
}

zval *zend_native_call_explicit_operand(
	zend_execute_data *caller,
	uint64_t encoded_operand,
	uint8_t *operand_type)
{
	znode_op operand;

	if (operand_type == NULL
			|| !zend_native_frameless_decode_operand(
				caller, encoded_operand, operand_type, &operand)
			|| *operand_type == IS_UNUSED) {
		return NULL;
	}
	return zend_native_frameless_argument(caller, *operand_type, operand);
}

static bool zend_native_receive_verify(
	zend_execute_data *execute_data,
	uint32_t argument_number,
	const zend_arg_info *argument_info,
	zval *argument)
{
	if (argument_info != NULL && ZEND_TYPE_IS_SET(argument_info->type)
			&& !zend_check_type_ex(
				&argument_info->type, argument, false, false)) {
		zend_verify_arg_error(
			execute_data->func, argument_info, argument_number, argument);
		return false;
	}
	return true;
}

zend_native_status zend_native_receive_explicit(
	zend_execute_data *execute_data,
	uint32_t source_opcode,
	uint32_t argument_number,
	uint64_t encoded_op2,
	uint32_t op2_payload,
	uint64_t encoded_result,
	uint32_t source_position)
{
	zend_op_array *op_array;
	zval *result;
	uint8_t result_type;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position >= execute_data->func->op_array.last
			|| argument_number == 0) {
		return ZEND_NATIVE_EXCEPTION;
	}
	op_array = &execute_data->func->op_array;
	execute_data->opline = &op_array->opcodes[source_position];
	result = zend_native_call_explicit_slot(
		execute_data, encoded_result, &result_type);
	if (result == NULL
			|| (result_type != IS_CV
				&& result_type != IS_VAR
				&& result_type != IS_TMP_VAR)) {
		zend_throw_error(NULL, "Invalid native receive result");
		return ZEND_NATIVE_EXCEPTION;
	}

	if (source_opcode == ZEND_RECV) {
		if (argument_number > ZEND_CALL_NUM_ARGS(execute_data)) {
			zend_missing_arg_error(execute_data);
			return ZEND_NATIVE_EXCEPTION;
		}
		if ((op2_payload & (UINT32_C(1) << Z_TYPE_P(result))) == 0) {
			const zend_arg_info *argument_info =
				op_array->arg_info != NULL
					&& argument_number <= op_array->num_args
				? &op_array->arg_info[argument_number - 1] : NULL;
			if (argument_info == NULL
					|| !zend_native_receive_verify(
						execute_data, argument_number,
						argument_info, result)) {
				return ZEND_NATIVE_EXCEPTION;
			}
		}
		return ZEND_NATIVE_RETURNED;
	}

	if (source_opcode == ZEND_RECV_INIT) {
		if (argument_number > ZEND_CALL_NUM_ARGS(execute_data)) {
			uint8_t default_type;
			zval *default_value = zend_native_call_explicit_operand(
				execute_data, encoded_op2, &default_type);
			if (default_value == NULL || default_type != IS_CONST) {
				zend_throw_error(NULL, "Invalid native receive default");
				return ZEND_NATIVE_EXCEPTION;
			}
			if (Z_TYPE_P(default_value) == IS_CONSTANT_AST) {
				ZVAL_COPY(result, default_value);
			} else {
				zend_native_zval_copy_deref_or_dup(result, default_value);
			}
			if (Z_TYPE_P(result) == IS_CONSTANT_AST
					&& zval_update_constant_ex(result, op_array->scope)
						== FAILURE) {
				zval_ptr_dtor_nogc(result);
				ZVAL_UNDEF(result);
				return ZEND_NATIVE_EXCEPTION;
			}
			return ZEND_NATIVE_RETURNED;
		}
		if ((op_array->fn_flags & ZEND_ACC_HAS_TYPE_HINTS) != 0) {
			const zend_arg_info *argument_info =
				op_array->arg_info != NULL
					&& argument_number <= op_array->num_args
				? &op_array->arg_info[argument_number - 1] : NULL;
			if (argument_info == NULL
					|| !zend_native_receive_verify(
						execute_data, argument_number,
						argument_info, result)) {
				return ZEND_NATIVE_EXCEPTION;
			}
		}
		return ZEND_NATIVE_RETURNED;
	}

	if (source_opcode == ZEND_RECV_VARIADIC) {
		uint32_t argument_count = ZEND_CALL_NUM_ARGS(execute_data);
		zend_arg_info *argument_info;

		if ((op_array->fn_flags & ZEND_ACC_VARIADIC) == 0
				|| op_array->num_args + 1 != argument_number
				|| op_array->arg_info == NULL) {
			zend_throw_error(NULL, "Invalid native variadic receive");
			return ZEND_NATIVE_EXCEPTION;
		}
		argument_info = &op_array->arg_info[argument_number - 1];
		if (argument_number <= argument_count) {
			zval *argument = EX_VAR_NUM(op_array->last_var + op_array->T);

			array_init_size(result,
				argument_count - argument_number + 1);
			zend_hash_real_init_packed(Z_ARRVAL_P(result));
			ZEND_HASH_FILL_PACKED(Z_ARRVAL_P(result)) {
				if (ZEND_TYPE_IS_SET(argument_info->type)) {
					ZEND_ADD_CALL_FLAG(
						execute_data, ZEND_CALL_FREE_EXTRA_ARGS);
				}
				do {
					if (!zend_native_receive_verify(
							execute_data, argument_number,
							argument_info, argument)) {
						ZEND_HASH_FILL_FINISH();
						return ZEND_NATIVE_EXCEPTION;
					}
					Z_TRY_ADDREF_P(argument);
					ZEND_HASH_FILL_ADD(argument);
					argument++;
				} while (++argument_number <= argument_count);
			} ZEND_HASH_FILL_END();
		} else {
			ZVAL_EMPTY_ARRAY(result);
		}
		if ((ZEND_CALL_INFO(execute_data)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_string *name;
			zval *argument;

			if (ZEND_TYPE_IS_SET(argument_info->type)) {
				SEPARATE_ARRAY(result);
				ZEND_HASH_MAP_FOREACH_STR_KEY_VAL(
						execute_data->extra_named_params,
						name, argument) {
					if (!zend_native_receive_verify(
							execute_data, argument_number,
							argument_info, argument)) {
						return ZEND_NATIVE_EXCEPTION;
					}
					Z_TRY_ADDREF_P(argument);
					zend_hash_add_new(
						Z_ARRVAL_P(result), name, argument);
				} ZEND_HASH_FOREACH_END();
			} else if (zend_hash_num_elements(Z_ARRVAL_P(result)) == 0) {
				GC_ADDREF(execute_data->extra_named_params);
				ZVAL_ARR(result, execute_data->extra_named_params);
			} else {
				SEPARATE_ARRAY(result);
				ZEND_HASH_MAP_FOREACH_STR_KEY_VAL(
						execute_data->extra_named_params,
						name, argument) {
					Z_TRY_ADDREF_P(argument);
					zend_hash_add_new(
						Z_ARRVAL_P(result), name, argument);
				} ZEND_HASH_FOREACH_END();
			}
		}
		return EG(exception) == NULL
			? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
	}

	zend_throw_error(NULL, "Invalid native receive opcode");
	return ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_receive_explicit_pending(
	zend_execute_data *caller,
	uint32_t source_opcode,
	uint32_t argument_number,
	uint64_t encoded_op2,
	uint32_t op2_payload,
	uint64_t encoded_result,
	uint32_t source_position)
{
	if (caller == NULL || caller->call == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Native pending receive frame is invalid");
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_receive_explicit(
		caller->call, source_opcode, argument_number, encoded_op2,
		op2_payload, encoded_result, source_position);
}

static void zend_native_frameless_consume(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	zval *slot;

	if (type != IS_TMP_VAR && type != IS_VAR) {
		return;
	}
	slot = zend_native_frameless_slot(execute_data, type, operand);
	if (slot != NULL && !Z_ISUNDEF_P(slot)) {
		zval_ptr_dtor(slot);
		ZVAL_UNDEF(slot);
	}
}

static void zend_native_frameless_observed_call_explicit(
	zend_execute_data *execute_data, zend_function *function,
	zval *result, zval **arguments, uint32_t argument_count)
{
	zend_execute_data *call = zend_vm_stack_push_call_frame_ex(
		zend_vm_calc_used_stack(argument_count, function),
		ZEND_CALL_NESTED_FUNCTION, function, argument_count, NULL);
	uint32_t index;
	uint32_t call_info;

	call->prev_execute_data = execute_data;
	for (index = 0; index < argument_count; index++) {
		if (Z_ISUNDEF_P(arguments[index])) {
			ZVAL_NULL(ZEND_CALL_VAR_NUM(call, index));
		} else {
			ZVAL_COPY_DEREF(ZEND_CALL_VAR_NUM(call, index), arguments[index]);
		}
	}
	EG(current_execute_data) = call;
	zend_observer_fcall_begin_prechecked(call, ZEND_OBSERVER_DATA(function));
	function->internal_function.handler(call, result);
	zend_observer_fcall_end(call, result);
	EG(current_execute_data) = execute_data;
	if (UNEXPECTED(EG(exception) != NULL)) {
		zend_rethrow_exception(execute_data);
	}
	zend_vm_stack_free_args(call);
	call_info = ZEND_CALL_INFO(call);
	if (UNEXPECTED(call_info & ZEND_CALL_ALLOCATED)) {
		zend_vm_stack_free_call_frame_ex(call_info, call);
	} else {
		EG(vm_stack_top) = (zval *) call;
	}
}

static zend_never_inline zend_native_status
zend_native_call_frameless_general(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t more_slots);

static zend_always_inline zval *zend_native_call_frameless_argument(
	zend_execute_data *execute_data, uint64_t descriptor, uint32_t index,
	uint32_t offset)
{
	return ((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT + index))
			& 1)
		? &execute_data->func->op_array.literals[offset]
		: (zval *) ((char *) execute_data + offset);
}

static zend_always_inline void zend_native_call_frameless_release(
	zend_execute_data *execute_data, uint64_t descriptor, uint32_t index,
	uint32_t offset)
{
	if ((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT + index)) & 1) {
		zval *slot = (zval *) ((char *) execute_data + offset);

		if (!Z_ISUNDEF_P(slot)) {
			zval_ptr_dtor(slot);
			ZVAL_UNDEF(slot);
		}
	}
}

/*
 * The VM's FRAMELESS_ICALL_2 and _3 handlers for defined arguments of an
 * unobserved function; an undefined CV, which warns, and everything else
 * take the general form below.
 */
zend_native_status zend_native_call_frameless_direct(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t more_slots)
{
	const uint32_t handler_index = (uint32_t) (descriptor & 0xffff);
	const uint32_t argument_count = (uint32_t) ((descriptor >> 16) & 3);

	if (EXPECTED(argument_count >= 2)
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
			&& !(ZEND_OBSERVER_ENABLED
				&& UNEXPECTED(!zend_observer_handler_is_unobserved(
					ZEND_OBSERVER_DATA(zend_flf_functions[handler_index]))))
#endif
			) {
		zval *first = zend_native_call_frameless_argument(
			execute_data, descriptor, 0, (uint32_t) (slots >> 32));
		zval *second = zend_native_call_frameless_argument(
			execute_data, descriptor, 1, (uint32_t) more_slots);
		zval *third = argument_count == 3
			? zend_native_call_frameless_argument(
				execute_data, descriptor, 2, (uint32_t) (more_slots >> 32))
			: NULL;

		if (Z_TYPE_P(first) != IS_UNDEF && Z_TYPE_P(second) != IS_UNDEF
				&& (third == NULL || Z_TYPE_P(third) != IS_UNDEF)) {
			zval *result = (zval *) ((char *) execute_data + (uint32_t) slots);

			execute_data->opline =
				&execute_data->func->op_array.opcodes[descriptor >> 32];
			ZVAL_NULL(result);
			if (third == NULL) {
				((zend_frameless_function_2)
					zend_flf_handlers[handler_index])(result,
						Z_ISREF_P(first) ? Z_REFVAL_P(first) : first,
						Z_ISREF_P(second) ? Z_REFVAL_P(second) : second);
			} else {
				((zend_frameless_function_3)
					zend_flf_handlers[handler_index])(result,
						Z_ISREF_P(first) ? Z_REFVAL_P(first) : first,
						Z_ISREF_P(second) ? Z_REFVAL_P(second) : second,
						Z_ISREF_P(third) ? Z_REFVAL_P(third) : third);
			}
			zend_native_call_frameless_release(execute_data, descriptor, 0,
				(uint32_t) (slots >> 32));
			zend_native_call_frameless_release(execute_data, descriptor, 1,
				(uint32_t) more_slots);
			if (third != NULL) {
				zend_native_call_frameless_release(execute_data, descriptor,
					2, (uint32_t) (more_slots >> 32));
			}
			return EG(exception) == NULL
				? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
		}
	}
	return zend_native_call_frameless_general(execute_data,
		descriptor, slots, more_slots);
}

static zend_never_inline zend_native_status
zend_native_call_frameless_general(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t more_slots)
{
	const uint32_t handler_index = (uint32_t) (descriptor & 0xffff);
	const uint32_t argument_count = (uint32_t) ((descriptor >> 16) & 3);
	const uint32_t offsets[3] = {
		(uint32_t) (slots >> 32), (uint32_t) more_slots,
		(uint32_t) (more_slots >> 32)};
	zend_op_array *op_array = &execute_data->func->op_array;
	zval *result = (zval *) ((char *) execute_data + (uint32_t) slots);
	zval *arguments[3] = {NULL, NULL, NULL};
	uint32_t index;

	ZEND_ASSERT(handler_index < zend_flf_count);
	execute_data->opline = &op_array->opcodes[descriptor >> 32];
	for (index = 0; index < argument_count; index++) {
		zval *value;

		if ((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT + index))
				& 1) {
			value = &op_array->literals[offsets[index]];
		} else {
			value = (zval *) ((char *) execute_data + offsets[index]);
		}
		if (((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_CV_SHIFT + index))
					& 1)
				&& UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
			uint32_t variable_index = (uint32_t)
				(value - ZEND_CALL_VAR_NUM(execute_data, 0));

			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(op_array->vars[variable_index]));
			if (EG(exception) != NULL) {
				goto cleanup;
			}
			value = &EG(uninitialized_zval);
		}
		ZVAL_DEREF(value);
		arguments[index] = value;
	}
	ZVAL_NULL(result);
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
	if (ZEND_OBSERVER_ENABLED && UNEXPECTED(!zend_observer_handler_is_unobserved(
			ZEND_OBSERVER_DATA(zend_flf_functions[handler_index])))) {
		zend_native_frameless_observed_call_explicit(
			execute_data, zend_flf_functions[handler_index],
			result, arguments, argument_count);
	} else
#endif
	switch (argument_count) {
		case 0:
			((zend_frameless_function_0)
				zend_flf_handlers[handler_index])(result);
			break;
		case 1:
			((zend_frameless_function_1)
				zend_flf_handlers[handler_index])(result, arguments[0]);
			break;
		case 2:
			((zend_frameless_function_2)
				zend_flf_handlers[handler_index])(
					result, arguments[0], arguments[1]);
			break;
		default:
			((zend_frameless_function_3)
				zend_flf_handlers[handler_index])(
					result, arguments[0], arguments[1], arguments[2]);
			break;
	}

cleanup:
	for (index = 0; index < argument_count; index++) {
		if ((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT + index))
				& 1) {
			zval *slot = (zval *) ((char *) execute_data + offsets[index]);

			if (!Z_ISUNDEF_P(slot)) {
				zval_ptr_dtor(slot);
				ZVAL_UNDEF(slot);
			}
		}
	}
	return EG(exception) == NULL ? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

/*
 * The general form of zend_native_call_frameless_*_address(): undefined
 * CVs warn, observers are notified; the arguments are given by address.
 */
static zend_never_inline zend_native_status
zend_native_call_frameless_address_general(
	zend_execute_data *execute_data, zval *result, zval **arguments,
	uint32_t argument_count, uint64_t descriptor)
{
	const uint32_t handler_index = (uint32_t) (descriptor & 0xffff);
	zend_op_array *op_array = &execute_data->func->op_array;
	zval *values[3] = {NULL, NULL, NULL};
	uint32_t index;

	ZEND_ASSERT(handler_index < zend_flf_count);
	execute_data->opline = &op_array->opcodes[descriptor >> 32];
	for (index = 0; index < argument_count; index++) {
		zval *value = arguments[index];

		if (((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_CV_SHIFT + index))
					& 1)
				&& UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
			uint32_t variable_index = (uint32_t)
				(value - ZEND_CALL_VAR_NUM(execute_data, 0));

			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(op_array->vars[variable_index]));
			if (EG(exception) != NULL) {
				goto cleanup;
			}
			value = &EG(uninitialized_zval);
		}
		ZVAL_DEREF(value);
		values[index] = value;
	}
	ZVAL_NULL(result);
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
	if (ZEND_OBSERVER_ENABLED && UNEXPECTED(!zend_observer_handler_is_unobserved(
			ZEND_OBSERVER_DATA(zend_flf_functions[handler_index])))) {
		zend_native_frameless_observed_call_explicit(
			execute_data, zend_flf_functions[handler_index],
			result, values, argument_count);
	} else
#endif
	if (argument_count == 2) {
		((zend_frameless_function_2) zend_flf_handlers[handler_index])(
			result, values[0], values[1]);
	} else {
		((zend_frameless_function_3) zend_flf_handlers[handler_index])(
			result, values[0], values[1], values[2]);
	}

cleanup:
	for (index = 0; index < argument_count; index++) {
		if ((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT + index))
				& 1) {
			zval *slot = arguments[index];

			if (!Z_ISUNDEF_P(slot)) {
				zval_ptr_dtor(slot);
				ZVAL_UNDEF(slot);
			}
		}
	}
	return EG(exception) == NULL ? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

static zend_always_inline void zend_native_call_frameless_release_address(
	uint64_t descriptor, uint32_t index, zval *slot)
{
	if (((descriptor >> (ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT + index)) & 1)
			&& !Z_ISUNDEF_P(slot)) {
		/* FREE_OP of a temporary argument. */
		zval_ptr_dtor_nogc(slot);
		ZVAL_UNDEF(slot);
	}
}

static zend_always_inline bool zend_native_call_frameless_unobserved(
	uint32_t handler_index)
{
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
	return !(ZEND_OBSERVER_ENABLED
		&& UNEXPECTED(!zend_observer_handler_is_unobserved(
			ZEND_OBSERVER_DATA(zend_flf_functions[handler_index]))));
#else
	(void) handler_index;
	return true;
#endif
}

zend_native_status zend_native_call_frameless_2_address(
	zend_execute_data *execute_data, zval *result, zval *first,
	zval *second, uint64_t descriptor)
{
	const uint32_t handler_index = (uint32_t) (descriptor & 0xffff);

	if (EXPECTED(Z_TYPE_P(first) != IS_UNDEF && Z_TYPE_P(second) != IS_UNDEF
			&& zend_native_call_frameless_unobserved(handler_index))) {
		execute_data->opline =
			&execute_data->func->op_array.opcodes[descriptor >> 32];
		ZVAL_NULL(result);
		((zend_frameless_function_2) zend_flf_handlers[handler_index])(result,
			Z_ISREF_P(first) ? Z_REFVAL_P(first) : first,
			Z_ISREF_P(second) ? Z_REFVAL_P(second) : second);
		zend_native_call_frameless_release_address(descriptor, 0, first);
		zend_native_call_frameless_release_address(descriptor, 1, second);
		return EG(exception) == NULL
			? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
	}
	{
		zval *arguments[2] = {first, second};

		return zend_native_call_frameless_address_general(
			execute_data, result, arguments, 2, descriptor);
	}
}

zend_native_status zend_native_call_frameless_3_address(
	zend_execute_data *execute_data, zval *result, zval *first,
	zval *second, zval *third, uint64_t descriptor)
{
	const uint32_t handler_index = (uint32_t) (descriptor & 0xffff);

	if (EXPECTED(Z_TYPE_P(first) != IS_UNDEF && Z_TYPE_P(second) != IS_UNDEF
			&& Z_TYPE_P(third) != IS_UNDEF
			&& zend_native_call_frameless_unobserved(handler_index))) {
		execute_data->opline =
			&execute_data->func->op_array.opcodes[descriptor >> 32];
		ZVAL_NULL(result);
		((zend_frameless_function_3) zend_flf_handlers[handler_index])(result,
			Z_ISREF_P(first) ? Z_REFVAL_P(first) : first,
			Z_ISREF_P(second) ? Z_REFVAL_P(second) : second,
			Z_ISREF_P(third) ? Z_REFVAL_P(third) : third);
		zend_native_call_frameless_release_address(descriptor, 0, first);
		zend_native_call_frameless_release_address(descriptor, 1, second);
		zend_native_call_frameless_release_address(descriptor, 2, third);
		return EG(exception) == NULL
			? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
	}
	{
		zval *arguments[3] = {first, second, third};

		return zend_native_call_frameless_address_general(
			execute_data, result, arguments, 3, descriptor);
	}
}

/*
 * The VM's FRAMELESS_ICALL_1 handler: the handler writes the result from
 * the (dereferenced) argument, and a temporary argument is released. An
 * undefined CV, which warns, and an observed function take the general form.
 */
zend_native_status zend_native_call_frameless_1(
	zend_execute_data *execute_data, uint64_t descriptor, uint64_t slots, uint64_t more_slots)
{
	const uint32_t handler_index = (uint32_t) (descriptor & 0xffff);
	zval *result = (zval *) ((char *) execute_data + (uint32_t) slots);
	zval *argument = ((descriptor
			>> ZEND_NATIVE_FRAMELESS_DIRECT_CONST_SHIFT) & 1)
		? &execute_data->func->op_array.literals[(uint32_t) (slots >> 32)]
		: (zval *) ((char *) execute_data + (uint32_t) (slots >> 32));

	if (UNEXPECTED(Z_TYPE_P(argument) == IS_UNDEF)
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
			|| (ZEND_OBSERVER_ENABLED
				&& UNEXPECTED(!zend_observer_handler_is_unobserved(
					ZEND_OBSERVER_DATA(zend_flf_functions[handler_index]))))
#endif
			) {
		return zend_native_call_frameless_direct(
			execute_data, descriptor, slots, more_slots);
	}
	execute_data->opline =
		&execute_data->func->op_array.opcodes[descriptor >> 32];
	ZVAL_NULL(result);
	((zend_frameless_function_1) zend_flf_handlers[handler_index])(
		result, Z_ISREF_P(argument) ? Z_REFVAL_P(argument) : argument);
	if ((descriptor >> ZEND_NATIVE_FRAMELESS_DIRECT_TMP_SHIFT) & 1) {
		zval_ptr_dtor_nogc(argument);
		ZVAL_UNDEF(argument);
	}
	return EG(exception) == NULL ? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_call_frameless_internal(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result_operand, uint64_t auxiliary,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	uint64_t encoded_arguments[3] = {op1, op2, auxiliary};
	uint8_t argument_types[3];
	znode_op argument_operands[3];
	uint8_t result_type;
	znode_op result_node;
	zval *arguments[3] = {NULL, NULL, NULL};
	zval *result;
	uint32_t argument_count;
	uint32_t index;
	zend_native_status status = ZEND_NATIVE_RETURNED;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position_id >= execute_data->func->op_array.last
			|| !ZEND_OP_IS_FRAMELESS_ICALL(source_opcode)
			|| extended_value >= zend_flf_count
			|| zend_flf_handlers[extended_value] == NULL
			|| zend_flf_functions[extended_value] == NULL
			|| !zend_native_frameless_decode_operand(
				execute_data, result_operand, &result_type, &result_node)
			|| result_type == IS_UNUSED
			|| (result = zend_native_frameless_slot(
				execute_data, result_type, result_node)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	argument_count = ZEND_FLF_NUM_ARGS(source_opcode);
	for (index = 0; index < 3; index++) {
		if (!zend_native_frameless_decode_operand(
				execute_data, encoded_arguments[index],
				&argument_types[index], &argument_operands[index])
				|| (index < argument_count
					? argument_types[index] == IS_UNUSED
					: argument_types[index] != IS_UNUSED)) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	/*
	 * The source position remains available for diagnostics, observers and
	 * exceptions. No semantic operand is read from this zend_op.
	 */
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	for (index = 0; index < argument_count; index++) {
		arguments[index] = zend_native_frameless_argument(
			execute_data, argument_types[index], argument_operands[index]);
		if (arguments[index] == NULL) {
			status = ZEND_NATIVE_EXCEPTION;
			goto cleanup;
		}
	}
	ZVAL_NULL(result);
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
	if (ZEND_OBSERVER_ENABLED && UNEXPECTED(!zend_observer_handler_is_unobserved(
			ZEND_OBSERVER_DATA(zend_flf_functions[extended_value])))) {
		zend_native_frameless_observed_call_explicit(
			execute_data, zend_flf_functions[extended_value],
			result, arguments, argument_count);
	} else
#endif
	{
		switch (argument_count) {
			case 0:
				((zend_frameless_function_0)
					zend_flf_handlers[extended_value])(result);
				break;
			case 1:
				((zend_frameless_function_1)
					zend_flf_handlers[extended_value])(
						result, arguments[0]);
				break;
			case 2:
				((zend_frameless_function_2)
					zend_flf_handlers[extended_value])(
						result, arguments[0], arguments[1]);
				break;
			case 3:
				((zend_frameless_function_3)
					zend_flf_handlers[extended_value])(
						result, arguments[0], arguments[1], arguments[2]);
				break;
			default:
				return ZEND_NATIVE_EXCEPTION;
		}
	}

cleanup:
	for (index = 0; index < argument_count; index++) {
		zend_native_frameless_consume(
			execute_data, argument_types[index], argument_operands[index]);
	}
	return EG(exception) == NULL ? status : ZEND_NATIVE_EXCEPTION;
}

static zend_native_entry_cell *zend_native_reentry_find(
	zend_native_reentry_scope *scope, zend_function *function)
{
	zend_native_reentry_scope *current;

	for (current = scope; current != NULL; current = current->previous) {
		zend_native_entry_cell *cell = current->resolver(
			current->resolver_context, function);

		if (cell != NULL) {
			return cell;
		}
	}
	return NULL;
}

/*
 * The cells reentries resolved under the active scope, by code: a
 * closure or an inherited copy shares the opcodes of the function it was
 * made from and resolves to the same cell. Entering or leaving a scope and
 * every call-resolution invalidation (a retired or reset cell, a destroyed
 * compiler, request end) start a new epoch.
 */
#define ZEND_NATIVE_REENTRY_CACHE_SIZE 64
typedef struct _zend_native_reentry_cache_entry {
	const zend_native_reentry_scope *scope;
	const zend_op *opcodes;
	zend_native_entry_cell *cell;
	uint64_t epoch;
	uint32_t last;
} zend_native_reentry_cache_entry;
ZEND_TLS zend_native_reentry_cache_entry
	zend_native_reentry_cache[ZEND_NATIVE_REENTRY_CACHE_SIZE];
ZEND_TLS uint64_t zend_native_reentry_cache_scope_epoch;

static zend_always_inline uint64_t zend_native_reentry_cache_epoch(void)
{
	return zend_native_call_resolution_cache_epoch_value()
		+ (zend_native_reentry_cache_scope_epoch << 40);
}

zend_native_entry_cell *zend_native_reentry_resolve(
	zend_function *function)
{
	zend_native_reentry_cache_entry *cached;
	zend_native_entry_cell *cell;
	uint64_t epoch;

	if (function == NULL || !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	epoch = zend_native_reentry_cache_epoch();
	cached = &zend_native_reentry_cache[
		((uintptr_t) function->op_array.opcodes >> 6)
			& (ZEND_NATIVE_REENTRY_CACHE_SIZE - 1)];
	if (cached->opcodes == function->op_array.opcodes
			&& cached->last == function->op_array.last
			&& cached->scope == zend_native_active_reentry_scope
			&& cached->epoch == epoch
			&& cached->cell->state == ZEND_NATIVE_ENTRY_READY) {
		return cached->cell;
	}
	cell = zend_native_reentry_find(
		zend_native_active_reentry_scope, function);
	/* Resolution may compile and invalidate: cache under the epoch that
	 * holds afterwards, and only cells naming this code. */
	if (cell != NULL && cell->state == ZEND_NATIVE_ENTRY_READY
			&& cell->function != NULL
			&& cell->function->op_array.opcodes
				== function->op_array.opcodes
			&& cell->function->op_array.last == function->op_array.last) {
		cached->scope = zend_native_active_reentry_scope;
		cached->opcodes = function->op_array.opcodes;
		cached->last = function->op_array.last;
		cached->cell = cell;
		cached->epoch = zend_native_reentry_cache_epoch();
	}
	return cell;
}

static zend_native_user_opcode_result zend_native_user_opcode_result_make(
	zend_execute_data *execute_data, uint32_t action)
{
	zend_native_user_opcode_result result = {
		.action = UINT32_MAX,
		.source_position = UINT32_MAX,
	};
	const zend_op_array *op_array;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| execute_data->opline == NULL) {
		return result;
	}
	op_array = &execute_data->func->op_array;
	if (execute_data->opline < op_array->opcodes
			|| execute_data->opline >= op_array->opcodes + op_array->last) {
		return result;
	}
	result.action = action;
	result.source_position =
		(uint32_t) (execute_data->opline - op_array->opcodes);
	return result;
}

zend_native_user_opcode_result zend_native_user_opcode_invoke(
	zend_execute_data *execute_data,
	zend_native_execution_context *context,
	uint32_t source_position_id)
{
	user_opcode_handler_t handler;
	zend_execute_data *entered;
	zend_native_entry_cell *cell;
	const zend_native_code *code;
	zend_object *entry_exception;
	zend_native_status status;
	int action;

	if (execute_data == NULL || context == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position_id >= execute_data->func->op_array.last) {
		return zend_native_user_opcode_result_make(NULL, UINT32_MAX);
	}
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	handler = zend_get_user_opcode_handler(execute_data->opline->opcode);
	if (handler == NULL) {
		return zend_native_user_opcode_result_make(
			execute_data, ZEND_USER_OPCODE_DISPATCH);
	}
	EG(current_execute_data) = execute_data;
	entry_exception = EG(exception);
	action = handler(execute_data);
	if (EG(exception) != NULL && EG(exception) != entry_exception) {
		EG(current_execute_data) = execute_data;
		return zend_native_user_opcode_result_make(NULL, UINT32_MAX);
	}
	if (action != ZEND_USER_OPCODE_ENTER) {
		return zend_native_user_opcode_result_make(
			execute_data, (uint32_t) action);
	}

	/*
	 * ENTER changes the active Zend frame. Resolve that frame through the
	 * process-local native entry registry and execute its published native
	 * body; no VM executor or opline handler participates.
	 */
	entered = EG(current_execute_data);
	if (entered == NULL || entered == execute_data
			|| (cell = zend_native_reentry_resolve(entered->func)) == NULL
			|| (code = zend_native_entry_cell_load(cell)) == NULL) {
		EG(current_execute_data) = execute_data;
		zend_throw_error(NULL,
			"User opcode ENTER target has no published native entry");
		return zend_native_user_opcode_result_make(NULL, UINT32_MAX);
	}
	if (cell->frame_probe != NULL) {
		cell->frame_probe(
			cell->frame_probe_context, execute_data, entered);
	}
	zend_native_entry_cell_retain_active(cell);
	status = zend_native_execute_observed_frame(code, entered, NULL);
	zend_native_entry_cell_release_active(cell);
	EG(current_execute_data) = execute_data;
	zend_vm_stack_free_call_frame(entered);
	if (status == ZEND_NATIVE_BAILOUT) {
		zend_bailout();
	}
	if (status == ZEND_NATIVE_EXCEPTION || EG(exception) != NULL) {
		return zend_native_user_opcode_result_make(NULL, UINT32_MAX);
	}
	return zend_native_user_opcode_result_make(
		execute_data, ZEND_USER_OPCODE_CONTINUE);
}

zend_result zend_native_reentry_startup(void)
{
	return zend_native_active_reentry_scope == NULL ? SUCCESS : FAILURE;
}

void zend_native_reentry_shutdown(void)
{
	zend_native_active_reentry_scope = NULL;
}

zend_result zend_native_reentry_scope_enter_resolver(
	zend_native_reentry_scope *scope,
	zend_native_reentry_resolver_t resolver,
	void *resolver_context)
{
	if (scope == NULL || resolver == NULL) {
		return FAILURE;
	}
	scope->resolver = resolver;
	scope->resolver_context = resolver_context;
	scope->previous = zend_native_active_reentry_scope;
	zend_native_active_reentry_scope = scope;
	zend_native_reentry_cache_scope_epoch++;
	return SUCCESS;
}

void zend_native_reentry_scope_leave(zend_native_reentry_scope *scope)
{
	ZEND_ASSERT(scope != NULL && zend_native_active_reentry_scope == scope);
	if (scope != NULL && zend_native_active_reentry_scope == scope) {
		zend_native_reentry_cache_scope_epoch++;
		zend_native_active_reentry_scope = scope->previous;
		scope->resolver = NULL;
		scope->resolver_context = NULL;
		scope->previous = NULL;
	}
}

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
			|| !ZEND_USER_CODE(cell->function->type)
			|| cell->state != ZEND_NATIVE_ENTRY_UNCOMPILED) {
		return FAILURE;
	}
	cell->state = ZEND_NATIVE_ENTRY_COMPILING;
	return SUCCESS;
}

/* Development aid: with ZEND_NATIVE_PERF_MAP set, record which PHP function
 * each published entry runs, to rename the entries of perf's JIT map. */
static void zend_native_perf_name_entry(
	const zend_native_entry_cell *cell, const zend_native_code *code)
{
	static int enabled = -1;
	const zend_function *function = cell->function;
	char path[64];
	FILE *names;

	if (enabled < 0) {
		const char *perf_map = getenv("ZEND_NATIVE_PERF_MAP");
		enabled = perf_map != NULL && perf_map[0] != '\0';
	}
	if (!enabled || function == NULL) {
		return;
	}
	snprintf(path, sizeof(path), "/tmp/perf-%d.names", (int) getpid());
	names = fopen(path, "a");
	if (names == NULL) {
		return;
	}
	fprintf(names, "%lx %s%s%s %s:%u\n",
		(unsigned long) (uintptr_t) zend_native_code_frame_entry(code),
		function->common.scope != NULL
			? ZSTR_VAL(function->common.scope->name) : "",
		function->common.scope != NULL ? "::" : "",
		function->common.function_name != NULL
			? ZSTR_VAL(function->common.function_name) : "{main}",
		ZEND_USER_CODE(function->type) && function->op_array.filename
			? ZSTR_VAL(function->op_array.filename) : "",
		ZEND_USER_CODE(function->type) ? function->op_array.line_start : 0);
	fclose(names);
}

zend_result zend_native_entry_cell_publish(
	zend_native_entry_cell *cell, const zend_native_code *code)
{
	if (cell == NULL || code == NULL
			|| cell->state != ZEND_NATIVE_ENTRY_COMPILING) {
		return FAILURE;
	}
	zend_native_perf_name_entry(cell, code);
	if (cell->tier2_state == ZEND_NATIVE_TIER2_NONE) {
		cell->tier2_countdown = zend_native_tier2_threshold();
	}
	cell->generation++;
	cell->published_epoch = cell->generation;
	cell->state = ZEND_NATIVE_ENTRY_READY;
	__atomic_store_n(&cell->code, code, __ATOMIC_RELEASE);
	return SUCCESS;
}

void zend_native_entry_cell_fail(zend_native_entry_cell *cell)
{
	if (cell != NULL && cell->active_calls == 0
			&& cell->suspended_frames == 0
			&& (cell->state == ZEND_NATIVE_ENTRY_COMPILING
				|| cell->state == ZEND_NATIVE_ENTRY_READY)) {
		__atomic_store_n(&cell->code, NULL, __ATOMIC_RELEASE);
		cell->retired_epoch = cell->published_epoch;
		cell->state = ZEND_NATIVE_ENTRY_FAILED;
	}
}

zend_result zend_native_entry_cell_reset(zend_native_entry_cell *cell)
{
	if (cell == NULL || cell->active_calls != 0
			|| cell->suspended_frames != 0) {
		return FAILURE;
	}
	zend_native_call_resolution_cache_invalidate();
	__atomic_store_n(&cell->code, NULL, __ATOMIC_RELEASE);
	cell->retired_epoch = cell->published_epoch;
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
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		return FAILURE;
	}
	op_array = &execute_data->func->op_array;
	supplied = ZEND_CALL_NUM_ARGS(execute_data);
	if (op_array->num_args != 0 && op_array->arg_info == NULL) {
		return FAILURE;
	}
	/* RECV opcodes validate in declaration order. In particular, a bad
	 * supplied argument wins over a later missing argument. */
	for (ordinal = 0; ordinal < op_array->num_args; ordinal++) {
		const zend_op *receive = &op_array->opcodes[ordinal];
		zval *argument = ZEND_CALL_ARG(execute_data, ordinal + 1);
		const zend_arg_info *argument_info = &op_array->arg_info[ordinal];
		const zval *default_value;

		if (ordinal < supplied) {
			if (ZEND_TYPE_IS_SET(argument_info->type)
					&& !zend_check_type_ex(
						&argument_info->type, argument, false, false)) {
				zend_verify_arg_error(
					execute_data->func, argument_info, ordinal + 1, argument);
				return FAILURE;
			}
			continue;
		}
		if (receive->opcode == ZEND_RECV) {
			zend_missing_arg_error(execute_data);
			return FAILURE;
		}
		if (receive->opcode != ZEND_RECV_INIT
				|| receive->op1.num != ordinal + 1
				|| receive->op2_type != IS_CONST) {
			return FAILURE;
		}
		default_value = RT_CONSTANT(receive, receive->op2);
		if (Z_TYPE_P(default_value) == IS_CONSTANT_AST) {
			ZVAL_COPY(argument, default_value);
		} else {
			zend_native_zval_copy_deref_or_dup(argument, default_value);
		}
		if (Z_TYPE_P(argument) == IS_CONSTANT_AST
				&& zval_update_constant_ex(argument, op_array->scope) == FAILURE) {
			zval_ptr_dtor_nogc(argument);
			ZVAL_UNDEF(argument);
			return FAILURE;
		}
		if (ZEND_TYPE_IS_SET(argument_info->type)
				&& !zend_check_type_ex(
					&argument_info->type, argument, false, false)) {
			zend_verify_arg_error(
				execute_data->func, argument_info, ordinal + 1, argument);
			return FAILURE;
		}
	}
	if ((op_array->fn_flags & ZEND_ACC_VARIADIC) != 0) {
		const zend_arg_info *argument_info =
			&op_array->arg_info[op_array->num_args];
		zval *variadic = ZEND_CALL_VAR_NUM(
			execute_data, op_array->num_args);
		uint32_t argument_number = op_array->num_args + 1;
		uint32_t extra_count = supplied > op_array->num_args
			? supplied - op_array->num_args : 0;
		uint32_t named_count =
			(ZEND_CALL_INFO(execute_data)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0
			? zend_hash_num_elements(execute_data->extra_named_params) : 0;

		array_init_size(variadic, extra_count + named_count);
		if (extra_count != 0 && ZEND_TYPE_IS_SET(argument_info->type)) {
			/* Type verification may coerce positional extra arguments in place.
			 * Match ZEND_RECV_VARIADIC by making the frame cleanup own those
			 * converted temporaries after the variadic array takes its copy. */
			ZEND_ADD_CALL_FLAG(execute_data, ZEND_CALL_FREE_EXTRA_ARGS);
		}
		for (ordinal = 0; ordinal < extra_count; ordinal++) {
			zval *source = ZEND_CALL_VAR_NUM(execute_data,
				op_array->last_var + op_array->T + ordinal);
			zval copy;
			if (ZEND_TYPE_IS_SET(argument_info->type)
					&& !zend_check_type_ex(
						&argument_info->type, source, false, false)) {
				zend_verify_arg_error(execute_data->func, argument_info,
					argument_number + ordinal, source);
				return FAILURE;
			}
			ZVAL_COPY(&copy, source);
			zend_hash_next_index_insert(Z_ARRVAL_P(variadic), &copy);
		}
		if (named_count != 0) {
			zend_string *name;
			zval *source;
			ZEND_HASH_MAP_FOREACH_STR_KEY_VAL(
					execute_data->extra_named_params, name, source) {
				zval copy;
				if (ZEND_TYPE_IS_SET(argument_info->type)
						&& !zend_check_type_ex(
							&argument_info->type, source, false, false)) {
					zend_verify_arg_error(execute_data->func, argument_info,
						argument_number + extra_count, source);
					return FAILURE;
				}
				ZVAL_COPY(&copy, source);
				zend_hash_add_new(Z_ARRVAL_P(variadic), name, &copy);
			} ZEND_HASH_FOREACH_END();
		}
	}
	return SUCCESS;
}

zend_result zend_native_call_prepare_dynamic_frame(
	zend_native_direct_activation *activation)
{
	zend_execute_data *execute_data;

	if (activation == NULL || zend_native_active_direct_call != activation) {
		return FAILURE;
	}
	execute_data = activation->callee;
	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->return_value == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| RUN_TIME_CACHE(&execute_data->func->op_array) == NULL
			|| !activation->raw_arguments_owned
			|| activation->frame_initialized) {
		return FAILURE;
	}
	if ((activation->resolution.placement_flags
			& ZEND_NATIVE_USER_CALL_PLACEMENTS_METADATA_PREFLIGHT) == 0) {
		if (!zend_native_call_preflight(
				activation->caller, execute_data->func)) {
			return FAILURE;
		}
		activation->resolution.placement_flags |=
			ZEND_NATIVE_USER_CALL_PLACEMENTS_METADATA_PREFLIGHT;
	}
	zend_init_func_execute_data(
		execute_data, &execute_data->func->op_array,
		execute_data->return_value);
	activation->raw_arguments_owned = false;
	activation->frame_initialized = true;
	EG(current_execute_data) = execute_data;
	if (zend_native_frame_prepare(execute_data) == FAILURE) {
		return FAILURE;
	}
	activation->frame_requires_finish = true;
	return SUCCESS;
}

zend_native_status zend_native_frame_observer_begin(
	zend_native_direct_activation *activation)
{
	zend_execute_data *execute_data = activation != NULL
		? activation->callee : NULL;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| zend_native_active_direct_call != activation
			|| !activation->frame_initialized) {
		return ZEND_NATIVE_EXCEPTION;
	}
	EG(current_execute_data) = execute_data;
	ZEND_OBSERVER_FCALL_BEGIN(execute_data);
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_frame_observer_end(
	zend_native_direct_activation *activation, zend_native_status status)
{
	zend_execute_data *execute_data = activation != NULL
		? activation->callee : NULL;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| zend_native_active_direct_call != activation
			|| !activation->frame_initialized) {
		if (activation != NULL) {
			activation->status = ZEND_NATIVE_EXCEPTION;
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	EG(current_execute_data) = execute_data;
	ZEND_OBSERVER_FCALL_END(execute_data,
		status == ZEND_NATIVE_RETURNED ? execute_data->return_value : NULL);
	status = status == ZEND_NATIVE_RETURNED && EG(exception) != NULL
		? ZEND_NATIVE_EXCEPTION : status;
	activation->status = status;
	return status;
}

zend_native_status zend_native_frame_finalize(
	zend_native_direct_activation *activation, zend_native_status status)
{
	if (activation == NULL || zend_native_active_direct_call != activation
			|| activation->callee == NULL
			|| !activation->frame_initialized
			|| !activation->frame_requires_finish) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid native frame finalization");
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_execution_finish_direct_frame(
		activation->callee, status);
	activation->frame_requires_finish = false;
	activation->frame_initialized = false;
	activation->status = status;
	if (status == ZEND_NATIVE_GENERATOR_CREATED) {
		activation->generator_created = true;
	}
	return status;
}

/*
 * Completion of a FAST_FRAME call that returned without exception: the leave
 * path of zend_native_execution_finish_direct_frame() and
 * zend_native_call_direct_release() for a frame with nothing but CVs, extra
 * arguments, a retained receiver and an active entry cell, in their order.
 */
static void zend_native_call_fast_complete(
	zend_native_direct_activation *activation, zend_execute_data *callee)
{
	zend_native_user_call_resolution *resolution = &activation->resolution;
	zend_execute_data *caller = activation->caller;
	zend_object *release_object;
	uint32_t ownership = resolution->ownership;

	ZEND_ASSERT((ownership & ~(ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT
		| ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE
		| ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE)) == 0);
	zend_vm_stack_free_extra_args(callee);
	zend_free_compiled_variables(callee);
	activation->frame_requires_finish = false;
	activation->frame_initialized = false;
	if (activation->cell_active && activation->cell != NULL) {
		zend_native_entry_cell_release_active(activation->cell);
		activation->cell_active = false;
	}
	if (activation->uses_discarded_return
			&& !Z_ISUNDEF(activation->discarded_return)) {
		zval_ptr_dtor(&activation->discarded_return);
		ZVAL_UNDEF(&activation->discarded_return);
	}
	/* The callee's RELEASE_THIS or CLOSURE flag now owns the retained
	 * receiver or closure. */
	resolution->owned_target = NULL;
	resolution->ownership = 0;
	if ((ownership & ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE) != 0) {
		zend_native_entry_cell_release_active(resolution->entry_cell);
	}
	release_object = (ZEND_CALL_INFO(callee) & ZEND_CALL_RELEASE_THIS) != 0
		? Z_OBJ(callee->This)
		: (ZEND_CALL_INFO(callee) & ZEND_CALL_CLOSURE) != 0
			? ZEND_CLOSURE_OBJECT(callee->func) : NULL;
	activation->callee = NULL;
	caller->call = activation->pending_call;
	EG(current_execute_data) = caller;
	zend_native_active_direct_call = activation->previous;
	zend_vm_stack_free_call_frame(callee);
	zend_native_frame_activation_pop(activation);
	if (release_object != NULL) {
		OBJ_RELEASE(release_object);
	}
}

/*
 * Whether a returned frame completes on the fast path; an undefined result
 * becomes null first. Its return type needs no check here: as in the VM,
 * the callee's own VERIFY_RETURN_TYPE (or VERIFY_NEVER_TYPE) checked or
 * coerced every returned value, unless the optimizer proved the type.
 */
static zend_always_inline bool zend_native_call_fast_return_valid(
	zend_execute_data *callee)
{
	zval *return_value = callee->return_value;

	if (Z_ISUNDEF_P(return_value)) {
		ZVAL_NULL(return_value);
	}
	return true;
}

uint32_t zend_native_call_invoke_user(
	zend_native_direct_activation *activation,
	zend_native_execution_context *context)
{
	zend_native_status status;

	if ((activation->resolution.placement_flags
				& ZEND_NATIVE_USER_CALL_PLACEMENTS_FAST_FRAME) != 0
			&& !ZEND_OBSERVER_ENABLED
			&& zend_native_active_direct_call == activation
			&& activation->raw_arguments_owned
			&& !activation->frame_initialized
			&& activation->callee != NULL) {
		zend_execute_data *callee = activation->callee;
		zend_op_array *op_array = &callee->func->op_array;
		uint32_t num_args = ZEND_CALL_NUM_ARGS(callee);
		void **run_time_cache = RUN_TIME_CACHE(op_array);

		bool simple = (op_array->fn_flags
				& (ZEND_ACC_HAS_TYPE_HINTS | ZEND_ACC_VARIADIC)) == 0
			&& num_args == op_array->num_args;

		if (EXPECTED(run_time_cache != NULL && simple)) {
			/* i_init_func_execute_data() for an untyped frame without
			 * extra arguments. */
			callee->prev_execute_data = EG(current_execute_data);
			callee->opline = op_array->opcodes + num_args;
			callee->call = NULL;
			for (uint32_t i = num_args; i < op_array->last_var; i++) {
				ZVAL_UNDEF(ZEND_CALL_VAR_NUM(callee, i));
			}
			callee->run_time_cache = run_time_cache;
		} else {
			zend_init_func_execute_data(
				callee, op_array, callee->return_value);
		}
		EG(current_execute_data) = callee;
		activation->raw_arguments_owned = false;
		activation->frame_initialized = true;
		if (!simple && zend_native_frame_prepare(callee) == FAILURE) {
			return 1;
		}
		activation->frame_requires_finish = true;
		status = activation->resolution.invoke_entry(callee, context);
		activation->status = status;
		if (EXPECTED(status == ZEND_NATIVE_RETURNED
				&& EG(exception) == NULL
				&& !zend_atomic_bool_load_ex(&EG(vm_interrupt))
				&& (ZEND_CALL_INFO(callee) & (ZEND_CALL_HAS_SYMBOL_TABLE
					| ZEND_CALL_HAS_EXTRA_NAMED_PARAMS)) == 0
				&& zend_native_call_fast_return_valid(callee))) {
			zend_native_call_fast_complete(activation, callee);
			return 0;
		}
		goto finalize;
	}
	if (zend_native_call_prepare_dynamic_frame(activation) != SUCCESS) {
		return 1;
	}
	status = zend_native_frame_observer_begin(activation);
	activation->status = status;
	if (status == ZEND_NATIVE_RETURNED) {
		activation->status = activation->resolution.invoke_entry(
			activation->callee, context);
	}
	zend_native_frame_observer_end(activation, activation->status);
finalize:
	status = zend_native_frame_finalize(activation, activation->status);
	if (status != ZEND_NATIVE_RETURNED
			&& status != ZEND_NATIVE_GENERATOR_CREATED) {
		return 1;
	}
	zend_native_frame_activation_release(activation);
	return 0;
}

/*
 * Leave a frame the call-site fast path pushed and entered (see
 * zend_native_user_call_site_header): the completion of
 * zend_native_call_invoke_user()'s FAST_FRAME path without an activation.
 * Returns ZEND_NATIVE_RETURNED when the caller continues, otherwise the
 * status its exception path propagates.
 */
static zend_always_inline uint32_t zend_native_call_fast_leave_inline(
	zend_execute_data *callee, uint32_t status, bool discard_result);

void zend_native_call_fast_release_cv(zval *variable)
{
	zend_refcounted *counted = Z_COUNTED_P(variable);

	/* The fast-call leave already dropped the CV's reference. */
	if (GC_REFCOUNT(counted) == 0) {
		ZVAL_NULL(variable);
		rc_dtor_func(counted);
	} else {
		gc_check_possible_root(counted);
	}
}

uint32_t zend_native_call_fast_release_this(
	zend_object *object, zval *result, bool discard_result)
{
	if (GC_REFCOUNT(object) != 0) {
		gc_possible_root((zend_refcounted *) object);
		return ZEND_NATIVE_RETURNED;
	}
	zend_objects_store_del(object);
	if (EG(exception) == NULL) {
		return ZEND_NATIVE_RETURNED;
	}
	if (!discard_result && result != NULL) {
		zval_ptr_dtor_nogc(result);
		ZVAL_UNDEF(result);
	}
	return ZEND_NATIVE_EXCEPTION;
}

uint32_t zend_native_call_fast_leave(
	zend_execute_data *callee, uint32_t status, bool discard_result)
{
	return zend_native_call_fast_leave_inline(
		callee, status, discard_result);
}

static zend_always_inline uint32_t zend_native_call_fast_leave_inline(
	zend_execute_data *callee, uint32_t status, bool discard_result)
{
	zend_execute_data *caller = callee->prev_execute_data;
	zend_object *release_object;
	zval *result;
	/* Read once: releasing the frame's values cannot change its flags,
	 * as the dying frame is no longer current. */
	uint32_t call_info = ZEND_CALL_INFO(callee);

	if (EXPECTED(status == ZEND_NATIVE_RETURNED && EG(exception) == NULL
			&& !zend_atomic_bool_load_ex(&EG(vm_interrupt))
			&& (call_info & (ZEND_CALL_HAS_SYMBOL_TABLE
				| ZEND_CALL_HAS_EXTRA_NAMED_PARAMS)) == 0
			&& zend_native_call_fast_return_valid(callee))) {
		/* An unused result is released at return, before the callee's CVs,
		 * as a NULL return_value makes the VM's RETURN do: a temporary dies
		 * without a GC root check, a CV's copy only drops its reference. */
		if (discard_result) {
			zval_ptr_dtor_nogc(callee->return_value);
			ZVAL_UNDEF(callee->return_value);
		}
		/* A variable destructor may inspect the backtrace: the dying frame
		 * is no longer current. The CVs are released as
		 * i_free_compiled_variables() releases them. */
		EG(current_execute_data) = caller;
		zend_vm_stack_free_extra_args_ex(call_info, callee);
		{
			zval *cv = ZEND_CALL_VAR_NUM(callee, 0);
			zval *end = cv + callee->func->op_array.last_var;

			for (; cv < end; cv++) {
				if (Z_REFCOUNTED_P(cv)) {
					zend_refcounted *counted = Z_COUNTED_P(cv);

					if (!GC_DELREF(counted)) {
						ZVAL_NULL(cv);
						rc_dtor_func(counted);
					} else {
						gc_check_possible_root(counted);
					}
				}
			}
		}
	} else {
		status = zend_native_execution_finish_direct_frame(callee, status);
		if (status == ZEND_NATIVE_BAILOUT) {
			return status;
		}
		EG(current_execute_data) = caller;
		if (discard_result && callee->return_value != NULL
				&& !Z_ISUNDEF_P(callee->return_value)) {
			zval_ptr_dtor(callee->return_value);
			ZVAL_UNDEF(callee->return_value);
		}
		if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
			status = ZEND_NATIVE_EXCEPTION;
		}
		/* The general finish may have changed the frame's flags. */
		call_info = ZEND_CALL_INFO(callee);
	}
	release_object = (call_info & ZEND_CALL_RELEASE_THIS) != 0
		? Z_OBJ(callee->This)
		: (call_info & ZEND_CALL_CLOSURE) != 0
			? ZEND_CLOSURE_OBJECT(callee->func) : NULL;
	result = callee->return_value;
	zend_vm_stack_free_call_frame_ex(call_info, callee);
	if (release_object != NULL) {
		OBJ_RELEASE(release_object);
		/* A throwing destructor fails the call: its result dies, as
		 * ZEND_HANDLE_EXCEPTION frees the throwing opline's result. */
		if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
			status = ZEND_NATIVE_EXCEPTION;
			if (!discard_result && result != NULL) {
				zval_ptr_dtor_nogc(result);
				ZVAL_UNDEF(result);
			}
		}
	}
	return status;
}

/*
 * zend_init_func_execute_data() and zend_native_frame_prepare() for a fast
 * frame whose target has typed parameters, defaults or extra arguments, as
 * zend_native_call_invoke_user() does for such a FAST_FRAME target. Returns
 * ZEND_NATIVE_RETURNED to enter, ZEND_NATIVE_EXCEPTION to leave.
 */
uint32_t zend_native_call_fast_prepare(zend_execute_data *callee)
{
	zend_execute_data *caller = callee->prev_execute_data;

	EG(current_execute_data) = caller;
	zend_init_func_execute_data(
		callee, &callee->func->op_array, callee->return_value);
	return zend_native_frame_prepare(callee) == SUCCESS
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

/* A fast call whose scalar result consumer received another type. */
void zend_native_call_fast_scalar_violation(void)
{
	zend_throw_error(
		NULL, "Dynamic native callee violated its scalar result contract");
}

/* SEND of an undefined CV on the fast path: warn; the caller sends null. */
void zend_native_call_fast_undefined_argument(
	zend_execute_data *caller, uint32_t variable, uint32_t source_position)
{
	if (source_position < caller->func->op_array.last) {
		caller->opline = &caller->func->op_array.opcodes[source_position];
	}
	if (variable < (uint32_t) caller->func->op_array.last_var) {
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(caller->func->op_array.vars[variable]));
	}
}

static ZEND_COLD ZEND_NORETURN void zend_native_call_abort(const char *message)
{
	zend_throw_error(NULL, "%s", message);
	zend_bailout();
}

static zend_function *zend_native_call_reject_target(const char *message)
{
	if (EG(exception) == NULL) {
		zend_throw_error(NULL, "%s", message);
	}
	return (zend_function *) &zend_pass_function;
}

static zval *zend_native_direct_operand(
	zend_execute_data *execute_data,
	const zend_mir_source_operand_ref *operand,
	bool allow_literal);

static zval *zend_native_call_read_r_operand(
	zend_execute_data *caller,
	const zend_mir_source_operand_ref *operand,
	bool allow_literal)
{
	zval *value = zend_native_direct_operand(caller, operand, allow_literal);

	if (value != NULL
			&& (operand->kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand->kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		if (operand->index >= (uint32_t) caller->func->op_array.last_var) {
			return NULL;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(caller->func->op_array.vars[operand->index]));
		if (EG(exception) != NULL) {
			return NULL;
		}
		value = &EG(uninitialized_zval);
	}
	if (value != NULL && Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	return value;
}

static zval *zend_native_call_source_slot(
	zend_execute_data *caller, const zend_mir_source_operand_ref *operand)
{
	if (operand == NULL
			|| (operand->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA)) {
		return NULL;
	}
	return zend_native_direct_operand(caller, operand, false);
}

static void zend_native_call_consume_source_slot(
	zend_execute_data *caller, const zend_mir_source_operand_ref *operand)
{
	zval *slot;

	if (operand == NULL
			|| (operand->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
				&& operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA)
			|| (operand->slot_kind != ZEND_MIR_SOURCE_SLOT_VAR
				&& operand->slot_kind != ZEND_MIR_SOURCE_SLOT_TMP)) {
		return;
	}
	slot = zend_native_call_source_slot(caller, operand);
	if (slot != NULL && !Z_ISUNDEF_P(slot)) {
		zval_ptr_dtor(slot);
		ZVAL_UNDEF(slot);
	}
}

static void zend_native_call_consume_resolved_init_operands(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor)
{
	/* On success the universal resolver first retains every invocation target
	 * that can be owned by a TMP/VAR source.  Failure paths have no published
	 * target and may consume the same explicit sources directly. */
	switch (descriptor->init_opcode) {
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
			zend_native_call_consume_source_slot(
				caller, &descriptor->init_op2);
			break;
		case ZEND_INIT_METHOD_CALL:
			zend_native_call_consume_source_slot(
				caller, &descriptor->init_op2);
			zend_native_call_consume_source_slot(
				caller, &descriptor->init_op1);
			break;
		case ZEND_INIT_STATIC_METHOD_CALL:
			zend_native_call_consume_source_slot(
				caller, &descriptor->init_op2);
			break;
		default:
			break;
	}
}

static zend_class_entry *zend_native_call_source_class(
	zend_execute_data *caller,
	const zend_mir_source_operand_ref *operand,
	uint32_t payload)
{
	zval *value;
	zend_op_array *op_array = &caller->func->op_array;

	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		if (operand->index >= op_array->last_literal
				|| operand->index + 1 >= op_array->last_literal) {
			return NULL;
		}
		value = &op_array->literals[operand->index];
		if (Z_TYPE_P(value) != IS_STRING || Z_TYPE_P(value + 1) != IS_STRING) {
			return NULL;
		}
		return zend_fetch_class_by_name(
			Z_STR_P(value), Z_STR_P(value + 1),
			ZEND_FETCH_CLASS_DEFAULT | ZEND_FETCH_CLASS_EXCEPTION);
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return zend_fetch_class(NULL, payload);
	}
	value = zend_native_call_source_slot(caller, operand);
	if (value == NULL || Z_TYPE_P(value) != IS_PTR) {
		return NULL;
	}
	return Z_CE_P(value);
}

/*
 * The run-time cache slot of the caller's INIT opline, which the VM uses
 * for the same call site (one pointer for a function name, the class and
 * method pair for a method name), or NULL without a run-time cache.
 */
static void **zend_native_call_init_cache_slot(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor, uint32_t slots)
{
	const zend_op_array *op_array = &caller->func->op_array;
	const zend_op *init;

	if (caller->run_time_cache == NULL
			|| descriptor->init_source_position >= op_array->last) {
		return NULL;
	}
	init = &op_array->opcodes[descriptor->init_source_position];
	if (init->opcode != descriptor->init_opcode
			|| init->result.num + slots * sizeof(void *)
				> (uint32_t) op_array->cache_size) {
		return NULL;
	}
	return (void **) ((char *) caller->run_time_cache + init->result.num);
}

static zend_function *zend_native_call_object_method(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_object **object_out, bool *owned_out,
	bool consume_sources, bool retain_target)
{
	zval *receiver;
	zval *name;
	zval *cache_name = NULL;
	void **cache_slot;
	zend_object *object;
	zend_object *original;
	zend_function *function;
	zend_op_array *op_array = &caller->func->op_array;

	*object_out = NULL;
	*owned_out = false;
	name = zend_native_call_read_r_operand(
		caller, &descriptor->init_op2, true);
	if (descriptor->init_op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& descriptor->init_op2.index + 1 < op_array->last_literal) {
		cache_name = &op_array->literals[descriptor->init_op2.index + 1];
	}
	if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Method name must be a string");
		}
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		}
		return NULL;
	}
	if (descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		receiver = &caller->This;
	} else {
		receiver = zend_native_call_read_r_operand(
			caller, &descriptor->init_op1, true);
	}
	if (receiver == NULL) {
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		}
		return NULL;
	}
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		zend_throw_error(NULL, "Call to a member function %s() on %s",
			Z_STRVAL_P(name), zend_zval_value_name(receiver));
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
			zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		}
		return NULL;
	}
	original = object = Z_OBJ_P(receiver);
	cache_slot = cache_name != NULL
		? zend_native_call_init_cache_slot(caller, descriptor, 2) : NULL;
	if (cache_slot != NULL && cache_slot[0] == object->ce) {
		function = cache_slot[1];
	} else {
		function = object->handlers->get_method(
			&object, Z_STR_P(name), cache_name);
		if (function != NULL && cache_slot != NULL && object == original
				&& (function->common.fn_flags
					& (ZEND_ACC_CALL_VIA_TRAMPOLINE
						| ZEND_ACC_NEVER_CACHE)) == 0) {
			cache_slot[0] = original->ce;
			cache_slot[1] = function;
		}
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& RUN_TIME_CACHE(&function->op_array) == NULL) {
			zend_init_func_run_time_cache(&function->op_array);
		}
	}
	if (function == NULL) {
		if (function == NULL && EG(exception) == NULL) {
			zend_undefined_method(original->ce, Z_STR_P(name));
		}
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
			zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		}
		return NULL;
	}
	if (consume_sources) {
		zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
	}
	if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0) {
		*object_out = (zend_object *) original->ce;
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		}
		return function;
	}
	*object_out = object;
	if (descriptor->init_op1.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		if (retain_target) {
			GC_ADDREF(object);
		}
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		}
		*owned_out = true;
	}
	return function;
}

static zend_function *zend_native_call_static_method(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_class_entry **called_scope_out, bool consume_sources)
{
	zend_class_entry *called_scope;
	zval *name;
	zval *cache_name = NULL;
	zend_function *function;
	zend_op_array *op_array = &caller->func->op_array;
	/* Like the VM: a literal class is cached in the first slot, and a
	 * literal method name keeps the class and method pair. */
	void **cache_slot = descriptor->init_op2.kind
			== ZEND_MIR_SOURCE_OPERAND_LITERAL
		? zend_native_call_init_cache_slot(caller, descriptor, 2) : NULL;

	if (cache_slot != NULL
			&& descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& cache_slot[0] != NULL) {
		called_scope = cache_slot[0];
	} else {
		called_scope = zend_native_call_source_class(
			caller, &descriptor->init_op1, descriptor->init_op1_payload);
		if (called_scope == NULL) {
			return NULL;
		}
		if (cache_slot != NULL && descriptor->init_op1.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			cache_slot[0] = called_scope;
			cache_slot[1] = NULL;
		}
	}
	if (descriptor->init_op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		function = called_scope->constructor;
		if (function == NULL) {
			zend_throw_error(NULL, "Cannot call constructor");
			return NULL;
		}
		if (Z_TYPE(caller->This) == IS_OBJECT
				&& Z_OBJ(caller->This)->ce != function->common.scope
				&& (function->common.fn_flags & ZEND_ACC_PRIVATE) != 0) {
			zend_throw_error(NULL, "Cannot call private %s::__construct()",
				ZSTR_VAL(called_scope->name));
			return NULL;
		}
	} else {
		name = zend_native_call_read_r_operand(
			caller, &descriptor->init_op2, true);
		if (descriptor->init_op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& descriptor->init_op2.index + 1 < op_array->last_literal) {
			cache_name = &op_array->literals[descriptor->init_op2.index + 1];
		}
		if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
			if (EG(exception) == NULL) {
				zend_throw_error(NULL, "Method name must be a string");
			}
			if (consume_sources) {
				zend_native_call_consume_source_slot(
					caller, &descriptor->init_op2);
			}
			return NULL;
		}
		if (cache_slot != NULL && cache_slot[0] == called_scope
				&& cache_slot[1] != NULL) {
			function = cache_slot[1];
		} else {
			function = called_scope->get_static_method != NULL
				? called_scope->get_static_method(
					called_scope, Z_STR_P(name))
				: zend_std_get_static_method(
					called_scope, Z_STR_P(name), cache_name);
			if (function != NULL && cache_slot != NULL
					&& (function->common.fn_flags
						& (ZEND_ACC_CALL_VIA_TRAMPOLINE
							| ZEND_ACC_NEVER_CACHE)) == 0) {
				if (function->type == ZEND_USER_FUNCTION
						&& RUN_TIME_CACHE(&function->op_array) == NULL) {
					zend_init_func_run_time_cache(&function->op_array);
				}
				cache_slot[0] = called_scope;
				cache_slot[1] = function;
			}
		}
		if (function == NULL && EG(exception) == NULL) {
			zend_undefined_method(called_scope, Z_STR_P(name));
		}
		if (consume_sources) {
			zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		}
	}
	if (function == NULL) {
		return NULL;
	}
	if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
		if (Z_TYPE(caller->This) != IS_OBJECT
				|| !instanceof_function(Z_OBJCE(caller->This), called_scope)) {
			zend_non_static_method_call(function);
			return NULL;
		}
		*called_scope_out = (zend_class_entry *) Z_OBJ(caller->This);
	} else {
		/* parent:: and self:: choose the lookup class from the source fetch,
		 * but forward the caller's late-static scope into the call frame. */
		if (descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& ((descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK)
						== ZEND_FETCH_CLASS_PARENT
					|| (descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK)
						== ZEND_FETCH_CLASS_SELF)) {
			called_scope = Z_TYPE(caller->This) == IS_OBJECT
				? Z_OBJCE(caller->This) : Z_CE(caller->This);
		}
		*called_scope_out = called_scope;
	}
	return function;
}

static zend_function *zend_native_call_constructor(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_object **object_out, bool *missing_out, bool retain_target)
{
	zend_class_entry *ce;
	zval *result;
	zend_function *constructor;

	*object_out = NULL;
	*missing_out = false;
	ce = zend_native_call_source_class(
		caller, &descriptor->init_op1, descriptor->init_op1_payload);
	if (ce == NULL
			|| descriptor->init_result.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return NULL;
	}
	result = zend_native_call_source_slot(caller, &descriptor->init_result);
	if (result == NULL) {
		return NULL;
	}
	if (object_init_ex(result, ce) == FAILURE) {
		ZVAL_UNDEF(result);
		return NULL;
	}
	constructor = Z_OBJ_HT_P(result)->get_constructor(Z_OBJ_P(result));
	if (constructor == NULL) {
		if (EG(exception) == NULL) {
			*missing_out = true;
		} else {
			zval_ptr_dtor(result);
			ZVAL_UNDEF(result);
		}
		return NULL;
	}
	*object_out = Z_OBJ_P(result);
	if (retain_target) {
		GC_ADDREF(*object_out);
	}
	return constructor;
}

static zend_function *zend_native_call_parent_property_hook(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_object **object_out)
{
	zend_class_entry *scope;
	zend_class_entry *parent;
	zend_property_info *property;
	zend_property_hook_kind hook_kind;
	zval *name;
	zend_function *function;

	*object_out = NULL;
	scope = caller->func->common.scope;
	if (scope == NULL || (parent = scope->parent) == NULL) {
		zend_throw_error(NULL,
			"Cannot use \"parent\" when current class scope has no parent");
		return NULL;
	}
	if (Z_TYPE(caller->This) != IS_OBJECT
			|| descriptor->init_op1.kind
				!= ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| descriptor->init_op2_payload > ZEND_PROPERTY_HOOK_SET) {
		zend_throw_error(NULL, "Malformed parent property hook call");
		return NULL;
	}
	name = zend_native_direct_operand(caller, &descriptor->init_op1, true);
	if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
		zend_throw_error(NULL, "Malformed parent property hook name");
		return NULL;
	}
	property = zend_hash_find_ptr(
		&parent->properties_info, Z_STR_P(name));
	if (property == NULL) {
		zend_throw_error(NULL, "Undefined property %s::$%s",
			ZSTR_VAL(parent->name), Z_STRVAL_P(name));
		return NULL;
	}
	if ((property->flags & ZEND_ACC_PRIVATE) != 0) {
		zend_throw_error(NULL, "Cannot access private property %s::$%s",
			ZSTR_VAL(parent->name), Z_STRVAL_P(name));
		return NULL;
	}
	hook_kind = (zend_property_hook_kind) descriptor->init_op2_payload;
	function = property->hooks != NULL
		? property->hooks[hook_kind] : NULL;
	if (function == NULL) {
		function = zend_get_property_hook_trampoline(
			property, hook_kind, Z_STR_P(name));
	}
	if (function == NULL) {
		return NULL;
	}
	*object_out = Z_OBJ(caller->This);
	return function;
}

static void zend_native_call_free_unconsumed_trampoline(
	zend_function *function)
{
	ZEND_ASSERT(function != NULL);
	ZEND_ASSERT((function->common.fn_flags
		& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0);
	if (function->common.function_name != NULL) {
		zend_string_release_ex(function->common.function_name, false);
	}
	zend_free_trampoline(function);
}

static void zend_native_call_release_target(zend_execute_data *call)
{
	uint32_t call_info = ZEND_CALL_INFO(call);

	/* A closure owns the zend_function embedded in its object.  Inspect and
	 * dispose a trampoline before releasing any target object that may own
	 * call->func.  This mirrors the VM call-frame teardown order. */
	if ((call->func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
		zend_native_call_free_unconsumed_trampoline(call->func);
	}
	if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
		OBJ_RELEASE(Z_OBJ(call->This));
	} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
		OBJ_RELEASE(ZEND_CLOSURE_OBJECT(call->func));
	}
}

static void zend_native_call_release_generator_source_target(
	zend_execute_data *call)
{
	/*
	 * ZEND_GENERATOR_CREATE copies the call frame to the generator heap frame.
	 * For a Closure, the invocation reference owned by the source frame becomes
	 * generator->func ownership and is released by the generator object
	 * destructor.  A receiver marked RELEASE_THIS is separately retained by
	 * ZEND_GENERATOR_CREATE and must still be released with the source frame.
	 */
	if ((ZEND_CALL_INFO(call) & ZEND_CALL_CLOSURE) != 0
			&& (ZEND_CALL_INFO(call) & ZEND_CALL_RELEASE_THIS) == 0) {
		return;
	}
	zend_native_call_release_target(call);
}

static zval *zend_native_call_callable_operand(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor)
{
	return zend_native_call_read_r_operand(
		caller, &descriptor->init_op2, true);
}

static bool zend_native_call_dynamic_syntax_target(
	zval *callable,
	zend_function **function_out,
	void **object_or_scope_out,
	uint32_t *call_info_out,
	bool retain_target)
{
	zend_function *function = NULL;
	void *object_or_scope = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_DYNAMIC;

	while (callable != NULL && Z_ISREF_P(callable)) {
		callable = Z_REFVAL_P(callable);
	}
	if (callable == NULL || Z_ISUNDEF_P(callable)) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Value of type undefined is not callable");
		}
		return false;
	}

	switch (Z_TYPE_P(callable)) {
		case IS_STRING: {
			zend_string *name = Z_STR_P(callable);
			zend_string *lower_name;
			const char *colon = zend_memrchr(
				ZSTR_VAL(name), ':', ZSTR_LEN(name));

			if (colon != NULL && colon > ZSTR_VAL(name)
					&& *(colon - 1) == ':') {
				size_t class_length = colon - ZSTR_VAL(name) - 1;
				size_t method_length = ZSTR_LEN(name) - class_length - 2;
				zend_string *class_name = zend_string_init(
					ZSTR_VAL(name), class_length, false);
				zend_string *method_name;
				zend_class_entry *called_scope = zend_fetch_class_by_name(
					class_name, NULL,
					ZEND_FETCH_CLASS_DEFAULT | ZEND_FETCH_CLASS_EXCEPTION);

				zend_string_release_ex(class_name, false);
				if (called_scope == NULL) {
					return false;
				}
				method_name = zend_string_init(
					ZSTR_VAL(name) + class_length + 2,
					method_length, false);
				function = called_scope->get_static_method != NULL
					? called_scope->get_static_method(called_scope, method_name)
					: zend_std_get_static_method(
						called_scope, method_name, NULL);
				if (function == NULL) {
					if (EG(exception) == NULL) {
						zend_undefined_method(called_scope, method_name);
					}
					zend_string_release_ex(method_name, false);
					return false;
				}
				zend_string_release_ex(method_name, false);
				if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
					zend_non_static_method_call(function);
					if ((function->common.fn_flags
							& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
						zend_string_release_ex(
							function->common.function_name, false);
						zend_free_trampoline(function);
					}
					return false;
				}
				object_or_scope = called_scope;
			} else {
				zval *entry;

				if (ZSTR_VAL(name)[0] == '\\') {
					lower_name = zend_string_alloc(ZSTR_LEN(name) - 1, false);
					zend_str_tolower_copy(
						ZSTR_VAL(lower_name), ZSTR_VAL(name) + 1,
						ZSTR_LEN(name) - 1);
				} else {
					lower_name = zend_string_tolower(name);
				}
				entry = zend_hash_find(EG(function_table), lower_name);
				zend_string_release_ex(lower_name, false);
				if (entry == NULL) {
					zend_throw_error(NULL, "Call to undefined function %s()",
						ZSTR_VAL(name));
					return false;
				}
				function = Z_FUNC_P(entry);
			}
			break;
		}
		case IS_OBJECT: {
			zend_object *callable_object = Z_OBJ_P(callable);
			zend_class_entry *called_scope = NULL;
			zend_object *bound_object = NULL;

			if (callable_object->handlers->get_closure == NULL
					|| callable_object->handlers->get_closure(
						callable_object, &called_scope, &function,
						&bound_object, false) != SUCCESS) {
				zend_throw_error(NULL, "Object of type %s is not callable",
					ZSTR_VAL(callable_object->ce->name));
				return false;
			}
			object_or_scope = called_scope;
			if ((function->common.fn_flags & ZEND_ACC_CLOSURE) != 0) {
				if (retain_target) {
					GC_ADDREF(ZEND_CLOSURE_OBJECT(function));
				}
				call_info |= ZEND_CALL_CLOSURE;
				if ((function->common.fn_flags & ZEND_ACC_FAKE_CLOSURE) != 0) {
					call_info |= ZEND_CALL_FAKE_CLOSURE;
				}
				if (bound_object != NULL) {
					object_or_scope = bound_object;
					call_info |= ZEND_CALL_HAS_THIS;
				}
			} else if (bound_object != NULL) {
				if (retain_target) {
					GC_ADDREF(bound_object);
				}
				object_or_scope = bound_object;
				call_info |= ZEND_CALL_RELEASE_THIS | ZEND_CALL_HAS_THIS;
			}
			break;
		}
		case IS_ARRAY: {
			zval *receiver;
			zval *method;

			if (zend_hash_num_elements(Z_ARRVAL_P(callable)) != 2) {
				zend_throw_error(NULL,
					"Array callback must have exactly two elements");
				return false;
			}
			receiver = zend_hash_index_find(Z_ARRVAL_P(callable), 0);
			method = zend_hash_index_find(Z_ARRVAL_P(callable), 1);
			if (receiver == NULL || method == NULL) {
				zend_throw_error(NULL,
					"Array callback has to contain indices 0 and 1");
				return false;
			}
			ZVAL_DEREF(receiver);
			ZVAL_DEREF(method);
			if (Z_TYPE_P(receiver) != IS_STRING
					&& Z_TYPE_P(receiver) != IS_OBJECT) {
				zend_throw_error(NULL,
					"First array member is not a valid class name or object");
				return false;
			}
			if (Z_TYPE_P(method) != IS_STRING) {
				zend_throw_error(NULL,
					"Second array member is not a valid method");
				return false;
			}
			if (Z_TYPE_P(receiver) == IS_STRING) {
				zend_class_entry *called_scope = zend_fetch_class_by_name(
					Z_STR_P(receiver), NULL,
					ZEND_FETCH_CLASS_DEFAULT | ZEND_FETCH_CLASS_EXCEPTION);

				if (called_scope == NULL) {
					return false;
				}
				function = called_scope->get_static_method != NULL
					? called_scope->get_static_method(
						called_scope, Z_STR_P(method))
					: zend_std_get_static_method(
						called_scope, Z_STR_P(method), NULL);
				if (function == NULL) {
					if (EG(exception) == NULL) {
						zend_undefined_method(called_scope, Z_STR_P(method));
					}
					return false;
				}
				if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
					zend_non_static_method_call(function);
					if ((function->common.fn_flags
							& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
						zend_string_release_ex(
							function->common.function_name, false);
						zend_free_trampoline(function);
					}
					return false;
				}
				object_or_scope = called_scope;
			} else {
				zend_object *object = Z_OBJ_P(receiver);

				function = Z_OBJ_HT_P(receiver)->get_method(
					&object, Z_STR_P(method), NULL);
				if (function == NULL) {
					if (EG(exception) == NULL) {
						zend_undefined_method(object->ce, Z_STR_P(method));
					}
					return false;
				}
				if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0) {
					object_or_scope = object->ce;
				} else {
					if (retain_target) {
						GC_ADDREF(object);
					}
					object_or_scope = object;
					call_info |= ZEND_CALL_RELEASE_THIS | ZEND_CALL_HAS_THIS;
				}
			}
			break;
		}
		default:
			zend_throw_error(NULL, "Value of type %s is not callable",
				zend_zval_type_name(callable));
			return false;
	}

	if (function == NULL || EG(exception) != NULL) {
		return false;
	}
	if (function->type == ZEND_USER_FUNCTION
			&& RUN_TIME_CACHE(&function->op_array) == NULL) {
		zend_init_func_run_time_cache(&function->op_array);
	}
	*function_out = function;
	*object_or_scope_out = object_or_scope;
	*call_info_out = call_info;
	return true;
}

static bool zend_native_call_dynamic_target(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_function **function_out, void **object_or_scope_out,
	uint32_t *call_info_out, bool retain_target)
{
	zend_fcall_info_cache fcc;
	zval *callable = zend_native_call_callable_operand(caller, descriptor);
	char *error = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_DYNAMIC;
	void *object_or_scope;

	if (descriptor->init_opcode == ZEND_INIT_DYNAMIC_CALL) {
		return zend_native_call_dynamic_syntax_target(
			callable, function_out, object_or_scope_out,
			call_info_out, retain_target);
	}
	if (callable == NULL || Z_ISUNDEF_P(callable)
			|| !zend_is_callable_ex(
				callable, NULL, 0, NULL, &fcc, &error)) {
		if (descriptor->init_opcode == ZEND_INIT_USER_CALL) {
			zval *name = zend_native_direct_operand(
				caller, &descriptor->init_op1, true);

			/* INIT_USER_CALL deliberately replaces and chains an exception
			 * raised while resolving the callback, just like the VM handler. */
			zend_type_error(
				"%s(): Argument #1 ($callback) must be a valid callback, %s",
				name != NULL && Z_TYPE_P(name) == IS_STRING
					? Z_STRVAL_P(name) : "call_user_func",
				error);
		} else if (EG(exception) == NULL) {
				zend_throw_error(NULL, "Value of type %s is not callable",
					callable != NULL && !Z_ISUNDEF_P(callable)
						? zend_zval_type_name(callable) : "undefined");
		}
		if (error != NULL) {
			efree(error);
		}
		return false;
	}
	if (error != NULL) {
		efree(error);
	}
	if (EG(exception) != NULL || fcc.function_handler == NULL) {
		return false;
	}
	*object_or_scope_out = object_or_scope = fcc.called_scope;
	if ((fcc.function_handler->common.fn_flags & ZEND_ACC_CLOSURE) != 0) {
		if (retain_target) {
			GC_ADDREF(ZEND_CLOSURE_OBJECT(fcc.function_handler));
		}
		call_info |= ZEND_CALL_CLOSURE;
		if ((fcc.function_handler->common.fn_flags
				& ZEND_ACC_FAKE_CLOSURE) != 0) {
			call_info |= ZEND_CALL_FAKE_CLOSURE;
		}
		if (fcc.object != NULL) {
			object_or_scope = fcc.object;
			call_info |= ZEND_CALL_HAS_THIS;
		}
	} else if (fcc.object != NULL) {
		if (retain_target) {
			GC_ADDREF(fcc.object);
		}
		object_or_scope = fcc.object;
		call_info |= ZEND_CALL_RELEASE_THIS | ZEND_CALL_HAS_THIS;
	}
	if (fcc.function_handler->type == ZEND_USER_FUNCTION
			&& RUN_TIME_CACHE(&fcc.function_handler->op_array) == NULL) {
		zend_init_func_run_time_cache(&fcc.function_handler->op_array);
	}
	*function_out = fcc.function_handler;
	*object_or_scope_out = object_or_scope;
	*call_info_out = call_info;
	return true;
}

static bool zend_native_call_named_target(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	bool namespace_fallback,
	zend_function **function_out, void **object_or_scope_out,
	uint32_t *call_info_out)
{
	const zend_op_array *op_array = &caller->func->op_array;
	zval *encoded_name;
	zval *function;
	void **cache_slot;
	uint32_t literal_index;

	if (descriptor->init_op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| op_array->literals == NULL) {
		return false;
	}
	literal_index = descriptor->init_op2.index;
	if (literal_index >= op_array->last_literal
			|| literal_index + (namespace_fallback ? 2 : 1)
				>= op_array->last_literal
			|| Z_TYPE(op_array->literals[literal_index + 1]) != IS_STRING
			|| (namespace_fallback
				&& Z_TYPE(op_array->literals[literal_index + 2]) != IS_STRING)) {
		return false;
	}
	encoded_name = &op_array->literals[literal_index];
	/* Like the VM, the INIT opline's run-time cache slot keeps the
	 * resolved function for the request. */
	cache_slot = zend_native_call_init_cache_slot(caller, descriptor, 1);
	if (cache_slot != NULL && cache_slot[0] != NULL) {
		*function_out = cache_slot[0];
		*object_or_scope_out = NULL;
		*call_info_out = ZEND_CALL_NESTED_FUNCTION;
		return true;
	}
	function = zend_hash_find_known_hash(
		EG(function_table), Z_STR(encoded_name[1]));
	if (function == NULL && namespace_fallback) {
		function = zend_hash_find_known_hash(
			EG(function_table), Z_STR(encoded_name[2]));
	}
	if (function == NULL || Z_TYPE_P(function) != IS_PTR) {
		zend_throw_error(NULL, "Call to undefined function %s()",
			Z_STRVAL(encoded_name[0]));
		return false;
	}
	*function_out = Z_FUNC_P(function);
	if ((*function_out)->type == ZEND_USER_FUNCTION
			&& RUN_TIME_CACHE(&(*function_out)->op_array) == NULL) {
		zend_init_func_run_time_cache(&(*function_out)->op_array);
	}
	if (cache_slot != NULL) {
		cache_slot[0] = *function_out;
	}
	*object_or_scope_out = NULL;
	*call_info_out = ZEND_CALL_NESTED_FUNCTION;
	return true;
}

static zend_native_status zend_native_call_dynamic_completed_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context);
static zend_native_status zend_native_call_dynamic_internal_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context);

static void zend_native_call_release_resolution_target(
	zend_native_user_call_resolution *resolution)
{
	uint32_t ownership = resolution->ownership;

	/* Trampolines are lookup products and must be destroyed before a target
	 * object that may own the function storage. */
	if ((ownership & ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE) != 0) {
		ZEND_ASSERT(resolution->function != NULL);
		zend_native_call_free_unconsumed_trampoline(resolution->function);
	}
	if ((ownership & (ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT
			| ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE)) != 0) {
		ZEND_ASSERT(resolution->owned_target != NULL);
		OBJ_RELEASE(resolution->owned_target);
	}
}

void zend_native_call_release_user_resolution(
	zend_native_user_call_resolution *resolution)
{
	zend_native_user_call_resolution owned;

	if (resolution == NULL) {
		return;
	}
	owned = *resolution;
	/* Clear the public ownership record before a target destructor can reenter
	 * or bail out, so outer cleanup cannot release the same resources twice. */
	memset(resolution, 0, sizeof(*resolution));
	if ((owned.ownership & ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE) != 0) {
		ZEND_ASSERT(owned.entry_cell != NULL);
		zend_native_entry_cell_release_active(owned.entry_cell);
	}
	if ((owned.ownership
			& ZEND_NATIVE_USER_CALL_OWNS_EXTRA_NAMED_PARAMS) != 0) {
		ZEND_ASSERT(owned.extra_named_params != NULL);
		zend_array_release(owned.extra_named_params);
	}
	zend_native_call_release_resolution_target(&owned);
}

static bool zend_native_call_entry_matches_function(
	const zend_native_entry_cell *entry_cell, const zend_function *function)
{
	const zend_op_array *published;
	const zend_op_array *resolved;

	if (entry_cell == NULL || entry_cell->function == NULL || function == NULL) {
		return false;
	}
	if (entry_cell->function == function) {
		return true;
	}
	if (!ZEND_USER_CODE(entry_cell->function->type)
			|| !ZEND_USER_CODE(function->type)) {
		return false;
	}
	published = &entry_cell->function->op_array;
	resolved = &function->op_array;
	/* Runtime closure instances keep invocation-specific binding state in the
	 * resolved function while sharing the source opcodes with the codeunit whose
	 * entry cell owns the published native body. */
	return published->opcodes != NULL
		&& published->opcodes == resolved->opcodes
		&& published->last == resolved->last;
}

static bool zend_native_call_resolution_user_entry(
	zend_function *function,
	zend_native_entry_cell *entry_cell_hint,
	zend_native_user_call_resolution *resolution)
{
	zend_native_entry_cell *entry_cell = NULL;
	const zend_native_code *code = NULL;
	zend_native_frame_entry_t frame_entry;

	if (RUN_TIME_CACHE(&function->op_array) == NULL) {
		zend_init_func_run_time_cache(&function->op_array);
	}
	if (entry_cell_hint != NULL
			&& entry_cell_hint->function == function
			&& entry_cell_hint->state == ZEND_NATIVE_ENTRY_READY) {
		entry_cell = entry_cell_hint;
		code = zend_native_entry_cell_load(entry_cell);
	}
	if (code == NULL) {
		entry_cell = zend_native_reentry_find(
			zend_native_active_reentry_scope, function);
		if (entry_cell != NULL
				&& zend_native_call_entry_matches_function(
					entry_cell, function)
				&& entry_cell->state == ZEND_NATIVE_ENTRY_READY) {
			code = zend_native_entry_cell_load(entry_cell);
		}
	}
	frame_entry = code != NULL ? zend_native_code_frame_entry(code) : NULL;
	if (EG(exception) != NULL || entry_cell == NULL || code == NULL
			|| !zend_native_call_entry_matches_function(entry_cell, function)
			|| frame_entry == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Resolved dynamic native user target is not ready");
		}
		return false;
	}
	resolution->entry_cell = entry_cell;
	resolution->code = code;
	resolution->frame_entry = frame_entry;
	return true;
}

static bool zend_native_call_resolution_set_sizes(
	zend_native_user_call_resolution *resolution,
	uint32_t raw_argument_count)
{
	uint64_t raw_frame_size;
	uint64_t normalized_frame_size = 0;

	raw_frame_size = zend_vm_calc_used_stack(
		raw_argument_count, resolution->function);
	if (resolution->target_kind == ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE) {
		normalized_frame_size = zend_vm_calc_used_stack(
			2, resolution->normalized_function);
	}
	if (raw_frame_size > UINT32_MAX || normalized_frame_size > UINT32_MAX) {
		zend_throw_error(NULL, "Native user call frame is too large");
		return false;
	}
	resolution->frame_size = (uint32_t) MAX(
		raw_frame_size, normalized_frame_size);
	/* Universal-call metadata lives in the separately reserved setup frame.
	 * Keep the v75 fields deterministic without asking generated code to append
	 * a second activation after the callee. */
	resolution->activation_size = 0;
	resolution->reservation_size = resolution->frame_size;
	return true;
}

static bool zend_native_call_resolution_classify(
	zend_native_entry_cell *entry_cell_hint,
	zend_native_user_call_resolution *resolution)
{
	zend_function *function = resolution->function;

	if (function == NULL || function == (zend_function *) &zend_pass_function) {
		zend_throw_error(NULL, "Resolved native call target is invalid");
		return false;
	}
	if ((function->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
		zend_function *normalized;

		/* Internal call-via-handler functions, including Closure::__invoke(),
		 * overloaded object methods and parent property-hook trampolines,
		 * perform their own forwarding.  Their handler may also consume the
		 * temporary function and clear execute_data::func before returning.
		 * Publish them as ordinary internal targets while retaining the lookup
		 * allocation until the call frame takes ownership. */
		if (function->type == ZEND_INTERNAL_FUNCTION
				&& function->internal_function.handler != NULL) {
			resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_INTERNAL;
			resolution->invoke_entry = zend_native_call_dynamic_internal_entry;
			resolution->ownership |= ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE;
			return true;
		}

		if (function->type != ZEND_USER_FUNCTION
				|| function->op_array.scope == NULL) {
			zend_throw_error(NULL, "Native call trampoline is malformed");
			return false;
		}
		normalized = (function->op_array.fn_flags & ZEND_ACC_STATIC) != 0
			? function->op_array.scope->__callstatic
			: function->op_array.scope->__call;
		if (normalized == NULL
				|| (normalized->common.fn_flags
					& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0
				|| (normalized->type != ZEND_USER_FUNCTION
					&& normalized->type != ZEND_INTERNAL_FUNCTION)) {
			zend_throw_error(NULL, "Native magic method target is missing");
			return false;
		}
		resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE;
		resolution->normalized_function = normalized;
		resolution->ownership |= ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE;
		if (normalized->type == ZEND_USER_FUNCTION
				&& !zend_native_call_resolution_user_entry(
					normalized, entry_cell_hint, resolution)) {
			return false;
		}
		return true;
	}
	if (function->type == ZEND_USER_FUNCTION) {
		if (!zend_native_call_resolution_user_entry(
				function, entry_cell_hint, resolution)) {
			return false;
		}
		resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER;
		resolution->invoke_entry = resolution->frame_entry;
		return true;
	}
	if (function->type == ZEND_INTERNAL_FUNCTION) {
		resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_INTERNAL;
		resolution->invoke_entry = zend_native_call_dynamic_internal_entry;
		return true;
	}
	zend_throw_error(NULL, "Resolved native call target type is unsupported");
	return false;
}

static void zend_native_call_resolution_retain_target(
	zend_native_user_call_resolution *resolution)
{
	if ((resolution->call_info & ZEND_CALL_RELEASE_THIS) != 0) {
		ZEND_ASSERT(resolution->object_or_called_scope != NULL);
		resolution->owned_target =
			(zend_object *) resolution->object_or_called_scope;
		GC_ADDREF(resolution->owned_target);
		resolution->ownership |= ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT;
	} else if ((resolution->call_info & ZEND_CALL_CLOSURE) != 0) {
		resolution->owned_target = ZEND_CLOSURE_OBJECT(resolution->function);
		GC_ADDREF(resolution->owned_target);
		resolution->ownership |= ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE;
	}
	if (resolution->entry_cell != NULL) {
		zend_native_entry_cell_retain_active(resolution->entry_cell);
		resolution->ownership |=
			ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE;
	}
}

static bool zend_native_call_argument_runtime_expansion(
	const zend_native_direct_internal_call_argument *argument)
{
	return argument->source_opcode == ZEND_SEND_UNPACK
		|| argument->source_opcode == ZEND_SEND_ARRAY;
}

static zend_string *zend_native_call_argument_name(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument)
{
	zend_op_array *op_array;
	zval *literal;

	if (caller == NULL || caller->func == NULL || argument == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| argument->auxiliary_operand.kind
				!= ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		return NULL;
	}
	op_array = &caller->func->op_array;
	if (argument->auxiliary_operand.index >= op_array->last_literal) {
		return NULL;
	}
	literal = &op_array->literals[argument->auxiliary_operand.index];
	return Z_TYPE_P(literal) == IS_STRING ? Z_STR_P(literal) : NULL;
}

static uint32_t zend_native_call_named_target_index(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument,
	const zend_function *function, const zend_string *name)
{
	void **cache_slot = NULL;
	void *unique_id;
	bool cacheable;
	uint32_t index;

	if (caller == NULL || caller->func == NULL || function == NULL
			|| name == NULL) {
		return ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID;
	}
	if (caller->run_time_cache != NULL
			&& argument->result_payload <= caller->func->op_array.cache_size
			&& 2 * sizeof(void *) <= caller->func->op_array.cache_size
				- argument->result_payload) {
		cache_slot = (void **) ((char *) caller->run_time_cache
			+ argument->result_payload);
	}
	unique_id = (void *) ((uintptr_t) function->common.arg_info | 1);
	cacheable = (function->type == ZEND_USER_FUNCTION
			&& (function->op_array.refcount == NULL
				|| (function->op_array.fn_flags & ZEND_ACC_CLOSURE) == 0))
		|| (function->type == ZEND_INTERNAL_FUNCTION
			&& (function->common.fn_flags & ZEND_ACC_NEVER_CACHE) == 0);
	if (cache_slot != NULL && cache_slot[0] == unique_id) {
		index = (uint32_t) (uintptr_t) cache_slot[1];
		return index <= function->common.num_args
			? index : ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID;
	}
	for (index = 0; index < function->common.num_args; index++) {
		const zend_arg_info *arg_info = function->common.arg_info != NULL
			? &function->common.arg_info[index] : NULL;

		if (arg_info != NULL && arg_info->name != NULL
				&& zend_string_equals(name, arg_info->name)) {
			if (cache_slot != NULL && cacheable) {
				cache_slot[0] = unique_id;
				cache_slot[1] = (void *) (uintptr_t) index;
			}
			return index;
		}
	}
	if ((function->common.fn_flags & ZEND_ACC_VARIADIC) != 0) {
		if (cache_slot != NULL && cacheable) {
			cache_slot[0] = unique_id;
			cache_slot[1] =
				(void *) (uintptr_t) function->common.num_args;
		}
		return function->common.num_args;
	}
	return ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID;
}

static bool zend_native_call_target_set_contains(
	const uint32_t *target_set, uint32_t target_capacity,
	uint32_t target_index)
{
	uint32_t slot = (target_index * UINT32_C(2654435761)) % target_capacity;
	uint32_t probe;

	for (probe = 0; probe < target_capacity; probe++) {
		uint32_t present = target_set[slot];
		if (present == target_index) {
			return true;
		}
		if (present == ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID) {
			return false;
		}
		slot = slot + 1 == target_capacity ? 0 : slot + 1;
	}
	return false;
}

static bool zend_native_call_target_set_insert(
	uint32_t *target_set, uint32_t target_capacity, uint32_t target_index)
{
	uint32_t slot = (target_index * UINT32_C(2654435761)) % target_capacity;
	uint32_t probe;

	for (probe = 0; probe < target_capacity; probe++) {
		if (target_set[slot]
				== ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID) {
			target_set[slot] = target_index;
			return true;
		}
		if (target_set[slot] == target_index) {
			return false;
		}
		slot = slot + 1 == target_capacity ? 0 : slot + 1;
	}
	return false;
}

static bool zend_native_call_resolution_build_placements(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t argument_count_hint,
	zend_native_user_call_placement *placements,
	uint32_t placement_capacity,
	uint32_t *target_set,
	uint32_t target_capacity,
	zend_native_user_call_resolution *resolution)
{
	zend_function *function = resolution->function;
	uint32_t planned_argument_count = argument_count_hint
		== ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO
		? descriptor->initial_argument_count : argument_count_hint;
	uint32_t direct_argument_count = 0;
	bool runtime_tail = false;
	bool positional;
	uint32_t index;

	if (placements == NULL || target_set == NULL
			|| placement_capacity < descriptor->argument_count
			|| (uint64_t) target_capacity
				< (uint64_t) descriptor->argument_count * 2 + 1) {
		zend_throw_error(NULL,
			"Native user call placement storage is too small");
		return false;
	}
	/* Positional arguments 0..n-1 in source order can neither overwrite one
	 * another nor leave a gap, so they need no target set. */
	for (index = 0; index < descriptor->argument_count; index++) {
		const zend_native_direct_internal_call_argument *argument =
			&descriptor->arguments[index];

		if (argument->mode > ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
				|| zend_native_call_argument_runtime_expansion(argument)
				|| argument->auxiliary_operand.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED
				|| (argument->auxiliary_payload != 0
					? argument->auxiliary_payload - 1
					: argument->ordinal) != index) {
			break;
		}
	}
	positional = index == descriptor->argument_count;
	if (!positional) {
		memset(target_set, 0xff, target_capacity * sizeof(*target_set));
	}
	resolution->placements = placements;
	resolution->placement_count = descriptor->argument_count;
	for (index = 0; index < descriptor->argument_count; index++) {
		const zend_native_direct_internal_call_argument *argument =
			&descriptor->arguments[index];
		zend_native_user_call_placement *placement = &placements[index];
		zend_string *name = NULL;
		uint32_t target_index =
			ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID;

		memset(placement, 0, sizeof(*placement));
		placement->source_index = index;
		placement->target_index =
			ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID;
		placement->mode = (uint32_t) argument->mode;
		if (argument->mode > ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER) {
			zend_throw_error(NULL, "Invalid native call argument mode");
			return false;
		}
		if (zend_native_call_argument_runtime_expansion(argument)) {
			runtime_tail = true;
		}
		if (!zend_native_call_argument_runtime_expansion(argument)
				&& argument->auxiliary_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			name = zend_native_call_argument_name(caller, argument);
			if (name == NULL) {
				zend_throw_error(NULL,
					"Invalid native named argument descriptor");
				return false;
			}
			placement->flags |= ZEND_NATIVE_USER_CALL_PLACEMENT_NAMED;
			target_index = zend_native_call_named_target_index(
				caller, argument, function, name);
			if (target_index == function->common.num_args
					&& (function->common.fn_flags & ZEND_ACC_VARIADIC) != 0) {
				target_index =
					ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID;
			}
			if (target_index
					== ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID) {
				if (resolution->target_kind
						== ZEND_NATIVE_USER_CALL_TARGET_NO_CALL) {
					/* The VM's dummy constructor frame reports an unknown named
					 * argument only after its source expression was evaluated. Keep
					 * it in the runtime tail so generated SEND code preserves that
					 * ordering before the explicit setter raises the error. */
					runtime_tail = true;
				} else if ((function->common.fn_flags
						& ZEND_ACC_VARIADIC) == 0) {
					caller->opline = &caller->func->op_array.opcodes[
						argument->source_position];
					zend_throw_error(NULL, "Unknown named parameter $%s",
						ZSTR_VAL(name));
					return false;
				} else {
					placement->flags |=
						ZEND_NATIVE_USER_CALL_PLACEMENT_EXTRA_NAMED;
					resolution->placement_flags |=
						ZEND_NATIVE_USER_CALL_PLACEMENTS_EXTRA_NAMED;
				}
				runtime_tail = true;
			}
		} else if (!zend_native_call_argument_runtime_expansion(argument)) {
			target_index = argument->auxiliary_payload != 0
				? argument->auxiliary_payload - 1 : argument->ordinal;
		}
		if (target_index
				!= ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID) {
			if (target_index >= ZEND_MIR_ID_MAX
					|| (!positional && !zend_native_call_target_set_insert(
						target_set, target_capacity, target_index))) {
				caller->opline = &caller->func->op_array.opcodes[
					argument->source_position];
				if (name != NULL) {
					zend_throw_error(NULL,
						"Named parameter $%s overwrites previous argument",
						ZSTR_VAL(name));
				} else {
					zend_throw_error(NULL,
						"Named parameter overwrites previous argument");
				}
				return false;
			}
			placement->target_index = target_index;
			if (ARG_SHOULD_BE_SENT_BY_REF(function, target_index + 1)) {
				placement->flags |=
					ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_SHOULD_REF;
			}
			if (ARG_MUST_BE_SENT_BY_REF(function, target_index + 1)) {
				placement->flags |=
					ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_MUST_REF;
			}
			/* A syntactically by-value SEND may still target a by-reference
			 * parameter after dynamic resolution. Keep it in the source-backed
			 * runtime tail: the explicit setter must either create a reference in
			 * the mutable source slot or raise for a literal. A direct scalar
			 * placement cannot preserve either behavior. */
			if ((placement->flags
					& (ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_SHOULD_REF
						| ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_MUST_REF)) != 0
					&& resolution->target_kind
						!= ZEND_NATIVE_USER_CALL_TARGET_NO_CALL
					&& argument->mode
						== ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
				runtime_tail = true;
				if (direct_argument_count <= target_index) {
					direct_argument_count = target_index + 1;
				}
			}
			if ((function->common.fn_flags & ZEND_ACC_VARIADIC) != 0
					&& function->common.num_args != 0
					&& target_index >= function->common.num_args - 1) {
				placement->flags |=
					ZEND_NATIVE_USER_CALL_PLACEMENT_VARIADIC;
			}
			if (planned_argument_count <= target_index) {
				planned_argument_count = target_index + 1;
			}
		}
		if (argument->mode != ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER) {
			/* Source operands may not have been evaluated at INIT. Generated
			 * SEND code performs this check at the source point and marks the
			 * current and remaining placements as one runtime tail if the actual
			 * reference state requires allocation or separation. */
			placement->flags |=
				ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_REF_CHECK;
		}
		if (!runtime_tail
				&& target_index
					!= ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID
				&& direct_argument_count <= target_index) {
			direct_argument_count = target_index + 1;
		}
		if (runtime_tail) {
			placement->flags |=
				ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION;
			resolution->placement_flags |=
				ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION;
		}
	}
	if (!runtime_tail && direct_argument_count < planned_argument_count) {
		direct_argument_count = planned_argument_count;
	}
	if (function->common.required_num_args < function->common.num_args) {
		resolution->placement_flags |=
			ZEND_NATIVE_USER_CALL_PLACEMENTS_HAS_DEFAULTS;
	}
	for (index = positional ? descriptor->argument_count : 0;
			index < planned_argument_count; index++) {
		if (positional || !zend_native_call_target_set_contains(
				target_set, target_capacity, index)) {
			resolution->placement_flags |=
				ZEND_NATIVE_USER_CALL_PLACEMENTS_MAY_HAVE_UNDEF;
			resolution->call_info |= ZEND_CALL_MAY_HAVE_UNDEF;
			break;
		}
	}
	resolution->argument_count = planned_argument_count;
	resolution->direct_argument_count = direct_argument_count;
	return true;
}

void *zend_native_frame_activation_reserve(uint32_t setup_size)
{
	zend_execute_data *setup_frame;
	uint32_t call_info = 0;

	if (setup_size < ZEND_CALL_FRAME_SLOT * sizeof(zval)) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Invalid native frame activation reservation");
		}
		return NULL;
	}
	ZEND_ASSERT_VM_STACK_GLOBAL;
	setup_frame = (zend_execute_data *) EG(vm_stack_top);
	if (UNEXPECTED(setup_size > (size_t) ((char *) EG(vm_stack_end)
			- (char *) setup_frame))) {
		setup_frame = (zend_execute_data *) zend_vm_stack_extend(setup_size);
		call_info = ZEND_CALL_ALLOCATED;
	} else {
		EG(vm_stack_top) = (zval *) ((char *) setup_frame + setup_size);
	}
	ZEND_CALL_INFO(setup_frame) = call_info;
	ZEND_ASSERT_VM_STACK_GLOBAL;
	return setup_frame;
}

/*
 * Everything a call site's resolution derives from its target alone: the
 * target kind and entry, the frame sizes and the argument placement. A hit
 * for the same function and argument count reuses it, so only the target
 * lookup and the ownership transfer run per call.
 */
typedef enum _zend_native_call_resolution_lookup {
	ZEND_NATIVE_CALL_LOOKUP_NONE = 0,
	/* The INIT opline's run-time cache slot holds the function. */
	ZEND_NATIVE_CALL_LOOKUP_FUNCTION = 1,
	/* The slot pair holds the receiver class and its method. */
	ZEND_NATIVE_CALL_LOOKUP_METHOD = 2,
	/* The slot pair holds the named, self, parent or static class and its
	 * method. */
	ZEND_NATIVE_CALL_LOOKUP_STATIC = 3,
	/*
	 * A callable of $f(...) or call_user_func*(): an interned function
	 * name, a closure whose code is the entry's, or an [$object, 'method']
	 * pair of the entry's class and interned method name (see
	 * zend_native_call_callable_identity). Only these stable identities key the
	 * entry; the function copy, bound object or receiver come from the
	 * callable called.
	 */
	ZEND_NATIVE_CALL_LOOKUP_CALLABLE = 4,
	/*
	 * The constructor of new: the entry's class, whose objects need no
	 * create_object handler and whose public constructor needs no scope
	 * check. The object is created per call into the slot at
	 * receiver_offset; a literal class name keeps its class for the request.
	 */
	ZEND_NATIVE_CALL_LOOKUP_NEW = 5
} zend_native_call_resolution_lookup;

typedef enum _zend_native_call_callable_kind {
	ZEND_NATIVE_CALL_CALLABLE_FUNCTION = 0,
	ZEND_NATIVE_CALL_CALLABLE_CLOSURE = 1,
	ZEND_NATIVE_CALL_CALLABLE_METHOD = 2
} zend_native_call_callable_kind;

typedef enum _zend_native_call_resolution_receiver {
	ZEND_NATIVE_CALL_RECEIVER_THIS = 0,
	ZEND_NATIVE_CALL_RECEIVER_CV = 1,
	/* A TMP/VAR receiver moves its reference into the call. */
	ZEND_NATIVE_CALL_RECEIVER_TEMPORARY = 2,
	/* A literal callable; receiver_offset is its literal index. */
	ZEND_NATIVE_CALL_RECEIVER_LITERAL = 3
} zend_native_call_resolution_receiver;

typedef struct _zend_native_call_resolution_cache_entry
	zend_native_call_resolution_cache_entry;

struct _zend_native_call_resolution_cache_entry {
	zend_native_call_resolution_cache_entry *next_allocated;
	/* The next entry of the same call site, most recently stored first. */
	zend_native_call_resolution_cache_entry *next_site;
	const zend_native_user_call_descriptor *descriptor;
	/* The target, or NULL for a closure callable, which matches code. */
	zend_function *function;
	const zend_op *code;
	/* A callable lookup's function or method name and method class. */
	zend_string *callable_name;
	zend_class_entry *callable_class;
	uint8_t callable_kind;
	uint32_t placement_capacity;
	uint32_t cache_slot_offset;
	uint32_t receiver_offset;
	uint8_t lookup;
	uint8_t receiver;
	const zend_op *caller_opcodes;
	uint64_t epoch;
	uint32_t argument_count;
	zend_native_user_call_resolution resolution;
	zend_native_user_call_placement placements[1];
};

/* A call site reaches its entries through the header of its descriptor;
 * the request's entries are chained for release at request end. */
#define ZEND_NATIVE_CALL_SITE_ENTRIES 2
static zend_native_call_resolution_cache_entry *
	zend_native_call_resolution_cache_entries;
static uint64_t zend_native_call_resolution_cache_epoch = 1;

static void zend_native_call_fast_publish(
	const zend_execute_data *caller,
	zend_native_user_call_site_header *header,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_call_resolution_cache_entry *entry);

const uint64_t *zend_native_call_cache_epoch_address(void)
{
	return &zend_native_call_resolution_cache_epoch;
}

void zend_native_call_resolution_cache_invalidate(void)
{
	zend_native_call_resolution_cache_epoch++;
}

void zend_native_call_resolution_cache_destroy(void)
{
	zend_native_call_resolution_cache_entry *entry =
		zend_native_call_resolution_cache_entries;

	while (entry != NULL) {
		zend_native_call_resolution_cache_entry *next = entry->next_allocated;

		efree(entry);
		entry = next;
	}
	zend_native_call_resolution_cache_entries = NULL;
	zend_native_call_resolution_cache_epoch++;
}

static zend_always_inline zend_native_call_resolution_cache_entry *
zend_native_call_resolution_cache_site(
	const zend_native_user_call_descriptor *descriptor)
{
	const zend_native_user_call_site_header *header =
		ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);

	return header->epoch == zend_native_call_resolution_cache_epoch
		? header->resolution : NULL;
}

static const zend_native_call_resolution_cache_entry *
zend_native_call_resolution_cache_find(
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_function *function, uint32_t argument_count)
{
	const zend_native_call_resolution_cache_entry *entry;

	/* A descriptor address can recur after its image was freed; the
	 * caller's opcodes tell the call sites apart. */
	for (entry = zend_native_call_resolution_cache_site(descriptor);
			entry != NULL; entry = entry->next_site) {
		if (entry->descriptor == descriptor
				&& entry->lookup != ZEND_NATIVE_CALL_LOOKUP_CALLABLE
				&& entry->function == function
				&& entry->caller_opcodes == caller->func->op_array.opcodes
				&& entry->argument_count == argument_count
				&& entry->epoch == zend_native_call_resolution_cache_epoch) {
			return entry;
		}
	}
	return NULL;
}

/* Record how a hit finds its target without the general resolver: only where
 * the VM run-time cache of the INIT opline already names this function. */
static void zend_native_call_resolution_cache_bind_lookup(
	zend_native_call_resolution_cache_entry *entry,
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_user_call_resolution *resolution)
{
	const zend_op_array *op_array = &caller->func->op_array;
	const zend_op *init = &op_array->opcodes[descriptor->init_source_position];
	void **slot;

	if (resolution->target_kind != ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER
			|| caller->run_time_cache == NULL) {
		return;
	}
	switch (descriptor->init_opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
			slot = zend_native_call_init_cache_slot(
				(zend_execute_data *) caller, descriptor, 1);
			if (slot == NULL || slot[0] != resolution->function
					|| resolution->object_or_called_scope != NULL
					|| resolution->call_info != ZEND_CALL_NESTED_FUNCTION) {
				return;
			}
			entry->lookup = ZEND_NATIVE_CALL_LOOKUP_FUNCTION;
			break;
		case ZEND_INIT_METHOD_CALL: {
			const zend_object *object = resolution->object_or_called_scope;
			const zend_mir_source_operand_ref *receiver = &descriptor->init_op1;

			slot = zend_native_call_init_cache_slot(
				(zend_execute_data *) caller, descriptor, 2);
			if (slot == NULL || object == NULL
					|| descriptor->init_op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| (resolution->function->common.fn_flags
						& ZEND_ACC_STATIC) != 0
					|| slot[0] != object->ce
					|| slot[1] != resolution->function) {
				return;
			}
			if (receiver->kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				if ((resolution->call_info & ZEND_CALL_RELEASE_THIS) != 0) {
					return;
				}
				entry->receiver = ZEND_NATIVE_CALL_RECEIVER_THIS;
			} else if ((receiver->kind == ZEND_MIR_SOURCE_OPERAND_SLOT
						|| receiver->kind == ZEND_MIR_SOURCE_OPERAND_SSA)
					&& (resolution->call_info & ZEND_CALL_RELEASE_THIS) != 0) {
				uint32_t physical;

				if (receiver->slot_kind == ZEND_MIR_SOURCE_SLOT_CV
						&& receiver->index < (uint32_t) op_array->last_var) {
					entry->receiver = ZEND_NATIVE_CALL_RECEIVER_CV;
					physical = receiver->index;
				} else if ((receiver->slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
							|| receiver->slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
						&& receiver->index < op_array->T) {
					entry->receiver = ZEND_NATIVE_CALL_RECEIVER_TEMPORARY;
					physical = (uint32_t) op_array->last_var + receiver->index;
				} else {
					return;
				}
				entry->receiver_offset = (uint32_t) (
					(ZEND_CALL_FRAME_SLOT + physical) * sizeof(zval));
			} else {
				return;
			}
			entry->lookup = ZEND_NATIVE_CALL_LOOKUP_METHOD;
			break;
		}
		case ZEND_NEW: {
			const zend_object *object = resolution->object_or_called_scope;
			const zval *result;

			if (object == NULL || object->ce->create_object != NULL
					|| object->handlers != object->ce->default_object_handlers
					|| object->handlers->get_constructor
						!= zend_std_get_constructor
					|| object->ce->constructor != resolution->function
					|| (resolution->function->common.fn_flags
						& ZEND_ACC_PUBLIC) == 0
					|| (descriptor->init_op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& descriptor->init_op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_UNUSED)) {
				return;
			}
			result = zend_native_call_source_slot(
				(zend_execute_data *) caller, &descriptor->init_result);
			if (result == NULL || (const char *) result
					< (const char *) ZEND_CALL_VAR_NUM(caller, 0)) {
				return;
			}
			entry->lookup = ZEND_NATIVE_CALL_LOOKUP_NEW;
			entry->receiver = ZEND_NATIVE_CALL_RECEIVER_TEMPORARY;
			entry->receiver_offset =
				(uint32_t) ((const char *) result - (const char *) caller);
			return;
		}
		case ZEND_INIT_STATIC_METHOD_CALL: {
			const zend_mir_source_operand_ref *class_operand =
				&descriptor->init_op1;
			const uint32_t fetch =
				descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK;

			slot = zend_native_call_init_cache_slot(
				(zend_execute_data *) caller, descriptor, 2);
			if (slot == NULL || slot[1] != resolution->function
					|| descriptor->init_op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| (class_operand->kind
							!= ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& (class_operand->kind
								!= ZEND_MIR_SOURCE_OPERAND_UNUSED
							|| (fetch != ZEND_FETCH_CLASS_SELF
								&& fetch != ZEND_FETCH_CLASS_PARENT
								&& fetch != ZEND_FETCH_CLASS_STATIC)))
					|| (resolution->call_info
						& ~(ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_HAS_THIS))
						!= 0) {
				return;
			}
			entry->lookup = ZEND_NATIVE_CALL_LOOKUP_STATIC;
			break;
		}
		default:
			return;
	}
	entry->cache_slot_offset = init->result.num;
}

/* The class a static call looks its method up in, as the VM fetches it. */
static zend_always_inline zend_class_entry *
zend_native_call_resolve_cached_static_class(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor, void **slot)
{
	zend_class_entry *scope;

	if (descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		return slot[0];
	}
	switch (descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK) {
		case ZEND_FETCH_CLASS_SELF:
			return caller->func->common.scope;
		case ZEND_FETCH_CLASS_PARENT:
			scope = caller->func->common.scope;
			return scope != NULL ? scope->parent : NULL;
		case ZEND_FETCH_CLASS_STATIC:
			return Z_TYPE(caller->This) == IS_OBJECT
				? Z_OBJCE(caller->This) : Z_CE(caller->This);
		default:
			return NULL;
	}
}

/* The callable operand of a callable lookup entry. */
static zend_always_inline zval *zend_native_call_cached_callable(
	zend_execute_data *caller,
	const zend_native_call_resolution_cache_entry *entry)
{
	return entry->receiver == ZEND_NATIVE_CALL_RECEIVER_LITERAL
		? &caller->func->op_array.literals[entry->receiver_offset]
		: (zval *) ((char *) caller + entry->receiver_offset);
}

/* The target a cached callable entry names for the callable now in its
 * slot; see zend_native_call_cached_callable_match(). */
typedef struct _zend_native_call_callable_hit {
	zval *slot;
	zend_function *function;
	zend_object *owned;
	void *object_or_called_scope;
	uint32_t call_info;
	uint32_t ownership;
	bool move_reference;
} zend_native_call_callable_hit;

/*
 * Whether the callable called is the entry's function name, closure code or
 * class and method name, and with which function copy, bound object or
 * receiver, call info and owned closure or receiver.
 */
static zend_always_inline bool zend_native_call_cached_callable_match(
	zend_execute_data *caller,
	const zend_native_call_resolution_cache_entry *entry,
	zend_native_call_callable_hit *hit)
{
	zval *callable;

	hit->slot = zend_native_call_cached_callable(caller, entry);
	hit->function = NULL;
	hit->owned = NULL;
	hit->object_or_called_scope = NULL;
	hit->call_info = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_DYNAMIC;
	hit->ownership = 0;
	hit->move_reference = false;
	callable = hit->slot;
	ZVAL_DEREF(callable);
	switch ((zend_native_call_callable_kind) entry->callable_kind) {
		case ZEND_NATIVE_CALL_CALLABLE_FUNCTION:
			if (Z_TYPE_P(callable) != IS_STRING
					|| Z_STR_P(callable) != entry->callable_name) {
				return false;
			}
			hit->function = entry->function;
			return true;
		case ZEND_NATIVE_CALL_CALLABLE_CLOSURE: {
			zend_class_entry *called_scope = NULL;
			zend_function *function = NULL;
			zend_object *bound = NULL;

			if (Z_TYPE_P(callable) != IS_OBJECT
					|| Z_OBJCE_P(callable) != zend_ce_closure
					|| Z_OBJ_HT_P(callable)->get_closure(Z_OBJ_P(callable),
						&called_scope, &function, &bound, false) != SUCCESS
					|| function == NULL
					|| function->type != ZEND_USER_FUNCTION
					|| function->op_array.opcodes != entry->code
					|| (function->common.fn_flags
						& (ZEND_ACC_CLOSURE | ZEND_ACC_FAKE_CLOSURE))
						!= ZEND_ACC_CLOSURE
					|| RUN_TIME_CACHE(&function->op_array) == NULL) {
				return false;
			}
			hit->function = function;
			hit->call_info |= ZEND_CALL_CLOSURE;
			if (bound != NULL) {
				hit->call_info |= ZEND_CALL_HAS_THIS;
			}
			hit->object_or_called_scope = bound != NULL
				? (void *) bound : (void *) called_scope;
			hit->owned = Z_OBJ_P(callable);
			hit->ownership = ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE;
			hit->move_reference = hit->slot == callable;
			return true;
		}
		case ZEND_NATIVE_CALL_CALLABLE_METHOD: {
			zval *receiver;
			zval *method;

			if (Z_TYPE_P(callable) != IS_ARRAY
					|| zend_hash_num_elements(Z_ARRVAL_P(callable)) != 2
					|| (receiver = zend_hash_index_find(
						Z_ARRVAL_P(callable), 0)) == NULL
					|| (method = zend_hash_index_find(
						Z_ARRVAL_P(callable), 1)) == NULL
					|| Z_TYPE_P(receiver) != IS_OBJECT
					|| Z_OBJCE_P(receiver) != entry->callable_class
					|| Z_TYPE_P(method) != IS_STRING
					|| Z_STR_P(method) != entry->callable_name) {
				return false;
			}
			hit->function = entry->function;
			hit->call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
			hit->object_or_called_scope = Z_OBJ_P(receiver);
			hit->owned = Z_OBJ_P(receiver);
			hit->ownership = ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT;
			return true;
		}
		default:
			return false;
	}
}

/* The call takes a reference to the closure or receiver; a temporary
 * callable is consumed as the general path does, handing over its
 * reference where it holds the target. */
static zend_always_inline void zend_native_call_cached_callable_take(
	const zend_native_call_resolution_cache_entry *entry,
	const zend_native_call_callable_hit *hit)
{
	if (entry->receiver == ZEND_NATIVE_CALL_RECEIVER_TEMPORARY) {
		if (hit->move_reference) {
			ZVAL_UNDEF(hit->slot);
		} else {
			if (hit->owned != NULL) {
				GC_ADDREF(hit->owned);
			}
			zval_ptr_dtor_nogc(hit->slot);
			ZVAL_UNDEF(hit->slot);
		}
	} else if (hit->owned != NULL) {
		GC_ADDREF(hit->owned);
	}
}

/*
 * A callable lookup hit: copy the cached resolution with the function copy,
 * bound object or receiver of the callable called, and take the call's
 * references.
 */
static zend_always_inline bool zend_native_call_resolve_cached_callable(
	zend_native_direct_activation *activation,
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_call_resolution_cache_entry *entry)
{
	zend_native_user_call_resolution *resolution = &activation->resolution;
	zend_native_user_call_placement *placements = resolution->placements;
	zend_native_call_callable_hit hit;
	uint32_t count;

	if (!zend_native_call_cached_callable_match(caller, entry, &hit)) {
		return false;
	}
	*resolution = entry->resolution;
	resolution->placements = placements;
	resolution->function = hit.function;
	resolution->object_or_called_scope = hit.object_or_called_scope;
	resolution->call_info = hit.call_info;
	count = entry->resolution.placement_count;
	for (uint32_t i = 0; i < count; i++) {
		placements[i] = entry->placements[i];
	}
	if (hit.owned != NULL) {
		resolution->owned_target = hit.owned;
		resolution->ownership |= hit.ownership;
	}
	zend_native_call_cached_callable_take(entry, &hit);
	if (resolution->entry_cell != NULL) {
		zend_native_entry_cell_retain_active(resolution->entry_cell);
		resolution->ownership |=
			ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE;
	}
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->init_source_position];
	EG(current_execute_data) = caller;
	return true;
}

/*
 * Whether C's constants and default properties are evaluated: in its flags,
 * or, for an immutable class, in the request's mutable data.
 */
static zend_always_inline bool zend_native_call_class_constants_updated(
	const zend_class_entry *ce)
{
	if ((ce->ce_flags & ZEND_ACC_CONSTANTS_UPDATED) != 0) {
		return true;
	}
	if (ZEND_MAP_PTR(ce->mutable_data) != NULL) {
		const zend_class_mutable_data *mutable_data =
			ZEND_MAP_PTR_GET_IMM(ce->mutable_data);

		return mutable_data != NULL
			&& (mutable_data->ce_flags & ZEND_ACC_CONSTANTS_UPDATED) != 0;
	}
	return false;
}

/*
 * A hit on a call site whose target the VM run-time cache names: copy the
 * cached resolution and take the ownership the general resolver would take,
 * without looking the target up or validating the descriptor again.
 */
static zend_always_inline bool zend_native_call_resolve_cached_entry(
	zend_native_direct_activation *activation,
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t argument_count_hint,
	const zend_native_call_resolution_cache_entry *entry)
{
	zend_native_user_call_resolution *resolution = &activation->resolution;
	zend_native_user_call_placement *placements = resolution->placements;
	zend_function *function;
	zend_object *object = NULL;
	void *static_scope = NULL;
	zval *receiver = NULL;
	void **slot;
	uint32_t count;

	if (entry->descriptor != descriptor
			|| entry->lookup == ZEND_NATIVE_CALL_LOOKUP_NONE
			|| entry->epoch != zend_native_call_resolution_cache_epoch
			|| entry->caller_opcodes != caller->func->op_array.opcodes
			|| entry->argument_count != (argument_count_hint
					== ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO
				? descriptor->initial_argument_count : argument_count_hint)
			|| entry->resolution.placement_count
				> activation->placement_capacity
			|| placements == NULL || caller->run_time_cache == NULL) {
		return false;
	}
	if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_CALLABLE) {
		return zend_native_call_resolve_cached_callable(
			activation, caller, descriptor, entry);
	}
	slot = (void **) ((char *) caller->run_time_cache
		+ entry->cache_slot_offset);
	if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_NEW) {
		zend_class_entry *ce = entry->callable_class;

		/* self:: and parent:: follow the caller's scope and static:: its
		 * called scope; a literal name resolved once per request. */
		if (descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& zend_fetch_class(NULL, descriptor->init_op1_payload) != ce) {
			return false;
		}
		if (!zend_native_call_class_constants_updated(ce)
				|| ce->constructor != entry->function) {
			return false;
		}
		receiver = (zval *) ((char *) caller + entry->receiver_offset);
		if (object_init_ex(receiver, ce) != SUCCESS) {
			return false;
		}
		object = Z_OBJ_P(receiver);
		GC_ADDREF(object);
		function = entry->function;
	} else if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_FUNCTION) {
		function = slot[0];
	} else if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_STATIC) {
		zend_class_entry *ce = zend_native_call_resolve_cached_static_class(
			caller, descriptor, slot);

		if (ce == NULL || slot[0] != ce || slot[1] != entry->function) {
			return false;
		}
		function = slot[1];
		if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
			if (Z_TYPE(caller->This) != IS_OBJECT
					|| !instanceof_function(Z_OBJCE(caller->This), ce)) {
				return false;
			}
			static_scope = Z_OBJ(caller->This);
		} else if (descriptor->init_op1.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK)
					!= ZEND_FETCH_CLASS_STATIC) {
			/* parent:: and self:: forward the caller's late-static scope. */
			static_scope = Z_TYPE(caller->This) == IS_OBJECT
				? (void *) Z_OBJCE(caller->This) : (void *) Z_CE(caller->This);
		} else {
			static_scope = ce;
		}
	} else {
		receiver = entry->receiver == ZEND_NATIVE_CALL_RECEIVER_THIS
			? &caller->This
			: (zval *) ((char *) caller + entry->receiver_offset);
		/* A global or static CV receiver is a reference. */
		if (entry->receiver == ZEND_NATIVE_CALL_RECEIVER_CV) {
			ZVAL_DEREF(receiver);
		}
		if (Z_TYPE_P(receiver) != IS_OBJECT) {
			return false;
		}
		object = Z_OBJ_P(receiver);
		if (object->ce != entry->callable_class) {
			return false;
		}
		function = entry->function;
	}
	if (function != entry->function) {
		return false;
	}
	*resolution = entry->resolution;
	resolution->placements = placements;
	resolution->object_or_called_scope = object != NULL
		? (void *) object : static_scope;
	count = entry->resolution.placement_count;
	for (uint32_t i = 0; i < count; i++) {
		placements[i] = entry->placements[i];
	}
	if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_NEW) {
		/* The result slot keeps the new object; the call owns a second
		 * reference, released with $this. */
		resolution->owned_target = object;
		resolution->ownership |= ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT;
	} else if (object != NULL
			&& entry->lookup == ZEND_NATIVE_CALL_LOOKUP_METHOD
			&& entry->receiver != ZEND_NATIVE_CALL_RECEIVER_THIS) {
		resolution->owned_target = object;
		resolution->ownership |= ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT;
		if (entry->receiver == ZEND_NATIVE_CALL_RECEIVER_TEMPORARY) {
			ZVAL_UNDEF(receiver);
		} else {
			GC_ADDREF(object);
		}
	}
	if (resolution->entry_cell != NULL) {
		zend_native_entry_cell_retain_active(resolution->entry_cell);
		resolution->ownership |=
			ZEND_NATIVE_USER_CALL_OWNS_ENTRY_CELL_ACTIVE;
	}
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->init_source_position];
	EG(current_execute_data) = caller;
	return true;
}

/* A hit on any entry of the call site; every miss leaves all unchanged. */
static zend_always_inline bool zend_native_call_resolve_cached(
	zend_native_direct_activation *activation,
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t argument_count_hint)
{
	const zend_native_call_resolution_cache_entry *entry;

	for (entry = zend_native_call_resolution_cache_site(descriptor);
			entry != NULL; entry = entry->next_site) {
		if (zend_native_call_resolve_cached_entry(activation, caller,
				descriptor, argument_count_hint, entry)) {
			zend_native_user_call_site_header *header =
				ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);

			/* A site first resolved in an earlier request, or whose entry
			 * survived, publishes its fast path on a hit; a published one
			 * keeps its target rather than following each receiver class. */
			if (header->fast_checked_epoch
					!= zend_native_call_resolution_cache_epoch) {
				zend_native_call_fast_publish(caller, header, descriptor, entry);
			}
			return true;
		}
	}
	return false;
}

/*
 * Where the callable of $f(...) or call_user_func*() lives: a literal, CV or
 * temporary. Returns false for any other operand or opcode.
 */
static bool zend_native_call_callable_location(
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint8_t *receiver, uint32_t *offset)
{
	const zend_op_array *op_array = &caller->func->op_array;
	const zend_mir_source_operand_ref *callable = &descriptor->init_op2;
	uint32_t physical;

	if (descriptor->init_opcode != ZEND_INIT_DYNAMIC_CALL
			&& descriptor->init_opcode != ZEND_INIT_USER_CALL) {
		return false;
	}
	if (callable->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		if (callable->index >= (uint32_t) op_array->last_literal) {
			return false;
		}
		*receiver = ZEND_NATIVE_CALL_RECEIVER_LITERAL;
		*offset = callable->index;
		return true;
	}
	if (callable->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& callable->kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return false;
	}
	if (callable->slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			&& callable->index < (uint32_t) op_array->last_var) {
		physical = callable->index;
		*receiver = ZEND_NATIVE_CALL_RECEIVER_CV;
	} else if ((callable->slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				|| callable->slot_kind == ZEND_MIR_SOURCE_SLOT_VAR)
			&& callable->index < op_array->T) {
		physical = (uint32_t) op_array->last_var + callable->index;
		*receiver = ZEND_NATIVE_CALL_RECEIVER_TEMPORARY;
	} else {
		return false;
	}
	*offset = (uint32_t) ((ZEND_CALL_FRAME_SLOT + physical) * sizeof(zval));
	return true;
}

/*
 * Whether a resolved dynamic call can be cached under a stable identity of
 * its callable, and which: an interned name of a plain function, a genuine
 * closure's code, or an [$object, 'method'] pair with an interned name
 * naming the resolved instance method of the object's class.
 */
static bool zend_native_call_callable_identity(
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_user_call_resolution *resolution,
	uint8_t *kind, uint8_t *receiver, uint32_t *offset)
{
	const zend_function *function = resolution->function;
	zval *callable;

	if (caller->run_time_cache == NULL
			|| !zend_native_call_callable_location(
				caller, descriptor, receiver, offset)) {
		return false;
	}
	callable = *receiver == ZEND_NATIVE_CALL_RECEIVER_LITERAL
		? &caller->func->op_array.literals[*offset]
		: (zval *) ((char *) caller + *offset);
	ZVAL_DEREF(callable);
	if ((function->common.fn_flags & ZEND_ACC_CLOSURE) != 0
			|| (resolution->call_info & ZEND_CALL_CLOSURE) != 0) {
		*kind = ZEND_NATIVE_CALL_CALLABLE_CLOSURE;
		return resolution->target_kind
				== ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER
			&& function->type == ZEND_USER_FUNCTION
			&& (function->common.fn_flags & ZEND_ACC_FAKE_CLOSURE) == 0
			&& (resolution->call_info & ZEND_CALL_FAKE_CLOSURE) == 0
			&& Z_TYPE_P(callable) == IS_OBJECT
			&& Z_OBJCE_P(callable) == zend_ce_closure;
	}
	if (Z_TYPE_P(callable) == IS_STRING) {
		*kind = ZEND_NATIVE_CALL_CALLABLE_FUNCTION;
		return ZSTR_IS_INTERNED(Z_STR_P(callable))
			&& function->common.scope == NULL
			&& resolution->object_or_called_scope == NULL;
	}
	if (Z_TYPE_P(callable) == IS_ARRAY
			&& zend_hash_num_elements(Z_ARRVAL_P(callable)) == 2) {
		zval *object = zend_hash_index_find(Z_ARRVAL_P(callable), 0);
		zval *method = zend_hash_index_find(Z_ARRVAL_P(callable), 1);

		*kind = ZEND_NATIVE_CALL_CALLABLE_METHOD;
		return object != NULL && method != NULL
			&& Z_TYPE_P(object) == IS_OBJECT
			&& Z_TYPE_P(method) == IS_STRING
			&& ZSTR_IS_INTERNED(Z_STR_P(method))
			&& (function->common.fn_flags & ZEND_ACC_STATIC) == 0
			&& (resolution->call_info
				& (ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS))
				== (ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS)
			&& resolution->object_or_called_scope == Z_OBJ_P(object);
	}
	return false;
}

/*
 * Publish a site's native fast path (see zend_native_user_call_site_header)
 * for the target just resolved, or withdraw it. The generated fast path
 * pushes a Zend frame for exactly the sent positional arguments and enters
 * the target like zend_native_call_invoke_user()'s FAST_FRAME path.
 */
/* The type bits a parameter accepts without a check or coercion; class
 * types, callable and other special types always take the generic check. */
static uint32_t zend_native_call_fast_receive_mask(const zend_arg_info *info)
{
	const uint32_t scalar_types = (UINT32_C(1) << (IS_RESOURCE + 1)) - 1;

	if (!ZEND_TYPE_IS_SET(info->type)) {
		return UINT32_MAX;
	}
	return (uint32_t) ZEND_TYPE_PURE_MASK(info->type) & scalar_types;
}

static const zend_native_call_fast_receive *
zend_native_call_fast_receive_prepare(
	zend_native_entry_cell *cell, const zend_op_array *op_array)
{
	zend_native_call_fast_receive *receive = &cell->fast_receive;
	uint32_t index;

	if (receive->state != ZEND_NATIVE_CALL_FAST_RECEIVE_UNKNOWN) {
		return receive;
	}
	receive->state = ZEND_NATIVE_CALL_FAST_RECEIVE_GENERIC;
	if (op_array->num_args > ZEND_NATIVE_CALL_FAST_RECEIVE_MAX
			|| (op_array->num_args != 0 && op_array->arg_info == NULL)) {
		return receive;
	}
	receive->num_args = op_array->num_args;
	for (index = 0; index < op_array->num_args; index++) {
		const zend_op *opline = &op_array->opcodes[index];
		const zval *value;

		receive->type_masks[index] =
			zend_native_call_fast_receive_mask(&op_array->arg_info[index]);
		receive->defaults[index] = NULL;
		if (opline->opcode == ZEND_RECV) {
			continue;
		}
		if (opline->opcode != ZEND_RECV_INIT
				|| opline->op1.num != index + 1
				|| opline->op2_type != IS_CONST) {
			return receive;
		}
		value = RT_CONSTANT(opline, opline->op2);
		/* A constant expression, a counted value or one the parameter
		 * type would coerce keeps the generic receive. */
		if (Z_TYPE_P(value) == IS_CONSTANT_AST || Z_REFCOUNTED_P(value)
				|| (receive->type_masks[index]
					& (UINT32_C(1) << Z_TYPE_P(value))) == 0) {
			return receive;
		}
		receive->defaults[index] = value;
	}
	receive->state = ZEND_NATIVE_CALL_FAST_RECEIVE_INLINE;
	return receive;
}

static void zend_native_call_fast_publish(
	const zend_execute_data *caller,
	zend_native_user_call_site_header *header,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_call_resolution_cache_entry *entry)
{
	const zend_native_user_call_resolution *resolution = &entry->resolution;
	const zend_function *function = resolution->function;
	const zend_op_array *op_array;
	void **run_time_cache;
	uint32_t index;
	uint32_t static_mode = 0;

	header->fast_epoch = 0;
	header->fast_checked_epoch = zend_native_call_resolution_cache_epoch;
	if ((resolution->placement_flags
				& ZEND_NATIVE_USER_CALL_PLACEMENTS_FAST_FRAME) == 0
			|| resolution->target_kind
				!= ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER
			|| function == NULL || function->type != ZEND_USER_FUNCTION
			|| resolution->entry_cell == NULL
			|| !resolution->entry_cell->lease_managed
			|| resolution->invoke_entry == NULL
			|| (resolution->placement_flags
				& ~(ZEND_NATIVE_USER_CALL_PLACEMENTS_FAST_FRAME
					| ZEND_NATIVE_USER_CALL_PLACEMENTS_HAS_DEFAULTS
					| ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION)) != 0
			|| (resolution->call_info
				& ~(ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_HAS_THIS
					| ZEND_CALL_RELEASE_THIS)) != 0
			|| entry->argument_count != descriptor->argument_count
			|| entry->argument_count != resolution->placement_count
			|| descriptor->initial_argument_count
				!= descriptor->argument_count) {
		return;
	}
	if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_FUNCTION) {
		if ((resolution->call_info & ZEND_CALL_HAS_THIS) != 0) {
			return;
		}
	} else if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_NEW) {
		/* new C(...) of a literal class: the constructor is called on the
		 * object the Init creates in the result slot. */
		if (descriptor->init_op1.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				|| entry->callable_class == NULL
				|| (resolution->call_info
					& (ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS))
					!= (ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS)) {
			return;
		}
	} else if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_STATIC) {
		const uint32_t fetch =
			descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK;
		const bool forwarding =
			descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			&& (fetch == ZEND_FETCH_CLASS_SELF
				|| fetch == ZEND_FETCH_CLASS_PARENT);
		const bool late =
			descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
			&& fetch == ZEND_FETCH_CLASS_STATIC;

		/* A named class or self::/parent:: names one class per request;
		 * static:: follows the caller's called scope, which the site
		 * compares with the class it was published for. */
		if (descriptor->init_op1.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& !forwarding && !late) {
			return;
		}
		if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
			/* self::/parent:: of an instance method keeps $this. */
			if (!forwarding || resolution->call_info
					!= (ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_HAS_THIS)) {
				return;
			}
			static_mode = ZEND_NATIVE_CALL_FAST_STATIC_THIS;
		} else if (resolution->call_info != ZEND_CALL_NESTED_FUNCTION) {
			return;
		} else if (forwarding) {
			static_mode = ZEND_NATIVE_CALL_FAST_STATIC_FORWARD;
		} else if (late) {
			static_mode = ZEND_NATIVE_CALL_FAST_STATIC_LATE;
		}
	} else if (entry->lookup == ZEND_NATIVE_CALL_LOOKUP_METHOD) {
		/* $this keeps its receiver; a CV receiver is retained by the call,
		 * a temporary one moves into it. */
		if (entry->receiver == ZEND_NATIVE_CALL_RECEIVER_THIS
				? (resolution->call_info & ZEND_CALL_RELEASE_THIS) != 0
				: (entry->receiver != ZEND_NATIVE_CALL_RECEIVER_CV
						&& entry->receiver
							!= ZEND_NATIVE_CALL_RECEIVER_TEMPORARY)
					|| (resolution->call_info & ZEND_CALL_RELEASE_THIS)
						== 0) {
			return;
		}
		/* $this->m() of a private method of the caller's class calls it
		 * for every receiver: $this is an instance of that class, and a
		 * subclass method of the same name does not override it (see
		 * zend_std_get_method()). A closure may bind another $this. */
		if (entry->receiver == ZEND_NATIVE_CALL_RECEIVER_THIS
				&& (function->common.fn_flags & ZEND_ACC_PRIVATE) != 0
				&& function->common.scope != NULL
				&& function->common.scope == caller->func->common.scope
				&& (caller->func->common.fn_flags & ZEND_ACC_CLOSURE) == 0) {
			static_mode = ZEND_NATIVE_CALL_FAST_ANY_THIS;
		}
	} else {
		return;
	}
	op_array = &function->op_array;
	run_time_cache = RUN_TIME_CACHE(op_array);
	if (run_time_cache == NULL
			|| (op_array->fn_flags & (ZEND_ACC_GENERATOR
				| ZEND_ACC_CLOSURE)) != 0
			|| resolution->frame_size == 0) {
		return;
	}
	header->fast_ref_mask = 0;
	for (index = 0; index < entry->argument_count; index++) {
		const zend_native_user_call_placement *placement =
			&entry->placements[index];
		const zend_native_direct_internal_call_argument *argument =
			&descriptor->arguments[index];

		/* A CV sent with SEND_VAR_EX to a declared by-reference parameter:
		 * the resolution leaves it to the runtime tail, and the fast Do
		 * passes the reference instead (fast_ref_mask). */
		if (index < 32 && index < op_array->num_args
				&& ZEND_ARG_SEND_MODE(&op_array->arg_info[index]) != 0
				&& argument->source_opcode == ZEND_SEND_VAR_EX
				&& argument->mode == ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
				&& argument->auxiliary_operand.kind
					== ZEND_MIR_SOURCE_OPERAND_UNUSED
				&& (argument->source_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_SLOT
					|| argument->source_operand.kind
						== ZEND_MIR_SOURCE_OPERAND_SSA)
				&& argument->source_operand.slot_kind
					== ZEND_MIR_SOURCE_SLOT_CV
				&& placement->source_index == index
				&& placement->target_index == index
				&& (placement->flags
					& ~(ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_REF_CHECK
						| ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION
						| ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_SHOULD_REF
						| ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_MUST_REF))
					== 0) {
			header->fast_ref_mask |= UINT32_C(1) << index;
			continue;
		}
		/* Any other runtime-tail placement keeps the universal protocol. */
		if ((placement->flags
				& ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION) != 0) {
			return;
		}

		/* A runtime by-reference check is decided here: the target takes
		 * this parameter by value. An argument past the declared parameters
		 * of a variadic target stays positional; the prepare collects it. */
		const bool variadic_target =
			(op_array->fn_flags & ZEND_ACC_VARIADIC) != 0;
		const bool variadic = variadic_target && index >= op_array->num_args;
		/* Placements flag the last declared parameter of a variadic
		 * target, too; it still receives its argument positionally. */
		const bool variadic_flag = variadic_target
			&& index + 1 >= op_array->num_args;
		if (placement->source_index != index
				|| (!variadic && placement->target_index != index)
				|| (placement->flags
					& ~(ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_REF_CHECK
						| (variadic_flag
							? ZEND_NATIVE_USER_CALL_PLACEMENT_VARIADIC
							: 0))) != 0
				|| argument->mode != ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
				|| argument->auxiliary_operand.kind
					!= ZEND_MIR_SOURCE_OPERAND_UNUSED
				|| (argument->source_opcode != ZEND_SEND_VAL
					&& argument->source_opcode != ZEND_SEND_VAL_EX
					&& argument->source_opcode != ZEND_SEND_VAR
					&& argument->source_opcode != ZEND_SEND_VAR_EX
					&& argument->source_opcode != ZEND_SEND_FUNC_ARG
					&& argument->source_opcode != ZEND_SEND_VAR_NO_REF_EX)
				|| (index < op_array->num_args
					? ZEND_ARG_SEND_MODE(&op_array->arg_info[index]) != 0
					: (op_array->fn_flags & ZEND_ACC_VARIADIC) != 0
						&& ZEND_ARG_SEND_MODE(
							&op_array->arg_info[op_array->num_args]) != 0)) {
			return;
		}
	}
	header->fast_key = entry->lookup == ZEND_NATIVE_CALL_LOOKUP_METHOD
			|| entry->lookup == ZEND_NATIVE_CALL_LOOKUP_NEW
		? (const void *) entry->callable_class
		: entry->lookup == ZEND_NATIVE_CALL_LOOKUP_STATIC
			? resolution->object_or_called_scope : NULL;
	header->fast_function = (zend_function *) function;
	header->fast_entry = resolution->invoke_entry;
	header->fast_run_time_cache = run_time_cache;
	header->fast_frame_size = resolution->frame_size;
	header->fast_call_info = resolution->call_info;
	header->fast_receive = zend_native_call_fast_receive_prepare(
		resolution->entry_cell, op_array);
	header->fast_cell = resolution->entry_cell;
	header->fast_default_count = 0;
	if ((op_array->fn_flags & ZEND_ACC_VARIADIC) != 0
			|| entry->argument_count > op_array->num_args
			|| header->fast_receive->state
				!= ZEND_NATIVE_CALL_FAST_RECEIVE_INLINE) {
		header->fast_flags = ZEND_NATIVE_CALL_FAST_PREPARE;
	} else {
		header->fast_flags =
			(op_array->fn_flags & ZEND_ACC_HAS_TYPE_HINTS) != 0
				? ZEND_NATIVE_CALL_FAST_CHECK_ARGS : 0;
		for (index = entry->argument_count; index < op_array->num_args;
				index++) {
			if (header->fast_receive->defaults[index] == NULL) {
				/* A missing required argument throws. */
				header->fast_flags = ZEND_NATIVE_CALL_FAST_PREPARE;
				break;
			}
		}
		if (header->fast_flags != ZEND_NATIVE_CALL_FAST_PREPARE
				&& entry->argument_count < op_array->num_args) {
			header->fast_flags |= ZEND_NATIVE_CALL_FAST_DEFAULTS;
			header->fast_default_count =
				op_array->num_args - entry->argument_count;
		}
	}
	header->fast_flags |= static_mode;
	/* A target whose parameters the inline receive takes is entered
	 * through its native call entry (ADR 0025 section 3), which checks the
	 * argument count and types itself and hands any other frame to
	 * zend_native_call_fast_do(). */
	header->fast_do_entry = (void *) zend_native_call_fast_do;
	if (header->fast_receive->state == ZEND_NATIVE_CALL_FAST_RECEIVE_INLINE
			&& (entry->argument_count <= op_array->num_args
				|| (op_array->fn_flags & ZEND_ACC_VARIADIC) != 0)
			&& (header->fast_flags & ZEND_NATIVE_CALL_FAST_NO_CALL) == 0
			&& header->fast_ref_mask == 0) {
		const zend_native_code *code =
			zend_native_entry_cell_load(resolution->entry_cell);
		void *fast_call_entry = code != NULL
			? zend_native_code_fast_call_entry(code) : NULL;

		if (fast_call_entry != NULL
				&& zend_native_code_frame_entry(code) == header->fast_entry) {
			/* Exactly the declared, untyped parameters: no frame check. */
			header->fast_do_entry = header->fast_flags == static_mode
					&& entry->argument_count == op_array->num_args
				? zend_native_code_exact_call_entry(code)
				: fast_call_entry;
		}
	}
	header->fast_epoch = zend_native_call_resolution_cache_epoch;
}

/*
 * Dynamic call targets (ADR 0025 section 3): a hook dispatcher calls many
 * callbacks through one site, more than a site's resolution entries hold.
 * The general resolver records every native target it resolved under a
 * stable callable identity: an interned function name, a closure's code, or
 * an object's class with an interned method name and the calling scope that
 * visibility was checked from. zend_native_call_fast_dynamic_init() finds
 * the target again without the general resolution and activation.
 */
#define ZEND_NATIVE_CALL_RECORDED_TARGETS 1024

typedef enum _zend_native_call_recorded_kind {
	ZEND_NATIVE_CALL_RECORDED_NONE = 0,
	ZEND_NATIVE_CALL_RECORDED_FUNCTION,
	ZEND_NATIVE_CALL_RECORDED_CLOSURE,
	ZEND_NATIVE_CALL_RECORDED_METHOD,
	ZEND_NATIVE_CALL_RECORDED_STATIC,
} zend_native_call_recorded_kind;

typedef struct _zend_native_call_recorded_target {
	uint64_t epoch;
	const void *key;
	const zend_string *method;
	const zend_class_entry *scope;
	zend_function *function;
	zend_class_entry *called_scope;
	zend_native_frame_entry_t entry;
	/* What the site's Do calls: the native call entry of the entry's code,
	 * else zend_native_call_fast_do(). */
	void *do_entry;
	const zend_native_call_fast_receive *receive;
	/* The target's entry cell, and whether the target outlives the request
	 * that recorded it: an immutable function and class, and permanent
	 * interned names (see zend_native_call_recorded_target_current()). */
	zend_native_entry_cell *cell;
	bool persistent;
	uint8_t kind;
} zend_native_call_recorded_target;

static zend_native_call_recorded_target
	zend_native_call_recorded_targets[ZEND_NATIVE_CALL_RECORDED_TARGETS];

static zend_always_inline zend_native_call_recorded_target *
zend_native_call_recorded_target_slot(
	const void *key, const zend_string *method,
	const zend_class_entry *scope)
{
	uintptr_t hash = (uintptr_t) key ^ ((uintptr_t) method >> 3)
		^ ((uintptr_t) scope >> 5);

	hash ^= hash >> 11;
	return &zend_native_call_recorded_targets[
		(hash >> 4) & (ZEND_NATIVE_CALL_RECORDED_TARGETS - 1)];
}

static void zend_native_call_recorded_target_record(
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_user_call_resolution *resolution)
{
	const zend_function *function = resolution->function;
	zend_native_call_recorded_target *target;
	const void *key;
	const zend_string *method = NULL;
	const zend_class_entry *scope = NULL;
	uint8_t kind;
	uint8_t receiver;
	uint32_t offset;
	zval *callable;

	if (resolution->target_kind != ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER
			|| function == NULL || function->type != ZEND_USER_FUNCTION
			|| resolution->entry_cell == NULL
			|| !resolution->entry_cell->lease_managed
			|| resolution->invoke_entry == NULL
			|| (function->common.fn_flags
				& (ZEND_ACC_GENERATOR | ZEND_ACC_DEPRECATED
					| ZEND_ACC_NODISCARD | ZEND_ACC_CALL_VIA_TRAMPOLINE))
				!= 0
			|| !zend_native_call_callable_location(
				caller, descriptor, &receiver, &offset)) {
		return;
	}
	callable = receiver == ZEND_NATIVE_CALL_RECEIVER_LITERAL
		? &caller->func->op_array.literals[offset]
		: (zval *) ((char *) caller + offset);
	ZVAL_DEREF(callable);
	if (Z_TYPE_P(callable) == IS_STRING) {
		/* A plain function name. */
		if (!ZSTR_IS_INTERNED(Z_STR_P(callable))
				|| function->common.scope != NULL
				|| resolution->object_or_called_scope != NULL) {
			return;
		}
		kind = ZEND_NATIVE_CALL_RECORDED_FUNCTION;
		key = Z_STR_P(callable);
	} else if (Z_TYPE_P(callable) == IS_OBJECT) {
		/* A genuine closure, matched by its code. */
		if (Z_OBJCE_P(callable) != zend_ce_closure
				|| (function->common.fn_flags
					& (ZEND_ACC_CLOSURE | ZEND_ACC_FAKE_CLOSURE))
					!= ZEND_ACC_CLOSURE
				|| (resolution->call_info & ZEND_CALL_FAKE_CLOSURE) != 0) {
			return;
		}
		kind = ZEND_NATIVE_CALL_RECORDED_CLOSURE;
		key = function->op_array.opcodes;
		function = NULL;
	} else if (Z_TYPE_P(callable) == IS_ARRAY
			&& zend_hash_num_elements(Z_ARRVAL_P(callable)) == 2) {
		zval *object = zend_hash_index_find(Z_ARRVAL_P(callable), 0);
		zval *name = zend_hash_index_find(Z_ARRVAL_P(callable), 1);

		if (object == NULL || name == NULL || Z_TYPE_P(name) != IS_STRING
				|| !ZSTR_IS_INTERNED(Z_STR_P(name))) {
			return;
		}
		method = Z_STR_P(name);
		scope = caller->func->common.scope;
		if (Z_TYPE_P(object) == IS_OBJECT) {
			/* [$object, 'method'] of an instance method. */
			if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0
					|| (resolution->call_info
						& (ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS))
						!= (ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS)
					|| resolution->object_or_called_scope != Z_OBJ_P(object)) {
				return;
			}
			kind = ZEND_NATIVE_CALL_RECORDED_METHOD;
			key = Z_OBJCE_P(object);
		} else if (Z_TYPE_P(object) == IS_STRING
				&& ZSTR_IS_INTERNED(Z_STR_P(object))) {
			/* ['Class', 'method'] of a static method called in the named
			 * class, not forwarded from a $this of it. */
			zend_class_entry *called_scope =
				(zend_class_entry *) resolution->object_or_called_scope;

			if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0
					|| (resolution->call_info & ZEND_CALL_HAS_THIS) != 0
					|| called_scope == NULL
					|| !zend_string_equals_ci(
						called_scope->name, Z_STR_P(object))
					|| (Z_TYPE(caller->This) == IS_OBJECT
						&& instanceof_function(
							Z_OBJCE(caller->This), called_scope))) {
				return;
			}
			kind = ZEND_NATIVE_CALL_RECORDED_STATIC;
			key = Z_STR_P(object);
		} else {
			return;
		}
	} else {
		return;
	}
	target = zend_native_call_recorded_target_slot(key, method, scope);
	target->epoch = zend_native_call_resolution_cache_epoch;
	target->key = key;
	target->method = method;
	target->scope = scope;
	target->function = (zend_function *) function;
	target->called_scope = kind == ZEND_NATIVE_CALL_RECORDED_STATIC
		? (zend_class_entry *) resolution->object_or_called_scope : NULL;
	target->entry = resolution->invoke_entry;
	{
		const zend_native_code *code =
			zend_native_entry_cell_load(resolution->entry_cell);
		void *native_entry = code != NULL
			? zend_native_code_fast_call_entry(code) : NULL;

		target->do_entry = native_entry != NULL
				&& zend_native_code_frame_entry(code)
					== resolution->invoke_entry
			? native_entry : (void *) zend_native_call_fast_do;
	}
	target->receive = zend_native_call_fast_receive_prepare(
		resolution->entry_cell, &resolution->function->op_array);
	target->cell = resolution->entry_cell;
	target->persistent = function != NULL
		&& (function->op_array.fn_flags & ZEND_ACC_IMMUTABLE) != 0
		&& (kind == ZEND_NATIVE_CALL_RECORDED_METHOD
			? (((const zend_class_entry *) key)->ce_flags
				& ZEND_ACC_IMMUTABLE) != 0
			: kind == ZEND_NATIVE_CALL_RECORDED_FUNCTION
				|| kind == ZEND_NATIVE_CALL_RECORDED_STATIC)
		&& (kind == ZEND_NATIVE_CALL_RECORDED_METHOD
			|| (GC_FLAGS((const zend_string *) key) & IS_STR_PERMANENT) != 0)
		&& (method == NULL || (GC_FLAGS(method) & IS_STR_PERMANENT) != 0)
		&& (scope == NULL || (scope->ce_flags & ZEND_ACC_IMMUTABLE) != 0)
		&& (kind != ZEND_NATIVE_CALL_RECORDED_STATIC
			|| (target->called_scope->ce_flags & ZEND_ACC_IMMUTABLE) != 0);
	target->kind = kind;
}

/*
 * Whether a recorded target found for a callable is current: recorded in
 * this request, or recorded in an earlier one for a target that outlives
 * it and still applies, as the VM resolves the callable again in every
 * request. Its entry cell must still hold the recorded entry, a function
 * name must still bind the function and a class name the class; it is then
 * current again.
 */
static zend_always_inline bool zend_native_call_recorded_target_current(
	zend_native_call_recorded_target *target)
{
	const zend_native_entry_cell *cell;
	const zend_native_code *code;

	if (EXPECTED(target->epoch == zend_native_call_resolution_cache_epoch)) {
		return true;
	}
	if (!target->persistent || (cell = target->cell) == NULL
			|| cell->state != ZEND_NATIVE_ENTRY_READY
			|| cell->function != target->function
			|| (code = zend_native_entry_cell_load(
				(zend_native_entry_cell *) cell)) == NULL
			|| zend_native_code_frame_entry(code) != target->entry) {
		return false;
	}
	if (target->kind == ZEND_NATIVE_CALL_RECORDED_FUNCTION
			? zend_hash_find_ptr_lc(EG(function_table),
				(zend_string *) target->key) != target->function
			: target->kind == ZEND_NATIVE_CALL_RECORDED_STATIC
				&& zend_hash_find_ptr_lc(EG(class_table),
					(zend_string *) target->key) != target->called_scope) {
		return false;
	}
	target->epoch = zend_native_call_resolution_cache_epoch;
	return true;
}

/*
 * The fast path of a call_user_func*() or $f(...) site: find the callable's
 * recorded native target, push its frame as ZEND_INIT_USER_CALL does and
 * link it as EX(call), taking the references the VM takes. The arguments
 * are then sent to EX(call) and the frame prepared generically. Returns the
 * target's entry, or NULL for the universal protocol with nothing changed.
 */
zend_native_call_dynamic_init_result zend_native_call_fast_dynamic_init(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor)
{
	const zend_native_call_dynamic_init_result miss = {NULL, NULL};
	zend_native_call_recorded_target *target;
	zend_function *function = NULL;
	zend_object *owned = NULL;
	void *object_or_called_scope = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_DYNAMIC;
	uint8_t receiver;
	uint32_t offset;
	zval *slot;
	zval *callable;
	zend_execute_data *call;

	if (ZEND_OBSERVER_ENABLED || EG(exception) != NULL
			|| !zend_native_call_callable_location(
				caller, descriptor, &receiver, &offset)) {
		return miss;
	}
	slot = receiver == ZEND_NATIVE_CALL_RECEIVER_LITERAL
		? &caller->func->op_array.literals[offset]
		: (zval *) ((char *) caller + offset);
	callable = slot;
	ZVAL_DEREF(callable);
	if (Z_TYPE_P(callable) == IS_STRING) {
		target = zend_native_call_recorded_target_slot(
			Z_STR_P(callable), NULL, NULL);
		if (target->key != Z_STR_P(callable)
				|| target->kind != ZEND_NATIVE_CALL_RECORDED_FUNCTION
				|| !zend_native_call_recorded_target_current(target)) {
			return miss;
		}
		function = target->function;
	} else if (Z_TYPE_P(callable) == IS_OBJECT) {
		zend_class_entry *called_scope = NULL;
		zend_object *bound = NULL;

		if (Z_OBJCE_P(callable) != zend_ce_closure
				|| Z_OBJ_HT_P(callable)->get_closure(Z_OBJ_P(callable),
					&called_scope, &function, &bound, false) != SUCCESS
				|| function == NULL || function->type != ZEND_USER_FUNCTION
				|| (function->common.fn_flags
					& (ZEND_ACC_CLOSURE | ZEND_ACC_FAKE_CLOSURE))
					!= ZEND_ACC_CLOSURE
				|| RUN_TIME_CACHE(&function->op_array) == NULL) {
			return miss;
		}
		target = zend_native_call_recorded_target_slot(
			function->op_array.opcodes, NULL, NULL);
		if (target->epoch != zend_native_call_resolution_cache_epoch
				|| target->key != function->op_array.opcodes
				|| target->kind != ZEND_NATIVE_CALL_RECORDED_CLOSURE) {
			return miss;
		}
		call_info |= ZEND_CALL_CLOSURE;
		if (bound != NULL) {
			call_info |= ZEND_CALL_HAS_THIS;
		}
		object_or_called_scope = bound != NULL
			? (void *) bound : (void *) called_scope;
		owned = Z_OBJ_P(callable);
	} else if (Z_TYPE_P(callable) == IS_ARRAY
			&& zend_hash_num_elements(Z_ARRVAL_P(callable)) == 2) {
		zval *object = zend_hash_index_find(Z_ARRVAL_P(callable), 0);
		zval *method = zend_hash_index_find(Z_ARRVAL_P(callable), 1);
		const void *key;

		if (object == NULL || method == NULL
				|| Z_TYPE_P(method) != IS_STRING) {
			return miss;
		}
		if (Z_TYPE_P(object) == IS_OBJECT) {
			key = Z_OBJCE_P(object);
		} else if (Z_TYPE_P(object) == IS_STRING) {
			key = Z_STR_P(object);
		} else {
			return miss;
		}
		target = zend_native_call_recorded_target_slot(
			key, Z_STR_P(method), caller->func->common.scope);
		if (target->key != key
				|| target->method != Z_STR_P(method)
				|| target->scope != caller->func->common.scope
				|| target->kind != (Z_TYPE_P(object) == IS_OBJECT
					? ZEND_NATIVE_CALL_RECORDED_METHOD
					: ZEND_NATIVE_CALL_RECORDED_STATIC)
				|| !zend_native_call_recorded_target_current(target)) {
			return miss;
		}
		function = target->function;
		if (Z_TYPE_P(object) == IS_OBJECT) {
			call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
			object_or_called_scope = Z_OBJ_P(object);
			owned = Z_OBJ_P(object);
		} else {
			/* The static method's late static binding: a $this of the
			 * class would forward its own class. */
			if (Z_TYPE(caller->This) == IS_OBJECT
					&& instanceof_function(
						Z_OBJCE(caller->This), target->called_scope)) {
				return miss;
			}
			object_or_called_scope = target->called_scope;
		}
	} else {
		return miss;
	}
	void **run_time_cache = RUN_TIME_CACHE(&function->op_array);

	if (run_time_cache == NULL) {
		zend_init_func_run_time_cache(&function->op_array);
		run_time_cache = RUN_TIME_CACHE(&function->op_array);
	}
	call = zend_vm_stack_push_call_frame(call_info, function,
		descriptor->initial_argument_count, object_or_called_scope);
	/* The call references the closure or receiver; a temporary callable is
	 * consumed, handing over its reference where it holds the target. */
	if (receiver == ZEND_NATIVE_CALL_RECEIVER_TEMPORARY) {
		if (owned != NULL && slot == callable
				&& Z_TYPE_P(slot) == IS_OBJECT && Z_OBJ_P(slot) == owned) {
			ZVAL_UNDEF(slot);
		} else {
			if (owned != NULL) {
				GC_ADDREF(owned);
			}
			zval_ptr_dtor_nogc(slot);
			ZVAL_UNDEF(slot);
		}
	} else if (owned != NULL) {
		GC_ADDREF(owned);
	}
	/* Until the Do, the frame's run-time cache slot carries how the
	 * target receives its parameters (zend_native_call_fast_do()) and
	 * its return value slot the run-time cache (the native call entry). */
	call->run_time_cache = (void **) target->receive;
	call->return_value = (zval *) run_time_cache;
	call->prev_execute_data = caller->call;
	caller->call = call;
	return (zend_native_call_dynamic_init_result) {
		target->entry, target->do_entry};
}

/* A literal, CV, temporary or VAR operand of a fast send; *temporary
 * tells whether the send consumes the slot. NULL for any other operand. */
static zval *zend_native_call_fast_send_operand(
	zend_execute_data *caller, const zend_mir_source_operand_ref *operand,
	bool *temporary)
{
	const zend_op_array *op_array = &caller->func->op_array;

	*temporary = false;
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		return &op_array->literals[operand->index];
	}
	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return NULL;
	}
	if (operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
		return ZEND_CALL_VAR_NUM(caller, operand->index);
	}
	if (operand->slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
			|| operand->slot_kind == ZEND_MIR_SOURCE_SLOT_VAR) {
		*temporary = true;
		return ZEND_CALL_VAR_NUM(caller,
			(uint32_t) op_array->last_var + operand->index);
	}
	return NULL;
}

/*
 * ZEND_SEND_ARRAY of a packed array without holes to a target taking every
 * sent element by value: each element, dereferenced, is copied into the
 * next argument as the VM copies it. The array_slice() form (an integer or
 * null length, the skip in the extended value) sends the slice as the VM's
 * handler computes it. Returns false with nothing changed for any other
 * array, operand or target.
 */
static bool zend_native_call_fast_send_packed(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument)
{
	zend_execute_data *call = caller->call;
	zend_function *function = call->func;
	const bool slice = argument->auxiliary_operand.kind
		!= ZEND_MIR_SOURCE_OPERAND_UNUSED;
	zval *source;
	zval *length_source = NULL;
	zval *length_value = NULL;
	zval *args;
	zval *target;
	HashTable *table;
	uint32_t count;
	uint32_t begin = 0;
	uint32_t sent;
	uint32_t first;
	bool temporary;
	bool length_temporary = false;

	source = zend_native_call_fast_send_operand(
		caller, &argument->source_operand, &temporary);
	if (source == NULL) {
		return false;
	}
	if (slice) {
		length_source = zend_native_call_fast_send_operand(
			caller, &argument->auxiliary_operand, &length_temporary);
		if (length_source == NULL) {
			return false;
		}
		length_value = length_source;
		ZVAL_DEREF(length_value);
		if (Z_TYPE_P(length_value) != IS_LONG
				&& Z_TYPE_P(length_value) != IS_NULL) {
			return false;
		}
	}
	args = source;
	ZVAL_DEREF(args);
	if (Z_TYPE_P(args) != IS_ARRAY
			|| (function->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
		return false;
	}
	table = Z_ARRVAL_P(args);
	count = zend_hash_num_elements(table);
	if (!HT_IS_PACKED(table) || !HT_IS_WITHOUT_HOLES(table)) {
		return false;
	}
	sent = count;
	if (slice) {
		const uint32_t skip = argument->extended_value;
		const zend_long remaining =
			skip < count ? (zend_long) (count - skip) : 0;
		zend_long length = Z_TYPE_P(length_value) == IS_LONG
			? Z_LVAL_P(length_value) : remaining;

		if (length < 0) {
			length += remaining;
		}
		begin = skip;
		sent = skip < count && length > 0
			? (uint32_t) MIN(length, remaining) : 0;
	}
	first = ZEND_CALL_NUM_ARGS(call);
	for (uint32_t index = 0; index < sent; index++) {
		if (ARG_SHOULD_BE_SENT_BY_REF(function, first + index + 1)) {
			return false;
		}
	}
	if (sent != 0) {
		zend_vm_stack_extend_call_frame(&call, first, sent);
		caller->call = call;
		target = ZEND_CALL_ARG(call, first + 1);
		for (uint32_t index = 0; index < sent; index++) {
			zval *element = &table->arPacked[begin + index];

			ZVAL_COPY_DEREF(target, element);
			target++;
		}
		ZEND_CALL_NUM_ARGS(call) = first + sent;
	}
	if (temporary) {
		zval_ptr_dtor_nogc(source);
		ZVAL_UNDEF(source);
	}
	if (length_temporary) {
		zval_ptr_dtor_nogc(length_source);
		ZVAL_UNDEF(length_source);
	}
	return true;
}

/* A send of a dynamic fast call site, to EX(call) as the VM sends; unpacked
 * named arguments fill their defaults as ZEND_CHECK_UNDEF_ARGS does. */
uint32_t zend_native_call_fast_send(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t index)
{
	const zend_native_direct_internal_call_argument *argument =
		&descriptor->arguments[index];

	if (argument->source_position < caller->func->op_array.last) {
		caller->opline =
			&caller->func->op_array.opcodes[argument->source_position];
	}
	if (argument->source_opcode == ZEND_SEND_ARRAY
			&& zend_native_call_fast_send_packed(caller, argument)) {
		return ZEND_NATIVE_RETURNED;
	}
	if (zend_native_call_set_explicit_argument(caller, argument) != SUCCESS) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid native dynamic call argument");
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	if (argument->source_opcode == ZEND_SEND_ARRAY
			&& (ZEND_CALL_INFO(caller->call) & ZEND_CALL_MAY_HAVE_UNDEF) != 0
			&& zend_handle_undef_args(caller->call) == FAILURE) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

/*
 * new C of a literal class without a constructor and without arguments:
 * the site's fast Init creates the object, as ZEND_NEW does, and its Do
 * has no frame to call (ZEND_NATIVE_CALL_FAST_NO_CALL).
 */
static void zend_native_call_fast_publish_new_without_constructor(
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_user_call_resolution *resolution)
{
	zend_native_user_call_site_header *header =
		ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);
	const zend_object *object;
	const zval *result;

	(void) resolution;
	if (header->fast_checked_epoch == zend_native_call_resolution_cache_epoch) {
		return;
	}
	header->fast_epoch = 0;
	header->fast_checked_epoch = zend_native_call_resolution_cache_epoch;
	/* The Init created the object in its result slot. */
	result = zend_native_call_source_slot(
		(zend_execute_data *) caller, &descriptor->init_result);
	if (result == NULL || (const char *) result
			< (const char *) ZEND_CALL_VAR_NUM(caller, 0)
			|| Z_TYPE_P(result) != IS_OBJECT) {
		return;
	}
	object = Z_OBJ_P(result);
	if (descriptor->argument_count != 0
			|| descriptor->initial_argument_count != 0
			|| descriptor->init_op1.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| object->ce->constructor != NULL
			|| object->ce->create_object != NULL
			|| object->handlers != object->ce->default_object_handlers
			|| object->handlers->get_constructor != zend_std_get_constructor) {
		return;
	}
	header->fast_key = object->ce;
	header->fast_function = NULL;
	header->fast_flags = ZEND_NATIVE_CALL_FAST_NO_CALL;
	header->fast_do_entry = (void *) zend_native_call_fast_do;
	header->fast_epoch = zend_native_call_resolution_cache_epoch;
}

static void zend_native_call_resolution_cache_store(
	const zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	const zend_native_user_call_resolution *resolution)
{
	zend_native_user_call_site_header *header;
	zend_native_call_resolution_cache_entry *entry;
	const uint32_t count = resolution->placement_count;
	const bool dynamic =
		descriptor->init_opcode == ZEND_INIT_DYNAMIC_CALL
		|| descriptor->init_opcode == ZEND_INIT_USER_CALL;
	uint8_t callable_kind = 0;
	uint8_t callable_receiver = 0;
	uint32_t callable_offset = 0;
	zend_string *callable_name = NULL;
	zend_class_entry *callable_class = NULL;
	const zend_op *code;

	/* A dynamic call is cached only under a stable identity of its callable:
	 * a closure's function lives in its object, and the address of a freed
	 * closure, receiver or name string identifies nothing. */
	if (dynamic) {
		zend_native_call_recorded_target_record(
			caller, descriptor, resolution);
	}
	if (dynamic && !zend_native_call_callable_identity(caller, descriptor,
				resolution, &callable_kind, &callable_receiver,
				&callable_offset)) {
		return;
	}
	if ((resolution->target_kind != ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER
				&& resolution->target_kind
					!= ZEND_NATIVE_USER_CALL_TARGET_INTERNAL)
			|| resolution->ownership != 0
			|| resolution->extra_named_params != NULL
			|| (resolution->placement_flags
				& ZEND_NATIVE_USER_CALL_PLACEMENTS_EXTRA_NAMED) != 0
			|| count > ZEND_MIR_ID_MAX) {
		return;
	}
	header = ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);
	if (dynamic) {
		zval *callable = callable_receiver == ZEND_NATIVE_CALL_RECEIVER_LITERAL
			? &caller->func->op_array.literals[callable_offset]
			: (zval *) ((char *) caller + callable_offset);

		ZVAL_DEREF(callable);
		if (callable_kind == ZEND_NATIVE_CALL_CALLABLE_FUNCTION) {
			callable_name = Z_STR_P(callable);
		} else if (callable_kind == ZEND_NATIVE_CALL_CALLABLE_METHOD) {
			callable_class = Z_OBJCE_P(
				zend_hash_index_find(Z_ARRVAL_P(callable), 0));
			callable_name = Z_STR_P(
				zend_hash_index_find(Z_ARRVAL_P(callable), 1));
		}
	} else if ((descriptor->init_opcode == ZEND_INIT_METHOD_CALL
				|| descriptor->init_opcode == ZEND_NEW)
			&& resolution->object_or_called_scope != NULL) {
		/* A method or new entry is keyed by the object's class: the runtime
		 * cache slot holds only the last class of a polymorphic site. */
		callable_class =
			((const zend_object *) resolution->object_or_called_scope)->ce;
	}
	code = resolution->function->type == ZEND_USER_FUNCTION
		? resolution->function->op_array.opcodes : NULL;
	/*
	 * A site keeps up to ZEND_NATIVE_CALL_SITE_ENTRIES entries, most recent
	 * first: a polymorphic site, such as a hook dispatcher, finds each of its
	 * targets. The entry of the same target is refreshed; beyond the limit
	 * the least recent one is reused. A replaced or too small entry stays
	 * allocated until request end.
	 */
	{
		zend_native_call_resolution_cache_entry *head =
			zend_native_call_resolution_cache_site(descriptor);
		zend_native_call_resolution_cache_entry **link = &head;
		zend_native_call_resolution_cache_entry **last = NULL;
		uint32_t depth = 0;

		for (entry = head; entry != NULL; entry = *link) {
			const bool same = entry->argument_count
					== resolution->argument_count
				&& (dynamic
					? entry->lookup == ZEND_NATIVE_CALL_LOOKUP_CALLABLE
						&& entry->callable_kind == callable_kind
						&& entry->callable_name == callable_name
						&& entry->callable_class == callable_class
						&& (callable_kind
								!= ZEND_NATIVE_CALL_CALLABLE_CLOSURE
							|| entry->code == code)
					: entry->lookup != ZEND_NATIVE_CALL_LOOKUP_CALLABLE
						&& entry->function == resolution->function
						&& entry->callable_class == callable_class);
			if (same) {
				break;
			}
			last = link;
			link = &entry->next_site;
			depth++;
		}
		if (entry == NULL && depth >= ZEND_NATIVE_CALL_SITE_ENTRIES
				&& last != NULL) {
			link = last;
			entry = *last;
		}
		if (entry != NULL) {
			*link = entry->next_site;
			if (entry->placement_capacity < count) {
				entry = NULL;
			}
		}
		if (entry == NULL) {
			entry = emalloc(offsetof(zend_native_call_resolution_cache_entry,
				placements)
				+ (count == 0 ? 1 : count) * sizeof(entry->placements[0]));
			entry->placement_capacity = count == 0 ? 1 : count;
			entry->next_allocated = zend_native_call_resolution_cache_entries;
			zend_native_call_resolution_cache_entries = entry;
		}
		entry->next_site = head;
		header->resolution = entry;
		header->epoch = zend_native_call_resolution_cache_epoch;
	}
	entry->descriptor = descriptor;
	entry->function = dynamic
			&& callable_kind == ZEND_NATIVE_CALL_CALLABLE_CLOSURE
		? NULL : resolution->function;
	entry->code = code;
	entry->callable_name = callable_name;
	entry->callable_class = callable_class;
	entry->callable_kind = callable_kind;
	entry->lookup = ZEND_NATIVE_CALL_LOOKUP_NONE;
	entry->receiver = ZEND_NATIVE_CALL_RECEIVER_THIS;
	entry->receiver_offset = 0;
	entry->cache_slot_offset = 0;
	if (dynamic) {
		entry->lookup = ZEND_NATIVE_CALL_LOOKUP_CALLABLE;
		entry->receiver = callable_receiver;
		entry->receiver_offset = callable_offset;
	} else {
		zend_native_call_resolution_cache_bind_lookup(
			entry, caller, descriptor, resolution);
	}
	entry->caller_opcodes = caller->func->op_array.opcodes;
	entry->epoch = zend_native_call_resolution_cache_epoch;
	entry->argument_count = resolution->argument_count;
	entry->resolution = *resolution;
	entry->resolution.placements = NULL;
	memcpy(entry->placements, resolution->placements,
		count * sizeof(entry->placements[0]));
	if (resolution->target_kind == ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER
			&& resolution->function->type == ZEND_USER_FUNCTION
			&& (resolution->function->common.fn_flags
				& (ZEND_ACC_GENERATOR | ZEND_ACC_DEPRECATED
					| ZEND_ACC_NODISCARD
					| ZEND_ACC_CALL_VIA_TRAMPOLINE)) == 0
			&& (resolution->placement_flags
				& ZEND_NATIVE_USER_CALL_PLACEMENTS_MAY_HAVE_UNDEF) == 0
			&& (resolution->call_info & ~(ZEND_CALL_NESTED_FUNCTION
				| ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS
				| ZEND_CALL_DYNAMIC | ZEND_CALL_CLOSURE)) == 0) {
		entry->resolution.placement_flags |=
			ZEND_NATIVE_USER_CALL_PLACEMENTS_FAST_FRAME;
	}
	zend_native_call_fast_publish(caller, header, descriptor, entry);
}

zend_native_user_call_resolution_status zend_native_call_resolve_user(
	zend_native_direct_activation *activation,
	zend_native_entry_cell *entry_cell_hint,
	uint32_t argument_count_hint)
{
	const zend_native_call_resolution_cache_entry *cached;
	zend_execute_data *caller;
	const zend_native_user_call_descriptor *descriptor;
	zend_native_user_call_placement *placements;
	zend_native_user_call_resolution *resolution;
	uint32_t placement_capacity;
	uint32_t *target_set;
	uint32_t target_capacity;
	zend_function *function = NULL;
	void *object_or_called_scope = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t argument_count;
	bool receiver_owned = false;
	bool no_call = false;

	if (activation == NULL || !activation->setup_record
			|| zend_native_active_direct_call != activation) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Native user call activation is missing");
		}
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	caller = activation->caller;
	descriptor = (const zend_native_user_call_descriptor *)
		activation->descriptor;
	resolution = &activation->resolution;
	placements = resolution->placements;
	placement_capacity = activation->placement_capacity;
	target_set = placements != NULL
		? (uint32_t *) (placements + placement_capacity) : NULL;
	if (activation->setup_frame == NULL || target_set == NULL
			|| (char *) target_set
				> (char *) activation->setup_frame + activation->setup_size) {
		zend_throw_error(NULL, "Invalid native call placement workspace");
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	if (caller != NULL && descriptor != NULL
			&& zend_native_call_resolve_cached(
				activation, caller, descriptor, argument_count_hint)) {
		return ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS;
	}
	target_capacity = (uint32_t) (
		((char *) activation->setup_frame + activation->setup_size
			- (char *) target_set) / sizeof(*target_set));
	memset(resolution, 0, sizeof(*resolution));
	resolution->placements = placements;
	if (caller == NULL || caller->func == NULL || descriptor == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| descriptor->argument_count > ZEND_MIR_ID_MAX
			|| descriptor->initial_argument_count > descriptor->argument_count
			|| descriptor->init_source_position
				>= caller->func->op_array.last
			|| descriptor->do_source_position >= caller->func->op_array.last
			|| (descriptor->flags
				& ~ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT) != 0
			|| (descriptor->do_opcode != ZEND_DO_UCALL
				&& descriptor->do_opcode != ZEND_DO_FCALL
				&& descriptor->do_opcode != ZEND_DO_FCALL_BY_NAME
				&& descriptor->do_opcode != ZEND_DO_ICALL)) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid native user call descriptor");
		}
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	argument_count = argument_count_hint
		== ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO
		? descriptor->initial_argument_count : argument_count_hint;
	if (argument_count > ZEND_MIR_ID_MAX) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Native user call argument count is invalid");
		}
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}

	/* Target lookup and reentry resolution may allocate, throw, or bail out.
	 * Keep every target borrowed until all such work has completed. A bailout is
	 * handled by the outer C-to-native boundary without an untracked retain. */
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->init_source_position];
	EG(current_execute_data) = caller;
	switch (descriptor->init_opcode) {
		case ZEND_INIT_FCALL: {
			zval *name = zend_native_direct_operand(
				caller, &descriptor->init_op2, true);

			if (name != NULL && Z_ISREF_P(name)) {
				name = Z_REFVAL_P(name);
			}
			void **cache_slot = zend_native_call_init_cache_slot(
				caller, descriptor, 1);

			if (cache_slot != NULL && cache_slot[0] != NULL) {
				function = cache_slot[0];
				break;
			}
			function = name != NULL && Z_TYPE_P(name) == IS_STRING
				? zend_fetch_function(Z_STR_P(name)) : NULL;
			if (function != NULL && cache_slot != NULL) {
				cache_slot[0] = function;
			}
			if (function == NULL && EG(exception) == NULL) {
				zend_throw_error(NULL, "Call to undefined function %s()",
					name != NULL && Z_TYPE_P(name) == IS_STRING
						? Z_STRVAL_P(name) : "unknown");
			}
			break;
		}
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
			if (!zend_native_call_named_target(
					caller, descriptor,
					descriptor->init_opcode == ZEND_INIT_NS_FCALL_BY_NAME,
					&function, &object_or_called_scope, &call_info)
					&& EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native named call target cannot be resolved");
			}
			break;
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
			if (!zend_native_call_dynamic_target(
					caller, descriptor, &function,
					&object_or_called_scope, &call_info, false)
					&& EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native dynamic call target cannot be resolved");
			}
			break;
		case ZEND_INIT_METHOD_CALL: {
			zend_object *object = NULL;

			function = zend_native_call_object_method(
				caller, descriptor, &object, &receiver_owned, false, false);
			object_or_called_scope = object;
			if (function != NULL && object != NULL
					&& (function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
				call_info |= ZEND_CALL_HAS_THIS;
				if (receiver_owned) {
					call_info |= ZEND_CALL_RELEASE_THIS;
				}
			} else if ((function == NULL || object == NULL)
					&& EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native method target cannot be resolved");
			}
			break;
		}
		case ZEND_INIT_STATIC_METHOD_CALL: {
			zend_class_entry *called_scope = NULL;

			function = zend_native_call_static_method(
				caller, descriptor, &called_scope, false);
			object_or_called_scope = called_scope;
			if (function != NULL && called_scope != NULL
					&& (function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
				call_info |= ZEND_CALL_HAS_THIS;
			} else if ((function == NULL || called_scope == NULL)
					&& EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native static method target cannot be resolved");
			}
			break;
		}
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL: {
			zend_object *object = NULL;

			function = zend_native_call_parent_property_hook(
				caller, descriptor, &object);
			object_or_called_scope = object;
			if (function != NULL && object != NULL) {
				call_info |= ZEND_CALL_HAS_THIS;
			} else if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native parent property hook target cannot be resolved");
			}
			break;
		}
		case ZEND_NEW: {
			zend_object *object = NULL;
			bool constructor_missing = false;

			function = zend_native_call_constructor(
				caller, descriptor, &object, &constructor_missing, false);
			if (constructor_missing) {
				no_call = true;
				break;
			}
			object_or_called_scope = object;
			if (function != NULL && object != NULL) {
				call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
			} else if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Native constructor target cannot be resolved");
			}
			break;
		}
		default:
			zend_throw_error(NULL, "Native call source opcode is invalid");
			return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	if (EG(exception) != NULL || (!no_call && function == NULL)) {
		zend_native_call_consume_resolved_init_operands(caller, descriptor);
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	resolution->function = function;
	resolution->object_or_called_scope = object_or_called_scope;
	resolution->call_info = call_info;
	resolution->argument_count = argument_count;
	if (no_call) {
		resolution->function = (zend_function *) &zend_pass_function;
		resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_NO_CALL;
		resolution->invoke_entry = zend_native_call_dynamic_completed_entry;
		if (!zend_native_call_resolution_build_placements(
				caller, descriptor, argument_count,
				placements, placement_capacity,
				target_set, target_capacity, resolution)
				|| !zend_native_call_resolution_set_sizes(
					resolution, resolution->argument_count)) {
			zend_native_call_release_user_resolution(resolution);
			zend_native_call_consume_resolved_init_operands(caller, descriptor);
			return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
		}
		if (descriptor->init_opcode == ZEND_NEW) {
			zend_native_call_fast_publish_new_without_constructor(
				caller, descriptor, resolution);
		}
		return ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS;
	}
	cached = zend_native_call_resolution_cache_find(
		caller, descriptor, function, argument_count);
	if (cached != NULL && cached->resolution.placement_count
			<= placement_capacity) {
		*resolution = cached->resolution;
		resolution->placements = placements;
		resolution->object_or_called_scope = object_or_called_scope;
		resolution->call_info = call_info;
		memcpy(placements, cached->placements,
			cached->resolution.placement_count * sizeof(*placements));
	} else {
		if (!zend_native_call_resolution_classify(
				entry_cell_hint, resolution)
				|| !zend_native_call_resolution_build_placements(
					caller, descriptor, argument_count,
					placements, placement_capacity,
					target_set, target_capacity, resolution)
				|| !zend_native_call_resolution_set_sizes(
					resolution, resolution->argument_count)) {
			zend_native_call_release_user_resolution(resolution);
			zend_native_call_consume_resolved_init_operands(
				caller, descriptor);
			return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
		}
		zend_native_call_resolution_cache_store(
			caller, descriptor, resolution);
	}
	zend_native_call_resolution_retain_target(resolution);
	zend_native_call_consume_resolved_init_operands(caller, descriptor);
	if (EG(exception) != NULL) {
		zend_native_call_release_user_resolution(resolution);
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	return ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS;
}

static bool zend_native_call_start_runtime_expansion(
	zend_native_direct_activation *activation)
{
	zend_native_user_call_resolution *resolution = &activation->resolution;
	zend_execute_data *callee = activation->callee;

	if ((resolution->placement_flags
			& ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_STARTED) != 0) {
		return true;
	}
	if (callee == NULL || activation->caller->call != callee
			|| resolution->direct_argument_count > resolution->argument_count) {
		return false;
	}
	ZEND_CALL_NUM_ARGS(callee) = resolution->direct_argument_count;
	resolution->placement_flags |=
		ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_STARTED;
	return true;
}

static bool zend_native_call_restore_runtime_target(
	zend_native_direct_activation *activation,
	const zend_native_user_call_placement *placement)
{
	zend_execute_data *callee = activation->callee;
	uint32_t ordinal;

	if (placement->target_index
			== ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_INVALID
			|| placement->target_index < ZEND_CALL_NUM_ARGS(callee)) {
		return true;
	}
	if (placement->target_index >= activation->resolution.argument_count) {
		return false;
	}
	/* The reserved frame covers every statically known target. Restore the
	 * undefined gap immediately before its source SEND, just as the VM does. */
	for (ordinal = ZEND_CALL_NUM_ARGS(callee);
			ordinal <= placement->target_index; ordinal++) {
		ZVAL_UNDEF(ZEND_CALL_ARG(callee, ordinal + 1));
	}
	ZEND_CALL_NUM_ARGS(callee) = placement->target_index + 1;
	return true;
}

zend_result zend_native_call_send_resolved_argument(
	zend_native_direct_activation *activation, uint32_t placement_index)
{
	zend_execute_data *caller;
	const zend_native_user_call_descriptor *descriptor;
	zend_native_user_call_resolution *resolution;
	zend_native_user_call_placement *placement;

	if (activation == NULL || !activation->setup_record
			|| zend_native_active_direct_call != activation) {
		zend_throw_error(NULL, "Invalid native user call activation");
		return FAILURE;
	}
	caller = activation->caller;
	descriptor = (const zend_native_user_call_descriptor *)
		activation->descriptor;
	resolution = &activation->resolution;
	if (caller == NULL || caller->func == NULL || descriptor == NULL
			|| activation->callee == NULL
			|| caller->call != activation->callee
			|| !ZEND_USER_CODE(caller->func->type)
			|| resolution->function == NULL
			|| activation->callee->func != resolution->function
			|| resolution->placement_count != descriptor->argument_count
			|| resolution->placements == NULL
			|| placement_index >= resolution->placement_count) {
		zend_throw_error(NULL, "Invalid resolved native argument state");
		return FAILURE;
	}
	placement = &resolution->placements[placement_index];
	if (placement->source_index >= descriptor->argument_count
			|| (placement->flags
				& ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION) == 0
			|| (placement->flags
				& ZEND_NATIVE_USER_CALL_PLACEMENT_SOURCE_EVALUATED) != 0
			|| !zend_native_call_start_runtime_expansion(activation)
			|| !zend_native_call_restore_runtime_target(
				activation, placement)) {
		zend_throw_error(NULL, "Invalid resolved native argument placement");
		return FAILURE;
	}
	if (zend_native_call_set_explicit_argument(
			caller,
			&descriptor->arguments[placement->source_index]) == FAILURE) {
		return FAILURE;
	}
	activation->callee = caller->call;
	if (activation->callee == NULL || activation->callee->func == NULL) {
		return FAILURE;
	}
	placement->flags |=
		ZEND_NATIVE_USER_CALL_PLACEMENT_SOURCE_EVALUATED;
	return SUCCESS;
}

zend_execute_data *zend_native_call_expand_user_arguments(
	zend_native_direct_activation *activation)
{
	zend_execute_data *caller;
	const zend_native_user_call_descriptor *descriptor;
	zend_native_user_call_resolution *resolution;
	zend_execute_data *callee;
	uint32_t index;

	if (activation == NULL || !activation->setup_record
			|| zend_native_active_direct_call != activation) {
		zend_throw_error(NULL, "Invalid native user call activation");
		return NULL;
	}
	caller = activation->caller;
	descriptor = (const zend_native_user_call_descriptor *)
		activation->descriptor;
	resolution = &activation->resolution;
	callee = activation->callee;
	if (caller == NULL || caller->func == NULL || descriptor == NULL
			|| callee == NULL || caller->call != callee
			|| !ZEND_USER_CODE(caller->func->type)
			|| resolution->function == NULL
			|| callee->func != resolution->function
			|| resolution->placement_count != descriptor->argument_count
			|| (resolution->placement_count != 0
				&& resolution->placements == NULL)
			|| resolution->direct_argument_count > resolution->argument_count) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Invalid native user call expansion state");
		}
		return NULL;
	}
	if ((resolution->placement_flags
			& ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION) == 0) {
		return callee;
	}
	if (!zend_native_call_start_runtime_expansion(activation)) {
		return NULL;
	}
	if ((resolution->placement_flags
			& ZEND_NATIVE_USER_CALL_PLACEMENTS_EXTRA_NAMED) != 0) {
		if ((ZEND_CALL_INFO(callee)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			if (callee->extra_named_params == NULL) {
				zend_throw_error(NULL,
					"Native user call extra named arguments are invalid");
				return NULL;
			}
		} else if (callee->extra_named_params != NULL) {
			zend_throw_error(NULL,
				"Native user call extra named arguments already exist");
			return NULL;
		} else {
			resolution->extra_named_params = zend_new_array(0);
			resolution->ownership |=
				ZEND_NATIVE_USER_CALL_OWNS_EXTRA_NAMED_PARAMS;
			callee->extra_named_params = resolution->extra_named_params;
			ZEND_ADD_CALL_FLAG(callee, ZEND_CALL_HAS_EXTRA_NAMED_PARAMS);
			resolution->extra_named_params = NULL;
			resolution->ownership &=
				~ZEND_NATIVE_USER_CALL_OWNS_EXTRA_NAMED_PARAMS;
		}
	}
	for (index = 0; index < resolution->placement_count; index++) {
		const zend_native_user_call_placement *placement =
			&resolution->placements[index];

		if ((placement->flags
				& ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION) == 0) {
			continue;
		}
		if ((placement->flags
				& ZEND_NATIVE_USER_CALL_PLACEMENT_SOURCE_EVALUATED) != 0) {
			continue;
		}
		if (!zend_native_call_restore_runtime_target(
				activation, placement)) {
			return NULL;
		}
		if (placement->source_index >= descriptor->argument_count
				|| zend_native_call_set_explicit_argument(
					caller,
					&descriptor->arguments[placement->source_index]) == FAILURE) {
			return NULL;
		}
		callee = caller->call;
		activation->callee = callee;
		if (callee == NULL || callee->func == NULL) {
			return NULL;
		}
	}
	resolution->argument_count = ZEND_CALL_NUM_ARGS(callee);
	if (!zend_native_call_resolution_set_sizes(
			resolution, resolution->argument_count)) {
		return NULL;
	}
	return callee;
}

void zend_native_call_begin(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor)
{
	zend_execute_data *call;
	zend_function *function;
	const zend_op *source_init;
	void *object_or_called_scope = NULL;
	bool receiver_owned = false;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t initial_argument_count;
	uint32_t index;

	if (caller == NULL || cell == NULL || descriptor == NULL) {
		zend_native_call_abort("Invalid pending native call state");
	}
	if (cell->state != ZEND_NATIVE_ENTRY_READY || cell->code == NULL
			|| cell->function == NULL) {
		zend_native_call_abort("Native callee entry is not ready");
	}
	function = cell->function;
	if (caller->func == NULL || !ZEND_USER_CODE(caller->func->type)
			|| descriptor->init_source_position
				>= caller->func->op_array.last
			|| descriptor->initial_argument_count
				> descriptor->argument_count) {
		zend_native_call_abort("Native call source position is invalid");
	}
	source_init = &caller->func->op_array.opcodes[
		descriptor->init_source_position];
	/*
	 * Target resolution may allocate objects or raise before the call frame
	 * exists (notably NEW creates the object before invoking its constructor).
	 * Publish the source call site first so exception file/line and backtrace
	 * state match the VM for every exit from call setup.
	 */
	caller->opline = source_init;
	EG(current_execute_data) = caller;
	initial_argument_count = descriptor->initial_argument_count;
#ifdef ZEND_CHECK_STACK_LIMIT
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		zend_bailout();
	}
#endif
	switch (descriptor->init_opcode) {
		case ZEND_INIT_FCALL: {
			zval *name = zend_native_direct_operand(
				caller, &descriptor->init_op2, true);
			zend_function *resolved =
				name != NULL && Z_TYPE_P(name) == IS_STRING
				? zend_fetch_function(Z_STR_P(name)) : NULL;

			if (resolved == NULL) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL, "Call to undefined function %s()",
						name != NULL && Z_TYPE_P(name) == IS_STRING
							? Z_STRVAL_P(name) : "unknown");
				}
				function = (zend_function *) &zend_pass_function;
				break;
			}
			if (resolved->type == ZEND_USER_FUNCTION
					&& resolved != function) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, resolved);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					function = zend_native_call_reject_target(
						"Native named target compilation failed");
					break;
				}
			}
			function = resolved;
			break;
		}
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
			if (!zend_native_call_named_target(
					caller, descriptor,
					descriptor->init_opcode == ZEND_INIT_NS_FCALL_BY_NAME,
					&function, &object_or_called_scope, &call_info)) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native named call target cannot be resolved");
				}
				function = (zend_function *) &zend_pass_function;
				object_or_called_scope = NULL;
				break;
			}
			if (function->type == ZEND_USER_FUNCTION) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, function);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					function = zend_native_call_reject_target(
						"Native named target compilation failed");
					object_or_called_scope = NULL;
				}
			}
			break;
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
			if (!zend_native_call_dynamic_target(
					caller, descriptor, &function,
					&object_or_called_scope, &call_info, true)) {
				zend_native_call_consume_source_slot(
					caller, &descriptor->init_op2);
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native dynamic call target cannot be resolved");
				}
				function = (zend_function *) &zend_pass_function;
				object_or_called_scope = NULL;
				break;
			}
			if (function->type == ZEND_USER_FUNCTION
					&& (function->common.fn_flags
						& ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, function);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
						OBJ_RELEASE((zend_object *) object_or_called_scope);
					} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
						OBJ_RELEASE(ZEND_CLOSURE_OBJECT(function));
					}
					function = zend_native_call_reject_target(
						"Native dynamic target compilation failed");
					object_or_called_scope = NULL;
					call_info = ZEND_CALL_NESTED_FUNCTION;
				}
			}
			zend_native_call_consume_source_slot(
				caller, &descriptor->init_op2);
			break;
		case ZEND_INIT_METHOD_CALL: {
			zend_object *object = NULL;
			zend_function *resolved = zend_native_call_object_method(
				caller, descriptor, &object, &receiver_owned, true, true);

			if (resolved == NULL || object == NULL) {
				if (receiver_owned) {
					OBJ_RELEASE(object);
				}
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native method target cannot be resolved");
				}
				function = (zend_function *) &zend_pass_function;
				object_or_called_scope = NULL;
				break;
			}
			if (resolved != function && resolved->type == ZEND_USER_FUNCTION
					&& (resolved->common.fn_flags
						& ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, resolved);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					if (receiver_owned) {
						OBJ_RELEASE(object);
					}
					function = zend_native_call_reject_target(
						"Native method target compilation failed");
					object_or_called_scope = NULL;
					break;
				}
			}
			function = resolved;
			object_or_called_scope = object;
			if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
				call_info |= ZEND_CALL_HAS_THIS;
				if (receiver_owned) {
					call_info |= ZEND_CALL_RELEASE_THIS;
				}
			}
			break;
		}
		case ZEND_INIT_STATIC_METHOD_CALL: {
			zend_class_entry *called_scope = NULL;
			zend_function *resolved = zend_native_call_static_method(
				caller, descriptor, &called_scope, true);

			if (resolved == NULL || called_scope == NULL) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native static method target cannot be resolved");
				}
				function = (zend_function *) &zend_pass_function;
				object_or_called_scope = NULL;
				break;
			}
			if (resolved != function && resolved->type == ZEND_USER_FUNCTION
					&& (resolved->common.fn_flags
						& ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, resolved);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					function = zend_native_call_reject_target(
						"Native static method target compilation failed");
					object_or_called_scope = NULL;
					break;
				}
			}
			function = resolved;
			object_or_called_scope = called_scope;
			if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
				call_info |= ZEND_CALL_HAS_THIS;
			}
			break;
		}
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL: {
			zend_object *object = NULL;
			zend_function *resolved =
				zend_native_call_parent_property_hook(
					caller, descriptor, &object);

			if (resolved == NULL || object == NULL) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native parent property hook target cannot be resolved");
				}
				function = (zend_function *) &zend_pass_function;
				object_or_called_scope = NULL;
				break;
			}
			if (resolved != function && resolved->type == ZEND_USER_FUNCTION
					&& (resolved->common.fn_flags
						& ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, resolved);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					function = zend_native_call_reject_target(
						"Native parent property hook compilation failed");
					object_or_called_scope = NULL;
					break;
				}
			}
			function = resolved;
			object_or_called_scope = object;
			call_info |= ZEND_CALL_HAS_THIS;
			break;
		}
		case ZEND_NEW: {
			zend_object *object = NULL;
			bool constructor_missing = false;
			zend_function *resolved = zend_native_call_constructor(
				caller, descriptor, &object, &constructor_missing, true);

			if (constructor_missing) {
				function = (zend_function *) &zend_pass_function;
				break;
			}
			if (resolved == NULL || object == NULL) {
				if (object != NULL) {
					OBJ_RELEASE(object);
				}
				if (EG(exception) == NULL) {
					zend_throw_error(NULL,
						"Native constructor target cannot be resolved");
				}
				function = (zend_function *) &zend_pass_function;
				object_or_called_scope = NULL;
				break;
			}
			if (resolved != function && resolved->type == ZEND_USER_FUNCTION) {
				cell = zend_native_reentry_find(
					zend_native_active_reentry_scope, resolved);
				if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
						|| cell->code == NULL) {
					OBJ_RELEASE(object);
					function = zend_native_call_reject_target(
						"Native constructor target compilation failed");
					object_or_called_scope = NULL;
					break;
				}
			}
			function = resolved;
			object_or_called_scope = object;
			call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
			break;
		}
		default:
			zend_native_call_abort("Native call source opcode is invalid");
	}
	call = zend_vm_stack_push_call_frame(
		call_info, function, initial_argument_count, object_or_called_scope);
	for (index = 0; index < initial_argument_count; index++) {
		ZVAL_UNDEF(ZEND_CALL_ARG(call, index + 1));
	}
	call->prev_execute_data = caller->call;
	caller->call = call;
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

static bool zend_native_is_call_start(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
			return true;
		default:
			return false;
	}
}

static bool zend_native_is_call_end(uint8_t opcode)
{
	switch (opcode) {
		case ZEND_DO_FCALL:
		case ZEND_DO_ICALL:
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			return true;
		default:
			return false;
	}
}

/*
 * A direct call transfers its arguments at DO, so it never builds the
 * pending frame through which the VM releases arguments already sent when an
 * exception interrupts the argument list. Their temporaries are outside
 * Zend's live ranges but still own the values: release those of every call
 * enclosing the throwing opline, found as cleanup_unfinished_calls() does.
 * Only valid while no call frame is pending; a group of overlapping calls is
 * either all direct or all materialized.
 */
static void zend_native_release_direct_pending_arguments(
	zend_execute_data *execute_data, const zend_op_array *op_array,
	uint32_t throw_op_num)
{
	const zend_op *opline = &op_array->opcodes[throw_op_num];
	uint32_t level = 0;

	if (zend_native_is_call_start(opline->opcode)) {
		if (throw_op_num == 0) {
			return;
		}
		opline--;
	}
	for (; opline >= op_array->opcodes; opline--) {
		if (zend_native_is_call_end(opline->opcode)) {
			level++;
		} else if (zend_native_is_call_start(opline->opcode)) {
			if (level != 0) {
				level--;
			}
		} else if (level == 0
				&& zend_native_generator_is_send(opline->opcode)
				&& (opline->op1_type & (IS_TMP_VAR | IS_VAR)) != 0) {
			zval *value = ZEND_CALL_VAR(execute_data, opline->op1.var);

			if (!Z_ISUNDEF_P(value)
					&& !zend_native_generator_temporary_is_live(op_array,
						EX_VAR_TO_NUM(opline->op1.var), throw_op_num)) {
				zval detached;

				ZVAL_COPY_VALUE(&detached, value);
				ZVAL_UNDEF(value);
				zval_ptr_dtor_nogc(&detached);
			}
		}
	}
}

void zend_native_cleanup_unfinished_exception(
	zend_execute_data *execute_data, uint32_t throw_op_num,
	uint32_t catch_op_num)
{
	const zend_op_array *op_array;
	const zend_op *throw_op;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		return;
	}
	op_array = &execute_data->func->op_array;
	if (throw_op_num >= op_array->last) {
		return;
	}
	throw_op = &op_array->opcodes[throw_op_num];
	if ((throw_op->result_type & (IS_VAR | IS_TMP_VAR)) != 0) {
		switch (throw_op->opcode) {
			/* Native calls may place their result directly in a pending
			 * argument. The source result slot is not authoritative and may
			 * never have been initialized when a suspended call is unwound. */
			case ZEND_DO_FCALL:
			case ZEND_DO_UCALL:
			case ZEND_DO_ICALL:
			case ZEND_DO_FCALL_BY_NAME:
			case ZEND_ADD_ARRAY_ELEMENT:
			case ZEND_ADD_ARRAY_UNPACK:
			case ZEND_ROPE_INIT:
			case ZEND_ROPE_ADD:
			case ZEND_FETCH_CLASS:
			case ZEND_DECLARE_ANON_CLASS:
				break;
			default:
				if (!zend_is_smart_branch(throw_op)) {
					zval *result = ZEND_CALL_VAR(
						execute_data, throw_op->result.var);
					/* A throwing operation may publish EG(exception) through its
					 * result slot without transferring a second reference.  The
					 * exception remains owned by EG(exception) until CATCH moves it;
					 * treating this borrowed alias as an ordinary temporary would
					 * destroy the exception before the handler can receive it. */
					if (EG(exception) != NULL
							&& Z_TYPE_P(result) == IS_OBJECT
							&& Z_OBJ_P(result) == EG(exception)
							&& GC_REFCOUNT(EG(exception)) == 1) {
						ZVAL_UNDEF(result);
					} else {
						zval_ptr_dtor_nogc(result);
					}
				}
		}
	}
	if (execute_data->call == NULL) {
		zend_native_release_direct_pending_arguments(
			execute_data, op_array, throw_op_num);
	}
	zend_cleanup_unfinished_execution(
		execute_data, throw_op_num, catch_op_num);
}

zend_result zend_native_prepare_finally_exception(
	zend_execute_data *caller, uint32_t source_opline_index)
{
	zend_op_array *op_array;
	zend_object *exception;
	uint32_t index;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| source_opline_index >= caller->func->op_array.last
			|| EG(exception) == NULL) {
		return FAILURE;
	}
	op_array = &caller->func->op_array;
	/* The exception may have been raised by a nested user frame, whose
	 * zend_rethrow_exception() replaced the caller's published opline with the
	 * shared HANDLE_EXCEPTION sentinel. The native edge is source-backed, so
	 * restore its exact call site before selecting catch/finally routing or
	 * propagating out of the frame. */
	caller->opline = &op_array->opcodes[source_opline_index];
	exception = EG(exception);
	/* Zend's dispatcher walks the innermost active region outward. */
	for (index = op_array->last_try_catch; index-- > 0;) {
		const zend_try_catch_element *region = &op_array->try_catch_array[index];
		const zend_op *fast_ret;
		zval *fast_call;

		/* Match zend_dispatch_try_catch_finally_helper(): regions whose try
		 * body begins after the throwing instruction are not active yet. */
		if (region->try_op > source_opline_index) {
			continue;
		}

		if (region->catch_op != 0
				&& source_opline_index < region->catch_op) {
			/* The generated exception edge enters the catch dispatcher, which
			 * still needs the exception in EG(exception). */
			return SUCCESS;
		}
		if (region->finally_op != 0
				&& source_opline_index < region->finally_op) {
			if (zend_is_unwind_exit(exception)) {
				continue;
			}
			if (region->finally_end >= op_array->last) {
				return FAILURE;
			}
			fast_ret = &op_array->opcodes[region->finally_end];
			if (fast_ret->opcode != ZEND_FAST_RET
					|| fast_ret->op1_type != IS_TMP_VAR) {
				return FAILURE;
			}
			zend_native_cleanup_unfinished_exception(caller,
				source_opline_index, region->finally_op);
			fast_call = ZEND_CALL_VAR(caller, fast_ret->op1.var);
			Z_OBJ_P(fast_call) = exception;
			EG(exception) = NULL;
			Z_OPLINE_NUM_P(fast_call) = UINT32_MAX;
			caller->opline = &op_array->opcodes[region->finally_op];
			return SUCCESS;
		}
		if (region->finally_op == 0 || region->finally_end == 0
				|| source_opline_index >= region->finally_end
				|| region->finally_end >= op_array->last) {
			continue;
		}
		fast_ret = &op_array->opcodes[region->finally_end];
		if (fast_ret->opcode != ZEND_FAST_RET
				|| fast_ret->op1_type != IS_TMP_VAR) {
			return FAILURE;
		}
		fast_call = ZEND_CALL_VAR(caller, fast_ret->op1.var);
		if (Z_OPLINE_NUM_P(fast_call) != UINT32_MAX) {
			uint32_t return_opline_index = Z_OPLINE_NUM_P(fast_call);

			if (return_opline_index >= op_array->last) {
				return FAILURE;
			}
			if ((op_array->opcodes[return_opline_index].op2_type
					& (IS_TMP_VAR | IS_VAR)) != 0) {
				zval *return_value = ZEND_CALL_VAR(caller,
					op_array->opcodes[return_opline_index].op2.var);

				zval_ptr_dtor(return_value);
				ZVAL_NULL(return_value);
			}
		}
		if (Z_OBJ_P(fast_call) != NULL) {
			if (zend_is_unwind_exit(exception)
					|| zend_is_graceful_exit(exception)) {
				OBJ_RELEASE(Z_OBJ_P(fast_call));
			} else {
				zend_exception_set_previous(exception, Z_OBJ_P(fast_call));
			}
			Z_OBJ_P(fast_call) = NULL;
		}
	}
	return SUCCESS;
}

static zend_native_status zend_native_call_invoke(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	zval *return_value)
{
	zend_execute_data *call;
	zend_execute_data *pending_call;
	const zend_native_code *code = NULL;
	zend_native_status status;

	if (caller == NULL || cell == NULL || return_value == NULL
			|| caller->call == NULL) {
		zend_native_call_abort("Invalid native invocation state");
	}
	call = caller->call;
	pending_call = call->prev_execute_data;
	if ((call->func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0
			&& call->func->type == ZEND_USER_FUNCTION) {
		zend_function *trampoline = call->func;
		zend_array *arguments = NULL;
		uint32_t argument_count = ZEND_CALL_NUM_ARGS(call);
		uint32_t call_info = ZEND_CALL_INFO(call);

		if (argument_count != 0) {
			zval *argument = ZEND_CALL_ARG(call, 1);
			zval *end = argument + argument_count;

			arguments = zend_new_array(argument_count);
			zend_hash_real_init_packed(arguments);
			ZEND_HASH_FILL_PACKED(arguments) {
				do {
					ZEND_HASH_FILL_ADD(argument);
					argument++;
				} while (argument != end);
			} ZEND_HASH_FILL_END();
		}
		call->func = (trampoline->op_array.fn_flags & ZEND_ACC_STATIC) != 0
			? trampoline->op_array.scope->__callstatic
			: trampoline->op_array.scope->__call;
		if (call->func == NULL) {
			zend_free_trampoline(trampoline);
			zend_native_call_abort("Native magic method target is missing");
		}
		ZEND_CALL_NUM_ARGS(call) = 2;
		ZVAL_STR(ZEND_CALL_ARG(call, 1), trampoline->common.function_name);
		if (arguments != NULL) {
			ZVAL_ARR(ZEND_CALL_ARG(call, 2), arguments);
		} else {
			ZVAL_EMPTY_ARRAY(ZEND_CALL_ARG(call, 2));
		}
		if ((call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zval *packed = ZEND_CALL_ARG(call, 2);

			if (zend_hash_num_elements(Z_ARRVAL_P(packed)) == 0) {
				GC_ADDREF(call->extra_named_params);
				ZVAL_ARR(packed, call->extra_named_params);
			} else {
				SEPARATE_ARRAY(packed);
				zend_hash_copy(Z_ARRVAL_P(packed), call->extra_named_params,
					zval_add_ref);
			}
		}
		zend_free_trampoline(trampoline);
	}
	if (call->func->type == ZEND_USER_FUNCTION
			&& call->func != (zend_function *) &zend_pass_function
			&& call->func != cell->function) {
		cell = zend_native_reentry_find(
			zend_native_active_reentry_scope, call->func);
	}
	if (call->func->type == ZEND_USER_FUNCTION
			&& call->func != (zend_function *) &zend_pass_function
			&& (cell == NULL
				|| (code = zend_native_entry_cell_load(cell)) == NULL)) {
		zend_native_call_abort("Resolved native invocation target is not ready");
	}
	if (EG(exception) == NULL
			&& call->func->type == ZEND_USER_FUNCTION
			&& call->func != (zend_function *) &zend_pass_function) {
		(void) zend_native_call_preflight(caller, call->func);
	}
	if (EG(exception) != NULL) {
		zend_vm_stack_free_args(call);
		if ((ZEND_CALL_INFO(call)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_free_extra_named_params(call->extra_named_params);
		}
		zend_native_call_release_target(call);
		zend_vm_stack_free_call_frame(call);
		caller->call = pending_call;
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((ZEND_CALL_INFO(call) & ZEND_CALL_MAY_HAVE_UNDEF) != 0
			&& zend_handle_undef_args(call) == FAILURE) {
		zend_vm_stack_free_args(call);
		if ((ZEND_CALL_INFO(call)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_free_extra_named_params(call->extra_named_params);
		}
		zend_native_call_release_target(call);
		zend_vm_stack_free_call_frame(call);
		caller->call = pending_call;
		return ZEND_NATIVE_EXCEPTION;
	}
	if (call->func == (zend_function *) &zend_pass_function) {
		zend_vm_stack_free_args(call);
		if ((ZEND_CALL_INFO(call)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_free_extra_named_params(call->extra_named_params);
		}
		ZVAL_NULL(return_value);
		zend_vm_stack_free_call_frame(call);
		caller->call = pending_call;
		return ZEND_NATIVE_RETURNED;
	}
	if (call->func->type == ZEND_INTERNAL_FUNCTION) {
		zend_native_internal_call_cell internal_cell;

		internal_cell.function = call->func;
		internal_cell.called_scope = NULL;
		internal_cell.receiver_kind = ZEND_NATIVE_INTERNAL_RECEIVER_NONE;
		return zend_native_internal_call_invoke_finish(
			caller, &internal_cell, return_value);
	}
	/* A rebound Closure has a request-local zend_op_array wrapper with the
	 * canonical opcode storage but a distinct scope and runtime cache.  The
	 * entry cell owns code for that canonical opcode body; frame initialization
	 * must nevertheless use the function actually selected by PHP so visibility,
	 * late-static binding, and closure-local state retain their runtime meaning. */
	caller->call = pending_call;
	zend_init_func_execute_data(call, &call->func->op_array, NULL);
	EG(current_execute_data) = caller;
	if (cell->frame_probe != NULL) {
		cell->frame_probe(cell->frame_probe_context, caller, call);
	}
	ZVAL_UNDEF(return_value);
	call->return_value = return_value;
	zend_native_entry_cell_retain_active(cell);
	EG(current_execute_data) = call;
	status = zend_native_execute_frame(code, call, NULL);
	EG(current_execute_data) = caller;
	zend_native_entry_cell_release_active(cell);
	if (status == ZEND_NATIVE_GENERATOR_CREATED) {
		zend_native_call_release_generator_source_target(call);
		status = ZEND_NATIVE_RETURNED;
	} else {
		zend_native_call_release_target(call);
	}
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		status = ZEND_NATIVE_EXCEPTION;
	}
	zend_vm_stack_free_call_frame(call);
	return status;
}

static zval *zend_native_direct_operand(
	zend_execute_data *execute_data,
	const zend_mir_source_operand_ref *operand,
	bool allow_literal)
{
	zend_op_array *op_array;
	uint32_t physical_slot;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| operand == NULL) {
		return NULL;
	}
	op_array = &execute_data->func->op_array;
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		return allow_literal && operand->index < op_array->last_literal
			? &op_array->literals[operand->index] : NULL;
	}
	if (operand->kind != ZEND_MIR_SOURCE_OPERAND_SLOT
			&& operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA) {
		return NULL;
	}
	switch (operand->slot_kind) {
		case ZEND_MIR_SOURCE_SLOT_CV:
			if (operand->index >= (uint32_t) op_array->last_var) {
				return NULL;
			}
			physical_slot = operand->index;
			break;
		case ZEND_MIR_SOURCE_SLOT_TMP:
		case ZEND_MIR_SOURCE_SLOT_VAR:
			if (operand->index >= op_array->T) {
				return NULL;
			}
			physical_slot = (uint32_t) op_array->last_var + operand->index;
			break;
		default:
			return NULL;
	}
	return ZEND_CALL_VAR_NUM(execute_data, physical_slot);
}

static zend_result zend_native_direct_transfer_argument(
	zend_execute_data *caller,
	zend_execute_data *callee,
	const zend_native_direct_call_argument *argument)
{
	const zend_mir_source_operand_ref *operand = &argument->source_operand;
	zval *source_slot = zend_native_direct_operand(caller, operand, true);
	zval *source = source_slot;
	zval *target;
	bool mutable_source;
	bool temporary_source;
	if (source == NULL || argument->ordinal >= ZEND_CALL_NUM_ARGS(callee)) {
		zend_throw_error(NULL,
			"Invalid direct native call argument source or ordinal");
		return FAILURE;
	}
	target = ZEND_CALL_ARG(callee, argument->ordinal + 1);
	mutable_source = operand->kind != ZEND_MIR_SOURCE_OPERAND_LITERAL;
	temporary_source = operand->kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
		&& operand->slot_kind != ZEND_MIR_SOURCE_SLOT_CV;
	if (temporary_source && Z_TYPE_P(source) == IS_INDIRECT) {
		source = Z_INDIRECT_P(source);
	}
	if (argument->mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		if (!Z_ISREF_P(source)) {
			if (!mutable_source) {
				zend_cannot_pass_by_reference(argument->ordinal + 1);
				return FAILURE;
			}
			ZVAL_MAKE_REF(source);
		}
		ZVAL_COPY(target, source);
		if (temporary_source) {
			zval_ptr_dtor(source_slot);
			ZVAL_UNDEF(source_slot);
		}
		return SUCCESS;
	}
	if (argument->mode != ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
		zend_throw_error(NULL, "Invalid direct native call argument mode");
		return FAILURE;
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
		zend_native_zval_copy_deref_or_dup(target, source);
	} else if (source != source_slot || Z_ISREF_P(source)) {
		zend_native_zval_copy_deref_or_dup(target, source);
		zval_ptr_dtor(source_slot);
		ZVAL_UNDEF(source_slot);
	} else {
		ZVAL_COPY_VALUE(target, source);
		ZVAL_UNDEF(source_slot);
	}
	return SUCCESS;
}

static bool zend_native_direct_scalar_payload(
	const zval *value,
	zend_mir_scalar_type_mask exact_type,
	uint64_t *payload)
{
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			*payload = 0;
			return Z_TYPE_P(value) == IS_NULL;
		case ZEND_MIR_SCALAR_TYPE_I1:
			*payload = Z_TYPE_P(value) == IS_TRUE;
			return Z_TYPE_P(value) == IS_FALSE || Z_TYPE_P(value) == IS_TRUE;
		case ZEND_MIR_SCALAR_TYPE_I64:
			*payload = (uint64_t) Z_LVAL_P(value);
			return Z_TYPE_P(value) == IS_LONG;
		case ZEND_MIR_SCALAR_TYPE_F64:
			memcpy(payload, &Z_DVAL_P(value), sizeof(*payload));
			return Z_TYPE_P(value) == IS_DOUBLE;
		default:
			*payload = 0;
			return exact_type == ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

static zend_native_status zend_native_call_direct_observed_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context)
{
	zend_native_direct_activation *activation;
	zend_native_status status;

	if (context == NULL || context->active_direct_call == NULL
			|| (activation = (zend_native_direct_activation *)
				*context->active_direct_call) == NULL
			|| activation->callee != execute_data
			|| activation->cell == NULL || activation->code == NULL) {
		zend_throw_error(NULL, "Invalid observed direct native call");
		return ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_execute_frame(
		activation->code, execute_data, NULL);
	activation->frame_initialized = false;
	return status;
}

typedef struct _zend_native_call_target_release {
	zend_function *trampoline;
	zend_object *object;
} zend_native_call_target_release;

static zend_native_call_target_release
zend_native_call_snapshot_target_release(
	zend_execute_data *call, bool dynamic_target, bool generator_created)
{
	zend_native_call_target_release release = {0};
	uint32_t call_info;

	if (call == NULL) {
		return release;
	}
	call_info = ZEND_CALL_INFO(call);
	if (dynamic_target) {
		/* ZEND_GENERATOR_CREATE transfers a Closure invocation reference from
		 * the source frame to the generator.  Every other dynamic source frame
		 * keeps the ordinary target-release contract. */
		if (generator_created
				&& (call_info & ZEND_CALL_CLOSURE) != 0
				&& (call_info & ZEND_CALL_RELEASE_THIS) == 0) {
			return release;
		}
		if (call->func != NULL
				&& (call->func->common.fn_flags
				& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
			release.trampoline = call->func;
		}
		if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
			release.object = Z_OBJ(call->This);
		} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
			release.object = ZEND_CLOSURE_OBJECT(call->func);
		}
	} else if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
		release.object = Z_OBJ(call->This);
	}
	return release;
}

static void zend_native_call_release_snapshotted_target(
	zend_native_call_target_release *release)
{
	/* A trampoline may refer to storage owned by the invocation object. */
	if (release->trampoline != NULL) {
		zend_native_call_free_unconsumed_trampoline(
			release->trampoline);
	}
	if (release->object != NULL) {
		OBJ_RELEASE(release->object);
	}
}

static void zend_native_call_transfer_resolution_target(
	zend_native_direct_activation *activation)
{
	zend_native_user_call_resolution *resolution = &activation->resolution;
	zend_execute_data *call = activation->callee;
	uint32_t ownership = resolution->ownership;
	uint32_t call_info;

	if (!activation->setup_record || !activation->dynamic_target
			|| call == NULL) {
		return;
	}
	call_info = ZEND_CALL_INFO(call);
	if ((ownership & ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE) != 0) {
		/* A call-via-handler internal target may destroy its temporary
		 * function and clear execute_data::func before returning.  In that
		 * case the callee has already consumed the transferred ownership. */
		ZEND_ASSERT(call->func == resolution->function
			|| (call->func == NULL && activation->internal_target));
		ZEND_ASSERT(call->func == NULL
			|| (call->func->common.fn_flags
				& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0);
		resolution->ownership &=
			~ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE;
	}
	if ((ownership & ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT) != 0) {
		ZEND_ASSERT((ownership
			& ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE) == 0);
		ZEND_ASSERT((call_info & ZEND_CALL_RELEASE_THIS) != 0);
		ZEND_ASSERT(resolution->owned_target == Z_OBJ(call->This));
		resolution->owned_target = NULL;
		resolution->ownership &=
			~ZEND_NATIVE_USER_CALL_OWNS_TARGET_OBJECT;
	} else if ((ownership
			& ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE) != 0) {
		ZEND_ASSERT((call_info & ZEND_CALL_CLOSURE) != 0);
		ZEND_ASSERT(resolution->owned_target
			== ZEND_CLOSURE_OBJECT(call->func));
		resolution->owned_target = NULL;
		resolution->ownership &=
			~ZEND_NATIVE_USER_CALL_OWNS_TARGET_CLOSURE;
	}
}

void zend_native_frame_activation_pop(
	zend_native_direct_activation *activation)
{
	zend_execute_data *setup_frame;

	if (activation == NULL || !activation->setup_record
			|| activation->setup_frame == NULL
			|| zend_native_active_direct_call == activation
			|| activation->callee != NULL
			|| activation->resolution.ownership != 0
			|| activation->cell_active
			|| activation->raw_arguments_owned
			|| activation->frame_initialized
			|| activation->frame_requires_finish
			|| !Z_ISUNDEF(activation->discarded_return)) {
		return;
	}
	setup_frame = activation->setup_frame;
	activation->setup_frame = NULL;
	activation->setup_record = false;
	zend_vm_stack_free_call_frame(setup_frame);
}

/*
 * A direct activation without a setup record leads its callee frame when the
 * frame shares the current VM-stack page and lives in emalloc() memory when
 * the frame starts a fresh page; Darwin's generated calls still append it to
 * the frame. Free the callee and the activation, which must not be used
 * afterwards.
 */
static void zend_native_direct_activation_free_callee(
	zend_native_direct_activation *activation, zend_execute_data *callee)
{
	bool heap = activation->heap_allocated;
	bool leads = !heap && !activation->setup_record
		&& (char *) activation < (char *) callee;

	zend_vm_stack_free_call_frame(callee);
	if (leads) {
		ZEND_ASSERT((zval *) callee == EG(vm_stack_top));
		EG(vm_stack_top) = (zval *) activation;
	} else if (heap) {
		efree(activation);
	}
}

static void zend_native_call_direct_release(
	zend_native_direct_activation *activation)
{
	zend_execute_data *callee = activation->callee;
	zend_execute_data *caller = activation->caller;
	zend_execute_data *pending_call = activation->pending_call;
	zend_native_direct_activation *previous = activation->previous;
	zend_native_call_target_release target_release;
	bool setup_record = activation->setup_record;
	zval exceptional_internal_return;
	zval *internal_return_value = NULL;
	bool has_exceptional_internal_return = false;

	ZVAL_UNDEF(&exceptional_internal_return);

	ZEND_ASSERT(zend_native_active_direct_call == activation);
	/* The Darwin AArch64 universal call path releases completed internal calls
	 * directly from generated code.  The handler may initialize its result
	 * before throwing, and call-owned cleanup may throw after it returns.  Keep
	 * the result address across release, detach it whenever an exception is
	 * observed, and destroy it only after the caller chain is restored. */
	if (activation->internal_target
			&& !activation->uses_discarded_return && callee != NULL
			&& callee->return_value != NULL
			&& !Z_ISUNDEF_P(callee->return_value)) {
		internal_return_value = callee->return_value;
	}
	if (EG(exception) != NULL && internal_return_value != NULL) {
		ZVAL_COPY_VALUE(&exceptional_internal_return, internal_return_value);
		ZVAL_UNDEF(internal_return_value);
		has_exceptional_internal_return = true;
	}
	if (callee != NULL && activation->frame_initialized) {
		zend_native_execution_cleanup_frame(activation->callee);
		activation->frame_initialized = false;
		activation->frame_requires_finish = false;
	} else if (callee != NULL && activation->raw_arguments_owned) {
		zend_vm_stack_free_args(activation->callee);
		activation->raw_arguments_owned = false;
	}
	if (callee != NULL && (ZEND_CALL_INFO(activation->callee)
			& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0
			&& activation->callee->extra_named_params != NULL) {
		zend_free_extra_named_params(
			activation->callee->extra_named_params);
		activation->callee->extra_named_params = NULL;
		ZEND_DEL_CALL_FLAG(
			activation->callee, ZEND_CALL_HAS_EXTRA_NAMED_PARAMS);
	}
	if (activation->cell_active && activation->cell != NULL) {
		zend_native_entry_cell_release_active(activation->cell);
		activation->cell_active = false;
	}
	if (activation->uses_discarded_return
			&& !Z_ISUNDEF(activation->discarded_return)) {
		zval_ptr_dtor(&activation->discarded_return);
		ZVAL_UNDEF(&activation->discarded_return);
	}
	if (setup_record) {
		/* Once the resolved callee exists, its call flags own the retained
		 * invocation target and trampoline.  Move that ownership out of the
		 * setup record before releasing the remaining resolution resources. */
		zend_native_call_transfer_resolution_target(activation);
		zend_native_call_release_user_resolution(&activation->resolution);
	}
	target_release = zend_native_call_snapshot_target_release(
		callee, activation->dynamic_target, activation->generator_created);
	/* Clear every activation field needed by pop before the callee frame can
	 * disappear.  Legacy activations live inside that frame, while universal
	 * activations live in the preceding setup record. */
	activation->callee = NULL;
	caller->call = pending_call;
	EG(current_execute_data) = caller;
	zend_native_active_direct_call = previous;
	if (callee != NULL) {
		zend_native_direct_activation_free_callee(activation, callee);
	}
	if (setup_record) {
		zend_native_frame_activation_pop(activation);
	}
	/* Object destruction may reenter or bail out.  Do it only after the logical
	 * activation chain and the physical VM stack both expose the restored
	 * caller, so an unwind cannot skip live callee/setup storage. */
	zend_native_call_release_snapshotted_target(&target_release);
	if (!has_exceptional_internal_return && EG(exception) != NULL
			&& internal_return_value != NULL
			&& !Z_ISUNDEF_P(internal_return_value)) {
		ZVAL_COPY_VALUE(&exceptional_internal_return, internal_return_value);
		ZVAL_UNDEF(internal_return_value);
		has_exceptional_internal_return = true;
	}
	if (has_exceptional_internal_return) {
		zval_ptr_dtor(&exceptional_internal_return);
	}
}

void zend_native_frame_activation_release(
	zend_native_direct_activation *activation)
{
	if (activation == NULL || zend_native_active_direct_call != activation) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid native frame activation release");
		}
		return;
	}
	if (activation->status == ZEND_NATIVE_BAILOUT) {
		zend_native_call_direct_abandon_activation(activation);
	} else {
		zend_native_call_direct_release(activation);
	}
}

static zend_native_status zend_native_call_direct_failed_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context)
{
	(void) execute_data;
	(void) context;
	return ZEND_NATIVE_EXCEPTION;
}

static bool zend_native_call_preflight(
	const zend_execute_data *caller, const zend_function *function)
{
	const zend_op *opline;
	uint32_t no_discard;

	if (caller == NULL || function == NULL || caller->opline == NULL) {
		return false;
	}
	opline = caller->opline;
	if (opline->opcode != ZEND_DO_FCALL
			&& opline->opcode != ZEND_DO_FCALL_BY_NAME) {
		return true;
	}
	no_discard = opline->result_type != IS_UNUSED ? 0 : ZEND_ACC_NODISCARD;
	if (UNEXPECTED(function->common.fn_flags
			& (ZEND_ACC_DEPRECATED | no_discard))) {
		if ((function->common.fn_flags & ZEND_ACC_DEPRECATED) != 0) {
			zend_deprecated_function(function);
		}
		if ((function->common.fn_flags & no_discard) != 0
				&& EG(exception) == NULL) {
			zend_nodiscard_function(function);
		}
	}
	return EG(exception) == NULL;
}

zend_native_direct_call_entry zend_native_call_direct_enter(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_direct_call_descriptor *descriptor,
	zend_native_execution_context *context)
{
	zend_native_direct_call_entry result = {
		.callee = caller,
		.entry = zend_native_call_direct_failed_entry
	};
	zend_native_direct_activation *activation;
	zend_execute_data *call;
	zend_native_frame_entry_t entry;
	const zend_native_code *code;
	zend_function *function;
	zval *return_value;
	uint32_t used_stack;
	uint32_t activation_size;
	uint32_t index;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	void *object_or_called_scope = NULL;
	bool receiver_owned = false;
	bool trivial_frame;

	if (caller == NULL || caller->func == NULL || context == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| cell == NULL || descriptor == NULL
			|| (code = zend_native_entry_cell_load(cell)) == NULL
			|| cell->function == NULL
			|| cell->function != descriptor->expected_function
			|| !ZEND_USER_CODE(cell->function->type)
			|| descriptor->argument_count > ZEND_MIR_ID_MAX
			|| (descriptor->flags
				& ~(ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME
					| ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER
					| ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE
					| ZEND_NATIVE_DIRECT_CALL_LEAF_SCALAR_FRAME
					| ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT
					| ZEND_NATIVE_DIRECT_CALL_GENERATION_LEASED)) != 0
			|| descriptor->source_position >= caller->func->op_array.last) {
		zend_throw_error(NULL, "Invalid direct native call descriptor");
		return result;
	}
	function = cell->function;
	entry = zend_native_code_frame_entry(code);
	if (entry == NULL || descriptor->frame_argument_count
			< function->common.required_num_args) {
		zend_throw_error(NULL, "Direct native callee entry is incompatible");
		return result;
	}
#ifdef ZEND_CHECK_STACK_LIMIT
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		return result;
	}
#endif
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->source_position];
	switch (descriptor->receiver_kind) {
		case ZEND_NATIVE_INTERNAL_RECEIVER_NONE:
			if (function->common.scope != NULL) {
				zend_throw_error(NULL,
					"Direct native function unexpectedly requires a receiver");
				return result;
			}
			break;
		case ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS:
			if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0
					|| function->common.scope == NULL
					|| Z_TYPE(caller->This) != IS_OBJECT
					|| !instanceof_function(
						Z_OBJCE(caller->This), function->common.scope)) {
				zend_throw_error(NULL,
					"Direct native method has no compatible caller receiver");
				return result;
			}
			call_info |= ZEND_CALL_HAS_THIS;
			object_or_called_scope = Z_OBJ(caller->This);
			break;
		case ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT: {
			zval *source_receiver = zend_native_direct_operand(
				caller, &descriptor->receiver_operand, false);
			zval *receiver = source_receiver;
			bool consume_receiver =
				(descriptor->flags
					& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0;

			if (receiver != NULL) {
				ZVAL_DEREF(receiver);
			}
			if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0
					|| function->common.scope == NULL
					|| receiver == NULL || Z_TYPE_P(receiver) != IS_OBJECT
					|| !instanceof_function(
						Z_OBJCE_P(receiver), function->common.scope)) {
				zend_throw_error(NULL,
					"Direct native method has an incompatible receiver");
				if (consume_receiver && source_receiver != NULL
						&& !Z_ISUNDEF_P(source_receiver)) {
					zval_ptr_dtor(source_receiver);
					ZVAL_UNDEF(source_receiver);
				}
				return result;
			}
			GC_ADDREF(Z_OBJ_P(receiver));
			call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
			object_or_called_scope = Z_OBJ_P(receiver);
			receiver_owned = true;
			if (consume_receiver) {
				zval_ptr_dtor(source_receiver);
				ZVAL_UNDEF(source_receiver);
			}
			break;
		}
		case ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE: {
			zend_class_entry *called_scope;
			bool inherit_called_scope =
				(descriptor->flags
					& ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE) != 0;

			if ((descriptor->flags
					& ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER) != 0
					|| (function->common.fn_flags & ZEND_ACC_STATIC) == 0
					|| function->common.scope == NULL
					|| (inherit_called_scope
						&& descriptor->called_scope != NULL)) {
				zend_throw_error(NULL,
					"Direct native static method has an invalid scope");
				return result;
			}
			called_scope = inherit_called_scope
				? zend_get_called_scope(caller) : descriptor->called_scope;
			if (called_scope == NULL
					|| !instanceof_function(
						called_scope, function->common.scope)) {
				zend_throw_error(NULL,
					"Direct native static method has an incompatible scope");
				return result;
			}
			if (UNEXPECTED(function->common.scope->ce_flags & ZEND_ACC_TRAIT)) {
				zend_error(E_DEPRECATED,
					"Calling static trait method %s::%s is deprecated, "
					"it should only be called on a class using the trait",
					ZSTR_VAL(function->common.scope->name),
					ZSTR_VAL(function->common.function_name));
				if (EG(exception) != NULL) {
					return result;
				}
			}
			object_or_called_scope = called_scope;
			break;
		}
		default:
			zend_throw_error(NULL, "Invalid direct native method receiver");
			return result;
	}
	trivial_frame =
		(descriptor->flags & ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME) != 0
		&& descriptor->frame_size
			== zend_vm_calc_used_stack(
				descriptor->frame_argument_count, function)
		&& function->op_array.scope == NULL
		&& function->op_array.num_args == descriptor->frame_argument_count
		&& function->op_array.required_num_args
			== descriptor->frame_argument_count
		&& function->op_array.last_var == function->op_array.num_args
		&& function->op_array.T == 0
		&& (function->op_array.fn_flags
			& (ZEND_ACC_VARIADIC | ZEND_ACC_CALL_VIA_TRAMPOLINE)) == 0;
	for (index = 0;
			trivial_frame && index < function->op_array.num_args; index++) {
		trivial_frame = function->op_array.arg_info == NULL
			|| !ZEND_TYPE_IS_SET(function->op_array.arg_info[index].type);
	}
	used_stack = trivial_frame
		? descriptor->frame_size
		: zend_vm_calc_used_stack(
			descriptor->frame_argument_count, function);
	activation_size = (uint32_t) (
		(sizeof(zend_native_direct_activation) + sizeof(zval) - 1)
			/ sizeof(zval) * sizeof(zval));
	if (used_stack > UINT32_MAX - activation_size) {
		if (receiver_owned) {
			OBJ_RELEASE((zend_object *) object_or_called_scope);
		}
		zend_throw_error(NULL, "Direct native call frame is too large");
		return result;
	}
	call = zend_vm_stack_push_call_frame_ex(
		used_stack + activation_size, call_info,
		function, descriptor->frame_argument_count, object_or_called_scope);
	if ((ZEND_CALL_INFO(call) & ZEND_CALL_ALLOCATED) == 0) {
		/* Lead the callee frame so it ends at the stack top, where a tier-2
		 * host entry may grow it (zend_native_direct_activation_free_callee()). */
		activation = (zend_native_direct_activation *) call;
		call = (zend_execute_data *) ((char *) call + activation_size);
		zend_vm_init_call_frame(call, call_info, function,
			descriptor->frame_argument_count, object_or_called_scope);
	} else {
		/* A fresh VM-stack page must start with the frame it frees, and
		 * nothing may follow the frame. */
		activation = emalloc(sizeof(*activation));
	}
	memset(activation, 0, sizeof(*activation));
	activation->heap_allocated =
		(ZEND_CALL_INFO(call) & ZEND_CALL_ALLOCATED) != 0;
	activation->caller = caller;
	activation->callee = call;
	activation->pending_call = caller->call;
	activation->cell = cell;
	activation->code = code;
	activation->descriptor = descriptor;
	activation->previous = zend_native_active_direct_call;
	ZVAL_UNDEF(&activation->discarded_return);
	zend_native_active_direct_call = activation;
	caller->call = call;
	for (index = 0; index < descriptor->frame_argument_count; index++) {
		ZVAL_UNDEF(ZEND_CALL_ARG(call, index + 1));
	}
	activation->raw_arguments_owned = true;
	for (index = 0; index < descriptor->argument_count; index++) {
		if (zend_native_direct_transfer_argument(
				caller, call, &descriptor->arguments[index]) == FAILURE) {
			goto finish;
		}
	}
	if (!zend_native_call_preflight(caller, function)) {
		goto finish;
	}
	if (descriptor->result_operand.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		activation->uses_discarded_return = true;
		return_value = &activation->discarded_return;
	} else {
		return_value = zend_native_direct_operand(
			caller, &descriptor->result_operand, false);
		if (return_value == NULL) {
			zend_throw_error(NULL, "Invalid direct native call result slot");
			goto finish;
		}
		ZVAL_UNDEF(return_value);
	}
	if (trivial_frame) {
		if (UNEXPECTED(!RUN_TIME_CACHE(&function->op_array))) {
			zend_init_func_run_time_cache(&function->op_array);
		}
		call->opline =
			function->op_array.opcodes + descriptor->frame_argument_count;
		call->call = NULL;
		call->return_value = return_value;
		call->prev_execute_data = caller;
		call->symbol_table = NULL;
		call->run_time_cache = RUN_TIME_CACHE(&function->op_array);
		call->extra_named_params = NULL;
		EG(current_execute_data) = call;
	} else {
		zend_init_func_execute_data(call, &function->op_array, return_value);
	}
	activation->raw_arguments_owned = false;
	activation->frame_initialized = true;
	if (cell->frame_probe != NULL) {
		cell->frame_probe(cell->frame_probe_context, caller, call);
	}
	zend_native_entry_cell_retain_active(cell);
	activation->cell_active = true;
	EG(current_execute_data) = call;
	if (context->observers_enabled) {
		result.entry = zend_native_call_direct_observed_entry;
	} else if (!trivial_frame
			&& zend_native_frame_prepare(call) == FAILURE) {
		zend_native_execution_cleanup_frame(call);
		activation->frame_initialized = false;
		goto finish;
	} else {
		activation->frame_requires_finish = true;
		result.entry = entry;
	}
	result.callee = call;
	return result;

finish:
	zend_native_call_direct_release(activation);
	return result;
}

zend_native_direct_call_result zend_native_call_direct_leave(
	zend_execute_data *caller,
	const zend_native_direct_call_descriptor *descriptor,
	zend_native_execution_context *context,
	zend_native_status status)
{
	zend_native_direct_call_result result = {
		.status = ZEND_NATIVE_EXCEPTION,
		.payload = 0
	};
	zend_native_direct_activation *activation;
	zval *return_value;

	if (caller == NULL || descriptor == NULL || context == NULL
			|| context->active_direct_call == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid direct native call completion");
		}
		return result;
	}
	activation = (zend_native_direct_activation *)
		*context->active_direct_call;
	if (activation == NULL || activation->caller != caller
			|| activation->descriptor != descriptor) {
		/* Direct-enter failures happen before an activation can be published.
		 * Route their exception from the source call site exactly once instead
		 * of treating the absent callee frame as a corrupt completion. */
		if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL) {
			result.status = zend_native_prepare_finally_exception(
				caller, descriptor->source_position) == SUCCESS
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} else if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid direct native call completion");
		}
		return result;
	}
	if (activation->frame_requires_finish) {
		status = zend_native_execution_finish_direct_frame(
			activation->callee, status);
		activation->frame_requires_finish = false;
		activation->frame_initialized = false;
	}
	if (status == ZEND_NATIVE_GENERATOR_CREATED) {
		activation->generator_created = true;
		status = ZEND_NATIVE_RETURNED;
	}
	if (status == ZEND_NATIVE_BAILOUT) {
		result.status = status;
		zend_native_call_direct_abandon_activation(activation);
		return result;
	}
	return_value = activation->uses_discarded_return
		? &activation->discarded_return
		: zend_native_direct_operand(
			caller, &descriptor->result_operand, false);
	if (status == ZEND_NATIVE_RETURNED
			&& (descriptor->flags
				& ZEND_NATIVE_DIRECT_CALL_REQUIRE_SCALAR_RESULT) != 0
			&& descriptor->result_type != ZEND_MIR_SCALAR_TYPE_NONE
			&& (return_value == NULL || !zend_native_direct_scalar_payload(
				return_value, descriptor->result_type, &result.payload))) {
		zend_throw_error(
			NULL, "Native callee violated its exact scalar result contract");
		status = ZEND_NATIVE_EXCEPTION;
	}
	if (status == ZEND_NATIVE_BAILOUT) {
		result.status = status;
		zend_native_call_direct_abandon_activation(activation);
		return result;
	}
	/* The completed callee remains published in caller->call until release.
	 * Zend's unfinished-execution cleanup interprets that field as pending call
	 * setup, so restore the caller before selecting its catch/finally route. */
	zend_native_call_direct_release(activation);
	/* Releasing $this or the closure may run a destructor that throws, as
	 * the VM's leave reports it to the call, whose result then dies as
	 * ZEND_HANDLE_EXCEPTION frees the throwing opline's result. */
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		status = ZEND_NATIVE_EXCEPTION;
		if (return_value != NULL) {
			zval_ptr_dtor_nogc(return_value);
			ZVAL_UNDEF(return_value);
		}
	}
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL) {
		status = zend_native_prepare_finally_exception(
			caller, descriptor->source_position) == SUCCESS
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	}
	result.status = status;
	return result;
}

bool zend_native_call_normalize_trampoline(
	zend_execute_data *call, zend_function *expected_target)
{
	zend_function *trampoline;
	zend_function *target;
	zend_array *arguments = NULL;
	uint32_t argument_count;
	uint32_t call_info;

	if ((call->func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0
			|| call->func->type != ZEND_USER_FUNCTION) {
		return true;
	}
	if (call->func->op_array.scope == NULL) {
		zend_throw_error(NULL, "Native call trampoline is malformed");
		return false;
	}
	trampoline = call->func;
	target = (trampoline->op_array.fn_flags & ZEND_ACC_STATIC) != 0
		? trampoline->op_array.scope->__callstatic
		: trampoline->op_array.scope->__call;
	if (target == NULL || (expected_target != NULL
			&& target != expected_target)) {
		zend_throw_error(NULL, "Native magic method target is missing");
		return false;
	}
	argument_count = ZEND_CALL_NUM_ARGS(call);
	call_info = ZEND_CALL_INFO(call);
	if (argument_count != 0) {
		zval *argument = ZEND_CALL_ARG(call, 1);
		zval *end = argument + argument_count;

		arguments = zend_new_array(argument_count);
		zend_hash_real_init_packed(arguments);
		ZEND_HASH_FILL_PACKED(arguments) {
			do {
				ZEND_HASH_FILL_ADD(argument);
				argument++;
			} while (argument != end);
		} ZEND_HASH_FILL_END();
	}
	call->func = target;
	ZEND_CALL_NUM_ARGS(call) = 2;
	ZVAL_STR(ZEND_CALL_ARG(call, 1), trampoline->common.function_name);
	if (arguments != NULL) {
		ZVAL_ARR(ZEND_CALL_ARG(call, 2), arguments);
	} else {
		ZVAL_EMPTY_ARRAY(ZEND_CALL_ARG(call, 2));
	}
	if ((call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
		zval *packed = ZEND_CALL_ARG(call, 2);

		if (zend_hash_num_elements(Z_ARRVAL_P(packed)) == 0) {
			GC_ADDREF(call->extra_named_params);
			ZVAL_ARR(packed, call->extra_named_params);
		} else {
			SEPARATE_ARRAY(packed);
			zend_hash_copy(Z_ARRVAL_P(packed), call->extra_named_params,
				zval_add_ref);
		}
	}
	zend_free_trampoline(trampoline);
	return true;
}

zend_native_user_call_resolution_status
zend_native_call_normalize_user_resolution(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_execute_data *callee,
	zend_native_user_call_resolution *resolution)
{
	zend_function *normalized;

	if (caller == NULL || caller->func == NULL || descriptor == NULL
			|| callee == NULL || resolution == NULL
			|| caller->call != callee
			|| !ZEND_USER_CODE(caller->func->type)
			|| descriptor->do_source_position >= caller->func->op_array.last
			|| resolution->target_kind
				!= ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE
			|| (resolution->ownership
				& ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE) == 0
			|| resolution->function == NULL
			|| callee->func != resolution->function
			|| resolution->normalized_function == NULL
			|| (callee->func->common.fn_flags
				& ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Invalid native magic-call normalization state");
		}
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	normalized = resolution->normalized_function;
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->do_source_position];
	EG(current_execute_data) = caller;
	if (!zend_native_call_preflight(caller, callee->func)) {
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	resolution->placement_flags |=
		ZEND_NATIVE_USER_CALL_PLACEMENTS_METADATA_PREFLIGHT;
	if (!zend_native_call_normalize_trampoline(callee, normalized)) {
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	resolution->ownership &= ~ZEND_NATIVE_USER_CALL_OWNS_TRAMPOLINE;
	resolution->function = normalized;
	resolution->normalized_function = NULL;
	resolution->argument_count = 2;
	if (normalized->type == ZEND_USER_FUNCTION) {
		if (resolution->entry_cell == NULL || resolution->code == NULL
				|| resolution->frame_entry == NULL) {
			zend_throw_error(NULL,
				"Normalized native user target is not ready");
			return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
		}
		resolution->target_kind =
			ZEND_NATIVE_USER_CALL_TARGET_NATIVE_USER;
		resolution->invoke_entry = resolution->frame_entry;
	} else if (normalized->type == ZEND_INTERNAL_FUNCTION) {
		resolution->target_kind = ZEND_NATIVE_USER_CALL_TARGET_INTERNAL;
		resolution->invoke_entry = zend_native_call_dynamic_internal_entry;
	} else {
		zend_throw_error(NULL, "Native magic method target is invalid");
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	return ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS;
}

static zend_native_status zend_native_call_dynamic_completed_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context)
{
	(void) execute_data;
	(void) context;
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

static zend_native_status zend_native_call_dynamic_internal_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context)
{
	zend_native_direct_activation *activation;
	zend_native_status status = ZEND_NATIVE_EXCEPTION;

	if (context == NULL || context->active_direct_call == NULL
			|| (activation = (zend_native_direct_activation *)
				*context->active_direct_call) == NULL
			|| activation->callee != execute_data
			|| !activation->dynamic_target
			|| !activation->internal_target
			|| execute_data->func == NULL
			|| execute_data->func->type != ZEND_INTERNAL_FUNCTION) {
		zend_throw_error(NULL, "Invalid dynamic internal native call");
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->prev_execute_data = activation->caller;
	/* An exception before the handler leaves no result
	 * (zend_native_call_direct_release() destroys a defined one). */
	if (execute_data->return_value != NULL) {
		ZVAL_UNDEF(execute_data->return_value);
	}
	if ((activation->resolution.placement_flags
			& ZEND_NATIVE_USER_CALL_PLACEMENTS_METADATA_PREFLIGHT) == 0) {
		EG(current_execute_data) = activation->caller;
		if (!zend_native_call_preflight(
				activation->caller, execute_data->func)) {
			goto complete;
		}
		activation->resolution.placement_flags |=
			ZEND_NATIVE_USER_CALL_PLACEMENTS_METADATA_PREFLIGHT;
	}
	EG(current_execute_data) = execute_data;
	if (EG(exception) != NULL) {
		goto complete;
	}
	if ((ZEND_CALL_INFO(execute_data) & ZEND_CALL_MAY_HAVE_UNDEF) != 0
			&& zend_handle_undef_args(execute_data) == FAILURE) {
		goto complete;
	}
	/* Internal handlers own parameter parsing and its observable error order.
	 * This also covers fake Closure::fromCallable() handlers, whose temporary
	 * function metadata intentionally advertises no ordinary parameters while
	 * the handler accepts and forwards the raw arguments. */
	#if ZEND_DEBUG
	bool should_throw = zend_internal_call_should_throw(
		execute_data->func, execute_data);
	#endif
	ZEND_OBSERVER_FCALL_BEGIN(execute_data);
	ZVAL_NULL(execute_data->return_value);
	if (EXPECTED(zend_execute_internal == NULL)) {
		execute_data->func->internal_function.handler(
			execute_data, execute_data->return_value);
	} else {
		zend_execute_internal(execute_data, execute_data->return_value);
	}
	#if ZEND_DEBUG
	if (EG(exception) == NULL && execute_data->func != NULL
			&& (execute_data->func->common.fn_flags
				& ZEND_ACC_FAKE_CLOSURE) == 0) {
		if (should_throw) {
			zend_internal_call_arginfo_violation(execute_data->func);
		}
		if ((execute_data->func->common.fn_flags
				& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
			bool valid = zend_verify_internal_return_type(
				execute_data->func, execute_data->return_value);
			ZEND_ASSERT(valid);
		}
		ZEND_ASSERT((execute_data->func->common.fn_flags
				& ZEND_ACC_RETURN_REFERENCE) != 0
			? Z_ISREF_P(execute_data->return_value)
			: !Z_ISREF_P(execute_data->return_value));
	}
	#endif
	status = EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
	ZEND_OBSERVER_FCALL_END(execute_data,
		status == ZEND_NATIVE_RETURNED
			? execute_data->return_value : NULL);
	if (UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(execute_data);
		if (EG(exception) != NULL) {
			status = ZEND_NATIVE_EXCEPTION;
		}
	}
complete:
	EG(current_execute_data) = activation->caller;
	return status;
}

zend_native_direct_call_result zend_native_call_direct(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_direct_call_descriptor *descriptor,
	zend_native_execution_context *context)
{
	zend_native_direct_call_entry invocation =
		zend_native_call_direct_enter(caller, cell, descriptor, context);

	if (invocation.callee == NULL || invocation.entry == NULL) {
		zend_native_direct_call_result result = {
			.status = ZEND_NATIVE_EXCEPTION,
			.payload = 0
		};
		return result;
	}
	return zend_native_call_direct_leave(
		caller, descriptor, context,
		invocation.entry(invocation.callee, context));
}

static bool zend_native_frame_descends_from(
	zend_execute_data *frame, zend_execute_data *ancestor)
{
	while (frame != NULL && frame != ancestor) {
		frame = frame->prev_execute_data;
	}
	return frame == ancestor;
}

void zend_native_call_direct_unwind(zend_execute_data *outermost)
{
	/*
	 * An observed direct callee owns its own C bailout boundary.  Its active
	 * call record, however, is owned by the generated caller and remains live
	 * until the callee returns a bailout status to that caller.  Unwind only
	 * activations nested below this boundary; freeing the activation whose
	 * callee is outermost would release the very frame whose observer and
	 * compiled variables this boundary still has to finish.
	 */
	while (zend_native_active_direct_call != NULL
			&& zend_native_active_direct_call->callee != outermost
			&& zend_native_frame_descends_from(
				zend_native_active_direct_call->caller, outermost)) {
		zend_native_direct_activation *activation =
			zend_native_active_direct_call;
		zend_native_call_direct_release(activation);
	}
}

static void zend_native_call_direct_abandon_activation(
	zend_native_direct_activation *activation)
{
	zend_execute_data *setup_frame = activation->setup_frame;
	zend_execute_data *callee = activation->callee;
	zend_execute_data *caller = activation->caller;
	zend_execute_data *pending_call = activation->pending_call;
	zend_native_direct_activation *previous = activation->previous;
	bool setup_record = activation->setup_record;

	ZEND_ASSERT(zend_native_active_direct_call == activation);
	/* A fatal bailout follows Zend's unclean-shutdown path. In particular, do
	 * not destroy frame values, call targets, pending arguments, or resolution
	 * retains here: bailout may have interrupted GC after it lowered a live
	 * value's refcount to zero. Only non-observable bookkeeping is safe. */
	if (activation->cell_active && activation->cell != NULL) {
		zend_native_entry_cell_release_active(activation->cell);
	}
	caller->call = pending_call;
	EG(current_execute_data) = caller;
	zend_native_active_direct_call = previous;
	if (callee != NULL) {
		zend_native_direct_activation_free_callee(activation, callee);
	}
	if (setup_record && setup_frame != NULL) {
		zend_vm_stack_free_call_frame(setup_frame);
	}
}

void zend_native_call_direct_abandon(zend_execute_data *outermost)
{
	/* As with normal unwind, the activation whose callee is outermost belongs
	 * to the generated caller outside this catcher. Discard only descendants. */
	while (zend_native_active_direct_call != NULL
			&& zend_native_active_direct_call->callee != outermost
			&& zend_native_frame_descends_from(
				zend_native_active_direct_call->caller, outermost)) {
		zend_native_direct_activation *activation =
			zend_native_active_direct_call;
		zend_execute_data *caller = activation->caller;

		zend_native_call_direct_abandon_activation(activation);
		if (caller == outermost) {
			break;
		}
	}
}

uint64_t zend_native_call_invoke_finish(
	zend_execute_data *caller, zend_native_entry_cell *cell)
{
	uint64_t payload_bits = 0;
	zval return_value;
	zend_native_status status = zend_native_call_invoke(
		caller, cell, &return_value);

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
	if (status != ZEND_NATIVE_RETURNED) {
		zend_bailout();
	}
	return payload_bits;
}


zend_native_status zend_native_call_invoke_finish_source(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor)
{
	const zend_op *opline;
	zval temporary;
	zval *return_value;
	uint8_t result_operand_type;
	zend_native_status status;

	if (caller == NULL || descriptor == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| descriptor->do_source_position
				>= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &caller->func->op_array.opcodes[
		descriptor->do_source_position];
	if (descriptor->do_opcode != ZEND_DO_UCALL
			&& descriptor->do_opcode != ZEND_DO_FCALL
			&& descriptor->do_opcode != ZEND_DO_FCALL_BY_NAME
			&& descriptor->do_opcode != ZEND_DO_ICALL
			&& descriptor->do_opcode != ZEND_CALLABLE_CONVERT
			&& descriptor->do_opcode != ZEND_CALLABLE_CONVERT_PARTIAL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT
			|| descriptor->do_opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
		return zend_native_call_convert_explicit(
			caller,
			descriptor->do_opcode,
			descriptor->do_op1_payload,
			zend_native_encode_source_operand(caller, &descriptor->do_op2),
			zend_native_encode_source_operand(caller, &descriptor->do_result),
			descriptor->do_extended_value,
			descriptor->do_source_position);
	}
	if (EG(exception) != NULL) {
		ZVAL_UNDEF(&temporary);
		return zend_native_call_invoke(caller, cell, &temporary);
	}
	caller->opline = opline;
	return_value = zend_native_direct_operand(
		caller, &descriptor->do_result, false);
	result_operand_type = descriptor->do_result.kind
			== ZEND_MIR_SOURCE_OPERAND_UNUSED
		? IS_UNUSED
		: descriptor->do_result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			? IS_CV
			: descriptor->do_result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
				? IS_VAR : IS_TMP_VAR;
	if (result_operand_type == IS_UNUSED) {
		ZVAL_UNDEF(&temporary);
		return_value = &temporary;
	} else if (return_value != NULL) {
		ZVAL_UNDEF(return_value);
	} else {
		return ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_call_invoke(caller, cell, return_value);
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL) {
		status = zend_native_prepare_finally_exception(
			caller, descriptor->do_source_position) == SUCCESS
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	}
	if (status != ZEND_NATIVE_RETURNED || return_value == &temporary) {
		if (!Z_ISUNDEF_P(return_value)) {
			zval_ptr_dtor(return_value);
			ZVAL_UNDEF(return_value);
		}
	}
	return status;
}


zend_native_status zend_native_call_convert_explicit(
	zend_execute_data *caller,
	uint32_t source_opcode,
	uint32_t op1_payload,
	uint64_t encoded_op2,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_position)
{
	zend_mir_source_operand_ref op2;
	zend_mir_source_operand_ref result;
	zend_execute_data *call;
	zend_execute_data *pending_call;
	zend_op_array *op_array;
	const zend_op *opline;
	zval temporary;
	zval *return_value;
	uint8_t result_operand_type;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| (source_opcode != ZEND_CALLABLE_CONVERT
				&& source_opcode != ZEND_CALLABLE_CONVERT_PARTIAL)
			|| !zend_native_decode_source_operand(caller, encoded_op2, &op2)
			|| !zend_native_decode_source_operand(caller, encoded_result, &result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	op_array = &caller->func->op_array;
	if (source_position >= op_array->last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &op_array->opcodes[source_position];
	call = caller->call;
	if (call == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	pending_call = call->prev_execute_data;
	if (EG(exception) != NULL) {
		zend_vm_stack_free_args(call);
		if ((ZEND_CALL_INFO(call)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_free_extra_named_params(call->extra_named_params);
		}
		zend_native_call_release_target(call);
		zend_vm_stack_free_call_frame(call);
		caller->call = pending_call;
		return ZEND_NATIVE_EXCEPTION;
	}
	caller->opline = opline;
	return_value = zend_native_direct_operand(caller, &result, false);
	result_operand_type = result.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED
		? IS_UNUSED
		: result.slot_kind == ZEND_MIR_SOURCE_SLOT_CV
			? IS_CV
			: result.slot_kind == ZEND_MIR_SOURCE_SLOT_VAR
				? IS_VAR : IS_TMP_VAR;
	if (result_operand_type == IS_UNUSED) {
		ZVAL_UNDEF(&temporary);
		return_value = &temporary;
	} else if (return_value != NULL) {
		ZVAL_UNDEF(return_value);
	} else {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (source_opcode == ZEND_CALLABLE_CONVERT) {
		if (extended_value != UINT32_MAX) {
			zend_object *closure;
			void **cache_slot;

			if (caller->run_time_cache == NULL
					|| extended_value > op_array->cache_size
					|| sizeof(void *)
						> op_array->cache_size - extended_value) {
				zend_throw_error(NULL,
					"Malformed native callable conversion cache offset");
				zend_vm_stack_free_call_frame(call);
				caller->call = pending_call;
				return ZEND_NATIVE_EXCEPTION;
			}
			cache_slot = (void **) (
				(char *) caller->run_time_cache + extended_value);
			closure = (zend_object *) *cache_slot;
			if (closure != NULL) {
				ZVAL_OBJ_COPY(return_value, closure);
			} else {
				const int shift = sizeof(size_t) == 4 ? 6 : 7;
				zend_ulong key = (zend_ulong) (uintptr_t) call->func;
				zval *closure_zv;

				key = (key >> shift)
					| (key << ((sizeof(key) * 8) - shift));
				closure_zv = zend_hash_index_lookup(
					&EG(callable_convert_cache), key);
				if (Z_TYPE_P(closure_zv) == IS_NULL) {
					zend_closure_from_frame(closure_zv, call);
				}
				ZEND_ASSERT(Z_TYPE_P(closure_zv) == IS_OBJECT);
				closure = Z_OBJ_P(closure_zv);
				ZVAL_OBJ_COPY(return_value, closure);
				*cache_slot = closure;
			}
		} else {
			zend_closure_from_frame(return_value, call);
		}
		if ((ZEND_CALL_INFO(call) & ZEND_CALL_RELEASE_THIS) != 0) {
			OBJ_RELEASE(Z_OBJ(call->This));
		}
		zend_vm_stack_free_call_frame(call);
		caller->call = pending_call;
		return ZEND_NATIVE_RETURNED;
	}
	{
		/* Mirrors ZEND_CALLABLE_CONVERT_PARTIAL: op1 is the PFA name literal,
		 * extended_value carries the cache slot offset plus
		 * ZEND_PARTIAL_FLAGS, and const_args lives in op2.num when op2 is
		 * unused or in the named-positions literal's Z_EXTRA otherwise.
		 * Read extended_value from the live opline: OPcache adds
		 * ZEND_PARTIAL_CACHEABLE_IN_SHM when it persists the script, after
		 * the native descriptor captured its copy. */
		const uint32_t partial_extended_value = opline->extended_value;
		uint32_t cache_offset = partial_extended_value & ~ZEND_PARTIAL_FLAGS;
		void **cache_slot;
		zval *pfa_name;
		zval *named_positions = NULL;
		uint32_t const_args;
		uint32_t call_info;

		if (caller->run_time_cache == NULL
				|| cache_offset > op_array->cache_size
				|| 2 * sizeof(void *) > op_array->cache_size - cache_offset
				|| opline->opcode != ZEND_CALLABLE_CONVERT_PARTIAL
				|| opline->op1_type != IS_CONST) {
			return ZEND_NATIVE_EXCEPTION;
		}
		pfa_name = RT_CONSTANT(opline, opline->op1);
		if (Z_TYPE_P(pfa_name) != IS_STRING) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			named_positions = zend_native_direct_operand(caller, &op2, true);
			if (named_positions == NULL
					|| Z_TYPE_P(named_positions) != IS_ARRAY) {
				return ZEND_NATIVE_EXCEPTION;
			}
			const_args = Z_EXTRA_P(named_positions);
		} else if (op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			const_args = opline->op2.num;
		} else {
			return ZEND_NATIVE_EXCEPTION;
		}
		cache_slot = (void **) ((char *) caller->run_time_cache + cache_offset);
		call_info = ZEND_CALL_INFO(call);
		zend_partial_create(return_value,
			op_array->scope, &call->This, call->func,
			ZEND_CALL_NUM_ARGS(call), ZEND_CALL_ARG(call, 1),
			(call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0
				? call->extra_named_params : NULL,
			named_positions != NULL ? Z_ARRVAL_P(named_positions) : NULL,
			op_array->filename, &opline->lineno, cache_slot,
			Z_STR_P(pfa_name), partial_extended_value & ZEND_PARTIAL_FLAGS,
			const_args);
		if ((call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_array_release(call->extra_named_params);
		}
		if ((call->func->common.fn_flags
				& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
			zend_free_trampoline(call->func);
		}
		if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
			OBJ_RELEASE(Z_OBJ(call->This));
		} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
			OBJ_RELEASE(ZEND_CLOSURE_OBJECT(call->func));
		}
		zend_vm_stack_free_call_frame(call);
		caller->call = pending_call;
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_call_convert_descriptor_explicit(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	const zend_native_user_call_descriptor *descriptor)
{
	uint32_t index;

	if (caller == NULL || cell == NULL || descriptor == NULL
			|| (descriptor->do_opcode != ZEND_CALLABLE_CONVERT
				&& descriptor->do_opcode != ZEND_CALLABLE_CONVERT_PARTIAL)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_native_call_begin(caller, cell, descriptor);
	for (index = 0; index < descriptor->argument_count
			&& EG(exception) == NULL; index++) {
		if (zend_native_call_set_source_argument(
				caller, descriptor, index) == FAILURE) {
			if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Invalid native callable conversion argument");
			}
			break;
		}
	}
	return zend_native_call_invoke_finish_source(caller, cell, descriptor);
}

/*
 * The universal Init of a source call site, out of line: reserve the setup
 * frame and its activation, make the activation active, resolve the target
 * and push the callee frame, as the generated universal Init did inline.
 * result_offset is the result slot's frame offset, or UINT32_MAX for a
 * discarded result. On failure nothing stays active and the status is
 * returned with the exception set.
 */
uint32_t zend_native_call_universal_init(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_native_entry_cell *entry_cell_hint,
	uint32_t setup_size,
	uint32_t argument_count,
	uint32_t result_offset)
{
	const size_t header_size = ZEND_CALL_FRAME_SLOT * sizeof(zval);
	const size_t activation_offset = ZEND_MM_ALIGNED_SIZE_EX(header_size,
		_Alignof(zend_native_direct_activation));
	const size_t placement_offset =
		activation_offset + sizeof(zend_native_direct_activation);
	zend_native_direct_activation *activation;
	zend_execute_data *setup;
	zend_execute_data *callee;
	uint32_t status;
	uint32_t frame_size;
	uint32_t call_info;
	bool allocated = false;

	setup = zend_native_frame_activation_reserve(setup_size);
	if (setup == NULL) {
		return ZEND_NATIVE_USER_CALL_RESOLUTION_FAILURE;
	}
	activation = (zend_native_direct_activation *)
		((char *) setup + activation_offset);
	memset(activation, 0, sizeof(*activation));
	activation->setup_frame = setup;
	activation->caller = caller;
	activation->pending_call = caller->call;
	activation->descriptor = descriptor;
	activation->setup_size = setup_size;
	activation->placement_capacity = argument_count;
	activation->resolution.placements = (zend_native_user_call_placement *)
		((char *) setup + placement_offset);
	activation->setup_record = true;
	activation->previous = zend_native_active_direct_call;
	zend_native_active_direct_call = activation;
	status = zend_native_call_resolve_user(activation, entry_cell_hint,
		ZEND_NATIVE_USER_CALL_ARGUMENT_COUNT_AUTO);
	if (status != ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS) {
		if (activation->resolution.ownership != 0) {
			zend_native_call_release_user_resolution(&activation->resolution);
		}
		zend_native_active_direct_call = activation->previous;
		if ((ZEND_CALL_INFO(setup) & ZEND_CALL_ALLOCATED) == 0) {
			activation->setup_record = false;
			EG(vm_stack_top) = (zval *) setup;
		} else {
			zend_native_frame_activation_pop(activation);
		}
		return status;
	}
	frame_size = activation->resolution.frame_size;
	callee = (zend_execute_data *) EG(vm_stack_top);
	if ((size_t) ((char *) EG(vm_stack_end) - (char *) callee) >= frame_size) {
		EG(vm_stack_top) = (zval *) ((char *) callee + frame_size);
	} else {
		callee = zend_native_call_reserve_dynamic_frame(caller, frame_size);
		allocated = true;
	}
	memset(callee, 0, header_size);
	callee->This.value.ptr = activation->resolution.object_or_called_scope;
	call_info = activation->resolution.call_info;
	if (allocated) {
		call_info |= ZEND_CALL_ALLOCATED;
	}
	ZEND_CALL_INFO(callee) = call_info;
	ZEND_CALL_NUM_ARGS(callee) = activation->resolution.argument_count;
	callee->func = activation->resolution.function;
	callee->prev_execute_data = activation->pending_call;
	callee->return_value = result_offset == UINT32_MAX
		? &activation->discarded_return
		: (zval *) ((char *) caller + result_offset);
	activation->callee = callee;
	caller->call = callee;
	activation->raw_arguments_owned = true;
	activation->uses_discarded_return = result_offset == UINT32_MAX;
	activation->dynamic_target = true;
	for (uint32_t index = 0; index < activation->resolution.argument_count;
			index++) {
		Z_TYPE_INFO_P(ZEND_CALL_ARG(callee, index + 1)) = IS_UNDEF;
	}
	return ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS;
}

/*
 * The universal Do of a source call site, out of line: normalize a
 * trampoline, then call the internal target's entry or invoke the user
 * target, releasing the activation as the generated universal Do did,
 * also when the call fails. Returns ZEND_NATIVE_RETURNED, or
 * ZEND_NATIVE_EXCEPTION with the exception pending.
 */
uint32_t zend_native_call_universal_do(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_native_execution_context *context)
{
	zend_native_direct_activation *activation = zend_native_active_direct_call;
	zend_native_user_call_resolution *resolution = &activation->resolution;

	if (resolution->target_kind == ZEND_NATIVE_USER_CALL_TARGET_NO_CALL) {
		zend_native_frame_activation_release(activation);
		return EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_RETURNED;
	}
	if (resolution->target_kind == ZEND_NATIVE_USER_CALL_TARGET_TRAMPOLINE
			&& zend_native_call_normalize_user_resolution(caller, descriptor,
				activation->callee, resolution)
				!= ZEND_NATIVE_USER_CALL_RESOLUTION_SUCCESS) {
		zend_native_frame_activation_release(activation);
		return ZEND_NATIVE_EXCEPTION;
	}
	if (resolution->target_kind == ZEND_NATIVE_USER_CALL_TARGET_INTERNAL) {
		activation->internal_target = true;
		if (resolution->invoke_entry(activation->callee, context)
				!= ZEND_NATIVE_RETURNED) {
			zend_native_frame_activation_release(activation);
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_native_frame_activation_release(activation);
	} else if (zend_native_call_invoke_user(activation, context) != 0) {
		zend_native_frame_activation_release(activation);
		return ZEND_NATIVE_EXCEPTION;
	}
	return EG(exception) != NULL
		? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_RETURNED;
}

/*
 * The universal Send of a frame-held argument, out of line: a placement the
 * resolution expands at run time is sent through the activation, any other
 * through the source setter, as the generated universal Send did. On
 * failure the activation is released and FAILURE returned with the
 * exception pending.
 */
zend_result zend_native_call_universal_send(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t argument_index)
{
	zend_native_direct_activation *activation = zend_native_active_direct_call;
	zend_result status;

	if (activation != NULL
			&& argument_index < activation->resolution.placement_count
			&& activation->resolution.placements != NULL
			&& (activation->resolution.placements[argument_index].flags
				& ZEND_NATIVE_USER_CALL_PLACEMENT_RUNTIME_EXPANSION) != 0) {
		status = zend_native_call_send_resolved_argument(
			activation, argument_index);
	} else {
		status = zend_native_call_set_source_argument(
			caller, descriptor, argument_index);
	}
	if (status != SUCCESS && activation != NULL) {
		zend_native_frame_activation_release(activation);
	}
	return status;
}

/*
 * The fast Init of new C(...) whose site published C's constructor: create
 * the object in the result slot and push the constructor frame as ZEND_NEW
 * does. Returns false with nothing changed when the class could fail to
 * instantiate or no longer has that constructor.
 */
bool zend_native_call_fast_new(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t result_offset)
{
	const zend_native_user_call_site_header *header =
		ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);
	zend_class_entry *ce = (zend_class_entry *) header->fast_key;
	zval *result = (zval *) ((char *) caller + result_offset);
	zend_execute_data *call;
	zend_object *object;

	if (header->fast_epoch != zend_native_call_resolution_cache_epoch
			&& !zend_native_call_fast_rearm(caller, descriptor)) {
		return false;
	}
	if (header->fast_function == NULL) {
		/* No constructor: only the object, no frame. */
		if ((header->fast_flags & ZEND_NATIVE_CALL_FAST_NO_CALL) == 0
				|| ce == NULL || ce->constructor != NULL
				|| ce->create_object != NULL
				|| !zend_native_call_class_constants_updated(ce)
				|| (ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT
					| ZEND_ACC_IMPLICIT_ABSTRACT_CLASS
					| ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_ENUM
					| ZEND_ACC_UNINSTANTIABLE)) != 0) {
			return false;
		}
		return object_init_ex(result, ce) == SUCCESS;
	}
	if (ce == NULL || ce->constructor != header->fast_function
			|| ce->create_object != NULL
			|| !zend_native_call_class_constants_updated(ce)
			|| (ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_TRAIT
				| ZEND_ACC_IMPLICIT_ABSTRACT_CLASS
				| ZEND_ACC_EXPLICIT_ABSTRACT_CLASS | ZEND_ACC_ENUM
				| ZEND_ACC_UNINSTANTIABLE)) != 0) {
		return false;
	}
	if (object_init_ex(result, ce) != SUCCESS) {
		return false;
	}
	object = Z_OBJ_P(result);
	call = zend_vm_stack_push_call_frame(header->fast_call_info,
		header->fast_function, descriptor->argument_count, object);
	GC_ADDREF(object);
	call->prev_execute_data = caller->call;
	caller->call = call;
	return true;
}

/*
 * A fast send of a VAR holding a reference to a by-value parameter, as
 * ZEND_SEND_VAR sends it: the referenced value moves into the argument and
 * the reference is released, freed when this was its last owner.
 */
void zend_native_call_fast_send_var_reference(zval *argument, zval *variable)
{
	zend_refcounted *reference = Z_COUNTED_P(variable);

	ZVAL_COPY_VALUE(argument, Z_REFVAL_P(variable));
	if (GC_DELREF(reference) == 0) {
		efree_size(reference, sizeof(zend_reference));
	} else if (Z_OPT_REFCOUNTED_P(argument)) {
		Z_ADDREF_P(argument);
	}
	ZVAL_UNDEF(variable);
}

/*
 * Extra positional arguments move behind the CVs and temporaries, as
 * zend_copy_extra_args() moves them for the VM.
 */
static void zend_native_call_fast_copy_extra_args(
	zend_execute_data *callee, const zend_op_array *op_array,
	uint32_t supplied)
{
	zval *source = ZEND_CALL_VAR_NUM(callee, supplied - 1);
	size_t delta = op_array->last_var + op_array->T - op_array->num_args;
	uint32_t count = supplied - op_array->num_args;
	uint32_t type_flags = 0;

	if (delta != 0) {
		delta *= sizeof(zval);
		do {
			type_flags |= Z_TYPE_INFO_P(source);
			ZVAL_COPY_VALUE((zval *) ((char *) source + delta), source);
			ZVAL_UNDEF(source);
			source--;
		} while (--count);
		if (Z_TYPE_INFO_REFCOUNTED(type_flags)) {
			ZEND_ADD_CALL_FLAG(callee, ZEND_CALL_FREE_EXTRA_ARGS);
		}
	} else {
		do {
			if (Z_REFCOUNTED_P(source)) {
				ZEND_ADD_CALL_FLAG(callee, ZEND_CALL_FREE_EXTRA_ARGS);
				break;
			}
			source--;
		} while (--count);
	}
}

uint64_t zend_native_deopt_stress_reenter(zend_execute_data *execute_data,
	uint32_t position, zend_native_frame_entry_t entry, uint32_t disarmed)
{
#if ZEND_DEBUG
	static int64_t period = -1;
	static uint64_t countdown;
	zend_native_execution_context context;

	if (period < 0) {
		const char *setting = getenv("ZEND_NATIVE_DEOPT_STRESS");
		period = setting != NULL ? ZEND_STRTOL(setting, NULL, 10) : 0;
		if (period < 0) {
			period = 0;
		}
	}
	if (disarmed != 0 || period == 0 || ++countdown < (uint64_t) period) {
		return 0;
	}
	countdown = 0;
	ZEND_ASSERT(position < execute_data->func->op_array.last);
	execute_data->opline = execute_data->func->op_array.opcodes + position;
	/* The context holds request-global addresses only. */
	zend_native_execution_context_init(&context);
	context.deopt_resume = true;
	return (UINT64_C(1) << 32) | (uint32_t) entry(execute_data, &context);
#else
	(void) execute_data;
	(void) position;
	(void) entry;
	(void) disarmed;
	return 0;
#endif
}

zend_native_status zend_native_deopt_transfer(zend_execute_data *execute_data,
	uint32_t position, zend_native_frame_entry_t entry)
{
	zend_native_execution_context context;

	ZEND_ASSERT(position < execute_data->func->op_array.last);
	execute_data->opline = execute_data->func->op_array.opcodes + position;
	/* The context holds request-global addresses only. */
	zend_native_execution_context_init(&context);
	context.deopt_resume = true;
	return entry(execute_data, &context);
}

zend_native_status zend_native_deopt_speculation(
	zend_execute_data *execute_data, uint32_t position,
	zend_native_frame_entry_t entry, zend_native_entry_cell *cell)
{
	if (cell->tier2_deopts < ZEND_NATIVE_TIER2_DEOPT_LIMIT) {
		cell->tier2_deopts++;
	}
	return zend_native_deopt_transfer(execute_data, position, entry);
}

void zend_native_call_receive_variadic(zend_execute_data *callee)
{
	const zend_op_array *op_array = &callee->func->op_array;
	const uint32_t supplied = ZEND_CALL_NUM_ARGS(callee);
	const uint32_t declared = op_array->num_args;
	zval *parameters = ZEND_CALL_VAR_NUM(callee, declared);
	zval *argument;
	uint32_t count;

	if (supplied <= declared) {
		ZVAL_EMPTY_ARRAY(parameters);
		return;
	}
	zend_native_call_fast_copy_extra_args(callee, op_array, supplied);
	argument = ZEND_CALL_VAR_NUM(callee, op_array->last_var + op_array->T);
	count = supplied - declared;
	array_init_size(parameters, count);
	zend_hash_real_init_packed(Z_ARRVAL_P(parameters));
	ZEND_HASH_FILL_PACKED(Z_ARRVAL_P(parameters)) {
		do {
			ZEND_HASH_FILL_SET(argument);
			Z_TRY_ADDREF_P(argument);
			ZEND_HASH_FILL_NEXT();
			argument++;
		} while (--count);
	} ZEND_HASH_FILL_END();
}

/*
 * What zend_native_call_fast_prepare() does for a frame whose supplied
 * arguments need no check or coercion, whose missing parameters have
 * literal defaults and whose variadic parameter, if any, is untyped: the
 * frame initialization of i_init_func_execute_data() and the RECV,
 * RECV_INIT and RECV_VARIADIC of the parameters. Returns false with the
 * frame unchanged for anything else.
 */
static bool zend_native_call_fast_receive_frame(
	zend_execute_data *callee, const zend_native_call_fast_receive *receive,
	void **run_time_cache)
{
	const zend_op_array *op_array = &callee->func->op_array;
	const uint32_t supplied = ZEND_CALL_NUM_ARGS(callee);
	const uint32_t declared = op_array->num_args;
	const uint32_t received = MIN(supplied, declared);
	const bool variadic = (op_array->fn_flags & ZEND_ACC_VARIADIC) != 0;
	uint32_t index;

	if (receive == NULL
			|| receive->state != ZEND_NATIVE_CALL_FAST_RECEIVE_INLINE
			|| receive->num_args != declared || run_time_cache == NULL
			|| (ZEND_CALL_INFO(callee) & (ZEND_CALL_HAS_EXTRA_NAMED_PARAMS
				| ZEND_CALL_MAY_HAVE_UNDEF)) != 0
			|| (variadic
				&& ZEND_TYPE_IS_SET(op_array->arg_info[declared].type))) {
		return false;
	}
	for (index = 0; index < received; index++) {
		if ((receive->type_masks[index] & (UINT32_C(1) << Z_TYPE_P(
				ZEND_CALL_ARG(callee, index + 1)))) == 0) {
			return false;
		}
	}
	for (index = supplied; index < declared; index++) {
		if (receive->defaults[index] == NULL) {
			return false;
		}
	}
	callee->opline = op_array->opcodes + received;
	if (supplied > declared) {
		zend_native_call_fast_copy_extra_args(callee, op_array, supplied);
	}
	{
		zval *variable = ZEND_CALL_VAR_NUM(callee, supplied);
		zval *end = ZEND_CALL_VAR_NUM(callee, op_array->last_var);

		for (; variable < end; variable++) {
			ZVAL_UNDEF(variable);
		}
	}
	for (index = supplied; index < declared; index++) {
		ZVAL_COPY_VALUE(ZEND_CALL_ARG(callee, index + 1),
			receive->defaults[index]);
	}
	if (variadic) {
		zval *parameters = ZEND_CALL_VAR_NUM(callee, declared);

		if (supplied > declared) {
			zval *argument = ZEND_CALL_VAR_NUM(callee,
				op_array->last_var + op_array->T);
			uint32_t count = supplied - declared;

			array_init_size(parameters, count);
			zend_hash_real_init_packed(Z_ARRVAL_P(parameters));
			ZEND_HASH_FILL_PACKED(Z_ARRVAL_P(parameters)) {
				do {
					ZEND_HASH_FILL_SET(argument);
					Z_TRY_ADDREF_P(argument);
					ZEND_HASH_FILL_NEXT();
					argument++;
				} while (--count);
			} ZEND_HASH_FILL_END();
		} else {
			ZVAL_EMPTY_ARRAY(parameters);
		}
	}
	callee->run_time_cache = run_time_cache;
	EG(current_execute_data) = callee;
	return true;
}

/*
 * The fast Do of a call site whose Init pushed the frame (see
 * zend_native_user_call_site_header), out of line: unlink the frame from
 * EX(call), initialize it for its target as i_init_func_execute_data()
 * does, receive the parameters natively or through
 * zend_native_call_fast_prepare(), enter the target and leave through
 * zend_native_call_fast_leave(). dynamic_entry is the entry a dynamic
 * site's Init resolved, NULL for a published one; result_offset is the
 * result slot's frame offset, UINT32_MAX for a discarded result.
 */
uint32_t zend_native_call_fast_do(
	zend_execute_data *caller,
	zend_native_execution_context *context,
	const zend_native_user_call_descriptor *descriptor,
	zend_native_frame_entry_t dynamic_entry,
	uint32_t result_offset)
{
	const zend_native_user_call_site_header *header =
		ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);
	zend_execute_data *callee = caller->call;
	const uint32_t flags = dynamic_entry != NULL
		? ZEND_NATIVE_CALL_FAST_PREPARE : header->fast_flags;

	if ((flags & ZEND_NATIVE_CALL_FAST_NO_CALL) != 0) {
		/* new C without a constructor: the Init pushed no frame. */
		return ZEND_NATIVE_RETURNED;
	}
	if (UNEXPECTED(dynamic_entry == NULL && header->fast_ref_mask != 0)) {
		/* SEND_VAR_EX to by-reference parameters: the sends copied the
		 * CVs; pass references to them instead, as the VM's send makes
		 * them (an undefined CV becoming null). */
		uint32_t mask = header->fast_ref_mask;

		do {
			const uint32_t index = (uint32_t) __builtin_ctz(mask);
			zval *argument = ZEND_CALL_ARG(callee, index + 1);
			zval *variable = ZEND_CALL_VAR_NUM(caller,
				descriptor->arguments[index].source_operand.index);

			zval_ptr_dtor_nogc(argument);
			if (Z_ISREF_P(variable)) {
				Z_ADDREF_P(variable);
			} else {
				if (Z_ISUNDEF_P(variable)) {
					ZVAL_NULL(variable);
				}
				ZVAL_MAKE_REF_EX(variable, 2);
			}
			ZVAL_REF(argument, Z_REF_P(variable));
			mask &= mask - 1;
		} while (mask != 0);
	}
	const bool discard = result_offset == UINT32_MAX;
	zval discarded;
	uint32_t status = ZEND_NATIVE_RETURNED;

	caller->call = callee->prev_execute_data;
	callee->return_value = discard
		? &discarded : (zval *) ((char *) caller + result_offset);
	ZVAL_UNDEF(callee->return_value);
	callee->prev_execute_data = caller;
	callee->call = NULL;
	if ((flags & ZEND_NATIVE_CALL_FAST_PREPARE) != 0) {
		/* A dynamic site's Init left the target's receive in the run-time
		 * cache slot. */
		const bool received = dynamic_entry != NULL
			? zend_native_call_fast_receive_frame(callee,
				(const zend_native_call_fast_receive *)
					callee->run_time_cache,
				RUN_TIME_CACHE(&callee->func->op_array))
			: zend_native_call_fast_receive_frame(callee,
				header->fast_receive, header->fast_run_time_cache);

		if (!received) {
			status = zend_native_call_fast_prepare(callee);
		}
	} else {
		const uint32_t argument_count = descriptor->argument_count;
		const zend_op_array *op_array = &callee->func->op_array;
		const zend_native_call_fast_receive *receive = header->fast_receive;
		bool prepare = false;

		/* EX(opline) skips the RECV of every supplied parameter; the
		 * remaining CVs start undefined. */
		callee->opline = op_array->opcodes + argument_count;
		{
			zval *variable = ZEND_CALL_VAR_NUM(callee, argument_count);
			zval *end = ZEND_CALL_VAR_NUM(callee, op_array->last_var);

			for (; variable < end; variable++) {
				ZVAL_UNDEF(variable);
			}
		}
		callee->run_time_cache = header->fast_run_time_cache;
		EG(current_execute_data) = callee;
		if ((flags & ZEND_NATIVE_CALL_FAST_CHECK_ARGS) != 0) {
			for (uint32_t index = 0; index < argument_count; index++) {
				if ((receive->type_masks[index]
						& (UINT32_C(1) << Z_TYPE_P(
							ZEND_CALL_ARG(callee, index + 1)))) == 0) {
					prepare = true;
					break;
				}
			}
		}
		if (prepare) {
			status = zend_native_call_fast_prepare(callee);
		} else if ((flags & ZEND_NATIVE_CALL_FAST_DEFAULTS) != 0) {
			for (uint32_t index = 0; index < header->fast_default_count;
					index++) {
				ZVAL_COPY_VALUE(ZEND_CALL_ARG(callee,
						argument_count + index + 1),
					receive->defaults[argument_count + index]);
			}
		}
	}
	if (status == ZEND_NATIVE_RETURNED) {
		status = (dynamic_entry != NULL ? dynamic_entry : header->fast_entry)(
			callee, context);
	}
	return zend_native_call_fast_leave_inline(callee, status, discard);
}

/*
 * The universal Expand of a source call site, out of line: a resolution
 * that expands arguments at run time expands them into the callee frame,
 * as the generated universal Expand did. On failure the activation is
 * released and FAILURE returned with the exception pending.
 */
zend_result zend_native_call_universal_expand(void)
{
	zend_native_direct_activation *activation = zend_native_active_direct_call;

	if ((activation->resolution.placement_flags
				& ZEND_NATIVE_USER_CALL_PLACEMENTS_RUNTIME_EXPANSION) == 0
			|| zend_native_call_expand_user_arguments(activation) != NULL) {
		return SUCCESS;
	}
	zend_native_frame_activation_release(activation);
	return FAILURE;
}

/*
 * Whether the literal class name of a stale new C or C::m() site still
 * binds the immutable class the site was published for, without loading.
 */
static bool zend_native_call_rearm_class_binding(
	const zend_op_array *op_array,
	const zend_native_user_call_descriptor *descriptor,
	const zend_class_entry *key)
{
	const uint32_t index = descriptor->init_op1.index + 1;
	zval *bound;

	if (key == NULL || (key->ce_flags & ZEND_ACC_IMMUTABLE) == 0
			|| descriptor->init_op1.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
			|| index >= (uint32_t) op_array->last_literal
			|| Z_TYPE(op_array->literals[index]) != IS_STRING) {
		return false;
	}
	bound = zend_hash_find(EG(class_table), Z_STR(op_array->literals[index]));
	return bound != NULL && Z_PTR_P(bound) == key;
}

/*
 * A published fast site whose epoch is stale, as every site is after a
 * request ends, would take the universal protocol once to be published
 * again. Re-arm it in place when its target still applies, as the VM's
 * first call in a request looks the function up again: a function name
 * must still bind the published function, and a method site's class must
 * be an immutable (persistent) class, whose identity and methods cannot
 * change between requests. The target must be immutable, so its entry cell
 * outlives the request that published it; the cell must still hold the
 * published native entry, and the run-time cache is the current request's.
 * Tried once per site and epoch; false leaves the site to the universal
 * protocol, which may still publish it in this epoch.
 */
static bool zend_native_call_fast_rearm_site(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor);

bool zend_native_call_fast_rearm(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor)
{
	zend_native_user_call_site_header *header =
		(zend_native_user_call_site_header *)
			ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);
	zend_native_entry_cell *cell;
	bool shared;

	if (header->fast_epoch == zend_native_call_resolution_cache_epoch
			|| header->fast_rearm_epoch
				== zend_native_call_resolution_cache_epoch) {
		return false;
	}
	header->fast_rearm_epoch = zend_native_call_resolution_cache_epoch;
	cell = header->fast_cell;
	shared = cell != NULL && header->fast_function != NULL
		&& (descriptor->init_opcode == ZEND_INIT_FCALL
			|| descriptor->init_opcode == ZEND_INIT_FCALL_BY_NAME
			|| (descriptor->init_opcode == ZEND_INIT_METHOD_CALL
				&& header->fast_key != NULL
				&& (((const zend_class_entry *) header->fast_key)->ce_flags
					& ZEND_ACC_IMMUTABLE) != 0));
	/* Another site already verified this target in the epoch: a name binds
	 * one function per request, and a method site's guard checks the class
	 * of every receiver. */
	if (shared && cell->fast_bound_epoch == zend_native_call_resolution_cache_epoch
			&& cell->fast_bound_entry == header->fast_entry) {
		header->fast_run_time_cache = cell->fast_bound_run_time_cache;
	} else if (!zend_native_call_fast_rearm_site(caller, descriptor)) {
		return false;
	} else if (shared) {
		cell->fast_bound_epoch = zend_native_call_resolution_cache_epoch;
		cell->fast_bound_entry = header->fast_entry;
		cell->fast_bound_run_time_cache = header->fast_run_time_cache;
	}
	header->fast_checked_epoch = zend_native_call_resolution_cache_epoch;
	header->fast_epoch = zend_native_call_resolution_cache_epoch;
	return true;
}

/*
 * Whether a stale site's published target still applies; the universal
 * protocol, which runs on false, may still publish the site this epoch.
 */
static bool zend_native_call_fast_rearm_site(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor)
{
	zend_native_user_call_site_header *header =
		(zend_native_user_call_site_header *)
			ZEND_NATIVE_USER_CALL_SITE_HEADER(descriptor);
	zend_function *function = header->fast_function;
	zend_native_entry_cell *cell = header->fast_cell;
	const zend_native_code *code;
	const zend_op_array *op_array = &caller->func->op_array;
	const zend_class_entry *key = (const zend_class_entry *) header->fast_key;

	if (function == NULL) {
		/* new C of a class without a constructor: only the class. */
		if (descriptor->init_opcode != ZEND_NEW
				|| (header->fast_flags & ZEND_NATIVE_CALL_FAST_NO_CALL) == 0
				|| !zend_native_call_rearm_class_binding(
					op_array, descriptor, key)) {
			return false;
		}
		return true;
	}
	/* Only an immutable (persistent) target keeps its function, entry cell
	 * and native code across requests; anything else may have been freed
	 * with the request that published it. */
	if (cell == NULL || function->type != ZEND_USER_FUNCTION
			|| (function->op_array.fn_flags & ZEND_ACC_IMMUTABLE) == 0) {
		return false;
	}
	switch (descriptor->init_opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME: {
			/* The lowercased name, as the VM's Init looks it up. */
			const uint32_t index = descriptor->init_op2.index
				+ (descriptor->init_opcode == ZEND_INIT_FCALL ? 0 : 1);
			zval *bound;

			if (descriptor->init_op2.kind != ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| index >= (uint32_t) op_array->last_literal
					|| Z_TYPE(op_array->literals[index]) != IS_STRING) {
				return false;
			}
			bound = zend_hash_find_known_hash(EG(function_table),
				Z_STR(op_array->literals[index]));
			if (bound == NULL
					&& descriptor->init_opcode == ZEND_INIT_NS_FCALL_BY_NAME
					&& index + 1 < (uint32_t) op_array->last_literal
					&& Z_TYPE(op_array->literals[index + 1]) == IS_STRING) {
				bound = zend_hash_find_known_hash(EG(function_table),
					Z_STR(op_array->literals[index + 1]));
			}
			if (bound == NULL || Z_PTR_P(bound) != function) {
				return false;
			}
			break;
		}
		case ZEND_INIT_METHOD_CALL:
			if (header->fast_key == NULL
					|| (((const zend_class_entry *) header->fast_key)->ce_flags
						& ZEND_ACC_IMMUTABLE) == 0
					|| (function->common.scope != NULL
						&& (function->common.scope->ce_flags
							& ZEND_ACC_IMMUTABLE) == 0)) {
				return false;
			}
			break;
		case ZEND_NEW:
			if (!zend_native_call_rearm_class_binding(
					op_array, descriptor, key)) {
				return false;
			}
			break;
		case ZEND_INIT_STATIC_METHOD_CALL:
			if (function->common.scope == NULL
					|| (function->common.scope->ce_flags
						& ZEND_ACC_IMMUTABLE) == 0) {
				return false;
			}
			if (descriptor->init_op1.kind
					== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
				/* C::m(): the name must still bind the published class; a
				 * call keeping the caller's $this keyed its object, so its
				 * class must be immutable and still have that method. */
				if ((header->fast_flags & ZEND_NATIVE_CALL_FAST_STATIC_THIS)
						== 0) {
					if (!zend_native_call_rearm_class_binding(
							op_array, descriptor, key)) {
						return false;
					}
				} else {
					const uint32_t class_index = descriptor->init_op1.index + 1;
					const uint32_t method_index = descriptor->init_op2.index + 1;
					zval *bound;
					const zend_class_entry *ce;

					if (descriptor->init_op2.kind
								!= ZEND_MIR_SOURCE_OPERAND_LITERAL
							|| class_index >= (uint32_t) op_array->last_literal
							|| method_index >= (uint32_t) op_array->last_literal
							|| Z_TYPE(op_array->literals[class_index]) != IS_STRING
							|| Z_TYPE(op_array->literals[method_index])
								!= IS_STRING
							|| (bound = zend_hash_find(EG(class_table),
								Z_STR(op_array->literals[class_index]))) == NULL
							|| ((ce = Z_PTR_P(bound))->ce_flags
								& ZEND_ACC_IMMUTABLE) == 0
							|| zend_hash_find_ptr(&ce->function_table,
								Z_STR(op_array->literals[method_index]))
								!= function) {
						return false;
					}
				}
			} else if ((descriptor->init_op1_payload & ZEND_FETCH_CLASS_MASK)
					== ZEND_FETCH_CLASS_STATIC) {
				/* static::m() compares the called scope with the key. */
				if (key == NULL || (key->ce_flags & ZEND_ACC_IMMUTABLE) == 0) {
					return false;
				}
			} else {
				/* self::/parent:: name the caller's (immutable) scope or
				 * its parent; a closure may be bound to another scope. */
				if ((op_array->fn_flags & ZEND_ACC_CLOSURE) != 0
						|| op_array->scope == NULL
						|| (op_array->scope->ce_flags & ZEND_ACC_IMMUTABLE)
							== 0) {
					return false;
				}
				if ((header->fast_flags & (ZEND_NATIVE_CALL_FAST_STATIC_THIS
						| ZEND_NATIVE_CALL_FAST_STATIC_FORWARD)) == 0) {
					return false;
				}
			}
			break;
		default:
			return false;
	}
	if (cell->state != ZEND_NATIVE_ENTRY_READY || cell->function != function
			|| (code = zend_native_entry_cell_load(cell)) == NULL
			|| zend_native_code_frame_entry(code) != header->fast_entry) {
		return false;
	}
	if (RUN_TIME_CACHE(&function->op_array) == NULL) {
		zend_init_func_run_time_cache(&function->op_array);
	}
	header->fast_run_time_cache = RUN_TIME_CACHE(&function->op_array);
	return true;
}

zend_execute_data *zend_native_call_reserve_dynamic_frame(
	zend_execute_data *caller, uint32_t reservation_size)
{
	zend_execute_data *call;

	if (caller == NULL) {
		return NULL;
	}
	call = (zend_execute_data *) zend_vm_stack_extend(reservation_size);
	caller->call = call;
	return call;
}

static zend_native_entry_cell zend_native_call_fragment_seed(
	zend_function *function)
{
	zend_native_entry_cell cell;

	memset(&cell, 0, sizeof(cell));
	cell.state = ZEND_NATIVE_ENTRY_READY;
	cell.function = function != NULL
		? function : (zend_function *) &zend_pass_function;
	cell.code = (const zend_native_code *) (uintptr_t) 1;
	return cell;
}

static zend_native_status zend_native_call_fragment_cleanup(
	zend_execute_data *caller, zend_native_entry_cell *cell)
{
	zval discarded;
	zend_native_status status;

	if (caller == NULL || caller->call == NULL) {
		return EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	}
	ZVAL_UNDEF(&discarded);
	status = zend_native_call_invoke(caller, cell, &discarded);
	if (!Z_ISUNDEF(discarded)) {
		zval_ptr_dtor(&discarded);
	}
	return status == ZEND_NATIVE_BAILOUT
		? ZEND_NATIVE_BAILOUT : ZEND_NATIVE_EXCEPTION;
}

zend_native_direct_call_result zend_native_call_fragment(
	zend_execute_data *caller,
	const zend_native_entry_cell *entry_cell,
	const zend_native_internal_call_cell *internal_cell,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t source_position)
{
	zend_native_direct_call_result result = {
		.status = ZEND_NATIVE_EXCEPTION,
		.payload = 0
	};
	zend_native_entry_cell seed;
	zend_native_entry_cell *cell;
	uint32_t index;

	if (caller == NULL || descriptor == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| source_position >= caller->func->op_array.last
			|| (entry_cell == NULL) == (internal_cell == NULL)) {
		return result;
	}
	if (entry_cell != NULL) {
		cell = (zend_native_entry_cell *) entry_cell;
	} else {
		seed = zend_native_call_fragment_seed(internal_cell->function);
		cell = &seed;
	}
	if (source_position == descriptor->init_source_position) {
		zend_native_call_begin(caller, cell, descriptor);
		if (EG(exception) != NULL) {
			result.status = zend_native_call_fragment_cleanup(caller, cell);
			return result;
		}
		result.status = ZEND_NATIVE_RETURNED;
		return result;
	}
	for (index = 0; index < descriptor->argument_count; index++) {
		if (descriptor->arguments[index].source_position != source_position) {
			continue;
		}
		if (zend_native_call_set_explicit_argument(
				caller, &descriptor->arguments[index]) == FAILURE) {
			if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Invalid native call-fragment argument");
			}
			result.status = zend_native_call_fragment_cleanup(caller, cell);
			/* The fragment cleanup has retired this nested call frame.  Resume
			 * Zend's unfinished-call walk at the retired call's INIT, not at its
			 * failing SEND; otherwise that SEND ordinal is applied to the now
			 * innermost enclosing call frame. */
			if (result.status == ZEND_NATIVE_EXCEPTION
					&& EG(exception) != NULL
					&& zend_native_prepare_finally_exception(caller,
						descriptor->init_source_position) == FAILURE) {
				result.status = ZEND_NATIVE_BAILOUT;
			}
			return result;
		}
		result.status = ZEND_NATIVE_RETURNED;
		return result;
	}
	if (source_position != descriptor->do_source_position) {
		return result;
	}
	result.status = zend_native_call_invoke_finish_source(
		caller, cell, descriptor);
	if (result.status == ZEND_NATIVE_RETURNED
			&& (descriptor->flags
				& ZEND_NATIVE_USER_CALL_REQUIRE_SCALAR_RESULT) != 0) {
		result.payload = zend_native_call_read_source_scalar(
			caller,
			zend_native_encode_source_operand(caller, &descriptor->do_result),
			descriptor->result_type);
	}
	return result;
}

static zend_native_status zend_native_call_check_func_arg_impl(
	zend_execute_data *caller,
	uint64_t encoded_op2,
	uint32_t arg_num)
{
	zend_execute_data *call = caller != NULL ? caller->call : NULL;

	if (call == NULL || call->func == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (ZEND_NATIVE_OPERAND_TYPE(encoded_op2) == IS_CONST) {
		uint8_t operand_type;
		zval *argument_name = zend_native_call_explicit_operand(
			caller, encoded_op2, &operand_type);
		uint32_t index;

		if (argument_name == NULL || operand_type != IS_CONST
				|| Z_TYPE_P(argument_name) != IS_STRING) {
			return ZEND_NATIVE_EXCEPTION;
		}
		arg_num = 0;
		if (call->func->common.arg_info != NULL) {
			for (index = 0;
					index < call->func->common.num_args;
					index++) {
				if (zend_string_equals(
						Z_STR_P(argument_name),
						call->func->common.arg_info[index].name)) {
					arg_num = index + 1;
					break;
				}
			}
		}
		if (arg_num == 0
				&& (call->func->common.fn_flags
					& ZEND_ACC_VARIADIC) != 0) {
			arg_num = call->func->common.num_args + 1;
		}
		if (arg_num == 0) {
			ZEND_DEL_CALL_FLAG(call, ZEND_CALL_SEND_ARG_BY_REF);
			return ZEND_NATIVE_RETURNED;
		}
	}
	if (arg_num == 0) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((arg_num <= MAX_ARG_FLAG_NUM
			&& QUICK_ARG_SHOULD_BE_SENT_BY_REF(call->func, arg_num))
			|| (arg_num > MAX_ARG_FLAG_NUM
				&& ARG_SHOULD_BE_SENT_BY_REF(call->func, arg_num))) {
		ZEND_ADD_CALL_FLAG(call, ZEND_CALL_SEND_ARG_BY_REF);
	} else {
		ZEND_DEL_CALL_FLAG(call, ZEND_CALL_SEND_ARG_BY_REF);
	}
	return ZEND_NATIVE_RETURNED;
}

static zend_native_status zend_native_call_check_undef_args_impl(
	zend_execute_data *caller)
{
	zend_execute_data *call = caller != NULL ? caller->call : NULL;

	if (call == NULL || call->func == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((ZEND_CALL_INFO(call) & ZEND_CALL_MAY_HAVE_UNDEF) != 0
			&& zend_handle_undef_args(call) == FAILURE) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_call_check_func_arg(
	zend_execute_data *caller,
	uint64_t encoded_op1,
	uint64_t encoded_op2,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_opcode,
	uint32_t source_position)
{
	(void) encoded_op1;
	(void) encoded_result;
	(void) extended_value;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| source_opcode != ZEND_CHECK_FUNC_ARG
			|| source_position >= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_call_check_func_arg_impl(
		caller, encoded_op2, ZEND_NATIVE_OPERAND_VALUE(encoded_op2));
}

zend_native_check_func_arg_result zend_native_call_check_func_arg_resolved(
	zend_native_direct_activation *activation,
	uint32_t placement_index)
{
	const zend_native_user_call_placement *placement;

	if (activation == NULL || !activation->setup_record
			|| zend_native_active_direct_call != activation
			|| activation->resolution.function == NULL
			|| placement_index >= activation->resolution.placement_count
			|| activation->resolution.placements == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Invalid resolved native function argument check");
		}
		return ZEND_NATIVE_CHECK_FUNC_ARG_EXCEPTION;
	}
	placement = &activation->resolution.placements[placement_index];
	return (placement->flags
			& (ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_SHOULD_REF
				| ZEND_NATIVE_USER_CALL_PLACEMENT_TARGET_MUST_REF)) != 0
			|| placement->mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
		? ZEND_NATIVE_CHECK_FUNC_ARG_BY_REFERENCE
		: ZEND_NATIVE_CHECK_FUNC_ARG_BY_VALUE;
}

zend_native_status zend_native_call_check_undef_args(
	zend_execute_data *caller,
	uint64_t encoded_op1,
	uint64_t encoded_op2,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_opcode,
	uint32_t source_position)
{
	(void) encoded_op1;
	(void) encoded_op2;
	(void) encoded_result;
	(void) extended_value;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| source_opcode != ZEND_CHECK_UNDEF_ARGS
			|| source_position >= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_call_check_undef_args_impl(caller);
}

zend_native_status zend_native_call_fragment_explicit(
	zend_execute_data *caller,
	uint32_t source_opcode,
	uint64_t encoded_op1,
	uint32_t op1_payload,
	uint64_t encoded_op2,
	uint32_t op2_payload,
	uint64_t encoded_result,
	uint32_t result_payload,
	uint32_t extended_value,
	uint32_t source_position)
{
	zend_native_user_call_descriptor descriptor;
	zend_native_direct_internal_call_argument argument;
	zend_native_entry_cell seed =
		zend_native_call_fragment_seed((zend_function *) &zend_pass_function);
	zend_native_status status;
	bool init = false;
	bool send = false;
	bool finish = false;
	bool check_func_arg = false;
	bool check_undef_args = false;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| source_position >= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	switch (source_opcode) {
		case ZEND_INIT_FCALL:
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
		case ZEND_INIT_METHOD_CALL:
		case ZEND_INIT_STATIC_METHOD_CALL:
		case ZEND_INIT_PARENT_PROPERTY_HOOK_CALL:
		case ZEND_NEW:
			init = true;
			break;
		case ZEND_SEND_VAL:
		case ZEND_SEND_VAL_EX:
		case ZEND_SEND_VAR:
		case ZEND_SEND_VAR_EX:
		case ZEND_SEND_REF:
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_FUNC_ARG:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_PLACEHOLDER:
			send = true;
			break;
		case ZEND_CHECK_FUNC_ARG:
			check_func_arg = true;
			break;
		case ZEND_CHECK_UNDEF_ARGS:
			check_undef_args = true;
			break;
		case ZEND_DO_UCALL:
		case ZEND_DO_FCALL:
		case ZEND_DO_FCALL_BY_NAME:
		case ZEND_DO_ICALL:
		case ZEND_CALLABLE_CONVERT:
		case ZEND_CALLABLE_CONVERT_PARTIAL:
			finish = true;
			break;
		default:
			return ZEND_NATIVE_EXCEPTION;
	}
	if (check_undef_args) {
		return zend_native_call_check_undef_args_impl(caller);
	}
	if (check_func_arg) {
		return zend_native_call_check_func_arg_impl(
			caller, encoded_op2, op2_payload);
	}
	memset(&descriptor, 0, sizeof(descriptor));
	if (!zend_native_decode_source_operand(caller, encoded_op1, &descriptor.init_op1)
			|| !zend_native_decode_source_operand(caller, encoded_op2, &descriptor.init_op2)
			|| !zend_native_decode_source_operand(caller, encoded_result, &descriptor.init_result)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (init) {
		descriptor.argument_count = extended_value;
		descriptor.initial_argument_count = extended_value;
		descriptor.init_source_position = source_position;
		descriptor.init_opcode = source_opcode;
		descriptor.init_op1_payload = op1_payload;
		descriptor.init_op2_payload = op2_payload;
		descriptor.init_result_payload = result_payload;
		descriptor.init_extended_value = extended_value;
		zend_native_call_begin(caller, &seed, &descriptor);
		if (EG(exception) != NULL) {
			return zend_native_call_fragment_cleanup(caller, &seed);
		}
		return ZEND_NATIVE_RETURNED;
	}
	if (send) {
		memset(&argument, 0, sizeof(argument));
		argument.ordinal = op2_payload != 0 ? op2_payload - 1 : 0;
		argument.mode = source_opcode == ZEND_SEND_REF
			? ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			: source_opcode == ZEND_SEND_PLACEHOLDER
				? ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER
				: ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE;
		argument.source_opcode = source_opcode;
		argument.source_position = source_position;
		argument.source_operand = descriptor.init_op1;
		argument.auxiliary_operand = descriptor.init_op2;
		argument.auxiliary_payload = op2_payload;
		argument.result_payload = result_payload;
		argument.extended_value = extended_value;
		if (zend_native_call_set_explicit_argument(
				caller, &argument) == SUCCESS) {
			return ZEND_NATIVE_RETURNED;
		}
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid native SEND fragment");
		}
		return zend_native_call_fragment_cleanup(caller, &seed);
	}
	descriptor.do_source_position = source_position;
	descriptor.do_opcode = source_opcode;
	descriptor.do_op1_payload = op1_payload;
	descriptor.do_op2_payload = op2_payload;
	descriptor.do_result_payload = result_payload;
	descriptor.do_extended_value = extended_value;
	descriptor.do_op1 = descriptor.init_op1;
	descriptor.do_op2 = descriptor.init_op2;
	descriptor.do_result = descriptor.init_result;
	status = zend_native_call_invoke_finish_source(
		caller, &seed, &descriptor);
	return finish ? status : ZEND_NATIVE_EXCEPTION;
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

/*
 * Tier 2 (ADR 0025 section 4): ZEND_NATIVE_TIER2_THRESHOLD calls of a
 * counted function queue it once for recompilation at request shutdown;
 * 0 or unset counts nothing.
 */
static zend_native_entry_cell *zend_native_tier2_queue[256];
static uint32_t zend_native_tier2_queue_length;

uint32_t zend_native_tier2_threshold(void)
{
	static int64_t threshold = -1;

	if (threshold < 0) {
		const char *value = getenv("ZEND_NATIVE_TIER2_THRESHOLD");
		long parsed = value != NULL ? strtol(value, NULL, 10) : 0;

		threshold = parsed > 0 && parsed < INT32_MAX ? parsed : 0;
	}
	return (uint32_t) threshold;
}

bool (*zend_native_tier2_recompile_hook)(zend_native_entry_cell *cell);

/*
 * Called by a counted function's entry (a register-preserving call, the
 * frame unpublished) when its countdown runs out: recompiles at once, like
 * a lazy compilation, or queues the function for request shutdown.
 */
static void zend_native_tier2_dump_feedback(const zend_native_entry_cell *cell)
{
	static FILE *file;
	const char *path = getenv("ZEND_NATIVE_FEEDBACK_DUMP");
	const zend_op_array *op_array;
	uint32_t index;

	if (path == NULL || cell->function == NULL
			|| cell->tier2_result_feedback == NULL) {
		return;
	}
	if (file == NULL && (file = fopen(path, "a")) == NULL) {
		return;
	}
	op_array = &cell->function->op_array;
	for (index = 0; index < op_array->last; index++) {
		const uint32_t feedback = cell->tier2_result_feedback[index];

		if (feedback == 0) {
			continue;
		}
		fprintf(file, "%s%s%s %u %s %u %04x\n",
			op_array->scope ? ZSTR_VAL(op_array->scope->name) : "",
			op_array->scope ? "::" : "",
			op_array->function_name ? ZSTR_VAL(op_array->function_name) : "{main}",
			index, zend_get_opcode_name(op_array->opcodes[index].opcode),
			feedback >> 16, feedback & 0xffff);
	}
	fflush(file);
}

void zend_native_tier2_note(zend_native_entry_cell *cell)
{
	zend_native_tier2_dump_feedback(cell);
	if (cell == NULL || cell->tier2_state != ZEND_NATIVE_TIER2_NONE) {
		return;
	}
	if (zend_native_tier2_recompile_hook != NULL
			&& EG(exception) == NULL
			&& zend_native_tier2_recompile_hook(cell)) {
		return;
	}
	if (zend_native_tier2_queue_length
			< sizeof(zend_native_tier2_queue)
				/ sizeof(zend_native_tier2_queue[0])) {
		cell->tier2_state = ZEND_NATIVE_TIER2_QUEUED;
		zend_native_tier2_queue[zend_native_tier2_queue_length++] = cell;
	}
}

void zend_native_tier2_record(
	zend_native_entry_cell *cell, const zend_execute_data *frame)
{
	const zend_function *function = cell->function;
	uint32_t declared;
	uint32_t passed;
	uint32_t index;

	if (function == NULL || !ZEND_USER_CODE(function->type)) {
		return;
	}
	if (cell->tier2_result_feedback == NULL && function->op_array.last != 0) {
		cell->tier2_result_feedback = pecalloc(
			function->op_array.last, sizeof(uint32_t), 1);
	}
	declared = MIN(function->op_array.num_args, 8);
	passed = MIN(ZEND_CALL_NUM_ARGS(frame), declared);
	for (index = 0; index < passed; index++) {
		cell->tier2_arg_types[index] |= (uint16_t) (UINT16_C(1)
			<< (Z_TYPE_P(ZEND_CALL_ARG(frame, index + 1)) & 15));
	}
	for (; index < declared; index++) {
		cell->tier2_arg_types[index] |= ZEND_NATIVE_TIER2_ARG_MISSING;
	}
}

uint32_t zend_native_tier2_take_queue(
	zend_native_entry_cell **cells, uint32_t capacity)
{
	uint32_t count = MIN(capacity, zend_native_tier2_queue_length);

	memcpy(cells, zend_native_tier2_queue, count * sizeof(*cells));
	memmove(zend_native_tier2_queue, zend_native_tier2_queue + count,
		(zend_native_tier2_queue_length - count) * sizeof(*cells));
	zend_native_tier2_queue_length -= count;
	return count;
}
