/*
  +----------------------------------------------------------------------+
  | Copyright © The PHP Group and Contributors.                          |
  +----------------------------------------------------------------------+
  | SPDX-License-Identifier: BSD-3-Clause                                |
  +----------------------------------------------------------------------+
*/

#include <assert.h>
#include <string.h>

#include "Zend/Native/Lowering/StraightLine/zend_mir_straight_line.h"
#include "Zend/Native/MIR/Verify/zend_mir_verify.h"
#include "tests/native/mir/contracts/fixture_host.h"

#define TEST_VALUE_CAPACITY 16
#define TEST_USE_CAPACITY 16
#define TEST_DIAGNOSTIC_CAPACITY 64

typedef struct _test_source {
	zend_mir_source_ssa_use_ref uses[TEST_USE_CAPACITY];
	uint32_t use_count;
} test_source;

typedef struct _test_diagnostics {
	zend_mir_diagnostic records[TEST_DIAGNOSTIC_CAPACITY];
	uint32_t count;
} test_diagnostics;

struct _zend_mir_lowering_context {
	const void *provider_context;
	zend_mir_function_id function_id;
	zend_mir_block_id block_id;
	zend_mir_lowering_status failure_status;
	zend_mir_lowering_diagnostic_code failure_code;
};

typedef struct _test_case {
	zend_mir_fixture_host host;
	zend_mir_value_fact_ref facts[TEST_VALUE_CAPACITY];
	uint32_t fact_count;
	struct _zend_mir_lowering_context lowering;
	test_source source;
	zend_mir_lowering_source_view source_view;
	zend_mir_straight_line_value value_storage[TEST_VALUE_CAPACITY];
	zend_mir_straight_line_lifetime lifetime;
	zend_mir_straight_line_slot slots[4];
	zend_mir_straight_line_entry entry;
	zend_mir_straight_line_provider_context provider_context;
} test_case;

static bool (*test_add_source_position_delegate)(
	void *context, const zend_mir_source_position_ref *source,
	zend_mir_source_position_id *out);
static uint32_t test_source_positions_before_failure;

const void *zend_mir_lowering_context_provider_context(
	const zend_mir_lowering_context *context)
{
	return context != NULL ? context->provider_context : NULL;
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

bool zend_mir_lowering_context_set_provider_failure(
	zend_mir_lowering_context *context, zend_mir_lowering_status status,
	zend_mir_lowering_diagnostic_code diagnostic)
{
	if (context == NULL || status == ZEND_MIR_LOWERING_SUCCESS
			|| diagnostic == ZEND_MIRL_OK) {
		return false;
	}
	context->failure_status = status;
	context->failure_code = diagnostic;
	return true;
}

static uint32_t test_ssa_use_count(const void *context)
{
	return ((const test_source *) context)->use_count;
}

static bool test_ssa_use_at(
	const void *context, uint32_t index, zend_mir_source_ssa_use_ref *out)
{
	const test_source *source = (const test_source *) context;

	if (out == NULL || index >= source->use_count) {
		return false;
	}
	*out = source->uses[index];
	return true;
}

static bool test_source_position_at(
	const void *context, zend_mir_source_position_id requested_id,
	zend_mir_source_position_ref *out)
{
	(void) context;
	if (out == NULL || !zend_mir_id_is_valid(requested_id)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->id = requested_id;
	out->file_symbol_id = 500;
	out->line = requested_id + 1;
	out->column_start = 1;
	out->column_end = 2;
	return true;
}

static bool test_faulting_add_source_position(
	void *context, const zend_mir_source_position_ref *source,
	zend_mir_source_position_id *out)
{
	if (test_source_positions_before_failure == 0) {
		return false;
	}
	test_source_positions_before_failure--;
	return test_add_source_position_delegate(context, source, out);
}

static bool test_resolve_operand(
	const void *context, const zend_mir_source_operand_ref *operand,
	zend_mir_value_id *value_id_out)
{
	(void) context;
	if (operand == NULL || value_id_out == NULL
			|| operand->ssa_variable_id > ZEND_MIR_VALUE_ORIGINAL_MAX) {
		return false;
	}
	*value_id_out =
		zend_mir_value_from_original_ssa(operand->ssa_variable_id);
	return true;
}

static bool test_add_value_fact(
	void *context, const zend_mir_value_fact_ref *fact,
	zend_mir_value_fact_id *out)
{
	test_case *test = (test_case *) context;

	if (fact == NULL || out == NULL
			|| !zend_mir_scalar_type_is_exact(fact->exact_type)
			|| (fact->flags & ZEND_MIR_VALUE_FACT_NON_REFCOUNTED) == 0
			|| test->fact_count >= TEST_VALUE_CAPACITY) {
		return false;
	}
	test->facts[test->fact_count] = *fact;
	*out = test->fact_count;
	test->fact_count++;
	return true;
}

static bool test_emit_diagnostic(
	void *context, const zend_mir_diagnostic *diagnostic)
{
	test_diagnostics *diagnostics = (test_diagnostics *) context;

	if (diagnostics->count >= TEST_DIAGNOSTIC_CAPACITY) {
		return false;
	}
	diagnostics->records[diagnostics->count++] = *diagnostic;
	return true;
}

static bool test_has_token(const test_diagnostics *diagnostics, const char *token)
{
	uint32_t index;

	for (index = 0; index < diagnostics->count; index++) {
		if (strstr(diagnostics->records[index].message, token) != NULL) {
			return true;
		}
	}
	return false;
}

static zend_mir_straight_line_proof_mask test_all_proofs(void)
{
	return ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_CALLS
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_REENTRY
		| ZEND_MIR_STRAIGHT_LINE_PROOF_EXACT_SCALAR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NOT_BY_REFERENCE
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_OBSERVER
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_DESTRUCTOR
		| ZEND_MIR_STRAIGHT_LINE_PROOF_NO_EXCEPTION;
}

static zend_mir_straight_line_value test_value(
	zend_mir_value_id id, zend_mir_representation representation,
	zend_mir_ownership_state ownership, zend_mir_scalar_type_mask type)
{
	zend_mir_straight_line_value value;

	memset(&value, 0, sizeof(value));
	value.value_id = id;
	value.representation = representation;
	value.ownership = ownership;
	value.exact_type = type;
	value.fact_flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED;
	return value;
}

static void test_case_init(test_case *test,
	const zend_mir_straight_line_value *initial)
{
	zend_mir_function_id function_id;
	zend_mir_block_id block_id;

	memset(test, 0, sizeof(*test));
	zend_mir_fixture_host_init(&test->host, 7);
	test->host.mutator.add_value_fact = test_add_value_fact;
	assert(test->host.mutator.add_function(
		test->host.mutator.context, 100, &function_id));
	assert(test->host.mutator.add_block(
		test->host.mutator.context, function_id, &block_id));
	assert(test->host.mutator.set_entry_block(
		test->host.mutator.context, function_id, block_id));
	assert(test->host.mutator.add_value(
		test->host.mutator.context, initial->value_id,
		initial->representation, initial->ownership));
	assert(zend_mir_straight_line_lifetime_init(
		&test->lifetime, test->value_storage, TEST_VALUE_CAPACITY));
	assert(zend_mir_straight_line_track_value(&test->lifetime, initial));

	test->source_view.contract_version = ZEND_MIR_CONTRACT_VERSION;
	test->source_view.context = &test->source;
	test->source_view.ssa_use_count = test_ssa_use_count;
	test->source_view.ssa_use_at = test_ssa_use_at;

	test->slots[0].slot_id = 1;
	test->slots[0].index = 0;
	test->slots[0].kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
	test->slots[0].value_id = initial->value_id;
	test->slots[0].value_representation = initial->representation;
	test->slots[0].materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	test->slots[0].ownership =
		initial->ownership == ZEND_MIR_OWNERSHIP_STATE_BORROWED
			? ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED
			: ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	test->slots[1].slot_id = 2;
	test->slots[1].index = 1;
	test->slots[1].kind = ZEND_MIR_FRAME_SLOT_KIND_TMP;
	test->slots[1].value_id = ZEND_MIR_ID_INVALID;
	test->slots[1].value_representation = ZEND_MIR_REPRESENTATION_INVALID;
	test->slots[1].materialization = ZEND_MIR_MATERIALIZATION_UNDEF;
	test->slots[1].ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;

	test->entry.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	test->entry.op_array_id = 300;
	test->entry.code_version_id = 1;
	test->entry.slots = test->slots;
	test->entry.slot_count = 2;
	test->entry.source_context = test;
	test->entry.source_position_at = test_source_position_at;
	test->entry.resolve_operand = test_resolve_operand;

	test->provider_context.source = &test->source_view;
	test->provider_context.lifetime = &test->lifetime;
	test->provider_context.entry = &test->entry;
	test->provider_context.proofs = test_all_proofs();
	test->lowering.provider_context = &test->provider_context;
	test->lowering.function_id = function_id;
	test->lowering.block_id = block_id;
}

static zend_mir_source_opcode_ref test_opcode(
	uint32_t opcode, uint32_t opline, uint32_t source_ssa, uint32_t result_ssa)
{
	zend_mir_source_opcode_ref source;

	memset(&source, 0, sizeof(source));
	source.opline_index = opline;
	source.zend_opcode_number = opcode;
	source.source_position_id = opline;
	source.op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.op1.slot_kind = ZEND_MIR_SOURCE_SLOT_TMP;
	source.op1.index = 0;
	source.op1.ssa_variable_id = source_ssa;
	source.result.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.result.slot_kind = ZEND_MIR_SOURCE_SLOT_TMP;
	source.result.index = 1;
	source.result.ssa_variable_id = result_ssa;
	return source;
}

static bool test_verify_host(
	zend_mir_fixture_host *host, test_diagnostics *collected)
{
	zend_mir_diagnostic_sink sink;

	memset(collected, 0, sizeof(*collected));
	memset(&sink, 0, sizeof(sink));
	sink.context = collected;
	sink.emit = test_emit_diagnostic;
	sink.limit = TEST_DIAGNOSTIC_CAPACITY;
	return zend_mir_verify_stage1(&host->view, &sink);
}

static void test_copy_and_move(void)
{
	test_case test;
	test_diagnostics diagnostics;
	zend_mir_straight_line_value initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
	zend_mir_source_opcode_ref copy =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 0, 0, 1);
	zend_mir_straight_line_value result;
	zend_mir_lowering_diagnostic_code code;

	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(code == ZEND_MIRL_OK);
	assert(test.host.instructions[1].opcode == ZEND_MIR_OPCODE_COPY);
	assert(test.host.instructions[1].ownership_actions
		== ZEND_MIR_OWNERSHIP_ACTION_MASK(ZEND_MIR_OWNERSHIP_ACTION_MOVE));
	assert(zend_mir_straight_line_value_at(&test.lifetime, 0, &result));
	assert(result.ownership == ZEND_MIR_OWNERSHIP_STATE_MOVED);
	assert(zend_mir_straight_line_value_at(&test.lifetime, 1, &result));
	assert(result.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED);

	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.uses[1].ssa_variable_id = 0;
	test.source.uses[1].opline_index = 1;
	test.source.use_count = 2;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(test.host.instructions[1].ownership_actions
		== ZEND_MIR_OWNERSHIP_ACTION_MASK(
			ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED));
	assert(zend_mir_straight_line_value_at(&test.lifetime, 0, &result));
	assert(result.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED);
	assert(zend_mir_straight_line_value_at(&test.lifetime, 1, &result));
	assert(result.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED);

	/*
	 * The independent scalar copy must remain usable after its source is
	 * dropped; a borrowed result would violate the lender lifetime here.
	 */
	{
		zend_mir_source_opcode_ref drop =
			test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE, 1, 0, 2);
		zend_mir_source_opcode_ref ret =
			test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN, 2, 1, 2);

		assert(zend_mir_lower_free(
			&test.lowering, &drop, &test.host.mutator,
			&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
		assert(zend_mir_lower_return(
			&test.lowering, &ret, &test.host.mutator,
			&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
		assert(test.host.instructions[2].opcode
			== ZEND_MIR_OPCODE_SCALAR_DROP);
		assert(test.host.instructions[3].opcode == ZEND_MIR_OPCODE_RETURN);
	}

	initial.fact_flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
	initial.integer_min = -5;
	initial.integer_max = 9;
	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(test.fact_count == 1);
	assert(test.facts[0].flags
		== (ZEND_MIR_VALUE_FACT_NON_REFCOUNTED
			| ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE));
	assert(test.facts[0].integer_min == -5);
	assert(test.facts[0].integer_max == 9);

	initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_BORROWED, ZEND_MIR_SCALAR_TYPE_I64);
	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(test.host.instructions[1].ownership_actions
		== ZEND_MIR_OWNERSHIP_ACTION_MASK(
			ZEND_MIR_OWNERSHIP_ACTION_PRODUCE_OWNED));
	assert(zend_mir_straight_line_value_at(&test.lifetime, 0, &result));
	assert(result.ownership == ZEND_MIR_OWNERSHIP_STATE_BORROWED);
	assert(zend_mir_straight_line_value_at(&test.lifetime, 1, &result));
	assert(result.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED);
	{
		zend_mir_source_opcode_ref ret =
			test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN, 1, 1, 2);

		assert(zend_mir_lower_return(
			&test.lowering, &ret, &test.host.mutator,
			&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
		assert(test_verify_host(&test.host, &diagnostics));
	}
}

static void test_duplicate_and_undef_rejected(void)
{
	test_case test;
	zend_mir_straight_line_value initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
	zend_mir_source_opcode_ref copy =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 0, 0, 1);
	zend_mir_source_opcode_ref duplicate =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 1, 0, 2);
	zend_mir_lowering_diagnostic_code code;

	test_case_init(&test, &initial);
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(test.host.instruction_count == 0);

	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.uses[0].operand_index = 0;
	test.source.uses[1] = test.source.uses[0];
	test.source.use_count = 2;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(test.host.instruction_count == 0);

	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_lower_copy_move(
		&test.lowering, &duplicate, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_INVALID_SOURCE);

	test_case_init(&test, &initial);
	test.value_storage[0].ownership = ZEND_MIR_OWNERSHIP_STATE_MOVED;
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 1;
	test.source.uses[0].operand_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &duplicate, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);
	assert(test.host.instruction_count == 0);

	test_case_init(&test, &initial);
	copy.op1.ssa_variable_id = 9;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);

	test_case_init(&test, &initial);
	copy = test_opcode(
		ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 0, 0, 1);
	copy.result.index = 99;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(test.host.instruction_count == 0);
}

static void test_lifetime_and_entry_validation(void)
{
	test_case test;
	zend_mir_straight_line_value invalid = test_value(
		0, ZEND_MIR_REPRESENTATION_DOUBLE,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
	zend_mir_straight_line_value storage[2];
	zend_mir_straight_line_lifetime lifetime;
	zend_mir_straight_line_value initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_BORROWED, ZEND_MIR_SCALAR_TYPE_I64);
	zend_mir_source_opcode_ref source =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN, 0, 0, 1);
	zend_mir_lowering_diagnostic_code code;

	assert(zend_mir_straight_line_lifetime_init(
		&lifetime, storage, sizeof(storage) / sizeof(storage[0])));
	assert(!zend_mir_straight_line_track_value(&lifetime, &invalid));
	assert(lifetime.count == 0);
	invalid = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
	invalid.fact_flags |= ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE
		| ZEND_MIR_VALUE_FACT_NONZERO;
	invalid.integer_min = -1;
	invalid.integer_max = 1;
	assert(!zend_mir_straight_line_track_value(&lifetime, &invalid));
	assert(lifetime.count == 0);
	invalid.integer_min = 1;
	invalid.integer_max = 2;
	assert(zend_mir_straight_line_track_value(&lifetime, &invalid));
	invalid.integer_max = 3;
	assert(!zend_mir_straight_line_track_value(&lifetime, &invalid));
	assert(lifetime.count == 1);

	test_case_init(&test, &initial);
	test.slots[1].slot_id = test.slots[0].slot_id;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);
	assert(test.host.instruction_count == 0);
	assert(test.host.frame_state_count == 0);
	assert(test.host.frame_slot_count == 0);
	assert(!test.lifetime.entry_emitted);
}

static void test_entry_and_scalar_returns(void)
{
	static const struct {
		zend_mir_representation representation;
		zend_mir_scalar_type_mask type;
	} cases[] = {
		{ZEND_MIR_REPRESENTATION_ZVAL, ZEND_MIR_SCALAR_TYPE_NULL},
		{ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_SCALAR_TYPE_I1},
		{ZEND_MIR_REPRESENTATION_I64, ZEND_MIR_SCALAR_TYPE_I64},
		{ZEND_MIR_REPRESENTATION_DOUBLE, ZEND_MIR_SCALAR_TYPE_F64}
	};
	uint32_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		test_case test;
		test_diagnostics diagnostics;
		zend_mir_straight_line_value initial = test_value(
			0, cases[index].representation,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED, cases[index].type);
		zend_mir_source_opcode_ref source =
			test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN, 0, 0, 1);
		zend_mir_lowering_diagnostic_code code;

		test_case_init(&test, &initial);
		assert(zend_mir_lower_return(
			&test.lowering, &source, &test.host.mutator,
			&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
		assert(test.host.instruction_count == 2);
		assert(test.host.instructions[0].opcode == ZEND_MIR_OPCODE_STATEPOINT);
		assert(test.host.instructions[1].opcode == ZEND_MIR_OPCODE_RETURN);
		assert(test.host.frame_state_count == 2);
		assert(test.host.frame_states[0].canonical);
		assert(test.host.frame_states[0].safepoint_class
			== ZEND_MIR_SAFEPOINT_CLASS_FUNCTION_ENTRY);
		assert(test.host.frame_states[0].slots.count == 2);
		assert(test.host.frame_states[0].roots.count == 0);
		assert(test.host.frame_states[0].cleanup_obligations.count == 0);
		assert(test.host.frame_slots[0].slot_id == 1);
		assert(test.host.frame_slots[0].kind == ZEND_MIR_FRAME_SLOT_KIND_TMP);
		assert(test.host.frame_slots[0].representation
			== ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL);
		assert(test.host.frame_slots[0].materialization
			== ZEND_MIR_MATERIALIZATION_MATERIALIZED);
		assert(!test.host.frame_slots[0].rooted);
		assert(!test.host.frame_slots[0].cleanup_required);
		assert(test.host.frame_slots[1].slot_id == 2);
		assert(test.host.frame_slots[1].materialization
			== ZEND_MIR_MATERIALIZATION_UNDEF);
		assert(!zend_mir_id_is_valid(test.host.frame_slots[1].value_id));
		assert(test.host.source_map_count == 2);
		assert(test.host.source_maps[0].owner_frame_id
			== test.host.frame_states[0].id);
		assert(test.host.source_maps[0].op_array_id == 300);
		assert(test.host.source_maps[0].opline_index == 0);
		assert(test.host.source_maps[0].opline_phase
			== ZEND_MIR_OPLINE_PHASE_BEFORE);
		assert(test_verify_host(&test.host, &diagnostics));
	}
}

static void test_free_and_deferred_cases(void)
{
	test_case test;
	zend_mir_straight_line_value initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
	zend_mir_source_opcode_ref source =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE, 0, 0, 1);
	zend_mir_straight_line_value current;
	zend_mir_lowering_diagnostic_code code;

	test_case_init(&test, &initial);
	assert(zend_mir_lower_free(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(test.host.instruction_count == 2);
	assert(test.host.instructions[0].opcode == ZEND_MIR_OPCODE_STATEPOINT);
	assert(test.host.instructions[1].opcode == ZEND_MIR_OPCODE_SCALAR_DROP);
	assert(test.host.instructions[1].representation
		== ZEND_MIR_REPRESENTATION_VOID);
	assert(!zend_mir_id_is_valid(test.host.instructions[1].result_id));
	assert(!zend_mir_id_is_valid(test.host.instructions[1].frame_state_id));
	assert(zend_mir_id_is_valid(
		test.host.instructions[1].source_position_id));
	assert(test.host.instructions[1].effects == 0);
	assert(test.host.instructions[1].reads == 0);
	assert(test.host.instructions[1].writes == 0);
	assert(test.host.instructions[1].barriers == 0);
	assert(test.host.instructions[1].ownership_actions == 0);
	assert(test.host.view.instruction_operand_count(
		test.host.view.context, test.host.instructions[1].id) == 1);
	assert(test.host.view.instruction_operand_at(
		test.host.view.context, test.host.instructions[1].id, 0,
		&current.value_id));
	assert(current.value_id == 0);
	assert(zend_mir_straight_line_value_at(&test.lifetime, 0, &current));
	assert(current.ownership == ZEND_MIR_OWNERSHIP_STATE_RELEASED);
	assert(!zend_mir_straight_line_track_value(&test.lifetime, &initial));
	assert(zend_mir_straight_line_value_at(&test.lifetime, 0, &current));
	assert(current.ownership == ZEND_MIR_OWNERSHIP_STATE_RELEASED);
	assert(zend_mir_lower_free(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);
	assert(test.host.instruction_count == 2);

	source = test_opcode(
		ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 1, 0, 1);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 1;
	test.source.uses[0].operand_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);
	assert(test.host.instruction_count == 2);

	source = test_opcode(
		ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE, 0, 0, 1);
	test_case_init(&test, &initial);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_DESTRUCTOR;
	assert(zend_mir_lower_free(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_DEFERRED);
	assert(code == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);

	test_case_init(&test, &initial);
	test.provider_context.proofs &=
		~ZEND_MIR_STRAIGHT_LINE_PROOF_NON_REFCOUNTED;
	assert(zend_mir_lower_free(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_MISSING_PROOF);

	test_case_init(&test, &initial);
	test.value_storage[0].fact_flags = 0;
	assert(zend_mir_lower_free(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);

	initial.ownership = ZEND_MIR_OWNERSHIP_STATE_BORROWED;
	test_case_init(&test, &initial);
	assert(zend_mir_lower_free(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);
	assert(test.host.instruction_count == 0);
	initial.ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;

	source = test_opcode(
		ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN, 0, 0, 1);
	test_case_init(&test, &initial);
	test.provider_context.proofs &=
		~ZEND_MIR_STRAIGHT_LINE_PROOF_NOT_BY_REFERENCE;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_MISSING_PROOF);
	assert(test.host.instruction_count == 0);

	test_case_init(&test, &initial);
	test.value_storage[0].exact_type = ZEND_MIR_SCALAR_TYPE_NONE;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_CONTRADICTORY_FACT);
	assert(test.host.instruction_count == 0);

	test_case_init(&test, &initial);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_REFERENCE;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_DEFERRED);
	assert(code == ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);
	assert(test.host.instruction_count == 0);

	test_case_init(&test, &initial);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_RETURN_BY_REFERENCE;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_DEFERRED);
	assert(code == ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);

	test_case_init(&test, &initial);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_PENDING_CALL;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_DEFERRED);
	assert(code == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);

	test_case_init(&test, &initial);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_CLEANUP;
	assert(zend_mir_lower_return(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_DEFERRED);
	assert(code == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);

	test_case_init(&test, &initial);
	source = test_opcode(
		ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 0, 0, 1);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_OLD_VALUE;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &source, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_DEFERRED);
	assert(code == ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);
}

static void test_structural_and_provider_claims(void)
{
	test_case test;
	zend_mir_straight_line_value initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I1,
		ZEND_MIR_OWNERSHIP_STATE_BORROWED, ZEND_MIR_SCALAR_TYPE_I1);
	zend_mir_source_opcode_ref source =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_NOP, 0, 0, 1);
	zend_mir_lowering_provider provider;
	zend_mir_lowering_claim claim;
	zend_mir_lowering_diagnostic_code code;

	test_case_init(&test, &initial);
	assert(zend_mir_lower_structural(
		&test.provider_context, &source, &code)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(test.host.instruction_count == 0);
	test.provider_context.hazards =
		ZEND_MIR_STRAIGHT_LINE_HAZARD_OBSERVER;
	assert(zend_mir_lower_structural(
		&test.provider_context, &source, &code)
		== ZEND_MIR_LOWERING_DEFERRED);
	test.provider_context.hazards = 0;
	test.provider_context.proofs &=
		~ZEND_MIR_STRAIGHT_LINE_PROOF_SINGLE_BLOCK;
	assert(zend_mir_lower_structural(
		&test.provider_context, &source, &code)
		== ZEND_MIR_LOWERING_REJECTED);
	assert(code == ZEND_MIRL_MISSING_PROOF);
	assert(zend_mir_lifetime_provider_init(
		&test.provider_context, &provider));
	assert(provider.claim_count(provider.context) == 2);
	assert(provider.claim_at(provider.context, 0, &claim));
	assert(claim.zend_opcode_number == ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN);
	assert(provider.claim_at(provider.context, 1, &claim));
	assert(claim.zend_opcode_number == ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE);
	assert(!provider.claim_at(provider.context, 2, &claim));
	test.source_view.contract_version++;
	assert(!zend_mir_lifetime_provider_init(
		&test.provider_context, &provider));
}

static void test_failure_atomic_local_state(void)
{
	static const uint32_t opcodes[] = {
		ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN,
		ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE,
		ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN
	};
	uint32_t index;

	for (index = 0; index < sizeof(opcodes) / sizeof(opcodes[0]); index++) {
		test_case test;
		zend_mir_straight_line_value initial = test_value(
			0, ZEND_MIR_REPRESENTATION_I64,
			ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
		zend_mir_source_opcode_ref source =
			test_opcode(opcodes[index], 0, 0, 1);
		zend_mir_straight_line_value current;
		zend_mir_lowering_diagnostic_code code;

		test_case_init(&test, &initial);
		if (opcodes[index] == ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN) {
			test.source.uses[0].ssa_variable_id = 0;
			test.source.uses[0].opline_index = 0;
			test.source.uses[0].operand_index = 0;
			test.source.use_count = 1;
		}
		test_add_source_position_delegate =
			test.host.mutator.add_source_position;
		test_source_positions_before_failure = 1;
		test.host.mutator.add_source_position =
			test_faulting_add_source_position;
		if (opcodes[index] == ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN) {
			assert(zend_mir_lower_copy_move(
				&test.lowering, &source, &test.host.mutator,
				&test.provider_context, &code) == ZEND_MIR_LOWERING_FAILED);
		} else if (opcodes[index] == ZEND_MIR_STRAIGHT_LINE_OPCODE_FREE) {
			assert(zend_mir_lower_free(
				&test.lowering, &source, &test.host.mutator,
				&test.provider_context, &code) == ZEND_MIR_LOWERING_FAILED);
		} else {
			assert(zend_mir_lower_return(
				&test.lowering, &source, &test.host.mutator,
				&test.provider_context, &code) == ZEND_MIR_LOWERING_FAILED);
		}
		assert(code == ZEND_MIRL_MUTATION_FAILED);
		assert(!test.lifetime.entry_emitted);
		assert(!zend_mir_id_is_valid(
			test.lifetime.entry_frame_state_id));
		assert(test.lifetime.count == 1);
		assert(zend_mir_straight_line_value_at(
			&test.lifetime, 0, &current));
		assert(current.ownership == ZEND_MIR_OWNERSHIP_STATE_OWNED);
		assert(test.slots[0].value_id == 0);
		assert(test.slots[0].materialization
			== ZEND_MIR_MATERIALIZATION_MATERIALIZED);
		assert(!zend_mir_id_is_valid(test.slots[1].value_id));
		assert(test.slots[1].materialization
			== ZEND_MIR_MATERIALIZATION_UNDEF);
	}
}

static void test_verifier_diagnostics(void)
{
	test_case test;
	test_diagnostics diagnostics;
	zend_mir_straight_line_value initial = test_value(
		0, ZEND_MIR_REPRESENTATION_I64,
		ZEND_MIR_OWNERSHIP_STATE_OWNED, ZEND_MIR_SCALAR_TYPE_I64);
	zend_mir_source_opcode_ref copy =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_QM_ASSIGN, 0, 0, 1);
	zend_mir_source_opcode_ref ret =
		test_opcode(ZEND_MIR_STRAIGHT_LINE_OPCODE_RETURN, 1, 1, 2);
	zend_mir_lowering_diagnostic_code code;

	test_case_init(&test, &initial);
	test.source.uses[0].ssa_variable_id = 0;
	test.source.uses[0].opline_index = 0;
	test.source.use_count = 1;
	assert(zend_mir_lower_copy_move(
		&test.lowering, &copy, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_lower_return(
		&test.lowering, &ret, &test.host.mutator,
		&test.provider_context, &code) == ZEND_MIR_LOWERING_SUCCESS);
	assert(test_verify_host(&test.host, &diagnostics));
	test.host.values[0].ownership = ZEND_MIR_OWNERSHIP_STATE_DESTROYED;
	assert(!test_verify_host(&test.host, &diagnostics));
	assert(test_has_token(&diagnostics, "MIRV0403"));

	test.host.values[0].ownership = ZEND_MIR_OWNERSHIP_STATE_OWNED;
	test.host.instructions[2].frame_state_id = ZEND_MIR_ID_INVALID;
	assert(!test_verify_host(&test.host, &diagnostics));
	assert(test_has_token(&diagnostics, "MIRV0507"));

	/*
	 * A second MOVE of operand zero is deliberately inserted before RETURN.
	 * Stage one must report both terminal use and double consume.
	 */
	test.host.instructions[3] = test.host.instructions[2];
	test.host.instructions[3].id = 3;
	test.host.instructions[3].frame_state_id = 1;
	test.host.instructions[2] = test.host.instructions[1];
	test.host.instructions[2].id = 2;
	test.host.instructions[2].result_id = 2;
	test.host.values[2] = test.host.values[1];
	test.host.values[2].id = 2;
	test.host.value_count = 3;
	test.host.instruction_count = 4;
	test.host.operands[2].instruction_id = 2;
	test.host.operands[2].value_id = 0;
	test.host.operands[3].instruction_id = 3;
	test.host.operands[3].value_id = 1;
	test.host.operand_count = 4;
	assert(!test_verify_host(&test.host, &diagnostics));
	assert(test_has_token(&diagnostics, "MIRV0403"));
	assert(test_has_token(&diagnostics, "MIRV0404"));
}

int main(void)
{
	test_copy_and_move();
	test_duplicate_and_undef_rejected();
	test_lifetime_and_entry_validation();
	test_entry_and_scalar_returns();
	test_free_and_deferred_cases();
	test_structural_and_provider_claims();
	test_failure_atomic_local_state();
	test_verifier_diagnostics();
	return 0;
}
