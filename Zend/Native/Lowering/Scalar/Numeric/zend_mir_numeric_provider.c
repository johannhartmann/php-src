/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <string.h>

#include "zend_mir_lower_numeric.h"

static const uint32_t zend_mir_numeric_arithmetic_opcodes[] = {
	ZEND_MIR_NUMERIC_OPCODE_ADD,
	ZEND_MIR_NUMERIC_OPCODE_SUB,
	ZEND_MIR_NUMERIC_OPCODE_MUL
};

static const uint32_t zend_mir_numeric_integer_opcodes[] = {
	ZEND_MIR_NUMERIC_OPCODE_MOD,
	ZEND_MIR_NUMERIC_OPCODE_SL,
	ZEND_MIR_NUMERIC_OPCODE_SR
};

static const uint32_t zend_mir_numeric_bitwise_opcodes[] = {
	ZEND_MIR_NUMERIC_OPCODE_BW_OR,
	ZEND_MIR_NUMERIC_OPCODE_BW_AND,
	ZEND_MIR_NUMERIC_OPCODE_BW_XOR,
	ZEND_MIR_NUMERIC_OPCODE_BW_NOT
};

static bool zend_mir_numeric_group_claims(
	zend_mir_numeric_provider_group group, const uint32_t **opcodes_out,
	uint32_t *count_out)
{
	if (opcodes_out == NULL || count_out == NULL) {
		return false;
	}
	switch (group) {
		case ZEND_MIR_NUMERIC_PROVIDER_GROUP_ARITHMETIC:
			*opcodes_out = zend_mir_numeric_arithmetic_opcodes;
			*count_out = (uint32_t) (
				sizeof(zend_mir_numeric_arithmetic_opcodes)
				/ sizeof(zend_mir_numeric_arithmetic_opcodes[0]));
			return true;
		case ZEND_MIR_NUMERIC_PROVIDER_GROUP_INTEGER:
			*opcodes_out = zend_mir_numeric_integer_opcodes;
			*count_out = (uint32_t) (
				sizeof(zend_mir_numeric_integer_opcodes)
				/ sizeof(zend_mir_numeric_integer_opcodes[0]));
			return true;
		case ZEND_MIR_NUMERIC_PROVIDER_GROUP_BITWISE:
			*opcodes_out = zend_mir_numeric_bitwise_opcodes;
			*count_out = (uint32_t) (
				sizeof(zend_mir_numeric_bitwise_opcodes)
				/ sizeof(zend_mir_numeric_bitwise_opcodes[0]));
			return true;
		default:
			return false;
	}
}

static bool zend_mir_numeric_group_contains(
	zend_mir_numeric_provider_group group, uint32_t zend_opcode_number)
{
	const uint32_t *opcodes;
	uint32_t count;
	uint32_t index;

	if (!zend_mir_numeric_group_claims(group, &opcodes, &count)) {
		return false;
	}
	for (index = 0; index < count; index++) {
		if (opcodes[index] == zend_opcode_number) {
			return true;
		}
	}
	return false;
}

static uint32_t zend_mir_numeric_claim_count(const void *context)
{
	const zend_mir_numeric_provider_binding *binding =
		(const zend_mir_numeric_provider_binding *) context;
	const uint32_t *opcodes;
	uint32_t count;

	if (binding == NULL
			|| !zend_mir_numeric_group_claims(
				binding->group, &opcodes, &count)) {
		return 0;
	}
	return count;
}

static bool zend_mir_numeric_claim_at(
	const void *context, uint32_t index, zend_mir_lowering_claim *out)
{
	const zend_mir_numeric_provider_binding *binding =
		(const zend_mir_numeric_provider_binding *) context;
	const uint32_t *opcodes;
	uint32_t count;

	if (binding == NULL || out == NULL
			|| !zend_mir_numeric_group_claims(
				binding->group, &opcodes, &count)
			|| index >= count) {
		return false;
	}
	out->zend_opcode_number = opcodes[index];
	out->semantic_family_id = ZEND_MIR_NUMERIC_FAMILY_ID;
	return true;
}

static zend_mir_lowering_status zend_mir_numeric_provider_lower(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator)
{
	const zend_mir_numeric_provider_binding *binding =
		(const zend_mir_numeric_provider_binding *)
			zend_mir_lowering_context_provider_context(context);
	zend_mir_lowering_diagnostic_code diagnostic = ZEND_MIRL_UNKNOWN_PROVIDER;
	zend_mir_lowering_status status;

	if (binding == NULL || binding->numeric == NULL) {
		status = ZEND_MIR_LOWERING_FAILED;
	} else if (source_opcode == NULL
			|| !zend_mir_numeric_group_contains(
				binding->group, source_opcode->zend_opcode_number)) {
		status = ZEND_MIR_LOWERING_DEFERRED;
		diagnostic = ZEND_MIRL_DEFERRED_OPCODE;
	} else {
		status = zend_mir_lower_numeric(
			context, source_opcode, mutator, binding->numeric, &diagnostic);
	}
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		(void) zend_mir_lowering_context_set_provider_failure(
			context, status, diagnostic);
	}
	return status;
}

static void zend_mir_numeric_initialize_provider(
	zend_mir_numeric_provider_set *provider_set, uint32_t index,
	zend_mir_numeric_provider_context *provider_context,
	zend_mir_numeric_provider_group group, uint32_t provider_id)
{
	provider_set->bindings[index].numeric = provider_context;
	provider_set->bindings[index].group = group;
	provider_set->providers[index].provider_id = provider_id;
	provider_set->providers[index].semantic_family_id =
		ZEND_MIR_NUMERIC_FAMILY_ID;
	provider_set->providers[index].context =
		&provider_set->bindings[index];
	provider_set->providers[index].claim_count =
		zend_mir_numeric_claim_count;
	provider_set->providers[index].claim_at = zend_mir_numeric_claim_at;
	provider_set->providers[index].lower = zend_mir_numeric_provider_lower;
}

bool zend_mir_numeric_provider_set_init(
	zend_mir_numeric_provider_context *provider_context,
	zend_mir_numeric_provider_set *provider_set)
{
	if (provider_context == NULL || provider_set == NULL
			|| provider_context->source == NULL
			|| provider_context->source->contract_version
				!= ZEND_MIR_CONTRACT_VERSION
			|| provider_context->source->ssa_use_count == NULL
			|| provider_context->source->ssa_use_at == NULL
			|| provider_context->source->ssa_def_count == NULL
			|| provider_context->source->ssa_def_at == NULL
			|| provider_context->source_context == NULL
			|| provider_context->resolve_operand == NULL
			|| provider_context->value_fact == NULL
			|| provider_context->source_position == NULL) {
		return false;
	}
	memset(provider_set, 0, sizeof(*provider_set));
	zend_mir_numeric_initialize_provider(
		provider_set, 0, provider_context,
		ZEND_MIR_NUMERIC_PROVIDER_GROUP_ARITHMETIC,
		ZEND_MIR_NUMERIC_ARITHMETIC_PROVIDER_ID);
	zend_mir_numeric_initialize_provider(
		provider_set, 1, provider_context,
		ZEND_MIR_NUMERIC_PROVIDER_GROUP_INTEGER,
		ZEND_MIR_NUMERIC_INTEGER_PROVIDER_ID);
	zend_mir_numeric_initialize_provider(
		provider_set, 2, provider_context,
		ZEND_MIR_NUMERIC_PROVIDER_GROUP_BITWISE,
		ZEND_MIR_NUMERIC_BITWISE_PROVIDER_ID);
	return true;
}

uint32_t zend_mir_numeric_provider_count(
	const zend_mir_numeric_provider_set *provider_set)
{
	return provider_set != NULL ? ZEND_MIR_NUMERIC_PROVIDER_COUNT : 0;
}

bool zend_mir_numeric_provider_at(
	const zend_mir_numeric_provider_set *provider_set, uint32_t index,
	zend_mir_lowering_provider *provider_out)
{
	if (provider_set == NULL || provider_out == NULL
			|| index >= ZEND_MIR_NUMERIC_PROVIDER_COUNT) {
		return false;
	}
	*provider_out = provider_set->providers[index];
	return true;
}
