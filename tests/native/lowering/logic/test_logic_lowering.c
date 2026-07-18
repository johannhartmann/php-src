/*
   +----------------------------------------------------------------------+
   | Copyright (c) The PHP Group                                          |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license.      |
   +----------------------------------------------------------------------+
*/

#include <assert.h>
#include <limits.h>
#include <string.h>

#include "Zend/Native/Lowering/Scalar/Logic/zend_mir_logic.h"

#define TEST_CAPACITY 32

struct _zend_mir_lowering_context {
	const void *provider_context;
	zend_mir_block_id block_id;
	zend_mir_lowering_status status;
	zend_mir_lowering_diagnostic_code diagnostic;
	uint32_t failure_count;
};

typedef struct _test_operand {
	zend_mir_instruction_id instruction_id;
	zend_mir_value_id value_id;
} test_operand;

typedef struct _test_host {
	zend_mir_value_record values[TEST_CAPACITY];
	zend_mir_instruction_record instructions[TEST_CAPACITY];
	zend_mir_value_fact_ref facts[TEST_CAPACITY];
	test_operand operands[TEST_CAPACITY * 2];
	uint32_t value_count;
	uint32_t instruction_count;
	uint32_t fact_count;
	uint32_t operand_count;
	uint32_t mutation_attempts;
	uint32_t fail_at_attempt;
	zend_mir_mutator mutator;
} test_host;

typedef struct _test_fixture {
	zend_mir_logic_value_binding bindings[3];
	zend_mir_logic_opcode_proof proof;
	zend_mir_logic_context logic;
	zend_mir_lowering_provider provider;
	zend_mir_lowering_context lowering;
	zend_mir_source_opcode_ref source;
	test_host host;
} test_fixture;

const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context)
{
	return context != NULL ? context->provider_context : NULL;
}

zend_mir_block_id zend_mir_lowering_context_block_id(
	const zend_mir_lowering_context *context)
{
	return context != NULL ? context->block_id : ZEND_MIR_ID_INVALID;
}

bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context,
	zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic)
{
	if (context == NULL || status <= ZEND_MIR_LOWERING_SUCCESS
			|| status > ZEND_MIR_LOWERING_FAILED
			|| diagnostic <= ZEND_MIRL_OK
			|| diagnostic >= ZEND_MIRL_DIAGNOSTIC_CODE_COUNT) {
		return false;
	}
	context->status = status;
	context->diagnostic = diagnostic;
	context->failure_count++;
	return true;
}

static bool test_mutation_allowed(test_host *host)
{
	host->mutation_attempts++;
	return host->fail_at_attempt == 0
		|| host->mutation_attempts != host->fail_at_attempt;
}

static bool test_has_value(const test_host *host, zend_mir_value_id id)
{
	uint32_t index;

	for (index = 0; index < host->value_count; index++) {
		if (host->values[index].id == id) {
			return true;
		}
	}
	return false;
}

static bool test_add_value(
	void *context,
	zend_mir_value_id requested_id,
	zend_mir_representation representation,
	zend_mir_ownership_state ownership)
{
	test_host *host = (test_host *) context;
	zend_mir_value_record *value;

	if (!test_mutation_allowed(host)
			|| host->value_count >= TEST_CAPACITY
			|| !zend_mir_id_is_valid(requested_id)
			|| test_has_value(host, requested_id)) {
		return false;
	}
	value = &host->values[host->value_count++];
	value->id = requested_id;
	value->representation = representation;
	value->ownership = ownership;
	return true;
}

static bool test_add_instruction(
	void *context,
	const zend_mir_instruction_record *requested,
	zend_mir_instruction_id *out)
{
	test_host *host = (test_host *) context;
	zend_mir_instruction_record *instruction;

	if (!test_mutation_allowed(host) || requested == NULL || out == NULL
			|| host->instruction_count >= TEST_CAPACITY
			|| !test_has_value(host, requested->result_id)) {
		return false;
	}
	instruction = &host->instructions[host->instruction_count];
	*instruction = *requested;
	instruction->id = host->instruction_count;
	*out = host->instruction_count++;
	return true;
}

static bool test_add_operand(
	void *context,
	zend_mir_instruction_id instruction_id,
	zend_mir_value_id value_id)
{
	test_host *host = (test_host *) context;
	test_operand *operand;

	if (!test_mutation_allowed(host)
			|| instruction_id >= host->instruction_count
			|| !test_has_value(host, value_id)
			|| host->operand_count >= TEST_CAPACITY * 2) {
		return false;
	}
	operand = &host->operands[host->operand_count++];
	operand->instruction_id = instruction_id;
	operand->value_id = value_id;
	return true;
}

static bool test_add_fact(
	void *context,
	const zend_mir_value_fact_ref *requested,
	zend_mir_value_fact_id *out)
{
	test_host *host = (test_host *) context;
	zend_mir_value_fact_ref *fact;

	if (!test_mutation_allowed(host) || requested == NULL || out == NULL
			|| host->fact_count >= TEST_CAPACITY
			|| !test_has_value(host, requested->value_id)
			|| !zend_mir_scalar_type_is_exact(requested->exact_type)) {
		return false;
	}
	fact = &host->facts[host->fact_count];
	*fact = *requested;
	fact->id = host->fact_count;
	*out = host->fact_count++;
	return true;
}

static zend_mir_representation test_representation(
	zend_mir_scalar_type_mask type)
{
	switch (type) {
		case ZEND_MIR_SCALAR_TYPE_I1:
			return ZEND_MIR_REPRESENTATION_I1;
		case ZEND_MIR_SCALAR_TYPE_I64:
			return ZEND_MIR_REPRESENTATION_I64;
		case ZEND_MIR_SCALAR_TYPE_F64:
			return ZEND_MIR_REPRESENTATION_DOUBLE;
		default:
			return ZEND_MIR_REPRESENTATION_ZVAL;
	}
}

static zend_mir_source_operand_ref test_source_operand(uint32_t ssa)
{
	zend_mir_source_operand_ref source;

	memset(&source, 0, sizeof(source));
	source.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.slot_kind = ZEND_MIR_SOURCE_SLOT_KIND_INVALID;
	source.index = ZEND_MIR_ID_INVALID;
	source.ssa_variable_id = ssa;
	return source;
}

static void test_set_binding(
	test_fixture *fixture,
	uint32_t index,
	zend_mir_value_id value_id,
	zend_mir_scalar_type_mask type,
	bool finite)
{
	zend_mir_logic_value_binding *binding = &fixture->bindings[index];
	zend_mir_value_record *value = &fixture->host.values[
		fixture->host.value_count++];

	memset(binding, 0, sizeof(*binding));
	binding->source = test_source_operand(index + 1);
	binding->value_id = value_id;
	binding->has_fact = true;
	binding->fact.id = index;
	binding->fact.value_id = value_id;
	binding->fact.exact_type = type;
	binding->fact.flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED
		| (finite ? ZEND_MIR_VALUE_FACT_FINITE : 0);
	binding->fact.provenance = ZEND_MIR_FACT_PROVENANCE_TYPE_ANALYSIS;
	binding->fact.provenance_source_position_id = 9;

	value->id = value_id;
	value->representation = test_representation(type);
	value->ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
}

static void test_fixture_init(
	test_fixture *fixture,
	uint32_t opcode,
	zend_mir_scalar_type_mask left_type,
	zend_mir_scalar_type_mask right_type)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->host.mutator.contract_version = ZEND_MIR_CONTRACT_VERSION;
	fixture->host.mutator.context = &fixture->host;
	fixture->host.mutator.add_value = test_add_value;
	fixture->host.mutator.add_instruction = test_add_instruction;
	fixture->host.mutator.add_operand = test_add_operand;
	fixture->host.mutator.add_value_fact = test_add_fact;

	test_set_binding(
		fixture, 0, 1, left_type, left_type == ZEND_MIR_SCALAR_TYPE_F64);
	test_set_binding(
		fixture, 1, 2, right_type, right_type == ZEND_MIR_SCALAR_TYPE_F64);
	memset(&fixture->bindings[2], 0, sizeof(fixture->bindings[2]));
	fixture->bindings[2].source = test_source_operand(3);
	fixture->bindings[2].value_id = 3;

	fixture->proof.opline_index = 7;
	fixture->proof.proofs = ZEND_MIR_LOGIC_PROOF_ALL;
	fixture->proof.temporary_value_id =
		zend_mir_value_from_synthetic(100);
	fixture->logic.bindings = fixture->bindings;
	fixture->logic.binding_count = 3;
	fixture->logic.opcode_proofs = &fixture->proof;
	fixture->logic.opcode_proof_count = 1;
	zend_mir_logic_provider_init(&fixture->provider, &fixture->logic);

	fixture->lowering.provider_context = &fixture->logic;
	fixture->lowering.block_id = 0;
	fixture->lowering.status = ZEND_MIR_LOWERING_SUCCESS;
	fixture->lowering.diagnostic = ZEND_MIRL_OK;

	fixture->source.opline_index = 7;
	fixture->source.zend_opcode_number = opcode;
	fixture->source.op1 = fixture->bindings[0].source;
	fixture->source.op2 = fixture->bindings[1].source;
	fixture->source.result = fixture->bindings[2].source;
	fixture->source.source_position_id = 9;
}

static zend_mir_lowering_status test_lower(test_fixture *fixture)
{
	return fixture->provider.lower(
		&fixture->lowering, &fixture->source, &fixture->host.mutator);
}

static uint32_t test_committed_mutations(const test_fixture *fixture)
{
	return fixture->host.value_count - 2
		+ fixture->host.instruction_count
		+ fixture->host.operand_count
		+ fixture->host.fact_count;
}

static void test_provider_claims(void)
{
	static const uint32_t expected[] = {
		14, 15, 16, 17, 18, 19, 20, 21, 51, 52, 170
	};
	zend_mir_logic_context logic;
	zend_mir_lowering_provider provider;
	uint32_t index;

	memset(&logic, 0, sizeof(logic));
	zend_mir_logic_provider_init(&provider, &logic);
	assert(provider.provider_id == ZEND_MIR_LOGIC_PROVIDER_ID);
	assert(provider.semantic_family_id == ZEND_MIR_LOGIC_SEMANTIC_FAMILY_ID);
	assert(provider.claim_count(provider.context)
		== sizeof(expected) / sizeof(expected[0]));
	for (index = 0; index < provider.claim_count(provider.context); index++) {
		zend_mir_lowering_claim claim;

		assert(provider.claim_at(provider.context, index, &claim));
		assert(claim.zend_opcode_number == expected[index]);
		assert(claim.semantic_family_id == ZEND_MIR_LOGIC_SEMANTIC_FAMILY_ID);
		assert(claim.zend_opcode_number != 123);
	}
	assert(!provider.claim_at(provider.context, index, NULL));
	zend_mir_logic_provider_init(&provider, NULL);
	assert(provider.claim_count(provider.context) == 0);
}

static void test_equality_pair_matrix(void)
{
	static const uint32_t opcodes[] = {
		ZEND_MIR_LOGIC_ZEND_IS_IDENTICAL,
		ZEND_MIR_LOGIC_ZEND_IS_NOT_IDENTICAL,
		ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_LOGIC_ZEND_IS_NOT_EQUAL
	};
	static const zend_mir_scalar_type_mask types[] = {
		ZEND_MIR_SCALAR_TYPE_NULL,
		ZEND_MIR_SCALAR_TYPE_I1,
		ZEND_MIR_SCALAR_TYPE_I64,
		ZEND_MIR_SCALAR_TYPE_F64
	};
	uint32_t opcode;
	uint32_t left;
	uint32_t right;

	for (opcode = 0; opcode < 4; opcode++) {
		for (left = 0; left < 4; left++) {
			for (right = 0; right < 4; right++) {
				test_fixture fixture;
				zend_mir_lowering_status status;
				bool accepted = left == right
					&& types[left] != ZEND_MIR_SCALAR_TYPE_NULL;

				test_fixture_init(
					&fixture, opcodes[opcode], types[left], types[right]);
				status = test_lower(&fixture);
				assert(status == (accepted
					? ZEND_MIR_LOWERING_SUCCESS
					: ZEND_MIR_LOWERING_DEFERRED));
				assert((fixture.host.instruction_count != 0) == accepted);
				if (!accepted) {
					assert(test_committed_mutations(&fixture) == 0);
					assert(fixture.lowering.failure_count == 1);
				}
			}
		}
	}
}

static void test_relational_pair_matrix(void)
{
	static const uint32_t opcodes[] = {
		ZEND_MIR_LOGIC_ZEND_IS_SMALLER,
		ZEND_MIR_LOGIC_ZEND_IS_SMALLER_OR_EQUAL,
		ZEND_MIR_LOGIC_ZEND_SPACESHIP
	};
	static const zend_mir_scalar_type_mask types[] = {
		ZEND_MIR_SCALAR_TYPE_NULL,
		ZEND_MIR_SCALAR_TYPE_I1,
		ZEND_MIR_SCALAR_TYPE_I64,
		ZEND_MIR_SCALAR_TYPE_F64
	};
	uint32_t opcode;
	uint32_t left;
	uint32_t right;

	for (opcode = 0; opcode < 3; opcode++) {
		for (left = 0; left < 4; left++) {
			for (right = 0; right < 4; right++) {
				test_fixture fixture;
				zend_mir_lowering_status status;
				bool accepted = left == right
					&& (types[left] == ZEND_MIR_SCALAR_TYPE_I64
						|| types[left] == ZEND_MIR_SCALAR_TYPE_F64);

				test_fixture_init(
					&fixture, opcodes[opcode], types[left], types[right]);
				status = test_lower(&fixture);
				assert(status == (accepted
					? ZEND_MIR_LOWERING_SUCCESS
					: ZEND_MIR_LOWERING_DEFERRED));
				assert((fixture.host.instruction_count != 0) == accepted);
				if (!accepted) {
					assert(test_committed_mutations(&fixture) == 0);
				}
			}
		}
	}
}

static void test_compare_opcodes_and_facts(void)
{
	static const struct {
		uint32_t source;
		zend_mir_scalar_type_mask type;
		zend_mir_opcode opcode;
		zend_mir_representation result_representation;
	} cases[] = {
		{16, ZEND_MIR_SCALAR_TYPE_I1, ZEND_MIR_OPCODE_I1_EQ,
			ZEND_MIR_REPRESENTATION_I1},
		{16, ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_OPCODE_I64_EQ,
			ZEND_MIR_REPRESENTATION_I1},
		{18, ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_OPCODE_F64_EQ,
			ZEND_MIR_REPRESENTATION_I1},
		{20, ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_OPCODE_I64_LT,
			ZEND_MIR_REPRESENTATION_I1},
		{20, ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_OPCODE_F64_LT,
			ZEND_MIR_REPRESENTATION_I1},
		{21, ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_OPCODE_I64_LE,
			ZEND_MIR_REPRESENTATION_I1},
		{21, ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_OPCODE_F64_LE,
			ZEND_MIR_REPRESENTATION_I1},
		{170, ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_OPCODE_I64_CMP,
			ZEND_MIR_REPRESENTATION_I64},
		{170, ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_OPCODE_F64_CMP,
			ZEND_MIR_REPRESENTATION_I64}
	};
	uint32_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		test_fixture fixture;
		const zend_mir_instruction_record *instruction;
		const zend_mir_value_fact_ref *fact;

		test_fixture_init(
			&fixture, cases[index].source, cases[index].type,
			cases[index].type);
		assert(test_lower(&fixture) == ZEND_MIR_LOWERING_SUCCESS);
		assert(fixture.host.instruction_count == 1);
		assert(fixture.host.fact_count == 1);
		instruction = &fixture.host.instructions[0];
		fact = &fixture.host.facts[0];
		assert(instruction->opcode == cases[index].opcode);
		assert(instruction->representation
			== cases[index].result_representation);
		assert(instruction->source_position_id == 9);
		assert(instruction->frame_state_id == ZEND_MIR_ID_INVALID);
		assert(instruction->effects == 0);
		assert(instruction->reads == 0);
		assert(instruction->writes == 0);
		assert(instruction->barriers == 0);
		assert(instruction->ownership_actions
			== ZEND_MIR_OWNERSHIP_ACTION_MASK(
				ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED));
		assert(fact->value_id == 3);
		assert(fact->provenance == ZEND_MIR_FACT_PROVENANCE_CONTRACT);
		assert(fact->provenance_source_position_id == 9);
		assert((fact->flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) != 0);
		if (cases[index].source == 170) {
			assert(fact->exact_type == ZEND_MIR_SCALAR_TYPE_I64);
			assert((fact->flags
				& ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0);
			assert(fact->integer_min == -1);
			assert(fact->integer_max == 1);
		} else {
			assert(fact->exact_type == ZEND_MIR_SCALAR_TYPE_I1);
		}
	}
}

static void test_negated_comparisons(void)
{
	static const uint32_t opcodes[] = {
		ZEND_MIR_LOGIC_ZEND_IS_NOT_IDENTICAL,
		ZEND_MIR_LOGIC_ZEND_IS_NOT_EQUAL
	};
	uint32_t index;

	for (index = 0; index < 2; index++) {
		test_fixture fixture;
		zend_mir_value_id temporary = zend_mir_value_from_synthetic(100);

		test_fixture_init(
			&fixture, opcodes[index],
			ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
		assert(test_lower(&fixture) == ZEND_MIR_LOWERING_SUCCESS);
		assert(fixture.host.instruction_count == 2);
		assert(fixture.host.instructions[0].opcode
			== ZEND_MIR_OPCODE_I64_EQ);
		assert(fixture.host.instructions[0].result_id == temporary);
		assert(fixture.host.instructions[1].opcode
			== ZEND_MIR_OPCODE_I1_NOT);
		assert(fixture.host.operands[2].value_id == temporary);
		assert(fixture.host.facts[1].value_id == 3);

		test_fixture_init(
			&fixture, opcodes[index],
			ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
		fixture.proof.temporary_value_id = ZEND_MIR_ID_INVALID;
		assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
		assert(test_committed_mutations(&fixture) == 0);
		assert(fixture.lowering.diagnostic == ZEND_MIRL_MISSING_PROOF);
	}
}

static void test_f64_boundaries_and_relational_deferrals(void)
{
	test_fixture fixture;

	/* Finite F64 includes both signed-zero encodings and uses typed F64 MIR. */
	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_IDENTICAL,
		ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_F64);
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_SUCCESS);
	assert(fixture.host.instructions[0].opcode == ZEND_MIR_OPCODE_F64_EQ);

	/* NaN and infinities lack the frozen finite_f64 proof and fail closed. */
	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_F64);
	fixture.bindings[0].fact.flags &= ~ZEND_MIR_VALUE_FACT_FINITE;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);
	assert(fixture.lowering.diagnostic == ZEND_MIRL_MISSING_PROOF);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_SPACESHIP,
		ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_F64);
	fixture.proof.proofs &= ~ZEND_MIR_LOGIC_PROOF_FINITE_F64;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);

	/* bool/null ordering and every int-double precision pair remain deferred. */
	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_SMALLER,
		ZEND_MIR_SCALAR_TYPE_I1, ZEND_MIR_SCALAR_TYPE_I1);
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_F64);
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);
}

static void test_boolean_lowering(void)
{
	static const zend_mir_scalar_type_mask types[] = {
		ZEND_MIR_SCALAR_TYPE_NULL,
		ZEND_MIR_SCALAR_TYPE_I1,
		ZEND_MIR_SCALAR_TYPE_I64,
		ZEND_MIR_SCALAR_TYPE_F64
	};
	static const struct {
		uint32_t source;
		zend_mir_scalar_type_mask left;
		zend_mir_scalar_type_mask right;
		zend_mir_opcode opcode;
	} cases[] = {
		{14, ZEND_MIR_SCALAR_TYPE_I1, ZEND_MIR_SCALAR_TYPE_I1,
			ZEND_MIR_OPCODE_I1_NOT},
		{15, ZEND_MIR_SCALAR_TYPE_I1, ZEND_MIR_SCALAR_TYPE_I1,
			ZEND_MIR_OPCODE_I1_XOR},
		{52, ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I1,
			ZEND_MIR_OPCODE_I64_TO_I1},
		{52, ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_I1,
			ZEND_MIR_OPCODE_F64_TO_I1}
	};
	uint32_t index;
	uint32_t left;
	uint32_t right;
	test_fixture fixture;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		test_fixture_init(
			&fixture, cases[index].source, cases[index].left,
			cases[index].right);
		assert(test_lower(&fixture) == ZEND_MIR_LOWERING_SUCCESS);
		assert(fixture.host.instructions[0].opcode == cases[index].opcode);
		assert(fixture.host.facts[0].exact_type
			== ZEND_MIR_SCALAR_TYPE_I1);
	}

	for (left = 0; left < 4; left++) {
		bool not_accepted = types[left] == ZEND_MIR_SCALAR_TYPE_I1;
		bool bool_accepted = types[left] == ZEND_MIR_SCALAR_TYPE_I64
			|| types[left] == ZEND_MIR_SCALAR_TYPE_F64;

		test_fixture_init(
			&fixture, ZEND_MIR_LOGIC_ZEND_BOOL_NOT,
			types[left], ZEND_MIR_SCALAR_TYPE_I1);
		assert(test_lower(&fixture) == (not_accepted
			? ZEND_MIR_LOWERING_SUCCESS : ZEND_MIR_LOWERING_DEFERRED));
		assert((fixture.host.instruction_count != 0) == not_accepted);

		test_fixture_init(
			&fixture, ZEND_MIR_LOGIC_ZEND_BOOL,
			types[left], ZEND_MIR_SCALAR_TYPE_I1);
		assert(test_lower(&fixture) == (bool_accepted
			? ZEND_MIR_LOWERING_SUCCESS : ZEND_MIR_LOWERING_DEFERRED));
		assert((fixture.host.instruction_count != 0) == bool_accepted);
	}
	for (left = 0; left < 4; left++) {
		for (right = 0; right < 4; right++) {
			bool accepted = types[left] == ZEND_MIR_SCALAR_TYPE_I1
				&& types[right] == ZEND_MIR_SCALAR_TYPE_I1;

			test_fixture_init(
				&fixture, ZEND_MIR_LOGIC_ZEND_BOOL_XOR,
				types[left], types[right]);
			assert(test_lower(&fixture) == (accepted
				? ZEND_MIR_LOWERING_SUCCESS
				: ZEND_MIR_LOWERING_DEFERRED));
			assert((fixture.host.instruction_count != 0) == accepted);
			if (!accepted) {
				assert(test_committed_mutations(&fixture) == 0);
			}
		}
	}

	/* A non-finite double may be infinity, but NaN needs safe-cast proof. */
	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_BOOL,
		ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_I1);
	fixture.bindings[0].fact.flags &= ~ZEND_MIR_VALUE_FACT_FINITE;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_SUCCESS);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_BOOL,
		ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_I1);
	fixture.proof.proofs &= ~ZEND_MIR_LOGIC_PROOF_SAFE_SCALAR_CAST;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);
}

static void test_cast_lowering(void)
{
	static const zend_mir_scalar_type_mask types[] = {
		ZEND_MIR_SCALAR_TYPE_NULL,
		ZEND_MIR_SCALAR_TYPE_I1,
		ZEND_MIR_SCALAR_TYPE_I64,
		ZEND_MIR_SCALAR_TYPE_F64
	};
	static const uint32_t targets[] = {
		ZEND_MIR_LOGIC_CAST_LONG,
		ZEND_MIR_LOGIC_CAST_DOUBLE
	};
	static const struct {
		zend_mir_scalar_type_mask source;
		uint32_t target;
		zend_mir_opcode opcode;
		zend_mir_representation representation;
	} cases[] = {
		{ZEND_MIR_SCALAR_TYPE_I1, 4, ZEND_MIR_OPCODE_I1_TO_I64,
			ZEND_MIR_REPRESENTATION_I64},
		{ZEND_MIR_SCALAR_TYPE_F64, 4,
			ZEND_MIR_OPCODE_F64_TO_I64_CHECKED,
			ZEND_MIR_REPRESENTATION_I64},
		{ZEND_MIR_SCALAR_TYPE_I1, 5, ZEND_MIR_OPCODE_I1_TO_F64,
			ZEND_MIR_REPRESENTATION_DOUBLE},
		{ZEND_MIR_SCALAR_TYPE_I64, 5, ZEND_MIR_OPCODE_I64_TO_F64,
			ZEND_MIR_REPRESENTATION_DOUBLE}
	};
	uint32_t index;
	uint32_t source;
	uint32_t target;
	test_fixture fixture;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		test_fixture_init(
			&fixture, ZEND_MIR_LOGIC_ZEND_CAST, cases[index].source,
			ZEND_MIR_SCALAR_TYPE_I1);
		fixture.source.extended_value = cases[index].target;
		assert(test_lower(&fixture) == ZEND_MIR_LOWERING_SUCCESS);
		assert(fixture.host.instructions[0].opcode == cases[index].opcode);
		assert(fixture.host.instructions[0].representation
			== cases[index].representation);
	}

	for (source = 0; source < 4; source++) {
		for (target = 0; target < 2; target++) {
			bool accepted = (targets[target] == ZEND_MIR_LOGIC_CAST_LONG
					&& (types[source] == ZEND_MIR_SCALAR_TYPE_I1
						|| types[source] == ZEND_MIR_SCALAR_TYPE_F64))
				|| (targets[target] == ZEND_MIR_LOGIC_CAST_DOUBLE
					&& (types[source] == ZEND_MIR_SCALAR_TYPE_I1
						|| types[source] == ZEND_MIR_SCALAR_TYPE_I64));

			test_fixture_init(
				&fixture, ZEND_MIR_LOGIC_ZEND_CAST, types[source],
				ZEND_MIR_SCALAR_TYPE_I1);
			fixture.source.extended_value = targets[target];
			assert(test_lower(&fixture) == (accepted
				? ZEND_MIR_LOWERING_SUCCESS
				: ZEND_MIR_LOWERING_DEFERRED));
			assert((fixture.host.instruction_count != 0) == accepted);
			if (!accepted) {
				assert(test_committed_mutations(&fixture) == 0);
			}
		}
	}

	/* Bool-target, string, and unsafe conversions defer. */
	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_CAST,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I1);
	fixture.source.extended_value = 13;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_CAST,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I1);
	fixture.source.extended_value = 6;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_CAST,
		ZEND_MIR_SCALAR_TYPE_F64, ZEND_MIR_SCALAR_TYPE_I1);
	fixture.source.extended_value = ZEND_MIR_LOGIC_CAST_LONG;
	fixture.bindings[0].fact.flags &= ~ZEND_MIR_VALUE_FACT_FINITE;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_CAST,
		ZEND_MIR_SCALAR_TYPE_I1, ZEND_MIR_SCALAR_TYPE_I1);
	fixture.source.extended_value = ZEND_MIR_LOGIC_CAST_LONG;
	fixture.proof.proofs &= ~ZEND_MIR_LOGIC_PROOF_NO_EXCEPTION;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);
}

static void test_rejection_and_mutation_boundaries(void)
{
	test_fixture fixture;

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.proof.proofs &= ~ZEND_MIR_LOGIC_PROOF_NO_CALLS;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.logic.binding_count = 2;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.bindings[1] = fixture.bindings[0];
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_REJECTED);
	assert(test_committed_mutations(&fixture) == 0);
	assert(fixture.lowering.diagnostic == ZEND_MIRL_CONTRADICTORY_FACT);

	test_fixture_init(
		&fixture, 123,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_REJECTED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.source.source_position_id = ZEND_MIR_ID_INVALID;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_REJECTED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.host.mutator.add_value_fact = NULL;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_REJECTED);
	assert(test_committed_mutations(&fixture) == 0);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.host.fail_at_attempt = 1;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_FAILED);
	assert(test_committed_mutations(&fixture) == 0);
	assert(fixture.lowering.diagnostic == ZEND_MIRL_MUTATION_FAILED);

	test_fixture_init(
		&fixture, ZEND_MIR_LOGIC_ZEND_IS_EQUAL,
		ZEND_MIR_SCALAR_TYPE_I64, ZEND_MIR_SCALAR_TYPE_I64);
	fixture.host.fail_at_attempt = 3;
	assert(test_lower(&fixture) == ZEND_MIR_LOWERING_FAILED);
	assert(fixture.lowering.diagnostic == ZEND_MIRL_MUTATION_FAILED);
}

int main(void)
{
	test_provider_claims();
	test_equality_pair_matrix();
	test_relational_pair_matrix();
	test_compare_opcodes_and_facts();
	test_negated_comparisons();
	test_f64_boundaries_and_relational_deferrals();
	test_boolean_lowering();
	test_cast_lowering();
	test_rejection_and_mutation_boundaries();
	return 0;
}
