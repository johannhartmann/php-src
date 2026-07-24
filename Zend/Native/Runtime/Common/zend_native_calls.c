/* Native user-function calls over real Zend execution frames. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_API.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_frameless_function.h"
#include "Zend/zend_object_handlers.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_partial.h"

#include <string.h>

#ifdef ZTS
# include "TSRM/TSRM.h"
#endif

static void (*zend_native_previous_execute_ex)(zend_execute_data *execute_data);
static uint32_t zend_native_reentry_users;
#ifdef ZTS
static MUTEX_T zend_native_reentry_mutex;
#endif
ZEND_TLS zend_native_reentry_scope *zend_native_active_reentry_scope;

ZEND_TLS zend_native_direct_activation *zend_native_active_direct_call;

void zend_native_execution_context_init(
	zend_native_execution_context *context)
{
	ZEND_ASSERT(context != NULL);
	context->vm_stack = &EG(vm_stack);
	context->vm_stack_top = &EG(vm_stack_top);
	context->vm_stack_end = &EG(vm_stack_end);
	context->current_execute_data = &EG(current_execute_data);
	context->active_direct_call = (void **) &zend_native_active_direct_call;
	context->map_ptr_base_address = (void **) &CG(map_ptr_base);
	context->vm_interrupt = &EG(vm_interrupt);
	context->exception = &EG(exception);
#ifdef ZEND_CHECK_STACK_LIMIT
	context->stack_limit = &EG(stack_limit);
#else
	context->stack_limit = NULL;
#endif
	context->observers_enabled = ZEND_OBSERVER_ENABLED;
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

static void zend_native_reentry_lock(void)
{
#ifdef ZTS
	ZEND_ASSERT(zend_native_reentry_mutex != NULL);
	tsrm_mutex_lock(zend_native_reentry_mutex);
#endif
}

static void zend_native_reentry_unlock(void)
{
#ifdef ZTS
	tsrm_mutex_unlock(zend_native_reentry_mutex);
#endif
}

static zend_native_entry_cell *zend_native_reentry_find(
	zend_native_reentry_scope *scope, zend_function *function)
{
	zend_native_reentry_scope *current;

	for (current = scope; current != NULL; current = current->previous) {
		uint32_t index;

		if (current->resolver != NULL) {
			zend_native_entry_cell *cell = current->resolver(
				current->resolver_context, function);

			if (cell != NULL) {
				return cell;
			}
		}
		for (index = 0; index < current->binding_count; index++) {
			if (current->bindings[index].function == function) {
				return current->bindings[index].entry_cell;
			}
		}
	}
	return NULL;
}

zend_native_entry_cell *zend_native_reentry_resolve(
	zend_function *function)
{
	if (function == NULL || !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	return zend_native_reentry_find(
		zend_native_active_reentry_scope, function);
}

static void zend_native_reentry_execute_ex(zend_execute_data *execute_data)
{
	zend_native_reentry_scope *scope = zend_native_active_reentry_scope;
	zend_native_entry_cell *cell;
	zend_execute_data *previous = execute_data->prev_execute_data;
	zend_native_status status;

	if (scope == NULL) {
		zend_native_previous_execute_ex(execute_data);
		return;
	}
	cell = zend_native_reentry_resolve(execute_data->func);
	if (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
			|| cell->code == NULL) {
		zend_throw_error(NULL,
			"Userland reentry target is not part of the native component");
		ZEND_OBSERVER_FCALL_END(execute_data, NULL);
		EG(current_execute_data) = previous;
		return;
	}
	if (cell->frame_probe != NULL) {
		cell->frame_probe(cell->frame_probe_context, previous, execute_data);
	}
	cell->active_calls++;
	EG(current_execute_data) = execute_data;
	status = zend_native_execute_observed_frame(cell->code, execute_data, NULL);
	EG(current_execute_data) = previous;
	cell->active_calls--;
	if (status == ZEND_NATIVE_BAILOUT) {
		zend_bailout();
	}
}

zend_result zend_native_reentry_startup(void)
{
#ifdef ZTS
	if (zend_native_reentry_mutex != NULL) {
		return FAILURE;
	}
	zend_native_reentry_mutex = tsrm_mutex_alloc();
	if (zend_native_reentry_mutex == NULL) {
		return FAILURE;
	}
#endif
	if (zend_native_previous_execute_ex != NULL
			|| zend_native_reentry_users != 0) {
#ifdef ZTS
		tsrm_mutex_free(zend_native_reentry_mutex);
		zend_native_reentry_mutex = NULL;
#endif
		return FAILURE;
	}
	return SUCCESS;
}

void zend_native_reentry_shutdown(void)
{
	zend_native_reentry_lock();
	if (zend_execute_ex == zend_native_reentry_execute_ex) {
		zend_execute_ex = zend_native_previous_execute_ex;
	}
	zend_native_previous_execute_ex = NULL;
	zend_native_reentry_users = 0;
	zend_native_active_reentry_scope = NULL;
	zend_native_reentry_unlock();
#ifdef ZTS
	tsrm_mutex_free(zend_native_reentry_mutex);
	zend_native_reentry_mutex = NULL;
#endif
}

zend_result zend_native_reentry_install(void)
{
	zend_result result = FAILURE;

	zend_native_reentry_lock();
	if (zend_native_reentry_users == 0) {
		if (zend_native_previous_execute_ex == NULL
				&& zend_execute_ex != zend_native_reentry_execute_ex) {
			zend_native_previous_execute_ex = zend_execute_ex;
			zend_execute_ex = zend_native_reentry_execute_ex;
			zend_native_reentry_users = 1;
			result = SUCCESS;
		}
	} else if (zend_native_reentry_users != UINT32_MAX
			&& zend_native_previous_execute_ex != NULL
			&& zend_execute_ex == zend_native_reentry_execute_ex) {
		zend_native_reentry_users++;
		result = SUCCESS;
	}
	zend_native_reentry_unlock();
	return result;
}

void zend_native_reentry_uninstall(void)
{
	zend_native_reentry_lock();
	if (zend_native_reentry_users != 0) {
		zend_native_reentry_users--;
		if (zend_native_reentry_users == 0) {
			if (zend_execute_ex == zend_native_reentry_execute_ex) {
				zend_execute_ex = zend_native_previous_execute_ex;
			}
			zend_native_previous_execute_ex = NULL;
		}
	}
	zend_native_reentry_unlock();
}

zend_result zend_native_reentry_scope_enter(
	zend_native_reentry_scope *scope,
	const zend_native_reentry_binding *bindings,
	uint32_t binding_count)
{
	return zend_native_reentry_scope_enter_resolver(
		scope, bindings, binding_count, NULL, NULL);
}

zend_result zend_native_reentry_scope_enter_resolver(
	zend_native_reentry_scope *scope,
	const zend_native_reentry_binding *bindings,
	uint32_t binding_count,
	zend_native_reentry_resolver_t resolver,
	void *resolver_context)
{
	uint32_t index;

	if (scope == NULL || bindings == NULL || binding_count == 0) {
		return FAILURE;
	}
	for (index = 0; index < binding_count; index++) {
		if (bindings[index].function == NULL
				|| bindings[index].entry_cell == NULL
				|| bindings[index].entry_cell->function != bindings[index].function
				|| bindings[index].entry_cell->state != ZEND_NATIVE_ENTRY_READY
				|| bindings[index].entry_cell->code == NULL) {
			return FAILURE;
		}
	}
	if (zend_native_reentry_install() == FAILURE) {
		return FAILURE;
	}
	scope->bindings = bindings;
	scope->binding_count = binding_count;
	scope->resolver = resolver;
	scope->resolver_context = resolver_context;
	scope->previous = zend_native_active_reentry_scope;
	zend_native_active_reentry_scope = scope;
	return SUCCESS;
}

void zend_native_reentry_scope_leave(zend_native_reentry_scope *scope)
{
	ZEND_ASSERT(scope != NULL && zend_native_active_reentry_scope == scope);
	if (scope != NULL && zend_native_active_reentry_scope == scope) {
		zend_native_active_reentry_scope = scope->previous;
		scope->bindings = NULL;
		scope->binding_count = 0;
		scope->resolver = NULL;
		scope->resolver_context = NULL;
		scope->previous = NULL;
		zend_native_reentry_uninstall();
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
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		return FAILURE;
	}
	op_array = &execute_data->func->op_array;
	supplied = ZEND_CALL_NUM_ARGS(execute_data);
	if (supplied < op_array->required_num_args) {
		zend_missing_arg_error(execute_data);
		return FAILURE;
	}
	for (ordinal = supplied;
			ordinal < op_array->num_args; ordinal++) {
		const zend_op *receive = &op_array->opcodes[ordinal];
		zval *argument = ZEND_CALL_ARG(execute_data, ordinal + 1);

		if (receive->opcode != ZEND_RECV_INIT
				|| receive->op1.num != ordinal + 1
				|| receive->op2_type != IS_CONST) {
			return FAILURE;
		}
		ZVAL_COPY(argument, RT_CONSTANT(receive, receive->op2));
		if (Z_TYPE_P(argument) == IS_CONSTANT_AST
				&& zval_update_constant_ex(argument, op_array->scope) == FAILURE) {
			zval_ptr_dtor_nogc(argument);
			ZVAL_UNDEF(argument);
			return FAILURE;
		}
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

static zend_function *zend_native_call_object_method(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_object **object_out, bool *owned_out)
{
	zval *receiver;
	zval *name;
	zval *cache_name = NULL;
	zend_object *object;
	zend_object *original;
	zend_function *function;
	zend_op_array *op_array = &caller->func->op_array;

	*object_out = NULL;
	*owned_out = false;
	name = zend_native_direct_operand(caller, &descriptor->init_op2, true);
	if (descriptor->init_op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
			&& descriptor->init_op2.index + 1 < op_array->last_literal) {
		cache_name = &op_array->literals[descriptor->init_op2.index + 1];
	}
	if (name != NULL && Z_ISREF_P(name)) {
		name = Z_REFVAL_P(name);
	}
	if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
		zend_throw_error(NULL, "Method name must be a string");
		zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		return NULL;
	}
	if (descriptor->init_op1.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		receiver = &caller->This;
	} else {
		receiver = zend_native_call_source_slot(caller, &descriptor->init_op1);
	}
	if (receiver == NULL) {
		zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		return NULL;
	}
	ZVAL_DEREF(receiver);
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		zend_throw_error(NULL, "Call to a member function %s() on %s",
			Z_STRVAL_P(name), zend_zval_value_name(receiver));
		zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		return NULL;
	}
	original = object = Z_OBJ_P(receiver);
	function = object->handlers->get_method(
		&object, Z_STR_P(name), cache_name);
	if (function == NULL) {
		if (function == NULL && EG(exception) == NULL) {
			zend_undefined_method(original->ce, Z_STR_P(name));
		}
		zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
		zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		return NULL;
	}
	zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
	if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0) {
		*object_out = (zend_object *) original->ce;
		zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		return function;
	}
	*object_out = object;
	if (descriptor->init_op1.kind != ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		GC_ADDREF(object);
		zend_native_call_consume_source_slot(caller, &descriptor->init_op1);
		*owned_out = true;
	}
	return function;
}

static zend_function *zend_native_call_static_method(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_class_entry **called_scope_out)
{
	zend_class_entry *called_scope;
	zval *name;
	zval *cache_name = NULL;
	zend_function *function;
	zend_op_array *op_array = &caller->func->op_array;

	called_scope = zend_native_call_source_class(
		caller, &descriptor->init_op1, descriptor->init_op1_payload);
	if (called_scope == NULL) {
		return NULL;
	}
	if (descriptor->init_op2.kind == ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		function = called_scope->constructor;
	} else {
		name = zend_native_direct_operand(caller, &descriptor->init_op2, true);
		if (descriptor->init_op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				&& descriptor->init_op2.index + 1 < op_array->last_literal) {
			cache_name = &op_array->literals[descriptor->init_op2.index + 1];
		}
		if (name != NULL && Z_ISREF_P(name)) {
			name = Z_REFVAL_P(name);
		}
		if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
			zend_throw_error(NULL, "Method name must be a string");
			zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
			return NULL;
		}
		function = called_scope->get_static_method != NULL
			? called_scope->get_static_method(called_scope, Z_STR_P(name))
			: zend_std_get_static_method(called_scope, Z_STR_P(name), cache_name);
		zend_native_call_consume_source_slot(caller, &descriptor->init_op2);
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
		*called_scope_out = called_scope;
	}
	return function;
}

static zend_function *zend_native_call_constructor(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_object **object_out, bool *missing_out)
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
	GC_ADDREF(*object_out);
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

static void zend_native_call_release_target(zend_execute_data *call)
{
	uint32_t call_info = ZEND_CALL_INFO(call);

	/* A closure owns the zend_function embedded in its object.  Inspect and
	 * dispose a trampoline before releasing any target object that may own
	 * call->func.  This mirrors the VM call-frame teardown order. */
	if ((call->func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
		zend_free_trampoline(call->func);
	}
	if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
		OBJ_RELEASE(Z_OBJ(call->This));
	} else if ((call_info & ZEND_CALL_CLOSURE) != 0) {
		OBJ_RELEASE(ZEND_CLOSURE_OBJECT(call->func));
	}
}

static zval *zend_native_call_callable_operand(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor)
{
	return zend_native_direct_operand(caller, &descriptor->init_op2, true);
}

static bool zend_native_call_dynamic_target(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	zend_function **function_out, void **object_or_scope_out,
	uint32_t *call_info_out)
{
	zend_fcall_info_cache fcc;
	zval *callable = zend_native_call_callable_operand(caller, descriptor);
	char *error = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION | ZEND_CALL_DYNAMIC;
	void *object_or_scope;

	if (callable == NULL || Z_ISUNDEF_P(callable)
			|| !zend_is_callable_ex(
				callable, NULL, 0, NULL, &fcc, &error)) {
		if (EG(exception) == NULL) {
			zend_type_error("Value of type %s is not callable",
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
		GC_ADDREF(ZEND_CLOSURE_OBJECT(fcc.function_handler));
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
		GC_ADDREF(fcc.object);
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
	function = zend_hash_find_known_hash(
		EG(function_table), Z_STR(encoded_name[1]));
	if (function == NULL && namespace_fallback) {
		function = zend_hash_find_known_hash(
			EG(function_table), Z_STR(encoded_name[2]));
	}
	if (function == NULL || Z_TYPE_P(function) != IS_PTR) {
		zend_throw_error(NULL, "Call to undefined function %s()",
			Z_STRVAL(encoded_name[1]));
		return false;
	}
	*function_out = Z_FUNC_P(function);
	if ((*function_out)->type == ZEND_USER_FUNCTION
			&& RUN_TIME_CACHE(&(*function_out)->op_array) == NULL) {
		zend_init_func_run_time_cache(&(*function_out)->op_array);
	}
	*object_or_scope_out = NULL;
	*call_info_out = ZEND_CALL_NESTED_FUNCTION;
	return true;
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

	if (caller == NULL || cell == NULL || descriptor == NULL
			|| caller->call != NULL) {
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

			if (resolved == NULL || resolved->type != ZEND_USER_FUNCTION) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL, "Call to undefined function %s()",
						name != NULL && Z_TYPE_P(name) == IS_STRING
							? Z_STRVAL_P(name) : "unknown");
				}
				function = (zend_function *) &zend_pass_function;
				break;
			}
			if (resolved != function) {
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
					&object_or_called_scope, &call_info)) {
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
				caller, descriptor, &object, &receiver_owned);

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
				caller, descriptor, &called_scope);

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
				caller, descriptor, &object, &constructor_missing);

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
	call->prev_execute_data = caller;
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
	exception = EG(exception);
	/* Zend's dispatcher walks the innermost active region outward. */
	for (index = op_array->last_try_catch; index-- > 0;) {
		const zend_try_catch_element *region = &op_array->try_catch_array[index];
		const zend_op *fast_ret;
		zval *fast_call;

		if (region->finally_op == 0 || region->finally_end == 0
				|| source_opline_index < region->finally_op
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
	zend_native_status status;

	if (caller == NULL || cell == NULL || return_value == NULL
			|| caller->call == NULL) {
		zend_native_call_abort("Invalid native invocation state");
	}
	call = caller->call;
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
			&& (cell == NULL || cell->state != ZEND_NATIVE_ENTRY_READY
				|| cell->code == NULL)) {
		zend_native_call_abort("Resolved native invocation target is not ready");
	}
	if (EG(exception) != NULL) {
		zend_vm_stack_free_args(call);
		if ((ZEND_CALL_INFO(call)
				& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
			zend_free_extra_named_params(call->extra_named_params);
		}
		zend_native_call_release_target(call);
		zend_vm_stack_free_call_frame(call);
		caller->call = NULL;
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
		caller->call = NULL;
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
		caller->call = NULL;
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
	zend_init_func_execute_data(call, &call->func->op_array, NULL);
	EG(current_execute_data) = caller;
	if (cell->frame_probe != NULL) {
		cell->frame_probe(cell->frame_probe_context, caller, call);
	}
	ZVAL_UNDEF(return_value);
	call->return_value = return_value;
	cell->active_calls++;
	EG(current_execute_data) = call;
	status = zend_native_execute_frame(cell->code, call, NULL);
	EG(current_execute_data) = caller;
	cell->active_calls--;
	if (status == ZEND_NATIVE_RETURNED && EG(exception) != NULL) {
		status = ZEND_NATIVE_EXCEPTION;
	}
	zend_native_call_release_target(call);
	zend_vm_stack_free_call_frame(call);
	caller->call = NULL;
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
	zval *source = zend_native_direct_operand(caller, operand, true);
	zval *target;
	bool mutable_source;

	if (source == NULL || argument->ordinal >= ZEND_CALL_NUM_ARGS(callee)) {
		return FAILURE;
	}
	target = ZEND_CALL_ARG(callee, argument->ordinal + 1);
	mutable_source = operand->kind != ZEND_MIR_SOURCE_OPERAND_LITERAL;
	if (argument->mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		if (!Z_ISREF_P(source)) {
			if (!mutable_source) {
				zend_cannot_pass_by_reference(argument->ordinal + 1);
				return FAILURE;
			}
			ZVAL_MAKE_REF(source);
		}
		ZVAL_COPY(target, source);
		return SUCCESS;
	}
	if (argument->mode != ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE) {
		return FAILURE;
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		ZVAL_COPY(target, source);
	} else if (operand->slot_kind == ZEND_MIR_SOURCE_SLOT_CV) {
		ZVAL_COPY_DEREF(target, source);
	} else {
		if (Z_ISREF_P(source)) {
			ZVAL_COPY_DEREF(target, source);
			zval_ptr_dtor(source);
		} else {
			ZVAL_COPY_VALUE(target, source);
		}
		ZVAL_UNDEF(source);
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
			|| activation->cell == NULL || activation->cell->code == NULL) {
		zend_throw_error(NULL, "Invalid observed direct native call");
		return ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_execute_frame(
		activation->cell->code, execute_data, NULL);
	activation->frame_initialized = false;
	return status;
}

static void zend_native_call_direct_release_receiver(
	zend_execute_data *call)
{
	if ((ZEND_CALL_INFO(call) & ZEND_CALL_RELEASE_THIS) != 0) {
		OBJ_RELEASE(Z_OBJ(call->This));
		ZEND_DEL_CALL_FLAG(call, ZEND_CALL_RELEASE_THIS);
	}
}

static void zend_native_call_direct_release(
	zend_native_direct_activation *activation)
{
	if (activation->frame_initialized) {
		zend_native_execution_cleanup_frame(activation->callee);
		activation->frame_initialized = false;
	} else if (activation->raw_arguments_owned) {
		zend_vm_stack_free_args(activation->callee);
		activation->raw_arguments_owned = false;
	}
	if (activation->cell_active && activation->cell != NULL
			&& activation->cell->active_calls != 0) {
		activation->cell->active_calls--;
		activation->cell_active = false;
	}
	if (activation->uses_discarded_return
			&& !Z_ISUNDEF(activation->discarded_return)) {
		zval_ptr_dtor(&activation->discarded_return);
		ZVAL_UNDEF(&activation->discarded_return);
	}
	activation->caller->call = NULL;
	EG(current_execute_data) = activation->caller;
	zend_native_active_direct_call = activation->previous;
	zend_native_call_direct_release_receiver(activation->callee);
	zend_vm_stack_free_call_frame(activation->callee);
}

static zend_native_status zend_native_call_direct_failed_entry(
	zend_execute_data *execute_data,
	zend_native_execution_context *context)
{
	(void) execute_data;
	(void) context;
	return ZEND_NATIVE_EXCEPTION;
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
			|| caller->call != NULL || cell->state != ZEND_NATIVE_ENTRY_READY
			|| cell->code == NULL || cell->function == NULL
			|| cell->function != descriptor->expected_function
			|| !ZEND_USER_CODE(cell->function->type)
			|| descriptor->argument_count > ZEND_MIR_ID_MAX
			|| (descriptor->flags
				& ~(ZEND_NATIVE_DIRECT_CALL_INLINE_FRAME
					| ZEND_NATIVE_DIRECT_CALL_CONSUME_RECEIVER
					| ZEND_NATIVE_DIRECT_CALL_INHERIT_CALLED_SCOPE)) != 0
			|| descriptor->source_position >= caller->func->op_array.last) {
		zend_throw_error(NULL, "Invalid direct native call descriptor");
		return result;
	}
	function = cell->function;
	entry = zend_native_code_frame_entry(cell->code);
	if (entry == NULL || descriptor->argument_count
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
			== zend_vm_calc_used_stack(descriptor->argument_count, function)
		&& function->op_array.scope == NULL
		&& function->op_array.num_args == descriptor->argument_count
		&& function->op_array.required_num_args == descriptor->argument_count
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
		: zend_vm_calc_used_stack(descriptor->argument_count, function);
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
		function, descriptor->argument_count, object_or_called_scope);
	activation = (zend_native_direct_activation *)
		((char *) call + used_stack);
	memset(activation, 0, sizeof(*activation));
	activation->caller = caller;
	activation->callee = call;
	activation->cell = cell;
	activation->descriptor = descriptor;
	activation->previous = zend_native_active_direct_call;
	ZVAL_UNDEF(&activation->discarded_return);
	zend_native_active_direct_call = activation;
	caller->call = call;
	for (index = 0; index < descriptor->argument_count; index++) {
		ZVAL_UNDEF(ZEND_CALL_ARG(call, index + 1));
	}
	activation->raw_arguments_owned = true;
	for (index = 0; index < descriptor->argument_count; index++) {
		if (zend_native_direct_transfer_argument(
				caller, call, &descriptor->arguments[index]) == FAILURE) {
			goto finish;
		}
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
			function->op_array.opcodes + descriptor->argument_count;
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
	cell->active_calls++;
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
			|| context->active_direct_call == NULL
			|| (activation = (zend_native_direct_activation *)
				*context->active_direct_call) == NULL
			|| activation->caller != caller
			|| activation->descriptor != descriptor) {
		if (EG(exception) == NULL) {
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
	return_value = activation->uses_discarded_return
		? &activation->discarded_return
		: zend_native_direct_operand(
			caller, &descriptor->result_operand, false);
	if (status == ZEND_NATIVE_RETURNED
			&& descriptor->result_type != ZEND_MIR_SCALAR_TYPE_NONE
			&& (return_value == NULL || !zend_native_direct_scalar_payload(
				return_value, descriptor->result_type, &result.payload))) {
		zend_throw_error(
			NULL, "Native callee violated its exact scalar result contract");
		status = ZEND_NATIVE_EXCEPTION;
	}
	result.status = status;
	zend_native_call_direct_release(activation);
	return result;
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

void zend_native_call_direct_unwind(zend_execute_data *outermost)
{
	while (zend_native_active_direct_call != NULL) {
		zend_native_direct_activation *activation =
			zend_native_active_direct_call;
		zend_execute_data *caller = activation->caller;

		if (activation->frame_initialized) {
			zend_native_execution_cleanup_frame(activation->callee);
			activation->frame_initialized = false;
		} else if (activation->raw_arguments_owned) {
			zend_vm_stack_free_args(activation->callee);
			activation->raw_arguments_owned = false;
		}
		if (activation->cell_active && activation->cell != NULL
				&& activation->cell->active_calls != 0) {
			activation->cell->active_calls--;
			activation->cell_active = false;
		}
		if (activation->uses_discarded_return
				&& !Z_ISUNDEF(activation->discarded_return)) {
			zval_ptr_dtor(&activation->discarded_return);
			ZVAL_UNDEF(&activation->discarded_return);
		}
		activation->caller->call = NULL;
		EG(current_execute_data) = caller;
		zend_native_active_direct_call = activation->previous;
		zend_native_call_direct_release_receiver(activation->callee);
		zend_vm_stack_free_call_frame(activation->callee);
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
	if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT) {
		zend_execute_data *call = caller->call;

		if (call == NULL || EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_closure_from_frame(return_value, call);
		if ((ZEND_CALL_INFO(call) & ZEND_CALL_RELEASE_THIS) != 0) {
			OBJ_RELEASE(Z_OBJ(call->This));
		}
		zend_vm_stack_free_call_frame(call);
		caller->call = NULL;
		return ZEND_NATIVE_RETURNED;
	}
	if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
		zend_execute_data *call = caller->call;
		void **cache_slot;
		zval *named_positions = NULL;
		zend_op_array *op_array = &caller->func->op_array;
		uint32_t call_info;

		if (call == NULL || caller->run_time_cache == NULL
				|| descriptor->do_op1_payload > op_array->cache_size
				|| 2 * sizeof(void *)
					> op_array->cache_size - descriptor->do_op1_payload) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (descriptor->do_op2.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			named_positions = zend_native_direct_operand(
				caller, &descriptor->do_op2, true);
			if (named_positions == NULL
					|| Z_TYPE_P(named_positions) != IS_ARRAY) {
				return ZEND_NATIVE_EXCEPTION;
			}
		} else if (descriptor->do_op2.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			return ZEND_NATIVE_EXCEPTION;
		}
		cache_slot = (void **) ((char *) caller->run_time_cache
			+ descriptor->do_op1_payload);
		call_info = ZEND_CALL_INFO(call);
		zend_partial_create(return_value,
			&call->This, call->func,
			ZEND_CALL_NUM_ARGS(call), ZEND_CALL_ARG(call, 1),
			(call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0
				? call->extra_named_params : NULL,
			named_positions != NULL ? Z_ARRVAL_P(named_positions) : NULL,
			op_array, opline, cache_slot,
			(descriptor->do_extended_value
				& ZEND_FCALL_USES_VARIADIC_PLACEHOLDER) != 0);
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
		caller->call = NULL;
		return EG(exception) == NULL
			? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_call_invoke(caller, cell, return_value);
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL
			&& zend_native_prepare_finally_exception(
				caller, descriptor->do_source_position) == FAILURE) {
		status = ZEND_NATIVE_BAILOUT;
	}
	if (status != ZEND_NATIVE_RETURNED || return_value == &temporary) {
		if (!Z_ISUNDEF_P(return_value)) {
			zval_ptr_dtor(return_value);
			ZVAL_UNDEF(return_value);
		}
	}
	return status;
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

uint64_t zend_native_abi_conformance(
	zend_execute_data *execute_data,
	const zval *first_argument_slot,
	uint64_t source_value,
	uint8_t zext8,
	int8_t sext8,
	uint16_t zext16,
	int16_t sext16,
	uint32_t zext32,
	int32_t sext32,
	uint64_t unsigned64,
	int64_t signed64,
	uint64_t spill_a,
	uint64_t spill_b,
	double fp0,
	double fp1,
	double fp2,
	double fp3,
	double fp4,
	double fp5,
	double fp6,
	double fp7,
	double fp8,
	double fp9)
{
	if (execute_data == NULL
			|| execute_data != EG(current_execute_data)
			|| execute_data->func == NULL
			|| execute_data->func->common.num_args < 1
			|| first_argument_slot != ZEND_CALL_ARG(execute_data, 1)
			|| Z_TYPE_P(first_argument_slot) != IS_LONG
			|| Z_LVAL_P(first_argument_slot) != 37
			|| source_value != UINT64_C(37)
			|| zext8 != UINT64_C(0xfe)
			|| sext8 != INT64_C(-128)
			|| zext16 != UINT64_C(0xfedc)
			|| sext16 != INT64_C(-32767)
			|| zext32 != UINT64_C(0xfedcba98)
			|| sext32 != INT64_C(-1985229329)
			|| unsigned64 != UINT64_C(0xfedcba9876543210)
			|| signed64 != INT64_C(-81985529216486895)
			|| spill_a != UINT64_C(0x0123456789abcdef)
			|| spill_b != UINT64_C(0x8877665544332211)
			|| fp0 != 1.5 || fp1 != -2.25 || fp2 != 3.125
			|| fp3 != -4.5 || fp4 != 5.75 || fp5 != -6.875
			|| fp6 != 7.0 || fp7 != -8.125 || fp8 != 9.25
			|| fp9 != -10.5) {
		return 0;
	}
	return ZEND_NATIVE_ABI_CONFORMANCE_RESULT;
}
