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

typedef struct _zend_mir_lowering_preflight_result {
	bool ok;
	zend_mir_lowering_status status;
	zend_mir_lowering_diagnostic_code code;
	zend_mir_source_opcode_ref source_opcode;
	bool has_source_opcode;
	const char *detail;
} zend_mir_lowering_preflight_result;

static zend_mir_lowering_result zend_mir_lowering_result_make(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code)
{
	zend_mir_lowering_result result;

	result.status = status;
	result.diagnostic_code = code;
	result.guarantees = 0;
	result.module = NULL;
	return result;
}

static zend_mir_lowering_preflight_result zend_mir_lowering_preflight_fail(
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code,
	const zend_mir_source_opcode_ref *source_opcode,
	const char *detail)
{
	zend_mir_lowering_preflight_result result;

	memset(&result, 0, sizeof(result));
	result.status = status;
	result.code = code;
	result.detail = detail;
	if (source_opcode != NULL) {
		result.source_opcode = *source_opcode;
		result.has_source_opcode = true;
	}
	return result;
}

static bool zend_mir_lowering_source_opline_exists(
	const zend_mir_lowering_source_view *source,
	uint32_t opcode_count, uint32_t opline_index,
	zend_mir_source_opcode_ref *out)
{
	uint32_t index;

	for (index = 0; index < opcode_count; index++) {
		zend_mir_source_opcode_ref opcode;

		if (!source->opcode_at(source->context, index, &opcode)) {
			return false;
		}
		if (opcode.opline_index == opline_index) {
			if (out != NULL) {
				*out = opcode;
			}
			return true;
		}
	}
	return false;
}

static bool zend_mir_lowering_ssa_exists(
	const zend_mir_lowering_source_view *source,
	uint32_t ssa_count, uint32_t ssa_variable_id)
{
	uint32_t index;

	for (index = 0; index < ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;

		if (!source->ssa_at(source->context, index, &ssa)) {
			return false;
		}
		if (ssa.ssa_variable_id == ssa_variable_id) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_lowering_literal_exists(
	const zend_mir_lowering_source_view *source,
	uint32_t literal_count, uint32_t literal_index)
{
	uint32_t index;

	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;

		if (!source->literal_at(source->context, index, &literal)) {
			return false;
		}
		if (literal.literal_index == literal_index) {
			return true;
		}
	}
	return false;
}

static bool zend_mir_lowering_operand_is_valid(
	const zend_mir_lowering_source_view *source,
	const zend_mir_source_operand_ref *operand,
	uint32_t ssa_count, uint32_t literal_count)
{
	switch (operand->kind) {
		case ZEND_MIR_SOURCE_OPERAND_UNUSED:
			return true;
		case ZEND_MIR_SOURCE_OPERAND_LITERAL:
			return zend_mir_lowering_literal_exists(
				source, literal_count, operand->index);
		case ZEND_MIR_SOURCE_OPERAND_SLOT:
			return operand->slot_kind >= ZEND_MIR_SOURCE_SLOT_CV
				&& operand->slot_kind <= ZEND_MIR_SOURCE_SLOT_VAR;
		case ZEND_MIR_SOURCE_OPERAND_SSA:
			return operand->ssa_variable_id <= ZEND_MIR_VALUE_ORIGINAL_MAX
				&& zend_mir_lowering_ssa_exists(
					source, ssa_count, operand->ssa_variable_id);
		default:
			return false;
	}
}

static const zend_mir_source_operand_ref *zend_mir_lowering_operand_at(
	const zend_mir_source_opcode_ref *opcode, uint32_t operand_index)
{
	switch (operand_index) {
		case 0:
			return &opcode->op1;
		case 1:
			return &opcode->op2;
		case 2:
			return &opcode->result;
		default:
			return NULL;
	}
}

static zend_mir_lowering_preflight_result zend_mir_lowering_preflight(
	zend_mir_lowering_context *context)
{
	const zend_mir_lowering_source_view *source = context->source;
	uint32_t opcode_count;
	uint32_t ssa_count;
	uint32_t use_count;
	uint32_t def_count;
	uint32_t literal_count;
	uint32_t index;
	uint32_t previous_opline = 0;
	bool has_previous_opline = false;

	if (context->shape.reachable_block_count != 1
			|| context->shape.has_control_flow
			|| context->shape.has_try_regions) {
		return zend_mir_lowering_preflight_fail(
			ZEND_MIR_LOWERING_DEFERRED,
			ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED, NULL,
			"source is outside the single reachable block slice");
	}
	if (context->shape.has_calls) {
		return zend_mir_lowering_preflight_fail(
			ZEND_MIR_LOWERING_DEFERRED,
			ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED, NULL,
			"source contains a runtime call boundary");
	}
	if (!context->shape.ssa_complete || source == NULL
			|| source->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| source->opcode_count == NULL || source->opcode_at == NULL
			|| source->ssa_count == NULL || source->ssa_at == NULL
			|| source->ssa_use_count == NULL || source->ssa_use_at == NULL
			|| source->ssa_def_count == NULL || source->ssa_def_at == NULL
			|| source->literal_count == NULL || source->literal_at == NULL
			|| context->registry == NULL || !context->registry->complete) {
		return zend_mir_lowering_preflight_fail(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
			"source view or registry contract is incomplete");
	}

	opcode_count = source->opcode_count(source->context);
	ssa_count = source->ssa_count(source->context);
	use_count = source->ssa_use_count(source->context);
	def_count = source->ssa_def_count(source->context);
	literal_count = source->literal_count(source->context);
	if (opcode_count == 0 || opcode_count > context->limits.max_opcodes
			|| ssa_count > context->limits.max_ssa_variables
			|| use_count > context->limits.max_ssa_uses
			|| def_count > context->limits.max_ssa_defs
			|| literal_count > context->limits.max_literals) {
		return zend_mir_lowering_preflight_fail(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
			"source view exceeds a configured bound");
	}

	for (index = 0; index < literal_count; index++) {
		zend_mir_source_literal_ref literal;

		if (!source->literal_at(source->context, index, &literal)
				|| literal.literal_index != index
				|| literal.kind < ZEND_MIR_SOURCE_LITERAL_NULL
				|| literal.kind > ZEND_MIR_SOURCE_LITERAL_DOUBLE_BITS) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
				"literal table is malformed");
		}
	}

	for (index = 0; index < ssa_count; index++) {
		zend_mir_source_ssa_ref ssa;
		uint32_t previous;

		if (!source->ssa_at(source->context, index, &ssa)
				|| ssa.ssa_variable_id > ZEND_MIR_VALUE_ORIGINAL_MAX
				|| ssa.source_slot_kind < ZEND_MIR_SOURCE_SLOT_CV
				|| ssa.source_slot_kind > ZEND_MIR_SOURCE_SLOT_VAR
				|| (ssa.definition_opline_index != ZEND_MIR_ID_INVALID
					&& !zend_mir_lowering_source_opline_exists(
						source, opcode_count, ssa.definition_opline_index, NULL))) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
				"SSA variable table is malformed");
		}
		for (previous = 0; previous < index; previous++) {
			zend_mir_source_ssa_ref prior;

			if (!source->ssa_at(source->context, previous, &prior)
					|| prior.ssa_variable_id == ssa.ssa_variable_id) {
				return zend_mir_lowering_preflight_fail(
					ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
					"SSA variable IDs are not unique");
			}
		}
	}

	for (index = 0; index < opcode_count; index++) {
		zend_mir_source_opcode_ref opcode;
		const zend_mir_lowering_profile_entry *profile_entry;
		zend_mir_lowering_diagnostic_code deferred_code;
		bool opcode_available =
			source->opcode_at(source->context, index, &opcode);

		if (!opcode_available
				|| (has_previous_opline && opcode.opline_index <= previous_opline)
				|| !zend_mir_lowering_operand_is_valid(
					source, &opcode.op1, ssa_count, literal_count)
				|| !zend_mir_lowering_operand_is_valid(
					source, &opcode.op2, ssa_count, literal_count)
				|| !zend_mir_lowering_operand_is_valid(
					source, &opcode.result, ssa_count, literal_count)) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE,
				opcode_available ? &opcode : NULL,
				"opcode sequence or operand reference is malformed");
		}
		has_previous_opline = true;
		previous_opline = opcode.opline_index;
		profile_entry = zend_mir_lowering_profile_find(
			context->registry->profile, opcode.zend_opcode_number);
		if (profile_entry == NULL
				|| profile_entry->disposition == ZEND_MIR_LOWERING_PROFILE_REJECTED) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_UNKNOWN_PROVIDER,
				&opcode, "opcode is outside the frozen W03 profile");
		}
		if (profile_entry->disposition != ZEND_MIR_LOWERING_PROFILE_ACCEPTED) {
			switch (profile_entry->disposition) {
				case ZEND_MIR_LOWERING_PROFILE_DEFERRED_W04:
					deferred_code = ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED;
					break;
				case ZEND_MIR_LOWERING_PROFILE_DEFERRED_W05:
					deferred_code = ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED;
					break;
				case ZEND_MIR_LOWERING_PROFILE_DEFERRED_W06:
					deferred_code = ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED;
					break;
				default:
					deferred_code = ZEND_MIRL_DEFERRED_OPCODE;
					break;
			}
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_DEFERRED, deferred_code, &opcode,
				"opcode is assigned to a later lowering wave");
		}
		if (zend_mir_lowering_registry_find(
				context->registry, opcode.zend_opcode_number) == NULL) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_UNKNOWN_PROVIDER,
				&opcode, "accepted opcode has no unique provider");
		}
	}

	for (index = 0; index < use_count; index++) {
		zend_mir_source_ssa_use_ref use;
		zend_mir_source_opcode_ref opcode;
		const zend_mir_source_operand_ref *operand;

		if (!source->ssa_use_at(source->context, index, &use)
				|| !zend_mir_lowering_ssa_exists(
					source, ssa_count, use.ssa_variable_id)
				|| !zend_mir_lowering_source_opline_exists(
					source, opcode_count, use.opline_index, &opcode)
				|| (operand = zend_mir_lowering_operand_at(
					&opcode, use.operand_index)) == NULL
				|| operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA
				|| operand->ssa_variable_id != use.ssa_variable_id) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
				"SSA use table is malformed");
		}
	}

	for (index = 0; index < def_count; index++) {
		zend_mir_source_ssa_def_ref definition;
		zend_mir_source_opcode_ref opcode;
		uint32_t previous;

		if (!source->ssa_def_at(source->context, index, &definition)
				|| !zend_mir_lowering_ssa_exists(
					source, ssa_count, definition.ssa_variable_id)
				|| !zend_mir_lowering_source_opline_exists(
					source, opcode_count, definition.opline_index, &opcode)
				|| definition.destination.kind != ZEND_MIR_SOURCE_OPERAND_SSA
				|| definition.destination.ssa_variable_id
					!= definition.ssa_variable_id) {
			return zend_mir_lowering_preflight_fail(
				ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
				"SSA definition table is malformed");
		}
		for (previous = 0; previous < index; previous++) {
			zend_mir_source_ssa_def_ref prior;

			if (!source->ssa_def_at(source->context, previous, &prior)
					|| prior.ssa_variable_id == definition.ssa_variable_id) {
				return zend_mir_lowering_preflight_fail(
					ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE, NULL,
					"SSA definitions are not unique");
			}
		}
	}

	{
		zend_mir_lowering_preflight_result result;
		memset(&result, 0, sizeof(result));
		result.ok = true;
		result.status = ZEND_MIR_LOWERING_SUCCESS;
		result.code = ZEND_MIRL_OK;
		return result;
	}
}

static zend_mir_lowering_result zend_mir_lowering_abort(
	zend_mir_lowering_context *context,
	zend_mir_module *module,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code,
	const zend_mir_source_opcode_ref *source_opcode,
	const char *detail)
{
	if (module != NULL) {
		context->module_ops.destroy(context->module_ops.context, module);
	}
	(void) zend_mir_lowering_emit_diagnostic(
		context, status, code, source_opcode, detail);
	context->current_provider = NULL;
	context->current_opcode = NULL;
	context->busy = false;
	return zend_mir_lowering_result_make(status, code);
}

zend_mir_lowering_result zend_mir_lower_source(
	zend_mir_lowering_context *context, zend_mir_mutator *requested_mutator)
{
	zend_mir_lowering_preflight_result preflight;
	zend_mir_module *module;
	zend_mir_mutator *mutator;
	const zend_mir_view *view;
	zend_mir_source_opcode_ref current_opcode;
	zend_mir_source_opcode_ref last_opcode;
	uint32_t opcode_count;
	uint32_t index;
	bool has_last_opcode = false;

	if (context == NULL || context->busy) {
		return zend_mir_lowering_result_make(
			ZEND_MIR_LOWERING_REJECTED, ZEND_MIRL_INVALID_SOURCE);
	}
	context->busy = true;
	context->function_id = ZEND_MIR_ID_INVALID;
	context->block_id = ZEND_MIR_ID_INVALID;
	context->current_provider = NULL;
	context->current_opcode = NULL;
	context->has_last_diagnostic_opline = false;

	preflight = zend_mir_lowering_preflight(context);
	if (!preflight.ok) {
		if (!preflight.has_source_opcode
				&& context->source != NULL
				&& context->source->opcode_count != NULL
				&& context->source->opcode_at != NULL
				&& context->source->opcode_count(context->source->context) != 0
				&& context->source->opcode_at(
					context->source->context, 0, &preflight.source_opcode)) {
			preflight.has_source_opcode = true;
		}
		return zend_mir_lowering_abort(
			context, NULL, preflight.status, preflight.code,
			preflight.has_source_opcode ? &preflight.source_opcode : NULL,
			preflight.detail);
	}
	opcode_count = context->source->opcode_count(context->source->context);
	if (!context->source->opcode_at(
			context->source->context, 0, &last_opcode)) {
		return zend_mir_lowering_abort(
			context, NULL, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_INVALID_SOURCE, NULL,
			"source changed after successful preflight");
	}
	has_last_opcode = true;

	module = context->module_ops.create(
		context->module_ops.context, context->module_id, context->diagnostics);
	if (module == NULL) {
		return zend_mir_lowering_abort(
			context, NULL, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_MUTATION_FAILED, &last_opcode, "module allocation failed");
	}
	mutator = context->module_ops.mutator(context->module_ops.context, module);
	if (mutator == NULL || (requested_mutator != NULL && requested_mutator != mutator)
			|| mutator->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| mutator->add_function == NULL || mutator->add_block == NULL
			|| mutator->set_entry_block == NULL || mutator->seal_function == NULL) {
		return zend_mir_lowering_abort(
			context, module, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_MUTATION_FAILED, &last_opcode,
			"module did not expose the requested current-contract mutator");
	}
	if (!mutator->add_function(
			mutator->context, context->function_symbol_id, &context->function_id)
			|| !zend_mir_id_is_valid(context->function_id)
			|| !mutator->add_block(
				mutator->context, context->function_id, &context->block_id)
			|| !zend_mir_id_is_valid(context->block_id)
			|| !mutator->set_entry_block(
				mutator->context, context->function_id, context->block_id)) {
		return zend_mir_lowering_abort(
			context, module, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_MUTATION_FAILED, &last_opcode,
			"initial function or entry block mutation failed");
	}

	for (index = 0; index < opcode_count; index++) {
		const zend_mir_lowering_provider *provider;
		zend_mir_lowering_status provider_status;
		zend_mir_lowering_diagnostic_code provider_code;

		if (!context->source->opcode_at(
				context->source->context, index, &current_opcode)) {
			return zend_mir_lowering_abort(
				context, module, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_MUTATION_FAILED, &last_opcode,
				"source changed after successful preflight");
		}
		provider = zend_mir_lowering_registry_find(
			context->registry, current_opcode.zend_opcode_number);
		if (provider == NULL) {
			return zend_mir_lowering_abort(
				context, module, ZEND_MIR_LOWERING_FAILED,
				ZEND_MIRL_UNKNOWN_PROVIDER, &current_opcode,
				"registry changed after successful preflight");
		}
		context->current_provider = provider;
		context->current_opcode = &current_opcode;
		context->provider_status = ZEND_MIR_LOWERING_SUCCESS;
		context->provider_diagnostic = ZEND_MIRL_OK;
		provider_status = provider->lower(context, &current_opcode, mutator);
		provider_code = context->provider_diagnostic;
		if (context->provider_status != ZEND_MIR_LOWERING_SUCCESS) {
			provider_status = context->provider_status;
		}
		if (provider_status != ZEND_MIR_LOWERING_SUCCESS) {
			if (provider_status <= ZEND_MIR_LOWERING_SUCCESS
					|| provider_status > ZEND_MIR_LOWERING_FAILED) {
				provider_status = ZEND_MIR_LOWERING_FAILED;
			}
			if (provider_code == ZEND_MIRL_OK) {
				provider_code = provider_status == ZEND_MIR_LOWERING_DEFERRED
					? ZEND_MIRL_DEFERRED_OPCODE
					: provider_status == ZEND_MIR_LOWERING_REJECTED
						? ZEND_MIRL_MISSING_PROOF
						: ZEND_MIRL_MUTATION_FAILED;
			}
			return zend_mir_lowering_abort(
				context, module, provider_status, provider_code,
				&current_opcode, "opcode provider did not complete");
		}
		last_opcode = current_opcode;
		has_last_opcode = true;
	}
	context->current_provider = NULL;
	context->current_opcode = NULL;

	if (!mutator->seal_function(mutator->context, context->function_id)) {
		return zend_mir_lowering_abort(
			context, module, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_MUTATION_FAILED,
			has_last_opcode ? &last_opcode : NULL,
			"function sealing failed");
	}
	if (!context->module_ops.finalize(context->module_ops.context, module)) {
		return zend_mir_lowering_abort(
			context, module, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_FINALIZE_FAILED,
			has_last_opcode ? &last_opcode : NULL,
			"module finalization failed");
	}
	view = context->module_ops.view(context->module_ops.context, module);
	if (view == NULL || view->contract_version != ZEND_MIR_CONTRACT_VERSION
			|| !context->module_ops.verify_stage1(
				context->module_ops.context, view, context->diagnostics)) {
		return zend_mir_lowering_abort(
			context, module, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_STAGE1_VERIFY_FAILED,
			has_last_opcode ? &last_opcode : NULL,
			"stage-1 verification failed");
	}
	if (!context->module_ops.verify_stage2(
			context->module_ops.context, view, context->diagnostics)) {
		return zend_mir_lowering_abort(
			context, module, ZEND_MIR_LOWERING_FAILED,
			ZEND_MIRL_STAGE2_VERIFY_FAILED,
			has_last_opcode ? &last_opcode : NULL,
			"stage-2 verification failed");
	}

	context->busy = false;
	{
		zend_mir_lowering_result result =
			zend_mir_lowering_result_make(ZEND_MIR_LOWERING_SUCCESS, ZEND_MIRL_OK);
		result.guarantees = ZEND_MIR_LOWERING_GUARANTEE_ALL;
		result.module = module;
		return result;
	}
}
