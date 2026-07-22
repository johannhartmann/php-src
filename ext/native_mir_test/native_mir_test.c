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
#include "Zend/zend_execute.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_smart_str.h"
#include "Zend/zend_vm_opcodes.h"
#include "Zend/Optimizer/zend_func_info.h"
#include "Zend/Optimizer/zend_optimizer.h"
#include "Zend/Optimizer/zend_optimizer_internal.h"

#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/Core/zend_mir_module_internal.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/MIR/zend_mir.h"
#include "Zend/Native/Calls/Model/zend_mir_call_model.h"
#include "Zend/Native/Values/Lowering/zend_mir_value_lowering.h"
#include "Zend/Native/Lowering/Core/zend_mir_lowering_internal.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"
#include "Zend/Native/Runtime/Common/zend_native_calls.h"
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
	NATIVE_MIR_TEST_FAULT_PLANNER_ALLOCATION,
	NATIVE_MIR_TEST_FAULT_TARGET_SNAPSHOT,
	NATIVE_MIR_TEST_FAULT_ARGUMENT_TABLE,
	NATIVE_MIR_TEST_FAULT_FRAME_STATE,
	NATIVE_MIR_TEST_FAULT_CALL_RECORD,
	NATIVE_MIR_TEST_FAULT_FINALIZE_FAILURE,
	NATIVE_MIR_TEST_FAULT_STAGE1_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_STAGE2_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_STRUCTURAL_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_SCALAR_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_CONTROL_FLOW_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_CALL_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_FINGERPRINT_RECOMPUTE_FAILURE,
	NATIVE_MIR_TEST_FAULT_VALUE_INVENTORY,
	NATIVE_MIR_TEST_FAULT_VALUE_PLAN,
	NATIVE_MIR_TEST_FAULT_VALUE_STORAGE,
	NATIVE_MIR_TEST_FAULT_VALUE_REFERENCE,
	NATIVE_MIR_TEST_FAULT_VALUE_ALIAS,
	NATIVE_MIR_TEST_FAULT_VALUE_EVENT,
	NATIVE_MIR_TEST_FAULT_VALUE_SEPARATION,
	NATIVE_MIR_TEST_FAULT_VALUE_CALL_TRANSFER,
	NATIVE_MIR_TEST_FAULT_VALUE_VERIFIER_FAILURE,
	NATIVE_MIR_TEST_FAULT_DUMP_FAILURE,
	NATIVE_MIR_TEST_FAULT_MAPPING_FAILURE
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

typedef struct _native_mir_test_native_function {
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
	native_mir_test_module_host module_host;
	native_mir_test_module_host *module_host_ref;
	zend_mir_module *module;
	zend_native_image *image;
	zend_native_code *code;
	zend_native_entry_cell entry_cell;
	zend_native_internal_call_cell *internal_call_cells;
	uint32_t internal_call_cell_count;
} native_mir_test_native_function;

typedef struct _native_mir_test_state {
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
	bool function_table_snapshot;
	native_mir_test_phase phase;
	native_mir_test_status status;
	native_mir_test_fault fault;
	native_mir_test_diagnostic *diagnostics;
	uint32_t diagnostic_count;
	uint32_t diagnostic_limit;
	uint32_t wave;
	bool execute_mode;
	zend_native_target target;
	size_t mir_chunk_size;
	const char *diagnostic_stage;
	native_mir_test_module_host module_host;
	native_mir_test_module_host *active_module_host;
	zend_mir_module *module;
	zend_native_image *native_image;
	zend_native_code *native_code;
	native_mir_test_native_function *native_functions;
	uint32_t native_function_count;
	uint32_t native_function_capacity;
	zval native_result;
	bool native_result_valid;
	bool native_writable_after_publish;
	bool native_executable_after_publish;
	bool native_exception;
	bool native_bailout;
	uint64_t vm_handler_calls;
	uint64_t execute_ex_calls;
	uint64_t opline_handler_calls;
	bool stack_probe_enabled;
	bool frame_chain_valid;
	native_mir_test_frame_probe *frame_probes;
	uint32_t frame_probe_count;
	uint32_t execute_repetitions;
	uint32_t completed_executions;
	smart_str dump;
	uint32_t dump_writes;
} native_mir_test_state;

static void native_mir_test_frame_probe_record(
	void *context,
	const zend_execute_data *caller,
	const zend_execute_data *callee)
{
	native_mir_test_state *state = context;
	native_mir_test_frame_probe *record;

	if (state == NULL || caller == NULL || callee == NULL
			|| state->frame_probe_count >= NATIVE_MIR_TEST_MAX_FRAME_PROBES) {
		if (state != NULL) {
			state->frame_chain_valid = false;
		}
		return;
	}
	record = &state->frame_probes[state->frame_probe_count++];
	record->caller_name = caller->func != NULL
		? caller->func->common.function_name : NULL;
	record->callee_name = callee->func != NULL
		? callee->func->common.function_name : NULL;
	record->caller_line = caller->opline != NULL ? caller->opline->lineno : 0;
	record->callee_line = callee->opline != NULL ? callee->opline->lineno : 0;
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

/*
 * Process-local W04 integration wrapper. Standalone bridge tests provide a
 * failure-atomic link stub; production builds use the real implementation.
 */
extern zend_mir_lowering_result zend_mir_lower_w04_zend_op_array(
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

extern zend_mir_w05_lowering_result zend_mir_lower_w05_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

extern zend_mir_w05_lowering_result zend_mir_lower_w07_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

extern zend_mir_w08_lowering_result zend_mir_lower_w08_zend_op_array(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics);

extern zend_mir_lowering_status zend_mir_frontend_project_w05_result_facts(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic);

extern zend_mir_lowering_status zend_mir_frontend_project_w08_result_facts(
	const zend_script *script,
	const zend_op_array *op_array,
	const zend_ssa *ssa,
	zend_ssa *projected_ssa,
	zend_mir_frontend_diagnostic *diagnostic);

extern zend_mir_w06_lowering_result zend_mir_lower_w06_zend_op_array(
	const zend_script *script,
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
	} else if (zend_string_equals_literal(value, "planner_allocation")) {
		*out = NATIVE_MIR_TEST_FAULT_PLANNER_ALLOCATION;
	} else if (zend_string_equals_literal(value, "target_snapshot")) {
		*out = NATIVE_MIR_TEST_FAULT_TARGET_SNAPSHOT;
	} else if (zend_string_equals_literal(value, "argument_table")) {
		*out = NATIVE_MIR_TEST_FAULT_ARGUMENT_TABLE;
	} else if (zend_string_equals_literal(value, "frame_state")) {
		*out = NATIVE_MIR_TEST_FAULT_FRAME_STATE;
	} else if (zend_string_equals_literal(value, "call_record")) {
		*out = NATIVE_MIR_TEST_FAULT_CALL_RECORD;
	} else if (zend_string_equals_literal(value, "finalize_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_FINALIZE_FAILURE;
	} else if (zend_string_equals_literal(value, "stage1_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_STAGE1_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(value, "stage2_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_STAGE2_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(
			value, "structural_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_STRUCTURAL_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(
			value, "scalar_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_SCALAR_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(
			value, "control_flow_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_CONTROL_FLOW_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(value, "call_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_CALL_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(
			value, "fingerprint_recompute_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_FINGERPRINT_RECOMPUTE_FAILURE;
	} else if (zend_string_equals_literal(value, "value_inventory")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_INVENTORY;
	} else if (zend_string_equals_literal(value, "value_plan")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_PLAN;
	} else if (zend_string_equals_literal(value, "value_storage")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_STORAGE;
	} else if (zend_string_equals_literal(value, "value_reference")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_REFERENCE;
	} else if (zend_string_equals_literal(value, "value_alias")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_ALIAS;
	} else if (zend_string_equals_literal(value, "value_event")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_EVENT;
	} else if (zend_string_equals_literal(value, "value_separation")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_SEPARATION;
	} else if (zend_string_equals_literal(value, "value_call_transfer")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_CALL_TRANSFER;
	} else if (zend_string_equals_literal(
			value, "value_verifier_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_VALUE_VERIFIER_FAILURE;
	} else if (zend_string_equals_literal(value, "dump_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_DUMP_FAILURE;
	} else if (zend_string_equals_literal(value, "mapping_failure")) {
		*out = NATIVE_MIR_TEST_FAULT_MAPPING_FAILURE;
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
						|| (Z_LVAL_P(value) != 3 && Z_LVAL_P(value) != 4
							&& Z_LVAL_P(value) != 5
							&& Z_LVAL_P(value) != 6
							&& Z_LVAL_P(value) != 7
							&& Z_LVAL_P(value) != 8)) {
				goto invalid_value;
			}
			state->wave = (uint32_t) Z_LVAL_P(value);
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
	HashTable *function_table;
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
	if (state->wave >= 4 && state->wave < 8
			&& state->selected->last_try_catch != 0) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0015",
			"W04 rejects protected source regions before SSA analysis");
		return false;
	}
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
	zend_optimize_script(
		&state->script, NATIVE_MIR_TEST_OPTIMIZATION_LEVEL, 0);
	CG(compiler_options) = state->original_compiler_options;
	state->compiler_options_saved = false;
	*state->compiled = state->script.main_op_array;
	if (state->selected->last != 0) {
		uint32_t index;

		state->source_opcodes =
			emalloc(state->selected->last * sizeof(*state->source_opcodes));
		state->source_opcode_count = state->selected->last;
		for (index = 0; index < state->source_opcode_count; index++) {
			state->source_opcodes[index] =
				state->selected->opcodes[index].opcode;
		}
	}
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

static bool native_mir_test_verify_w04_scalar(
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
			"W04 scalar verifier view is incomplete", false, 0);
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
				"W04 scalar instruction callback failed", false, 0);
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
				"W04 scalar instruction violates its descriptor",
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
					"W04 scalar operand lacks its exact proof",
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
					"W04 scalar result lacks its exact proof",
					false, 0);
				return false;
			}
		} else if (zend_mir_id_is_valid(instruction.result_id)
				|| instruction.representation
					!= ZEND_MIR_REPRESENTATION_VOID) {
			native_mir_test_add_diagnostic(
				state, "MIRV", "MIRV0622",
				"W04 scalar drop defines an unexpected result",
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
	return state->wave >= 4
		? native_mir_test_verify_w04_scalar(state, view)
		: zend_mir_verify_w03_scalar(view, diagnostics);
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
	zend_mir_lowering_result result,
	uint32_t wave)
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
	if (!(wave >= 5
			? ((result.status == ZEND_MIR_LOWERING_SUCCESS
					&& result.diagnostic_code == ZEND_MIRL_OK
					&& result.guarantees
						== ZEND_MIR_LOWERING_GUARANTEE_FINALIZED
					&& result.module != NULL)
				|| (result.status != ZEND_MIR_LOWERING_STATUS_INVALID
					&& result.status != ZEND_MIR_LOWERING_SUCCESS
					&& result.diagnostic_code != ZEND_MIRL_OK
					&& result.guarantees == 0
					&& result.module == NULL))
			: (wave >= 4
				? zend_mir_lowering_result_is_w04_failure_atomic(&result)
				: zend_mir_lowering_result_is_failure_atomic(&result)))) {
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
		snprintf(message, sizeof(message), "W%02u lowering completed", wave);
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
	snprintf(message, sizeof(message), "W%02u lowering completed", wave);
	native_mir_test_add_diagnostic(
		state, "MIRL", "MIRL0000", message, false, 0);
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
	return native_mir_test_publish_lowering_result(state, result, 3);
}

static bool native_mir_test_lower_w04_and_dump(native_mir_test_state *state)
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
	result = zend_mir_lower_w04_zend_op_array(
		state->selected, &state->ssa, &module_ops, &diagnostics);
	return native_mir_test_publish_lowering_result(state, result, 4);
}

static bool native_mir_test_lower_w05_and_dump(native_mir_test_state *state)
{
	zend_mir_lowering_module_ops module_ops;
	zend_mir_diagnostic_sink diagnostics;
	zend_mir_w05_lowering_result result;
#ifdef ZEND_MIR_W05_TEST_FAULTS
	zend_mir_w05_test_fault call_fault = ZEND_MIR_W05_TEST_FAULT_NONE;
#endif

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
#ifdef ZEND_MIR_W05_TEST_FAULTS
	switch (state->fault) {
		case NATIVE_MIR_TEST_FAULT_PLANNER_ALLOCATION:
			call_fault = ZEND_MIR_W05_TEST_FAULT_PLANNER_ALLOCATION;
			break;
		case NATIVE_MIR_TEST_FAULT_TARGET_SNAPSHOT:
			call_fault = ZEND_MIR_W05_TEST_FAULT_TARGET_SNAPSHOT;
			break;
		case NATIVE_MIR_TEST_FAULT_ARGUMENT_TABLE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_ARGUMENT_TABLE;
			break;
		case NATIVE_MIR_TEST_FAULT_FRAME_STATE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_FRAME_STATE;
			break;
		case NATIVE_MIR_TEST_FAULT_CALL_RECORD:
			call_fault = ZEND_MIR_W05_TEST_FAULT_CALL_RECORD;
			break;
		case NATIVE_MIR_TEST_FAULT_CALL_VERIFIER_FAILURE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_CALL_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_STRUCTURAL_VERIFIER_FAILURE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_STRUCTURAL_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_SCALAR_VERIFIER_FAILURE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_SCALAR_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_CONTROL_FLOW_VERIFIER_FAILURE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_CONTROL_FLOW_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_FINGERPRINT_RECOMPUTE_FAILURE:
			call_fault = ZEND_MIR_W05_TEST_FAULT_FINGERPRINT_RECOMPUTE;
			break;
		default:
			break;
	}
	zend_mir_w05_test_set_fault(call_fault);
#endif
	result = state->wave >= 8
		? zend_mir_lower_w08_zend_op_array(
			&state->script, state->selected, &state->ssa,
			&module_ops, &diagnostics)
		: state->wave >= 7
		? zend_mir_lower_w07_zend_op_array(
			&state->script, state->selected, &state->ssa,
			&module_ops, &diagnostics)
		: zend_mir_lower_w05_zend_op_array(
			&state->script, state->selected, &state->ssa,
			&module_ops, &diagnostics);
#ifdef ZEND_MIR_W05_TEST_FAULTS
	zend_mir_w05_test_set_fault(ZEND_MIR_W05_TEST_FAULT_NONE);
#endif
	if (!(state->wave >= 8
			? zend_mir_lowering_result_is_w08_failure_atomic(&result)
			: zend_mir_lowering_result_is_w05_failure_atomic(&result))) {
		char detail[256];

		snprintf(
			detail, sizeof(detail),
			"W05 lowering returned a non-atomic result "
			"(status=%u diagnostic=%u guarantees=%u prerequisite=%u "
			"capabilities=%u debts=%u modeled=%u codegen=%u module=%u)",
			(unsigned int) result.lowering.status,
			(unsigned int) result.lowering.diagnostic_code,
			(unsigned int) result.lowering.guarantees,
			(unsigned int) result.prerequisite_guarantees,
			(unsigned int) result.capabilities,
			(unsigned int) result.semantic_debts,
			(unsigned int) result.modeled,
			(unsigned int) result.codegen_eligible,
			(unsigned int) (result.lowering.module != NULL));
		if (result.lowering.module != NULL) {
			native_mir_test_module_destroy(
				state, result.lowering.module);
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0007",
			detail);
		return false;
	}
	return native_mir_test_publish_lowering_result(
		state, result.lowering, state->wave);
}

static bool native_mir_test_lower_w06_and_dump(native_mir_test_state *state)
{
	zend_mir_lowering_module_ops module_ops;
	zend_mir_diagnostic_sink diagnostics;
	zend_mir_w06_lowering_result result;
#ifdef ZEND_MIR_W06_TEST_FAULTS
	zend_mir_w06_test_fault value_fault = ZEND_MIR_W06_TEST_FAULT_NONE;
#endif

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
#ifdef ZEND_MIR_W06_TEST_FAULTS
	switch (state->fault) {
		case NATIVE_MIR_TEST_FAULT_VALUE_INVENTORY:
			value_fault = ZEND_MIR_W06_TEST_FAULT_INVENTORY;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_PLAN:
			value_fault = ZEND_MIR_W06_TEST_FAULT_PLAN;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_STORAGE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_STORAGE;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_REFERENCE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_REFERENCE_CELL;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_ALIAS:
			value_fault = ZEND_MIR_W06_TEST_FAULT_ALIAS;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_EVENT:
			value_fault = ZEND_MIR_W06_TEST_FAULT_EVENT;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_SEPARATION:
			value_fault = ZEND_MIR_W06_TEST_FAULT_SEPARATION;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_CALL_TRANSFER:
			value_fault = ZEND_MIR_W06_TEST_FAULT_CALL_TRANSFER;
			break;
		case NATIVE_MIR_TEST_FAULT_STRUCTURAL_VERIFIER_FAILURE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_STRUCTURAL_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_SCALAR_VERIFIER_FAILURE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_SCALAR_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_CONTROL_FLOW_VERIFIER_FAILURE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_CONTROL_FLOW_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_CALL_VERIFIER_FAILURE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_CALL_VERIFIER;
			break;
		case NATIVE_MIR_TEST_FAULT_FINGERPRINT_RECOMPUTE_FAILURE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_FINGERPRINT_RECOMPUTE;
			break;
		case NATIVE_MIR_TEST_FAULT_VALUE_VERIFIER_FAILURE:
			value_fault = ZEND_MIR_W06_TEST_FAULT_VALUE_VERIFIER;
			break;
		default:
			break;
	}
	zend_mir_w06_test_set_fault(value_fault);
#endif
	result = zend_mir_lower_w06_zend_op_array(
		&state->script, state->selected, &state->ssa,
		&module_ops, &diagnostics);
#ifdef ZEND_MIR_W06_TEST_FAULTS
	zend_mir_w06_test_set_fault(ZEND_MIR_W06_TEST_FAULT_NONE);
#endif
	if (!zend_mir_lowering_result_is_w06_failure_atomic(&result)) {
		if (result.prerequisite.lowering.module != NULL) {
			native_mir_test_module_destroy(
				state, result.prerequisite.lowering.module);
		}
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0007",
			"W06 lowering returned a non-atomic result");
		return false;
	}
	return native_mir_test_publish_lowering_result(
		state, result.prerequisite.lowering, 6);
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
	if (state->wave == 6) {
		return native_mir_test_lower_w06_and_dump(state);
	}
	if (state->wave == 5 || state->wave == 7 || state->wave == 8) {
		return native_mir_test_lower_w05_and_dump(state);
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

static bool native_mir_test_is_scalar_zval(const zval *value)
{
	switch (Z_TYPE_P(value)) {
		case IS_NULL:
		case IS_FALSE:
		case IS_TRUE:
		case IS_LONG:
		case IS_DOUBLE:
			return true;
		default:
			return false;
	}
}

static zend_mir_scalar_type_mask native_mir_test_scalar_type_from_zval(
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

static zend_mir_scalar_type_mask native_mir_test_argument_runtime_type(
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

static uint32_t native_mir_test_may_be_from_scalar_type(
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

static bool native_mir_test_merge_argument_type(
	native_mir_test_state *state,
	native_mir_test_native_function *function,
	uint32_t ordinal,
	zend_mir_scalar_type_mask type)
{
	if (function == NULL || ordinal >= function->argument_type_count
			|| (!zend_mir_scalar_type_is_exact(type)
				&& !(state->wave >= 8 && type == ZEND_MIR_SCALAR_TYPE_NONE))) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0012",
			"native scalar operations require exact argument types");
		return false;
	}
	if (type == ZEND_MIR_SCALAR_TYPE_NONE) {
		return true;
	}
	if (function->argument_types[ordinal] != ZEND_MIR_SCALAR_TYPE_NONE
			&& function->argument_types[ordinal] != type) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0012",
			"W07 entry cell has conflicting scalar call signatures");
		return false;
	}
	function->argument_types[ordinal] = type;
	return true;
}

static zend_op_array *native_mir_test_resolve_native_target(
	native_mir_test_state *state,
	zend_op_array *caller,
	const zend_mir_call_target_ref *target)
{
	zend_function *function;
	uint32_t declaration_id = 1;

	if (target == NULL
			|| target->kind != ZEND_MIR_CALL_TARGET_DIRECT_USER
			|| target->function_symbol_id != target->op_array_id) {
		return NULL;
	}
	if (target->op_array_id == 0) {
		return caller;
	}
	ZEND_HASH_FOREACH_PTR(&state->script.function_table, function) {
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

static zend_function *native_mir_test_resolve_internal_target(
	native_mir_test_state *state,
	zend_op_array *caller,
	const zend_mir_call_view *calls,
	const zend_mir_call_target_ref *target,
	const zend_op **init_opline_out)
{
	uint32_t index;

	if (state == NULL || caller == NULL || calls == NULL || target == NULL
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
				function_index < state->native_function_count;
				function_index++) {
			if (state->native_functions[function_index].op_array == caller) {
				ssa = &state->native_functions[function_index].ssa;
				break;
			}
		}
		function = ssa == NULL ? NULL
			: zend_mir_zend_source_resolve_internal_call(
				&state->script, caller, ssa,
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

static native_mir_test_native_function *native_mir_test_find_native_function(
	native_mir_test_state *state, const zend_op_array *op_array)
{
	uint32_t index;

	for (index = 0; index < state->native_function_count; index++) {
		if (state->native_functions[index].op_array == op_array) {
			return &state->native_functions[index];
		}
	}
	return NULL;
}

static native_mir_test_native_function *native_mir_test_add_native_function(
	native_mir_test_state *state, zend_op_array *op_array)
{
	native_mir_test_native_function *function;

	function = native_mir_test_find_native_function(state, op_array);
	if (function != NULL) {
		return function;
	}
	if (op_array == NULL
			|| state->native_function_count >= state->native_function_capacity) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_CODEGEN, "native", "NATIVE0005",
			"native call component exceeds the compiled script");
		return NULL;
	}
	function = &state->native_functions[state->native_function_count++];
	memset(function, 0, sizeof(*function));
	function->op_array = op_array;
	function->module_host_ref = &function->module_host;
	function->argument_type_count = op_array->num_args;
	if (function->argument_type_count != 0) {
		function->argument_types = ecalloc(
			function->argument_type_count,
			sizeof(*function->argument_types));
	}
	zend_native_entry_cell_init(
		&function->entry_cell, (zend_function *) op_array);
	if (state->stack_probe_enabled) {
		zend_native_entry_cell_set_frame_probe(
			&function->entry_cell, native_mir_test_frame_probe_record, state);
	}
	if (zend_native_entry_cell_begin_compile(&function->entry_cell) == FAILURE) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_CODEGEN, "native", "NATIVE0003",
			"native entry cell rejected synchronous compilation");
		return NULL;
	}
	return function;
}

static bool native_mir_test_build_function_ssa(
	native_mir_test_state *state,
	native_mir_test_native_function *function)
{
	zend_optimizer_ctx optimizer;

	if (state->wave < 8 && function->op_array->last_try_catch != 0) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0015",
			"protected native callees require W08");
		return false;
	}
	function->ssa_arena = zend_arena_create(NATIVE_MIR_TEST_ARENA_SIZE);
	if (function->ssa_arena == NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_SSA, "ssa", "SSA0002",
			"unable to allocate a native callee SSA arena");
		return false;
	}
	memset(&optimizer, 0, sizeof(optimizer));
	optimizer.arena = function->ssa_arena;
	optimizer.script = &state->script;
	optimizer.optimization_level = ZEND_OPTIMIZER_PASS_6;
	if (zend_dfa_analyze_op_array(
			function->op_array, &optimizer, &function->ssa) == FAILURE) {
		function->ssa_arena = optimizer.arena;
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_SSA, "ssa", "SSA0001",
			"SSA analysis rejected a reachable native callee");
		return false;
	}
	function->ssa_arena = optimizer.arena;
	return true;
}

static zend_mir_scalar_type_mask native_mir_test_ssa_exact_type(
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

static zend_mir_scalar_type_mask native_mir_test_operand_exact_type(
	const zend_op_array *op_array, const zend_ssa *ssa,
	uint32_t opline_index, uint8_t operand_type, const znode_op *operand,
	int ssa_use)
{
	if (operand_type == IS_CONST) {
		return native_mir_test_scalar_type_from_zval(
			RT_CONSTANT(&op_array->opcodes[opline_index], *operand));
	}
	return native_mir_test_ssa_exact_type(ssa, ssa_use);
}

static uint32_t native_mir_test_zend_type_from_scalar_type(
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

static bool native_mir_test_verify_projected_return_type(
	const native_mir_test_native_function *function, uint32_t opline_index)
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
	type = native_mir_test_operand_exact_type(
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

static bool native_mir_test_prepare_w07_projection(
	native_mir_test_state *state,
	native_mir_test_native_function *function)
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
	if (echo_count != 0) {
		function->source_effects = ecalloc(
			echo_count, sizeof(*function->source_effects));
	}
	memcpy(function->projected_opcodes, source->opcodes,
		(size_t) source->last * sizeof(*function->projected_opcodes));
	memcpy(function->projected_literals, source->literals,
		(size_t) source->last_literal * sizeof(*function->projected_literals));
	memcpy(function->projected_ssa_ops, function->ssa.ops,
		(size_t) source->last * sizeof(*function->projected_ssa_ops));
	memcpy(function->projected_ssa_vars, function->ssa.vars,
		(size_t) function->ssa.vars_count
			* sizeof(*function->projected_ssa_vars));
	memcpy(function->projected_ssa_var_info, function->ssa.var_info,
		(size_t) function->ssa.vars_count
			* sizeof(*function->projected_ssa_var_info));
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
	if ((state->wave >= 8
			? zend_mir_frontend_project_w08_result_facts(
				&state->script, source, &function->ssa,
				&function->projected_ssa, &frontend_diagnostic)
			: zend_mir_frontend_project_w05_result_facts(
				&state->script, source, &function->ssa,
				&function->projected_ssa, &frontend_diagnostic))
			!= ZEND_MIR_LOWERING_SUCCESS) {
		char code[16];

		snprintf(code, sizeof(code), "MIRL%04u",
			(unsigned int) frontend_diagnostic.code);
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_REJECTED,
			NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", code,
			"W07 cannot project a direct scalar call result");
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
			zend_mir_scalar_type_mask type = native_mir_test_operand_exact_type(
				&function->projected_op_array, &function->projected_ssa,
				index, opline->op1_type, &opline->op1, ssa_op->op1_use);

			effect->source_position_id = index;
			effect->kind = ZEND_NATIVE_SOURCE_EFFECT_ECHO_SCALAR;
			effect->exact_type = type;
			if (!zend_mir_scalar_type_is_exact(type)) {
				native_mir_test_fail(
					state, NATIVE_MIR_TEST_STATUS_REJECTED,
					NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0012",
					"W07 echo requires an exact scalar value");
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
				native_mir_test_operand_exact_type(
					&function->projected_op_array,
					&function->projected_ssa,
					index, opline->op1_type, &opline->op1,
					ssa_op->op1_use);

			if (!native_mir_test_verify_projected_return_type(
					function, index)) {
				native_mir_test_fail(
					state, NATIVE_MIR_TEST_STATUS_REJECTED,
					NATIVE_MIR_TEST_PHASE_LOWERING, "MIRL", "MIRL0012",
					"W07 cannot prove the scalar return type check");
				return false;
			}
			const zend_arg_info *return_info =
				function->op_array->arg_info - 1;
			uint32_t operand_zend_type =
				native_mir_test_zend_type_from_scalar_type(operand_type);

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
					native_mir_test_may_be_from_scalar_type(operand_type);
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
				type = native_mir_test_scalar_type_from_zval(
					RT_CONSTANT(original, original->op2));
			}
			if (zend_mir_scalar_type_is_exact(type)
					&& ssa_op->result_def >= 0
					&& ssa_op->result_def < function->projected_ssa.vars_count) {
				function->projected_ssa_var_info[ssa_op->result_def].type =
					native_mir_test_may_be_from_scalar_type(type);
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

static bool native_mir_test_lower_native_function(
	native_mir_test_state *state,
	native_mir_test_native_function *function)
{
	native_mir_test_state local = *state;
	uint32_t diagnostic_count = state->diagnostic_count;
	bool has_call = false;
	uint32_t opcode_index;

	if (state->wave >= 7
			&& function->projected_opcodes == NULL
			&& !native_mir_test_prepare_w07_projection(state, function)) {
		return false;
	}
	local.selected = state->wave >= 7
		? &function->projected_op_array : function->op_array;
	local.ssa_arena = function->ssa_arena;
	local.ssa = state->wave >= 7 ? function->projected_ssa : function->ssa;
	memset(&function->module_host, 0, sizeof(function->module_host));
	function->module_host.fail_enabled =
		state->fault == NATIVE_MIR_TEST_FAULT_MODULE_OOM;
	local.active_module_host = &function->module_host;
	local.module = NULL;
	local.native_image = NULL;
	local.native_code = NULL;
	local.native_functions = NULL;
	local.native_function_count = 0;
	local.native_function_capacity = 0;
	local.source_opcodes = NULL;
	memset(&local.dump, 0, sizeof(local.dump));
	for (opcode_index = 0; opcode_index < local.selected->last;
			opcode_index++) {
		uint8_t opcode = local.selected->opcodes[opcode_index].opcode;

		if (opcode == ZEND_DO_UCALL || opcode == ZEND_DO_FCALL
				|| (state->wave >= 8 && opcode == ZEND_DO_ICALL)) {
			has_call = true;
			break;
		}
	}
	if (!(state->wave == 6
			? native_mir_test_lower_w06_and_dump(&local)
			: has_call
				? native_mir_test_lower_w05_and_dump(&local)
				: local.ssa.cfg.blocks_count > 1
					? native_mir_test_lower_w04_and_dump(&local)
					: native_mir_test_lower_w03_and_dump(&local))) {
		function->module = local.module;
		state->phase = local.phase;
		state->status = local.status;
		state->diagnostic_count = local.diagnostic_count;
		state->diagnostic_stage = local.diagnostic_stage;
		return false;
	}
	function->module = local.module;
	if (function->op_array == state->selected && diagnostic_count == 0) {
		state->diagnostic_count = local.diagnostic_count;
		state->diagnostic_stage = local.diagnostic_stage;
	} else {
		/* Reachable callees are an internal synchronous compilation detail. */
		state->diagnostic_count = diagnostic_count;
	}
	return true;
}

static bool native_mir_test_discover_native_callees(
	native_mir_test_state *state,
	native_mir_test_native_function *function)
{
	const zend_mir_call_view *calls =
		zend_mir_module_get_call_view(function->module);
	uint32_t target_count;
	uint32_t index;

	if (calls == NULL) {
		return true;
	}
	if (calls->call_target_count == NULL || calls->call_target_at == NULL) {
		native_mir_test_fail(
			state, NATIVE_MIR_TEST_STATUS_ERROR,
			NATIVE_MIR_TEST_PHASE_CODEGEN, "native", "NATIVE0003",
			"native call model is unavailable");
		return false;
	}
	target_count = calls->call_target_count(calls->context);
	for (index = 0; index < target_count; index++) {
		zend_mir_call_target_ref target;
		zend_op_array *callee;

		if (!calls->call_target_at(calls->context, index, &target)) {
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				NATIVE_MIR_TEST_PHASE_CODEGEN, "native", "NATIVE0003",
				"native call target table is unreadable");
			return false;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			if (state->wave < 8 || native_mir_test_resolve_internal_target(
					state, function->op_array, calls, &target, NULL) == NULL) {
				return false;
			}
			continue;
		}
		callee = native_mir_test_resolve_native_target(
			state, function->op_array, &target);
		if (callee == NULL
				|| native_mir_test_add_native_function(state, callee) == NULL) {
			if (state->phase != NATIVE_MIR_TEST_PHASE_CODEGEN
					|| state->status == NATIVE_MIR_TEST_STATUS_ACCEPTED) {
				native_mir_test_fail(
					state, NATIVE_MIR_TEST_STATUS_ERROR,
					NATIVE_MIR_TEST_PHASE_CODEGEN, "native", "NATIVE0003",
					"native call target cannot be resolved to its source function");
			}
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
		native_mir_test_native_function *native_callee;
		const zend_mir_view *view = native_mir_test_module_view(
			state, function->module);
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
				callee = native_mir_test_resolve_native_target(
					state, function->op_array, &target);
				break;
			}
		}
		if (!found_target) {
			return false;
		}
		if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
			continue;
		}
		native_callee = native_mir_test_find_native_function(state, callee);
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
			if (!found || !native_mir_test_merge_argument_type(
					state, native_callee, argument.ordinal,
					native_mir_test_argument_runtime_type(
						native_callee->op_array, argument.ordinal,
						fact.exact_type))) {
				return false;
			}
		}
	}
	return true;
}

static void native_mir_test_fail_native_component(
	native_mir_test_state *state)
{
	uint32_t index;

	for (index = 0; index < state->native_function_count; index++) {
		zend_native_entry_cell_fail(
			&state->native_functions[index].entry_cell);
	}
}

static bool native_mir_test_prepare_native_component(
	native_mir_test_state *state, HashTable *arguments)
{
	native_mir_test_native_function *selected;
	uint32_t script_function_count =
		zend_hash_num_elements(&state->script.function_table);
	uint32_t index;

	if (script_function_count == UINT32_MAX) {
		native_mir_test_backend_failure(
			state, NATIVE_MIR_TEST_PHASE_CODEGEN, NULL);
		return false;
	}
	state->native_function_capacity = script_function_count + 1;
	state->native_functions = ecalloc(
		state->native_function_capacity, sizeof(*state->native_functions));
	selected = native_mir_test_add_native_function(state, state->selected);
	if (selected == NULL) {
		return false;
	}
	if (arguments != NULL) {
		zval *argument;
		uint32_t ordinal = 0;

		ZEND_HASH_FOREACH_VAL(arguments, argument) {
			zend_mir_scalar_type_mask type =
				native_mir_test_argument_runtime_type(
					selected->op_array, ordinal,
					native_mir_test_scalar_type_from_zval(argument));

			if (!native_mir_test_merge_argument_type(
					state, selected, ordinal, type)) {
				return false;
			}
			ordinal++;
		} ZEND_HASH_FOREACH_END();
	}
	selected->ssa_arena = state->ssa_arena;
	selected->ssa = state->ssa;
	if (state->wave < 7) {
		selected->module_host_ref = &state->module_host;
		selected->module = state->module;
	}
	state->ssa_arena = NULL;
	state->module = NULL;

	for (index = 0; index < state->native_function_count; index++) {
		native_mir_test_native_function *function =
			&state->native_functions[index];

		if (function->module == NULL) {
			if ((function->ssa.ops == NULL
					&& !native_mir_test_build_function_ssa(state, function))
					|| !native_mir_test_lower_native_function(state, function)) {
				native_mir_test_fail_native_component(state);
				return false;
			}
		}
		if (!native_mir_test_discover_native_callees(state, function)) {
			native_mir_test_fail_native_component(state);
			return false;
		}
	}
	return true;
}

static bool native_mir_test_compile_native_component(
	native_mir_test_state *state)
{
	zend_native_diagnostic diagnostic;
	uint32_t index;

	state->phase = NATIVE_MIR_TEST_PHASE_CODEGEN;
	for (index = 0; index < state->native_function_count; index++) {
		native_mir_test_native_function *function =
			&state->native_functions[index];
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
			native_mir_test_native_function *native_callee;

			if (!calls->call_target_at(calls->context, target_index, &target)) {
				efree(bindings);
				efree(internal_bindings);
				native_mir_test_backend_failure(
					state, state->phase, NULL);
				native_mir_test_fail_native_component(state);
				return false;
			}
			if (target.kind == ZEND_MIR_CALL_TARGET_DIRECT_INTERNAL) {
				zend_function *internal;
				const zend_op *init_opline = NULL;
				zend_native_internal_receiver_kind receiver_kind =
					ZEND_NATIVE_INTERNAL_RECEIVER_NONE;
				zend_class_entry *called_scope = NULL;

				internal = native_mir_test_resolve_internal_target(
					state, function->op_array, calls, &target, &init_opline);
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
						native_mir_test_fail(
							state, NATIVE_MIR_TEST_STATUS_REJECTED,
							NATIVE_MIR_TEST_PHASE_CODEGEN,
							"native", "NATIVE0003",
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
			callee = native_mir_test_resolve_native_target(
				state, function->op_array, &target);
			native_callee = native_mir_test_find_native_function(state, callee);
			if (native_callee == NULL) {
				goto binding_failure;
			}
			bindings[binding_count].target_id = target.id;
			bindings[binding_count].entry_cell = &native_callee->entry_cell;
			binding_count++;
		}
		function->internal_call_cell_count = internal_binding_count;
		memset(&diagnostic, 0, sizeof(diagnostic));
		if ((state->wave >= 8
				? zend_tpde_compile_module_w08(
					state->target,
					native_mir_test_module_view(state, function->module),
					bindings, binding_count,
					internal_bindings, internal_binding_count,
					function->source_effects, function->source_effect_count,
					function->op_array->num_args,
					&function->image, &diagnostic)
				: zend_tpde_compile_module_w07(
					state->target,
					native_mir_test_module_view(state, function->module),
					bindings, binding_count,
					function->source_effects, function->source_effect_count,
					function->op_array->num_args,
					&function->image, &diagnostic)) == FAILURE) {
			efree(bindings);
			efree(internal_bindings);
			native_mir_test_backend_failure(state, state->phase, &diagnostic);
			native_mir_test_fail_native_component(state);
			return false;
		}
		efree(bindings);
		efree(internal_bindings);
		continue;

binding_failure:
		native_mir_test_backend_failure(state, state->phase, NULL);
binding_rejected:
		efree(bindings);
		efree(internal_bindings);
		native_mir_test_fail_native_component(state);
		return false;
	}

	state->phase = NATIVE_MIR_TEST_PHASE_PUBLISH;
	if (state->fault == NATIVE_MIR_TEST_FAULT_MAPPING_FAILURE) {
		memset(&diagnostic, 0, sizeof(diagnostic));
		diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
		snprintf(diagnostic.message, sizeof(diagnostic.message),
			"injected native mapping failure");
		native_mir_test_backend_failure(state, state->phase, &diagnostic);
		native_mir_test_fail_native_component(state);
		return false;
	}
	for (index = 0; index < state->native_function_count; index++) {
		native_mir_test_native_function *function =
			&state->native_functions[index];

		memset(&diagnostic, 0, sizeof(diagnostic));
		if (zend_native_publish_image(
				state->target, function->image, &function->code,
				&diagnostic) == FAILURE) {
			native_mir_test_backend_failure(state, state->phase, &diagnostic);
			native_mir_test_fail_native_component(state);
			return false;
		}
		if (zend_native_code_is_writable(function->code)
				|| !zend_native_code_is_executable(function->code)) {
			memset(&diagnostic, 0, sizeof(diagnostic));
			diagnostic.code = ZEND_NATIVE_DIAGNOSTIC_MAPPING_FAILED;
			snprintf(diagnostic.message, sizeof(diagnostic.message),
				"published code violates the W^X contract");
			native_mir_test_backend_failure(state, state->phase, &diagnostic);
			native_mir_test_fail_native_component(state);
			return false;
		}
	}
	for (index = 0; index < state->native_function_count; index++) {
		native_mir_test_native_function *function =
			&state->native_functions[index];

		if (zend_native_entry_cell_publish(
				&function->entry_cell, function->code) == FAILURE) {
			native_mir_test_backend_failure(
				state, state->phase, NULL);
			native_mir_test_fail_native_component(state);
			return false;
		}
	}
	state->native_image = state->native_functions[0].image;
	state->native_code = state->native_functions[0].code;
	state->native_writable_after_publish = false;
	state->native_executable_after_publish = true;
	return true;
}

static bool native_mir_test_scalar_from_zval(
	const zval *value, zend_native_scalar *scalar)
{
	memset(scalar, 0, sizeof(*scalar));
	switch (Z_TYPE_P(value)) {
		case IS_NULL:
			scalar->kind = ZEND_NATIVE_SCALAR_NULL;
			return true;
		case IS_FALSE:
		case IS_TRUE:
			scalar->kind = ZEND_NATIVE_SCALAR_BOOL;
			scalar->payload_bits = Z_TYPE_P(value) == IS_TRUE;
			return true;
		case IS_LONG:
			scalar->kind = ZEND_NATIVE_SCALAR_LONG;
			scalar->payload_bits = (uint64_t) Z_LVAL_P(value);
			return true;
		case IS_DOUBLE:
			scalar->kind = ZEND_NATIVE_SCALAR_DOUBLE;
			memcpy(&scalar->payload_bits, &Z_DVAL_P(value), sizeof(double));
			return true;
		default:
			return false;
	}
}

static zend_native_status native_mir_test_execute_frame(
	native_mir_test_state *state,
	HashTable *arguments,
	zend_native_diagnostic *diagnostic)
{
	uint32_t argument_count = arguments != NULL
		? zend_hash_num_elements(arguments) : 0;
	if (state->wave < 7) {
		zend_native_scalar *native_arguments = argument_count != 0
			? safe_emalloc(argument_count, sizeof(*native_arguments), 0)
			: NULL;
		zend_native_scalar result;
		uint32_t index;

		for (index = 0; index < argument_count; index++) {
			zval *argument = zend_hash_index_find(arguments, index);

			if (argument == NULL || !native_mir_test_scalar_from_zval(
					argument, &native_arguments[index])) {
				efree(native_arguments);
				return ZEND_NATIVE_EXCEPTION;
			}
		}
		if (zend_native_execute(
				state->native_code, native_arguments, argument_count,
				&result, diagnostic) == FAILURE) {
			efree(native_arguments);
			return ZEND_NATIVE_EXCEPTION;
		}
		efree(native_arguments);
		switch (result.kind) {
			case ZEND_NATIVE_SCALAR_NULL:
				ZVAL_NULL(&state->native_result);
				break;
			case ZEND_NATIVE_SCALAR_BOOL:
				ZVAL_BOOL(&state->native_result, result.payload_bits != 0);
				break;
			case ZEND_NATIVE_SCALAR_LONG:
				ZVAL_LONG(&state->native_result, (zend_long) result.payload_bits);
				break;
			case ZEND_NATIVE_SCALAR_DOUBLE: {
				double value;
				memcpy(&value, &result.payload_bits, sizeof(value));
				ZVAL_DOUBLE(&state->native_result, value);
				break;
			}
			default:
				return ZEND_NATIVE_EXCEPTION;
		}
		return ZEND_NATIVE_RETURNED;
	}
	zend_execute_data *previous = EG(current_execute_data);
	zend_execute_data *frame;
	uint32_t index;
	zend_native_status status;

	frame = zend_vm_stack_push_call_frame(
		ZEND_CALL_NESTED_FUNCTION, (zend_function *) state->selected,
		argument_count, NULL);
	for (index = 0; index < argument_count; ++index) {
		zval *argument = zend_hash_index_find(arguments, index);

		ZEND_ASSERT(argument != NULL);
		if (state->wave >= 8) {
			ZVAL_COPY(ZEND_CALL_ARG(frame, index + 1), argument);
		} else {
			ZVAL_COPY_VALUE(ZEND_CALL_ARG(frame, index + 1), argument);
		}
	}
	ZVAL_UNDEF(&state->native_result);
	zend_init_func_execute_data(frame, state->selected, &state->native_result);
	status = zend_native_execute_frame(state->native_code, frame, diagnostic);
	EG(current_execute_data) = previous;
	zend_vm_stack_free_call_frame(frame);
	return status;
}

static bool native_mir_test_execute_module(
	native_mir_test_state *state, HashTable *arguments)
{
	zend_native_diagnostic diagnostic;
	uint32_t argument_count = arguments != NULL
		? zend_hash_num_elements(arguments) : 0;
	uint32_t index;

	if (!native_mir_test_prepare_native_component(state, arguments)
			|| !native_mir_test_compile_native_component(state)) {
		return false;
	}
	for (index = 0; index < argument_count; index++) {
		zval *argument = zend_hash_index_find(arguments, index);

		if (argument == NULL
				|| (state->wave < 8
					&& !native_mir_test_is_scalar_zval(argument))) {
			native_mir_test_fail(
				state, NATIVE_MIR_TEST_STATUS_ERROR,
				NATIVE_MIR_TEST_PHASE_EXECUTE, "native", "INVALID_ARGUMENTS",
				"native arguments must be a packed list of supported values");
			return false;
		}
	}
	state->phase = NATIVE_MIR_TEST_PHASE_EXECUTE;
	memset(&diagnostic, 0, sizeof(diagnostic));
	for (index = 0; index < state->execute_repetitions; ++index) {
		zend_native_status native_status = native_mir_test_execute_frame(
			state, arguments, &diagnostic);

		if (native_status != ZEND_NATIVE_RETURNED) {
			state->native_exception = native_status == ZEND_NATIVE_EXCEPTION;
			state->native_bailout = native_status == ZEND_NATIVE_BAILOUT;
			native_mir_test_backend_failure(state, state->phase, &diagnostic);
			return false;
		}
		state->completed_executions++;
	}
	state->native_result_valid = true;
	state->status = NATIVE_MIR_TEST_STATUS_ACCEPTED;
	state->phase = NATIVE_MIR_TEST_PHASE_COMPLETE;
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
	if (state->native_result_valid) {
		zval_ptr_dtor(&state->native_result);
		state->native_result_valid = false;
	}
	if (state->native_functions != NULL) {
		for (index = 0; index < state->native_function_count; index++) {
			native_mir_test_native_function *function =
				&state->native_functions[index];

			(void) zend_native_entry_cell_reset(&function->entry_cell);
			if (function->code != NULL) {
				zend_native_code_destroy(function->code);
			}
			if (function->image != NULL) {
				zend_native_image_destroy(function->image);
			}
			if (function->module != NULL) {
				zend_mir_module_destroy(function->module);
			} else {
				native_mir_test_module_reset(function->module_host_ref);
			}
			if (function->ssa_arena != NULL) {
				zend_arena_destroy(function->ssa_arena);
			}
			efree(function->source_effects);
			efree(function->internal_call_cells);
			efree(function->argument_types);
			efree(function->projected_ssa_var_info);
			efree(function->projected_ssa_vars);
			efree(function->projected_ssa_ops);
			efree(function->projected_opcodes);
		}
		efree(state->native_functions);
		state->native_functions = NULL;
		state->native_function_count = 0;
		state->native_function_capacity = 0;
		state->native_code = NULL;
		state->native_image = NULL;
		state->module = NULL;
		state->ssa_arena = NULL;
	} else {
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
	if (state->wave >= 4) {
		add_assoc_long(return_value, "wave", state->wave);
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

		array_init(&execution);
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
		add_assoc_long(&execution, "executions",
			(zend_long) state->completed_executions);
		add_assoc_bool(&execution, "writable_after_publish",
			state->native_writable_after_publish);
		add_assoc_bool(&execution, "executable_after_publish",
			state->native_executable_after_publish);
		add_assoc_long(&execution, "image_size",
			(zend_long) zend_native_image_size(state->native_image));
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

			ZVAL_COPY(&value, &state->native_result);
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
	state->wave = 4;
	state->execute_mode = true;
	state->execute_repetitions = 1;
	state->frame_chain_valid = true;
#if defined(__APPLE__) && defined(__aarch64__)
	state->target = ZEND_NATIVE_TARGET_DARWIN_ARM64;
#elif defined(__linux__) && defined(__x86_64__)
	state->target = ZEND_NATIVE_TARGET_LINUX_AMD64;
#else
# error "native_mir_test execute bridge supports only Darwin arm64 and Linux x86-64"
#endif
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
	if (state->stack_probe_enabled) {
		state->frame_probes = ecalloc(
			NATIVE_MIR_TEST_MAX_FRAME_PROBES, sizeof(*state->frame_probes));
	}
	state->module_host.fail_enabled =
		state->fault == NATIVE_MIR_TEST_FAULT_MODULE_OOM;
	state->module_host.fail_after = 0;

	/* This path never invokes an opline handler or a VM execute function. */
	zend_try {
		if (native_mir_test_compile(state)
				&& native_mir_test_build_ssa(state)
				&& (state->wave >= 7
					|| native_mir_test_lower_and_dump(state))) {
			(void) native_mir_test_execute_module(state, arguments);
		}
	} zend_catch {
		bailed_out = true;
	} zend_end_try();

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
	native_mir_test_cleanup(state);
	smart_str_free(&state->dump);
	efree(state->source_opcodes);
	efree(state->diagnostics);
	efree(state->frame_probes);
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
