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

static zval *zend_native_frameless_slot(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_CV && type != IS_VAR && type != IS_TMP_VAR) {
		return NULL;
	}
	return ZEND_CALL_VAR(execute_data, operand.var);
}

static zval *zend_native_frameless_argument(
	zend_execute_data *execute_data, const zend_op *opline,
	uint8_t type, znode_op operand)
{
	zval *value;

	if (type == IS_CONST) {
		value = RT_CONSTANT(opline, operand);
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

zend_native_status zend_native_call_frameless_internal(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline;
	const zend_op *op_data = NULL;
	zval *arguments[3] = {NULL, NULL, NULL};
	zval *result;
	uint32_t argument_count;

	if (execute_data == NULL || execute_data->func == NULL
			|| execute_data->func->type != ZEND_USER_FUNCTION
			|| source_opline_index >= execute_data->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &execute_data->func->op_array.opcodes[source_opline_index];
	if (!ZEND_OP_IS_FRAMELESS_ICALL(opline->opcode)
			|| opline->extended_value >= zend_flf_count
			|| zend_flf_handlers[opline->extended_value] == NULL
			|| zend_flf_functions[opline->extended_value] == NULL
			|| opline->result_type == IS_UNUSED
			|| (result = zend_native_frameless_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	argument_count = ZEND_FLF_NUM_ARGS(opline->opcode);
	if (argument_count == 3) {
		if (source_opline_index + 1 >= execute_data->func->op_array.last
				|| (op_data = opline + 1)->opcode != ZEND_OP_DATA) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	execute_data->opline = opline;
	if (argument_count >= 1
			&& (arguments[0] = zend_native_frameless_argument(
				execute_data, opline, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (argument_count >= 2
			&& (arguments[1] = zend_native_frameless_argument(
				execute_data, opline, opline->op2_type, opline->op2)) == NULL) {
		zend_native_frameless_consume(
			execute_data, opline->op1_type, opline->op1);
		return ZEND_NATIVE_EXCEPTION;
	}
	if (argument_count == 3
			&& (arguments[2] = zend_native_frameless_argument(
				execute_data, op_data, op_data->op1_type, op_data->op1)) == NULL) {
		zend_native_frameless_consume(
			execute_data, opline->op1_type, opline->op1);
		zend_native_frameless_consume(
			execute_data, opline->op2_type, opline->op2);
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_NULL(result);
#if !ZEND_VM_SPEC || ZEND_OBSERVER_ENABLED
	if (ZEND_OBSERVER_ENABLED && UNEXPECTED(!zend_observer_handler_is_unobserved(
			ZEND_OBSERVER_DATA(zend_flf_functions[opline->extended_value])))) {
		zend_frameless_observed_call(execute_data);
	} else
#endif
	{
		switch (argument_count) {
			case 0:
				((zend_frameless_function_0)
					zend_flf_handlers[opline->extended_value])(result);
				break;
			case 1:
				((zend_frameless_function_1)
					zend_flf_handlers[opline->extended_value])(
						result, arguments[0]);
				break;
			case 2:
				((zend_frameless_function_2)
					zend_flf_handlers[opline->extended_value])(
						result, arguments[0], arguments[1]);
				break;
			case 3:
				((zend_frameless_function_3)
					zend_flf_handlers[opline->extended_value])(
						result, arguments[0], arguments[1], arguments[2]);
				break;
			default:
				return ZEND_NATIVE_EXCEPTION;
		}
	}
	zend_native_frameless_consume(
		execute_data, opline->op1_type, opline->op1);
	zend_native_frameless_consume(
		execute_data, opline->op2_type, opline->op2);
	if (op_data != NULL) {
		zend_native_frameless_consume(
			execute_data, op_data->op1_type, op_data->op1);
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
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

		for (index = 0; index < current->binding_count; index++) {
			if (current->bindings[index].function == function) {
				return current->bindings[index].entry_cell;
			}
		}
		if (current->resolver != NULL) {
			zend_native_entry_cell *cell = current->resolver(
				current->resolver_context, function);

			if (cell != NULL) {
				return cell;
			}
		}
	}
	return NULL;
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
	cell = zend_native_reentry_find(scope, execute_data->func);
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

static zval *zend_native_call_source_slot(
	zend_execute_data *caller, uint8_t type, znode_op operand)
{
	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_CV && type != IS_VAR && type != IS_TMP_VAR) {
		return NULL;
	}
	return ZEND_CALL_VAR(caller, operand.var);
}

static void zend_native_call_consume_source_slot(
	zend_execute_data *caller, uint8_t type, znode_op operand)
{
	zval *slot;

	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_VAR && type != IS_TMP_VAR) {
		return;
	}
	slot = zend_native_call_source_slot(caller, type, operand);
	if (slot != NULL && !Z_ISUNDEF_P(slot)) {
		zval_ptr_dtor(slot);
		ZVAL_UNDEF(slot);
	}
}

static zend_class_entry *zend_native_call_source_class(
	zend_execute_data *caller, const zend_op *source_init, znode_op operand,
	uint8_t type, uint32_t cache_offset)
{
	zval *value;

	if (type == IS_CONST) {
		value = RT_CONSTANT(source_init, operand);
		if (Z_TYPE_P(value) != IS_STRING
				|| Z_TYPE_P(value + 1) != IS_STRING) {
			return NULL;
		}
		return zend_fetch_class_by_name(
			Z_STR_P(value), Z_STR_P(value + 1),
			ZEND_FETCH_CLASS_DEFAULT | ZEND_FETCH_CLASS_EXCEPTION);
	}
	if (type == IS_UNUSED) {
		return zend_fetch_class(NULL, operand.num);
	}
	if (type != IS_VAR && type != IS_TMP_VAR && type != IS_CV) {
		return NULL;
	}
	value = zend_native_call_source_slot(caller, type, operand);
	if (value == NULL || Z_TYPE_P(value) != IS_PTR) {
		return NULL;
	}
	(void) cache_offset;
	return Z_CE_P(value);
}

static zend_function *zend_native_call_object_method(
	zend_execute_data *caller, const zend_op *source_init,
	zend_object **object_out, bool *owned_out)
{
	zval *receiver;
	zval *name;
	zend_object *object;
	zend_object *original;
	zend_function *function;

	*object_out = NULL;
	*owned_out = false;
	name = source_init->op2_type == IS_CONST
		? RT_CONSTANT(source_init, source_init->op2)
		: zend_native_call_source_slot(
			caller, source_init->op2_type, source_init->op2);
	if (name != NULL && Z_ISREF_P(name)) {
		name = Z_REFVAL_P(name);
	}
	if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
		zend_throw_error(NULL, "Method name must be a string");
		zend_native_call_consume_source_slot(
			caller, source_init->op2_type, source_init->op2);
		return NULL;
	}
	if (source_init->op1_type == IS_UNUSED) {
		receiver = &caller->This;
	} else {
		receiver = zend_native_call_source_slot(
			caller, source_init->op1_type, source_init->op1);
	}
	if (receiver == NULL) {
		zend_native_call_consume_source_slot(
			caller, source_init->op2_type, source_init->op2);
		return NULL;
	}
	ZVAL_DEREF(receiver);
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		zend_throw_error(NULL, "Call to a member function %s() on %s",
			Z_STRVAL_P(name), zend_zval_value_name(receiver));
		zend_native_call_consume_source_slot(
			caller, source_init->op2_type, source_init->op2);
		zend_native_call_consume_source_slot(
			caller, source_init->op1_type, source_init->op1);
		return NULL;
	}
	original = object = Z_OBJ_P(receiver);
	function = object->handlers->get_method(
		&object, Z_STR_P(name), source_init->op2_type == IS_CONST
			? RT_CONSTANT(source_init, source_init->op2) + 1 : NULL);
	if (function == NULL) {
		if (function == NULL && EG(exception) == NULL) {
			zend_undefined_method(original->ce, Z_STR_P(name));
		}
		zend_native_call_consume_source_slot(
			caller, source_init->op2_type, source_init->op2);
		zend_native_call_consume_source_slot(
			caller, source_init->op1_type, source_init->op1);
		return NULL;
	}
	zend_native_call_consume_source_slot(
		caller, source_init->op2_type, source_init->op2);
	if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0) {
		*object_out = (zend_object *) original->ce;
		zend_native_call_consume_source_slot(
			caller, source_init->op1_type, source_init->op1);
		return function;
	}
	*object_out = object;
	if (source_init->op1_type != IS_UNUSED) {
		GC_ADDREF(object);
		zend_native_call_consume_source_slot(
			caller, source_init->op1_type, source_init->op1);
		*owned_out = true;
	}
	return function;
}

static zend_function *zend_native_call_static_method(
	zend_execute_data *caller, const zend_op *source_init,
	zend_class_entry **called_scope_out)
{
	zend_class_entry *called_scope;
	zval *name;
	zend_function *function;

	called_scope = zend_native_call_source_class(
		caller, source_init, source_init->op1, source_init->op1_type,
		source_init->result.num);
	if (called_scope == NULL) {
		return NULL;
	}
	if (source_init->op2_type == IS_UNUSED) {
		function = called_scope->constructor;
	} else {
		name = source_init->op2_type == IS_CONST
			? RT_CONSTANT(source_init, source_init->op2)
			: zend_native_call_source_slot(
				caller, source_init->op2_type, source_init->op2);
		if (name != NULL && Z_ISREF_P(name)) {
			name = Z_REFVAL_P(name);
		}
		if (name == NULL || Z_TYPE_P(name) != IS_STRING) {
			zend_throw_error(NULL, "Method name must be a string");
			zend_native_call_consume_source_slot(
				caller, source_init->op2_type, source_init->op2);
			return NULL;
		}
		function = called_scope->get_static_method != NULL
			? called_scope->get_static_method(called_scope, Z_STR_P(name))
			: zend_std_get_static_method(
				called_scope, Z_STR_P(name),
				source_init->op2_type == IS_CONST
					? RT_CONSTANT(source_init, source_init->op2) + 1 : NULL);
		zend_native_call_consume_source_slot(
			caller, source_init->op2_type, source_init->op2);
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
	zend_execute_data *caller, const zend_op *source_init,
	zend_object **object_out, bool *missing_out)
{
	zend_class_entry *ce;
	zval *result;
	zend_function *constructor;

	*object_out = NULL;
	*missing_out = false;
	ce = zend_native_call_source_class(
		caller, source_init, source_init->op1, source_init->op1_type,
		source_init->op2.num);
	if (ce == NULL || source_init->result_type == IS_UNUSED) {
		return NULL;
	}
	result = zend_native_call_source_slot(
		caller, source_init->result_type, source_init->result);
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
	zend_execute_data *caller, const zend_op *source_init,
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
			|| source_init->op1_type != IS_CONST
			|| source_init->op2.num > ZEND_PROPERTY_HOOK_SET) {
		zend_throw_error(NULL, "Malformed parent property hook call");
		return NULL;
	}
	name = RT_CONSTANT(source_init, source_init->op1);
	if (Z_TYPE_P(name) != IS_STRING) {
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
	hook_kind = (zend_property_hook_kind) source_init->op2.num;
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
	zend_execute_data *caller, const zend_op *opline)
{
	switch (opline->op2_type) {
		case IS_CONST:
			return (zval *) RT_CONSTANT(opline, opline->op2);
		case IS_CV:
		case IS_VAR:
		case IS_TMP_VAR:
			return ZEND_CALL_VAR(caller, opline->op2.var);
		default:
			return NULL;
	}
}

static bool zend_native_call_dynamic_target(
	zend_execute_data *caller, const zend_op *opline,
	zend_function **function_out, void **object_or_scope_out,
	uint32_t *call_info_out)
{
	zend_fcall_info_cache fcc;
	zval *callable = zend_native_call_callable_operand(caller, opline);
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

void zend_native_call_begin(
	zend_execute_data *caller,
	zend_native_entry_cell *cell,
	uint32_t argument_count,
	uint32_t source_opline_index)
{
	zend_execute_data *call;
	zend_function *function;
	const zend_op *source_init;
	void *object_or_called_scope = NULL;
	bool receiver_owned = false;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t initial_argument_count;
	uint32_t index;

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
	source_init = &caller->func->op_array.opcodes[source_opline_index];
	initial_argument_count = source_init->extended_value;
	if (initial_argument_count > argument_count) {
		zend_native_call_abort("Native callee argument count is invalid");
	}
#ifdef ZEND_CHECK_STACK_LIMIT
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		zend_bailout();
	}
#endif
	switch (source_init->opcode) {
		case ZEND_INIT_FCALL: {
			zval *name = RT_CONSTANT(source_init, source_init->op2);
			zend_function *resolved = Z_TYPE_P(name) == IS_STRING
				? zend_fetch_function(Z_STR_P(name)) : NULL;

			if (resolved == NULL || resolved->type != ZEND_USER_FUNCTION) {
				if (EG(exception) == NULL) {
					zend_throw_error(NULL, "Call to undefined function %s()",
						Z_TYPE_P(name) == IS_STRING
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
					zend_native_call_abort(
						"Native named target compilation failed");
				}
			}
			function = resolved;
			break;
		}
		case ZEND_INIT_FCALL_BY_NAME:
		case ZEND_INIT_NS_FCALL_BY_NAME:
		case ZEND_INIT_DYNAMIC_CALL:
		case ZEND_INIT_USER_CALL:
			if (!zend_native_call_dynamic_target(
					caller, source_init, &function,
					&object_or_called_scope, &call_info)) {
				if ((source_init->op2_type & (IS_VAR | IS_TMP_VAR)) != 0) {
					zval *callable = ZEND_CALL_VAR(
						caller, source_init->op2.var);
					zval_ptr_dtor(callable);
					ZVAL_UNDEF(callable);
				}
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
					zend_native_call_abort(
						"Native dynamic target compilation failed");
				}
			}
			if ((source_init->op2_type & (IS_VAR | IS_TMP_VAR)) != 0) {
				zval *callable = ZEND_CALL_VAR(caller, source_init->op2.var);
				zval_ptr_dtor(callable);
				ZVAL_UNDEF(callable);
			}
			break;
		case ZEND_INIT_METHOD_CALL: {
			zend_object *object = NULL;
			zend_function *resolved = zend_native_call_object_method(
				caller, source_init, &object, &receiver_owned);

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
					zend_native_call_abort(
						"Native method target compilation failed");
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
				caller, source_init, &called_scope);

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
					zend_native_call_abort(
						"Native static method target compilation failed");
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
					caller, source_init, &object);

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
					zend_native_call_abort(
						"Native parent property hook compilation failed");
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
				caller, source_init, &object, &constructor_missing);

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
					zend_native_call_abort(
						"Native constructor target compilation failed");
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
	caller->opline = &caller->func->op_array.opcodes[source_opline_index];
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

zend_result zend_native_prepare_finally_exception(
	zend_execute_data *caller, uint32_t source_opline_index)
{
	zend_op_array *op_array;
	zend_object *exception;
	uint32_t index;

	if (caller == NULL || caller->func == NULL
			|| caller->func->type != ZEND_USER_FUNCTION
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
	if (opline->opcode != ZEND_DO_UCALL
			&& opline->opcode != ZEND_DO_FCALL
			&& opline->opcode != ZEND_DO_FCALL_BY_NAME
			&& opline->opcode != ZEND_DO_ICALL
			&& opline->opcode != ZEND_CALLABLE_CONVERT
			&& opline->opcode != ZEND_CALLABLE_CONVERT_PARTIAL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (EG(exception) != NULL) {
		ZVAL_UNDEF(&temporary);
		return zend_native_call_invoke(caller, cell, &temporary);
	}
	caller->opline = opline;
	if (opline->result_type == IS_UNUSED) {
		ZVAL_UNDEF(&temporary);
		return_value = &temporary;
	} else if (opline->result_type == IS_CV
			|| opline->result_type == IS_VAR
			|| opline->result_type == IS_TMP_VAR) {
		return_value = ZEND_CALL_VAR(caller, opline->result.var);
		ZVAL_UNDEF(return_value);
	} else {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->opcode == ZEND_CALLABLE_CONVERT) {
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
	if (opline->opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
		zend_execute_data *call = caller->call;
		void **cache_slot;
		zval *named_positions = NULL;
		zend_op_array *op_array = &caller->func->op_array;
		uint32_t call_info;

		if (call == NULL || caller->run_time_cache == NULL
				|| opline->op1.num > op_array->cache_size
				|| 2 * sizeof(void *)
					> op_array->cache_size - opline->op1.num) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (opline->op2_type == IS_CONST) {
			named_positions = RT_CONSTANT(opline, opline->op2);
			if (Z_TYPE_P(named_positions) != IS_ARRAY) {
				return ZEND_NATIVE_EXCEPTION;
			}
		} else if (opline->op2_type != IS_UNUSED) {
			return ZEND_NATIVE_EXCEPTION;
		}
		cache_slot = (void **) ((char *) caller->run_time_cache
			+ opline->op1.num);
		call_info = ZEND_CALL_INFO(call);
		zend_partial_create(return_value,
			&call->This, call->func,
			ZEND_CALL_NUM_ARGS(call), ZEND_CALL_ARG(call, 1),
			(call_info & ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0
				? call->extra_named_params : NULL,
			named_positions != NULL ? Z_ARRVAL_P(named_positions) : NULL,
			op_array, opline, cache_slot,
			(opline->extended_value
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
				caller, do_opline_index) == FAILURE) {
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
