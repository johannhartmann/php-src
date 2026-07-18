#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Zend/Native/Lowering/Frontend/zend_mir_zend_source.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_type_info.h"
#include "Zend/zend_vm_opcodes.h"
#include "Zend/Optimizer/zend_ssa.h"

typedef struct _fuzz_fixture {
	zend_op_array op_array;
	zend_op opcodes[2];
	zend_ssa ssa;
	zend_ssa_op ssa_ops[2];
	zend_ssa_var ssa_vars[2];
	zend_ssa_var_info var_info[2];
	zend_ssa_block ssa_blocks[1];
	zend_basic_block cfg_blocks[1];
	uint32_t cfg_map[2];
} fuzz_fixture;

static uint64_t fuzz_random(uint64_t *state)
{
	uint64_t value = *state;
	value ^= value << 13;
	value ^= value >> 7;
	value ^= value << 17;
	*state = value;
	return value;
}

static void initialize_fixture(fuzz_fixture *fixture)
{
	memset(fixture, 0, sizeof(*fixture));
	memset(fixture->ssa_ops, 0xff, sizeof(fixture->ssa_ops));

	fixture->op_array.last_var = 1;
	fixture->op_array.T = 1;
	fixture->op_array.last = 2;
	fixture->op_array.opcodes = fixture->opcodes;

	fixture->opcodes[0].opcode = ZEND_QM_ASSIGN;
	fixture->opcodes[0].op1_type = IS_CV;
	fixture->opcodes[0].op1.var = EX_NUM_TO_VAR(0);
	fixture->opcodes[0].op2_type = IS_UNUSED;
	fixture->opcodes[0].result_type = IS_TMP_VAR;
	fixture->opcodes[0].result.var = EX_NUM_TO_VAR(1);
	fixture->opcodes[0].lineno = 10;
	fixture->opcodes[1].opcode = ZEND_RETURN;
	fixture->opcodes[1].op1_type = IS_TMP_VAR;
	fixture->opcodes[1].op1.var = EX_NUM_TO_VAR(1);
	fixture->opcodes[1].op2_type = IS_UNUSED;
	fixture->opcodes[1].result_type = IS_UNUSED;
	fixture->opcodes[1].lineno = 11;

	fixture->ssa_ops[0].op1_use = 0;
	fixture->ssa_ops[0].result_def = 1;
	fixture->ssa_ops[1].op1_use = 1;
	fixture->ssa.vars_count = 2;
	fixture->ssa.ops = fixture->ssa_ops;
	fixture->ssa.vars = fixture->ssa_vars;
	fixture->ssa.var_info = fixture->var_info;
	fixture->ssa.blocks = fixture->ssa_blocks;
	fixture->ssa.vars[0].var = 0;
	fixture->ssa.vars[0].definition = -1;
	fixture->ssa.vars[0].use_chain = -1;
	fixture->ssa.vars[1].var = 1;
	fixture->ssa.vars[1].definition = 0;
	fixture->ssa.vars[1].use_chain = -1;
	fixture->ssa.var_info[0].type = MAY_BE_LONG;
	fixture->ssa.var_info[0].has_range = true;
	fixture->ssa.var_info[0].range.min = 1;
	fixture->ssa.var_info[0].range.max = 10;
	fixture->ssa.var_info[1] = fixture->ssa.var_info[0];

	fixture->ssa.cfg.blocks_count = 1;
	fixture->ssa.cfg.blocks = fixture->cfg_blocks;
	fixture->ssa.cfg.map = fixture->cfg_map;
	fixture->cfg_blocks[0].flags = ZEND_BB_REACHABLE;
	fixture->cfg_blocks[0].start = 0;
	fixture->cfg_blocks[0].len = 2;
	fixture->cfg_blocks[0].predecessor_offset = -1;
}

static void mutate_source(fuzz_fixture *fixture, uint64_t value)
{
	switch (value % 10U) {
		case 0:
			break;
		case 1:
			fixture->opcodes[0].op1_type = UINT8_MAX;
			break;
		case 2:
			fixture->opcodes[0].op1.var = 0;
			break;
		case 3:
			fixture->ssa_ops[0].op1_use = 2;
			break;
		case 4:
			fixture->ssa_ops[0].result_def = -1;
			break;
		case 5:
			fixture->cfg_map[1] = 1;
			break;
		case 6:
			fixture->ssa.cfg.blocks_count = 2;
			break;
		case 7:
			fixture->op_array.last_try_catch = 1;
			break;
		case 8:
			fixture->ssa.var_info[0].type = MAY_BE_STRING;
			break;
		default:
			fixture->opcodes[0].opcode = ZEND_JMP;
			break;
	}
}

static int fuzz_source(uint64_t *state)
{
	fuzz_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_lowering_status status;

	initialize_fixture(&fixture);
	mutate_source(&fixture, fuzz_random(state));
	memset(&source, 0xa5, sizeof(source));
	status = zend_mir_zend_source_init(
		&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic);
	if (diagnostic.status != status) {
		return 1;
	}
	if (status == ZEND_MIR_LOWERING_SUCCESS) {
		if (diagnostic.code != ZEND_MIRL_OK
				|| !zend_mir_zend_source_view(&source, &view)
				|| view.opcode_count(view.context) != 2
				|| view.ssa_count(view.context) != 2) {
			return 1;
		}
	} else if (source.initialized != 0 || source.op_array != NULL
			|| source.ssa != NULL || zend_mir_zend_source_view(&source, &view)) {
		return 1;
	}
	return 0;
}

static int fuzz_source_view(uint64_t *state)
{
	fuzz_fixture fixture;
	zend_mir_zend_source source;
	zend_mir_zend_source corrupted;
	zend_mir_frontend_diagnostic diagnostic;
	zend_mir_lowering_source_view view;
	zend_mir_source_opcode_ref opcode;
	zend_mir_source_ssa_ref ssa;
	uint32_t opcode_index;
	uint32_t ssa_index;

	initialize_fixture(&fixture);
	if (zend_mir_zend_source_init(
			&source, &fixture.op_array, &fixture.ssa, 17, 23, &diagnostic)
			!= ZEND_MIR_LOWERING_SUCCESS
			|| !zend_mir_zend_source_view(&source, &view)) {
		return 1;
	}
	opcode_index = (uint32_t) (fuzz_random(state) % 5U);
	ssa_index = (uint32_t) (fuzz_random(state) % 5U);
	if (view.opcode_at(view.context, opcode_index, &opcode)
			!= (opcode_index < 2)
			|| view.ssa_at(view.context, ssa_index, &ssa) != (ssa_index < 2)
			|| view.opcode_at(view.context, 0, NULL)
			|| view.ssa_at(view.context, 0, NULL)) {
		return 1;
	}
	corrupted = source;
	corrupted.initialized ^= (uint32_t) fuzz_random(state) | 1U;
	if (zend_mir_zend_source_view(&corrupted, &view)) {
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t state;
	uint32_t source_cases;
	uint32_t source_view_cases;
	uint32_t index;

	if (argc != 4) {
		fprintf(stderr, "usage: %s SEED SOURCE_CASES SOURCE_VIEW_CASES\n", argv[0]);
		return 2;
	}
	state = strtoull(argv[1], NULL, 10);
	source_cases = (uint32_t) strtoul(argv[2], NULL, 10);
	source_view_cases = (uint32_t) strtoul(argv[3], NULL, 10);
	if (state == 0) {
		state = UINT64_C(0x9e3779b97f4a7c15);
	}
	for (index = 0; index < source_cases; index++) {
		if (fuzz_source(&state) != 0) {
			fprintf(stderr, "source mutation failed at case %u\n", index);
			return 1;
		}
	}
	for (index = 0; index < source_view_cases; index++) {
		if (fuzz_source_view(&state) != 0) {
			fprintf(stderr, "source-view mutation failed at case %u\n", index);
			return 1;
		}
	}
	return 0;
}
