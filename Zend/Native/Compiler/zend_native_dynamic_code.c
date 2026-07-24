#include "Zend/Native/Compiler/zend_native_dynamic_code.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/Native/Compiler/zend_native_compiler_internal.h"
#include "Zend/Native/Lowering/zend_mir_lowering_source.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include <stdint.h>

#define ZEND_NATIVE_FAKE_OP_ARRAY ((zend_op_array *) (intptr_t) -1)

ZEND_TLS zend_native_dynamic_compiler
	*zend_native_active_dynamic_compiler;

void zend_native_dynamic_compiler_init(
	zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(compiler != NULL);
	memset(compiler, 0, sizeof(*compiler));
	zend_hash_init(
		&compiler->entries_by_op_array, 8, NULL, NULL, false);
}

void zend_native_dynamic_compiler_bind_product(
	zend_native_dynamic_compiler *compiler,
	zend_native_compiler *product_compiler)
{
	ZEND_ASSERT(compiler != NULL);
	ZEND_ASSERT(compiler->owned_op_array_count == 0);
	ZEND_ASSERT(compiler->entry_count == 0);
	compiler->product_compiler = product_compiler;
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
	zend_hash_destroy(&compiler->entries_by_op_array);
	memset(compiler, 0, sizeof(*compiler));
}

void zend_native_dynamic_compiler_activate(
	zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(compiler != NULL);
	ZEND_ASSERT(zend_native_active_dynamic_compiler == NULL);
	zend_native_active_dynamic_compiler = compiler;
}

zend_native_entry_cell *zend_native_dynamic_compiler_lookup(
	const zend_native_dynamic_compiler *compiler,
	const zend_op_array *op_array)
{
	zend_native_entry_cell *entry_cell;

	if (compiler == NULL || op_array == NULL) {
		return NULL;
	}
	entry_cell = zend_hash_index_find_ptr(
		&compiler->entries_by_op_array,
		(zend_ulong) (uintptr_t) op_array);
	return entry_cell;
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
	if (zend_hash_index_add_ptr(
			&compiler->entries_by_op_array,
			(zend_ulong) (uintptr_t) op_array, entry_cell) == NULL) {
		return FAILURE;
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

static bool zend_native_dynamic_decode_operand(
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

static zval *zend_native_dynamic_operand(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand)
{
	zval *value;

	if (operand_type == IS_CONST) {
		return operand.constant < execute_data->func->op_array.last_literal
			? &execute_data->func->op_array.literals[operand.constant] : NULL;
	}
	if (operand_type != IS_TMP_VAR && operand_type != IS_CV) {
		return NULL;
	}
	value = ZEND_CALL_VAR(execute_data, operand.var);
	if (operand_type == IS_CV && Z_TYPE_P(value) == IS_UNDEF) {
		uint32_t variable_index = EX_VAR_TO_NUM(operand.var);

		if (variable_index < execute_data->func->op_array.last_var) {
			zend_error(E_WARNING, "Undefined variable $%s",
				ZSTR_VAL(execute_data->func->op_array.vars[variable_index]));
		}
		return &EG(uninitialized_zval);
	}
	return value;
}

static void zend_native_dynamic_free_operand(
	zend_execute_data *execute_data, uint8_t operand_type, znode_op operand)
{
	zval *value;

	if (operand_type != IS_TMP_VAR) {
		return;
	}
	value = ZEND_CALL_VAR(execute_data, operand.var);
	if (!Z_ISUNDEF_P(value)) {
		zval_ptr_dtor_nogc(value);
		ZVAL_UNDEF(value);
	}
}

zend_native_status zend_native_execute_include_or_eval(
	zend_execute_data *execute_data,
	uint64_t encoded_op1, uint64_t encoded_op2, uint64_t encoded_result,
	uint32_t extended_value, uint32_t source_opcode,
	uint32_t source_position_id)
{
	zend_native_dynamic_compiler *compiler =
		zend_native_active_dynamic_compiler;
	uint8_t op1_type;
	uint8_t op2_type;
	uint8_t result_type;
	znode_op op1;
	znode_op op2;
	znode_op result_operand;
	zend_op_array *new_op_array;
	zend_execute_data *call;
	zend_execute_data *previous;
	zend_native_entry_cell *entry_cell;
	zval *filename;
	zval *result = NULL;
	zend_native_status status;
	zend_native_diagnostic diagnostic;
	zend_native_compile_diagnostic compile_diagnostic;
	uint32_t call_info;
	uint32_t first_function_bucket;
	uint32_t first_class_bucket;

	if (compiler == NULL
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opcode != ZEND_INCLUDE_OR_EVAL
			|| source_position_id >= execute_data->func->op_array.last
			|| !zend_native_dynamic_decode_operand(
				execute_data, encoded_op1, &op1_type, &op1)
			|| !zend_native_dynamic_decode_operand(
				execute_data, encoded_op2, &op2_type, &op2)
			|| !zend_native_dynamic_decode_operand(
				execute_data, encoded_result, &result_type, &result_operand)
			|| (op1_type != IS_CONST
				&& op1_type != IS_TMP_VAR && op1_type != IS_CV)
			|| op2_type != IS_UNUSED
			|| (result_type != IS_UNUSED
				&& result_type != IS_TMP_VAR && result_type != IS_VAR)
			|| (extended_value != ZEND_EVAL
				&& extended_value != ZEND_INCLUDE
				&& extended_value != ZEND_INCLUDE_ONCE
				&& extended_value != ZEND_REQUIRE
				&& extended_value != ZEND_REQUIRE_ONCE)) {
		zend_throw_error(NULL,
			"Native dynamic compiler is unavailable for include/eval");
		return ZEND_NATIVE_EXCEPTION;
	}
	if ((filename = zend_native_dynamic_operand(
			execute_data, op1_type, op1)) == NULL) {
		zend_throw_error(NULL, "Malformed native include/eval operation");
		return ZEND_NATIVE_EXCEPTION;
	}
	execute_data->opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	first_function_bucket = EG(function_table)->nNumUsed;
	first_class_bucket = EG(class_table)->nNumUsed;
	new_op_array = zend_include_or_eval(filename, extended_value);
	zend_native_dynamic_free_operand(execute_data, op1_type, op1);
	if (EG(exception) != NULL) {
		if (new_op_array != ZEND_NATIVE_FAKE_OP_ARRAY
				&& new_op_array != NULL) {
			destroy_op_array(new_op_array);
			efree_size(new_op_array, sizeof(zend_op_array));
		}
		if (result_type != IS_UNUSED) {
			ZVAL_UNDEF(ZEND_CALL_VAR(execute_data, result_operand.var));
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	if (result_type != IS_UNUSED) {
		result = ZEND_CALL_VAR(execute_data, result_operand.var);
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
	if (compiler->product_compiler != NULL) {
		memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
		if (zend_native_compiler_compile_dynamic_component(
				compiler->product_compiler, new_op_array,
				first_function_bucket, first_class_bucket,
				&entry_cell, &compile_diagnostic) == FAILURE) {
			entry_cell = NULL;
			if (EG(exception) == NULL) {
				zend_throw_error(NULL, "%s",
					compile_diagnostic.message[0] != '\0'
						? compile_diagnostic.message
						: "Native compilation failed for dynamic codeunit");
			}
		}
	} else {
		entry_cell = zend_native_reentry_resolve(
			(zend_function *) new_op_array);
	}
	if (entry_cell == NULL
			|| entry_cell->state != ZEND_NATIVE_ENTRY_READY
			|| entry_cell->code == NULL
			|| zend_native_dynamic_compiler_publish(
				compiler, new_op_array, entry_cell) == FAILURE) {
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
	call_info = ZEND_CALL_INFO(call);
	zend_vm_stack_free_call_frame(call);
	if ((call_info & ZEND_CALL_NEEDS_REATTACH) != 0) {
		if (execute_data->func->op_array.last_var > 0) {
			zend_attach_symbol_table(execute_data);
		} else {
			ZEND_ADD_CALL_FLAG(execute_data, ZEND_CALL_NEEDS_REATTACH);
		}
	}
	/*
	 * The compiler takes ownership even when compilation fails. The native
	 * registry must retain the codeunit root while declarations, closures or
	 * active frames can still reach any child op_array.
	 */
	return EG(exception) == NULL ? status : ZEND_NATIVE_EXCEPTION;
}
