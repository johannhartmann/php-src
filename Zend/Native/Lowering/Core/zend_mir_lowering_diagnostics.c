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

#include <stdio.h>
#include <string.h>

#include "zend_mir_lowering_internal.h"

const char *zend_mir_lowering_diagnostic_token(
	zend_mir_lowering_diagnostic_code code)
{
	switch (code) {
		case ZEND_MIRL_OK:
			return ZEND_MIRL_TOKEN_OK;
		case ZEND_MIRL_DEFERRED_OPCODE:
			return ZEND_MIRL_TOKEN_DEFERRED_OPCODE;
		case ZEND_MIRL_MISSING_PROOF:
			return ZEND_MIRL_TOKEN_MISSING_PROOF;
		case ZEND_MIRL_CONTRADICTORY_FACT:
			return ZEND_MIRL_TOKEN_CONTRADICTORY_FACT;
		case ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM:
			return ZEND_MIRL_TOKEN_DUPLICATE_PROVIDER_CLAIM;
		case ZEND_MIRL_UNKNOWN_PROVIDER:
			return ZEND_MIRL_TOKEN_UNKNOWN_PROVIDER;
		case ZEND_MIRL_INVALID_SOURCE:
			return ZEND_MIRL_TOKEN_INVALID_SOURCE;
		case ZEND_MIRL_MUTATION_FAILED:
			return ZEND_MIRL_TOKEN_MUTATION_FAILED;
		case ZEND_MIRL_FINALIZE_FAILED:
			return ZEND_MIRL_TOKEN_FINALIZE_FAILED;
		case ZEND_MIRL_STAGE1_VERIFY_FAILED:
			return ZEND_MIRL_TOKEN_STAGE1_VERIFY_FAILED;
		case ZEND_MIRL_STAGE2_VERIFY_FAILED:
			return ZEND_MIRL_TOKEN_STAGE2_VERIFY_FAILED;
		case ZEND_MIRL_W04_CONTROL_FLOW_DEFERRED:
			return ZEND_MIRL_TOKEN_W04_CONTROL_FLOW_DEFERRED;
		case ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED:
			return ZEND_MIRL_TOKEN_W05_RUNTIME_EFFECT_DEFERRED;
		case ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED:
			return ZEND_MIRL_TOKEN_W06_REFERENCE_SEMANTICS_DEFERRED;
		default:
			return "[MIRL????]";
	}
}

static zend_mir_diagnostic_code zend_mir_lowering_mir_code(
	zend_mir_lowering_diagnostic_code code)
{
	switch (code) {
		case ZEND_MIRL_MUTATION_FAILED:
			return ZEND_MIR_DIAGNOSTIC_ALLOCATION_FAILED;
		case ZEND_MIRL_FINALIZE_FAILED:
		case ZEND_MIRL_STAGE1_VERIFY_FAILED:
		case ZEND_MIRL_STAGE2_VERIFY_FAILED:
			return ZEND_MIR_DIAGNOSTIC_INVALID_CFG;
		case ZEND_MIRL_INVALID_SOURCE:
		case ZEND_MIRL_CONTRADICTORY_FACT:
			return ZEND_MIR_DIAGNOSTIC_INVALID_VALUE_FACT;
		default:
			return ZEND_MIR_DIAGNOSTIC_UNMODELED_SEMANTICS;
	}
}

bool zend_mir_lowering_emit_diagnostic(zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code code,
	const zend_mir_source_opcode_ref *source_opcode,
	const char *detail)
{
	zend_mir_diagnostic diagnostic;
	uint32_t opline = ZEND_MIR_ID_INVALID;
	uint32_t opcode = ZEND_MIR_ID_INVALID;

	if (context == NULL || code <= ZEND_MIRL_OK
			|| code >= ZEND_MIRL_DIAGNOSTIC_CODE_COUNT) {
		return false;
	}
	if (source_opcode != NULL) {
		opline = source_opcode->opline_index;
		opcode = source_opcode->zend_opcode_number;
		if (context->has_last_diagnostic_opline
				&& opline < context->last_diagnostic_opline) {
			return false;
		}
		context->last_diagnostic_opline = opline;
		context->has_last_diagnostic_opline = true;
	}

	memset(&diagnostic, 0, sizeof(diagnostic));
	diagnostic.code = zend_mir_lowering_mir_code(code);
	diagnostic.severity = status == ZEND_MIR_LOWERING_FAILED
		? ZEND_MIR_DIAGNOSTIC_FATAL : ZEND_MIR_DIAGNOSTIC_ERROR;
	diagnostic.location.module_id = context->module_id;
	diagnostic.location.function_id = context->function_id;
	diagnostic.location.block_id = context->block_id;
	diagnostic.location.instruction_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.frame_state_id = ZEND_MIR_ID_INVALID;
	diagnostic.location.source_position_id = source_opcode != NULL
		? source_opcode->source_position_id : ZEND_MIR_ID_INVALID;
	(void) snprintf(diagnostic.message, sizeof(diagnostic.message),
		"%s opline=%u opcode=%u %s",
		zend_mir_lowering_diagnostic_token(code), opline, opcode,
		detail != NULL ? detail : "lowering failed");
	return zend_mir_diagnostic_sink_emit(context->diagnostics, &diagnostic);
}
