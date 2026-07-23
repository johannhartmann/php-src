/* Direct calls to compile-time resolved Zend internal functions. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"

#include <string.h>

typedef struct _zend_native_internal_execution_state {
	zend_execute_data *caller;
	zend_execute_data *call;
	zval *return_value;
	zend_native_status status;
	bool observer_started;
	bool observer_finished;
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
	uint32_t initial_argument_count;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t index;

	if (caller == NULL || caller->call != NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| cell == NULL || cell->function == NULL
			|| cell->function->type != ZEND_INTERNAL_FUNCTION
			|| source_opline_index >= caller->func->op_array.last) {
		return FAILURE;
	}
	function = cell->function;
	source_opline = &caller->func->op_array.opcodes[source_opline_index];
	initial_argument_count = source_opline->extended_value;
	if (initial_argument_count > argument_count) {
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
		call_info, function, initial_argument_count, object_or_called_scope);
	for (index = 0; index < initial_argument_count; index++) {
		ZVAL_UNDEF(ZEND_CALL_ARG(call, index + 1));
	}
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
			|| !ZEND_USER_CODE(caller->func->type)
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
		case ZEND_SEND_UNPACK:
		case ZEND_SEND_ARRAY:
		case ZEND_SEND_USER:
		case ZEND_SEND_VAR_NO_REF:
		case ZEND_SEND_VAR_NO_REF_EX:
		case ZEND_SEND_FUNC_ARG:
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

static void zend_native_release_source_operand(zval *value, uint8_t operand_type)
{
	if (value != NULL
			&& (operand_type == IS_VAR || operand_type == IS_TMP_VAR)) {
		zval_ptr_dtor(value);
		ZVAL_UNDEF(value);
	}
}

static uint32_t zend_native_argument_number_by_name(
	const zend_function *function, const zend_string *name)
{
	uint32_t index;

	if (function == NULL || name == NULL || function->common.arg_info == NULL) {
		return 0;
	}
	for (index = 0; index < function->common.num_args; index++) {
		const zend_arg_info *info = &function->common.arg_info[index];
		if (info->name != NULL && zend_string_equals(name, info->name)) {
			return index + 1;
		}
	}
	return (function->common.fn_flags & ZEND_ACC_VARIADIC) != 0
		? function->common.num_args + 1 : 0;
}

static void zend_native_traversable_by_reference_warning(
	const zend_function *function, uint32_t argument_number)
{
	zend_error(E_WARNING,
		"Cannot pass by-reference argument %d of %s%s%s()"
		" by unpacking a Traversable, passing by-value instead",
		argument_number,
		function->common.scope != NULL
			? ZSTR_VAL(function->common.scope->name) : "",
		function->common.scope != NULL ? "::" : "",
		function->common.function_name != NULL
			? ZSTR_VAL(function->common.function_name) : "{main}");
}

static zend_result zend_native_call_unpack_array(
	zend_execute_data *caller, zval *source, uint8_t operand_type)
{
	zend_execute_data *call = caller->call;
	zend_function *function = call->func;
	zval *args = source;
	HashTable *table;
	zval *argument;
	zend_string *name;
	uint32_t argument_number = ZEND_CALL_NUM_ARGS(call) + 1;
	bool have_named_parameters = false;
	bool can_reference_buckets = operand_type == IS_CV || operand_type == IS_VAR;

	while (Z_ISREF_P(args)) {
		args = Z_REFVAL_P(args);
	}
	if (Z_TYPE_P(args) != IS_ARRAY) {
		return FAILURE;
	}
	table = Z_ARRVAL_P(args);
	zend_vm_stack_extend_call_frame(
		&call, argument_number - 1, zend_hash_num_elements(table));
	caller->call = call;

	if (can_reference_buckets && GC_REFCOUNT(table) > 1) {
		uint32_t candidate_number = argument_number;
		bool separate = false;

		ZEND_HASH_FOREACH_STR_KEY_VAL(table, name, argument) {
			if (name != NULL) {
				candidate_number = zend_native_argument_number_by_name(
					function, name);
			}
			if (candidate_number != 0
					&& ARG_SHOULD_BE_SENT_BY_REF(
						function, candidate_number)) {
				separate = true;
				break;
			}
			candidate_number++;
		} ZEND_HASH_FOREACH_END();
		if (separate) {
			SEPARATE_ARRAY(args);
			table = Z_ARRVAL_P(args);
		}
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(table, name, argument) {
		zval *target;

		if (name != NULL) {
			void *cache_slot[2] = {NULL, NULL};
			have_named_parameters = true;
			target = zend_handle_named_arg(
				&call, name, &argument_number, cache_slot);
			caller->call = call;
			if (target == NULL) {
				zend_native_release_source_operand(source, operand_type);
				return FAILURE;
			}
		} else {
			if (have_named_parameters) {
				zend_throw_error(NULL,
					"Cannot use positional argument after named argument during unpacking");
				zend_native_release_source_operand(source, operand_type);
				return FAILURE;
			}
			target = ZEND_CALL_ARG(call, argument_number);
			ZEND_CALL_NUM_ARGS(call)++;
		}

		if (ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
			if (Z_ISREF_P(argument)) {
				Z_ADDREF_P(argument);
				ZVAL_REF(target, Z_REF_P(argument));
			} else if (can_reference_buckets) {
				ZVAL_MAKE_REF_EX(argument, 2);
				ZVAL_REF(target, Z_REF_P(argument));
			} else {
				Z_TRY_ADDREF_P(argument);
				ZVAL_NEW_REF(target, argument);
			}
		} else {
			ZVAL_COPY_DEREF(target, argument);
		}
		argument_number++;
	} ZEND_HASH_FOREACH_END();

	zend_native_release_source_operand(source, operand_type);
	return EG(exception) == NULL ? SUCCESS : FAILURE;
}

static zend_result zend_native_call_unpack_traversable(
	zend_execute_data *caller, zval *source, uint8_t operand_type)
{
	zend_execute_data *call = caller->call;
	zend_function *function = call->func;
	zval *args = source;
	zend_class_entry *class_entry;
	zend_object_iterator *iterator;
	const zend_object_iterator_funcs *functions;
	uint32_t argument_number = ZEND_CALL_NUM_ARGS(call) + 1;
	bool have_named_parameters = false;

	while (Z_ISREF_P(args)) {
		args = Z_REFVAL_P(args);
	}
	if (Z_TYPE_P(args) != IS_OBJECT) {
		return FAILURE;
	}
	class_entry = Z_OBJCE_P(args);
	if (class_entry->get_iterator == NULL) {
		zend_type_error("Only arrays and Traversables can be unpacked, %s given",
			zend_zval_value_name(args));
		zend_native_release_source_operand(source, operand_type);
		return FAILURE;
	}
	iterator = class_entry->get_iterator(class_entry, args, 0);
	if (iterator == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_exception_ex(NULL, 0,
				"Object of type %s did not create an Iterator",
				ZSTR_VAL(class_entry->name));
		}
		zend_native_release_source_operand(source, operand_type);
		return FAILURE;
	}
	functions = iterator->funcs;
	if (functions->rewind != NULL) {
		functions->rewind(iterator);
	}
	while (EG(exception) == NULL && functions->valid(iterator) == SUCCESS) {
		zval *argument = functions->get_current_data(iterator);
		zval *target;
		zend_string *name = NULL;
		zend_string *key_string = NULL;

		if (EG(exception) != NULL || argument == NULL) {
			if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Traversable returned no value during argument unpacking");
			}
			break;
		}
		if (functions->get_current_key != NULL) {
			zval key;
			ZVAL_UNDEF(&key);
			functions->get_current_key(iterator, &key);
			if (EG(exception) != NULL) {
				if (!Z_ISUNDEF(key)) {
					zval_ptr_dtor(&key);
				}
				break;
			}
			if (Z_TYPE(key) == IS_STRING) {
				zend_ulong numeric_key;
				key_string = Z_STR(key);
				if (!ZEND_HANDLE_NUMERIC(key_string, numeric_key)) {
					name = key_string;
				}
			} else if (Z_TYPE(key) != IS_LONG) {
				zend_throw_error(NULL,
					"Keys must be of type int|string during argument unpacking");
				zval_ptr_dtor(&key);
				break;
			}
		}

		if (name != NULL) {
			void *cache_slot[2] = {NULL, NULL};
			have_named_parameters = true;
			target = zend_handle_named_arg(
				&call, name, &argument_number, cache_slot);
			caller->call = call;
			if (target == NULL) {
				zend_string_release(key_string);
				break;
			}
		} else {
			if (key_string != NULL) {
				zend_string_release(key_string);
				key_string = NULL;
			}
			if (have_named_parameters) {
				zend_throw_error(NULL,
					"Cannot use positional argument after named argument during unpacking");
				break;
			}
			zend_vm_stack_extend_call_frame(
				&call, argument_number - 1, 1);
			caller->call = call;
			target = ZEND_CALL_ARG(call, argument_number);
			ZEND_CALL_NUM_ARGS(call)++;
		}

		ZVAL_DEREF(argument);
		Z_TRY_ADDREF_P(argument);
		if (ARG_MUST_BE_SENT_BY_REF(function, argument_number)) {
			zend_native_traversable_by_reference_warning(
				function, argument_number);
			ZVAL_NEW_REF(target, argument);
		} else {
			ZVAL_COPY_VALUE(target, argument);
		}
		if (key_string != NULL) {
			zend_string_release(key_string);
		}
		if (EG(exception) != NULL) {
			break;
		}
		functions->move_forward(iterator);
		argument_number++;
	}
	zend_iterator_dtor(iterator);
	zend_native_release_source_operand(source, operand_type);
	return EG(exception) == NULL ? SUCCESS : FAILURE;
}

static zend_result zend_native_call_send_unpack(
	zend_execute_data *caller, zval *source, uint8_t operand_type)
{
	zval *args = source;

	while (Z_ISREF_P(args)) {
		args = Z_REFVAL_P(args);
	}
	if (Z_TYPE_P(args) == IS_ARRAY) {
		return zend_native_call_unpack_array(caller, source, operand_type);
	}
	if (Z_TYPE_P(args) == IS_OBJECT) {
		return zend_native_call_unpack_traversable(caller, source, operand_type);
	}
	if (operand_type == IS_CV && Z_TYPE_P(args) == IS_UNDEF) {
		zend_throw_error(NULL, "Only arrays and Traversables can be unpacked, null given");
	} else {
		zend_type_error("Only arrays and Traversables can be unpacked, %s given",
			zend_zval_value_name(args));
	}
	zend_native_release_source_operand(source, operand_type);
	return FAILURE;
}

static zend_result zend_native_call_send_user(
	zend_execute_data *caller, const zend_op *send,
	zval *source, uint8_t operand_type)
{
	zend_execute_data *call = caller->call;
	zend_function *function = call->func;
	uint32_t argument_number = send->op2.num;
	zval *argument = source;
	zval *target;

	if (argument_number == 0
			|| argument_number > ZEND_CALL_NUM_ARGS(call)) {
		return FAILURE;
	}
	target = ZEND_CALL_VAR(call, send->result.var);
	ZVAL_DEREF(argument);
	if (ARG_MUST_BE_SENT_BY_REF(function, argument_number)) {
		zend_param_must_be_ref(function, argument_number);
		Z_TRY_ADDREF_P(argument);
		ZVAL_NEW_REF(target, argument);
	} else {
		ZVAL_COPY(target, argument);
	}
	zend_native_release_source_operand(source, operand_type);
	return EG(exception) == NULL ? SUCCESS : FAILURE;
}

static zval *zend_native_source_op2(
	zend_execute_data *caller, const zend_op *send)
{
	switch (send->op2_type) {
		case IS_CONST:
			return (zval *) RT_CONSTANT(send, send->op2);
		case IS_CV:
		case IS_VAR:
		case IS_TMP_VAR:
			return ZEND_CALL_VAR(caller, send->op2.var);
		default:
			return NULL;
	}
}

static void zend_native_send_array_copy_argument(
	zend_function *function, uint32_t argument_number,
	zval *target, zval *argument)
{
	bool wrap_reference = false;

	if (ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
		if (!Z_ISREF_P(argument)
				&& !ARG_MAY_BE_SENT_BY_REF(function, argument_number)) {
			zend_param_must_be_ref(function, argument_number);
			wrap_reference = true;
		}
	} else if (Z_ISREF_P(argument)
			&& (function->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) == 0) {
		argument = Z_REFVAL_P(argument);
	}
	if (!wrap_reference) {
		ZVAL_COPY(target, argument);
	} else {
		Z_TRY_ADDREF_P(argument);
		ZVAL_NEW_REF(target, argument);
	}
}

static zend_result zend_native_call_send_array(
	zend_execute_data *caller, const zend_op *send,
	zval *source, uint8_t operand_type)
{
	zend_execute_data *call = caller->call;
	zend_function *function = call->func;
	zval *args = source;
	HashTable *table;
	zval *argument;

	while (Z_ISREF_P(args)) {
		args = Z_REFVAL_P(args);
	}
	if (Z_TYPE_P(args) != IS_ARRAY) {
		zend_type_error(
			"call_user_func_array(): Argument #2 ($args) must be of type array, %s given",
			zend_zval_value_name(args));
		zend_native_release_source_operand(source, operand_type);
		return FAILURE;
	}
	table = Z_ARRVAL_P(args);
	if (send->op2_type != IS_UNUSED) {
		zval *length_source = zend_native_source_op2(caller, send);
		zval *length_value = length_source;
		uint32_t skip = send->extended_value;
		uint32_t count = zend_hash_num_elements(table);
		zend_long length;

		if (length_source == NULL) {
			zend_native_release_source_operand(source, operand_type);
			return FAILURE;
		}
		ZVAL_DEREF(length_value);
		if (Z_TYPE_P(length_value) == IS_LONG) {
			length = Z_LVAL_P(length_value);
		} else if (Z_TYPE_P(length_value) == IS_NULL) {
			length = skip < count ? (zend_long) (count - skip) : 0;
		} else if (ZEND_CALL_USES_STRICT_TYPES(caller)
				|| !zend_parse_arg_long_weak(length_value, &length, 3)) {
			zend_type_error(
				"array_slice(): Argument #3 ($length) must be of type ?int, %s given",
				zend_zval_value_name(length_value));
			zend_native_release_source_operand(
				length_source, send->op2_type);
			zend_native_release_source_operand(source, operand_type);
			return FAILURE;
		}

		if (length < 0) {
			zend_long remaining = skip < count
				? (zend_long) (count - skip) : 0;
			length += remaining;
		}
		if (skip < count && length > 0) {
			uint32_t argument_number = 1;
			zval *target;

			if (length > (zend_long) (count - skip)) {
				length = (zend_long) (count - skip);
			}
			zend_vm_stack_extend_call_frame(
				&call, 0, (uint32_t) length);
			caller->call = call;
			target = ZEND_CALL_ARG(call, 1);
			ZEND_HASH_FOREACH_VAL(table, argument) {
				if (skip > 0) {
					skip--;
					continue;
				}
				if ((zend_long) (argument_number - 1) >= length) {
					break;
				}
				zend_native_send_array_copy_argument(
					function, argument_number, target, argument);
				ZEND_CALL_NUM_ARGS(call)++;
				argument_number++;
				target++;
			} ZEND_HASH_FOREACH_END();
		}
		zend_native_release_source_operand(
			length_source, send->op2_type);
	} else {
		zend_string *name;
		uint32_t argument_number = 1;
		zval *target;
		bool have_named_parameters = false;

		zend_vm_stack_extend_call_frame(
			&call, 0, zend_hash_num_elements(table));
		caller->call = call;
		target = ZEND_CALL_ARG(call, 1);
		ZEND_HASH_FOREACH_STR_KEY_VAL(table, name, argument) {
			if (name != NULL) {
				void *cache_slot[2] = {NULL, NULL};
				have_named_parameters = true;
				target = zend_handle_named_arg(
					&call, name, &argument_number, cache_slot);
				caller->call = call;
				if (target == NULL) {
					zend_native_release_source_operand(source, operand_type);
					return FAILURE;
				}
			} else if (have_named_parameters) {
				zend_throw_error(NULL,
					"Cannot use positional argument after named argument");
				zend_native_release_source_operand(source, operand_type);
				return FAILURE;
			}

			zend_native_send_array_copy_argument(
				function, argument_number, target, argument);
			if (name == NULL) {
				ZEND_CALL_NUM_ARGS(call)++;
				argument_number++;
				target++;
			}
		} ZEND_HASH_FOREACH_END();
	}
	zend_native_release_source_operand(source, operand_type);
	return EG(exception) == NULL ? SUCCESS : FAILURE;
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
	const zend_op *send;
	zval *target;
	uint32_t argument_number;
	zval *value;

	if (EG(exception) != NULL || caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| caller->call == NULL
			|| send_opline_index >= caller->func->op_array.last
			|| mode > ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER) {
		return FAILURE;
	}
	send = &caller->func->op_array.opcodes[send_opline_index];
	call = caller->call;
	caller->opline = send;
	if (mode == ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER) {
		if (send->opcode != ZEND_SEND_PLACEHOLDER) {
			return FAILURE;
		}
		if (send->op2_type == IS_CONST) {
			zval *name = RT_CONSTANT(send, send->op2);
			void **cache_slot;

			if (Z_TYPE_P(name) != IS_STRING
					|| caller->run_time_cache == NULL
					|| send->result.num > caller->func->op_array.cache_size
					|| 2 * sizeof(void *)
						> caller->func->op_array.cache_size
							- send->result.num) {
				return FAILURE;
			}
			cache_slot = (void **) ((char *) caller->run_time_cache
				+ send->result.num);
			target = zend_handle_named_arg(
				&call, Z_STR_P(name), &argument_number, cache_slot);
			caller->call = call;
			if (target == NULL) {
				return FAILURE;
			}
		} else if (send->op2_type == IS_UNUSED) {
			target = ZEND_CALL_VAR(call, send->result.var);
		} else {
			return FAILURE;
		}
		Z_TYPE_INFO_P(target) = _IS_PLACEHOLDER;
		return SUCCESS;
	}
	value = zend_native_source_argument(
		caller, send_opline_index, &mutable_value, &operand_type);
	if (value == NULL
			|| mode > ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		return FAILURE;
	}
	function = call->func;
	if (function == NULL
			|| (function->type != ZEND_INTERNAL_FUNCTION
				&& function->type != ZEND_USER_FUNCTION)) {
		return FAILURE;
	}
	if (send->opcode == ZEND_SEND_UNPACK) {
		return zend_native_call_send_unpack(
			caller, value, operand_type);
	}
	if (send->opcode == ZEND_SEND_ARRAY) {
		return zend_native_call_send_array(
			caller, send, value, operand_type);
	}
	if (send->opcode == ZEND_SEND_USER) {
		return zend_native_call_send_user(
			caller, send, value, operand_type);
	}
	if (send->opcode == ZEND_SEND_FUNC_ARG) {
		bool send_by_reference;

		if (operand_type != IS_VAR) {
			zend_native_release_source_operand(value, operand_type);
			return FAILURE;
		}
		if (send->op2_type == IS_CONST) {
			zval *name = RT_CONSTANT(send, send->op2);
			void **cache_slot;

			if (Z_TYPE_P(name) != IS_STRING
					|| caller->run_time_cache == NULL
					|| send->result.num > caller->func->op_array.cache_size
					|| 2 * sizeof(void *)
						> caller->func->op_array.cache_size
							- send->result.num) {
				zend_native_release_source_operand(value, operand_type);
				return FAILURE;
			}
			cache_slot = (void **) ((char *) caller->run_time_cache
				+ send->result.num);
			target = zend_handle_named_arg(
				&call, Z_STR_P(name), &argument_number, cache_slot);
			caller->call = call;
			if (target == NULL) {
				zend_native_release_source_operand(value, operand_type);
				return FAILURE;
			}
		} else if (send->op2_type == IS_UNUSED) {
			argument_number = send->op2.num;
			if (argument_number == 0) {
				argument_number = ordinal + 1;
			}
			target = ZEND_CALL_VAR(call, send->result.var);
		} else {
			zend_native_release_source_operand(value, operand_type);
			return FAILURE;
		}
		send_by_reference =
			ARG_SHOULD_BE_SENT_BY_REF(function, argument_number);
		if (send_by_reference) {
			if (Z_ISREF_P(value)) {
				Z_ADDREF_P(value);
			} else {
				if (!mutable_value) {
					zend_cannot_pass_by_reference(argument_number);
					zend_native_release_source_operand(
						value, operand_type);
					return FAILURE;
				}
				ZVAL_MAKE_REF_EX(value, 2);
			}
			ZVAL_REF(target, Z_REF_P(value));
			zend_native_release_source_operand(value, operand_type);
			return EG(exception) == NULL ? SUCCESS : FAILURE;
		}
		if (Z_ISREF_P(value)) {
			zend_refcounted *reference = Z_COUNTED_P(value);
			zval *referent = Z_REFVAL_P(value);

			ZVAL_COPY_VALUE(target, referent);
			if (GC_DELREF(reference) == 0) {
				efree_size(reference, sizeof(zend_reference));
			} else if (Z_OPT_REFCOUNTED_P(target)) {
				Z_ADDREF_P(target);
			}
		} else {
			ZVAL_COPY_VALUE(target, value);
		}
		ZVAL_UNDEF(value);
		return SUCCESS;
	}
	if (send->op2_type == IS_CONST) {
		zval *name = RT_CONSTANT(send, send->op2);
		void *cache_slot[2] = {NULL, NULL};
		if (Z_TYPE_P(name) != IS_STRING) {
			return FAILURE;
		}
		target = zend_handle_named_arg(
			&call, Z_STR_P(name), &argument_number, cache_slot);
		caller->call = call;
		if (target == NULL) {
			return FAILURE;
		}
	} else {
		argument_number = send->op2.num != 0
			? send->op2.num : ordinal + 1;
		if (argument_number == 0
				|| argument_number > ZEND_CALL_NUM_ARGS(call)) {
			return FAILURE;
		}
		target = ZEND_CALL_ARG(call, argument_number);
	}
	/*
	 * SEND_VAR_EX and SEND_REF materialize a reference in the canonical
	 * caller slot when the resolved parameter requires one.  The native
	 * path must perform the same mutation before the argument is copied into
	 * the internal call frame; literals can never become by-reference args.
	 */
	if ((mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			|| ARG_SHOULD_BE_SENT_BY_REF(function, argument_number))
			&& !Z_ISREF_P(value)) {
		if (!mutable_value) {
			zend_cannot_pass_by_reference(argument_number);
			return FAILURE;
		}
		ZVAL_MAKE_REF(value);
	}
	if (mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
			|| ARG_SHOULD_BE_SENT_BY_REF(function, argument_number)) {
		ZVAL_COPY(target, value);
		return SUCCESS;
	}
	if ((send->opcode == ZEND_SEND_VAR_NO_REF
			|| send->opcode == ZEND_SEND_VAR_NO_REF_EX)
			&& !Z_ISREF_P(value)) {
		ZVAL_COPY(target, value);
		ZVAL_NEW_REF(target, target);
		zend_error(E_NOTICE, "Only variables should be passed by reference");
		return EG(exception) == NULL ? SUCCESS : FAILURE;
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
	state->observer_started = false;
	state->observer_finished = false;
	ZVAL_NULL(state->return_value);
	EG(current_execute_data) = state->call;

	zend_try {
		if (EG(exception) != NULL) {
			state->status = ZEND_NATIVE_EXCEPTION;
		} else if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_MAY_HAVE_UNDEF) != 0
				&& zend_handle_undef_args(state->call) == FAILURE) {
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} else if (!zend_native_internal_count_is_valid(
				state->call->func, ZEND_CALL_NUM_ARGS(state->call))) {
			zend_wrong_parameters_count_error(
				state->call->func->common.required_num_args,
				(state->call->func->common.fn_flags & ZEND_ACC_VARIADIC) != 0
					? (uint32_t) -1 : state->call->func->common.num_args);
			state->status = ZEND_NATIVE_EXCEPTION;
		} else {
			state->observer_started = true;
			ZEND_OBSERVER_FCALL_BEGIN(state->call);
			if (EXPECTED(zend_execute_internal == NULL)) {
				state->call->func->internal_function.handler(
					state->call, state->return_value);
			} else {
				zend_execute_internal(state->call, state->return_value);
			}
			state->status = EG(exception) == NULL
				? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
			ZEND_OBSERVER_FCALL_END(state->call,
				state->status == ZEND_NATIVE_RETURNED
					? state->return_value : NULL);
			state->observer_finished = true;
			if (UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
				zend_fcall_interrupt(state->call);
				if (EG(exception) != NULL) {
					state->status = ZEND_NATIVE_EXCEPTION;
				}
			}
		}
	} zend_catch {
		state->status = EG(exception) != NULL
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	} zend_end_try();

	if (state->observer_started && !state->observer_finished) {
		zend_try {
			ZEND_OBSERVER_FCALL_END(state->call, NULL);
			state->observer_finished = true;
		} zend_catch {
			state->observer_finished = true;
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} zend_end_try();
	}
	EG(current_execute_data) = state->caller;
	zend_vm_stack_free_args(state->call);
	if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_RELEASE_THIS) != 0) {
		OBJ_RELEASE(Z_OBJ(state->call->This));
	} else if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_CLOSURE) != 0) {
		OBJ_RELEASE(ZEND_CLOSURE_OBJECT(state->call->func));
	}
	if ((state->call->func->common.fn_flags
			& ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0) {
		zend_free_trampoline(state->call->func);
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
			|| !ZEND_USER_CODE(caller->func->type)
			|| do_opline_index >= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &caller->func->op_array.opcodes[do_opline_index];
	if (opline->opcode != ZEND_DO_ICALL
			&& opline->opcode != ZEND_DO_FCALL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (EG(exception) != NULL) {
		ZVAL_UNDEF(&temporary);
		return zend_native_internal_call_invoke_finish(
			caller, cell, &temporary);
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
	status = zend_native_internal_call_invoke_finish(
		caller, cell, return_value);
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL
			&& zend_native_prepare_finally_exception(
				caller, do_opline_index) == FAILURE) {
		status = ZEND_NATIVE_BAILOUT;
	}
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
			|| !ZEND_USER_CODE(caller->func->type)
			|| do_opline_index >= caller->func->op_array.last) {
		goto mismatch;
	}
	opline = &caller->func->op_array.opcodes[do_opline_index];
	if ((opline->opcode != ZEND_DO_ICALL
			&& opline->opcode != ZEND_DO_UCALL
			&& opline->opcode != ZEND_DO_FCALL)
			|| (opline->result_type != IS_CV
				&& opline->result_type != IS_VAR
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

zend_native_status zend_native_return_source_zval(
	zend_execute_data *execute_data, uint32_t return_opline_index)
{
	const zend_op *opline;
	zval *source;
	zval *source_slot;
	zval *return_value;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| return_opline_index >= execute_data->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &execute_data->func->op_array.opcodes[return_opline_index];
	if ((opline->opcode != ZEND_RETURN
			&& opline->opcode != ZEND_RETURN_BY_REF)
			|| (opline->op1_type != IS_CONST && opline->op1_type != IS_CV
				&& opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_VAR)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->opline = opline;
	source_slot = opline->op1_type == IS_CONST
		? RT_CONSTANT(opline, opline->op1)
		: ZEND_CALL_VAR(execute_data, opline->op1.var);
	source = source_slot;
	return_value = execute_data->return_value;
	if (opline->opcode == ZEND_RETURN_BY_REF) {
		bool temporary = opline->op1_type == IS_TMP_VAR
			|| opline->op1_type == IS_VAR;
		if (opline->op1_type == IS_VAR
				&& Z_TYPE_P(source) == IS_INDIRECT) {
			source = Z_INDIRECT_P(source);
		}

		if ((opline->op1_type == IS_CONST
				|| opline->op1_type == IS_TMP_VAR)
				|| (opline->op1_type == IS_VAR
					&& opline->extended_value == ZEND_RETURNS_VALUE)) {
			zend_error(E_NOTICE,
				"Only variable references should be returned by reference");
			if (UNEXPECTED(EG(exception) != NULL)) {
				return ZEND_NATIVE_EXCEPTION;
			}
			if (return_value == NULL) {
				if (temporary && !Z_ISUNDEF_P(source_slot)) {
					zval_ptr_dtor(source_slot);
					ZVAL_UNDEF(source_slot);
				}
				return ZEND_NATIVE_RETURNED;
			}
			if (opline->op1_type == IS_VAR && Z_ISREF_P(source)) {
				ZVAL_COPY_VALUE(return_value, source);
				ZVAL_UNDEF(source_slot);
			} else {
				ZVAL_NEW_REF(return_value, source);
				if (opline->op1_type == IS_CONST) {
					Z_TRY_ADDREF_P(source);
				} else {
					ZVAL_UNDEF(source_slot);
				}
			}
			zend_return_unwrap_ref(execute_data, return_value);
			return ZEND_NATIVE_RETURNED;
		}

		if (opline->op1_type == IS_VAR
				&& opline->extended_value == ZEND_RETURNS_FUNCTION
				&& !Z_ISREF_P(source)) {
			zend_error(E_NOTICE,
				"Only variable references should be returned by reference");
			if (UNEXPECTED(EG(exception) != NULL)) {
				return ZEND_NATIVE_EXCEPTION;
			}
			if (return_value != NULL) {
				ZVAL_NEW_REF(return_value, source);
				ZVAL_UNDEF(source_slot);
			} else if (!Z_ISUNDEF_P(source_slot)) {
				zval_ptr_dtor(source_slot);
				ZVAL_UNDEF(source_slot);
			}
			zend_return_unwrap_ref(execute_data, return_value);
			return ZEND_NATIVE_RETURNED;
		}

		if (return_value != NULL) {
			if (Z_ISREF_P(source)) {
				Z_ADDREF_P(source);
			} else {
				ZVAL_MAKE_REF_EX(source, 2);
			}
			ZVAL_REF(return_value, Z_REF_P(source));
		}
		if (temporary && !Z_ISUNDEF_P(source_slot)) {
			zval_ptr_dtor(source_slot);
			ZVAL_UNDEF(source_slot);
		}
		zend_return_unwrap_ref(execute_data, return_value);
		return ZEND_NATIVE_RETURNED;
	}
	if (opline->op1_type == IS_CV && Z_ISUNDEF_P(source)) {
		if (return_value != NULL) {
			ZVAL_NULL(return_value);
		}
		return ZEND_NATIVE_RETURNED;
	}
	if (return_value == NULL) {
		if (opline->op1_type != IS_CV && !Z_ISUNDEF_P(source)) {
			zval_ptr_dtor(source);
			ZVAL_UNDEF(source);
		}
		return ZEND_NATIVE_RETURNED;
	}
	if (opline->op1_type == IS_CONST) {
		ZVAL_COPY(return_value, source);
	} else if (opline->op1_type == IS_CV || Z_ISREF_P(source)) {
		ZVAL_COPY_DEREF(return_value, source);
		if (opline->op1_type != IS_CV) {
			zval_ptr_dtor(source);
			ZVAL_UNDEF(source);
		}
	} else {
		ZVAL_COPY_VALUE(return_value, source);
		ZVAL_UNDEF(source);
	}
	return ZEND_NATIVE_RETURNED;
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
			|| !ZEND_USER_CODE(execute_data->func->type)
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

static const zend_try_catch_element *zend_native_finally_region(
	const zend_op_array *op_array, uint32_t finally_opline_index)
{
	uint32_t index;

	if (op_array == NULL || finally_opline_index >= op_array->last) {
		return NULL;
	}
	for (index = 0; index < op_array->last_try_catch; index++) {
		const zend_try_catch_element *region = &op_array->try_catch_array[index];
		if (region->finally_op == finally_opline_index
				&& region->finally_end < op_array->last
				&& op_array->opcodes[region->finally_end].opcode == ZEND_FAST_RET) {
			return region;
		}
	}
	return NULL;
}

zend_native_status zend_native_finally_enter(
	zend_execute_data *execute_data, uint32_t finally_opline_index)
{
	const zend_op_array *op_array;
	const zend_try_catch_element *region;
	zval *fast_call;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		return ZEND_NATIVE_BAILOUT;
	}
	op_array = &execute_data->func->op_array;
	region = zend_native_finally_region(op_array, finally_opline_index);
	if (region == NULL) {
		return ZEND_NATIVE_BAILOUT;
	}
	execute_data->opline = &op_array->opcodes[finally_opline_index];
	if (EG(exception) == NULL) {
		return ZEND_NATIVE_RETURNED;
	}
	fast_call = ZEND_CALL_VAR(
		execute_data, op_array->opcodes[region->finally_end].op1.var);
	Z_OBJ_P(fast_call) = EG(exception);
	EG(exception) = NULL;
	Z_OPLINE_NUM_P(fast_call) = UINT32_MAX;
	return ZEND_NATIVE_RETURNED;
}

void zend_native_finally_call(
	zend_execute_data *execute_data, uint32_t fast_call_opline_index)
{
	const zend_op *opline;
	zval *fast_call;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| fast_call_opline_index >= execute_data->func->op_array.last) {
		zend_bailout();
	}
	opline = &execute_data->func->op_array.opcodes[fast_call_opline_index];
	if (opline->opcode != ZEND_FAST_CALL || opline->result_type != IS_TMP_VAR) {
		zend_bailout();
	}
	execute_data->opline = opline;
	fast_call = ZEND_CALL_VAR(execute_data, opline->result.var);
	Z_OBJ_P(fast_call) = NULL;
	Z_OPLINE_NUM_P(fast_call) = fast_call_opline_index;
}

uint32_t zend_native_finally_return(
	zend_execute_data *execute_data, uint32_t fast_ret_opline_index)
{
	const zend_op_array *op_array;
	const zend_op *opline;
	zval *fast_call;
	uint32_t continuation;
	uint32_t selected = ZEND_MIR_ID_INVALID;
	uint32_t handler = ZEND_MIR_ID_INVALID;
	uint32_t index;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| fast_ret_opline_index >= execute_data->func->op_array.last) {
		return UINT32_MAX;
	}
	op_array = &execute_data->func->op_array;
	opline = &op_array->opcodes[fast_ret_opline_index];
	if (opline->opcode != ZEND_FAST_RET || opline->op1_type != IS_TMP_VAR) {
		return UINT32_MAX;
	}
	execute_data->opline = opline;
	fast_call = ZEND_CALL_VAR(execute_data, opline->op1.var);
	continuation = Z_OPLINE_NUM_P(fast_call);
	if (continuation != UINT32_MAX) {
		return continuation;
	}
	EG(exception) = Z_OBJ_P(fast_call);
	Z_OBJ_P(fast_call) = NULL;
	/* Continue zend_dispatch_try_catch_finally_helper outside this finally. */
	for (index = 0; index < op_array->last_try_catch; index++) {
		const zend_try_catch_element *region = &op_array->try_catch_array[index];
		if (region->try_op <= fast_ret_opline_index
				&& ((region->catch_op != 0
						&& fast_ret_opline_index < region->catch_op)
					|| (region->finally_end != 0
						&& fast_ret_opline_index < region->finally_end))) {
			selected = index;
		}
	}
	while (zend_mir_id_is_valid(selected)) {
		const zend_try_catch_element *region = &op_array->try_catch_array[selected];
		if (region->catch_op != 0 && fast_ret_opline_index < region->catch_op) {
			handler = region->catch_op;
			break;
		}
		if (region->finally_op != 0
				&& fast_ret_opline_index < region->finally_op) {
			handler = region->finally_op;
			break;
		}
		selected = selected == 0 ? ZEND_MIR_ID_INVALID : selected - 1;
	}
	return zend_mir_id_is_valid(handler)
			&& handler < ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
		? ZEND_NATIVE_FINALLY_EXCEPTION_FLAG | handler
		: ZEND_NATIVE_FINALLY_PROPAGATE;
}

void zend_native_interrupt_poll(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opline_index >= execute_data->func->op_array.last) {
		zend_throw_error(NULL, "Invalid native interrupt source position");
		zend_bailout();
	}
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_opline_index];
	if (UNEXPECTED(zend_atomic_bool_load_ex(&EG(vm_interrupt)))) {
		zend_fcall_interrupt(execute_data);
		/*
		 * Generated code has no VM exception-dispatch continuation at an
		 * asynchronous backedge. Transfer to the C-only native frame boundary,
		 * which preserves EG(exception) and restores the complete Zend frame
		 * chain before returning to the caller.
		 */
		if (UNEXPECTED(EG(exception) != NULL)) {
			zend_bailout();
		}
	}
}
