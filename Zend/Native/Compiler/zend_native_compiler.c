#include "Zend/Native/Compiler/zend_native_compiler.h"
#include "Zend/Native/Compiler/zend_native_compiler_internal.h"

#include "Zend/zend_closures.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_observer.h"
#include "Zend/Optimizer/zend_func_info.h"
#include "Zend/Optimizer/zend_inference.h"
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
#include <stdio.h>
#include <string.h>

#define ZEND_NATIVE_COMPILER_ARENA_SIZE (64 * 1024)
#define ZEND_NATIVE_COMPILER_DEFAULT_CHUNK_SIZE 64

typedef struct _zend_native_compiler_module_allocation {
	struct _zend_native_compiler_module_allocation *next;
} zend_native_compiler_module_allocation;

typedef struct _zend_native_compiler_module_host {
	zend_native_compiler_module_allocation *allocations;
	uint32_t successful_allocations;
	bool fail_allocation;
} zend_native_compiler_module_host;

typedef struct _zend_native_runtime_source {
	zend_op_array op_array;
	struct _zend_native_runtime_source *next;
} zend_native_runtime_source;

typedef struct _zend_native_compiled_function {
	zend_op_array *op_array;
	zend_native_op_array_identity op_array_identity;
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
	uint32_t image_owner_index;
	uint32_t image_component_index;
	zend_native_image *image;
	zend_native_code *code;
	zend_native_entry_cell entry_cell;
	zend_native_internal_call_cell *internal_call_cells;
	uint32_t internal_call_cell_count;
	zend_native_codeunit_state state;
	bool publish_pending;
	bool leaf_scalar_frame_known;
	bool leaf_scalar_frame;
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
	bool source_probe;
	bool defer_publication;
	bool direct_reentry;
	zend_native_external_reentry_resolver_t external_reentry_resolver;
	void *external_reentry_context;
	bool persistent;
	zend_native_compiled_function **functions;
	zend_op_array **publication_log;
	HashTable functions_by_op_array;
	HashTable source_op_arrays_by_opcodes;
	zend_native_runtime_source *runtime_sources;
	zend_op_array **script_functions_by_declaration_id;
	uint32_t script_function_count;
	uint32_t function_count;
	uint32_t function_capacity;
	uint32_t publication_count;
	uint32_t publication_capacity;
	zend_native_compiled_function **component_heads;
	uint32_t component_head_capacity;
	uint32_t published_component_count;
	zend_native_compiler_stats stats;
	bool failed;
	bool transients_released;
	zend_native_compile_diagnostic last_diagnostic;
#ifdef ZTS
	MUTEX_T mutation_mutex;
#endif
};

typedef struct _zend_native_compiler_session {
	zend_native_compiler *compiler;
	zend_native_reentry_binding *reentry_bindings;
	uint32_t reentry_binding_capacity;
	uint32_t reentry_binding_count;
	uint32_t reentry_binding_function_count;
	zend_native_reentry_scope reentry_scope;
	zend_native_dynamic_compiler dynamic_compiler;
	zend_script request_script;
	zend_native_compiler *request_compiler;
	uint64_t execute_ns;
	uint64_t first_execute_ns;
	uint64_t last_execute_ns;
	uint32_t executions;
	bool reentry_active;
	bool dynamic_compiler_active;
} zend_native_compiler_session;

ZEND_TLS HashTable zend_native_compiler_sessions;
ZEND_TLS bool zend_native_compiler_sessions_active;

static void zend_native_compiler_mutation_lock(
	const zend_native_compiler *compiler)
{
#ifdef ZTS
	if (compiler != NULL && compiler->mutation_mutex != NULL) {
		tsrm_mutex_lock(compiler->mutation_mutex);
	}
#else
	(void) compiler;
#endif
}

static void zend_native_compiler_mutation_unlock(
	const zend_native_compiler *compiler)
{
#ifdef ZTS
	if (compiler != NULL && compiler->mutation_mutex != NULL) {
		tsrm_mutex_unlock(compiler->mutation_mutex);
	}
#else
	(void) compiler;
#endif
}

static void zend_native_compiler_session_destroy(
	zend_native_compiler_session *session)
{
	if (session == NULL) {
		return;
	}
	ZEND_ASSERT(!session->reentry_active);
	ZEND_ASSERT(!session->dynamic_compiler_active);
	zend_native_compiler_destroy(session->request_compiler);
	zend_native_dynamic_compiler_destroy(&session->dynamic_compiler);
	efree(session->reentry_bindings);
	efree(session);
}

static void zend_native_compiler_session_dtor(zval *value)
{
	zend_native_compiler_session_destroy(Z_PTR_P(value));
}

static zend_native_compiler_session *zend_native_compiler_session_find(
	const zend_native_compiler *compiler)
{
	if (!zend_native_compiler_sessions_active || compiler == NULL) {
		return NULL;
	}
	return zend_hash_index_find_ptr(
		&zend_native_compiler_sessions,
		(zend_ulong) (uintptr_t) compiler);
}

static zend_native_compiler_session *zend_native_compiler_session_get(
	zend_native_compiler *compiler)
{
	zend_native_compiler_session *session;

	if (compiler == NULL) {
		return NULL;
	}
	session = zend_native_compiler_session_find(compiler);
	if (session != NULL) {
		return session;
	}
	if (!zend_native_compiler_sessions_active) {
		zend_hash_init(
			&zend_native_compiler_sessions, 4, NULL,
			zend_native_compiler_session_dtor, false);
		zend_native_compiler_sessions_active = true;
	}
	session = ecalloc(1, sizeof(*session));
	session->compiler = compiler;
	zend_native_dynamic_compiler_init(&session->dynamic_compiler);
	zend_native_dynamic_compiler_bind_product(
		&session->dynamic_compiler, compiler);
	if (zend_hash_index_add_ptr(
			&zend_native_compiler_sessions,
			(zend_ulong) (uintptr_t) compiler, session) == NULL) {
		zend_native_compiler_session_destroy(session);
		return NULL;
	}
	return session;
}

static void zend_native_compiler_session_release(
	zend_native_compiler *compiler)
{
	zend_native_compiler_session *session =
		zend_native_compiler_session_find(compiler);

	if (session == NULL) {
		return;
	}
	ZEND_ASSERT(!session->reentry_active);
	ZEND_ASSERT(!session->dynamic_compiler_active);
	zend_hash_index_del(
		&zend_native_compiler_sessions,
		(zend_ulong) (uintptr_t) compiler);
	if (zend_hash_num_elements(&zend_native_compiler_sessions) == 0) {
		zend_hash_destroy(&zend_native_compiler_sessions);
		zend_native_compiler_sessions_active = false;
	}
}

static void zend_native_compiler_session_record_execution(
	zend_native_compiler *compiler, uint64_t elapsed)
{
	zend_native_compiler_session *session;

	if (!compiler->persistent) {
		compiler->stats.execute_ns += elapsed;
		compiler->stats.last_execute_ns = elapsed;
		if (compiler->stats.executions == 0) {
			compiler->stats.first_execute_ns = elapsed;
		}
		compiler->stats.executions++;
		return;
	}
	session = zend_native_compiler_session_find(compiler);
	ZEND_ASSERT(session != NULL);
	if (session == NULL) {
		return;
	}
	session->execute_ns += elapsed;
	session->last_execute_ns = elapsed;
	if (session->executions == 0) {
		session->first_execute_ns = elapsed;
	}
	session->executions++;
}

static void zend_native_compiler_session_flush_stats(
	zend_native_compiler *compiler)
{
	zend_native_compiler_session *session =
		zend_native_compiler_session_find(compiler);
	uint32_t previous_executions;

	if (session == NULL || session->executions == 0) {
		return;
	}
	__atomic_fetch_add(
		&compiler->stats.execute_ns, session->execute_ns, __ATOMIC_RELAXED);
	__atomic_store_n(
		&compiler->stats.last_execute_ns,
		session->last_execute_ns, __ATOMIC_RELAXED);
	previous_executions = __atomic_fetch_add(
		&compiler->stats.executions,
		session->executions, __ATOMIC_RELAXED);
	if (previous_executions == 0) {
		__atomic_store_n(
			&compiler->stats.first_execute_ns,
			session->first_execute_ns, __ATOMIC_RELAXED);
	}
	session->execute_ns = 0;
	session->first_execute_ns = 0;
	session->last_execute_ns = 0;
	session->executions = 0;
}

static zend_native_compiler *zend_native_compiler_request_companion(
	zend_native_compiler *compiler,
	zend_op_array *root,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiler_session *session =
		zend_native_compiler_session_get(compiler);
	zend_native_compiler_config config;

	if (session == NULL || root == NULL) {
		return NULL;
	}
	if (session->request_compiler != NULL) {
		session->request_script.function_table = *EG(function_table);
		session->request_script.class_table = *EG(class_table);
		return session->request_compiler;
	}
	memset(&session->request_script, 0, sizeof(session->request_script));
	session->request_script.main_op_array = *root;
	session->request_script.function_table = *EG(function_table);
	session->request_script.class_table = *EG(class_table);
	session->request_script.filename = root->filename;
	memset(&config, 0, sizeof(config));
	config.script = &session->request_script;
	config.target = compiler->target;
	config.mir_chunk_size = compiler->mir_chunk_size;
	config.frame_probe = compiler->frame_probe;
	config.frame_probe_context = compiler->frame_probe_context;
	config.source_probe = compiler->source_probe;
	config.direct_reentry = true;
	session->request_compiler =
		zend_native_compiler_create(&config, diagnostic);
	return session->request_compiler;
}

static void *zend_native_compiler_alloc(
	const zend_native_compiler *compiler, size_t size, bool zero)
{
	void *allocation = pemalloc(size, compiler->persistent);

	if (zero) {
		memset(allocation, 0, size);
	}
	return allocation;
}

static void *zend_native_compiler_realloc(
	const zend_native_compiler *compiler, void *allocation,
	size_t count, size_t size)
{
	return safe_perealloc(
		allocation, count, size, 0, compiler->persistent);
}

static void zend_native_compiler_free(
	const zend_native_compiler *compiler, void *allocation)
{
	pefree(allocation, compiler->persistent);
}

static bool zend_native_compiler_index_script_functions(
	zend_native_compiler *compiler);

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
	zend_native_compiler_module_allocation *allocation;
	size_t alignment_mask;
	size_t prefix_size;
	size_t allocation_size;
	uintptr_t address;

	if (host == NULL || host->fail_allocation
			|| size == 0 || alignment == 0
			|| (alignment & (alignment - 1)) != 0
			|| sizeof(*allocation) > SIZE_MAX - (alignment - 1)) {
		return NULL;
	}
	alignment_mask = alignment - 1;
	prefix_size = sizeof(*allocation) + alignment_mask;
	if (size > SIZE_MAX - prefix_size) {
		return NULL;
	}
	allocation_size = prefix_size + size;
	allocation = malloc(allocation_size);
	if (allocation == NULL) {
		return NULL;
	}
	address = (uintptr_t) allocation + sizeof(*allocation);
	if (address > UINTPTR_MAX - alignment_mask) {
		free(allocation);
		return NULL;
	}
	allocation->next = host->allocations;
	host->allocations = allocation;
	host->successful_allocations++;
	return (void *) ((address + alignment_mask) & ~alignment_mask);
}

static void zend_native_compiler_module_reset(void *context)
{
	zend_native_compiler_module_host *host = context;
	zend_native_compiler_module_allocation *allocation;

	if (host == NULL) {
		return;
	}
	allocation = host->allocations;
	while (allocation != NULL) {
		zend_native_compiler_module_allocation *next = allocation->next;

		free(allocation);
		allocation = next;
	}
	host->allocations = NULL;
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
			|| host->allocations != NULL) {
		return NULL;
	}
	host->fail_allocation =
		module_context->compiler->fault
			== ZEND_NATIVE_COMPILE_FAULT_MODULE_ALLOCATION;
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
			&& zend_native_op_array_identity_matches(
				&function->op_array_identity, op_array)
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
	compiler->functions = zend_native_compiler_realloc(
		compiler, compiler->functions, new_capacity,
		sizeof(*compiler->functions));
	memset(compiler->functions + compiler->function_capacity, 0,
		(new_capacity - compiler->function_capacity)
			* sizeof(*compiler->functions));
	compiler->function_capacity = new_capacity;
	return true;
}

static bool zend_native_compiler_reserve_publications(
	zend_native_compiler *compiler, uint32_t additional)
{
	uint32_t required;
	uint32_t new_capacity;

	if (additional > UINT32_MAX - compiler->publication_count) {
		return false;
	}
	required = compiler->publication_count + additional;
	if (required <= compiler->publication_capacity) {
		return true;
	}
	new_capacity = compiler->publication_capacity < 8
		? 8 : compiler->publication_capacity;
	while (new_capacity < required) {
		if (new_capacity > UINT32_MAX / 2) {
			new_capacity = required;
			break;
		}
		new_capacity *= 2;
	}
	compiler->publication_log = zend_native_compiler_realloc(
		compiler, compiler->publication_log, new_capacity,
		sizeof(*compiler->publication_log));
	compiler->publication_capacity = new_capacity;
	return true;
}

static void zend_native_compiler_record_publication(
	zend_native_compiler *compiler,
	const zend_native_compiled_function *function)
{
	ZEND_ASSERT(compiler->publication_count
		< compiler->publication_capacity);
	ZEND_ASSERT(function != NULL
		&& function->entry_cell.state == ZEND_NATIVE_ENTRY_READY);
	compiler->publication_log[compiler->publication_count++] =
		function->op_array;
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
	function = zend_native_compiler_alloc(
		compiler, sizeof(*function), true);
	function->op_array = op_array;
	zend_native_op_array_identity_capture(
		&function->op_array_identity, op_array);
	function->registry_index = compiler->function_count;
	function->state = ZEND_NATIVE_CODEUNIT_COMPILING;
	zend_native_entry_cell_init(
		&function->entry_cell, (zend_function *) op_array);
	function->entry_cell.lease_managed = compiler->persistent;
	if (compiler->frame_probe != NULL) {
		zend_native_entry_cell_set_frame_probe(
			&function->entry_cell, compiler->frame_probe,
			compiler->frame_probe_context);
	}
	if (zend_native_entry_cell_begin_compile(
			&function->entry_cell) == FAILURE) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native entry cell rejected synchronous compilation");
		zend_native_compiler_free(compiler, function);
		return NULL;
	}
	if (zend_hash_index_update_ptr(
			&compiler->functions_by_op_array,
			(zend_ulong) (uintptr_t) op_array, function) == NULL) {
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_COMPILING) {
			zend_native_entry_cell_fail(&function->entry_cell);
		}
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native codeunit index insertion failed");
		zend_native_compiler_free(compiler, function);
		return NULL;
	}
	compiler->functions[compiler->function_count++] = function;
	compiler->transients_released = false;
	return function;
}

static bool zend_native_compiler_source_position_dominates(
	const zend_ssa *ssa, uint32_t definition, uint32_t use)
{
	uint32_t definition_block;
	int32_t use_block;

	if (ssa == NULL || ssa->cfg.blocks == NULL || ssa->cfg.map == NULL) {
		return false;
	}
	definition_block = ssa->cfg.map[definition];
	use_block = (int32_t) ssa->cfg.map[use];
	if (definition_block >= ssa->cfg.blocks_count || use_block < 0
			|| (uint32_t) use_block >= ssa->cfg.blocks_count) {
		return false;
	}
	if (definition_block == (uint32_t) use_block) {
		return definition <= use;
	}
	while (use_block >= 0 && (uint32_t) use_block != definition_block) {
		use_block = ssa->cfg.blocks[use_block].idom;
	}
	return use_block >= 0 && (uint32_t) use_block == definition_block;
}

static bool zend_native_compiler_cv_has_one_scalar_constant_definition(
	const zend_op_array *op_array, const zend_ssa *ssa, uint32_t cv,
	uint32_t use)
{
	uint32_t definition_count = 0;
	uint32_t definition = 0;
	int32_t variable;

	for (variable = 0; variable < ssa->vars_count; variable++) {
		if (ssa->vars[variable].var != (int32_t) cv) {
			continue;
		}
		if (ssa->vars[variable].definition_phi != NULL) {
			return false;
		}
		if (ssa->vars[variable].definition < 0) {
			continue;
		}
		definition_count++;
		definition = (uint32_t) ssa->vars[variable].definition;
	}
	if (definition_count != 1 || definition >= op_array->last
			|| !zend_native_compiler_source_position_dominates(
				ssa, definition, use)) {
		return false;
	}
	{
		const zend_op *opline = &op_array->opcodes[definition];
		const zval *literal;

		if (opline->opcode != ZEND_ASSIGN || opline->op1_type != IS_CV
				|| EX_VAR_TO_NUM(opline->op1.var) != cv
				|| opline->op2_type != IS_CONST) {
			return false;
		}
		literal = RT_CONSTANT(opline, opline->op2);
		return Z_TYPE_P(literal) == IS_LONG
			|| Z_TYPE_P(literal) == IS_DOUBLE
			|| Z_TYPE_P(literal) == IS_FALSE
			|| Z_TYPE_P(literal) == IS_TRUE
			|| Z_TYPE_P(literal) == IS_NULL;
	}
}

static bool zend_native_compiler_has_stable_local_dynamic_reads(
	const zend_op_array *op_array, const zend_ssa *ssa)
{
	uint32_t index;
	bool found_dynamic_read = false;

	if (op_array == NULL || ssa == NULL || op_array->function_name == NULL
			|| op_array->opcodes == NULL || op_array->vars == NULL
			|| ssa->ops == NULL || ssa->vars == NULL
			|| !(ssa->cfg.flags & ZEND_FUNC_INDIRECT_VAR_ACCESS)
			|| (ssa->cfg.flags & ZEND_FUNC_HAS_CALLS)
			|| (op_array->fn_flags
				& (ZEND_ACC_RETURN_REFERENCE | ZEND_ACC_GENERATOR))
			|| op_array->last_try_catch != 0) {
		return false;
	}
	for (index = 0; index < op_array->num_args; index++) {
		if (ARG_SHOULD_BE_SENT_BY_REF(
				(zend_function *) op_array, index + 1)) {
			return false;
		}
	}
	for (index = 0; index < op_array->last; index++) {
		const zend_op *opline = &op_array->opcodes[index];

		if (zend_get_user_opcode_handler(opline->opcode) != NULL) {
			return false;
		}
		switch (opline->opcode) {
			case ZEND_ASSIGN_REF:
			case ZEND_SEND_REF:
			case ZEND_NEW:
			case ZEND_INCLUDE_OR_EVAL:
			case ZEND_UNSET_VAR:
			case ZEND_FETCH_W:
			case ZEND_FETCH_RW:
			case ZEND_FETCH_FUNC_ARG:
			case ZEND_FETCH_IS:
			case ZEND_FETCH_UNSET:
			case ZEND_ISSET_ISEMPTY_VAR:
			case ZEND_TICKS:
			case ZEND_FE_RESET_RW:
			case ZEND_FE_FETCH_RW:
			case ZEND_MAKE_REF:
			case ZEND_USER_OPCODE:
			case ZEND_YIELD:
			case ZEND_YIELD_FROM:
			case ZEND_BIND_GLOBAL:
			case ZEND_BIND_LEXICAL:
			case ZEND_BIND_STATIC:
			case ZEND_SEPARATE:
			case ZEND_RETURN_BY_REF:
			case ZEND_DECLARE_LAMBDA_FUNCTION:
				return false;
			case ZEND_FETCH_R:
				break;
			default:
				continue;
		}
		if (opline->extended_value != ZEND_FETCH_LOCAL
				|| opline->op1_type != IS_CV) {
			return false;
		}
		{
			const zend_ssa_op *ssa_op = &ssa->ops[index];
			const uint32_t name_cv = EX_VAR_TO_NUM(opline->op1.var);
			const zend_ssa_var *name_variable;
			const zend_op *name_definition;
			const zval *name_literal;
			uint32_t target_cv;
			int32_t name_ssa = ssa_op->op1_use;
			uint32_t name_definition_count = 0;
			int32_t variable;

			if (name_cv >= op_array->last_var || name_ssa < 0
					|| name_ssa >= ssa->vars_count) {
				return false;
			}
			name_variable = &ssa->vars[name_ssa];
			if (name_variable->var != (int32_t) name_cv
					|| name_variable->definition < 0
					|| (uint32_t) name_variable->definition >= op_array->last
					|| !zend_native_compiler_source_position_dominates(
						ssa, (uint32_t) name_variable->definition, index)) {
				return false;
			}
			for (variable = 0; variable < ssa->vars_count; variable++) {
				if (ssa->vars[variable].var != (int32_t) name_cv) {
					continue;
				}
				if (ssa->vars[variable].definition_phi != NULL) {
					return false;
				}
				if (ssa->vars[variable].definition >= 0) {
					name_definition_count++;
				}
			}
			if (name_definition_count != 1) {
				return false;
			}
			name_definition = &op_array->opcodes[name_variable->definition];
			if (name_definition->opcode != ZEND_ASSIGN
					|| name_definition->op1_type != IS_CV
					|| EX_VAR_TO_NUM(name_definition->op1.var) != name_cv
					|| name_definition->op2_type != IS_CONST
					|| ssa->ops[name_variable->definition].op1_def != name_ssa) {
				return false;
			}
			name_literal = RT_CONSTANT(
				name_definition, name_definition->op2);
			if (Z_TYPE_P(name_literal) != IS_STRING) {
				return false;
			}
			for (target_cv = 0; target_cv < op_array->last_var; target_cv++) {
				if (zend_string_equals(
						op_array->vars[target_cv], Z_STR_P(name_literal))) {
					break;
				}
			}
			if (target_cv == op_array->last_var
					|| !zend_native_compiler_cv_has_one_scalar_constant_definition(
						op_array, ssa, target_cv, index)) {
				return false;
			}
		}
		found_dynamic_read = true;
	}
	return found_dynamic_read;
}

static zend_result zend_native_compiler_restore_stable_dynamic_read_ssa(
	zend_optimizer_ctx *optimizer, zend_op_array *op_array, zend_ssa *ssa)
{
	int32_t variable;

	if (!zend_native_compiler_has_stable_local_dynamic_reads(op_array, ssa)) {
		return SUCCESS;
	}
	for (variable = 0; variable < ssa->vars_count; variable++) {
		if (ssa->vars[variable].alias == SYMTABLE_ALIAS) {
			ssa->vars[variable].alias = NO_ALIAS;
		}
	}
	memset(ssa->var_info, 0,
		(size_t) ssa->vars_count * sizeof(zend_ssa_var_info));
	if (zend_ssa_inference(
			&optimizer->arena, op_array, optimizer->script, ssa,
			optimizer->optimization_level) == FAILURE) {
		return FAILURE;
	}
	return zend_ssa_escape_analysis(optimizer->script, op_array, ssa);
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
	if (zend_native_compiler_restore_stable_dynamic_read_ssa(
			&optimizer, function->op_array, &function->ssa) == FAILURE) {
		function->ssa_arena = optimizer.arena;
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_SSA,
			1, "SSA inference rejected stable local dynamic reads");
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
	if (source->last > UINT32_MAX - echo_count
			|| (compiler->source_probe
				&& source->last + echo_count > UINT32_MAX - source->last)) {
		return false;
	}
	function->source_effect_capacity = source->last + echo_count
		+ (compiler->source_probe ? source->last : 0);
	if (function->source_effect_capacity != 0) {
		function->source_effects = zend_native_compiler_alloc(
			compiler,
			function->source_effect_capacity
				* sizeof(*function->source_effects),
			true);
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
	for (index = 0; compiler->source_probe && index < source->last; index++) {
		zend_native_source_effect *effect =
			&function->source_effects[function->source_effect_count++];

		effect->source_position_id = index;
		effect->kind = ZEND_NATIVE_SOURCE_EFFECT_DEBUG_PROBE;
		effect->exact_type = ZEND_MIR_SCALAR_TYPE_NONE;
		effect->target_block_id = ZEND_MIR_ID_INVALID;
	}
	function->source_effects_prepared = true;
	return true;
}

static uint32_t zend_native_compiler_exception_route_next(
	uint32_t *next, uint32_t index)
{
	uint32_t root = index;

	while (next[root] != root) {
		root = next[root];
	}
	while (next[index] != index) {
		uint32_t parent = next[index];

		next[index] = root;
		index = parent;
	}
	return root;
}

static void zend_native_compiler_fill_exception_route(
	uint32_t *handler_oplines, uint32_t *next, uint32_t opcode_count,
	uint32_t begin, uint32_t end, uint32_t handler_opline)
{
	uint32_t index;

	if (begin >= end || end > opcode_count || handler_opline >= opcode_count) {
		return;
	}
	index = zend_native_compiler_exception_route_next(next, begin);
	while (index < end) {
		handler_oplines[index] = handler_opline;
		next[index] = zend_native_compiler_exception_route_next(
			next, index + 1);
		index = next[index];
	}
}

static bool zend_native_compiler_prepare_exception_routes(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function)
{
	const zend_op_array *op_array = function->op_array;
	uint32_t *next;
	uint32_t index;

	if (op_array == NULL || function->ssa.cfg.map == NULL) {
		return false;
	}
	function->exception_handler_oplines = zend_native_compiler_alloc(
		compiler,
		(op_array->last == 0 ? 1 : op_array->last)
			* sizeof(*function->exception_handler_oplines),
		true);
	next = zend_native_compiler_alloc(
		compiler, ((size_t) op_array->last + 1) * sizeof(*next), true);
	for (index = 0; index < op_array->last; index++) {
		function->exception_handler_oplines[index] = ZEND_MIR_ID_INVALID;
		next[index] = index;
	}
	next[op_array->last] = op_array->last;
	for (index = op_array->last_try_catch; index != 0; index--) {
		const zend_try_catch_element *region =
			&op_array->try_catch_array[index - 1];
		uint32_t finally_begin = region->catch_op != 0
			? region->catch_op : region->try_op;

		if (region->catch_op != 0
				&& region->catch_op < op_array->last
				&& function->ssa.cfg.map[region->catch_op]
					< function->ssa.cfg.blocks_count) {
			zend_native_compiler_fill_exception_route(
				function->exception_handler_oplines, next, op_array->last,
				region->try_op, region->catch_op, region->catch_op);
		}
		if (region->finally_op != 0
				&& region->finally_op < op_array->last
				&& function->ssa.cfg.map[region->finally_op]
					< function->ssa.cfg.blocks_count) {
			zend_native_compiler_fill_exception_route(
				function->exception_handler_oplines, next, op_array->last,
				finally_begin, region->finally_op, region->finally_op);
		}
	}
	for (index = 0; index < op_array->last; index++) {
		const zend_op *opline = &op_array->opcodes[index];

		if ((opline->opcode == ZEND_FREE || opline->opcode == ZEND_FE_FREE)
				&& (opline->extended_value & ZEND_FREE_ON_RETURN) != 0) {
			function->exception_handler_oplines[index] =
				opline->op2.opline_num < op_array->last - index
					? function->exception_handler_oplines[
						index + opline->op2.opline_num]
					: ZEND_MIR_ID_INVALID;
		}
	}
	zend_native_compiler_free(compiler, next);
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
				&& instruction.opcode
					!= ZEND_MIR_OPCODE_VALUE_COND_BRANCH
				&& instruction.opcode != ZEND_MIR_OPCODE_ITERATOR_BRANCH
				&& instruction.opcode != ZEND_MIR_OPCODE_CATCH_ENTER
				&& instruction.opcode != ZEND_MIR_OPCODE_THROW_SOURCE_ZVAL
				&& instruction.opcode != ZEND_MIR_OPCODE_GENERATOR_CREATE
				&& instruction.opcode != ZEND_MIR_OPCODE_GENERATOR_YIELD
				&& instruction.opcode
					!= ZEND_MIR_OPCODE_GENERATOR_YIELD_FROM)
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
			&& !zend_native_compiler_prepare_exception_routes(
				compiler, function)) {
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
	memset(&compiler->last_diagnostic, 0,
		sizeof(compiler->last_diagnostic));
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
		char message[192];
		const char *function_name = function->op_array->function_name != NULL
			? ZSTR_VAL(function->op_array->function_name) : "{main}";

		if (compiler->last_diagnostic.message[0] != '\0'
				&& compiler->last_diagnostic.code
					== result.lowering.diagnostic_code) {
			if (diagnostic != NULL) {
				*diagnostic = compiler->last_diagnostic;
			}
			return false;
		}
		snprintf(message, sizeof(message),
			"native lowering rejected reachable function %.96s (MIRL%04u)",
			function_name, (unsigned int) result.lowering.diagnostic_code);
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_LOWERING,
			result.lowering.diagnostic_code,
			message);
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
	zend_native_compiler *compiler,
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
	first = zend_native_compiler_alloc(
		compiler, target_count * sizeof(*first), false);
	last = safe_emalloc(target_count, sizeof(*last), 0);
	if (site_count != 0) {
		next = zend_native_compiler_alloc(
			compiler, site_count * sizeof(*next), false);
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
	zend_native_compiler_free(compiler, next);
	efree(last);
	zend_native_compiler_free(compiler, first);
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
			/*
			 * A monomorphic method may belong to another OPcache owner
			 * (notably a preloaded class).  Its process address is not a
			 * persistent identity in this script's image.  Keep the call
			 * indirect so runtime owner resolution selects that owner's
			 * entry cell without serializing a foreign function pointer.
			 */
			if (function->op_array.opcodes == NULL
					|| zend_hash_index_find_ptr(
						&compiler->source_op_arrays_by_opcodes,
						(zend_ulong) (uintptr_t)
							function->op_array.opcodes) == NULL) {
				return caller;
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

	/* Deferred OPcache bundles are built against owner class entries before
	 * request-local classes are necessarily linked.  A direct method descriptor
	 * would retain that owner's function and scope, while an overridable receiver
	 * may belong to a request-local derived class.  Keep those calls on the
	 * resolving user-call path.  A method declared by a final same-owner class
	 * cannot acquire a derived override, and its source op_array is already
	 * represented by the bundle's symbolic user-function reference. */
	if (compiler != NULL
			&& (compiler->defer_publication || compiler->persistent)
			&& target->kind == ZEND_MIR_CALL_TARGET_METHOD_USER
			&& (callee == NULL || callee->scope == NULL
				|| (callee->scope->ce_flags & ZEND_ACC_FINAL) == 0)) {
		return false;
	}
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

static bool zend_native_compiler_exact_scalar_satisfies_type(
	zend_mir_scalar_type_mask exact_type, zend_type type)
{
	uint32_t accepted;

	if (!ZEND_TYPE_IS_SET(type)) {
		return true;
	}
	if (!zend_mir_scalar_type_is_exact(exact_type)
			|| !ZEND_TYPE_IS_ONLY_MASK(type)) {
		return false;
	}
	accepted = ZEND_TYPE_PURE_MASK(type);
	switch (exact_type) {
		case ZEND_MIR_SCALAR_TYPE_NULL:
			return (accepted & MAY_BE_NULL) != 0;
		case ZEND_MIR_SCALAR_TYPE_I1:
			return (accepted & MAY_BE_BOOL) == MAY_BE_BOOL;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return (accepted & MAY_BE_LONG) != 0;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return (accepted & MAY_BE_DOUBLE) != 0;
		default:
			return false;
	}
}

static bool zend_native_compiler_index_u32(
	const HashTable *index, uint32_t id, uint32_t *value)
{
	zval *entry;

	if (index == NULL || value == NULL
			|| (entry = zend_hash_index_find(index, id)) == NULL
			|| Z_TYPE_P(entry) != IS_LONG || Z_LVAL_P(entry) < 0
			|| (zend_ulong) Z_LVAL_P(entry) > UINT32_MAX) {
		return false;
	}
	*value = (uint32_t) Z_LVAL_P(entry);
	return true;
}

static zend_mir_scalar_type_mask zend_native_compiler_index_exact_type(
	const HashTable *exact_types, zend_mir_value_id value_id)
{
	uint32_t exact_type;

	if (!zend_native_compiler_index_u32(
			exact_types, value_id, &exact_type)) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	return (zend_mir_scalar_type_mask) exact_type;
}

static zend_mir_scalar_type_mask
zend_native_compiler_source_operand_exact_type(
	const HashTable *exact_types,
	const zend_mir_source_operand_ref *operand)
{
	zend_mir_value_id value_id;

	if (operand == NULL) {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	if (operand->kind == ZEND_MIR_SOURCE_OPERAND_LITERAL) {
		value_id = zend_mir_value_from_synthetic(operand->index);
	} else if ((operand->kind == ZEND_MIR_SOURCE_OPERAND_SLOT
				|| operand->kind == ZEND_MIR_SOURCE_OPERAND_SSA)
			&& operand->ssa_variable_id != UINT32_MAX) {
		value_id = zend_mir_value_from_original_ssa(
			operand->ssa_variable_id);
	} else {
		return ZEND_MIR_SCALAR_TYPE_NONE;
	}
	return zend_mir_id_is_valid(value_id)
		? zend_native_compiler_index_exact_type(exact_types, value_id)
		: ZEND_MIR_SCALAR_TYPE_NONE;
}

/*
 * A leaf-scalar frame is deliberately stricter than "currently emits no
 * helper". It is a complete MIR proof that the generated entry cannot call,
 * allocate, throw, observe, interrupt, own a refcounted value, or otherwise
 * require a published Zend activation. Such an entry still receives the
 * canonical frame ABI, but the caller may keep that frame private.
 */
static bool zend_native_compiler_calculate_leaf_scalar_frame(
	zend_native_compiler *compiler,
	const zend_native_compiled_function *function)
{
	const zend_mir_view *view;
	const zend_mir_value_view *values;
	zend_mir_function_record mir_function;
	HashTable block_indices;
	HashTable exact_types;
	HashTable leaf_exact_types;
	HashTable executable_operands;
	HashTable executable_indices;
	bool *reachable;
	uint32_t *queue;
	uint32_t block_count;
	uint32_t queue_head = 0;
	uint32_t queue_tail = 0;
	uint32_t instruction_count;
	uint32_t index;
	uint32_t value_fact_count;
	uint32_t executable_operation_count;
	bool has_return = false;
	bool eligible = false;

	if (compiler == NULL || function == NULL || function->module == NULL
			|| function->op_array == NULL
			|| compiler->frame_probe != NULL
			|| compiler->source_probe
			|| function->source_effect_count != 0
			|| function->op_array->scope != NULL
			|| (function->op_array->fn_flags
				& (ZEND_ACC_VARIADIC | ZEND_ACC_CALL_VIA_TRAMPOLINE
					| ZEND_ACC_GENERATOR)) != 0) {
		return false;
	}
	view = zend_native_compiler_module_view(compiler, function->module);
	values = zend_mir_module_value_view_from_view(view);
	if (view == NULL || view->function_count(view->context) != 1
			|| values == NULL
			|| !view->function_at(view->context, 0, &mir_function)
			|| !zend_mir_id_is_valid(mir_function.entry_block_id)
			|| view->block_at == NULL || view->successor_count == NULL
			|| view->successor_at == NULL
			|| view->instruction_at == NULL
			|| view->instruction_operand_count == NULL
			|| view->instruction_operand_at == NULL
			|| view->value_fact_count == NULL
			|| view->value_fact_at == NULL
			|| values->executable_operation_count == NULL
			|| values->executable_operation_at == NULL) {
		return false;
	}
	block_count = view->block_count(view->context);
	if (block_count == 0 || block_count > ZEND_MIR_ID_MAX) {
		return false;
	}
	value_fact_count = view->value_fact_count(view->context);
	executable_operation_count =
		values->executable_operation_count(values->context);
	zend_hash_init(&block_indices, block_count, NULL, NULL, false);
	zend_hash_init(&exact_types, value_fact_count, NULL, NULL, false);
	zend_hash_init(&leaf_exact_types, executable_operation_count,
		NULL, NULL, false);
	zend_hash_init(
		&executable_operands, executable_operation_count, NULL, NULL, false);
	zend_hash_init(
		&executable_indices, executable_operation_count, NULL, NULL, false);
	reachable = ecalloc(block_count, sizeof(*reachable));
	queue = safe_emalloc(block_count, sizeof(*queue), 0);
	for (index = 0; index < block_count; index++) {
		zend_mir_block_record block;
		zval entry;

		if (!view->block_at(view->context, index, &block)) {
			goto done;
		}
		ZVAL_LONG(&entry, index);
		if (zend_hash_index_add_new(
				&block_indices, block.id, &entry) == NULL) {
			goto done;
		}
	}
	for (index = 0; index < value_fact_count; index++) {
		zend_mir_value_fact_ref fact;
		zval entry;

		if (!view->value_fact_at(view->context, index, &fact)) {
			goto done;
		}
		ZVAL_LONG(&entry, fact.exact_type);
		if (zend_hash_index_add_new(
				&exact_types, fact.value_id, &entry) == NULL) {
			goto done;
		}
	}
	for (index = 0; index < executable_operation_count; index++) {
		zend_mir_executable_value_ref operation;
		zend_mir_value_id value_id;
		zval entry;

		if (!values->executable_operation_at(
				values->context, index, &operation)) {
			goto done;
		}
		ZVAL_LONG(&entry, index);
		if (zend_hash_index_add_new(
				&executable_indices, operation.id, &entry) == NULL) {
			goto done;
		}
		switch (operation.op1.kind) {
			case ZEND_MIR_SOURCE_OPERAND_LITERAL:
				value_id =
					zend_mir_value_from_synthetic(operation.op1.index);
				break;
			case ZEND_MIR_SOURCE_OPERAND_SLOT:
			case ZEND_MIR_SOURCE_OPERAND_SSA:
				if (operation.op1.ssa_variable_id == UINT32_MAX) {
					goto done;
				}
				value_id = zend_mir_value_from_original_ssa(
					operation.op1.ssa_variable_id);
				break;
			default:
				continue;
		}
		if (!zend_mir_id_is_valid(value_id)) {
			goto done;
		}
		ZVAL_LONG(&entry, value_id);
		if (zend_hash_index_add_new(
				&executable_operands, operation.id, &entry) == NULL) {
			goto done;
		}
	}
	{
		uint32_t entry;
		if (!zend_native_compiler_index_u32(
				&block_indices, mir_function.entry_block_id, &entry)) {
			goto done;
		}
		reachable[entry] = true;
		queue[queue_tail++] = entry;
	}
	while (queue_head < queue_tail) {
		zend_mir_block_record block;
		uint32_t successor_count;
		uint32_t successor_index;

		if (!view->block_at(
				view->context, queue[queue_head++], &block)) {
			goto done;
		}
		successor_count = view->successor_count(view->context, block.id);
		for (successor_index = 0;
				successor_index < successor_count; successor_index++) {
			zend_mir_block_id successor_id;
			uint32_t successor;

			if (!view->successor_at(view->context, block.id,
					successor_index, &successor_id)
					|| !zend_native_compiler_index_u32(
						&block_indices, successor_id, &successor)) {
				goto done;
			}
			if (!reachable[successor]) {
				reachable[successor] = true;
				queue[queue_tail++] = (uint32_t) successor;
			}
		}
	}

	instruction_count = view->instruction_count(view->context);
	for (index = 0; index < instruction_count; index++) {
		zend_mir_instruction_record instruction;
		uint32_t block;
		bool pure_opcode;

		if (!view->instruction_at(view->context, index, &instruction)
				|| !zend_native_compiler_index_u32(
					&block_indices, instruction.block_id, &block)) {
			goto done;
		}
		if (!reachable[block]) {
			continue;
		}
		if (instruction.opcode == ZEND_MIR_OPCODE_VERIFY_RETURN_TYPE) {
			zend_mir_value_id value_id = ZEND_MIR_ID_INVALID;
			zend_mir_scalar_type_mask exact_type =
				ZEND_MIR_SCALAR_TYPE_NONE;

			if (function->op_array->arg_info == NULL
					|| (function->op_array->fn_flags
							& ZEND_ACC_HAS_RETURN_TYPE) == 0
					|| !zend_native_compiler_index_u32(
						&executable_operands, instruction.id, &value_id)
					|| (!zend_native_compiler_index_u32(
							&leaf_exact_types, value_id, &exact_type)
						&& !zend_mir_scalar_type_is_exact(
							exact_type =
								zend_native_compiler_index_exact_type(
									&exact_types, value_id)))
					|| !zend_mir_scalar_type_is_exact(exact_type)
					|| !zend_native_compiler_exact_scalar_satisfies_type(
						exact_type,
						function->op_array->arg_info[-1].type)) {
				goto done;
			}
			continue;
		}
		if (instruction.opcode == ZEND_MIR_OPCODE_VALUE_BINARY_OP) {
			zend_mir_executable_value_ref operation;
			uint32_t operation_index;
			bool direct_long_operand;

			if (!zend_native_compiler_index_u32(
					&executable_indices, instruction.id,
					&operation_index)
					|| !values->executable_operation_at(
						values->context, operation_index, &operation)
					|| operation.id != instruction.id) {
				goto done;
			}
			if ((operation.source_opcode != ZEND_ADD
						&& operation.source_opcode != ZEND_SUB)
					|| (operation.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operation.result.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| (operation.result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_TMP
						&& operation.result.slot_kind
							!= ZEND_MIR_SOURCE_SLOT_VAR)
					|| operation.result.ssa_variable_id == UINT32_MAX
					|| operation.result_storage_id == ZEND_MIR_ID_INVALID
					|| zend_native_compiler_source_operand_exact_type(
						&exact_types, &operation.op1)
						!= ZEND_MIR_SCALAR_TYPE_I64
					|| zend_native_compiler_source_operand_exact_type(
						&exact_types, &operation.op2)
						!= ZEND_MIR_SCALAR_TYPE_I64) {
				goto done;
			}
			direct_long_operand =
				operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_LITERAL
				|| ((operation.op1.kind == ZEND_MIR_SOURCE_OPERAND_SLOT
						|| operation.op1.kind
							== ZEND_MIR_SOURCE_OPERAND_SSA)
					&& operation.op1.slot_kind
						== ZEND_MIR_SOURCE_SLOT_CV
					&& operation.op1_storage_id != ZEND_MIR_ID_INVALID);
			direct_long_operand = direct_long_operand
				&& (operation.op2.kind
						== ZEND_MIR_SOURCE_OPERAND_LITERAL
					|| ((operation.op2.kind
								== ZEND_MIR_SOURCE_OPERAND_SLOT
							|| operation.op2.kind
								== ZEND_MIR_SOURCE_OPERAND_SSA)
						&& operation.op2.slot_kind
							== ZEND_MIR_SOURCE_SLOT_CV
						&& operation.op2_storage_id
							!= ZEND_MIR_ID_INVALID));
			if (!direct_long_operand
					|| (operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& operation.op1_storage_id
							== operation.result_storage_id)
					|| (operation.op2.kind
							!= ZEND_MIR_SOURCE_OPERAND_LITERAL
						&& operation.op2_storage_id
							== operation.result_storage_id)) {
				goto done;
			}
			{
				zval result_type;
				ZVAL_LONG(&result_type, ZEND_MIR_SCALAR_TYPE_I64);
				zend_hash_index_update(&leaf_exact_types,
					zend_mir_value_from_original_ssa(
						operation.result.ssa_variable_id),
					&result_type);
			}
			continue;
		}
		if (instruction.opcode == ZEND_MIR_OPCODE_RETURN_SOURCE_ZVAL) {
			zend_mir_executable_value_ref operation;
			zend_mir_value_id value_id;
			zend_mir_scalar_type_mask exact_type;
			uint32_t operation_index;

			if (!zend_native_compiler_index_u32(
					&executable_indices, instruction.id,
					&operation_index)
					|| !values->executable_operation_at(
						values->context, operation_index, &operation)
					|| operation.id != instruction.id
					|| operation.source_opcode != ZEND_RETURN
					|| (operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_SLOT
						&& operation.op1.kind
							!= ZEND_MIR_SOURCE_OPERAND_SSA)
					|| operation.op1.ssa_variable_id == UINT32_MAX
					|| operation.op1.slot_kind < ZEND_MIR_SOURCE_SLOT_CV
					|| operation.op1.slot_kind > ZEND_MIR_SOURCE_SLOT_VAR
					|| operation.op1_storage_id == ZEND_MIR_ID_INVALID
					|| operation.op2.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED
					|| operation.result.kind
						!= ZEND_MIR_SOURCE_OPERAND_UNUSED) {
				goto done;
			}
			value_id = zend_mir_value_from_original_ssa(
				operation.op1.ssa_variable_id);
			if (!zend_native_compiler_index_u32(
					&leaf_exact_types, value_id, &exact_type)
					&& !zend_mir_scalar_type_is_exact(
						exact_type =
							zend_native_compiler_index_exact_type(
								&exact_types, value_id))) {
				goto done;
			}
			if (!zend_mir_scalar_type_is_exact(exact_type)) {
				goto done;
			}
			has_return = true;
			continue;
		}
		pure_opcode =
			instruction.opcode == ZEND_MIR_OPCODE_CONSTANT
			|| instruction.opcode == ZEND_MIR_OPCODE_PHI
			|| instruction.opcode == ZEND_MIR_OPCODE_COPY
			|| instruction.opcode == ZEND_MIR_OPCODE_CANONICALIZE
			|| instruction.opcode == ZEND_MIR_OPCODE_STATEPOINT
			|| instruction.opcode == ZEND_MIR_OPCODE_BRANCH
			|| instruction.opcode == ZEND_MIR_OPCODE_COND_BRANCH
			|| instruction.opcode == ZEND_MIR_OPCODE_RETURN
			|| instruction.opcode == ZEND_MIR_OPCODE_UNREACHABLE
			|| (instruction.opcode >= ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW
				&& instruction.opcode <= ZEND_MIR_OPCODE_SCALAR_DROP);
		if (!pure_opcode
				|| (instruction.representation
						== ZEND_MIR_REPRESENTATION_ZVAL
					&& !(instruction.opcode == ZEND_MIR_OPCODE_CONSTANT
						&& zend_native_compiler_index_exact_type(
							&exact_types, instruction.result_id)
							== ZEND_MIR_SCALAR_TYPE_NULL))
				|| instruction.effects != 0 || instruction.reads != 0
				|| instruction.writes != 0 || instruction.barriers != 0
				|| instruction.ownership_actions != 0) {
			goto done;
		}
		has_return = has_return
			|| instruction.opcode == ZEND_MIR_OPCODE_RETURN;
	}
	eligible = has_return;

done:
	efree(queue);
	efree(reachable);
	zend_hash_destroy(&executable_indices);
	zend_hash_destroy(&executable_operands);
	zend_hash_destroy(&leaf_exact_types);
	zend_hash_destroy(&exact_types);
	zend_hash_destroy(&block_indices);
	return eligible;
}

static bool zend_native_compiler_function_has_leaf_scalar_frame(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function)
{
	if (!function->leaf_scalar_frame_known) {
		function->leaf_scalar_frame =
			zend_native_compiler_calculate_leaf_scalar_frame(
				compiler, function);
		function->leaf_scalar_frame_known = true;
	}
	return function->leaf_scalar_frame;
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

static zend_op_array *zend_native_compiler_retain_runtime_source(
	zend_native_compiler *compiler, zend_op_array *resolved)
{
	zend_native_runtime_source *retained;
	zend_op_array *source;
	zend_ulong key;

	if (compiler == NULL || resolved == NULL || resolved->opcodes == NULL) {
		return resolved;
	}
	key = (zend_ulong) (uintptr_t) resolved->opcodes;
	source = zend_hash_index_find_ptr(
		&compiler->source_op_arrays_by_opcodes, key);
	if (source != NULL) {
		return source->last == resolved->last ? source : NULL;
	}
	/* Immutable script op_arrays already have stable owner storage.  Runtime
	 * closures instead embed a shallow op_array copy in the closure object;
	 * retaining that object would incorrectly extend the lifetime of bound
	 * values.  Keep an independent metadata copy and one shared-code refcount
	 * lease so native code never points through the ephemeral closure object. */
	if (resolved->refcount == NULL) {
		return zend_hash_index_add_ptr(
			&compiler->source_op_arrays_by_opcodes, key, resolved) != NULL
			? resolved : NULL;
	}
	retained = zend_native_compiler_alloc(
		compiler, sizeof(*retained), true);
	retained->op_array = *resolved;
	if (retained->op_array.function_name != NULL) {
		zend_string_addref(retained->op_array.function_name);
	}
	(*retained->op_array.refcount)++;
	retained->op_array.fn_flags &= ~ZEND_ACC_HEAP_RT_CACHE;
	ZEND_MAP_PTR_INIT(retained->op_array.run_time_cache, NULL);
	ZEND_MAP_PTR_INIT(retained->op_array.static_variables_ptr, NULL);
	if (zend_hash_index_add_ptr(
			&compiler->source_op_arrays_by_opcodes, key,
			&retained->op_array) == NULL) {
		destroy_op_array(&retained->op_array);
		zend_native_compiler_free(compiler, retained);
		return NULL;
	}
	retained->next = compiler->runtime_sources;
	compiler->runtime_sources = retained;
	return &retained->op_array;
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

static bool zend_native_compiler_assign_static_component(
	zend_native_compiler *compiler, uint32_t *component_count)
{
	uint32_t index;
	uint32_t required_heads = 2;
	bool populated = false;

	if (compiler->function_count == UINT32_MAX) {
		return false;
	}
	if (compiler->component_head_capacity < required_heads) {
		compiler->component_heads = zend_native_compiler_realloc(
			compiler, compiler->component_heads, required_heads,
			sizeof(*compiler->component_heads));
		compiler->component_head_capacity = required_heads;
	}
	memset(compiler->component_heads, 0,
		required_heads * sizeof(*compiler->component_heads));
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		function->component_id = 0;
		function->next_component_member = NULL;
		if (function->state != ZEND_NATIVE_CODEUNIT_COMPILING) {
			continue;
		}
		/*
		 * compile_locked() begins with one selected root and discovers every
		 * transitively reachable static user target before reaching this
		 * point.  Those still-COMPILING functions are therefore one native
		 * callgraph component, including acyclic leaf edges as well as SCCs.
		 * Keeping them in one TPDE image is what makes local calls, generic
		 * body inlining and component-atomic publication available to the
		 * common case; splitting every acyclic edge into singleton SCCs only
		 * recreated Entry-Cell transport between statically known functions.
		 */
		function->component_id = 1;
		function->next_component_member = compiler->component_heads[1];
		compiler->component_heads[1] = function;
		populated = true;
	}
	*component_count = populated ? 1 : 0;
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

typedef struct _zend_native_component_build_member {
	zend_native_compiled_function *function;
	zend_native_component_member backend;
	zend_native_call_binding *bindings;
	zend_native_internal_call_binding *internal_bindings;
} zend_native_component_build_member;

static void zend_native_compiler_free_component_build(
	zend_native_component_build_member *members, uint32_t member_count)
{
	uint32_t index;

	if (members == NULL) {
		return;
	}
	for (index = 0; index < member_count; index++) {
		efree(members[index].bindings);
		efree(members[index].internal_bindings);
	}
	efree(members);
}

static bool zend_native_compiler_prepare_component_member(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function,
	zend_native_component_build_member *member,
	const uint32_t *component_member_by_registry,
	uint32_t component_registry_count,
	zend_native_compile_diagnostic *product_diagnostic)
{
	const zend_mir_call_view *calls =
		zend_mir_module_get_call_view(function->module);
	uint32_t target_count = calls != NULL
		? calls->call_target_count(calls->context) : 0;
	uint32_t target_index;

	if (target_count != 0) {
		member->bindings = safe_emalloc(
			target_count, sizeof(*member->bindings), 0);
		member->internal_bindings = safe_emalloc(
			target_count, sizeof(*member->internal_bindings), 0);
		function->internal_call_cells = zend_native_compiler_alloc(
			compiler,
			target_count * sizeof(*function->internal_call_cells), true);
	}
	for (target_index = 0; target_index < target_count; target_index++) {
		zend_mir_call_target_ref target;
		zend_op_array *callee;
		zend_native_compiled_function *native_callee;

		if (!calls->call_target_at(calls->context, target_index, &target)) {
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
				return false;
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
					return false;
				}
			} else if (init_opline->opcode == ZEND_INIT_STATIC_METHOD_CALL) {
				receiver_kind = ZEND_NATIVE_INTERNAL_RECEIVER_CALLED_SCOPE;
				called_scope = internal->common.scope;
			} else if (init_opline->opcode != ZEND_INIT_FCALL
					&& init_opline->opcode != ZEND_NEW) {
				return false;
			}
			if (zend_native_internal_call_cell_init(
					&function->internal_call_cells[
						member->backend.internal_binding_count],
					internal, called_scope, receiver_kind) == FAILURE) {
				return false;
			}
			member->internal_bindings[
				member->backend.internal_binding_count].target_id = target.id;
			member->internal_bindings[
				member->backend.internal_binding_count].call_cell =
					&function->internal_call_cells[
						member->backend.internal_binding_count];
			member->backend.internal_binding_count++;
			continue;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DYNAMIC) {
			member->bindings[member->backend.user_binding_count].target_id =
				target.id;
			member->bindings[
				member->backend.user_binding_count].entry_cell =
					&function->entry_cell;
			member->bindings[
				member->backend.user_binding_count].component_target_index =
					UINT32_MAX;
			member->bindings[
				member->backend.user_binding_count].direct_native = false;
			member->bindings[
				member->backend.user_binding_count].leaf_scalar_frame = false;
			member->backend.user_binding_count++;
			continue;
		}
		callee = zend_native_compiler_resolve_native_target(
			compiler, function, calls, &target);
		native_callee = zend_native_compiler_find_function(compiler, callee);
		if (native_callee == NULL
				|| native_callee->state == ZEND_NATIVE_CODEUNIT_FAILED) {
			return false;
		}
		member->bindings[member->backend.user_binding_count].target_id =
			target.id;
		member->bindings[member->backend.user_binding_count].entry_cell =
			&native_callee->entry_cell;
		member->bindings[
			member->backend.user_binding_count].component_target_index =
				native_callee->registry_index < component_registry_count
					? component_member_by_registry[
						native_callee->registry_index]
					: UINT32_MAX;
		member->bindings[member->backend.user_binding_count].direct_native =
			zend_native_compiler_target_is_direct_native(
				compiler, function, calls, &target, callee);
		member->bindings[
			member->backend.user_binding_count].leaf_scalar_frame =
				member->bindings[
					member->backend.user_binding_count].direct_native
				&& zend_native_compiler_function_has_leaf_scalar_frame(
					compiler, native_callee);
		member->backend.user_binding_count++;
	}
	function->internal_call_cell_count =
		member->backend.internal_binding_count;
	member->backend.module =
		zend_native_compiler_module_view(compiler, function->module);
	member->backend.user_bindings = member->bindings;
	member->backend.internal_bindings = member->internal_bindings;
	member->backend.effects = function->source_effects;
	member->backend.effect_count = function->source_effect_count;
	member->backend.frame_argument_count = function->op_array->num_args;
	member->backend.source_op_array = function->op_array;
	member->backend.source_ssa = &function->ssa;
	return true;
}

static bool zend_native_compiler_compile_shared_component(
	zend_native_compiler *compiler,
	uint32_t component_id,
	zend_native_compile_diagnostic *product_diagnostic)
{
	zend_native_component_build_member *members;
	zend_native_component_member *backend_members = NULL;
	uint32_t *component_member_by_registry = NULL;
	zend_native_compiled_function *function;
	zend_native_image *image = NULL;
	zend_native_code *owner_code = NULL;
	zend_native_diagnostic diagnostic;
	const zend_native_runtime_api *runtime = zend_native_runtime_get();
	zend_native_runtime_api injected_runtime;
	zend_native_runtime_helper injected_helpers[
		ZEND_NATIVE_HELPER_COUNT - 1];
	uint32_t member_count = 0;
	uint32_t index = 0;
	uint32_t registry_index;
	zend_hrtime_t phase_started;

	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (function->entry_cell.state != ZEND_NATIVE_ENTRY_READY
				&& function->state != ZEND_NATIVE_CODEUNIT_FAILED) {
			member_count++;
		}
	}
	if (member_count == 0) {
		return true;
	}
	members = ecalloc(member_count, sizeof(*members));
	component_member_by_registry = safe_emalloc(
		compiler->function_count,
		sizeof(*component_member_by_registry), 0);
	for (registry_index = 0;
			registry_index < compiler->function_count; registry_index++) {
		component_member_by_registry[registry_index] = UINT32_MAX;
	}
	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY
				|| function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
			continue;
		}
		members[index].function = function;
		component_member_by_registry[function->registry_index] = index;
		index++;
	}
	ZEND_ASSERT(index == member_count);
	for (index = 0; index < member_count; index++) {
		if (!zend_native_compiler_prepare_component_member(
				compiler, members[index].function, &members[index],
				component_member_by_registry, compiler->function_count,
				product_diagnostic)) {
			goto failure;
		}
	}
	if (compiler->unavailable_runtime_helper != 0) {
		uint32_t helper_index;

		if (runtime->helper_count > ZEND_NATIVE_HELPER_COUNT - 1) {
			goto failure;
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
			goto failure;
		}
		injected_runtime = *runtime;
		injected_runtime.helpers = injected_helpers;
		runtime = &injected_runtime;
	}
	backend_members = safe_emalloc(
		member_count, sizeof(*backend_members), 0);
	for (index = 0; index < member_count; index++) {
		backend_members[index] = members[index].backend;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	phase_started = zend_hrtime();
	if (zend_tpde_compile_component_w14_with_runtime(
			compiler->target, backend_members, member_count,
			runtime, &image, &diagnostic) == FAILURE) {
		compiler->stats.codegen_ns += zend_hrtime() - phase_started;
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_CODEGEN, &diagnostic);
		goto failure;
	}
	compiler->stats.codegen_ns += zend_hrtime() - phase_started;
	members[0].function->image = image;
	for (index = 0; index < member_count; index++) {
		members[index].function->publish_pending = true;
		members[index].function->image_owner_index =
			members[0].function->registry_index;
		members[index].function->image_component_index = index;
	}
	{
		zend_native_image_metrics image_metrics;

		memset(&image_metrics, 0, sizeof(image_metrics));
		zend_native_image_get_metrics(image, &image_metrics);
		compiler->stats.native_code_bytes += zend_native_image_size(image);
		compiler->stats.runtime_helper_sites +=
			image_metrics.runtime_helper_sites;
		compiler->stats.source_opline_decode_sites +=
			image_metrics.source_opline_decode_sites;
		compiler->stats.guard_sites += image_metrics.guard_sites;
		compiler->stats.slow_path_sites += image_metrics.slow_path_sites;
		compiler->stats.direct_call_sites += image_metrics.direct_call_sites;
		compiler->stats.direct_leaf_scalar_sites +=
			image_metrics.direct_leaf_scalar_sites;
		compiler->stats.direct_typed_body_sites +=
			image_metrics.direct_typed_body_sites;
		compiler->stats.direct_call_frame_bytes +=
			image_metrics.direct_call_frame_bytes;
		compiler->stats.inner_call_runtime_helper_calls +=
			image_metrics.inner_call_runtime_helper_calls;
		compiler->stats.inner_call_heap_allocations +=
			image_metrics.inner_call_heap_allocations;
		compiler->stats.inner_call_catcher_boundaries +=
			image_metrics.inner_call_catcher_boundaries;
	}
	if (compiler->defer_publication) {
		function = compiler->component_heads[component_id];
		while (function != NULL) {
			zend_native_compiled_function *next =
				function->next_component_member;

			if (function->publish_pending) {
				function->state = ZEND_NATIVE_CODEUNIT_IMAGE_READY;
				function->publish_pending = false;
			}
			function->component_id = 0;
			function->next_component_member = NULL;
			function = next;
		}
		compiler->component_heads[component_id] = NULL;
		efree(backend_members);
		efree(component_member_by_registry);
		zend_native_compiler_free_component_build(
			members, member_count);
		return true;
	}
	if (compiler->fault == ZEND_NATIVE_COMPILE_FAULT_MAPPING) {
		memset(&diagnostic, 0, sizeof(diagnostic));
		diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
		snprintf(diagnostic.message, sizeof(diagnostic.message),
			"injected native mapping failure");
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
		goto failure;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	phase_started = zend_hrtime();
	if (zend_native_publish_image(
			compiler->target, image, &owner_code,
			&diagnostic) == FAILURE) {
		compiler->stats.publish_ns += zend_hrtime() - phase_started;
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
		goto failure;
	}
	compiler->stats.publish_ns += zend_hrtime() - phase_started;
	members[0].function->code = owner_code;
	for (index = 1; index < member_count; index++) {
		memset(&diagnostic, 0, sizeof(diagnostic));
		if (zend_native_code_component_view(
				owner_code, index, &members[index].function->code,
				&diagnostic) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, &diagnostic);
			goto failure;
		}
	}
	for (index = 0; index < member_count; index++) {
		if (zend_native_code_is_writable(members[index].function->code)
				|| !zend_native_code_is_executable(
					members[index].function->code)) {
			zend_native_compiler_set_diagnostic(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
				ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
				"published component violates the W^X contract");
			goto failure;
		}
	}
	if (compiler->fault == ZEND_NATIVE_COMPILE_FAULT_ENTRY_PUBLISH) {
		zend_native_compiler_set_diagnostic(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
			ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED,
			"injected entry-cell publication failure");
		goto failure;
	}
	if (!zend_native_compiler_reserve_publications(
			compiler, member_count)) {
		zend_native_compiler_set_diagnostic(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native publication index allocation failed");
		goto failure;
	}
	for (index = 0; index < member_count; index++) {
		if (members[index].function->entry_cell.state
				!= ZEND_NATIVE_ENTRY_COMPILING
				|| zend_native_entry_cell_publish(
					&members[index].function->entry_cell,
					members[index].function->code) == FAILURE) {
			goto failure;
		}
	}
	for (index = 0; index < member_count; index++) {
		zend_native_compiler_record_publication(
			compiler, members[index].function);
	}
	for (index = 0; index < member_count; index++) {
		members[index].function->state = ZEND_NATIVE_CODEUNIT_READY;
		members[index].function->publish_pending = false;
	}
	function = compiler->component_heads[component_id];
	while (function != NULL) {
		zend_native_compiled_function *next =
			function->next_component_member;

		function->component_id = 0;
		function->next_component_member = NULL;
		function = next;
	}
	compiler->component_heads[component_id] = NULL;
	compiler->published_component_count++;
	efree(backend_members);
	efree(component_member_by_registry);
	zend_native_compiler_free_component_build(members, member_count);
	return true;

failure:
	if (product_diagnostic == NULL || product_diagnostic->message[0] == '\0') {
		zend_native_compiler_backend_failure(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_CODEGEN, NULL);
	}
	efree(backend_members);
	efree(component_member_by_registry);
	zend_native_compiler_free_component_build(members, member_count);
	zend_native_compiler_fail_pending_component(compiler, component_id);
	return false;
}

static bool zend_native_compiler_compile_native_component(
	zend_native_compiler *compiler,
	uint32_t component_id,
	zend_native_compile_diagnostic *product_diagnostic)
{
	zend_native_diagnostic diagnostic;
	zend_native_compiled_function *function;
	uint32_t publication_batch_count = 0;

	if (component_id != 0
			&& component_id < compiler->component_head_capacity) {
		return zend_native_compiler_compile_shared_component(
			compiler, component_id, product_diagnostic);
	}

	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY) {
			continue;
		}
		if (function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
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
			function->internal_call_cells = zend_native_compiler_alloc(
				compiler,
				target_count * sizeof(*function->internal_call_cells),
				true);
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
				} else if (init_opline->opcode != ZEND_INIT_FCALL
						&& init_opline->opcode != ZEND_NEW) {
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
				bindings[binding_count].component_target_index = UINT32_MAX;
				bindings[binding_count].direct_native = false;
				bindings[binding_count].leaf_scalar_frame = false;
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
			bindings[binding_count].component_target_index = UINT32_MAX;
			bindings[binding_count].direct_native =
				zend_native_compiler_target_is_direct_native(
					compiler, function, calls, &target, callee);
			bindings[binding_count].leaf_scalar_frame =
				bindings[binding_count].direct_native
				&& zend_native_compiler_function_has_leaf_scalar_frame(
					compiler, native_callee);
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
				&function->ssa,
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
		compiler->stats.direct_leaf_scalar_sites +=
			image_metrics.direct_leaf_scalar_sites;
		compiler->stats.direct_typed_body_sites +=
			image_metrics.direct_typed_body_sites;
		compiler->stats.direct_call_frame_bytes +=
			image_metrics.direct_call_frame_bytes;
		compiler->stats.inner_call_runtime_helper_calls +=
			image_metrics.inner_call_runtime_helper_calls;
		compiler->stats.inner_call_heap_allocations +=
			image_metrics.inner_call_heap_allocations;
		compiler->stats.inner_call_catcher_boundaries +=
			image_metrics.inner_call_catcher_boundaries;
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

	if (compiler->defer_publication) {
		function = compiler->component_heads[component_id];
		while (function != NULL) {
			zend_native_compiled_function *next =
				function->next_component_member;

			if (function->publish_pending) {
				function->state = ZEND_NATIVE_CODEUNIT_IMAGE_READY;
				function->publish_pending = false;
			}
			function->component_id = 0;
			function->next_component_member = NULL;
			function = next;
		}
		compiler->component_heads[component_id] = NULL;
		return true;
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
	/*
	 * Validate the complete component before making its first entry visible.
	 * With the compiler mutation lock held, no entry-cell state can change
	 * between this preflight and the infallible release stores below.
	 */
	for (function = compiler->component_heads[component_id];
			function != NULL;
			function = function->next_component_member) {
		if (function->publish_pending
				&& (function->code == NULL
					|| function->entry_cell.state
						!= ZEND_NATIVE_ENTRY_COMPILING)) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, NULL);
			zend_native_compiler_fail_pending_component(
				compiler, component_id);
			return false;
		}
		if (function->publish_pending) {
			publication_batch_count++;
		}
	}
	if (!zend_native_compiler_reserve_publications(
			compiler, publication_batch_count)) {
		zend_native_compiler_set_diagnostic(
			compiler, product_diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native publication index allocation failed");
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
		if (zend_native_entry_cell_publish(
				&function->entry_cell, function->code) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, product_diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH, NULL);
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
			zend_native_compiler_record_publication(compiler, function);
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

static zend_result zend_native_compiler_compile_locked(
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
	/*
	 * Runtime declarations extend the script after compiler creation. Index
	 * the selected root and its nested definitions before lowering so a
	 * request-local Closure op_array can recover the source-backed call,
	 * branch and value metadata owned by its declaration.
	 */
	root = zend_native_compiler_retain_runtime_source(compiler, root);
	if (root == NULL || !zend_native_compiler_index_source_op_array(
			compiler, root, 0)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic,
			ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native source codeunit index cannot be extended");
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
	if (root_function != NULL && compiler->defer_publication
			&& root_function->state == ZEND_NATIVE_CODEUNIT_IMAGE_READY) {
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

		if (function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
			continue;
		}
		if (function->entry_cell.state == ZEND_NATIVE_ENTRY_READY) {
			continue;
		}
		if (function->state == ZEND_NATIVE_CODEUNIT_IMAGE_READY) {
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
				&& !zend_native_compiler_index_call_sites(
					compiler, function)) {
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
	if (!zend_native_compiler_assign_static_component(
			compiler, &component_count)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
			"native static callgraph component cannot be constructed");
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
			|| (compiler->defer_publication
				? root_function->state != ZEND_NATIVE_CODEUNIT_IMAGE_READY
				: root_function->entry_cell.state
					!= ZEND_NATIVE_ENTRY_READY)) {
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

zend_result zend_native_compiler_compile(
	zend_native_compiler *compiler,
	zend_op_array *root,
	const zend_mir_scalar_type_mask *supplied_argument_types,
	uint32_t supplied_argument_count,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_result result;

	zend_native_compiler_mutation_lock(compiler);
	result = zend_native_compiler_compile_locked(
		compiler, root, supplied_argument_types,
		supplied_argument_count, diagnostic);
	zend_native_compiler_mutation_unlock(compiler);
	return result;
}

static zend_native_entry_cell *
zend_native_compiler_prepare_op_array_locked(
	zend_native_compiler *compiler,
	zend_op_array *source_op_array,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiled_function *function;

	if (zend_native_compiler_compile_locked(
			compiler, source_op_array, NULL, 0, diagnostic) == FAILURE) {
		return NULL;
	}
	source_op_array = zend_native_compiler_canonical_reentry_op_array(
		compiler, source_op_array);
	function = zend_native_compiler_find_function(
		compiler, source_op_array);
	return function != NULL
			&& function->entry_cell.state == ZEND_NATIVE_ENTRY_READY
		? &function->entry_cell : NULL;
}

zend_native_entry_cell *zend_native_compiler_prepare_function(
	zend_native_compiler *compiler,
	zend_function *function,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_entry_cell *entry_cell = NULL;
	zend_op_array *source_op_array;

	if (compiler == NULL || function == NULL
			|| !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	zend_native_compiler_mutation_lock(compiler);
	source_op_array = zend_native_compiler_canonical_reentry_op_array(
		compiler, &function->op_array);
	if (source_op_array == NULL) {
		source_op_array = &function->op_array;
	}
	entry_cell = zend_native_compiler_prepare_op_array_locked(
		compiler, source_op_array, diagnostic);
	zend_native_compiler_mutation_unlock(compiler);
	return entry_cell;
}

zend_result zend_native_compiler_compile_dynamic_component(
	zend_native_compiler *compiler,
	zend_op_array *root,
	uint32_t first_function_bucket,
	uint32_t first_class_bucket,
	zend_native_compiler **component_compiler,
	uint32_t *first_compiled_function,
	zend_native_entry_cell **root_entry,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_compiled_function *compiled_root;
	uint64_t registered_codeunits;

	if (root_entry != NULL) {
		*root_entry = NULL;
	}
	if (component_compiler != NULL) {
		*component_compiler = NULL;
	}
	if (first_compiled_function != NULL) {
		*first_compiled_function = 0;
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
	if (compiler->persistent) {
		zend_native_compiler *request_compiler =
			zend_native_compiler_request_companion(
				compiler, root, diagnostic);

		if (request_compiler == NULL) {
			return FAILURE;
		}
		return zend_native_compiler_compile_dynamic_component(
			request_compiler, root, first_function_bucket,
			first_class_bucket, component_compiler,
			first_compiled_function, root_entry, diagnostic);
	}
	if (compiler->direct_reentry
			&& compiler->script->function_table.pDestructor != NULL
			&& compiler->script->class_table.pDestructor != NULL) {
		/*
		 * Dynamic include/eval may grow the request symbol tables before it
		 * reenters a compiler that borrows their storage. Refresh those views
		 * and the matching symbolic function index as one snapshot, so
		 * compilation never reads released HashTable storage or resolves a new
		 * declaration through an older index. Owner-script tables have no
		 * element destructors; keep those independent tables intact.
		 */
		compiler->script->function_table = *EG(function_table);
		compiler->script->class_table = *EG(class_table);
		if (!zend_native_compiler_index_script_functions(compiler)) {
			zend_native_compiler_set_diagnostic(
				compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
				ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
				"native script function index cannot be refreshed");
			return FAILURE;
		}
	}
	if (component_compiler != NULL) {
		*component_compiler = compiler;
	}
	if (first_compiled_function != NULL) {
		*first_compiled_function = compiler->function_count;
	}
	registered_codeunits = zend_native_compiler_dynamic_codeunit_count(
		first_function_bucket, first_class_bucket);
	if (zend_native_compiler_compile(
			compiler, root, NULL, 0, diagnostic) == FAILURE) {
		return FAILURE;
	}
	root = zend_native_compiler_canonical_reentry_op_array(compiler, root);
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
	zend_native_entry_cell *entry_cell;
	zend_op_array *source_op_array;
	zend_native_compile_diagnostic diagnostic;
	zend_native_external_reentry_resolver_t external_resolver;
	void *external_context;
	uint32_t first_compiled_function;

	if (compiler == NULL || resolved == NULL
			|| !ZEND_USER_CODE(resolved->type)) {
		return NULL;
	}
	memset(&diagnostic, 0, sizeof(diagnostic));
	zend_native_compiler_mutation_lock(compiler);
	first_compiled_function = compiler->function_count;
	source_op_array = zend_native_compiler_canonical_reentry_op_array(
		compiler, &resolved->op_array);
	if (source_op_array == NULL) {
		if (compiler->external_reentry_resolver != NULL) {
			external_resolver = compiler->external_reentry_resolver;
			external_context = compiler->external_reentry_context;
			zend_native_compiler_mutation_unlock(compiler);
			return external_resolver(external_context, resolved);
		}
		/* The exact Zend function is the owner identity for a codeunit that
		 * became visible only after runtime declaration or autoload. */
		source_op_array = &resolved->op_array;
	}
	entry_cell = zend_native_compiler_prepare_op_array_locked(
		compiler, source_op_array, &diagnostic);
	zend_native_compiler_mutation_unlock(compiler);
	if (entry_cell != NULL) {
		/* Lazy reentry publishes immutable code before returning the entry cell.
		 * Keep that code and its stable cell, but do not charge request memory for
		 * SSA, MIR and image data that execution can no longer observe. */
		zend_native_compiler_release_ready_transients(
			compiler, first_compiled_function);
	}
	if (entry_cell == NULL) {
		if (EG(exception) == NULL) {
			zend_throw_error(NULL, "%s",
				diagnostic.message[0] != '\0'
					? diagnostic.message
					: "Native codeunit compilation failed");
		}
		return NULL;
	}
	return entry_cell;
}

zend_native_entry_cell *zend_native_compiler_lookup(
	const zend_native_compiler *compiler, const zend_function *function)
{
	zend_native_compiled_function *compiled;
	zend_op_array *source_op_array;
	zend_native_entry_cell *entry_cell;

	if (compiler == NULL || function == NULL
			|| !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	zend_native_compiler_mutation_lock(compiler);
	source_op_array = zend_hash_index_find_ptr(
		&compiler->source_op_arrays_by_opcodes,
		(zend_ulong) (uintptr_t) function->op_array.opcodes);
	compiled = zend_native_compiler_find_function(
		compiler,
		source_op_array != NULL
				&& source_op_array->last == function->op_array.last
			? source_op_array : &function->op_array);
	entry_cell = compiled != NULL
			&& compiled->entry_cell.state == ZEND_NATIVE_ENTRY_READY
		? &compiled->entry_cell : NULL;
	zend_native_compiler_mutation_unlock(compiler);
	return entry_cell;
}

zend_result zend_native_compiler_snapshot_publication_delta(
	const zend_native_compiler *compiler, uint32_t first_index,
	zend_op_array **op_arrays, uint32_t capacity, uint32_t *ready_count,
	uint32_t *next_index)
{
	uint32_t count;
	zend_result result;

	if (compiler == NULL || ready_count == NULL || next_index == NULL
			|| (capacity != 0 && op_arrays == NULL)) {
		return FAILURE;
	}
	zend_native_compiler_mutation_lock(compiler);
	if (first_index > compiler->publication_count) {
		zend_native_compiler_mutation_unlock(compiler);
		return FAILURE;
	}
	count = compiler->publication_count - first_index;
	*ready_count = count;
	*next_index = compiler->publication_count;
	result = count <= capacity ? SUCCESS : FAILURE;
	if (result == SUCCESS && count != 0) {
		memcpy(op_arrays, compiler->publication_log + first_index,
			count * sizeof(*op_arrays));
	}
	zend_native_compiler_mutation_unlock(compiler);
	return result;
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
	if (compiler->persistent) {
		zend_native_compiler_session *session =
			zend_native_compiler_session_find(compiler);

		stats->compile_ns = __atomic_load_n(
			&compiler->stats.compile_ns, __ATOMIC_RELAXED);
		stats->ssa_ns = compiler->stats.ssa_ns;
		stats->lowering_ns = compiler->stats.lowering_ns;
		stats->codegen_ns = compiler->stats.codegen_ns;
		stats->publish_ns = compiler->stats.publish_ns;
		stats->native_code_bytes =
			compiler->stats.native_code_bytes;
		stats->runtime_helper_sites =
			compiler->stats.runtime_helper_sites;
		stats->source_opline_decode_sites =
			compiler->stats.source_opline_decode_sites;
		stats->guard_sites = compiler->stats.guard_sites;
		stats->slow_path_sites = compiler->stats.slow_path_sites;
		stats->direct_call_sites =
			compiler->stats.direct_call_sites;
		stats->direct_leaf_scalar_sites =
			compiler->stats.direct_leaf_scalar_sites;
		stats->direct_typed_body_sites =
			compiler->stats.direct_typed_body_sites;
		stats->direct_call_frame_bytes =
			compiler->stats.direct_call_frame_bytes;
		stats->inner_call_runtime_helper_calls =
			compiler->stats.inner_call_runtime_helper_calls;
		stats->inner_call_heap_allocations =
			compiler->stats.inner_call_heap_allocations;
		stats->inner_call_catcher_boundaries =
			compiler->stats.inner_call_catcher_boundaries;
		stats->registered_codeunits =
			compiler->stats.registered_codeunits;
		stats->execute_ns = __atomic_load_n(
			&compiler->stats.execute_ns, __ATOMIC_RELAXED);
		stats->first_execute_ns = __atomic_load_n(
			&compiler->stats.first_execute_ns, __ATOMIC_RELAXED);
		stats->last_execute_ns = __atomic_load_n(
			&compiler->stats.last_execute_ns, __ATOMIC_RELAXED);
		stats->executions = __atomic_load_n(
			&compiler->stats.executions, __ATOMIC_RELAXED);
		if (session != NULL) {
			stats->execute_ns += session->execute_ns;
			stats->last_execute_ns = session->executions != 0
				? session->last_execute_ns : stats->last_execute_ns;
			if (stats->executions == 0
					&& session->executions != 0) {
				stats->first_execute_ns =
					session->first_execute_ns;
			}
			stats->executions += session->executions;
		}
	} else {
		*stats = compiler->stats;
	}
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
	zend_native_compiler_session *session =
		zend_native_compiler_session_get(compiler);
	zend_result entered;

	if (session == NULL || session->reentry_active
			|| session->dynamic_compiler_active) {
		return FAILURE;
	}
	memset(&session->reentry_scope, 0, sizeof(session->reentry_scope));
	if (compiler->direct_reentry) {
		entered = zend_native_reentry_scope_enter_resolver_direct(
			&session->reentry_scope, NULL, 0,
			zend_native_compiler_resolve_reentry, compiler);
	} else {
		uint32_t binding_count = 0;

		zend_native_compiler_mutation_lock(compiler);
		if (compiler->function_count == 0) {
			zend_native_compiler_mutation_unlock(compiler);
			return FAILURE;
		}
		if (session->reentry_binding_function_count
				!= compiler->function_count) {
			if (session->reentry_binding_capacity
					< compiler->function_count) {
				session->reentry_bindings = safe_erealloc(
					session->reentry_bindings,
					compiler->function_count,
					sizeof(*session->reentry_bindings), 0);
				session->reentry_binding_capacity =
					compiler->function_count;
			}
			for (uint32_t index = 0;
					index < compiler->function_count; index++) {
				zend_native_compiled_function *function =
					compiler->functions[index];

				if (function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
					continue;
				}
				if (function->entry_cell.state
						!= ZEND_NATIVE_ENTRY_READY) {
					zend_native_compiler_mutation_unlock(compiler);
					return FAILURE;
				}
				session->reentry_bindings[binding_count].function =
					(zend_function *) function->op_array;
				session->reentry_bindings[binding_count].entry_cell =
					&function->entry_cell;
				binding_count++;
			}
			session->reentry_binding_count = binding_count;
			session->reentry_binding_function_count =
				compiler->function_count;
		}
		zend_native_compiler_mutation_unlock(compiler);
		entered = zend_native_reentry_scope_enter_resolver(
			&session->reentry_scope, session->reentry_bindings,
			session->reentry_binding_count,
			zend_native_compiler_resolve_reentry, compiler);
	}
	if (entered == FAILURE) {
		return FAILURE;
	}
	session->reentry_active = true;
	zend_native_dynamic_compiler_activate(&session->dynamic_compiler);
	session->dynamic_compiler_active = true;
	return SUCCESS;
}

static void zend_native_compiler_leave(zend_native_compiler *compiler)
{
	zend_native_compiler_session *session =
		zend_native_compiler_session_find(compiler);

	if (session == NULL) {
		return;
	}
	if (session->dynamic_compiler_active) {
		zend_native_dynamic_compiler_deactivate(
			&session->dynamic_compiler);
		session->dynamic_compiler_active = false;
	}
	if (session->reentry_active) {
		zend_native_reentry_scope_leave(&session->reentry_scope);
		session->reentry_active = false;
	}
}

zend_result zend_native_compiler_activate_session(
	zend_native_compiler *compiler)
{
	return zend_native_compiler_enter(compiler);
}

void zend_native_compiler_deactivate_session(
	zend_native_compiler *compiler)
{
	zend_native_compiler_leave(compiler);
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
	const zend_native_code *code;
	zend_execute_data *previous;
	zend_execute_data *frame;
	zval receiver;
	void *object_or_called_scope = NULL;
	bool top_code;
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
	top_code = function->common.function_name == NULL;
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		if (argument == NULL) {
			return ZEND_NATIVE_EXCEPTION;
		}
	}
	memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
	phase_started = zend_hrtime();
	entry_cell = zend_native_compiler_prepare_function(
		compiler, function, &compile_diagnostic);
	__atomic_fetch_add(
		&compiler->stats.compile_ns, zend_hrtime() - phase_started,
		__ATOMIC_RELAXED);
	if (entry_cell == NULL) {
		if (diagnostic != NULL) {
			diagnostic->code = ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR;
			snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
				compile_diagnostic.message);
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	if (entry_cell == NULL
			|| (code = zend_native_entry_cell_load(entry_cell)) == NULL
			|| zend_native_compiler_enter(compiler) == FAILURE) {
		return ZEND_NATIVE_EXCEPTION;
	}
	ZVAL_UNDEF(&receiver);
	if (top_code) {
		object_or_called_scope = zend_get_this_object(EG(current_execute_data));
		if (object_or_called_scope != NULL) {
			call_info = ZEND_CALL_TOP_CODE
				| ZEND_CALL_HAS_SYMBOL_TABLE | ZEND_CALL_HAS_THIS;
		} else {
			object_or_called_scope =
				zend_get_called_scope(EG(current_execute_data));
			call_info = ZEND_CALL_TOP_CODE | ZEND_CALL_HAS_SYMBOL_TABLE;
		}
	} else if (function->common.scope != NULL) {
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
	if (top_code) {
		frame->symbol_table = previous != NULL
			? zend_rebuild_symbol_table() : &EG(symbol_table);
	}
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		ZVAL_COPY(ZEND_CALL_ARG(frame, index + 1), argument);
	}
	ZVAL_UNDEF(result);
	if (top_code) {
		zend_init_code_execute_data(frame, &function->op_array, result);
	} else {
		zend_init_func_execute_data(frame, &function->op_array, result);
	}
	phase_started = zend_hrtime();
	status = zend_native_execute_frame(code, frame, diagnostic);
	elapsed = zend_hrtime() - phase_started;
	zend_native_compiler_session_record_execution(compiler, elapsed);
	EG(current_execute_data) = previous;
	zend_vm_stack_free_call_frame(frame);
	if (!Z_ISUNDEF(receiver)) {
		zval_ptr_dtor(&receiver);
	}
	zend_native_compiler_leave(compiler);
	return status;
}

static zend_native_status zend_native_compiler_execute_active_impl(
	zend_native_compiler *compiler,
	zend_native_entry_cell *entry_cell,
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic,
	bool observer_already_started)
{
	zend_execute_data *previous;
	zend_native_status status;
	zend_hrtime_t phase_started;
	uint64_t elapsed;

	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (compiler == NULL || execute_data == NULL
			|| execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)
			|| entry_cell == NULL || code == NULL) {
		return ZEND_NATIVE_EXCEPTION;
	}
	previous = execute_data->prev_execute_data;
	EG(current_execute_data) = execute_data;
	zend_native_entry_cell_retain_active(entry_cell);
	phase_started = zend_hrtime();
	status = observer_already_started
		? zend_native_execute_observed_frame(
			code, execute_data, diagnostic)
		: zend_native_execute_frame(
			code, execute_data, diagnostic);
	elapsed = zend_hrtime() - phase_started;
	zend_native_entry_cell_release_active(entry_cell);
	EG(current_execute_data) = previous;
	zend_native_compiler_session_record_execution(compiler, elapsed);
	return status;
}

static zend_native_status zend_native_compiler_execute_published_impl(
	zend_native_compiler *compiler,
	zend_native_entry_cell *entry_cell,
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic,
	bool observer_already_started)
{
	zend_native_status status;

	if (zend_native_compiler_enter(compiler) == FAILURE) {
		return ZEND_NATIVE_EXCEPTION;
	}
	status = zend_native_compiler_execute_active_impl(
		compiler, entry_cell, code, execute_data, diagnostic,
		observer_already_started);
	zend_native_compiler_leave(compiler);
	return status;
}

zend_native_status zend_native_compiler_execute_published(
	zend_native_compiler *compiler,
	zend_native_entry_cell *entry_cell,
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_compiler_execute_published_impl(
		compiler, entry_cell, code, execute_data, diagnostic, false);
}

zend_native_status zend_native_compiler_execute_observed_published(
	zend_native_compiler *compiler,
	zend_native_entry_cell *entry_cell,
	const zend_native_code *code,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_compiler_execute_published_impl(
		compiler, entry_cell, code, execute_data, diagnostic, true);
}

zend_native_status zend_native_compiler_execute_entry(
	zend_native_compiler *compiler,
	zend_native_entry_cell *entry_cell,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_compiler_execute_published(
		compiler, entry_cell, zend_native_entry_cell_load(entry_cell),
		execute_data, diagnostic);
}

static zend_native_status zend_native_compiler_execute_data_impl(
	zend_native_compiler *compiler,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic,
	bool observer_already_started)
{
	zend_native_compile_diagnostic compile_diagnostic;
	zend_native_entry_cell *entry_cell;
	zend_hrtime_t phase_started;

	if (diagnostic != NULL) {
		memset(diagnostic, 0, sizeof(*diagnostic));
	}
	if (compiler == NULL || execute_data == NULL
			|| execute_data->func == NULL
			|| !ZEND_USER_CODE(execute_data->func->type)) {
		return ZEND_NATIVE_EXCEPTION;
	}
	memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
	phase_started = zend_hrtime();
	entry_cell = zend_native_compiler_prepare_function(
		compiler, execute_data->func, &compile_diagnostic);
	__atomic_fetch_add(
		&compiler->stats.compile_ns, zend_hrtime() - phase_started,
		__ATOMIC_RELAXED);
	if (entry_cell == NULL) {
		if (diagnostic != NULL) {
			diagnostic->code = ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR;
			snprintf(diagnostic->message, sizeof(diagnostic->message), "%s",
				compile_diagnostic.message);
		}
		return ZEND_NATIVE_EXCEPTION;
	}
	return zend_native_compiler_execute_published_impl(
		compiler, entry_cell, zend_native_entry_cell_load(entry_cell),
		execute_data, diagnostic, observer_already_started);
}

zend_native_status zend_native_compiler_execute_data(
	zend_native_compiler *compiler,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_compiler_execute_data_impl(
		compiler, execute_data, diagnostic, false);
}

zend_native_status zend_native_compiler_execute_observed_data(
	zend_native_compiler *compiler,
	zend_execute_data *execute_data,
	zend_native_diagnostic *diagnostic)
{
	return zend_native_compiler_execute_data_impl(
		compiler, execute_data, diagnostic, true);
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
	compiler->script_functions_by_declaration_id =
		zend_native_compiler_realloc(
			compiler,
			compiler->script_functions_by_declaration_id,
			count + 1,
			sizeof(*compiler->script_functions_by_declaration_id));
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
	compiler = pecalloc(1, sizeof(*compiler), config->persistent);
	compiler->persistent = config->persistent;
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
	compiler->source_probe = config->source_probe;
	compiler->defer_publication = config->defer_publication;
	compiler->direct_reentry = config->direct_reentry;
	compiler->external_reentry_resolver =
		config->external_reentry_resolver;
	compiler->external_reentry_context =
		config->external_reentry_context;
	zend_hash_init(
		&compiler->source_op_arrays_by_opcodes, 32, NULL, NULL,
		compiler->persistent);
	if (!zend_native_compiler_index_source_op_arrays(compiler)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native source codeunit index cannot be constructed");
		zend_hash_destroy(&compiler->source_op_arrays_by_opcodes);
		pefree(compiler, compiler->persistent);
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
		pefree(compiler, compiler->persistent);
		return NULL;
	}
	zend_hash_init(
		&compiler->functions_by_op_array, 8, NULL, NULL,
		compiler->persistent);
#ifdef ZTS
	if (compiler->persistent) {
		compiler->mutation_mutex = tsrm_mutex_alloc();
		if (compiler->mutation_mutex == NULL) {
			zend_hash_destroy(&compiler->functions_by_op_array);
			zend_native_compiler_free(
				compiler,
				compiler->script_functions_by_declaration_id);
			zend_hash_destroy(
				&compiler->source_op_arrays_by_opcodes);
			pefree(compiler, true);
			return NULL;
		}
	}
#endif
	return compiler;
}

#define ZEND_NATIVE_BUNDLE_MAGIC UINT64_C(0x0033314c444e425a)
#define ZEND_NATIVE_BUNDLE_FORMAT 2u
#define ZEND_NATIVE_BUNDLE_MAX_BYTES (UINT64_C(1) << 28)

typedef enum _zend_native_bundle_reference_type {
	ZEND_NATIVE_BUNDLE_REFERENCE_USER_FUNCTION = 1,
	ZEND_NATIVE_BUNDLE_REFERENCE_INTERNAL_FUNCTION = 2,
	ZEND_NATIVE_BUNDLE_REFERENCE_CLASS = 3
} zend_native_bundle_reference_type;

typedef struct _zend_native_bundle_header {
	uint64_t magic;
	uint32_t format;
	uint32_t target;
	uint32_t runtime_abi;
	uint32_t function_count;
	uint32_t reference_count;
	uint32_t reserved;
	uint64_t function_offset;
	uint64_t reference_offset;
	uint64_t string_offset;
	uint64_t image_offset;
	uint64_t total_size;
	uint64_t checksum;
} zend_native_bundle_header;

typedef struct _zend_native_bundle_function_record {
	uint32_t source_ordinal;
	uint32_t image_owner_index;
	uint32_t image_component_index;
	uint32_t reserved;
	uint64_t image_offset;
	uint64_t image_size;
} zend_native_bundle_function_record;

typedef struct _zend_native_bundle_reference_record {
	uint32_t type;
	uint32_t source_ordinal;
	uint32_t name_length;
	uint32_t scope_length;
	uint64_t name_offset;
	uint64_t scope_offset;
} zend_native_bundle_reference_record;

typedef struct _zend_native_bundle_reference {
	zend_native_bundle_reference_type type;
	uint32_t source_ordinal;
	const zend_string *name;
	const zend_string *scope_name;
} zend_native_bundle_reference;

typedef struct _zend_native_bundle_builder {
	zend_native_compiler *compiler;
	zend_native_bundle_reference *references;
	uint32_t reference_count;
	uint32_t reference_capacity;
} zend_native_bundle_builder;

typedef struct _zend_native_bundle_decoder {
	zend_native_compiler *compiler;
	const unsigned char *bytes;
	size_t size;
	const zend_native_bundle_reference_record *references;
	uint32_t reference_count;
} zend_native_bundle_decoder;

static bool zend_native_bundle_source_ordinal_op_array(
	zend_op_array *op_array, const zend_op_array *target,
	uint32_t *cursor, uint32_t *ordinal, uint32_t depth)
{
	uint32_t index;

	if (op_array == NULL || target == NULL || cursor == NULL
			|| ordinal == NULL || depth > 64) {
		return false;
	}
	if (op_array == target) {
		*ordinal = *cursor;
		return true;
	}
	if (*cursor == UINT32_MAX) {
		return false;
	}
	(*cursor)++;
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		if (zend_native_bundle_source_ordinal_op_array(
				op_array->dynamic_func_defs[index], target,
				cursor, ordinal, depth + 1)) {
			return true;
		}
	}
	return false;
}

static bool zend_native_bundle_source_ordinal(
	zend_script *script, const zend_op_array *target, uint32_t *ordinal)
{
	zend_function *function;
	zend_class_entry *class_entry;
	zend_property_info *property_info;
	uint32_t hook_index;
	uint32_t cursor = 0;

	if (script == NULL || target == NULL || ordinal == NULL) {
		return false;
	}
	if (zend_native_bundle_source_ordinal_op_array(
			&script->main_op_array, target, &cursor, ordinal, 0)) {
		return true;
	}
	ZEND_HASH_FOREACH_PTR(&script->function_table, function) {
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& zend_native_bundle_source_ordinal_op_array(
					&function->op_array, target,
					&cursor, ordinal, 0)) {
			return true;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&script->class_table, class_entry) {
		if (class_entry == NULL) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
			if (function != NULL && function->type == ZEND_USER_FUNCTION
					&& zend_native_bundle_source_ordinal_op_array(
						&function->op_array, target,
						&cursor, ordinal, 0)) {
				return true;
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
			for (hook_index = 0;
					hook_index < ZEND_PROPERTY_HOOK_COUNT; hook_index++) {
				function = property_info->hooks[hook_index];
				if (function != NULL
						&& function->type == ZEND_USER_FUNCTION
						&& zend_native_bundle_source_ordinal_op_array(
							&function->op_array, target,
							&cursor, ordinal, 0)) {
					return true;
				}
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();
	return false;
}

static zend_op_array *zend_native_bundle_source_at_op_array(
	zend_op_array *op_array, uint32_t wanted,
	uint32_t *cursor, uint32_t depth)
{
	uint32_t index;
	zend_op_array *found;

	if (op_array == NULL || cursor == NULL || depth > 64) {
		return NULL;
	}
	if (*cursor == wanted) {
		return op_array;
	}
	if (*cursor == UINT32_MAX) {
		return NULL;
	}
	(*cursor)++;
	for (index = 0; index < op_array->num_dynamic_func_defs; index++) {
		found = zend_native_bundle_source_at_op_array(
			op_array->dynamic_func_defs[index], wanted,
			cursor, depth + 1);
		if (found != NULL) {
			return found;
		}
	}
	return NULL;
}

static zend_op_array *zend_native_bundle_source_at(
	zend_script *script, uint32_t wanted)
{
	zend_function *function;
	zend_class_entry *class_entry;
	zend_property_info *property_info;
	uint32_t hook_index;
	uint32_t cursor = 0;
	zend_op_array *found;

	if (script == NULL) {
		return NULL;
	}
	found = zend_native_bundle_source_at_op_array(
		&script->main_op_array, wanted, &cursor, 0);
	if (found != NULL) {
		return found;
	}
	ZEND_HASH_FOREACH_PTR(&script->function_table, function) {
		if (function == NULL || function->type != ZEND_USER_FUNCTION) {
			continue;
		}
		found = zend_native_bundle_source_at_op_array(
			&function->op_array, wanted, &cursor, 0);
		if (found != NULL) {
			return found;
		}
	} ZEND_HASH_FOREACH_END();
	ZEND_HASH_FOREACH_PTR(&script->class_table, class_entry) {
		if (class_entry == NULL) {
			continue;
		}
		ZEND_HASH_FOREACH_PTR(&class_entry->function_table, function) {
			if (function == NULL || function->type != ZEND_USER_FUNCTION) {
				continue;
			}
			found = zend_native_bundle_source_at_op_array(
				&function->op_array, wanted, &cursor, 0);
			if (found != NULL) {
				return found;
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
			for (hook_index = 0;
					hook_index < ZEND_PROPERTY_HOOK_COUNT; hook_index++) {
				function = property_info->hooks[hook_index];
				if (function == NULL
						|| function->type != ZEND_USER_FUNCTION) {
					continue;
				}
				found = zend_native_bundle_source_at_op_array(
					&function->op_array, wanted, &cursor, 0);
				if (found != NULL) {
					return found;
				}
			}
		} ZEND_HASH_FOREACH_END();
	} ZEND_HASH_FOREACH_END();
	return NULL;
}

static uint64_t zend_native_bundle_checksum(
	const unsigned char *bytes, size_t size)
{
	uint64_t hash = UINT64_C(1469598103934665603);
	size_t checksum_offset =
		offsetof(zend_native_bundle_header, checksum);
	size_t index;

	for (index = 0; index < size; index++) {
		unsigned char value =
			index >= checksum_offset
				&& index < checksum_offset + sizeof(uint64_t)
			? 0 : bytes[index];
		hash ^= value;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static bool zend_native_bundle_reference_matches(
	const zend_native_bundle_reference *reference,
	zend_native_bundle_reference_type type,
	uint32_t source_ordinal,
	const zend_string *name,
	const zend_string *scope_name)
{
	return reference->type == type
		&& reference->source_ordinal == source_ordinal
		&& (reference->name == name
			|| (reference->name != NULL && name != NULL
				&& zend_string_equals(reference->name, name)))
		&& (reference->scope_name == scope_name
			|| (reference->scope_name != NULL && scope_name != NULL
				&& zend_string_equals(reference->scope_name, scope_name)));
}

static bool zend_native_bundle_add_reference(
	zend_native_bundle_builder *builder,
	zend_native_bundle_reference_type type,
	uint32_t source_ordinal,
	const zend_string *name,
	const zend_string *scope_name,
	uint64_t *token)
{
	uint32_t index;

	for (index = 0; index < builder->reference_count; index++) {
		if (zend_native_bundle_reference_matches(
				&builder->references[index], type, source_ordinal,
				name, scope_name)) {
			*token = (uint64_t) index + 1;
			return true;
		}
	}
	if (builder->reference_count == UINT32_MAX) {
		return false;
	}
	if (builder->reference_count == builder->reference_capacity) {
		uint32_t capacity = builder->reference_capacity == 0
			? 16 : builder->reference_capacity * 2;
		builder->references = safe_erealloc(
			builder->references, capacity,
			sizeof(*builder->references), 0);
		builder->reference_capacity = capacity;
	}
	builder->references[builder->reference_count].type = type;
	builder->references[builder->reference_count].source_ordinal =
		source_ordinal;
	builder->references[builder->reference_count].name = name;
	builder->references[builder->reference_count].scope_name = scope_name;
	*token = (uint64_t) builder->reference_count + 1;
	builder->reference_count++;
	return true;
}

static bool zend_native_bundle_encode_reference(
	void *context,
	zend_native_image_reference_kind kind,
	const void *address,
	uint64_t *token)
{
	zend_native_bundle_builder *builder = context;
	const zend_function *function;
	uint32_t source_ordinal;

	if (builder == NULL || address == NULL || token == NULL) {
		return false;
	}
	if (kind == ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL) {
		const zend_native_entry_cell *cell = address;
		function = cell->function;
	} else if (kind == ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION) {
		function = address;
	} else if (kind == ZEND_NATIVE_IMAGE_REFERENCE_CLASS) {
		const zend_class_entry *class_entry = address;
		return class_entry->name != NULL
			&& zend_native_bundle_add_reference(
				builder, ZEND_NATIVE_BUNDLE_REFERENCE_CLASS,
				UINT32_MAX, class_entry->name, NULL, token);
	} else {
		return false;
	}
	if (function == NULL) {
		return false;
	}
	if (function->type == ZEND_USER_FUNCTION) {
		if (!zend_native_bundle_source_ordinal(
				builder->compiler->script, &function->op_array,
				&source_ordinal)) {
			return false;
		}
		return zend_native_bundle_add_reference(
			builder, ZEND_NATIVE_BUNDLE_REFERENCE_USER_FUNCTION,
			source_ordinal, NULL, NULL, token);
	}
	if (function->type != ZEND_INTERNAL_FUNCTION
			|| function->common.function_name == NULL) {
		return false;
	}
	return zend_native_bundle_add_reference(
		builder, ZEND_NATIVE_BUNDLE_REFERENCE_INTERNAL_FUNCTION,
		UINT32_MAX, function->common.function_name,
		function->common.scope != NULL
			? function->common.scope->name : NULL,
		token);
}

static bool zend_native_bundle_span(
	size_t size, uint64_t offset, uint64_t length)
{
	return offset <= size && length <= size - (size_t) offset;
}

static zend_string *zend_native_bundle_string(
	const zend_native_bundle_decoder *decoder,
	uint64_t offset, uint32_t length)
{
	if (length == 0
			|| !zend_native_bundle_span(decoder->size, offset, length)) {
		return NULL;
	}
	return zend_string_init(
		(const char *) decoder->bytes + offset, length, false);
}

static bool zend_native_bundle_decode_reference(
	void *context,
	zend_native_image_reference_kind kind,
	uint64_t token,
	const void **address)
{
	zend_native_bundle_decoder *decoder = context;
	const zend_native_bundle_reference_record *reference;
	zend_function *function = NULL;
	zend_class_entry *class_entry = NULL;
	zend_string *name = NULL;
	zend_string *lower_name = NULL;
	zend_string *scope_name = NULL;
	zend_string *lower_scope = NULL;

	if (decoder == NULL || address == NULL || token == 0
			|| token > decoder->reference_count) {
		return false;
	}
	*address = NULL;
	reference = &decoder->references[token - 1];
	if (reference->type == ZEND_NATIVE_BUNDLE_REFERENCE_USER_FUNCTION) {
		zend_op_array *op_array = zend_native_bundle_source_at(
			decoder->compiler->script, reference->source_ordinal);
		zend_native_compiled_function *compiled =
			zend_native_compiler_find_function(
				decoder->compiler, op_array);
		if (op_array == NULL || compiled == NULL
				|| (kind != ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL
					&& kind != ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION)) {
			return false;
		}
		*address = kind == ZEND_NATIVE_IMAGE_REFERENCE_ENTRY_CELL
			? (const void *) &compiled->entry_cell
			: (const void *) op_array;
		return true;
	}
	name = zend_native_bundle_string(
		decoder, reference->name_offset, reference->name_length);
	if (name == NULL) {
		return false;
	}
	lower_name = zend_string_tolower(name);
	if (reference->type == ZEND_NATIVE_BUNDLE_REFERENCE_CLASS) {
		class_entry = zend_hash_find_ptr(EG(class_table), lower_name);
		if (kind == ZEND_NATIVE_IMAGE_REFERENCE_CLASS
				&& class_entry != NULL) {
			*address = class_entry;
		}
		goto done;
	}
	if (reference->type
			!= ZEND_NATIVE_BUNDLE_REFERENCE_INTERNAL_FUNCTION
			|| kind != ZEND_NATIVE_IMAGE_REFERENCE_FUNCTION) {
		goto done;
	}
	if (reference->scope_length == 0) {
		function = zend_hash_find_ptr(EG(function_table), lower_name);
	} else {
		scope_name = zend_native_bundle_string(
			decoder, reference->scope_offset,
			reference->scope_length);
		if (scope_name != NULL) {
			lower_scope = zend_string_tolower(scope_name);
			class_entry = zend_hash_find_ptr(
				EG(class_table), lower_scope);
			if (class_entry != NULL) {
				function = zend_hash_find_ptr(
					&class_entry->function_table, lower_name);
			}
		}
	}
	if (function != NULL && function->type == ZEND_INTERNAL_FUNCTION) {
		*address = function;
	}

done:
	if (lower_scope != NULL) {
		zend_string_release(lower_scope);
	}
	if (scope_name != NULL) {
		zend_string_release(scope_name);
	}
	zend_string_release(lower_name);
	zend_string_release(name);
	return *address != NULL;
}

zend_result zend_native_compiler_serialize_bundle(
	zend_native_compiler *compiler,
	unsigned char **out_bytes,
	size_t *out_size,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_bundle_builder builder;
	zend_native_bundle_header header;
	zend_native_bundle_function_record *function_records = NULL;
	zend_native_bundle_reference_record *reference_records = NULL;
	unsigned char **images = NULL;
	size_t *image_sizes = NULL;
	unsigned char *bytes = NULL;
	uint64_t cursor;
	uint32_t index;
	zend_native_diagnostic image_diagnostic;

	if (out_bytes != NULL) {
		*out_bytes = NULL;
	}
	if (out_size != NULL) {
		*out_size = 0;
	}
	if (compiler == NULL || out_bytes == NULL || out_size == NULL
			|| !compiler->defer_publication
			|| compiler->function_count == 0) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
			ZEND_NATIVE_DIAGNOSTIC_INVALID_ARGUMENT,
			"native image bundle producer is not ready");
		return FAILURE;
	}
	memset(&builder, 0, sizeof(builder));
	memset(&header, 0, sizeof(header));
	builder.compiler = compiler;
	images = ecalloc(compiler->function_count, sizeof(*images));
	image_sizes = ecalloc(
		compiler->function_count, sizeof(*image_sizes));
	function_records = ecalloc(
		compiler->function_count, sizeof(*function_records));
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		if (function->state != ZEND_NATIVE_CODEUNIT_IMAGE_READY
				|| function->image_owner_index
					>= compiler->function_count
				|| function->image_component_index
					>= compiler->function_count
				|| (function->image_component_index == 0
					&& (function->image_owner_index != index
						|| function->image == NULL))
				|| (function->image_component_index != 0
					&& (function->image_owner_index == index
						|| function->image != NULL))
				|| !zend_native_bundle_source_ordinal(
					compiler->script, function->op_array,
					&function_records[index].source_ordinal)) {
			goto malformed;
		}
		function_records[index].image_owner_index =
			function->image_owner_index;
		function_records[index].image_component_index =
			function->image_component_index;
		if (function->image_component_index != 0) {
			continue;
		}
		memset(&image_diagnostic, 0, sizeof(image_diagnostic));
		if (zend_native_image_serialize(
				function->image,
				zend_native_bundle_encode_reference, &builder,
				&images[index], &image_sizes[index],
				&image_diagnostic) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
				&image_diagnostic);
			goto failure;
		}
	}
	reference_records = ecalloc(
		builder.reference_count, sizeof(*reference_records));
	header.magic = ZEND_NATIVE_BUNDLE_MAGIC;
	header.format = ZEND_NATIVE_BUNDLE_FORMAT;
	header.target = compiler->target;
	header.runtime_abi = ZEND_NATIVE_RUNTIME_ABI_VERSION;
	header.function_count = compiler->function_count;
	header.reference_count = builder.reference_count;
	header.function_offset = sizeof(header);
	header.reference_offset = header.function_offset
		+ (uint64_t) compiler->function_count
			* sizeof(*function_records);
	header.string_offset = header.reference_offset
		+ (uint64_t) builder.reference_count
			* sizeof(*reference_records);
	cursor = header.string_offset;
	for (index = 0; index < builder.reference_count; index++) {
		const zend_native_bundle_reference *reference =
			&builder.references[index];
		zend_native_bundle_reference_record *record =
			&reference_records[index];
		record->type = reference->type;
		record->source_ordinal = reference->source_ordinal;
		if (reference->name != NULL) {
			record->name_offset = cursor;
			record->name_length = ZSTR_LEN(reference->name);
			cursor += record->name_length;
		}
		if (reference->scope_name != NULL) {
			record->scope_offset = cursor;
			record->scope_length = ZSTR_LEN(reference->scope_name);
			cursor += record->scope_length;
		}
	}
	header.image_offset = cursor;
	for (index = 0; index < compiler->function_count; index++) {
		if (function_records[index].image_component_index != 0) {
			continue;
		}
		function_records[index].image_offset = cursor;
		function_records[index].image_size = image_sizes[index];
		cursor += image_sizes[index];
		if (cursor > ZEND_NATIVE_BUNDLE_MAX_BYTES) {
			goto malformed;
		}
	}
	for (index = 0; index < compiler->function_count; index++) {
		const uint32_t owner =
			function_records[index].image_owner_index;
		if (function_records[index].image_component_index == 0) {
			continue;
		}
		if (owner >= compiler->function_count
				|| function_records[owner].image_component_index != 0
				|| function_records[owner].image_owner_index != owner) {
			goto malformed;
		}
		function_records[index].image_offset =
			function_records[owner].image_offset;
		function_records[index].image_size = 0;
	}
	header.total_size = cursor;
	bytes = malloc((size_t) cursor);
	if (bytes == NULL) {
		goto allocation_failure;
	}
	memset(bytes, 0, (size_t) cursor);
	memcpy(bytes, &header, sizeof(header));
	memcpy(bytes + header.function_offset, function_records,
		(size_t) compiler->function_count * sizeof(*function_records));
	memcpy(bytes + header.reference_offset, reference_records,
		(size_t) builder.reference_count * sizeof(*reference_records));
	for (index = 0; index < builder.reference_count; index++) {
		const zend_native_bundle_reference *reference =
			&builder.references[index];
		const zend_native_bundle_reference_record *record =
			&reference_records[index];
		if (record->name_length != 0) {
			memcpy(bytes + record->name_offset,
				ZSTR_VAL(reference->name), record->name_length);
		}
		if (record->scope_length != 0) {
			memcpy(bytes + record->scope_offset,
				ZSTR_VAL(reference->scope_name), record->scope_length);
		}
	}
	for (index = 0; index < compiler->function_count; index++) {
		if (image_sizes[index] != 0) {
			memcpy(bytes + function_records[index].image_offset,
				images[index], image_sizes[index]);
		}
	}
	header.checksum = zend_native_bundle_checksum(
		bytes, (size_t) header.total_size);
	memcpy(bytes, &header, sizeof(header));
	*out_bytes = bytes;
	*out_size = (size_t) header.total_size;
	goto success;

malformed:
	zend_native_compiler_set_diagnostic(
		compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		"native image bundle contains an unstable source identity");
	goto failure;
allocation_failure:
	zend_native_compiler_set_diagnostic(
		compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_CODEGEN,
		ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
		"native image bundle allocation failed");
failure:
	free(bytes);
success:
	for (index = 0; images != NULL
			&& index < compiler->function_count; index++) {
		zend_native_serialized_image_destroy(images[index]);
	}
	efree(reference_records);
	efree(function_records);
	efree(image_sizes);
	efree(images);
	efree(builder.references);
	return *out_bytes != NULL ? SUCCESS : FAILURE;
}

zend_result zend_native_compiler_import_bundle(
	zend_native_compiler *compiler,
	const unsigned char *bytes,
	size_t size,
	zend_native_compile_diagnostic *diagnostic)
{
	zend_native_bundle_header header;
	const zend_native_bundle_function_record *functions;
	const zend_native_bundle_reference_record *references;
	zend_native_bundle_decoder decoder;
	zend_native_diagnostic image_diagnostic;
	const char *malformed_message =
		"persistent native image bundle header is incompatible";
	uint32_t index;

	if (compiler == NULL || bytes == NULL || size < sizeof(header)
			|| size > ZEND_NATIVE_BUNDLE_MAX_BYTES
			|| compiler->defer_publication) {
		goto malformed;
	}
	memcpy(&header, bytes, sizeof(header));
	if (header.magic != ZEND_NATIVE_BUNDLE_MAGIC
			|| header.format != ZEND_NATIVE_BUNDLE_FORMAT
			|| header.target != (uint32_t) compiler->target
			|| header.runtime_abi != ZEND_NATIVE_RUNTIME_ABI_VERSION
			|| header.function_count == 0
			|| header.total_size != size
			|| header.checksum != zend_native_bundle_checksum(bytes, size)
			|| !zend_native_bundle_span(
				size, header.function_offset,
				(uint64_t) header.function_count
					* sizeof(*functions))
			|| !zend_native_bundle_span(
				size, header.reference_offset,
				(uint64_t) header.reference_count
					* sizeof(*references))
			|| header.function_offset != sizeof(header)
			|| header.reference_offset < header.function_offset
			|| header.string_offset < header.reference_offset
			|| header.image_offset < header.string_offset
			|| header.image_offset > size) {
		goto malformed;
	}
	functions = (const zend_native_bundle_function_record *)
		(bytes + header.function_offset);
	references = (const zend_native_bundle_reference_record *)
		(bytes + header.reference_offset);
	for (index = 0; index < header.reference_count; index++) {
		const zend_native_bundle_reference_record *reference =
			&references[index];
		if ((reference->type != ZEND_NATIVE_BUNDLE_REFERENCE_USER_FUNCTION
				&& reference->type
					!= ZEND_NATIVE_BUNDLE_REFERENCE_INTERNAL_FUNCTION
				&& reference->type
					!= ZEND_NATIVE_BUNDLE_REFERENCE_CLASS)
				|| (reference->name_length != 0
					&& (!zend_native_bundle_span(
						size, reference->name_offset,
						reference->name_length)
						|| reference->name_offset < header.string_offset
						|| reference->name_offset
							+ reference->name_length
								> header.image_offset))
				|| (reference->scope_length != 0
					&& (!zend_native_bundle_span(
						size, reference->scope_offset,
						reference->scope_length)
						|| reference->scope_offset < header.string_offset
						|| reference->scope_offset
							+ reference->scope_length
								> header.image_offset))) {
			malformed_message =
				"persistent native image bundle reference table is malformed";
			goto malformed;
		}
	}
	for (index = 0; index < header.function_count; index++) {
		const zend_native_bundle_function_record *record =
			&functions[index];
		const uint32_t owner = record->image_owner_index;
		zend_op_array *op_array = zend_native_bundle_source_at(
			compiler->script, record->source_ordinal);
		if (op_array == NULL
				|| owner >= header.function_count
				|| record->image_component_index
					>= header.function_count
				|| functions[owner].image_owner_index != owner
				|| functions[owner].image_component_index != 0
				|| functions[owner].image_size == 0
				|| functions[owner].image_offset
					< header.image_offset
				|| !zend_native_bundle_span(
					size, functions[owner].image_offset,
					functions[owner].image_size)
				|| (record->image_component_index == 0
					&& (owner != index || record->image_size == 0))
				|| (record->image_component_index != 0
					&& (owner == index || record->image_size != 0
						|| record->image_offset
							!= functions[owner].image_offset))
				|| zend_native_compiler_add_function(
					compiler, op_array, diagnostic) == NULL) {
			malformed_message =
				"persistent native image bundle source identity is unavailable";
			goto malformed;
		}
		compiler->functions[index]->image_owner_index = owner;
		compiler->functions[index]->image_component_index =
			record->image_component_index;
		for (uint32_t prior = 0; prior < index; prior++) {
			if (functions[prior].image_owner_index == owner
					&& functions[prior].image_component_index
						== record->image_component_index) {
				malformed_message =
					"persistent native image bundle component table is ambiguous";
				goto malformed;
			}
		}
	}
	memset(&decoder, 0, sizeof(decoder));
	decoder.compiler = compiler;
	decoder.bytes = bytes;
	decoder.size = size;
	decoder.references = references;
	decoder.reference_count = header.reference_count;
	for (index = 0; index < header.function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		if (function->image_component_index != 0) {
			function->publish_pending = true;
			continue;
		}
		memset(&image_diagnostic, 0, sizeof(image_diagnostic));
		if (zend_native_image_deserialize(
				bytes + functions[index].image_offset,
				(size_t) functions[index].image_size,
				zend_native_bundle_decode_reference, &decoder,
				&function->image, &image_diagnostic) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
				&image_diagnostic);
			goto failure;
		}
		function->publish_pending = true;
	}
	for (index = 0; index < header.function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		if (function->image_component_index != 0) {
			continue;
		}
		zend_native_image_metrics metrics;
		zend_hrtime_t started = zend_hrtime();
		if (zend_native_publish_image(
				compiler->target, function->image,
				&function->code, &image_diagnostic) == FAILURE
				|| zend_native_code_is_writable(function->code)
				|| !zend_native_code_is_executable(function->code)) {
			compiler->stats.publish_ns += zend_hrtime() - started;
			zend_native_compiler_backend_failure(
				compiler, diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
				&image_diagnostic);
			goto failure;
		}
		compiler->stats.publish_ns += zend_hrtime() - started;
		zend_native_image_get_metrics(function->image, &metrics);
		compiler->stats.native_code_bytes +=
			zend_native_image_size(function->image);
		compiler->stats.runtime_helper_sites +=
			metrics.runtime_helper_sites;
		compiler->stats.source_opline_decode_sites +=
			metrics.source_opline_decode_sites;
		compiler->stats.guard_sites += metrics.guard_sites;
		compiler->stats.slow_path_sites += metrics.slow_path_sites;
		compiler->stats.direct_call_sites += metrics.direct_call_sites;
		compiler->stats.direct_leaf_scalar_sites +=
			metrics.direct_leaf_scalar_sites;
		compiler->stats.direct_typed_body_sites +=
			metrics.direct_typed_body_sites;
		compiler->stats.direct_call_frame_bytes +=
			metrics.direct_call_frame_bytes;
		compiler->stats.inner_call_runtime_helper_calls +=
			metrics.inner_call_runtime_helper_calls;
		compiler->stats.inner_call_heap_allocations +=
			metrics.inner_call_heap_allocations;
		compiler->stats.inner_call_catcher_boundaries +=
			metrics.inner_call_catcher_boundaries;
	}
	for (index = 0; index < header.function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		zend_native_compiled_function *owner;

		if (function->image_component_index == 0) {
			continue;
		}
		owner = compiler->functions[function->image_owner_index];
		memset(&image_diagnostic, 0, sizeof(image_diagnostic));
		if (owner->code == NULL
				|| zend_native_code_component_view(
					owner->code, function->image_component_index,
					&function->code, &image_diagnostic) == FAILURE) {
			zend_native_compiler_backend_failure(
				compiler, diagnostic,
				ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
				&image_diagnostic);
			goto failure;
		}
	}
	if (!zend_native_compiler_reserve_publications(
			compiler, header.function_count)) {
		zend_native_compiler_set_diagnostic(
			compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
			ZEND_NATIVE_DIAGNOSTIC_ALLOCATION_FAILED,
			"native publication index allocation failed");
		goto failure;
	}
	for (index = 0; index < header.function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		if (function->code == NULL
				|| function->entry_cell.state
					!= ZEND_NATIVE_ENTRY_COMPILING) {
			goto failure;
		}
	}
	for (index = 0; index < header.function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];
		if (zend_native_entry_cell_publish(
				&function->entry_cell, function->code) == FAILURE) {
			goto failure;
		}
	}
	for (index = 0; index < header.function_count; index++) {
		compiler->functions[index]->state = ZEND_NATIVE_CODEUNIT_READY;
		zend_native_compiler_record_publication(
			compiler, compiler->functions[index]);
		compiler->functions[index]->publish_pending = false;
	}
	compiler->published_component_count++;
	return SUCCESS;

malformed:
	zend_native_compiler_set_diagnostic(
		compiler, diagnostic, ZEND_NATIVE_COMPILE_PHASE_PUBLISH,
		ZEND_NATIVE_DIAGNOSTIC_MALFORMED_MIR,
		malformed_message);
failure:
	zend_native_compiler_fail_pending_component(compiler, 0);
	return FAILURE;
}

void zend_native_compiler_bundle_destroy(unsigned char *bytes)
{
	free(bytes);
}

static void zend_native_compiler_release_function_transients(
	zend_native_compiler *compiler,
	zend_native_compiled_function *function)
{
	if (function->image != NULL) {
		zend_native_image_destroy(function->image);
		function->image = NULL;
	}
	if (function->module != NULL) {
		zend_mir_module_destroy(function->module);
		function->module = NULL;
	} else {
		zend_native_compiler_module_reset(&function->module_host);
	}
	if (function->ssa_arena != NULL) {
		zend_arena_destroy(function->ssa_arena);
		function->ssa_arena = NULL;
	}
	memset(&function->ssa, 0, sizeof(function->ssa));
	zend_native_compiler_free(compiler, function->source_effects);
	function->source_effects = NULL;
	function->source_effect_count = 0;
	function->source_effect_capacity = 0;
	function->source_effects_prepared = false;
	zend_native_compiler_free(
		compiler, function->exception_handler_oplines);
	function->exception_handler_oplines = NULL;
	zend_native_compiler_free(
		compiler, function->first_call_site_by_target);
	function->first_call_site_by_target = NULL;
	zend_native_compiler_free(
		compiler, function->next_call_site_by_site);
	function->next_call_site_by_site = NULL;
	function->call_target_count = 0;
	function->call_site_count = 0;
	function->call_sites_indexed = false;
}

void zend_native_compiler_release_ready_transients(
	zend_native_compiler *compiler, uint32_t first_function_index)
{
	uint32_t index;

	if (compiler == NULL || first_function_index >= compiler->function_count) {
		return;
	}
	zend_native_compiler_mutation_lock(compiler);
	for (index = first_function_index; index < compiler->function_count;
			index++) {
		zend_native_compiled_function *function = compiler->functions[index];

		if (function == NULL
				|| function->state != ZEND_NATIVE_CODEUNIT_READY
				|| function->entry_cell.active_calls != 0
				|| function->entry_cell.suspended_frames != 0) {
			continue;
		}
		if (!function->leaf_scalar_frame_known) {
			(void) zend_native_compiler_function_has_leaf_scalar_frame(
				compiler, function);
		}
		zend_native_compiler_release_function_transients(
			compiler, function);
	}
	zend_native_compiler_mutation_unlock(compiler);
}

static void zend_native_compiler_release_runtime_source(
	zend_native_compiler *compiler, zend_op_array *source)
{
	zend_native_runtime_source **link = &compiler->runtime_sources;

	while (*link != NULL) {
		zend_native_runtime_source *retained = *link;

		if (&retained->op_array != source) {
			link = &retained->next;
			continue;
		}
		*link = retained->next;
		destroy_op_array(&retained->op_array);
		zend_native_compiler_free(compiler, retained);
		return;
	}
}

bool zend_native_compiler_retire_dynamic_component(
	zend_native_compiler *compiler, uint32_t first_function_index,
	const zend_op_array *root)
{
	zend_native_compiled_function *root_function;
	zend_op_array *root_source;
	uint32_t publication_start;
	uint32_t retired_count;
	uint32_t index;

	if (compiler == NULL || root == NULL || compiler->persistent) {
		return false;
	}
	zend_native_compiler_mutation_lock(compiler);
	if (first_function_index >= compiler->function_count) {
		zend_native_compiler_mutation_unlock(compiler);
		return false;
	}
	retired_count = compiler->function_count - first_function_index;
	if (retired_count > compiler->publication_count) {
		zend_native_compiler_mutation_unlock(compiler);
		return false;
	}
	publication_start = compiler->publication_count - retired_count;
	root_source = zend_native_compiler_canonical_reentry_op_array(
		compiler, root);
	root_function = root_source != NULL
		? zend_native_compiler_find_function(compiler, root_source) : NULL;
	if (root_function == NULL
			|| root_function->registry_index != first_function_index) {
		zend_native_compiler_mutation_unlock(compiler);
		return false;
	}
	for (index = first_function_index;
			index < compiler->function_count; index++) {
		zend_native_compiled_function *function = compiler->functions[index];

		if (function == NULL || function->registry_index != index
				|| function->state != ZEND_NATIVE_CODEUNIT_READY
				|| function->entry_cell.state != ZEND_NATIVE_ENTRY_READY
				|| function->entry_cell.active_calls != 0
				|| function->entry_cell.suspended_frames != 0
				|| function->image_owner_index < first_function_index
				|| function->image_owner_index >= compiler->function_count) {
			zend_native_compiler_mutation_unlock(compiler);
			return false;
		}
	}
	for (index = publication_start;
			index < compiler->publication_count; index++) {
		zend_native_compiled_function *published =
			zend_native_compiler_find_function(
				compiler, compiler->publication_log[index]);

		if (published == NULL
				|| published->registry_index < first_function_index) {
			zend_native_compiler_mutation_unlock(compiler);
			return false;
		}
	}
	for (index = compiler->function_count;
			index-- > first_function_index;) {
		zend_native_compiled_function *function = compiler->functions[index];
		zend_native_compiled_function *indexed = zend_hash_index_find_ptr(
			&compiler->functions_by_op_array,
			(zend_ulong) (uintptr_t) function->op_array);

		if (indexed == function) {
			zend_hash_index_del(
				&compiler->functions_by_op_array,
				(zend_ulong) (uintptr_t) function->op_array);
		}
		(void) zend_native_entry_cell_reset(&function->entry_cell);
		if (function->code != NULL) {
			zend_native_code_destroy(function->code);
		}
		zend_native_compiler_release_function_transients(
			compiler, function);
		zend_native_compiler_free(
			compiler, function->internal_call_cells);
		zend_native_compiler_free(compiler, function);
		compiler->functions[index] = NULL;
	}
	compiler->function_count = first_function_index;
	compiler->publication_count = publication_start;
	if (zend_hash_index_find_ptr(
			&compiler->source_op_arrays_by_opcodes,
			(zend_ulong) (uintptr_t) root->opcodes) == root_source) {
		zend_hash_index_del(
			&compiler->source_op_arrays_by_opcodes,
			(zend_ulong) (uintptr_t) root->opcodes);
	}
	zend_native_compiler_release_runtime_source(compiler, root_source);
	zend_native_compiler_mutation_unlock(compiler);
	return true;
}

void zend_native_compiler_end_request(zend_native_compiler *compiler)
{
	uint32_t index;

	if (compiler == NULL) {
		return;
	}
	zend_native_compiler_leave(compiler);
	zend_native_compiler_session_flush_stats(compiler);
	zend_native_compiler_session_release(compiler);
	if (!compiler->persistent) {
		return;
	}
	zend_native_compiler_mutation_lock(compiler);
	/*
	 * Publication retirement owns this precondition.  Never unmap code that
	 * is still executing or backs a suspended generator frame.
	 */
	if (compiler->transients_released
			|| zend_native_compiler_active_call_count(compiler) != 0
			|| zend_native_compiler_suspended_frame_count(compiler) != 0) {
		zend_native_compiler_mutation_unlock(compiler);
		return;
	}
	for (index = 0; index < compiler->function_count; index++) {
		zend_native_compiled_function *function =
			compiler->functions[index];

		if (function != NULL
				&& function->state == ZEND_NATIVE_CODEUNIT_READY) {
			/*
			 * Persistent compilers retain this property after request-owned
			 * source and MIR data are released.  Freeze it while both are
			 * still available; destruction has no consumer for an unknown
			 * value and must not start a new source analysis.
			 */
			if (!function->leaf_scalar_frame_known) {
				(void) zend_native_compiler_function_has_leaf_scalar_frame(
					compiler, function);
			}
			zend_native_compiler_release_function_transients(
				compiler, function);
		}
	}
	compiler->transients_released = true;
	zend_native_compiler_mutation_unlock(compiler);
}

void zend_native_compiler_destroy(zend_native_compiler *compiler)
{
	uint32_t index;
	zend_native_runtime_source *runtime_source;

	if (compiler == NULL) {
		return;
	}
	/*
	 * The executor keeps stale generations on its retirement list until
	 * active calls and suspended generator frames have both drained.
	 */
	if (!zend_native_compiler_is_quiescent(compiler)) {
		return;
	}
	zend_native_compiler_leave(compiler);
	zend_native_compiler_session_flush_stats(compiler);
	zend_native_compiler_session_release(compiler);
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
		zend_native_compiler_release_function_transients(
			compiler, function);
		zend_native_compiler_free(
			compiler, function->internal_call_cells);
		zend_native_compiler_free(compiler, function);
	}
	zend_native_compiler_free(compiler, compiler->component_heads);
	zend_native_compiler_free(
		compiler, compiler->script_functions_by_declaration_id);
	zend_hash_destroy(&compiler->functions_by_op_array);
	zend_hash_destroy(&compiler->source_op_arrays_by_opcodes);
	runtime_source = compiler->runtime_sources;
	while (runtime_source != NULL) {
		zend_native_runtime_source *next = runtime_source->next;

		destroy_op_array(&runtime_source->op_array);
		zend_native_compiler_free(compiler, runtime_source);
		runtime_source = next;
	}
	zend_native_compiler_free(compiler, compiler->publication_log);
	zend_native_compiler_free(compiler, compiler->functions);
#ifdef ZTS
	if (compiler->mutation_mutex != NULL) {
		tsrm_mutex_free(compiler->mutation_mutex);
		compiler->mutation_mutex = NULL;
	}
#endif
	pefree(compiler, compiler->persistent);
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
	zend_native_compiled_function *owner;

	if (compiler == NULL || function == NULL
			|| !ZEND_USER_CODE(function->type)) {
		return NULL;
	}
	compiled = zend_native_compiler_find_function(
		(zend_native_compiler *) compiler, &function->op_array);
	if (compiled == NULL) {
		return NULL;
	}
	if (compiled->image != NULL) {
		return compiled->image;
	}
	if (compiled->image_owner_index >= compiler->function_count) {
		return NULL;
	}
	owner = compiler->functions[compiled->image_owner_index];
	return owner != NULL ? owner->image : NULL;
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

uint32_t zend_native_compiler_suspended_frame_count(
	const zend_native_compiler *compiler)
{
	uint32_t suspended_frames = 0;
	uint32_t index;

	if (compiler == NULL) {
		return 0;
	}
	for (index = 0; index < compiler->function_count; index++) {
		suspended_frames +=
			compiler->functions[index]->entry_cell.suspended_frames;
	}
	return suspended_frames;
}

bool zend_native_compiler_is_quiescent(
	const zend_native_compiler *compiler)
{
	bool quiescent;

	if (compiler == NULL) {
		return true;
	}
	zend_native_compiler_mutation_lock(compiler);
	quiescent = zend_native_compiler_active_call_count(compiler) == 0
		&& zend_native_compiler_suspended_frame_count(compiler) == 0;
	zend_native_compiler_mutation_unlock(compiler);
	return quiescent;
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

		if (function->state == ZEND_NATIVE_CODEUNIT_FAILED) {
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
