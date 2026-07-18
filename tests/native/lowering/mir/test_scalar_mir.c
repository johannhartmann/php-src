#include "Zend/Native/MIR/Core/zend_mir_arena.h"
#include "Zend/Native/MIR/Scalar/zend_mir_scalar_descriptors.h"
#include "Zend/Native/MIR/Scalar/zend_mir_verify_scalar.h"
#include "Zend/Native/MIR/Semantics/zend_mir_effect_summary.h"
#include "Zend/Native/MIR/Text/zend_mir_dump.h"
#include "tests/native/mir/text/mir_test_parser.h"
#include "tests/native/mir/verify/fixtures/verify_fixtures.h"

#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAX_FACTS 8
#define TEST_ALLOCATIONS 64
#define TEST_TEXT_CAPACITY 65536

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
			__FILE__, __LINE__, #condition); \
		return false; \
	} \
} while (0)

typedef struct _test_diagnostics {
	uint32_t count;
	char messages[64][ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY];
	char last_message[ZEND_MIR_DIAGNOSTIC_MESSAGE_CAPACITY];
} test_diagnostics;

typedef struct _test_text {
	char bytes[TEST_TEXT_CAPACITY];
	size_t length;
} test_text;

typedef struct _test_allocator {
	void *raw[TEST_ALLOCATIONS];
	uint32_t raw_count;
	uint32_t allocation_count;
	uint32_t fail_at;
} test_allocator;

static zend_mir_value_fact_ref test_facts[TEST_MAX_FACTS];
static uint32_t test_fact_count;

static bool test_emit(void *context, const zend_mir_diagnostic *diagnostic)
{
	test_diagnostics *captured = (test_diagnostics *) context;

	captured->count++;
	if (captured->count <= 64) {
		memcpy(captured->messages[captured->count - 1], diagnostic->message,
			sizeof(captured->messages[0]));
		captured->messages[captured->count - 1]
			[sizeof(captured->messages[0]) - 1] = '\0';
	}
	memcpy(captured->last_message, diagnostic->message,
		sizeof(captured->last_message));
	captured->last_message[sizeof(captured->last_message) - 1] = '\0';
	return true;
}

static zend_mir_diagnostic_sink test_sink(test_diagnostics *captured)
{
	zend_mir_diagnostic_sink sink;

	memset(captured, 0, sizeof(*captured));
	sink.context = captured;
	sink.emit = test_emit;
	sink.limit = 64;
	sink.emitted = 0;
	return sink;
}

static uint32_t test_value_fact_count(const void *context)
{
	(void) context;
	return test_fact_count;
}

static bool test_value_fact_at(
		const void *context, uint32_t index, zend_mir_value_fact_ref *out)
{
	(void) context;
	if (out == NULL || index >= test_fact_count) {
		return false;
	}
	*out = test_facts[index];
	return true;
}

static void test_bind_facts(zend_mir_fixture_host *host)
{
	host->view.value_fact_count = test_value_fact_count;
	host->view.value_fact_at = test_value_fact_at;
}

static zend_mir_value_fact_ref test_i64_fact(
		zend_mir_value_fact_id id, zend_mir_value_id value,
		int64_t minimum, int64_t maximum)
{
	zend_mir_value_fact_ref fact;

	memset(&fact, 0, sizeof(fact));
	fact.id = id;
	fact.value_id = value;
	fact.exact_type = ZEND_MIR_SCALAR_TYPE_I64;
	fact.flags = ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	if (minimum > 0 || maximum < 0) {
		fact.flags |= ZEND_MIR_VALUE_FACT_NONZERO;
	}
	fact.integer_min = minimum;
	fact.integer_max = maximum;
	fact.provenance = ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS;
	fact.provenance_source_position_id = 0;
	return fact;
}

static zend_mir_value_fact_ref test_f64_fact(
		zend_mir_value_fact_id id, zend_mir_value_id value)
{
	zend_mir_value_fact_ref fact;

	memset(&fact, 0, sizeof(fact));
	fact.id = id;
	fact.value_id = value;
	fact.exact_type = ZEND_MIR_SCALAR_TYPE_F64;
	fact.flags = ZEND_MIR_VALUE_FACT_FINITE
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	fact.provenance = ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS;
	fact.provenance_source_position_id = 0;
	return fact;
}

static void test_build_valid(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record *instruction;

	zend_mir_verify_fixture_linear(host);
	test_bind_facts(host);
	host->values[0].id = 0;
	host->values[0].representation = ZEND_MIR_REPRESENTATION_I64;
	host->values[0].ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
	host->values[1] = host->values[0];
	host->values[1].id = 1;
	host->values[2] = host->values[0];
	host->values[2].id = 2;
	host->value_count = 3;

	host->constants[0].value_id = 0;
	host->constants[0].representation = ZEND_MIR_REPRESENTATION_I64;
	host->constants[0].kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	host->constants[0].payload_bits = 10;
	host->constants[0].symbol_id = ZEND_MIR_ID_INVALID;
	host->constants[1] = host->constants[0];
	host->constants[1].value_id = 1;
	host->constants[1].payload_bits = 1;
	host->constant_count = 2;

	instruction = &host->instructions[0];
	instruction->id = 0;
	instruction->opcode = ZEND_MIR_OPCODE_STATEPOINT;
	instruction->representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction->result_id = ZEND_MIR_ID_INVALID;
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	instruction->effects = 0;
	instruction->reads = 0;
	instruction->writes = 0;
	instruction->barriers = 0;
	instruction->ownership_actions = 0;

	instruction = &host->instructions[1];
	memset(instruction, 0, sizeof(*instruction));
	instruction->id = 1;
	instruction->block_id = 0;
	instruction->opcode = ZEND_MIR_OPCODE_CONSTANT;
	instruction->representation = ZEND_MIR_REPRESENTATION_I64;
	instruction->result_id = 0;
	instruction->frame_state_id = ZEND_MIR_ID_INVALID;
	instruction->source_position_id = ZEND_MIR_ID_INVALID;

	instruction = &host->instructions[2];
	*instruction = host->instructions[1];
	instruction->id = 2;
	instruction->result_id = 1;

	instruction = &host->instructions[3];
	memset(instruction, 0, sizeof(*instruction));
	instruction->id = 3;
	instruction->block_id = 0;
	instruction->opcode = ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW;
	instruction->representation = ZEND_MIR_REPRESENTATION_I64;
	instruction->result_id = 2;
	instruction->frame_state_id = ZEND_MIR_ID_INVALID;
	instruction->source_position_id = 0;

	instruction = &host->instructions[4];
	memset(instruction, 0, sizeof(*instruction));
	instruction->id = 4;
	instruction->block_id = 0;
	instruction->opcode = ZEND_MIR_OPCODE_RETURN;
	instruction->representation = ZEND_MIR_REPRESENTATION_VOID;
	instruction->result_id = ZEND_MIR_ID_INVALID;
	instruction->frame_state_id = 0;
	instruction->source_position_id = 0;
	host->instruction_count = 5;

	host->operands[0].instruction_id = 3;
	host->operands[0].value_id = 0;
	host->operands[1].instruction_id = 3;
	host->operands[1].value_id = 1;
	host->operands[2].instruction_id = 4;
	host->operands[2].value_id = 2;
	host->operand_count = 3;

	test_facts[0] = test_i64_fact(2, 2, 11, 22);
	test_facts[1] = test_i64_fact(0, 0, 10, 20);
	test_facts[2] = test_i64_fact(1, 1, 1, 2);
	test_fact_count = 3;
}

static void test_build_valid_f64(zend_mir_fixture_host *host)
{
	uint32_t index;

	test_build_valid(host);
	for (index = 0; index < 3; index++) {
		host->values[index].representation = ZEND_MIR_REPRESENTATION_DOUBLE;
		test_facts[index] = test_f64_fact(index, index);
	}
	for (index = 0; index < 2; index++) {
		host->constants[index].representation = ZEND_MIR_REPRESENTATION_DOUBLE;
		host->constants[index].kind = ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS;
	}
	host->instructions[1].representation = ZEND_MIR_REPRESENTATION_DOUBLE;
	host->instructions[2].representation = ZEND_MIR_REPRESENTATION_DOUBLE;
	host->instructions[3].opcode = ZEND_MIR_OPCODE_F64_ADD;
	host->instructions[3].representation = ZEND_MIR_REPRESENTATION_DOUBLE;
}

static zend_mir_value_fact_ref test_requirement_fact(
		zend_mir_value_fact_id id, zend_mir_value_id value_id,
		const zend_mir_scalar_value_requirement *requirement)
{
	zend_mir_value_fact_ref fact;

	memset(&fact, 0, sizeof(fact));
	fact.id = id;
	fact.value_id = value_id;
	fact.exact_type = requirement->exact_type == ZEND_MIR_SCALAR_TYPE_NONE
		? ZEND_MIR_SCALAR_TYPE_I64 : requirement->exact_type;
	fact.flags = requirement->required_flags;
	fact.provenance = ZEND_MIR_FACT_PROVENANCE_RANGE_ANALYSIS;
	fact.provenance_source_position_id = 0;
	if ((fact.flags & ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE) != 0) {
		fact.integer_min = 1;
		fact.integer_max = 1;
	}
	return fact;
}

static zend_mir_representation test_requirement_representation(
		const zend_mir_scalar_value_requirement *requirement)
{
	return requirement->representation == ZEND_MIR_REPRESENTATION_INVALID
		? ZEND_MIR_REPRESENTATION_I64 : requirement->representation;
}

static void test_set_constant(
		zend_mir_constant_record *constant, zend_mir_value_id value_id,
		zend_mir_representation representation)
{
	constant->value_id = value_id;
	constant->representation = representation;
	constant->kind = representation == ZEND_MIR_REPRESENTATION_DOUBLE
		? ZEND_MIR_CONSTANT_KIND_DOUBLE_BITS
		: ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
	constant->payload_bits = 1;
	constant->symbol_id = ZEND_MIR_ID_INVALID;
}

static void test_set_result_proof(
		zend_mir_opcode opcode, zend_mir_value_fact_ref *result)
{
	switch (opcode) {
		case ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW:
			result->integer_min = 2;
			result->integer_max = 2;
			break;
		case ZEND_MIR_OPCODE_I64_SUB_NO_OVERFLOW:
		case ZEND_MIR_OPCODE_I64_MOD_NONZERO:
		case ZEND_MIR_OPCODE_I64_SHR_CHECKED:
			result->integer_min = 0;
			result->integer_max = 0;
			break;
		case ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW:
			result->integer_min = 1;
			result->integer_max = 1;
			break;
		case ZEND_MIR_OPCODE_I64_SHL_CHECKED:
			result->integer_min = 2;
			result->integer_max = 2;
			break;
		case ZEND_MIR_OPCODE_I64_CMP:
		case ZEND_MIR_OPCODE_F64_CMP:
		case ZEND_MIR_OPCODE_F64_TO_I64_CHECKED:
			result->integer_min = -1;
			result->integer_max = 1;
			break;
		case ZEND_MIR_OPCODE_I1_TO_I64:
			result->integer_min = 0;
			result->integer_max = 1;
			break;
		default:
			break;
	}
}

static void test_build_descriptor_module(
		zend_mir_fixture_host *host,
		const zend_mir_scalar_descriptor *descriptor)
{
	zend_mir_representation operand_representations[2];
	uint32_t index;

	test_build_valid(host);
	for (index = 0; index < 2; index++) {
		const zend_mir_scalar_value_requirement *requirement =
			index < descriptor->operand_count
				? &descriptor->operands[index] : &descriptor->operands[0];

		operand_representations[index] =
			test_requirement_representation(requirement);
		host->values[index].representation = operand_representations[index];
		host->values[index].ownership = requirement->ownership;
		test_set_constant(&host->constants[index], index,
			operand_representations[index]);
		test_facts[index] = test_requirement_fact(index, index, requirement);
		host->instructions[index + 1].representation =
			operand_representations[index];
	}
	host->instructions[3].opcode = descriptor->opcode;
	host->instructions[3].representation = descriptor->has_result
		? descriptor->result.representation : ZEND_MIR_REPRESENTATION_VOID;
	host->instructions[3].result_id =
		descriptor->has_result ? 2 : ZEND_MIR_ID_INVALID;
	host->instructions[3].source_position_id = 0;
	host->instructions[3].frame_state_id = ZEND_MIR_ID_INVALID;
	host->instructions[3].effects = descriptor->effects;
	host->instructions[3].reads = descriptor->reads;
	host->instructions[3].writes = descriptor->writes;
	host->instructions[3].barriers = descriptor->barriers;
	host->instructions[3].ownership_actions = descriptor->ownership_actions;
	host->operand_count = 0;
	for (index = 0; index < descriptor->operand_count; index++) {
		host->operands[host->operand_count].instruction_id = 3;
		host->operands[host->operand_count++].value_id = index;
	}
	if (descriptor->has_result) {
		host->values[2].representation = descriptor->result.representation;
		host->values[2].ownership = descriptor->result.ownership;
		test_facts[2] = test_requirement_fact(2, 2, &descriptor->result);
		test_set_result_proof(descriptor->opcode, &test_facts[2]);
		host->operands[host->operand_count].instruction_id = 4;
		host->operands[host->operand_count++].value_id = 2;
		host->value_count = 3;
		test_fact_count = 3;
	} else {
		host->value_count = 2;
		test_fact_count = 2;
	}
}

static bool test_descriptor_catalog(void)
{
	uint32_t opcode;

	CHECK(zend_mir_scalar_descriptor_at(ZEND_MIR_OPCODE_CONSTANT) == NULL);
	CHECK(zend_mir_scalar_descriptor_at(ZEND_MIR_OPCODE_INVALID) == NULL);
	for (opcode = ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW;
			opcode < ZEND_MIR_OPCODE_COUNT; opcode++) {
		const zend_mir_scalar_descriptor *descriptor =
			zend_mir_scalar_descriptor_at((zend_mir_opcode) opcode);

		CHECK(descriptor != NULL);
		CHECK(descriptor->opcode == (zend_mir_opcode) opcode);
		CHECK(descriptor->label != NULL && descriptor->label[0] != '\0');
		CHECK(descriptor->operand_count >= 1);
		CHECK(descriptor->operand_count <= ZEND_MIR_SCALAR_MAX_OPERANDS);
		CHECK(descriptor->requires_source);
		CHECK(!descriptor->requires_frame);
		CHECK(descriptor->effects == 0);
		CHECK(descriptor->reads == 0);
		CHECK(descriptor->writes == 0);
		CHECK(descriptor->barriers == 0);
		CHECK(descriptor->ownership_actions == 0);
		if (descriptor->has_result) {
			CHECK(descriptor->result.exact_type != ZEND_MIR_SCALAR_TYPE_NONE);
			CHECK((descriptor->result.required_flags
				& ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) != 0);
		}
	}
	return true;
}

static bool test_every_descriptor_verifies(void)
{
	uint32_t opcode;

	for (opcode = ZEND_MIR_OPCODE_I64_ADD_NO_OVERFLOW;
			opcode < ZEND_MIR_OPCODE_COUNT; opcode++) {
		const zend_mir_scalar_descriptor *descriptor =
			zend_mir_scalar_descriptor_at((zend_mir_opcode) opcode);
		zend_mir_fixture_host host;
		test_diagnostics captured;
		zend_mir_diagnostic_sink sink;

		CHECK(descriptor != NULL);
		test_build_descriptor_module(&host, descriptor);
		sink = test_sink(&captured);
		if (!zend_mir_verify_w03_scalar(&host.view, &sink)) {
			fprintf(stderr, "descriptor %s failed verification: %s\n",
				descriptor->label, captured.last_message);
			return false;
		}
		CHECK(captured.count == 0);
	}
	return true;
}

static bool test_expect_invalid(zend_mir_fixture_host *host, const char *code)
{
	zend_mir_fixture_host snapshot = *host;
	zend_mir_value_fact_ref fact_snapshot[TEST_MAX_FACTS];
	test_diagnostics captured;
	zend_mir_diagnostic_sink sink = test_sink(&captured);
	uint32_t index;
	bool found = false;

	memcpy(fact_snapshot, test_facts, sizeof(fact_snapshot));
	CHECK(!zend_mir_verify_w03_scalar(&host->view, &sink));
	CHECK(captured.count != 0);
	CHECK(memcmp(&snapshot, host, sizeof(*host)) == 0);
	CHECK(memcmp(fact_snapshot, test_facts, sizeof(fact_snapshot)) == 0);
	for (index = 0; index < captured.count && index < 64; index++) {
		if (strstr(captured.messages[index], code) != NULL) {
			found = true;
		}
	}
	if (!found) {
		fprintf(stderr, "expected diagnostic %s; captured:\n", code);
		for (index = 0; index < captured.count && index < 64; index++) {
			fprintf(stderr, "  %s\n", captured.messages[index]);
		}
	}
	CHECK(found);
	return true;
}

static bool test_stage2_valid_and_nonmutation(void)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host snapshot;
	zend_mir_value_fact_ref fact_snapshot[TEST_MAX_FACTS];
	test_diagnostics captured;
	zend_mir_diagnostic_sink sink;

	test_build_valid(&host);
	snapshot = host;
	memcpy(fact_snapshot, test_facts, sizeof(fact_snapshot));
	sink = test_sink(&captured);
	CHECK(zend_mir_verify_w03_scalar(&host.view, &sink));
	CHECK(captured.count == 0);
	CHECK(memcmp(&snapshot, &host, sizeof(host)) == 0);
	CHECK(memcmp(fact_snapshot, test_facts, sizeof(fact_snapshot)) == 0);

	test_build_valid_f64(&host);
	sink = test_sink(&captured);
	CHECK(zend_mir_verify_w03_scalar(&host.view, &sink));
	CHECK(captured.count == 0);
	return true;
}

static bool test_stage2_negative_facts(void)
{
	zend_mir_fixture_host host;

	test_build_valid(&host);
	test_facts[1].id = test_facts[0].id;
	CHECK(test_expect_invalid(&host, "MIRV0611"));

	test_build_valid(&host);
	test_facts[1].value_id = test_facts[0].value_id;
	CHECK(test_expect_invalid(&host, "MIRV0612"));

	test_build_valid(&host);
	test_facts[1].exact_type =
		ZEND_MIR_SCALAR_TYPE_I64 | ZEND_MIR_SCALAR_TYPE_F64;
	CHECK(test_expect_invalid(&host, "MIRV0610"));

	test_build_valid(&host);
	test_facts[1].integer_min = 20;
	test_facts[1].integer_max = 10;
	CHECK(test_expect_invalid(&host, "MIRV0610"));

	test_build_valid(&host);
	test_facts[1].integer_min = INT64_MAX;
	test_facts[1].integer_max = INT64_MIN;
	test_facts[2].integer_min = INT64_MAX;
	test_facts[2].integer_max = INT64_MIN;
	CHECK(test_expect_invalid(&host, "MIRV0610"));

	test_build_valid(&host);
	test_facts[1].flags |= UINT32_C(1) << 31;
	CHECK(test_expect_invalid(&host, "MIRV0610"));

	test_build_valid(&host);
	test_fact_count = 2;
	CHECK(test_expect_invalid(&host, "MIRV0613"));

	test_build_valid(&host);
	test_facts[1].provenance_source_position_id = ZEND_MIR_ID_INVALID;
	CHECK(test_expect_invalid(&host, "MIRV0626"));
	return true;
}

static bool test_stage2_negative_contracts(void)
{
	zend_mir_fixture_host host;
	const zend_mir_scalar_descriptor *descriptor;

	test_build_valid(&host);
	test_facts[0].integer_min = 12;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	test_build_valid(&host);
	test_facts[1].integer_max = INT64_MAX;
	test_facts[2].integer_max = 2;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	test_build_valid(&host);
	host.instructions[3].reads = ZEND_MIR_MEMORY_DOMAIN_MASK(
		ZEND_MIR_MEMORY_DOMAIN_FRAME_ARGS);
	CHECK(test_expect_invalid(&host, "MIRV0624"));

	test_build_valid(&host);
	host.values[2].ownership = ZEND_MIR_OWNERSHIP_STATE_BORROWED;
	CHECK(test_expect_invalid(&host, "MIRV0625"));

	test_build_valid(&host);
	host.instructions[3].source_position_id = ZEND_MIR_ID_INVALID;
	CHECK(test_expect_invalid(&host, "MIRV0626"));

	test_build_valid(&host);
	test_facts[0].flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	test_facts[0].integer_min = 0;
	test_facts[0].integer_max = 0;
	CHECK(test_expect_invalid(&host, "MIRV0622"));

	test_build_valid(&host);
	host.instructions[3].opcode = (zend_mir_opcode) ZEND_MIR_OPCODE_COUNT;
	CHECK(test_expect_invalid(&host, "MIRV0102"));

	test_build_valid_f64(&host);
	test_facts[2].provenance = ZEND_MIR_FACT_PROVENANCE_SSA;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(
		ZEND_MIR_OPCODE_I64_MUL_NO_OVERFLOW);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[0].integer_min = INT64_MAX;
	test_facts[0].integer_max = INT64_MAX;
	test_facts[1].integer_min = 2;
	test_facts[1].integer_max = 2;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(
		ZEND_MIR_OPCODE_I64_MOD_NONZERO);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[0].integer_min = INT64_MIN;
	test_facts[0].integer_max = INT64_MIN;
	test_facts[1].integer_min = -1;
	test_facts[1].integer_max = -1;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(
		ZEND_MIR_OPCODE_I64_SHL_CHECKED);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[1].integer_min = 64;
	test_facts[1].integer_max = 64;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(
		ZEND_MIR_OPCODE_I64_SHR_CHECKED);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[1].integer_min = 64;
	test_facts[1].integer_max = 64;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(ZEND_MIR_OPCODE_I64_CMP);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[2].integer_min = 0;
	test_facts[2].integer_max = 0;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(ZEND_MIR_OPCODE_I1_TO_I64);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[2].integer_min = 1;
	test_facts[2].integer_max = 1;
	CHECK(test_expect_invalid(&host, "MIRV0623"));

	descriptor = zend_mir_scalar_descriptor_at(
		ZEND_MIR_OPCODE_F64_TO_I64_CHECKED);
	CHECK(descriptor != NULL);
	test_build_descriptor_module(&host, descriptor);
	test_facts[2].provenance = ZEND_MIR_FACT_PROVENANCE_SSA;
	CHECK(test_expect_invalid(&host, "MIRV0623"));
	return true;
}

static bool test_stage2_negative_scope(void)
{
	zend_mir_fixture_host host;
	zend_mir_effect_summary combined;
	zend_mir_effect_summary expanded;
	zend_mir_effect_mask handled = 0;
	zend_mir_frame_state_ref *frame;
	zend_mir_source_map_ref *map;
	uint32_t effect;

	test_build_valid(&host);
	CHECK(zend_mir_effect_summary_from_effect(
		ZEND_MIR_EFFECT_THROW, &combined));
	while ((combined.effects & ~handled) != 0) {
		zend_mir_effect_mask pending = combined.effects & ~handled;

		for (effect = 0; effect < ZEND_MIR_EFFECT_COUNT; effect++) {
			zend_mir_effect_summary atomic;
			zend_mir_effect_mask bit = ZEND_MIR_EFFECT_MASK(effect);

			if ((pending & bit) == 0) {
				continue;
			}
			CHECK(zend_mir_effect_summary_from_effect(
				(zend_mir_effect) effect, &atomic));
			CHECK(zend_mir_effect_summary_compose(
				&expanded, &combined, &atomic));
			combined = expanded;
			handled |= bit;
		}
	}
	host.instructions[4].opcode = ZEND_MIR_OPCODE_THROW;
	host.instructions[4].effects = combined.effects;
	host.instructions[4].reads = combined.reads;
	host.instructions[4].writes = combined.writes;
	host.instructions[4].barriers = combined.barriers;
	frame = &host.frame_states[host.frame_state_count];
	*frame = host.frame_states[0];
	frame->id = host.frame_state_count++;
	frame->safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_OBSERVER;
	frame->slots.offset = host.frame_slot_count;
	frame->slots.count = 1;
	host.frame_slots[host.frame_slot_count].slot_id = host.frame_slot_count;
	host.frame_slots[host.frame_slot_count].value_id = 2;
	host.frame_slots[host.frame_slot_count].kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
	host.frame_slots[host.frame_slot_count].representation =
		ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	host.frame_slots[host.frame_slot_count].materialization =
		ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	host.frame_slots[host.frame_slot_count++].ownership =
		ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	map = &host.source_maps[host.source_map_count];
	*map = host.source_maps[0];
	map->id = host.source_map_count++;
	map->owner_frame_id = frame->id;
	host.instructions[4].frame_state_id = frame->id;
	CHECK(test_expect_invalid(&host, "MIRV0627"));
	return true;
}

static bool test_stage2_oom(void)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host snapshot;
	zend_mir_value_fact_ref fact_snapshot[TEST_MAX_FACTS];
	test_diagnostics captured;
	zend_mir_diagnostic_sink sink;
	uint32_t allocation;

	for (allocation = 0; allocation < 5; allocation++) {
		test_build_valid(&host);
		snapshot = host;
		memcpy(fact_snapshot, test_facts, sizeof(fact_snapshot));
		sink = test_sink(&captured);
		zend_mir_verify_scalar_test_fail_allocation_after(allocation);
		CHECK(!zend_mir_verify_w03_scalar(&host.view, &sink));
		CHECK(strstr(captured.last_message, "MIRV0603") != NULL);
		CHECK(memcmp(&snapshot, &host, sizeof(host)) == 0);
		CHECK(memcmp(fact_snapshot, test_facts, sizeof(fact_snapshot)) == 0);
	}
	test_build_valid(&host);
	sink = test_sink(&captured);
	zend_mir_verify_scalar_test_fail_allocation_after(5);
	CHECK(zend_mir_verify_w03_scalar(&host.view, &sink));
	CHECK(captured.count == 0);
	return true;
}

static bool test_text_write(void *context, const char *bytes, size_t length)
{
	test_text *text = (test_text *) context;

	if (length > sizeof(text->bytes) - text->length) {
		return false;
	}
	memcpy(text->bytes + text->length, bytes, length);
	text->length += length;
	return true;
}

static bool test_dump(
		const zend_mir_view *view, test_text *text)
{
	zend_mir_text_writer writer;

	memset(text, 0, sizeof(*text));
	writer.context = text;
	writer.write = test_text_write;
	return zend_mir_dump_text(view, &writer, NULL);
}

static bool test_parse_replacement(
		const test_text *source, const char *from, const char *to,
		zend_mir_test_text_error_code expected)
{
	zend_mir_fixture_host parsed;
	zend_mir_test_text_error error;
	char bytes[TEST_TEXT_CAPACITY];
	char *match;
	size_t length = strlen(from);

	CHECK(source->length < sizeof(bytes));
	CHECK(length == strlen(to));
	memcpy(bytes, source->bytes, source->length);
	bytes[source->length] = '\0';
	match = strstr(bytes, from);
	CHECK(match != NULL);
	memcpy(match, to, length);
	CHECK(!zend_mir_test_parse_text(bytes, source->length, &parsed, &error));
	CHECK(error.code == expected);
	return true;
}

static bool test_text_facts(void)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host parsed;
	zend_mir_test_text_error error;
	test_text without_callbacks;
	test_text empty_callbacks;
	test_text with_facts;
	test_text without_range;
	const char *fact0;
	const char *fact1;
	const char *fact2;

	test_build_valid(&host);
	host.view.value_fact_count = NULL;
	host.view.value_fact_at = NULL;
	CHECK(test_dump(&host.view, &without_callbacks));
	test_bind_facts(&host);
	test_fact_count = 0;
	CHECK(test_dump(&host.view, &empty_callbacks));
	CHECK(without_callbacks.length == empty_callbacks.length);
	CHECK(memcmp(without_callbacks.bytes, empty_callbacks.bytes,
		without_callbacks.length) == 0);

	test_build_valid(&host);
	test_facts[0].integer_min = INT64_MIN;
	test_facts[0].flags &= ~ZEND_MIR_VALUE_FACT_NONZERO;
	CHECK(test_dump(&host.view, &with_facts));
	CHECK(with_facts.length < sizeof(with_facts.bytes));
	with_facts.bytes[with_facts.length] = '\0';
	fact0 = strstr(with_facts.bytes, "fact vf0 value v0 ");
	fact1 = strstr(with_facts.bytes, "fact vf1 value v1 ");
	fact2 = strstr(with_facts.bytes, "fact vf2 value v2 ");
	CHECK(fact0 != NULL && fact1 != NULL && fact2 != NULL);
	CHECK(fact0 < fact1 && fact1 < fact2);
	CHECK(strstr(with_facts.bytes, "opcode i64_add_no_overflow ") != NULL);
	CHECK(strstr(with_facts.bytes,
		"range -9223372036854775808:22") != NULL);
	CHECK(zend_mir_test_parse_text(with_facts.bytes, with_facts.length,
		&parsed, &error));
	CHECK(test_parse_replacement(&with_facts,
		"fact vf1 value v1", "fact vf0 value v1",
		ZEND_MIR_TEST_TEXT_DUPLICATE_ID));
	CHECK(test_parse_replacement(&with_facts,
		"fact vf1 value v1", "fact vf1 value v0",
		ZEND_MIR_TEST_TEXT_DUPLICATE_ID));
	CHECK(test_parse_replacement(&with_facts,
		"range 10:20", "range 20:10",
		ZEND_MIR_TEST_TEXT_NONCANONICAL));
	CHECK(test_parse_replacement(&with_facts,
		"flags 0x0000000b", "flags 0x8000000b",
		ZEND_MIR_TEST_TEXT_NONCANONICAL));

	test_build_valid(&host);
	test_facts[0].flags = ZEND_MIR_VALUE_FACT_NONZERO
		| ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	test_facts[0].integer_min = 0;
	test_facts[0].integer_max = 0;
	CHECK(test_dump(&host.view, &without_range));
	CHECK(zend_mir_test_parse_text(without_range.bytes, without_range.length,
		&parsed, &error));

	test_fact_count = UINT32_C(1048577);
	{
		test_diagnostics captured;
		zend_mir_diagnostic_sink sink = test_sink(&captured);
		zend_mir_text_writer writer;
		test_text bounded;
		writer.context = &bounded;
		writer.write = test_text_write;
		memset(&bounded, 0, sizeof(bounded));
		CHECK(!zend_mir_dump_text(&host.view, &writer, &sink));
		CHECK(captured.count == 1);
	}
	test_fact_count = 3;
	return true;
}

static void *test_allocate(void *context, size_t size, size_t alignment)
{
	test_allocator *allocator = (test_allocator *) context;
	uintptr_t address;
	void *raw;

	allocator->allocation_count++;
	if (allocator->allocation_count == allocator->fail_at
			|| allocator->raw_count >= TEST_ALLOCATIONS
			|| alignment == 0 || (alignment & (alignment - 1)) != 0
			|| size > SIZE_MAX - alignment) {
		return NULL;
	}
	raw = malloc(size + alignment);
	if (raw == NULL) {
		return NULL;
	}
	address = ((uintptr_t) raw + alignment - 1)
		& ~(uintptr_t) (alignment - 1);
	allocator->raw[allocator->raw_count++] = raw;
	return (void *) address;
}

static void test_reset(void *context)
{
	test_allocator *allocator = (test_allocator *) context;
	uint32_t index;

	for (index = 0; index < allocator->raw_count; index++) {
		free(allocator->raw[index]);
	}
	allocator->raw_count = 0;
}

static bool test_core_fact_storage_once(uint32_t fail_at, bool expect_fact)
{
	test_allocator allocator;
	zend_mir_allocator vtable;
	zend_mir_module *module;
	zend_mir_mutator *mutator;
	const zend_mir_view *view;
	zend_mir_value_fact_ref requested;
	zend_mir_value_fact_ref stored;
	zend_mir_value_fact_id id = 99;

	memset(&allocator, 0, sizeof(allocator));
	allocator.fail_at = fail_at;
	vtable.context = &allocator;
	vtable.allocate = test_allocate;
	vtable.reset = test_reset;
	module = zend_mir_module_create(9, &vtable, 64, NULL, NULL);
	CHECK(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	view = zend_mir_module_get_view(module);
	CHECK(mutator != NULL && view != NULL);
	CHECK(mutator->add_value(mutator->context, 7,
		ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	requested = test_i64_fact(77, 7, -4, 9);
	if (expect_fact) {
		CHECK(mutator->add_value_fact(mutator->context, &requested, &id));
		CHECK(id == 0);
		CHECK(view->value_fact_count(view->context) == 1);
		CHECK(view->value_fact_at(view->context, 0, &stored));
		CHECK(stored.id == 0 && stored.value_id == 7);
		CHECK(zend_mir_module_finalize(module));
		CHECK(zend_mir_module_get_mutator(module) == NULL);
		id = 88;
		CHECK(!mutator->add_value_fact(mutator->context, &requested, &id));
		CHECK(id == 88);
		CHECK(view->value_fact_count(view->context) == 1);
	} else {
		CHECK(!mutator->add_value_fact(mutator->context, &requested, &id));
		CHECK(id == 99);
		CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FAILED);
	}
	zend_mir_module_destroy(module);
	CHECK(allocator.raw_count == 0);
	return true;
}

static bool test_core_fact_storage(void)
{
	test_allocator allocator;
	zend_mir_allocator vtable;
	zend_mir_module *module;
	zend_mir_mutator *mutator;
	zend_mir_value_fact_ref fact;
	zend_mir_value_fact_id id;

	CHECK(test_core_fact_storage_once(0, true));
	CHECK(test_core_fact_storage_once(4, false));

	memset(&allocator, 0, sizeof(allocator));
	vtable.context = &allocator;
	vtable.allocate = test_allocate;
	vtable.reset = test_reset;
	module = zend_mir_module_create(10, &vtable, 64, NULL, NULL);
	CHECK(module != NULL);
	mutator = zend_mir_module_get_mutator(module);
	CHECK(mutator->add_value(mutator->context, 1,
		ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	fact = test_i64_fact(0, 1, 1, 2);
	CHECK(mutator->add_value_fact(mutator->context, &fact, &id));
	id = 42;
	CHECK(!mutator->add_value_fact(mutator->context, &fact, &id));
	CHECK(id == 42);
	CHECK(zend_mir_module_get_state(module) == ZEND_MIR_MODULE_FAILED);
	zend_mir_module_destroy(module);
	return true;
}

int main(void)
{
	if (!test_descriptor_catalog()
			|| !test_every_descriptor_verifies()
			|| !test_core_fact_storage()
			|| !test_stage2_valid_and_nonmutation()
			|| !test_stage2_negative_facts()
			|| !test_stage2_negative_contracts()
			|| !test_stage2_negative_scope()
			|| !test_stage2_oom()
			|| !test_text_facts()) {
		return 1;
	}
	puts("W03 scalar MIR facts, descriptors, text, and verifier tests passed");
	return 0;
}
