#include "Zend/Native/Compiler/zend_native_dynamic_code.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"

#include <stdint.h>

#define ZEND_NATIVE_FAKE_OP_ARRAY ((zend_op_array *) (intptr_t) -1)

ZEND_TLS const zend_native_dynamic_compiler
	*zend_native_active_dynamic_compiler;

void zend_native_dynamic_compiler_activate(
	const zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(compiler != NULL && compiler->compile_execute != NULL);
	ZEND_ASSERT(zend_native_active_dynamic_compiler == NULL);
	zend_native_active_dynamic_compiler = compiler;
}

void zend_native_dynamic_compiler_deactivate(
	const zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(zend_native_active_dynamic_compiler == compiler);
	zend_native_active_dynamic_compiler = NULL;
}

static zval *zend_native_dynamic_operand(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *operand;

	if (opline->op1_type == IS_CONST) {
		return RT_CONSTANT(opline, opline->op1);
	}
	if (opline->op1_type != IS_TMP_VAR && opline->op1_type != IS_CV) {
		return NULL;
	}
	operand = ZEND_CALL_VAR(execute_data, opline->op1.var);
	if (opline->op1_type == IS_CV && Z_TYPE_P(operand) == IS_UNDEF) {
		uint32_t variable_index = EX_VAR_TO_NUM(opline->op1.var);

		if (variable_index < execute_data->func->op_array.last_var) {
			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		}
		return &EG(uninitialized_zval);
	}
	return operand;
}

static void zend_native_dynamic_free_operand(
	zend_execute_data *execute_data, const zend_op *opline)
{
	zval *operand;

	if (opline->op1_type != IS_TMP_VAR) {
		return;
	}
	operand = ZEND_CALL_VAR(execute_data, opline->op1.var);
	if (!Z_ISUNDEF_P(operand)) {
		zval_ptr_dtor_nogc(operand);
		ZVAL_UNDEF(operand);
	}
}

zend_native_status zend_native_execute_include_or_eval(
	zend_execute_data *execute_data, uint32_t source_opline_index)
{
	const zend_native_dynamic_compiler *compiler =
		zend_native_active_dynamic_compiler;
	const zend_op *opline;
	zend_op_array *new_op_array;
	zend_execute_data *call;
	zval *filename;
	zval *result = NULL;
	zend_native_status status;
	uint32_t call_info;

	if (compiler == NULL || compiler->compile_execute == NULL
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opline_index >= execute_data->func->op_array.last) {
		zend_throw_error(NULL,
			"Native dynamic compiler is unavailable for include/eval");
		return ZEND_NATIVE_EXCEPTION;
	}
	opline = &execute_data->func->op_array.opcodes[source_opline_index];
	if (opline->opcode != ZEND_INCLUDE_OR_EVAL
			|| (filename = zend_native_dynamic_operand(
				execute_data, opline)) == NULL) {
		zend_throw_error(NULL, "Malformed native include/eval operation");
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->opline = opline;
	new_op_array = zend_include_or_eval(filename, opline->extended_value);
	zend_native_dynamic_free_operand(execute_data, opline);
	if (EG(exception) != NULL) {
		if (new_op_array != ZEND_NATIVE_FAKE_OP_ARRAY
				&& new_op_array != NULL) {
			destroy_op_array(new_op_array);
			efree_size(new_op_array, sizeof(zend_op_array));
		}
		if (opline->result_type != IS_UNUSED) {
			ZVAL_UNDEF(ZEND_CALL_VAR(execute_data, opline->result.var));
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	if (opline->result_type != IS_UNUSED) {
		result = ZEND_CALL_VAR(execute_data, opline->result.var);
	}
	if (new_op_array == ZEND_NATIVE_FAKE_OP_ARRAY) {
		if (result != NULL) {
			ZVAL_TRUE(result);
		}
		return ZEND_NATIVE_RETURNED;
	}
	if (new_op_array == NULL) {
		if (result != NULL) {
			ZVAL_FALSE(result);
		}
		return ZEND_NATIVE_RETURNED;
	}

	new_op_array->scope = execute_data->func->op_array.scope;
	call_info = (Z_TYPE_INFO(execute_data->This) & ZEND_CALL_HAS_THIS)
		| ZEND_CALL_NESTED_CODE | ZEND_CALL_HAS_SYMBOL_TABLE;
	call = zend_vm_stack_push_call_frame(
		call_info, (zend_function *) new_op_array, 0,
		Z_PTR(execute_data->This));
	if ((ZEND_CALL_INFO(execute_data) & ZEND_CALL_HAS_SYMBOL_TABLE) != 0) {
		call->symbol_table = execute_data->symbol_table;
	} else {
		call->symbol_table = zend_rebuild_symbol_table();
	}
	call->prev_execute_data = execute_data;
	zend_init_code_execute_data(call, new_op_array, result);
	status = compiler->compile_execute(compiler->context, new_op_array, call);
	zend_vm_stack_free_call_frame(call);
	zend_destroy_static_vars(new_op_array);
	destroy_op_array(new_op_array);
	efree_size(new_op_array, sizeof(zend_op_array));
	return EG(exception) == NULL ? status : ZEND_NATIVE_EXCEPTION;
}
