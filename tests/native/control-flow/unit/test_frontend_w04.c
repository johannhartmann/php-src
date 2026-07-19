#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/Optimizer/zend_ssa.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_type_info.h"
#include "Zend/zend_vm_opcodes.h"

typedef struct _w04_frontend_fixture {
	zend_op_array op_array;
	zend_op opcodes[4];
	zend_ssa ssa;
	zend_ssa_op ssa_ops[4];
	zend_ssa_var ssa_vars[4];
	zend_ssa_var_info var_info[4];
	zend_ssa_block ssa_blocks[4];
	zend_basic_block cfg_blocks[4];
	uint32_t cfg_map[4];
	int predecessors[4];
	zend_ssa_phi phi;
	int phi_sources[2];
} w04_frontend_fixture;

static void w04_init_ssa_op(zend_ssa_op *op)
{
	memset(op, 0xff, sizeof(*op));
}

static void w04_set_successors(
	zend_basic_block *block, uint32_t count, int first, int second)
{
	block->successors = block->successors_storage;
	block->successors_count = count;
	block->successors_storage[0] = first;
	block->successors_storage[1] = second;
}

static void w04_init_diamond(w04_frontend_fixture *fixture)
{
	uint32_t i;
	memset(fixture, 0, sizeof(*fixture));
	for (i = 0; i < 4; i++) {
		w04_init_ssa_op(&fixture->ssa_ops[i]);
		fixture->cfg_map[i] = i;
		fixture->cfg_blocks[i].flags = ZEND_BB_REACHABLE;
		fixture->cfg_blocks[i].start = i;
		fixture->cfg_blocks[i].len = 1;
		fixture->cfg_blocks[i].idom = i == 0 ? -1 : 0;
		fixture->cfg_blocks[i].loop_header = -1;
		fixture->cfg_blocks[i].predecessor_offset = -1;
		fixture->ssa_vars[i].var = i == 0 ? 0 : 1;
		fixture->ssa_vars[i].definition = -1;
		fixture->ssa_vars[i].use_chain = -1;
		fixture->var_info[i].type = MAY_BE_LONG;
		fixture->var_info[i].has_range = true;
		fixture->var_info[i].range.min = 0;
		fixture->var_info[i].range.max = 100;
	}

	fixture->op_array.last_var = 1;
	fixture->op_array.T = 1;
	fixture->op_array.last = 4;
	fixture->op_array.opcodes = fixture->opcodes;

	fixture->opcodes[0].opcode = ZEND_JMPZ;
	fixture->opcodes[0].op1_type = IS_CV;
	fixture->opcodes[0].op1.var = EX_NUM_TO_VAR(0);
	fixture->opcodes[0].op2_type = IS_UNUSED;
	fixture->opcodes[0].result_type = IS_UNUSED;
	fixture->opcodes[0].lineno = 10;
	fixture->ssa_ops[0].op1_use = 0;

	for (i = 1; i <= 2; i++) {
		fixture->opcodes[i].opcode = ZEND_QM_ASSIGN;
		fixture->opcodes[i].op1_type = IS_CV;
		fixture->opcodes[i].op1.var = EX_NUM_TO_VAR(0);
		fixture->opcodes[i].op2_type = IS_UNUSED;
		fixture->opcodes[i].result_type = IS_TMP_VAR;
		fixture->opcodes[i].result.var = EX_NUM_TO_VAR(1);
		fixture->opcodes[i].lineno = 10 + i;
		fixture->ssa_ops[i].op1_use = 0;
		fixture->ssa_ops[i].result_def = (int) i;
		fixture->ssa_vars[i].definition = (int) i;
	}

	fixture->opcodes[3].opcode = ZEND_RETURN;
	fixture->opcodes[3].op1_type = IS_TMP_VAR;
	fixture->opcodes[3].op1.var = EX_NUM_TO_VAR(1);
	fixture->opcodes[3].op2_type = IS_UNUSED;
	fixture->opcodes[3].result_type = IS_UNUSED;
	fixture->opcodes[3].lineno = 13;
	fixture->ssa_ops[3].op1_use = 3;

	w04_set_successors(&fixture->cfg_blocks[0], 2, 2, 1);
	w04_set_successors(&fixture->cfg_blocks[1], 1, 3, -1);
	w04_set_successors(&fixture->cfg_blocks[2], 1, 3, -1);
	w04_set_successors(&fixture->cfg_blocks[3], 0, -1, -1);
	fixture->predecessors[0] = 0;
	fixture->predecessors[1] = 0;
	fixture->predecessors[2] = 1;
	fixture->predecessors[3] = 2;
	fixture->cfg_blocks[1].predecessor_offset = 0;
	fixture->cfg_blocks[1].predecessors_count = 1;
	fixture->cfg_blocks[2].predecessor_offset = 1;
	fixture->cfg_blocks[2].predecessors_count = 1;
	fixture->cfg_blocks[3].predecessor_offset = 2;
	fixture->cfg_blocks[3].predecessors_count = 2;

	fixture->phi.pi = -1;
	fixture->phi.var = 1;
	fixture->phi.ssa_var = 3;
	fixture->phi.block = 3;
	fixture->phi.sources = fixture->phi_sources;
	fixture->phi_sources[0] = 1;
	fixture->phi_sources[1] = 2;
	fixture->ssa_blocks[3].phis = &fixture->phi;
	fixture->ssa_vars[3].definition_phi = &fixture->phi;

	fixture->ssa.cfg.blocks_count = 4;
	fixture->ssa.cfg.edges_count = 4;
	fixture->ssa.cfg.blocks = fixture->cfg_blocks;
	fixture->ssa.cfg.predecessors = fixture->predecessors;
	fixture->ssa.cfg.map = fixture->cfg_map;
	fixture->ssa.vars_count = 4;
	fixture->ssa.blocks = fixture->ssa_blocks;
	fixture->ssa.ops = fixture->ssa_ops;
	fixture->ssa.vars = fixture->ssa_vars;
	fixture->ssa.var_info = fixture->var_info;
}

static void test_w04_diamond_projection(void)
{
	w04_frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_source_block_ref block;
	zend_mir_source_edge_ref edge;
	zend_mir_source_phi_ref phi;
	zend_mir_source_phi_input_ref input;
	zend_mir_lowering_status status;

	w04_init_diamond(&fixture);
	status = zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic);
	if (status != ZEND_MIR_LOWERING_SUCCESS) {
		fprintf(stderr, "diamond projection failed: status=%u code=%u\n",
			(unsigned int) status, (unsigned int) diagnostic.code);
	}
	assert(status == ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_zend_source_view(&source, &view));
	assert(view.contract_version == ZEND_MIR_W04_CONTRACT_VERSION);
	assert(view.block_count(view.context) == 4);
	assert(view.edge_count(view.context) == 4);
	assert(view.phi_count(view.context) == 1);
	assert(view.phi_input_count(view.context) == 2);
	assert(view.block_at(view.context, 3, &block));
	assert(block.immediate_dominator == 0);
	assert(view.edge_at(view.context, 0, &edge));
	assert(edge.from_block_id == 0 && edge.to_block_id == 2);
	assert(edge.successor_index == 0 && edge.predecessor_index == 0);
	assert((edge.flags & ZEND_MIR_SOURCE_EDGE_EXPLICIT_JUMP) != 0);
	assert(view.edge_at(view.context, 1, &edge));
	assert(edge.to_block_id == 1 && edge.successor_index == 1);
	assert((edge.flags & ZEND_MIR_SOURCE_EDGE_FALLTHROUGH) != 0);
	assert(view.phi_at(view.context, 0, &phi));
	assert(phi.block_id == 3 && phi.result_ssa_variable_id == 3);
	assert(phi.kind == ZEND_MIR_SOURCE_PHI_MERGE);
	assert(view.phi_input_at(view.context, 0, &input));
	assert(input.input_index == 0 && input.predecessor_block_id == 1);
	assert(input.source_ssa_variable_id == 1);
	assert(view.phi_input_at(view.context, 1, &input));
	assert(input.input_index == 1 && input.predecessor_block_id == 2);
	assert(input.source_ssa_variable_id == 2);
}

static void test_w04_pi_and_fail_closed_projection(void)
{
	w04_frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_source_phi_ref phi;
	zend_mir_source_phi_input_ref input;

	w04_init_diamond(&fixture);
	fixture.phi.pi = 2;
	fixture.phi.constraint.type.type_mask = MAY_BE_LONG;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);
	assert(zend_mir_zend_source_view(&source, &view));
	assert(view.phi_at(view.context, 0, &phi));
	assert(phi.kind == ZEND_MIR_SOURCE_PHI_PI_TYPE);
	assert(view.phi_input_count(view.context) == 1);
	assert(view.phi_input_at(view.context, 0, &input));
	assert(input.input_index == 1 && input.predecessor_block_id == 2);

	w04_init_diamond(&fixture);
	fixture.cfg_blocks[2].flags |= ZEND_BB_PROTECTED;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic.code == ZEND_MIRL_W04_PROTECTED_REGION);

	w04_init_diamond(&fixture);
	fixture.cfg_blocks[2].flags |= ZEND_BB_IRREDUCIBLE_LOOP;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic.code == ZEND_MIRL_W04_IRREDUCIBLE_LOOP);

	w04_init_diamond(&fixture);
	fixture.predecessors[3] = 0;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic.code == ZEND_MIRL_W04_MALFORMED_CFG);

	w04_init_diamond(&fixture);
	fixture.op_array.last_try_catch = 1;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic.code == ZEND_MIRL_W04_PROTECTED_REGION);

	w04_init_diamond(&fixture);
	fixture.phi.pi = 2;
	fixture.phi.constraint.type.ce = (zend_class_entry *) &fixture;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic.code == ZEND_MIRL_W04_UNSUPPORTED_PHI_PI);

	w04_init_diamond(&fixture);
	fixture.cfg_blocks[1].idom = 2;
	fixture.cfg_blocks[2].idom = 1;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic.code == ZEND_MIRL_W04_MALFORMED_CFG);

	w04_init_diamond(&fixture);
	fixture.opcodes[0].opcode = ZEND_DO_FCALL;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic.code == ZEND_MIRL_W05_RUNTIME_EFFECT_DEFERRED);

	w04_init_diamond(&fixture);
	fixture.opcodes[0].opcode = ZEND_ASSIGN_REF;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_DEFERRED);
	assert(diagnostic.code == ZEND_MIRL_W06_REFERENCE_SEMANTICS_DEFERRED);
}

static void test_w04_smart_branch_result_type(void)
{
	w04_frontend_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;

	w04_init_diamond(&fixture);
	fixture.opcodes[0].opcode = ZEND_IS_SMALLER;
	fixture.opcodes[0].op1_type = IS_CV;
	fixture.opcodes[0].op1.var = EX_NUM_TO_VAR(0);
	fixture.opcodes[0].op2_type = IS_CV;
	fixture.opcodes[0].op2.var = EX_NUM_TO_VAR(0);
	fixture.opcodes[0].result_type = IS_TMP_VAR | IS_SMART_BRANCH_JMPZ;
	fixture.opcodes[0].result.var = EX_NUM_TO_VAR(1);
	fixture.ssa_ops[0].op1_use = 0;
	fixture.ssa_ops[0].op2_use = 0;
	fixture.ssa_ops[0].result_def = 1;
	fixture.ssa_vars[1].definition = 0;

	fixture.opcodes[1].opcode = ZEND_JMPZ;
	fixture.opcodes[1].op1_type = IS_TMP_VAR;
	fixture.opcodes[1].op1.var = EX_NUM_TO_VAR(1);
	fixture.opcodes[1].op2_type = IS_UNUSED;
	fixture.opcodes[1].result_type = IS_UNUSED;
	w04_init_ssa_op(&fixture.ssa_ops[1]);
	fixture.ssa_ops[1].op1_use = 1;

	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_SUCCESS);

	fixture.opcodes[1].opcode = ZEND_JMPNZ;
	assert(zend_mir_zend_source_init_w04(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
		== ZEND_MIR_LOWERING_REJECTED);
	assert(diagnostic.code == ZEND_MIRL_W04_BRANCH_PROOF_FAILED);
}

int main(void)
{
	test_w04_diamond_projection();
	test_w04_pi_and_fail_closed_projection();
	test_w04_smart_branch_result_type();
	puts("W04 frontend projection tests: ok");
	return 0;
}
