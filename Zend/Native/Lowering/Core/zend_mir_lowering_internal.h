/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | https://www.php.net/license/3_01.txt                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef ZEND_MIR_LOWERING_INTERNAL_H
#define ZEND_MIR_LOWERING_INTERNAL_H

#include "../zend_mir_lowering.h"

#define ZEND_MIR_LOWERING_MAX_PROVIDERS UINT32_C(32)
#define ZEND_MIR_LOWERING_MAX_CLAIMS UINT32_C(64)

struct _zend_mir_zend_source;

typedef enum _zend_mir_lowering_profile_disposition {
	ZEND_MIR_LOWERING_PROFILE_ACCEPTED = 0,
	ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04 = 1,
	ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05 = 2,
	ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06 = 3,
	ZEND_MIR_LOWERING_PROFILE_DEFERRED_OTHER = 4,
	ZEND_MIR_LOWERING_PROFILE_REJECTED = 5,
	ZEND_MIR_LOWERING_PROFILE_DISPOSITION_INVALID = -1
} zend_mir_lowering_profile_disposition;

typedef struct _zend_mir_lowering_profile_entry {
	uint32_t zend_opcode_number;
	zend_mir_lowering_profile_disposition disposition;
} zend_mir_lowering_profile_entry;

typedef struct _zend_mir_lowering_profile {
	const zend_mir_lowering_profile_entry *entries;
	uint32_t entry_count;
} zend_mir_lowering_profile;

typedef struct _zend_mir_lowering_provider_array {
	const zend_mir_lowering_provider *providers;
	uint32_t provider_count;
} zend_mir_lowering_provider_array;

/*
 * The frozen source view contains the linear opcode and SSA records, while
 * front-end CFG facts remain process-local.  Supplying them separately keeps
 * the public source ABI stable and makes the W04 boundary explicit.
 */
typedef struct _zend_mir_lowering_source_shape {
	uint32_t reachable_block_count;
	bool has_control_flow;
	bool has_calls;
	bool has_try_regions;
	bool ssa_complete;
} zend_mir_lowering_source_shape;

typedef struct _zend_mir_lowering_limits {
	uint32_t max_opcodes;
	uint32_t max_ssa_variables;
	uint32_t max_ssa_uses;
	uint32_t max_ssa_defs;
	uint32_t max_literals;
} zend_mir_lowering_limits;

typedef struct _zend_mir_lowering_module_ops {
	void *context;
	zend_mir_module *(*create)(void *context, zend_mir_module_id module_id,
		zend_mir_diagnostic_sink *diagnostics);
	void (*destroy)(void *context, zend_mir_module *module);
	zend_mir_mutator *(*mutator)(void *context, zend_mir_module *module);
	const zend_mir_view *(*view)(void *context, const zend_mir_module *module);
	bool (*finalize)(void *context, zend_mir_module *module);
	bool (*verify_stage1)(void *context, const zend_mir_view *view,
		zend_mir_diagnostic_sink *diagnostics);
	bool (*verify_stage2)(void *context, const zend_mir_view *view,
		zend_mir_diagnostic_sink *diagnostics);
} zend_mir_lowering_module_ops;

typedef struct _zend_mir_lowering_dispatch_entry {
	zend_mir_lowering_claim claim;
	uint32_t provider_index;
} zend_mir_lowering_dispatch_entry;

struct _zend_mir_lowering_registry {
	const zend_mir_lowering_profile *profile;
	zend_mir_lowering_provider providers[ZEND_MIR_LOWERING_MAX_PROVIDERS];
	uint32_t provider_count;
	zend_mir_lowering_dispatch_entry dispatch[ZEND_MIR_LOWERING_MAX_CLAIMS];
	uint32_t dispatch_count;
	zend_mir_lowering_claim builtin_nop_claim;
	bool complete;
};

struct _zend_mir_lowering_context {
	const zend_mir_lowering_source_view *source;
	zend_mir_lowering_source_shape shape;
	const zend_mir_lowering_registry *registry;
	zend_mir_lowering_module_ops module_ops;
	zend_mir_diagnostic_sink *diagnostics;
	zend_mir_lowering_limits limits;
	zend_mir_module_id module_id;
	zend_mir_symbol_id function_symbol_id;
	zend_mir_function_id function_id;
	zend_mir_block_id block_id;
	const zend_mir_lowering_provider *current_provider;
	const zend_mir_source_opcode_ref *current_opcode;
	zend_mir_lowering_status provider_status;
	zend_mir_lowering_diagnostic_code provider_diagnostic;
	uint32_t last_diagnostic_opline;
	const void *value_fact_context;
	bool (*value_fact_at)(const void *context, zend_mir_value_id value_id,
		zend_mir_value_fact_ref *fact_out);
	const struct _zend_mir_zend_source *zend_source;
	bool has_last_diagnostic_opline;
	bool values_predeclared;
	bool busy;
};

bool zend_mir_lowering_registry_init(zend_mir_lowering_registry *registry,
	const zend_mir_lowering_profile *profile,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
bool zend_mir_lowering_registry_construct(zend_mir_lowering_registry *registry,
	const zend_mir_lowering_profile *profile,
	const zend_mir_lowering_provider_array *provider_arrays,
	uint32_t provider_array_count,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
bool zend_mir_lowering_registry_validate(zend_mir_lowering_registry *registry,
	zend_mir_lowering_diagnostic_code *diagnostic_out);
const zend_mir_lowering_profile *zend_mir_lowering_w03_profile(void);
const zend_mir_lowering_profile_entry *zend_mir_lowering_profile_find(
	const zend_mir_lowering_profile *profile, uint32_t zend_opcode_number);
const zend_mir_lowering_provider *zend_mir_lowering_registry_find(
	const zend_mir_lowering_registry *registry, uint32_t zend_opcode_number);
bool zend_mir_lowering_register_nop(zend_mir_lowering_registry *registry,
	uint32_t semantic_family_id, zend_mir_lowering_diagnostic_code *diagnostic_out);

bool zend_mir_lowering_context_init(zend_mir_lowering_context *context,
	const zend_mir_lowering_source_view *source,
	const zend_mir_lowering_source_shape *shape,
	const zend_mir_lowering_registry *registry,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics,
	zend_mir_module_id module_id,
	zend_mir_symbol_id function_symbol_id,
	const zend_mir_lowering_limits *limits);
const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context);
zend_mir_function_id zend_mir_lowering_context_function_id(
	const zend_mir_lowering_context *context);
zend_mir_block_id zend_mir_lowering_context_block_id(
	const zend_mir_lowering_context *context);
bool zend_mir_lowering_context_set_block_id(
	zend_mir_lowering_context *context, zend_mir_block_id block_id);
bool zend_mir_lowering_context_set_value_fact_resolver(
	zend_mir_lowering_context *context, const void *resolver_context,
	bool (*value_fact_at)(const void *resolver_context,
		zend_mir_value_id value_id, zend_mir_value_fact_ref *fact_out));
bool zend_mir_lowering_context_set_zend_source(
	zend_mir_lowering_context *context,
	const struct _zend_mir_zend_source *source);
bool zend_mir_lowering_context_value_fact(
	const zend_mir_lowering_context *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *fact_out);
bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic);

bool zend_mir_lowering_emit_diagnostic(zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code,
	const zend_mir_source_opcode_ref *source_opcode,
	const char *detail);
const char *zend_mir_lowering_diagnostic_token(
	zend_mir_lowering_diagnostic_code code);

#endif /* ZEND_MIR_LOWERING_INTERNAL_H */
