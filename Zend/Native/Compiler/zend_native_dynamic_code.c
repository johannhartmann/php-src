#include "Zend/Native/Compiler/zend_native_dynamic_code.h"
#include "Zend/Native/Runtime/Common/zend_native_operands.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_call_stack.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_virtual_cwd.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"
#include "Zend/Native/Compiler/zend_native_compiler_internal.h"
#include "Zend/Native/Lowering/zend_mir_lowering_source.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"

#include <stdint.h>

/* C stack kept free above EG(stack_limit) before an include is compiled from
 * native code, for Zend's parser and compiler and for TPDE. */
#define ZEND_NATIVE_INCLUDE_STACK_RESERVE (128u * 1024u)

#define ZEND_NATIVE_FAKE_OP_ARRAY ((zend_op_array *) (intptr_t) -1)

ZEND_TLS zend_native_dynamic_compiler
	*zend_native_active_dynamic_compiler;

void zend_native_dynamic_compiler_init(
	zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(compiler != NULL);
	memset(compiler, 0, sizeof(*compiler));
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
	ZEND_ASSERT(compiler->previous_active == NULL);
	for (index = compiler->owned_op_array_count; index-- > 0;) {
		zend_op_array *op_array = compiler->owned_op_arrays[index];

		zend_destroy_static_vars(op_array);
		destroy_op_array(op_array);
		efree_size(op_array, sizeof(zend_op_array));
	}
	efree(compiler->owned_op_arrays);
	efree(compiler->entries);
	efree(compiler->completed_include_once_sites);
	memset(compiler, 0, sizeof(*compiler));
}

void zend_native_dynamic_compiler_activate(
	zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(compiler != NULL);
	ZEND_ASSERT(compiler != zend_native_active_dynamic_compiler);
	ZEND_ASSERT(compiler->previous_active == NULL);
	compiler->previous_active = zend_native_active_dynamic_compiler;
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
	/* Code created last is looked up first. */
	for (index = compiler->entry_count; index-- > 0;) {
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
	/* The op_array was created and adopted for this publication. */
	ZEND_ASSERT(zend_native_dynamic_compiler_lookup(compiler, op_array)
		== NULL);
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

static bool zend_native_dynamic_compiler_can_retire_last(
	const zend_native_dynamic_compiler *compiler,
	const zend_op_array *op_array, const zend_native_entry_cell *entry_cell)
{
	const zend_native_dynamic_entry *entry;

	if (compiler == NULL || op_array == NULL || entry_cell == NULL
			|| compiler->owned_op_array_count == 0
			|| compiler->entry_count == 0
			|| compiler->owned_op_arrays[
				compiler->owned_op_array_count - 1] != op_array) {
		return false;
	}
	entry = &compiler->entries[compiler->entry_count - 1];
	return entry->op_array == op_array && entry->entry_cell == entry_cell;
}

static void zend_native_dynamic_compiler_retire_last(
	zend_native_dynamic_compiler *compiler, zend_op_array *op_array)
{
	ZEND_ASSERT(compiler != NULL);
	ZEND_ASSERT(compiler->owned_op_array_count != 0);
	ZEND_ASSERT(compiler->entry_count != 0);
	ZEND_ASSERT(compiler->owned_op_arrays[
		compiler->owned_op_array_count - 1] == op_array);
	ZEND_ASSERT(compiler->entries[compiler->entry_count - 1].op_array
		== op_array);
	compiler->entry_count--;
	compiler->owned_op_array_count--;
	zend_destroy_static_vars(op_array);
	destroy_op_array(op_array);
	efree_size(op_array, sizeof(zend_op_array));
}

static bool zend_native_include_once_site_completed(
	const zend_native_dynamic_compiler *compiler,
	const zend_op *source_opline)
{
	uint32_t index;

	for (index = compiler->completed_include_once_site_count;
			index-- > 0;) {
		if (compiler->completed_include_once_sites[index] == source_opline) {
			return true;
		}
	}
	return false;
}

zend_native_status zend_native_execute_const_include_once(
	zend_execute_data *execute_data, uint64_t op1,
	uint32_t extended_value, uint32_t source_position_id)
{
	zend_native_dynamic_compiler *compiler =
		zend_native_active_dynamic_compiler;
	const zend_op *source_opline;
	const uint64_t unused_operand = IS_UNUSED;

	if (compiler == NULL || execute_data == NULL
			|| execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_position_id >= execute_data->func->op_array.last
			|| (extended_value != ZEND_INCLUDE_ONCE
				&& extended_value != ZEND_REQUIRE_ONCE)) {
		zend_throw_error(NULL,
			"Malformed native constant include_once operation");
		return ZEND_NATIVE_EXCEPTION;
	}
	source_opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	if (zend_native_include_once_site_completed(compiler, source_opline)) {
		execute_data->opline = source_opline;
		return ZEND_NATIVE_RETURNED;
	}
	return zend_native_execute_include_or_eval(execute_data, op1,
		unused_operand, unused_operand, extended_value,
		ZEND_INCLUDE_OR_EVAL, source_position_id);
}

static void zend_native_complete_include_once_site(
	zend_native_dynamic_compiler *compiler, const zend_op *source_opline)
{
	uint32_t old_capacity;
	uint32_t new_capacity;

	if (zend_native_include_once_site_completed(compiler, source_opline)) {
		return;
	}
	if (compiler->completed_include_once_site_count
			== compiler->completed_include_once_site_capacity) {
		old_capacity = compiler->completed_include_once_site_capacity;
		new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
		if (new_capacity <= old_capacity) {
			return;
		}
		compiler->completed_include_once_sites = safe_erealloc(
			compiler->completed_include_once_sites, new_capacity,
			sizeof(*compiler->completed_include_once_sites), 0);
		compiler->completed_include_once_site_capacity = new_capacity;
	}
	compiler->completed_include_once_sites[
		compiler->completed_include_once_site_count++] = source_opline;
}

void zend_native_dynamic_compiler_deactivate(
	zend_native_dynamic_compiler *compiler)
{
	ZEND_ASSERT(zend_native_active_dynamic_compiler == compiler);
	zend_native_active_dynamic_compiler = compiler->previous_active;
	compiler->previous_active = NULL;
}

static bool zend_native_dynamic_decode_operand(
	zend_execute_data *execute_data, uint64_t encoded,
	uint8_t *operand_type, znode_op *operand)
{
	return zend_native_decode_explicit_operand(
		execute_data, encoded, operand_type, operand);
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
	const zend_op *source_opline;
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
	const zend_native_code *code;
	zval *filename;
	zval *result = NULL;
	zend_native_status status;
	zend_native_diagnostic diagnostic;
	zend_native_compile_diagnostic compile_diagnostic;
	zend_native_compiler *component_compiler = NULL;
	uint32_t call_info;
	uint32_t first_function_bucket;
	uint32_t first_class_bucket;
	uint32_t first_compiled_function = 0;
	bool ephemeral_codeunit = false;
	bool cached_include = false;

	if (compiler == NULL
			|| execute_data == NULL || execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| source_opcode != ZEND_INCLUDE_OR_EVAL
			|| source_position_id >= execute_data->func->op_array.last) {
		zend_throw_error(NULL,
			"Native dynamic compiler is unavailable for include/eval");
		return ZEND_NATIVE_EXCEPTION;
	}
	source_opline =
		&execute_data->func->op_array.opcodes[source_position_id];
	execute_data->opline = source_opline;
	if ((extended_value == ZEND_INCLUDE_ONCE
			|| extended_value == ZEND_REQUIRE_ONCE)
			&& ZEND_NATIVE_OPERAND_TYPE(encoded_result) == IS_UNUSED
			&& zend_native_include_once_site_completed(
				compiler, source_opline)) {
		return ZEND_NATIVE_RETURNED;
	}
	if (!zend_native_dynamic_decode_operand(
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
	if (result_type != IS_UNUSED) {
		result = ZEND_CALL_VAR(execute_data, result_operand.var);
	}
	/*
	 * Avoid re-entering zend_include_or_eval(), including its bailout-state
	 * allocation, after an absolute include_once operand has been resolved and
	 * included.  A miss retains the complete semantic path.
	 */
	if (op1_type == IS_CONST
			&& (extended_value == ZEND_INCLUDE_ONCE
				|| extended_value == ZEND_REQUIRE_ONCE)
			&& Z_TYPE_P(filename) == IS_STRING
			&& IS_ABSOLUTE_PATH(
				Z_STRVAL_P(filename), Z_STRLEN_P(filename))) {
		zend_string *resolved_path = zend_resolve_path(Z_STR_P(filename));

		if (resolved_path != NULL) {
			bool already_included = zend_hash_exists(
				&EG(included_files), resolved_path);

			zend_string_release(resolved_path);
			if (already_included) {
				zend_native_complete_include_once_site(
					compiler, source_opline);
				if (result != NULL) {
					ZVAL_TRUE(result);
				}
				return ZEND_NATIVE_RETURNED;
			}
		}
	}
#ifdef ZEND_CHECK_STACK_LIMIT
	/* Zend parses and compiles the include before the TPDE compile checked
	 * below; its parser and AST compiler are deeply recursive too, so keep the
	 * same reserve for them. */
	if ((uintptr_t) zend_call_stack_position()
			<= (uintptr_t) EG(stack_limit) + ZEND_NATIVE_INCLUDE_STACK_RESERVE) {
		zend_native_dynamic_free_operand(execute_data, op1_type, op1);
		zend_call_stack_size_error();
		if (result_type != IS_UNUSED) {
			ZVAL_UNDEF(ZEND_CALL_VAR(execute_data, result_operand.var));
		}
		return ZEND_NATIVE_EXCEPTION;
	}
#endif
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
	if (new_op_array == ZEND_NATIVE_FAKE_OP_ARRAY) {
		if (op1_type == IS_CONST
				&& (extended_value == ZEND_INCLUDE_ONCE
					|| extended_value == ZEND_REQUIRE_ONCE)) {
			zend_native_complete_include_once_site(
				compiler, source_opline);
		}
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
	/*
	 * An op array without a refcount does not own its opcodes. OPcache
	 * preloading returns such an array after sharing its opcodes with the
	 * persistent script it optimizes later, so optimizing it here would free
	 * opcodes that the script still references.
	 */
	if (new_op_array->refcount != NULL
			&& !zend_native_executor_op_array_is_cache_owned(new_op_array)) {
		zend_optimize_runtime_op_array(
			new_op_array, extended_value == ZEND_EVAL);
	}
	call_info = (Z_TYPE_INFO(execute_data->This) & ZEND_CALL_HAS_THIS)
		| ZEND_CALL_NESTED_CODE | ZEND_CALL_HAS_SYMBOL_TABLE;
	previous = EG(current_execute_data);
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
	EG(current_execute_data) = previous;
	if (!zend_native_dynamic_compiler_adopt(compiler, new_op_array)) {
		zend_vm_stack_free_call_frame(call);
		zend_destroy_static_vars(new_op_array);
		destroy_op_array(new_op_array);
		efree_size(new_op_array, sizeof(zend_op_array));
		zend_throw_error(NULL,
			"Native dynamic codeunit owner capacity overflow");
		return ZEND_NATIVE_EXCEPTION;
	}
	ephemeral_codeunit = new_op_array->num_dynamic_func_defs == 0
		&& first_function_bucket == EG(function_table)->nNumUsed
		&& first_class_bucket == EG(class_table)->nNumUsed;
#ifdef ZEND_CHECK_STACK_LIMIT
	/* TPDE compilation consumes substantially more C stack than dispatching a
	 * VM opcode. Preserve enough of Zend's reserved stack for the exception
	 * path before recursively compiling an include from native code. */
	if ((uintptr_t) zend_call_stack_position()
			<= (uintptr_t) EG(stack_limit) + ZEND_NATIVE_INCLUDE_STACK_RESERVE) {
		zend_call_stack_size_error();
		entry_cell = NULL;
	} else
#endif
	if (compiler->product_compiler != NULL
			&& extended_value != ZEND_EVAL
			&& ((entry_cell = zend_native_executor_resolve_cached_include(
					new_op_array)) != NULL
				|| EG(exception) != NULL)) {
		/* The persistent generation of the cached script serves the code;
		 * the include itself still executes on this frame below. */
		cached_include = entry_cell != NULL;
	} else if (compiler->product_compiler != NULL) {
		const char *reason = zend_native_compile_trace_reason;
		zend_result compiled;

		memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
		zend_native_compile_trace_reason =
			extended_value == ZEND_EVAL ? "eval" : "include";
		compiled = zend_native_compiler_compile_dynamic_component(
			compiler->product_compiler, new_op_array,
			first_function_bucket, first_class_bucket,
			&component_compiler, &first_compiled_function,
			&entry_cell, &compile_diagnostic);
		zend_native_compile_trace_reason = reason;
		if (compiled == FAILURE) {
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
			|| (code = zend_native_entry_cell_load(entry_cell)) == NULL
			|| zend_native_dynamic_compiler_publish(
				compiler, new_op_array, entry_cell) == FAILURE) {
		if (component_compiler != NULL) {
			zend_native_compiler_release_ready_transients(
				component_compiler, first_compiled_function);
		}
		zend_vm_stack_free_call_frame(call);
		if (EG(exception) == NULL) {
			zend_throw_error(NULL,
				"Native compilation failed for dynamically created code");
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	if (cached_include) {
		zend_native_entry_cell_retain_active(entry_cell);
	}
	EG(current_execute_data) = call;
	status = zend_native_execute_frame(
		code, call, &diagnostic);
	EG(current_execute_data) = previous;
	if (cached_include) {
		zend_native_entry_cell_release_active(entry_cell);
	}
	call_info = ZEND_CALL_INFO(call);
	zend_vm_stack_free_call_frame(call);
	if (component_compiler != NULL) {
		bool retired = ephemeral_codeunit
			&& zend_native_dynamic_compiler_can_retire_last(
				compiler, new_op_array, entry_cell)
			&& zend_native_compiler_retire_dynamic_component(
				component_compiler, first_compiled_function,
				new_op_array);

		if (retired) {
			zend_native_dynamic_compiler_retire_last(
				compiler, new_op_array);
		} else {
			zend_native_compiler_release_ready_transients(
				component_compiler, first_compiled_function);
		}
	}
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
