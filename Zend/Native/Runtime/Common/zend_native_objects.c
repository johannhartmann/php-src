/* Exact source-backed object operations for native frames. */

#include "Zend/Native/Runtime/Common/zend_native_objects.h"

#include "Zend/zend_API.h"
#include "Zend/zend_constants.h"
#include "Zend/zend_closures.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_inheritance.h"
#include "Zend/zend_object_handlers.h"
#include "Zend/zend_operators.h"

#include "Zend/Native/Lowering/zend_mir_lowering_source.h"

#include <string.h>

typedef struct _zend_native_explicit_object_operation {
	uint8_t op1_type;
	uint8_t op2_type;
	uint8_t result_type;
	znode_op op1;
	znode_op op2;
	znode_op result;
	uint32_t extended_value;
	uint32_t source_position_id;
} zend_native_explicit_object_operation;

static bool zend_native_object_decode_explicit_operand(
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

static bool zend_native_object_init_explicit_operation(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id, uint8_t expected_opcode,
	zend_native_explicit_object_operation *operation)
{
	if (operation == NULL || source_opcode != expected_opcode
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position_id >= execute_data->func->op_array.last
			|| !zend_native_object_decode_explicit_operand(
				execute_data, op1, &operation->op1_type, &operation->op1)
			|| !zend_native_object_decode_explicit_operand(
				execute_data, op2, &operation->op2_type, &operation->op2)
			|| !zend_native_object_decode_explicit_operand(
				execute_data, result,
				&operation->result_type, &operation->result)) {
		return false;
	}
	operation->extended_value = extended_value;
	operation->source_position_id = source_position_id;
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	return true;
}

static const zend_op *zend_native_object_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opline_index >= execute_data->func->op_array.last) {
		return NULL;
	}
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_opline_index];
	return execute_data->opline;
}

static zval *zend_native_object_slot(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_CV && type != IS_VAR && type != IS_TMP_VAR) {
		return NULL;
	}
	return ZEND_CALL_VAR(execute_data, operand.var);
}

static zval *zend_native_object_read_explicit(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	zval *value;

	if (type == IS_CONST) {
		return operand.constant < execute_data->func->op_array.last_literal
			? &execute_data->func->op_array.literals[operand.constant] : NULL;
	}
	value = zend_native_object_slot(execute_data, type, operand);
	if (value != NULL && type == IS_VAR && Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	return value;
}

static zval *zend_native_object_read(
	zend_execute_data *execute_data, const zend_op *opline,
	uint8_t type, znode_op operand)
{
	zval *value;

	if (type == IS_CONST) {
		return RT_CONSTANT(opline, operand);
	}
	value = zend_native_object_slot(execute_data, type, operand);
	if (value != NULL && type == IS_VAR && Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	return value;
}

static zval *zend_native_object_receiver(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *receiver;

	if (opline->op1_type == IS_UNUSED) {
		receiver = &execute_data->This;
	} else {
		receiver = zend_native_object_read(
			execute_data, opline, opline->op1_type, opline->op1);
	}
	if (receiver != NULL && Z_ISREF_P(receiver)) {
		receiver = Z_REFVAL_P(receiver);
	}
	return receiver;
}

static zend_string *zend_native_object_name(
	zend_execute_data *execute_data, const zend_op *opline,
	zend_string **temporary)
{
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);

	*temporary = NULL;
	if (property == NULL) {
		return NULL;
	}
	if (opline->op2_type == IS_CONST && Z_TYPE_P(property) == IS_STRING) {
		return Z_STR_P(property);
	}
	return zval_try_get_tmp_string(property, temporary);
}

static void zend_native_object_replace(zval *target, zval *value)
{
	if (target == value) {
		if (Z_ISREF_P(target)) {
			zend_unwrap_reference(target);
		}
		return;
	}
	/* Opcode result slots are dead on entry, but temporary stack storage is
	 * intentionally not cleared when a frame is recycled.  Destructing the
	 * stale bits here can therefore release a value owned by an older frame.
	 * Native opcode implementations must overwrite result slots exactly like
	 * their VM counterparts and only consume explicit source operands. */
	ZVAL_COPY_DEREF(target, value);
}

static void zend_native_object_consume(
	zend_execute_data *execute_data, uint8_t type, znode_op operand,
	const zval *preserve)
{
	zval *slot;

	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_TMP_VAR && type != IS_VAR) {
		return;
	}
	slot = zend_native_object_slot(execute_data, type, operand);
	if (slot != NULL && slot != preserve && !Z_ISUNDEF_P(slot)) {
		zval_ptr_dtor_nogc(slot);
		ZVAL_UNDEF(slot);
	}
}

static zend_native_status zend_native_object_status(void)
{
	return EG(exception) == NULL ? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_throw_source_zval(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_object_opline(
		execute_data, source_opline_index);
	zval *value;

	if (opline == NULL || opline->opcode != ZEND_THROW
			|| (opline->op1_type != IS_CONST
				&& opline->op1_type != IS_TMP_VAR
				&& opline->op1_type != IS_CV)
			|| opline->op2_type != IS_UNUSED
			|| opline->result_type != IS_UNUSED) {
		zend_throw_error(NULL, "Malformed native throw source operation");
		return ZEND_NATIVE_EXCEPTION;
	}
	value = zend_native_object_read(
		execute_data, opline, opline->op1_type, opline->op1);
	if (value == NULL) {
		zend_throw_error(NULL, "Malformed native throw operand");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CV && UNEXPECTED(Z_TYPE_P(value) == IS_UNDEF)) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

		if (variable_index >= execute_data->func->op_array.last_var) {
			zend_throw_error(NULL, "Malformed native throw variable");
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_WARNING, "Undefined variable $%s",
			ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		if (EG(exception) != NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
		value = &EG(uninitialized_zval);
	}
	if (Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	if (Z_TYPE_P(value) != IS_OBJECT) {
		zend_throw_error(NULL, "Can only throw objects");
		zend_native_object_consume(
			execute_data, opline->op1_type, opline->op1, NULL);
		return ZEND_NATIVE_EXCEPTION;
	}
	Z_TRY_ADDREF_P(value);
	zend_throw_exception_object(value);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, NULL);
	return ZEND_NATIVE_EXCEPTION;
}

static zend_native_status zend_native_declare_anon_class(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_class_entry *class_entry;
	zend_string *runtime_key;
	zval *class_slot;
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL || opline->op1_type != IS_CONST
			|| Z_TYPE_P(RT_CONSTANT(opline, opline->op1)) != IS_STRING) {
		zend_throw_error(NULL, "Malformed native anonymous class declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	runtime_key = Z_STR_P(RT_CONSTANT(opline, opline->op1));
	class_slot = zend_hash_find_known_hash(EG(class_table), runtime_key);
	if (class_slot == NULL || Z_TYPE_P(class_slot) != IS_PTR) {
		zend_throw_error(NULL, "Missing native anonymous class declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	class_entry = Z_CE_P(class_slot);
	if ((class_entry->ce_flags & ZEND_ACC_LINKED) == 0) {
		zend_string *parent_name = opline->op2_type == IS_CONST
			? Z_STR_P(RT_CONSTANT(opline, opline->op2)) : NULL;

		class_entry = zend_do_link_class(
			class_entry, parent_name, runtime_key);
		if (class_entry == NULL || EG(exception) != NULL) {
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	ZVAL_CE(result, class_entry);
	return ZEND_NATIVE_RETURNED;
}

static zend_native_status zend_native_declare_function(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_op_array *op_array;
	zend_function *function;
	zval *name;

	if (execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| opline->opcode != ZEND_DECLARE_FUNCTION
			|| opline->op1_type != IS_CONST
			|| opline->op2.num >= execute_data->func->op_array.num_dynamic_func_defs
			|| execute_data->func->op_array.dynamic_func_defs == NULL) {
		zend_throw_error(NULL, "Malformed native function declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	op_array = &execute_data->func->op_array;
	name = RT_CONSTANT(opline, opline->op1);
	if (Z_TYPE_P(name) != IS_STRING) {
		zend_throw_error(NULL, "Malformed native function declaration name");
		return ZEND_NATIVE_EXCEPTION;
	}
	function = (zend_function *)
		op_array->dynamic_func_defs[opline->op2.num];
	if (function == NULL) {
		zend_throw_error(NULL, "Missing native function declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	do_bind_function(function, name);
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

static zend_native_status zend_native_declare_class(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *class_name;
	zend_string *parent_name = NULL;

	if (opline->opcode != ZEND_DECLARE_CLASS
			|| opline->op1_type != IS_CONST) {
		zend_throw_error(NULL, "Malformed native class declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	class_name = RT_CONSTANT(opline, opline->op1);
	if (Z_TYPE_P(class_name) != IS_STRING) {
		zend_throw_error(NULL, "Malformed native class declaration name");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op2_type == IS_CONST) {
		zval *parent = RT_CONSTANT(opline, opline->op2);

		if (Z_TYPE_P(parent) != IS_STRING) {
			zend_throw_error(NULL, "Malformed native class parent name");
			return ZEND_NATIVE_EXCEPTION;
		}
		parent_name = Z_STR_P(parent);
	} else if (opline->op2_type != IS_UNUSED) {
		zend_throw_error(NULL, "Malformed native class declaration parent");
		return ZEND_NATIVE_EXCEPTION;
	}
	do_bind_class(class_name, parent_name);
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

static zend_native_status zend_native_declare_class_delayed(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_op_array *op_array = &execute_data->func->op_array;
	zval *lowercase_name;
	zval *runtime_definition;
	zval *parent;
	void **cache_slot;
	zend_class_entry *class_entry;

	if (opline->opcode != ZEND_DECLARE_CLASS_DELAYED
			|| opline->op1_type != IS_CONST
			|| opline->op2_type != IS_CONST
			|| execute_data->run_time_cache == NULL
			|| opline->extended_value > op_array->cache_size
			|| sizeof(void *) > op_array->cache_size - opline->extended_value) {
		zend_throw_error(NULL, "Malformed delayed native class declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	lowercase_name = RT_CONSTANT(opline, opline->op1);
	parent = RT_CONSTANT(opline, opline->op2);
	if (Z_TYPE_P(lowercase_name) != IS_STRING
			|| Z_TYPE_P(lowercase_name + 1) != IS_STRING
			|| Z_TYPE_P(parent) != IS_STRING) {
		zend_throw_error(NULL, "Malformed delayed native class metadata");
		return ZEND_NATIVE_EXCEPTION;
	}
	cache_slot = (void **) ((char *) execute_data->run_time_cache
		+ opline->extended_value);
	class_entry = (zend_class_entry *) cache_slot[0];
	if (class_entry == NULL) {
		runtime_definition = zend_hash_find_known_hash(
			EG(class_table), Z_STR_P(lowercase_name + 1));
		if (runtime_definition != NULL) {
			class_entry = zend_bind_class_in_slot(
				runtime_definition, lowercase_name, Z_STR_P(parent));
			if (EG(exception) != NULL) {
				return ZEND_NATIVE_EXCEPTION;
			}
		}
		cache_slot[0] = class_entry;
	}
	return ZEND_NATIVE_RETURNED;
}

static zend_native_status zend_native_declare_lambda(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_function *function;
	zend_class_entry *called_scope;
	zval *receiver = NULL;
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| opline->op2.num
				>= execute_data->func->op_array.num_dynamic_func_defs
			|| execute_data->func->op_array.dynamic_func_defs == NULL) {
		zend_throw_error(NULL, "Malformed native closure declaration");
		return ZEND_NATIVE_EXCEPTION;
	}
	function = (zend_function *) execute_data->func->op_array
		.dynamic_func_defs[opline->op2.num];
	if (function == NULL) {
		zend_throw_error(NULL, "Missing native closure body");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE(execute_data->This) == IS_OBJECT) {
		called_scope = Z_OBJCE(execute_data->This);
		if ((function->common.fn_flags & ZEND_ACC_STATIC) == 0
				&& (execute_data->func->common.fn_flags & ZEND_ACC_STATIC) == 0) {
			receiver = &execute_data->This;
		}
	} else {
		called_scope = Z_CE(execute_data->This);
	}
	zend_create_closure(result, function, execute_data->func->op_array.scope,
		called_scope, receiver);
	return zend_native_object_status();
}

static zend_native_status zend_native_bind_lexical(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *closure = zend_native_object_read(
		execute_data, opline, opline->op1_type, opline->op1);
	zval *variable = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);

	if (closure == NULL || Z_TYPE_P(closure) != IS_OBJECT
			|| variable == NULL) {
		zend_throw_error(NULL, "Malformed native closure binding");
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((opline->extended_value & ZEND_BIND_REF) != 0) {
		if (Z_ISREF_P(variable)) {
			Z_ADDREF_P(variable);
		} else {
			ZVAL_MAKE_REF_EX(variable, 2);
		}
	} else {
		if (Z_ISUNDEF_P(variable)
				&& (opline->extended_value & ZEND_BIND_IMPLICIT) == 0) {
			zend_throw_error(NULL, "Undefined variable captured by native closure");
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_DEREF(variable);
		Z_TRY_ADDREF_P(variable);
	}
	zend_closure_bind_var_ex(closure,
		opline->extended_value & ~(ZEND_BIND_REF | ZEND_BIND_IMPLICIT),
		variable);
	return zend_native_object_status();
}

static zend_native_status zend_native_bind_static(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_op_array *op_array = &execute_data->func->op_array;
	HashTable *static_variables = ZEND_MAP_PTR_GET(op_array->static_variables_ptr);
	zval *variable = zend_native_object_slot(
		execute_data, opline->op1_type, opline->op1);
	zval *value;
	uint32_t offset = opline->extended_value
		& ~(ZEND_BIND_REF | ZEND_BIND_IMPLICIT | ZEND_BIND_EXPLICIT);

	if (variable == NULL || op_array->static_variables == NULL) {
		zend_throw_error(NULL, "Malformed native static binding");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(variable) == IS_INDIRECT) {
		variable = Z_INDIRECT_P(variable);
	}
	if (static_variables == NULL) {
		static_variables = zend_array_dup(op_array->static_variables);
		ZEND_MAP_PTR_SET(op_array->static_variables_ptr, static_variables);
	}
	if (offset >= HT_USED_SIZE(static_variables)) {
		zend_throw_error(NULL, "Native static binding offset is out of range");
		return ZEND_NATIVE_EXCEPTION;
	}
	value = (zval *) ((char *) static_variables->arData + offset);
	if ((opline->extended_value & ZEND_BIND_REF) != 0) {
		if (!Z_ISREF_P(value)) {
			zend_reference *reference = emalloc(sizeof(*reference));
			zval *initial = opline->op2_type == IS_UNUSED ? value
				: zend_native_object_read(execute_data, opline,
					opline->op2_type, opline->op2);

			if (initial == NULL) {
				efree(reference);
				zend_throw_error(NULL, "Malformed native static initializer");
				return ZEND_NATIVE_EXCEPTION;
			}
			GC_SET_REFCOUNT(reference, 2);
			GC_TYPE_INFO(reference) = GC_REFERENCE;
			if (opline->op2_type == IS_UNUSED) {
				ZVAL_COPY_VALUE(&reference->val, value);
			} else {
				ZVAL_DEREF(initial);
				ZVAL_COPY(&reference->val, initial);
				zend_native_object_consume(execute_data,
					opline->op2_type, opline->op2, NULL);
			}
			reference->sources.ptr = NULL;
			Z_REF_P(value) = reference;
			Z_TYPE_INFO_P(value) = IS_REFERENCE_EX;
			zval_ptr_dtor(variable);
			ZVAL_REF(variable, reference);
		} else {
			Z_ADDREF_P(value);
			zval_ptr_dtor(variable);
			ZVAL_REF(variable, Z_REF_P(value));
			zend_native_object_consume(execute_data,
				opline->op2_type, opline->op2, NULL);
		}
	} else {
		zval_ptr_dtor(variable);
		ZVAL_COPY(variable, value);
	}
	return zend_native_object_status();
}

static zend_native_status zend_native_object_bad_receiver(
	zval *receiver, zval *property, bool read_context)
{
	if (read_context) {
		zend_wrong_property_read(receiver, property);
	} else {
		zend_throw_error(NULL, "Attempt to modify property on %s",
			zend_zval_value_name(receiver));
	}
	return zend_native_object_status();
}

static bool zend_native_object_apply_fetch_flags(
	zval *result, zend_property_info *property_info, uint32_t flags)
{
	zval *value = Z_TYPE_P(result) == IS_INDIRECT
		? Z_INDIRECT_P(result) : result;

	flags &= ZEND_FETCH_OBJ_FLAGS;
	if (flags == 0 || property_info == NULL
			|| property_info == ZEND_WRONG_PROPERTY_INFO) {
		return true;
	}
	if (flags == ZEND_FETCH_REF) {
		if (!Z_ISREF_P(value)) {
			if (Z_ISUNDEF_P(value)) {
				if (!ZEND_TYPE_ALLOW_NULL(property_info->type)) {
					zend_throw_error(NULL,
						"Cannot access uninitialized non-nullable property %s::$%s by reference",
						ZSTR_VAL(property_info->ce->name),
						zend_get_unmangled_property_name(property_info->name));
					ZVAL_ERROR(result);
					return false;
				}
				ZVAL_NULL(value);
			}
			ZVAL_NEW_REF(value, value);
			ZEND_REF_ADD_TYPE_SOURCE(Z_REF_P(value), property_info);
		}
		return true;
	}
	if (flags == ZEND_FETCH_DIM_WRITE && Z_TYPE_P(value) <= IS_FALSE
			&& ZEND_TYPE_IS_SET(property_info->type)
			&& (ZEND_TYPE_FULL_MASK(property_info->type) & MAY_BE_ARRAY) == 0) {
		zend_string *type = zend_type_to_string(property_info->type);

		zend_type_error(
			"Cannot auto-initialize an array inside property %s::$%s of type %s",
			ZSTR_VAL(property_info->ce->name),
			zend_get_unmangled_property_name(property_info->name),
			ZSTR_VAL(type));
		zend_string_release(type);
		ZVAL_ERROR(result);
		return false;
	}
	return true;
}

static zend_native_status zend_native_object_fetch(
	zend_execute_data *execute_data, const zend_op *opline, int fetch_type)
{
	zval *receiver = zend_native_object_receiver(execute_data, opline);
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zend_string *temporary;
	zend_string *name;
	zend_property_info *property_info;
	zval *value;

	if (receiver == NULL || property == NULL || result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		ZVAL_NULL(result);
		return zend_native_object_bad_receiver(
			receiver, property,
			fetch_type == BP_VAR_R || fetch_type == BP_VAR_IS);
	}
	name = zend_native_object_name(execute_data, opline, &temporary);
	if (name == NULL) {
		ZVAL_UNDEF(result);
		return zend_native_object_status();
	}
	property_info = zend_get_property_info(Z_OBJCE_P(receiver), name, 1);
	if (fetch_type == BP_VAR_R || fetch_type == BP_VAR_IS) {
		value = Z_OBJ_HT_P(receiver)->read_property(
			Z_OBJ_P(receiver), name, fetch_type, NULL, result);
		if (value != result) {
			zend_native_object_replace(result, value);
		} else if (Z_ISREF_P(result)) {
			zend_unwrap_reference(result);
		}
	} else {
		value = Z_OBJ_HT_P(receiver)->get_property_ptr_ptr(
			Z_OBJ_P(receiver), name, fetch_type, NULL);
		if (value == NULL) {
			value = Z_OBJ_HT_P(receiver)->read_property(
				Z_OBJ_P(receiver), name, fetch_type, NULL, result);
		}
		if (value != result) {
			ZVAL_INDIRECT(result, value);
		}
		if (!zend_native_object_apply_fetch_flags(
				result, property_info, opline->extended_value)) {
			if (temporary != NULL) {
				zend_tmp_string_release(temporary);
			}
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_fetch_r_explicit(
	zend_execute_data *execute_data,
	const zend_native_explicit_object_operation *operation)
{
	zval *receiver;
	zval *property = zend_native_object_read_explicit(
		execute_data, operation->op2_type, operation->op2);
	zval *result = zend_native_object_slot(
		execute_data, operation->result_type, operation->result);
	zend_string *temporary = NULL;
	zend_string *name;
	zval *value;
	void **cache_slot = NULL;

	if (operation->op1_type == IS_UNUSED) {
		receiver = &execute_data->This;
	} else {
		receiver = zend_native_object_read_explicit(
			execute_data, operation->op1_type, operation->op1);
	}
	if (receiver != NULL && Z_ISREF_P(receiver)) {
		receiver = Z_REFVAL_P(receiver);
	}
	if (receiver == NULL || property == NULL || result == NULL) {
		zend_throw_error(NULL, "Malformed native object read operands");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		ZVAL_NULL(result);
		return zend_native_object_bad_receiver(receiver, property, true);
	}
	if (operation->op2_type == IS_CONST
			&& Z_TYPE_P(property) == IS_STRING) {
		name = Z_STR_P(property);
		if (execute_data->run_time_cache != NULL) {
			uint32_t cache_offset =
				operation->extended_value & ~ZEND_FETCH_REF;
			const uint32_t cache_size =
				execute_data->func->op_array.cache_size;

			if (cache_offset > cache_size
					|| 3 * sizeof(void *) > cache_size - cache_offset) {
				zend_throw_error(NULL,
					"Malformed native object property cache offset");
				return ZEND_NATIVE_EXCEPTION;
			}
			cache_slot = (void **) (
				(char *) execute_data->run_time_cache + cache_offset);
		}
	} else {
		name = zval_try_get_tmp_string(property, &temporary);
		if (name == NULL) {
			ZVAL_UNDEF(result);
			return zend_native_object_status();
		}
	}
	value = Z_OBJ_HT_P(receiver)->read_property(
		Z_OBJ_P(receiver), name, BP_VAR_R, cache_slot, result);
	if (value != result) {
		zend_native_object_replace(result, value);
	} else if (Z_ISREF_P(result)) {
		zend_unwrap_reference(result);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, operation->op2_type, operation->op2, result);
	zend_native_object_consume(
		execute_data, operation->op1_type, operation->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_assign(
	zend_execute_data *execute_data, const zend_op *opline, bool by_reference)
{
	const zend_op *data_opline = opline + 1;
	zval *receiver = zend_native_object_receiver(execute_data, opline);
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);
	zval *value;
	zval *written;
	zval *result = opline->result_type == IS_UNUSED ? NULL
		: zend_native_object_slot(
			execute_data, opline->result_type, opline->result);
	zend_string *temporary;
	zend_string *name;

	if ((uint32_t) (data_opline - execute_data->func->op_array.opcodes)
			>= execute_data->func->op_array.last
			|| data_opline->opcode != ZEND_OP_DATA) {
		return ZEND_NATIVE_EXCEPTION;
	}
	value = zend_native_object_read(
		execute_data, data_opline, data_opline->op1_type, data_opline->op1);
	if (receiver == NULL || property == NULL || value == NULL
			|| (result == NULL && opline->result_type != IS_UNUSED)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		if (result != NULL) {
			ZVAL_NULL(result);
		}
		return zend_native_object_bad_receiver(
			receiver, property, false);
	}
	name = zend_native_object_name(execute_data, opline, &temporary);
	if (name == NULL) {
		if (result != NULL) {
			ZVAL_UNDEF(result);
		}
		return zend_native_object_status();
	}
	if (by_reference && !Z_ISREF_P(value)) {
		ZVAL_MAKE_REF(value);
	}
	written = Z_OBJ_HT_P(receiver)->write_property(
		Z_OBJ_P(receiver), name, value, NULL);
	if (result != NULL && written != NULL) {
		zend_native_object_replace(result, written);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, data_opline->op1_type, data_opline->op1, result);
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_assign_op(
	zend_execute_data *execute_data, const zend_op *opline)
{
	const zend_op *data_opline = opline + 1;
	zval *receiver = zend_native_object_receiver(execute_data, opline);
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);
	zval *right;
	zval current;
	zval updated;
	zval *read;
	zval *written;
	zval *result = opline->result_type == IS_UNUSED ? NULL
		: zend_native_object_slot(
			execute_data, opline->result_type, opline->result);
	zend_string *temporary;
	zend_string *name;
	binary_op_type operation;

	ZVAL_UNDEF(&current);
	ZVAL_UNDEF(&updated);
	if ((uint32_t) (data_opline - execute_data->func->op_array.opcodes)
			>= execute_data->func->op_array.last
			|| data_opline->opcode != ZEND_OP_DATA
			|| receiver == NULL || property == NULL
			|| Z_TYPE_P(receiver) != IS_OBJECT) {
		return ZEND_NATIVE_EXCEPTION;
	}
	right = zend_native_object_read(
		execute_data, data_opline, data_opline->op1_type, data_opline->op1);
	name = zend_native_object_name(execute_data, opline, &temporary);
	operation = get_binary_op(opline->extended_value);
	if (right == NULL || name == NULL || operation == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	read = Z_OBJ_HT_P(receiver)->read_property(
		Z_OBJ_P(receiver), name, BP_VAR_RW, NULL, &current);
	if (read != &current) {
		ZVAL_COPY_DEREF(&current, read);
	} else if (Z_ISREF_P(&current)) {
		zend_unwrap_reference(&current);
	}
	if (EG(exception) == NULL
			&& operation(&updated, &current, right) == SUCCESS) {
		written = Z_OBJ_HT_P(receiver)->write_property(
			Z_OBJ_P(receiver), name, &updated, NULL);
		if (result != NULL && written != NULL) {
			zend_native_object_replace(result, written);
		}
	}
	if (!Z_ISUNDEF(updated)) {
		zval_ptr_dtor(&updated);
	}
	if (!Z_ISUNDEF(current)) {
		zval_ptr_dtor(&current);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, data_opline->op1_type, data_opline->op1, result);
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_unset(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *receiver = zend_native_object_receiver(execute_data, opline);
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);
	zend_string *temporary;
	zend_string *name;

	if (receiver == NULL || property == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(receiver) != IS_OBJECT) {
		return zend_native_object_bad_receiver(
			receiver, property, false);
	}
	name = zend_native_object_name(execute_data, opline, &temporary);
	if (name == NULL) {
		return zend_native_object_status();
	}
	Z_OBJ_HT_P(receiver)->unset_property(Z_OBJ_P(receiver), name, NULL);
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, NULL);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, NULL);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_isset(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *receiver = zend_native_object_receiver(execute_data, opline);
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zend_string *temporary;
	zend_string *name;
	bool isempty = (opline->extended_value & ZEND_ISEMPTY) != 0;
	bool found = false;

	if (receiver == NULL || property == NULL || result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(receiver) == IS_OBJECT) {
		name = zend_native_object_name(execute_data, opline, &temporary);
		if (name != NULL) {
			found = Z_OBJ_HT_P(receiver)->has_property(
				Z_OBJ_P(receiver), name, isempty, NULL) != 0;
		}
		if (temporary != NULL) {
			zend_tmp_string_release(temporary);
		}
	}
	ZVAL_BOOL(result, isempty ^ found);
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_incdec(
	zend_execute_data *execute_data, const zend_op *opline,
	bool post, bool decrement)
{
	zval *receiver = zend_native_object_receiver(execute_data, opline);
	zval *property = zend_native_object_read(
		execute_data, opline, opline->op2_type, opline->op2);
	zval *result = opline->result_type == IS_UNUSED ? NULL
		: zend_native_object_slot(
			execute_data, opline->result_type, opline->result);
	zend_string *temporary;
	zend_string *name;
	zval current;
	zval *read;
	zval *written;

	ZVAL_UNDEF(&current);
	if (receiver == NULL || property == NULL
			|| Z_TYPE_P(receiver) != IS_OBJECT) {
		return ZEND_NATIVE_EXCEPTION;
	}
	name = zend_native_object_name(execute_data, opline, &temporary);
	if (name == NULL) {
		return zend_native_object_status();
	}
	read = Z_OBJ_HT_P(receiver)->read_property(
		Z_OBJ_P(receiver), name, BP_VAR_RW, NULL, &current);
	if (read != &current) {
		ZVAL_COPY_DEREF(&current, read);
	} else if (Z_ISREF_P(&current)) {
		zend_unwrap_reference(&current);
	}
	if (result != NULL && post) {
		zend_native_object_replace(result, &current);
	}
	if (EG(exception) == NULL
			&& (decrement ? decrement_function(&current)
				: increment_function(&current)) == SUCCESS) {
		written = Z_OBJ_HT_P(receiver)->write_property(
			Z_OBJ_P(receiver), name, &current, NULL);
		if (result != NULL && !post && written != NULL) {
			zend_native_object_replace(result, written);
		}
	}
	if (!Z_ISUNDEF(current)) {
		zval_ptr_dtor(&current);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_instanceof(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *value = zend_native_object_read(
		execute_data, opline, opline->op1_type, opline->op1);
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zend_class_entry *class_entry = NULL;

	if (value == NULL || result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	while (Z_ISREF_P(value)) {
		value = Z_REFVAL_P(value);
	}
	if (opline->op2_type == IS_CONST) {
		zval *name = RT_CONSTANT(opline, opline->op2);
		if (Z_TYPE_P(name) == IS_STRING) {
			zval *lower_name = name + 1;
			class_entry = zend_lookup_class_ex(
				Z_STR_P(name), Z_TYPE_P(lower_name) == IS_STRING
					? Z_STR_P(lower_name) : NULL,
				ZEND_FETCH_CLASS_NO_AUTOLOAD);
		}
	} else if (opline->op2_type == IS_UNUSED) {
		class_entry = zend_fetch_class(NULL, opline->op2.num);
	} else {
		zval *class_value = zend_native_object_read(
			execute_data, opline, opline->op2_type, opline->op2);
		if (class_value != NULL && Z_TYPE_P(class_value) == IS_PTR) {
			class_entry = Z_PTR_P(class_value);
		}
	}
	ZVAL_BOOL(result, Z_TYPE_P(value) == IS_OBJECT && class_entry != NULL
		&& instanceof_function(Z_OBJCE_P(value), class_entry));
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_object_clone(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *value = zend_native_object_receiver(execute_data, opline);
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zend_object *clone;

	if (value == NULL || result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_TYPE_P(value) != IS_OBJECT) {
		ZVAL_UNDEF(result);
		zend_type_error("clone(): Argument #1 ($object) must be of type object, %s given",
			zend_zval_value_name(value));
		return ZEND_NATIVE_EXCEPTION;
	}
	if (Z_OBJ_HT_P(value)->clone_obj == NULL) {
		ZVAL_UNDEF(result);
		zend_throw_error(NULL, "Trying to clone an uncloneable object of class %s",
			ZSTR_VAL(Z_OBJCE_P(value)->name));
		return ZEND_NATIVE_EXCEPTION;
	}
	clone = Z_OBJ_HT_P(value)->clone_obj(Z_OBJ_P(value));
	if (clone == NULL) {
		ZVAL_UNDEF(result);
		return zend_native_object_status();
	}
	ZVAL_OBJ(result, clone);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_class_entry *zend_native_static_class(
	zend_execute_data *execute_data, const zend_op *opline, znode_op operand,
	uint8_t type)
{
	zval *class_name;

	if (type == IS_CONST) {
		class_name = RT_CONSTANT(opline, operand);
		if (Z_TYPE_P(class_name) != IS_STRING) {
			zend_type_error("Class name must be a valid object or a string");
			return NULL;
		}
		return zend_fetch_class_by_name(
			Z_STR_P(class_name),
			Z_TYPE_P(class_name + 1) == IS_STRING ? Z_STR_P(class_name + 1) : NULL,
			ZEND_FETCH_CLASS_DEFAULT | ZEND_FETCH_CLASS_EXCEPTION);
	}
	if (type == IS_UNUSED) {
		return zend_fetch_class(NULL, operand.num);
	}
	class_name = zend_native_object_read(
		execute_data, opline, type, operand);
	if (class_name == NULL || Z_TYPE_P(class_name) != IS_PTR
			|| Z_CE_P(class_name) == NULL) {
		zend_type_error("Class name must be a valid object or a string");
		return NULL;
	}
	return Z_CE_P(class_name);
}

static zend_string *zend_native_static_name(
	zend_execute_data *execute_data, const zend_op *opline, znode_op operand,
	uint8_t type, zend_string **temporary)
{
	zval *name = zend_native_object_read(
		execute_data, opline, type, operand);

	*temporary = NULL;
	if (name == NULL) {
		return NULL;
	}
	if (type == IS_CONST && Z_TYPE_P(name) == IS_STRING) {
		return Z_STR_P(name);
	}
	return zval_try_get_tmp_string(name, temporary);
}

static zval *zend_native_static_property(
	zend_execute_data *execute_data, const zend_op *opline, int fetch_type,
	zend_property_info **property_info, zend_string **temporary)
{
	zend_class_entry *class_entry = zend_native_static_class(
		execute_data, opline, opline->op2, opline->op2_type);
	zend_string *name;

	*property_info = NULL;
	*temporary = NULL;
	if (class_entry == NULL) {
		return NULL;
	}
	name = zend_native_static_name(
		execute_data, opline, opline->op1, opline->op1_type, temporary);
	if (name == NULL) {
		return NULL;
	}
	return zend_std_get_static_property_with_info(
		class_entry, name, fetch_type, property_info);
}

static bool zend_native_static_indirect_access_allowed(
	zend_property_info *property_info, zval *property, int fetch_type)
{
	if (property_info == NULL || property == NULL
			|| !(property_info->flags & ZEND_ACC_PPP_SET_MASK)
			|| (fetch_type != BP_VAR_W && fetch_type != BP_VAR_RW
				&& fetch_type != BP_VAR_UNSET)
			|| zend_asymmetric_property_has_set_access(property_info)) {
		return true;
	}
	if (Z_TYPE_P(property) == IS_OBJECT) {
		return false;
	}
	if (fetch_type != BP_VAR_UNSET || Z_TYPE_P(property) != IS_UNDEF) {
		zend_asymmetric_visibility_property_modification_error(
			property_info, "indirectly modify");
	}
	return false;
}

static zend_native_status zend_native_static_fetch(
	zend_execute_data *execute_data, const zend_op *opline, int fetch_type)
{
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zend_property_info *property_info;
	zend_string *temporary;
	zval *property = zend_native_static_property(
		execute_data, opline, fetch_type, &property_info, &temporary);
	bool copy = fetch_type == BP_VAR_R || fetch_type == BP_VAR_IS;

	if (result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (property == NULL) {
		property = &EG(uninitialized_zval);
	} else if (!zend_native_static_indirect_access_allowed(
			property_info, property, fetch_type)) {
		/* Object values remain readable for asymmetric indirect access. */
		copy = Z_TYPE_P(property) == IS_OBJECT;
		if (!copy) {
			property = &EG(uninitialized_zval);
		}
	}
	if (copy) {
		zend_native_object_replace(result, property);
	} else {
		ZVAL_INDIRECT(result, property);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_static_assign(
	zend_execute_data *execute_data, const zend_op *opline, bool by_reference)
{
	const zend_op *data_opline = opline + 1;
	zend_property_info *property_info;
	zend_string *temporary;
	zval *property;
	zval *value;
	zval *assigned;
	zval *result = opline->result_type == IS_UNUSED ? NULL
		: zend_native_object_slot(
			execute_data, opline->result_type, opline->result);
	zend_refcounted *garbage = NULL;
	bool strict = ZEND_CALL_USES_STRICT_TYPES(execute_data);

	if ((uint32_t) (data_opline - execute_data->func->op_array.opcodes)
			>= execute_data->func->op_array.last
			|| data_opline->opcode != ZEND_OP_DATA) {
		return ZEND_NATIVE_EXCEPTION;
	}
	property = zend_native_static_property(
		execute_data, opline, BP_VAR_W, &property_info, &temporary);
	value = zend_native_object_read(
		execute_data, data_opline, data_opline->op1_type, data_opline->op1);
	if (property == NULL || property_info == NULL || value == NULL) {
		if (temporary != NULL) {
			zend_tmp_string_release(temporary);
		}
		return zend_native_object_status();
	}

	assigned = property;
	if (by_reference) {
		if (property_info->flags & ZEND_ACC_PPP_SET_MASK
				&& !zend_asymmetric_property_has_set_access(property_info)) {
			zend_asymmetric_visibility_property_modification_error(
				property_info, "indirectly modify");
			assigned = &EG(uninitialized_zval);
		} else if (ZEND_TYPE_IS_SET(property_info->type)
				&& !zend_verify_prop_assignable_by_ref(
					property_info, value, strict)) {
			assigned = &EG(uninitialized_zval);
		} else {
			zend_reference *reference;
			if (!Z_ISREF_P(value)) {
				ZVAL_MAKE_REF(value);
			}
			if (Z_ISREF_P(property)) {
				ZEND_REF_DEL_TYPE_SOURCE(Z_REF_P(property), property_info);
			}
			reference = Z_REF_P(value);
			GC_ADDREF(reference);
			if (Z_REFCOUNTED_P(property)) {
				garbage = Z_COUNTED_P(property);
			}
			ZVAL_REF(property, reference);
			if (ZEND_TYPE_IS_SET(property_info->type)) {
				ZEND_REF_ADD_TYPE_SOURCE(reference, property_info);
			}
		}
	} else if (ZEND_TYPE_IS_SET(property_info->type)) {
		zval typed_value;
		if ((property_info->flags & ZEND_ACC_READONLY)
				&& !(Z_PROP_FLAG_P(property) & IS_PROP_REINITABLE)) {
			zend_readonly_property_modification_error(property_info);
			assigned = &EG(uninitialized_zval);
		} else if ((property_info->flags & ZEND_ACC_PPP_SET_MASK)
				&& !zend_asymmetric_property_has_set_access(property_info)) {
			zend_asymmetric_visibility_property_modification_error(
				property_info, "modify");
			assigned = &EG(uninitialized_zval);
		} else {
			ZVAL_COPY_DEREF(&typed_value, value);
			if (zend_verify_property_type(property_info, &typed_value, strict)) {
				Z_PROP_FLAG_P(property) &= ~IS_PROP_REINITABLE;
				assigned = zend_assign_to_variable_ex(
					property, &typed_value, IS_TMP_VAR, strict, &garbage);
			} else {
				zval_ptr_dtor(&typed_value);
				assigned = &EG(uninitialized_zval);
			}
		}
	} else {
		assigned = zend_assign_to_variable_ex(
			property, value, data_opline->op1_type, strict, &garbage);
	}
	if (result != NULL) {
		zend_native_object_replace(result, assigned);
	}
	if (garbage != NULL) {
		GC_DTOR(garbage);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, data_opline->op1_type, data_opline->op1, result);
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_static_update(
	zend_execute_data *execute_data, const zend_op *opline, bool assign_op,
	bool post, bool decrement)
{
	const zend_op *data_opline = opline + 1;
	zend_property_info *property_info;
	zend_string *temporary;
	zval *property = zend_native_static_property(
		execute_data, opline, BP_VAR_RW, &property_info, &temporary);
	zval *target = property;
	zval *right = NULL;
	zval updated;
	zval original;
	zval *result = opline->result_type == IS_UNUSED ? NULL
		: zend_native_object_slot(
			execute_data, opline->result_type, opline->result);
	bool strict = ZEND_CALL_USES_STRICT_TYPES(execute_data);
	bool valid = false;

	ZVAL_UNDEF(&updated);
	ZVAL_UNDEF(&original);
	if (property == NULL || property_info == NULL) {
		if (temporary != NULL) {
			zend_tmp_string_release(temporary);
		}
		return zend_native_object_status();
	}
	if ((property_info->flags & ZEND_ACC_PPP_SET_MASK)
			&& !zend_asymmetric_property_has_set_access(property_info)) {
		zend_asymmetric_visibility_property_modification_error(
			property_info, "indirectly modify");
		goto done;
	}
	if (Z_ISREF_P(target)) {
		target = Z_REFVAL_P(target);
	}
	ZVAL_COPY(&original, target);
	if (assign_op) {
		binary_op_type operation;
		if ((uint32_t) (data_opline - execute_data->func->op_array.opcodes)
				>= execute_data->func->op_array.last
				|| data_opline->opcode != ZEND_OP_DATA) {
			goto done;
		}
		right = zend_native_object_read(
			execute_data, data_opline, data_opline->op1_type, data_opline->op1);
		operation = get_binary_op(opline->extended_value);
		if (right == NULL || operation == NULL
				|| operation(&updated, target, right) != SUCCESS) {
			goto done;
		}
	} else {
		ZVAL_COPY(&updated, target);
		if ((decrement ? decrement_function(&updated)
				: increment_function(&updated)) != SUCCESS) {
			goto done;
		}
	}
	if (Z_ISREF_P(property) && ZEND_REF_HAS_TYPE_SOURCES(Z_REF_P(property))) {
		valid = zend_verify_ref_assignable_zval(
			Z_REF_P(property), &updated, strict);
	} else if (ZEND_TYPE_IS_SET(property_info->type)) {
		valid = zend_verify_property_type(property_info, &updated, strict);
	} else {
		valid = true;
	}
	if (valid) {
		zval_ptr_dtor(target);
		ZVAL_COPY_VALUE(target, &updated);
		ZVAL_UNDEF(&updated);
		if (result != NULL) {
			zend_native_object_replace(result, post ? &original : target);
		}
	}

done:
	if (!Z_ISUNDEF(updated)) {
		zval_ptr_dtor(&updated);
	}
	if (!Z_ISUNDEF(original)) {
		zval_ptr_dtor(&original);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	if (assign_op) {
		zend_native_object_consume(
			execute_data, data_opline->op1_type, data_opline->op1, result);
	}
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_static_isset(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_property_info *property_info;
	zend_string *temporary;
	zval *property = zend_native_static_property(
		execute_data, opline, BP_VAR_IS, &property_info, &temporary);
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	bool isempty = (opline->extended_value & ZEND_ISEMPTY) != 0;
	bool value;

	if (result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (!isempty) {
		value = property != NULL && Z_TYPE_P(property) > IS_NULL
			&& (!Z_ISREF_P(property)
				|| Z_TYPE_P(Z_REFVAL_P(property)) != IS_NULL);
	} else {
		value = property == NULL || !i_zend_is_true(property);
	}
	ZVAL_BOOL(result, value);
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_static_unset(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_class_entry *class_entry = zend_native_static_class(
		execute_data, opline, opline->op2, opline->op2_type);
	zend_string *temporary;
	zend_string *name = zend_native_static_name(
		execute_data, opline, opline->op1, opline->op1_type, &temporary);

	if (class_entry != NULL && name != NULL) {
		zend_std_unset_static_property(class_entry, name);
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, NULL);
	return zend_native_object_status();
}

static zend_native_status zend_native_class_constant(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zend_class_entry *class_entry = zend_native_static_class(
		execute_data, opline, opline->op1, opline->op1_type);
	zend_string *temporary;
	zend_string *name = zend_native_static_name(
		execute_data, opline, opline->op2, opline->op2_type, &temporary);
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zval *value = NULL;

	if (result == NULL || class_entry == NULL || name == NULL) {
		if (result != NULL) {
			ZVAL_UNDEF(result);
		}
		if (temporary != NULL) {
			zend_tmp_string_release(temporary);
		}
		return zend_native_object_status();
	}
	if (opline->op2_type != IS_CONST
			&& zend_string_equals_literal_ci(name, "class")) {
		ZVAL_STR_COPY(result, class_entry->name);
	} else {
		const zend_class_entry *scope = execute_data->func->op_array.scope;
		value = zend_get_class_constant_ex(
			class_entry->name, name, scope,
			ZEND_FETCH_CLASS_DEFAULT | ZEND_FETCH_CLASS_EXCEPTION);
		if (value != NULL) {
			zend_native_object_replace(result, value);
		} else {
			ZVAL_UNDEF(result);
		}
	}
	if (temporary != NULL) {
		zend_tmp_string_release(temporary);
	}
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_fetch_class(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zval *class_name = NULL;
	zend_class_entry *class_entry = NULL;

	if (result == NULL) {
		zend_throw_error(NULL, "Malformed native class fetch result");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op2_type == IS_UNUSED) {
		class_entry = zend_fetch_class(NULL, opline->op1.num);
	} else if (opline->op2_type == IS_CONST) {
		class_name = RT_CONSTANT(opline, opline->op2);
		if (Z_TYPE_P(class_name) == IS_STRING) {
			class_entry = zend_fetch_class_by_name(
				Z_STR_P(class_name),
				Z_TYPE_P(class_name + 1) == IS_STRING
					? Z_STR_P(class_name + 1) : NULL,
				opline->op1.num);
		} else {
			zend_throw_error(NULL,
				"Class name must be a valid object or a string");
		}
	} else {
		class_name = zend_native_object_read(
			execute_data, opline, opline->op2_type, opline->op2);
		while (class_name != NULL && Z_ISREF_P(class_name)) {
			class_name = Z_REFVAL_P(class_name);
		}
		if (class_name != NULL && Z_TYPE_P(class_name) == IS_OBJECT) {
			class_entry = Z_OBJCE_P(class_name);
		} else if (class_name != NULL && Z_TYPE_P(class_name) == IS_STRING) {
			class_entry = zend_fetch_class(
				Z_STR_P(class_name), opline->op1.num);
		} else {
			if (class_name != NULL && opline->op2_type == IS_CV
					&& Z_ISUNDEF_P(class_name)) {
				uint32_t variable = EX_VAR_TO_NUM(opline->op2.var);

				if (variable < execute_data->func->op_array.last_var) {
					zend_error_unchecked(E_WARNING,
						"Undefined variable $%S",
						execute_data->func->op_array.vars[variable]);
				}
			}
			if (EG(exception) == NULL) {
				zend_throw_error(NULL,
					"Class name must be a valid object or a string");
			}
		}
	}
	if (class_entry != NULL) {
		ZVAL_CE(result, class_entry);
	} else {
		ZVAL_UNDEF(result);
	}
	zend_native_object_consume(
		execute_data, opline->op2_type, opline->op2, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_fetch_class_name(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zend_class_entry *scope;

	if (result == NULL) {
		zend_throw_error(NULL, "Malformed native class-name fetch result");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type != IS_UNUSED) {
		zval *object = zend_native_object_read(
			execute_data, opline, opline->op1_type, opline->op1);

		while (object != NULL && Z_ISREF_P(object)) {
			object = Z_REFVAL_P(object);
		}
		if (object == NULL || Z_TYPE_P(object) != IS_OBJECT) {
			zend_type_error("Cannot use \"::class\" on %s",
				object == NULL ? "an invalid value" : zend_zval_value_name(object));
			ZVAL_UNDEF(result);
			zend_native_object_consume(
				execute_data, opline->op1_type, opline->op1, NULL);
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_STR_COPY(result, Z_OBJCE_P(object)->name);
		zend_native_object_consume(
			execute_data, opline->op1_type, opline->op1, result);
		return zend_native_object_status();
	}

	scope = execute_data->func->op_array.scope;
	if (scope == NULL) {
		zend_throw_error(NULL, "Cannot use \"%s\" in the global scope",
			opline->op1.num == ZEND_FETCH_CLASS_SELF ? "self"
				: opline->op1.num == ZEND_FETCH_CLASS_PARENT
					? "parent" : "static");
		ZVAL_UNDEF(result);
		return ZEND_NATIVE_EXCEPTION;
	}
	switch (opline->op1.num) {
		case ZEND_FETCH_CLASS_SELF:
			ZVAL_STR_COPY(result, scope->name);
			break;
		case ZEND_FETCH_CLASS_PARENT:
			if (scope->parent == NULL) {
				zend_throw_error(NULL,
					"Cannot use \"parent\" when current class scope has no parent");
				ZVAL_UNDEF(result);
				return ZEND_NATIVE_EXCEPTION;
			}
			ZVAL_STR_COPY(result, scope->parent->name);
			break;
		case ZEND_FETCH_CLASS_STATIC: {
			zend_class_entry *called_scope;

			if (Z_TYPE(execute_data->This) == IS_OBJECT) {
				called_scope = Z_OBJCE(execute_data->This);
			} else {
				called_scope = Z_CE(execute_data->This);
			}
			if (called_scope == NULL) {
				zend_throw_error(NULL,
					"Cannot resolve called class for native static::class");
				ZVAL_UNDEF(result);
				return ZEND_NATIVE_EXCEPTION;
			}
			ZVAL_STR_COPY(result, called_scope->name);
			break;
		}
		default:
			zend_throw_error(NULL, "Malformed native class-name fetch type");
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
	}
	return ZEND_NATIVE_RETURNED;
}

static zend_native_status zend_native_get_class(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);
	zval *object;

	if (result == NULL || opline->op2_type != IS_UNUSED) {
		zend_throw_error(NULL, "Malformed native get_class operation");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_UNUSED) {
		zend_class_entry *scope = execute_data->func->common.scope;

		if (scope == NULL) {
			zend_throw_error(NULL,
				"get_class() without arguments must be called from within a class");
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_error(E_DEPRECATED,
			"Calling get_class() without arguments is deprecated");
		if (EG(exception) != NULL) {
			ZVAL_UNDEF(result);
			return ZEND_NATIVE_EXCEPTION;
		}
		ZVAL_STR_COPY(result, scope->name);
		return ZEND_NATIVE_RETURNED;
	}

	object = zend_native_object_read(
		execute_data, opline, opline->op1_type, opline->op1);
	while (object != NULL && Z_ISREF_P(object)) {
		object = Z_REFVAL_P(object);
	}
	if (object != NULL && Z_TYPE_P(object) == IS_OBJECT) {
		ZVAL_STR_COPY(result, Z_OBJCE_P(object)->name);
	} else {
		if (object != NULL && opline->op1_type == IS_CV
				&& Z_TYPE_P(object) == IS_UNDEF) {
			uint32_t variable = EX_VAR_TO_NUM(opline->op1.var);

			if (variable < execute_data->func->op_array.last_var) {
				zend_error_unchecked(E_WARNING,
					"Undefined variable $%S",
					execute_data->func->op_array.vars[variable]);
			}
			object = &EG(uninitialized_zval);
		}
		if (EG(exception) == NULL) {
			zend_type_error(
				"get_class(): Argument #1 ($object) must be of type object, %s given",
				object == NULL ? "unknown" : zend_zval_value_name(object));
		}
		ZVAL_UNDEF(result);
	}
	zend_native_object_consume(
		execute_data, opline->op1_type, opline->op1, result);
	return zend_native_object_status();
}

static zend_native_status zend_native_fetch_this(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *result = zend_native_object_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL || Z_TYPE(execute_data->This) != IS_OBJECT) {
		if (result != NULL) {
			ZVAL_UNDEF(result);
		}
		zend_throw_error(NULL, "Using $this when not in object context");
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_COPY(result, &execute_data->This);
	return ZEND_NATIVE_RETURNED;
}

static const zend_op *zend_native_object_exact_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode)
{
	const zend_op *opline = zend_native_object_opline(
		execute_data, source_opline_index);

	if (opline == NULL || opline->opcode != expected_opcode) {
		zend_throw_error(NULL, "Malformed source operation for native object semantics");
		return NULL;
	}
	return opline;
}

#define ZEND_NATIVE_OBJECT_EXACT_HELPER(name, source_opcode, operation) \
	zend_native_status name( \
		zend_execute_data *execute_data, uint32_t source_opline_index) \
	{ \
		const zend_op *opline = zend_native_object_exact_opline( \
			execute_data, source_opline_index, source_opcode); \
		if (opline == NULL) { \
			return ZEND_NATIVE_EXCEPTION; \
		} \
		return operation; \
	}

ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_declare_anon_class,
	ZEND_DECLARE_ANON_CLASS,
	zend_native_declare_anon_class(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_this,
	ZEND_FETCH_THIS, zend_native_fetch_this(execute_data, opline))
zend_native_status zend_native_execute_object_fetch_r(
	zend_execute_data *execute_data,
	uint64_t op1, uint64_t op2, uint64_t result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_explicit_object_operation operation;

	if (!zend_native_object_init_explicit_operation(
			execute_data, op1, op2, result, extended_value, source_opcode,
			source_position_id, ZEND_FETCH_OBJ_R, &operation)) {
		zend_throw_error(NULL, "Malformed native object read operation");
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_object_fetch_r_explicit(execute_data, &operation);
}
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_w,
	ZEND_FETCH_OBJ_W, zend_native_object_fetch(execute_data, opline, BP_VAR_W))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_rw,
	ZEND_FETCH_OBJ_RW, zend_native_object_fetch(execute_data, opline, BP_VAR_RW))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_is,
	ZEND_FETCH_OBJ_IS, zend_native_object_fetch(execute_data, opline, BP_VAR_IS))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_func_arg,
	ZEND_FETCH_OBJ_FUNC_ARG,
	zend_native_object_fetch(execute_data, opline,
		execute_data->call != NULL
			&& (ZEND_CALL_INFO(execute_data->call)
				& ZEND_CALL_SEND_ARG_BY_REF) != 0 ? BP_VAR_W : BP_VAR_R))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_unset,
	ZEND_FETCH_OBJ_UNSET,
	zend_native_object_fetch(execute_data, opline, BP_VAR_UNSET))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_assign,
	ZEND_ASSIGN_OBJ, zend_native_object_assign(execute_data, opline, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_assign_ref,
	ZEND_ASSIGN_OBJ_REF, zend_native_object_assign(execute_data, opline, true))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_assign_op,
	ZEND_ASSIGN_OBJ_OP, zend_native_object_assign_op(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_unset,
	ZEND_UNSET_OBJ, zend_native_object_unset(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_isset_isempty,
	ZEND_ISSET_ISEMPTY_PROP_OBJ, zend_native_object_isset(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_pre_inc,
	ZEND_PRE_INC_OBJ, zend_native_object_incdec(execute_data, opline, false, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_pre_dec,
	ZEND_PRE_DEC_OBJ, zend_native_object_incdec(execute_data, opline, false, true))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_post_inc,
	ZEND_POST_INC_OBJ, zend_native_object_incdec(execute_data, opline, true, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_post_dec,
	ZEND_POST_DEC_OBJ, zend_native_object_incdec(execute_data, opline, true, true))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_instanceof,
	ZEND_INSTANCEOF, zend_native_object_instanceof(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_clone,
	ZEND_CLONE, zend_native_object_clone(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_fetch_r,
	ZEND_FETCH_STATIC_PROP_R, zend_native_static_fetch(execute_data, opline, BP_VAR_R))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_fetch_w,
	ZEND_FETCH_STATIC_PROP_W, zend_native_static_fetch(execute_data, opline, BP_VAR_W))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_fetch_rw,
	ZEND_FETCH_STATIC_PROP_RW, zend_native_static_fetch(execute_data, opline, BP_VAR_RW))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_fetch_is,
	ZEND_FETCH_STATIC_PROP_IS, zend_native_static_fetch(execute_data, opline, BP_VAR_IS))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_fetch_func_arg,
	ZEND_FETCH_STATIC_PROP_FUNC_ARG,
	zend_native_static_fetch(execute_data, opline,
		execute_data->call != NULL
			&& (ZEND_CALL_INFO(execute_data->call)
				& ZEND_CALL_SEND_ARG_BY_REF) != 0 ? BP_VAR_W : BP_VAR_R))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_fetch_unset,
	ZEND_FETCH_STATIC_PROP_UNSET,
	zend_native_static_fetch(execute_data, opline, BP_VAR_UNSET))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_assign,
	ZEND_ASSIGN_STATIC_PROP, zend_native_static_assign(execute_data, opline, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_assign_ref,
	ZEND_ASSIGN_STATIC_PROP_REF, zend_native_static_assign(execute_data, opline, true))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_assign_op,
	ZEND_ASSIGN_STATIC_PROP_OP,
	zend_native_static_update(execute_data, opline, true, false, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_pre_inc,
	ZEND_PRE_INC_STATIC_PROP,
	zend_native_static_update(execute_data, opline, false, false, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_pre_dec,
	ZEND_PRE_DEC_STATIC_PROP,
	zend_native_static_update(execute_data, opline, false, false, true))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_post_inc,
	ZEND_POST_INC_STATIC_PROP,
	zend_native_static_update(execute_data, opline, false, true, false))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_post_dec,
	ZEND_POST_DEC_STATIC_PROP,
	zend_native_static_update(execute_data, opline, false, true, true))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_isset_isempty,
	ZEND_ISSET_ISEMPTY_STATIC_PROP, zend_native_static_isset(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_static_unset,
	ZEND_UNSET_STATIC_PROP, zend_native_static_unset(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_class,
	ZEND_FETCH_CLASS, zend_native_fetch_class(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_fetch_class_constant,
	ZEND_FETCH_CLASS_CONSTANT, zend_native_class_constant(execute_data, opline))
zend_native_status zend_native_execute_object_fetch_class_name(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_object_opline(
		execute_data, source_opline_index);

	if (opline == NULL) {
		zend_throw_error(NULL,
			"Malformed source operation for native class-name semantics");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->opcode == ZEND_GET_CLASS) {
		return zend_native_get_class(execute_data, opline);
	}
	if (opline->opcode == ZEND_FETCH_CLASS_NAME) {
		return zend_native_fetch_class_name(execute_data, opline);
	}
	zend_throw_error(NULL,
		"Malformed source operation for native class-name semantics");
	return ZEND_NATIVE_EXCEPTION;
}
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_declare_lambda,
	ZEND_DECLARE_LAMBDA_FUNCTION, zend_native_declare_lambda(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_bind_lexical,
	ZEND_BIND_LEXICAL, zend_native_bind_lexical(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_bind_static,
	ZEND_BIND_STATIC, zend_native_bind_static(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_declare_function,
	ZEND_DECLARE_FUNCTION, zend_native_declare_function(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(zend_native_execute_object_declare_class,
	ZEND_DECLARE_CLASS, zend_native_declare_class(execute_data, opline))
ZEND_NATIVE_OBJECT_EXACT_HELPER(
	zend_native_execute_object_declare_class_delayed,
	ZEND_DECLARE_CLASS_DELAYED,
	zend_native_declare_class_delayed(execute_data, opline))

#undef ZEND_NATIVE_OBJECT_EXACT_HELPER
