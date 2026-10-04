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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__APPLE__) && defined(__aarch64__)) \
		|| (defined(__linux__) && defined(__x86_64__))
# include <execinfo.h>
#endif
#include "php.h"
#include "php_native_mir_test.h"
#include "native_mir_test_arginfo.h"
#include "ext/standard/info.h"

#include "Zend/zend_compile.h"
#include "Zend/zend_execute.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_observer.h"
#include "Zend/zend_smart_str.h"
#include "Zend/zend_type_info.h"
#include "Zend/zend_vm_probe.h"
#include "Zend/zend_vm_opcodes.h"
#include "Zend/Optimizer/zend_func_info.h"
#include "Zend/Optimizer/zend_optimizer.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"

#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/MIR/zend_mir.h"
#include "Zend/Native/Calls/Model/zend_mir_call_model.h"
#include "Zend/Native/Compiler/zend_native_compiler.h"
#include "Zend/Native/Values/Lowering/zend_mir_value_lowering.h"
#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source_internal.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
#include "Zend/Native/Runtime/Common/zend_native_runtime.h"
#include "Zend/Native/TPDE/Common/zend_tpde_backend.h"

#define NATIVE_MIR_TEST_SCHEMA_VERSION 1
#define NATIVE_MIR_TEST_DEFAULT_DIAGNOSTIC_LIMIT 32
#define NATIVE_MIR_TEST_MAX_DIAGNOSTIC_LIMIT 256
#define NATIVE_MIR_TEST_ARENA_SIZE (64 * 1024)
#define NATIVE_MIR_TEST_MIN_MIR_CHUNK_SIZE 64
#define NATIVE_MIR_TEST_MAX_MIR_CHUNK_SIZE (1024 * 1024)
#define NATIVE_MIR_TEST_OPTIMIZATION_LEVEL ((zend_long) 0x7FFEBFFF)
#define NATIVE_MIR_TEST_MAX_FRAME_PROBES 2048
#define NATIVE_MIR_TEST_MAX_PROBE_ARGUMENTS 128

typedef enum _native_mir_test_phase {
	NATIVE_MIR_TEST_PHASE_COMPILE = 0,
	NATIVE_MIR_TEST_PHASE_SSA,
	NATIVE_MIR_TEST_PHASE_LOWERING,
	NATIVE_MIR_TEST_PHASE_VERIFY,
	NATIVE_MIR_TEST_PHASE_DUMP,
	NATIVE_MIR_TEST_PHASE_CODEGEN,
	NATIVE_MIR_TEST_PHASE_PUBLISH,
	NATIVE_MIR_TEST_PHASE_EXECUTE,
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
	NATIVE_MIR_TEST_FAULT_FINALIZE_FAILURE,
	NATIVE_MIR_TEST_FAULT_STAGE1_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_STAGE2_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_DUMP_FAILURE,
	NATIVE_MIR_TEST_FAULT_MAPPING_FAILURE,
	NATIVE_MIR_TEST_FAULT_ENTRY_PUBLISH_FAILURE
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

typedef struct _native_mir_test_frame_probe {
	const zend_string *caller_name;
	const zend_string *callee_name;
	uint32_t caller_line;
	uint32_t callee_line;
	uint32_t argument_count;
	uint8_t argument_types[NATIVE_MIR_TEST_MAX_PROBE_ARGUMENTS];
	bool previous_matches_caller;
} native_mir_test_frame_probe;

typedef struct _native_mir_test_state {
	struct _native_mir_test_state *retained_next;
	zend_string *source;
	zend_string *filename;
	zend_string *function_name;
	zend_op_array *compiled;
	zend_op_array *selected;
	uint8_t *source_opcodes;
	uint32_t source_opcode_count;
	zend_arena *ssa_arena;
	zend_ssa ssa;
	zend_script script;
	bool script_initialized;
	uint32_t original_compiler_options;
	bool compiler_options_saved;
	bool ignore_user_functions;
	uint32_t function_table_used_before;
	uint32_t function_table_used_after_compile;
	bool function_table_snapshot;
	uint32_t class_table_used_before;
	uint32_t class_table_used_after_compile;
	bool class_table_snapshot;
	zend_class_entry **detached_classes;
	uint32_t detached_class_count;
	native_mir_test_phase phase;
	native_mir_test_status status;
	native_mir_test_fault fault;
	native_mir_test_diagnostic *diagnostics;
	uint32_t diagnostic_count;
	uint32_t diagnostic_limit;
	bool execute_mode;
	zend_native_target target;
	size_t mir_chunk_size;
	const char *diagnostic_stage;
	native_mir_test_module_host module_host;
	native_mir_test_module_host *active_module_host;
	zend_mir_module *module;
	zend_native_image *native_image;
	zend_native_code *native_code;
	zend_native_compiler *product_compiler;
	zval native_result;
	bool native_result_valid;
	bool native_writable_after_publish;
	bool native_executable_after_publish;
	bool native_exception;
	bool native_bailout;
	uint64_t vm_handler_calls;
	uint64_t execute_ex_calls;
	uint64_t opline_handler_calls;
	uint32_t vm_probe_depth;
	struct _native_mir_test_state *vm_probe_parent;
	uint64_t user_opcode_calls;
	uint64_t generator_reentry_gateway_calls;
	user_opcode_handler_t previous_user_opcode_handler;
	uint32_t user_opcode_action;
	uint32_t user_opcode_advance;
	uint8_t user_opcode;
	bool user_opcode_configured;
	bool user_opcode_installed;
	bool user_opcode_entered;
	bool stack_probe_enabled;
	bool vm_probe_calibration_enabled;
	bool frame_chain_valid;
	native_mir_test_frame_probe *frame_probes;
	uint32_t frame_probe_count;
	uint32_t execute_repetitions;
	uint32_t completed_executions;
	uint32_t unwind_registrations_before;
	smart_str dump;
	uint32_t dump_writes;
} native_mir_test_state;

ZEND_TLS native_mir_test_state *native_mir_test_active_state;
ZEND_TLS native_mir_test_state *native_mir_test_retained_states;

void zend_native_mir_test_probe_vm_handler(void)
{
	native_mir_test_state *state = native_mir_test_active_state;

	while (state != NULL) {
		if (state->vm_probe_depth != 0) {
			state->vm_handler_calls++;
		}
		state = state->vm_probe_parent;
	}
}

void zend_native_mir_test_probe_execute_ex(void)
{
	native_mir_test_state *state = native_mir_test_active_state;

	while (state != NULL) {
		if (state->vm_probe_depth != 0) {
			state->execute_ex_calls++;
		}
		state = state->vm_probe_parent;
	}
}

void zend_native_mir_test_probe_opline_handler(void)
{
	native_mir_test_state *state = native_mir_test_active_state;

	while (state != NULL) {
		if (state->vm_probe_depth != 0) {
			state->opline_handler_calls++;
		}
		state = state->vm_probe_parent;
	}
}

static int native_mir_test_user_opcode_handler(zend_execute_data *execute_data)
{
	native_mir_test_state *state = native_mir_test_active_state;

	if (state == NULL || !state->user_opcode_installed
			|| execute_data == NULL || execute_data->opline == NULL
			|| execute_data->opline->opcode != state->user_opcode) {
		return ZEND_USER_OPCODE_DISPATCH;
	}
	state->user_opcode_calls++;
	if (state->user_opcode_action == ZEND_USER_OPCODE_ENTER) {
		zend_execute_data *entered;

		if (state->user_opcode_entered) {
			return ZEND_USER_OPCODE_CONTINUE;
		}
		state->user_opcode_entered = true;
		entered = zend_vm_stack_push_call_frame(
			ZEND_CALL_NESTED_FUNCTION, execute_data->func, 0, NULL);
		zend_init_func_execute_data(
			entered, &execute_data->func->op_array, NULL);
		EG(current_execute_data) = entered;
		return ZEND_USER_OPCODE_ENTER;
	}
	if (state->user_opcode_advance != 0
			&& execute_data->func != NULL
			&& ZEND_USER_CODE(execute_data->func->type)
			&& execute_data->opline
				>= execute_data->func->op_array.opcodes
			&& execute_data->opline
				< execute_data->func->op_array.opcodes
					+ execute_data->func->op_array.last
			&& state->user_opcode_advance
				< (uint32_t) (
					execute_data->func->op_array.opcodes
						+ execute_data->func->op_array.last
					- execute_data->opline)) {
		execute_data->opline += state->user_opcode_advance;
	}
	return (int) state->user_opcode_action;
}

static bool native_mir_test_install_user_opcode(native_mir_test_state *state)
{
	if (!state->user_opcode_configured) {
		return true;
	}
	state->previous_user_opcode_handler =
		zend_get_user_opcode_handler(state->user_opcode);
	if (zend_set_user_opcode_handler(
			state->user_opcode, native_mir_test_user_opcode_handler)
			== FAILURE) {
		return false;
	}
	state->user_opcode_installed = true;
	return true;
}

static void native_mir_test_restore_user_opcode(native_mir_test_state *state)
{
	if (!state->user_opcode_installed) {
		return;
	}
	(void) zend_set_user_opcode_handler(
		state->user_opcode, state->previous_user_opcode_handler);
	state->user_opcode_installed = false;
}

static void native_mir_test_cleanup(native_mir_test_state *state);

static void native_mir_test_release_state(native_mir_test_state *state)
{
	native_mir_test_cleanup(state);
	smart_str_free(&state->dump);
	efree(state->source_opcodes);
	efree(state->diagnostics);
	efree(state->frame_probes);
	efree(state);
}

static void native_mir_test_retain_state(native_mir_test_state *state)
{
	state->retained_next = native_mir_test_retained_states;
	native_mir_test_retained_states = state;
}

static void native_mir_test_detach_compiled_symbols(
	native_mir_test_state *state)
{
	HashTable *function_table = CG(function_table);
	HashTable *class_table = CG(class_table);
	uint32_t index;
	uint32_t class_count = 0;

	if (state->function_table_snapshot && function_table != NULL) {
		index = state->function_table_used_after_compile;
		while (index > state->function_table_used_before) {
			Bucket *bucket = &function_table->arData[--index];

			if (Z_TYPE(bucket->val) != IS_UNDEF && bucket->key != NULL) {
				(void) zend_hash_del(function_table, bucket->key);
			}
		}
		state->function_table_snapshot = false;
	}
	if (!state->class_table_snapshot || class_table == NULL) {
		return;
	}
	for (index = state->class_table_used_before;
			index < state->class_table_used_after_compile; index++) {
		Bucket *bucket = &class_table->arData[index];

		if (Z_TYPE(bucket->val) == IS_PTR) {
			class_count++;
		}
	}
	if (class_count != 0) {
		state->detached_classes = safe_emalloc(
			class_count, sizeof(*state->detached_classes), 0);
	}
	index = state->class_table_used_after_compile;
	while (index > state->class_table_used_before) {
		Bucket *bucket = &class_table->arData[--index];

		if (Z_TYPE(bucket->val) == IS_PTR) {
			zend_class_add_ref(&bucket->val);
			state->detached_classes[state->detached_class_count++] =
				Z_PTR(bucket->val);
		}
		if (Z_TYPE(bucket->val) != IS_UNDEF && bucket->key != NULL) {
			(void) zend_hash_del(class_table, bucket->key);
		}
	}
	state->class_table_snapshot = false;
}

static void native_mir_test_frame_probe_record(
	void *context,
	const zend_execute_data *caller,
	const zend_execute_data *callee)
{
	native_mir_test_state *state = context;
	native_mir_test_frame_probe *record;

	if (state == NULL || caller == NULL || callee == NULL) {
		if (state != NULL) {
			state->frame_chain_valid = false;
		}
		return;
	}
	if (callee->func != NULL
			&& (callee->func->common.fn_flags & ZEND_ACC_GENERATOR) != 0
			&& callee->opline != NULL
			&& callee->opline->opcode != ZEND_GENERATOR_CREATE) {
		state->generator_reentry_gateway_calls++;
		return;
	}
	if (state->frame_probe_count >= NATIVE_MIR_TEST_MAX_FRAME_PROBES) {
		state->frame_chain_valid = false;
		return;
	}
	record = &state->frame_probes[state->frame_probe_count++];
	record->caller_name = caller->func != NULL
		? caller->func->common.function_name : NULL;
	record->callee_name = callee->func != NULL
		? callee->func->common.function_name : NULL;
	record->caller_line = caller->func != NULL
		&& caller->func->type == ZEND_USER_FUNCTION
		&& caller->opline != NULL ? caller->opline->lineno : 0;
	record->callee_line = callee->func != NULL
		&& callee->func->type == ZEND_USER_FUNCTION
		&& callee->opline != NULL ? callee->opline->lineno : 0;
	record->argument_count = ZEND_CALL_NUM_ARGS(callee);
	if (record->argument_count > NATIVE_MIR_TEST_MAX_PROBE_ARGUMENTS) {
		state->frame_chain_valid = false;
	} else {
		uint32_t argument_index;

		for (argument_index = 0;
				argument_index < record->argument_count; argument_index++) {
			const zval *argument = ZEND_CALL_ARG(
				(zend_execute_data *) callee, argument_index + 1);

			record->argument_types[argument_index] = Z_TYPE_P(argument);
		}
	}
	record->previous_matches_caller = callee->prev_execute_data == caller;
	state->frame_chain_valid = state->frame_chain_valid
		&& record->previous_matches_caller;
}

extern zend_mir_lowering_result zend_mir_lower_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

extern zend_function *zend_mir_zend_source_resolve_user_method_call(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	uint32_t init_opline_index);

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
		case NATIVE_MIR_TEST_PHASE_CODEGEN:
			return "codegen";
		case NATIVE_MIR_TEST_PHASE_PUBLISH:
			return "publish";
		case NATIVE_MIR_TEST_PHASE_EXECUTE:
			return "execute";
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
	} else if (zend_string_equals_literal(value, "finalize_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_FINALIZE_FAILURE;
	} else if (zend_string_equals_literal(value, "stage1_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_STAGE1_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(value, "stage2_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_STAGE2_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(value, "dump_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_DUMP_FAILURE;
	} else if (zend_string_equals_literal(value, "mapping_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_MAPPING_FAILURE;
	} else if (zend_string_equals_literal(value, "entry_publish_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_ENTRY_PUBLISH_FAILURE;
	} else {
		return false;
	}
	return true;
}

static bool native_mir_test_opcode_from_name(
	zend_string *name, uint8_t *opcode_out)
{
	uint32_t opcode;

	if (name == NULL || opcode_out == NULL) {
		return false;
	}
	for (opcode = 0; opcode <= UINT8_MAX; opcode++) {
		const char *candidate = zend_get_opcode_name((uint8_t) opcode);

		if (candidate != NULL
				&& ZSTR_LEN(name) == strlen(candidate)
				&& memcmp(ZSTR_VAL(name), candidate, ZSTR_LEN(name)) == 0) {
			*opcode_out = (uint8_t) opcode;
			return true;
		}
	}
	return false;
}

static bool native_mir_test_parse_user_opcode(
	native_mir_test_state *state, zval *value)
{
	HashTable *configuration;
	zval *opcode;
	zval *action;
	zval *dispatch_to;
	zval *advance;
	uint8_t selected_opcode;
	uint8_t target_opcode = 0;
	uint32_t selected_action;

	if (!state->execute_mode || Z_TYPE_P(value) != IS_ARRAY) {
		return false;
	}
	configuration = Z_ARRVAL_P(value);
	opcode = zend_hash_str_find(configuration, ZEND_STRL("opcode"));
	action = zend_hash_str_find(configuration, ZEND_STRL("action"));
	dispatch_to = zend_hash_str_find(
		configuration, ZEND_STRL("dispatch_to"));
	advance = zend_hash_str_find(configuration, ZEND_STRL("advance"));
	if (opcode == NULL || Z_TYPE_P(opcode) != IS_STRING
			|| !native_mir_test_opcode_from_name(
				Z_STR_P(opcode), &selected_opcode)
			|| action == NULL || Z_TYPE_P(action) != IS_STRING) {
		return false;
	}
	if (zend_string_equals_literal(Z_STR_P(action), "continue")) {
		selected_action = ZEND_USER_OPCODE_CONTINUE;
	} else if (zend_string_equals_literal(Z_STR_P(action), "return")) {
		selected_action = ZEND_USER_OPCODE_RETURN;
	} else if (zend_string_equals_literal(Z_STR_P(action), "dispatch")) {
		selected_action = ZEND_USER_OPCODE_DISPATCH;
	} else if (zend_string_equals_literal(Z_STR_P(action), "enter")) {
		selected_action = ZEND_USER_OPCODE_ENTER;
	} else if (zend_string_equals_literal(Z_STR_P(action), "leave")) {
		selected_action = ZEND_USER_OPCODE_LEAVE;
	} else if (zend_string_equals_literal(Z_STR_P(action), "dispatch_to")) {
		if (dispatch_to == NULL || Z_TYPE_P(dispatch_to) != IS_STRING
				|| !native_mir_test_opcode_from_name(
					Z_STR_P(dispatch_to), &target_opcode)) {
			return false;
		}
		selected_action = ZEND_USER_OPCODE_DISPATCH_TO | target_opcode;
	} else {
		return false;
	}
	if (dispatch_to != NULL
			&& selected_action < ZEND_USER_OPCODE_DISPATCH_TO) {
		return false;
	}
	if (advance != NULL) {
		if (Z_TYPE_P(advance) != IS_LONG || Z_LVAL_P(advance) < 0
				|| (zend_ulong) Z_LVAL_P(advance) > UINT32_MAX) {
			return false;
		}
		state->user_opcode_advance = (uint32_t) Z_LVAL_P(advance);
	}
	state->user_opcode = selected_opcode;
	state->user_opcode_action = selected_action;
	state->user_opcode_configured = true;
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
		} else if (zend_string_equals_literal(key, "target")) {
			if (!state->execute_mode || Z_TYPE_P(value) != IS_STRING) {
				goto invalid_value;
			}
			if (zend_string_equals_literal(Z_STR_P(value), "darwin-arm64-dev")) {
				state->target = ZEND_NATIVE_TARGET_DARWIN_ARM64;
			} else if (zend_string_equals_literal(
					Z_STR_P(value), "linux-amd64-prod")) {
				state->target = ZEND_NATIVE_TARGET_LINUX_AMD64;
			} else {
				goto invalid_value;
			}
		} else if (zend_string_equals_literal(key, "repeat")) {
			if (!state->execute_mode || Z_TYPE_P(value) != IS_LONG
					|| Z_LVAL_P(value) < 1 || Z_LVAL_P(value) > 1000) {
				goto invalid_value;
			}
			state->execute_repetitions = (uint32_t) Z_LVAL_P(value);
		} else if (zend_string_equals_literal(key, "stack_probe")) {
			if (!state->execute_mode || Z_TYPE_P(value) != IS_TRUE) {
				goto invalid_value;
			}
			state->stack_probe_enabled = true;
		} else if (zend_string_equals_literal(key, "vm_probe_calibration")) {
			if (!state->execute_mode || Z_TYPE_P(value) != IS_TRUE) {
				goto invalid_value;
			}
			state->vm_probe_calibration_enabled = true;
		} else if (zend_string_equals_literal(key, "user_opcode")) {
			if (!native_mir_test_parse_user_opcode(state, value)) {
				goto invalid_value;
			}
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
		} else if (zend_string_equals_literal(key, "compiler_mode")) {
			if (Z_TYPE_P(value) != IS_STRING
					|| !zend_string_equals_literal(
						Z_STR_P(value), "ignore_user_functions")) {
				goto invalid_value;
			}
			state->ignore_user_functions = true;
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

static bool native_mir_test_validate_arguments(
	native_mir_test_state *state, HashTable *arguments)
{
	zend_ulong index;
	zend_string *key;
	uint32_t expected_index = 0;

	if (arguments == NULL) {
		return true;
	}
	ZEND_HASH_FOREACH_KEY(arguments, index, key) {
		if (key != NULL || index != expected_index) {
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				NATIVE_MIR_TEST_PHASE_COMPILE, "bridge", "INVALID_ARGUMENTS",
				"arguments must be a dense zero-based numeric array");
			return false;
		}
		expected_index++;
	} ZEND_HASH_FOREACH_END();
	return true;
}

static zend_op_array *native_mir_test_select_op_array(
	native_mir_test_state *state)
{
	HashTable *function_table;
	const char *separator;
	uint32_t index;

	if (state->function_name == NULL) {
		return state->compiled;
	}
	separator = php_memnstr(
		ZSTR_VAL(state->function_name), "::", sizeof("::") - 1,
		ZSTR_VAL(state->function_name) + ZSTR_LEN(state->function_name));
	if (separator != NULL) {
		size_t class_length = (size_t) (
			separator - ZSTR_VAL(state->function_name));
		size_t method_length = ZSTR_LEN(state->function_name)
			- class_length - (sizeof("::") - 1);
		zend_string *class_name;
		zend_string *method_name;
		zend_class_entry *ce;
		zend_function *method;

		if (class_length == 0 || method_length == 0) {
			return NULL;
		}
		class_name = zend_string_init(
			ZSTR_VAL(state->function_name), class_length, false);
		method_name = zend_string_init(
			separator + (sizeof("::") - 1), method_length, false);
		zend_str_tolower(ZSTR_VAL(class_name), class_length);
		zend_str_tolower(ZSTR_VAL(method_name), method_length);
		ce = zend_hash_find_ptr(CG(class_table), class_name);
		method = ce != NULL
			? zend_hash_find_ptr(&ce->function_table, method_name) : NULL;
		zend_string_release(class_name);
		zend_string_release(method_name);
		return method != NULL && method->type == ZEND_USER_FUNCTION
			? &method->op_array : NULL;
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
	HashTable *function_table;
	HashTable *class_table;
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

		if (function != NULL && function->function_name != NULL
				&& function->scope == NULL) {
			(void) zend_hash_update_ptr(
				&state->script.function_table,
				function->function_name, function);
		} else if (function != NULL && function->scope != NULL
				&& function->scope->name != NULL) {
			zend_string *lcname = zend_string_tolower(function->scope->name);

			(void) zend_hash_update_ptr(
				&state->script.class_table, lcname, function->scope);
			zend_string_release(lcname);
		}
	}
	function_table = CG(function_table);
	if (state->function_table_snapshot && function_table != NULL) {
		for (index = state->function_table_used_before;
				index < function_table->nNumUsed; index++) {
			Bucket *bucket = &function_table->arData[index];
			zend_function *function;

			if (Z_TYPE(bucket->val) != IS_PTR || bucket->key == NULL) {
				continue;
			}
			function = Z_PTR(bucket->val);
			if (function == NULL || function->type != ZEND_USER_FUNCTION
					|| function->op_array.function_name == NULL
					|| function->op_array.filename == NULL
					|| state->compiled->filename == NULL
					|| !zend_string_equals(
						function->op_array.filename,
						state->compiled->filename)) {
				continue;
			}
			(void) zend_hash_update_ptr(
				&state->script.function_table,
				function->op_array.function_name, function);
		}
	}
	if (state->selected != state->compiled
			&& state->selected->function_name != NULL
			&& state->selected->scope == NULL) {
		(void) zend_hash_update_ptr(
			&state->script.function_table,
			state->selected->function_name, state->selected);
	}
	class_table = CG(class_table);
	if (state->class_table_snapshot && class_table != NULL) {
		for (index = state->class_table_used_before;
				index < class_table->nNumUsed; index++) {
			Bucket *bucket = &class_table->arData[index];
			zend_class_entry *ce;

			if (Z_TYPE(bucket->val) != IS_PTR || bucket->key == NULL) {
				continue;
			}
			ce = Z_PTR(bucket->val);
			if (ce == NULL || ce->type != ZEND_USER_CLASS
					|| ce->info.user.filename == NULL
					|| state->compiled->filename == NULL
					|| !zend_string_equals(
						ce->info.user.filename, state->compiled->filename)) {
				continue;
			}
			(void) zend_hash_update_ptr(
				&state->script.class_table, bucket->key, ce);
		}
	}
}

static bool native_mir_test_bind_classes(native_mir_test_state *state)
{
	uint32_t index;

	for (index = 0; index < state->compiled->last; index++) {
		zend_op *opline = &state->compiled->opcodes[index];

		if (opline->opcode == ZEND_DECLARE_CLASS) {
			zval *lcname = RT_CONSTANT(opline, opline->op1);
			zend_string *lc_parent_name = opline->op2_type == IS_CONST
				? Z_STR_P(RT_CONSTANT(opline, opline->op2)) : NULL;

			if (do_bind_class(lcname, lc_parent_name) == FAILURE) {
				return false;
			}
		} else if (opline->opcode == ZEND_DECLARE_CLASS_DELAYED) {
			zval *lcname = RT_CONSTANT(opline, opline->op1);
			zval *slot = zend_hash_find_known_hash(
				EG(class_table), Z_STR_P(lcname + 1));

			if (slot != NULL && zend_bind_class_in_slot(
					slot, lcname,
					Z_STR_P(RT_CONSTANT(opline, opline->op2))) == NULL) {
				return false;
			}
		}
		if (EG(exception) != NULL) {
			return false;
		}
	}
	return true;
}

static void native_mir_test_capture_source_opcodes(native_mir_test_state *state)
{
	uint32_t index;

	if (state->source_opcodes != NULL || state->selected->last == 0) {
		return;
	}
	state->source_opcodes =
		emalloc(state->selected->last * sizeof(*state->source_opcodes));
	state->source_opcode_count = state->selected->last;
	for (index = 0; index < state->source_opcode_count; index++) {
		state->source_opcodes[index] = state->selected->opcodes[index].opcode;
	}
}

static bool native_mir_test_build_ssa(native_mir_test_state *state)
{
	zend_optimizer_ctx optimizer;

	state->phase = NATIVE_MIR_TEST_PHASE_SSA;
	native_mir_test_capture_source_opcodes(state);
	if (state->fault == NATIVE_MIR_TEST_FAULT_SSA_FAILURE) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_SSA, "ssa", "SSA0001",
			"injected SSA analysis failure");
		return false;
	}
	native_mir_test_init_script(state);
	state->original_compiler_options = CG(compiler_options);
	state->compiler_options_saved = true;
	if (state->ignore_user_functions) {
		CG(compiler_options) |= ZEND_COMPILE_IGNORE_USER_FUNCTIONS;
	}
	/* Classes are linked before methods are selected so inherited and trait
	 * methods are addressable.  An imported trait method and its trait
	 * declaration may then share opcode storage; optimizing the synthetic
	 * script would visit that storage twice and leave stale SSA definitions
	 * behind, so SSA is built from the linked source directly.  Production
	 * OPcache optimizes before class linking and has no such alias. */
	CG(compiler_options) = state->original_compiler_options;
	state->compiler_options_saved = false;
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
	if (zend_dfa_analyze_op_array_with_dynamic_bindings(
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
	void *allocation;
	size_t alignment_mask;
	uintptr_t address;

	if (host == NULL || host->arena == NULL || size == 0
			|| alignment == 0 || (alignment & (alignment - 1)) != 0
			|| size > SIZE_MAX - (alignment - 1)
			|| (host->fail_enabled
				&& host->successful_allocations >= host->fail_after)) {
		return NULL;
	}
	alignment_mask = alignment - 1;
	host->successful_allocations++;
	allocation = zend_arena_alloc(&host->arena, size + alignment_mask);
	address = (uintptr_t) allocation;
	if (address > UINTPTR_MAX - alignment_mask) {
		return NULL;
	}
	return (void *) ((address + alignment_mask) & ~alignment_mask);
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
	native_mir_test_module_host *host = state != NULL
		? (state->active_module_host != NULL
			? state->active_module_host : &state->module_host)
		: NULL;
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
	native_mir_test_state *state = context;

	if (state->fault == NATIVE_MIR_TEST_FAULT_FINALIZE_FAILURE) {
		return false;
	}
	return zend_mir_module_finalize(module);
}

static bool native_mir_test_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	native_mir_test_state *state = context;

	state->phase = NATIVE_MIR_TEST_PHASE_VERIFY;
	state->diagnostic_stage = "MIRV";
	if (state->fault == NATIVE_MIR_TEST_FAULT_STAGE1_VERIFIER_FAILURE) {
		return false;
	}
	return zend_mir_verify_stage1(view, diagnostics);
}

static bool native_mir_test_find_value(
	const zend_mir_view *view, zend_mir_value_id value_id,
	zend_mir_value_record *out)
{
	uint32_t index;

	for (index = 0; index < view->value_count(view->context); index++) {
		zend_mir_value_record value;
		if (!view->value_at(view->context, index, &value)) {
			return false;
		}
		if (value.id == value_id) {
			*out = value;
			return true;
		}
	}
	return false;
}

static bool native_mir_test_find_fact(
	const zend_mir_view *view, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *out)
{
	uint32_t index;

	for (index = 0; index < view->value_fact_count(view->context); index++) {
		zend_mir_value_fact_ref fact;
		if (!view->value_fact_at(view->context, index, &fact)) {
			return false;
		}
		if (fact.value_id == value_id) {
			*out = fact;
			return true;
		}
	}
	return false;
}

static bool native_mir_test_scalar_requirement_matches(
	const zend_mir_scalar_value_requirement *requirement,
	const zend_mir_value_record *value,
	const zend_mir_value_fact_ref *fact)
{
	return zend_mir_scalar_fact_is_well_formed(fact)
		&& (requirement->representation == ZEND_MIR_REPRESENTATION_INVALID
			|| value->representation == requirement->representation)
		&& (requirement->exact_type == ZEND_MIR_SCALAR_TYPE_NONE
			|| fact->exact_type == requirement->exact_type)
		&& (fact->flags & requirement->required_flags)
			== requirement->required_flags
		&& value->ownership == requirement->ownership;
}

static bool native_mir_test_verify_scalar(
	native_mir_test_state *state, const zend_mir_view *view)
{
	uint32_t instruction_index;

	if (view->instruction_count == NULL || view->instruction_at == NULL
			|| view->instruction_operand_count == NULL
			|| view->instruction_operand_at == NULL
			|| view->value_count == NULL || view->value_at == NULL
			|| view->value_fact_count == NULL || view->value_fact_at == NULL) {
		native_mir_test_add_diagnostic(
			state, "MIRV", "MIRV0601",
			"scalar verifier view is incomplete", false, 0);
		return false;
	}
	for (instruction_index = 0;
			instruction_index < view->instruction_count(view->context);
			instruction_index++) {
		zend_mir_instruction_record instruction;
		const zend_mir_scalar_descriptor *descriptor;
		uint32_t operand_count;
		uint32_t operand_index;

		if (!view->instruction_at(
				view->context, instruction_index, &instruction)) {
			native_mir_test_add_diagnostic(
				state, "MIRV", "MIRV0604",
				"scalar instruction callback failed", false, 0);
			return false;
		}
		descriptor = zend_mir_scalar_descriptor_at(instruction.opcode);
		if (descriptor == NULL) {
			continue;
		}
		operand_count = view->instruction_operand_count(
			view->context, instruction.id);
		if (descriptor->opcode != instruction.opcode
				|| operand_count != descriptor->operand_count
				|| instruction.effects != descriptor->effects
				|| instruction.reads != descriptor->reads
				|| instruction.writes != descriptor->writes
				|| instruction.barriers != descriptor->barriers
				|| instruction.ownership_actions
					!= descriptor->ownership_actions
				|| (descriptor->requires_source
					&& !zend_mir_id_is_valid(
						instruction.source_position_id))
				|| (!descriptor->requires_frame
					&& zend_mir_id_is_valid(
						instruction.frame_state_id))) {
			native_mir_test_add_diagnostic(
				state, "MIRV", "MIRV0624",
				"scalar instruction violates its descriptor",
				false, 0);
			return false;
		}
		for (operand_index = 0; operand_index < operand_count;
				operand_index++) {
			zend_mir_value_id operand_id;
			zend_mir_value_record value;
			zend_mir_value_fact_ref fact;
			if (!view->instruction_operand_at(
					view->context, instruction.id, operand_index,
					&operand_id)
					|| !native_mir_test_find_value(
						view, operand_id, &value)
					|| !native_mir_test_find_fact(
						view, operand_id, &fact)
					|| !native_mir_test_scalar_requirement_matches(
						&descriptor->operands[operand_index],
						&value, &fact)) {
				native_mir_test_add_diagnostic(
					state, "MIRV", "MIRV0621",
					"scalar operand lacks its exact proof",
					false, 0);
				return false;
			}
		}
		if (descriptor->has_result) {
			zend_mir_value_record value;
			zend_mir_value_fact_ref fact;
			if (!zend_mir_id_is_valid(instruction.result_id)
					|| instruction.representation
						!= descriptor->result.representation
					|| !native_mir_test_find_value(
						view, instruction.result_id, &value)
					|| !native_mir_test_find_fact(
						view, instruction.result_id, &fact)
					|| !native_mir_test_scalar_requirement_matches(
						&descriptor->result, &value, &fact)) {
				native_mir_test_add_diagnostic(
					state, "MIRV", "MIRV0622",
					"scalar result lacks its exact proof",
					false, 0);
				return false;
			}
		} else if (zend_mir_id_is_valid(instruction.result_id)
				|| instruction.representation
					!= ZEND_MIR_REPRESENTATION_VOID) {
			native_mir_test_add_diagnostic(
				state, "MIRV", "MIRV0622",
				"scalar drop defines an unexpected result",
				false, 0);
			return false;
		}
	}
	return true;
}

static bool native_mir_test_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	native_mir_test_state *state = context;

	state->phase = NATIVE_MIR_TEST_PHASE_VERIFY;
	state->diagnostic_stage = "MIRV";
	if (state->fault == NATIVE_MIR_TEST_FAULT_STAGE2_VERIFIER_FAILURE) {
		return false;
	}
	(void) diagnostics;
	return native_mir_test_verify_scalar(state, view);
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
 * Provider-specific state stays private to the test extension so the test
 * adapter does not introduce a public production orchestration API.
 */
static bool native_mir_test_publish_lowering_result(
	native_mir_test_state *state,
	zend_mir_lowering_result result)
{
	zend_mir_diagnostic_sink diagnostics;
	const zend_mir_view *view;
	zend_mir_text_writer writer;
	char code[16];
	char message[32];

	memset(&diagnostics, 0, sizeof(diagnostics));
	diagnostics.context = state;
	diagnostics.emit = native_mir_test_emit_mir_diagnostic;
	diagnostics.limit = state->diagnostic_limit;
	if (!((result.status == ZEND_MIR_LOWERING_SUCCESS
				&& result.diagnostic_code == ZEND_MIRL_OK
				&& result.guarantees
					== ZEND_MIR_LOWERING_GUARANTEE_FINALIZED
				&& result.module != NULL)
			|| (result.status != ZEND_MIR_LOWERING_STATUS_INVALID
				&& result.status != ZEND_MIR_LOWERING_SUCCESS
				&& result.diagnostic_code != ZEND_MIRL_OK
				&& result.guarantees == 0
				&& result.module == NULL))) {
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
	if (state->execute_mode) {
		state->status = NATIVE_MIR_TEST_STATUS_ACCEPTED;
		state->phase = NATIVE_MIR_TEST_PHASE_COMPLETE;
		snprintf(message, sizeof(message), "lowering completed");
		native_mir_test_add_diagnostic(
			state, "MIRL", "MIRL0000", message, false, 0);
		return true;
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
	snprintf(message, sizeof(message), "lowering completed");
	native_mir_test_add_diagnostic(
		state, "MIRL", "MIRL0000", message, false, 0);
	return true;
}

static bool native_mir_test_lower_module_and_dump(native_mir_test_state *state)
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
	result = state->target == ZEND_NATIVE_TARGET_LINUX_AMD64
		? zend_mir_lower_typed_zend_op_array(
			&state->script, state->selected, &state->ssa,
			&module_ops, &diagnostics)
		: zend_mir_lower_zend_op_array(
			&state->script, state->selected, &state->ssa,
			&module_ops, &diagnostics);
	if (!zend_mir_lowering_result_is_failure_atomic(
			&result, ZEND_MIR_LOWERING_GUARANTEE_FINALIZED)) {
		char detail[256];

		snprintf(
			detail, sizeof(detail),
			"lowering returned a non-atomic result "
			"(status=%u diagnostic=%u guarantees=%u module=%u)",
			(unsigned int) result.status,
			(unsigned int) result.diagnostic_code,
			(unsigned int) result.guarantees,
			(unsigned int) (result.module != NULL));
		if (result.module != NULL) {
			native_mir_test_module_destroy(
				state, result.module);
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0007",
			detail);
		return false;
	}
	return native_mir_test_publish_lowering_result(state, result);
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
	return native_mir_test_lower_module_and_dump(state);
}

static bool native_mir_test_compile(native_mir_test_state *state)
{
	state->phase = NATIVE_MIR_TEST_PHASE_COMPILE;
	state->original_compiler_options = CG(compiler_options);
	state->compiler_options_saved = true;
	state->function_table_used_before = CG(function_table)->nNumUsed;
	state->function_table_snapshot = true;
	state->class_table_used_before = CG(class_table)->nNumUsed;
	state->class_table_snapshot = true;
	CG(compiler_options) =
		state->original_compiler_options | ZEND_COMPILE_WITHOUT_EXECUTION;
	if (state->ignore_user_functions) {
		CG(compiler_options) |= ZEND_COMPILE_IGNORE_USER_FUNCTIONS;
	}
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
	if (!native_mir_test_bind_classes(state)) {
		if (EG(exception) != NULL) {
			zend_clear_exception();
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_COMPILE, "compile", "COMPILE_ERROR",
			"source class linking failed");
		return false;
	}
	state->function_table_used_after_compile =
		CG(function_table)->nNumUsed;
	state->class_table_used_after_compile =
		CG(class_table)->nNumUsed;
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

static void native_mir_test_backend_failure(
	native_mir_test_state *state,
	native_mir_test_phase phase,
	const zend_native_diagnostic *diagnostic)
{
	char code[16];

	snprintf(code, sizeof(code), "NATIVE%04u",
		(unsigned int) (diagnostic != NULL ? diagnostic->code : 0));
	native_mir_test_fail(
		state, NATIVE_MIR_TEST_STATUS_ERROR, phase, "native", code,
		diagnostic != NULL && diagnostic->message[0] != '\0'
			? diagnostic->message : "native backend operation failed");
}

static native_mir_test_phase native_mir_test_product_phase(
	zend_native_compile_phase phase)
{
	switch (phase) {
		case ZEND_NATIVE_COMPILE_PHASE_SSA:
			return NATIVE_MIR_TEST_PHASE_SSA;
		case ZEND_NATIVE_COMPILE_PHASE_LOWERING:
			return NATIVE_MIR_TEST_PHASE_LOWERING;
		case ZEND_NATIVE_COMPILE_PHASE_CODEGEN:
			return NATIVE_MIR_TEST_PHASE_CODEGEN;
		case ZEND_NATIVE_COMPILE_PHASE_PUBLISH:
			return NATIVE_MIR_TEST_PHASE_PUBLISH;
		case ZEND_NATIVE_COMPILE_PHASE_EXECUTE:
			return NATIVE_MIR_TEST_PHASE_EXECUTE;
	}
	return NATIVE_MIR_TEST_PHASE_COMPILE;
}

static void native_mir_test_product_observer(
	void *context, const zend_native_compile_diagnostic *source)
{
	native_mir_test_state *state = context;
	char code[16];
	const char *stage;

	if (state == NULL || source == NULL) {
		return;
	}
	if (native_mir_test_extract_token(source->message, code)) {
		stage = memcmp(code, "MIRL", 4) == 0 ? "MIRL" : "MIRV";
	} else if (source->phase == ZEND_NATIVE_COMPILE_PHASE_LOWERING) {
		stage = "MIRL";
		snprintf(code, sizeof(code), "MIRL%04u",
			(unsigned int) source->code);
	} else if (source->phase == ZEND_NATIVE_COMPILE_PHASE_SSA) {
		stage = "ssa";
		snprintf(code, sizeof(code), "SSA%04u",
			(unsigned int) source->code);
	} else {
		stage = "native";
		snprintf(code, sizeof(code), "NATIVE%04u",
			(unsigned int) source->code);
	}
	native_mir_test_add_diagnostic(
		state, stage, code,
		source->message[0] != '\0'
			? source->message : "native compiler operation failed",
		source->has_source_opline, source->source_opline);
}

static zend_native_compile_fault native_mir_test_product_fault(
	native_mir_test_fault fault)
{
	switch (fault) {
		case NATIVE_MIR_TEST_FAULT_SSA_FAILURE:
			return ZEND_NATIVE_COMPILE_FAULT_SSA;
		case NATIVE_MIR_TEST_FAULT_MODULE_OOM:
			return ZEND_NATIVE_COMPILE_FAULT_MODULE_ALLOCATION;
		case NATIVE_MIR_TEST_FAULT_FINALIZE_FAILURE:
			return ZEND_NATIVE_COMPILE_FAULT_MODULE_FINALIZE;
		case NATIVE_MIR_TEST_FAULT_STAGE1_VERIFIER_FAILURE:
			return ZEND_NATIVE_COMPILE_FAULT_STAGE1_VERIFY;
		case NATIVE_MIR_TEST_FAULT_STAGE2_VERIFIER_FAILURE:
			return ZEND_NATIVE_COMPILE_FAULT_STAGE2_VERIFY;
		case NATIVE_MIR_TEST_FAULT_MAPPING_FAILURE:
			return ZEND_NATIVE_COMPILE_FAULT_MAPPING;
		case NATIVE_MIR_TEST_FAULT_ENTRY_PUBLISH_FAILURE:
			return ZEND_NATIVE_COMPILE_FAULT_ENTRY_PUBLISH;
		default:
			return ZEND_NATIVE_COMPILE_FAULT_NONE;
	}
}

static bool native_mir_test_calibrate_vm_probes(
	native_mir_test_state *state, HashTable *arguments)
{
	zend_execute_data *previous;
	zend_execute_data *frame;
	zend_vm_stack previous_stack;
	zval *previous_stack_top;
	zval result;
	uint32_t argument_count;
	uint32_t index;
	bool bailed_out = false;

	if (!state->vm_probe_calibration_enabled) {
		return true;
	}
	if (state->vm_handler_calls != 0 || state->execute_ex_calls != 0
			|| state->opline_handler_calls != 0) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_EXECUTE, "bridge", "VM_PROBE",
			"native execution entered the VM before probe calibration");
		return false;
	}
	if (state->selected->scope != NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_EXECUTE, "bridge", "VM_PROBE",
			"VM probe calibration requires a free function");
		return false;
	}

	argument_count = arguments != NULL
		? zend_hash_num_elements(arguments) : 0;
	previous = EG(current_execute_data);
	previous_stack = EG(vm_stack);
	previous_stack_top = EG(vm_stack_top);
	frame = zend_vm_stack_push_call_frame(
		ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC,
		(zend_function *) state->selected,
		argument_count, NULL);
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		if (UNEXPECTED(argument == NULL)) {
			uint32_t copied_index;

			for (copied_index = 0; copied_index < index; copied_index++) {
				zval_ptr_dtor(ZEND_CALL_ARG(frame, copied_index + 1));
			}
			zend_vm_stack_free_call_frame(frame);
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				NATIVE_MIR_TEST_PHASE_EXECUTE, "bridge", "INVALID_ARGUMENTS",
				"arguments must be a dense zero-based numeric array");
			return false;
		}
		ZVAL_COPY(ZEND_CALL_ARG(frame, index + 1), argument);
	}
	ZVAL_UNDEF(&result);
	zend_init_func_execute_data(frame, state->selected, &result);
	zend_try {
		ZEND_OBSERVER_FCALL_BEGIN(frame);
		execute_ex(frame);
	} zend_catch {
		bailed_out = true;
	} zend_end_try();
	EG(current_execute_data) = previous;
	if (UNEXPECTED(bailed_out)) {
		zend_native_execution_cleanup_frame(frame);
		while (UNEXPECTED(EG(vm_stack) != previous_stack)) {
			zend_vm_stack page = EG(vm_stack);

			EG(vm_stack) = page->prev;
			efree(page);
		}
		EG(vm_stack_top) = previous_stack_top;
		EG(vm_stack_end) = previous_stack->end;
	} else {
		zend_vm_stack_free_call_frame(frame);
	}
	if (!Z_ISUNDEF(result)) {
		zval_ptr_dtor(&result);
	}
	if (UNEXPECTED(bailed_out)) {
		zend_bailout();
	}
	if (EG(exception) != NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_EXECUTE, "bridge", "VM_PROBE",
			"VM probe calibration raised an exception");
		return false;
	}
	if (state->vm_handler_calls == 0 || state->execute_ex_calls == 0
			|| state->opline_handler_calls == 0) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_EXECUTE, "bridge", "VM_PROBE",
			"VM probe calibration did not reach every VM probe");
		return false;
	}
	return true;
}

static bool native_mir_test_execute_product(
	native_mir_test_state *state, HashTable *arguments)
{
	zend_native_compiler_config config;
	zend_native_compile_diagnostic compile_diagnostic;
	zend_native_diagnostic diagnostic;
	zend_native_entry_cell *entry_cell;
	uint32_t index;

	if (!state->script_initialized) {
		native_mir_test_init_script(state);
	}
	if (state->product_compiler == NULL) {
		memset(&config, 0, sizeof(config));
		config.script = &state->script;
		config.target = state->target;
		config.mir_chunk_size = state->mir_chunk_size;
		config.frame_probe = state->stack_probe_enabled
			? native_mir_test_frame_probe_record : NULL;
		config.frame_probe_context = state;
		config.observer = native_mir_test_product_observer;
		config.observer_context = state;
		config.fault = native_mir_test_product_fault(state->fault);
		memset(&compile_diagnostic, 0, sizeof(compile_diagnostic));
		state->product_compiler = zend_native_compiler_create(
			&config, &compile_diagnostic);
		if (state->product_compiler == NULL) {
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				native_mir_test_product_phase(compile_diagnostic.phase),
				"native", "NATIVE0003",
				compile_diagnostic.message[0] != '\0'
					? compile_diagnostic.message
					: "native compiler creation failed");
			return false;
		}
	}
	state->phase = NATIVE_MIR_TEST_PHASE_EXECUTE;
	for (index = 0; index < state->execute_repetitions; index++) {
		zend_native_status status;

		memset(&diagnostic, 0, sizeof(diagnostic));
		status = zend_native_compiler_execute(
			state->product_compiler,
			(zend_function *) state->selected,
			arguments, &state->native_result, &diagnostic);
		if (status != ZEND_NATIVE_RETURNED) {
			state->native_exception = status == ZEND_NATIVE_EXCEPTION;
			state->native_bailout = status == ZEND_NATIVE_BAILOUT;
			if (state->diagnostic_count == 0) {
				native_mir_test_backend_failure(
					state, NATIVE_MIR_TEST_PHASE_EXECUTE, &diagnostic);
			} else {
				state->status = NATIVE_MIR_TEST_STATUS_ERROR;
				state->phase = NATIVE_MIR_TEST_PHASE_EXECUTE;
			}
			if (!Z_ISUNDEF(state->native_result)) {
				zval_ptr_dtor(&state->native_result);
				ZVAL_UNDEF(&state->native_result);
			}
			return false;
		}
		state->completed_executions++;
		if (index + 1 < state->execute_repetitions
				&& !Z_ISUNDEF(state->native_result)) {
			zval_ptr_dtor(&state->native_result);
			ZVAL_UNDEF(&state->native_result);
		}
	}
	entry_cell = zend_native_compiler_lookup(
		state->product_compiler, (zend_function *) state->selected);
	if (entry_cell == NULL || entry_cell->code == NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_PUBLISH, "native", "NATIVE0003",
			"native product compiler did not publish the selected entry");
		return false;
	}
	state->native_code = (zend_native_code *) entry_cell->code;
	state->native_image = (zend_native_image *)
		zend_native_compiler_image_for(
			state->product_compiler, (zend_function *) state->selected);
	state->native_writable_after_publish =
		!zend_native_compiler_all_code_is_wx(state->product_compiler);
	state->native_executable_after_publish =
		zend_native_compiler_all_code_is_wx(state->product_compiler);
	state->native_result_valid = true;
	state->status = NATIVE_MIR_TEST_STATUS_ACCEPTED;
	state->phase = NATIVE_MIR_TEST_PHASE_COMPLETE;
	return true;
}

static bool native_mir_test_execute_module_inner(
	native_mir_test_state *state, HashTable *arguments)
{
	return native_mir_test_execute_product(state, arguments)
		&& native_mir_test_calibrate_vm_probes(state, arguments);
}

static bool native_mir_test_execute_module(
	native_mir_test_state *state, HashTable *arguments)
{
	bool result;
	uint32_t previous_depth = state->vm_probe_depth;

	if (UNEXPECTED(previous_depth == UINT32_MAX)) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_EXECUTE, "bridge", "VM_PROBE",
			"VM probe nesting depth overflow");
		return false;
	}
	state->vm_probe_depth = previous_depth + 1;
	result = native_mir_test_execute_module_inner(state, arguments);
	state->vm_probe_depth = previous_depth;
	return result;
}

void zend_native_call_resolution_cache_invalidate(void);

ZEND_FUNCTION(native_mir_test_call_cache_invalidate)
{
	ZEND_PARSE_PARAMETERS_NONE();
	zend_native_call_resolution_cache_invalidate();
}

ZEND_FUNCTION(native_mir_test_unwind_probe)
{
	void *frames[64];
	int frame_count;
	int frame_index;
	uint32_t function_index;
	zend_long native_frame_count = 0;
	native_mir_test_state *state = native_mir_test_active_state;

	ZEND_PARSE_PARAMETERS_NONE();

	if (state == NULL) {
		RETURN_LONG(0);
	}
	frame_count = backtrace(frames, (int) (sizeof(frames) / sizeof(frames[0])));
	for (frame_index = 0; frame_index < frame_count; frame_index++) {
		if (state->product_compiler != NULL) {
			for (function_index = 0;
					function_index < zend_native_compiler_function_count(
						state->product_compiler);
					function_index++) {
				if (zend_native_code_contains_address(
						zend_native_compiler_code_at(
							state->product_compiler, function_index),
						frames[frame_index])) {
					native_frame_count++;
					break;
				}
			}
		}
	}
	RETURN_LONG(native_frame_count);
}

static void native_mir_test_cleanup_static_variables(zend_op_array *op_array)
{
	if (op_array != NULL && op_array->type == ZEND_USER_FUNCTION
			&& ZEND_MAP_PTR(op_array->static_variables_ptr)) {
		HashTable *table = ZEND_MAP_PTR_GET(op_array->static_variables_ptr);

		if (table != NULL) {
			zend_array_destroy(table);
			ZEND_MAP_PTR_SET(op_array->static_variables_ptr, NULL);
		}
	}
}

static void native_mir_test_cleanup_class_request_data(zend_class_entry *ce)
{
	if (ce->default_static_members_count) {
		zend_cleanup_internal_class_data(ce);
	}
	if (ZEND_MAP_PTR(ce->mutable_data)) {
		if (ZEND_MAP_PTR_GET_IMM(ce->mutable_data)) {
			zend_cleanup_mutable_class_data(ce);
		}
	} else if (ce->type == ZEND_USER_CLASS
			&& (ce->ce_flags & ZEND_ACC_IMMUTABLE) == 0) {
		zend_class_constant *constant;
		zval *property = ce->default_properties_table;
		zval *property_end = property + ce->default_properties_count;

		ZEND_HASH_MAP_FOREACH_PTR(&ce->constants_table, constant) {
			if (constant->ce == ce) {
				zval_ptr_dtor_nogc(&constant->value);
				ZVAL_UNDEF(&constant->value);
			}
		} ZEND_HASH_FOREACH_END();
		while (property != property_end) {
			i_zval_ptr_dtor(property);
			ZVAL_UNDEF(property);
			property++;
		}
	}
	if (ce->type == ZEND_USER_CLASS && ce->backed_enum_table != NULL) {
		zend_hash_release(ce->backed_enum_table);
		ce->backed_enum_table = NULL;
	}
	if ((ce->ce_flags & ZEND_HAS_STATIC_IN_METHODS) != 0) {
		zend_op_array *method;

		ZEND_HASH_MAP_FOREACH_PTR(&ce->function_table, method) {
			native_mir_test_cleanup_static_variables(method);
		} ZEND_HASH_FOREACH_END();
	}
	if (ce->num_hooked_props != 0) {
		zend_property_info *property_info;

		ZEND_HASH_MAP_FOREACH_PTR(&ce->properties_info, property_info) {
			uint32_t hook_index;

			if (property_info->ce != ce || property_info->hooks == NULL) {
				continue;
			}
			for (hook_index = 0; hook_index < ZEND_PROPERTY_HOOK_COUNT;
					hook_index++) {
				if (property_info->hooks[hook_index] != NULL) {
					native_mir_test_cleanup_static_variables(
						&property_info->hooks[hook_index]->op_array);
				}
			}
		} ZEND_HASH_FOREACH_END();
	}
}

static void native_mir_test_cleanup(native_mir_test_state *state)
{
	HashTable *function_table;
	HashTable *class_table;
	uint32_t index;

	native_mir_test_restore_user_opcode(state);
	if (state->compiler_options_saved) {
		CG(compiler_options) = state->original_compiler_options;
		state->compiler_options_saved = false;
	}
	if (state->native_result_valid) {
		zval_ptr_dtor(&state->native_result);
		state->native_result_valid = false;
	}
	if (state->product_compiler != NULL) {
		zend_native_compiler_destroy(state->product_compiler);
		state->product_compiler = NULL;
		state->native_code = NULL;
		state->native_image = NULL;
	}
	if (state->native_code != NULL) {
		zend_native_code_destroy(state->native_code);
		state->native_code = NULL;
	}
	if (state->native_image != NULL) {
		zend_native_image_destroy(state->native_image);
		state->native_image = NULL;
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
		zend_function *function;

		ZEND_HASH_FOREACH_PTR(&state->script.function_table, function) {
			if (function != NULL && function->type == ZEND_USER_FUNCTION) {
				native_mir_test_cleanup_static_variables(
					&function->op_array);
			}
		} ZEND_HASH_FOREACH_END();
		native_mir_test_cleanup_static_variables(
			&state->script.main_op_array);
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
				native_mir_test_cleanup_static_variables(
					(zend_op_array *) Z_PTR(bucket->val));
				(void) zend_hash_del(function_table, bucket->key);
			}
		}
		state->function_table_snapshot = false;
	}
	class_table = CG(class_table);
	if (state->class_table_snapshot && class_table != NULL) {
		/* Request-owned class data is deliberately released before the class
		 * table itself. Mirror request shutdown for this isolated source unit.
		 * Alias buckets share the class entry and must not clean it twice. */
		index = class_table->nNumUsed;
		while (index > state->class_table_used_before) {
			Bucket *bucket = &class_table->arData[--index];

			if (Z_TYPE(bucket->val) != IS_UNDEF
					&& Z_TYPE(bucket->val) != IS_ALIAS_PTR) {
				native_mir_test_cleanup_class_request_data(
					Z_PTR(bucket->val));
			}
		}
		index = class_table->nNumUsed;
		while (index > state->class_table_used_before) {
			Bucket *bucket = &class_table->arData[--index];

			if (Z_TYPE(bucket->val) != IS_UNDEF && bucket->key != NULL) {
				(void) zend_hash_del(class_table, bucket->key);
			}
		}
		state->class_table_snapshot = false;
	}
	if (state->compiled != NULL) {
		for (index = 0;
				index < state->compiled->num_dynamic_func_defs; index++) {
			native_mir_test_cleanup_static_variables(
				state->compiled->dynamic_func_defs[index]);
		}
		native_mir_test_cleanup_static_variables(state->compiled);
		destroy_op_array(state->compiled);
		efree_size(state->compiled, sizeof(zend_op_array));
		state->compiled = NULL;
		state->selected = NULL;
	}
	for (index = state->detached_class_count; index-- > 0;) {
		zval class_value;
		zend_class_entry *class_entry = state->detached_classes[index];

		native_mir_test_cleanup_class_request_data(class_entry);
		ZVAL_PTR(&class_value, class_entry);
		destroy_zend_class(&class_value);
	}
	efree(state->detached_classes);
	state->detached_classes = NULL;
	state->detached_class_count = 0;
}

static void native_mir_test_build_result(
	native_mir_test_state *state, zval *return_value)
{
	zval source;
	zval diagnostics;
	zval source_opcodes;
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
	array_init(&source_opcodes);
	for (index = 0; index < state->source_opcode_count; index++) {
		const char *name = zend_get_opcode_name(state->source_opcodes[index]);

		add_next_index_string(
			&source_opcodes, name != NULL ? (char *) name : "UNKNOWN");
	}
	add_assoc_zval(return_value, "source_opcodes", &source_opcodes);
	if (state->status == NATIVE_MIR_TEST_STATUS_ACCEPTED
			&& state->dump.s != NULL) {
		add_assoc_str(
			return_value, "mir", zend_string_copy(state->dump.s));
	} else {
		add_assoc_null(return_value, "mir");
	}
	if (state->execute_mode) {
		zval execution;
		zend_native_compiler_stats compiler_stats;

		array_init(&execution);
		memset(&compiler_stats, 0, sizeof(compiler_stats));
		if (state->product_compiler != NULL) {
			zend_native_compiler_get_stats(
				state->product_compiler, &compiler_stats);
		}
		add_assoc_string(&execution, "target",
			(char *) zend_native_target_id(state->target));
		add_assoc_string(&execution, "target_triple",
			(char *) zend_native_target_triple(state->target));
		add_assoc_string(&execution, "status",
			state->native_bailout ? "bailout"
				: state->native_exception ? "exception"
					: state->native_result_valid ? "returned" : "not_executed");
		add_assoc_bool(&execution, "exception", state->native_exception);
		add_assoc_bool(&execution, "bailout", state->native_bailout);
		add_assoc_long(&execution, "vm_handler_calls",
			(zend_long) state->vm_handler_calls);
		add_assoc_long(&execution, "execute_ex_calls",
			(zend_long) state->execute_ex_calls);
		add_assoc_long(&execution, "opline_handler_calls",
			(zend_long) state->opline_handler_calls);
		add_assoc_long(&execution, "user_opcode_calls",
			(zend_long) state->user_opcode_calls);
		add_assoc_long(&execution, "generator_reentry_gateway_calls",
			(zend_long) state->generator_reentry_gateway_calls);
		add_assoc_long(&execution, "executions",
			(zend_long) state->completed_executions);
		add_assoc_long(&execution, "native_codeunits",
			state->product_compiler != NULL
				? (zend_long) zend_native_compiler_native_codeunit_count(
					state->product_compiler)
				: 0);
		add_assoc_long(&execution, "native_components",
			state->product_compiler != NULL
				? (zend_long)
					zend_native_compiler_published_component_count(
						state->product_compiler)
				: 0);
		add_assoc_long(&execution, "suspendable_reserved",
			state->product_compiler != NULL
				? (zend_long) zend_native_compiler_codeunit_count(
					state->product_compiler,
					ZEND_NATIVE_CODEUNIT_SUSPENDABLE_RESERVED)
				: 0);
		add_assoc_long(&execution, "failed_codeunits",
			state->product_compiler != NULL
				? (zend_long) zend_native_compiler_codeunit_count(
					state->product_compiler,
					ZEND_NATIVE_CODEUNIT_FAILED)
				: 0);
		add_assoc_long(&execution, "unwind_registrations_before",
			0);
		{
			const uint32_t unwind_registrations_live =
				zend_native_live_unwind_registration_count();

			/*
			 * The invoking script may itself be native and therefore own an
			 * unrelated live unwind registration. Report registrations owned
			 * by this compile/execute invocation so lifecycle checks remain
			 * stable across an interpreted or native caller.
			 */
			add_assoc_long(&execution, "unwind_registrations_live",
				unwind_registrations_live
						>= state->unwind_registrations_before
					? (zend_long) (unwind_registrations_live
						- state->unwind_registrations_before)
					: 0);
		}
		{
			uint32_t active_calls = state->product_compiler != NULL
				? zend_native_compiler_active_call_count(
					state->product_compiler)
				: 0;

			add_assoc_long(&execution, "entry_active_calls",
				(zend_long) active_calls);
		}
		add_assoc_bool(&execution, "writable_after_publish",
			state->native_writable_after_publish);
		add_assoc_bool(&execution, "executable_after_publish",
			state->native_executable_after_publish);
		add_assoc_bool(&execution, "unwind_registered",
			zend_native_code_has_unwind_info(state->native_code));
		add_assoc_long(&execution, "image_size",
			(zend_long) zend_native_image_size(state->native_image));
		if (state->product_compiler != NULL) {
			zval performance;

			array_init(&performance);
			add_assoc_long(&performance, "compile_ns",
				(zend_long) compiler_stats.compile_ns);
			add_assoc_long(&performance, "ssa_ns",
				(zend_long) compiler_stats.ssa_ns);
			add_assoc_long(&performance, "lowering_ns",
				(zend_long) compiler_stats.lowering_ns);
			add_assoc_long(&performance, "codegen_ns",
				(zend_long) compiler_stats.codegen_ns);
			add_assoc_long(&performance, "publish_ns",
				(zend_long) compiler_stats.publish_ns);
			add_assoc_long(&performance, "execute_ns",
				(zend_long) compiler_stats.execute_ns);
			add_assoc_long(&performance, "first_execute_ns",
				(zend_long) compiler_stats.first_execute_ns);
			add_assoc_long(&performance, "last_execute_ns",
				(zend_long) compiler_stats.last_execute_ns);
			add_assoc_long(&performance, "native_code_bytes",
				(zend_long) compiler_stats.native_code_bytes);
			add_assoc_long(&performance, "registered_codeunits",
				(zend_long) compiler_stats.registered_codeunits);
			add_assoc_long(&performance, "compiled_codeunits",
				(zend_long) compiler_stats.native_codeunits);
			add_assoc_long(&performance, "ready_codeunits",
				(zend_long) compiler_stats.ready_codeunits);
			add_assoc_long(&performance, "published_components",
				(zend_long) compiler_stats.published_components);
			add_assoc_long(&performance, "runtime_helper_sites",
				(zend_long) compiler_stats.runtime_helper_sites);
			add_assoc_long(&performance, "source_opline_decode_sites",
				(zend_long) compiler_stats.source_opline_decode_sites);
			add_assoc_long(&performance, "guard_sites",
				(zend_long) compiler_stats.guard_sites);
			add_assoc_long(&performance, "slow_path_sites",
				(zend_long) compiler_stats.slow_path_sites);
			add_assoc_long(&performance, "direct_call_sites",
				(zend_long) compiler_stats.direct_call_sites);
			add_assoc_long(&performance, "direct_leaf_scalar_sites",
				(zend_long) compiler_stats.direct_leaf_scalar_sites);
			add_assoc_long(&performance, "direct_typed_body_sites",
				(zend_long) compiler_stats.direct_typed_body_sites);
			add_assoc_long(&performance, "direct_call_frame_bytes",
				(zend_long) compiler_stats.direct_call_frame_bytes);
			add_assoc_long(&performance, "inner_call_runtime_helper_calls",
				(zend_long) compiler_stats.inner_call_runtime_helper_calls);
			add_assoc_long(&performance, "inner_call_heap_allocations",
				(zend_long) compiler_stats.inner_call_heap_allocations);
			add_assoc_long(&performance, "inner_call_catcher_boundaries",
				(zend_long) compiler_stats.inner_call_catcher_boundaries);
			add_assoc_long(&performance, "executions",
				(zend_long) compiler_stats.executions);
			add_assoc_zval(&execution, "performance", &performance);
		}
		if (state->native_image != NULL) {
			static const char hex[] = "0123456789abcdef";
			const unsigned char *bytes =
				zend_native_image_bytes(state->native_image);
			size_t byte_count = zend_native_image_size(state->native_image);

			ZEND_ASSERT(byte_count <= ZSTR_MAX_LEN / 2);
			zend_string *encoded = zend_string_alloc(byte_count * 2, false);
			size_t byte_index;

			for (byte_index = 0; byte_index < byte_count; ++byte_index) {
				ZSTR_VAL(encoded)[byte_index * 2] = hex[bytes[byte_index] >> 4];
				ZSTR_VAL(encoded)[byte_index * 2 + 1] =
					hex[bytes[byte_index] & 0x0f];
			}
			ZSTR_VAL(encoded)[byte_count * 2] = '\0';
			add_assoc_str(&execution, "machine_code", encoded);
		} else {
			add_assoc_null(&execution, "machine_code");
		}
		if (state->native_result_valid) {
			zval value;

			ZVAL_COPY_VALUE(&value, &state->native_result);
			ZVAL_UNDEF(&state->native_result);
			state->native_result_valid = false;
			add_assoc_zval(&execution, "return_value", &value);
		} else {
			add_assoc_null(&execution, "return_value");
		}
		if (state->stack_probe_enabled) {
			zval trace;

			array_init(&trace);
			for (index = 0; index < state->frame_probe_count; index++) {
				const native_mir_test_frame_probe *record =
					&state->frame_probes[index];
				zval frame;
				zval argument_types;
				uint32_t argument_index;

				array_init(&frame);
				if (record->caller_name != NULL) {
					add_assoc_str(&frame, "caller",
						zend_string_copy((zend_string *) record->caller_name));
				} else {
					add_assoc_string(&frame, "caller", "{main}");
				}
				if (record->callee_name != NULL) {
					add_assoc_str(&frame, "callee",
						zend_string_copy((zend_string *) record->callee_name));
				} else {
					add_assoc_string(&frame, "callee", "{main}");
				}
				add_assoc_long(&frame, "caller_line", record->caller_line);
				add_assoc_long(&frame, "callee_line", record->callee_line);
				add_assoc_long(&frame, "argument_count", record->argument_count);
				array_init(&argument_types);
				for (argument_index = 0;
						argument_index < record->argument_count
						&& argument_index < NATIVE_MIR_TEST_MAX_PROBE_ARGUMENTS;
						argument_index++) {
					add_next_index_string(
						&argument_types,
						zend_get_type_by_const(record->argument_types[argument_index]));
				}
				add_assoc_zval(&frame, "argument_types", &argument_types);
				add_assoc_bool(&frame, "previous_matches_caller",
					record->previous_matches_caller);
				add_next_index_zval(&trace, &frame);
			}
			add_assoc_bool(&execution, "frame_chain_valid",
				state->frame_chain_valid);
			add_assoc_zval(&execution, "stack_trace", &trace);
		}
		add_assoc_zval(return_value, "execution", &execution);
	}
}

static zend_native_target native_mir_test_default_target(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
	return ZEND_NATIVE_TARGET_DARWIN_ARM64;
#elif defined(__linux__) && defined(__x86_64__)
	return ZEND_NATIVE_TARGET_LINUX_AMD64;
#else
# error "native_mir_test supports only Darwin arm64 and Linux x86-64"
#endif
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
	state->target = native_mir_test_default_target();
	state->mir_chunk_size = ZEND_MIR_CORE_DEFAULT_CHUNK_SIZE;
	state->phase = NATIVE_MIR_TEST_PHASE_COMPILE;
	state->status = NATIVE_MIR_TEST_STATUS_ERROR;
	state->diagnostics = ecalloc(
		NATIVE_MIR_TEST_MAX_DIAGNOSTIC_LIMIT,
		sizeof(state->diagnostics[0]));
	if (options != NULL && !native_mir_test_parse_options(state, options)) {
		native_mir_test_build_result(state, return_value);
		efree(state->diagnostics);
		efree(state->frame_probes);
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
	efree(state->source_opcodes);
	efree(state->diagnostics);
	efree(state);
}

ZEND_FUNCTION(native_mir_test_compile_execute)
{
	zend_string *source;
	zend_string *filename;
	HashTable *arguments = NULL;
	HashTable *options = NULL;
	native_mir_test_state *state;
	native_mir_test_state *previous_active_state;
	bool bailed_out = false;

	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(source)
		Z_PARAM_PATH_STR(filename)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY_HT(arguments)
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
	state->execute_mode = true;
	state->execute_repetitions = 1;
	state->frame_chain_valid = true;
	state->unwind_registrations_before =
		zend_native_live_unwind_registration_count();
	state->target = native_mir_test_default_target();
	state->mir_chunk_size = ZEND_MIR_CORE_DEFAULT_CHUNK_SIZE;
	state->phase = NATIVE_MIR_TEST_PHASE_COMPILE;
	state->status = NATIVE_MIR_TEST_STATUS_ERROR;
	state->diagnostics = ecalloc(
		NATIVE_MIR_TEST_MAX_DIAGNOSTIC_LIMIT,
		sizeof(state->diagnostics[0]));
	if (options != NULL && !native_mir_test_parse_options(state, options)) {
		native_mir_test_build_result(state, return_value);
		efree(state->diagnostics);
		efree(state->frame_probes);
		efree(state);
		return;
	}
	if (!native_mir_test_validate_arguments(state, arguments)) {
		native_mir_test_build_result(state, return_value);
		efree(state->diagnostics);
		efree(state->frame_probes);
		efree(state);
		return;
	}
	if (state->stack_probe_enabled) {
		state->frame_probes = ecalloc(
			NATIVE_MIR_TEST_MAX_FRAME_PROBES, sizeof(*state->frame_probes));
	}
	state->module_host.fail_enabled =
		state->fault == NATIVE_MIR_TEST_FAULT_MODULE_OOM;
	state->module_host.fail_after = 0;

	/* Arm the VM probes only while the native module is executing. */
	previous_active_state = native_mir_test_active_state;
	state->vm_probe_parent = previous_active_state;
	native_mir_test_active_state = state;
	zend_try {
		if (native_mir_test_compile(state)) {
			bool source_ready;

			if (!native_mir_test_install_user_opcode(state)) {
				native_mir_test_fail(
					state, NATIVE_MIR_TEST_STATUS_ERROR,
					NATIVE_MIR_TEST_PHASE_COMPILE, "bridge",
					"USER_OPCODE_INSTALL",
					"failed to install requested user opcode handler");
				source_ready = false;
			} else {
				native_mir_test_init_script(state);
				native_mir_test_capture_source_opcodes(state);
				source_ready = true;
			}
			if (source_ready) {
				(void) native_mir_test_execute_module(state, arguments);
			}
		}
	} zend_catch {
		bailed_out = true;
	} zend_end_try();
	state->vm_probe_depth = 0;
	native_mir_test_restore_user_opcode(state);
	native_mir_test_active_state = previous_active_state;
	state->vm_probe_parent = NULL;

	if (bailed_out) {
		state->native_bailout = true;
		if (EG(exception) != NULL) {
			state->native_exception = true;
			zend_clear_exception();
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR, state->phase,
			"native", "BAILOUT", "native execution path bailed out");
	}
	native_mir_test_build_result(state, return_value);
	if (state->product_compiler != NULL) {
		/*
		 * Compilers are request owners. Dynamic declarations, closures,
		 * include versions and their native entries may outlive this outer
		 * call, so the complete owner stays alive until the executor and
		 * compiler have released every request value.
		 */
		native_mir_test_detach_compiled_symbols(state);
		native_mir_test_retain_state(state);
	} else {
		native_mir_test_release_state(state);
	}
}

PHP_MINFO_FUNCTION(native_mir_test)
{
	(void) zend_module;
	php_info_print_table_start();
	php_info_print_table_row(
		2, "native_mir_test support", "enabled (test-only)");
	php_info_print_table_end();
}

PHP_MINIT_FUNCTION(native_mir_test)
{
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(native_mir_test)
{
	return SUCCESS;
}

PHP_RINIT_FUNCTION(native_mir_test)
{
#if defined(ZTS) && defined(COMPILE_DL_NATIVE_MIR_TEST)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	native_mir_test_active_state = NULL;
	native_mir_test_retained_states = NULL;
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(native_mir_test)
{
	native_mir_test_state *state = native_mir_test_retained_states;

	while (state != NULL) {
		if (state->native_result_valid) {
			zval_ptr_dtor(&state->native_result);
			state->native_result_valid = false;
		}
		state = state->retained_next;
	}
	native_mir_test_active_state = NULL;
	return SUCCESS;
}

ZEND_MODULE_POST_ZEND_DEACTIVATE_D(native_mir_test)
{
	native_mir_test_state *state = native_mir_test_retained_states;

	native_mir_test_retained_states = NULL;
	while (state != NULL) {
		native_mir_test_state *next = state->retained_next;

		/*
		 * Zend has already destroyed the request function and class tables.
		 * The retained source owner now releases only its own roots and
		 * generated code; it must not revisit those tables.
		 */
		state->function_table_snapshot = false;
		state->class_table_snapshot = false;
		native_mir_test_release_state(state);
		state = next;
	}
	return SUCCESS;
}

zend_module_entry native_mir_test_module_entry = {
	STANDARD_MODULE_HEADER,
	"native_mir_test",
	ext_functions,
	PHP_MINIT(native_mir_test),
	PHP_MSHUTDOWN(native_mir_test),
	PHP_RINIT(native_mir_test),
	PHP_RSHUTDOWN(native_mir_test),
	PHP_MINFO(native_mir_test),
	PHP_NATIVE_MIR_TEST_VERSION,
	NO_MODULE_GLOBALS,
	ZEND_MODULE_POST_ZEND_DEACTIVATE_N(native_mir_test),
	STANDARD_MODULE_PROPERTIES_EX
};
