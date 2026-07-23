#include "Zend/Native/MIR/Verify/zend_mir_verify.h"
#include "Zend/Native/MIR/Verify/zend_mir_verify_internal.h"
#include "Zend/Native/MIR/Semantics/zend_mir_ownership.h"
#include "fixtures/verify_fixtures.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct _verify_diagnostics {
	zend_mir_diagnostic records[ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT];
	uint32_t count;
	bool accept;
} verify_diagnostics;

static bool collect_diagnostic(void *context, const zend_mir_diagnostic *diagnostic)
{
	verify_diagnostics *collected = (verify_diagnostics *) context;
	if (collected->count < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT) {
		collected->records[collected->count] = *diagnostic;
	}
	collected->count++;
	return collected->accept;
}

static bool verify_host(zend_mir_fixture_host *host, uint32_t limit,
		verify_diagnostics *collected)
{
	zend_mir_diagnostic_sink sink;
	memset(collected, 0, sizeof(*collected));
	collected->accept = true;
	sink.context = collected;
	sink.emit = collect_diagnostic;
	sink.limit = limit;
	sink.emitted = 0;
	return zend_mir_verify_stage1(&host->view, &sink);
}

static bool diagnostics_contain(
		const verify_diagnostics *collected, const char *token)
{
	uint32_t index;
	for (index = 0; index < collected->count
			&& index < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT; index++) {
		if (strstr(collected->records[index].message, token) != NULL) {
			return true;
		}
	}
	return false;
}

static const zend_mir_diagnostic *find_diagnostic(
		const verify_diagnostics *collected, const char *token)
{
	uint32_t index;
	for (index = 0; index < collected->count
			&& index < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT; index++) {
		if (strstr(collected->records[index].message, token) != NULL) {
			return &collected->records[index];
		}
	}
	return NULL;
}

static void assert_diagnostics_ordered(const verify_diagnostics *collected)
{
	uint32_t index;

	for (index = 1; index < collected->count; index++) {
		const zend_mir_diagnostic *previous = &collected->records[index - 1];
		const zend_mir_diagnostic *current = &collected->records[index];
		uint32_t previous_code;
		uint32_t current_code;
		uint32_t previous_key[7];
		uint32_t current_key[7];
		uint32_t key_index;
		bool ordered = false;

		assert(sscanf(previous->message, "[MIRV%4u]", &previous_code) == 1);
		assert(sscanf(current->message, "[MIRV%4u]", &current_code) == 1);
		previous_key[0] = previous_code / 100;
		previous_key[1] = previous->location.function_id;
		previous_key[2] = previous->location.block_id;
		previous_key[3] = previous->location.instruction_id;
		previous_key[4] = previous->location.frame_state_id;
		previous_key[5] = previous->location.source_position_id;
		previous_key[6] = previous_code;
		current_key[0] = current_code / 100;
		current_key[1] = current->location.function_id;
		current_key[2] = current->location.block_id;
		current_key[3] = current->location.instruction_id;
		current_key[4] = current->location.frame_state_id;
		current_key[5] = current->location.source_position_id;
		current_key[6] = current_code;

		for (key_index = 0; key_index < 7; key_index++) {
			if (previous_key[key_index] < current_key[key_index]) {
				ordered = true;
				break;
			}
			if (previous_key[key_index] > current_key[key_index]) {
				break;
			}
		}
		if (!ordered) {
			for (key_index = 0; key_index < 7; key_index++) {
				if (previous_key[key_index] != current_key[key_index]) {
					break;
				}
			}
			ordered = key_index == 7;
		}
		assert(ordered);
	}
}

static void expect_rejected(
		zend_mir_fixture_host *host, const char *token)
{
	verify_diagnostics collected;
	bool accepted = verify_host(
		host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected);
	if (accepted) {
		fprintf(stderr, "expected rejection with %s, but verification passed\n", token);
		assert(false);
	}
	if (!diagnostics_contain(&collected, token)) {
		uint32_t index;
		fprintf(stderr, "expected %s, got:\n", token);
		for (index = 0; index < collected.count
				&& index < ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT; index++) {
			fprintf(stderr, "  %s\n", collected.records[index].message);
		}
		assert(false);
	}
}

static bool failing_function_at(
		const void *context, uint32_t index, zend_mir_function_record *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

static uint32_t excessive_count(const void *context)
{
	(void) context;
	return ZEND_MIR_VERIFY_ENTITY_HARD_LIMIT + 1;
}

static uint32_t no_predecessors(
		const void *context, zend_mir_block_id block_id)
{
	(void) context;
	(void) block_id;
	return 0;
}

static void add_valid_slot(zend_mir_fixture_host *host)
{
	zend_mir_frame_slot_ref *slot = &host->frame_slots[0];
	memset(slot, 0, sizeof(*slot));
	slot->slot_id = 0;
	slot->value_id = 0;
	slot->kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
	slot->representation = ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	slot->materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	slot->ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	host->frame_slot_count = 1;
	host->frame_states[0].slots.count = 1;
}

static void apply_action_semantics(
		zend_mir_instruction_record *instruction, zend_mir_ownership_action action)
{
	const zend_mir_ownership_action_descriptor *descriptor =
		zend_mir_ownership_action_descriptor_at(action);
	assert(descriptor != NULL);
	instruction->effects = descriptor->effects;
	instruction->reads = descriptor->reads;
	instruction->writes = descriptor->writes;
	instruction->barriers = descriptor->barriers;
	instruction->ownership_actions = ZEND_MIR_OWNERSHIP_ACTION_MASK(action);
}

static void make_double_destroy(zend_mir_fixture_host *host)
{
	zend_mir_instruction_record saved_return = host->instructions[2];
	zend_mir_instruction_record *first = &host->instructions[1];
	zend_mir_instruction_record *second = &host->instructions[2];

	memset(first, 0, sizeof(*first));
	first->id = 1;
	first->block_id = 0;
	first->opcode = ZEND_MIR_OPCODE_STATEPOINT;
	first->representation = ZEND_MIR_REPRESENTATION_VOID;
	first->result_id = ZEND_MIR_ID_INVALID;
	first->frame_state_id = 0;
	first->source_position_id = 0;
	apply_action_semantics(first, ZEND_MIR_OWNERSHIP_ACTION_DESTROY);

	*second = *first;
	second->id = 2;
	saved_return.id = 3;
	host->instructions[3] = saved_return;
	host->instruction_count = 4;
	host->operands[0].instruction_id = 3;
	host->operands[1].instruction_id = 1;
	host->operands[1].value_id = 0;
	host->operands[2].instruction_id = 2;
	host->operands[2].value_id = 0;
	host->operand_count = 3;
}

static void test_code_names(void)
{
	assert(strcmp(zend_mir_verify_code_name(ZEND_MIR_VERIFY_OK), "MIRV0000") == 0);
	assert(strcmp(zend_mir_verify_code_name(
		ZEND_MIR_VERIFY_DUPLICATE_DEFINITION), "MIRV0104") == 0);
	assert(strcmp(zend_mir_verify_code_name(
		ZEND_MIR_VERIFY_PHI_EDGE_NOT_DOMINATING), "MIRV0302") == 0);
	assert(strcmp(zend_mir_verify_code_name(
		ZEND_MIR_VERIFY_INVALID_SOURCE_MAP), "MIRV0511") == 0);
	assert(strcmp(zend_mir_verify_code_name(
		ZEND_MIR_VERIFY_CODE_INVALID), "MIRV9999") == 0);
}

static void test_positive_and_nonmutation(void)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host before;
	verify_diagnostics collected;
	zend_mir_frame_state_ref *resume_frame;

	zend_mir_verify_fixture_linear(&host);
	before = host;
	assert(verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	assert(collected.count == 0);
	assert(memcmp(&before, &host, sizeof(host)) == 0);

	zend_mir_verify_fixture_diamond(&host);
	before = host;
	assert(verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	assert(collected.count == 0);
	assert(memcmp(&before, &host, sizeof(host)) == 0);

	/* Positive layout/root/cleanup and single-entry resume coverage. */
	zend_mir_verify_fixture_linear(&host);
	add_valid_slot(&host);
	host.frame_slots[0].rooted = true;
	host.frame_slots[0].cleanup_required = true;
	host.roots[0] = 0;
	host.root_count = 1;
	host.cleanups[0].slot_id = 0;
	host.cleanups[0].action = ZEND_MIR_CLEANUP_ACTION_DESTROY;
	host.cleanups[0].state = ZEND_MIR_CLEANUP_STATE_PENDING;
	host.cleanup_count = 1;
	host.frame_states[0].roots.count = 1;
	host.frame_states[0].cleanup_obligations.count = 1;
	resume_frame = &host.frame_states[1];
	*resume_frame = host.frame_states[0];
	resume_frame->id = 1;
	resume_frame->opline_phase = ZEND_MIR_OPLINE_PHASE_SUSPENDED;
	resume_frame->suspend_kind = ZEND_MIR_SUSPEND_KIND_GENERATOR;
	resume_frame->suspend_state_id = 9;
	resume_frame->resume.allowed = true;
	resume_frame->resume.entry_kind =
		ZEND_MIR_RESUME_ENTRY_KIND_SINGLE_ENTRY_DISPATCHER;
	resume_frame->resume.resume_id = 8;
	resume_frame->resume.code_version_id = resume_frame->code_version_id;
	resume_frame->resume.target_opline_index = 5;
	resume_frame->safepoint_class =
		ZEND_MIR_SAFEPOINT_CLASS_GENERATOR_SUSPEND;
	host.frame_state_count = 2;
	assert(verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	assert(collected.count == 0);

	/* Positive registered ownership-transition coverage. */
	zend_mir_verify_fixture_linear(&host);
	host.values[1].id = 1;
	host.values[1].representation = ZEND_MIR_REPRESENTATION_I64;
	host.values[1].ownership = ZEND_MIR_OWNERSHIP_STATE_BORROWED;
	host.value_count = 2;
	host.instructions[3] = host.instructions[2];
	host.instructions[3].id = 3;
	host.instructions[2].id = 2;
	host.instructions[2].opcode = ZEND_MIR_OPCODE_COPY;
	host.instructions[2].representation = ZEND_MIR_REPRESENTATION_I64;
	host.instructions[2].result_id = 1;
	host.instructions[2].frame_state_id = ZEND_MIR_ID_INVALID;
	host.instructions[2].source_position_id = ZEND_MIR_ID_INVALID;
	apply_action_semantics(
		&host.instructions[2], ZEND_MIR_OWNERSHIP_ACTION_BORROW);
	host.instruction_count = 4;
	host.operands[0].instruction_id = 3;
	host.operands[0].value_id = 1;
	host.operands[1].instruction_id = 2;
	host.operands[1].value_id = 0;
	host.operand_count = 2;
	assert(verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	assert(collected.count == 0);

	/* The frozen constant contract permits canonical zval scalar bits. */
	zend_mir_verify_fixture_linear(&host);
	host.values[0].representation = ZEND_MIR_REPRESENTATION_ZVAL;
	host.constants[0].representation = ZEND_MIR_REPRESENTATION_ZVAL;
	host.instructions[1].representation = ZEND_MIR_REPRESENTATION_ZVAL;
	assert(verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	assert(collected.count == 0);

	/* Undefined SSA inputs are legal only when published at entry and framed. */
	zend_mir_verify_fixture_linear(&host);
	host.values[0].ownership = ZEND_MIR_OWNERSHIP_STATE_BORROWED;
	host.constant_count = 0;
	host.instructions[1] = host.instructions[2];
	host.instructions[1].id = 1;
	host.instruction_count = 2;
	host.operands[0].instruction_id = 1;
	host.operands[1].instruction_id = 0;
	host.operands[1].value_id = 0;
	host.operand_count = 2;
	add_valid_slot(&host);
	host.frame_slots[0].ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_BORROWED;
	assert(verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	assert(collected.count == 0);
}

static void test_entry_failures(void)
{
	zend_mir_fixture_host host;
	verify_diagnostics collected;
	zend_mir_view view;
	zend_mir_diagnostic_sink sink;

	memset(&collected, 0, sizeof(collected));
	collected.accept = true;
	sink.context = &collected;
	sink.emit = collect_diagnostic;
	sink.limit = ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT;
	sink.emitted = 0;
	assert(!zend_mir_verify_stage1(NULL, &sink));
	assert(diagnostics_contain(&collected, "MIRV0001"));

	zend_mir_verify_fixture_linear(&host);
	view = host.view;
	view.value_at = NULL;
	host.view = view;
	expect_rejected(&host, "MIRV0005");

	zend_mir_verify_fixture_linear(&host);
	host.view.contract_version = UINT32_C(0x00020000);
	expect_rejected(&host, "MIRV0004");

	zend_mir_verify_fixture_linear(&host);
	host.module_id = ZEND_MIR_ID_INVALID;
	expect_rejected(&host, "MIRV0100");

	zend_mir_verify_fixture_linear(&host);
	host.view.function_at = failing_function_at;
	expect_rejected(&host, "MIRV0006");

	zend_mir_verify_fixture_linear(&host);
	host.view.function_count = excessive_count;
	expect_rejected(&host, "MIRV0007");

	zend_mir_verify_fixture_linear(&host);
	zend_mir_verify_test_fail_allocation_after(0);
	expect_rejected(&host, "MIRV0002");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[0].opcode = (zend_mir_opcode) ZEND_MIR_W10_OPCODE_COUNT;
	host.instructions[1].opcode = (zend_mir_opcode) ZEND_MIR_W10_OPCODE_COUNT;
	host.instructions[2].opcode = (zend_mir_opcode) ZEND_MIR_W10_OPCODE_COUNT;
	assert(!verify_host(&host, 2, &collected));
	assert(diagnostics_contain(&collected, "MIRV0003"));
}

static void test_identity_and_representation_failures(void)
{
	zend_mir_fixture_host host;

	zend_mir_verify_fixture_linear(&host);
	host.values[0].id = ZEND_MIR_ID_INVALID;
	expect_rejected(&host, "MIRV0100");

	zend_mir_verify_fixture_linear(&host);
	host.values[1] = host.values[0];
	host.value_count = 2;
	expect_rejected(&host, "MIRV0101");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[1].opcode = (zend_mir_opcode) ZEND_MIR_W10_OPCODE_COUNT;
	expect_rejected(&host, "MIRV0102");

	zend_mir_verify_fixture_linear(&host);
	host.values[0].ownership = (zend_mir_ownership_state) 99;
	expect_rejected(&host, "MIRV0102");

	zend_mir_verify_fixture_linear(&host);
	host.operands[0].value_id = 99;
	expect_rejected(&host, "MIRV0103");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[0].result_id = 0;
	host.instructions[0].representation = ZEND_MIR_REPRESENTATION_I64;
	expect_rejected(&host, "MIRV0104");

	zend_mir_verify_fixture_linear(&host);
	host.constants[0].symbol_id = 9;
	expect_rejected(&host, "MIRV0105");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[1].representation = ZEND_MIR_REPRESENTATION_DOUBLE;
	expect_rejected(&host, "MIRV0106");
}

static void test_cfg_failures(void)
{
	zend_mir_fixture_host host;

	zend_mir_verify_fixture_linear(&host);
	host.blocks[0].function_id = 99;
	expect_rejected(&host, "MIRV0200");

	zend_mir_verify_fixture_linear(&host);
	host.functions[0].entry_block_id = 99;
	expect_rejected(&host, "MIRV0201");

	zend_mir_verify_fixture_linear(&host);
	host.blocks[1].id = 1;
	host.blocks[1].function_id = 0;
	host.block_count = 2;
	expect_rejected(&host, "MIRV0201");

	zend_mir_verify_fixture_linear(&host);
	host.blocks[1].id = 1;
	host.blocks[1].function_id = 0;
	host.blocks[2].id = 2;
	host.blocks[2].function_id = 0;
	host.block_count = 3;
	host.instructions[3].id = 3;
	host.instructions[3].block_id = 1;
	host.instructions[3].opcode = ZEND_MIR_OPCODE_BRANCH;
	host.instructions[3].representation = ZEND_MIR_REPRESENTATION_CONTROL;
	host.instructions[3].result_id = ZEND_MIR_ID_INVALID;
	host.instructions[3].frame_state_id = ZEND_MIR_ID_INVALID;
	host.instructions[3].source_position_id = ZEND_MIR_ID_INVALID;
	host.instructions[4] = host.instructions[3];
	host.instructions[4].id = 4;
	host.instructions[4].block_id = 2;
	host.instruction_count = 5;
	host.edges[0].from = 1;
	host.edges[0].to = 2;
	host.edges[1].from = 2;
	host.edges[1].to = 1;
	host.edge_count = 2;
	expect_rejected(&host, "MIRV0201");

	zend_mir_verify_fixture_linear(&host);
	host.edges[0].from = 0;
	host.edges[0].to = 99;
	host.edge_count = 1;
	expect_rejected(&host, "MIRV0202");

	zend_mir_verify_fixture_linear(&host);
	host.edges[0].from = 0;
	host.edges[0].to = 0;
	host.edges[1] = host.edges[0];
	host.edge_count = 2;
	expect_rejected(&host, "MIRV0203");

	zend_mir_verify_fixture_diamond(&host);
	host.view.predecessor_count = no_predecessors;
	expect_rejected(&host, "MIRV0204");

	zend_mir_verify_fixture_linear(&host);
	host.instruction_count = 2;
	expect_rejected(&host, "MIRV0205");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[3] = host.instructions[0];
	host.instructions[3].id = 3;
	host.instruction_count = 4;
	expect_rejected(&host, "MIRV0206");

	zend_mir_verify_fixture_linear(&host);
	host.operands[1] = host.operands[0];
	host.operand_count = 2;
	expect_rejected(&host, "MIRV0207");

	zend_mir_verify_fixture_diamond(&host);
	host.operands[2].instruction_id = 8;
	expect_rejected(&host, "MIRV0208");
}

static void test_dominance_failures(void)
{
	zend_mir_fixture_host host;

	zend_mir_verify_fixture_linear(&host);
	host.instructions[1].id = 3;
	host.instructions[2].id = 2;
	expect_rejected(&host, "MIRV0300");

	zend_mir_verify_fixture_linear(&host);
	host.constant_count = 0;
	host.instructions[1] = host.instructions[2];
	host.instructions[1].id = 1;
	host.instruction_count = 2;
	host.operands[0].instruction_id = 1;
	expect_rejected(&host, "MIRV0300");

	zend_mir_verify_fixture_linear(&host);
	host.values[1].id = 1;
	host.values[1].representation = ZEND_MIR_REPRESENTATION_I64;
	host.values[1].ownership = ZEND_MIR_OWNERSHIP_STATE_BORROWED;
	host.value_count = 2;
	expect_rejected(&host, "MIRV0300");

	zend_mir_verify_fixture_diamond(&host);
	host.operands[3].value_id = 1;
	expect_rejected(&host, "MIRV0301");

	zend_mir_verify_fixture_diamond(&host);
	host.operands[1].value_id = 2;
	host.operands[2].value_id = 1;
	expect_rejected(&host, "MIRV0302");
}

static void test_semantic_failures(void)
{
	zend_mir_fixture_host host;
	const zend_mir_atomic_effect_descriptor *descriptor;

	zend_mir_verify_fixture_linear(&host);
	descriptor = zend_mir_atomic_effect_descriptor_at(ZEND_MIR_EFFECT_CALL_INTERNAL);
	assert(descriptor != NULL);
	host.instructions[0].effects =
		ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_CALL_INTERNAL);
	host.instructions[0].reads = descriptor->reads;
	host.instructions[0].writes = descriptor->writes;
	host.instructions[0].barriers = descriptor->barriers;
	expect_rejected(&host, "MIRV0400");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[0].effects = UINT16_C(0x8000);
	expect_rejected(&host, "MIRV0400");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[0].reads = UINT32_C(0x80000000);
	expect_rejected(&host, "MIRV0400");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[0].ownership_actions = UINT16_C(0x8000);
	expect_rejected(&host, "MIRV0400");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[0].effects = ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_ALLOCATE);
	expect_rejected(&host, "MIRV0401");

	zend_mir_verify_fixture_linear(&host);
	apply_action_semantics(
		&host.instructions[2], ZEND_MIR_OWNERSHIP_ACTION_BORROW);
	expect_rejected(&host, "MIRV0402");

	zend_mir_verify_fixture_linear(&host);
	host.values[0].ownership = ZEND_MIR_OWNERSHIP_STATE_DESTROYED;
	expect_rejected(&host, "MIRV0403");

	zend_mir_verify_fixture_linear(&host);
	make_double_destroy(&host);
	expect_rejected(&host, "MIRV0404");
}

static void test_frame_failures(void)
{
	zend_mir_fixture_host host;
	verify_diagnostics collected;
	const zend_mir_diagnostic *diagnostic;

	zend_mir_verify_fixture_linear(&host);
	host.frame_states[0].canonical = false;
	expect_rejected(&host, "MIRV0500");

	zend_mir_verify_fixture_linear(&host);
	host.frame_states[0].slots.offset = 1;
	host.frame_states[0].slots.count = 1;
	expect_rejected(&host, "MIRV0500");

	zend_mir_verify_fixture_linear(&host);
	host.frame_states[0].parent_id = 0;
	expect_rejected(&host, "MIRV0501");

	zend_mir_verify_fixture_linear(&host);
	add_valid_slot(&host);
	host.frame_slots[0].kind = (zend_mir_frame_slot_kind) 99;
	expect_rejected(&host, "MIRV0502");

	zend_mir_verify_fixture_linear(&host);
	add_valid_slot(&host);
	host.frame_slots[0].rooted = true;
	expect_rejected(&host, "MIRV0503");

	zend_mir_verify_fixture_linear(&host);
	add_valid_slot(&host);
	host.frame_slots[0].cleanup_required = true;
	expect_rejected(&host, "MIRV0504");

	zend_mir_verify_fixture_linear(&host);
	host.frame_states[0].return_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_NATIVE;
	expect_rejected(&host, "MIRV0505");

	zend_mir_verify_fixture_linear(&host);
	host.frame_states[0].resume.allowed = true;
	expect_rejected(&host, "MIRV0506");

	zend_mir_verify_fixture_linear(&host);
	host.instructions[2].frame_state_id = ZEND_MIR_ID_INVALID;
	expect_rejected(&host, "MIRV0507");

	zend_mir_verify_fixture_linear(&host);
	host.values[0].ownership = ZEND_MIR_OWNERSHIP_STATE_BORROWED;
	host.constant_count = 0;
	host.instructions[1] = host.instructions[2];
	host.instructions[1].id = 1;
	host.instruction_count = 2;
	host.operands[0].instruction_id = 1;
	host.operands[1].instruction_id = 0;
	host.operands[1].value_id = 0;
	host.operand_count = 2;
	expect_rejected(&host, "MIRV0507");

	zend_mir_verify_fixture_linear(&host);
	host.frame_states[0].safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_ALLOCATION;
	expect_rejected(&host, "MIRV0508");

	zend_mir_verify_fixture_linear(&host);
	host.source_positions[0].line = 0;
	assert(!verify_host(
		&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	diagnostic = find_diagnostic(&collected, "MIRV0509");
	assert(diagnostic != NULL);
	assert(diagnostic->location.module_id == 7);
	assert(diagnostic->location.source_position_id == 0);

	zend_mir_verify_fixture_linear(&host);
	host.instructions[2].source_position_id = ZEND_MIR_ID_INVALID;
	expect_rejected(&host, "MIRV0510");

	zend_mir_verify_fixture_linear(&host);
	host.source_maps[0].op_array_id = ZEND_MIR_ID_INVALID;
	assert(!verify_host(
		&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &collected));
	diagnostic = find_diagnostic(&collected, "MIRV0511");
	assert(diagnostic != NULL);
	assert(diagnostic->location.frame_state_id == 0);
	assert(diagnostic->location.source_position_id == 0);
}

static void test_determinism_and_negative_nonmutation(void)
{
	zend_mir_fixture_host host;
	zend_mir_fixture_host before;
	verify_diagnostics first;
	verify_diagnostics second;
	const zend_mir_diagnostic *diagnostic;
	zend_mir_instruction_record instruction;
	zend_mir_block_record block;
	zend_mir_value_record value;

	zend_mir_verify_fixture_diamond(&host);
	host.operands[1].value_id = 2;
	host.operands[2].value_id = 1;
	before = host;
	assert(!verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &first));
	assert(memcmp(&before, &host, sizeof(host)) == 0);
	assert_diagnostics_ordered(&first);

	/* View enumeration order is incidental; stable IDs define diagnostics. */
	instruction = host.instructions[0];
	host.instructions[0] = host.instructions[8];
	host.instructions[8] = instruction;
	block = host.blocks[0];
	host.blocks[0] = host.blocks[3];
	host.blocks[3] = block;
	value = host.values[0];
	host.values[0] = host.values[3];
	host.values[3] = value;
	before = host;
	assert(!verify_host(&host, ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT, &second));
	assert(first.count == second.count);
	assert(memcmp(first.records, second.records,
		sizeof(first.records[0]) * first.count) == 0);
	assert(memcmp(&before, &host, sizeof(host)) == 0);
	assert_diagnostics_ordered(&second);
	diagnostic = find_diagnostic(&first, "MIRV0302");
	assert(diagnostic != NULL);
	assert(diagnostic->location.module_id == 7);
	assert(diagnostic->location.function_id == 0);
	assert(diagnostic->location.block_id == 3);
	assert(diagnostic->location.instruction_id == 7);
	assert(strstr(diagnostic->message, "operand=v") != NULL);
}

static void test_sink_rejection_stops_verification(void)
{
	zend_mir_fixture_host host;
	verify_diagnostics collected;
	zend_mir_diagnostic_sink sink;

	zend_mir_verify_fixture_linear(&host);
	host.instructions[1].opcode = (zend_mir_opcode) 99;
	memset(&collected, 0, sizeof(collected));
	collected.accept = false;
	sink.context = &collected;
	sink.emit = collect_diagnostic;
	sink.limit = ZEND_MIR_VERIFY_DIAGNOSTIC_HARD_LIMIT;
	sink.emitted = 0;
	assert(!zend_mir_verify_stage1(&host.view, &sink));
	assert(collected.count == 1);
}

int main(void)
{
	test_code_names();
	test_positive_and_nonmutation();
	test_entry_failures();
	test_identity_and_representation_failures();
	test_cfg_failures();
	test_dominance_failures();
	test_semantic_failures();
	test_frame_failures();
	test_determinism_and_negative_nonmutation();
	test_sink_rejection_stops_verification();
	puts("verify tests: ok");
	return 0;
}
