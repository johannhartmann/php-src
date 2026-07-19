/*
   +----------------------------------------------------------------------+
   | PHP Version 8                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "php.h"
#include "php_native_mir_test.h"
#include "native_mir_test_arginfo.h"
#include "ext/standard/info.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_smart_str.h"
#include "Zend/Optimizer/zend_func_info.h"
#include "Zend/Optimizer/zend_optimizer.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"

#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/zend_mir.h"
#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"

#define NATIVE_MIR_TEST_SCHEMA_VERSION 1
#define NATIVE_MIR_TEST_DEFAULT_DIAGNOSTIC_LIMIT 32
#define NATIVE_MIR_TEST_MAX_DIAGNOSTIC_LIMIT 256
#define NATIVE_MIR_TEST_ARENA_SIZE (64 * 1024)
#define NATIVE_MIR_TEST_MIN_MIR_CHUNK_SIZE 64
#define NATIVE_MIR_TEST_MAX_MIR_CHUNK_SIZE (1024 * 1024)
#define NATIVE_MIR_TEST_OPTIMIZATION_LEVEL ((zend_long) 0x7FFEBFFF)

typedef enum _native_mir_test_phase {
	NATIVE_MIR_TEST_PHASE_COMPILE = 0,
	NATIVE_MIR_TEST_PHASE_SSA,
	NATIVE_MIR_TEST_PHASE_LOWERING,
	NATIVE_MIR_TEST_PHASE_VERIFY,
	NATIVE_MIR_TEST_PHASE_DUMP,
	NATIVE_MIR_TEST_PHASE_COMPLETE
} native_mir_test_phase;

typedef enum _native_mir_test_status {
	NATIVE_MIR_TEST_STATUS_ACCEPTED = 0,
	NATIVE_MIR_TEST_STATUS_REJECTED,
	NATIVE_MIR_TEST_STATUS_ERROR
} native_mir_test_status;

typedef enum _native_mir_test_fault {
	NATIVE_MIR_TEST_FAULT_NONE = 0,
	NATIVE_MIR_TEST_FAULT_COMPILE_BAILOUT,
	NATIVE_MIR_TEST_FAULT_SSA_FAILURE,
	NATIVE_MIR_TEST_FAULT_LOWER_FAILURE,
	NATIVE_MIR_TEST_FAULT_MODULE_OOM,
	NATIVE_MIR_TEST_FAULT_DUMP_FAILURE
} native_mir_test_fault;

typedef struct _native_mir_test_diagnostic {
	char stage[8];
	char code[16];
	char message[ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY];
	uint32_t opline;
	bool has_opline;
} native_mir_test_diagnostic;

typedef struct _native_mir_test_module_host {
	zend_arena *arena;
	uint32_t successful_allocations;
	uint32_t fail_after;
	bool fail_enabled;
} native_mir_test_module_host;

typedef struct _native_mir_test_state {
	zend_string *source;
	zend_string *filename;
	zend_string *function_name;
	zend_op_array *compiled;
	zend_op_array *selected;
	zend_arena *ssa_arena;
	zend_ssa ssa;
	zend_script script;
	bool script_initialized;
	uint32_t original_compiler_options;
	bool compiler_options_saved;
	uint32_t function_table_used_before;
	bool function_table_snapshot;
	native_mir_test_phase phase;
	native_mir_test_status status;
	native_mir_test_fault fault;
	native_mir_test_diagnostic *diagnostics;
	uint32_t diagnostic_count;
	uint32_t diagnostic_limit;
	uint32_t wave;
	size_t mir_chunk_size;
	const char *diagnostic_stage;
	native_mir_test_module_host module_host;
	zend_mir_module *module;
	smart_str dump;
	uint32_t dump_writes;
} native_mir_test_state;

/*
 * This private integration point constructs the deterministic provider list
 * and adapts the borrowed Zend op-array/SSA for one synchronous lowering call.
 * The test extension owns the module host and the returned module; no process
 * pointer enters persistent MIR or the canonical dump.
 */
extern zend_mir_lowering_result zend_mir_lower_w03_zend_source(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

static const char *native_mir_test_phase_name(native_mir_test_phase phase)
{
	switch (phase) {
		case NATIVE_MIR_TEST_PHASE_COMPILE:
			return "compile";
		case NATIVE_MIR_TEST_PHASE_SSA:
			return "ssa";
		case NATIVE_MIR_TEST_PHASE_LOWERING:
			return "lowering";
		case NATIVE_MIR_TEST_PHASE_VERIFY:
			return "verify";
		case NATIVE_MIR_TEST_PHASE_DUMP:
			return "dump";
		case NATIVE_MIR_TEST_PHASE_COMPLETE:
			return "complete";
	}
	return "compile";
}

static const char *native_mir_test_status_name(native_mir_test_status status)
{
	switch (status) {
		case NATIVE_MIR_TEST_STATUS_ACCEPTED:
			return "accepted";
		case NATIVE_MIR_TEST_STATUS_REJECTED:
			return "rejected";
		case NATIVE_MIR_TEST_STATUS_ERROR:
			return "error";
	}
	return "error";
}

static bool native_mir_test_extract_token(
	const char *message, char code[16])
{
	size_t index;

	if (message == NULL || message[0] != '['
			|| (memcmp(message + 1, "MIRL", 4) != 0
				&& memcmp(message + 1, "MIRV", 4) != 0)) {
		return false;
	}
	for (index = 5; index < 9; index++) {
		if (message[index] < '0' || message[index] > '9') {
			return false;
		}
	}
	if (message[9] != ']') {
		return false;
	}
	memcpy(code, message + 1, 8);
	code[8] = '\0';
	return true;
}

static void native_mir_test_add_diagnostic(
	native_mir_test_state *state,
	const char *stage,
	const char *code,
	const char *message,
	bool has_opline,
	uint32_t opline)
{
	native_mir_test_diagnostic *diagnostic;

	if (state == NULL || stage == NULL || code == NULL || message == NULL
			|| state->diagnostic_count >= state->diagnostic_limit) {
		return;
	}
	diagnostic = &state->diagnostics[state->diagnostic_count++];
	memset(diagnostic, 0, sizeof(*diagnostic));
	snprintf(diagnostic->stage, sizeof(diagnostic->stage), "%s", stage);
	snprintf(diagnostic->code, sizeof(diagnostic->code), "%s", code);
	snprintf(diagnostic->message, sizeof(diagnostic->message), "%s", message);
	diagnostic->has_opline = has_opline;
	diagnostic->opline = opline;
}

static bool native_mir_test_emit_mir_diagnostic(
	void *context, const zend_mir_diagnostic *source)
{
	native_mir_test_state *state = context;
	char code[16];
	const char *stage;
	const char *opline_token;
	unsigned int opline = 0;
	bool has_opline = false;

	if (state == NULL || source == NULL) {
		return false;
	}
	if (state->diagnostic_count >= state->diagnostic_limit) {
		return false;
	}
	stage = state->diagnostic_stage != NULL
		? state->diagnostic_stage : "MIRV";
	if (native_mir_test_extract_token(source->message, code)) {
		stage = memcmp(code, "MIRL", 4) == 0 ? "MIRL" : "MIRV";
	} else {
		snprintf(code, sizeof(code), "%s%04u",
			strcmp(stage, "MIRL") == 0 ? "MIRL" : "MIRV",
			(unsigned int) source->code);
	}
	opline_token = strstr(source->message, " opline=");
	if (opline_token != NULL
			&& sscanf(opline_token, " opline=%u", &opline) == 1
			&& opline != ZEND_MIR_ID_INVALID) {
		has_opline = true;
	}
	native_mir_test_add_diagnostic(
		state, stage, code, source->message, has_opline, opline);
	return true;
}

static int native_mir_test_compare_diagnostics(
	const void *left_pointer, const void *right_pointer)
{
	const native_mir_test_diagnostic *left = left_pointer;
	const native_mir_test_diagnostic *right = right_pointer;
	int comparison;

	comparison = strcmp(left->stage, right->stage);
	if (comparison != 0) {
		return comparison;
	}
	comparison = strcmp(left->code, right->code);
	if (comparison != 0) {
		return comparison;
	}
	if (left->has_opline != right->has_opline) {
		return left->has_opline ? 1 : -1;
	}
	if (left->has_opline && left->opline != right->opline) {
		return left->opline < right->opline ? -1 : 1;
	}
	return strcmp(left->message, right->message);
}

static void native_mir_test_fail(
	native_mir_test_state *state,
	native_mir_test_status status,
	native_mir_test_phase phase,
	const char *stage,
	const char *code,
	const char *message)
{
	state->status = status;
	state->phase = phase;
	native_mir_test_add_diagnostic(
		state, stage, code, message, false, 0);
}

static uint64_t native_mir_test_source_hash(
	const zend_string *filename, const zend_string *source)
{
	uint64_t hash = UINT64_C(0xcbf29ce484222325);
	size_t index;

	for (index = 0; index < ZSTR_LEN(filename); index++) {
		hash ^= (unsigned char) ZSTR_VAL(filename)[index];
		hash *= UINT64_C(0x100000001b3);
	}
	hash ^= 0;
	hash *= UINT64_C(0x100000001b3);
	for (index = 0; index < ZSTR_LEN(source); index++) {
		hash ^= (unsigned char) ZSTR_VAL(source)[index];
		hash *= UINT64_C(0x100000001b3);
	}
	return hash;
}

static bool native_mir_test_fault_from_string(
	zend_string *value, native_mir_test_fault *out)
{
	if (zend_string_equals_literal(value, "compile_bailout")) {
		*out = NATIVE_MIR_TEST_FAULT_COMPILE_BAILOUT;
	} else if (zend_string_equals_literal(value, "ssa_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_SSA_FAILURE;
	} else if (zend_string_equals_literal(value, "lower_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_LOWER_FAILURE;
	} else if (zend_string_equals_literal(value, "module_oom")) {
		*out = NATIVE_MIR_TEST_FAULT_MODULE_OOM;
	} else if (zend_string_equals_literal(value, "dump_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_DUMP_FAILURE;
	} else {
		return false;
	}
	return true;
}

static bool native_mir_test_parse_options(
	native_mir_test_state *state, HashTable *options)
{
	zend_string *key;
	zval *value;

	ZEND_HASH_FOREACH_STR_KEY_VAL(options, key, value) {
		if (key == NULL) {
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				NATIVE_MIR_TEST_PHASE_COMPILE, "bridge", "INVALID_OPTIONS",
				"options must use string keys");
			return false;
		}
		if (zend_string_equals_literal(key, "function")) {
			if (Z_TYPE_P(value) == IS_NULL) {
				continue;
			}
			if (Z_TYPE_P(value) != IS_STRING || Z_STRLEN_P(value) == 0) {
				goto invalid_value;
			}
			state->function_name = Z_STR_P(value);
		} else if (zend_string_equals_literal(key, "diagnostic_limit")) {
			if (Z_TYPE_P(value) != IS_LONG || Z_LVAL_P(value) < 1
					|| Z_LVAL_P(value)
						> NATIVE_MIR_TEST_MAX_DIAGNOSTIC_LIMIT) {
				goto invalid_value;
			}
			state->diagnostic_limit = (uint32_t) Z_LVAL_P(value);
		} else if (zend_string_equals_literal(key, "wave")) {
			if (Z_TYPE_P(value) != IS_LONG
					|| (Z_LVAL_P(value) != 3 && Z_LVAL_P(value) != 4)) {
				goto invalid_value;
			}
			state->wave = (uint32_t) Z_LVAL_P(value);
		} else if (zend_string_equals_literal(key, "arena_chunk_size")) {
			if (Z_TYPE_P(value) != IS_LONG
					|| Z_LVAL_P(value) < NATIVE_MIR_TEST_MIN_MIR_CHUNK_SIZE
					|| Z_LVAL_P(value) > NATIVE_MIR_TEST_MAX_MIR_CHUNK_SIZE) {
				goto invalid_value;
			}
			state->mir_chunk_size = (size_t) Z_LVAL_P(value);
		} else if (zend_string_equals_literal(key, "fault")) {
			if (Z_TYPE_P(value) == IS_NULL) {
				continue;
			}
			if (Z_TYPE_P(value) != IS_STRING
					|| !native_mir_test_fault_from_string(
						Z_STR_P(value), &state->fault)) {
				goto invalid_value;
			}
		} else {
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				NATIVE_MIR_TEST_PHASE_COMPILE, "bridge", "INVALID_OPTIONS",
				"unknown compile/dump option");
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;

invalid_value:
	native_mir_test_fail(
		state, NATIVE_MIR_TEST_STATUS_ERROR,
		NATIVE_MIR_TEST_PHASE_COMPILE, "bridge", "INVALID_OPTIONS",
		"compile/dump option has an invalid value");
	return false;
}

static zend_op_array *native_mir_test_select_op_array(
	native_mir_test_state *state)
{
	HashTable *function_table;
	uint32_t index;

	if (state->function_name == NULL) {
		return state->compiled;
	}
	function_table = CG(function_table);
	if (!state->function_table_snapshot || function_table == NULL) {
		return NULL;
	}
	for (index = state->function_table_used_before;
			index < function_table->nNumUsed; index++) {
		Bucket *bucket = &function_table->arData[index];
		zend_function *function;

		if (Z_TYPE(bucket->val) != IS_PTR || bucket->key == NULL) {
			continue;
		}
		function = Z_PTR(bucket->val);
		if (function != NULL && function->type == ZEND_USER_FUNCTION
				&& function->op_array.function_name != NULL
				&& zend_string_equals_ci(
					function->op_array.function_name,
					state->function_name)) {
			return &function->op_array;
		}
	}
	return NULL;
}

static void native_mir_test_init_script(native_mir_test_state *state)
{
	uint32_t index;

	memset(&state->script, 0, sizeof(state->script));
	state->script.filename = state->compiled->filename;
	state->script.main_op_array = *state->compiled;
	zend_hash_init(
		&state->script.function_table,
		state->compiled->num_dynamic_func_defs, NULL, NULL, false);
	zend_hash_init(&state->script.class_table, 0, NULL, NULL, false);
	state->script_initialized = true;
	for (index = 0; index < state->compiled->num_dynamic_func_defs; index++) {
		zend_op_array *function =
			state->compiled->dynamic_func_defs[index];

		if (function != NULL && function->function_name != NULL) {
			(void) zend_hash_update_ptr(
				&state->script.function_table,
				function->function_name, function);
		}
	}
	if (state->selected != state->compiled
			&& state->selected->function_name != NULL) {
		(void) zend_hash_update_ptr(
			&state->script.function_table,
			state->selected->function_name, state->selected);
	}
}

static bool native_mir_test_build_ssa(native_mir_test_state *state)
{
	zend_optimizer_ctx optimizer;

	state->phase = NATIVE_MIR_TEST_PHASE_SSA;
	if (state->fault == NATIVE_MIR_TEST_FAULT_SSA_FAILURE) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_SSA, "ssa", "SSA0001",
			"injected SSA analysis failure");
		return false;
	}
	native_mir_test_init_script(state);
	zend_optimize_script(
		&state->script, NATIVE_MIR_TEST_OPTIMIZATION_LEVEL, 0);
	*state->compiled = state->script.main_op_array;
	state->ssa_arena = zend_arena_create(NATIVE_MIR_TEST_ARENA_SIZE);
	if (state->ssa_arena == NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_SSA, "ssa", "SSA0002",
			"unable to allocate the SSA arena");
		return false;
	}
	memset(&optimizer, 0, sizeof(optimizer));
	optimizer.arena = state->ssa_arena;
	optimizer.script = &state->script;
	optimizer.optimization_level = ZEND_OPTIMIZER_PASS_6;
	if (zend_dfa_analyze_op_array(
			state->selected, &optimizer, &state->ssa) == FAILURE) {
		state->ssa_arena = optimizer.arena;
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_SSA, "ssa", "SSA0001",
			"SSA analysis rejected the compiled source");
		return false;
	}
	state->ssa_arena = optimizer.arena;
	return true;
}

static void *native_mir_test_module_allocate(
	void *context, size_t size, size_t alignment)
{
	native_mir_test_module_host *host = context;

	if (host == NULL || host->arena == NULL || size == 0
			|| alignment > ZEND_MM_ALIGNMENT
			|| (host->fail_enabled
				&& host->successful_allocations >= host->fail_after)) {
		return NULL;
	}
	host->successful_allocations++;
	return zend_arena_alloc(&host->arena, size);
}

static void native_mir_test_module_reset(void *context)
{
	native_mir_test_module_host *host = context;

	if (host != NULL && host->arena != NULL) {
		zend_arena_destroy(host->arena);
		host->arena = NULL;
	}
}

static zend_mir_module *native_mir_test_module_create(
	void *context, zend_mir_module_id module_id,
	zend_mir_diagnostic_sink *diagnostics)
{
	native_mir_test_state *state = context;
	native_mir_test_module_host *host =
		state != NULL ? &state->module_host : NULL;
	zend_mir_allocator allocator;

	if (host == NULL || host->arena != NULL) {
		return NULL;
	}
	host->arena = zend_arena_create(NATIVE_MIR_TEST_ARENA_SIZE);
	if (host->arena == NULL) {
		return NULL;
	}
	allocator.context = host;
	allocator.allocate = native_mir_test_module_allocate;
	allocator.reset = native_mir_test_module_reset;
	return zend_mir_module_create(
		module_id, &allocator, state->mir_chunk_size,
		NULL, diagnostics);
}

static void native_mir_test_module_destroy(
	void *context, zend_mir_module *module)
{
	(void) context;
	zend_mir_module_destroy(module);
}

static zend_mir_mutator *native_mir_test_module_mutator(
	void *context, zend_mir_module *module)
{
	(void) context;
	return zend_mir_module_get_mutator(module);
}

static const zend_mir_view *native_mir_test_module_view(
	void *context, const zend_mir_module *module)
{
	(void) context;
	return zend_mir_module_get_view(module);
}

static bool native_mir_test_module_finalize(
	void *context, zend_mir_module *module)
{
	(void) context;
	return zend_mir_module_finalize(module);
}

static bool native_mir_test_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	native_mir_test_state *state = context;

	state->phase = NATIVE_MIR_TEST_PHASE_VERIFY;
	state->diagnostic_stage = "MIRV";
	return zend_mir_verify_stage1(view, diagnostics);
}

static bool native_mir_test_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	native_mir_test_state *state = context;

	state->phase = NATIVE_MIR_TEST_PHASE_VERIFY;
	state->diagnostic_stage = "MIRV";
	return zend_mir_verify_w03_scalar(view, diagnostics);
}

static bool native_mir_test_dump_write(
	void *context, const char *bytes, size_t length)
{
	native_mir_test_state *state = context;

	if (state == NULL || bytes == NULL) {
		return false;
	}
	if (state->fault == NATIVE_MIR_TEST_FAULT_DUMP_FAILURE) {
		state->dump_writes++;
		return false;
	}
	smart_str_appendl(&state->dump, bytes, length);
	return true;
}

/*
 * Provider-specific state is assembled here after the W03 specialist
 * providers have been merged. Keeping it private to the test extension
 * prevents a public production orchestration API from being introduced.
 */
static bool native_mir_test_publish_lowering_result(
	native_mir_test_state *state,
	zend_mir_lowering_result result,
	bool w04)
{
	zend_mir_diagnostic_sink diagnostics;
	const zend_mir_view *view;
	zend_mir_text_writer writer;
	char code[16];

	memset(&diagnostics, 0, sizeof(diagnostics));
	diagnostics.context = state;
	diagnostics.emit = native_mir_test_emit_mir_diagnostic;
	diagnostics.limit = state->diagnostic_limit;
	if (!(w04
			? zend_mir_lowering_result_is_w04_failure_atomic(&result)
			: zend_mir_lowering_result_is_failure_atomic(&result))) {
		if (result.module != NULL) {
			native_mir_test_module_destroy(state, result.module);
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0007",
			"integrated lowering returned a non-atomic result");
		return false;
	}
	snprintf(
		code, sizeof(code), "MIRL%04u",
		(unsigned int) result.diagnostic_code);
	if (result.status != ZEND_MIR_LOWERING_SUCCESS) {
		native_mir_test_fail(
			state,
			result.status == ZEND_MIR_LOWERING_FAILED
				? NATIVE_MIR_TEST_STATUS_ERROR
				: NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", code,
			"integrated lowering did not publish a module");
		return false;
	}

	state->module = result.module;
	view = native_mir_test_module_view(state, state->module);
	if (view == NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_DUMP, "MIRV", "MIRV0011",
			"verified module did not expose a read-only view");
		return false;
	}
	state->phase = NATIVE_MIR_TEST_PHASE_DUMP;
	state->diagnostic_stage = "MIRV";
	writer.context = state;
	writer.write = native_mir_test_dump_write;
	if (!zend_mir_dump_text(view, &writer, &diagnostics)) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_DUMP, "MIRV", "MIRV0011",
			"canonical MIR dump failed");
		return false;
	}
	smart_str_0(&state->dump);
	state->status = NATIVE_MIR_TEST_STATUS_ACCEPTED;
	state->phase = NATIVE_MIR_TEST_PHASE_COMPLETE;
	native_mir_test_add_diagnostic(
		state, "MIRL", "MIRL0000",
		w04 ? "W04 lowering completed" : "W03 lowering completed",
		false, 0);
	return true;
}

static bool native_mir_test_lower_w03_and_dump(native_mir_test_state *state)
{
	zend_mir_lowering_module_ops module_ops;
	zend_mir_diagnostic_sink diagnostics;
	zend_mir_lowering_result result;

	memset(&module_ops, 0, sizeof(module_ops));
	module_ops.context = state;
	module_ops.create = native_mir_test_module_create;
	module_ops.destroy = native_mir_test_module_destroy;
	module_ops.mutator = native_mir_test_module_mutator;
	module_ops.view = native_mir_test_module_view;
	module_ops.finalize = native_mir_test_module_finalize;
	module_ops.verify_stage1 = native_mir_test_verify_stage1;
	module_ops.verify_stage2 = native_mir_test_verify_stage2;

	memset(&diagnostics, 0, sizeof(diagnostics));
	diagnostics.context = state;
	diagnostics.emit = native_mir_test_emit_mir_diagnostic;
	diagnostics.limit = state->diagnostic_limit;
	result = zend_mir_lower_w03_zend_source(
		state->selected, &state->ssa, &module_ops, &diagnostics);
	return native_mir_test_publish_lowering_result(state, result, false);
}

/*
 * W04-A is deliberately absent from the specialist branch. The branch-local
 * bridge tests link the frozen entry symbol to a failure-atomic test stub.
 * W04-I must replace this stub-only invocation with A-owned source/context
 * setup when it wires the production files; this test extension does not
 * implement CFG semantics.
 */
static bool native_mir_test_lower_w04_and_dump(native_mir_test_state *state)
{
	zend_mir_lowering_result result;

	result = zend_mir_lower_w04_zend_source(NULL, NULL, NULL);
	return native_mir_test_publish_lowering_result(state, result, true);
}

static bool native_mir_test_lower_and_dump(native_mir_test_state *state)
{
	state->phase = NATIVE_MIR_TEST_PHASE_LOWERING;
	state->diagnostic_stage = "MIRL";
	if (state->fault == NATIVE_MIR_TEST_FAULT_LOWER_FAILURE) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0007",
			"injected lowering failure");
		return false;
	}
	return state->wave == 4
		? native_mir_test_lower_w04_and_dump(state)
		: native_mir_test_lower_w03_and_dump(state);
}

static bool native_mir_test_compile(native_mir_test_state *state)
{
	state->phase = NATIVE_MIR_TEST_PHASE_COMPILE;
	state->original_compiler_options = CG(compiler_options);
	state->compiler_options_saved = true;
	state->function_table_used_before = CG(function_table)->nNumUsed;
	state->function_table_snapshot = true;
	CG(compiler_options) =
		state->original_compiler_options | ZEND_COMPILE_WITHOUT_EXECUTION;
	if (state->fault == NATIVE_MIR_TEST_FAULT_COMPILE_BAILOUT) {
		zend_bailout();
	}
	state->compiled = zend_compile_string(
		state->source, ZSTR_VAL(state->filename),
		ZEND_COMPILE_POSITION_AT_OPEN_TAG);
	CG(compiler_options) = state->original_compiler_options;
	state->compiler_options_saved = false;
	if (state->compiled == NULL || EG(exception) != NULL) {
		if (EG(exception) != NULL) {
			zend_clear_exception();
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_COMPILE, "compile", "COMPILE_ERROR",
			"source compilation failed");
		return false;
	}
	state->selected = native_mir_test_select_op_array(state);
	if (state->selected == NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_COMPILE, "MIRL", "MIRL0006",
			"requested compiled function was not found");
		return false;
	}
	return true;
}

static void native_mir_test_cleanup(native_mir_test_state *state)
{
	HashTable *function_table;
	uint32_t index;

	if (state->compiler_options_saved) {
		CG(compiler_options) = state->original_compiler_options;
		state->compiler_options_saved = false;
	}
	if (state->module != NULL) {
		zend_mir_module_destroy(state->module);
		state->module = NULL;
	} else {
		native_mir_test_module_reset(&state->module_host);
	}
	if (state->ssa_arena != NULL) {
		zend_arena_destroy(state->ssa_arena);
		state->ssa_arena = NULL;
	}
	if (state->script_initialized) {
		zend_hash_destroy(&state->script.function_table);
		zend_hash_destroy(&state->script.class_table);
		state->script_initialized = false;
	}
	function_table = CG(function_table);
	if (state->function_table_snapshot && function_table != NULL) {
		index = function_table->nNumUsed;
		while (index > state->function_table_used_before) {
			Bucket *bucket = &function_table->arData[--index];

			if (Z_TYPE(bucket->val) != IS_UNDEF && bucket->key != NULL) {
				(void) zend_hash_del(function_table, bucket->key);
			}
		}
		state->function_table_snapshot = false;
	}
	if (state->compiled != NULL) {
		destroy_op_array(state->compiled);
		efree_size(state->compiled, sizeof(zend_op_array));
		state->compiled = NULL;
		state->selected = NULL;
	}
}

static void native_mir_test_build_result(
	native_mir_test_state *state, zval *return_value)
{
	zval source;
	zval diagnostics;
	uint64_t hash;
	char source_id[sizeof("fnv1a64:") + 16];
	uint32_t index;

	hash = native_mir_test_source_hash(state->filename, state->source);
	snprintf(source_id, sizeof(source_id), "fnv1a64:%016" PRIx64, hash);
	if (state->diagnostic_count > 1) {
		qsort(
			state->diagnostics, state->diagnostic_count,
			sizeof(state->diagnostics[0]),
			native_mir_test_compare_diagnostics);
	}

	array_init(return_value);
	add_assoc_long(
		return_value, "schema_version", NATIVE_MIR_TEST_SCHEMA_VERSION);
	if (state->wave == 4) {
		add_assoc_long(return_value, "wave", 4);
	}
	add_assoc_string(
		return_value, "status",
		(char *) native_mir_test_status_name(state->status));
	add_assoc_string(
		return_value, "phase",
		(char *) native_mir_test_phase_name(state->phase));

	array_init(&source);
	add_assoc_str(&source, "filename", zend_string_copy(state->filename));
	add_assoc_long(&source, "byte_length", ZSTR_LEN(state->source));
	add_assoc_string(&source, "source_id", source_id);
	add_assoc_zval(return_value, "source", &source);

	array_init(&diagnostics);
	for (index = 0; index < state->diagnostic_count; index++) {
		native_mir_test_diagnostic *source_diagnostic =
			&state->diagnostics[index];
		zval diagnostic;

		array_init(&diagnostic);
		add_assoc_string(
			&diagnostic, "stage", source_diagnostic->stage);
		add_assoc_string(
			&diagnostic, "code", source_diagnostic->code);
		add_assoc_string(
			&diagnostic, "message", source_diagnostic->message);
		if (source_diagnostic->has_opline) {
			add_assoc_long(
				&diagnostic, "opline", source_diagnostic->opline);
		} else {
			add_assoc_null(&diagnostic, "opline");
		}
		add_next_index_zval(&diagnostics, &diagnostic);
	}
	add_assoc_zval(return_value, "diagnostics", &diagnostics);
	if (state->status == NATIVE_MIR_TEST_STATUS_ACCEPTED
			&& state->dump.s != NULL) {
		add_assoc_str(
			return_value, "mir", zend_string_copy(state->dump.s));
	} else {
		add_assoc_null(return_value, "mir");
	}
}

ZEND_FUNCTION(native_mir_test_compile_dump)
{
	zend_string *source;
	zend_string *filename;
	HashTable *options = NULL;
	native_mir_test_state *state;
	bool bailed_out = false;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(source)
		Z_PARAM_PATH_STR(filename)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT(options)
	ZEND_PARSE_PARAMETERS_END();

	if (ZSTR_LEN(filename) == 0) {
		zend_argument_value_error(2, "must not be empty");
		RETURN_THROWS();
	}
	state = ecalloc(1, sizeof(*state));
	state->source = source;
	state->filename = filename;
	state->diagnostic_limit = NATIVE_MIR_TEST_DEFAULT_DIAGNOSTIC_LIMIT;
	state->wave = 3;
	state->mir_chunk_size = ZEND_MIR_CORE_DEFAULT_CHUNK_SIZE;
	state->phase = NATIVE_MIR_TEST_PHASE_COMPILE;
	state->status = NATIVE_MIR_TEST_STATUS_ERROR;
	state->diagnostics = ecalloc(
		NATIVE_MIR_TEST_MAX_DIAGNOSTIC_LIMIT,
		sizeof(state->diagnostics[0]));
	if (options != NULL && !native_mir_test_parse_options(state, options)) {
		native_mir_test_build_result(state, return_value);
		efree(state->diagnostics);
		efree(state);
		return;
	}
	state->module_host.fail_enabled =
		state->fault == NATIVE_MIR_TEST_FAULT_MODULE_OOM;
	state->module_host.fail_after = 0;

	zend_try {
		if (native_mir_test_compile(state)
				&& native_mir_test_build_ssa(state)) {
			(void) native_mir_test_lower_and_dump(state);
		}
	} zend_catch {
		bailed_out = true;
	} zend_end_try();

	if (bailed_out) {
		if (EG(exception) != NULL) {
			zend_clear_exception();
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR, state->phase,
			state->phase == NATIVE_MIR_TEST_PHASE_COMPILE
				? "compile" : "bridge",
			"BAILOUT", "native compile/dump phase bailed out");
	}
	native_mir_test_cleanup(state);
	native_mir_test_build_result(state, return_value);
	smart_str_free(&state->dump);
	efree(state->diagnostics);
	efree(state);
}

PHP_MINFO_FUNCTION(native_mir_test)
{
	(void) zend_module;
	php_info_print_table_start();
	php_info_print_table_row(
		2, "native_mir_test support", "enabled (test-only)");
	php_info_print_table_end();
}

zend_module_entry native_mir_test_module_entry = {
	STANDARD_MODULE_HEADER,
	"native_mir_test",
	ext_functions,
	NULL,
	NULL,
	NULL,
	NULL,
	PHP_MINFO(native_mir_test),
	PHP_NATIVE_MIR_TEST_VERSION,
	STANDARD_MODULE_PROPERTIES
};
