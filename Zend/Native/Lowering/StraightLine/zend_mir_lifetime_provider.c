/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include "zend_mir_straight_line.h"

static uint32_t zend_mir_lifetime_claim_count(const void *context)
{
	return context != NULL ? 3 : 0;
}

static bool zend_mir_lifetime_claim_at(
	const void *context, uint32_t index, zend_mir_lowering_claim *out)
{
	static const uint32_t opcodes[] = {
		ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN,
		ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN,
		ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE
	};

	if (context == NULL || out == NULL
			|| index >= sizeof(opcodes) / sizeof(opcodes[0])) {
		return false;
	}
	out->zend_opcode_number = opcodes[index];
	out->semantic_family_id = ZEND_MIR_STRAIGHT_LINE_FAMILY_ID;
	return true;
}

static zend_mir_lowering_status zend_mir_lifetime_lower(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator)
{
	zend_mir_straight_line_provider_context *provider_context =
		(zend_mir_straight_line_provider_context *)
			zend_mir_lowering_context_provider_context(context);
	zend_mir_lowering_diagnostic_code diagnostic = ZEND_MIRL_INVALID_SOURCE;
	zend_mir_lowering_status status;

	if (provider_context == NULL || source_opcode == NULL) {
		status = ZEND_MIR_LOWERING_FAILED;
		diagnostic = ZEND_MIRL_UNKNOWN_PROVIDER;
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN) {
		status = zend_mir_lower_copy_move(
			context, source_opcode, mutator, provider_context, &diagnostic);
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN) {
		status = zend_mir_lower_return(
			context, source_opcode, mutator, provider_context, &diagnostic);
	} else if (source_opcode->zend_opcode_number
			== ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE) {
		status = zend_mir_lower_free(
			context, source_opcode, mutator, provider_context, &diagnostic);
	} else {
		status = ZEND_MIR_LOWERING_DEFERRED;
		diagnostic = ZEND_MIRL_DEFERRED_OPCODE;
	}
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		(void) zend_mir_lowering_context_set_provider_failure(
			context, status, diagnostic);
	}
	return status;
}

bool zend_mir_lifetime_provider_init(
	zend_mir_straight_line_provider_context *provider_context,
	zend_mir_lowering_provider *provider_out)
{
	if (provider_context == NULL || provider_out == NULL
			|| provider_context->source == NULL
			|| provider_context->source->contract_version
				!= ZEND_MIR_CONTRACT_VERSION
			|| provider_context->lifetime == NULL
			|| provider_context->lifetime->values == NULL
			|| provider_context->lifetime->capacity == 0
			|| provider_context->lifetime->count
				> provider_context->lifetime->capacity
			|| provider_context->entry == NULL
			|| provider_context->entry->source_position_at == NULL) {
		return false;
	}
	provider_out->provider_id = ZEND_MIR_STRAIGHT_LINE_PROVIDER_ID;
	provider_out->semantic_family_id = ZEND_MIR_STRAIGHT_LINE_FAMILY_ID;
	provider_out->context = provider_context;
	provider_out->claim_count = zend_mir_lifetime_claim_count;
	provider_out->claim_at = zend_mir_lifetime_claim_at;
	provider_out->lower = zend_mir_lifetime_lower;
	return true;
}
