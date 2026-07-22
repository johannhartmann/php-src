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

#include <string.h>

#include "zend_mir_lowering_internal.h"

static const zend_mir_lowering_limits zend_mir_lowering_default_limits = {
	UINT32_C(4096),
	UINT32_C(4096),
	UINT32_C(16384),
	UINT32_C(4096),
	UINT32_C(4096)
};

static bool zend_mir_lowering_limits_are_valid(
	const zend_mir_lowering_limits *limits)
{
	return limits != NULL
		&& limits->max_opcodes != 0
		&& limits->max_ssa_variables != 0
		&& limits->max_ssa_uses != 0
		&& limits->max_ssa_defs != 0
		&& limits->max_literals != 0;
}

bool zend_mir_lowering_context_init(zend_mir_lowering_context *context,
	const zend_mir_lowering_source_view *source,
	const zend_mir_lowering_source_shape *shape,
	const zend_mir_lowering_registry *registry,
	const zend_mir_lowering_module_ops *module_ops,
	zend_mir_diagnostic_sink *diagnostics,
	zend_mir_module_id module_id,
	zend_mir_symbol_id function_symbol_id,
	const zend_mir_lowering_limits *limits)
{
	if (context == NULL || source == NULL || shape == NULL || registry == NULL
			|| !registry->complete || module_ops == NULL
			|| module_ops->create == NULL || module_ops->destroy == NULL
			|| module_ops->mutator == NULL || module_ops->view == NULL
			|| module_ops->finalize == NULL || module_ops->verify_stage1 == NULL
			|| module_ops->verify_stage2 == NULL
			|| !zend_mir_id_is_valid(module_id)
			|| !zend_mir_id_is_valid(function_symbol_id)
			|| (limits != NULL && !zend_mir_lowering_limits_are_valid(limits))) {
		return false;
	}

	memset(context, 0, sizeof(*context));
	context->source = source;
	context->shape = *shape;
	context->registry = registry;
	context->module_ops = *module_ops;
	context->diagnostics = diagnostics;
	context->limits = limits != NULL ? *limits : zend_mir_lowering_default_limits;
	context->module_id = module_id;
	context->function_symbol_id = function_symbol_id;
	context->function_id = ZEND_MIR_ID_INVALID;
	context->block_id = ZEND_MIR_ID_INVALID;
	context->provider_status = ZEND_MIR_LOWERING_SUCCESS;
	context->provider_diagnostic = ZEND_MIRL_OK;
	return true;
}

const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context)
{
	if (context == NULL || context->current_provider == NULL) {
		return NULL;
	}
	return context->current_provider->context;
}

zend_mir_function_id zend_mir_lowering_context_function_id(
	const zend_mir_lowering_context *context)
{
	return context != NULL ? context->function_id : ZEND_MIR_ID_INVALID;
}

zend_mir_block_id zend_mir_lowering_context_block_id(
	const zend_mir_lowering_context *context)
{
	return context != NULL ? context->block_id : ZEND_MIR_ID_INVALID;
}

bool zend_mir_lowering_context_set_block_id(
	zend_mir_lowering_context *context, zend_mir_block_id block_id)
{
	if (context == NULL || !zend_mir_id_is_valid(block_id)) {
		return false;
	}
	context->block_id = block_id;
	return true;
}

bool zend_mir_lowering_context_set_value_fact_resolver(
	zend_mir_lowering_context *context, const void *resolver_context,
	bool (*value_fact_at)(const void *resolver_context,
		zend_mir_value_id value_id, zend_mir_value_fact_ref *fact_out))
{
	if (context == NULL || resolver_context == NULL || value_fact_at == NULL
			|| context->busy) {
		return false;
	}
	context->value_fact_context = resolver_context;
	context->value_fact_at = value_fact_at;
	return true;
}

bool zend_mir_lowering_context_set_zend_source(
	zend_mir_lowering_context *context,
	const struct _zend_mir_zend_source *source)
{
	if (context == NULL || source == NULL || context->busy) {
		return false;
	}
	context->zend_source = source;
	return true;
}

bool zend_mir_lowering_context_set_post_call_composition(
	zend_mir_lowering_context *context, const void *composition_context,
	bool (*compose)(const void *composition_context,
		zend_mir_lowering_context *lowering_context,
		zend_mir_module *module,
		const zend_mir_control_flow_map *control_flow_map))
{
	if (context == NULL || composition_context == NULL || compose == NULL
			|| context->busy) {
		return false;
	}
	context->post_call_composition_context = composition_context;
	context->post_call_composition = compose;
	return true;
}

bool zend_mir_lowering_context_value_fact(
	const zend_mir_lowering_context *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *fact_out)
{
	return context != NULL && context->value_fact_at != NULL
		&& fact_out != NULL
		&& context->value_fact_at(
			context->value_fact_context, value_id, fact_out)
		&& fact_out->value_id == value_id
		&& zend_mir_scalar_type_is_exact(fact_out->exact_type);
}

bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic)
{
	if (context == NULL || context->current_provider == NULL
			|| status <= ZEND_MIR_LOWERING_SUCCESS
			|| status > ZEND_MIR_LOWERING_FAILED
			|| diagnostic <= ZEND_MIRL_OK
			|| diagnostic >= ZEND_MIRL_DIAGNOSTIC_CODE_COUNT) {
		return false;
	}
	context->provider_status = status;
	context->provider_diagnostic = diagnostic;
	return true;
}
