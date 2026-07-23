/* Exact source-backed variable, global and constant binding semantics. */

#include "Zend/Native/Runtime/Common/zend_native_bindings.h"

#include "Zend/zend_API.h"
#include "Zend/zend_constants.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_operators.h"

static const zend_op *zend_native_dynamic_opline(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode)
{
	const zend_op *opline;

	if (execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opline_index >= execute_data->func->op_array.last) {
		return NULL;
	}
	opline = &execute_data->func->op_array.opcodes[source_opline_index];
	if (opline->opcode != expected_opcode) {
		return NULL;
	}
	execute_data->opline = opline;
	return opline;
}

static zval *zend_native_dynamic_slot(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	type &= IS_CONST | IS_TMP_VAR | IS_VAR | IS_CV;
	if (type != IS_TMP_VAR && type != IS_VAR && type != IS_CV) {
		return NULL;
	}
	return ZEND_CALL_VAR(execute_data, operand.var);
}

static zval *zend_native_dynamic_read(
	zend_execute_data *execute_data, const zend_op *opline,
	uint8_t type, znode_op operand)
{
	zval *value;

	if (type == IS_CONST) {
		return RT_CONSTANT(opline, operand);
	}
	value = zend_native_dynamic_slot(execute_data, type, operand);
	if (value != NULL && type == IS_VAR && Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	return value;
}

static void zend_native_dynamic_consume(
	zend_execute_data *execute_data, uint8_t type, znode_op operand)
{
	zval *slot;

	if (type != IS_TMP_VAR && type != IS_VAR) {
		return;
	}
	slot = zend_native_dynamic_slot(execute_data, type, operand);
	if (slot != NULL && !Z_ISUNDEF_P(slot)) {
		zval_ptr_dtor_nogc(slot);
		ZVAL_UNDEF(slot);
	}
}

static HashTable *zend_native_dynamic_symbol_table(
	zend_execute_data *execute_data, uint32_t fetch_type)
{
	if ((fetch_type & (ZEND_FETCH_GLOBAL_LOCK | ZEND_FETCH_GLOBAL)) != 0) {
		return &EG(symbol_table);
	}
	if ((fetch_type & ZEND_FETCH_LOCAL) == 0) {
		return NULL;
	}
	if ((ZEND_CALL_INFO(execute_data) & ZEND_CALL_HAS_SYMBOL_TABLE) == 0) {
		return zend_rebuild_symbol_table();
	}
	return execute_data->symbol_table;
}

static zend_native_status zend_native_dynamic_fetch_this(
	zend_execute_data *execute_data, const zend_op *opline, int fetch_type)
{
	zval *result = zend_native_dynamic_slot(
		execute_data, opline->result_type, opline->result);

	if (result == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	switch (fetch_type) {
		case BP_VAR_R:
			if (Z_TYPE(execute_data->This) == IS_OBJECT) {
				ZVAL_COPY(result, &execute_data->This);
			} else {
				ZVAL_NULL(result);
				zend_error_unchecked(E_WARNING, "Undefined variable $this");
			}
			break;
		case BP_VAR_IS:
			if (Z_TYPE(execute_data->This) == IS_OBJECT) {
				ZVAL_COPY(result, &execute_data->This);
			} else {
				ZVAL_NULL(result);
			}
			break;
		case BP_VAR_W:
		case BP_VAR_RW:
			ZVAL_UNDEF(result);
			zend_throw_error(NULL, "Cannot re-assign $this");
			break;
		case BP_VAR_UNSET:
			ZVAL_UNDEF(result);
			break;
		default:
			return ZEND_NATIVE_EXCEPTION;
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

static zend_native_status zend_native_dynamic_fetch(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	uint8_t expected_opcode, int fetch_type)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index, expected_opcode);
	HashTable *symbol_table;
	zval *name_value;
	zval *result;
	zval *value;
	zend_string *name;
	zend_string *temporary_name = NULL;

	if (opline == NULL || opline->result_type == IS_UNUSED
			|| (name_value = zend_native_dynamic_read(
				execute_data, opline, opline->op1_type, opline->op1)) == NULL
			|| (result = zend_native_dynamic_slot(
				execute_data, opline->result_type, opline->result)) == NULL
			|| (symbol_table = zend_native_dynamic_symbol_table(
				execute_data, opline->extended_value)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->op1_type == IS_CONST) {
		if (Z_TYPE_P(name_value) != IS_STRING) {
			return ZEND_NATIVE_EXCEPTION;
		}
		name = Z_STR_P(name_value);
	} else {
		if (opline->op1_type == IS_CV && Z_TYPE_P(name_value) == IS_UNDEF) {
			uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

			if (variable_index < execute_data->func->op_array.last_var) {
				zend_error(E_WARNING, "Undefined variable $%s",
					ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
			}
			if (EG(exception) != NULL) {
				ZVAL_UNDEF(result);
				return ZEND_NATIVE_EXCEPTION;
			}
			name_value = &EG(uninitialized_zval);
		}
		name = zval_try_get_tmp_string(name_value, &temporary_name);
		if (name == NULL) {
			ZVAL_UNDEF(result);
			zend_native_dynamic_consume(
				execute_data, opline->op1_type, opline->op1);
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	value = zend_hash_find_ex(
		symbol_table, name, opline->op1_type == IS_CONST);
	if (value == NULL || (Z_TYPE_P(value) == IS_INDIRECT
			&& Z_TYPE_P(Z_INDIRECT_P(value)) == IS_UNDEF)) {
		if (zend_string_equals(name, ZSTR_KNOWN(ZEND_STR_THIS))) {
			zend_native_status status = zend_native_dynamic_fetch_this(
				execute_data, opline, fetch_type);

			zend_tmp_string_release(temporary_name);
			if ((opline->extended_value & ZEND_FETCH_GLOBAL_LOCK) == 0) {
				zend_native_dynamic_consume(
					execute_data, opline->op1_type, opline->op1);
			}
			return status;
		}
		if (value != NULL) {
			value = Z_INDIRECT_P(value);
		}
		if (fetch_type == BP_VAR_W) {
			if (value == NULL) {
				value = zend_hash_add_new(
					symbol_table, name, &EG(uninitialized_zval));
			} else {
				ZVAL_NULL(value);
			}
		} else if (fetch_type == BP_VAR_IS || fetch_type == BP_VAR_UNSET) {
			value = &EG(uninitialized_zval);
		} else {
			zend_error_unchecked(E_WARNING, "Undefined %svariable $%S",
				(opline->extended_value & ZEND_FETCH_GLOBAL) != 0
					? "global " : "", name);
			if (fetch_type == BP_VAR_RW && EG(exception) == NULL) {
				if (value == NULL) {
					value = zend_hash_update(
						symbol_table, name, &EG(uninitialized_zval));
				} else {
					ZVAL_NULL(value);
				}
			} else {
				value = &EG(uninitialized_zval);
			}
		}
	} else if (Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	if (fetch_type == BP_VAR_R || fetch_type == BP_VAR_IS) {
		ZVAL_COPY_DEREF(result, value);
	} else {
		ZVAL_INDIRECT(result, value);
	}
	zend_tmp_string_release(temporary_name);
	if ((opline->extended_value & ZEND_FETCH_GLOBAL_LOCK) == 0) {
		zend_native_dynamic_consume(
			execute_data, opline->op1_type, opline->op1);
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

#define ZEND_NATIVE_DYNAMIC_FETCH(name, opcode, mode) \
	zend_native_status name( \
		zend_execute_data *execute_data, uint32_t source_opline_index) \
	{ \
		return zend_native_dynamic_fetch( \
			execute_data, source_opline_index, opcode, mode); \
	}

ZEND_NATIVE_DYNAMIC_FETCH(
	zend_native_dynamic_fetch_r, ZEND_FETCH_R, BP_VAR_R)
ZEND_NATIVE_DYNAMIC_FETCH(
	zend_native_dynamic_fetch_w, ZEND_FETCH_W, BP_VAR_W)
ZEND_NATIVE_DYNAMIC_FETCH(
	zend_native_dynamic_fetch_rw, ZEND_FETCH_RW, BP_VAR_RW)
ZEND_NATIVE_DYNAMIC_FETCH(
	zend_native_dynamic_fetch_is, ZEND_FETCH_IS, BP_VAR_IS)
ZEND_NATIVE_DYNAMIC_FETCH(
	zend_native_dynamic_fetch_unset, ZEND_FETCH_UNSET, BP_VAR_UNSET)

zend_native_status zend_native_dynamic_fetch_func_arg(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	int fetch_type = execute_data != NULL && execute_data->call != NULL
		&& (ZEND_CALL_INFO(execute_data->call)
			& ZEND_CALL_SEND_ARG_BY_REF) != 0 ? BP_VAR_W : BP_VAR_R;

	return zend_native_dynamic_fetch(
		execute_data, source_opline_index, ZEND_FETCH_FUNC_ARG, fetch_type);
}

#undef ZEND_NATIVE_DYNAMIC_FETCH

static zend_string *zend_native_dynamic_name(
	zend_execute_data *execute_data, const zend_op *opline,
	zend_string **temporary)
{
	zval *value = zend_native_dynamic_read(
		execute_data, opline, opline->op1_type, opline->op1);

	*temporary = NULL;
	if (value == NULL) {
		return NULL;
	}
	if (opline->op1_type == IS_CONST) {
		return Z_TYPE_P(value) == IS_STRING ? Z_STR_P(value) : NULL;
	}
	if (opline->op1_type == IS_CV && Z_TYPE_P(value) == IS_UNDEF) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

		if (variable_index < execute_data->func->op_array.last_var) {
			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		}
		if (EG(exception) != NULL) {
			return NULL;
		}
		value = &EG(uninitialized_zval);
	}
	return zval_try_get_tmp_string(value, temporary);
}

zend_native_status zend_native_dynamic_unset_var(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index, ZEND_UNSET_VAR);
	HashTable *symbol_table;
	zend_string *temporary;
	zend_string *name;

	if (opline == NULL
			|| (name = zend_native_dynamic_name(
				execute_data, opline, &temporary)) == NULL
			|| (symbol_table = zend_native_dynamic_symbol_table(
				execute_data, opline->extended_value)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	zend_hash_del_ind(symbol_table, name);
	zend_tmp_string_release(temporary);
	zend_native_dynamic_consume(
		execute_data, opline->op1_type, opline->op1);
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_dynamic_isset_isempty_var(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index, ZEND_ISSET_ISEMPTY_VAR);
	HashTable *symbol_table;
	zend_string *temporary;
	zend_string *name;
	zval *value;
	zval *result;
	bool truth;

	if (opline == NULL
			|| (name = zend_native_dynamic_name(
				execute_data, opline, &temporary)) == NULL
			|| (symbol_table = zend_native_dynamic_symbol_table(
				execute_data, opline->extended_value)) == NULL
			|| (result = zend_native_dynamic_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	value = zend_hash_find_ex(
		symbol_table, name, opline->op1_type == IS_CONST);
	zend_tmp_string_release(temporary);
	zend_native_dynamic_consume(
		execute_data, opline->op1_type, opline->op1);
	if (value != NULL && Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
	}
	if (value == NULL || Z_TYPE_P(value) == IS_UNDEF) {
		truth = (opline->extended_value & ZEND_ISEMPTY) != 0;
	} else if ((opline->extended_value & ZEND_ISEMPTY) == 0) {
		if (Z_ISREF_P(value)) {
			value = Z_REFVAL_P(value);
		}
		truth = Z_TYPE_P(value) > IS_NULL;
	} else {
		truth = !zend_is_true(value);
	}
	ZVAL_BOOL(result, truth);
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_dynamic_bind_global(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index, ZEND_BIND_GLOBAL);
	zval *name;
	zval *global;
	zval *local;
	zend_reference *reference;
	zend_refcounted *garbage = NULL;

	if (opline == NULL || opline->op1_type != IS_CV
			|| opline->op2_type != IS_CONST
			|| (name = RT_CONSTANT(opline, opline->op2)) == NULL
			|| Z_TYPE_P(name) != IS_STRING
			|| (local = zend_native_dynamic_slot(
				execute_data, opline->op1_type, opline->op1)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	global = zend_hash_find_known_hash(&EG(symbol_table), Z_STR_P(name));
	if (global == NULL) {
		global = zend_hash_add_new(
			&EG(symbol_table), Z_STR_P(name), &EG(uninitialized_zval));
	} else if (Z_TYPE_P(global) == IS_INDIRECT) {
		global = Z_INDIRECT_P(global);
		if (Z_TYPE_P(global) == IS_UNDEF) {
			ZVAL_NULL(global);
		}
	}
	if (!Z_ISREF_P(global)) {
		ZVAL_MAKE_REF_EX(global, 2);
		reference = Z_REF_P(global);
	} else {
		reference = Z_REF_P(global);
		GC_ADDREF(reference);
	}
	if (Z_REFCOUNTED_P(local)) {
		garbage = Z_COUNTED_P(local);
	}
	ZVAL_REF(local, reference);
	if (garbage != NULL) {
		if (GC_DELREF(garbage) == 0) {
			rc_dtor_func(garbage);
		} else {
			gc_check_possible_root(garbage);
		}
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_dynamic_fetch_globals(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index, ZEND_FETCH_GLOBALS);
	zval *result;

	if (opline == NULL || (result = zend_native_dynamic_slot(
			execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_ARR(result,
		zend_proptable_to_symtable(&EG(symbol_table), true));
	return ZEND_NATIVE_RETURNED;
}

zend_native_status zend_native_dynamic_fetch_constant(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index, ZEND_FETCH_CONSTANT);
	const zval *key;
	zval *value;
	zval *result;

	if (opline == NULL || opline->op2_type != IS_CONST
			|| (result = zend_native_dynamic_slot(
				execute_data, opline->result_type, opline->result)) == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	key = RT_CONSTANT(opline, opline->op2) + 1;
	if (Z_TYPE_P(key) != IS_STRING) {
		return ZEND_NATIVE_EXCEPTION;
	}
	value = zend_get_constant_ex(
		Z_STR_P(key), execute_data->func->op_array.scope, opline->op1.num);
	if (value == NULL && (opline->op1.num
			& IS_CONSTANT_UNQUALIFIED_IN_NAMESPACE) != 0) {
		key++;
		if (Z_TYPE_P(key) == IS_STRING) {
			value = zend_get_constant_ex(
				Z_STR_P(key), execute_data->func->op_array.scope, 0);
		}
	}
	if (value == NULL) {
		ZVAL_UNDEF(result);
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "Undefined constant \"%s\"",
				Z_STRVAL_P(RT_CONSTANT(opline, opline->op2)));
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_COPY_OR_DUP(result, value);
	return ZEND_NATIVE_RETURNED;
}

static zend_native_status zend_native_dynamic_declare_constant_impl(
	zend_execute_data *execute_data, uint32_t source_opline_index,
	bool attributed)
{
	const zend_op *opline = zend_native_dynamic_opline(
		execute_data, source_opline_index,
		attributed ? ZEND_DECLARE_ATTRIBUTED_CONST : ZEND_DECLARE_CONST);
	zval *name;
	zval *value;
	zend_constant constant;
	zend_constant *registered;

	if (opline == NULL || opline->op1_type != IS_CONST
			|| opline->op2_type != IS_CONST
			|| (name = RT_CONSTANT(opline, opline->op1)) == NULL
			|| (value = RT_CONSTANT(opline, opline->op2)) == NULL
			|| Z_TYPE_P(name) != IS_STRING) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_COPY(&constant.value, value);
	if (Z_TYPE(constant.value) == IS_CONSTANT_AST
			&& zval_update_constant_ex(
				&constant.value, execute_data->func->op_array.scope)
				!= SUCCESS) {
		zval_ptr_dtor_nogc(&constant.value);
		return ZEND_NATIVE_EXCEPTION;
	}
	ZEND_CONSTANT_SET_FLAGS(&constant, 0, PHP_USER_CONSTANT);
	constant.name = zend_string_copy(Z_STR_P(name));
	registered = zend_register_constant(&constant);
	if (registered != NULL && attributed) {
		const zend_op *op_data;
		zval *attributes;

		if (source_opline_index + 1 >= execute_data->func->op_array.last
				|| (op_data = opline + 1)->opcode != ZEND_OP_DATA
				|| (attributes = zend_native_dynamic_read(
					execute_data, op_data,
					op_data->op1_type, op_data->op1)) == NULL
				|| Z_TYPE_P(attributes) != IS_PTR) {
			return ZEND_NATIVE_EXCEPTION;
		}
		zend_constant_add_attributes(
			registered, (HashTable *) Z_PTR_P(attributes));
	}
	return EG(exception) == NULL
		? ZEND_NATIVE_RETURNED : ZEND_NATIVE_EXCEPTION;
}

zend_native_status zend_native_dynamic_declare_constant(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_dynamic_declare_constant_impl(
		execute_data, source_opline_index, false);
}

zend_native_status zend_native_dynamic_declare_attributed_constant(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	return zend_native_dynamic_declare_constant_impl(
		execute_data, source_opline_index, true);
}
