#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "Zend/Native/Lowering/ControlFlow/zend_mir_control_flow_internal.h"
#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Native/Lowering/zend_mir_lowering_zend.h"
#include "Zend/Native/MIR/Verify/zend_mir_verify_control_flow.h"
#include "tests/native/mir/contracts/fixture_host.h"

typedef struct _test_source {
	zend_mir_source_opcode_ref opcodes[8];
	zend_mir_source_ssa_ref ssa[8];
	zend_mir_source_block_ref blocks[8];
	zend_mir_source_edge_ref edges[8];
	zend_mir_source_phi_ref phis[2];
	zend_mir_source_phi_input_ref inputs[8];
	zend_mir_source_literal_ref literals[2];
	uint32_t opcode_count;
	uint32_t block_count;
	uint32_t ssa_count;
	uint32_t edge_count;
	uint32_t phi_count;
	uint32_t input_count;
	uint32_t literal_count;
} test_source;

#define COUNT_CALLBACK(name, field) \
	static uint32_t name(const void *context) \
	{ \
		return ((const test_source *) context)->field; \
	}
#define AT_CALLBACK(name, type, field, count_field) \
	static bool name(const void *context, uint32_t index, type *out) \
	{ \
		const test_source *source = context; \
		if (out == NULL || index >= source->count_field) return false; \
		*out = source->field[index]; \
		return true; \
	}

COUNT_CALLBACK(test_opcode_count, opcode_count)
AT_CALLBACK(test_opcode_at, zend_mir_source_opcode_ref, opcodes, opcode_count)
COUNT_CALLBACK(test_ssa_count, ssa_count)
AT_CALLBACK(test_ssa_at, zend_mir_source_ssa_ref, ssa, ssa_count)
COUNT_CALLBACK(test_block_count, block_count)
AT_CALLBACK(test_block_at, zend_mir_source_block_ref, blocks, block_count)
COUNT_CALLBACK(test_edge_count, edge_count)
AT_CALLBACK(test_edge_at, zend_mir_source_edge_ref, edges, edge_count)
COUNT_CALLBACK(test_phi_count, phi_count)
AT_CALLBACK(test_phi_at, zend_mir_source_phi_ref, phis, phi_count)
COUNT_CALLBACK(test_input_count, input_count)
AT_CALLBACK(test_input_at, zend_mir_source_phi_input_ref, inputs, input_count)
COUNT_CALLBACK(test_literal_count, literal_count)
AT_CALLBACK(test_literal_at, zend_mir_source_literal_ref, literals, literal_count)

static uint32_t test_zero_count(const void *context)
{
	(void) context;
	return 0;
}

static bool test_unused_at(const void *context, uint32_t index, void *out)
{
	(void) context;
	(void) index;
	(void) out;
	return false;
}

static zend_mir_lowering_source_view test_view(test_source *source)
{
	zend_mir_lowering_source_view view;
	memset(&view, 0, sizeof(view));
	view.contract_version = ZEND_MIR_W04_CONTRACT_VERSION;
	view.context = source;
	view.opcode_count = test_opcode_count;
	view.opcode_at = test_opcode_at;
	view.ssa_count = test_ssa_count;
	view.ssa_at = test_ssa_at;
	view.ssa_use_count = test_zero_count;
	view.ssa_use_at = (bool (*)(const void *, uint32_t,
		zend_mir_source_ssa_use_ref *)) test_unused_at;
	view.ssa_def_count = test_zero_count;
	view.ssa_def_at = (bool (*)(const void *, uint32_t,
		zend_mir_source_ssa_def_ref *)) test_unused_at;
	view.literal_count = test_literal_count;
	view.literal_at = test_literal_at;
	view.block_count = test_block_count;
	view.block_at = test_block_at;
	view.edge_count = test_edge_count;
	view.edge_at = test_edge_at;
	view.phi_count = test_phi_count;
	view.phi_at = test_phi_at;
	view.phi_input_count = test_input_count;
	view.phi_input_at = test_input_at;
	return view;
}

static test_source diamond_source(void)
{
	test_source source;
	uint32_t i;
	memset(&source, 0, sizeof(source));
	source.opcode_count = 4;
	source.block_count = 4;
	source.edge_count = 4;
	source.ssa_count = 3;
	source.phi_count = 1;
	source.input_count = 2;
	for (i = 0; i < 4; i++) {
		source.blocks[i].id = i;
		source.blocks[i].first_opcode_ordinal = i;
		source.blocks[i].opcode_count = 1;
		source.blocks[i].flags = ZEND_MIR_SOURCE_BLOCK_REACHABLE;
		source.blocks[i].immediate_dominator =
			i == 0 ? ZEND_MIR_ID_INVALID : 0;
		source.blocks[i].loop_header = ZEND_MIR_ID_INVALID;
		source.opcodes[i].opline_index = i;
		source.opcodes[i].block_id = i;
		source.opcodes[i].source_position_id = i;
	}
	for (i = 0; i < source.ssa_count; i++) {
		source.ssa[i].ssa_variable_id = i;
		source.ssa[i].definition_opline_index = ZEND_MIR_ID_INVALID;
		source.ssa[i].source_slot = i;
		source.ssa[i].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	}
	source.blocks[0].flags |= ZEND_MIR_SOURCE_BLOCK_ENTRY;
	source.opcodes[0].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMPZ;
	source.opcodes[0].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[0].op1.ssa_variable_id = 0;
	source.opcodes[1].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.opcodes[2].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.edges[0] = (zend_mir_source_edge_ref) {
		0, 0, 1, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[1] = (zend_mir_source_edge_ref) {
		1, 0, 2, 1, 0, ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
	};
	source.edges[2] = (zend_mir_source_edge_ref) {
		2, 1, 3, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[3] = (zend_mir_source_edge_ref) {
		3, 2, 3, 0, 1, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.phis[0].id = 0;
	source.phis[0].block_id = 3;
	source.phis[0].result_ssa_variable_id = 2;
	source.phis[0].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	source.phis[0].source_slot_index = 2;
	source.phis[0].kind = ZEND_MIR_SOURCE_PHI_MERGE;
	source.inputs[0] = (zend_mir_source_phi_input_ref) { 0, 0, 1, 0 };
	source.inputs[1] = (zend_mir_source_phi_input_ref) { 0, 1, 2, 1 };
	return source;
}

static test_source empty_fallthrough_source(void)
{
	test_source source;
	memset(&source, 0, sizeof(source));
	source.block_count = 2;
	source.edge_count = 1;
	source.blocks[0].id = 0;
	source.blocks[0].flags =
		ZEND_MIR_SOURCE_BLOCK_ENTRY | ZEND_MIR_SOURCE_BLOCK_REACHABLE;
	source.blocks[0].immediate_dominator = ZEND_MIR_ID_INVALID;
	source.blocks[0].loop_header = ZEND_MIR_ID_INVALID;
	source.blocks[1].id = 1;
	source.blocks[1].flags = ZEND_MIR_SOURCE_BLOCK_REACHABLE;
	source.blocks[1].immediate_dominator = 0;
	source.blocks[1].loop_header = ZEND_MIR_ID_INVALID;
	source.edges[0] = (zend_mir_source_edge_ref) {
		0, 0, 1, 0, 0, ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
	};
	return source;
}

static test_source loop_source(void)
{
	test_source source;
	uint32_t i;
	memset(&source, 0, sizeof(source));
	source.opcode_count = 3;
	source.block_count = 3;
	source.edge_count = 3;
	source.ssa_count = 1;
	for (i = 0; i < source.block_count; i++) {
		source.blocks[i].id = i;
		source.blocks[i].first_opcode_ordinal = i;
		source.blocks[i].opcode_count = 1;
		source.blocks[i].flags = ZEND_MIR_SOURCE_BLOCK_REACHABLE;
		source.blocks[i].immediate_dominator =
			i == 0 ? ZEND_MIR_ID_INVALID : 0;
		source.blocks[i].loop_header =
			i == 1 ? 0 : ZEND_MIR_ID_INVALID;
		source.opcodes[i].opline_index = i;
		source.opcodes[i].block_id = i;
		source.opcodes[i].source_position_id = i;
	}
	source.blocks[0].flags |= ZEND_MIR_SOURCE_BLOCK_ENTRY
		| ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER;
	source.ssa[0].ssa_variable_id = 0;
	source.ssa[0].definition_opline_index = ZEND_MIR_ID_INVALID;
	source.ssa[0].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	source.opcodes[0].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMPNZ;
	source.opcodes[0].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[0].op1.ssa_variable_id = 0;
	source.opcodes[1].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.opcodes[2].zend_opcode_number = 62;
	source.edges[0] = (zend_mir_source_edge_ref) {
		0, 0, 1, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[1] = (zend_mir_source_edge_ref) {
		1, 0, 2, 1, 0, ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
	};
	source.edges[2] = (zend_mir_source_edge_ref) {
		2, 1, 0, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
			| ZEND_MIR_SOURCE_EDGE_BACKEDGE
			| ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY
	};
	return source;
}

static test_source loop_carried_phi_source(void)
{
	test_source source;
	uint32_t i;
	memset(&source, 0, sizeof(source));
	source.opcode_count = 4;
	source.block_count = 4;
	source.edge_count = 4;
	source.ssa_count = 3;
	source.phi_count = 1;
	source.input_count = 2;
	for (i = 0; i < source.block_count; i++) {
		source.blocks[i].id = i;
		source.blocks[i].first_opcode_ordinal = i;
		source.blocks[i].opcode_count = 1;
		source.blocks[i].flags = ZEND_MIR_SOURCE_BLOCK_REACHABLE;
		source.blocks[i].immediate_dominator =
			i == 0 ? ZEND_MIR_ID_INVALID : i == 1 ? 0 : 1;
		source.blocks[i].loop_header =
			i == 2 ? 1 : ZEND_MIR_ID_INVALID;
		source.opcodes[i].opline_index = i;
		source.opcodes[i].block_id = i;
		source.opcodes[i].source_position_id = i;
	}
	source.blocks[0].flags |= ZEND_MIR_SOURCE_BLOCK_ENTRY;
	source.blocks[1].flags |= ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER;
	source.opcodes[0].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.opcodes[1].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMPZ;
	source.opcodes[1].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[1].op1.ssa_variable_id = 2;
	source.opcodes[2].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.opcodes[3].zend_opcode_number = 62;
	source.edges[0] = (zend_mir_source_edge_ref) {
		0, 0, 1, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[1] = (zend_mir_source_edge_ref) {
		1, 1, 3, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[2] = (zend_mir_source_edge_ref) {
		2, 1, 2, 1, 0, ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
	};
	source.edges[3] = (zend_mir_source_edge_ref) {
		3, 2, 1, 0, 1, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
			| ZEND_MIR_SOURCE_EDGE_BACKEDGE
			| ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY
	};
	for (i = 0; i < source.ssa_count; i++) {
		source.ssa[i].ssa_variable_id = i;
		source.ssa[i].definition_opline_index = ZEND_MIR_ID_INVALID;
		source.ssa[i].source_slot = 0;
		source.ssa[i].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	}
	source.ssa[1].definition_opline_index = 2;
	source.phis[0].id = 0;
	source.phis[0].block_id = 1;
	source.phis[0].result_ssa_variable_id = 2;
	source.phis[0].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	source.phis[0].source_slot_index = 0;
	source.phis[0].kind = ZEND_MIR_SOURCE_PHI_MERGE;
	source.inputs[0] = (zend_mir_source_phi_input_ref) { 0, 0, 0, 0 };
	source.inputs[1] = (zend_mir_source_phi_input_ref) { 0, 1, 2, 1 };
	return source;
}

static test_source multiple_backedge_source(void)
{
	test_source source;
	uint32_t i;
	memset(&source, 0, sizeof(source));
	source.opcode_count = 4;
	source.block_count = 5;
	source.edge_count = 6;
	source.ssa_count = 4;
	source.phi_count = 1;
	source.input_count = 3;
	for (i = 0; i < source.block_count; i++) {
		source.blocks[i].id = i;
		source.blocks[i].first_opcode_ordinal = i < 4 ? i : 4;
		source.blocks[i].opcode_count = i < 4 ? 1 : 0;
		source.blocks[i].flags = ZEND_MIR_SOURCE_BLOCK_REACHABLE;
		source.blocks[i].immediate_dominator = i == 0
			? ZEND_MIR_ID_INVALID : i == 1 ? 0 : i == 3 ? 2 : 1;
		source.blocks[i].loop_header =
			i == 2 || i == 3 ? 1 : ZEND_MIR_ID_INVALID;
		if (i < 4) {
			source.opcodes[i].opline_index = i;
			source.opcodes[i].block_id = i;
			source.opcodes[i].source_position_id = i;
		}
	}
	source.blocks[0].flags |= ZEND_MIR_SOURCE_BLOCK_ENTRY;
	source.blocks[1].flags |= ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER;
	source.opcodes[0].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.opcodes[1].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMPZ;
	source.opcodes[1].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[1].op1.ssa_variable_id = 3;
	source.opcodes[2].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMPNZ;
	source.opcodes[2].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[2].op1.ssa_variable_id = 1;
	source.opcodes[3].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMP;
	source.edges[0] = (zend_mir_source_edge_ref) {
		0, 0, 1, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[1] = (zend_mir_source_edge_ref) {
		1, 1, 4, 0, 0, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	source.edges[2] = (zend_mir_source_edge_ref) {
		2, 1, 2, 1, 0, ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
	};
	source.edges[3] = (zend_mir_source_edge_ref) {
		3, 2, 1, 0, 1, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
			| ZEND_MIR_SOURCE_EDGE_BACKEDGE
			| ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY
	};
	source.edges[4] = (zend_mir_source_edge_ref) {
		4, 2, 3, 1, 0, ZEND_MIR_SOURCE_EDGE_FALLTHROUGH
	};
	source.edges[5] = (zend_mir_source_edge_ref) {
		5, 3, 1, 0, 2, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
			| ZEND_MIR_SOURCE_EDGE_BACKEDGE
			| ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY
	};
	for (i = 0; i < source.ssa_count; i++) {
		source.ssa[i].ssa_variable_id = i;
		source.ssa[i].definition_opline_index = ZEND_MIR_ID_INVALID;
		source.ssa[i].source_slot = 0;
		source.ssa[i].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	}
	source.ssa[1].definition_opline_index = 2;
	source.ssa[2].definition_opline_index = 3;
	source.phis[0].id = 0;
	source.phis[0].block_id = 1;
	source.phis[0].result_ssa_variable_id = 3;
	source.phis[0].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	source.phis[0].source_slot_index = 0;
	source.phis[0].kind = ZEND_MIR_SOURCE_PHI_MERGE;
	source.inputs[0] = (zend_mir_source_phi_input_ref) { 0, 0, 0, 0 };
	source.inputs[1] = (zend_mir_source_phi_input_ref) { 0, 1, 2, 1 };
	source.inputs[2] = (zend_mir_source_phi_input_ref) { 0, 2, 3, 2 };
	return source;
}

static void test_branch_order(void)
{
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_FALSE, 0) == 1);
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_FALSE, 1) == 0);
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_TRUE, 0) == 0);
	assert(zend_mir_w04_mir_successor_for_source(
		ZEND_MIR_W04_BRANCH_IF_TRUE, 1) == 1);
	assert(zend_mir_w04_branch_kind_for_opcode(ZEND_MIR_W04_OPCODE_JMPZ_EX)
		== ZEND_MIR_W04_BRANCH_IF_FALSE_WITH_RESULT);
	assert(zend_mir_w04_branch_kind_for_opcode(ZEND_MIR_W04_OPCODE_JMPNZ_EX)
		== ZEND_MIR_W04_BRANCH_IF_TRUE_WITH_RESULT);
}

static void test_validation(void)
{
	test_source source = diamond_source();
	zend_mir_lowering_source_view view = test_view(&source);
	zend_mir_w04_validation validation;
	assert(zend_mir_w04_validate_source(&view, &validation));
	assert(validation.entry_block_id == 0);
	assert((validation.proofs & ZEND_MIR_W04_PROOF_PHI_PREDECESSOR_ORDER) != 0);

	source.blocks[2].flags |= ZEND_MIR_SOURCE_BLOCK_PROTECTED;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_PROTECTED_REGION);
	source.blocks[2].flags &= ~ZEND_MIR_SOURCE_BLOCK_PROTECTED;

	source.blocks[2].flags |= ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_IRREDUCIBLE_LOOP);
	source.blocks[2].flags &= ~ZEND_MIR_SOURCE_BLOCK_IRREDUCIBLE;

	source.inputs[1].input_index = 0;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_UNSUPPORTED_PHI_PI);

	source = diamond_source();
	view = test_view(&source);
	source.input_count = 1;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_UNSUPPORTED_PHI_PI);

	source = diamond_source();
	view = test_view(&source);
	source.edge_count = 5;
	source.edges[4] = (zend_mir_source_edge_ref) {
		4, 0, 3, 2, 2, ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP
	};
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_MALFORMED_CFG);

	source = diamond_source();
	view = test_view(&source);
	source.blocks[1].immediate_dominator = 2;
	source.blocks[2].immediate_dominator = 1;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_MALFORMED_CFG);

	source = diamond_source();
	view = test_view(&source);
	source.phis[0].kind = ZEND_MIR_SOURCE_PHI_PI_TYPE;
	source.phis[0].constraint.type_mask = 1;
	source.inputs[0] = source.inputs[1];
	source.input_count = 1;
	assert(zend_mir_w04_validate_source(&view, &validation));
	source.phis[0].constraint.type_mask = 0;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_UNSUPPORTED_PHI_PI);
}

static void test_loop_validation(void)
{
	test_source source = loop_source();
	zend_mir_lowering_source_view view = test_view(&source);
	zend_mir_w04_validation validation;
	assert(zend_mir_w04_validate_source(&view, &validation));
	source.blocks[0].flags &= ~ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_IRREDUCIBLE_LOOP);
	source.blocks[0].flags |= ZEND_MIR_SOURCE_BLOCK_LOOP_HEADER;
	source.edges[0].flags |= ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_MALFORMED_CFG);

	source = loop_carried_phi_source();
	view = test_view(&source);
	assert(zend_mir_w04_validate_source(&view, &validation));
	source.ssa[1].definition_opline_index = 3;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_UNSUPPORTED_PHI_PI);

	source = multiple_backedge_source();
	view = test_view(&source);
	assert(zend_mir_w04_validate_source(&view, &validation));
	source.edges[5].flags &= ~ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY;
	assert(!zend_mir_w04_validate_source(&view, &validation));
	assert(validation.diagnostic == ZEND_MIRL_W04_MALFORMED_CFG);
}

static void test_map(void)
{
	zend_mir_control_flow_map_storage storage;
	zend_mir_control_flow_block_mapping block = { 0, 7 };
	zend_mir_block_id found = ZEND_MIR_ID_INVALID;
	assert(zend_mir_control_flow_map_storage_init(&storage, 2, 1, 1));
	assert(zend_mir_control_flow_map_add_block(&storage, &block));
	assert(!zend_mir_control_flow_map_add_block(&storage, &block));
	assert(zend_mir_control_flow_map_find_block(
		&storage.public_map, 0, &found));
	assert(found == 7);
	zend_mir_control_flow_map_storage_destroy(&storage);
}

static zend_mir_instruction_id add_instruction(
	zend_mir_fixture_host *host, zend_mir_block_id block,
	zend_mir_opcode opcode, zend_mir_value_id result)
{
	zend_mir_instruction_record record;
	zend_mir_instruction_id id;
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = block;
	record.opcode = opcode;
	record.representation = opcode == ZEND_MIR_OPCODE_PHI
		? ZEND_MIR_REPRESENTATION_ZVAL
		: opcode == ZEND_MIR_OPCODE_COND_BRANCH
			|| opcode == ZEND_MIR_OPCODE_BRANCH
			? ZEND_MIR_REPRESENTATION_CONTROL
			: ZEND_MIR_REPRESENTATION_VOID;
	record.result_id = result;
	record.frame_state_id = ZEND_MIR_ID_INVALID;
	record.source_position_id = ZEND_MIR_ID_INVALID;
	assert(host->mutator.add_instruction(host->mutator.context, &record, &id));
	return id;
}

static void test_stage3(void)
{
	test_source source = diamond_source();
	zend_mir_lowering_source_view source_view = test_view(&source);
	zend_mir_fixture_host host;
	zend_mir_control_flow_map_storage storage;
	zend_mir_function_id function;
	zend_mir_block_id blocks[4];
	zend_mir_instruction_id branch[3];
	zend_mir_instruction_id ex_copy;
	zend_mir_instruction_id phi;
	uint32_t i;
	zend_mir_fixture_host_init(&host, 9);
	assert(host.mutator.add_function(host.mutator.context, 1, &function));
	for (i = 0; i < 4; i++) {
		assert(host.mutator.add_block(
			host.mutator.context, function, &blocks[i]));
	}
	assert(host.mutator.set_entry_block(
		host.mutator.context, function, blocks[0]));
	for (i = 0; i < 3; i++) {
		assert(host.mutator.add_value(host.mutator.context, i,
			ZEND_MIR_REPRESENTATION_ZVAL,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	}
	branch[0] = add_instruction(
		&host, blocks[0], ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_operand(host.mutator.context, branch[0], 0));
	branch[1] = add_instruction(
		&host, blocks[1], ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_ID_INVALID);
	branch[2] = add_instruction(
		&host, blocks[2], ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_ID_INVALID);
	phi = add_instruction(&host, blocks[3], ZEND_MIR_OPCODE_PHI, 2);
	assert(host.mutator.add_operand(host.mutator.context, phi, 0));
	assert(host.mutator.add_operand(host.mutator.context, phi, 1));
	(void) add_instruction(
		&host, blocks[3], ZEND_MIR_OPCODE_RETURN, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_edge(host.mutator.context, blocks[0], blocks[2]));
	assert(host.mutator.add_edge(host.mutator.context, blocks[0], blocks[1]));
	assert(host.mutator.add_edge(host.mutator.context, blocks[1], blocks[3]));
	assert(host.mutator.add_edge(host.mutator.context, blocks[2], blocks[3]));
	assert(zend_mir_control_flow_map_storage_init(&storage, 4, 4, 1));
	for (i = 0; i < 4; i++) {
		zend_mir_control_flow_block_mapping mapping = { i, blocks[i] };
		assert(zend_mir_control_flow_map_add_block(&storage, &mapping));
	}
	{
		zend_mir_control_flow_edge_mapping edges[4] = {
			{ 0, blocks[0], blocks[1], branch[0], ZEND_MIR_ID_INVALID, 1 },
			{ 1, blocks[0], blocks[2], branch[0], ZEND_MIR_ID_INVALID, 0 },
			{ 2, blocks[1], blocks[3], branch[1], ZEND_MIR_ID_INVALID, 0 },
			{ 3, blocks[2], blocks[3], branch[2], ZEND_MIR_ID_INVALID, 0 },
		};
		for (i = 0; i < 4; i++) {
			assert(zend_mir_control_flow_map_add_edge(&storage, &edges[i]));
		}
	}
	{
		zend_mir_control_flow_phi_mapping mapping = { 0, phi, 2 };
		assert(zend_mir_control_flow_map_add_phi(&storage, &mapping));
	}
	assert(zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	storage.blocks[1].mir_block_id = storage.blocks[0].mir_block_id;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	storage.blocks[1].mir_block_id = blocks[1];
	storage.edges[0].mir_successor_index = 0;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	storage.edges[0].mir_successor_index = 1;
	source.edges[2].flags |= ZEND_MIR_SOURCE_EDGE_INTERRUPT_BOUNDARY;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	zend_mir_control_flow_map_storage_destroy(&storage);

	source = diamond_source();
	source.ssa_count = 4;
	source.ssa[3] = source.ssa[0];
	source.ssa[3].ssa_variable_id = 3;
	source.ssa[3].source_slot = 3;
	source.ssa[3].definition_opline_index = 0;
	source.opcodes[0].zend_opcode_number = ZEND_MIR_W04_OPCODE_JMPZ_EX;
	source.opcodes[0].result.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[0].result.ssa_variable_id = 3;
	source_view = test_view(&source);
	zend_mir_fixture_host_init(&host, 11);
	assert(host.mutator.add_function(host.mutator.context, 1, &function));
	for (i = 0; i < 4; i++) {
		assert(host.mutator.add_block(
			host.mutator.context, function, &blocks[i]));
		assert(host.mutator.add_value(host.mutator.context, i,
			i == 3 ? ZEND_MIR_REPRESENTATION_I1
				: ZEND_MIR_REPRESENTATION_ZVAL,
			ZEND_MIR_OWNERSHIP_STATE_BORROWED));
	}
	assert(host.mutator.set_entry_block(
		host.mutator.context, function, blocks[0]));
	ex_copy = add_instruction(
		&host, blocks[0], ZEND_MIR_OPCODE_COPY, 3);
	host.instructions[ex_copy].representation = ZEND_MIR_REPRESENTATION_I1;
	assert(host.mutator.add_operand(host.mutator.context, ex_copy, 0));
	branch[0] = add_instruction(
		&host, blocks[0], ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_operand(host.mutator.context, branch[0], 0));
	branch[1] = add_instruction(
		&host, blocks[1], ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_ID_INVALID);
	branch[2] = add_instruction(
		&host, blocks[2], ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_ID_INVALID);
	phi = add_instruction(&host, blocks[3], ZEND_MIR_OPCODE_PHI, 2);
	assert(host.mutator.add_operand(host.mutator.context, phi, 0));
	assert(host.mutator.add_operand(host.mutator.context, phi, 1));
	(void) add_instruction(
		&host, blocks[3], ZEND_MIR_OPCODE_RETURN, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_edge(host.mutator.context, blocks[0], blocks[2]));
	assert(host.mutator.add_edge(host.mutator.context, blocks[0], blocks[1]));
	assert(host.mutator.add_edge(host.mutator.context, blocks[1], blocks[3]));
	assert(host.mutator.add_edge(host.mutator.context, blocks[2], blocks[3]));
	assert(zend_mir_control_flow_map_storage_init(&storage, 4, 4, 1));
	for (i = 0; i < 4; i++) {
		zend_mir_control_flow_block_mapping mapping = { i, blocks[i] };
		assert(zend_mir_control_flow_map_add_block(&storage, &mapping));
	}
	{
		zend_mir_control_flow_edge_mapping edges[4] = {
			{ 0, blocks[0], blocks[1], branch[0], ZEND_MIR_ID_INVALID, 1 },
			{ 1, blocks[0], blocks[2], branch[0], ZEND_MIR_ID_INVALID, 0 },
			{ 2, blocks[1], blocks[3], branch[1], ZEND_MIR_ID_INVALID, 0 },
			{ 3, blocks[2], blocks[3], branch[2], ZEND_MIR_ID_INVALID, 0 },
		};
		for (i = 0; i < 4; i++) {
			assert(zend_mir_control_flow_map_add_edge(&storage, &edges[i]));
		}
	}
	{
		zend_mir_control_flow_phi_mapping mapping = { 0, phi, 2 };
		assert(zend_mir_control_flow_map_add_phi(&storage, &mapping));
	}
	assert(zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	host.operands[0].value_id = 1;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	zend_mir_control_flow_map_storage_destroy(&storage);
}

static void test_stage3_loop_statepoint(void)
{
	test_source source = loop_source();
	zend_mir_lowering_source_view source_view = test_view(&source);
	zend_mir_fixture_host host;
	zend_mir_control_flow_map_storage storage;
	zend_mir_function_id function;
	zend_mir_block_id blocks[4];
	zend_mir_instruction_id cond;
	zend_mir_instruction_id backedge;
	zend_mir_instruction_id statepoint;
	zend_mir_frame_slot_ref frame_slot;
	uint32_t frame_slot_index;
	zend_mir_frame_state_ref frame;
	zend_mir_frame_state_id frame_id;
	zend_mir_source_position_ref source_position;
	zend_mir_source_position_id source_position_id;
	zend_mir_source_map_ref source_map;
	zend_mir_source_map_id source_map_id;
	zend_mir_instruction_record record;
	uint32_t i;
	zend_mir_fixture_host_init(&host, 10);
	assert(host.mutator.add_function(host.mutator.context, 1, &function));
	for (i = 0; i < 4; i++) {
		assert(host.mutator.add_block(
			host.mutator.context, function, &blocks[i]));
	}
	assert(host.mutator.set_entry_block(
		host.mutator.context, function, blocks[0]));
	assert(host.mutator.add_value(host.mutator.context, 0,
		ZEND_MIR_REPRESENTATION_I1, ZEND_MIR_OWNERSHIP_STATE_OWNED));
	memset(&source_position, 0, sizeof(source_position));
	source_position.id = ZEND_MIR_ID_INVALID;
	source_position.file_symbol_id = 1;
	source_position.line = 1;
	assert(host.mutator.add_source_position(host.mutator.context,
		&source_position, &source_position_id));
	source.opcodes[1].source_position_id = source_position_id;
	cond = add_instruction(
		&host, blocks[0], ZEND_MIR_OPCODE_COND_BRANCH, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_operand(host.mutator.context, cond, 0));
	backedge = add_instruction(
		&host, blocks[1], ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_ID_INVALID);
	(void) add_instruction(
		&host, blocks[2], ZEND_MIR_OPCODE_RETURN, ZEND_MIR_ID_INVALID);
	memset(&frame_slot, 0, sizeof(frame_slot));
	frame_slot.slot_id = 0;
	frame_slot.value_id = 0;
	frame_slot.kind = ZEND_MIR_FRAME_SLOT_KIND_CV;
	frame_slot.representation =
		ZEND_MIR_FRAME_SLOT_REPRESENTATION_CANONICAL_ZVAL;
	frame_slot.materialization = ZEND_MIR_MATERIALIZATION_MATERIALIZED;
	frame_slot.ownership = ZEND_MIR_FRAME_SLOT_OWNERSHIP_FRAME_OWNED;
	assert(host.mutator.add_frame_slot(
		host.mutator.context, &frame_slot, &frame_slot_index));
	memset(&frame, 0, sizeof(frame));
	frame.id = ZEND_MIR_ID_INVALID;
	frame.function_id = function;
	frame.parent_id = ZEND_MIR_ID_INVALID;
	frame.function_kind = ZEND_MIR_FUNCTION_KIND_USER;
	frame.opline_index = 1;
	frame.opline_phase = ZEND_MIR_OPLINE_PHASE_BEFORE;
	frame.slots.offset = frame_slot_index;
	frame.slots.count = 1;
	frame.return_continuation.kind = ZEND_MIR_CONTINUATION_KIND_TERMINAL;
	frame.return_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.return_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.exception_continuation = frame.return_continuation;
	frame.bailout_continuation.kind =
		ZEND_MIR_CONTINUATION_KIND_NONLOCAL_BAILOUT;
	frame.bailout_continuation.frame_state_id = ZEND_MIR_ID_INVALID;
	frame.bailout_continuation.opline_index = ZEND_MIR_ID_INVALID;
	frame.suspend_kind = ZEND_MIR_SUSPEND_KIND_NONE;
	frame.suspend_state_id = ZEND_MIR_ID_INVALID;
	frame.resume.entry_kind = ZEND_MIR_RESUME_ENTRY_KIND_NONE;
	frame.resume.resume_id = ZEND_MIR_ID_INVALID;
	frame.resume.code_version_id = ZEND_MIR_ID_INVALID;
	frame.resume.target_opline_index = ZEND_MIR_ID_INVALID;
	frame.safepoint_class = ZEND_MIR_SAFEPOINT_CLASS_OBSERVER;
	frame.canonical = true;
	assert(host.mutator.add_frame_state(
		host.mutator.context, &frame, &frame_id));
	memset(&source_map, 0, sizeof(source_map));
	source_map.id = ZEND_MIR_ID_INVALID;
	source_map.source_position_id = source_position_id;
	source_map.op_array_id = 1;
	source_map.opline_index = frame.opline_index;
	source_map.opline_phase = frame.opline_phase;
	source_map.owner_frame_id = frame_id;
	assert(host.mutator.add_source_map(
		host.mutator.context, &source_map, &source_map_id));
	memset(&record, 0, sizeof(record));
	record.id = ZEND_MIR_ID_INVALID;
	record.block_id = blocks[3];
	record.opcode = ZEND_MIR_OPCODE_STATEPOINT;
	record.representation = ZEND_MIR_REPRESENTATION_VOID;
	record.result_id = ZEND_MIR_ID_INVALID;
	record.frame_state_id = frame_id;
	record.source_position_id = source_position_id;
	record.effects =
		ZEND_MIR_EFFECT_MASK(ZEND_MIR_EFFECT_INTERRUPT_BOUNDARY);
	record.reads =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_FRAME_CALL_CHAIN)
		| ZEND_MIR_MEMORY_DOMAIN_MASK(
			ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT);
	record.writes =
		ZEND_MIR_MEMORY_DOMAIN_MASK(ZEND_MIR_MEMORY_DOMAIN_ENGINE_INTERRUPT);
	record.barriers = ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_SAFEPOINT)
		| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_OBSERVER)
		| ZEND_MIR_BARRIER_MASK(ZEND_MIR_BARRIER_INTERRUPT);
	assert(host.mutator.add_instruction(
		host.mutator.context, &record, &statepoint));
	assert(host.mutator.add_operand(
		host.mutator.context, statepoint, frame_slot.value_id));
	(void) add_instruction(
		&host, blocks[3], ZEND_MIR_OPCODE_BRANCH, ZEND_MIR_ID_INVALID);
	assert(host.mutator.add_edge(
		host.mutator.context, blocks[0], blocks[1]));
	assert(host.mutator.add_edge(
		host.mutator.context, blocks[0], blocks[2]));
	assert(host.mutator.add_edge(
		host.mutator.context, blocks[1], blocks[3]));
	assert(host.mutator.add_edge(
		host.mutator.context, blocks[3], blocks[0]));
	assert(zend_mir_control_flow_map_storage_init(&storage, 3, 3, 0));
	for (i = 0; i < 3; i++) {
		zend_mir_control_flow_block_mapping mapping = { i, blocks[i] };
		assert(zend_mir_control_flow_map_add_block(&storage, &mapping));
	}
	{
		zend_mir_control_flow_edge_mapping edges[3] = {
			{ 0, blocks[0], blocks[1], cond, ZEND_MIR_ID_INVALID, 0 },
			{ 1, blocks[0], blocks[2], cond, ZEND_MIR_ID_INVALID, 1 },
			{ 2, blocks[1], blocks[3], backedge, statepoint, 0 },
		};
		for (i = 0; i < 3; i++) {
			assert(zend_mir_control_flow_map_add_edge(&storage, &edges[i]));
		}
	}
	assert(zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	host.frame_slots[frame_slot_index].value_id = ZEND_MIR_ID_INVALID;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	host.frame_slots[frame_slot_index].value_id = 0;
	host.operand_count--;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	host.operand_count++;
	assert(host.mutator.add_source_map(
		host.mutator.context, &source_map, &source_map_id));
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	host.source_map_count--;
	storage.edges[2].edge_statepoint_instruction_id = ZEND_MIR_ID_INVALID;
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	storage.edges[2].edge_statepoint_instruction_id = statepoint;
	assert(host.mutator.add_instruction(
		host.mutator.context, &record, &statepoint));
	assert(!zend_mir_verify_w04_control_flow(
		&host.view, &source_view, &storage.public_map, NULL));
	zend_mir_control_flow_map_storage_destroy(&storage);
}

/*
 * The production edge-statepoint emitter uses these adapter functions only
 * for interrupt edges. This storage-independent builder fixture has no Zend
 * frame slots and deliberately contains no interrupt edge.
 */
uint32_t zend_mir_zend_source_slot_count(const zend_mir_zend_source *source)
{
	(void) source;
	return 0;
}

bool zend_mir_zend_source_slot_at(
	const zend_mir_zend_source *source, uint32_t index,
	zend_mir_source_slot_ref *out)
{
	(void) source;
	(void) index;
	(void) out;
	return false;
}

typedef enum _builder_fault {
	BUILDER_FAULT_NONE = 0,
	BUILDER_FAULT_BLOCK_MAP,
	BUILDER_FAULT_VALUE_FACT,
	BUILDER_FAULT_PHI,
	BUILDER_FAULT_PROVIDER,
	BUILDER_FAULT_FINALIZE,
	BUILDER_FAULT_STAGE1,
	BUILDER_FAULT_STAGE2,
	BUILDER_FAULT_STAGE3
} builder_fault;

typedef struct _builder_host builder_host;

struct _zend_mir_module {
	builder_host *host;
};

struct _builder_host {
	zend_mir_fixture_host fixture;
	struct _zend_mir_module module;
	builder_fault fault;
	uint32_t value_fact_count;
	uint32_t destroy_count;
	bool (*original_add_block)(
		void *, zend_mir_function_id, zend_mir_block_id *);
	bool (*original_add_instruction)(
		void *, const zend_mir_instruction_record *, zend_mir_instruction_id *);
	bool (*original_successor_at)(
		const void *, zend_mir_block_id, uint32_t, zend_mir_block_id *);
};

static bool builder_add_block(
	void *context, zend_mir_function_id function_id, zend_mir_block_id *out)
{
	builder_host *host = context;
	if (host->fault == BUILDER_FAULT_BLOCK_MAP) {
		return false;
	}
	return host->original_add_block(context, function_id, out);
}

static bool builder_add_instruction(
	void *context, const zend_mir_instruction_record *record,
	zend_mir_instruction_id *out)
{
	builder_host *host = context;
	if (host->fault == BUILDER_FAULT_PHI && record != NULL
			&& record->opcode == ZEND_MIR_OPCODE_PHI) {
		return false;
	}
	return host->original_add_instruction(context, record, out);
}

static bool builder_successor_at(
	const void *context, zend_mir_block_id block_id, uint32_t index,
	zend_mir_block_id *out)
{
	const builder_host *host = context;
	if (host->fault == BUILDER_FAULT_STAGE3) {
		return false;
	}
	return host->original_successor_at(context, block_id, index, out);
}

static bool builder_add_value_fact(
	void *context, const zend_mir_value_fact_ref *fact,
	zend_mir_value_fact_id *out)
{
	builder_host *host = context;
	if (fact == NULL || out == NULL || host->fault == BUILDER_FAULT_VALUE_FACT) {
		return false;
	}
	*out = host->value_fact_count++;
	return true;
}

static zend_mir_module *builder_create(
	void *context, zend_mir_module_id module_id,
	zend_mir_diagnostic_sink *diagnostics)
{
	builder_host *host = context;
	(void) diagnostics;
	assert(module_id == 9);
	host->module.host = host;
	return &host->module;
}

static void builder_destroy(void *context, zend_mir_module *module)
{
	builder_host *host = context;
	assert(module == &host->module);
	host->destroy_count++;
}

static zend_mir_mutator *builder_mutator(
	void *context, zend_mir_module *module)
{
	builder_host *host = context;
	assert(module == &host->module);
	return &host->fixture.mutator;
}

static const zend_mir_view *builder_view(
	void *context, const zend_mir_module *module)
{
	builder_host *host = context;
	assert(module == &host->module);
	return &host->fixture.view;
}

static bool builder_finalize(void *context, zend_mir_module *module)
{
	builder_host *host = context;
	assert(module == &host->module);
	return host->fault != BUILDER_FAULT_FINALIZE;
}

static bool builder_verify_stage1(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	builder_host *host = context;
	(void) diagnostics;
	assert(view == &host->fixture.view);
	return host->fault != BUILDER_FAULT_STAGE1;
}

static bool builder_verify_stage2(
	void *context, const zend_mir_view *view,
	zend_mir_diagnostic_sink *diagnostics)
{
	builder_host *host = context;
	(void) diagnostics;
	assert(view == &host->fixture.view);
	return host->fault != BUILDER_FAULT_STAGE2;
}

static void builder_host_init(
	builder_host *host, zend_mir_lowering_module_ops *ops,
	builder_fault fault)
{
	memset(host, 0, sizeof(*host));
	memset(ops, 0, sizeof(*ops));
	zend_mir_fixture_host_init(&host->fixture, 9);
	host->fault = fault;
	host->fixture.mutator.context = host;
	host->fixture.view.context = host;
	host->original_add_block = host->fixture.mutator.add_block;
	host->original_add_instruction = host->fixture.mutator.add_instruction;
	host->original_successor_at = host->fixture.view.successor_at;
	host->fixture.mutator.add_block = builder_add_block;
	host->fixture.mutator.add_instruction = builder_add_instruction;
	host->fixture.mutator.add_value_fact = builder_add_value_fact;
	host->fixture.view.successor_at = builder_successor_at;
	ops->context = host;
	ops->create = builder_create;
	ops->destroy = builder_destroy;
	ops->mutator = builder_mutator;
	ops->view = builder_view;
	ops->finalize = builder_finalize;
	ops->verify_stage1 = builder_verify_stage1;
	ops->verify_stage2 = builder_verify_stage2;
}

static bool builder_fact_at(
	const void *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *out)
{
	const test_source *source = context;
	uint32_t i;
	if (out == NULL) {
		return false;
	}
	for (i = 0; i < source->ssa_count; i++) {
		if (zend_mir_value_from_original_ssa(source->ssa[i].ssa_variable_id)
				== value_id) {
			memset(out, 0, sizeof(*out));
			out->id = ZEND_MIR_ID_INVALID;
			out->value_id = value_id;
			out->exact_type = source->ssa[i].ssa_variable_id == 3
					|| source->ssa[i].source_slot_kind
						== ZEND_MIR_SOURCE_SLOT_TMP
				? ZEND_MIR_SCALAR_TYPE_I1 : ZEND_MIR_SCALAR_TYPE_I64;
			out->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED
				| ZEND_MIR_VALUE_FACT_FINITE
				| ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
			out->integer_min = 0;
			out->integer_max = 100;
			out->provenance = ZEND_MIR_FACT_PROVENANCE_CONTRACT;
			out->provenance_source_position_id = ZEND_MIR_ID_INVALID;
			return true;
		}
	}
	for (i = 0; i < source->literal_count; i++) {
		if (zend_mir_value_from_synthetic(source->literals[i].literal_index)
				== value_id) {
			memset(out, 0, sizeof(*out));
			out->id = ZEND_MIR_ID_INVALID;
			out->value_id = value_id;
			out->exact_type = ZEND_MIR_SCALAR_TYPE_I64;
			out->flags = ZEND_MIR_VALUE_FACT_NON_REFCOUNTED
				| ZEND_MIR_VALUE_FACT_FINITE
				| ZEND_MIR_VALUE_FACT_HAS_INTEGER_RANGE;
			out->integer_min = (int64_t) source->literals[i].payload_bits;
			out->integer_max = (int64_t) source->literals[i].payload_bits;
			out->provenance = ZEND_MIR_FACT_PROVENANCE_CONTRACT;
			out->provenance_source_position_id = ZEND_MIR_ID_INVALID;
			return true;
		}
	}
	return false;
}

static bool builder_no_fact(
	const void *context, zend_mir_value_id value_id,
	zend_mir_value_fact_ref *out)
{
	(void) context;
	(void) value_id;
	(void) out;
	return false;
}

static zend_mir_lowering_status builder_test_provider_lower(
	zend_mir_lowering_context *context,
	const zend_mir_source_opcode_ref *source_opcode,
	zend_mir_mutator *mutator)
{
	builder_host *host = (builder_host *)
		zend_mir_lowering_context_provider_context(context);
	if (host == NULL || source_opcode == NULL || mutator == NULL
			|| host->fault == BUILDER_FAULT_PROVIDER) {
		return ZEND_MIR_LOWERING_FAILED;
	}
	return ZEND_MIR_LOWERING_SUCCESS;
}

static zend_mir_lowering_result run_provider_builder(builder_fault fault)
{
	test_source source = empty_fallthrough_source();
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_module_ops ops;
	zend_mir_lowering_context context;
	zend_mir_control_flow_map map;
	builder_host host;
	zend_mir_lowering_result result;

	source.opcode_count = 1;
	source.blocks[0].opcode_count = 1;
	source.blocks[1].first_opcode_ordinal = 1;
	source.opcodes[0].opline_index = 0;
	source.opcodes[0].zend_opcode_number = 0;
	source.opcodes[0].block_id = 0;
	source.opcodes[0].source_position_id = 0;
	source_view = test_view(&source);
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 2;
	shape.has_control_flow = true;
	shape.ssa_complete = true;
	builder_host_init(&host, &ops, fault);
	memset(&registry, 0, sizeof(registry));
	registry.providers[0].provider_id = 1;
	registry.providers[0].semantic_family_id = 1;
	registry.providers[0].context = &host;
	registry.providers[0].lower = builder_test_provider_lower;
	registry.provider_count = 1;
	registry.dispatch[0].claim.zend_opcode_number = 0;
	registry.dispatch[0].claim.semantic_family_id = 1;
	registry.dispatch[0].provider_index = 0;
	registry.dispatch_count = 1;
	registry.complete = true;
	assert(zend_mir_lowering_context_init(
		&context, &source_view, &shape, &registry, &ops, NULL, 9, 7, NULL));
	result = zend_mir_lower_w04_zend_source(&context, NULL, &map);
	if (fault == BUILDER_FAULT_NONE) {
		assert(result.status == ZEND_MIR_LOWERING_SUCCESS);
		assert(result.module == &host.module);
		assert(host.destroy_count == 0);
		builder_destroy(&host, result.module);
	} else {
		assert(result.status == ZEND_MIR_LOWERING_FAILED);
		assert(result.module == NULL);
		assert(result.guarantees == 0);
		assert(host.destroy_count == 1);
	}
	return result;
}

static void test_bool_identity_builder(void)
{
	test_source source = empty_fallthrough_source();
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_module_ops ops;
	zend_mir_lowering_context context;
	zend_mir_control_flow_map map;
	builder_host host;
	zend_mir_lowering_result result;

	source.opcode_count = 1;
	source.blocks[0].opcode_count = 1;
	source.blocks[1].first_opcode_ordinal = 1;
	source.ssa_count = 2;
	source.ssa[0].ssa_variable_id = 0;
	source.ssa[0].definition_opline_index = ZEND_MIR_ID_INVALID;
	source.ssa[0].source_slot_kind = ZEND_MIR_SOURCE_SLOT_TMP;
	source.ssa[1] = source.ssa[0];
	source.ssa[1].ssa_variable_id = 1;
	source.ssa[1].definition_opline_index = 0;
	source.opcodes[0].opline_index = 0;
	source.opcodes[0].zend_opcode_number = 52;
	source.opcodes[0].block_id = 0;
	source.opcodes[0].source_position_id = 0;
	source.opcodes[0].op1.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[0].op1.ssa_variable_id = 0;
	source.opcodes[0].result.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
	source.opcodes[0].result.ssa_variable_id = 1;
	source_view = test_view(&source);
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 2;
	shape.has_control_flow = true;
	shape.ssa_complete = true;
	memset(&registry, 0, sizeof(registry));
	registry.complete = true;
	builder_host_init(&host, &ops, BUILDER_FAULT_NONE);
	assert(zend_mir_lowering_context_init(
		&context, &source_view, &shape, &registry, &ops, NULL, 9, 7, NULL));
	assert(zend_mir_lowering_context_set_value_fact_resolver(
		&context, &source, builder_fact_at));
	result = zend_mir_lower_w04_zend_source(&context, NULL, &map);
	if (result.status != ZEND_MIR_LOWERING_SUCCESS) {
		fprintf(stderr,
			"bool identity failed: status=%u diagnostic=%u "
			"instructions=%u values=%u facts=%u destroys=%u\n",
			(unsigned int) result.status,
			(unsigned int) result.diagnostic_code,
			host.fixture.instruction_count, host.fixture.value_count,
			host.value_fact_count, host.destroy_count);
	}
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);
	assert(result.guarantees == ZEND_MIR_LOWERING_GUARANTEE_W04_ALL);
	assert(host.fixture.instruction_count == 3);
	assert(host.fixture.instructions[0].opcode == ZEND_MIR_OPCODE_COPY);
	assert(host.fixture.instructions[0].result_id
		== zend_mir_value_from_original_ssa(1));
	assert(host.fixture.operands[0].value_id
		== zend_mir_value_from_original_ssa(0));
	builder_destroy(&host, result.module);
	assert(host.destroy_count == 1);
}

static zend_mir_lowering_result run_builder(
	builder_fault fault, uint32_t branch_opcode, bool extended,
	bool literal_condition)
{
	test_source source = diamond_source();
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_module_ops ops;
	zend_mir_lowering_context context;
	zend_mir_control_flow_map map;
	builder_host host;
	zend_mir_lowering_result result;

	/* The merge block is intentionally empty after its PHI. */
	source.opcode_count = 3;
	source.blocks[3].first_opcode_ordinal = 3;
	source.blocks[3].opcode_count = 0;
	source.opcodes[0].zend_opcode_number = branch_opcode;
	if (extended) {
		source.ssa_count = 4;
		source.ssa[3] = source.ssa[0];
		source.ssa[3].ssa_variable_id = 3;
		source.ssa[3].source_slot = 3;
		source.ssa[3].definition_opline_index = 0;
		source.opcodes[0].result.kind = ZEND_MIR_SOURCE_OPERAND_SSA;
		source.opcodes[0].result.ssa_variable_id = 3;
	}
	source_view = test_view(&source);
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 4;
	shape.has_control_flow = true;
	shape.ssa_complete = true;
	memset(&registry, 0, sizeof(registry));
	registry.complete = true;
	builder_host_init(&host, &ops, fault);
	if (literal_condition) {
		zend_mir_value_id value_id = zend_mir_value_from_synthetic(0);
		zend_mir_constant_record constant;
		source.literal_count = 1;
		source.literals[0].literal_index = 0;
		source.literals[0].kind = ZEND_MIR_SOURCE_LITERAL_LONG_BITS;
		source.literals[0].payload_bits = 1;
		source.opcodes[0].op1.kind = ZEND_MIR_SOURCE_OPERAND_LITERAL;
		source.opcodes[0].op1.index = 0;
		assert(host.fixture.mutator.add_value(host.fixture.mutator.context,
			value_id, ZEND_MIR_REPRESENTATION_I64,
			ZEND_MIR_OWNERSHIP_STATE_OWNED));
		memset(&constant, 0, sizeof(constant));
		constant.value_id = value_id;
		constant.representation = ZEND_MIR_REPRESENTATION_I64;
		constant.kind = ZEND_MIR_CONSTANT_KIND_SIGNED_INTEGER_BITS;
		constant.payload_bits = 1;
		constant.symbol_id = ZEND_MIR_ID_INVALID;
		assert(host.fixture.mutator.add_constant(
			host.fixture.mutator.context, &constant));
	}
	assert(zend_mir_lowering_context_init(
		&context, &source_view, &shape, &registry, &ops, NULL, 9, 7, NULL));
	assert(zend_mir_lowering_context_set_value_fact_resolver(
		&context, &source, builder_fact_at));
	result = zend_mir_lower_w04_zend_source(&context, NULL, &map);
	if (result.status == ZEND_MIR_LOWERING_SUCCESS) {
		assert(result.module == &host.module);
		assert(result.guarantees == ZEND_MIR_LOWERING_GUARANTEE_W04_ALL);
		assert(map.context == NULL);
		assert(host.destroy_count == 0);
		builder_destroy(&host, result.module);
		assert(host.destroy_count == 1);
	} else {
		if (fault == BUILDER_FAULT_NONE) {
			fprintf(stderr,
				"builder host: functions=%u blocks=%u values=%u facts=%u "
				"instructions=%u operands=%u edges=%u destroys=%u\n",
				host.fixture.function_count, host.fixture.block_count,
				host.fixture.value_count, host.value_fact_count,
				host.fixture.instruction_count, host.fixture.operand_count,
				host.fixture.edge_count, host.destroy_count);
		}
		assert(result.module == NULL);
		assert(result.guarantees == 0);
		assert(host.destroy_count == 1);
	}
	return result;
}

static zend_mir_lowering_result run_loop_builder(bool attach_zend_source)
{
	test_source source = loop_carried_phi_source();
	zend_mir_lowering_source_view source_view;
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_module_ops ops;
	zend_mir_lowering_context context;
	zend_mir_control_flow_map map;
	zend_mir_zend_source zend_source;
	builder_host host;
	zend_mir_lowering_result result;
	uint32_t statepoint_count = 0;
	uint32_t i;

	/* The exit block is empty; all other opcodes are W04 terminators. */
	source.opcode_count = 3;
	source.blocks[3].first_opcode_ordinal = 3;
	source.blocks[3].opcode_count = 0;
	source_view = test_view(&source);
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 4;
	shape.has_control_flow = true;
	shape.ssa_complete = true;
	memset(&registry, 0, sizeof(registry));
	registry.complete = true;
	builder_host_init(&host, &ops, BUILDER_FAULT_NONE);
	for (i = 0; i < source.opcode_count; i++) {
		zend_mir_source_position_ref position;
		zend_mir_source_position_id position_id;
		memset(&position, 0, sizeof(position));
		position.id = ZEND_MIR_ID_INVALID;
		position.file_symbol_id = 1;
		position.line = i + 1;
		position.column_start = 1;
		position.column_end = 1;
		assert(host.fixture.mutator.add_source_position(
			host.fixture.mutator.context, &position, &position_id));
		assert(position_id == source.opcodes[i].source_position_id);
	}
	assert(zend_mir_lowering_context_init(
		&context, &source_view, &shape, &registry, &ops, NULL, 9, 7, NULL));
	assert(zend_mir_lowering_context_set_value_fact_resolver(
		&context, &source, builder_fact_at));
	memset(&zend_source, 0, sizeof(zend_source));
	zend_source.op_array_id = 1;
	if (attach_zend_source) {
		assert(zend_mir_lowering_context_set_zend_source(
			&context, &zend_source));
	}
	result = zend_mir_lower_w04_zend_source(&context, NULL, &map);
	if (attach_zend_source) {
		assert(result.status == ZEND_MIR_LOWERING_SUCCESS);
		assert(result.diagnostic_code == ZEND_MIRL_OK);
		assert(result.guarantees == ZEND_MIR_LOWERING_GUARANTEE_W04_ALL);
		assert(host.fixture.frame_state_count == 1);
		assert(host.fixture.source_map_count == 1);
		for (i = 0; i < host.fixture.instruction_count; i++) {
			if (host.fixture.instructions[i].opcode
					== ZEND_MIR_OPCODE_STATEPOINT) {
				statepoint_count++;
			}
		}
		assert(statepoint_count == 1);
		builder_destroy(&host, result.module);
		assert(host.destroy_count == 1);
	} else {
		assert(result.status == ZEND_MIR_LOWERING_FAILED);
		assert(result.diagnostic_code == ZEND_MIRL_MUTATION_FAILED);
		assert(result.module == NULL);
		assert(result.guarantees == 0);
		assert(host.destroy_count == 1);
	}
	return result;
}

static void test_empty_fallthrough_builder(void)
{
	test_source source = empty_fallthrough_source();
	zend_mir_lowering_source_view source_view = test_view(&source);
	zend_mir_lowering_source_shape shape;
	zend_mir_lowering_registry registry;
	zend_mir_lowering_module_ops ops;
	zend_mir_lowering_context context;
	zend_mir_control_flow_map map;
	builder_host host;
	zend_mir_lowering_result result;

	source.ssa_count = 1;
	source.ssa[0].ssa_variable_id = 0;
	source.ssa[0].definition_opline_index = ZEND_MIR_ID_INVALID;
	source.ssa[0].source_slot_kind = ZEND_MIR_SOURCE_SLOT_CV;
	memset(&shape, 0, sizeof(shape));
	shape.reachable_block_count = 2;
	shape.has_control_flow = true;
	shape.ssa_complete = true;
	memset(&registry, 0, sizeof(registry));
	registry.complete = true;
	builder_host_init(&host, &ops, BUILDER_FAULT_NONE);
	assert(zend_mir_lowering_context_init(
		&context, &source_view, &shape, &registry, &ops, NULL, 9, 7, NULL));
	assert(zend_mir_lowering_context_set_value_fact_resolver(
		&context, &source, builder_no_fact));
	result = zend_mir_lower_w04_zend_source(&context, NULL, &map);
	if (result.status != ZEND_MIR_LOWERING_SUCCESS) {
		fprintf(stderr, "factless builder failed: status=%u diagnostic=%u\n",
			(unsigned int) result.status,
			(unsigned int) result.diagnostic_code);
	}
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);
	assert(result.guarantees == ZEND_MIR_LOWERING_GUARANTEE_W04_ALL);
	assert(host.fixture.edge_count == 1);
	assert(host.fixture.instruction_count == 2);
	assert(host.fixture.instructions[0].opcode == ZEND_MIR_OPCODE_BRANCH);
	assert(host.fixture.instructions[1].opcode == ZEND_MIR_OPCODE_UNREACHABLE);
	builder_destroy(&host, result.module);
	assert(host.destroy_count == 1);
}

static void test_builder_and_failure_atomicity(void)
{
	zend_mir_lowering_result result = run_builder(
		BUILDER_FAULT_NONE, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	if (result.status != ZEND_MIR_LOWERING_SUCCESS) {
		fprintf(stderr, "builder failed: status=%u diagnostic=%u\n",
			(unsigned int) result.status,
			(unsigned int) result.diagnostic_code);
	}
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);
	assert(result.diagnostic_code == ZEND_MIRL_OK);

	result = run_builder(
		BUILDER_FAULT_NONE, ZEND_MIR_W04_OPCODE_JMPNZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);

	result = run_builder(
		BUILDER_FAULT_NONE, ZEND_MIR_W04_OPCODE_JMPZ_EX, true, false);
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);

	result = run_builder(
		BUILDER_FAULT_NONE, ZEND_MIR_W04_OPCODE_JMPNZ_EX, true, false);
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);

	result = run_builder(
		BUILDER_FAULT_NONE, ZEND_MIR_W04_OPCODE_JMPZ, false, true);
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);

	result = run_builder(
		BUILDER_FAULT_BLOCK_MAP, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_MUTATION_FAILED);

	result = run_builder(
		BUILDER_FAULT_VALUE_FACT, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_MUTATION_FAILED);

	result = run_builder(
		BUILDER_FAULT_PHI, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_MUTATION_FAILED);

	result = run_provider_builder(BUILDER_FAULT_NONE);
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);

	result = run_provider_builder(BUILDER_FAULT_PROVIDER);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_MUTATION_FAILED);

	test_bool_identity_builder();

	result = run_builder(
		BUILDER_FAULT_FINALIZE, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_FINALIZE_FAILED);

	result = run_builder(
		BUILDER_FAULT_STAGE1, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_STAGE1_VERIFY_FAILED);

	result = run_builder(
		BUILDER_FAULT_STAGE2, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_STAGE2_VERIFY_FAILED);

	result = run_builder(
		BUILDER_FAULT_STAGE3, ZEND_MIR_W04_OPCODE_JMPZ, false, false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);
	assert(result.diagnostic_code == ZEND_MIRL_STAGE3_VERIFY_FAILED);

	result = run_loop_builder(true);
	assert(result.status == ZEND_MIR_LOWERING_SUCCESS);

	result = run_loop_builder(false);
	assert(result.status == ZEND_MIR_LOWERING_FAILED);

	test_empty_fallthrough_builder();
}

int main(void)
{
	test_branch_order();
	test_validation();
	test_loop_validation();
	test_map();
	test_stage3();
	test_stage3_loop_statepoint();
	test_builder_and_failure_atomicity();
	puts("W04 control-flow unit tests: ok");
	return 0;
}
