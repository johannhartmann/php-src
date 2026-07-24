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
#include "Zend/zend_hrtime.h"

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
	uint32_t registry_index;
	uint32_t component_id;
	struct _zend_native_compiled_function *next_component_member;
	zend_arena *ssa_arena;
	zend_ssa ssa;
	zend_native_source_effect *source_effects;
	uint32_t source_effect_count;
	uint32_t source_effect_capacity;
	bool source_effects_prepared;
	uint32_t *exception_handler_oplines;
	zend_native_compiler_module_host module_host;
	zend_mir_module *module;
	uint32_t *first_call_site_by_target;
	uint32_t *next_call_site_by_site;
	uint32_t call_target_count;
	uint32_t call_site_count;
	bool call_sites_indexed;
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
	HashTable functions_by_op_array;
	HashTable source_op_arrays_by_opcodes;
	zend_op_array **script_functions_by_declaration_id;
	uint32_t script_function_count;
	uint32_t function_count;
	uint32_t function_capacity;
	zend_native_compiled_function **component_heads;
	uint32_t component_head_capacity;
	zend_native_reentry_binding *reentry_bindings;
	uint32_t reentry_binding_capacity;
	zend_native_reentry_scope reentry_scope;
	bool reentry_active;
	zend_native_dynamic_compiler dynamic_compiler;
	bool dynamic_compiler_active;
	uint32_t published_component_count;
	zend_native_compiler_stats stats;
	bool failed;
	zend_native_compile_diagnostic last_diagnostic;
};

static uint64_t zend_native_compiler_dynamic_codeunit_count(
	uint32_t first_function_bucket, uint32_t first_class_bucket);

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

static zend_native_compiled_function *zend_native_compiler_find_function(
	const zend_native_compiler *compiler, const zend_op_array *op_array)
{
	zend_native_compiled_function *function;

	if (compiler == NULL || op_array == NULL) {
		return NULL;
	}
	function = zend_hash_index_find_ptr(
		&compiler->functions_by_op_array,
		(zend_ulong) (uintptr_t) op_array);
	return function != NULL && function->op_array == op_array
		? function : NULL;
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
	function->registry_index = compiler->function_count;
	function->state = (op_array->fn_flags & ZEND_ACC_GENERATOR) != 0
		? ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
		: ZEND_NATIVE_CODEUNIT_COMPILING;
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
		efree(function);
		return NULL;
	}
	if (zend_hash_index_add_ptr(
			&compiler->functions_by_op_array,
			(zend_ulong) (uintptr_t) op_array, function) == NULL) {
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_COMPILING) {
			zend_native_entry_cell_fail(&function->entry_cell);
		}
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native codeunit index insertion failed");
		efree(function);
		return NULL;
	}
	compiler->functions[compiler->function_count++] = function;
	return function;
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

static bool zend_native_compiler_prepare_source_effects(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function)
{
	const zend_op_array *source = function->op_array;
	uint32_t echo_count = 0;
	uint32_t index;

	if (source == NULL || function->ssa.ops == NULL
			|| (function->ssa.vars_count != 0
				&& (function->ssa.vars == NULL
					|| function->ssa.var_info == NULL))) {
		return false;
	}
	if (compiler->abi_conformance_probe) {
		for (index = 0; index < source->last; index++) {
			if (source->opcodes[index].opcode == ZEND_ECHO) {
				echo_count++;
			}
		}
	}
	if (source->last > UINT32_MAX - echo_count) {
		return false;
	}
	function->source_effect_capacity = source->last + echo_count;
	if (function->source_effect_capacity != 0) {
		function->source_effects = ecalloc(
			function->source_effect_capacity,
			sizeof(*function->source_effects));
	}
	for (index = 0; compiler->abi_conformance_probe
			&& index < source->last; index++) {
		const zend_op *original = &source->opcodes[index];
		const zend_ssa_op *ssa_op = &function->ssa.ops[index];

		if (original->opcode == ZEND_ECHO) {
			zend_mir_scalar_type_mask type = zend_native_compiler_operand_exact_type(
				source, &function->ssa, index, original->op1_type,
				&original->op1, ssa_op->op1_use);

			if (!zend_mir_scalar_type_is_exact(type)) {
				zend_native_compiler_set_diagnostic(
					compiler, NULL, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
					ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED,
					"native echo requires an exact scalar value");
				return false;
			}
			if (compiler->abi_conformance_probe) {
				zend_native_source_effect *effect =
					&function->source_effects[function->source_effect_count++];

				effect->source_position_id = index;
				effect->kind = ZEND_NATIVE_SOURCE_EFFECT_ABI_CONFORMANCE;
				effect->exact_type = type;
				effect->target_block_id = ZEND_MIR_ID_INVALID;
			}
		}
	}
	function->source_effects_prepared = true;
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
	zend_mir_block_id *handler_blocks;
	uint32_t instruction_count;
	uint32_t index;

	if (view == NULL || function->exception_handler_oplines == NULL) {
		return false;
	}
	handler_blocks = emalloc(
		(function->op_array->last == 0 ? 1 : function->op_array->last)
			* sizeof(*handler_blocks));
	for (index = 0; index < function->op_array->last; index++) {
		handler_blocks[index] = ZEND_MIR_ID_INVALID;
	}
	instruction_count = view->instruction_count(view->context);
	for (index = 0; index < instruction_count; index++) {
		zend_mir_instruction_record instruction;

		if (!view->instruction_at(view->context, index, &instruction)) {
			efree(handler_blocks);
			return false;
		}
		if (zend_mir_id_is_valid(instruction.source_position_id)
				&& instruction.source_position_id < function->op_array->last
				&& (instruction.opcode == ZEND_MIR_OPCODE_CATCH_ENTER
					|| instruction.opcode
						== ZEND_MIR_OPCODE_FINALLY_ENTER)) {
			zend_mir_block_id *handler_block =
				&handler_blocks[instruction.source_position_id];

			if (zend_mir_id_is_valid(*handler_block)
					&& *handler_block != instruction.block_id) {
				efree(handler_blocks);
				return false;
			}
			*handler_block = instruction.block_id;
		}
	}
	for (index = 0; index < instruction_count; index++) {
		zend_mir_instruction_record instruction;
		uint32_t handler_opline;
		zend_mir_block_id target_block;

		if (!view->instruction_at(view->context, index, &instruction)) {
			efree(handler_blocks);
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
		target_block = handler_opline < function->op_array->last
			? handler_blocks[handler_opline]
			: ZEND_MIR_ID_INVALID;
		if (!zend_mir_id_is_valid(target_block)
				|| function->source_effect_count
					>= function->source_effect_capacity) {
			efree(handler_blocks);
			return false;
		}
		zend_native_source_effect *effect =
			&function->source_effects[function->source_effect_count++];
		effect->source_position_id = instruction.source_position_id;
		effect->kind = ZEND_NATIVE_SOURCE_EFFECT_EXCEPTION_ROUTE;
		effect->exact_type = ZEND_MIR_SCALAR_TYPE_NONE;
		effect->target_block_id = target_block;
	}
	efree(handler_blocks);
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

	if (!function->source_effects_prepared
			&& !zend_native_compiler_prepare_source_effects(
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
		compiler->script, function->op_array,
		&function->ssa, &module_ops, &diagnostics);
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

static bool zend_native_compiler_index_call_sites(
	zend_native_compiled_function *function)
{
	const zend_mir_call_view *calls =
		zend_mir_module_get_call_view(function->module);
	uint32_t *first = NULL;
	uint32_t *last = NULL;
	uint32_t *next = NULL;
	uint32_t target_count;
	uint32_t site_count;
	uint32_t index;

	if (calls == NULL) {
		function->call_sites_indexed = true;
		return true;
	}
	if (function->call_sites_indexed) {
		return true;
	}
	if (calls->call_target_count == NULL || calls->call_target_at == NULL
			|| calls->call_site_count == NULL || calls->call_site_at == NULL) {
		return false;
	}
	target_count = calls->call_target_count(calls->context);
	site_count = calls->call_site_count(calls->context);
	if (target_count == 0) {
		function->call_sites_indexed = site_count == 0;
		return function->call_sites_indexed;
	}
	first = safe_emalloc(target_count, sizeof(*first), 0);
	last = safe_emalloc(target_count, sizeof(*last), 0);
	if (site_count != 0) {
		next = safe_emalloc(site_count, sizeof(*next), 0);
	}
	for (index = 0; index < target_count; index++) {
		zend_mir_call_target_ref target;

		first[index] = UINT32_MAX;
		last[index] = UINT32_MAX;
		if (!calls->call_target_at(calls->context, index, &target)
				|| target.id != index) {
			goto failure;
		}
	}
	for (index = 0; index < site_count; index++) {
		zend_mir_call_site_ref site;

		next[index] = UINT32_MAX;
		if (!calls->call_site_at(calls->context, index, &site)
				|| site.target_id >= target_count) {
			goto failure;
		}
		if (last[site.target_id] == UINT32_MAX) {
			first[site.target_id] = index;
		} else {
			next[last[site.target_id]] = index;
		}
		last[site.target_id] = index;
	}
	efree(last);
	function->first_call_site_by_target = first;
	function->next_call_site_by_site = next;
	function->call_target_count = target_count;
	function->call_site_count = site_count;
	function->call_sites_indexed = true;
	return true;

failure:
	efree(next);
	efree(last);
	efree(first);
	return false;
}

static zend_op_array *zend_native_compiler_resolve_native_target(
	zend_native_compiler *compiler,
	zend_native_compiled_function *caller_function,
	const zend_mir_call_view *calls,
	const zend_mir_call_target_ref *target)
{
	zend_function *function;
	const zend_ssa *caller_ssa = NULL;
	uint32_t index;
	zend_op_array *caller = caller_function != NULL
		? caller_function->op_array : NULL;

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
		if (compiler == NULL || caller_function == NULL || caller == NULL
				|| calls == NULL
				|| target->id >= caller_function->call_target_count
				|| calls->call_site_count == NULL
				|| calls->call_site_at == NULL) {
			return NULL;
		}
		caller_ssa = &caller_function->ssa;
		if (caller_ssa == NULL) {
			return NULL;
		}
		for (index = caller_function->first_call_site_by_target[target->id];
				index != UINT32_MAX;
				index = caller_function->next_call_site_by_site[index]) {
			zend_mir_call_site_ref site;

			if (index >= caller_function->call_site_count
					|| !calls->call_site_at(calls->context, index, &site)
					|| site.target_id != target->id) {
				return NULL;
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
	return target->op_array_id <= compiler->script_function_count
		? compiler->script_functions_by_declaration_id[target->op_array_id]
		: NULL;
}

static bool zend_native_compiler_target_is_direct_native(
	zend_native_compiler *compiler,
	zend_native_compiled_function *caller_function,
	const zend_mir_call_view *calls,
	const zend_mir_call_target_ref *target,
	const zend_op_array *callee)
{
	uint32_t index;
	bool found = false;

	if (target->kind == ZEND_MIR_CALL_TARGET_DIRECT_USER) {
		return true;
	}
	if (target->kind != ZEND_MIR_CALL_TARGET_METHOD_USER
			|| compiler == NULL || caller_function == NULL
			|| caller_function->op_array == NULL || calls == NULL
			|| calls->call_site_count == NULL || calls->call_site_at == NULL
			|| callee == NULL) {
		return false;
	}
	if (target->id >= caller_function->call_target_count) {
		return false;
	}
	for (index = caller_function->first_call_site_by_target[target->id];
			index != UINT32_MAX;
			index = caller_function->next_call_site_by_site[index]) {
		zend_mir_call_site_ref site;
		const zend_op *init;
		zend_function *resolved;
		bool inherit_called_scope;

		if (index >= caller_function->call_site_count
				|| !calls->call_site_at(calls->context, index, &site)
				|| site.target_id != target->id) {
			return false;
		}
		if (site.source_init_opline_index >= caller_function->op_array->last) {
			return false;
		}
		init = &caller_function->op_array->opcodes[
			site.source_init_opline_index];
		if (init->opcode == ZEND_INIT_METHOD_CALL) {
			if (init->op1_type != IS_UNUSED
					&& init->op1_type != IS_CV
					&& init->op1_type != IS_VAR
					&& init->op1_type != IS_TMP_VAR) {
				return false;
			}
		} else if (init->opcode != ZEND_INIT_STATIC_METHOD_CALL) {
			return false;
		}
		resolved = zend_mir_zend_source_resolve_monomorphic_user_method_call(
			compiler->script, caller_function->op_array,
			&caller_function->ssa, site.source_init_opline_index);
		if (resolved == NULL || resolved->type != ZEND_USER_FUNCTION
				|| &resolved->op_array != callee) {
			return false;
		}
		if (init->opcode == ZEND_INIT_METHOD_CALL) {
			if ((resolved->common.fn_flags & ZEND_ACC_STATIC) != 0) {
				return false;
			}
		} else if (!zend_mir_zend_source_direct_static_call_scope(
				compiler->script, caller_function->op_array,
				site.source_init_opline_index, resolved,
				&inherit_called_scope)) {
			return false;
		}
		found = true;
	}
	return found;
}

static zend_function *zend_native_compiler_resolve_internal_target(
	zend_native_compiler *compiler,
	zend_native_compiled_function *caller_function,
	const zend_mir_call_view *calls,
	const zend_mir_call_target_ref *target,
	const zend_op **init_opline_out)
{
	uint32_t index;
	zend_op_array *caller = caller_function != NULL
		? caller_function->op_array : NULL;

	if (compiler == NULL || caller_function == NULL || caller == NULL
			|| calls == NULL || target == NULL
			|| target->kind != ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
			|| calls->call_site_count == NULL || calls->call_site_at == NULL) {
		return NULL;
	}
	if (target->id >= caller_function->call_target_count) {
		return NULL;
	}
	for (index = caller_function->first_call_site_by_target[target->id];
			index != UINT32_MAX;
			index = caller_function->next_call_site_by_site[index]) {
		zend_mir_call_site_ref site;
		zend_function *function;
		const zend_op *init;
		const zend_ssa *ssa = &caller_function->ssa;

		if (index >= caller_function->call_site_count
				|| !calls->call_site_at(calls->context, index, &site)
				|| site.target_id != target->id) {
			return NULL;
		}
		if (site.source_init_opline_index >= caller->last) {
			return NULL;
		}
		init = &caller->opcodes[site.source_init_opline_index];
		function = zend_mir_zend_source_resolve_internal_call(
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

static bool zend_native_compiler_index_source_op_array(
	zend_native_compiler *compiler, zend_op_array *op_array, uint32_t depth)
{
	uint32_t index;

	if (op_array == NULL || depth > 64) {
		return false;
	}
	if (op_array->opcodes != NULL) {
		zend_ulong key = (zend_ulong) (uintptr_t) op_array->opcodes;
		zend_op_array *indexed = zend_hash_index_find_ptr(
			&compiler->source_op_arrays_by_opcodes, key);

		if (indexed == NULL
				&& zend_hash_index_add_ptr(
					&compiler->source_op_arrays_by_opcodes,
					key, op_array) == NULL) {
			return false;
		}
	}
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (!zend_native_compiler_index_source_op_array(
				compiler, op_array->dynamic_func_defs[index], depth + 1)) {
			return false;
		}
	}
	return true;
}

static bool zend_native_compiler_index_source_class(
	zend_native_compiler *compiler, zend_class_entry *class_entry)
{
	zend_function *function;
	zend_property_info *property_info;
	uint32_t hook_index;

	if (class_entry == NULL) {
		return true;
	}
	ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_compiler_index_source_op_array(
					compiler, &function->op_array, 0)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	if (class_entry->num_hooked_props == 0) {
		return true;
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
			if (function != NULL && function->type == ZEND_USER_FUNCTION
					&& !zend_native_compiler_index_source_op_array(
						compiler, &function->op_array, 0)) {
				return false;
			}
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static bool zend_native_compiler_index_source_op_arrays(
	zend_native_compiler *compiler)
{
	zend_function *function;
	zend_class_entry *class_entry;

	if (!zend_native_compiler_index_source_op_array(
			compiler, &compiler->script->main_op_array, 0)) {
		return false;
	}
	ZEND_HASH_FOREACH_PTR(&compiler->script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& !zend_native_compiler_index_source_op_array(
					compiler, &function->op_array, 0)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&compiler->script->class_table, class_entry) {
		if (!zend_native_compiler_index_source_class(
				compiler, class_entry)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

static zend_op_array *zend_native_compiler_canonical_reentry_op_array(
	zend_native_compiler *compiler, const zend_op_array *resolved)
{
	zend_op_array *source;

	if (compiler == NULL || resolved == NULL || resolved->opcodes == NULL) {
		return NULL;
	}
	source = zend_hash_index_find_ptr(
		&compiler->source_op_arrays_by_opcodes,
		(zend_ulong) (uintptr_t) resolved->opcodes);
	return source != NULL && source->last == resolved->last
		? source : NULL;
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
					compiler, function, calls, &target, NULL) == NULL) {
				return false;
			}
			continue;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
			continue;
		}
		callee = zend_native_compiler_resolve_native_target(
			compiler, function, calls, &target);
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
		uint32_t argument_index;

		if (!calls->call_site_at(calls->context, index, &site)) {
			return false;
		}
		if (site.target_id >= target_count
				|| !calls->call_target_at(
					calls->context, site.target_id, &target)
				|| target.id != site.target_id) {
			return false;
		}
		if (target.kind != ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			callee = zend_native_compiler_resolve_native_target(
				compiler, function, calls, &target);
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			zend_function *internal = zend_native_compiler_resolve_internal_target(
				compiler, function, calls, &target, NULL);

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
			continue;
		}
		/*
		 * A baseline entry is a property of the function body and its static
		 * scope, never of the first observed call-site types.  User arguments
		 * remain canonical zvals unless the callee's own declared type and SSA
		 * prove an exact scalar after zend_native_frame_prepare().
		 */
	}
	return true;
}

typedef struct _zend_native_scc_state {
	zend_native_compiler *compiler;
	uint32_t *indices;
	uint32_t *lowlinks;
	uint32_t *stack;
	bool *on_stack;
	uint32_t next_index;
	uint32_t stack_count;
	uint32_t component_count;
} zend_native_scc_state;

static bool zend_native_compiler_visit_scc(
	zend_native_scc_state *state, uint32_t function_index)
{
	zend_native_compiled_function *function =
		state->compiler->functions[function_index];
	const zend_mir_call_view *calls =
		zend_mir_module_get_call_view(function->module);
	uint32_t target_count = calls != NULL
		? calls->call_target_count(calls->context) : 0;
	uint32_t target_index;

	state->indices[function_index] = state->next_index;
	state->lowlinks[function_index] = state->next_index++;
	state->stack[state->stack_count++] = function_index;
	state->on_stack[function_index] = true;
	for (target_index = 0; target_index < target_count; target_index++) {
		zend_mir_call_target_ref target;
		zend_op_array *callee;
		zend_native_compiled_function *native_callee;
		uint32_t callee_index;

		if (!calls->call_target_at(
				calls->context, target_index, &target)) {
			return false;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL
				|| target.kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
			continue;
		}
		callee = zend_native_compiler_resolve_native_target(
			state->compiler, function, calls, &target);
		native_callee = zend_native_compiler_find_function(
			state->compiler, callee);
		if (native_callee == NULL) {
			return false;
		}
		if (native_callee->state != ZEND_NATIVE_CODEUNIT_COMPILING) {
			continue;
		}
		callee_index = native_callee->registry_index;
		if (state->indices[callee_index] == UINT32_MAX) {
			if (!zend_native_compiler_visit_scc(state, callee_index)) {
				return false;
			}
			if (state->lowlinks[callee_index]
					< state->lowlinks[function_index]) {
				state->lowlinks[function_index] =
					state->lowlinks[callee_index];
			}
		} else if (state->on_stack[callee_index]
				&& state->indices[callee_index]
					< state->lowlinks[function_index]) {
			state->lowlinks[function_index] =
				state->indices[callee_index];
		}
	}
	if (state->lowlinks[function_index] == state->indices[function_index]) {
		uint32_t member_index;
		uint32_t component_id = ++state->component_count;

		do {
			if (state->stack_count == 0) {
				return false;
			}
			member_index = state->stack[--state->stack_count];
			state->on_stack[member_index] = false;
			zend_native_compiled_function *member =
				state->compiler->functions[member_index];

			member->component_id = component_id;
			member->next_component_member =
				state->compiler->component_heads[component_id];
			state->compiler->component_heads[component_id] = member;
		} while (member_index != function_index);
	}
	return true;
}

static bool zend_native_compiler_assign_sccs(
	zend_native_compiler *compiler, uint32_t *component_count)
{
	zend_native_scc_state state;
	uint32_t index;
	uint32_t required_heads;

	memset(&state, 0, sizeof(state));
	state.compiler = compiler;
	if (compiler->function_count == UINT32_MAX) {
		return false;
	}
	required_heads = compiler->function_count + 1;
	if (compiler->component_head_capacity < required_heads) {
		compiler->component_heads = safe_erealloc(
			compiler->component_heads, required_heads,
			sizeof(*compiler->component_heads), 0);
		compiler->component_head_capacity = required_heads;
	}
	memset(compiler->component_heads, 0,
		required_heads * sizeof(*compiler->component_heads));
	state.indices = safe_emalloc(
		compiler->function_count, sizeof(*state.indices), 0);
	state.lowlinks = safe_emalloc(
		compiler->function_count, sizeof(*state.lowlinks), 0);
	state.stack = safe_emalloc(
		compiler->function_count, sizeof(*state.stack), 0);
	state.on_stack = ecalloc(
		compiler->function_count, sizeof(*state.on_stack));
	for (index = 0; index < compiler->function_count; index++) {
		state.indices[index] = UINT32_MAX;
		state.lowlinks[index] = UINT32_MAX;
		compiler->functions[index]->component_id = 0;
		compiler->functions[index]->next_component_member = NULL;
	}
	for (index = 0; index < compiler->function_count; index++) {
		if (compiler->functions[index]->state
					!= ZEND_NATIVE_CODEUNIT_COMPILING
				|| state.indices[index] != UINT32_MAX) {
			continue;
		}
		if (!zend_native_compiler_visit_scc(&state, index)) {
			efree(state.on_stack);
			efree(state.stack);
			efree(state.lowlinks);
			efree(state.indices);
			return false;
		}
	}
	*component_count = state.component_count;
	efree(state.on_stack);
	efree(state.stack);
	efree(state.lowlinks);
	efree(state.indices);
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
	zend_native_compiler *compiler, uint32_t component_id)
{
	zend_native_compiled_function *function;

	if (component_id != 0
			&& component_id < compiler->component_head_capacity) {
		function = compiler->component_heads[component_id];
		while (function != NULL) {
			zend_native_compiled_function *next =
				function->next_component_member;

			if (function->publish_pending
					|| function->entry_cell.state
						== ZEND_NATIVE_ENTRY_COMPILING) {
				zend_native_entry_cell_fail(&function->entry_cell);
				function->state = ZEND_NATIVE_CODEUNIT_FAILED;
				function->publish_pending = false;
			}
			function->component_id = 0;
			function->next_component_member = NULL;
			function = next;
		}
		compiler->component_heads[component_id] = NULL;
		return;
	}
	for (uint32_t index = 0; index < compiler->function_count; index++) {
		function = compiler->functions[index];
		if (function->publish_pending
				|| function->entry_cell.state
					== ZEND_NATIVE_ENTRY_COMPILING) {
			zend_native_entry_cell_fail(&function->entry_cell);
			function->state = ZEND_NATIVE_CODEUNIT_FAILED;
			function->publish_pending = false;
		}
		function->component_id = 0;
		function->next_component_member = NULL;
	}
}

static bool zend_native_compiler_compile_native_component(
	zend_native_compiler *compiler,
	uint32_t component_id,
	zend_native_compile_diagnostic *product_diagnostic)
{
	zend_native_diagnostic diagnostic;
	zend_native_compiled_function *function;

	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY) {
			continue;
		}
		if (function->state
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
				|| function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
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
				zend_native_compiler_fail_pending_component(
					compiler, component_id);
				return false;
			}
			if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
				zend_function *internal;
				const zend_op *init_opline = NULL;
				zend_native_internal_receiver_kind receiver_kind =
					ZEND_NATIVE_INTERNAL_RECEIVER_NONE;
				zend_class_entry *called_scope = NULL;

				internal = zend_native_compiler_resolve_internal_target(
					compiler, function, calls, &target, &init_opline);
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
				bindings[binding_count].direct_native = false;
				binding_count++;
				continue;
			}
			callee = zend_native_compiler_resolve_native_target(
				compiler, function, calls, &target);
			native_callee = zend_native_compiler_find_function(compiler, callee);
			if (native_callee == NULL
					|| native_callee->state
						== ZEND_NATIVE_CODEUNIT_FAILED) {
				goto binding_failure;
			}
			bindings[binding_count].target_id = target.id;
			bindings[binding_count].entry_cell = &native_callee->entry_cell;
			bindings[binding_count].direct_native =
				zend_native_compiler_target_is_direct_native(
					compiler, function, calls, &target, callee);
			binding_count++;
		}
		function->internal_call_cell_count = internal_binding_count;
		memset(&diagnostic, 0, sizeof(diagnostic));
		const zend_native_runtime_api *runtime = zend_native_runtime_get();
		zend_native_runtime_api injected_runtime;
		zend_hrtime_t phase_started;
		zend_result compile_result;
		zend_native_image_metrics image_metrics;
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
				zend_native_compiler_fail_pending_component(
					compiler, component_id);
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
				zend_native_compiler_fail_pending_component(
					compiler, component_id);
				return false;
			}
			injected_runtime = *runtime;
			injected_runtime.helpers = injected_helpers;
			runtime = &injected_runtime;
		}
		phase_started = zend_hrtime();
		compile_result = zend_tpde_compile_module_w08_with_runtime(
				compiler->target,
				zend_native_compiler_module_view(compiler, function->module),
				bindings, binding_count,
				internal_bindings, internal_binding_count,
				function->source_effects, function->source_effect_count,
				function->op_array->num_args,
				function->op_array,
				runtime,
				&function->image, &diagnostic);
		compiler->stats.codegen_ns += zend_hrtime() - phase_started;
		if (compile_result == FAILURE) {
			efree(bindings);
			efree(internal_bindings);
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_CODEGEN, &diagnostic);
			zend_native_compiler_fail_pending_component(
				compiler, component_id);
			return false;
		}
		memset(&image_metrics, 0, sizeof(image_metrics));
		zend_native_image_get_metrics(function->image, &image_metrics);
		compiler->stats.native_code_bytes +=
			zend_native_image_size(function->image);
		compiler->stats.runtime_helper_sites +=
			image_metrics.runtime_helper_sites;
		compiler->stats.source_opline_decode_sites +=
			image_metrics.source_opline_decode_sites;
		compiler->stats.guard_sites += image_metrics.guard_sites;
		compiler->stats.slow_path_sites += image_metrics.slow_path_sites;
		compiler->stats.direct_call_sites += image_metrics.direct_call_sites;
		compiler->stats.direct_call_frame_bytes +=
			image_metrics.direct_call_frame_bytes;
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
		zend_native_compiler_fail_pending_component(
			compiler, component_id);
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
		zend_native_compiler_fail_pending_component(
			compiler, component_id);
		return false;
	}
	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (!function->publish_pending) {
			continue;
		}
		memset(&diagnostic, 0, sizeof(diagnostic));
		zend_hrtime_t publish_started = zend_hrtime();
		zend_result publish_result = zend_native_publish_image(
				compiler->target, function->image, &function->code,
				&diagnostic);
		compiler->stats.publish_ns += zend_hrtime() - publish_started;
		if (publish_result == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
			zend_native_compiler_fail_pending_component(
				compiler, component_id);
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
			zend_native_compiler_fail_pending_component(
				compiler, component_id);
			return false;
		}
	}
	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (!function->publish_pending) {
			continue;
		}
		if (zend_native_entry_cell_publish(
				&function->entry_cell, function->code) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, NULL);
			zend_native_compiler_fail_pending_component(
				compiler, component_id);
			return false;
		}
		if (compiler->fault == ZEND_NATIVE_COMPILE_FAULT_ENTRY_PUBLISH) {
			memset(&diagnostic, 0, sizeof(diagnostic));
			diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
			snprintf(diagnostic.message, sizeof(diagnostic.message),
				"injected entry-cell publication failure");
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
			zend_native_compiler_fail_pending_component(
				compiler, component_id);
			return false;
		}
	}
	/*
	 * Keep every member publish_pending until the complete entry-cell batch has
	 * been published. zend_native_compiler_fail_pending_component() can then
	 * roll an already-published READY cell back to FAILED if any later member
	 * rejects publication. No entry from this component becomes committed
	 * independently of its siblings.
	 */
	function = compiler->component_heads[component_id];
	while (function != NULL) {
		zend_native_compiled_function *next =
			function->next_component_member;

		if (function->publish_pending) {
			function->state = ZEND_NATIVE_CODEUNIT_READY;
			function->publish_pending = false;
		}
		function->component_id = 0;
		function->next_component_member = NULL;
		function = next;
	}
	compiler->component_heads[component_id] = NULL;
	compiler->published_component_count++;
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
	uint32_t component_count;
	uint32_t component_id;
	uint32_t index;

	(void) supplied_argument_types;
	(void) supplied_argument_count;
	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (compiler == NULL || root == NULL) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"invalid native compiler input");
		return FAILURE;
	}
	root_function = zend_native_compiler_find_function(compiler, root);
	if (root_function != NULL
			&& root_function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native codeunit previously failed compilation");
		return FAILURE;
	}
	if (root_function != NULL
			&& root_function->entry_cell.state
				== ZEND_NATIVE_ENTRY_READY) {
		return SUCCESS;
	}
	/*
	 * A request starts with exactly the selected root. Static user-call
	 * discovery below grows this component with transitively reachable
	 * codeunits; unrelated script functions, methods and closures remain
	 * absent from the native registry until reentry selects them.
	 */
	if (zend_native_compiler_add_function(
			compiler, root, diagnostic) == NULL) {
		goto failure;
	}
	root_function = zend_native_compiler_find_function(compiler, root);
	ZEND_ASSERT(root_function != NULL);
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		zend_hrtime_t phase_started;

		if (function->state
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
				|| function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
			continue;
		}
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY) {
			continue;
		}
		if (function->module == NULL) {
			bool phase_result;

			phase_started = zend_hrtime();
			phase_result = zend_native_compiler_build_ssa(
				compiler, function, diagnostic);
			compiler->stats.ssa_ns += zend_hrtime() - phase_started;
			if (!phase_result) {
				goto failure;
			}
			phase_started = zend_hrtime();
			phase_result = zend_native_compiler_lower_function(
				compiler, function, diagnostic);
			compiler->stats.lowering_ns += zend_hrtime() - phase_started;
			if (!phase_result) {
				goto failure;
			}
		}
		if (!function->call_sites_indexed
				&& !zend_native_compiler_index_call_sites(function)) {
			zend_native_compiler_set_diagnostic(
				compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
				ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
				"native call-site index cannot be constructed");
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
	if (!zend_native_compiler_assign_sccs(
			compiler, &component_count)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"native call graph cannot be partitioned into SCCs");
		goto failure;
	}
	for (component_id = 1; component_id <= component_count;
			component_id++) {
		if (!zend_native_compiler_compile_native_component(
				compiler, component_id, diagnostic)) {
			goto failure;
		}
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
	zend_native_compiler_fail_pending_component(compiler, 0);
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
	zend_native_compiled_function *compiled_root;
	uint64_t registered_codeunits;

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
	registered_codeunits = zend_native_compiler_dynamic_codeunit_count(
		first_function_bucket, first_class_bucket);
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
	compiler->stats.registered_codeunits += registered_codeunits;
	return SUCCESS;

failure:
	zend_native_compiler_fail_pending_component(compiler, 0);
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
		if (function->state == ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
			zend_throw_error(NULL,
				"Suspendable codeunit is reserved for native W12 activation");
			return NULL;
		}
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
	if (function != NULL
			&& function->state == ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED) {
		zend_throw_error(NULL,
			"Suspendable codeunit is reserved for native W12 activation");
		return NULL;
	}
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

uint32_t zend_native_compiler_published_component_count(
	const zend_native_compiler *compiler)
{
	return compiler != NULL ? compiler->published_component_count : 0;
}

void zend_native_compiler_get_stats(
	const zend_native_compiler *compiler, zend_native_compiler_stats *stats)
{
	if (stats == NULL) {
		return;
	}
	memset(stats, 0, sizeof(*stats));
	if (compiler == NULL) {
		return;
	}
	*stats = compiler->stats;
	stats->native_codeunits = compiler->function_count;
	stats->ready_codeunits = zend_native_compiler_codeunit_count(
		compiler, ZEND_NATIVE_CODEUNIT_READY);
	stats->published_components = compiler->published_component_count;
}

static uint64_t zend_native_compiler_dynamic_codeunit_count(
	uint32_t first_function_bucket, uint32_t first_class_bucket)
{
	uint64_t count = 1;
	uint32_t index;

	for (index = first_function_bucket;
			index < EG(function_table)->nNumUsed; index++) {
		zval *value = &EG(function_table)->arData[index].val;
		zend_function *function;

		if (Z_TYPE_P(value) == IS_UNDEF) {
			continue;
		}
		function = Z_PTR_P(value);
		if (function != NULL && function->type == ZEND_USER_FUNCTION) {
			count++;
		}
	}
	for (index = first_class_bucket;
			index < EG(class_table)->nNumUsed; index++) {
		zval *value = &EG(class_table)->arData[index].val;
		zend_class_entry *class_entry;
		zend_function *function;

		if (Z_TYPE_P(value) == IS_UNDEF) {
			continue;
		}
		class_entry = Z_PTR_P(value);
		if (class_entry == NULL) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
			if (function != NULL && function->type == ZEND_USER_FUNCTION) {
				count++;
			}
		} ZEND_HASH_FOREACH_END();
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
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
				|| function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
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
	zend_native_entry_cell *entry_cell;
	zend_execute_data *previous;
	zend_execute_data *frame;
	zval receiver;
	void *object_or_called_scope = NULL;
	uint32_t argument_count =
		arguments != NULL ? zend_hash_num_elements(arguments) : 0;
	uint32_t call_info = ZEND_CALL_NESTED_FUNCTION;
	uint32_t index;
	zend_native_status status;
	zend_hrtime_t phase_started;
	uint64_t elapsed;

	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (compiler == NULL || function == NULL || result == NULL
			|| !ZEND_USER_CODE(function->type)
			|| (argument_count > function->common.num_args
				&& (function->common.fn_flags & ZEND_ACC_VARIADIC) == 0)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		if (argument == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
	phase_started = zend_hrtime();
	if (zend_native_compiler_compile(
			compiler, &function->op_array, NULL, 0,
			&compile_diagnostic) == FAILURE) {
		compiler->stats.compile_ns += zend_hrtime() - phase_started;
		if (diagnostic != NULL) {
			diagnostic->code = ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR;
			snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
				compile_diagnostic.message);
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	compiler->stats.compile_ns += zend_hrtime() - phase_started;
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
	phase_started = zend_hrtime();
	status = zend_native_execute_frame(entry_cell->code, frame, diagnostic);
	elapsed = zend_hrtime() - phase_started;
	compiler->stats.execute_ns += elapsed;
	compiler->stats.last_execute_ns = elapsed;
	if (compiler->stats.executions == 0) {
		compiler->stats.first_execute_ns = elapsed;
	}
	compiler->stats.executions++;
	EG(current_execute_data) = previous;
	zend_vm_stack_free_call_frame(frame);
	if (!Z_ISUNDEF(receiver)) {
		zval_ptr_dtor(&receiver);
	}
	zend_native_compiler_leave(compiler);
	return status;
}

static bool zend_native_compiler_index_script_functions(
	zend_native_compiler *compiler)
{
	zend_function *function;
	uint32_t count = 0;
	uint32_t declaration_id = 1;

	ZEND_HASH_FOREACH_PTR(&compiler->script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION) {
			if (count == ZEND_MIR_ID_MAX) {
				return false;
			}
			count++;
		}
	} ZEND_HASH_FOREACH_END();
	if (count == 0) {
		return true;
	}
	compiler->script_functions_by_declaration_id =
		safe_emalloc(count + 1,
			sizeof(*compiler->script_functions_by_declaration_id), 0);
	compiler->script_functions_by_declaration_id[0] = NULL;
	ZEND_HASH_FOREACH_PTR(&compiler->script->function_table, function) {
		if (function == NULL || function->type != ZEND_USER_FUNCTION) {
			continue;
		}
		compiler->script_functions_by_declaration_id[declaration_id++] =
			&function->op_array;
	} ZEND_HASH_FOREACH_END();
	compiler->script_function_count = count;
	return true;
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
	zend_hash_init(
		&compiler->source_op_arrays_by_opcodes, 32, NULL, NULL, false);
	if (!zend_native_compiler_index_source_op_arrays(compiler)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native source codeunit index cannot be constructed");
		zend_hash_destroy(&compiler->source_op_arrays_by_opcodes);
		efree(compiler);
		return NULL;
	}
	compiler->stats.registered_codeunits =
		zend_hash_num_elements(&compiler->source_op_arrays_by_opcodes);
	if (!zend_native_compiler_index_script_functions(compiler)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native script function index cannot be constructed");
		zend_hash_destroy(&compiler->source_op_arrays_by_opcodes);
		efree(compiler);
		return NULL;
	}
	zend_hash_init(
		&compiler->functions_by_op_array, 8, NULL, NULL, false);
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
		efree(function->first_call_site_by_target);
		efree(function->next_call_site_by_site);
		efree(function->internal_call_cells);
		efree(function);
	}
	efree(compiler->reentry_bindings);
	efree(compiler->component_heads);
	efree(compiler->script_functions_by_declaration_id);
	zend_hash_destroy(&compiler->functions_by_op_array);
	zend_hash_destroy(&compiler->source_op_arrays_by_opcodes);
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
				== ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED
				|| function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
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
