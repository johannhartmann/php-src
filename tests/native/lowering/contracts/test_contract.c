#include <assert.h>
#include <string.h>

#include "lowering_fixture_host.h"

int main(void)
{
	zend_mir_lowering_fixture_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_fixture_facts facts;
	zend_mir_view view;
	zend_mir_mutator mutator;
	zend_mir_value_fact_ref fact;
	zend_mir_value_fact_ref read_back;
	zend_mir_value_fact_id fact_id;
	zend_mir_lowering_fixture_registry registry;
	zend_mir_lowering_provider provider_a = {2, 20, NULL, NULL, NULL, NULL};
	zend_mir_lowering_provider provider_b = {1, 10, NULL, NULL, NULL, NULL};
	zend_mir_lowering_provider provider_duplicate = {3, 30, NULL, NULL, NULL, NULL};
	zend_mir_lowering_provider read_provider;
	zend_mir_lowering_claim claim_a = {1, 20};
	zend_mir_lowering_claim claim_b = {2, 10};
	zend_mir_lowering_claim claim_duplicate = {1, 30};
	zend_mir_lowering_code code;
	zend_mir_lowering_result result;

	assert(ZEND_MIR_CONTRACT_VERSION_MAJOR == 1);
	assert(ZEND_MIR_CONTRACT_VERSION_MINOR == 2);
	assert(ZEND_MIR_OPCODE_CONSTANT == 0);
	assert(ZEND_MIR_OPCODE_UNREACHABLE == 9);
	assert(ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW == 10);
	assert(ZEND_MIR_OPCODE_SCALAR_DROP == 40);

	memset(&registry, 0, sizeof(registry));
	zend_mir_lowering_fixture_source_init(&source, &source_view);
	source.opcodes[0].zend_opcode_number = 0;
	source.opcode_count = 1;
	assert(source_view.opcode_count(source_view.context) == 1);
	assert(source_view.opcode_at(source_view.context, 0, &source.opcodes[1]));
	assert(source.opcodes[1].zend_opcode_number == 0);

	zend_mir_lowering_fixture_facts_init(&facts, &view, &mutator);
	memset(&fact, 0, sizeof(fact));
	fact.value_id = 7;
	fact.exact_type = ZEND_MIR_SCALAR_TYPE_I64;
	fact.flags = ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	fact.integer_min = -10;
	fact.integer_max = 10;
	fact.provenance = ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS;
	fact.provenance_source_position_id = ZEND_MIR_ID_INVALID;
	assert(mutator.add_value_fact(mutator.context, &fact, &fact_id));
	assert(fact_id == 0);
	assert(view.value_fact_count(view.context) == 1);
	assert(view.value_fact_at(view.context, 0, &read_back));
	assert(read_back.value_id == 7);
	assert(!mutator.add_value_fact(mutator.context, &fact, &fact_id));
	fact.value_id = 8;
	fact.exact_type = ZEND_MIR_SCALAR_TYPE_I64 | ZEND_MIR_SCALAR_TYPE_I1;
	assert(!mutator.add_value_fact(mutator.context, &fact, &fact_id));

	assert(zend_mir_lowering_fixture_registry_add(
		&registry, &provider_a, &claim_a, 1, &code));
	assert(zend_mir_lowering_fixture_registry_add(
		&registry, &provider_b, &claim_b, 1, &code));
	assert(zend_mir_lowering_fixture_registry_at(&registry, 0, &read_provider));
	assert(read_provider.provider_id == 1);
	assert(!zend_mir_lowering_fixture_registry_add(
		&registry, &provider_duplicate, &claim_duplicate, 1, &code));
	assert(code == ZEND_MIRL_DUPLICATE_PROVIDER_CLAIM);

	result.status = ZEND_MIR_LOWERING_FAILED;
	result.diagnostic_code = ZEND_MIRL_MUTATION_FAILED;
	result.guarantees = 0;
	result.module = NULL;
	assert(zend_mir_lowering_result_is_failure_atomic(&result));
	result.status = ZEND_MIR_LOWERING_SUCCESS;
	result.diagnostic_code = ZEND_MIRL_OK;
	result.guarantees = ZEND_MIR_LOWERING_GUARANTEE_ALL;
	result.module = (zend_mir_module *) &result;
	assert(zend_mir_lowering_result_is_failure_atomic(&result));
	result.guarantees = 0;
	assert(!zend_mir_lowering_result_is_failure_atomic(&result));
	return 0;
}
