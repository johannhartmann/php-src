#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "Zend/Native/Lowering/Scalar/Numeric/zend_mir_lower_numeric.h"

enum {
	TEST_FACT_CAPACITY = 4,
	TEST_OPERAND_CAPACITY = 4
};

struct _zend_mir_lowering_context {
	const void *provider_context;
	zend_mir_block_id block_id;
	zend_mir_lowering_status failure_status;
	zend_mir_lowering_diagnostic_code failure_diagnostic;
};

typedef struct _test_host {
	zend_mir_value_fact_ref input_facts[TEST_FACT_CAPACITY];
	uint32_t input_fact_count;
	zend_mir_source_ssa_use_ref uses[2];
	uint32_t use_count;
	zend_mir_source_ssa_def_ref definitions[1];
	uint32_t definition_count;
	zend_mir_source_position_ref position;
	zend_mir_value_record emitted_value;
	zend_mir_instruction_record emitted_instruction;
	zend_mir_value_id emitted_operands[TEST_OPERAND_CAPACITY];
	zend_mir_value_fact_ref emitted_fact;
	uint32_t position_count;
	uint32_t value_count;
	uint32_t instruction_count;
	uint32_t operand_count;
	uint32_t fact_count;
	uint32_t mutation_attempt;
	uint32_t fail_mutation_at;
} test_host;

typedef struct _test_case {
	test_host host;
	struct _zend_mir_lowering_context lowering;
	zend_mir_numeric_provider_context provider;
	zend_mir_lowering_source_view source_view;
	zend_mir_mutator mutator;
	zend_mir_source_opcode_ref source;
} test_case;

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
	zend_mir_lowering_context *context, zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic)
{
	if (context == NULL || status == ZEND_MIR_LOWERING_SUCCESS
			|| diagnostic == ZEND_MIRL_OK) {
		return false;
	}
	context->failure_status = status;
	context->failure_diagnostic = diagnostic;
	return true;
}

static bool test_resolve_operand(
	const void *context, const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out)
{
	(void) context;
	if (operand == NULL || value_id_out == NULL
			|| operand->kind != ZEND_MIR_SOURCE_OPERAND_SSA
			|| operand->ssa_variable_id > ZEND_MIR_VALUE_ORIGINAL_MAX) {
		return false;
	}
	*value_id_out =
		zend_mir_value_from_original_ssa(operand->ssa_variable_id);
	return true;
}

static bool test_value_fact(
	const void *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *fact_out)
{
	const test_host *host = (const test_host *) context;
	uint32_t index;

	if (host == NULL || fact_out == NULL) {
		return false;
	}
	for (index = 0; index < host->input_fact_count; index++) {
		if (host->input_facts[index].value_id == value_id) {
			*fact_out = host->input_facts[index];
			return true;
		}
	}
	return false;
}

static bool test_source_position(
	const void *context, zend_mir_source_position_id requested_id,
	zend_mir_source_position_ref *position_out)
{
	const test_host *host = (const test_host *) context;

	if (host == NULL || position_out == NULL
			|| host->position.id != requested_id) {
		return false;
	}
	*position_out = host->position;
	return true;
}

static uint32_t test_ssa_use_count(const void *context)
{
	return ((const test_host *) context)->use_count;
}

static bool test_ssa_use_at(
	const void *context, uint32_t index, zend_mir_source_ssa_use_ref *out)
{
	const test_host *host = (const test_host *) context;

	if (out == NULL || index >= host->use_count) {
		return false;
	}
	*out = host->uses[index];
	return true;
}

static uint32_t test_ssa_def_count(const void *context)
{
	return ((const test_host *) context)->definition_count;
}

static bool test_ssa_def_at(
	const void *context, uint32_t index, zend_mir_source_ssa_def_ref *out)
{
	const test_host *host = (const test_host *) context;

	if (out == NULL || index >= host->definition_count) {
		return false;
	}
	*out = host->definitions[index];
	return true;
}

static bool test_mutation_succeeds(test_host *host)
{
	host->mutation_attempt++;
	return host->fail_mutation_at == 0
		|| host->mutation_attempt != host->fail_mutation_at;
}

static bool test_add_source_position(
	void *context, const zend_mir_source_position_ref *position,
	zend_mir_source_position_id *out)
{
	test_host *host = (test_host *) context;

	if (host == NULL || position == NULL || out == NULL
			|| !test_mutation_succeeds(host)) {
		return false;
	}
	host->position_count++;
	*out = position->id;
	return true;
}

static bool test_add_value(
	void *context, zend_mir_value_id requested_id,
	zend_mir_representation representation,
	zend_mir_ownership_state ownership)
{
	test_host *host = (test_host *) context;

	if (host == NULL || !test_mutation_succeeds(host)) {
		return false;
	}
	host->emitted_value.id = requested_id;
	host->emitted_value.representation = representation;
	host->emitted_value.ownership = ownership;
	host->value_count++;
	return true;
}

static bool test_add_instruction(
	void *context, const zend_mir_instruction_record *instruction,
	zend_mir_instruction_id *out)
{
	test_host *host = (test_host *) context;

	if (host == NULL || instruction == NULL || out == NULL
			|| !test_mutation_succeeds(host)) {
		return false;
	}
	host->emitted_instruction = *instruction;
	host->instruction_count++;
	*out = 0;
	return true;
}

static bool test_add_operand(
	void *context, zend_mir_instruction_id instruction_id,
	zend_mir_value_id value_id)
{
	test_host *host = (test_host *) context;

	if (host == NULL || instruction_id != 0
			|| host->operand_count >= TEST_OPERAND_CAPACITY
			|| !test_mutation_succeeds(host)) {
		return false;
	}
	host->emitted_operands[host->operand_count++] = value_id;
	return true;
}

static bool test_add_value_fact(
	void *context, const zend_mir_value_fact_ref *fact,
	zend_mir_value_fact_id *out)
{
	test_host *host = (test_host *) context;

	if (host == NULL || fact == NULL || out == NULL
			|| !test_mutation_succeeds(host)) {
		return false;
	}
	host->emitted_fact = *fact;
	host->fact_count++;
	*out = 0;
	return true;
}

static zend_mir_value_fact_ref test_fact(
	zend_mir_value_id value_id, zend_mir_scalar_type_mask exact_type,
	int64_t minimum, int64_t maximum, bool has_range)
{
	zend_mir_value_fact_ref fact = {
		.id = value_id,
		.value_id = value_id,
		.exact_type = exact_type,
		.flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED,
		.integer_min = minimum,
		.integer_max = maximum,
		.provenance = ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS,
		.provenance_source_position_id = 0
	};

	if (has_range) {
		fact.flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
	}
	return fact;
}

static zend_mir_numeric_proof_mask test_all_proofs(void)
{
	return ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK
		| ZEND_MIR_NUMERIC_PROOF_NO_CALLS
		| ZEND_MIR_NUMERIC_PROOF_NO_REENTRY
		| ZEND_MIR_NUMERIC_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_NUMERIC_PROOF_NO_EXCEPTION;
}

static void test_case_init(
	test_case *test, uint32_t opcode,
	zend_mir_value_fact_ref left, zend_mir_value_fact_ref right)
{
	memset(test, 0, sizeof(*test));
	test->host.input_facts[0] = left;
	test->host.input_facts[1] = right;
	test->host.input_fact_count = 2;
	test->host.position.id = 0;
	test->host.position.file_symbol_id = 17;
	test->host.position.line = 23;
	test->host.position.column_start = 4;
	test->host.position.column_end = 9;
	test->host.uses[0].ssa_variable_id = left.value_id;
	test->host.uses[0].opline_index = 5;
	test->host.uses[0].operand_index = 0;
	test->host.uses[1].ssa_variable_id = right.value_id;
	test->host.uses[1].opline_index = 5;
	test->host.uses[1].operand_index = 1;
	test->host.use_count =
		opcode == ZEND_MIR_NUMERIC_OPCODE_BW_NOT ? 1 : 2;
	test->host.definitions[0].ssa_variable_id = 3;
	test->host.definitions[0].opline_index = 5;
	test->host.definition_count = 1;
	test->lowering.provider_context = &test->provider;
	test->lowering.block_id = 7;
	test->lowering.failure_status = ZEND_MIR_LOWERING_STATUS_INVALID;
	test->lowering.failure_diagnostic = ZEND_MIRL_OK;
	test->source_view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	test->source_view.context = &test->host;
	test->source_view.ssa_use_count = test_ssa_use_count;
	test->source_view.ssa_use_at = test_ssa_use_at;
	test->source_view.ssa_def_count = test_ssa_def_count;
	test->source_view.ssa_def_at = test_ssa_def_at;
	test->provider.source = &test->source_view;
	test->provider.source_context = &test->host;
	test->provider.resolve_operand = test_resolve_operand;
	test->provider.value_fact = test_value_fact;
	test->provider.source_position = test_source_position;
	test->provider.proofs = test_all_proofs();
	test->mutator.contract_version = ZEND_MIR_CONTRACT_VERSION;
	test->mutator.context = &test->host;
	test->mutator.add_source_position = test_add_source_position;
	test->mutator.add_value = test_add_value;
	test->mutator.add_instruction = test_add_instruction;
	test->mutator.add_operand = test_add_operand;
	test->mutator.add_value_fact = test_add_value_fact;
	test->source.opline_index = 5;
	test->source.zend_opcode_number = opcode;
	test->source.op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	test->source.op1.ssa_variable_id = left.value_id;
	test->source.op2.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	test->source.op2.ssa_variable_id = right.value_id;
	test->source.result.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	test->source.result.ssa_variable_id = 3;
	test->source.source_position_id = 0;
	if (opcode == ZEND_MIR_NUMERIC_OPCODE_BW_NOT) {
		test->source.op2.kind = ZEND_MIR_SOURCE_OPERAND_UNUSED;
	}
}

static uint32_t test_mutation_count(const test_case *test)
{
	return test->host.position_count + test->host.value_count
		+ test->host.instruction_count + test->host.operand_count
		+ test->host.fact_count;
}

static void test_assert_success(
	test_case *test, zend_mir_opcode expected_opcode,
	zend_mir_representation expected_representation,
	uint32_t expected_operand_count)
{
	zend_mir_lowering_diagnostic_code diagnostic =
		ZEND_MIRL_INVALID_SOURCE;

	assert(zend_mir_lower_numeric(
		&test->lowering, &test->source, &test->mutator,
		&test->provider, &diagnostic) == ZEND_MIR_LOWERING_SUCCESS);
	assert(diagnostic == ZEND_MIRL_OK);
	assert(test->host.position_count == 1);
	assert(test->host.value_count == 1);
	assert(test->host.instruction_count == 1);
	assert(test->host.operand_count == expected_operand_count);
	assert(test->host.fact_count == 1);
	assert(test->host.emitted_value.id == 3);
	assert(test->host.emitted_value.representation
		== expected_representation);
	assert(test->host.emitted_value.ownership
		== ZEND_MIR_OWNERSHIP_STATE_OWNED);
	assert(test->host.emitted_instruction.block_id == 7);
	assert(test->host.emitted_instruction.opcode == expected_opcode);
	assert(test->host.emitted_instruction.representation
		== expected_representation);
	assert(test->host.emitted_instruction.result_id == 3);
	assert(test->host.emitted_instruction.frame_state_id
		== ZEND_MIR_ID_INVALID);
	assert(test->host.emitted_instruction.source_position_id == 0);
	assert(test->host.emitted_instruction.effects == 0);
	assert(test->host.emitted_instruction.reads == 0);
	assert(test->host.emitted_instruction.writes == 0);
	assert(test->host.emitted_instruction.barriers == 0);
	assert(test->host.emitted_instruction.ownership_actions == 0);
	assert(test->host.emitted_operands[0] == 1);
	if (expected_operand_count == 2) {
		assert(test->host.emitted_operands[1] == 2);
	}
	assert(test->host.emitted_fact.value_id == 3);
	assert((test->host.emitted_fact.flags
		& ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) != 0);
	assert(test->host.emitted_fact.provenance_source_position_id == 0);
}

static void test_range_proofs(void)
{
	zend_mir_numeric_range result;

	assert(zend_mir_numeric_range_add(
		(zend_mir_numeric_range) {INT64_MAX - 1, INT64_MAX - 1},
		(zend_mir_numeric_range) {1, 1}, &result));
	assert(result.minimum == INT64_MAX && result.maximum == INT64_MAX);
	assert(!zend_mir_numeric_range_add(
		(zend_mir_numeric_range) {INT64_MAX, INT64_MAX},
		(zend_mir_numeric_range) {1, 1}, &result));
	assert(zend_mir_numeric_range_subtract(
		(zend_mir_numeric_range) {INT64_MIN + 1, INT64_MIN + 1},
		(zend_mir_numeric_range) {1, 1}, &result));
	assert(result.minimum == INT64_MIN && result.maximum == INT64_MIN);
	assert(!zend_mir_numeric_range_subtract(
		(zend_mir_numeric_range) {INT64_MIN, INT64_MIN},
		(zend_mir_numeric_range) {1, 1}, &result));
	assert(zend_mir_numeric_range_multiply(
		(zend_mir_numeric_range) {-3, 4},
		(zend_mir_numeric_range) {-2, 5}, &result));
	assert(result.minimum == -15 && result.maximum == 20);
	assert(!zend_mir_numeric_range_multiply(
		(zend_mir_numeric_range) {INT64_MIN, INT64_MIN},
		(zend_mir_numeric_range) {-1, -1}, &result));
	assert(zend_mir_numeric_modulo_is_safe(
		(zend_mir_numeric_range) {INT64_MIN + 1, INT64_MAX},
		(zend_mir_numeric_range) {-1, -1}));
	assert(zend_mir_numeric_range_modulo(
		(zend_mir_numeric_range) {INT64_MIN, INT64_MAX},
		(zend_mir_numeric_range) {2, 2}, &result));
	assert(result.minimum == -1 && result.maximum == 1);
	assert(zend_mir_numeric_range_modulo(
		(zend_mir_numeric_range) {0, INT64_MAX},
		(zend_mir_numeric_range) {INT64_MIN, INT64_MIN}, &result));
	assert(result.minimum == 0 && result.maximum == INT64_MAX);
	assert(zend_mir_numeric_range_modulo(
		(zend_mir_numeric_range) {INT64_MIN, 0},
		(zend_mir_numeric_range) {2, 4}, &result));
	assert(result.minimum == -3 && result.maximum == 0);
	assert(!zend_mir_numeric_modulo_is_safe(
		(zend_mir_numeric_range) {INT64_MIN, INT64_MAX},
		(zend_mir_numeric_range) {-1, -1}));
	assert(!zend_mir_numeric_modulo_is_safe(
		(zend_mir_numeric_range) {-8, 8},
		(zend_mir_numeric_range) {-1, 1}));
	assert(zend_mir_numeric_shift_left(
		(zend_mir_numeric_range) {-1, -1},
		(zend_mir_numeric_range) {63, 63}, &result));
	assert(result.minimum == INT64_MIN);
	assert(!zend_mir_numeric_shift_left(
		(zend_mir_numeric_range) {1, 1},
		(zend_mir_numeric_range) {63, 63}, &result));
	assert(!zend_mir_numeric_shift_left(
		(zend_mir_numeric_range) {1, 1},
		(zend_mir_numeric_range) {-1, 1}, &result));
	assert(zend_mir_numeric_shift_right(
		(zend_mir_numeric_range) {-1, -1},
		(zend_mir_numeric_range) {63, 63}, &result));
	assert(result.minimum == -1 && result.maximum == -1);
	assert(!zend_mir_numeric_shift_right(
		(zend_mir_numeric_range) {1, 1},
		(zend_mir_numeric_range) {64, 64}, &result));
}

static void test_integer_arithmetic(void)
{
	static const struct {
		uint32_t source_opcode;
		zend_mir_opcode mir_opcode;
	} operations[] = {
		{ZEND_MIR_NUMERIC_OPCODE_ADD,
			ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW},
		{ZEND_MIR_NUMERIC_OPCODE_SUB,
			ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW},
		{ZEND_MIR_NUMERIC_OPCODE_MUL,
			ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW}
	};
	uint32_t index;

	for (index = 0; index < sizeof(operations) / sizeof(operations[0]);
			index++) {
		test_case test;

		test_case_init(
			&test, operations[index].source_opcode,
			test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, -8, 8, true),
			test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, -4, 4, true));
		test_assert_success(
			&test, operations[index].mir_opcode,
			ZEND_MIR_REPRESENTATION_I64, 2);
		assert(test.host.emitted_fact.exact_type
			== ZEND_MIR_SCALAR_TYPE_I64);
		assert((test.host.emitted_fact.flags
			& ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0);
	}
}

static void test_double_arithmetic(void)
{
	static const struct {
		uint32_t source_opcode;
		zend_mir_opcode mir_opcode;
	} operations[] = {
		{ZEND_MIR_NUMERIC_OPCODE_ADD, ZEND_MIR_OPCODE_F64_ADD},
		{ZEND_MIR_NUMERIC_OPCODE_SUB, ZEND_MIR_OPCODE_F64_SUB},
		{ZEND_MIR_NUMERIC_OPCODE_MUL, ZEND_MIR_OPCODE_F64_MUL}
	};
	uint32_t index;

	for (index = 0; index < sizeof(operations) / sizeof(operations[0]);
			index++) {
		test_case test;

		/*
		 * No FINITE fact is supplied: the typed opcode must preserve the
		 * native IEEE behavior for NaN, infinities, and signed zero.
		 */
		test_case_init(
			&test, operations[index].source_opcode,
			test_fact(1, ZEND_MIR_SCALAR_TYPE_F64, 0, 0, false),
			test_fact(2, ZEND_MIR_SCALAR_TYPE_F64, 0, 0, false));
		test_assert_success(
			&test, operations[index].mir_opcode,
			ZEND_MIR_REPRESENTATION_DOUBLE, 2);
		assert(test.host.emitted_fact.exact_type
			== ZEND_MIR_SCALAR_TYPE_F64);
		assert((test.host.emitted_fact.flags
			& (ZEND_MIR_VALUE_FACT_FINITE
				| ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE)) == 0);
	}
}

static void test_integer_special_operations(void)
{
	static const struct {
		uint32_t source_opcode;
		zend_mir_opcode mir_opcode;
	} operations[] = {
		{ZEND_MIR_NUMERIC_OPCODE_MOD, ZEND_MIR_OPCODE_I64_MOD_NONZERO},
		{ZEND_MIR_NUMERIC_OPCODE_SL, ZEND_MIR_OPCODE_I64_SHL_CHECKED},
		{ZEND_MIR_NUMERIC_OPCODE_SR, ZEND_MIR_OPCODE_I64_SHR_CHECKED}
	};
	uint32_t index;

	for (index = 0; index < sizeof(operations) / sizeof(operations[0]);
			index++) {
		test_case test;

		test_case_init(
			&test, operations[index].source_opcode,
			test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, -16, 16, true),
			test_fact(
				2, ZEND_MIR_SCALAR_TYPE_I64,
				operations[index].source_opcode == ZEND_MIR_NUMERIC_OPCODE_MOD
					? 2 : 0,
				operations[index].source_opcode == ZEND_MIR_NUMERIC_OPCODE_MOD
					? 4 : 3,
				true));
		test_assert_success(
			&test, operations[index].mir_opcode,
			ZEND_MIR_REPRESENTATION_I64, 2);
		assert((test.host.emitted_fact.flags
			& ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0);
		if (operations[index].source_opcode == ZEND_MIR_NUMERIC_OPCODE_MOD) {
			assert(test.host.emitted_fact.integer_min == -3);
			assert(test.host.emitted_fact.integer_max == 3);
		}
	}
}

static void test_bitwise_operations(void)
{
	static const struct {
		uint32_t source_opcode;
		zend_mir_opcode mir_opcode;
		uint32_t operands;
	} operations[] = {
		{ZEND_MIR_NUMERIC_OPCODE_BW_OR, ZEND_MIR_OPCODE_I64_BIT_OR, 2},
		{ZEND_MIR_NUMERIC_OPCODE_BW_AND, ZEND_MIR_OPCODE_I64_BIT_AND, 2},
		{ZEND_MIR_NUMERIC_OPCODE_BW_XOR, ZEND_MIR_OPCODE_I64_BIT_XOR, 2},
		{ZEND_MIR_NUMERIC_OPCODE_BW_NOT, ZEND_MIR_OPCODE_I64_BIT_NOT, 1}
	};
	uint32_t index;

	for (index = 0; index < sizeof(operations) / sizeof(operations[0]);
			index++) {
		test_case test;

		test_case_init(
			&test, operations[index].source_opcode,
			test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 0, 0, false),
			test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 0, 0, false));
		test_assert_success(
			&test, operations[index].mir_opcode,
			ZEND_MIR_REPRESENTATION_I64, operations[index].operands);
	}
}

static void test_rejections_are_pre_mutation(void)
{
	test_case test;
	zend_mir_lowering_diagnostic_code diagnostic;

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_ADD,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, INT64_MAX, INT64_MAX, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_MISSING_PROOF);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_MOD,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, INT64_MIN, INT64_MIN, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, -1, -1, true));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_MISSING_PROOF);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_SL,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, -1, -1, true));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_MISSING_PROOF);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_SR,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 64, 64, true));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_MISSING_PROOF);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_ADD,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 2, 2, true));
	test.provider.proofs &= ~ZEND_MIR_NUMERIC_PROOF_SINGLE_BLOCK;
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_MISSING_PROOF);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_MUL,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 0, 0, false),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 2, 2, true));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_MISSING_PROOF);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_ADD,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 2, 2, true));
	test.host.uses[1].operand_index = 0;
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic == ZEND_MIRL_INVALID_SOURCE);
	assert(test_mutation_count(&test) == 0);
}

static void test_deferred_paths(void)
{
	test_case test;
	zend_mir_lowering_diagnostic_code diagnostic;
	const zend_mir_numeric_hazard_mask runtime_hazards[] = {
		ZEND_MIR_NUMERIC_HAZARD_STRING,
		ZEND_MIR_NUMERIC_HAZARD_OBJECT,
		ZEND_MIR_NUMERIC_HAZARD_ARRAY,
		ZEND_MIR_NUMERIC_HAZARD_HELPER
	};
	uint32_t index;

	for (index = 0;
			index < sizeof(runtime_hazards) / sizeof(runtime_hazards[0]);
			index++) {
		test_case_init(
			&test, ZEND_MIR_NUMERIC_OPCODE_BW_OR,
			test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 0, 0, false),
			test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 0, 0, false));
		test.provider.hazards = runtime_hazards[index];
		assert(zend_mir_lower_numeric(
			&test.lowering, &test.source, &test.mutator, &test.provider,
			&diagnostic) == ZEND_MIR_LOWERING_DEFERRED);
		assert(diagnostic == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
		assert(test_mutation_count(&test) == 0);
	}

	test.provider.hazards = ZEND_MIR_NUMERIC_HAZARD_REFERENCE;
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic == ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_BW_AND,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_F64, 0, 0, false),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_F64, 0, 0, false));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
	assert(test_mutation_count(&test) == 0);

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_DIV,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 2, 2, true));
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);
	assert(test_mutation_count(&test) == 0);

	test.source.zend_opcode_number = ZEND_MIR_NUMERIC_OPCODE_POW;
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_DEFERRED);
	assert(test_mutation_count(&test) == 0);
}

static void test_mutation_failure(void)
{
	test_case test;
	zend_mir_lowering_diagnostic_code diagnostic;

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_ADD,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 2, 2, true));
	test.host.fail_mutation_at = 3;
	assert(zend_mir_lower_numeric(
		&test.lowering, &test.source, &test.mutator, &test.provider,
		&diagnostic) == ZEND_MIR_LOWERING_FAILED);
	assert(diagnostic == ZEND_MIRL_MUTATION_FAILED);
	assert(test.host.instruction_count == 0);
	assert(test.host.fact_count == 0);
}

static void test_provider_set(void)
{
	test_case test;
	zend_mir_numeric_provider_set provider_set;
	zend_mir_lowering_provider provider;
	zend_mir_lowering_claim claim;
	uint32_t expected_ids[] = {
		ZEND_MIR_NUMERIC_ARITHMETIC_PROVIDER_ID,
		ZEND_MIR_NUMERIC_INTEGER_PROVIDER_ID,
		ZEND_MIR_NUMERIC_BITWISE_PROVIDER_ID
	};
	uint32_t expected_counts[] = {3, 3, 4};
	uint32_t expected_opcodes[][4] = {
		{ZEND_MIR_NUMERIC_OPCODE_ADD, ZEND_MIR_NUMERIC_OPCODE_SUB,
			ZEND_MIR_NUMERIC_OPCODE_MUL, 0},
		{ZEND_MIR_NUMERIC_OPCODE_MOD, ZEND_MIR_NUMERIC_OPCODE_SL,
			ZEND_MIR_NUMERIC_OPCODE_SR, 0},
		{ZEND_MIR_NUMERIC_OPCODE_BW_OR, ZEND_MIR_NUMERIC_OPCODE_BW_AND,
			ZEND_MIR_NUMERIC_OPCODE_BW_XOR,
			ZEND_MIR_NUMERIC_OPCODE_BW_NOT}
	};
	uint32_t provider_index;
	uint32_t claim_index;

	test_case_init(
		&test, ZEND_MIR_NUMERIC_OPCODE_ADD,
		test_fact(1, ZEND_MIR_SCALAR_TYPE_I64, 1, 1, true),
		test_fact(2, ZEND_MIR_SCALAR_TYPE_I64, 2, 2, true));
	assert(zend_mir_numeric_provider_set_init(
		&test.provider, &provider_set));
	assert(zend_mir_numeric_provider_count(&provider_set)
		== ZEND_MIR_NUMERIC_PROVIDER_COUNT);
	for (provider_index = 0;
			provider_index < ZEND_MIR_NUMERIC_PROVIDER_COUNT;
			provider_index++) {
		assert(zend_mir_numeric_provider_at(
			&provider_set, provider_index, &provider));
		assert(provider.provider_id == expected_ids[provider_index]);
		assert(provider.semantic_family_id == ZEND_MIR_NUMERIC_FAMILY_ID);
		assert(provider.claim_count(provider.context)
			== expected_counts[provider_index]);
		for (claim_index = 0;
				claim_index < expected_counts[provider_index];
				claim_index++) {
			assert(provider.claim_at(
				provider.context, claim_index, &claim));
			assert(claim.zend_opcode_number
				== expected_opcodes[provider_index][claim_index]);
			assert(claim.semantic_family_id
				== ZEND_MIR_NUMERIC_FAMILY_ID);
		}
	}
	assert(!zend_mir_numeric_provider_at(
		&provider_set, ZEND_MIR_NUMERIC_PROVIDER_COUNT, &provider));

	assert(zend_mir_numeric_provider_at(&provider_set, 0, &provider));
	test.lowering.provider_context = provider.context;
	assert(provider.lower(
		&test.lowering, &test.source, &test.mutator)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_numeric_provider_at(&provider_set, 1, &provider));
	test.lowering.provider_context = provider.context;
	assert(provider.lower(
		&test.lowering, &test.source, &test.mutator)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(test.lowering.failure_status == ZEND_MIR_LOWERING_DEFERRED);
	assert(test.lowering.failure_diagnostic == ZEND_MIRL_DEFERRED_OPCODE);
}

int main(void)
{
	test_range_proofs();
	test_integer_arithmetic();
	test_double_arithmetic();
	test_integer_special_operations();
	test_bitwise_operations();
	test_rejections_are_pre_mutation();
	test_deferred_paths();
	test_mutation_failure();
	test_provider_set();
	puts("W03-C numeric lowering C tests passed");
	return 0;
}
