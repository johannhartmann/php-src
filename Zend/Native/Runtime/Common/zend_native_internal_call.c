/* Direct calls to compile-time resolved Zend internal functions. */

#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"
#include "Zend/Native/Runtime/Common/zend_native_values.h"

#include "Zend/zend_exceptions.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_generators.h"
#include "Zend/zend_observer.h"

#include <stdlib.h>
#include <string.h>

typedef struct _zend_native_internal_execution_state {
	zend_execute_data *caller;
	zend_execute_data *call;
	zval *return_value;
	zend_native_status status;
	bool observer_started;
	bool observer_finished;
} zend_native_internal_execution_state;

typedef struct _zend_native_internal_top_execution_state {
	zend_execute_data *caller;
	zend_execute_data *call;
	zval *return_value;
	zval discarded_return;
	zend_native_status status;
	bool uses_discarded_return;
	bool observer_started;
	bool observer_finished;
} zend_native_internal_top_execution_state;

static uint64_t zend_native_internal_encode_source_operand(
	const zend_mir_source_operand_ref *operand)
{
	return ((uint64_t) operand->kind & UINT64_C(0xff))
		| (((uint64_t) operand->slot_kind & UINT64_C(0xff)) << 8)
		| ((uint64_t) operand->index << 16);
}

static bool zend_native_internal_call_preflight(
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
	if (mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		ZVAL_COPY(target, source);
	} else {
		zend_native_zval_copy_deref_or_dup(target, source);
	}
	return SUCCESS;
}

static zval *zend_native_explicit_operand(
	zend_execute_data *caller,
	const zend_mir_source_operand_ref *operand,
	bool allow_literal,
	bool *mutable_value,
	uint8_t *operand_type)
{
	const zend_op_array *op_array;
	uint32_t physical_slot;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| operand == NULL || mutable_value == NULL
			|| operand_type == NULL) {
		return NULL;
	}
	op_array = &caller->func->op_array;
	*mutable_value = false;
	*operand_type = IS_UNUSED;
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		if (!allow_literal || operand->index >= op_array->last_literal) {
			return NULL;
		}
		*operand_type = IS_CONST;
		return &op_array->literals[operand->index];
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
			*operand_type = IS_CV;
			break;
		case ZEND_MIR_SOURCE_SLOT_TMP:
		case ZEND_MIR_SOURCE_SLOT_VAR:
			if (operand->index >= op_array->T) {
				return NULL;
			}
			physical_slot = (uint32_t) op_array->last_var + operand->index;
			*operand_type = operand->slot_kind == ZEND_MIR_SOURCE_SLOT_TMP
				? IS_TMP_VAR : IS_VAR;
			break;
		default:
			return NULL;
	}
	*mutable_value = true;
	return ZEND_CALL_VAR_NUM(caller, physical_slot);
}

static void zend_native_release_source_operand(
	zval *value, uint8_t operand_type);

static zend_result zend_native_internal_call_begin_explicit(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor)
{
	zend_execute_data *call;
	zend_function *function;
	void *object_or_called_scope = NULL;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t index;

	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| cell == NULL || cell->function == NULL
			|| cell->function->type != ZEND_INTERNAL_FUNCTION
			|| descriptor == NULL
			|| descriptor->init_source_position >= caller->func->op_array.last
			|| descriptor->initial_argument_count
				> descriptor->argument_count) {
		return FAILURE;
	}
	function = cell->function;
	if (cell->receiver_kind == ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS) {
		if ((ZEND_CALL_INFO(caller) & ZEND_CALL_HAS_THIS) == 0
				|| function->common.scope == NULL
				|| !instanceof_function(
					Z_OBJCE(caller->This), function->common.scope)) {
			return FAILURE;
		}
		object_or_called_scope = Z_OBJ(caller->This);
		call_info |= ZEND_CALL_HAS_THIS;
	} else if (cell->receiver_kind
			== ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE) {
		if (function->common.scope == NULL || cell->called_scope == NULL
				|| !instanceof_function(
					cell->called_scope, function->common.scope)) {
			return FAILURE;
		}
		if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0) {
			if (Z_TYPE(caller->This) != IS_OBJECT
					|| !instanceof_function(
						Z_OBJCE(caller->This), function->common.scope)) {
				zend_non_static_method_call(function);
				return FAILURE;
			}
			object_or_called_scope = Z_OBJ(caller->This);
			call_info |= ZEND_CALL_HAS_THIS;
		} else {
			object_or_called_scope = cell->called_scope;
		}
	} else if (cell->receiver_kind
			== ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT) {
		bool mutable_receiver;
		uint8_t receiver_type;
		zval *source_receiver = zend_native_explicit_operand(
			caller, &descriptor->receiver_operand, false,
			&mutable_receiver, &receiver_type);
		zval *receiver = source_receiver;

		if (receiver == NULL
				|| (receiver_type != IS_CV && receiver_type != IS_VAR
					&& receiver_type != IS_TMP_VAR)
				|| function->common.scope == NULL) {
			return FAILURE;
		}
		ZVAL_DEREF(receiver);
		if (Z_TYPE_P(receiver) != IS_OBJECT
				|| !instanceof_function(
					Z_OBJCE_P(receiver), function->common.scope)) {
			return FAILURE;
		}
		object_or_called_scope = Z_OBJ_P(receiver);
		GC_ADDREF((zend_object *) object_or_called_scope);
		call_info |= ZEND_CALL_HAS_THIS | ZEND_CALL_RELEASE_THIS;
		zend_native_release_source_operand(source_receiver, receiver_type);
	} else if (cell->receiver_kind != ZEND_NATIVE_INTERNAL_RECEIVER_NONE
			|| descriptor->receiver_operand.kind
				!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return FAILURE;
	}

#ifdef ZEND_CHECK_STACK_LIMIT
	if (UNEXPECTED(zend_call_stack_overflowed(EG(stack_limit)))) {
		zend_call_stack_size_error();
		if ((call_info & ZEND_CALL_RELEASE_THIS) != 0) {
			OBJ_RELEASE((zend_object *) object_or_called_scope);
		}
		return FAILURE;
	}
#endif
	call = zend_vm_stack_push_call_frame(
		call_info, function, descriptor->initial_argument_count,
		object_or_called_scope);
	for (index = 0; index < descriptor->initial_argument_count; index++) {
		ZVAL_UNDEF(ZEND_CALL_ARG(call, index + 1));
	}
	call->prev_execute_data = caller->call;
	caller->call = call;
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->init_source_position];
	return SUCCESS;
}

zend_result zend_native_internal_call_begin(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor)
{
	return zend_native_internal_call_begin_explicit(caller, cell, descriptor);
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
	zend_native_call_publish_moved_frame(caller, call);

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
			zend_native_call_publish_moved_frame(caller, call);
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
				zval copy;
				zend_native_zval_copy_deref_or_dup(&copy, argument);
				ZVAL_NEW_REF(target, &copy);
			}
		} else {
			zend_native_zval_copy_deref_or_dup(target, argument);
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
			zend_native_call_publish_moved_frame(caller, call);
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
			zend_native_call_publish_moved_frame(caller, call);
			target = ZEND_CALL_ARG(call, argument_number);
			ZEND_CALL_NUM_ARGS(call)++;
		}

		ZVAL_DEREF(argument);
		if (ARG_MUST_BE_SENT_BY_REF(function, argument_number)) {
			zval copy;
			zend_native_zval_copy_deref_or_dup(&copy, argument);
			zend_native_traversable_by_reference_warning(
				function, argument_number);
			ZVAL_NEW_REF(target, &copy);
		} else {
			zend_native_zval_copy_deref_or_dup(target, argument);
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
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument_descriptor,
	zval *source, uint8_t operand_type)
{
	zend_execute_data *call = caller->call;
	zend_function *function = call->func;
	uint32_t argument_number = argument_descriptor->auxiliary_payload;
	zval *argument = source;
	zval *target;

	if (argument_number == 0
			|| argument_number > ZEND_CALL_NUM_ARGS(call)) {
		return FAILURE;
	}
	target = ZEND_CALL_VAR(call, argument_descriptor->result_payload);
	ZVAL_DEREF(argument);
	if (ARG_MUST_BE_SENT_BY_REF(function, argument_number)) {
		zval copy;
		zend_param_must_be_ref(function, argument_number);
		zend_native_zval_copy_deref_or_dup(&copy, argument);
		ZVAL_NEW_REF(target, &copy);
	} else {
		zend_native_zval_copy_deref_or_dup(target, argument);
	}
	zend_native_release_source_operand(source, operand_type);
	return EG(exception) == NULL ? SUCCESS : FAILURE;
}

static zval *zend_native_source_op2(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument,
	uint8_t *operand_type)
{
	bool mutable_value;

	return zend_native_explicit_operand(
		caller, &argument->auxiliary_operand, true,
		&mutable_value, operand_type);
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
	if (!wrap_reference && !Z_ISREF_P(argument)
			&& !ARG_SHOULD_BE_SENT_BY_REF(
			function, argument_number)) {
		zend_native_zval_copy_deref_or_dup(target, argument);
	} else if (!wrap_reference) {
		/* CALL_VIA_TRAMPOLINE retains reference wrappers until __call()
		 * repacks the raw arguments. Non-trampoline references were already
		 * replaced with their referent above. */
		ZVAL_COPY(target, argument);
	} else {
		zval copy;
		zend_native_zval_copy_deref_or_dup(&copy, argument);
		ZVAL_NEW_REF(target, &copy);
	}
}

static zend_result zend_native_call_send_array(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument_descriptor,
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
	if (argument_descriptor->auxiliary_operand.kind
			!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		uint8_t length_operand_type;
		zval *length_source = zend_native_source_op2(
			caller, argument_descriptor, &length_operand_type);
		zval *length_value = length_source;
		uint32_t skip = argument_descriptor->extended_value;
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
				length_source, length_operand_type);
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
			zend_native_call_publish_moved_frame(caller, call);
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
			length_source, length_operand_type);
	} else {
		zend_string *name;
		uint32_t argument_number = 1;
		zval *target;
		bool have_named_parameters = false;

		zend_vm_stack_extend_call_frame(
			&call, 0, zend_hash_num_elements(table));
		zend_native_call_publish_moved_frame(caller, call);
		target = ZEND_CALL_ARG(call, 1);
		ZEND_HASH_FOREACH_STR_KEY_VAL(table, name, argument) {
			if (name != NULL) {
				void *cache_slot[2] = {NULL, NULL};
				have_named_parameters = true;
				target = zend_handle_named_arg(
					&call, name, &argument_number, cache_slot);
				zend_native_call_publish_moved_frame(caller, call);
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

static zval *zend_native_direct_internal_argument_target(
		zend_execute_data *caller,
		const zend_native_direct_internal_call_argument *argument,
		uint32_t *argument_number)
{
	zend_execute_data *call = caller->call;
	zval *target;

	if (argument->auxiliary_operand.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		uint8_t auxiliary_type;
		zval *name = zend_native_source_op2(
			caller, argument, &auxiliary_type);
		void *cache_slot[2] = {NULL, NULL};

		if (name == NULL || auxiliary_type != IS_CONST
				|| Z_TYPE_P(name) != IS_STRING) {
			return NULL;
		}
		target = zend_handle_named_arg(
			&call, Z_STR_P(name), argument_number, cache_slot);
		zend_native_call_publish_moved_frame(caller, call);
		return target;
	}
	if (argument->auxiliary_operand.kind
			!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return NULL;
	}
	*argument_number = argument->auxiliary_payload != 0
		? argument->auxiliary_payload : argument->ordinal + 1;
	if (*argument_number == 0
			|| *argument_number > ZEND_CALL_NUM_ARGS(call)) {
		return NULL;
	}
	return ZEND_CALL_ARG(call, *argument_number);
}

zend_result zend_native_call_set_explicit_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_argument *argument)
{
	bool mutable_value;
	uint8_t operand_type;
	zend_execute_data *call;
	zend_function *function;
	zval *target;
	uint32_t argument_number;
	zval *value;
	zval *source_value;
	bool indirect_value;
	bool direct_send_by_reference;

	if (EG(exception) != NULL || caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| caller->call == NULL
			|| argument == NULL
			|| argument->source_position >= caller->func->op_array.last
			|| argument->mode > ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER) {
		return FAILURE;
	}
	call = caller->call;
	caller->opline = &caller->func->op_array.opcodes[
		argument->source_position];
	if (argument->mode == ZEND_NATIVE_CALL_ARGUMENT_PLACEHOLDER) {
		if (argument->source_opcode != ZEND_SEND_PLACEHOLDER) {
			return FAILURE;
		}
		if (argument->auxiliary_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			uint8_t auxiliary_type;
			zval *name = zend_native_source_op2(
				caller, argument, &auxiliary_type);
			void **cache_slot;

			if (name == NULL || auxiliary_type != IS_CONST
					|| Z_TYPE_P(name) != IS_STRING
					|| caller->run_time_cache == NULL
					|| argument->result_payload
						> caller->func->op_array.cache_size
					|| 2 * sizeof(void *)
						> caller->func->op_array.cache_size
							- argument->result_payload) {
				return FAILURE;
			}
			cache_slot = (void **) ((char *) caller->run_time_cache
				+ argument->result_payload);
			target = zend_handle_named_arg(
				&call, Z_STR_P(name), &argument_number, cache_slot);
			zend_native_call_publish_moved_frame(caller, call);
			if (target == NULL) {
				return FAILURE;
			}
		} else if (argument->auxiliary_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			target = ZEND_CALL_VAR(call, argument->result_payload);
		} else {
			return FAILURE;
		}
		Z_TYPE_INFO_P(target) = _IS_PLACEHOLDER;
		return SUCCESS;
	}
	value = zend_native_explicit_operand(
		caller, &argument->source_operand, true,
		&mutable_value, &operand_type);
	if (value == NULL
			|| argument->mode > ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE) {
		return FAILURE;
	}
	source_value = value;
	indirect_value = Z_TYPE_P(value) == IS_INDIRECT;
	if (indirect_value) {
		value = Z_INDIRECT_P(value);
	}
	function = call->func;
	if (function == NULL
			|| (function->type != ZEND_INTERNAL_FUNCTION
				&& function->type != ZEND_USER_FUNCTION)) {
		return FAILURE;
	}
	if (argument->source_opcode == ZEND_SEND_UNPACK) {
		return zend_native_call_send_unpack(
			caller, value, operand_type);
	}
	if (argument->source_opcode == ZEND_SEND_ARRAY) {
		return zend_native_call_send_array(
			caller, argument, value, operand_type);
	}
	if (argument->source_opcode == ZEND_SEND_USER) {
		return zend_native_call_send_user(
			caller, argument, value, operand_type);
	}
	if (argument->source_opcode == ZEND_SEND_FUNC_ARG) {
		bool send_by_reference;

		if (operand_type != IS_VAR) {
			zend_native_release_source_operand(source_value, operand_type);
			return FAILURE;
		}
		if (argument->auxiliary_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_LITERAL) {
			uint8_t auxiliary_type;
			zval *name = zend_native_source_op2(
				caller, argument, &auxiliary_type);
			void **cache_slot;

			if (name == NULL || auxiliary_type != IS_CONST
					|| Z_TYPE_P(name) != IS_STRING
					|| caller->run_time_cache == NULL
					|| argument->result_payload
						> caller->func->op_array.cache_size
					|| 2 * sizeof(void *)
						> caller->func->op_array.cache_size
							- argument->result_payload) {
				zend_native_release_source_operand(
					source_value, operand_type);
				return FAILURE;
			}
			cache_slot = (void **) ((char *) caller->run_time_cache
				+ argument->result_payload);
			target = zend_handle_named_arg(
				&call, Z_STR_P(name), &argument_number, cache_slot);
			zend_native_call_publish_moved_frame(caller, call);
			if (target == NULL) {
				zend_native_release_source_operand(
					source_value, operand_type);
				return FAILURE;
			}
		} else if (argument->auxiliary_operand.kind
				== ZEND_MIR_SOURCE_OPERAND_UNUSED) {
			argument_number = argument->auxiliary_payload;
			if (argument_number == 0) {
				argument_number = argument->ordinal + 1;
			}
			target = ZEND_CALL_VAR(call, argument->result_payload);
		} else {
			zend_native_release_source_operand(source_value, operand_type);
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
						source_value, operand_type);
					return FAILURE;
				}
				ZVAL_MAKE_REF_EX(value, 2);
			}
			ZVAL_REF(target, Z_REF_P(value));
			zend_native_release_source_operand(source_value, operand_type);
			return EG(exception) == NULL ? SUCCESS : FAILURE;
		}
		if (indirect_value) {
			/*
			 * FETCH_*_FUNC_ARG exposes an lvalue through an IS_INDIRECT
			 * temporary.  The temporary owns no part of the referenced
			 * element, so a by-value send copies the referent and consumes
			 * only the indirect wrapper.
			 */
			zend_native_zval_copy_deref_or_dup(target, value);
			zend_native_release_source_operand(source_value, operand_type);
		} else if (Z_ISREF_P(value)) {
			zend_refcounted *reference = Z_COUNTED_P(value);
			zval *referent = Z_REFVAL_P(value);

			ZVAL_COPY_VALUE(target, referent);
			if (GC_DELREF(reference) == 0) {
				efree_size(reference, sizeof(zend_reference));
			} else {
				zend_native_zval_copy_deref_or_dup(target, target);
			}
			/*
			 * The reference wrapper has been consumed explicitly above.  Optimized
			 * op arrays may reuse this physical temporary for a later definition;
			 * native result helpers require a consumed slot to be undefined.
			 */
			ZVAL_UNDEF(source_value);
		} else {
			ZVAL_COPY_VALUE(target, value);
			ZVAL_UNDEF(source_value);
		}
		return SUCCESS;
	}
	target = zend_native_direct_internal_argument_target(
		caller, argument, &argument_number);
	if (target == NULL) {
		return FAILURE;
	}
	/*
	 * SEND_VAL_EX is never an lvalue, even when its TMP slot is mutable.
	 * The VM rejects it before reading the value when the resolved parameter
	 * requires a reference; do not turn the temporary into a reference cell.
	 */
	if (argument->source_opcode == ZEND_SEND_VAL_EX
			&& ARG_MUST_BE_SENT_BY_REF(function, argument_number)) {
		zend_cannot_pass_by_reference(argument_number);
		zend_native_release_source_operand(source_value, operand_type);
		return FAILURE;
	}
	/*
	 * SEND_VAR_NO_REF passes a temporary to a parameter that requires a
	 * reference.  The VM moves the temporary into the call frame and wraps
	 * that frame slot, rather than turning the consumed source slot into a
	 * reference.  The EX form only diagnoses MUST_REF; PREFER_REF accepts the
	 * same temporary by value.
	 */
	if (argument->source_opcode == ZEND_SEND_VAR_NO_REF
			|| (argument->source_opcode == ZEND_SEND_VAR_NO_REF_EX
				&& ARG_SHOULD_BE_SENT_BY_REF(
					function, argument_number))) {
		bool diagnose_non_reference =
			argument->source_opcode == ZEND_SEND_VAR_NO_REF
			|| !ARG_MAY_BE_SENT_BY_REF(function, argument_number);

		if (operand_type != IS_VAR) {
			return FAILURE;
		}
		if (Z_ISREF_P(value)) {
			ZVAL_COPY(target, value);
		} else {
			zend_native_zval_copy_deref_or_dup(target, value);
			if (diagnose_non_reference) {
				ZVAL_MAKE_REF(target);
				zend_error(E_NOTICE,
					"Only variables should be passed by reference");
			}
		}
		zend_native_release_source_operand(source_value, operand_type);
		return EG(exception) == NULL ? SUCCESS : FAILURE;
	}
	/*
	 * SEND_VAR_EX and SEND_REF materialize a reference in the canonical
	 * caller slot when the resolved parameter requires one.  The native
	 * path must perform the same mutation before the argument is copied into
	 * the internal call frame; literals can never become by-reference args.
	 */
	/*
	 * PREFER_REF accepts literals by value while still passing mutable lvalues
	 * by reference.  Only MUST_REF rejects a non-lvalue source.
	 */
	direct_send_by_reference =
		argument->mode == ZEND_NATIVE_CALL_ARGUMENT_BY_REFERENCE
		|| ARG_MUST_BE_SENT_BY_REF(function, argument_number)
		|| (mutable_value
			&& ARG_MAY_BE_SENT_BY_REF(function, argument_number));
	if (direct_send_by_reference
			&& !Z_ISREF_P(value)) {
		if (!mutable_value) {
			zend_cannot_pass_by_reference(argument_number);
			return FAILURE;
		}
		/* SEND_VAR_EX fetches a CV for write when the resolved target expects
		 * a reference. An undefined CV therefore becomes null before PHP wraps
		 * the canonical caller slot in a reference. */
		if (operand_type == IS_CV && Z_ISUNDEF_P(value)) {
			ZVAL_NULL(value);
		}
		ZVAL_MAKE_REF(value);
	}
	if (direct_send_by_reference) {
		ZVAL_COPY(target, value);
		zend_native_release_source_operand(source_value, operand_type);
		return SUCCESS;
	}
	if (operand_type == IS_CV && Z_ISUNDEF_P(value)
			&& (argument->source_opcode == ZEND_SEND_VAR
				|| argument->source_opcode == ZEND_SEND_VAR_EX)) {
		uint32_t variable_index = argument->source_operand.index;

		ZEND_ASSERT(variable_index
			< (uint32_t) caller->func->op_array.last_var);
		zend_error_unchecked(E_WARNING, "Undefined variable $%S",
			caller->func->op_array.vars[variable_index]);
		ZVAL_NULL(target);
		return EG(exception) == NULL ? SUCCESS : FAILURE;
	}
	if (operand_type == IS_CONST) {
		zend_native_zval_copy_deref_or_dup(target, value);
	} else if (operand_type == IS_CV) {
		zend_native_zval_copy_deref_or_dup(target, value);
	} else if (indirect_value) {
		zend_native_zval_copy_deref_or_dup(target, value);
		zend_native_release_source_operand(source_value, operand_type);
	} else if (operand_type == IS_VAR || operand_type == IS_TMP_VAR) {
		if (Z_ISREF_P(value)) {
			zend_native_zval_copy_deref_or_dup(target, value);
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

zend_result zend_native_call_set_source_argument(
	zend_execute_data *caller,
	const zend_native_user_call_descriptor *descriptor,
	uint32_t argument_index)
{
	if (caller == NULL || descriptor == NULL
			|| argument_index >= descriptor->argument_count) {
		return FAILURE;
	}
	return zend_native_call_set_explicit_argument(
		caller, &descriptor->arguments[argument_index]);
}

zend_result zend_native_direct_internal_call_set_source_argument(
	zend_execute_data *caller,
	const zend_native_direct_internal_call_descriptor *descriptor,
	uint32_t argument_index)
{
	if (caller == NULL || descriptor == NULL
			|| argument_index >= descriptor->argument_count) {
		if (EG(exception) == NULL) {
			zend_throw_error(
				NULL, "Invalid direct internal call argument descriptor");
		}
		return FAILURE;
	}
	if (zend_native_call_set_explicit_argument(
			caller, &descriptor->arguments[argument_index]) == FAILURE) {
		if (EG(exception) == NULL) {
			zend_throw_error(
				NULL, "Invalid direct internal call argument descriptor");
		}
		return FAILURE;
	}
	return SUCCESS;
}

static zval *zend_native_direct_internal_scalar_target(
		zend_execute_data *caller,
		const zend_native_direct_internal_call_descriptor *descriptor,
		uint32_t argument_index,
		uint32_t *argument_number)
{
	const zend_native_direct_internal_call_argument *argument;
	zval *target;

	if (EG(exception) != NULL || caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type) || caller->call == NULL
			|| caller->call->func == NULL || descriptor == NULL
			|| argument_index >= descriptor->argument_count) {
		return NULL;
	}
	argument = &descriptor->arguments[argument_index];
	if (argument->mode != ZEND_NATIVE_CALL_ARGUMENT_BY_VALUE
			|| argument->source_position >= caller->func->op_array.last) {
		return NULL;
	}
	caller->opline = &caller->func->op_array.opcodes[
		argument->source_position];
	target = zend_native_direct_internal_argument_target(
		caller, argument, argument_number);
	if (target != NULL
			&& ARG_MUST_BE_SENT_BY_REF(
				caller->call->func, *argument_number)) {
		zend_cannot_pass_by_reference(*argument_number);
		return NULL;
	}
	return target;
}

zend_result zend_native_direct_internal_call_set_integer_argument(
		zend_execute_data *caller,
		const zend_native_direct_internal_call_descriptor *descriptor,
		uint32_t argument_index,
		uint64_t payload_bits,
		zend_mir_scalar_type_mask exact_type)
{
	uint32_t argument_number;
	zval *target = zend_native_direct_internal_scalar_target(
		caller, descriptor, argument_index, &argument_number);

	if (target == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(
				NULL, "Invalid direct internal call argument descriptor");
		}
		return FAILURE;
	}
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			ZVAL_NULL(target);
			break;
		case ZEND_MIR_SCALAR_TYPE_I1:
			ZVAL_BOOL(target, payload_bits != 0);
			break;
		case ZEND_MIR_SCALAR_TYPE_I64:
			ZVAL_LONG(target, (zend_long) payload_bits);
			break;
		default:
			zend_throw_error(
				NULL, "Invalid direct internal call scalar argument type");
			return FAILURE;
	}
	return SUCCESS;
}

zend_result zend_native_direct_internal_call_set_double_argument(
		zend_execute_data *caller,
		const zend_native_direct_internal_call_descriptor *descriptor,
		uint32_t argument_index,
		double value)
{
	uint32_t argument_number;
	zval *target = zend_native_direct_internal_scalar_target(
		caller, descriptor, argument_index, &argument_number);

	if (target == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(
				NULL, "Invalid direct internal call argument descriptor");
		}
		return FAILURE;
	}
	ZVAL_DOUBLE(target, value);
	return SUCCESS;
}

zend_native_status zend_native_internal_call_invoke_finish(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	zval *return_value)
{
	zend_native_internal_execution_state *state;
	zend_execute_data *pending_call;

	if (caller == NULL || caller->call == NULL || cell == NULL
			|| cell->function == NULL || caller->call->func != cell->function
			|| return_value == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	state = emalloc(sizeof(*state));
	state->caller = caller;
	state->call = caller->call;
	pending_call = state->call->prev_execute_data;
	state->call->prev_execute_data = caller;
	caller->call = pending_call;
	state->return_value = return_value;
	state->status = ZEND_NATIVE_BAILOUT;
	state->observer_started = false;
	state->observer_finished = false;
	ZVAL_NULL(state->return_value);
	EG(current_execute_data) = state->call;

	zend_try {
		if (EG(exception) != NULL) {
			state->status = ZEND_NATIVE_EXCEPTION;
		} else if (!zend_native_internal_call_preflight(
				state->caller, state->call->func)) {
			state->status = ZEND_NATIVE_EXCEPTION;
		} else if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_MAY_HAVE_UNDEF) != 0
				&& zend_handle_undef_args(state->call) == FAILURE) {
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} else {
			/* Internal handlers own their parameter parsing and error ordering.
			 * Some handlers intentionally perform semantic checks before reporting
			 * an argument-count mismatch, so a generic native precheck would not
			 * match ZEND_DO_ICALL. */
			#if ZEND_DEBUG
			bool should_throw = zend_internal_call_should_throw(
				state->call->func, state->call);
			#endif
			state->observer_started = true;
			ZEND_OBSERVER_FCALL_BEGIN(state->call);
			if (EXPECTED(zend_execute_internal == NULL)) {
				state->call->func->internal_function.handler(
					state->call, state->return_value);
			} else {
				zend_execute_internal(state->call, state->return_value);
			}
			#if ZEND_DEBUG
			if (EG(exception) == NULL && state->call->func != NULL
					&& (state->call->func->common.fn_flags
						& ZEND_ACC_FAKE_CLOSURE) == 0) {
				if (should_throw) {
					zend_internal_call_arginfo_violation(state->call->func);
				}
				if ((state->call->func->common.fn_flags
						& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
					bool valid = zend_verify_internal_return_type(
						state->call->func, state->return_value);
					ZEND_ASSERT(valid);
				}
				ZEND_ASSERT((state->call->func->common.fn_flags
						& ZEND_ACC_RETURN_REFERENCE) != 0
					? Z_ISREF_P(state->return_value)
					: !Z_ISREF_P(state->return_value));
			}
			#endif
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
	if ((ZEND_CALL_INFO(state->call)
			& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
		zend_free_extra_named_params(state->call->extra_named_params);
		state->call->extra_named_params = NULL;
		ZEND_DEL_CALL_FLAG(state->call, ZEND_CALL_HAS_EXTRA_NAMED_PARAMS);
	}
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
	if (state->status != ZEND_NATIVE_RETURNED
			&& !Z_ISUNDEF_P(state->return_value)) {
		zval_ptr_dtor(state->return_value);
		ZVAL_UNDEF(state->return_value);
	}
	switch (state->status) {
		case ZEND_NATIVE_RETURNED:
			efree(state);
			return ZEND_NATIVE_RETURNED;
		case ZEND_NATIVE_EXCEPTION:
			efree(state);
			return ZEND_NATIVE_EXCEPTION;
		default:
			efree(state);
			return ZEND_NATIVE_BAILOUT;
	}
}

zend_native_status zend_native_internal_call_execute_top(
	zend_execute_data *call)
{
	zend_native_internal_top_execution_state *state;

	if (call == NULL || call->func == NULL
			|| call->func->type != ZEND_INTERNAL_FUNCTION) {
		return ZEND_NATIVE_EXCEPTION;
	}
	state = emalloc(sizeof(*state));
	state->caller = call->prev_execute_data;
	state->call = call;
	state->uses_discarded_return = call->return_value == NULL;
	if (state->uses_discarded_return) {
		ZVAL_UNDEF(&state->discarded_return);
		state->return_value = &state->discarded_return;
	} else {
		state->return_value = call->return_value;
	}
	state->status = ZEND_NATIVE_BAILOUT;
	state->observer_started = false;
	state->observer_finished = false;
	ZVAL_NULL(state->return_value);
	EG(current_execute_data) = state->call;

	zend_try {
		if (EG(exception) != NULL) {
			state->status = ZEND_NATIVE_EXCEPTION;
		} else if ((ZEND_CALL_INFO(state->call)
				& ZEND_CALL_MAY_HAVE_UNDEF) != 0
				&& zend_handle_undef_args(state->call) == FAILURE) {
			state->status = EG(exception) != NULL
				? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
		} else {
			/* As in the nested internal-call path, the handler owns parameter
			 * parsing and the ordering of semantic errors versus arity errors. */
			#if ZEND_DEBUG
			bool should_throw = zend_internal_call_should_throw(
				state->call->func, state->call);
			#endif
			state->observer_started = true;
			ZEND_OBSERVER_FCALL_BEGIN(state->call);
			if (EXPECTED(zend_execute_internal == NULL)) {
				state->call->func->internal_function.handler(
					state->call, state->return_value);
			} else {
				zend_execute_internal(state->call, state->return_value);
			}
			#if ZEND_DEBUG
			if (EG(exception) == NULL && state->call->func != NULL
					&& (state->call->func->common.fn_flags
						& ZEND_ACC_FAKE_CLOSURE) == 0) {
				if (should_throw) {
					zend_internal_call_arginfo_violation(state->call->func);
				}
				if ((state->call->func->common.fn_flags
						& ZEND_ACC_HAS_RETURN_TYPE) != 0) {
					bool valid = zend_verify_internal_return_type(
						state->call->func, state->return_value);
					ZEND_ASSERT(valid);
				}
				ZEND_ASSERT((state->call->func->common.fn_flags
						& ZEND_ACC_RETURN_REFERENCE) != 0
					? Z_ISREF_P(state->return_value)
					: !Z_ISREF_P(state->return_value));
			}
			#endif
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
	if ((ZEND_CALL_INFO(state->call)
			& ZEND_CALL_HAS_EXTRA_NAMED_PARAMS) != 0) {
		zend_free_extra_named_params(state->call->extra_named_params);
	}
	if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_RELEASE_THIS) != 0) {
		OBJ_RELEASE(Z_OBJ(state->call->This));
	} else if ((ZEND_CALL_INFO(state->call) & ZEND_CALL_CLOSURE) != 0) {
		OBJ_RELEASE(ZEND_CLOSURE_OBJECT(state->call->func));
	}
	if (state->status != ZEND_NATIVE_RETURNED
			&& !Z_ISUNDEF_P(state->return_value)) {
		zval_ptr_dtor(state->return_value);
		ZVAL_UNDEF(state->return_value);
	}
	if (state->uses_discarded_return
			&& !Z_ISUNDEF(state->discarded_return)) {
		zval_ptr_dtor(&state->discarded_return);
	}
	switch (state->status) {
		case ZEND_NATIVE_RETURNED:
			efree(state);
			return ZEND_NATIVE_RETURNED;
		case ZEND_NATIVE_EXCEPTION:
			efree(state);
			return ZEND_NATIVE_EXCEPTION;
		default:
			efree(state);
			return ZEND_NATIVE_BAILOUT;
	}
}

static bool zend_native_internal_scalar_payload(
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

zend_native_direct_call_result zend_native_internal_call_direct(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor)
{
	zend_native_direct_call_result result = {
		.status = ZEND_NATIVE_EXCEPTION,
		.payload = 0
	};
	zval temporary;
	zval *return_value = &temporary;
	zend_native_status status;
	uint32_t index;
	bool mutable_result;
	uint8_t result_operand_type;

	ZVAL_UNDEF(&temporary);
	if (caller == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| descriptor == NULL
			|| descriptor->do_source_position >= caller->func->op_array.last
			|| zend_native_internal_call_begin_explicit(
				caller, cell, descriptor) == FAILURE) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Invalid direct internal call descriptor");
		}
		return result;
	}
	for (index = 0; index < descriptor->argument_count; index++) {
		if (zend_native_call_set_explicit_argument(
				caller, &descriptor->arguments[index]) == FAILURE) {
			if (EG(exception) == NULL) {
				zend_throw_error(
					NULL, "Invalid direct internal call argument descriptor");
			}
			status = zend_native_internal_call_invoke_finish(
				caller, cell, &temporary);
			result.status = status == ZEND_NATIVE_BAILOUT
				? ZEND_NATIVE_BAILOUT : ZEND_NATIVE_EXCEPTION;
			if (!Z_ISUNDEF(temporary)) {
				zval_ptr_dtor(&temporary);
			}
			return result;
		}
	}
	if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT
			|| descriptor->do_opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
		result.status = zend_native_call_convert_explicit(
			caller,
			descriptor->do_opcode,
			descriptor->do_op1_payload,
			zend_native_internal_encode_source_operand(&descriptor->do_op2),
			zend_native_internal_encode_source_operand(
				&descriptor->result_operand),
			descriptor->do_extended_value,
			descriptor->do_source_position);
		return result;
	}
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->do_source_position];
	if (descriptor->result_operand.kind
			!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		return_value = zend_native_explicit_operand(
			caller, &descriptor->result_operand, false,
			&mutable_result, &result_operand_type);
		if (return_value == NULL
				|| (result_operand_type != IS_CV
					&& result_operand_type != IS_VAR
					&& result_operand_type != IS_TMP_VAR)) {
			zend_throw_error(
				NULL, "Invalid direct internal call result descriptor");
			status = zend_native_internal_call_invoke_finish(
				caller, cell, &temporary);
			result.status = status == ZEND_NATIVE_BAILOUT
				? ZEND_NATIVE_BAILOUT : ZEND_NATIVE_EXCEPTION;
			if (!Z_ISUNDEF(temporary)) {
				zval_ptr_dtor(&temporary);
			}
			return result;
		}
		ZVAL_UNDEF(return_value);
	}
	status = zend_native_internal_call_invoke_finish(
		caller, cell, return_value);
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL) {
		result.status = zend_native_prepare_finally_exception(
			caller, descriptor->do_source_position) == SUCCESS
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	} else if (status == ZEND_NATIVE_RETURNED
			&& (descriptor->flags
				& ZEND_NATIVE_DIRECT_INTERNAL_CALL_REQUIRE_SCALAR_RESULT) != 0
			&& descriptor->result_type != ZEND_MIR_SCALAR_TYPE_NONE
			&& !zend_native_internal_scalar_payload(
				return_value, descriptor->result_type, &result.payload)) {
		zend_throw_error(NULL,
			"Native internal callee violated its exact scalar result contract");
		result.status = ZEND_NATIVE_EXCEPTION;
	} else {
		result.status = status;
	}
	if (return_value == &temporary && !Z_ISUNDEF(temporary)) {
		zval_ptr_dtor(&temporary);
	}
	return result;
}

zend_native_status zend_native_internal_call_invoke_finish_source(
	zend_execute_data *caller,
	const zend_native_internal_call_cell *cell,
	const zend_native_direct_internal_call_descriptor *descriptor)
{
	zval temporary;
	zval *return_value;
	zend_native_status status;
	bool mutable_result;
	uint8_t result_operand_type;

	if (caller == NULL || descriptor == NULL || caller->func == NULL
			|| !ZEND_USER_CODE(caller->func->type)
			|| descriptor->do_source_position
				>= caller->func->op_array.last) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (descriptor->do_opcode == ZEND_CALLABLE_CONVERT
			|| descriptor->do_opcode == ZEND_CALLABLE_CONVERT_PARTIAL) {
		return zend_native_call_convert_explicit(
			caller,
			descriptor->do_opcode,
			descriptor->do_op1_payload,
			zend_native_internal_encode_source_operand(&descriptor->do_op2),
			zend_native_internal_encode_source_operand(
				&descriptor->result_operand),
			descriptor->do_extended_value,
			descriptor->do_source_position);
	}
	if (EG(exception) != NULL) {
		ZVAL_UNDEF(&temporary);
		return zend_native_internal_call_invoke_finish(
			caller, cell, &temporary);
	}
	caller->opline = &caller->func->op_array.opcodes[
		descriptor->do_source_position];
	if (descriptor->result_operand.kind
			== ZEND_MIR_SOURCE_OPERAND_UNUSED) {
		ZVAL_UNDEF(&temporary);
		return_value = &temporary;
	} else {
		return_value = zend_native_explicit_operand(
			caller, &descriptor->result_operand, false,
			&mutable_result, &result_operand_type);
		if (return_value == NULL
				|| (result_operand_type != IS_CV
					&& result_operand_type != IS_VAR
					&& result_operand_type != IS_TMP_VAR)) {
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_UNDEF(return_value);
	}
	status = zend_native_internal_call_invoke_finish(
		caller, cell, return_value);
	if (status == ZEND_NATIVE_EXCEPTION && EG(exception) != NULL) {
		status = zend_native_prepare_finally_exception(
			caller, descriptor->do_source_position) == SUCCESS
			? ZEND_NATIVE_EXCEPTION : ZEND_NATIVE_BAILOUT;
	}
	if (return_value == &temporary && !Z_ISUNDEF(temporary)) {
		zval_ptr_dtor(&temporary);
	}
	return status;
}

uint64_t zend_native_call_read_source_scalar(
	zend_execute_data *caller,
	uint64_t result_operand,
	zend_mir_scalar_type_mask exact_type)
{
	const zval *value;
	uint8_t result_operand_type;
	uint64_t payload_bits = 0;
	bool matches = false;

	value = zend_native_call_explicit_slot(
		caller, result_operand, &result_operand_type);
	if (value == NULL
			|| (result_operand_type != IS_CV
				&& result_operand_type != IS_VAR
				&& result_operand_type != IS_TMP_VAR)) {
		goto mismatch;
	}
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
		"Native call violated its exact scalar result contract");
	zend_bailout();
	return 0;
}

zend_native_status zend_native_return_source_zval(
	zend_execute_data *execute_data,
	uint32_t source_position,
	uint64_t encoded_operand,
	uint32_t source_opcode,
	uint32_t extended_value)
{
	zend_mir_source_operand_ref operand;
	bool mutable_value;
	uint8_t operand_type;
	zval *source;
	zval *source_slot;
	zval *return_value;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position >= execute_data->func->op_array.last
			|| (source_opcode != ZEND_RETURN
				&& source_opcode != ZEND_RETURN_BY_REF)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	memset(&operand, 0, sizeof(operand));
	operand.kind = (zend_mir_source_operand_kind)
		(encoded_operand & UINT64_C(0xff));
	operand.slot_kind = (zend_mir_source_slot_kind)
		((encoded_operand >> 8) & UINT64_C(0xff));
	operand.index = (uint32_t) (encoded_operand >> 16);
	operand.ssa_variable_id = ZEND_MIR_ID_INVALID;
	source_slot = zend_native_explicit_operand(
		execute_data, &operand, true, &mutable_value, &operand_type);
	if (source_slot == NULL
			|| (operand_type != IS_CONST && operand_type != IS_CV
				&& operand_type != IS_TMP_VAR
				&& operand_type != IS_VAR)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_position];
	source = source_slot;
	return_value = execute_data->return_value;
	if (source_opcode == ZEND_RETURN_BY_REF) {
		bool temporary = operand_type == IS_TMP_VAR
			|| operand_type == IS_VAR;
		if (operand_type == IS_CV && Z_ISUNDEF_P(source)) {
			ZVAL_NULL(source);
		}
		if (operand_type == IS_VAR
				&& Z_TYPE_P(source) == IS_INDIRECT) {
			source = Z_INDIRECT_P(source);
		}

		if ((operand_type == IS_CONST
				|| operand_type == IS_TMP_VAR)
				|| (operand_type == IS_VAR
					&& extended_value == ZEND_RETURNS_VALUE)) {
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
			if (operand_type == IS_VAR && Z_ISREF_P(source)) {
				ZVAL_COPY_VALUE(return_value, source);
				ZVAL_UNDEF(source_slot);
			} else {
				if (operand_type == IS_CONST) {
					zval copy;

					zend_native_zval_copy_deref_or_dup(&copy, source);
					ZVAL_NEW_REF(return_value, &copy);
				} else {
					ZVAL_NEW_REF(return_value, source);
					ZVAL_UNDEF(source_slot);
				}
			}
			zend_return_unwrap_ref(execute_data, return_value);
			return ZEND_NATIVE_RETURNED;
		}

		if (operand_type == IS_VAR
				&& extended_value == ZEND_RETURNS_FUNCTION
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
	if (operand_type == IS_CV && Z_ISUNDEF_P(source)) {
		if (operand.index >= execute_data->func->op_array.last_var) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[operand.index]));
		if (UNEXPECTED(EG(exception) != NULL)) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if (return_value != NULL) {
			ZVAL_NULL(return_value);
		}
		return ZEND_NATIVE_RETURNED;
	}
	if (return_value == NULL) {
		if (operand_type != IS_CV && !Z_ISUNDEF_P(source)) {
			zval_ptr_dtor(source);
			ZVAL_UNDEF(source);
		}
		return ZEND_NATIVE_RETURNED;
	}
	if (operand_type == IS_CONST) {
		zend_native_zval_copy_deref_or_dup(return_value, source);
	} else if (operand_type == IS_CV || Z_ISREF_P(source)) {
		zend_native_zval_copy_deref_or_dup(return_value, source);
		if (operand_type != IS_CV) {
			zval_ptr_dtor(source);
			ZVAL_UNDEF(source);
		}
	} else {
		ZVAL_COPY_VALUE(return_value, source);
		ZVAL_UNDEF(source);
	}
	return ZEND_NATIVE_RETURNED;
}

static uint32_t zend_native_finally_unwind_target(
	zend_execute_data *execute_data, const zend_op_array *op_array,
	uint32_t try_catch_offset, uint32_t source_position);

static uint32_t zend_native_try_catch_offset_for_source(
	const zend_op_array *op_array, uint32_t source_position)
{
	uint32_t current = UINT32_MAX;
	uint32_t index;

	for (index = 0; index < op_array->last_try_catch; index++) {
		const zend_try_catch_element *region =
			&op_array->try_catch_array[index];

		if (region->try_op > source_position) {
			break;
		}
		if (source_position < region->catch_op
				|| source_position < region->finally_end) {
			current = index;
		}
	}
	return current;
}

uint32_t zend_native_catch_enter(
	zend_execute_data *execute_data, uint32_t catch_opline_index)
{
	const zend_op_array *op_array;
	const zend_op *opline;
	const zend_op *throw_opline;
	zend_class_entry *catch_ce;
	zend_class_entry *exception_ce;
	zend_object *exception;
	uint32_t cache_offset;
	bool chained_catch;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| catch_opline_index >= execute_data->func->op_array.last) {
		return ZEND_NATIVE_BAILOUT;
	}
	op_array = &execute_data->func->op_array;
	opline = &op_array->opcodes[catch_opline_index];
	if (opline->opcode != ZEND_CATCH || EG(exception) == NULL) {
		return ZEND_NATIVE_CATCH_EXCEPTION;
	}
	/*
	 * Source-backed argument evaluation can raise after a universal native call
	 * setup has published its callee but before the call phase itself runs.  In
	 * that state the callee is owned by the active direct-call record, not by the
	 * VM's unfinished-call chain.  Release it before the VM cleanup below walks
	 * execute_data->call; its activation records the real preceding pending call
	 * and restores both that link and the VM-stack top.
	 */
	zend_native_call_direct_unwind(execute_data);
	/*
	 * A native exception edge bypasses the VM's HANDLE_EXCEPTION dispatch.
	 * Retire live temporaries and restore BEGIN_SILENCE state before entering
	 * the source catch, using the same source-opline interval as the VM.
	 */
	throw_opline = execute_data->opline;
	if (throw_opline == EG(exception_op)
			&& EG(opline_before_exception) != NULL) {
		throw_opline = EG(opline_before_exception);
	}
	chained_catch = throw_opline >= op_array->opcodes
		&& throw_opline < op_array->opcodes + op_array->last
		&& throw_opline->opcode == ZEND_CATCH
		&& (throw_opline->extended_value & ZEND_LAST_CATCH) == 0
		&& OP_JMP_ADDR(throw_opline, throw_opline->op2) == opline;
	if (throw_opline >= op_array->opcodes
			&& throw_opline < op_array->opcodes + op_array->last
			&& throw_opline != opline
			&& !chained_catch) {
		uint32_t source_position =
			(uint32_t) (throw_opline - op_array->opcodes);
		uint32_t continuation = zend_native_finally_unwind_target(
			execute_data, op_array,
			zend_native_try_catch_offset_for_source(
				op_array, source_position),
			source_position);

		if (continuation
				!= (ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
					| catch_opline_index)) {
			return continuation == ZEND_NATIVE_FINALLY_PROPAGATE
				? ZEND_NATIVE_CATCH_EXCEPTION : continuation;
		}
		zend_native_cleanup_unfinished_exception(execute_data,
			source_position, catch_opline_index);
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
			uint32_t continuation;

			zend_rethrow_exception(execute_data);
			continuation = zend_native_finally_unwind_target(
				execute_data, op_array,
				zend_native_try_catch_offset_for_source(
					op_array, catch_opline_index),
				catch_opline_index);
			return continuation == ZEND_NATIVE_FINALLY_PROPAGATE
				? ZEND_NATIVE_CATCH_EXCEPTION : continuation;
		}
		return ZEND_NATIVE_CATCH_BRANCH;
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
		? ZEND_NATIVE_CATCH_MATCHED : ZEND_NATIVE_CATCH_EXCEPTION;
}

uint32_t zend_native_catch_explicit(
	zend_execute_data *execute_data,
	uint64_t encoded_class,
	uint64_t encoded_result,
	uint32_t extended_value,
	uint32_t source_position)
{
	const zend_op_array *op_array;
	zval *class_name;
	zval *result;
	uint8_t class_type;
	uint8_t result_type;
	zend_class_entry *catch_ce;
	zend_class_entry *exception_ce;
	zend_object *exception;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position >= execute_data->func->op_array.last) {
		return ZEND_NATIVE_CATCH_EXCEPTION;
	}
	op_array = &execute_data->func->op_array;
	execute_data->opline = &op_array->opcodes[source_position];
	if (EG(exception) == NULL) {
		return ZEND_NATIVE_CATCH_BRANCH;
	}
	class_name = zend_native_call_explicit_operand(
		execute_data, encoded_class, &class_type);
	result = zend_native_call_explicit_slot(
		execute_data, encoded_result, &result_type);
	if (class_name == NULL || class_type != IS_CONST
			|| Z_TYPE_P(class_name) != IS_STRING
			|| class_name + 1 >= op_array->literals + op_array->last_literal
			|| Z_TYPE_P(class_name + 1) != IS_STRING
			|| (result_type != IS_UNUSED
				&& result_type != IS_CV
				&& result_type != IS_VAR
				&& result_type != IS_TMP_VAR)
			|| (result_type != IS_UNUSED && result == NULL)) {
		zend_throw_error(NULL, "Invalid native CATCH operands");
		return ZEND_NATIVE_CATCH_EXCEPTION;
	}
	catch_ce = zend_fetch_class_by_name(
		Z_STR_P(class_name), Z_STR_P(class_name + 1),
		ZEND_FETCH_CLASS_NO_AUTOLOAD | ZEND_FETCH_CLASS_SILENT);
	exception_ce = EG(exception)->ce;
	if (exception_ce != catch_ce
			&& (catch_ce == NULL
				|| !instanceof_function(exception_ce, catch_ce))) {
		if ((extended_value & ZEND_LAST_CATCH) != 0) {
			zend_rethrow_exception(execute_data);
			return ZEND_NATIVE_CATCH_EXCEPTION;
		}
		return ZEND_NATIVE_CATCH_BRANCH;
	}
	exception = EG(exception);
	EG(exception) = NULL;
	if (result_type != IS_UNUSED) {
		zval tmp;
		ZVAL_OBJ(&tmp, exception);
		zend_assign_to_variable(result, &tmp, IS_TMP_VAR, true);
	} else {
		OBJ_RELEASE(exception);
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_CATCH_MATCHED : ZEND_NATIVE_CATCH_EXCEPTION;
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
	const zend_op *throw_opline;
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
	if (EG(exception) != NULL && zend_is_unwind_exit(EG(exception))) {
		return ZEND_NATIVE_EXCEPTION;
	}
	throw_opline = execute_data->opline;
	if (throw_opline == EG(exception_op)
			&& EG(opline_before_exception) != NULL) {
		throw_opline = EG(opline_before_exception);
	}
	if (EG(exception) != NULL
			&& throw_opline >= op_array->opcodes
			&& throw_opline < op_array->opcodes + op_array->last
			&& throw_opline != &op_array->opcodes[finally_opline_index]) {
		zend_native_cleanup_unfinished_exception(execute_data,
			(uint32_t) (throw_opline - op_array->opcodes),
			finally_opline_index);
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

static uint32_t zend_native_finally_unwind_target(
	zend_execute_data *execute_data,
	const zend_op_array *op_array,
	uint32_t try_catch_offset,
	uint32_t source_position)
{
	zend_object *exception = EG(exception);

	for (; try_catch_offset != UINT32_MAX; try_catch_offset--) {
		const zend_try_catch_element *region;

		if (try_catch_offset >= op_array->last_try_catch) {
			return ZEND_NATIVE_FINALLY_PROPAGATE;
		}
		region = &op_array->try_catch_array[try_catch_offset];
		if (region->catch_op != 0
				&& source_position < region->catch_op
				&& exception != NULL) {
			return region->catch_op < ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
				? ZEND_NATIVE_FINALLY_EXCEPTION_FLAG | region->catch_op
				: ZEND_NATIVE_FINALLY_PROPAGATE;
		}
		if (region->finally_op != 0
				&& source_position < region->finally_op) {
			const zend_op *fast_ret;
			zval *fast_call;

			if (exception != NULL && zend_is_unwind_exit(exception)) {
				continue;
			}
			if (region->finally_op >= ZEND_NATIVE_FINALLY_EXCEPTION_FLAG
					|| region->finally_end >= op_array->last) {
				return ZEND_NATIVE_FINALLY_PROPAGATE;
			}
			fast_ret = &op_array->opcodes[region->finally_end];
			if (fast_ret->opcode != ZEND_FAST_RET
					|| fast_ret->op1_type != IS_TMP_VAR) {
				return ZEND_NATIVE_FINALLY_PROPAGATE;
			}
			zend_native_cleanup_unfinished_exception(
				execute_data, source_position, region->finally_op);
			fast_call = ZEND_CALL_VAR(execute_data, fast_ret->op1.var);
			Z_OBJ_P(fast_call) = exception;
			EG(exception) = NULL;
			Z_OPLINE_NUM_P(fast_call) = UINT32_MAX;
			return ZEND_NATIVE_FINALLY_EXCEPTION_FLAG | region->finally_op;
		}
		if (region->finally_end != 0
				&& source_position < region->finally_end) {
			const zend_op *fast_ret;
			zval *fast_call;
			uint32_t continuation;

			if (region->finally_end >= op_array->last) {
				return ZEND_NATIVE_FINALLY_PROPAGATE;
			}
			fast_ret = &op_array->opcodes[region->finally_end];
			if (fast_ret->opcode != ZEND_FAST_RET
					|| fast_ret->op1_type != IS_TMP_VAR) {
				return ZEND_NATIVE_FINALLY_PROPAGATE;
			}
			fast_call = ZEND_CALL_VAR(execute_data, fast_ret->op1.var);
			continuation = Z_OPLINE_NUM_P(fast_call);
			if (continuation != UINT32_MAX
					&& continuation < op_array->last
					&& (op_array->opcodes[continuation].op2_type
						& (IS_TMP_VAR | IS_VAR))) {
				zval_ptr_dtor(ZEND_CALL_VAR(execute_data,
					op_array->opcodes[continuation].op2.var));
			}
			if (Z_OBJ_P(fast_call) != NULL) {
				if (exception != NULL) {
					if (zend_is_unwind_exit(exception)
							|| zend_is_graceful_exit(exception)) {
						OBJ_RELEASE(Z_OBJ_P(fast_call));
					} else {
						zend_exception_set_previous(
							exception, Z_OBJ_P(fast_call));
					}
				} else {
					exception = EG(exception) = Z_OBJ_P(fast_call);
				}
			}
		}
	}
	return ZEND_NATIVE_FINALLY_PROPAGATE;
}

static uint32_t zend_native_finally_complete_forced_generator(
	zend_execute_data *execute_data, uint32_t continuation)
{
	zend_generator *generator;

	if (continuation != ZEND_NATIVE_FINALLY_PROPAGATE
			|| EG(exception) != NULL
			|| (ZEND_CALL_INFO(execute_data) & ZEND_CALL_GENERATOR) == 0
			|| (generator =
				(zend_generator *) execute_data->return_value) == NULL
			|| (generator->flags & ZEND_GENERATOR_FORCED_CLOSE) == 0) {
		return continuation;
	}
	ZEND_OBSERVER_FCALL_END(execute_data, NULL);
	EG(current_execute_data) = execute_data->prev_execute_data;
	/* Forced finally propagation exits before the source generator reaches its
	 * natural return. Preserve unfinished-execution cleanup for active loop and
	 * temporary live ranges, matching the VM destructor path. */
	zend_generator_close(generator, false);
	return ZEND_NATIVE_FINALLY_GENERATOR_RETURNED;
}

uint32_t zend_native_finally_return(
	zend_execute_data *execute_data, uint32_t fast_ret_opline_index)
{
	const zend_op_array *op_array;
	const zend_op *opline;
	zval *fast_call;
	uint32_t continuation;

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
	return zend_native_finally_complete_forced_generator(
		execute_data, zend_native_finally_unwind_target(
			execute_data, op_array, opline->op2.num,
			fast_ret_opline_index));
}

uint32_t zend_native_finally_return_explicit(
	zend_execute_data *execute_data,
	uint64_t encoded_operand,
	uint32_t try_catch_offset,
	uint32_t source_position)
{
	const zend_op_array *op_array;
	zval *fast_call;
	uint8_t operand_type;
	uint32_t continuation;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		return ZEND_NATIVE_FINALLY_PROPAGATE;
	}
	op_array = &execute_data->func->op_array;
	if (source_position >= op_array->last
			|| (fast_call = zend_native_call_explicit_slot(
				execute_data, encoded_operand, &operand_type)) == NULL
			|| operand_type != IS_TMP_VAR) {
		return ZEND_NATIVE_FINALLY_PROPAGATE;
	}
	execute_data->opline = &op_array->opcodes[source_position];
	continuation = Z_OPLINE_NUM_P(fast_call);
	if (continuation != UINT32_MAX) {
		return continuation;
	}
	EG(exception) = Z_OBJ_P(fast_call);
	Z_OBJ_P(fast_call) = NULL;
	return zend_native_finally_complete_forced_generator(
		execute_data, zend_native_finally_unwind_target(
			execute_data, op_array, try_catch_offset, source_position));
}

zend_native_status zend_native_discard_exception(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	const zend_op_array *op_array;
	const zend_op *opline;
	zval *fast_call;
	uint8_t operand_type;
	uint32_t continuation;

	(void) op2;
	(void) result;
	(void) extended_value;
	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opcode != ZEND_DISCARD_EXCEPTION) {
		return ZEND_NATIVE_EXCEPTION;
	}
	op_array = &execute_data->func->op_array;
	if (source_position_id >= op_array->last
			|| (opline = &op_array->opcodes[source_position_id])->opcode
				!= ZEND_DISCARD_EXCEPTION
			|| (fast_call = zend_native_call_explicit_slot(
				execute_data, op1, &operand_type)) == NULL
			|| (operand_type != IS_TMP_VAR && operand_type != IS_VAR)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->opline = opline;
	continuation = Z_OPLINE_NUM_P(fast_call);
	if (continuation != UINT32_MAX) {
		const zend_op *fast_call_opline;

		if (continuation >= op_array->last
				|| (fast_call_opline = &op_array->opcodes[continuation])
					->opcode != ZEND_FAST_CALL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		if ((fast_call_opline->op2_type & (IS_TMP_VAR | IS_VAR)) != 0) {
			zval *return_value =
				ZEND_CALL_VAR(execute_data, fast_call_opline->op2.var);

			zval_ptr_dtor(return_value);
			ZVAL_NULL(return_value);
		}
	}
	if (Z_OBJ_P(fast_call) != NULL) {
		OBJ_RELEASE(Z_OBJ_P(fast_call));
		Z_OBJ_P(fast_call) = NULL;
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
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
