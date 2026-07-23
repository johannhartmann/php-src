#include "Zend/Native/Compiler/zend_native_compiler.h"
#include "Zend/Native/Compiler/zend_native_compiler_internal.h"

#include "Zend/zend_closures.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/Optimizer/zend_func_info.h"
#include "Zend/Optimizer/zend_optimizer.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"
#include "Zend/Native/Compiler/zend_native_dynamic_code.h"
#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source_internal.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"
#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/MIR/zend_mir.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"

#include <stdint.h>
#include <string.h>

#define ZEND_NATIVE_COMPILER_ARENA_SIZE (64 * 1024)
#define ZEND_NATIVE_COMPILER_DEFAULT_CHUNK_SIZE 64

typedef struct _zend_native_compiler_module_host {
	zend_arena *arena;
	uint32_t successful_allocations;
	bool fail_allocation;
} zend_native_compiler_module_host;

typedef struct _zend_native_compiled_function {
	zend_op_array *op_array;
	zend_arena *ssa_arena;
	zend_ssa ssa;
	zend_op_array projected_op_array;
	zend_ssa projected_ssa;
	zend_op *projected_opcodes;
	zval *projected_literals;
	zend_ssa_op *projected_ssa_ops;
	zend_ssa_var *projected_ssa_vars;
	zend_ssa_var_info *projected_ssa_var_info;
	zend_mir_scalar_type_mask *argument_types;
	uint32_t argument_type_count;
	zend_native_source_effect *source_effects;
	uint32_t source_effect_count;
	uint32_t source_effect_capacity;
	uint32_t *exception_handler_oplines;
	zend_native_compiler_module_host module_host;
	zend_mir_module *module;
	zend_native_image *image;
	zend_native_code *code;
	zend_native_entry_cell entry_cell;
	zend_native_internal_call_cell *internal_call_cells;
	uint32_t internal_call_cell_count;
	zend_native_codeunit_state state;
	bool publish_pending;
} zend_native_compiled_function;

struct _zend_native_compiler {
	zend_script *script;
	zend_native_target target;
	size_t mir_chunk_size;
	zend_native_frame_probe_t frame_probe;
	void *frame_probe_context;
	zend_native_compile_observer_t observer;
	void *observer_context;
	zend_native_compile_fault fault;
	uint32_t unavailable_runtime_helper;
	bool abi_conformance_probe;
	zend_native_compiled_function **functions;
	uint32_t function_count;
	uint32_t function_capacity;
	zend_native_reentry_binding *reentry_bindings;
	uint32_t reentry_binding_capacity;
	zend_native_reentry_scope reentry_scope;
	bool reentry_active;
	zend_native_dynamic_compiler dynamic_compiler;
	bool dynamic_compiler_active;
	bool failed;
	zend_native_compile_diagnostic last_diagnostic;
};

static void zend_native_compiler_set_diagnostic(
	zend_native_compiler *compiler,
	zend_native_compile_diagnostic *diagnostic,
	zend_native_compile_phase phase,
	uint32_t code,
	const char *message)
{
	zend_native_compile_diagnostic local;

	memset(&local, 0, sizeof(local));
	local.phase = phase;
	local.code = code;
	if (message != NULL) {
		snprintf(local.message, sizeof(local.message), "%s", message);
	}
	if (diagnostic != NULL) {
		*diagnostic = local;
	}
	if (compiler != NULL) {
		compiler->last_diagnostic = local;
	}
	if (compiler != NULL && compiler->observer != NULL) {
		compiler->observer(compiler->observer_context, &local);
	}
}

static bool zend_native_compiler_emit_mir_diagnostic(
	void *context,
	const zend_mir_diagnostic *diagnostic)
{
	zend_native_compiler *compiler = context;
	zend_native_compile_diagnostic product;

	if (compiler == NULL || diagnostic == NULL) {
		return false;
	}
	memset(&product, 0, sizeof(product));
	product.phase = ZEND_NATIVE_COMPILE_PHASE_LOWERING;
	product.code = diagnostic->code;
	if (zend_mir_id_is_valid(diagnostic->location.source_position_id)) {
		product.source_opline = diagnostic->location.source_position_id;
		product.has_source_opline = true;
	}
	snprintf(product.message, sizeof(product.message), "%s",
		diagnostic->message);
	if (compiler->observer != NULL) {
		compiler->observer(compiler->observer_context, &product);
	}
	compiler->last_diagnostic = product;
	return true;
}

static void *zend_native_compiler_module_allocate(
	void *context, size_t size, size_t alignment)
{
	zend_native_compiler_module_host *host = context;
	void *allocation;
	size_t alignment_mask;
	uintptr_t address;

	if (host == NULL || host->arena == NULL || host->fail_allocation
			|| size == 0 || alignment == 0
			|| (alignment & (alignment - 1)) != 0
			|| size > SIZE_MAX - (alignment - 1)) {
		return NULL;
	}
	alignment_mask = alignment - 1;
	allocation = zend_arena_alloc(&host->arena, size + alignment_mask);
	address = (uintptr_t) allocation;
	if (address > UINTPTR_MAX - alignment_mask) {
		return NULL;
	}
	host->successful_allocations++;
	return (void *) ((address + alignment_mask) & ~alignment_mask);
}

static void zend_native_compiler_module_reset(void *context)
{
	zend_native_compiler_module_host *host = context;

	if (host != NULL && host->arena != NULL) {
		zend_arena_destroy(host->arena);
		host->arena = NULL;
	}
}

typedef struct _zend_native_compiler_module_context {
	zend_native_compiler *compiler;
	zend_native_compiler_module_host *host;
} zend_native_compiler_module_context;

static zend_mir_module *zend_native_compiler_module_create(
	void *context, zend_mir_module_id module_id,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_native_compiler_module_context *module_context = context;
	zend_native_compiler_module_host *host;
	zend_mir_allocator allocator;

	if (module_context == NULL || module_context->compiler == NULL
			|| (host = module_context->host) == NULL
			|| host->arena != NULL) {
		return NULL;
	}
	host->fail_allocation =
		module_context->compiler->fault
			== ZEND_NATIVE_COMPILE_FAULT_MODULE_ALLOCATION;
	host->arena = zend_arena_create(ZEND_NATIVE_COMPILER_ARENA_SIZE);
	if (host->arena == NULL) {
		return NULL;
	}
	allocator.context = host;
	allocator.allocate = zend_native_compiler_module_allocate;
	allocator.reset = zend_native_compiler_module_reset;
	return zend_mir_module_create(
		module_id, &allocator, module_context->compiler->mir_chunk_size,
		NULL, diagnostics);
}

static void zend_native_compiler_module_destroy(
	void *context, zend_mir_module *module)
{
	(void) context;
	zend_mir_module_destroy(module);
}

static zend_mir_mutator *zend_native_compiler_module_mutator(
	void *context, zend_mir_module *module)
{
	(void) context;
	return zend_mir_module_get_mutator(module);
}

static const zend_mir_view *zend_native_compiler_module_view(
	void *context, const zend_mir_module *module)
{
	(void) context;
	return zend_mir_module_get_view(module);
}

static bool zend_native_compiler_module_finalize(
	void *context, zend_mir_module *module)
{
	zend_native_compiler_module_context *module_context = context;

	return module_context->compiler->fault
			!= ZEND_NATIVE_COMPILE_FAULT_MODULE_FINALIZE
		&& zend_mir_module_finalize(module);
}

static bool zend_native_compiler_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_native_compiler_module_context *module_context = context;

	return module_context->compiler->fault
			!= ZEND_NATIVE_COMPILE_FAULT_STAGE1_VERIFY
		&& zend_mir_verify_stage1(view, diagnostics);
}

static bool zend_native_compiler_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	zend_native_compiler_module_context *module_context = context;

	return module_context->compiler->fault
			!= ZEND_NATIVE_COMPILE_FAULT_STAGE2_VERIFY
		&& zend_mir_verify_w03_scalar(view, diagnostics);
}

static zend_mir_scalar_type_mask zend_native_compiler_scalar_type_from_zval(
	const zval *value)
{
	switch (Z_TYPE_P(value)) {
		case IS_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case IS_FALSE:
		case IS_TRUE:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case IS_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case IS_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

static zend_mir_scalar_type_mask zend_native_compiler_argument_runtime_type(
	const zend_op_array *op_array,
	uint32_t ordinal,
	zend_mir_scalar_type_mask supplied_type)
{
	uint32_t type_mask;

	if (op_array == NULL || ordinal >= op_array->num_args
			|| op_array->arg_info == NULL) {
		return supplied_type;
	}
	type_mask = ZEND_TYPE_PURE_MASK(op_array->arg_info[ordinal].type);
	switch (type_mask) {
		case MAY_BE_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case MAY_BE_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case MAY_BE_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return supplied_type;
	}
}

static uint32_t zend_native_compiler_may_be_from_scalar_type(
	zend_mir_scalar_type_mask type)
{
	switch (type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			return MAY_BE_NULL;
		case ZEND_MIR_SCALAR_TYPE_I1:
			return MAY_BE_BOOL;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return MAY_BE_LONG;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return MAY_BE_DOUBLE;
		default:
			return 0;
	}
}

static zend_mir_scalar_type_mask zend_native_compiler_ssa_exact_type(
	const zend_ssa *ssa, int variable)
{
	uint32_t type;

	if (ssa == NULL || ssa->var_info == NULL || variable < 0
			|| variable >= ssa->vars_count) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	type = ssa->var_info[variable].type;
	switch (type) {
		case MAY_BE_NULL:
			return ZEND_MIR_SCALAR_TYPE_NULL;
		case MAY_BE_FALSE:
		case MAY_BE_TRUE:
		case MAY_BE_BOOL:
			return ZEND_MIR_SCALAR_TYPE_I1;
		case MAY_BE_LONG:
			return ZEND_MIR_SCALAR_TYPE_I64;
		case MAY_BE_DOUBLE:
			return ZEND_MIR_SCALAR_TYPE_F64;
		default:
			return ZEND_MIR_SCALAR_TYPE_NONE;
	}
}

static zend_mir_scalar_type_mask zend_native_compiler_operand_exact_type(
	const zend_op_array *op_array, const zend_ssa *ssa,
	uint32_t opline_index, uint8_t operand_type, const znode_op *operand,
	int ssa_use)
{
	if (operand_type == IS_CONST) {
		return zend_native_compiler_scalar_type_from_zval(
			RT_CONSTANT(&op_array->opcodes[opline_index], *operand));
	}
	return zend_native_compiler_ssa_exact_type(ssa, ssa_use);
}

static uint32_t zend_native_compiler_zend_type_from_scalar_type(
	zend_mir_scalar_type_mask type)
{
	switch (type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			return IS_NULL;
		case ZEND_MIR_SCALAR_TYPE_I1:
			return IS_FALSE;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return IS_LONG;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return IS_DOUBLE;
		default:
			return IS_UNDEF;
	}
}

static zend_native_compiled_function *zend_native_compiler_find_function(
	const zend_native_compiler *compiler, const zend_op_array *op_array)
{
	uint32_t index;

	for (index = 0; index < compiler->function_count; index++) {
		if (compiler->functions[index]->op_array == op_array) {
			return compiler->functions[index];
		}
	}
	return NULL;
}

static bool zend_native_compiler_reserve_functions(
	zend_native_compiler *compiler, uint32_t required)
{
	uint32_t new_capacity;

	if (required <= compiler->function_capacity) {
		return true;
	}
	new_capacity = compiler->function_capacity < 8
		? 8 : compiler->function_capacity;
	while (new_capacity < required) {
		if (new_capacity > UINT32_MAX / 2) {
			return false;
		}
		new_capacity *= 2;
	}
	compiler->functions = safe_erealloc(
		compiler->functions, new_capacity,
		sizeof(*compiler->functions), 0);
	memset(compiler->functions + compiler->function_capacity, 0,
		(new_capacity - compiler->function_capacity)
			* sizeof(*compiler->functions));
	compiler->function_capacity = new_capacity;
	return true;
}

static zend_native_compiled_function *zend_native_compiler_add_function(
	zend_native_compiler *compiler,
	zend_op_array *op_array,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiled_function *function;

	function = zend_native_compiler_find_function(compiler, op_array);
	if (function != NULL) {
		return function;
	}
	if (op_array == NULL
			|| !zend_native_compiler_reserve_functions(
				compiler, compiler->function_count + 1)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native codeunit registry allocation failed");
		return NULL;
	}
	function = ecalloc(1, sizeof(*function));
	function->op_array = op_array;
	function->state = (op_array->fn_flags & ZEND_ACC_GENERATOR) != 0
		? ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
		: ZEND_NATIVE_CODEUNIT_COMPILING;
	function->argument_type_count = op_array->num_args;
	if (function->argument_type_count != 0) {
		function->argument_types = ecalloc(
			function->argument_type_count,
			sizeof(*function->argument_types));
	}
	zend_native_entry_cell_init(
		&function->entry_cell, (zend_function *) op_array);
	if (compiler->frame_probe != NULL) {
		zend_native_entry_cell_set_frame_probe(
			&function->entry_cell, compiler->frame_probe,
			compiler->frame_probe_context);
	}
	if (function->state != ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
			&& zend_native_entry_cell_begin_compile(
				&function->entry_cell) == FAILURE) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native entry cell rejected synchronous compilation");
		efree(function->argument_types);
		efree(function);
		return NULL;
	}
	compiler->functions[compiler->function_count++] = function;
	return function;
}

static bool zend_native_compiler_merge_argument_type(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function,
	uint32_t ordinal,
	zend_mir_scalar_type_mask type,
	zend_native_compile_diagnostic *diagnostic)
{
	if (function == NULL || ordinal >= function->argument_type_count
			|| (!zend_mir_scalar_type_is_exact(type)
				&& type != ZEND_MIR_SCALAR_TYPE_NONE)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT,
			"native scalar operation has an invalid argument type");
		return false;
	}
	if (type == ZEND_MIR_SCALAR_TYPE_NONE) {
		return true;
	}
	if (function->argument_types[ordinal] != ZEND_MIR_SCALAR_TYPE_NONE
			&& function->argument_types[ordinal] != type) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			ZEND_MIRL_W05_UNSUPPORTED_ARGUMENT,
			"native entry cell has conflicting scalar call signatures");
		return false;
	}
	function->argument_types[ordinal] = type;
	return true;
}

static bool zend_native_compiler_build_ssa(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_optimizer_ctx optimizer;

	if (compiler->fault == ZEND_NATIVE_COMPILE_FAULT_SSA) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_SSA,
			1, "injected SSA analysis failure");
		return false;
	}
	function->ssa_arena =
		zend_arena_create(ZEND_NATIVE_COMPILER_ARENA_SIZE);
	if (function->ssa_arena == NULL) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_SSA,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"unable to allocate native SSA arena");
		return false;
	}
	memset(&optimizer, 0, sizeof(optimizer));
	optimizer.arena = function->ssa_arena;
	optimizer.script = compiler->script;
	optimizer.optimization_level = ZEND_OPTIMIZER_PASS_6;
	if (zend_dfa_analyze_op_array_with_dynamic_bindings(
			function->op_array, &optimizer, &function->ssa) == FAILURE) {
		function->ssa_arena = optimizer.arena;
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_SSA,
			1, "SSA analysis rejected a reachable native codeunit");
		return false;
	}
	function->ssa_arena = optimizer.arena;
	return true;
}

static bool zend_native_compiler_verify_projected_return_type(
	const zend_native_compiled_function *function, uint32_t opline_index)
{
	const zend_op *opline =
		&function->projected_op_array.opcodes[opline_index];
	const zend_ssa_op *ssa_op = &function->projected_ssa.ops[opline_index];
	const zend_arg_info *return_info;
	zend_mir_scalar_type_mask type;
	uint32_t type_mask;

	if (opline->op1_type == IS_UNUSED
			|| function->op_array->arg_info == NULL) {
		return false;
	}
	type = zend_native_compiler_operand_exact_type(
		&function->projected_op_array, &function->projected_ssa, opline_index,
		opline->op1_type, &opline->op1, ssa_op->op1_use);
	if (!zend_mir_scalar_type_is_exact(type)) {
		return false;
	}
	return_info = function->op_array->arg_info - 1;
	type_mask = ZEND_TYPE_PURE_MASK(return_info->type);
	return type_mask == MAY_BE_NULL
		|| type_mask == MAY_BE_FALSE
		|| type_mask == MAY_BE_TRUE
		|| type_mask == MAY_BE_BOOL
		|| type_mask == MAY_BE_LONG
		|| type_mask == MAY_BE_DOUBLE;
}

static bool zend_native_compiler_prepare_projection(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function)
{
	const zend_op_array *source = function->op_array;
	zend_mir_frontend_diagnostic frontend_diagnostic;
	uint32_t echo_count = 0;
	uint32_t return_check_count = 0;
	uint32_t projected_variable_count;
	uint32_t next_ssa_variable;
	uint32_t next_literal;
	size_t projected_opcode_bytes;
	size_t projected_literal_bytes;
	size_t projected_storage_bytes;
	uint32_t projected_literal_count;
	uint32_t echo_index = 0;
	uint32_t index;

	if (source == NULL || function->ssa.ops == NULL
			|| (function->ssa.vars_count != 0
				&& (function->ssa.vars == NULL
					|| function->ssa.var_info == NULL))) {
		return false;
	}
	for (index = 0; index < source->last; index++) {
		if (source->opcodes[index].opcode == ZEND_ECHO) {
			echo_count++;
		} else if (source->opcodes[index].opcode == ZEND_VERIFY_RETURN_TYPE) {
			return_check_count++;
		}
	}
	if (echo_count > (UINT32_MAX - (uint32_t) function->ssa.vars_count) / 2
			|| source->last > UINT32_MAX - echo_count
			|| source->T > UINT32_MAX - echo_count
			|| source->last_literal > UINT32_MAX - echo_count
			|| source->last_literal + echo_count
				> UINT32_MAX - return_check_count) {
		return false;
	}
	projected_variable_count =
		(uint32_t) function->ssa.vars_count + echo_count * 2;
	projected_literal_count =
		source->last_literal + echo_count + return_check_count;
	if ((size_t) (source->last == 0 ? 1 : source->last)
			> (SIZE_MAX - 15) / sizeof(*function->projected_opcodes)
			|| (size_t) (projected_literal_count == 0
				? 1 : projected_literal_count)
				> SIZE_MAX / sizeof(*function->projected_literals)) {
		return false;
	}
	projected_opcode_bytes = ZEND_MM_ALIGNED_SIZE_EX(
		(size_t) (source->last == 0 ? 1 : source->last)
			* sizeof(*function->projected_opcodes), 16);
	projected_literal_bytes =
		(size_t) (projected_literal_count == 0 ? 1 : projected_literal_count)
			* sizeof(*function->projected_literals);
	if (projected_opcode_bytes > SIZE_MAX - projected_literal_bytes) {
		return false;
	}
	projected_storage_bytes = projected_opcode_bytes + projected_literal_bytes;
	/* Runtime constant operands are signed offsets from their opline. Keep
	 * projected opcodes and literals in the same allocation, as pass two does,
	 * so the representation remains valid with the system allocator under
	 * AddressSanitizer as well as with Zend MM. */
	function->projected_opcodes = ecalloc(1, projected_storage_bytes);
	function->projected_literals = (zval *) (
		(char *) function->projected_opcodes + projected_opcode_bytes);
	function->projected_ssa_ops = ecalloc(
		source->last == 0 ? 1 : source->last,
		sizeof(*function->projected_ssa_ops));
	function->projected_ssa_vars = ecalloc(
		projected_variable_count == 0 ? 1 : projected_variable_count,
		sizeof(*function->projected_ssa_vars));
	function->projected_ssa_var_info = ecalloc(
		projected_variable_count == 0 ? 1 : projected_variable_count,
		sizeof(*function->projected_ssa_var_info));
	function->source_effect_capacity = echo_count + source->last;
	if (function->source_effect_capacity != 0) {
		function->source_effects = ecalloc(
			function->source_effect_capacity,
			sizeof(*function->source_effects));
	}
	if (source->last != 0) {
		memcpy(function->projected_opcodes, source->opcodes,
			(size_t) source->last * sizeof(*function->projected_opcodes));
		memcpy(function->projected_ssa_ops, function->ssa.ops,
			(size_t) source->last * sizeof(*function->projected_ssa_ops));
	}
	if (source->last_literal != 0) {
		memcpy(function->projected_literals, source->literals,
			(size_t) source->last_literal
				* sizeof(*function->projected_literals));
	}
	if (function->ssa.vars_count != 0) {
		memcpy(function->projected_ssa_vars, function->ssa.vars,
			(size_t) function->ssa.vars_count
				* sizeof(*function->projected_ssa_vars));
		memcpy(function->projected_ssa_var_info, function->ssa.var_info,
			(size_t) function->ssa.vars_count
				* sizeof(*function->projected_ssa_var_info));
	}
	function->projected_op_array = *source;
	function->projected_op_array.opcodes = function->projected_opcodes;
	function->projected_op_array.literals = function->projected_literals;
	function->projected_op_array.T = source->T + echo_count;
	function->projected_op_array.last_literal = source->last_literal;
	function->projected_ssa = function->ssa;
	function->projected_ssa.ops = function->projected_ssa_ops;
	function->projected_ssa.vars = function->projected_ssa_vars;
	function->projected_ssa.var_info = function->projected_ssa_var_info;
	memset(&frontend_diagnostic, 0, sizeof(frontend_diagnostic));
	if (zend_mir_frontend_project_w10_result_facts(
			compiler->script, source, &function->ssa,
			&function->projected_ssa, &frontend_diagnostic)
			!= ZEND_MIR_LOWERING_SUCCESS) {
		zend_native_compiler_set_diagnostic(
			compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			frontend_diagnostic.code,
			"cannot project a direct native call result");
		return false;
	}
	function->projected_ssa.vars_count = (int) projected_variable_count;
	next_ssa_variable = (uint32_t) function->ssa.vars_count;
	next_literal = source->last_literal;

	for (index = 0; index < source->last; index++) {
		const zend_op *original = &source->opcodes[index];
		zend_op *opline = &function->projected_opcodes[index];
		zend_ssa_op *ssa_op = &function->projected_ssa_ops[index];
		ptrdiff_t literal_index;

		if (opline->op1_type == IS_CONST) {
			literal_index = RT_CONSTANT(original, original->op1) - source->literals;
			if (literal_index < 0
					|| (uint32_t) literal_index >= source->last_literal) {
				return false;
			}
#if ZEND_USE_ABS_CONST_ADDR
			opline->op1.zv = &function->projected_literals[literal_index];
#else
			opline->op1.constant = (uint32_t) (
				(char *) &function->projected_literals[literal_index]
				- (char *) opline);
#endif
		}
		if (opline->op2_type == IS_CONST) {
			literal_index = RT_CONSTANT(original, original->op2) - source->literals;
			if (literal_index < 0
					|| (uint32_t) literal_index >= source->last_literal) {
				return false;
			}
#if ZEND_USE_ABS_CONST_ADDR
			opline->op2.zv = &function->projected_literals[literal_index];
#else
			opline->op2.constant = (uint32_t) (
				(char *) &function->projected_literals[literal_index]
				- (char *) opline);
#endif
		}
		if (original->opcode == ZEND_ECHO) {
			zend_native_source_effect *effect =
				&function->source_effects[function->source_effect_count++];
			uint32_t ssa_variable = next_ssa_variable++;
			uint32_t variable = source->last_var + source->T + echo_index;
			zend_ssa_var *ssa_var =
				&function->projected_ssa_vars[ssa_variable];
			zend_mir_scalar_type_mask type = zend_native_compiler_operand_exact_type(
				&function->projected_op_array, &function->projected_ssa,
				index, opline->op1_type, &opline->op1, ssa_op->op1_use);

			effect->source_position_id = index;
			effect->kind = compiler->abi_conformance_probe
				? ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE
				: ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR;
			effect->exact_type = type;
			effect->target_block_id = ZEND_MIR_ID_INVALID;
			if (!zend_mir_scalar_type_is_exact(type)) {
				zend_native_compiler_set_diagnostic(
					compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
					ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED,
					"native echo requires an exact scalar value");
				return false;
			}
			if (type == ZEND_MIR_SCALAR_TYPE_NULL) {
				uint32_t literal = next_literal++;

				if (ssa_op->op1_use >= 0) {
					int original_use = ssa_op->op1_use;

					zend_ssa_unlink_use_chain(
						&function->projected_ssa, (int) index, original_use);
					ssa_op->op1_use = -1;
					ssa_op->op1_use_chain = -1;
				}
				ZVAL_FALSE(&function->projected_literals[literal]);
#if ZEND_USE_ABS_CONST_ADDR
				opline->op1.zv = &function->projected_literals[literal];
#else
				opline->op1.constant = (uint32_t) (
					(char *) &function->projected_literals[literal]
					- (char *) opline);
#endif
				opline->op1_type = IS_CONST;
			}
			/* Use the scalar conversion that is valid for the proven source
			 * type. The result is deliberately dead: TPDE replaces this
			 * verified carrier with the source-level echo effect. */
			opline->opcode = type == ZEND_MIR_SCALAR_TYPE_I1
				|| type == ZEND_MIR_SCALAR_TYPE_NULL
				? ZEND_BOOL_NOT : ZEND_BOOL;
			opline->op2_type = IS_UNUSED;
			memset(&opline->op2, 0, sizeof(opline->op2));
			opline->result_type = IS_TMP_VAR;
			opline->result.var = NUM_VAR(variable);
			ssa_op->op2_use = -1;
			ssa_op->op2_def = -1;
			ssa_op->op2_use_chain = -1;
			ssa_op->result_use = -1;
			ssa_op->result_def = (int) ssa_variable;
			ssa_op->res_use_chain = -1;
			memset(ssa_var, 0, sizeof(*ssa_var));
			ssa_var->var = (int) variable;
			ssa_var->scc = -1;
			ssa_var->definition = (int) index;
			ssa_var->use_chain = -1;
			function->projected_ssa_var_info[ssa_variable].type = MAY_BE_BOOL;
			echo_index++;
			continue;
		}
		if (original->opcode == ZEND_VERIFY_RETURN_TYPE) {
			uint32_t lineno = opline->lineno;
			zend_mir_scalar_type_mask operand_type =
				zend_native_compiler_operand_exact_type(
					&function->projected_op_array,
					&function->projected_ssa,
					index, opline->op1_type, &opline->op1,
					ssa_op->op1_use);

			if (!zend_native_compiler_verify_projected_return_type(
					function, index)) {
				if (ssa_op->result_def >= 0
						&& ssa_op->result_def
							< function->projected_ssa.vars_count) {
					/* W10 keeps object and call results as canonical zvals.  The
					 * native frame epilogue performs the authoritative Zend return
					 * type check; retain the SSA definition as an identity copy. */
					opline->opcode = ZEND_QM_ASSIGN;
					opline->op2_type = IS_UNUSED;
					memset(&opline->op2, 0, sizeof(opline->op2));
					ssa_op->op2_use = -1;
					ssa_op->op2_def = -1;
					ssa_op->op2_use_chain = -1;
					function->projected_ssa_var_info[
						ssa_op->result_def].has_range = 0;
					continue;
				}
				zend_ssa_rename_defs_of_instr(
					&function->projected_ssa, ssa_op);
				zend_ssa_remove_instr(
					&function->projected_ssa, opline, ssa_op);
				opline->lineno = lineno;
				continue;
			}
			/*
			 * Scalar return checks that reach this point were retained as an
			 * identity operation above. The real Zend frame epilogue owns the
			 * authoritative return-type check and coercion.
			 */
			const zend_arg_info *return_info =
				function->op_array->arg_info - 1;
			uint32_t operand_zend_type =
				zend_native_compiler_zend_type_from_scalar_type(operand_type);

			if (ssa_op->result_def >= 0
					&& !ZEND_TYPE_CONTAINS_CODE(
						return_info->type, operand_zend_type)) {
				/* Constant return checks define the temporary consumed by
				 * RETURN. Preserve that definition with a scalar identity
				 * operation. The declared-type conversion remains below;
				 * zend_native_execute_frame performs the actual Zend return
				 * type check and any permitted scalar coercion. */
				{
					uint32_t literal = next_literal++;

					if (operand_type == ZEND_MIR_SCALAR_TYPE_I1) {
						opline->opcode = ZEND_BOOL_XOR;
						ZVAL_FALSE(&function->projected_literals[literal]);
					} else {
						opline->opcode = ZEND_ADD;
						if (operand_type == ZEND_MIR_SCALAR_TYPE_I64) {
							ZVAL_LONG(&function->projected_literals[literal], 0);
						} else {
							ZVAL_DOUBLE(
								&function->projected_literals[literal], 0.0);
						}
					}
					opline->op2_type = IS_CONST;
#if ZEND_USE_ABS_CONST_ADDR
					opline->op2.zv = &function->projected_literals[literal];
#else
					opline->op2.constant = (uint32_t) (
						(char *) &function->projected_literals[literal]
						- (char *) opline);
#endif
					function->projected_op_array.last_literal = next_literal;
				}
				function->projected_ssa_var_info[ssa_op->result_def].type =
					zend_native_compiler_may_be_from_scalar_type(operand_type);
				continue;
			}
			zend_ssa_rename_defs_of_instr(
				&function->projected_ssa, ssa_op);
			zend_ssa_remove_instr(&function->projected_ssa, opline, ssa_op);
			opline->lineno = lineno;
			continue;
		}
		if (original->opcode == ZEND_RECV
				|| original->opcode == ZEND_RECV_INIT) {
			uint32_t ordinal = original->op1.num - 1;
			zend_mir_scalar_type_mask type = ordinal < function->argument_type_count
				? function->argument_types[ordinal]
				: ZEND_MIR_SCALAR_TYPE_NONE;

			if (original->opcode == ZEND_RECV_INIT
					&& type == ZEND_MIR_SCALAR_TYPE_NONE
					&& original->op2_type == IS_CONST) {
				type = zend_native_compiler_scalar_type_from_zval(
					RT_CONSTANT(original, original->op2));
			}
			if (zend_mir_scalar_type_is_exact(type)
					&& ssa_op->result_def >= 0
					&& ssa_op->result_def < function->projected_ssa.vars_count) {
				function->projected_ssa_var_info[ssa_op->result_def].type =
					zend_native_compiler_may_be_from_scalar_type(type);
			}
			if (original->opcode == ZEND_RECV_INIT) {
				opline->opcode = ZEND_RECV;
				opline->op2_type = IS_UNUSED;
				memset(&opline->op2, 0, sizeof(opline->op2));
			}
		}
	}
	function->projected_ssa.vars_count = (int) next_ssa_variable;
	function->projected_op_array.last_literal = next_literal;
	return true;
}

static bool zend_native_compiler_prepare_exception_routes(
	zend_native_compiled_function *function)
{
	uint32_t index;

	if (function->op_array == NULL || function->ssa.cfg.map == NULL) {
		return false;
	}
	function->exception_handler_oplines = ecalloc(
		function->op_array->last == 0 ? 1 : function->op_array->last,
		sizeof(*function->exception_handler_oplines));
	for (index = 0; index < function->op_array->last; index++) {
		zend_mir_source_block_id source_handler_block;
		uint32_t handler_opline;

		function->exception_handler_oplines[index] = ZEND_MIR_ID_INVALID;
		if (zend_mir_zend_op_array_exception_handler(
				function->op_array, &function->ssa, index,
				&source_handler_block, &handler_opline)) {
			function->exception_handler_oplines[index] = handler_opline;
		}
	}
	return true;
}

static bool zend_native_compiler_add_exception_routes(
	zend_native_compiler *compiler, zend_native_compiled_function *function)
{
	const zend_mir_view *view = zend_native_compiler_module_view(
		compiler, function->module);
	uint32_t instruction_count;
	uint32_t index;

	if (view == NULL || function->exception_handler_oplines == NULL) {
		return false;
	}
	instruction_count = view->instruction_count(view->context);
	for (index = 0; index < instruction_count; index++) {
		zend_mir_instruction_record instruction;
		uint32_t handler_opline;
		zend_mir_block_id target_block = ZEND_MIR_ID_INVALID;
		uint32_t candidate_index;

		if (!view->instruction_at(view->context, index, &instruction)) {
			return false;
		}
		if ((!zend_mir_opcode_is_executable_value(instruction.opcode)
				&& instruction.opcode != ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL)
				|| !zend_mir_id_is_valid(instruction.source_position_id)
				|| instruction.source_position_id
					>= function->op_array->last
				|| !zend_mir_id_is_valid(handler_opline =
					function->exception_handler_oplines[
						instruction.source_position_id])) {
			continue;
		}
		for (candidate_index = 0; candidate_index < instruction_count;
				candidate_index++) {
			zend_mir_instruction_record candidate;

			if (!view->instruction_at(
					view->context, candidate_index, &candidate)) {
				return false;
			}
			if (candidate.source_position_id == handler_opline
					&& (candidate.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
						|| candidate.opcode
							== ZEND_MIR_OPCODE_FINALLY_ENTER)) {
				if (zend_mir_id_is_valid(target_block)
						&& target_block != candidate.block_id) {
					return false;
				}
				target_block = candidate.block_id;
			}
		}
		if (!zend_mir_id_is_valid(target_block)
				|| function->source_effect_count
					>= function->source_effect_capacity) {
			return false;
		}
		zend_native_source_effect *effect =
			&function->source_effects[function->source_effect_count++];
		effect->source_position_id = instruction.source_position_id;
		effect->kind = ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE;
		effect->exact_type = ZEND_MIR_SCALAR_TYPE_NONE;
		effect->target_block_id = target_block;
	}
	return true;
}

static bool zend_native_compiler_lower_function(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiler_module_context module_context;
	zend_mir_lowering_module_ops module_ops;
	zend_mir_diagnostic_sink diagnostics;
	zend_mir_w08_lowering_result result;

	if (function->projected_opcodes == NULL
			&& !zend_native_compiler_prepare_projection(
				compiler, function)) {
		if (diagnostic != NULL) {
			*diagnostic = compiler->last_diagnostic;
		}
		return false;
	}
	if (function->exception_handler_oplines == NULL
			&& !zend_native_compiler_prepare_exception_routes(function)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			ZEND_MIRL_W04_SOURCE_MIR_MAPPING_FAILED,
			"cannot build native exception routes");
		return false;
	}
	memset(&function->module_host, 0, sizeof(function->module_host));
	module_context.compiler = compiler;
	module_context.host = &function->module_host;
	memset(&module_ops, 0, sizeof(module_ops));
	module_ops.context = &module_context;
	module_ops.create = zend_native_compiler_module_create;
	module_ops.destroy = zend_native_compiler_module_destroy;
	module_ops.mutator = zend_native_compiler_module_mutator;
	module_ops.view = zend_native_compiler_module_view;
	module_ops.finalize = zend_native_compiler_module_finalize;
	module_ops.verify_stage1 = zend_native_compiler_verify_stage1;
	module_ops.verify_stage2 = zend_native_compiler_verify_stage2;
	memset(&diagnostics, 0, sizeof(diagnostics));
	diagnostics.context = compiler;
	diagnostics.emit = zend_native_compiler_emit_mir_diagnostic;
	diagnostics.limit = UINT32_MAX;
	result = zend_mir_lower_w11_zend_op_array(
		compiler->script, &function->projected_op_array,
		&function->projected_ssa, &module_ops, &diagnostics);
	if (!zend_mir_lowering_result_is_w08_failure_atomic(&result)) {
		if (result.lowering.module != NULL) {
			zend_mir_module_destroy(result.lowering.module);
		}
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			ZEND_MIRL_MUTATION_FAILED,
			"native lowering returned a non-atomic result");
		return false;
	}
	if (result.lowering.status != ZEND_MIR_LOWERING_SUCCESS
			|| result.lowering.module == NULL) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			result.lowering.diagnostic_code,
			"native lowering rejected a reachable codeunit");
		return false;
	}
	function->module = result.lowering.module;
	if (!zend_native_compiler_add_exception_routes(compiler, function)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			ZEND_MIRL_W04_SOURCE_MIR_MAPPING_FAILED,
			"native exception route mapping failed");
		return false;
	}
	return true;
}

static zend_op_array *zend_native_compiler_resolve_native_target(
	zend_native_compiler *compiler,
	zend_op_array *caller,
	const zend_mir_call_view *calls,
	const zend_mir_call_target_ref *target)
{
	zend_function *function;
	const zend_ssa *caller_ssa = NULL;
	uint32_t declaration_id = 1;
	uint32_t index;

	if (target != NULL && target->kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
		/* Dynamic call sites carry no persistent function identity.  The
		 * caller cell is only a codegen placeholder; zend_native_call_begin()
		 * resolves and compiles the concrete request-local target. */
		return caller;
	}
	if (target == NULL
			|| (target->kind != ZEND_MIR_CALL_TARGET_DIRECT_USER
				&& target->kind != ZEND_MIR_CALL_TARGET_METHOD_USER)
			|| target->function_symbol_id != target->op_array_id) {
		return NULL;
	}
	if (target->kind == ZEND_MIR_CALL_TARGET_METHOD_USER) {
		if (compiler == NULL || caller == NULL || calls == NULL
				|| calls->call_site_count == NULL
				|| calls->call_site_at == NULL) {
			return NULL;
		}
		for (index = 0; index < compiler->function_count; index++) {
			if (compiler->functions[index]->op_array == caller) {
				caller_ssa = &compiler->functions[index]->ssa;
				break;
			}
		}
		if (caller_ssa == NULL) {
			return NULL;
		}
		for (index = 0; index < calls->call_site_count(calls->context); index++) {
			zend_mir_call_site_ref site;

			if (!calls->call_site_at(calls->context, index, &site)
					|| site.target_id != target->id) {
				continue;
			}
			if (site.source_init_opline_index >= caller->last) {
				return NULL;
			}
			if (caller->opcodes[site.source_init_opline_index].opcode
					== ZEND_NEW && target->num_args == 0
					&& target->required_num_args == 0) {
				/* A constructorless NEW is represented by Zend as NEW followed
				 * by an empty DO_FCALL.  The runtime creates the object and
				 * consumes that empty call without invoking user code. */
				return caller;
			}
			function = zend_mir_zend_source_resolve_user_method_call(
				compiler->script, caller, caller_ssa,
				site.source_init_opline_index);
			if (function == NULL
					&& (caller->opcodes[site.source_init_opline_index].opcode
							== ZEND_INIT_METHOD_CALL
						|| caller->opcodes[site.source_init_opline_index].opcode
							== ZEND_INIT_STATIC_METHOD_CALL
						|| caller->opcodes[site.source_init_opline_index].opcode
							== ZEND_INIT_PARENT_PROPERTY_HOOK_CALL)) {
				/* The receiver class is intentionally request-local for a
				 * polymorphic instance or static method call.  Bind the generated
				 * site to the caller cell as a placeholder; zend_native_call_begin()
				 * resolves the concrete method and the reentry resolver compiles it
				 * atomically. */
				return caller;
			}
			if (function == NULL || function->type != ZEND_USER_FUNCTION) {
				return NULL;
			}
			return &function->op_array;
		}
		return NULL;
	}
	if (target->op_array_id == 0) {
		return caller;
	}
	ZEND_HASH_FOREACH_PTR(&compiler->script->function_table, function) {
		if (function == NULL || function->type != ZEND_USER_FUNCTION) {
			continue;
		}
		if (declaration_id == target->op_array_id) {
			return &function->op_array;
		}
		if (declaration_id == ZEND_MIR_ID_MAX) {
			break;
		}
		declaration_id++;
	} ZEND_HASH_FOREACH_END();
	return NULL;
}

static zend_function *zend_native_compiler_resolve_internal_target(
	zend_native_compiler *compiler,
	zend_op_array *caller,
	const zend_mir_call_view *calls,
	const zend_mir_call_target_ref *target,
	const zend_op **init_opline_out)
{
	uint32_t index;

	if (compiler == NULL || caller == NULL || calls == NULL || target == NULL
			|| target->kind != ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
			|| calls->call_site_count == NULL || calls->call_site_at == NULL) {
		return NULL;
	}
	for (index = 0; index < calls->call_site_count(calls->context); index++) {
		zend_mir_call_site_ref site;
		zend_function *function;
		const zend_op *init;
		uint32_t function_index;
		const zend_ssa *ssa = NULL;

		if (!calls->call_site_at(calls->context, index, &site)
				|| site.target_id != target->id) {
			continue;
		}
		if (site.source_init_opline_index >= caller->last) {
			return NULL;
		}
		init = &caller->opcodes[site.source_init_opline_index];
		for (function_index = 0;
				function_index < compiler->function_count;
				function_index++) {
			if (compiler->functions[function_index]->op_array == caller) {
				ssa = &compiler->functions[function_index]->ssa;
				break;
			}
		}
		function = ssa == NULL ? NULL
			: zend_mir_zend_source_resolve_internal_call(
				compiler->script, caller, ssa,
				site.source_init_opline_index);
		if (function == NULL || function->type != ZEND_INTERNAL_FUNCTION) {
			return NULL;
		}
		if (init_opline_out != NULL) {
			*init_opline_out = init;
		}
		return function;
	}
	return NULL;
}

static const zend_arg_info *zend_native_compiler_internal_argument_info(
	const zend_function *function, uint32_t ordinal)
{
	if (function == NULL || function->type != ZEND_INTERNAL_FUNCTION
			|| function->common.arg_info == NULL
			|| function->common.num_args == 0) {
		return NULL;
	}
	if (ordinal < function->common.num_args) {
		return &function->common.arg_info[ordinal];
	}
	if ((function->common.fn_flags & ZEND_ACC_VARIADIC) != 0) {
		return &function->common.arg_info[function->common.num_args - 1];
	}
	return NULL;
}

static zend_op_array *zend_native_compiler_resolve_callback_argument(
	zend_native_compiler *compiler,
	const zend_op_array *caller,
	const zend_mir_call_argument_ref *argument,
	bool nullable,
	bool *no_user_reentry)
{
	const zend_op *send;
	zval *callback;
	zend_string *lower_name;
	zend_function *function;

	*no_user_reentry = false;
	if (argument == NULL || argument->send_opline_index >= caller->last) {
		return NULL;
	}
	send = &caller->opcodes[argument->send_opline_index];
	if (send->op1_type != IS_CONST) {
		return NULL;
	}
	callback = RT_CONSTANT(send, send->op1);
	if (nullable && Z_TYPE_P(callback) == IS_NULL) {
		*no_user_reentry = true;
		return NULL;
	}
	if (Z_TYPE_P(callback) != IS_STRING) {
		return NULL;
	}
	lower_name = zend_string_tolower(Z_STR_P(callback));
	function = zend_hash_find_ptr(
		&compiler->script->function_table, lower_name);
	if (function == NULL) {
		function = zend_hash_find_ptr(EG(function_table), lower_name);
		if (function != NULL && function->type == ZEND_INTERNAL_FUNCTION) {
			*no_user_reentry = true;
			function = NULL;
		}
	}
	zend_string_release(lower_name);
	return function != NULL && function->type == ZEND_USER_FUNCTION
		? &function->op_array : NULL;
}

static zend_op_array *zend_native_compiler_find_source_op_array(
	zend_op_array *candidate, const zend_op_array *resolved, uint32_t depth)
{
	uint32_t index;

	if (candidate == NULL || resolved == NULL || depth > 64) {
		return NULL;
	}
	if (candidate == resolved
			|| (candidate->opcodes == resolved->opcodes
				&& candidate->last == resolved->last)) {
		return candidate;
	}
	for (index = 0; index < candidate->num_dynamic_func_defs; index++) {
		zend_op_array *found = zend_native_compiler_find_source_op_array(
			candidate->dynamic_func_defs[index], resolved, depth + 1);

		if (found != NULL) {
			return found;
		}
	}
	return NULL;
}

static zend_op_array *zend_native_compiler_canonical_reentry_op_array(
	zend_native_compiler *compiler, const zend_op_array *resolved)
{
	zend_op_array *found;
	zend_function *function;
	zend_class_entry *class_entry;
	uint32_t index;

	if (compiler == NULL || resolved == NULL) {
		return NULL;
	}
	for (index = 0; index < compiler->function_count; index++) {
		found = zend_native_compiler_find_source_op_array(
			compiler->functions[index]->op_array, resolved, 0);
		if (found != NULL) {
			return found;
		}
	}
	found = zend_native_compiler_find_source_op_array(
		&compiler->script->main_op_array, resolved, 0);
	if (found != NULL) {
		return found;
	}
	ZEND_HASH_FOREACH_PTR(&compiler->script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION) {
			found = zend_native_compiler_find_source_op_array(
				&function->op_array, resolved, 0);
			if (found != NULL) {
				return found;
			}
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&compiler->script->class_table, class_entry) {
		zend_property_info *property_info;
		uint32_t hook_index;

		if (class_entry == NULL) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
			if (function != NULL && function->type == ZEND_USER_FUNCTION) {
				found = zend_native_compiler_find_source_op_array(
					&function->op_array, resolved, 0);
				if (found != NULL) {
					return found;
				}
			}
		} ZEND_HASH_FOREACH_END();
		if (class_entry->num_hooked_props == 0) {
			continue;
		}
		ZEND_HASH_MAP_FOREACH_PTR(
				&class_entry->properties_info, property_info) {
			if (property_info->ce != class_entry
					|| property_info->hooks == NULL) {
				continue;
			}
			for (hook_index = 0; hook_index < ZEND_PROPERTY_HOOK_COUNT;
					hook_index++) {
				function = property_info->hooks[hook_index];
				if (function == NULL || function->type != ZEND_USER_FUNCTION) {
					continue;
				}
				found = zend_native_compiler_find_source_op_array(
					&function->op_array, resolved, 0);
				if (found != NULL) {
					return found;
				}
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(EG(function_table), function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION) {
			found = zend_native_compiler_find_source_op_array(
				&function->op_array, resolved, 0);
			if (found != NULL) {
				return found;
			}
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(EG(class_table), class_entry) {
		zend_property_info *property_info;
		uint32_t hook_index;

		if (class_entry == NULL) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
			if (function != NULL && function->type == ZEND_USER_FUNCTION) {
				found = zend_native_compiler_find_source_op_array(
					&function->op_array, resolved, 0);
				if (found != NULL) {
					return found;
				}
			}
		} ZEND_HASH_FOREACH_END();
		if (class_entry->num_hooked_props == 0) {
			continue;
		}
		ZEND_HASH_MAP_FOREACH_PTR(
				&class_entry->properties_info, property_info) {
			if (property_info->ce != class_entry
					|| property_info->hooks == NULL) {
				continue;
			}
			for (hook_index = 0; hook_index < ZEND_PROPERTY_HOOK_COUNT;
					hook_index++) {
				function = property_info->hooks[hook_index];
				if (function == NULL || function->type != ZEND_USER_FUNCTION) {
					continue;
				}
				found = zend_native_compiler_find_source_op_array(
					&function->op_array, resolved, 0);
				if (found != NULL) {
					return found;
				}
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();
	return NULL;
}

static bool zend_native_compiler_discover_native_callees(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function)
{
	const zend_mir_call_view *calls =
		zend_mir_module_get_call_view(function->module);
	uint32_t target_count;
	uint32_t index;

	if (calls == NULL) {
		return true;
	}
	if (calls->call_target_count == NULL || calls->call_target_at == NULL) {
		zend_native_compiler_set_diagnostic(
			compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"native call model is unavailable");
		return false;
	}
	target_count = calls->call_target_count(calls->context);
	for (index = 0; index < target_count; index++) {
		zend_mir_call_target_ref target;
		zend_op_array *callee;

		if (!calls->call_target_at(calls->context, index, &target)) {
			zend_native_compiler_set_diagnostic(
				compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"native call target table is unreadable");
			return false;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			if (zend_native_compiler_resolve_internal_target(
					compiler, function->op_array, calls, &target, NULL) == NULL) {
				return false;
			}
			continue;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
			continue;
		}
		callee = zend_native_compiler_resolve_native_target(
			compiler, function->op_array, calls, &target);
		if (callee == NULL
				|| zend_native_compiler_add_function(
					compiler, callee, NULL) == NULL) {
			zend_native_compiler_set_diagnostic(
				compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
				ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
				"native call target cannot be resolved to its source function");
			return false;
		}
	}
	if (calls->call_site_count == NULL || calls->call_site_at == NULL
			|| calls->call_argument_at == NULL) {
		return false;
	}
	for (index = 0; index < calls->call_site_count(calls->context); index++) {
		zend_mir_call_site_ref site;
		zend_mir_call_target_ref target;
		zend_op_array *callee = NULL;
		zend_native_compiled_function *native_callee;
		const zend_mir_view *view = zend_native_compiler_module_view(
			compiler, function->module);
		uint32_t target_index;
		uint32_t argument_index;
		bool found_target = false;

		if (!calls->call_site_at(calls->context, index, &site)) {
			return false;
		}
		for (target_index = 0; target_index < target_count; target_index++) {
			if (!calls->call_target_at(
					calls->context, target_index, &target)) {
				return false;
			}
			if (target.id == site.target_id) {
				found_target = true;
				if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
					callee = NULL;
					break;
				}
				callee = zend_native_compiler_resolve_native_target(
					compiler, function->op_array, calls, &target);
				break;
			}
		}
		if (!found_target) {
			return false;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			zend_function *internal = zend_native_compiler_resolve_internal_target(
				compiler, function->op_array, calls, &target, NULL);

			if (internal == NULL) {
				return false;
			}
			for (argument_index = 0; argument_index < site.arguments.count;
					argument_index++) {
				zend_mir_call_argument_ref argument;
				const zend_arg_info *argument_info;
				uint32_t type_mask;
				zend_op_array *callback;
				bool no_user_reentry;

				if (!calls->call_argument_at(calls->context,
						site.arguments.offset + argument_index, &argument)) {
					return false;
				}
				argument_info = zend_native_compiler_internal_argument_info(
					internal, argument.ordinal);
				type_mask = argument_info != NULL
					? ZEND_TYPE_FULL_MASK(argument_info->type) : 0;
				if ((type_mask & MAY_BE_CALLABLE) == 0) {
					continue;
				}
				callback = zend_native_compiler_resolve_callback_argument(
					compiler, function->op_array, &argument,
					(type_mask & MAY_BE_NULL) != 0, &no_user_reentry);
				if (no_user_reentry) {
					continue;
				}
				if (callback == NULL) {
					/* W10 callback APIs resolve callable values at runtime.  The
					 * request-local execute hook compiles an already loaded user
					 * target on first reentry; invalid callables remain the internal
					 * API's semantic error, not a native compile-time rejection. */
					continue;
				}
				if (callback == NULL
						|| zend_native_compiler_add_function(
							compiler, callback, NULL) == NULL) {
					zend_native_compiler_set_diagnostic(
						compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
						ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
						"internal callable argument is not a compile-time native target");
					return false;
				}
			}
			continue;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
			continue;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_METHOD_USER
				&& callee == function->op_array) {
			/* Constructorless NEW and open receiver-polymorphic methods use
			 * the caller entry cell as a runtime placeholder.  They have no
			 * statically distinct callee signature to merge here. */
			continue;
		}
		native_callee = zend_native_compiler_find_function(compiler, callee);
		if (native_callee == NULL || view == NULL) {
			return false;
		}
		for (argument_index = 0; argument_index < site.arguments.count;
				argument_index++) {
			zend_mir_call_argument_ref argument;
			zend_mir_value_fact_ref fact;
			uint32_t fact_index;
			bool found = false;

			if (!calls->call_argument_at(
					calls->context, site.arguments.offset + argument_index,
					&argument)) {
				return false;
			}
			/* W09 user calls transfer canonical source zvals. Their source
			 * ordinal may be named or variadic and is intentionally not a
			 * machine-scalar signature for the callee. Runtime argument
			 * placement and the callee's RECV projection provide the exact
			 * Zend semantics. */
			if (!zend_mir_id_is_valid(argument.value_id)) {
				continue;
			}
			for (fact_index = 0;
					fact_index < view->value_fact_count(view->context);
					fact_index++) {
				if (!view->value_fact_at(view->context, fact_index, &fact)) {
					return false;
				}
				if (fact.value_id == argument.value_id) {
					found = true;
					break;
				}
			}
			if (!found || !zend_native_compiler_merge_argument_type(
					compiler, native_callee, argument.ordinal,
					zend_native_compiler_argument_runtime_type(
						native_callee->op_array, argument.ordinal,
						fact.exact_type), NULL)) {
				return false;
			}
		}
	}
	return true;
}

static void zend_native_compiler_backend_failure(
	zend_native_compiler *compiler,
	zend_native_compile_diagnostic *product_diagnostic,
	zend_native_compile_phase phase,
	const zend_native_diagnostic *backend_diagnostic)
{
	zend_native_compiler_set_diagnostic(
		compiler, product_diagnostic, phase,
		backend_diagnostic != NULL
			? backend_diagnostic->code
			: ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
		backend_diagnostic != NULL && backend_diagnostic->message[0] != '\0'
			? backend_diagnostic->message
			: "native backend operation failed");
}

static void zend_native_compiler_fail_pending_component(
	zend_native_compiler *compiler)
{
	uint32_t index;

	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (function->publish_pending
				|| function->entry_cell.state
					== ZEND_NATIVE_ENTRY_COMPILING) {
			zend_native_entry_cell_fail(&function->entry_cell);
			function->state = ZEND_NATIVE_CODEUNIT_FAILED;
			function->publish_pending = false;
		}
	}
}

static bool zend_native_compiler_compile_native_component(
	zend_native_compiler *compiler,
	zend_native_compile_diagnostic *product_diagnostic)
{
	zend_native_diagnostic diagnostic;
	uint32_t index;

	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY) {
			continue;
		}
		if (function->state
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
			continue;
		}
		const zend_mir_call_view *calls =
			zend_mir_module_get_call_view(function->module);
		zend_native_call_binding *bindings = NULL;
		zend_native_internal_call_binding *internal_bindings = NULL;
		uint32_t target_count = calls != NULL
			? calls->call_target_count(calls->context) : 0;
		uint32_t binding_count = 0;
		uint32_t internal_binding_count = 0;
		uint32_t target_index;

		if (target_count != 0) {
			bindings = safe_emalloc(
				target_count, sizeof(*bindings), 0);
			internal_bindings = safe_emalloc(
				target_count, sizeof(*internal_bindings), 0);
			function->internal_call_cells = ecalloc(
				target_count, sizeof(*function->internal_call_cells));
		}
		for (target_index = 0; target_index < target_count; target_index++) {
			zend_mir_call_target_ref target;
			zend_op_array *callee;
			zend_native_compiled_function *native_callee;

			if (!calls->call_target_at(calls->context, target_index, &target)) {
				efree(bindings);
				efree(internal_bindings);
				zend_native_compiler_backend_failure(
					compiler, product_diagnostic,
					ZEND_NATIVE_COMPILE_PHASE_CODEGEN, NULL);
				zend_native_compiler_fail_pending_component(compiler);
				return false;
			}
			if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
				zend_function *internal;
				const zend_op *init_opline = NULL;
				zend_native_internal_receiver_kind receiver_kind =
					ZEND_NATIVE_INTERNAL_RECEIVER_NONE;
				zend_class_entry *called_scope = NULL;

				internal = zend_native_compiler_resolve_internal_target(
					compiler, function->op_array, calls, &target, &init_opline);
				if (internal == NULL || init_opline == NULL) {
					goto binding_failure;
				}
				if (init_opline->opcode == ZEND_INIT_METHOD_CALL) {
					if (init_opline->op1_type == IS_UNUSED) {
						receiver_kind =
							ZEND_NATIVE_INTERNAL_RECEIVER_CALLER_THIS;
					} else if (init_opline->op1_type == IS_CV
							|| init_opline->op1_type == IS_VAR
							|| init_opline->op1_type == IS_TMP_VAR) {
						receiver_kind =
							ZEND_NATIVE_INTERNAL_RECEIVER_SOURCE_OBJECT;
					} else {
						zend_native_compiler_set_diagnostic(
							compiler, product_diagnostic,
							ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
							ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
							"native internal method receiver is unsupported");
						goto binding_rejected;
					}
				} else if (init_opline->opcode == ZEND_INIT_STATIC_METHOD_CALL) {
					receiver_kind = ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE;
					called_scope = internal->common.scope;
				} else if (init_opline->opcode != ZEND_INIT_FCALL) {
					goto binding_failure;
				}
				if (zend_native_internal_call_cell_init(
						&function->internal_call_cells[internal_binding_count],
						internal, called_scope, receiver_kind) == FAILURE) {
					goto binding_failure;
				}
				internal_bindings[internal_binding_count].target_id = target.id;
				internal_bindings[internal_binding_count].call_cell =
					&function->internal_call_cells[internal_binding_count];
				internal_binding_count++;
				continue;
			}
			if (target.kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
				bindings[binding_count].target_id = target.id;
				bindings[binding_count].entry_cell = &function->entry_cell;
				binding_count++;
				continue;
			}
			callee = zend_native_compiler_resolve_native_target(
				compiler, function->op_array, calls, &target);
			native_callee = zend_native_compiler_find_function(compiler, callee);
			if (native_callee == NULL) {
				goto binding_failure;
			}
			bindings[binding_count].target_id = target.id;
			bindings[binding_count].entry_cell = &native_callee->entry_cell;
			binding_count++;
		}
		function->internal_call_cell_count = internal_binding_count;
		memset(&diagnostic, 0, sizeof(diagnostic));
		const zend_native_runtime_api *runtime = zend_native_runtime_get();
		zend_native_runtime_api injected_runtime;
		zend_native_runtime_helper injected_helpers[
			ZEND_NATIVE_HELPER_COUNT - 1];
		if (compiler->unavailable_runtime_helper != 0) {
			uint32_t helper_index;

			if (runtime->helper_count > ZEND_NATIVE_HELPER_COUNT - 1) {
				efree(bindings);
				efree(internal_bindings);
				zend_native_compiler_backend_failure(
					compiler, product_diagnostic,
					ZEND_NATIVE_COMPILE_PHASE_CODEGEN, NULL);
				zend_native_compiler_fail_pending_component(compiler);
				return false;
			}
			memcpy(injected_helpers, runtime->helpers,
				runtime->helper_count * sizeof(*injected_helpers));
			for (helper_index = 0;
					helper_index < runtime->helper_count; helper_index++) {
				if (injected_helpers[helper_index].id
						== compiler->unavailable_runtime_helper) {
					injected_helpers[helper_index].address = NULL;
					break;
				}
			}
			if (helper_index == runtime->helper_count) {
				efree(bindings);
				efree(internal_bindings);
				zend_native_compiler_backend_failure(
					compiler, product_diagnostic,
					ZEND_NATIVE_COMPILE_PHASE_CODEGEN, NULL);
				zend_native_compiler_fail_pending_component(compiler);
				return false;
			}
			injected_runtime = *runtime;
			injected_runtime.helpers = injected_helpers;
			runtime = &injected_runtime;
		}
		if (zend_tpde_compile_module_w08_with_runtime(
				compiler->target,
				zend_native_compiler_module_view(compiler, function->module),
				bindings, binding_count,
				internal_bindings, internal_binding_count,
				function->source_effects, function->source_effect_count,
				function->op_array->num_args,
				runtime,
				&function->image, &diagnostic) == FAILURE) {
			efree(bindings);
			efree(internal_bindings);
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_CODEGEN, &diagnostic);
			zend_native_compiler_fail_pending_component(compiler);
			return false;
		}
		function->publish_pending = true;
		efree(bindings);
		efree(internal_bindings);
		continue;

binding_failure:
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_CODEGEN, NULL);
binding_rejected:
		efree(bindings);
		efree(internal_bindings);
		zend_native_compiler_fail_pending_component(compiler);
		return false;
	}

	if (compiler->fault == ZEND_NATIVE_COMPILE_FAULT_MAPPING) {
		memset(&diagnostic, 0, sizeof(diagnostic));
		diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
		snprintf(diagnostic.message, sizeof(diagnostic.message),
			"injected native mapping failure");
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
		zend_native_compiler_fail_pending_component(compiler);
		return false;
	}
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (!function->publish_pending) {
			continue;
		}
		memset(&diagnostic, 0, sizeof(diagnostic));
		if (zend_native_publish_image(
				compiler->target, function->image, &function->code,
				&diagnostic) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
			zend_native_compiler_fail_pending_component(compiler);
			return false;
		}
		if (zend_native_code_is_writable(function->code)
				|| !zend_native_code_is_executable(function->code)) {
			memset(&diagnostic, 0, sizeof(diagnostic));
			diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
			snprintf(diagnostic.message, sizeof(diagnostic.message),
				"published code violates the W^X contract");
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
			zend_native_compiler_fail_pending_component(compiler);
			return false;
		}
	}
	if (compiler->fault == ZEND_NATIVE_COMPILE_FAULT_ENTRY_PUBLISH) {
		memset(&diagnostic, 0, sizeof(diagnostic));
		diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
		snprintf(diagnostic.message, sizeof(diagnostic.message),
			"injected entry-cell publication failure");
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
		zend_native_compiler_fail_pending_component(compiler);
		return false;
	}
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (!function->publish_pending) {
			continue;
		}
		if (zend_native_entry_cell_publish(
				&function->entry_cell, function->code) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, NULL);
			zend_native_compiler_fail_pending_component(compiler);
			return false;
		}
		function->state = ZEND_NATIVE_CODEUNIT_READY;
		function->publish_pending = false;
	}
	return true;
}

static bool zend_native_compiler_add_nested_functions(
	zend_native_compiler *compiler,
	zend_op_array *op_array,
	zend_native_compile_diagnostic *diagnostic,
	uint32_t depth)
{
	uint32_t index;

	if (op_array == NULL || depth > 64
			|| zend_native_compiler_add_function(
				compiler, op_array, diagnostic) == NULL) {
		return false;
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (!zend_native_compiler_add_nested_functions(
				compiler, op_array->dynamic_func_defs[index],
				diagnostic, depth + 1)) {
			return false;
		}
	}
	return true;
}

static bool zend_native_compiler_add_class_functions(
	zend_native_compiler *compiler,
	zend_class_entry *class_entry,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_function *function;
	zend_property_info *property_info;

	if (class_entry == NULL) {
		return true;
	}
	ZEND_HASH_MAP_FOREACH_PTR(&class_entry->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_compiler_add_nested_functions(
					compiler, &function->op_array, diagnostic, 0)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	if (class_entry->num_hooked_props == 0) {
		return true;
	}
	ZEND_HASH_MAP_FOREACH_PTR(
			&class_entry->properties_info, property_info) {
		uint32_t hook_index;

		if (property_info->ce != class_entry
				|| property_info->hooks == NULL) {
			continue;
		}
		for (hook_index = 0; hook_index < ZEND_PROPERTY_HOOK_COUNT;
				hook_index++) {
			function = property_info->hooks[hook_index];
			if (function != NULL && function->type == ZEND_USER_FUNCTION
					&& !zend_native_compiler_add_nested_functions(
						compiler, &function->op_array, diagnostic, 0)) {
				return false;
			}
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static bool zend_native_compiler_add_script_component(
	zend_native_compiler *compiler,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_function *function;
	zend_class_entry *class_entry;

	if (compiler->script == NULL
			|| !zend_native_compiler_add_nested_functions(
				compiler, &compiler->script->main_op_array,
				diagnostic, 0)) {
		return false;
	}
	ZEND_HASH_MAP_FOREACH_PTR(
			&compiler->script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_compiler_add_nested_functions(
					compiler, &function->op_array, diagnostic, 0)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_MAP_FOREACH_PTR(
			&compiler->script->class_table, class_entry) {
		if (!zend_native_compiler_add_class_functions(
				compiler, class_entry, diagnostic)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

zend_result zend_native_compiler_compile(
	zend_native_compiler *compiler,
	zend_op_array *root,
	const zend_mir_scalar_type_mask *supplied_argument_types,
	uint32_t supplied_argument_count,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiled_function *root_function;
	uint32_t index;

	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (compiler == NULL || root == NULL
			|| supplied_argument_count > root->num_args
			|| (supplied_argument_count != 0
				&& supplied_argument_types == NULL)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"invalid native compiler input");
		return FAILURE;
	}
	root_function = zend_native_compiler_find_function(compiler, root);
	if (root_function != NULL
			&& root_function->entry_cell.state
				== ZEND_NATIVE_ENTRY_READY) {
		return SUCCESS;
	}
	/*
	 * The initial Zend script is one compilation owner. Compile its complete
	 * function/class/closure component, not only the selected root. Dynamic
	 * include/eval owners are added separately through their exact root and
	 * recursively reachable child op_arrays.
	 */
	if (compiler->function_count == 0
			&& !zend_native_compiler_add_script_component(
				compiler, diagnostic)) {
		goto failure;
	}
	if (!zend_native_compiler_add_nested_functions(
			compiler, root, diagnostic, 0)) {
		goto failure;
	}
	root_function = zend_native_compiler_find_function(compiler, root);
	ZEND_ASSERT(root_function != NULL);
	for (index = 0; index < supplied_argument_count; index++) {
		if (!zend_native_compiler_merge_argument_type(
				compiler, root_function, index,
				zend_native_compiler_argument_runtime_type(
					root, index, supplied_argument_types[index]),
				diagnostic)) {
			goto failure;
		}
	}
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (function->state
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
			continue;
		}
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY) {
			continue;
		}
		if (function->module == NULL
				&& (!zend_native_compiler_build_ssa(
						compiler, function, diagnostic)
					|| !zend_native_compiler_lower_function(
						compiler, function, diagnostic))) {
			goto failure;
		}
		if (!zend_native_compiler_discover_native_callees(
				compiler, function)) {
			if (diagnostic != NULL) {
				*diagnostic = compiler->last_diagnostic;
			}
			goto failure;
		}
	}
	if (!zend_native_compiler_compile_native_component(
			compiler, diagnostic)) {
		goto failure;
	}
	root_function = zend_native_compiler_find_function(compiler, root);
	if (root_function == NULL
			|| (root_function->entry_cell.state != ZEND_NATIVE_ENTRY_READY
				&& root_function->state
					!= ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
			ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"native root entry was not published");
		goto failure;
	}
	return SUCCESS;

failure:
	zend_native_compiler_fail_pending_component(compiler);
	if (diagnostic != NULL && diagnostic->message[0] == '\0') {
		*diagnostic = compiler->last_diagnostic;
	}
	return FAILURE;
}

zend_result zend_native_compiler_compile_dynamic_component(
	zend_native_compiler *compiler,
	zend_op_array *root,
	uint32_t first_function_bucket,
	uint32_t first_class_bucket,
	zend_native_entry_cell **root_entry,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_function *function;
	zend_class_entry *class_entry;
	zend_native_compiled_function *compiled_root;

	if (root_entry != NULL) {
		*root_entry = NULL;
	}
	if (compiler == NULL || root == NULL
			|| first_function_bucket > EG(function_table)->nNumUsed
			|| first_class_bucket > EG(class_table)->nNumUsed) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"invalid dynamic codeunit symbol-table snapshot");
		return FAILURE;
	}
	if (!zend_native_compiler_add_nested_functions(
			compiler, root, diagnostic, 0)) {
		goto failure;
	}
	ZEND_HASH_FOREACH_PTR_FROM(
			EG(function_table), function, first_function_bucket) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_compiler_add_nested_functions(
					compiler, &function->op_array, diagnostic, 0)) {
			goto failure;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR_FROM(
			EG(class_table), class_entry, first_class_bucket) {
		if (class_entry != NULL && class_entry->type == ZEND_USER_CLASS
				&& !zend_native_compiler_add_class_functions(
					compiler, class_entry, diagnostic)) {
			goto failure;
		}
	} ZEND_HASH_FOREACH_END();
	if (zend_native_compiler_compile(
			compiler, root, NULL, 0, diagnostic) == FAILURE) {
		return FAILURE;
	}
	compiled_root = zend_native_compiler_find_function(compiler, root);
	if (compiled_root == NULL
			|| compiled_root->state != ZEND_NATIVE_CODEUNIT_READY
			|| compiled_root->entry_cell.state != ZEND_NATIVE_ENTRY_READY
			|| compiled_root->entry_cell.code == NULL) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
			ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"dynamic codeunit root entry was not published");
		goto failure;
	}
	if (root_entry != NULL) {
		*root_entry = &compiled_root->entry_cell;
	}
	return SUCCESS;

failure:
	zend_native_compiler_fail_pending_component(compiler);
	return FAILURE;
}

static zend_native_entry_cell *zend_native_compiler_resolve_reentry(
	void *context, zend_function *resolved)
{
	zend_native_compiler *compiler = context;
	zend_native_compiled_function *function;
	zend_op_array *source_op_array;
	zend_native_compile_diagnostic diagnostic;

	if (compiler == NULL || resolved == NULL
			|| !ZEND_USER_CODE(resolved->type)) {
		return NULL;
	}
	source_op_array = zend_native_compiler_canonical_reentry_op_array(
		compiler, &resolved->op_array);
	if (source_op_array == NULL) {
		/* The exact Zend function is the owner identity for a codeunit that
		 * became visible only after runtime declaration or autoload. */
		source_op_array = &resolved->op_array;
	}
	function = zend_native_compiler_find_function(
		compiler, source_op_array);
	if (function != NULL) {
		return function->entry_cell.state == ZEND_NATIVE_ENTRY_READY
			? &function->entry_cell : NULL;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	if (zend_native_compiler_compile(
			compiler, source_op_array, NULL, 0, &diagnostic) == FAILURE) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native codeunit compilation failed");
		}
		return NULL;
	}
	function = zend_native_compiler_find_function(
		compiler, source_op_array);
	return function != NULL
			&& function->entry_cell.state == ZEND_NATIVE_ENTRY_READY
		? &function->entry_cell : NULL;
}

zend_native_entry_cell *zend_native_compiler_lookup(
	const zend_native_compiler *compiler, const zend_function *function)
{
	zend_native_compiled_function *compiled;

	if (compiler == NULL || function == NULL
			|| !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	compiled = zend_native_compiler_find_function(
		compiler, &function->op_array);
	return compiled != NULL
			&& compiled->entry_cell.state == ZEND_NATIVE_ENTRY_READY
		? &compiled->entry_cell : NULL;
}

zend_native_codeunit_state zend_native_compiler_codeunit_state(
	const zend_native_compiler *compiler, const zend_function *function)
{
	zend_native_compiled_function *compiled;

	if (compiler == NULL || function == NULL
			|| !ZEND_USER_CODE(function->type)) {
		return ZEND_NATIVE_CODEUNIT_UNSEEN;
	}
	compiled = zend_native_compiler_find_function(
		compiler, &function->op_array);
	return compiled != NULL ? compiled->state : ZEND_NATIVE_CODEUNIT_UNSEEN;
}

uint32_t zend_native_compiler_codeunit_count(
	const zend_native_compiler *compiler, zend_native_codeunit_state state)
{
	uint32_t count = 0;
	uint32_t index;

	if (compiler == NULL || state == ZEND_NATIVE_CODEUNIT_UNSEEN) {
		return 0;
	}
	for (index = 0; index < compiler->function_count; index++) {
		if (compiler->functions[index]->state == state) {
			count++;
		}
	}
	return count;
}

static zend_result zend_native_compiler_enter(
	zend_native_compiler *compiler)
{
	uint32_t index;

	if (compiler->reentry_active || compiler->dynamic_compiler_active
			|| compiler->function_count == 0) {
		return FAILURE;
	}
	if (compiler->reentry_binding_capacity < compiler->function_count) {
		compiler->reentry_bindings = safe_erealloc(
			compiler->reentry_bindings, compiler->function_count,
			sizeof(*compiler->reentry_bindings), 0);
		compiler->reentry_binding_capacity = compiler->function_count;
	}
	uint32_t binding_count = 0;

	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (function->state
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
			continue;
		}
		if (function->entry_cell.state != ZEND_NATIVE_ENTRY_READY) {
			return FAILURE;
		}
		compiler->reentry_bindings[binding_count].function =
			(zend_function *) function->op_array;
		compiler->reentry_bindings[binding_count].entry_cell =
			&function->entry_cell;
		binding_count++;
	}
	memset(&compiler->reentry_scope, 0, sizeof(compiler->reentry_scope));
	if (zend_native_reentry_scope_enter_resolver(
			&compiler->reentry_scope, compiler->reentry_bindings,
			binding_count, zend_native_compiler_resolve_reentry,
			compiler) == FAILURE) {
		return FAILURE;
	}
	compiler->reentry_active = true;
	zend_native_dynamic_compiler_activate(&compiler->dynamic_compiler);
	compiler->dynamic_compiler_active = true;
	return SUCCESS;
}

static void zend_native_compiler_leave(zend_native_compiler *compiler)
{
	if (compiler->dynamic_compiler_active) {
		zend_native_dynamic_compiler_deactivate(
			&compiler->dynamic_compiler);
		compiler->dynamic_compiler_active = false;
	}
	if (compiler->reentry_active) {
		zend_native_reentry_scope_leave(&compiler->reentry_scope);
		compiler->reentry_active = false;
	}
}

zend_native_status zend_native_compiler_execute(
	zend_native_compiler *compiler,
	zend_function *function,
	HashTable *arguments,
	zval *result,
	zend_native_diagnostic *diagnostic)
{
	zend_native_compile_diagnostic compile_diagnostic;
	zend_mir_scalar_type_mask *argument_types = NULL;
	zend_native_entry_cell *entry_cell;
	zend_execute_data *previous;
	zend_execute_data *frame;
	zval receiver;
	void *object_or_called_scope = NULL;
	uint32_t argument_count =
		arguments != NULL ? zend_hash_num_elements(arguments) : 0;
	uint32_t typed_argument_count;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t index;
	zend_native_status status;

	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (compiler == NULL || function == NULL || result == NULL
			|| !ZEND_USER_CODE(function->type)
			|| (argument_count > function->common.num_args
				&& (function->common.fn_flags & ZEND_ACC_VARIADIC) == 0)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	typed_argument_count = argument_count < function->common.num_args
		? argument_count : function->common.num_args;
	if (typed_argument_count != 0) {
		argument_types = safe_emalloc(
			typed_argument_count, sizeof(*argument_types), 0);
	}
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		if (argument == NULL) {
			efree(argument_types);
			return ZEND_NATIVE_EXCEPTION;
		}
		if (index < typed_argument_count) {
			argument_types[index] =
				zend_native_compiler_scalar_type_from_zval(argument);
		}
	}
	memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
	if (zend_native_compiler_compile(
			compiler, &function->op_array, argument_types,
			typed_argument_count,
			&compile_diagnostic) == FAILURE) {
		efree(argument_types);
		if (diagnostic != NULL) {
			diagnostic->code = ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR;
			snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
				compile_diagnostic.message);
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	efree(argument_types);
	if (zend_native_compiler_codeunit_state(compiler, function)
			== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
		if (diagnostic != NULL) {
			diagnostic->code = ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT;
			snprintf(diagnostic->message, sizeof(diagnostic->message),
				"suspendable codeunit is reserved for native W12 activation");
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	entry_cell = zend_native_compiler_lookup(compiler, function);
	if (entry_cell == NULL || zend_native_compiler_enter(compiler) == FAILURE) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_UNDEF(&receiver);
	if (function->common.scope != NULL) {
		if ((function->common.fn_flags & ZEND_ACC_STATIC) != 0) {
			object_or_called_scope = function->common.scope;
		} else {
			if (object_init_ex(&receiver, function->common.scope)
					!= SUCCESS) {
				zend_native_compiler_leave(compiler);
				return ZEND_NATIVE_EXCEPTION;
			}
			object_or_called_scope = Z_OBJ(receiver);
			call_info |= ZEND_CALL_HAS_THIS;
		}
	}
	previous = EG(current_execute_data);
	frame = zend_vm_stack_push_call_frame(
		call_info, function, argument_count, object_or_called_scope);
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		ZVAL_COPY(ZEND_CALL_ARG(frame, index + 1), argument);
	}
	ZVAL_UNDEF(result);
	zend_init_func_execute_data(frame, &function->op_array, result);
	status = zend_native_execute_frame(entry_cell->code, frame, diagnostic);
	EG(current_execute_data) = previous;
	zend_vm_stack_free_call_frame(frame);
	if (!Z_ISUNDEF(receiver)) {
		zval_ptr_dtor(&receiver);
	}
	zend_native_compiler_leave(compiler);
	return status;
}

zend_native_compiler *zend_native_compiler_create(
	const zend_native_compiler_config *config,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiler *compiler;

	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (config == NULL || config->script == NULL
			|| (config->target != ZEND_NATIVE_TARGET_DARWIN_ARM64
				&& config->target != ZEND_NATIVE_TARGET_LINUX_AMD64)) {
		return NULL;
	}
	compiler = ecalloc(1, sizeof(*compiler));
	compiler->script = config->script;
	compiler->target = config->target;
	compiler->mir_chunk_size = config->mir_chunk_size != 0
		? config->mir_chunk_size
		: ZEND_NATIVE_COMPILER_DEFAULT_CHUNK_SIZE;
	compiler->frame_probe = config->frame_probe;
	compiler->frame_probe_context = config->frame_probe_context;
	compiler->observer = config->observer;
	compiler->observer_context = config->observer_context;
	compiler->fault = config->fault;
	compiler->unavailable_runtime_helper =
		config->unavailable_runtime_helper;
	compiler->abi_conformance_probe = config->abi_conformance_probe;
	zend_native_dynamic_compiler_init(&compiler->dynamic_compiler);
	zend_native_dynamic_compiler_bind_product(
		&compiler->dynamic_compiler, compiler);
	return compiler;
}

void zend_native_compiler_destroy(zend_native_compiler *compiler)
{
	uint32_t index;

	if (compiler == NULL) {
		return;
	}
	zend_native_compiler_leave(compiler);
	for (index = compiler->function_count; index-- > 0;) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (function == NULL) {
			continue;
		}
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY
				&& function->entry_cell.active_calls == 0) {
			(void) zend_native_entry_cell_reset(&function->entry_cell);
		} else if (function->entry_cell.state
				== ZEND_NATIVE_ENTRY_COMPILING) {
			zend_native_entry_cell_fail(&function->entry_cell);
		}
		if (function->code != NULL) {
			zend_native_code_destroy(function->code);
		}
		if (function->image != NULL) {
			zend_native_image_destroy(function->image);
		}
		if (function->module != NULL) {
			zend_mir_module_destroy(function->module);
		} else {
			zend_native_compiler_module_reset(&function->module_host);
		}
		if (function->ssa_arena != NULL) {
			zend_arena_destroy(function->ssa_arena);
		}
		efree(function->source_effects);
		efree(function->exception_handler_oplines);
		efree(function->internal_call_cells);
		efree(function->argument_types);
		efree(function->projected_ssa_var_info);
		efree(function->projected_ssa_vars);
		efree(function->projected_ssa_ops);
		efree(function->projected_opcodes);
		efree(function);
	}
	efree(compiler->reentry_bindings);
	efree(compiler->functions);
	zend_native_dynamic_compiler_destroy(&compiler->dynamic_compiler);
	efree(compiler);
}

uint32_t zend_native_compiler_function_count(
	const zend_native_compiler *compiler)
{
	return compiler != NULL ? compiler->function_count : 0;
}

const zend_native_code *zend_native_compiler_code_at(
	const zend_native_compiler *compiler, uint32_t index)
{
	return compiler != NULL && index < compiler->function_count
		? compiler->functions[index]->code : NULL;
}

const zend_native_image *zend_native_compiler_image_at(
	const zend_native_compiler *compiler, uint32_t index)
{
	return compiler != NULL && index < compiler->function_count
		? compiler->functions[index]->image : NULL;
}

const zend_native_image *zend_native_compiler_image_for(
	const zend_native_compiler *compiler, const zend_function *function)
{
	zend_native_compiled_function *compiled;

	if (compiler == NULL || function == NULL
			|| !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	compiled = zend_native_compiler_find_function(
		(zend_native_compiler *) compiler, &function->op_array);
	return compiled != NULL ? compiled->image : NULL;
}

uint32_t zend_native_compiler_active_call_count(
	const zend_native_compiler *compiler)
{
	uint32_t active_calls = 0;
	uint32_t index;

	if (compiler == NULL) {
		return 0;
	}
	for (index = 0; index < compiler->function_count; index++) {
		active_calls += compiler->functions[index]->entry_cell.active_calls;
	}
	return active_calls;
}

bool zend_native_compiler_all_code_is_wx(
	const zend_native_compiler *compiler)
{
	bool found_ready = false;
	uint32_t index;

	if (compiler == NULL || compiler->function_count == 0) {
		return false;
	}
	for (index = 0; index < compiler->function_count; index++) {
		const zend_native_compiled_function *function =
			compiler->functions[index];
		const zend_native_code *code;

		if (function->state
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
			continue;
		}
		code = function->code;

		if (code == NULL || zend_native_code_is_writable(code)
				|| !zend_native_code_is_executable(code)) {
			return false;
		}
		found_ready = true;
	}
	return found_ready;
}
