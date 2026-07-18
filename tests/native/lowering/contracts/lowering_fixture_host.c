#include <string.h>

#include "lowering_fixture_host.h"

#define DEFINE_COUNT_AT(prefix, type, field, count_field) \
	static uint32_t prefix##_count(const void *context) \
	{ \
		return ((const zend_mir_lowering_fixture_source *) context)->count_field; \
	} \
	static bool prefix##_at(const void *context, uint32_t index, type *out) \
	{ \
		const zend_mir_lowering_fixture_source *host = \
			(const zend_mir_lowering_fixture_source *) context; \
		if (out == NULL || index >= host->count_field) { \
			return false; \
		} \
		*out = host->field[index]; \
		return true; \
	}

DEFINE_COUNT_AT(fixture_opcode, zend_mir_source_opcode_ref, opcodes, opcode_count)
DEFINE_COUNT_AT(fixture_ssa, zend_mir_source_ssa_ref, ssa, ssa_count)
DEFINE_COUNT_AT(fixture_use, zend_mir_source_ssa_use_ref, uses, use_count)
DEFINE_COUNT_AT(fixture_def, zend_mir_source_ssa_def_ref, defs, def_count)
DEFINE_COUNT_AT(fixture_literal, zend_mir_source_literal_ref, literals, literal_count)

void zend_mir_lowering_fixture_source_init(zend_mir_lowering_fixture_source *host,
	zend_mir_lowering_source_view *view)
{
	memset(host, 0, sizeof(*host));
	memset(view, 0, sizeof(*view));
	view->contract_version = ZEND_MIR_CONTRACT_VERSION;
	view->context = host;
	view->opcode_count = fixture_opcode_count;
	view->opcode_at = fixture_opcode_at;
	view->ssa_count = fixture_ssa_count;
	view->ssa_at = fixture_ssa_at;
	view->ssa_use_count = fixture_use_count;
	view->ssa_use_at = fixture_use_at;
	view->ssa_def_count = fixture_def_count;
	view->ssa_def_at = fixture_def_at;
	view->literal_count = fixture_literal_count;
	view->literal_at = fixture_literal_at;
}

static uint32_t fixture_fact_count(const void *context)
{
	return ((const zend_mir_lowering_fixture_facts *) context)->count;
}

static bool fixture_fact_at(const void *context, uint32_t index, zend_mir_value_fact_ref *out)
{
	const zend_mir_lowering_fixture_facts *host =
		(const zend_mir_lowering_fixture_facts *) context;
	if (out == NULL || index >= host->count) {
		return false;
	}
	*out = host->facts[index];
	return true;
}

bool zend_mir_lowering_fixture_add_fact(zend_mir_lowering_fixture_facts *host,
	const zend_mir_value_fact_ref *fact, zend_mir_value_fact_id *out)
{
	uint32_t i;
	if (host == NULL || fact == NULL || out == NULL
			|| host->count >= ZEND_MIR_LOWERING_FIXTURE_CAPACITY
			|| !zend_mir_id_is_valid(fact->value_id)
			|| !zend_mir_scalar_type_is_exact(fact->exact_type)
			|| fact->provenance < ZEND_MIR_FACT_PROVENANCE_SSA
			|| fact->provenance >= ZEND_MIR_FACT_PROVENANCE_COUNT
			|| ((fact->flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0
				&& (fact->exact_type != ZEND_MIR_SCALAR_TYPE_I64
					|| fact->integer_min > fact->integer_max))) {
		return false;
	}
	for (i = 0; i < host->count; i++) {
		if (host->facts[i].value_id == fact->value_id) {
			return false;
		}
	}
	host->facts[host->count] = *fact;
	host->facts[host->count].id = host->count;
	*out = host->count++;
	return true;
}

static bool fixture_add_fact_callback(void *context, const zend_mir_value_fact_ref *fact,
	zend_mir_value_fact_id *out)
{
	return zend_mir_lowering_fixture_add_fact(
		(zend_mir_lowering_fixture_facts *) context, fact, out);
}

void zend_mir_lowering_fixture_facts_init(zend_mir_lowering_fixture_facts *host,
	zend_mir_view *view, zend_mir_mutator *mutator)
{
	memset(host, 0, sizeof(*host));
	memset(view, 0, sizeof(*view));
	memset(mutator, 0, sizeof(*mutator));
	view->contract_version = ZEND_MIR_CONTRACT_VERSION;
	view->context = host;
	view->value_fact_count = fixture_fact_count;
	view->value_fact_at = fixture_fact_at;
	mutator->contract_version = ZEND_MIR_CONTRACT_VERSION;
	mutator->context = host;
	mutator->add_value_fact = fixture_add_fact_callback;
}

static bool provider_less(const zend_mir_lowering_provider *left,
	const zend_mir_lowering_provider *right)
{
	return left->semantic_family_id < right->semantic_family_id
		|| (left->semantic_family_id == right->semantic_family_id
			&& left->provider_id < right->provider_id);
}

bool zend_mir_lowering_fixture_registry_add(zend_mir_lowering_fixture_registry *host,
	const zend_mir_lowering_provider *provider, const zend_mir_lowering_claim *claims,
	uint32_t claim_count, zend_mir_lowering_code *code_out)
{
	uint32_t i;
	uint32_t j;
	uint32_t position;
	if (host == NULL || provider == NULL || code_out == NULL
			|| claim_count > ZEND_MIR_LOWERING_FIXTURE_CAPACITY
			|| host->count >= ZEND_MIR_LOWERING_FIXTURE_CAPACITY
			|| (claim_count != 0 && claims == NULL)) {
		if (code_out != NULL) {
			*code_out = ZEND_MIRL_INVALID_SOURCE;
		}
		return false;
	}
	for (i = 0; i < host->count; i++) {
		if (host->providers[i].provider_id == provider->provider_id) {
			*code_out = ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM;
			return false;
		}
		for (j = 0; j < host->claim_counts[i]; j++) {
			uint32_t candidate;
			for (candidate = 0; candidate < claim_count; candidate++) {
				if (host->claims[i][j].zend_opcode_number
						== claims[candidate].zend_opcode_number) {
					*code_out = ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM;
					return false;
				}
			}
		}
	}
	position = host->count;
	while (position != 0 && provider_less(provider, &host->providers[position - 1])) {
		host->providers[position] = host->providers[position - 1];
		host->claim_counts[position] = host->claim_counts[position - 1];
		memcpy(host->claims[position], host->claims[position - 1],
			sizeof(host->claims[position]));
		position--;
	}
	host->providers[position] = *provider;
	host->claim_counts[position] = claim_count;
	if (claim_count != 0) {
		memcpy(host->claims[position], claims, claim_count * sizeof(*claims));
	}
	host->count++;
	*code_out = ZEND_MIRL_OK;
	return true;
}

bool zend_mir_lowering_fixture_registry_at(const zend_mir_lowering_fixture_registry *host,
	uint32_t index, zend_mir_lowering_provider *out)
{
	if (host == NULL || out == NULL || index >= host->count) {
		return false;
	}
	*out = host->providers[index];
	return true;
}
