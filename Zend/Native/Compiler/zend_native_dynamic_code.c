#include "Zend/Native/Compiler/zend_native_dynamic_code.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include <stdint.h>

#define ZEND_NATIVE_FAKE_OP_ARRAY ((zend_op_array *) (intptr_t) -1)

ZEND_TLS zend_native_dynamic_compiler
	*zend_native_active_dynamic_compiler;

void zend_native_dynamic_compiler_init(
	zend_native_dynamic_compiler *compiler,
	void *context,
	zend_native_dynamic_compile_t compile)
{
	ZEND_ASSERT(compiler != NULL && compile != NULL);
	memset(compiler, 0, sizeof(*compiler));
	compiler->context = context;
	compiler->compile = compile;
}

void zend_native_dynamic_compiler_destroy(
	zend_native_dynamic_compiler *compiler)
{
	uint32_t index;

	ZEND_ASSERT(compiler != NULL);
	ZEND_ASSERT(zend_native_active_dynamic_compiler != compiler);
	for (index = compiler->owned_op_array_count; index-- > 0;) {
		zend_op_array *op_array = compiler->owned_op_arrays[index];

		zend_destroy_static_vars(op_array);
		destroy_op_array(op_array);
		efree_size(op_array, sizeof(zend_op_array));
	}
	efree(compiler->owned_op_arrays);
	efree(compiler->entries);
	memset(compiler, 0, sizeof(*compiler));
}

void zend_native_dynamic_compiler_activate(
	zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(compiler != NULL && compiler->compile != NULL);
	ZEND_ASSERT(zend_native_active_dynamic_compiler == NULL);
	zend_native_active_dynamic_compiler = compiler;
}

zend_native_entry_cell *zend_native_dynamic_compiler_lookup(
	const zend_native_dynamic_compiler *compiler,
	const zend_op_array *op_array)
{
	uint32_t index;

	if (compiler == NULL || op_array == NULL) {
		return NULL;
	}
	for (index = 0; index < compiler->entry_count; index++) {
		if (compiler->entries[index].op_array == op_array) {
			return compiler->entries[index].entry_cell;
		}
	}
	return NULL;
}

zend_result zend_native_dynamic_compiler_publish(
	zend_native_dynamic_compiler *compiler,
	zend_op_array *op_array,
	zend_native_entry_cell *entry_cell)
{
	uint32_t old_capacity;
	uint32_t new_capacity;

	if (compiler == NULL || op_array == NULL || entry_cell == NULL
			|| entry_cell->state != ZEND_NATIVE_ENTRY_READY
			|| entry_cell->code == NULL) {
		return FAILURE;
	}
	if (zend_native_dynamic_compiler_lookup(compiler, op_array) != NULL) {
		return FAILURE;
	}
	if (compiler->entry_count == compiler->entry_capacity) {
		old_capacity = compiler->entry_capacity;
		new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
		if (new_capacity <= old_capacity) {
			return FAILURE;
		}
		compiler->entries = safe_erealloc(
			compiler->entries, new_capacity,
			sizeof(*compiler->entries), 0);
		compiler->entry_capacity = new_capacity;
	}
	compiler->entries[compiler->entry_count].op_array = op_array;
	compiler->entries[compiler->entry_count].entry_cell = entry_cell;
	compiler->entry_count++;
	return SUCCESS;
}

static bool zend_native_dynamic_compiler_adopt(
	zend_native_dynamic_compiler *compiler, zend_op_array *op_array)
{
	uint32_t old_capacity;
	uint32_t new_capacity;

	if (compiler->owned_op_array_count
			== compiler->owned_op_array_capacity) {
		old_capacity = compiler->owned_op_array_capacity;
		new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
		if (new_capacity <= old_capacity) {
			return false;
		}
		compiler->owned_op_arrays = safe_erealloc(
			compiler->owned_op_arrays, new_capacity,
			sizeof(*compiler->owned_op_arrays), 0);
		compiler->owned_op_array_capacity = new_capacity;
	}
	compiler->owned_op_arrays[compiler->owned_op_array_count++] = op_array;
	return true;
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
	zend_native_dynamic_compiler *compiler =
		zend_native_active_dynamic_compiler;
	const zend_op *opline;
	zend_op_array *new_op_array;
	zend_execute_data *call;
	zend_execute_data *previous;
	zend_native_entry_cell *entry_cell;
	zval *filename;
	zval *result = NULL;
	zend_native_status status;
	zend_native_diagnostic diagnostic;
	uint32_t call_info;

	if (compiler == NULL || compiler->compile == NULL
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
	if (!zend_native_dynamic_compiler_adopt(compiler, new_op_array)) {
		zend_vm_stack_free_call_frame(call);
		zend_destroy_static_vars(new_op_array);
		destroy_op_array(new_op_array);
		efree_size(new_op_array, sizeof(zend_op_array));
		zend_throw_error(NULL,
			"Native dynamic codeunit owner capacity overflow");
		return ZEND_NATIVE_EXCEPTION;
	}
	if (compiler->compile(compiler->context, new_op_array) == FAILURE
			|| (entry_cell = zend_native_dynamic_compiler_lookup(
				compiler, new_op_array)) == NULL
			|| entry_cell->state != ZEND_NATIVE_ENTRY_READY
			|| entry_cell->code == NULL) {
		zend_vm_stack_free_call_frame(call);
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Native compilation failed for dynamically created code");
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	previous = EG(current_execute_data);
	EG(current_execute_data) = call;
	status = zend_native_execute_frame(
		entry_cell->code, call, &diagnostic);
	EG(current_execute_data) = previous;
	zend_vm_stack_free_call_frame(call);
	/*
	 * compile_execute takes ownership even when compilation fails. The native
	 * registry must retain the codeunit root while declarations, closures or
	 * active frames can still reach any child op_array.
	 */
	return EG(exception) == NULL ? status : ZEND_NATIVE_EXCEPTION;
}
